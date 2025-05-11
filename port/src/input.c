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

// PSP: Remove controller config, pads, and all SDL controller state.
static u32 binds[MAXCONTROLLERS][CK_TOTAL_COUNT][INPUT_MAX_BINDS];
static char bindStrs[MAXCONTROLLERS][CK_TOTAL_COUNT][256];
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

void inputSetDefaultKeyBinds(s32 cidx, s32 n64mode)
{
	memset(binds[cidx], 0, sizeof(binds[cidx]));
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
	for (s32 i = 0; i < MAXCONTROLLERS; ++i) {
		for (u32 ck = 0; ck < CK_TOTAL_COUNT; ++ck) {
			inputParseBindString(i, ck, bindStrs[i][ck]);
		}
	}
}

// PSP: Replace inputInit with controller setup
s32 inputInit(void)
{
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
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

static inline s32 inputAxisScale(s32 x, const s32 deadzone, const f32 scale)
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

// PSP: Replace inputReadController with direct SceCtrlData reading.
s32 inputReadController(s32 idx, OSContPad *npad)
{
    if (idx != 0 || !npad) return -1;

    SceCtrlData pad;
    sceCtrlPeekBufferPositive(&pad, 1);

    npad->button = 0;
    npad->stick_x = pad.Lx - 128;
    npad->stick_y = pad.Ly - 128;
    npad->rstick_x = 0;
    npad->rstick_y = 0;

    if (pad.Buttons & PSP_CTRL_TRIANGLE) npad->button |= U_CBUTTONS;
    if (pad.Buttons & PSP_CTRL_CROSS)    npad->button |= D_CBUTTONS;
    if (pad.Buttons & PSP_CTRL_SQUARE)   npad->button |= L_CBUTTONS;
    if (pad.Buttons & PSP_CTRL_CIRCLE)   npad->button |= R_CBUTTONS;
    if (pad.Buttons & PSP_CTRL_RTRIGGER) npad->button |= R_TRIG;
    if (pad.Buttons & PSP_CTRL_LTRIGGER) npad->button |= L_TRIG;
    if (pad.Buttons & PSP_CTRL_START)    npad->button |= START_BUTTON;
    if (pad.Buttons & PSP_CTRL_SELECT)   npad->button |= Z_TRIG;
    if (pad.Buttons & PSP_CTRL_UP)       npad->button |= U_JPAD;
    if (pad.Buttons & PSP_CTRL_DOWN)     npad->button |= D_JPAD;
    if (pad.Buttons & PSP_CTRL_LEFT)     npad->button |= B_BUTTON;
    if (pad.Buttons & PSP_CTRL_RIGHT)    npad->button |= A_BUTTON;
    //if (pad.Buttons & PSP_CTRL_HOME)     npad->button |= B_BUTTON;
    //if (pad.Buttons & PSP_CTRL_HOLD)     npad->button |= A_BUTTON;

    return 0;
}

// PSP: No mouse, stub
static inline void inputUpdateMouse(void) {}

void inputUpdate(void)
{
    // PSP: nothing needed
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
    return 0;
}

void inputControllerSetSticksSwapped(s32 cidx, s32 swapped)
{
    // PSP: no-op
}

s32 inputControllerGetDualAnalog(s32 cidx)
{
    return 1;
}

void inputControllerSetDualAnalog(s32 cidx, s32 enable)
{
    // PSP: no-op
}

s32 inputControllerGetCancelCButtons(s32 cidx)
{
    return 0;
}

void inputControllerSetCancelCButtons(s32 cidx, s32 cancel)
{
    // PSP: no-op
}

f32 inputControllerGetAxisScale(s32 cidx, s32 stick, s32 axis)
{
    return 1.0f;
}

void inputControllerSetAxisScale(s32 cidx, s32 stick, s32 axis, f32 value)
{
    // PSP: no-op
}

f32 inputControllerGetAxisDeadzone(s32 cidx, s32 stick, s32 axis)
{
    return 0.0f;
}

void inputControllerSetAxisDeadzone(s32 cidx, s32 stick, s32 axis, f32 value)
{
    // PSP: no-op
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
}

const u32 *inputKeyGetBinds(s32 idx, u32 ck)
{
	if (idx < 0 || idx >= INPUT_MAX_CONTROLLERS || ck >= CK_TOTAL_COUNT) {
		return NULL;
	}
	return binds[idx][ck];
}

s32 inputKeyPressed(u32 vk)
{
    // PSP: not used, always 0
    return 0;
}

s32 inputKeyJustPressed(u32 vk)
{
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
    return "UNKNOWN";
}

s32 inputGetKeyByName(const char *name)
{
    return -1;
}

void inputClearLastKey(void)
{
    // PSP: no-op
}

s32 inputGetLastKey(void)
{
    return 0;
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
    // PSP: nothing to register
}
