#include <assert.h>
#include <stdint.h>

#include "mixer.h"
#include "mixer_cmd.h"

static Acmd *g_MixerCmdBase = NULL;
static uintptr_t *g_MixerCmdAux = NULL;

void mixerCmdListBegin(Acmd *base, uintptr_t *auxData)
{
	g_MixerCmdBase = base;
	g_MixerCmdAux = auxData;
}

void mixerCmdListEnd(void)
{
	g_MixerCmdBase = NULL;
	g_MixerCmdAux = NULL;
}

void mixerCmdSetAux(Acmd *cmd, uintptr_t value)
{
	assert(g_MixerCmdBase != NULL);
	assert(g_MixerCmdAux != NULL);
	g_MixerCmdAux[cmd - g_MixerCmdBase] = value;
}

void mixerExecCommandList(const Acmd *cmdList, const uintptr_t *auxData, s32 cmdCount)
{
	for (s32 i = 0; i < cmdCount; i++) {
		const Acmd *cmd = &cmdList[i];
		const uint32_t w0 = cmd->words.w0;
		const uint32_t w1 = cmd->words.w1;
		const uintptr_t aux = auxData ? auxData[i] : 0;

		switch (w0 >> 24) {
		case A_SPNOOP:
			aDisableImpl((uint16_t)(w0 & 0xffff), w1 >> 16, w1 & 0xffff);
			break;
		case A_ADPCM:
			aADPCMdecImpl((w1 >> 28) & 0xf, (int16_t *)(uintptr_t)aux,
					(w1 >> 16) & 0xfff, (w1 >> 12) & 0xf, w1 & 0xfff);
			break;
		case A_CLEARBUFF:
			aClearBufferImpl((uint16_t)(w0 & 0xffff), (int)w1);
			break;
		case A_ENVMIXER:
			aEnvMixerImpl((w0 >> 16) & 0xff, (int16_t *)(uintptr_t)w1, (int16_t)(w0 & 0xffff));
			break;
		case A_LOADBUFF:
			aLoadBufferImpl((const void *)(uintptr_t)w1, w0 & 0xfff, (w0 >> 12) & 0xfff);
			break;
		case A_RESAMPLE:
			aResampleImpl((w1 >> 30) & 0x3, (w1 >> 14) & 0xffff, (int16_t *)(uintptr_t)aux,
					(w1 >> 2) & 0xfff, w1 & 0x3);
			break;
		case A_SAVEBUFF:
			aSaveBufferImpl(w0 & 0xfff, (int16_t *)(uintptr_t)w1, (w0 >> 12) & 0xfff);
			break;
		case A_SETVOL:
			aSetVolumeImpl((w0 >> 16) & 0xff, (int16_t)(w0 & 0xffff),
					(int16_t)(w1 >> 16), (int16_t)(w1 & 0xffff));
			break;
		case A_DMEMMOVE:
			aDMEMMoveImpl((uint16_t)(w0 & 0xffff), (uint16_t)(w1 >> 16), w1 & 0xffff);
			break;
		case A_LOADADPCM:
			aLoadADPCMImpl(w0 & 0xffffff, (const int16_t *)(uintptr_t)w1);
			break;
		case A_MIXER:
			aMixImpl((w0 >> 16) & 0xff, (int16_t)(w0 & 0xffff), w1 >> 16, w1 & 0xffff);
			break;
		case A_INTERLEAVE:
			aInterleaveImpl();
			break;
		case A_POLEF:
			aPoleFilterImpl((w0 >> 16) & 0xff, (int16_t)(w0 & 0xffff), w1 >> 24, w1 & 0xffffff);
			break;
		case A_SETLOOP:
			aSetLoopImpl((ADPCM_STATE *)(uintptr_t)w1);
			break;
		default:
			break;
		}
	}
}
