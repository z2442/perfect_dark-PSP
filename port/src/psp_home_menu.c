#include "psp_home_menu.h"

#include <PR/os_thread.h>
#include "game/menu.h"
#include "constants.h"
#include "input.h"
#include "psp_home_menu_renderer.h"

#include <PR/ultratypes.h>
#include <pspctrl.h>
#include <pspimpose_driver.h>
#include <pspkernel.h>
#include <stdio.h>
#include <string.h>

#define HOME_ITEM_RESUME 0
#define HOME_ITEM_MAPPING 1
#define HOME_ITEM_EXIT 2
#define HOME_ITEM_COUNT 3

#define HOME_SCREEN_MAIN 0
#define HOME_SCREEN_MAPPING 1

#define HOME_INPUT_LOCKOUT_USEC 500000U
#define HOME_INPUT_DEBOUNCE_USEC 150000U
#define HOME_STATUS_FRAMES 120
#define HOME_BUTTON_MASK \
	(PSP_CTRL_CIRCLE | PSP_CTRL_CROSS | PSP_CTRL_START | PSP_CTRL_UP | PSP_CTRL_DOWN | \
	 PSP_CTRL_LEFT | PSP_CTRL_RIGHT)

typedef struct PdPspHomeBinding {
	const char *name;
	u32 contkey;
} PdPspHomeBinding;

typedef struct PdPspPhysicalButton {
	const char *name;
	u32 virtkey;
} PdPspPhysicalButton;

static const PdPspHomeBinding g_HomeBindings[] = {
	{ "A Button", CK_A },
	{ "B Button", CK_B },
	{ "Z Trigger", CK_ZTRIG },
	{ "Start", CK_START },
	{ "L Trigger", CK_LTRIG },
	{ "R Trigger", CK_RTRIG },
	{ "C Up", CK_C_U },
	{ "C Down", CK_C_D },
	{ "C Left", CK_C_L },
	{ "C Right", CK_C_R },
};

static const PdPspPhysicalButton g_PhysicalButtons[] = {
	{ "None", 0 },
	{ "Cross", VK_JOY1_BEGIN + 0 },
	{ "Circle", VK_JOY1_BEGIN + 1 },
	{ "Square", VK_JOY1_BEGIN + 2 },
	{ "Triangle", VK_JOY1_BEGIN + 3 },
	{ "Select", VK_JOY1_BEGIN + 4 },
	{ "Start", VK_JOY1_BEGIN + 6 },
	{ "L Trigger", VK_JOY1_BEGIN + 30 },
	{ "R Trigger", VK_JOY1_BEGIN + 31 },
	{ "D-Pad Up", VK_JOY1_BEGIN + 11 },
	{ "D-Pad Down", VK_JOY1_BEGIN + 12 },
	{ "D-Pad Left", VK_JOY1_BEGIN + 13 },
	{ "D-Pad Right", VK_JOY1_BEGIN + 14 },
};

static s32 g_HomeActive;
static int g_HomeSelected;
static int g_HomeScreen;
static int g_HomeControlSelected;
static int g_HomeStatusTimer;
static u32 g_HomeButtons;
static u32 g_HomeLastButtons;
static u32 g_HomeLastPolledButtons;
static u32 g_HomeLockoutStart;
static u32 g_HomeLastInput;
static char g_HomeStatus[64];

static u32 pdPspHomeReadButtons(void)
{
	SceCtrlData pad = { 0 };

	if (sceCtrlPeekBufferPositive(&pad, 1) > 0) {
		g_HomeButtons = pad.Buttons;
	}
	return g_HomeButtons;
}

static int pdPspHomeDeadzoneRow(void) { return pdPspHomeMenuGetBindingCount(); }
static int pdPspHomeSaveRow(void) { return pdPspHomeMenuGetBindingCount() + 1; }
static int pdPspHomeResetRow(void) { return pdPspHomeMenuGetBindingCount() + 2; }
static int pdPspHomeBackRow(void) { return pdPspHomeMenuGetBindingCount() + 3; }
static int pdPspHomeRowCount(void) { return pdPspHomeMenuGetBindingCount() + 4; }

static void pdPspHomeSetStatus(const char *status)
{
	snprintf(g_HomeStatus, sizeof(g_HomeStatus), "%s", status);
	g_HomeStatusTimer = HOME_STATUS_FRAMES;
}

