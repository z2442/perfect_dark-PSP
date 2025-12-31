#include <pspctrl.h>
#include <pspkernel.h>
#include <pspdebug.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include <ctype.h>
#include <PR/ultratypes.h>
#include <PR/os_thread.h>
#include <PR/os_cont.h>
#include "platform.h"
#include "input.h"
#include "video.h"
#include "config.h"
#include "utils.h"
#include "system.h"
#include "fs.h"

#define MAX_BIND_STR 256
#define PSP_CONTROLS_FNAME "pspcontrols.ini"
#define PSP_CONTROLS_PATH "$E/" PSP_CONTROLS_FNAME
#define PSP_ESCAPE_COMBO (PSP_CTRL_START | PSP_CTRL_SELECT)
#define PSP_DELETE_COMBO (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER | PSP_CTRL_SELECT)

// PSP: Remove controller config, pads, and all SDL controller state.
static u32 binds[MAXCONTROLLERS][CK_TOTAL_COUNT][INPUT_MAX_BINDS];
static char bindStrs[MAXCONTROLLERS][CK_TOTAL_COUNT][256];
static s32 inputSwapSticks[MAXCONTROLLERS];
static s32 inputDualAnalog[MAXCONTROLLERS];
static s32 inputCancelCButtons[MAXCONTROLLERS];
static f32 inputAxisScale[MAXCONTROLLERS][2][2];
static f32 inputAxisDeadzone[MAXCONTROLLERS][2][2];
static SceCtrlData pspPad;
static u32 pspButtons;
static u32 pspButtonsPrev;
static s32 pspAnalogX;
static s32 pspAnalogY;
static s32 inputSaveDirty = 0;
static s32 inputSaveCountdown = 0;
static s32 inputSuppressDirty = 0;
static s32 fakeControllers = 0;
static s32 firstController = 0;
static s32 connectedMask = 1;
static s32 mouseEnabled = 0;
static s32 mouseX, mouseY;
static s32 mouseDX, mouseDY;
static u32 mouseButtons;
static s32 mouseWheel = 0;
static s32 mouseLocked = 0;
static s32 mouseLockMode = MLOCK_AUTO;
static u64 mouseCursorTime = 0;
static s32 mouseShowCursor = 1;
static f32 mouseSensX = 1.5f;
static f32 mouseSensY = 1.5f;
static s32 lastKey = 0;
static char lastChar = 0;
static s32 textInput = 0;
static char *clipboardText = NULL;

static const char *ckNames[CK_TOTAL_COUNT] = {
	"R_CBUTTONS",
	"L_CBUTTONS",
	"D_CBUTTONS",
	"U_CBUTTONS",
	"R_TRIG",
	"L_TRIG",
	"X_BUTTON",
	"Y_BUTTON",
	"R_JPAD",
	"L_JPAD",
	"D_JPAD",
	"U_JPAD",
	"START_BUTTON",
	"Z_TRIG",
	"B_BUTTON",
	"A_BUTTON",
	"STICK_XNEG",
	"STICK_XPOS",
	"STICK_YNEG",
	"STICK_YPOS",
	"ACCEPT_BUTTON",
	"CANCEL_BUTTON",
	"CK_0040",
	"CK_0080",
	"CK_0100",
	"CK_0200",
	"CK_0400",
	"CK_0800",
	"CK_1000",
	"CK_2000",
	"CK_4000",
	"CK_8000"
};

static const char *vkPunctNames[] = {
	"MINUS", "EQUALS", "LEFTBRACKET", "RIGHTBRACKET", "BACKSLASH",
	"HASH", "SEMICOLON", "APOSTROPHE", "GRAVE", "COMMA", "PERIOD", "SLASH"
};

static const char *vkMouseNames[] = {
	"MOUSE_LEFT",
	"MOUSE_MIDDLE",
	"MOUSE_RIGHT",
	"MOUSE_X1",
	"MOUSE_X2",
	"MOUSE_WHEEL_UP",
	"MOUSE_WHEEL_DN",
};

