#include "audio.h"

#include "system.h"

#include <PR/ultratypes.h>
#include <pspaudio.h>
#include <pspkernel.h>
#include <stdbool.h>
#include <string.h>

#define AUDIO_CHANNELS 2U
#define AUDIO_SOURCE_FREQUENCY 22050U
#define AUDIO_OUTPUT_FREQUENCY 44100U
#define AUDIO_OUTPUT_CHUNK_FRAMES 768U
#define AUDIO_SOURCE_CHUNK_FRAMES \
	((AUDIO_OUTPUT_CHUNK_FRAMES * AUDIO_SOURCE_FREQUENCY + AUDIO_OUTPUT_FREQUENCY - 1U) / \
		AUDIO_OUTPUT_FREQUENCY)
#define AUDIO_RING_FRAMES 16384U
#define AUDIO_RING_MASK (AUDIO_RING_FRAMES - 1U)
#define AUDIO_TARGET_CHUNKS 6U
#define AUDIO_STARTUP_CHUNKS 4U
#define AUDIO_URGENT_CHUNKS 3U
#define AUDIO_RECOVERY_CHUNKS 2U
#define AUDIO_MAX_PRIME_UPDATES (AUDIO_STARTUP_CHUNKS + 2U)
#define AUDIO_MAX_CATCHUP_UPDATES 2U
#define AUDIO_UPDATE_USEC (1000000U / 60U)

#define AUDIO_GAME_THREAD_PRIORITY 0x20
#define AUDIO_OUTPUT_THREAD_PRIORITY (AUDIO_GAME_THREAD_PRIORITY - 2)
#define AUDIO_PRODUCER_THREAD_PRIORITY AUDIO_GAME_THREAD_PRIORITY
#define AUDIO_PRODUCER_URGENT_PRIORITY (AUDIO_GAME_THREAD_PRIORITY - 1)

#if (AUDIO_RING_FRAMES & (AUDIO_RING_FRAMES - 1)) != 0
#error AUDIO_RING_FRAMES must be a power of two
#endif

static s16 g_AudioRing[AUDIO_RING_FRAMES * AUDIO_CHANNELS] __attribute__((aligned(64)));
static s16 g_AudioSrcMix[2][AUDIO_SOURCE_CHUNK_FRAMES * AUDIO_CHANNELS] __attribute__((aligned(64)));
static s16 g_AudioFallbackMix[2][AUDIO_OUTPUT_CHUNK_FRAMES * AUDIO_CHANNELS] __attribute__((aligned(64)));

static volatile u32 g_AudioReadPos;
static volatile u32 g_AudioWritePos;
static volatile s32 g_AudioOutputRunning;
static volatile s32 g_AudioProducerRunning;
static volatile s32 g_AudioInitialized;
static volatile s32 g_AudioPlaybackPrimed;
static volatile s32 g_AudioPlaybackStarted;

static SceUID g_AudioOutputThread = -1;
static SceUID g_AudioProducerThread = -1;
static s32 g_AudioChannel = -1;
static s32 g_AudioHardwareSrc;
static void (*g_AudioProduceFrame)(void);

static inline u32 audioBufferedFrames(void)
{
	return g_AudioWritePos - g_AudioReadPos;
}

static inline u32 audioTargetFrames(void)
{
	return AUDIO_TARGET_CHUNKS * AUDIO_SOURCE_CHUNK_FRAMES;
}

static inline u32 audioStartupFrames(void)
{
	return AUDIO_STARTUP_CHUNKS * AUDIO_SOURCE_CHUNK_FRAMES;
}

static inline u32 audioUrgentFrames(void)
{
	return AUDIO_URGENT_CHUNKS * AUDIO_SOURCE_CHUNK_FRAMES;
}

static inline u32 audioRecoveryFrames(void)
{
	return AUDIO_RECOVERY_CHUNKS * AUDIO_SOURCE_CHUNK_FRAMES;
}

static inline u32 audioReportableFrames(void)
{
	u32 buffered = audioBufferedFrames();
	u32 target = audioTargetFrames();

	return buffered > target ? buffered - target : 0;
}

static void audioCopyFromRing(s16 *dst, u32 readpos, u32 frames)
{
	u32 first = frames;
	u32 index = readpos & AUDIO_RING_MASK;

	if (first > AUDIO_RING_FRAMES - index) {
		first = AUDIO_RING_FRAMES - index;
	}

	memcpy(dst, &g_AudioRing[index * AUDIO_CHANNELS], first * AUDIO_CHANNELS * sizeof(s16));

	if (first != frames) {
		memcpy(dst + first * AUDIO_CHANNELS, g_AudioRing,
				(frames - first) * AUDIO_CHANNELS * sizeof(s16));
	}
}

static void audioUpsampleFallback(s16 *dst, const s16 *src)
{
	for (u32 i = 0; i < AUDIO_SOURCE_CHUNK_FRAMES; i++) {
		const s16 left = src[i * 2];
		const s16 right = src[i * 2 + 1];
		dst[i * 4] = left;
		dst[i * 4 + 1] = right;
		dst[i * 4 + 2] = left;
		dst[i * 4 + 3] = right;
	}
}