static void pdPspHomeOpen(void)
{
	g_HomeActive = 1;
	g_HomeSelected = HOME_ITEM_RESUME;
	g_HomeScreen = HOME_SCREEN_MAIN;
	g_HomeControlSelected = 0;
	g_HomeStatusTimer = 0;
	g_HomeStatus[0] = '\0';
	g_HomeLockoutStart = sceKernelGetSystemTimeLow();
	g_HomeLastInput = g_HomeLockoutStart;
	g_HomeLastButtons = g_HomeButtons;
	pdPspHomeMenuRendererSetActive(1);
	pdPspHomeMenuRendererRequestBackground();
}

static void pdPspHomeClose(void)
{
	g_HomeActive = 0;
	g_HomeScreen = HOME_SCREEN_MAIN;
	g_HomeStatusTimer = 0;
	pdPspHomeMenuRendererSetActive(0);
}

static void pdPspHomeCycleBinding(int index, int direction)
{
	const u32 *binds;
	u32 current;
	int physical = 0;
	int count = (int)(sizeof(g_PhysicalButtons) / sizeof(g_PhysicalButtons[0]));

	if (index < 0 || index >= pdPspHomeMenuGetBindingCount()) {
		return;
	}

	binds = inputKeyGetBinds(0, g_HomeBindings[index].contkey);
	current = binds ? binds[0] : 0;

	for (int i = 0; i < count; i++) {
		if (g_PhysicalButtons[i].virtkey == current) {
			physical = i;
			break;
		}
	}

	physical += direction >= 0 ? 1 : -1;
	if (physical < 0) physical = count - 1;
	if (physical >= count) physical = 0;

	inputKeyClearBinds(0, g_HomeBindings[index].contkey);
	if (g_PhysicalButtons[physical].virtkey != 0) {
		inputKeyBind(0, g_HomeBindings[index].contkey, 0, g_PhysicalButtons[physical].virtkey);
	}
}

static void pdPspHomeAdjustRow(int direction)
{
	if (g_HomeControlSelected < pdPspHomeMenuGetBindingCount()) {
		pdPspHomeCycleBinding(g_HomeControlSelected, direction);
	} else if (g_HomeControlSelected == pdPspHomeDeadzoneRow()) {
		f32 deadzone = inputControllerGetAxisDeadzone(0, 0, 0) + direction * 0.02f;
		if (deadzone < 0.0f) deadzone = 0.0f;
		if (deadzone > 0.8f) deadzone = 0.8f;
		for (int stick = 0; stick < 2; stick++) {
			for (int axis = 0; axis < 2; axis++) {
				inputControllerSetAxisDeadzone(0, stick, axis, deadzone);
			}
		}
	}
}

static void pdPspHomeActivateRow(void)
{
	if (g_HomeControlSelected < pdPspHomeMenuGetBindingCount()) {
		pdPspHomeCycleBinding(g_HomeControlSelected, 1);
	} else if (g_HomeControlSelected == pdPspHomeDeadzoneRow()) {
		pdPspHomeAdjustRow(1);
	} else if (g_HomeControlSelected == pdPspHomeSaveRow()) {
		inputSaveBinds();
		pdPspHomeSetStatus("Saved pspcontrols.ini");
	} else if (g_HomeControlSelected == pdPspHomeResetRow()) {
		inputSetDefaultKeyBinds(0, 0);
		for (int stick = 0; stick < 2; stick++) {
			for (int axis = 0; axis < 2; axis++) {
				inputControllerSetAxisDeadzone(0, stick, axis, 0.078125f);
			}
		}
		inputSaveBinds();
		pdPspHomeSetStatus("Defaults saved");
	} else if (g_HomeControlSelected == pdPspHomeBackRow()) {
		g_HomeScreen = HOME_SCREEN_MAIN;
	}
}

void pdPspHomeMenuInit(void)
{
	g_HomeActive = 0;
	g_HomeSelected = HOME_ITEM_RESUME;
	g_HomeScreen = HOME_SCREEN_MAIN;
	g_HomeControlSelected = 0;
	g_HomeStatusTimer = 0;
	g_HomeButtons = 0;
	g_HomeLastButtons = 0;
	g_HomeLastPolledButtons = pdPspHomeReadButtons();
	g_HomeStatus[0] = '\0';

	/* Suppress Sony's overlay; the required exit callback remains non-exiting. */
	sceImposeSetHomePopup(0);
}

void pdPspHomeMenuPoll(void)
{
	u32 buttons = pdPspHomeReadButtons();
	u32 pressed = buttons & ~g_HomeLastPolledButtons;
	g_HomeLastPolledButtons = buttons;

	if ((pressed & PSP_CTRL_HOME) && !g_HomeActive) {
		pdPspHomeOpen();
	}
}

s32 pdPspHomeMenuIsOpen(void)
{
	return g_HomeActive;
}