static const char *vkJoyNames[] = {
	"JOY1_A",
	"JOY1_B",
	"JOY1_X",
	"JOY1_Y",
	"JOY1_BACK",
	"JOY1_GUIDE",
	"JOY1_START",
	"JOY1_LSTICK",
	"JOY1_RSTICK",
	"JOY1_LSHOULDER",
	"JOY1_RSHOULDER",
	"JOY1_DPAD_UP",
	"JOY1_DPAD_DOWN",
	"JOY1_DPAD_LEFT",
	"JOY1_DPAD_RIGHT",
	"JOY1_BUTTON_15",
	"JOY1_BUTTON_16",
	"JOY1_BUTTON_17",
	"JOY1_BUTTON_18",
	"JOY1_BUTTON_19",
	"JOY1_TOUCHPAD",
	"JOY1_BUTTON_21",
	"JOY1_BUTTON_22",
	"JOY1_BUTTON_23",
	"JOY1_BUTTON_24",
	"JOY1_BUTTON_25",
	"JOY1_BUTTON_26",
	"JOY1_BUTTON_27",
	"JOY1_BUTTON_28",
	"JOY1_BUTTON_29",
	"JOY1_LTRIGGER",
	"JOY1_RTRIGGER",
};

static char vkNames[VK_TOTAL_COUNT][64];

static s8 vkPrevState[VK_TOTAL_COUNT];

#define INPUT_ARRAYCOUNT(a) ((s32)(sizeof(a) / sizeof((a)[0])))

static void inputMarkDirty(void)
{
	if (inputSuppressDirty) {
		return;
	}
	inputSaveDirty = 1;
	inputSaveCountdown = 30;
}

static void inputMaybeSave(void)
{
	if (!inputSaveDirty) {
		return;
	}
	if (inputSaveCountdown > 0) {
		--inputSaveCountdown;
		if (inputSaveCountdown > 0) {
			return;
		}
	}
	inputSaveDirty = 0;
	inputSaveBinds();
}

enum {
	VK_JOY1_A = VK_JOY1_BEGIN + 0,
	VK_JOY1_B = VK_JOY1_BEGIN + 1,
	VK_JOY1_X = VK_JOY1_BEGIN + 2,
	VK_JOY1_Y = VK_JOY1_BEGIN + 3,
	VK_JOY1_BACK = VK_JOY1_BEGIN + 4,
	VK_JOY1_GUIDE = VK_JOY1_BEGIN + 5,
	VK_JOY1_START = VK_JOY1_BEGIN + 6,
	VK_JOY1_LSTICK = VK_JOY1_BEGIN + 7,
	VK_JOY1_RSTICK = VK_JOY1_BEGIN + 8,
	VK_JOY1_LSHOULDER = VK_JOY1_BEGIN + 9,
	VK_JOY1_RSHOULDER = VK_JOY1_BEGIN + 10,
	VK_JOY1_DPAD_UP = VK_JOY1_BEGIN + 11,
	VK_JOY1_DPAD_DOWN = VK_JOY1_BEGIN + 12,
	VK_JOY1_DPAD_LEFT = VK_JOY1_BEGIN + 13,
	VK_JOY1_DPAD_RIGHT = VK_JOY1_BEGIN + 14,
	VK_JOY1_BUTTON_15 = VK_JOY1_BEGIN + 15,
	VK_JOY1_BUTTON_16 = VK_JOY1_BEGIN + 16,
	VK_JOY1_BUTTON_17 = VK_JOY1_BEGIN + 17,
	VK_JOY1_BUTTON_18 = VK_JOY1_BEGIN + 18,
	VK_JOY1_BUTTON_19 = VK_JOY1_BEGIN + 19,
	VK_JOY1_TOUCHPAD = VK_JOY1_BEGIN + 20,
	VK_JOY1_BUTTON_21 = VK_JOY1_BEGIN + 21,
	VK_JOY1_BUTTON_22 = VK_JOY1_BEGIN + 22,
	VK_JOY1_BUTTON_23 = VK_JOY1_BEGIN + 23,
	VK_JOY1_BUTTON_24 = VK_JOY1_BEGIN + 24,
	VK_JOY1_BUTTON_25 = VK_JOY1_BEGIN + 25,
	VK_JOY1_BUTTON_26 = VK_JOY1_BEGIN + 26,
	VK_JOY1_BUTTON_27 = VK_JOY1_BEGIN + 27,
	VK_JOY1_BUTTON_28 = VK_JOY1_BEGIN + 28,
	VK_JOY1_BUTTON_29 = VK_JOY1_BEGIN + 29,
};

struct pspvkmap {
	u32 vk;
	u32 button;
	const char *name;
};

