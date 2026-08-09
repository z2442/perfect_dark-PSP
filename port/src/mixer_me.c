#include "mixer_me.h"

#include "mixer_cmd.h"

#ifdef PD_PSP_AUDIO_ME

#ifndef asm
#define asm __asm__
#endif

#include <me-core-mapper/me-core.h>
#include <pspintrman.h>
#include <pspkernel.h>
#include <psptypes.h>
#include <stdbool.h>
#include <stdint.h>

#define MIXER_ME_QUEUE_DEPTH 4U
#define MIXER_ME_READY_TIMEOUT_USEC 250000U
#define MIXER_ME_POLL_USEC 100U

#define MIXER_ME_PROGRESS_ENTERED 0x10U
#define MIXER_ME_PROGRESS_BOOT_RELEASED 0x20U
#define MIXER_ME_PROGRESS_READY 0x100U

typedef struct MixerMeJob_s {
	volatile uintptr_t cmdList;
	volatile uintptr_t auxData;
	volatile u32 cmdCount;
} MixerMeJob;

typedef struct MixerMeShared_s {
	volatile u32 submitSeq;
	volatile u32 doneSeq;
	volatile u32 exitRequested;
	volatile u32 bootRelease;
	volatile u32 ready;
	volatile u32 progress;
	volatile u32 completionInterruptEnabled;
	volatile MixerMeJob jobs[MIXER_ME_QUEUE_DEPTH];
} MixerMeShared;

enum {
	MIXER_ME_SHARED_WORDS = (sizeof(MixerMeShared) + sizeof(u32) - 1) / sizeof(u32)
};

static volatile u32 g_MixerMeSharedStorage[MIXER_ME_SHARED_WORDS]
	__attribute__((aligned(64), section(".uncached")));

#define MIXER_ME_SHARED \
	((volatile MixerMeShared *)(uintptr_t)(UNCACHED_USER_MASK | (uintptr_t)g_MixerMeSharedStorage))

static s32 g_MixerMeBootStarted;
static s32 g_MixerMeBootResult;
static s32 g_MixerMeInitialized;
static s32 g_MixerMeReady;
static u32 g_MixerMeConsumedSeq;
static u32 g_MixerMePendingCount;
static SceUID g_MixerMeCompletionSema = -1;
static s32 g_MixerMeCompletionInterruptReady;

static void mixerMeDrainCompletionSema(void)
{
	if (!g_MixerMeCompletionInterruptReady) {
		return;
	}

	while (sceKernelPollSema(g_MixerMeCompletionSema, 1) == 0) {
	}
}

static u32 mixerMeClaimCompleted(void)
{
	const u32 doneseq = MIXER_ME_SHARED->doneSeq;
	u32 completed;

	if (doneseq == g_MixerMeConsumedSeq) {
		return 0;
	}

	completed = doneseq - g_MixerMeConsumedSeq;
	g_MixerMeConsumedSeq = doneseq;

	if (completed >= g_MixerMePendingCount) {
		g_MixerMePendingCount = 0;
	} else {
		g_MixerMePendingCount -= completed;
	}

	mixerMeDrainCompletionSema();
	sceKernelDcacheWritebackInvalidateAll();
	return completed;
}

static void mixerMeCompletionInterrupt(int subintr, void *arg)
{
	(void)subintr;
	(void)arg;
	if (g_MixerMeCompletionSema >= 0) {
		sceKernelSignalSema(g_MixerMeCompletionSema, 1);
	}
}

static s32 mixerMeInstallCompletionInterrupt(void)
{
	s32 result;

	if (g_MixerMeCompletionInterruptReady) {
		return 0;
	}

	g_MixerMeCompletionSema = sceKernelCreateSema("PD Audio ME Done", 0, 0, 1, NULL);
	if (g_MixerMeCompletionSema < 0) {
		return g_MixerMeCompletionSema;
	}

	result = sceKernelRegisterSubIntrHandler(PSP_MECODEC_INT, 0, mixerMeCompletionInterrupt, NULL);
	if (result < 0) {
		sceKernelDeleteSema(g_MixerMeCompletionSema);
		g_MixerMeCompletionSema = -1;
		return result;
	}

	result = sceKernelEnableSubIntr(PSP_MECODEC_INT, 0);
	if (result < 0) {
		sceKernelReleaseSubIntrHandler(PSP_MECODEC_INT, 0);
		sceKernelDeleteSema(g_MixerMeCompletionSema);
		g_MixerMeCompletionSema = -1;
		return result;
	}

	MIXER_ME_SHARED->completionInterruptEnabled = true;
	meLibSync();
	g_MixerMeCompletionInterruptReady = true;
	return 0;
}

