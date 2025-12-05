#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <PR/ultratypes.h>
#include "lib/rzip.h"
#include "romdata.h"
#include "fs.h"
#include "system.h"
#include "preprocess.h"
#include "platform.h"
#include "types.h"
#include "preprocess/common.h"

#ifndef PLATFORM_N64
#include <zlib.h>
#endif

#define ROM_FSEEK_IF_NEEDED(pos) \
    do { \
        if (g_RomFpPos != (u32)(pos)) { \
            if (fseek(g_RomFp, (long)(pos), SEEK_SET) != 0) return -1; \
            g_RomFpPos = (u32)(pos); \
        } \
    } while (0)

//Used to enable debug on rom streaming. 
//#define PDDEBUG

#define ROMDATA_ROM_NAME "pd." VERSION_ROMID ".z64"
#define ROMDATA_ROM_SIZE 33554432

#if VERSION == VERSION_NTSC_FINAL
#define ROMDATA_ROM_TITLE "Perfect Dark"
#define ROMDATA_ROM_ID "NPDE"
#define ROMDATA_ROM_DESC "NTSC v1.1"
#define ROMDATA_FILES_OFS 0x28080
#define ROMDATA_DATA_OFS 0x39850
#elif VERSION == VERSION_PAL_FINAL
#define ROMDATA_ROM_TITLE "Perfect Dark"
#define ROMDATA_ROM_ID "NPDP"
#define ROMDATA_ROM_DESC "PAL"
#define ROMDATA_FILES_OFS 0x28910
#define ROMDATA_DATA_OFS 0x39850
#elif VERSION == VERSION_JPN_FINAL
#define ROMDATA_ROM_TITLE "PERFECT DARK"
#define ROMDATA_ROM_ID "NPDJ"
#define ROMDATA_ROM_DESC "JPN"
#define ROMDATA_FILES_OFS 0x28800
#define ROMDATA_DATA_OFS 0x39850
#else
#error "This ROM version is unsupported."
#endif

#define ROMDATA_MAX_FILES 2048

#define GBC_ROM_NAME "pd.gbc"
#define GBC_ROM_SIZE 4194304

static FILE *g_RomFp = NULL; // NEW: File pointer for the ROM
static u32 g_RomFpPos = 0; // Track current file position to avoid redundant fseeks
u32 g_RomFileSize;
static char g_RomPath[FS_MAXPATH + 1];

static u8 *romDataSeg; // This is the inflated 1173-compressed segment, still needed in RAM
static u32 romDataSegSize;
static const char *romName = ROMDATA_ROM_NAME;

// NEW: Buffer for file names loaded from ROM
static char *g_RomFileNameBlockBuffer = NULL;
static u32 g_RomFileNameBlockSize = 0;

// --- Streamed segment LRU cache ---
#define MAX_STREAMED_SEGMENTS 4
typedef struct {
    const char *name;
    u8 *data;
    u32 size;
} StreamedSegmentEntry;

static StreamedSegmentEntry g_StreamedSegmentCache[MAX_STREAMED_SEGMENTS];
static u32 g_StreamedSegmentLRU = 0;

// ROM read-ahead cache for streaming (simple ring/LRU)
#ifndef PLATFORM_N64
#define ROM_STREAM_CACHE_SLOTS 64
#define ROM_STREAM_CACHE_CHUNK (16 * 1024)
typedef struct {
    u32 base;   // aligned base offset in ROM
    u32 size;   // valid bytes loaded into buf
    u8 *buf;    // allocated buffer
} RomCacheSlot;

static RomCacheSlot g_RomCache[ROM_STREAM_CACHE_SLOTS];
static u32 g_RomCacheNext = 0;
// Track sequential access to avoid unnecessary prefetch on random I/O
static u32 g_RomLastReadEnd = 0;
#endif


enum loadsource {
	SRC_UNLOADED = 0,     // Not loaded, or external file not yet loaded
	SRC_ROM_IN_FILE,      // Resides in ROM file, not yet loaded to RAM
	SRC_ROM_LOADED,       // Loaded from ROM file into RAM
	SRC_EXTERNAL          // Loaded from external file into RAM
};

struct romfilepatch {
	u32 ofs;
	u32 len;
	const char *src;
	const char *dst;
};

struct romfile {
	u8 **segstart;
	u8 **segend;
	const char *name;
	u8 *data;               // Pointer to loaded data in RAM
	u32 rom_offset;         // Offset in the ROM file (if source is ROM)
	u32 size;
	preprocessfunc preprocess;
	s32 source;             // enum loadsource
	s32 preprocessed;
	const struct romfilepatch *patches;
	u32 numpatches;
};

/* patches for individual files; applied on file load, before preprocFuncs, but */
/* after unzip; only applied when loading from a ROM file                       */
static const struct romfilepatch filePatches[] = {
	/* FILE_USETUPLUE: fixes Jon's double "if what" in Infiltration outro */
	{ 0x92a2, 1, "\x6c", "\x99" },
	{ 0x92b0, 1, "\x6c", "\x99" },
};

static struct romfile fileSlots[ROMDATA_MAX_FILES] = {
	[FILE_USETUPLUE] = { .patches = &filePatches[0], .numpatches = 2 },
};

#define ROMSEG_START(n) _ ## n ## SegmentRomStart
#define ROMSEG_END(n) _ ## n ## SegmentRomEnd