static const struct pspvkmap pspKeyMap[] = {
	{ VK_JOY1_A,       PSP_CTRL_CROSS,    "PSP_CROSS" },
	{ VK_JOY1_B,       PSP_CTRL_CIRCLE,   "PSP_CIRCLE" },
	{ VK_JOY1_X,       PSP_CTRL_SQUARE,   "PSP_SQUARE" },
	{ VK_JOY1_Y,       PSP_CTRL_TRIANGLE, "PSP_TRIANGLE" },
	{ VK_JOY1_LTRIG,   PSP_CTRL_LTRIGGER, "PSP_LTRIGGER" },
	{ VK_JOY1_RTRIG,   PSP_CTRL_RTRIGGER, "PSP_RTRIGGER" },
	{ VK_JOY1_START,   PSP_CTRL_START,    "PSP_START" },
	{ VK_JOY1_BACK,    PSP_CTRL_SELECT,   "PSP_SELECT" },
	{ VK_JOY1_DPAD_UP,    PSP_CTRL_UP,    "PSP_DPAD_UP" },
	{ VK_JOY1_DPAD_DOWN,  PSP_CTRL_DOWN,  "PSP_DPAD_DOWN" },
	{ VK_JOY1_DPAD_LEFT,  PSP_CTRL_LEFT,  "PSP_DPAD_LEFT" },
	{ VK_JOY1_DPAD_RIGHT, PSP_CTRL_RIGHT, "PSP_DPAD_RIGHT" },
};

static void inputSetDefaultKeyBindsInternal(s32 cidx, s32 n64mode, s32 markDirty)
{
	if (cidx < 0 || cidx >= INPUT_MAX_CONTROLLERS) {
		return;
	}

	memset(binds[cidx], 0, sizeof(binds[cidx]));

	(void)n64mode;

	inputKeyBind(cidx, CK_C_U,   -1, VK_JOY1_Y);
	inputKeyBind(cidx, CK_C_D,   -1, VK_JOY1_A);
	inputKeyBind(cidx, CK_C_L,   -1, VK_JOY1_X);
	inputKeyBind(cidx, CK_C_R,   -1, VK_JOY1_B);
	inputKeyBind(cidx, CK_ZTRIG, -1, VK_JOY1_RTRIG);
	inputKeyBind(cidx, CK_LTRIG, -1, VK_JOY1_LTRIG);
	inputKeyBind(cidx, CK_START, -1, VK_JOY1_START);
	inputKeyBind(cidx, CK_RTRIG, -1, VK_JOY1_BACK);
	inputKeyBind(cidx, CK_DPAD_U, -1, VK_JOY1_DPAD_UP);
	inputKeyBind(cidx, CK_DPAD_D, -1, VK_JOY1_DPAD_DOWN);
	inputKeyBind(cidx, CK_B, -1, VK_JOY1_DPAD_LEFT);
	inputKeyBind(cidx, CK_A, -1, VK_JOY1_DPAD_RIGHT);

	if (markDirty) {
		inputMarkDirty();
	}
}

void inputSetDefaultKeyBinds(s32 cidx, s32 n64mode)
{
	inputSetDefaultKeyBindsInternal(cidx, n64mode, 1);
}

static void inputResetDefaults(void)
{
	inputSaveDirty = 0;
	inputSaveCountdown = 0;

	for (s32 i = 0; i < MAXCONTROLLERS; ++i) {
		memset(bindStrs[i], 0, sizeof(bindStrs[i]));
		inputSwapSticks[i] = 0;
		inputDualAnalog[i] = 1;
		inputCancelCButtons[i] = 0;

		for (s32 stick = 0; stick < 2; ++stick) {
			for (s32 axis = 0; axis < 2; ++axis) {
				inputAxisScale[i][stick][axis] = 1.0f;
				inputAxisDeadzone[i][stick][axis] = 0.078125f;
			}
		}

		inputSetDefaultKeyBindsInternal(i, 0, 0);
	}
}

// PSP: No controller hotplug or SDL event filter needed.

// PSP: No SDL key names
static inline void inputInitKeyNames(void) {}

void inputSaveBinds(void)
{
	char *bindstr;

	for (s32 i = 0; i < MAXCONTROLLERS; ++i) {
		for (u32 ck = 0; ck < CK_TOTAL_COUNT; ++ck) {
			bindstr = bindStrs[i][ck];
			bindstr[0] = '\0';
			for (s32 b = 0; b < INPUT_MAX_BINDS; ++b) {
				if (binds[i][ck][b]) {
					if (b) {
						strncat(bindstr, ", ", MAX_BIND_STR - 1);
					}
					strncat(bindstr, inputGetKeyName(binds[i][ck][b]), MAX_BIND_STR - 1);
				}
			}
			if (!bindstr[0]) {
				strcpy(bindstr, "NONE");
			}
		}
	}

	configSavePrefix(PSP_CONTROLS_PATH, "Input.");
}