__attribute__((noinline, aligned(4)))
void meLibOnException(void)
{
	MIXER_ME_SHARED->ready = false;
	MIXER_ME_SHARED->exitRequested = true;
	meLibSync();
	if (MIXER_ME_SHARED->completionInterruptEnabled) {
		meLibSendExternalSoftInterrupt();
	}
	meLibHalt();
}

__attribute__((noinline, aligned(4)))
void meLibOnExternalInterrupt(void)
{
	meLibOnException();
}

__attribute__((noinline, aligned(4)))
void meLibOnProcess(void)
{
	MIXER_ME_SHARED->progress = MIXER_ME_PROGRESS_ENTERED;
	meLibSync();

	while (!MIXER_ME_SHARED->bootRelease) {
		meLibDelayPipeline();
	}

	MIXER_ME_SHARED->progress = MIXER_ME_PROGRESS_BOOT_RELEASED;
	meLibSync();
	MIXER_ME_SHARED->doneSeq = 0;
	MIXER_ME_SHARED->ready = true;
	MIXER_ME_SHARED->progress = MIXER_ME_PROGRESS_READY;
	meLibSync();

	while (!MIXER_ME_SHARED->exitRequested) {
		const u32 doneseq = MIXER_ME_SHARED->doneSeq;
		const volatile MixerMeJob *job;

		if (MIXER_ME_SHARED->submitSeq == doneseq) {
			meLibDelayPipeline();
			continue;
		}

		job = &MIXER_ME_SHARED->jobs[doneseq % MIXER_ME_QUEUE_DEPTH];
		meCoreDcacheWritebackInvalidateAll();
		mixerExecCommandList((const Acmd *)(uintptr_t)job->cmdList,
				(const uintptr_t *)(uintptr_t)job->auxData, job->cmdCount);
		meCoreDcacheWritebackInvalidateAll();
		meLibSync();
		MIXER_ME_SHARED->doneSeq = doneseq + 1;
		meLibSync();

		if (MIXER_ME_SHARED->completionInterruptEnabled) {
			meLibSendExternalSoftInterrupt();
		}
	}

	MIXER_ME_SHARED->ready = false;
	MIXER_ME_SHARED->doneSeq = MIXER_ME_SHARED->submitSeq;
	meLibSync();
	meLibHalt();
}

int mixerMeBoot(void)
{
	if (g_MixerMeBootStarted) {
		return g_MixerMeBootResult >= 0;
	}

	MIXER_ME_SHARED->submitSeq = 0;
	MIXER_ME_SHARED->doneSeq = 0;
	MIXER_ME_SHARED->exitRequested = false;
	MIXER_ME_SHARED->bootRelease = false;
	MIXER_ME_SHARED->ready = false;
	MIXER_ME_SHARED->progress = 0;
	MIXER_ME_SHARED->completionInterruptEnabled = false;

	for (u32 i = 0; i < MIXER_ME_QUEUE_DEPTH; i++) {
		MIXER_ME_SHARED->jobs[i].cmdList = 0;
		MIXER_ME_SHARED->jobs[i].auxData = 0;
		MIXER_ME_SHARED->jobs[i].cmdCount = 0;
	}

	meLibSync();
	g_MixerMeBootStarted = true;
	g_MixerMeBootResult = meLibDefaultInit();
	return g_MixerMeBootResult >= 0;
}

int mixerMeInit(void)
{
	u32 start;

	if (g_MixerMeInitialized) {
		return g_MixerMeReady;
	}
	g_MixerMeInitialized = true;

	if (!mixerMeBoot()) {
		return 0;
	}

	/* Optional: waits retain polling if the firmware cannot install it. */
	(void)mixerMeInstallCompletionInterrupt();
	MIXER_ME_SHARED->bootRelease = true;
	meLibSync();

	start = sceKernelGetSystemTimeLow();
	while (MIXER_ME_SHARED->progress != MIXER_ME_PROGRESS_READY) {
		if (sceKernelGetSystemTimeLow() - start >= MIXER_ME_READY_TIMEOUT_USEC) {
			MIXER_ME_SHARED->exitRequested = true;
			MIXER_ME_SHARED->bootRelease = true;
			meLibSync();
			return 0;
		}
		sceKernelDelayThread(MIXER_ME_POLL_USEC);
	}

	g_MixerMeConsumedSeq = 0;
	g_MixerMePendingCount = 0;
	g_MixerMeReady = true;
	return 1;
}