/* segment table for ntsc-final                                                     */
/* size will get calculated automatically if it is 0                                */
/* if there are replacement files in the data dir, they will be loaded instead      */
/* offsets are specified for ntsc-final, pal-final and jpn-final in that order      */
#define ROMSEG_LIST() \
	ROMSEG_DECL_SEG(fontjpnsingle,      0x194b20,  0x180330,  0x0,       0x0,      preprocessJpnFont       ) \
	ROMSEG_DECL_SEG(fontjpnmulti,       0x19fb40,  0x18b340,  0x0,       0x0,      preprocessJpnFont       ) \
	ROMSEG_DECL_SEG(animations,         0x1a15c0,  0x18cdc0,  0x190c50,  0x0,      preprocessAnimations    ) \
	ROMSEG_DECL_SEG(mpconfigs,          0x7d0a40,  0x7bc240,  0x7c00d0,  0x11e0,   preprocessMpConfigs     ) \
	ROMSEG_DECL_SEG(mpstringsE,         0x7d1c20,  0x7bd420,  0x7c12b0,  0x3700,   NULL                    ) \
	ROMSEG_DECL_SEG(mpstringsJ,         0x7d5320,  0x7c0b20,  0x7c49b0,  0x3700,   NULL                    ) \
	ROMSEG_DECL_SEG(mpstringsP,         0x7d8a20,  0x7c4220,  0x7c80b0,  0x3700,   NULL                    ) \
	ROMSEG_DECL_SEG(mpstringsG,         0x7dc120,  0x7c7920,  0x7cb7b0,  0x3700,   NULL                    ) \
	ROMSEG_DECL_SEG(mpstringsF,         0x7df820,  0x7cb020,  0x7ceeb0,  0x3700,   NULL                    ) \
	ROMSEG_DECL_SEG(mpstringsS,         0x7e2f20,  0x7ce720,  0x7d25b0,  0x3700,   NULL                    ) \
	ROMSEG_DECL_SEG(mpstringsI,         0x7e6620,  0x7d1e20,  0x7d5cb0,  0x3700,   NULL                    ) \
	ROMSEG_DECL_SEG(firingrange,        0x7e9d20,  0x7d5520,  0x7d93b0,  0x1550,   NULL                    ) \
	ROMSEG_DECL_SEG(fonttahoma,         0x7f7860,  0x7e3060,  0x7e6ef0,  0x0,      preprocessFont          ) \
	ROMSEG_DECL_SEG(fontnumeric,        0x7f8b20,  0x7e4320,  0x7e81b0,  0x0,      preprocessFont          ) \
	ROMSEG_DECL_SEG(fonthandelgothicsm, 0x7f9d30,  0x7e5530,  0x7e93c0,  0x0,      preprocessFont          ) \
	ROMSEG_DECL_SEG(fonthandelgothicxs, 0x7fbfb0,  0x7e87b0,  0x7ec640,  0x0,      preprocessFont          ) \
	ROMSEG_DECL_SEG(fonthandelgothicmd, 0x7fdd80,  0x7eae20,  0x7eecb0,  0x0,      preprocessFont          ) \
	ROMSEG_DECL_SEG(fonthandelgothiclg, 0x8008e0,  0x7eee70,  0x7f2d00,  0x0,      preprocessFont          ) \
	ROMSEG_DECL_SEG(sfxctl,             0x80a250,  0x7f87e0,  0x7fc670,  0x2fb80,  preprocessALBankFile    ) \
	ROMSEG_DECL_SEG(sfxtbl,             0x839dd0,  0x828360,  0x82c1f0,  0x4c2160, NULL                    ) \
	ROMSEG_DECL_SEG(seqctl,             0xcfbf30,  0xcea4c0,  0xcee350,  0xa060,   preprocessALBankFile    ) \
	ROMSEG_DECL_SEG(seqtbl,             0xd05f90,  0xcf4520,  0xcf83b0,  0x17c070, NULL                    ) \
	ROMSEG_DECL_SEG(sequences,          0xe82000,  0xe70590,  0xe74420,  0x563a0,  preprocessSequences     ) \
	ROMSEG_DECL_SEG(texturesdata,       0x1d65f40, 0x1d5ca20, 0x1d61f90, 0x0,      NULL                    ) \
	ROMSEG_DECL_SEG(textureslist,       0x1ff7ca0, 0x1fee780, 0x1ff68f0, 0x0,      preprocessTexturesList  ) \
	ROMSEG_DECL_SEG(copyright,          0x1ffea20, 0x1ff5500, 0x1ffd6b0, 0xb30,    NULL                    ) \
	ROMSEG_DECL_SEG(fontjpn,            0x0,       0x0,       0x178c40,  0x17920,  preprocessJpnFont       )

// declare the vars first
#undef ROMSEG_DECL_SEG
#define ROMSEG_DECL_SEG(name, ofs_ntsc, ofs_pal, ofs_jpn, size, preproc) u8 *ROMSEG_START(name), *ROMSEG_END(name);
ROMSEG_LIST()

// this is part of the animations seg and as such does not follow the naming convention
// these are set in preprocessAnimations
u8 *_animationsTableRomStart;
u8 *_animationsTableRomEnd;

// then build the table
#undef ROMSEG_DECL_SEG

#if VERSION == VERSION_NTSC_FINAL
#define ROMSEG_DECL_SEG(name, ofs_ntsc, ofs_pal, ofs_jpn, size, preproc) { &ROMSEG_START(name), &ROMSEG_END(name), #name, NULL, (u32)ofs_ntsc, size, preproc, SRC_ROM_IN_FILE, 0, NULL, 0 },
#elif VERSION == VERSION_PAL_FINAL
#define ROMSEG_DECL_SEG(name, ofs_ntsc, ofs_pal, ofs_jpn, size, preproc) { &ROMSEG_START(name), &ROMSEG_END(name), #name, NULL, (u32)ofs_pal, size, preproc, SRC_ROM_IN_FILE, 0, NULL, 0 },
#elif VERSION == VERSION_JPN_FINAL
#define ROMSEG_DECL_SEG(name, ofs_ntsc, ofs_pal, ofs_jpn, size, preproc) { &ROMSEG_START(name), &ROMSEG_END(name), #name, NULL, (u32)ofs_jpn, size, preproc, SRC_ROM_IN_FILE, 0, NULL, 0 },
#endif

static struct romfile romSegs[] = {
	ROMSEG_LIST()
	{ NULL, NULL, NULL, NULL, 0, 0, NULL, SRC_UNLOADED, 0, NULL, 0 }, // Terminator
};

static preprocessfunc filePreprocFuncs[] = {
	/* LOADTYPE_NONE  */ NULL,
	/* LOADTYPE_BG    */ NULL, // loaded in parts
	/* LOADTYPE_TILES */ preprocessTilesFile,
	/* LOADTYPE_LANG  */ preprocessLangFile,
	/* LOADTYPE_SETUP */ preprocessSetupFile,
	/* LOADTYPE_PADS  */ preprocessPadsFile,
	/* LOADTYPE_MODEL */ preprocessModelFile,
	/* LOADTYPE_GUN   */ preprocessGunFile,
};

static inline void romdataWrongRomError(const char *fmt, ...)
{
	char reason[1024];
	reason[0] = '\0';

	va_list args;
	va_start(args, fmt);
	vsnprintf(reason, sizeof(reason), fmt, args);
	va_end(args);

	sysFatalError("Wrong ROM file.\n%s\nEnsure that you have the correct " ROMDATA_ROM_DESC " ROM in z64 format.", reason);
}

static inline s32 romdataInflatePartialFromFile(u32 srcOffset, u8 *dst, u32 dstLen)
{
#ifndef PLATFORM_N64
    // Inflate raw DEFLATE (no header) from g_RomFp starting at srcOffset
    if (!g_RomFp || !dst || dstLen == 0) return 0;
    if (g_RomFpPos != srcOffset) {
        if (fseek(g_RomFp, (long)srcOffset, SEEK_SET) != 0) return 0;
        g_RomFpPos = srcOffset;
    }

    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    int ret = inflateInit2(&strm, -15);
    if (ret != Z_OK) return 0;

    const size_t INBUFSZ = 64 * 1024;
    u8 *inbuf = sysMemVolAlloc(INBUFSZ);
    if (!inbuf) { inflateEnd(&strm); return 0; }

    u32 produced = 0;
    int zret = Z_OK;
    while (produced < dstLen && zret != Z_STREAM_END) {
        size_t nread = fread(inbuf, 1, INBUFSZ, g_RomFp);
        g_RomFpPos += (u32)nread;
        if (nread == 0) break; // EOF or error
        strm.next_in = inbuf;
        strm.avail_in = (uInt)nread;

        while (strm.avail_in > 0 && produced < dstLen) {
            uInt outRem = (uInt)(dstLen - produced);
            strm.next_out = dst + produced;
            strm.avail_out = outRem;
            zret = inflate(&strm, Z_SYNC_FLUSH);

            uInt made = outRem - strm.avail_out;
            produced += made;

            if (zret == Z_STREAM_END) break;
            if (zret != Z_OK && zret != Z_BUF_ERROR) {
                // Error
                sysMemFree(inbuf);
                inflateEnd(&strm);
                return 0;
            }
            if (made == 0 && zret == Z_BUF_ERROR) {
                // Need more input
                break;
            }
        }
    }

    sysMemFree(inbuf);
    inflateEnd(&strm);
    return (s32)produced;
#else
    (void)srcOffset; (void)dst; (void)dstLen;
    return 0;
#endif
}

