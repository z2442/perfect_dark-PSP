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

static FILE *g_RomFp = NULL;
static u32 g_RomFpPos = 0;
u32 g_RomFileSize;
static char g_RomPath[FS_MAXPATH + 1];

static u8 *romDataSeg;
static u32 romDataSegSize;
static const char *romName = ROMDATA_ROM_NAME;

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

// ============================================================================
// IMPROVED ROM CACHE - Hash-based with proper LRU and prefetching
// ============================================================================
#ifndef PLATFORM_N64

// Increased cache size for better hit rate (1MB total vs 1MB before)
#define ROM_CACHE_SLOTS 64
#define ROM_CACHE_CHUNK_SIZE (32 * 1024)  // 32KB chunks for better locality
#define ROM_CACHE_HASH_SIZE 256  // Must be power of 2

// Prefetch configuration
#define ROM_PREFETCH_CHUNKS 2  // Number of chunks to prefetch ahead
#define ROM_SEQ_THRESHOLD 3    // Number of sequential reads to trigger aggressive prefetch

typedef struct RomCacheSlot {
    u32 base;           // Aligned base offset in ROM
    u32 size;           // Valid bytes in buffer
    u32 access_time;    // LRU timestamp
    u32 access_count;   // Access frequency counter
    u8 *buf;            // Data buffer (pre-allocated)
    struct RomCacheSlot *hash_next;  // Hash chain
    struct RomCacheSlot *lru_prev;   // LRU doubly-linked list
    struct RomCacheSlot *lru_next;
} RomCacheSlot;

static RomCacheSlot g_RomCache[ROM_CACHE_SLOTS];
static RomCacheSlot *g_RomCacheHash[ROM_CACHE_HASH_SIZE];
static RomCacheSlot *g_RomCacheLRUHead = NULL;  // Most recently used
static RomCacheSlot *g_RomCacheLRUTail = NULL;  // Least recently used
static u32 g_RomCacheTime = 0;  // Global timestamp for LRU

// Sequential access tracking
static u32 g_RomLastReadEnd = 0;
static u32 g_RomSeqCount = 0;  // Count of consecutive sequential reads

// Cache statistics
static struct {
    u32 hits;
    u32 misses;
    u32 evictions;
    u32 prefetches;
} g_RomCacheStats;

// Hash function for cache lookup
static inline u32 romCacheHash(u32 base) {
    // Simple hash - base is already aligned to chunk size
    return (base / ROM_CACHE_CHUNK_SIZE) & (ROM_CACHE_HASH_SIZE - 1);
}

// Move slot to front of LRU list (most recently used)
static inline void romCacheLRUTouch(RomCacheSlot *slot) {
    if (!slot || slot == g_RomCacheLRUHead) return;
    
    // Remove from current position
    if (slot->lru_prev) slot->lru_prev->lru_next = slot->lru_next;
    if (slot->lru_next) slot->lru_next->lru_prev = slot->lru_prev;
    if (slot == g_RomCacheLRUTail) g_RomCacheLRUTail = slot->lru_prev;
    
    // Insert at head
    slot->lru_prev = NULL;
    slot->lru_next = g_RomCacheLRUHead;
    if (g_RomCacheLRUHead) g_RomCacheLRUHead->lru_prev = slot;
    g_RomCacheLRUHead = slot;
    if (!g_RomCacheLRUTail) g_RomCacheLRUTail = slot;
    
    slot->access_time = ++g_RomCacheTime;
    slot->access_count++;
}

// Find slot in cache by base offset
static inline RomCacheSlot *romCacheLookup(u32 base) {
    u32 hash = romCacheHash(base);
    RomCacheSlot *slot = g_RomCacheHash[hash];
    
    while (slot) {
        if (slot->base == base && slot->size > 0) {
            romCacheLRUTouch(slot);
            g_RomCacheStats.hits++;
            return slot;
        }
        slot = slot->hash_next;
    }
    
    g_RomCacheStats.misses++;
    return NULL;
}

