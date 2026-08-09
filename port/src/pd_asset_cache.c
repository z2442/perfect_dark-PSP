#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>

#include "fs.h"
#include "pd_asset_cache.h"
#include "system.h"

#define PD_ASSET_CACHE_MAGIC "PDAC0001"
#define PD_ASSET_CACHE_VERSION 1u
#define PD_ASSET_CACHE_ALIGNMENT 64u
#define PD_ASSET_CACHE_IO_SIZE (64u * 1024u)
#define PD_ASSET_CACHE_PATH_MAX (FS_MAXPATH + 1)

typedef struct PdAssetCacheHeader {
	char magic[8];
	u32 format_version;
	u32 game_version;
	u32 rom_size;
	u32 rom_crc32;
	u32 rom_header_crc1;
	u32 rom_header_crc2;
	u32 source_table_crc32;
	u32 file_count;
	u32 entry_size;
	u32 entries_offset;
	u32 raw_offset;
	u32 total_size;
	u32 flags;
	u32 reserved[2];
} PdAssetCacheHeader;

static PdAssetCacheEntry *g_AssetEntries;
static u32 g_AssetEntryCount;
static u32 g_AssetRawOffset;
static u32 g_AssetPackedSize;
static char g_AssetPath[PD_ASSET_CACHE_PATH_MAX];

static u32 pdAssetAlign(u32 value)
{
	return (value + PD_ASSET_CACHE_ALIGNMENT - 1u) & ~(PD_ASSET_CACHE_ALIGNMENT - 1u);
}

static s32 pdAssetReadAt(FILE *file, u32 offset, void *dst, u32 size)
{
	if (fseek(file, (long)offset, SEEK_SET) != 0) return 0;
	return fread(dst, 1, size, file) == size;
}

static s32 pdAssetWriteZeros(FILE *file, u32 size)
{
	static const u8 zeros[PD_ASSET_CACHE_ALIGNMENT] = { 0 };

	while (size != 0) {
		u32 amount = size < sizeof(zeros) ? size : sizeof(zeros);
		if (fwrite(zeros, 1, amount, file) != amount) return 0;
		size -= amount;
	}
	return 1;
}

static u32 pdAssetSourceTableCrc(const PdAssetCacheSource *files, u32 file_count)
{
	uLong crc = crc32(0L, Z_NULL, 0);
	return (u32)crc32(crc, (const Bytef *)files,
			(uInt)(file_count * sizeof(*files)));
}

static s32 pdAssetReadRomIdentity(FILE *rom, u32 *crc1, u32 *crc2)
{
	u8 header[0x18];

	if (!pdAssetReadAt(rom, 0, header, sizeof(header))) return 0;
	*crc1 = ((u32)header[0x10] << 24) | ((u32)header[0x11] << 16)
			| ((u32)header[0x12] << 8) | header[0x13];
	*crc2 = ((u32)header[0x14] << 24) | ((u32)header[0x15] << 16)
			| ((u32)header[0x16] << 8) | header[0x17];
	return 1;
}

static s32 pdAssetLoadIndex(FILE *cache, const PdAssetCacheHeader *header)
{
	PdAssetCacheEntry *entries;

	entries = malloc(header->file_count * sizeof(*entries));
	if (!entries) return 0;
	if (!pdAssetReadAt(cache, header->entries_offset, entries,
			header->file_count * sizeof(*entries))) {
		free(entries);
		return 0;
	}

	for (u32 i = 0; i < header->file_count; i++) {
		const PdAssetCacheEntry *entry = &entries[i];
		if (entry->data_size != 0
				&& ((u64)entry->data_offset + entry->data_size > header->total_size
					|| entry->rom_size == 0)) {
			free(entries);
			return 0;
		}
	}

	free(g_AssetEntries);
	g_AssetEntries = entries;
	g_AssetEntryCount = header->file_count;
	g_AssetRawOffset = header->raw_offset;
	g_AssetPackedSize = header->total_size;
	return 1;
}