static inline void romdataLoadRom(void)
{
    #ifdef PDDEBUG
	sysLogPrintf(LOG_NOTE, "ROM file: %s", romName);
    #endif
    // Get ROM file size first
    s32 rom_size_check = fsFileSize(romName);
    if (rom_size_check < 0) {
        sysFatalError("Could not get size of ROM file %s.\nEnsure that it is in the %s directory.", romName, fsFullPath(""));
    }
    g_RomFileSize = (u32)rom_size_check;

    // Resolve full path once and open the ROM file
    strncpy(g_RomPath, fsFullPath(romName), sizeof(g_RomPath) - 1);
    g_RomPath[sizeof(g_RomPath) - 1] = '\0';
    g_RomFp = fopen(g_RomPath, "rb");
    if (g_RomFp) {
        /* Use a larger user-space buffer to reduce read syscalls. 128 KiB plays well with 32 KiB cache blocks. */
        setvbuf(g_RomFp, NULL, _IOFBF, 128 * 1024);
    }
	if (!g_RomFp) {
		sysFatalError("Could not open ROM file %s.\nEnsure that it is in the %s directory.", romName, fsFullPath(""));
	}

    // Check for archive signatures by reading first few bytes
    unsigned char header_check[4];
    if (fread(header_check, 1, sizeof(header_check), g_RomFp) != sizeof(header_check)) {
        fclose(g_RomFp); g_RomFp = NULL;
        romdataWrongRomError("Could not read initial bytes from ROM.");
    }
    rewind(g_RomFp); // Go back to start
    g_RomFpPos = 0;

	if (!memcmp(header_check, "PK", 2) || !memcmp(header_check, "Rar", 3) || !memcmp(header_check, "7z", 2)) {
        fclose(g_RomFp); g_RomFp = NULL;
		romdataWrongRomError("Your ROM is in an archive file. Please extract it.");
	}

	if (g_RomFileSize != ROMDATA_ROM_SIZE) {
        fclose(g_RomFp); g_RomFp = NULL;
		romdataWrongRomError("ROM size does not match: expected: %u, got: %u.", ROMDATA_ROM_SIZE, g_RomFileSize);
	}

    // Read and check ROM header fields
    char rom_id_buf[4];
    char rom_title_buf[sizeof(ROMDATA_ROM_TITLE) -1];

    fseek(g_RomFp, 0x3b, SEEK_SET);
    g_RomFpPos = 0x3b;
    if (fread(rom_id_buf, 1, 4, g_RomFp) != 4) { /* error */ }
    g_RomFpPos += 4;
    fseek(g_RomFp, 0x20, SEEK_SET);
    g_RomFpPos = 0x20;
    if (fread(rom_title_buf, 1, sizeof(rom_title_buf), g_RomFp) != sizeof(rom_title_buf)) { /* error */ }
    g_RomFpPos += (u32)sizeof(rom_title_buf);

	if (memcmp(rom_id_buf, ROMDATA_ROM_ID, 4) || memcmp(rom_title_buf, ROMDATA_ROM_TITLE, sizeof(ROMDATA_ROM_TITLE) - 1)) {
        fclose(g_RomFp); g_RomFp = NULL;
		romdataWrongRomError("ROM header does not match.");
	}

	// Read the compressed data segment header
    u8 zipped_header[5];
    // Init ROM cache slots
#ifndef PLATFORM_N64
    for (u32 i = 0; i < ROM_STREAM_CACHE_SLOTS; ++i) {
        g_RomCache[i].base = 0xffffffffu;
        g_RomCache[i].size = 0;
        g_RomCache[i].buf = NULL;
    }
#endif

    fseek(g_RomFp, ROMDATA_DATA_OFS, SEEK_SET);
    g_RomFpPos = ROMDATA_DATA_OFS;
    if (fread(zipped_header, 1, 5, g_RomFp) != 5) {
        fclose(g_RomFp); g_RomFp = NULL;
        sysFatalError("Could not read data segment header from ROM.");
    }
    g_RomFpPos += 5;

	if (!rzipIs1173(zipped_header)) {
        fclose(g_RomFp); g_RomFp = NULL;
		romdataWrongRomError("Data segment is not 1173-compressed.");
	}

	const u32 dataSegLen = ((u32)zipped_header[2] << 16) | ((u32)zipped_header[3] << 8) | (u32)zipped_header[4];
	if (dataSegLen < ROMDATA_FILES_OFS) { // ROMDATA_FILES_OFS is an offset *within* the inflated data segment
        fclose(g_RomFp); g_RomFp = NULL;
		romdataWrongRomError("Data segment too small (%u), need at least %u for file table.", dataSegLen, ROMDATA_FILES_OFS);
	}

    // Only inflate the portion necessary to reach the file offsets table and its entries.
    u32 need_unzipped = ROMDATA_FILES_OFS + (ROMDATA_MAX_FILES + 2) * sizeof(u32);
    if (need_unzipped > dataSegLen) need_unzipped = dataSegLen;

    romDataSeg = sysMemVolAlloc(need_unzipped);
    if (!romDataSeg) {
        fclose(g_RomFp); g_RomFp = NULL;
        sysFatalError("Could not allocate %u bytes for partial data segment.", need_unzipped);
    }

    // Inflate starting after the 5-byte 1173 header
    s32 outbytes = romdataInflatePartialFromFile(ROMDATA_DATA_OFS + 5, romDataSeg, need_unzipped);
    if (outbytes < (s32)need_unzipped) {
        sysMemFree(romDataSeg); romDataSeg = NULL;
        fclose(g_RomFp); g_RomFp = NULL;
        sysFatalError("Partial inflate failed: needed %u, got %d", need_unzipped, outbytes);
    }

    romDataSegSize = need_unzipped;
}