static inline void inputParseBindString(const s32 ctrl, const u32 ck, char *bindstr)
{
	if (!bindstr[0]) {
		// empty string, keep defaults
		return;
	}

	// unbind all first
	memset(binds[ctrl][ck], 0, sizeof(binds[ctrl][ck]));

	if (!strcasecmp(bindstr, "NONE")) {
		// explicitly nothing bound
		return;
	}

	const char *tok = strtok(bindstr, ", ");
	while (tok) {
		if (tok[0]) {
			const s32 vk = inputGetKeyByName(tok);
			if (vk > 0) {
				inputKeyBind(ctrl, ck, -1, vk);
			}
		}
		tok = strtok(NULL, ", ");
	}
}

static inline void inputLoadBinds(void)
{
	inputSuppressDirty = 1;
	for (s32 i = 0; i < MAXCONTROLLERS; ++i) {
		for (u32 ck = 0; ck < CK_TOTAL_COUNT; ++ck) {
			inputParseBindString(i, ck, bindStrs[i][ck]);
		}
	}
	inputSuppressDirty = 0;
}

// PSP: Replace inputInit with controller setup
s32 inputInit(void)
{
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
	inputResetDefaults();
	if (fsFileSize(PSP_CONTROLS_PATH) > 0) {
		configLoad(PSP_CONTROLS_PATH);
		inputLoadBinds();
	} else {
		inputSaveBinds();
	}
    return 1;
}

static inline s32 inputBindPressed(const s32 idx, const u32 ck)
{
	for (s32 i = 0; i < INPUT_MAX_BINDS; ++i) {
		if (binds[idx][ck][i]) {
			if (inputKeyPressed(binds[idx][ck][i])) {
				return 1;
			}
		}
	}
	return 0;
}

static inline s32 inputApplyAxisScale(s32 x, const s32 deadzone, const f32 scale)
{
	if (abs(x) < deadzone) {
		return 0;
	} else {
		// rescale to fit the non-deadzone range
		if (x < 0) {
			x += deadzone;
		} else {
			x -= deadzone;
		}
		x = x * 32768 / (32768 - deadzone);
		// scale with sensitivity
		x *= scale;
		return (x > 32767) ? 32767 : ((x < -32768) ? -32768 : x);
	}
}

static inline f32 inputClampFloat(f32 val, f32 min, f32 max)
{
	if (val < min) return min;
	if (val > max) return max;
	return val;
}

static inline s8 inputScaleAxisToN64(s32 raw, f32 deadzone, f32 scale)
{
	const f32 clampedDeadzone = inputClampFloat(deadzone, 0.f, 1.f);
	const f32 clampedScale = inputClampFloat(scale, 0.f, 10.f);
	const s32 raw16 = raw << 8;
	const s32 dz = (s32)(clampedDeadzone * 32768.f + 0.5f);
	if (dz >= 32768) {
		return 0;
	}
	const s32 scaled = inputApplyAxisScale(raw16, dz, clampedScale);
	s32 stick = (scaled * 80) / 32768;
	if (stick > 80) stick = 80;
	if (stick < -80) stick = -80;
	return (s8)stick;
}

// PSP: Replace inputReadController with direct SceCtrlData reading.
s32 inputReadController(s32 idx, OSContPad *npad)
{
	if (idx != 0 || !npad) return -1;

	npad->button = 0;
	npad->stick_x = 0;
	npad->stick_y = 0;
	npad->rstick_x = 0;
	npad->rstick_y = 0;

	const s32 swap = inputControllerGetSticksSwapped(idx);
	const s32 stickIdx = swap ? 1 : 0;
	const s8 scaledX = inputScaleAxisToN64(pspAnalogX, inputAxisDeadzone[idx][stickIdx][0], inputAxisScale[idx][stickIdx][0]);
	const s8 scaledY = inputScaleAxisToN64(pspAnalogY, inputAxisDeadzone[idx][stickIdx][1], inputAxisScale[idx][stickIdx][1]);

	s32 useMovement = inputControllerGetDualAnalog(idx) ? 1 : 0;
	if (swap) {
		useMovement = !useMovement;
	}

	if (useMovement) {
		npad->stick_x = scaledX;
		npad->stick_y = scaledY;
	} else {
		if (scaledX > 0) npad->button |= R_CBUTTONS;
		else if (scaledX < 0) npad->button |= L_CBUTTONS;
		if (scaledY > 0) npad->button |= D_CBUTTONS;
		else if (scaledY < 0) npad->button |= U_CBUTTONS;
	}

	const s32 cancelC = inputControllerGetCancelCButtons(idx);
	for (u32 ck = 0; ck < CK_TOTAL_COUNT; ++ck) {
		if (cancelC && (ck == CK_C_U || ck == CK_C_D || ck == CK_C_L || ck == CK_C_R)) {
			continue;
		}
		if (inputBindPressed(idx, ck)) {
			npad->button |= (1u << ck);
		}
	}

	return 0;
}

