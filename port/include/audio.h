#ifndef _IN_AUDIO_H
#define _IN_AUDIO_H

#include <PR/ultratypes.h>

s32 audioInit(void);
s32 audioSetFrequency(u32 frequency);
s32 audioStartProducer(void (*produceFrame)(void));
s32 audioRequestFrames(u32 count);
s32 audioGetBytesBuffered(void);
s32 audioGetSamplesBuffered(void);
void audioSetNextBuffer(const s16 *buf, u32 len);
void audioEndFrame(void);
void audioShutdown(void);

#endif