#ifndef PLATFORM_N64
// Read from ROM file at byte offset into destination buffer; returns bytes read or -1 on error
s32 romdataReadFromRom(u32 offset, void *dst, u32 len) {
    if ((u64)offset + (u64)len > (u64)g_RomFileSize) return -1;

#if !defined(PLATFORM_N64)
    // Detect simple sequential pattern (new read starts where the previous ended)
    const int is_seq = (offset == g_RomLastReadEnd);

    /* Fast path for very small reads: go straight to FILE* without touching the cache. */

    // Try cache first for small aligned reads
    if (len <= ROM_STREAM_CACHE_CHUNK) {
        const u32 base = offset & ~(ROM_STREAM_CACHE_CHUNK - 1);
        const u32 within = offset - base;
        if (within + len <= ROM_STREAM_CACHE_CHUNK) {
            for (u32 i = 0; i < ROM_STREAM_CACHE_SLOTS; ++i) {
                if (g_RomCache[i].buf && g_RomCache[i].base == base && g_RomCache[i].size >= within + len) {
                    memcpy(dst, g_RomCache[i].buf + within, len);
                    g_RomLastReadEnd = offset + len;
                    return (s32)len;
                }
            }
            // Refill cache slot using the already-open g_RomFp
            u32 idx = g_RomCacheNext++ % ROM_STREAM_CACHE_SLOTS;
            if (!g_RomCache[idx].buf) {
                g_RomCache[idx].buf = sysMemVolAlloc(ROM_STREAM_CACHE_CHUNK);
                if (!g_RomCache[idx].buf) return -1;
            }
            if (!g_RomFp) return -1;
            ROM_FSEEK_IF_NEEDED(base);
            size_t n2 = fread(g_RomCache[idx].buf, 1, ROM_STREAM_CACHE_CHUNK, g_RomFp);
            g_RomFpPos += (u32)n2;
            g_RomCache[idx].base = base;
            g_RomCache[idx].size = (u32)n2;
            /* Read-ahead: only prefetch next chunk if we are reading sequentially. */
            if (is_seq) {
                const u32 next_base = base + ROM_STREAM_CACHE_CHUNK;
                if (next_base < g_RomFileSize) {
                    u32 pidx = g_RomCacheNext++ % ROM_STREAM_CACHE_SLOTS;
                    if (!g_RomCache[pidx].buf) {
                        g_RomCache[pidx].buf = sysMemVolAlloc(ROM_STREAM_CACHE_CHUNK);
                        if (!g_RomCache[pidx].buf) {
                            g_RomCache[pidx].size = 0; // skip if alloc failed
                        }
                    }
                    if (g_RomCache[pidx].buf) {
                        ROM_FSEEK_IF_NEEDED(next_base);
                        size_t pn = fread(g_RomCache[pidx].buf, 1, ROM_STREAM_CACHE_CHUNK, g_RomFp);
                        g_RomFpPos += (u32)pn;
                        g_RomCache[pidx].base = next_base;
                        g_RomCache[pidx].size = (u32)pn;
                    }
                }
            }
            if (within + len <= g_RomCache[idx].size) {
                memcpy(dst, g_RomCache[idx].buf + within, len);
                g_RomLastReadEnd = offset + len;
                return (s32)len;
            }
            // Fall through to direct read if cache insufficient
        }
        else {
            // Request spans two cache blocks; satisfy from two cached chunks to avoid direct read
            // First block
            const u32 head_len = ROM_STREAM_CACHE_CHUNK - within;
            const u32 tail_len = len - head_len;
            // Ensure first block is cached
            u32 idx = 0xffffffffu;
            for (u32 i = 0; i < ROM_STREAM_CACHE_SLOTS; ++i) {
                if (g_RomCache[i].buf && g_RomCache[i].base == base && g_RomCache[i].size >= ROM_STREAM_CACHE_CHUNK) {
                    idx = i; break;
                }
            }
            if (idx == 0xffffffffu) {
                idx = g_RomCacheNext++ % ROM_STREAM_CACHE_SLOTS;
                if (!g_RomCache[idx].buf) {
                    g_RomCache[idx].buf = sysMemVolAlloc(ROM_STREAM_CACHE_CHUNK);
                    if (!g_RomCache[idx].buf) return -1;
                }
                ROM_FSEEK_IF_NEEDED(base);
                size_t nA = fread(g_RomCache[idx].buf, 1, ROM_STREAM_CACHE_CHUNK, g_RomFp);
                g_RomFpPos += (u32)nA;
                g_RomCache[idx].base = base;
                g_RomCache[idx].size = (u32)nA;
                if (g_RomCache[idx].size < ROM_STREAM_CACHE_CHUNK) {
                    // Can't satisfy full head; copy what we can and return early
                    const u32 can = g_RomCache[idx].size > within ? (g_RomCache[idx].size - within) : 0;
                    if (can) memcpy(dst, g_RomCache[idx].buf + within, can);
                    g_RomLastReadEnd = offset + can;
                    return (s32)can;
                }
            }
            // Ensure second block is cached
            const u32 next_base = base + ROM_STREAM_CACHE_CHUNK;
            u32 pidx = 0xffffffffu;
            for (u32 i = 0; i < ROM_STREAM_CACHE_SLOTS; ++i) {
                if (g_RomCache[i].buf && g_RomCache[i].base == next_base && g_RomCache[i].size > 0) {
                    pidx = i; break;
                }
            }
            if (pidx == 0xffffffffu) {
                pidx = g_RomCacheNext++ % ROM_STREAM_CACHE_SLOTS;
                if (!g_RomCache[pidx].buf) {
                    g_RomCache[pidx].buf = sysMemVolAlloc(ROM_STREAM_CACHE_CHUNK);
                    if (!g_RomCache[pidx].buf) return -1;
                }
                ROM_FSEEK_IF_NEEDED(next_base);
                size_t nB = fread(g_RomCache[pidx].buf, 1, ROM_STREAM_CACHE_CHUNK, g_RomFp);
                g_RomFpPos += (u32)nB;
                g_RomCache[pidx].base = next_base;
                g_RomCache[pidx].size = (u32)nB;
            }

            // Copy from both cached blocks
            memcpy(dst, g_RomCache[idx].buf + within, head_len);
            const u32 avail_tail = (tail_len <= g_RomCache[pidx].size) ? tail_len : g_RomCache[pidx].size;
            memcpy((u8 *)dst + head_len, g_RomCache[pidx].buf, avail_tail);
            g_RomLastReadEnd = offset + head_len + avail_tail;
            return (s32)(head_len + avail_tail);
        }
    }

    // Direct read using the already-open g_RomFp
    if (!g_RomFp) return -1;
    ROM_FSEEK_IF_NEEDED(offset);
    size_t n = fread(dst, 1, len, g_RomFp);
    g_RomFpPos += (u32)n;
    g_RomLastReadEnd = offset + (u32)n;
    return (s32)n;
#else
    // N64 path unused here
    return -1;
#endif
}
#endif

static inline void romdataUpdateSegStartEnd(struct romfile* seg)
{
    if (seg->segstart) {
        *seg->segstart = seg->data;
    }

	if (seg->segend) {
		*seg->segend = seg->data + seg->size;
	}
}

