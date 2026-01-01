// see https://github.com/n64decomp/007/blob/master/tools/mktex/src/libpdtex/reader.c
// and https://github.com/doomhack/perfect_dark/blob/master/src/lib/rzip.c

#include <zlib.h>

#include "lib/rzip.h"

void *var80091558; // g_RzipUnused

static z_stream s_rzip_stream;
static bool s_rzip_stream_ready = false;
static bool s_rzip_stream_busy = false;

static int rzipInitStream(z_stream *strm)
{
	*strm = (z_stream){ 0 };
	return inflateInit2(strm, -15);
}

static z_stream *rzipAcquireStream(z_stream *temp, bool *use_temp, int *init_ret)
{
	if (!s_rzip_stream_ready) {
		*init_ret = rzipInitStream(&s_rzip_stream);
		if (*init_ret == Z_OK) {
			s_rzip_stream_ready = true;
		}
	}

	if (s_rzip_stream_ready && !s_rzip_stream_busy) {
		*init_ret = inflateReset(&s_rzip_stream);
		if (*init_ret == Z_OK) {
			s_rzip_stream_busy = true;
			*use_temp = false;
			return &s_rzip_stream;
		}
		inflateEnd(&s_rzip_stream);
		s_rzip_stream_ready = false;
	}

	*init_ret = rzipInitStream(temp);
	if (*init_ret != Z_OK) {
		return NULL;
	}

	*use_temp = true;
	return temp;
}

static void rzipReleaseStream(z_stream *strm, bool use_temp)
{
	if (use_temp) {
		inflateEnd(strm);
	} else {
		s_rzip_stream_busy = false;
	}
}

bool rzipIs1172(void *buffer)
{
	const u8* src = buffer;
	return (src[0] == 0x11 && src[1] == 0x72);
}

bool rzipIs1173(void *buffer)
{
	const u8* src = buffer;
	return (src[0] == 0x11 && src[1] == 0x73);
}

static inline s32 rzipInflate1172(z_stream *strm, const u8 *src, void *dst)
{
	strm->avail_in = 0x2000;
	strm->next_in = (Bytef *)src;

	do {
		strm->avail_out = 0x2000;
		strm->next_out = dst;
		if (inflate(strm, Z_FINISH) == Z_STREAM_ERROR) {
			rmonPrintf("rzipInflate1172: Z_STREAM_ERROR\n");
			return 0;
		}
	} while (strm->avail_out == 0);

	return strm->total_out;
}

static inline s32 rzipInflate1173(z_stream *strm, const u8 *src, void *dst, u32 dstLen)
{
	strm->avail_in = -1; // compressed size unknown
	strm->next_in = (Bytef *)src;
	strm->avail_out = dstLen;
	strm->next_out = dst;

	if (inflate(strm, Z_SYNC_FLUSH) == Z_STREAM_ERROR) {
		rmonPrintf("rzipInflate1173: Z_STREAM_ERROR\n");
		return 0;
	}

	return strm->total_out;
}

s32 rzipInflate(void *srcp, void *dst, void *scratch)
{
	s32 ret = 0;
	const u8 *src = srcp;
	z_stream temp = { 0 };
	z_stream *strm = NULL;
	bool use_temp = false;
	const u16 tag = ((u16)src[0] << 8) | src[1];
	void *next_in = NULL;
	u32 total_out = 0;
	int init_ret = Z_OK;

	strm = rzipAcquireStream(&temp, &use_temp, &init_ret);
	if (strm == NULL) {
		rmonPrintf("rzipInflate: inflate init/reset failed: %d\n", init_ret);
		return 0;
	}

	if (tag == 0x1173) {
		// 1173, we know the uncompressed length
		const u32 dstLen = ((u32)src[2] << 16) | ((u32)src[3] << 8) | (u32)src[4];
		ret = rzipInflate1173(strm, src + 5, dst, dstLen);
	} else if (tag == 0x1172) {
		// 1172, uncompressed length unknown
		ret = rzipInflate1172(strm, src + 2, dst);
	} else {
		rmonPrintf("rzipInflate: input not in any known rare zip format\n");
		ret = 0;
	}

	next_in = strm->next_in;
	total_out = strm->total_out;
	rzipReleaseStream(strm, use_temp);

	if (ret) {
		var80091558 = next_in;
		return total_out;
	} else {
		return 0;
	}
}

u32 rzipInit(void)
{
	// this builds tables in the original assembly version, we don't need that
	return 0;
}

void *rzipGetSomething(void)
{
	return var80091558;
}