// PSP: No mouse, stub
static inline void inputUpdateMouse(void) {}

void inputUpdate(void)
{
	pspButtonsPrev = pspButtons;
	sceCtrlPeekBufferPositive(&pspPad, 1);
	pspButtons = pspPad.Buttons;
	pspAnalogX = (s32)pspPad.Lx - 128;
	pspAnalogY = 128 - (s32)pspPad.Ly;

	s32 captured = 0;
	if ((pspButtons & PSP_DELETE_COMBO) == PSP_DELETE_COMBO &&
			(pspButtonsPrev & PSP_DELETE_COMBO) != PSP_DELETE_COMBO) {
		lastKey = VK_DELETE;
		captured = 1;
	}

	if ((pspButtons & PSP_ESCAPE_COMBO) == PSP_ESCAPE_COMBO &&
			(pspButtonsPrev & PSP_ESCAPE_COMBO) != PSP_ESCAPE_COMBO) {
		lastKey = VK_ESCAPE;
		captured = 1;
	}

	if (!captured) {
		const u32 newButtons = pspButtons & ~pspButtonsPrev;
		if (newButtons) {
			for (s32 i = 0; i < INPUT_ARRAYCOUNT(pspKeyMap); ++i) {
				if (newButtons & pspKeyMap[i].button) {
					lastKey = pspKeyMap[i].vk;
					break;
				}
			}
		}
	}

	inputMaybeSave();
}

s32 inputControllerConnected(s32 idx)
{
    // Only one controller supported, always connected
    return (idx == 0);
}

s32 inputRumbleSupported(s32 idx)
{
    // PSP: no rumble support
    return 0;
}

void inputRumble(s32 idx, f32 strength, f32 time)
{
    // PSP: no rumble
}

f32 inputRumbleGetStrength(s32 cidx)
{
    // PSP: no rumble
    return 0.0f;
}

void inputRumbleSetStrength(s32 cidx, f32 val)
{
    // PSP: no rumble
}

s32 inputControllerMask(void)
{
    return 1;
}

s32 inputControllerGetSticksSwapped(s32 cidx)
{
	if (cidx < 0 || cidx >= INPUT_MAX_CONTROLLERS) {
		return 0;
	}
	return inputSwapSticks[cidx];
}

void inputControllerSetSticksSwapped(s32 cidx, s32 swapped)
{
	if (cidx < 0 || cidx >= INPUT_MAX_CONTROLLERS) {
		return;
	}
	swapped = swapped ? 1 : 0;
	if (inputSwapSticks[cidx] != swapped) {
		inputSwapSticks[cidx] = swapped;
		inputMarkDirty();
	}
}

s32 inputControllerGetDualAnalog(s32 cidx)
{
	if (cidx < 0 || cidx >= INPUT_MAX_CONTROLLERS) {
		return 1;
	}
	return inputDualAnalog[cidx];
}

void inputControllerSetDualAnalog(s32 cidx, s32 enable)
{
	if (cidx < 0 || cidx >= INPUT_MAX_CONTROLLERS) {
		return;
	}
	enable = enable ? 1 : 0;
	if (inputDualAnalog[cidx] != enable) {
		inputDualAnalog[cidx] = enable;
		inputMarkDirty();
	}
}

s32 inputControllerGetCancelCButtons(s32 cidx)
{
	if (cidx < 0 || cidx >= INPUT_MAX_CONTROLLERS) {
		return 0;
	}
	return inputCancelCButtons[cidx];
}

