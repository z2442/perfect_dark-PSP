#ifndef MIXER_MP3_H
#define MIXER_MP3_H

#include <stdint.h>
#include <PR/abi.h>
#include <PR/ultratypes.h>

/* Port-only opcode, outside the original 0..15 audio ABI. */
#define MIXER_CMD_MP3 0x10

#ifdef PD_PSP_AUDIO_ME
void mixerMp3BeginList(u32 slot);
void mixerMp3Queue(Acmd *cmd, const void *source, u32 size, void *out, int reset);
void mixerMp3Exec(const void *packet, void *out);
void *mixerMp3OutputBuffer(u32 slot);
#endif

#endif
