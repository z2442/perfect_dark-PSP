#include "psp_timing.h"

#ifdef __PSP__
#include <pspkernel.h>

#include <PR/ultratypes.h>

#define PD_PSP_VI_RATE_HZ 60U
#define PD_PSP_FRAME_BASE_USEC 16666U
#define PD_PSP_FRAME_REMAINDER 40U

static u32 g_NextFrameUsec;
static u32 g_FrameRemainder;
static s32 g_TimingInitialized;

static inline s32 pdPspTimeDiff(u32 a, u32 b)
{
	return (s32)(a - b);
}

void pdPspTimingReset(void)
{
	g_NextFrameUsec = 0;
	g_FrameRemainder = 0;
	g_TimingInitialized = 0;
}

void pdPspPaceFrame60(void)
{
	u32 frameusec = PD_PSP_FRAME_BASE_USEC;
	u32 now = sceKernelGetSystemTimeLow();
	s32 waitusec;

	g_FrameRemainder += PD_PSP_FRAME_REMAINDER;

	if (g_FrameRemainder >= PD_PSP_VI_RATE_HZ) {
		frameusec++;
		g_FrameRemainder -= PD_PSP_VI_RATE_HZ;
	}

	if (!g_TimingInitialized) {
		g_NextFrameUsec = now;
		g_TimingInitialized = 1;
	}

	waitusec = pdPspTimeDiff(g_NextFrameUsec, now);

	if (waitusec > 0) {
		sceKernelDelayThread((u32)waitusec);
	} else if (waitusec < 0) {
		/* A late frame starts a fresh interval; never run catch-up frames. */
		g_NextFrameUsec = now;
	}

	g_NextFrameUsec += frameusec;
}
#else
void pdPspTimingReset(void) {}
void pdPspPaceFrame60(void) {}
#endif