void inputControllerSetCancelCButtons(s32 cidx, s32 cancel)
{
	if (cidx < 0 || cidx >= INPUT_MAX_CONTROLLERS) {
		return;
	}
	cancel = cancel ? 1 : 0;
	if (inputCancelCButtons[cidx] != cancel) {
		inputCancelCButtons[cidx] = cancel;
		inputMarkDirty();
	}
}

f32 inputControllerGetAxisScale(s32 cidx, s32 stick, s32 axis)
{
	if (cidx < 0 || cidx >= INPUT_MAX_CONTROLLERS || stick < 0 || stick > 1 || axis < 0 || axis > 1) {
		return 1.0f;
	}
	return inputAxisScale[cidx][stick][axis];
}

void inputControllerSetAxisScale(s32 cidx, s32 stick, s32 axis, f32 value)
{
	if (cidx < 0 || cidx >= INPUT_MAX_CONTROLLERS || stick < 0 || stick > 1 || axis < 0 || axis > 1) {
		return;
	}
	const f32 clamped = inputClampFloat(value, 0.f, 10.f);
	if (inputAxisScale[cidx][stick][axis] != clamped) {
		inputAxisScale[cidx][stick][axis] = clamped;
		inputMarkDirty();
	}
}

f32 inputControllerGetAxisDeadzone(s32 cidx, s32 stick, s32 axis)
{
	if (cidx < 0 || cidx >= INPUT_MAX_CONTROLLERS || stick < 0 || stick > 1 || axis < 0 || axis > 1) {
		return 0.0f;
	}
	return inputAxisDeadzone[cidx][stick][axis];
}

void inputControllerSetAxisDeadzone(s32 cidx, s32 stick, s32 axis, f32 value)
{
	if (cidx < 0 || cidx >= INPUT_MAX_CONTROLLERS || stick < 0 || stick > 1 || axis < 0 || axis > 1) {
		return;
	}
	const f32 clamped = inputClampFloat(value, 0.f, 1.f);
	if (inputAxisDeadzone[cidx][stick][axis] != clamped) {
		inputAxisDeadzone[cidx][stick][axis] = clamped;
		inputMarkDirty();
	}
}

s32 inputGetConnectedControllers(s32 *out)
{
    if (out) out[0] = 0;
    return 1;
}

s32 inputGetAssignedControllerId(s32 cidx)
{
    return 0;
}

const char *inputGetConnectedControllerName(s32 id)
{
    return "PSP Controller";
}

s32 inputAssignController(s32 cidx, s32 id)
{
    // PSP: always one controller, always assigned
    return (cidx == 0 && id == 0);
}

void inputKeyBind(s32 idx, u32 ck, s32 bind, u32 vk)
{
	if (idx < 0 || idx >= INPUT_MAX_CONTROLLERS || bind >= INPUT_MAX_BINDS || ck >= CK_TOTAL_COUNT) {
		return;
	}

	if (bind < 0) {
		for (s32 i = 0; i < INPUT_MAX_BINDS; ++i) {
			if (binds[idx][ck][i] == 0) {
				bind = i;
				break;
			}
		}
		if (bind < 0) {
			bind = INPUT_MAX_BINDS - 1; // just overwrite last
		}
	}

	binds[idx][ck][bind] = vk;
	inputMarkDirty();
}

const u32 *inputKeyGetBinds(s32 idx, u32 ck)
{
	if (idx < 0 || idx >= INPUT_MAX_CONTROLLERS || ck >= CK_TOTAL_COUNT) {
		return NULL;
	}
	return binds[idx][ck];
}

static inline s32 inputPspNameMatches(const char *name, const char *pspName)
{
	if (!strcasecmp(name, pspName)) {
		return 1;
	}
	if (!strncasecmp(pspName, "PSP_", 4)) {
		return !strcasecmp(name, pspName + 4);
	}
	return 0;
}

static const struct pspvkmap *inputFindPspKeyByVk(u32 vk)
{
	for (s32 i = 0; i < INPUT_ARRAYCOUNT(pspKeyMap); ++i) {
		if (pspKeyMap[i].vk == vk) {
			return &pspKeyMap[i];
		}
	}
	return NULL;
}

static const struct pspvkmap *inputFindPspKeyByName(const char *name)
{
	for (s32 i = 0; i < INPUT_ARRAYCOUNT(pspKeyMap); ++i) {
		if (inputPspNameMatches(name, pspKeyMap[i].name)) {
			return &pspKeyMap[i];
		}
	}
	return NULL;
}