int mixerMeIsReady(void)
{
	return g_MixerMeReady && MIXER_ME_SHARED->ready;
}

void mixerMeSubmit(const Acmd *cmdlist, const uintptr_t *auxdata, s32 cmdcount)
{
	u32 submitseq;
	volatile MixerMeJob *job;

	if (!mixerMeIsReady()) {
		mixerExecCommandList(cmdlist, auxdata, cmdcount);
		return;
	}

	while ((MIXER_ME_SHARED->submitSeq - MIXER_ME_SHARED->doneSeq) >= MIXER_ME_QUEUE_DEPTH) {
		if (g_MixerMeCompletionInterruptReady) {
			sceKernelWaitSema(g_MixerMeCompletionSema, 1, NULL);
		} else {
			sceKernelDelayThread(MIXER_ME_POLL_USEC);
		}
	}

	sceKernelDcacheWritebackInvalidateAll();
	submitseq = MIXER_ME_SHARED->submitSeq;
	job = &MIXER_ME_SHARED->jobs[submitseq % MIXER_ME_QUEUE_DEPTH];
	job->cmdList = (uintptr_t)cmdlist;
	job->auxData = (uintptr_t)auxdata;
	job->cmdCount = (u32)cmdcount;
	meLibSync();
	MIXER_ME_SHARED->submitSeq = submitseq + 1;
	meLibSync();
	g_MixerMePendingCount++;
}

u32 mixerMeConsumeAvailable(void)
{
	if (g_MixerMePendingCount == 0 || !mixerMeIsReady()) {
		return 0;
	}
	return mixerMeClaimCompleted();
}

u32 mixerMeWait(void)
{
	u32 completed;

	if (g_MixerMePendingCount == 0 || !mixerMeIsReady()) {
		return 0;
	}

	completed = mixerMeClaimCompleted();
	while (completed == 0) {
		if (!mixerMeIsReady()) {
			return 0;
		}
		if (g_MixerMeCompletionInterruptReady) {
			sceKernelWaitSema(g_MixerMeCompletionSema, 1, NULL);
		} else {
			sceKernelDelayThread(MIXER_ME_POLL_USEC);
		}
		completed = mixerMeClaimCompleted();
	}
	return completed;
}

void mixerMeShutdown(void)
{
	if (!g_MixerMeInitialized) {
		return;
	}

	while (g_MixerMePendingCount != 0 && mixerMeIsReady()) {
		mixerMeWait();
	}

	if (mixerMeIsReady()) {
		MIXER_ME_SHARED->exitRequested = true;
		meLibSync();

		for (u32 i = 0; i < MIXER_ME_READY_TIMEOUT_USEC / MIXER_ME_POLL_USEC &&
				MIXER_ME_SHARED->ready; i++) {
			sceKernelDelayThread(MIXER_ME_POLL_USEC);
		}
	}

	if (g_MixerMeCompletionInterruptReady) {
		sceKernelDisableSubIntr(PSP_MECODEC_INT, 0);
		sceKernelReleaseSubIntrHandler(PSP_MECODEC_INT, 0);
		g_MixerMeCompletionInterruptReady = false;
	}
	if (g_MixerMeCompletionSema >= 0) {
		sceKernelDeleteSema(g_MixerMeCompletionSema);
		g_MixerMeCompletionSema = -1;
	}

	g_MixerMeReady = false;
	g_MixerMeInitialized = false;
	g_MixerMePendingCount = 0;
}

#else

int mixerMeBoot(void) { return 0; }
int mixerMeInit(void) { return 0; }
int mixerMeIsReady(void) { return 0; }
void mixerMeSubmit(const Acmd *cmdlist, const uintptr_t *auxdata, s32 cmdcount)
{
	mixerExecCommandList(cmdlist, auxdata, cmdcount);
	(void)cmdlist;
	(void)auxdata;
	(void)cmdcount;
}
u32 mixerMeConsumeAvailable(void) { return 0; }
u32 mixerMeWait(void) { return 0; }
void mixerMeShutdown(void) {}

#endif