// Evict LRU slot and prepare for reuse
static inline RomCacheSlot *romCacheEvictLRU(void) {
    RomCacheSlot *victim = g_RomCacheLRUTail;
    if (!victim) return NULL;
    
    // Remove from hash chain
    if (victim->base != 0xffffffffu) {
        u32 hash = romCacheHash(victim->base);
        RomCacheSlot **ptr = &g_RomCacheHash[hash];
        while (*ptr && *ptr != victim) {
            ptr = &(*ptr)->hash_next;
        }
        if (*ptr == victim) *ptr = victim->hash_next;
    }
    
    // Move to front of LRU (will be most recent after fill)
    romCacheLRUTouch(victim);
    
    victim->base = 0xffffffffu;
    victim->size = 0;
    victim->access_count = 0;
    
    g_RomCacheStats.evictions++;
    return victim;
}

// Insert slot into hash table
static inline void romCacheHashInsert(RomCacheSlot *slot) {
    u32 hash = romCacheHash(slot->base);
    slot->hash_next = g_RomCacheHash[hash];
    g_RomCacheHash[hash] = slot;
}

// Fill cache slot with data from ROM
static inline int romCacheFill(RomCacheSlot *slot, u32 base, int is_prefetch) {
    if (!g_RomFp || base >= g_RomFileSize) return 0;
    
    slot->base = base;
    slot->size = 0;
    
    ROM_FSEEK_IF_NEEDED(base);
    u32 to_read = ROM_CACHE_CHUNK_SIZE;
    if (base + to_read > g_RomFileSize) {
        to_read = g_RomFileSize - base;
    }
    
    size_t n = fread(slot->buf, 1, to_read, g_RomFp);
    g_RomFpPos += (u32)n;
    slot->size = (u32)n;
    
    if (slot->size > 0) {
        romCacheHashInsert(slot);
        if (is_prefetch) g_RomCacheStats.prefetches++;
        return 1;
    }
    
    slot->base = 0xffffffffu;
    return 0;
}

// Prefetch next chunks for sequential access
static inline void romCachePrefetch(u32 current_base, int aggressive) {
    int chunks_to_prefetch = aggressive ? ROM_PREFETCH_CHUNKS * 2 : ROM_PREFETCH_CHUNKS;
    
    for (int i = 1; i <= chunks_to_prefetch; ++i) {
        u32 prefetch_base = current_base + (ROM_CACHE_CHUNK_SIZE * i);
        if (prefetch_base >= g_RomFileSize) break;
        
        // Check if already cached
        if (romCacheLookup(prefetch_base)) continue;
        
        // Get victim slot and fill
        RomCacheSlot *slot = romCacheEvictLRU();
        if (slot && slot->buf) {
            romCacheFill(slot, prefetch_base, 1);
        }
    }
}

// Initialize ROM cache system
static inline void romCacheInit(void) {
    memset(&g_RomCacheStats, 0, sizeof(g_RomCacheStats));
    memset(g_RomCacheHash, 0, sizeof(g_RomCacheHash));
    
    // Pre-allocate all cache buffers and build LRU chain
    for (u32 i = 0; i < ROM_CACHE_SLOTS; ++i) {
        g_RomCache[i].base = 0xffffffffu;
        g_RomCache[i].size = 0;
        g_RomCache[i].access_time = 0;
        g_RomCache[i].access_count = 0;
        g_RomCache[i].hash_next = NULL;
        
        // Pre-allocate buffer
        g_RomCache[i].buf = sysMemVolAlloc(ROM_CACHE_CHUNK_SIZE);
        if (!g_RomCache[i].buf) {
            sysFatalError("Failed to allocate ROM cache slot %u", i);
        }
        
        // Build LRU chain
        g_RomCache[i].lru_prev = (i > 0) ? &g_RomCache[i - 1] : NULL;
        g_RomCache[i].lru_next = (i < ROM_CACHE_SLOTS - 1) ? &g_RomCache[i + 1] : NULL;
    }
    
    g_RomCacheLRUHead = &g_RomCache[0];
    g_RomCacheLRUTail = &g_RomCache[ROM_CACHE_SLOTS - 1];
    
    g_RomLastReadEnd = 0;
    g_RomSeqCount = 0;
}