s32 inputKeyPressed(u32 vk)
{
	if (vk == VK_ESCAPE) {
		if ((pspButtons & PSP_DELETE_COMBO) == PSP_DELETE_COMBO) {
			return 0;
		}
		return (pspButtons & PSP_ESCAPE_COMBO) == PSP_ESCAPE_COMBO;
	}
	if (vk == VK_DELETE) {
		return (pspButtons & PSP_DELETE_COMBO) == PSP_DELETE_COMBO;
	}
	if (vk == VK_JOY1_LSHOULDER) {
		return (pspButtons & PSP_CTRL_LTRIGGER) != 0;
	}
	if (vk == VK_JOY1_RSHOULDER) {
		return (pspButtons & PSP_CTRL_RTRIGGER) != 0;
	}

	const struct pspvkmap *map = inputFindPspKeyByVk(vk);
	if (map) {
		return (pspButtons & map->button) != 0;
	}

	return 0;
}

s32 inputKeyJustPressed(u32 vk)
{
	if (vk >= VK_TOTAL_COUNT) {
		return 0;
	}
	const s8 pressed = inputKeyPressed(vk);
	const s32 result = pressed && !vkPrevState[vk];
	vkPrevState[vk] = pressed;
	return result;
}

static inline u32 inputContToContKey(const u32 cont)
{
	if (cont == 0) {
		return 0;
	}
	// just a log2 to convert CONT_* to their indices
	return 32 - __builtin_clz(cont - 1);
}

s32 inputButtonPressed(s32 idx, u32 contbtn)
{
	if (idx < 0 || idx >= INPUT_MAX_CONTROLLERS || contbtn == 0) {
		return 0;
	}

	for (u32 ck = 0; ck < CK_TOTAL_COUNT; ++ck) {
		if (contbtn & (1u << ck)) {
			if (inputBindPressed(idx, ck)) {
				return 1;
			}
		}
	}

	return 0;
}

void inputLockMouse(s32 lock)
{
    // PSP: no mouse
}

s32 inputMouseIsLocked(void)
{
    return 0;
}

s32 inputMouseGetPosition(s32 *x, s32 *y)
{
    return 0;
}

void inputMouseGetRawDelta(s32 *dx, s32 *dy)
{
    if (dx) *dx = 0;
    if (dy) *dy = 0;
}

void inputMouseGetScaledDelta(f32 *dx, f32 *dy)
{
    if (dx) *dx = 0.f;
    if (dy) *dy = 0.f;
}

void inputMouseGetAbsScaledDelta(f32 *dx, f32 *dy)
{
    if (dx) *dx = 0.f;
    if (dy) *dy = 0.f;
}

void inputMouseGetSpeed(f32 *x, f32 *y)
{
    if (x) *x = 0.f;
    if (y) *y = 0.f;
}

void inputMouseSetSpeed(f32 x, f32 y)
{
    // PSP: no-op
}

s32 inputMouseIsEnabled(void)
{
    return 0;
}

void inputMouseEnable(s32 enabled)
{
    // PSP: no-op
}

s32 inputAutoLockMouse(s32 wantlock)
{
    return 0;
}

void inputMouseShowCursor(s32 show)
{
    // PSP: no-op
}

s32 inputGetMouseLockMode(void)
{
    return 0;
}

void inputSetMouseLockMode(s32 lockmode)
{
    // PSP: no-op
}

const char *inputGetContKeyName(u32 ck)
{
	if (ck >= CK_TOTAL_COUNT) {
		return "";
	}
	return ckNames[ck];
}

s32 inputGetContKeyByName(const char *name)
{
	for (u32 i = 0; i < CK_TOTAL_COUNT; ++i) {
		if (!strcmp(name, ckNames[i])) {
			return i;
		}
	}
	sysLogPrintf(LOG_WARNING, "unknown bind name: `%s`", name);
	return -1;
}

const char *inputGetKeyName(s32 vk)
{
	const struct pspvkmap *map = inputFindPspKeyByVk(vk);
	if (map) {
		return map->name;
	}
	if (vk == VK_ESCAPE) {
		return "ESCAPE";
	}
	if (vk == VK_DELETE) {
		return "DELETE";
	}
	if (vk >= VK_JOY1_BEGIN && vk < VK_JOY1_BEGIN + INPUT_ARRAYCOUNT(vkJoyNames)) {
		return vkJoyNames[vk - VK_JOY1_BEGIN];
	}
	if (vk >= VK_MOUSE_BEGIN && vk < VK_MOUSE_BEGIN + INPUT_ARRAYCOUNT(vkMouseNames)) {
		return vkMouseNames[vk - VK_MOUSE_BEGIN];
	}
	return "UNKNOWN";
}

