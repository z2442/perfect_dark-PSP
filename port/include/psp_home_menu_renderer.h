#ifndef PD_PSP_HOME_MENU_RENDERER_H
#define PD_PSP_HOME_MENU_RENDERER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void pdPspHomeMenuRendererSetActive(int active);
void pdPspHomeMenuRendererRequestBackground(void);
void pdPspHomeMenuRendererRender(int selected, int screen, int controlSelected,
		const char *status, uint8_t red, uint8_t green, uint8_t blue);
void pdPspAssetProgressRender(uint32_t permille, const char *status);

#ifdef __cplusplus
}
#endif

#endif