static s32 pdAssetValidate(const char *cache_path, u32 game_version, u32 rom_size,
		u32 crc1, u32 crc2, u32 source_crc, u32 file_count)
{
	PdAssetCacheHeader header;
	FILE *cache = fopen(cache_path, "rb");
	long actual_size;
	s32 valid = 0;

	if (!cache) return 0;
	if (fread(&header, 1, sizeof(header), cache) != sizeof(header)) goto done;
	if (memcmp(header.magic, PD_ASSET_CACHE_MAGIC, sizeof(header.magic)) != 0
			|| header.format_version != PD_ASSET_CACHE_VERSION
			|| header.game_version != game_version
			|| header.rom_size != rom_size
			|| header.rom_header_crc1 != crc1
			|| header.rom_header_crc2 != crc2
			|| header.source_table_crc32 != source_crc
			|| header.file_count != file_count
			|| header.entry_size != sizeof(PdAssetCacheEntry)
			|| header.entries_offset != sizeof(PdAssetCacheHeader)
			|| header.raw_offset < header.entries_offset + file_count * sizeof(PdAssetCacheEntry)
			|| (u64)header.raw_offset + rom_size > header.total_size) {
		goto done;
	}

	if (fseek(cache, 0, SEEK_END) != 0) goto done;
	actual_size = ftell(cache);
	if (actual_size < 0 || (u32)actual_size != header.total_size) goto done;
	valid = pdAssetLoadIndex(cache, &header);

done:
	fclose(cache);
	return valid;
}

s32 pdAssetCacheOpenExisting(const char *cache_path, u32 game_version,
		u32 rom_size, u32 file_count)
{
	PdAssetCacheHeader header;
	FILE *cache;
	long actual_size;
	s32 valid = 0;

	pdAssetCacheShutdown();
	if (!cache_path || file_count == 0) return 0;
	cache = fopen(cache_path, "rb");
	if (!cache) return 0;
	if (fread(&header, 1, sizeof(header), cache) != sizeof(header)) goto done;
	if (memcmp(header.magic, PD_ASSET_CACHE_MAGIC, sizeof(header.magic)) != 0
			|| header.format_version != PD_ASSET_CACHE_VERSION
			|| header.game_version != game_version
			|| header.rom_size != rom_size
			|| header.file_count != file_count
			|| header.entry_size != sizeof(PdAssetCacheEntry)
			|| header.entries_offset != sizeof(PdAssetCacheHeader)
			|| header.raw_offset < header.entries_offset + file_count * sizeof(PdAssetCacheEntry)
			|| (u64)header.raw_offset + rom_size > header.total_size) {
		goto done;
	}
	if (fseek(cache, 0, SEEK_END) != 0) goto done;
	actual_size = ftell(cache);
	if (actual_size < 0 || (u32)actual_size != header.total_size) goto done;
	valid = pdAssetLoadIndex(cache, &header);
	if (valid) {
		strncpy(g_AssetPath, cache_path, sizeof(g_AssetPath) - 1);
		g_AssetPath[sizeof(g_AssetPath) - 1] = '\0';
	}

done:
	fclose(cache);
	return valid;
}

static s32 pdAssetInflateToFile(FILE *rom, FILE *cache, u32 rom_offset,
		u32 rom_size, u32 expected_size, u8 *input, u8 *output)
{
	z_stream stream;
	u32 remaining;
	int result;

	if (rom_size <= 5 || fseek(rom, (long)(rom_offset + 5), SEEK_SET) != 0) return 0;
	memset(&stream, 0, sizeof(stream));
	if (inflateInit2(&stream, -15) != Z_OK) return 0;

	remaining = rom_size - 5;
	result = Z_OK;
	while (result != Z_STREAM_END) {
		if (stream.avail_in == 0 && remaining != 0) {
			u32 amount = remaining < PD_ASSET_CACHE_IO_SIZE ? remaining : PD_ASSET_CACHE_IO_SIZE;
			size_t got = fread(input, 1, amount, rom);
			if (got == 0) break;
			remaining -= (u32)got;
			stream.next_in = input;
			stream.avail_in = (uInt)got;
		}

		stream.next_out = output;
		stream.avail_out = PD_ASSET_CACHE_IO_SIZE;
		result = inflate(&stream, Z_NO_FLUSH);
		if (result != Z_OK && result != Z_STREAM_END) break;

		const u32 made = PD_ASSET_CACHE_IO_SIZE - stream.avail_out;
		if (made != 0 && fwrite(output, 1, made, cache) != made) {
			result = Z_ERRNO;
			break;
		}
		if (made == 0 && stream.avail_in == 0 && remaining == 0
				&& result != Z_STREAM_END) {
			break;
		}
	}

	const u32 total_out = (u32)stream.total_out;
	inflateEnd(&stream);
	return result == Z_STREAM_END && total_out == expected_size;
}