// Cleanup ROM cache
static inline void romCacheShutdown(void) {
    for (u32 i = 0; i < ROM_CACHE_SLOTS; ++i) {
        if (g_RomCache[i].buf) {
            sysMemFree(g_RomCache[i].buf);
            g_RomCache[i].buf = NULL;
        }
    }
    
#ifdef PDDEBUG
    if (g_RomCacheStats.hits + g_RomCacheStats.misses > 0) {
        float hit_rate = (float)g_RomCacheStats.hits / (g_RomCacheStats.hits + g_RomCacheStats.misses) * 100.0f;
        sysLogPrintf(LOG_NOTE, "ROM Cache Stats: Hits=%u, Misses=%u, Hit Rate=%.2f%%, Evictions=%u, Prefetches=%u",
                     g_RomCacheStats.hits, g_RomCacheStats.misses, hit_rate,
                     g_RomCacheStats.evictions, g_RomCacheStats.prefetches);
    }
#endif
}

#endif // !PLATFORM_N64

// ============================================================================
// End of improved ROM cache
// ============================================================================

enum loadsource {
	SRC_UNLOADED = 0,
	SRC_ROM_IN_FILE,
	SRC_ROM_LOADED,
	SRC_EXTERNAL
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
	u8 *data;
	u32 rom_offset;
	u32 size;
	preprocessfunc preprocess;
	s32 source;
	s32 preprocessed;
	const struct romfilepatch *patches;
	u32 numpatches;
};

static const struct romfilepatch filePatches[] = {
	{ 0x92a2, 1, "\x6c", "\x99" },
	{ 0x92b0, 1, "\x6c", "\x99" },
};

static struct romfile fileSlots[ROMDATA_MAX_FILES] = {
	[FILE_USETUPLUE] = { .patches = &filePatches[0], .numpatches = 2 },
};

#define ROMSEG_START(n) _ ## n ## SegmentRomStart
#define ROMSEG_END(n) _ ## n ## SegmentRomEnd

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

#undef ROMSEG_DECL_SEG
#define ROMSEG_DECL_SEG(name, ofs_ntsc, ofs_pal, ofs_jpn, size, preproc) u8 *ROMSEG_START(name), *ROMSEG_END(name);
ROMSEG_LIST()

u8 *_animationsTableRomStart;
u8 *_animationsTableRomEnd;

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
	{ NULL, NULL, NULL, NULL, 0, 0, NULL, SRC_UNLOADED, 0, NULL, 0 },
};

