#include <PR/ultratypes.h>
#include <stdio.h>
#include <pspaudio.h>
#include <pspaudiolib.h>
#include <pspkernel.h>
#include "platform.h"
#include "config.h"
#include "audio.h"
#include "system.h"
#include <string.h>


/* Game‑side audio settings */
static int  audioChan   = -1;      /* PSP channel handle */
static s32  bufferSize  = 512;     /* game frames at 22 kHz */
static s32  queueLimit  = 8192;
static s32  pspFrames   = 0;       /* 44 kHz frames sent to HW each call */

/* --- Non‑blocking audio ring buffer ----------------------------------- */
#define AUDIO_RING_FRAMES   32768   /* 0.74 s of audio at 44 kHz */
#define AUDIO_FRAME_SAMPLES 2                /* L + R */
static s16 audioRingBuf[AUDIO_RING_FRAMES * AUDIO_FRAME_SAMPLES];
/* read/write indices expressed in *frames* (not samples) */
static volatile u32 ringRead  = 0;
static volatile u32 ringWrite = 0;
/* State for simple 2× linear interpolation */
static s16 prevLeft  = 0;
static s16 prevRight = 0;
static int  havePrev = 0;
/* ---------------------------------------------------------------------- */

/* --- Audio thread ---------------------------------------------------- */
static SceUID audioThreadId   = -1;
static volatile int audioThreadRunning = 0;
/* --------------------------------------------------------------------- */

/* Thread that continuously feeds PSP audio */
static int audioThread(SceSize args, void *argp)
{
    static s16 mixBuf[4096];                 /* pspFrames (≤2048) * stereo */
    while (audioThreadRunning)
    {
        u32 needFrames = pspFrames;          /* 44 kHz frame count */
        for (u32 f = 0; f < needFrames; ++f)
        {
            u32 offMix  = f * AUDIO_FRAME_SAMPLES;
            if (ringRead != ringWrite)
            {
                u32 offRing = ringRead * AUDIO_FRAME_SAMPLES;
                mixBuf[offMix]     = audioRingBuf[offRing];
                mixBuf[offMix + 1] = audioRingBuf[offRing + 1];
                ringRead = (ringRead + 1) % AUDIO_RING_FRAMES;
            }
            else
            {
                /* Silence when underrun */
                mixBuf[offMix]     = 0;
                mixBuf[offMix + 1] = 0;
            }
        }
        sceAudioOutputBlocking(audioChan,
                               PSP_AUDIO_VOLUME_MAX,
                               mixBuf);
    }
    sceKernelExitDeleteThread(0);
    return 0;
}

s32 audioInit(void)
{
    pspFrames = bufferSize * 2;                /* we upsample 2× */
    int chan = sceAudioChReserve(-1, pspFrames, PSP_AUDIO_FORMAT_STEREO); /* auto-allocate channel */
    if (chan < 0) {
        sysLogPrintf(LOG_ERROR, "Failed to reserve audio channel");
        return -1;
    }
    audioChan = chan;
    /* bufferSize remains the *game* frame count (22 kHz), pspFrames is 44 kHz */
    ringRead  = ringWrite = 0;
    memset(audioRingBuf, 0, sizeof(audioRingBuf));

    /* Launch async audio thread */
    audioThreadRunning = 1;
    audioThreadId = sceKernelCreateThread("AsyncAudio", audioThread,
                                          17, 0x10000, 0, NULL);
    if (audioThreadId >= 0)
        sceKernelStartThread(audioThreadId, 0, NULL);

    return 0;
}

// PSP audio does not expose direct buffer size queries
s32 audioGetBytesBuffered(void)
{
    return 0;
}

s32 audioGetSamplesBuffered(void)
{
    u32 frames = (ringWrite >= ringRead)
                   ? (ringWrite - ringRead)
                   : (AUDIO_RING_FRAMES - ringRead + ringWrite);
    return frames * AUDIO_FRAME_SAMPLES;   /* convert frames -> samples (stereo) */
}

void audioSetNextBuffer(const s16 *buf, u32 len)
{
    /* lenBytes is BYTES of interleaved stereo S16 samples at 22 050 Hz (4 bytes per frame) */
    u32 inFrames = len / (sizeof(s16) * AUDIO_FRAME_SAMPLES);  /* len / 4 */

    for (u32 i = 0; i < inFrames; ++i)
    {
        s16 currLeft  = buf[i * 2];
        s16 currRight = buf[i * 2 + 1];

        /* If we have a previous frame, output an interpolated frame first */
        if (havePrev)
        {
            u32 nextWrite = (ringWrite + 1) % AUDIO_RING_FRAMES;
            if (nextWrite == ringRead)
                return;                       /* ring full – drop remainder */

            u32 off = ringWrite * AUDIO_FRAME_SAMPLES;
            audioRingBuf[off]     = (prevLeft  + currLeft)  >> 1;   /* simple average */
            audioRingBuf[off + 1] = (prevRight + currRight) >> 1;
            ringWrite = nextWrite;
        }
        else
        {
            havePrev = 1; /* first frame ever seen */
        }

        /* Now output the current frame */
        {
            u32 nextWrite = (ringWrite + 1) % AUDIO_RING_FRAMES;
            if (nextWrite == ringRead)
                return;

            u32 off = ringWrite * AUDIO_FRAME_SAMPLES;
            audioRingBuf[off]     = currLeft;
            audioRingBuf[off + 1] = currRight;
            ringWrite = nextWrite;
        }

        /* Save for next interpolation */
        prevLeft  = currLeft;
        prevRight = currRight;
    }
}

void audioEndFrame(void)
{
    /* Async thread handles output */
}

PD_CONSTRUCTOR static void audioConfigInit(void)
{
	configRegisterInt("Audio.BufferSize", &bufferSize, 0, 512 * 1024);
	configRegisterInt("Audio.QueueLimit", &queueLimit, 0, 512 * 1024);
}
