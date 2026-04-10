#include "mixer_me.h"

#include "mixer_cmd.h"

#ifdef PD_PSP_AUDIO_ME

#ifndef asm
#define asm __asm__
#endif

#include <pspkernel.h>
#include <me-core-mapper/me-core.h>

typedef struct MixerMeShared_s {
	volatile u32 submitSeq;
	volatile u32 doneSeq;
	volatile u32 exitRequested;
	volatile u32 ready;
	volatile uintptr_t cmdList;
	volatile uintptr_t auxData;
	volatile u32 cmdCount;
} MixerMeShared;

meLibMakeUncachedMem(g_MixerMeSharedStorage, 8, u32Me);
#define MIXER_ME_SHARED ((volatile MixerMeShared *)(uintptr_t)(UNCACHED_USER_MASK | (u32Me)g_MixerMeSharedStorage))

static int g_MixerMeInitialized = 0;
static int g_MixerMeReady = 0;
static int g_MixerMePending = 0;

__attribute__((noinline, aligned(4)))
void meLibOnProcess(void)
{
	MIXER_ME_SHARED->ready = 1;
	MIXER_ME_SHARED->doneSeq = 0;

	while (!MIXER_ME_SHARED->exitRequested) {
		if (MIXER_ME_SHARED->submitSeq == MIXER_ME_SHARED->doneSeq) {
			meLibDelayPipeline();
			continue;
		}

		meCoreDcacheWritebackInvalidateAll();
		mixerExecCommandList((const Acmd *)(uintptr_t)MIXER_ME_SHARED->cmdList,
				(const uintptr_t *)(uintptr_t)MIXER_ME_SHARED->auxData,
				MIXER_ME_SHARED->cmdCount);
		meCoreDcacheWritebackInvalidateAll();

		meLibSync();
		MIXER_ME_SHARED->doneSeq = MIXER_ME_SHARED->submitSeq;
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
	MIXER_ME_SHARED->cmdList = 0;
	MIXER_ME_SHARED->auxData = 0;
	MIXER_ME_SHARED->cmdCount = 0;

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
		g_MixerMePending = 0;
		return;
	}

	sceKernelDcacheWritebackInvalidateAll();

	MIXER_ME_SHARED->cmdList = (uintptr_t)cmdList;
	MIXER_ME_SHARED->auxData = (uintptr_t)auxData;
	MIXER_ME_SHARED->cmdCount = (u32)cmdCount;
	meLibSync();

	MIXER_ME_SHARED->submitSeq++;
	meLibSync();
	g_MixerMePending = 1;
}

void mixerMeWait(void)
{
	if (!g_MixerMePending) {
		return;
	}

	if (!mixerMeIsReady()) {
		g_MixerMePending = 0;
		return;
	}

	const u32 target = MIXER_ME_SHARED->submitSeq;

	while (MIXER_ME_SHARED->doneSeq != target) {
		sceKernelDelayThread(100);
	}

	sceKernelDcacheWritebackInvalidateAll();
	g_MixerMePending = 0;
}

void mixerMeShutdown(void)
{
	if (!g_MixerMeInitialized) {
		return;
	}

	mixerMeWait();

	if (mixerMeIsReady()) {
		MIXER_ME_SHARED->exitRequested = 1;
		meLibSync();

		for (int i = 0; i < 1000 && MIXER_ME_SHARED->ready; i++) {
			sceKernelDelayThread(1000);
		}
	}

	g_MixerMeReady = 0;
	g_MixerMeInitialized = 0;
	g_MixerMePending = 0;
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

void mixerMeWait(void)
{
}

void mixerMeShutdown(void)
{
}

#endif
