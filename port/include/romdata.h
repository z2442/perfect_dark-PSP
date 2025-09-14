#ifndef _IN_ROMDATA_H
#define _IN_ROMDATA_H

#include <PR/ultratypes.h>

extern u32 g_RomFileSize;

s32 romdataInit(void);
void romdataShutdown(void);

// Streaming helpers: represent ROM file offsets as special romptr values
// On non-N64 builds we encode a ROM offset into the high address space so
// dmaExec can identify and read directly from the ROM file on disk.
#ifndef PLATFORM_N64
#include <stdint.h>
#include "types.h"

// Use a tagged high-bit pattern to encode ROM offsets into a romptr_t without
// colliding with real pointers. On 64-bit, user pointers are typically below
// 0x8000_0000_0000_0000; on 32-bit PSP, user memory is below 0x8000_0000.
// These tags should therefore never collide with real pointers.
#include <limits.h>
#if UINTPTR_MAX > 0xFFFFFFFFu
  #define ROMADDR_FILE_OFFSET_TAG ((romptr_t)0xF000000000000000ull)
#else
  #define ROMADDR_FILE_OFFSET_TAG ((romptr_t)0x80000000u)
#endif

#define ROMPTR_FROM_OFFSET(off)  ((romptr_t)(ROMADDR_FILE_OFFSET_TAG | (romptr_t)(off)))
#define ROMPTR_IS_FILE(addr)     ((((romptr_t)(addr)) & (romptr_t)ROMADDR_FILE_OFFSET_TAG) == (romptr_t)ROMADDR_FILE_OFFSET_TAG)
#define ROMPTR_TO_OFFSET(addr)   ((u32)((romptr_t)(addr) & ~(romptr_t)ROMADDR_FILE_OFFSET_TAG))

// Read from the open ROM file into dst; returns bytes read or negative on error
s32 romdataReadFromRom(u32 offset, void *dst, u32 len);
#endif

u8 *romdataFileLoad(s32 fileNum, u32 *outSize);
void romdataFilePreprocess(s32 fileNum, s32 loadType, u8 *data, u32 size, u32 *outSize);
void romdataFileFree(s32 fileNum);
const char *romdataFileGetName(s32 fileNum);

u8 *romdataFileGetData(s32 fileNum);
s32 romdataFileGetSize(s32 fileNum);

s32 romdataFileGetNumForName(const char *name);

u8 *romdataSegGetData(const char *segName);
u8 *romdataSegGetDataEnd(const char *segName);
u32 romdataSegGetSize(const char *segName);
u32 romdataFileGetEstimatedSize(const u32 size, const u32 loadtype);

s32 romdataCheckGbcRom(void);

#endif
