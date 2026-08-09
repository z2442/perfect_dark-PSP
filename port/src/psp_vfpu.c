#include "psp_vfpu.h"

#include <stdint.h>
#include <string.h>

void *pdPspMemcpyVfpu(void *dst, const void *src, size_t size)
{
#ifdef __PSP__
	void *originaldst = dst;
	uint8_t *dst8 = dst;
	const uint8_t *src8 = src;
	uint32_t *dst32;
	const uint32_t *src32;

	if (size < 16 || (((uintptr_t)src8 ^ (uintptr_t)dst8) & 3) != 0) {
		return memcpy(dst, src, size);
	}

	while (((uintptr_t)dst8 & 3) != 0) {
		*dst8++ = *src8++;
		size--;
	}

	dst32 = (uint32_t *)dst8;
	src32 = (const uint32_t *)src8;

	while (((uintptr_t)dst32 & 0xf) != 0) {
		*dst32++ = *src32++;
		size -= 4;
	}

	dst8 = (uint8_t *)dst32;
	src8 = (const uint8_t *)src32;

	if (((uintptr_t)src8 & 0xf) == 0) {
		while (size > 63) {
			__asm__ volatile(
				"lv.q C000, 0(%1)\n"
				"lv.q C010, 16(%1)\n"
				"lv.q C020, 32(%1)\n"
				"lv.q C030, 48(%1)\n"
				"sv.q C000, 0(%0)\n"
				"sv.q C010, 16(%0)\n"
				"sv.q C020, 32(%0)\n"
				"sv.q C030, 48(%0)\n"
				"addiu %2, %2, -64\n"
				"addiu %1, %1, 64\n"
				"addiu %0, %0, 64\n"
				: "+r"(dst8), "+r"(src8), "+r"(size) : : "memory");
		}

		while (size > 15) {
			__asm__ volatile(
				"lv.q C000, 0(%1)\n"
				"sv.q C000, 0(%0)\n"
				"addiu %2, %2, -16\n"
				"addiu %1, %1, 16\n"
				"addiu %0, %0, 16\n"
				: "+r"(dst8), "+r"(src8), "+r"(size) : : "memory");
		}
	} else {
		while (size > 63) {
			__asm__ volatile(
				"ulv.q C000, 0(%1)\n"
				"ulv.q C010, 16(%1)\n"
				"ulv.q C020, 32(%1)\n"
				"ulv.q C030, 48(%1)\n"
				"sv.q C000, 0(%0)\n"
				"sv.q C010, 16(%0)\n"
				"sv.q C020, 32(%0)\n"
				"sv.q C030, 48(%0)\n"
				"addiu %2, %2, -64\n"
				"addiu %1, %1, 64\n"
				"addiu %0, %0, 64\n"
				: "+r"(dst8), "+r"(src8), "+r"(size) : : "memory");
		}

		while (size > 15) {
			__asm__ volatile(
				"ulv.q C000, 0(%1)\n"
				"sv.q C000, 0(%0)\n"
				"addiu %2, %2, -16\n"
				"addiu %1, %1, 16\n"
				"addiu %0, %0, 16\n"
				: "+r"(dst8), "+r"(src8), "+r"(size) : : "memory");
		}
	}

	dst32 = (uint32_t *)dst8;
	src32 = (const uint32_t *)src8;

	while (size > 3) {
		*dst32++ = *src32++;
		size -= 4;
	}

	dst8 = (uint8_t *)dst32;
	src8 = (const uint8_t *)src32;

	while (size != 0) {
		*dst8++ = *src8++;
		size--;
	}

	return originaldst;
#else
	return memcpy(dst, src, size);
#endif
}