PdPspHomeMenuResult pdPspHomeMenuRunFrame(void)
{
	u32 menucolour;
	u32 pressed;
	u32 now;

	if (!g_HomeActive) {
		return PD_PSP_HOME_MENU_NONE;
	}

	pressed = g_HomeButtons & ~g_HomeLastButtons;
	g_HomeLastButtons = g_HomeButtons;
	g_HomeLastPolledButtons = g_HomeButtons;
	if (g_HomeStatusTimer > 0) g_HomeStatusTimer--;

	now = sceKernelGetSystemTimeLow();
	if (now - g_HomeLockoutStart >= HOME_INPUT_LOCKOUT_USEC) {
		pressed &= HOME_BUTTON_MASK;
		if (pressed != 0 && now - g_HomeLastInput < HOME_INPUT_DEBOUNCE_USEC) {
			pressed = 0;
		} else if (pressed != 0) {
			g_HomeLastInput = now;
		}

		if (g_HomeScreen == HOME_SCREEN_MAPPING) {
			if (pressed & PSP_CTRL_CIRCLE) {
				g_HomeScreen = HOME_SCREEN_MAIN;
			} else if (pressed & PSP_CTRL_UP) {
				g_HomeControlSelected--;
				if (g_HomeControlSelected < 0) g_HomeControlSelected = pdPspHomeRowCount() - 1;
			} else if (pressed & PSP_CTRL_DOWN) {
				g_HomeControlSelected = (g_HomeControlSelected + 1) % pdPspHomeRowCount();
			} else if (pressed & PSP_CTRL_LEFT) {
				pdPspHomeAdjustRow(-1);
			} else if (pressed & PSP_CTRL_RIGHT) {
				pdPspHomeAdjustRow(1);
			} else if (pressed & (PSP_CTRL_CROSS | PSP_CTRL_START)) {
				pdPspHomeActivateRow();
			}
		} else {
			if (pressed & PSP_CTRL_UP) {
				g_HomeSelected = (g_HomeSelected + HOME_ITEM_COUNT - 1) % HOME_ITEM_COUNT;
			} else if (pressed & PSP_CTRL_DOWN) {
				g_HomeSelected = (g_HomeSelected + 1) % HOME_ITEM_COUNT;
			}

			if (pressed & (PSP_CTRL_CROSS | PSP_CTRL_START)) {
				if (g_HomeSelected == HOME_ITEM_RESUME) {
					pdPspHomeClose();
					return PD_PSP_HOME_MENU_NONE;
				}
				if (g_HomeSelected == HOME_ITEM_EXIT) {
					pdPspHomeClose();
					return PD_PSP_HOME_MENU_EXIT_GAME;
				}
				g_HomeScreen = HOME_SCREEN_MAPPING;
			}
		}
	}

	/* Match the blue accent used by Perfect Dark's default in-game pause dialogs. */
	menucolour = g_MenuColours[MENUDIALOGTYPE_DEFAULT].dialog_border1;
	pdPspHomeMenuRendererRender(g_HomeSelected, g_HomeScreen, g_HomeControlSelected,
			g_HomeStatusTimer > 0 ? g_HomeStatus : NULL,
			(u8)(menucolour >> 24), (u8)(menucolour >> 16), (u8)(menucolour >> 8));
	return PD_PSP_HOME_MENU_NONE;
}

int pdPspHomeMenuGetBindingCount(void)
{
	return (int)(sizeof(g_HomeBindings) / sizeof(g_HomeBindings[0]));
}

const char *pdPspHomeMenuGetBindingName(int index)
{
	return index >= 0 && index < pdPspHomeMenuGetBindingCount() ? g_HomeBindings[index].name : "";
}

void pdPspHomeMenuGetBindingValue(int index, char *buffer, size_t size)
{
	const u32 *binds;
	u32 current;

	if (size == 0) return;
	buffer[0] = '\0';
	if (index < 0 || index >= pdPspHomeMenuGetBindingCount()) return;

	binds = inputKeyGetBinds(0, g_HomeBindings[index].contkey);
	current = binds ? binds[0] : 0;
	for (u32 i = 0; i < sizeof(g_PhysicalButtons) / sizeof(g_PhysicalButtons[0]); i++) {
		if (g_PhysicalButtons[i].virtkey == current) {
			snprintf(buffer, size, "%s", g_PhysicalButtons[i].name);
			return;
		}
	}
	snprintf(buffer, size, "%s", inputGetKeyName(current));
}

int pdPspHomeMenuGetDeadzone(void)
{
	return (int)(inputControllerGetAxisDeadzone(0, 0, 0) * 100.0f + 0.5f);
}