static s32 pdAssetBuild(const char *rom_path, const char *cache_path,
		u32 game_version, u32 rom_size, u32 crc1, u32 crc2,
		const PdAssetCacheSource *files, u32 file_count, u32 source_crc,
		PdAssetCacheProgress progress)
{
	PdAssetCacheHeader header;
	PdAssetCacheEntry *entries = NULL;
	FILE *rom = NULL;
	FILE *cache = NULL;
	u8 *input = NULL;
	u8 *output = NULL;
	char temp_path[PD_ASSET_CACHE_PATH_MAX + 8];
	u32 raw_offset;
	u32 copied = 0;
	uLong rom_crc = crc32(0L, Z_NULL, 0);
	s32 ok = 0;

	if (snprintf(temp_path, sizeof(temp_path), "%s.tmp", cache_path) >= (int)sizeof(temp_path)) {
		return 0;
	}
	remove(temp_path);

	entries = calloc(file_count, sizeof(*entries));
	input = malloc(PD_ASSET_CACHE_IO_SIZE);
	output = malloc(PD_ASSET_CACHE_IO_SIZE);
	rom = fopen(rom_path, "rb");
	cache = fopen(temp_path, "wb+");
	if (!entries || !input || !output || !rom || !cache) goto done;
	setvbuf(rom, NULL, _IOFBF, 256 * 1024);
	setvbuf(cache, NULL, _IOFBF, 256 * 1024);

	raw_offset = pdAssetAlign(sizeof(header) + file_count * sizeof(*entries));
	memset(&header, 0, sizeof(header));
	if (!pdAssetWriteZeros(cache, raw_offset)) goto done;
	if (fseek(rom, 0, SEEK_SET) != 0) goto done;

	if (progress) progress(0, "Copying ROM data");
	while (copied < rom_size) {
		u32 amount = rom_size - copied;
		if (amount > PD_ASSET_CACHE_IO_SIZE) amount = PD_ASSET_CACHE_IO_SIZE;
		if (fread(input, 1, amount, rom) != amount
				|| fwrite(input, 1, amount, cache) != amount) goto done;
		rom_crc = crc32(rom_crc, input, amount);
		copied += amount;
		if (progress && ((copied & ((512u * 1024u) - 1u)) == 0 || copied == rom_size)) {
			progress((copied * 600u) / rom_size, "Copying ROM data");
		}
	}

	if (progress) progress(600, "Expanding game assets");
	for (u32 i = 0; i < file_count; i++) {
		const PdAssetCacheSource *source = &files[i];
		PdAssetCacheEntry *entry = &entries[i];
		u8 rzip_header[5];

		entry->rom_offset = source->rom_offset;
		entry->rom_size = source->rom_size;
		if (source->rom_size == 0
				|| (u64)source->rom_offset + source->rom_size > rom_size) continue;

		entry->data_offset = raw_offset + source->rom_offset;
		entry->data_size = source->rom_size;
		if (source->rom_size < sizeof(rzip_header)
				|| !pdAssetReadAt(rom, source->rom_offset, rzip_header, sizeof(rzip_header))
				|| rzip_header[0] != 0x11 || rzip_header[1] != 0x73) {
			continue;
		}

		const u32 expected_size = ((u32)rzip_header[2] << 16)
				| ((u32)rzip_header[3] << 8) | rzip_header[4];
		const long current = ftell(cache);
		if (current < 0) goto done;
		const u32 aligned = pdAssetAlign((u32)current);
		if (!pdAssetWriteZeros(cache, aligned - (u32)current)) goto done;

		entry->data_offset = aligned;
		entry->data_size = expected_size;
		entry->flags = PD_ASSET_CACHE_FLAG_INFLATED;
		if (!pdAssetInflateToFile(rom, cache, source->rom_offset,
				source->rom_size, expected_size, input, output)) {
			sysLogPrintf(LOG_ERROR, "Asset cache: failed inflating file %u", i);
			goto done;
		}

		if (progress && ((i & 15u) == 0 || i + 1 == file_count)) {
			char status[64];
			snprintf(status, sizeof(status), "Expanding game assets %u/%u",
					(unsigned int)(i + 1), (unsigned int)file_count);
			progress(600u + ((i + 1u) * 390u) / file_count, status);
		}
	}

	const long total_size = ftell(cache);
	if (total_size < 0 || (u64)total_size > 0xffffffffu) goto done;

	memcpy(header.magic, PD_ASSET_CACHE_MAGIC, sizeof(header.magic));
	header.format_version = PD_ASSET_CACHE_VERSION;
	header.game_version = game_version;
	header.rom_size = rom_size;
	header.rom_crc32 = (u32)rom_crc;
	header.rom_header_crc1 = crc1;
	header.rom_header_crc2 = crc2;
	header.source_table_crc32 = source_crc;
	header.file_count = file_count;
	header.entry_size = sizeof(PdAssetCacheEntry);
	header.entries_offset = sizeof(PdAssetCacheHeader);
	header.raw_offset = raw_offset;
	header.total_size = (u32)total_size;
	header.flags = 1;

	if (fseek(cache, 0, SEEK_SET) != 0
			|| fwrite(&header, 1, sizeof(header), cache) != sizeof(header)
			|| fwrite(entries, sizeof(*entries), file_count, cache) != file_count
			|| fflush(cache) != 0) goto done;

	fclose(cache);
	cache = NULL;
	remove(cache_path);
	if (rename(temp_path, cache_path) != 0) goto done;
	if (progress) progress(1000, "Game data ready");
	ok = 1;

done:
	if (cache) fclose(cache);
	if (rom) fclose(rom);
	free(output);
	free(input);
	free(entries);
	if (!ok) {
		remove(temp_path);
		sysLogPrintf(LOG_ERROR, "Asset cache: build failed (%s)", strerror(errno));
	}
	return ok;
}

