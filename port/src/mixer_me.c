#include "mixer_me.h"

#include "mixer_cmd.h"

#ifdef PD_PSP_AUDIO_ME

#ifndef asm
#define asm __asm__
#endif

#include <stdint.h>
#include <psptypes.h>
#include <pspkernel.h>
#include <me-core-mapper/me-core.h>

enum {
	MIXER_ME_QUEUE_DEPTH = 4
};

typedef struct MixerMeJob_s {
	volatile uintptr_t cmdList;
	volatile uintptr_t auxData;
	volatile u32 cmdCount;
} MixerMeJob;

typedef struct MixerMeShared_s {
	volatile u32 submitSeq;
	volatile u32 doneSeq;
	volatile u32 exitRequested;
	volatile u32 ready;
	volatile MixerMeJob jobs[MIXER_ME_QUEUE_DEPTH];
} MixerMeShared;

enum {
	MIXER_ME_SHARED_WORDS = (sizeof(MixerMeShared) + sizeof(u32) - 1) / sizeof(u32)
};


static volatile u32 g_MixerMeSharedStorage[MIXER_ME_SHARED_WORDS]
	__attribute__((aligned(64)));

#define MIXER_ME_SHARED \
	((volatile MixerMeShared *)(uintptr_t)(UNCACHED_USER_MASK | (uintptr_t)g_MixerMeSharedStorage))
	
static int g_MixerMeInitialized = 0;
static int g_MixerMeReady = 0;
static u32 g_MixerMeConsumedSeq = 0;
static u32 g_MixerMePendingCount = 0;

static u32 mixerMeClaimCompleted(void)
{
	const u32 doneSeq = MIXER_ME_SHARED->doneSeq;

	if (doneSeq == g_MixerMeConsumedSeq) {
		return 0;
	}

	const u32 completed = doneSeq - g_MixerMeConsumedSeq;

	g_MixerMeConsumedSeq = doneSeq;
	if (completed >= g_MixerMePendingCount) {
		g_MixerMePendingCount = 0;
	} else {
		g_MixerMePendingCount -= completed;
	}

	sceKernelDcacheWritebackInvalidateAll();
	return completed;
}

__attribute__((noinline, aligned(4)))
void meLibOnProcess(void)
{
	MIXER_ME_SHARED->ready = 1;
	MIXER_ME_SHARED->doneSeq = 0;

	while (!MIXER_ME_SHARED->exitRequested) {
		const u32 doneSeq = MIXER_ME_SHARED->doneSeq;

		if (MIXER_ME_SHARED->submitSeq == doneSeq) {
			meLibDelayPipeline();
			continue;
		}

		const volatile MixerMeJob *job = &MIXER_ME_SHARED->jobs[doneSeq % MIXER_ME_QUEUE_DEPTH];

		meCoreDcacheWritebackInvalidateAll();
		mixerExecCommandList((const Acmd *)(uintptr_t)job->cmdList,
				(const uintptr_t *)(uintptr_t)job->auxData,
				job->cmdCount);
		meCoreDcacheWritebackInvalidateAll();

		meLibSync();
		MIXER_ME_SHARED->doneSeq = doneSeq + 1;
		meLibSync();
	}

	MIXER_ME_SHARED->ready = 0;
	MIXER_ME_SHARED->doneSeq = MIXER_ME_SHARED->submitSeq;
	meLibSync();
	meLibHalt();
}

int mixerMeInit(void)
{
	if (g_MixerMeInitialized) {
		return g_MixerMeReady;
	}

	g_MixerMeInitialized = 1;
	MIXER_ME_SHARED->submitSeq = 0;
	MIXER_ME_SHARED->doneSeq = 0;
	MIXER_ME_SHARED->exitRequested = 0;
	MIXER_ME_SHARED->ready = 0;
	for (u32 i = 0; i < MIXER_ME_QUEUE_DEPTH; i++) {
		MIXER_ME_SHARED->jobs[i].cmdList = 0;
		MIXER_ME_SHARED->jobs[i].auxData = 0;
		MIXER_ME_SHARED->jobs[i].cmdCount = 0;
	}
	g_MixerMeConsumedSeq = 0;
	g_MixerMePendingCount = 0;

	if (meLibDefaultInit() < 0) {
		g_MixerMeReady = 0;
		return 0;
	}

	for (int i = 0; i < 1000 && !MIXER_ME_SHARED->ready; i++) {
		sceKernelDelayThread(1000);
	}

	g_MixerMeReady = MIXER_ME_SHARED->ready ? 1 : 0;
	return g_MixerMeReady;
}

int mixerMeIsReady(void)
{
	return g_MixerMeReady && MIXER_ME_SHARED->ready;
}

void mixerMeSubmit(const Acmd *cmdList, const uintptr_t *auxData, s32 cmdCount)
{
	if (!mixerMeIsReady()) {
		mixerExecCommandList(cmdList, auxData, cmdCount);
		return;
	}

	while ((MIXER_ME_SHARED->submitSeq - MIXER_ME_SHARED->doneSeq) >= MIXER_ME_QUEUE_DEPTH) {
		sceKernelDelayThread(100);
	}

	sceKernelDcacheWritebackInvalidateAll();

	const u32 submitSeq = MIXER_ME_SHARED->submitSeq;
	volatile MixerMeJob *job = &MIXER_ME_SHARED->jobs[submitSeq % MIXER_ME_QUEUE_DEPTH];

	job->cmdList = (uintptr_t)cmdList;
	job->auxData = (uintptr_t)auxData;
	job->cmdCount = (u32)cmdCount;
	meLibSync();

	MIXER_ME_SHARED->submitSeq = submitSeq + 1;
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
	if (g_MixerMePendingCount == 0) {
		return 0;
	}

	if (!mixerMeIsReady()) {
		g_MixerMeConsumedSeq = MIXER_ME_SHARED->doneSeq;
		g_MixerMePendingCount = 0;
		return 0;
	}

	u32 completed = mixerMeConsumeAvailable();

	while (completed == 0) {
		sceKernelDelayThread(100);
		completed = mixerMeConsumeAvailable();
	}

	return completed;
}

void mixerMeShutdown(void)
{
	if (!g_MixerMeInitialized) {
		return;
	}

	while (g_MixerMePendingCount != 0) {
		mixerMeWait();
	}

	if (mixerMeIsReady()) {
		MIXER_ME_SHARED->exitRequested = 1;
		meLibSync();

		for (int i = 0; i < 1000 && MIXER_ME_SHARED->ready; i++) {
			sceKernelDelayThread(1000);
		}
	}

	g_MixerMeReady = 0;
	g_MixerMeInitialized = 0;
	g_MixerMeConsumedSeq = 0;
	g_MixerMePendingCount = 0;
}

#else

int mixerMeInit(void)
{
	return 0;
}

int mixerMeIsReady(void)
{
	return 0;
}

void mixerMeSubmit(const Acmd *cmdList, const uintptr_t *auxData, s32 cmdCount)
{
	mixerExecCommandList(cmdList, auxData, cmdCount);
	(void)cmdList;
	(void)auxData;
	(void)cmdCount;
}

u32 mixerMeConsumeAvailable(void)
{
	return 0;
}

u32 mixerMeWait(void)
{
	return 0;
}

void mixerMeShutdown(void)
{
}

#endif
