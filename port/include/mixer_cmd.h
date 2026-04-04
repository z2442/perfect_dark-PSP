#ifndef MIXER_CMD_H
#define MIXER_CMD_H

#include <stdint.h>
#include <PR/abi.h>
#include <PR/ultratypes.h>

void mixerCmdListBegin(Acmd *base, uintptr_t *auxData);
void mixerCmdListEnd(void);
void mixerCmdSetAux(Acmd *cmd, uintptr_t value);

void mixerExecCommandList(const Acmd *cmdList, const uintptr_t *auxData, s32 cmdCount);

#endif