static preprocessfunc filePreprocFuncs[] = {
	/* LOADTYPE_NONE  */ NULL,
	/* LOADTYPE_BG    */ NULL,
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
        if (nread == 0) break;
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
                sysMemFree(inbuf);
                inflateEnd(&strm);
                return 0;
            }
            if (made == 0 && zret == Z_BUF_ERROR) {
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
    
    s32 rom_size_check = fsFileSize(romName);
    if (rom_size_check < 0) {
        sysFatalError("Could not get size of ROM file %s.\nEnsure that it is in the %s directory.", romName, fsFullPath(""));
    }
    g_RomFileSize = (u32)rom_size_check;

    strncpy(g_RomPath, fsFullPath(romName), sizeof(g_RomPath) - 1);
    g_RomPath[sizeof(g_RomPath) - 1] = '\0';
    g_RomFp = fopen(g_RomPath, "rb");
    if (g_RomFp) {
        // Larger buffer size for better throughput
        setvbuf(g_RomFp, NULL, _IOFBF, 256 * 1024);
    }
	if (!g_RomFp) {
		sysFatalError("Could not open ROM file %s.\nEnsure that it is in the %s directory.", romName, fsFullPath(""));
	}

    unsigned char header_check[4];
    if (fread(header_check, 1, sizeof(header_check), g_RomFp) != sizeof(header_check)) {
        fclose(g_RomFp); g_RomFp = NULL;
        romdataWrongRomError("Could not read initial bytes from ROM.");
    }
    rewind(g_RomFp);
    g_RomFpPos = 0;

	if (!memcmp(header_check, "PK", 2) || !memcmp(header_check, "Rar", 3) || !memcmp(header_check, "7z", 2)) {
        fclose(g_RomFp); g_RomFp = NULL;
		romdataWrongRomError("Your ROM is in an archive file. Please extract it.");
	}

	if (g_RomFileSize != ROMDATA_ROM_SIZE) {
        fclose(g_RomFp); g_RomFp = NULL;
		romdataWrongRomError("ROM size does not match: expected: %u, got: %u.", ROMDATA_ROM_SIZE, g_RomFileSize);
	}

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

    u8 zipped_header[5];
    
    // Initialize improved ROM cache
#ifndef PLATFORM_N64
    romCacheInit();
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
	if (dataSegLen < ROMDATA_FILES_OFS) {
        fclose(g_RomFp); g_RomFp = NULL;
		romdataWrongRomError("Data segment too small (%u), need at least %u for file table.", dataSegLen, ROMDATA_FILES_OFS);
	}

    u32 need_unzipped = ROMDATA_FILES_OFS + (ROMDATA_MAX_FILES + 2) * sizeof(u32);
    if (need_unzipped > dataSegLen) need_unzipped = dataSegLen;

    romDataSeg = sysMemVolAlloc(need_unzipped);
    if (!romDataSeg) {
        fclose(g_RomFp); g_RomFp = NULL;
        sysFatalError("Could not allocate %u bytes for partial data segment.", need_unzipped);
    }

    s32 outbytes = romdataInflatePartialFromFile(ROMDATA_DATA_OFS + 5, romDataSeg, need_unzipped);
    if (outbytes < (s32)need_unzipped) {
        sysMemFree(romDataSeg); romDataSeg = NULL;
        fclose(g_RomFp); g_RomFp = NULL;
        sysFatalError("Partial inflate failed: needed %u, got %d", need_unzipped, outbytes);
    }

    romDataSegSize = need_unzipped;
}