static inline void romdataInitSegment(struct romfile *seg)
{
	if (seg->rom_offset == 0 && seg->source == SRC_ROM_IN_FILE) { // rom_offset is used as initial offset storage
		// unused in this ROM, or not ROM sourced initially. Mark as unloaded.
        seg->source = SRC_UNLOADED; 
        #ifdef PDDEBUG
		sysLogPrintf(LOG_NOTE, "Skipping segment %s (zero offset)", seg->name);
        #endif
        // Clear data and size to be safe, ensure segstart/end are NULL if not used
        seg->data = NULL;
        seg->size = 0;
        romdataUpdateSegStartEnd(seg); // Will set segstart/end to NULL/0
		return;
	}

    // Calculate size if not specified (size == 0)
    if (seg->size == 0 && seg->source == SRC_ROM_IN_FILE) {
        if (seg[1].name && seg[1].rom_offset != 0) { // Next segment exists and is valid
            seg->size = seg[1].rom_offset - seg->rom_offset;
        } else { // This is the last segment or next is invalid
            seg->size = g_RomFileSize - seg->rom_offset;
        }
        if ((s32)seg->size < 0) { // Check for logic error or bad offset
            #ifdef PDDEBUG
            sysLogPrintf(LOG_ERROR, "Segment %s calculated negative size. ROM Offset: 0x%X", seg->name, seg->rom_offset);
            #endif
            seg->size = 0; // Prevent further issues
            seg->source = SRC_UNLOADED;
            return;
        }
	}

    if (seg->source == SRC_ROM_IN_FILE) { // Stream from ROM, or load+preprocess if required
        if (!g_RomFp || seg->size == 0 || seg->rom_offset + seg->size > g_RomFileSize) {
            #ifdef PDDEBUG
            sysLogPrintf(LOG_ERROR, "Segment %s invalid for streaming (ROM present: %d, offset: 0x%X, size: %u) size/offset issue.", seg->name, !!g_RomFp, seg->rom_offset, seg->size);
            #endif
            seg->source = SRC_UNLOADED;
            return;
        }
        
        // If the segment needs preprocessing (eg. textures list, mpconfigs, fonts),
        // we must load the segment into RAM and preprocess, then expose the RAM pointer via REF_SEG.
        // Exception: animations uses a tiny metadata table; handle it specially to keep RAM low.
        // Also: Do NOT preprocess audio banks (sfxctl, seqctl) or sequences here — audio code
        // expects original ROM layout and does its own runtime patching.
        const bool is_audio_bank = (strcmp(seg->name, "sfxctl") == 0) || (strcmp(seg->name, "seqctl") == 0);
        const bool is_sequences   = (strcmp(seg->name, "sequences") == 0);
        // For audio data tables (sfxtbl/seqtbl), do not force RAM: they stream via audiodma/osPiStartDma.
        if (seg->preprocess && seg->preprocess != preprocessAnimations) {
            // Load whole segment to RAM
            seg->data = sysMemVolAlloc(seg->size);
            if (!seg->data) {
                sysFatalError("Could not allocate %u bytes for segment %s preprocess", seg->size, seg->name);
            }
            s32 n = romdataReadFromRom(seg->rom_offset, seg->data, seg->size);
            if (n < (s32)seg->size) {
                sysMemFree(seg->data); seg->data = NULL;
                sysFatalError("Failed reading segment %s for preprocess", seg->name);
            }
            u8 *processed = seg->preprocess(seg->data, seg->size, &seg->size);
            if (processed && processed != seg->data) {
                sysMemFree(seg->data);
                seg->data = processed;
            }
            seg->source = SRC_ROM_LOADED; // Now RAM‑backed
            romdataUpdateSegStartEnd(seg);
            seg->preprocessed = 1;
            return;
        }

        // Animations: publish small preprocessed metadata table, stream frame/header payloads
        if (seg->preprocess == preprocessAnimations) {
            const u32 animtbl_len = 0x38a0; // as per linker script
            u32 animtbl_off = seg->rom_offset + seg->size - animtbl_len;
            u8 *tbl = sysMemVolAlloc(animtbl_len);
            if (!tbl) {
                sysFatalError("Failed to alloc animations table");
            }
            s32 n = romdataReadFromRom(animtbl_off, tbl, animtbl_len);
            if (n < (s32)animtbl_len) {
                sysMemFree(tbl);
                sysFatalError("Failed to read animations table from ROM");
            }
            u32 *animtbl = (u32*)tbl;
            *animtbl = PD_BE32(*animtbl);
            const u32 count = *animtbl++;
            struct animtableentry *anim = (struct animtableentry *)animtbl;
            for (u32 i = 0; i < count; ++i, ++anim) {
                anim->numframes     = PD_BE16(anim->numframes);
                anim->bytesperframe = PD_BE16(anim->bytesperframe);
                anim->headerlen     = PD_BE16(anim->headerlen);
                anim->data          = PD_BE32(anim->data);
#ifndef PLATFORM_N64
                extern s32 modAnimationLoadDescriptor(u16 num, struct animtableentry *anim);
                if (modAnimationLoadDescriptor((u16)i, anim) > 0) {
                    anim->data = 0xffffffff;
                }
#endif
            }
            _animationsTableRomStart = tbl;
            _animationsTableRomEnd = tbl + animtbl_len;
            seg->preprocessed = 1;
            // Stream the actual segment payload
            if (seg->segstart) *seg->segstart = (u8*)(uintptr_t)ROMPTR_FROM_OFFSET(seg->rom_offset);
            if (seg->segend)   *seg->segend   = (u8*)(uintptr_t)ROMPTR_FROM_OFFSET(seg->rom_offset + seg->size);
            seg->data = NULL;
            return;
        }

        // Default: stream segment directly via encoded ROM pointers
        if (seg->segstart) *seg->segstart = (u8*)(uintptr_t)ROMPTR_FROM_OFFSET(seg->rom_offset);
        if (seg->segend)   *seg->segend   = (u8*)(uintptr_t)ROMPTR_FROM_OFFSET(seg->rom_offset + seg->size);
        seg->data = NULL;
        return;
    }
    
    if (!seg->data && seg->source != SRC_UNLOADED) { // Failed to load for some reason (external load path)
        sysFatalError("Segment %s has no data after load attempt.", seg->name);
        return;
    }


	romdataUpdateSegStartEnd(seg);

	// Call the post load function if any
    if (seg->preprocess && !seg->preprocessed && seg->data) {
        u8* processedData = seg->preprocess(seg->data, seg->size, &seg->size); // seg->size can be updated

		if (processedData) {
			if (processedData != seg->data) { // New buffer returned
				if (seg->source == SRC_EXTERNAL || seg->source == SRC_ROM_LOADED) {
					sysMemFree(seg->data); // Free original buffer
#ifdef PDDEBUG
					sysLogPrintf(LOG_NOTE, "Freed original buffer for segment %s after preprocessing", seg->name);
#endif
				}
				seg->data = processedData;
			}
			romdataUpdateSegStartEnd(seg);
			seg->preprocessed = 1;
		} else {
			#ifdef PDDEBUG
			sysLogPrintf(LOG_WARNING, "Preprocessing segment %s returned NULL. Segment might be unusable.", seg->name);
			#endif
			seg->preprocessed = 1; // Treat as done to avoid repeated attempts
		}
	}
}

static inline s32 romdataLoadExternalFileList(void)
{
    // Free existing romDataSeg if it was from ROM, to replace with filenames.lst
    if (romDataSeg) {
        sysMemFree(romDataSeg);
        romDataSeg = NULL;
        romDataSegSize = 0;
    }

	romDataSeg = fsFileLoad("filenames.lst", &romDataSegSize); 
	if (!romDataSeg || !romDataSegSize) {
		return 0;
	}

	s32 n = 1; // File numbers are 1-indexed
	char *p = (char *)romDataSeg;
	while (*p && n < ROMDATA_MAX_FILES) {
		while (*p && isspace((unsigned char)*p)) ++p; // Skip whitespace
		if (*p) {
			const char *start = p;
			while (*p && !isspace((unsigned char)*p)) ++p; // Skip to next whitespace
			if (*p) { // If not end of buffer
				*p++ = '\0'; // Null terminate
			}
            if (fileSlots[n].source != SRC_EXTERNAL) { // Don't overwrite already loaded external meta
                fileSlots[n].name = start; // Points into romDataSeg (filenames.lst buffer)
                // Other fields (.data, .size, .rom_offset) remain as they were or default
                // .source should be SRC_UNLOADED if relying on this name for later external load
                fileSlots[n].source = SRC_UNLOADED; // Mark as needing load (external)
            }
			n++;
		}
	}
    // Note: romDataSeg now holds filenames.lst content. It needs to be freed in shutdown.
	return n - 1;
}