static int audioOutputMain(SceSize args, void *argp)
{
	u32 mixindex = 0;
	(void)args;
	(void)argp;

	while (g_AudioOutputRunning) {
		s16 *srcmix = g_AudioSrcMix[mixindex];
		u32 readpos = g_AudioReadPos;
		u32 available = audioBufferedFrames();
		u32 consumed = 0;
		u32 primeframes = g_AudioPlaybackStarted ? audioRecoveryFrames() : audioStartupFrames();
		s32 result;

		if (!g_AudioPlaybackPrimed && available >= primeframes) {
			g_AudioPlaybackPrimed = true;
			g_AudioPlaybackStarted = true;
		}

		if (g_AudioPlaybackPrimed && available >= AUDIO_SOURCE_CHUNK_FRAMES) {
			audioCopyFromRing(srcmix, readpos, AUDIO_SOURCE_CHUNK_FRAMES);
			consumed = AUDIO_SOURCE_CHUNK_FRAMES;
		} else {
			/* Once starved, rebuild a reserve instead of alternating short PCM
			 * blocks with silence on every hardware submission. */
			g_AudioPlaybackPrimed = false;
			memset(srcmix, 0, sizeof(g_AudioSrcMix[0]));
		}

		__sync_synchronize();
		g_AudioReadPos = readpos + consumed;

		if (g_AudioHardwareSrc) {
			sceKernelDcacheWritebackRange(srcmix, sizeof(g_AudioSrcMix[0]));
			result = sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, srcmix);
		} else {
			s16 *outmix = g_AudioFallbackMix[mixindex];
			audioUpsampleFallback(outmix, srcmix);
			sceKernelDcacheWritebackRange(outmix, sizeof(g_AudioFallbackMix[0]));
			result = sceAudioOutputBlocking(g_AudioChannel, PSP_AUDIO_VOLUME_MAX, outmix);
		}

		if (result < 0) {
			g_AudioPlaybackPrimed = false;
			sceKernelDelayThread(1000);
		}
		mixindex ^= 1;
	}

	sceKernelExitThread(0);
	return 0;
}

static int audioProducerMain(SceSize args, void *argp)
{
	u32 nextupdate;
	u32 updates;
	s32 priorityboosted = false;

	(void)args;
	(void)argp;

	/* Build the initial reserve before the output thread begins consuming it.
	 * ME completions are published by amgrFrame, so a few extra updates are
	 * permitted while the small ME queue drains. */
	for (updates = 0; g_AudioProducerRunning && updates < AUDIO_MAX_PRIME_UPDATES
			&& audioBufferedFrames() < audioStartupFrames(); updates++) {
		g_AudioProduceFrame();
	}

	nextupdate = sceKernelGetSystemTimeLow();

	while (g_AudioProducerRunning) {
		u32 now;
		s32 delayusec;
		s32 missedupdate = false;

		nextupdate += AUDIO_UPDATE_USEC;
		now = sceKernelGetSystemTimeLow();
		delayusec = (s32)(nextupdate - now);

		if (delayusec > 0) {
			sceKernelDelayThread((u32)delayusec);
			now = sceKernelGetSystemTimeLow();
			delayusec = (s32)(nextupdate - now);
		}

		if (delayusec < -(s32)AUDIO_UPDATE_USEC) {
			missedupdate = true;
			nextupdate = now;
		}

		if (!priorityboosted && audioBufferedFrames() < audioUrgentFrames()) {
			if (sceKernelChangeThreadPriority(sceKernelGetThreadId(),
					AUDIO_PRODUCER_URGENT_PRIORITY) >= 0) {
				priorityboosted = true;
			}
		} else if (priorityboosted && audioBufferedFrames() >= audioTargetFrames()) {
			if (sceKernelChangeThreadPriority(sceKernelGetThreadId(),
					AUDIO_PRODUCER_THREAD_PRIORITY) >= 0) {
				priorityboosted = false;
			}
		}

		if (g_AudioProducerRunning && g_AudioProduceFrame != NULL) {
			u32 maxupdates = missedupdate ? AUDIO_MAX_CATCHUP_UPDATES : 1U;

			for (updates = 0; updates < maxupdates && g_AudioProducerRunning; updates++) {
				g_AudioProduceFrame();
			}
		}
	}

	if (priorityboosted) {
		sceKernelChangeThreadPriority(sceKernelGetThreadId(), AUDIO_PRODUCER_THREAD_PRIORITY);
	}

	sceKernelExitThread(0);
	return 0;
}