s32 inputGetKeyByName(const char *name)
{
	if (!name || !name[0]) {
		return -1;
	}
	if (!strcasecmp(name, "ESC") || !strcasecmp(name, "ESCAPE")) {
		return VK_ESCAPE;
	}
	if (!strcasecmp(name, "DEL") || !strcasecmp(name, "DELETE")) {
		return VK_DELETE;
	}

	const struct pspvkmap *map = inputFindPspKeyByName(name);
	if (map) {
		return map->vk;
	}

	for (s32 i = 0; i < INPUT_ARRAYCOUNT(vkJoyNames); ++i) {
		if (!strcasecmp(name, vkJoyNames[i])) {
			return VK_JOY1_BEGIN + i;
		}
	}

	for (s32 i = 0; i < INPUT_ARRAYCOUNT(vkMouseNames); ++i) {
		if (!strcasecmp(name, vkMouseNames[i])) {
			return VK_MOUSE_BEGIN + i;
		}
	}

	return -1;
}

void inputClearLastKey(void)
{
	lastKey = 0;
}

s32 inputGetLastKey(void)
{
	return lastKey;
}

void inputStartTextInput(void)
{
    // PSP: no-op
}

void inputClearLastTextChar(void)
{
    // PSP: no-op
}

char inputGetLastTextChar(void)
{
    return 0;
}

static inline s32 filterChar(const char ch)
{
	return isalnum(ch) || ch == ' ' || ch == '?' || ch == '!' || ch == '.';
}

s32 inputTextHandler(char *out, const u32 outSize, s32 *curCol, s32 oskCharsOnly)
{
    return 0;
}

void inputClearClipboard(void)
{
    // PSP: no clipboard
}

const char *inputGetClipboard(void)
{
    return NULL;
}

void inputStopTextInput(void)
{
    // PSP: no-op
}

s32 inputIsTextInputActive(void)
{
    return 0;
}

u32 inputGetKeyModState(void)
{
    return 0;
}

PD_CONSTRUCTOR static void inputConfigInit(void)
{
	inputResetDefaults();

	for (s32 i = 0; i < MAXCONTROLLERS; ++i) {
		configRegisterInt(strFmt("Input.Player%d.SwapSticks", i + 1), &inputSwapSticks[i], 0, 1);
		configRegisterInt(strFmt("Input.Player%d.DualAnalog", i + 1), &inputDualAnalog[i], 0, 1);
		configRegisterInt(strFmt("Input.Player%d.CancelCButtons", i + 1), &inputCancelCButtons[i], 0, 1);
		configRegisterFloat(strFmt("Input.Player%d.AxisScale.LStickX", i + 1), &inputAxisScale[i][0][0], 0.f, 10.f);
		configRegisterFloat(strFmt("Input.Player%d.AxisScale.LStickY", i + 1), &inputAxisScale[i][0][1], 0.f, 10.f);
		configRegisterFloat(strFmt("Input.Player%d.AxisScale.RStickX", i + 1), &inputAxisScale[i][1][0], 0.f, 10.f);
		configRegisterFloat(strFmt("Input.Player%d.AxisScale.RStickY", i + 1), &inputAxisScale[i][1][1], 0.f, 10.f);
		configRegisterFloat(strFmt("Input.Player%d.AxisDeadzone.LStickX", i + 1), &inputAxisDeadzone[i][0][0], 0.f, 1.f);
		configRegisterFloat(strFmt("Input.Player%d.AxisDeadzone.LStickY", i + 1), &inputAxisDeadzone[i][0][1], 0.f, 1.f);
		configRegisterFloat(strFmt("Input.Player%d.AxisDeadzone.RStickX", i + 1), &inputAxisDeadzone[i][1][0], 0.f, 1.f);
		configRegisterFloat(strFmt("Input.Player%d.AxisDeadzone.RStickY", i + 1), &inputAxisDeadzone[i][1][1], 0.f, 1.f);

		for (u32 ck = 0; ck < CK_TOTAL_COUNT; ++ck) {
			configRegisterString(strFmt("Input.Player%d.Bind.%s", i + 1, ckNames[ck]), bindStrs[i][ck], MAX_BIND_STR);
		}
	}
}