static inline void romdataInitFiles(void)
{
	if (!g_RomFp) { // No ROM file available
		if (!romdataLoadExternalFileList()) {
			sysFatalError("No ROM file or external filename table (filenames.lst) found.");
		}
		return;
	}

	// romDataSeg is already loaded and inflated from ROM by romdataLoadRom()
	const u32 *rom_disk_offsets = (u32 *)(romDataSeg + ROMDATA_FILES_OFS);
	u32 current_file_idx = 1; // Files are 1-indexed
	u32 name_table_main_rom_offset = 0;
	s32 actual_num_files = 0;

	while(current_file_idx < ROMDATA_MAX_FILES && rom_disk_offsets[current_file_idx] != 0) {
		u32 offset_entry1 = PD_BE32(rom_disk_offsets[current_file_idx]);
		u32 offset_entry2 = PD_BE32(rom_disk_offsets[current_file_idx + 1]); // Next entry

		if (offset_entry2 != 0) { // This is a regular file entry
			fileSlots[current_file_idx].rom_offset = offset_entry1;
			fileSlots[current_file_idx].size = offset_entry2 - offset_entry1;
			fileSlots[current_file_idx].data = NULL; // Not loaded yet
			fileSlots[current_file_idx].source = SRC_ROM_IN_FILE;
			fileSlots[current_file_idx].preprocessed = 0;
			// .name will be set later
            if (fileSlots[current_file_idx].size == 0) {
                #ifdef PDDEBUG
                 sysLogPrintf(LOG_WARNING, "File %d from ROM has zero size. ROM Offset: 0x%X", current_file_idx, offset_entry1);
                #endif
                }
            if (offset_entry1 + fileSlots[current_file_idx].size > g_RomFileSize) {
                #ifdef PDDEBUG
                sysLogPrintf(LOG_ERROR, "File %d from ROM (offset 0x%X, size %u) exceeds ROM size %u.", 
                             current_file_idx, offset_entry1, fileSlots[current_file_idx].size, g_RomFileSize);
                #endif
                fileSlots[current_file_idx].source = SRC_UNLOADED; // Mark as invalid
            }
			actual_num_files = current_file_idx;
		} else { // Next entry is 0, so current offset_entry1 is for the name table block
			name_table_main_rom_offset = offset_entry1;
			break; 
		}
		current_file_idx++;
	}
    if (current_file_idx >= ROMDATA_MAX_FILES && rom_disk_offsets[current_file_idx] != 0) {
        #ifdef PDDEBUG
        sysLogPrintf(LOG_WARNING, "File table in ROM exceeds ROMDATA_MAX_FILES. Some files may be ignored.");
        #endif
    }


	// Load and parse the name table block from ROM
	if (name_table_main_rom_offset != 0 && actual_num_files > 0) {
        // Step 1: Read the array of relative name offsets from the start of the name table block.
        // We need to know how many name offsets there are. Assume it's actual_num_files + 1 (for 1-based indexing).
        u32 num_name_offsets_to_read = actual_num_files + 2; // Read for files 1..N, slot 0, and a terminator
        if (num_name_offsets_to_read > ROMDATA_MAX_FILES) num_name_offsets_to_read = ROMDATA_MAX_FILES;

        u32* temp_name_relative_offsets_from_rom = sysMemVolAlloc(num_name_offsets_to_read * sizeof(u32));
        if (!temp_name_relative_offsets_from_rom) {
            sysFatalError("Failed to alloc for temp name offsets");
        }

        fseek(g_RomFp, name_table_main_rom_offset, SEEK_SET);
        if (fread(temp_name_relative_offsets_from_rom, sizeof(u32), num_name_offsets_to_read, g_RomFp) != num_name_offsets_to_read) {
             sysMemFree(temp_name_relative_offsets_from_rom);
             sysFatalError("Failed to read name offsets array from ROM 0x%X", name_table_main_rom_offset);
        }

        // Step 2 & 3: Find the extent of the string data to determine total block size.
        u32 max_string_data_relative_offset = 0;
        for (s32 i = 1; i <= actual_num_files; ++i) {
            u32 current_rel_offset = PD_BE32(temp_name_relative_offsets_from_rom[i]);
            if (current_rel_offset > max_string_data_relative_offset) {
                max_string_data_relative_offset = current_rel_offset;
            }
        }
        
        u32 end_of_name_block_relative_offset = max_string_data_relative_offset;
        if (max_string_data_relative_offset > 0) { // If there are any names
            // Find length of the string at the furthest relative offset
            char temp_name_char_buffer[256]; // Assume names (paths) aren't excessively long
            fseek(g_RomFp, name_table_main_rom_offset + max_string_data_relative_offset, SEEK_SET);
            size_t name_bytes_read = fread(temp_name_char_buffer, 1, sizeof(temp_name_char_buffer) -1, g_RomFp);
            temp_name_char_buffer[name_bytes_read] = '\0'; // Ensure null termination
            end_of_name_block_relative_offset = max_string_data_relative_offset + strlen(temp_name_char_buffer) + 1;
        } else { // No names, or all offsets are 0
             end_of_name_block_relative_offset = (actual_num_files + 1) * sizeof(u32); // just size of offsets table
        }
        sysMemFree(temp_name_relative_offsets_from_rom); temp_name_relative_offsets_from_rom = NULL;


        // Step 4: Allocate buffer and read the entire name table block.
        g_RomFileNameBlockSize = end_of_name_block_relative_offset;
        if (g_RomFileNameBlockSize > 0) {
            g_RomFileNameBlockBuffer = sysMemVolAlloc(g_RomFileNameBlockSize);
            if (!g_RomFileNameBlockBuffer) {
                sysFatalError("Failed to allocate %u for name block", g_RomFileNameBlockSize);
            }
            fseek(g_RomFp, name_table_main_rom_offset, SEEK_SET);
            if (fread(g_RomFileNameBlockBuffer, 1, g_RomFileNameBlockSize, g_RomFp) != g_RomFileNameBlockSize) {
                sysMemFree(g_RomFileNameBlockBuffer); g_RomFileNameBlockBuffer = NULL; g_RomFileNameBlockSize = 0;
                sysFatalError("Failed to read name block from ROM 0x%X", name_table_main_rom_offset);
            }

            const u32 *name_rel_offsets_ptr_in_buffer = (const u32*) g_RomFileNameBlockBuffer;
            for (s32 name_assign_idx = 1; name_assign_idx <= actual_num_files; ++name_assign_idx) {
                u32 rel_offset = PD_BE32(name_rel_offsets_ptr_in_buffer[name_assign_idx]);
                if (rel_offset != 0 && rel_offset < g_RomFileNameBlockSize) {
                    // Only assign if the file slot is intended for ROM_IN_FILE, not already external etc.
                    if (fileSlots[name_assign_idx].source == SRC_ROM_IN_FILE || fileSlots[name_assign_idx].source == SRC_UNLOADED) {
                         fileSlots[name_assign_idx].name = (const char*)(g_RomFileNameBlockBuffer + rel_offset);
                    }
                } else {
                    // fileSlots[name_assign_idx].name = NULL; // Or some default error string
                    #ifdef PDDEBUG
                    sysLogPrintf(LOG_WARNING, "File %d has invalid name offset %u in name block.", name_assign_idx, rel_offset);
                    #endif
                }
            }
        } else {
            #ifdef PDDEBUG
            sysLogPrintf(LOG_WARNING, "Name table block size calculated as zero. No file names loaded from ROM.");
            #endif
        }
	} else if (g_RomFp) { // ROM is open, but no name table found or no files.
        #ifdef PDDEBUG
        sysLogPrintf(LOG_WARNING, "No file name table processed from ROM. File names will be unavailable unless from filenames.lst.");
        #endif
        // Attempt to load external file list as a fallback
        if (!romdataLoadExternalFileList()) {
            #ifdef PDDEBUG
			sysLogPrintf(LOG_WARNING, "Fallback to filenames.lst also failed or file not found.");
            #endif
		}
    }
}


static inline struct romfile *romdataGetSeg(const char *name)
{
	struct romfile *seg = romSegs;
	while (seg->name && strcmp(name, seg->name)) {
		++seg;
	}
    if (!seg->name) { // Not found
        #ifdef PDDEBUG
        sysLogPrintf(LOG_ERROR, "Segment '%s' not found in romSegs table.", name);
        #endif
        // Return a dummy or handle error, for now, return the terminator
        return &romSegs[ARRAYCOUNT(romSegs)-1]; 
    }
	return seg;
}

