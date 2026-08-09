#ifndef PD_PSP_HOME_MENU_H
#define PD_PSP_HOME_MENU_H

#include <PR/ultratypes.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum PdPspHomeMenuResult {
	PD_PSP_HOME_MENU_NONE,
	PD_PSP_HOME_MENU_EXIT_GAME,
} PdPspHomeMenuResult;

void pdPspHomeMenuInit(void);
void pdPspHomeMenuPoll(void);
s32 pdPspHomeMenuIsOpen(void);
PdPspHomeMenuResult pdPspHomeMenuRunFrame(void);

int pdPspHomeMenuGetBindingCount(void);
const char *pdPspHomeMenuGetBindingName(int index);
void pdPspHomeMenuGetBindingValue(int index, char *buffer, size_t size);
int pdPspHomeMenuGetDeadzone(void);

#ifdef __cplusplus
}
#endif

#endif
