#ifndef _IN_UNALIGNED_H
#define _IN_UNALIGNED_H

#include <stddef.h>
#include <stdint.h>

#include "platform.h"

static inline uint16_t pd_load_be16_unaligned(const void *srcv)
{
	const uint8_t *src = (const uint8_t *)srcv;
	return (uint16_t)(((uint16_t)src[0] << 8) | (uint16_t)src[1]);
}

static inline uint32_t pd_load_be32_unaligned(const void *srcv)
{
	const uint8_t *src = (const uint8_t *)srcv;
	return ((uint32_t)src[0] << 24)
		| ((uint32_t)src[1] << 16)
		| ((uint32_t)src[2] << 8)
		| (uint32_t)src[3];
}

static inline uint16_t pd_load_u16_unaligned(const void *srcv)
{
	const uint8_t *src = (const uint8_t *)srcv;
#if defined(PLATFORM_LITTLE_ENDIAN)
	return (uint16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8));
#else
	return (uint16_t)(((uint16_t)src[0] << 8) | (uint16_t)src[1]);
#endif
}

static inline uint32_t pd_load_u32_unaligned(const void *srcv)
{
	const uint8_t *src = (const uint8_t *)srcv;
#if defined(PLATFORM_LITTLE_ENDIAN)
	return ((uint32_t)src[0] << 0)
		| ((uint32_t)src[1] << 8)
		| ((uint32_t)src[2] << 16)
		| ((uint32_t)src[3] << 24);
#else
	return ((uint32_t)src[0] << 24)
		| ((uint32_t)src[1] << 16)
		| ((uint32_t)src[2] << 8)
		| (uint32_t)src[3];
#endif
}

static inline int16_t pd_load_s16_unaligned(const void *srcv)
{
	return (int16_t)pd_load_u16_unaligned(srcv);
}

static inline int32_t pd_load_s32_unaligned(const void *srcv)
{
	return (int32_t)pd_load_u32_unaligned(srcv);
}

static inline float pd_load_f32_unaligned(const void *srcv)
{
	union {
		uint32_t u;
		float f;
	} value;
	value.u = pd_load_u32_unaligned(srcv);
	return value.f;
}

static inline uintptr_t pd_load_uptr_unaligned(const void *srcv)
{
	const uint8_t *src = (const uint8_t *)srcv;
#if PD_PTR_SIZE == 8
#if defined(PLATFORM_LITTLE_ENDIAN)
	return ((uintptr_t)src[0] << 0)
		| ((uintptr_t)src[1] << 8)
		| ((uintptr_t)src[2] << 16)
		| ((uintptr_t)src[3] << 24)
		| ((uintptr_t)src[4] << 32)
		| ((uintptr_t)src[5] << 40)
		| ((uintptr_t)src[6] << 48)
		| ((uintptr_t)src[7] << 56);
#else
	return ((uintptr_t)src[0] << 56)
		| ((uintptr_t)src[1] << 48)
		| ((uintptr_t)src[2] << 40)
		| ((uintptr_t)src[3] << 32)
		| ((uintptr_t)src[4] << 24)
		| ((uintptr_t)src[5] << 16)
		| ((uintptr_t)src[6] << 8)
		| ((uintptr_t)src[7] << 0);
#endif
#else
	return (uintptr_t)pd_load_u32_unaligned(srcv);
#endif
}

static inline void pd_store_u16_unaligned(void *dstv, uint16_t value)
{
	uint8_t *dst = (uint8_t *)dstv;
#if defined(PLATFORM_LITTLE_ENDIAN)
	dst[0] = (uint8_t)(value >> 0);
	dst[1] = (uint8_t)(value >> 8);
#else
	dst[0] = (uint8_t)(value >> 8);
	dst[1] = (uint8_t)(value >> 0);
#endif
}

static inline void pd_store_u32_unaligned(void *dstv, uint32_t value)
{
	uint8_t *dst = (uint8_t *)dstv;
#if defined(PLATFORM_LITTLE_ENDIAN)
	dst[0] = (uint8_t)(value >> 0);
	dst[1] = (uint8_t)(value >> 8);
	dst[2] = (uint8_t)(value >> 16);
	dst[3] = (uint8_t)(value >> 24);
#else
	dst[0] = (uint8_t)(value >> 24);
	dst[1] = (uint8_t)(value >> 16);
	dst[2] = (uint8_t)(value >> 8);
	dst[3] = (uint8_t)(value >> 0);
#endif
}

static inline void pd_store_s32_unaligned(void *dstv, int32_t value)
{
	pd_store_u32_unaligned(dstv, (uint32_t)value);
}

static inline void pd_store_f32_unaligned(void *dstv, float value)
{
	union {
		uint32_t u;
		float f;
	} raw;
	raw.f = value;
	pd_store_u32_unaligned(dstv, raw.u);
}

static inline void pd_store_uptr_unaligned(void *dstv, uintptr_t value)
{
	uint8_t *dst = (uint8_t *)dstv;
#if PD_PTR_SIZE == 8
#if defined(PLATFORM_LITTLE_ENDIAN)
	dst[0] = (uint8_t)(value >> 0);
	dst[1] = (uint8_t)(value >> 8);
	dst[2] = (uint8_t)(value >> 16);
	dst[3] = (uint8_t)(value >> 24);
	dst[4] = (uint8_t)(value >> 32);
	dst[5] = (uint8_t)(value >> 40);
	dst[6] = (uint8_t)(value >> 48);
	dst[7] = (uint8_t)(value >> 56);
#else
	dst[0] = (uint8_t)(value >> 56);
	dst[1] = (uint8_t)(value >> 48);
	dst[2] = (uint8_t)(value >> 40);
	dst[3] = (uint8_t)(value >> 32);
	dst[4] = (uint8_t)(value >> 24);
	dst[5] = (uint8_t)(value >> 16);
	dst[6] = (uint8_t)(value >> 8);
	dst[7] = (uint8_t)(value >> 0);
#endif
#else
	pd_store_u32_unaligned(dstv, (uint32_t)value);
#endif
}

static inline void pd_copy_bytes_unaligned(void *dstv, const void *srcv, size_t len)
{
	uint8_t *dst = (uint8_t *)dstv;
	const uint8_t *src = (const uint8_t *)srcv;

	while (len >= 16) {
		dst[0] = src[0];
		dst[1] = src[1];
		dst[2] = src[2];
		dst[3] = src[3];
		dst[4] = src[4];
		dst[5] = src[5];
		dst[6] = src[6];
		dst[7] = src[7];
		dst[8] = src[8];
		dst[9] = src[9];
		dst[10] = src[10];
		dst[11] = src[11];
		dst[12] = src[12];
		dst[13] = src[13];
		dst[14] = src[14];
		dst[15] = src[15];

		dst += 16;
		src += 16;
		len -= 16;
	}

	while (len > 0) {
		*dst++ = *src++;
		len--;
	}
}

#endif