s32 audioInit(void)
{
	s32 result;

	if (g_AudioInitialized) {
		return 0;
	}

	g_AudioReadPos = 0;
	g_AudioWritePos = 0;
	g_AudioPlaybackPrimed = false;
	g_AudioPlaybackStarted = false;
	memset(g_AudioRing, 0, sizeof(g_AudioRing));

	result = sceAudioSRCChReserve(AUDIO_SOURCE_CHUNK_FRAMES, AUDIO_SOURCE_FREQUENCY, AUDIO_CHANNELS);
	if (result >= 0) {
		g_AudioHardwareSrc = true;
	} else {
		g_AudioChannel = sceAudioChReserve(-1, AUDIO_OUTPUT_CHUNK_FRAMES, PSP_AUDIO_FORMAT_STEREO);
		if (g_AudioChannel < 0) {
			sysLogPrintf(LOG_ERROR, "Unable to reserve PSP audio output");
			return -1;
		}
		g_AudioHardwareSrc = false;
	}

	g_AudioOutputRunning = true;
	g_AudioOutputThread = sceKernelCreateThread("PD Audio Output", audioOutputMain,
			AUDIO_OUTPUT_THREAD_PRIORITY, 0x10000, PSP_THREAD_ATTR_VFPU, NULL);
	if (g_AudioOutputThread < 0 || sceKernelStartThread(g_AudioOutputThread, 0, NULL) < 0) {
		g_AudioOutputRunning = false;
		if (g_AudioHardwareSrc) {
			sceAudioSRCChRelease();
		} else if (g_AudioChannel >= 0) {
			sceAudioChRelease(g_AudioChannel);
			g_AudioChannel = -1;
		}
		sysLogPrintf(LOG_ERROR, "Unable to start PSP audio output thread");
		return -1;
	}

	g_AudioInitialized = true;
	return 0;
}

s32 audioSetFrequency(u32 frequency)
{
	(void)frequency;
	return AUDIO_SOURCE_FREQUENCY;
}

s32 audioStartProducer(void (*produceFrame)(void))
{
	if (!g_AudioInitialized || produceFrame == NULL) {
		return -1;
	}
	if (g_AudioProducerThread >= 0) {
		return 0;
	}

	g_AudioProduceFrame = produceFrame;
	g_AudioProducerRunning = true;
	g_AudioProducerThread = sceKernelCreateThread("PD Audio Producer", audioProducerMain,
			AUDIO_PRODUCER_THREAD_PRIORITY, 0x18000, PSP_THREAD_ATTR_VFPU, NULL);
	if (g_AudioProducerThread < 0 || sceKernelStartThread(g_AudioProducerThread, 0, NULL) < 0) {
		g_AudioProducerRunning = false;
		g_AudioProducerThread = -1;
		return -1;
	}

	return 0;
}

s32 audioRequestFrames(u32 count)
{
	(void)count;

	if (!g_AudioProducerRunning) {
		return -1;
	}

	/* Compatibility hook for the scheduler. Production is intentionally
	 * clocked by the audio thread rather than by rendered game frames. */
	return 0;
}

s32 audioGetBytesBuffered(void)
{
	return (s32)(audioReportableFrames() * AUDIO_CHANNELS * sizeof(s16));
}

s32 audioGetSamplesBuffered(void)
{
	return (s32)(audioReportableFrames() * AUDIO_CHANNELS);
}

void audioSetNextBuffer(const s16 *buf, u32 len)
{
	u32 frames = len / (AUDIO_CHANNELS * sizeof(s16));
	u32 writepos = g_AudioWritePos;
	u32 freeframes = AUDIO_RING_FRAMES - audioBufferedFrames();
	u32 first;
	u32 index;

	if (frames > freeframes) {
		frames = freeframes;
	}
	if (frames == 0) {
		return;
	}

	index = writepos & AUDIO_RING_MASK;
	first = frames;
	if (first > AUDIO_RING_FRAMES - index) {
		first = AUDIO_RING_FRAMES - index;
	}

	memcpy(&g_AudioRing[index * AUDIO_CHANNELS], buf, first * AUDIO_CHANNELS * sizeof(s16));
	if (first != frames) {
		memcpy(g_AudioRing, buf + first * AUDIO_CHANNELS,
				(frames - first) * AUDIO_CHANNELS * sizeof(s16));
	}

	__sync_synchronize();
	g_AudioWritePos = writepos + frames;
}

void audioEndFrame(void)
{
}

void audioShutdown(void)
{
	if (g_AudioProducerThread >= 0) {
		g_AudioProducerRunning = false;
		sceKernelWaitThreadEnd(g_AudioProducerThread, NULL);
		sceKernelDeleteThread(g_AudioProducerThread);
		g_AudioProducerThread = -1;
	}

	if (g_AudioOutputThread >= 0) {
		g_AudioOutputRunning = false;
		sceKernelWaitThreadEnd(g_AudioOutputThread, NULL);
		sceKernelDeleteThread(g_AudioOutputThread);
		g_AudioOutputThread = -1;
	}

	if (g_AudioHardwareSrc) {
		sceAudioSRCChRelease();
		g_AudioHardwareSrc = false;
	} else if (g_AudioChannel >= 0) {
		sceAudioChRelease(g_AudioChannel);
		g_AudioChannel = -1;
	}

	g_AudioProduceFrame = NULL;
	g_AudioInitialized = false;
}
