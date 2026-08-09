#ifndef PD_ASSET_CACHE_H
#define PD_ASSET_CACHE_H

#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PD_ASSET_CACHE_FLAG_INFLATED 0x00000001u

typedef struct PdAssetCacheSource {
	u32 rom_offset;
	u32 rom_size;
} PdAssetCacheSource;

typedef struct PdAssetCacheEntry {
	u32 rom_offset;
	u32 rom_size;
	u32 data_offset;
	u32 data_size;
	u32 flags;
} PdAssetCacheEntry;

typedef void (*PdAssetCacheProgress)(u32 permille, const char *status);

/*
 * Validate (or create) the persistent asset pack and retain its index in RAM.
 * The pack contains a raw ROM backing image for streamed ranges plus unpacked
 * copies of every top-level 1173 file.
 */
s32 pdAssetCachePrepare(const char *rom_path, const char *cache_path,
		u32 game_version, u32 rom_size, const PdAssetCacheSource *files,
		u32 file_count, PdAssetCacheProgress progress);
/* Open a previously completed pack without touching the source ROM. */
s32 pdAssetCacheOpenExisting(const char *cache_path, u32 game_version,
		u32 rom_size, u32 file_count);
void pdAssetCacheShutdown(void);

s32 pdAssetCacheIsReady(void);
u32 pdAssetCacheGetRawOffset(void);
u32 pdAssetCacheGetPackedSize(void);
const char *pdAssetCacheGetPath(void);
const PdAssetCacheEntry *pdAssetCacheGetEntry(u32 file_num);

#ifdef __cplusplus
}
#endif

#endif
