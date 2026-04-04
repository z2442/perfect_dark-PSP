#ifndef MIXER_ME_H
#define MIXER_ME_H

#include <stdint.h>
#include <PR/abi.h>
#include <PR/ultratypes.h>

int mixerMeInit(void);
int mixerMeIsReady(void);
void mixerMeSubmit(const Acmd *cmdList, const uintptr_t *auxData, s32 cmdCount);
void mixerMeWait(void);
void mixerMeShutdown(void);

#endif
