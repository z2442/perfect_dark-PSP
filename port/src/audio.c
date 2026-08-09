#include "audio.h"

#include "system.h"

#include <PR/ultratypes.h>
#include <pspaudio.h>
#include <pspkernel.h>
#include <stdbool.h>
#include <string.h>

#define AUDIO_CHANNELS 2U
#define AUDIO_SOURCE_FREQUENCY 22050U
#define AUDIO_CHUNK_FRAMES 768U
#define AUDIO_RING_FRAMES 16384U
#define AUDIO_RING_MASK (AUDIO_RING_FRAMES - 1U)
#define AUDIO_STARTUP_CHUNKS 2U

#define AUDIO_GAME_THREAD_PRIORITY 0x20
#define AUDIO_OUTPUT_THREAD_PRIORITY (AUDIO_GAME_THREAD_PRIORITY - 2)
#define AUDIO_PRODUCER_THREAD_PRIORITY AUDIO_GAME_THREAD_PRIORITY

#if (AUDIO_RING_FRAMES & (AUDIO_RING_FRAMES - 1)) != 0
#error AUDIO_RING_FRAMES must be a power of two
#endif

static s16 g_AudioRing[AUDIO_RING_FRAMES * AUDIO_CHANNELS] __attribute__((aligned(64)));
static s16 g_AudioSrcMix[2][AUDIO_CHUNK_FRAMES * AUDIO_CHANNELS] __attribute__((aligned(64)));
static s16 g_AudioFallbackMix[2][AUDIO_CHUNK_FRAMES * 2U * AUDIO_CHANNELS] __attribute__((aligned(64)));

static volatile u32 g_AudioReadPos;
static volatile u32 g_AudioWritePos;
static volatile s32 g_AudioOutputRunning;
static volatile s32 g_AudioProducerRunning;
static volatile s32 g_AudioInitialized;

static SceUID g_AudioOutputThread = -1;
static SceUID g_AudioProducerThread = -1;
static SceUID g_AudioProducerSema = -1;
static s32 g_AudioChannel = -1;
static s32 g_AudioHardwareSrc;
static void (*g_AudioProduceFrame)(void);

static inline u32 audioBufferedFrames(void)
{
	return g_AudioWritePos - g_AudioReadPos;
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
	for (u32 i = 0; i < AUDIO_CHUNK_FRAMES; i++) {
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
		u32 buffered = audioBufferedFrames();

		if (buffered < AUDIO_STARTUP_CHUNKS * AUDIO_CHUNK_FRAMES) {
			sceKernelDelayThread(1000);
			continue;
		}

		while (g_AudioOutputRunning) {
			s16 *srcmix = g_AudioSrcMix[mixindex];
			u32 readpos = g_AudioReadPos;
			u32 available = audioBufferedFrames();
			u32 copied = available < AUDIO_CHUNK_FRAMES ? available : AUDIO_CHUNK_FRAMES;
			s32 result;

			if (copied != 0) {
				audioCopyFromRing(srcmix, readpos, copied);
			}
			if (copied < AUDIO_CHUNK_FRAMES) {
				memset(srcmix + copied * AUDIO_CHANNELS, 0,
						(AUDIO_CHUNK_FRAMES - copied) * AUDIO_CHANNELS * sizeof(s16));
			}

			__sync_synchronize();
			g_AudioReadPos = readpos + copied;

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
				sceKernelDelayThread(1000);
			}
			mixindex ^= 1;
		}
	}

	sceKernelExitThread(0);
	return 0;
}

static int audioProducerMain(SceSize args, void *argp)
{
	(void)args;
	(void)argp;

	while (g_AudioProducerRunning) {
		if (sceKernelWaitSema(g_AudioProducerSema, 1, NULL) < 0) {
			break;
		}

		if (g_AudioProducerRunning && g_AudioProduceFrame != NULL) {
			g_AudioProduceFrame();
		}
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
	memset(g_AudioRing, 0, sizeof(g_AudioRing));

	result = sceAudioSRCChReserve(AUDIO_CHUNK_FRAMES, AUDIO_SOURCE_FREQUENCY, AUDIO_CHANNELS);
	if (result >= 0) {
		g_AudioHardwareSrc = true;
	} else {
		g_AudioChannel = sceAudioChReserve(-1, AUDIO_CHUNK_FRAMES * 2U, PSP_AUDIO_FORMAT_STEREO);
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
	g_AudioProducerSema = sceKernelCreateSema("PD Audio Updates", 0, 0, 16, NULL);
	if (g_AudioProducerSema < 0) {
		return -1;
	}

	g_AudioProducerRunning = true;
	g_AudioProducerThread = sceKernelCreateThread("PD Audio Producer", audioProducerMain,
			AUDIO_PRODUCER_THREAD_PRIORITY, 0x18000, PSP_THREAD_ATTR_VFPU, NULL);
	if (g_AudioProducerThread < 0 || sceKernelStartThread(g_AudioProducerThread, 0, NULL) < 0) {
		g_AudioProducerRunning = false;
		sceKernelDeleteSema(g_AudioProducerSema);
		g_AudioProducerSema = -1;
		g_AudioProducerThread = -1;
		return -1;
	}

	return 0;
}

s32 audioRequestFrames(u32 count)
{
	if (!g_AudioProducerRunning || g_AudioProducerSema < 0) {
		return -1;
	}

	if (count > 8) {
		count = 8;
	}

	while (count-- != 0) {
		if (sceKernelSignalSema(g_AudioProducerSema, 1) < 0) {
			break;
		}
	}
	return 0;
}

s32 audioGetBytesBuffered(void)
{
	return (s32)(audioBufferedFrames() * AUDIO_CHANNELS * sizeof(s16));
}

s32 audioGetSamplesBuffered(void)
{
	return (s32)(audioBufferedFrames() * AUDIO_CHANNELS);
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
		sceKernelSignalSema(g_AudioProducerSema, 1);
		sceKernelWaitThreadEnd(g_AudioProducerThread, NULL);
		sceKernelDeleteThread(g_AudioProducerThread);
		g_AudioProducerThread = -1;
	}
	if (g_AudioProducerSema >= 0) {
		sceKernelDeleteSema(g_AudioProducerSema);
		g_AudioProducerSema = -1;
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