s32 romdataInit(void)
{
	const char *altRomName = sysArgGetString("--rom-file");
	if (altRomName) {
		romName = altRomName;
	}

    // Initialize fileSlots sources to UNLOADED and data to NULL
    for (int i = 0; i < ROMDATA_MAX_FILES; ++i) {
        // Preserve patches if already set (like for FILE_USETUPLUE)
        const struct romfilepatch *patches = fileSlots[i].patches;
        u32 numpatches = fileSlots[i].numpatches;
        memset(&fileSlots[i], 0, sizeof(struct romfile)); // Zero out
        fileSlots[i].source = SRC_UNLOADED;
        fileSlots[i].patches = patches; // Restore
        fileSlots[i].numpatches = numpatches; // Restore
    }


	romdataLoadRom(); // Opens g_RomFp, loads and inflates romDataSeg

    // Initialize segments: load from external or read from g_RomFp into RAM
	for (struct romfile *seg = romSegs; seg->name; ++seg) {
		romdataInitSegment(seg);
	}

	// Initialize file metadata (offsets, sizes, names) from romDataSeg and g_RomFp
	romdataInitFiles();
    #ifdef PDDEBUG
	sysLogPrintf(LOG_NOTE, "romdataInit: ROM processing complete. ROM Size: %u", g_RomFileSize);
    #endif

	return 0;
}

// NEW: Shutdown function
void romdataShutdown(void) {
#ifdef PDDEBUG
    sysLogPrintf(LOG_NOTE, "romdataShutdown: Cleaning up ROM data.");
#endif
    // Free data for segments
    for (struct romfile *seg = romSegs; seg->name; ++seg) {
        if ((seg->source == SRC_ROM_LOADED || seg->source == SRC_EXTERNAL) && seg->data) {
            sysMemFree(seg->data);
            seg->data = NULL;
        }
    }

    // Free data for fileSlots
    for (int i = 0; i < ROMDATA_MAX_FILES; ++i) {
        if ((fileSlots[i].source == SRC_ROM_LOADED || fileSlots[i].source == SRC_EXTERNAL) && fileSlots[i].data) {
            sysMemFree(fileSlots[i].data);
            fileSlots[i].data = NULL;
        }
        // Note: fileSlots[i].name might point into g_RomFileNameBlockBuffer or romDataSeg (if from filenames.lst)
        // It does not need separate freeing per slot if those main buffers are freed.
    }

    // Free streamed segment LRU cache
    for (u32 i = 0; i < MAX_STREAMED_SEGMENTS; ++i) {
        if (g_StreamedSegmentCache[i].data) {
            sysMemFree(g_StreamedSegmentCache[i].data);
            g_StreamedSegmentCache[i].data = NULL;
            g_StreamedSegmentCache[i].name = NULL;
            g_StreamedSegmentCache[i].size = 0;
        }
    }

    if (g_RomFileNameBlockBuffer) {
        sysMemFree(g_RomFileNameBlockBuffer);
        g_RomFileNameBlockBuffer = NULL;
        g_RomFileNameBlockSize = 0;
    }

    if (romDataSeg) { // romDataSeg could be from ROM inflation or filenames.lst
        sysMemFree(romDataSeg);
        romDataSeg = NULL;
        romDataSegSize = 0;
    }

    if (g_RomFp) {
        fclose(g_RomFp);
        g_RomFp = NULL;
    }
#ifndef PLATFORM_N64
    for (u32 i = 0; i < ROM_STREAM_CACHE_SLOTS; ++i) {
        if (g_RomCache[i].buf) {
            sysMemFree(g_RomCache[i].buf);
            g_RomCache[i].buf = NULL;
            g_RomCache[i].base = 0;
            g_RomCache[i].size = 0;
        }
    }
#endif
#ifdef PDDEBUG
    sysLogPrintf(LOG_NOTE, "romdataShutdown: Cleanup complete.");
#endif
}


static inline bool romdataCheckGbcRomContents(const u8 *gbcRomFile, const u32 gbcRomSize)
{
	if (gbcRomSize != GBC_ROM_SIZE) {
		return false;
	}
	if (memcmp(gbcRomFile + 0x134, "PerfDark   VPDE", 15) != 0) return false;
	if (memcmp(gbcRomFile + 0x144, "4Y", 2) != 0) return false;
	if (gbcRomFile[0x14D] != 0xA1 || gbcRomFile[0x14E] != 0xAD || gbcRomFile[0x14F] != 0x0F) return false;
	return true;
}

s32 romdataCheckGbcRom(void)
{
	if (fsFileSize(GBC_ROM_NAME) < 0) {
		return false;
	}
	u32 gbcRomSize = 0;
	u8 *gbcRomFile = fsFileLoad(GBC_ROM_NAME, &gbcRomSize);
	if (!gbcRomFile) return false;
	const bool ret = romdataCheckGbcRomContents(gbcRomFile, gbcRomSize);
	sysMemFree(gbcRomFile);
    #ifdef PDDEBUG
	if (ret) sysLogPrintf(LOG_NOTE, "romdataCheckGbcRom: valid GBC rom found");
    #endif
	return ret;
}

s32 romdataFileGetSize(s32 fileNum)
{
	if (fileNum < 1 || fileNum >= ROMDATA_MAX_FILES) {
        #ifdef PDDEBUG
		sysLogPrintf(LOG_ERROR, "romdataFileGetSize: invalid file num %d", fileNum);
        #endif
		return -1;
	}
    // If data is already loaded, size is known.
    // If SRC_ROM_IN_FILE or SRC_UNLOADED (for external), .size should be set by romdataInitFiles or romdataLoadExternalFileList
    // The romdataFileLoad call is mostly to ensure external files are checked and their size potentially updated.
	if (romdataFileLoad(fileNum, NULL) != NULL) { // Call load to ensure it's processed if external, or just to check status
        return fileSlots[fileNum].size;
    }
    // If still unloaded (e.g. romdataFileLoad failed or it's purely ROM_IN_FILE and not yet loaded to RAM)
    // .size should still be valid if it was from romdataInitFiles.
    if (fileSlots[fileNum].source == SRC_ROM_IN_FILE && fileSlots[fileNum].size > 0) {
        return fileSlots[fileNum].size;
    }
    #ifdef PDDEBUG
	sysLogPrintf(LOG_ERROR, "romdataFileGetSize: could not determine size for file num %d (name: %s, source: %d)", 
                 fileNum, fileSlots[fileNum].name ? fileSlots[fileNum].name : "N/A", fileSlots[fileNum].source);
    #endif
	return -1;
}

u8 *romdataFileGetData(s32 fileNum)
{
    // This function implies the data is already loaded.
    // Let's ensure romdataFileLoad is called to actually load it if it's not.
	return romdataFileLoad(fileNum, NULL);
}

u8 *romdataFileLoad(s32 fileNum, u32 *outSize)
{
	if (fileNum < 1 || fileNum >= ROMDATA_MAX_FILES) {
        #ifdef PDDEBUG
		sysLogPrintf(LOG_ERROR, "romdataFileLoad: invalid file num %d", fileNum);
        #endif
		return NULL;
	}

    struct romfile *file = &fileSlots[fileNum];

    // If already loaded (from external or ROM), return existing data
    if ((file->source == SRC_EXTERNAL || file->source == SRC_ROM_LOADED) && file->data) {
        if (outSize) *outSize = file->size;
        return file->data;
    }

    // If not loaded externally, and it's marked as being in ROM file but not yet in RAM,
    // return a special encoded pointer which represents the ROM file offset. The DMA layer
    // will detect and stream from disk directly instead of bcopy-ing from memory.
    if (file->source == SRC_ROM_IN_FILE) {
        if (g_RomFp && file->rom_offset > 0 && file->size > 0 && file->rom_offset + file->size <= g_RomFileSize) {
            if (outSize) *outSize = file->size;
#ifndef PLATFORM_N64
            return (u8*)(uintptr_t)ROMPTR_FROM_OFFSET(file->rom_offset);
#else
            return NULL;
#endif
        } else {
#ifdef PDDEBUG
            sysLogPrintf(LOG_WARNING, "File %d (%s) marked SRC_ROM_IN_FILE but invalid (ROM present: %d, offset: 0x%X, size: %u)",
                         fileNum, file->name ? file->name : "N/A", !!g_RomFp, file->rom_offset, file->size);
#endif
        }
    }

    // If we reach here, file was not loaded or streamable
    // If it was already loaded, we returned earlier.
    // This path means it's UNLOADED and not found externally, or ROM_IN_FILE and failed to load from ROM.
	if (outSize) *outSize = 0; // Default to 0 if not loaded
	return NULL; // Could not load
}