#ifndef PLATFORM_N64
// OPTIMIZED: Read from ROM file using improved cache
s32 romdataReadFromRom(u32 offset, void *dst, u32 len) {
    if ((u64)offset + (u64)len > (u64)g_RomFileSize) return -1;
    if (!dst || len == 0) return 0;

    // Detect sequential access pattern
    const int is_sequential = (offset == g_RomLastReadEnd);
    if (is_sequential) {
        g_RomSeqCount++;
    } else if (offset < g_RomLastReadEnd || offset > g_RomLastReadEnd + ROM_CACHE_CHUNK_SIZE) {
        g_RomSeqCount = 0;  // Reset on jump
    }
    
    const int aggressive_prefetch = (g_RomSeqCount >= ROM_SEQ_THRESHOLD);

    // For very large reads, bypass cache and read directly
    if (len > ROM_CACHE_CHUNK_SIZE * 2) {
        if (!g_RomFp) return -1;
        ROM_FSEEK_IF_NEEDED(offset);
        size_t n = fread(dst, 1, len, g_RomFp);
        g_RomFpPos += (u32)n;
        g_RomLastReadEnd = offset + (u32)n;
        return (s32)n;
    }

    u8 *out = (u8 *)dst;
    u32 remaining = len;
    u32 current_offset = offset;
    s32 total_read = 0;

    while (remaining > 0) {
        const u32 base = current_offset & ~(ROM_CACHE_CHUNK_SIZE - 1);
        const u32 within = current_offset - base;
        const u32 chunk_avail = ROM_CACHE_CHUNK_SIZE - within;
        const u32 to_copy = (remaining < chunk_avail) ? remaining : chunk_avail;

        // Try cache lookup
        RomCacheSlot *slot = romCacheLookup(base);
        
        if (!slot) {
            // Cache miss - evict LRU and fill
            slot = romCacheEvictLRU();
            if (!slot || !slot->buf) return -1;
            
            if (!romCacheFill(slot, base, 0)) {
                return total_read > 0 ? total_read : -1;
            }
            
            // Prefetch if sequential
            if (is_sequential || aggressive_prefetch) {
                romCachePrefetch(base, aggressive_prefetch);
            }
        }

        // Copy from cache
        if (within + to_copy <= slot->size) {
            memcpy(out, slot->buf + within, to_copy);
            total_read += to_copy;
            out += to_copy;
            remaining -= to_copy;
            current_offset += to_copy;
        } else {
            // Partial chunk at end of file
            u32 can_copy = (slot->size > within) ? (slot->size - within) : 0;
            if (can_copy > 0) {
                memcpy(out, slot->buf + within, can_copy);
                total_read += can_copy;
            }
            break;
        }
    }

    g_RomLastReadEnd = offset + total_read;
    return total_read;
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
	if (seg->rom_offset == 0 && seg->source == SRC_ROM_IN_FILE) {
        seg->source = SRC_UNLOADED;
        #ifdef PDDEBUG
		sysLogPrintf(LOG_NOTE, "Skipping segment %s (zero offset)", seg->name);
        #endif
        seg->data = NULL;
        seg->size = 0;
        romdataUpdateSegStartEnd(seg);
		return;
	}

    if (seg->size == 0 && seg->source == SRC_ROM_IN_FILE) {
        if (seg[1].name && seg[1].rom_offset != 0) {
            seg->size = seg[1].rom_offset - seg->rom_offset;
        } else {
            seg->size = g_RomFileSize - seg->rom_offset;
        }
        if ((s32)seg->size < 0) {
            #ifdef PDDEBUG
            sysLogPrintf(LOG_ERROR, "Segment %s calculated negative size. ROM Offset: 0x%X", seg->name, seg->rom_offset);
            #endif
            seg->size = 0;
            seg->source = SRC_UNLOADED;
            return;
        }
	}

    if (seg->source == SRC_ROM_IN_FILE) {
        if (!g_RomFp || seg->size == 0 || seg->rom_offset + seg->size > g_RomFileSize) {
            #ifdef PDDEBUG
            sysLogPrintf(LOG_ERROR, "Segment %s invalid for streaming (ROM present: %d, offset: 0x%X, size: %u) size/offset issue.", seg->name, !!g_RomFp, seg->rom_offset, seg->size);
            #endif
            seg->source = SRC_UNLOADED;
            return;
        }
        
        const bool is_audio_bank = (strcmp(seg->name, "sfxctl") == 0) || (strcmp(seg->name, "seqctl") == 0);
        const bool is_sequences   = (strcmp(seg->name, "sequences") == 0);
        
        if (seg->preprocess && seg->preprocess != preprocessAnimations) {
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
            seg->source = SRC_ROM_LOADED;
            romdataUpdateSegStartEnd(seg);
            seg->preprocessed = 1;
            return;
        }

        if (seg->preprocess == preprocessAnimations) {
            const u32 animtbl_len = 0x38a0;
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
            if (seg->segstart) *seg->segstart = (u8*)(uintptr_t)ROMPTR_FROM_OFFSET(seg->rom_offset);
            if (seg->segend)   *seg->segend   = (u8*)(uintptr_t)ROMPTR_FROM_OFFSET(seg->rom_offset + seg->size);
            seg->data = NULL;
            return;
        }

        if (seg->segstart) *seg->segstart = (u8*)(uintptr_t)ROMPTR_FROM_OFFSET(seg->rom_offset);
        if (seg->segend)   *seg->segend   = (u8*)(uintptr_t)ROMPTR_FROM_OFFSET(seg->rom_offset + seg->size);
        seg->data = NULL;
        return;
    }
    
    if (!seg->data && seg->source != SRC_UNLOADED) {
        sysFatalError("Segment %s has no data after load attempt.", seg->name);
        return;
    }

	romdataUpdateSegStartEnd(seg);

    if (seg->preprocess && !seg->preprocessed && seg->data) {
        u8* processedData = seg->preprocess(seg->data, seg->size, &seg->size);

		if (processedData) {
			if (processedData != seg->data) {
				if (seg->source == SRC_EXTERNAL || seg->source == SRC_ROM_LOADED) {
					sysMemFree(seg->data);
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
			seg->preprocessed = 1;
		}
	}
}

static inline s32 romdataLoadExternalFileList(void)
{
    if (romDataSeg) {
        sysMemFree(romDataSeg);
        romDataSeg = NULL;
        romDataSegSize = 0;
    }

	romDataSeg = fsFileLoad("filenames.lst", &romDataSegSize); 
	if (!romDataSeg || !romDataSegSize) {
		return 0;
	}

	s32 n = 1;
	char *p = (char *)romDataSeg;
	while (*p && n < ROMDATA_MAX_FILES) {
		while (*p && isspace((unsigned char)*p)) ++p;
		if (*p) {
			const char *start = p;
			while (*p && !isspace((unsigned char)*p)) ++p;
			if (*p) {
				*p++ = '\0';
			}
            if (fileSlots[n].source != SRC_EXTERNAL) {
                fileSlots[n].name = start;
                fileSlots[n].source = SRC_UNLOADED;
            }
			n++;
		}
	}
	return n - 1;
}

static inline void romdataInitFiles(void)
{
	if (!g_RomFp) {
		if (!romdataLoadExternalFileList()) {
			sysFatalError("No ROM file or external filename table (filenames.lst) found.");
		}
		return;
	}

	const u32 *rom_disk_offsets = (u32 *)(romDataSeg + ROMDATA_FILES_OFS);
	u32 current_file_idx = 1;
	u32 name_table_main_rom_offset = 0;
	s32 actual_num_files = 0;

	while(current_file_idx < ROMDATA_MAX_FILES && rom_disk_offsets[current_file_idx] != 0) {
		u32 offset_entry1 = PD_BE32(rom_disk_offsets[current_file_idx]);
		u32 offset_entry2 = PD_BE32(rom_disk_offsets[current_file_idx + 1]);

		if (offset_entry2 != 0) {
			fileSlots[current_file_idx].rom_offset = offset_entry1;
			fileSlots[current_file_idx].size = offset_entry2 - offset_entry1;
			fileSlots[current_file_idx].data = NULL;
			fileSlots[current_file_idx].source = SRC_ROM_IN_FILE;
			fileSlots[current_file_idx].preprocessed = 0;
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
                fileSlots[current_file_idx].source = SRC_UNLOADED;
            }
			actual_num_files = current_file_idx;
		} else {
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

	if (name_table_main_rom_offset != 0 && actual_num_files > 0) {
        u32 num_name_offsets_to_read = actual_num_files + 2;
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

        u32 max_string_data_relative_offset = 0;
        for (s32 i = 1; i <= actual_num_files; ++i) {
            u32 current_rel_offset = PD_BE32(temp_name_relative_offsets_from_rom[i]);
            if (current_rel_offset > max_string_data_relative_offset) {
                max_string_data_relative_offset = current_rel_offset;
            }
        }
        
        u32 end_of_name_block_relative_offset = max_string_data_relative_offset;
        if (max_string_data_relative_offset > 0) {
            char temp_name_char_buffer[256];
            fseek(g_RomFp, name_table_main_rom_offset + max_string_data_relative_offset, SEEK_SET);
            size_t name_bytes_read = fread(temp_name_char_buffer, 1, sizeof(temp_name_char_buffer) -1, g_RomFp);
            temp_name_char_buffer[name_bytes_read] = '\0';
            end_of_name_block_relative_offset = max_string_data_relative_offset + strlen(temp_name_char_buffer) + 1;
        } else {
             end_of_name_block_relative_offset = (actual_num_files + 1) * sizeof(u32);
        }
        sysMemFree(temp_name_relative_offsets_from_rom); temp_name_relative_offsets_from_rom = NULL;

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
                    if (fileSlots[name_assign_idx].source == SRC_ROM_IN_FILE || fileSlots[name_assign_idx].source == SRC_UNLOADED) {
                         fileSlots[name_assign_idx].name = (const char*)(g_RomFileNameBlockBuffer + rel_offset);
                    }
                } else {
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
	} else if (g_RomFp) {
        #ifdef PDDEBUG
        sysLogPrintf(LOG_WARNING, "No file name table processed from ROM. File names will be unavailable unless from filenames.lst.");
        #endif
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
    if (!seg->name) {
        #ifdef PDDEBUG
        sysLogPrintf(LOG_ERROR, "Segment '%s' not found in romSegs table.", name);
        #endif
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

    for (int i = 0; i < ROMDATA_MAX_FILES; ++i) {
        const struct romfilepatch *patches = fileSlots[i].patches;
        u32 numpatches = fileSlots[i].numpatches;
        memset(&fileSlots[i], 0, sizeof(struct romfile));
        fileSlots[i].source = SRC_UNLOADED;
        fileSlots[i].patches = patches;
        fileSlots[i].numpatches = numpatches;
    }

	romdataLoadRom();

	for (struct romfile *seg = romSegs; seg->name; ++seg) {
		romdataInitSegment(seg);
	}

	romdataInitFiles();
    #ifdef PDDEBUG
	sysLogPrintf(LOG_NOTE, "romdataInit: ROM processing complete. ROM Size: %u", g_RomFileSize);
    #endif

	return 0;
}

void romdataShutdown(void) {
#ifdef PDDEBUG
    sysLogPrintf(LOG_NOTE, "romdataShutdown: Cleaning up ROM data.");
#endif
    
    for (struct romfile *seg = romSegs; seg->name; ++seg) {
        if ((seg->source == SRC_ROM_LOADED || seg->source == SRC_EXTERNAL) && seg->data) {
            sysMemFree(seg->data);
            seg->data = NULL;
        }
    }

    for (int i = 0; i < ROMDATA_MAX_FILES; ++i) {
        if ((fileSlots[i].source == SRC_ROM_LOADED || fileSlots[i].source == SRC_EXTERNAL) && fileSlots[i].data) {
            sysMemFree(fileSlots[i].data);
            fileSlots[i].data = NULL;
        }
    }

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

    if (romDataSeg) {
        sysMemFree(romDataSeg);
        romDataSeg = NULL;
        romDataSegSize = 0;
    }

#ifndef PLATFORM_N64
    romCacheShutdown();
#endif

    if (g_RomFp) {
        fclose(g_RomFp);
        g_RomFp = NULL;
    }

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
    
	if (romdataFileLoad(fileNum, NULL) != NULL) {
        return fileSlots[fileNum].size;
    }
    
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

    if ((file->source == SRC_EXTERNAL || file->source == SRC_ROM_LOADED) && file->data) {
        if (outSize) *outSize = file->size;
        return file->data;
    }

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

	if (outSize) *outSize = 0;
	return NULL;
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

	if (data && size && (file->source == SRC_ROM_LOADED || file->source == SRC_ROM_IN_FILE)) {
		if (loadType > 0 && (u32)loadType < ARRAYCOUNT(filePreprocFuncs) && filePreprocFuncs[loadType]) {
			for (u32 i = 0; i < file->numpatches; ++i) {
				const struct romfilepatch *p = &file->patches[i];
                if (p->ofs + p->len <= size) {
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
			filePreprocFuncs[loadType](data, size, outSize); 
		} else if (outSize) {
            *outSize = size;
        }
	} else if (outSize) {
        *outSize = size;
    }
}

void romdataFileFree(s32 fileNum)
{
	if (fileNum < 1 || fileNum >= ROMDATA_MAX_FILES) {
        #ifdef PDDEBUG
		sysLogPrintf(LOG_ERROR, "romdataFileFree: invalid file num %d", fileNum);
        #endif
		return;
	}

    struct romfile *file = &fileSlots[fileNum];

	if ((file->source == SRC_EXTERNAL || file->source == SRC_ROM_LOADED) && file->data) {
		sysMemFree(file->data);
		file->data = NULL;
	}

    if (file->source == SRC_EXTERNAL) {
	    file->source = SRC_UNLOADED;
    } else if (file->source == SRC_ROM_LOADED) {
        file->source = SRC_ROM_IN_FILE;
    }
    file->preprocessed = 0;
}

const char *romdataFileGetName(s32 fileNum)
{
	if (fileNum < 1 || fileNum >= ROMDATA_MAX_FILES) {
		return NULL;
	}
	return fileSlots[fileNum].name;
}

s32 romdataFileGetNumForName(const char *name)
{
	if (!name || !name[0]) {
		return -1;
	}
	for (s32 i = 1; i < ROMDATA_MAX_FILES; ++i) {
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