/* The PSP has no keyboard console. Network messages still go to the game log. */
#include "console.h"
#include "system.h"
#include <stdio.h>
#include <stdarg.h>
void conInit(void) {}
Gfx *conRender(Gfx *gdl) { return gdl; }
void conTick(void) {}
s32 conIsOpen(void) { return 0; }
void conPrint(s32 show, const char *text) { (void)show; sysLogPrintf(LOG_NOTE, "%s", text); }
void conPrintLn(s32 show, const char *text) { conPrint(show, text); }
void conPrintf(s32 show, const char *format, ...) {
 char text[256]; va_list args; va_start(args,format); vsnprintf(text,sizeof(text),format,args); va_end(args); conPrint(show,text);
}