void romdataFilePreprocess(s32 fileNum, s32 loadType, u8 *data, u32 size, u32 *outSize)
{
	if (fileNum < 1 || fileNum >= ROMDATA_MAX_FILES) {
        #ifdef PDDEBUG
		sysLogPrintf(LOG_ERROR, "romdataFilePreprocess: invalid file num %d", fileNum);
        #endif
		return;
	}

    struct romfile *file = &fileSlots[fileNum];

	// Patches are applied if the data came from the ROM (SRC_ROM_LOADED or SRC_ROM_IN_FILE)
    // and has not been preprocessed yet in this load cycle.
	if (data && size && (file->source == SRC_ROM_LOADED || file->source == SRC_ROM_IN_FILE) /*&& !file->preprocessed*/) {
		if (loadType > 0 && (u32)loadType < ARRAYCOUNT(filePreprocFuncs) && filePreprocFuncs[loadType]) {
			// Apply patches
			for (u32 i = 0; i < file->numpatches; ++i) {
				const struct romfilepatch *p = &file->patches[i];
                if (p->ofs + p->len <= size) { // Bounds check
                    if (!memcmp(data + p->ofs, p->src, p->len)) {
                        memcpy(data + p->ofs, p->dst, p->len);
                        #ifdef PDDEBUG
                        sysLogPrintf(LOG_NOTE, "File %d (%s) patched at offset 0x%x", fileNum, file->name, p->ofs);
                        #endif
                    }
                } else {
                    #ifdef PDDEBUG
                     sysLogPrintf(LOG_WARNING, "File %d (%s) patch at offset 0x%x out of bounds (size %u).", fileNum, file->name, p->ofs, size);
                    #endif
                    }
			}
			// Then preprocess. Preprocessing function might update the size via outSize.
            // It operates on the 'data' buffer passed in.
			filePreprocFuncs[loadType](data, size, outSize); 
			// file->preprocessed = 1; // Mark as preprocessed for this instance of data.
                                     // If data is freed and reloaded, it would need preprocessing again.
		} else if (outSize) {
            *outSize = size; // No preprocessing, size remains same
        }
	} else if (outSize) {
        *outSize = size; // No preprocessing (not ROM, or no func), size remains same
    }
}

void romdataFileFree(s32 fileNum)
{
	if (fileNum < 1 || fileNum >= ROMDATA_MAX_FILES) {
        #ifdef PDDEBUG
		sysLogPrintf(LOG_ERROR, "romdataFileFree: invalid file num %d", fileNum); // Corrected log from fsFileFree
        #endif
		return;
	}

    struct romfile *file = &fileSlots[fileNum];

	if ((file->source == SRC_EXTERNAL || file->source == SRC_ROM_LOADED) && file->data) {
		sysMemFree(file->data);
		file->data = NULL;
        // file->size remains as the original size of the file, not reset to 0.
	}

    // Reset source to allow reloading
    if (file->source == SRC_EXTERNAL) {
	    file->source = SRC_UNLOADED; // Can be checked externally again
    } else if (file->source == SRC_ROM_LOADED) {
        file->source = SRC_ROM_IN_FILE; // Can be re-read from ROM
    }
    file->preprocessed = 0; // Reset preprocessed state
}

const char *romdataFileGetName(s32 fileNum)
{
	if (fileNum < 1 || fileNum >= ROMDATA_MAX_FILES) {
		return NULL;
	}
    // Name should be set during romdataInitFiles or romdataLoadExternalFileList
	return fileSlots[fileNum].name;
}

s32 romdataFileGetNumForName(const char *name)
{
	if (!name || !name[0]) {
		return -1;
	}
	for (s32 i = 1; i < ROMDATA_MAX_FILES; ++i) { // Files are 1-indexed
		if (fileSlots[i].name && strcmp(fileSlots[i].name, name) == 0) {
			return i;
		}
	}
	return -1;
}

u8 *romdataSegGetData(const char *segName) {
    struct romfile *seg = romdataGetSeg(segName);
    if (seg && seg->data) return seg->data;
#ifdef PDDEBUG
    sysLogPrintf(LOG_WARNING, "romdataSegGetData: Segment '%s' has no data.", segName);
#endif
    return NULL;
}

u8 *romdataSegGetPersistent(const char *segName) {
    struct romfile *seg = romdataGetSeg(segName);
    if (!seg || seg->source != SRC_ROM_IN_FILE || seg->size == 0) return seg ? seg->data : NULL;

    for (u32 i = 0; i < MAX_STREAMED_SEGMENTS; ++i) {
        if (g_StreamedSegmentCache[i].name && strcmp(g_StreamedSegmentCache[i].name, segName) == 0) {
            return g_StreamedSegmentCache[i].data;
        }
    }

    u8 *raw = romdataSegGetData(segName);
    if (!raw) sysFatalError("Failed to stream %s", segName);

    u32 idx = g_StreamedSegmentLRU++ % MAX_STREAMED_SEGMENTS;
    if (g_StreamedSegmentCache[idx].data) sysMemFree(g_StreamedSegmentCache[idx].data);

    g_StreamedSegmentCache[idx].data = raw;
    g_StreamedSegmentCache[idx].name = segName;
    g_StreamedSegmentCache[idx].size = seg->size;

    return raw;
}

u8 *romdataSegGetDataEnd(const char *segName) {
    struct romfile *seg = romdataGetSeg(segName);
    if (seg && seg->data) return seg->data + seg->size;
    #ifdef PDDEBUG
    sysLogPrintf(LOG_WARNING, "romdataSegGetDataEnd: Segment '%s' has no data/size.", segName);
    #endif
    return NULL;
}

u32 romdataSegGetSize(const char *segName) {
    struct romfile *seg = romdataGetSeg(segName);
    if (seg) return seg->size;
    #ifdef PDDEBUG
    sysLogPrintf(LOG_WARNING, "romdataSegGetSize: Segment '%s' not found or has no size.", segName);
    #endif
    return 0;
}

// romdataFileGetEstimatedSize remains the same as it's platform-dependent hinting.
u32 romdataFileGetEstimatedSize(const u32 size, const u32 loadtype)
{
#ifdef PLATFORM_64BIT
	switch (loadtype) {
	case LOADTYPE_BG:	   return (u32)(size * 1.1f);
	case LOADTYPE_TILES: return (u32)(size * 1.1f);
	case LOADTYPE_LANG:  return (u32)(size * 1.3f);
	case LOADTYPE_SETUP: return (u32)(size * 1.5f);
	case LOADTYPE_PADS:  return (u32)(size * 1.7f);
	case LOADTYPE_MODEL: return (u32)(size * 1.7f);
	case LOADTYPE_GUN: return (u32)(size * 1.7f);
	default:
    #ifdef PDDEBUG
		sysLogPrintf(LOG_WARNING, "romdataFileGetEstimatedSize: wrong loadtype %d", loadtype);
    #endif
	}
#endif
	return size;
}