s32 pdAssetCachePrepare(const char *rom_path, const char *cache_path,
		u32 game_version, u32 rom_size, const PdAssetCacheSource *files,
		u32 file_count, PdAssetCacheProgress progress)
{
	FILE *rom;
	u32 crc1;
	u32 crc2;
	u32 source_crc;
	char cache_dir[PD_ASSET_CACHE_PATH_MAX];

	pdAssetCacheShutdown();
	if (!rom_path || !cache_path || !files || file_count == 0) return 0;
	rom = fopen(rom_path, "rb");
	if (!rom) return 0;
	if (!pdAssetReadRomIdentity(rom, &crc1, &crc2)) {
		fclose(rom);
		return 0;
	}
	fclose(rom);

	source_crc = pdAssetSourceTableCrc(files, file_count);
	if (!pdAssetValidate(cache_path, game_version, rom_size, crc1, crc2,
			source_crc, file_count)) {
		const char *slash = strrchr(cache_path, '/');
		if (slash) {
			size_t length = (size_t)(slash - cache_path);
			if (length >= sizeof(cache_dir)) return 0;
			memcpy(cache_dir, cache_path, length);
			cache_dir[length] = '\0';
			if (mkdir(cache_dir, 0777) != 0 && errno != EEXIST) return 0;
		}
		if (!pdAssetBuild(rom_path, cache_path, game_version, rom_size, crc1, crc2,
				files, file_count, source_crc, progress)
				|| !pdAssetValidate(cache_path, game_version, rom_size, crc1, crc2,
					source_crc, file_count)) {
			return 0;
		}
	}

	strncpy(g_AssetPath, cache_path, sizeof(g_AssetPath) - 1);
	g_AssetPath[sizeof(g_AssetPath) - 1] = '\0';
	sysLogPrintf(LOG_NOTE, "Asset cache: using %s (%u bytes)",
			g_AssetPath, g_AssetPackedSize);
	return 1;
}

void pdAssetCacheShutdown(void)
{
	free(g_AssetEntries);
	g_AssetEntries = NULL;
	g_AssetEntryCount = 0;
	g_AssetRawOffset = 0;
	g_AssetPackedSize = 0;
	g_AssetPath[0] = '\0';
}

s32 pdAssetCacheIsReady(void)
{
	return g_AssetEntries != NULL && g_AssetEntryCount != 0;
}

u32 pdAssetCacheGetRawOffset(void)
{
	return g_AssetRawOffset;
}

u32 pdAssetCacheGetPackedSize(void)
{
	return g_AssetPackedSize;
}

const char *pdAssetCacheGetPath(void)
{
	return g_AssetPath;
}

const PdAssetCacheEntry *pdAssetCacheGetEntry(u32 file_num)
{
	if (!g_AssetEntries || file_num >= g_AssetEntryCount) return NULL;
	return &g_AssetEntries[file_num];
}
