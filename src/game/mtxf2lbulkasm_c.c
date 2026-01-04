#include <ultra64.h>
#include "constants.h"
#include "bss.h"
#include "lib/rng.h"
#include "data.h"
#include "types.h"

void mtxF2LBulk(Mtxf *mtx, s32 count)
{
#ifndef GBI_FLOATS
#if defined(__PSP__)
	/*
	 * Note: Mtxf is a union of float and packed words. This means writing the
	 * packed `l` layout destroys the source float `m` values. Load all 4 rows
	 * before storing any output.
	 */
	if (((uintptr_t) mtx & 0xf) == 0) {
		__asm__ volatile(
			".set push\n"
			".set noreorder\n"
			"lui        $t8, 0xffff\n" /* $t8 = 0xffff0000 */
			"lv.s       S100, 0(%[scale])\n" /* scale0 */
			"lv.s       S101, 4(%[scale])\n" /* scale1 */
			"vdiv.s     S102, S101, S100\n"  /* ratio = scale1 / scale0 */
			"1:\n"
			"lv.q       C000, 0(%[mtx])\n"
			"lv.q       C010, 16(%[mtx])\n"
			"lv.q       C020, 32(%[mtx])\n"
			"lv.q       C030, 48(%[mtx])\n"
			"vscl.q     C000, C000, S100\n"
			"vscl.q     C010, C010, S100\n"
			"vscl.q     C020, C020, S100\n"
			"vscl.q     C030, C030, S100\n"
			"vmul.s     S003, S003, S102\n"
			"vmul.s     S013, S013, S102\n"
			"vmul.s     S023, S023, S102\n"
			"vmul.s     S033, S033, S102\n"
			"vf2iz.q    C000, C000, 0\n"
			"vf2iz.q    C010, C010, 0\n"
			"vf2iz.q    C020, C020, 0\n"
			"vf2iz.q    C030, C030, 0\n"

			/* Rows 0/1 -> l[0] and l[2] */
			"mfv        $t0, S000\n"
			"mfv        $t1, S001\n"
			"mfv        $t2, S002\n"
			"mfv        $t3, S003\n"
			"mfv        $t4, S010\n"
			"mfv        $t5, S011\n"
			"mfv        $t6, S012\n"
			"mfv        $t7, S013\n"

			/* l[0][0], l[2][0] */
			"and        $t9, $t0, $t8\n"
			"srl        $a2, $t1, 16\n"
			"or         $t9, $t9, $a2\n"
			"sll        $a3, $t0, 16\n"
			"andi       $a2, $t1, 0xffff\n"
			"or         $a3, $a3, $a2\n"
			"sw         $t9, 0(%[mtx])\n"
			"sw         $a3, 32(%[mtx])\n"

			/* l[0][1], l[2][1] */
			"and        $t9, $t2, $t8\n"
			"srl        $a2, $t3, 16\n"
			"or         $t9, $t9, $a2\n"
			"sll        $a3, $t2, 16\n"
			"andi       $a2, $t3, 0xffff\n"
			"or         $a3, $a3, $a2\n"
			"sw         $t9, 4(%[mtx])\n"
			"sw         $a3, 36(%[mtx])\n"

			/* l[0][2], l[2][2] */
			"and        $t9, $t4, $t8\n"
			"srl        $a2, $t5, 16\n"
			"or         $t9, $t9, $a2\n"
			"sll        $a3, $t4, 16\n"
			"andi       $a2, $t5, 0xffff\n"
			"or         $a3, $a3, $a2\n"
			"sw         $t9, 8(%[mtx])\n"
			"sw         $a3, 40(%[mtx])\n"

			/* l[0][3], l[2][3] */
			"and        $t9, $t6, $t8\n"
			"srl        $a2, $t7, 16\n"
			"or         $t9, $t9, $a2\n"
			"sll        $a3, $t6, 16\n"
			"andi       $a2, $t7, 0xffff\n"
			"or         $a3, $a3, $a2\n"
			"sw         $t9, 12(%[mtx])\n"
			"sw         $a3, 44(%[mtx])\n"

			/* Rows 2/3 -> l[1] and l[3] */
			"mfv        $t0, S020\n"
			"mfv        $t1, S021\n"
			"mfv        $t2, S022\n"
			"mfv        $t3, S023\n"
			"mfv        $t4, S030\n"
			"mfv        $t5, S031\n"
			"mfv        $t6, S032\n"
			"mfv        $t7, S033\n"

			/* l[1][0], l[3][0] */
			"and        $t9, $t0, $t8\n"
			"srl        $a2, $t1, 16\n"
			"or         $t9, $t9, $a2\n"
			"sll        $a3, $t0, 16\n"
			"andi       $a2, $t1, 0xffff\n"
			"or         $a3, $a3, $a2\n"
			"sw         $t9, 16(%[mtx])\n"
			"sw         $a3, 48(%[mtx])\n"

			/* l[1][1], l[3][1] */
			"and        $t9, $t2, $t8\n"
			"srl        $a2, $t3, 16\n"
			"or         $t9, $t9, $a2\n"
			"sll        $a3, $t2, 16\n"
			"andi       $a2, $t3, 0xffff\n"
			"or         $a3, $a3, $a2\n"
			"sw         $t9, 20(%[mtx])\n"
			"sw         $a3, 52(%[mtx])\n"

			/* l[1][2], l[3][2] */
			"and        $t9, $t4, $t8\n"
			"srl        $a2, $t5, 16\n"
			"or         $t9, $t9, $a2\n"
			"sll        $a3, $t4, 16\n"
			"andi       $a2, $t5, 0xffff\n"
			"or         $a3, $a3, $a2\n"
			"sw         $t9, 24(%[mtx])\n"
			"sw         $a3, 56(%[mtx])\n"

			/* l[1][3], l[3][3] */
			"and        $t9, $t6, $t8\n"
			"srl        $a2, $t7, 16\n"
			"or         $t9, $t9, $a2\n"
			"sll        $a3, $t6, 16\n"
			"andi       $a2, $t7, 0xffff\n"
			"or         $a3, $a3, $a2\n"
			"sw         $t9, 28(%[mtx])\n"
			"sw         $a3, 60(%[mtx])\n"

			"addiu      %[count], %[count], -1\n"
			"bnez       %[count], 1b\n"
			" addiu     %[mtx], %[mtx], 64\n"
			".set pop\n"
			: [mtx] "+r"(mtx), [count] "+r"(count)
			: [scale] "r"(var8005ef10)
			: "t0",
			  "t1",
			  "t2",
			  "t3",
			  "t4",
			  "t5",
			  "t6",
			  "t7",
			  "t8",
			  "t9",
			  "a2",
			  "a3",
			  "memory");
	} else {
		__asm__ volatile(
			".set push\n"
			".set noreorder\n"
			"lui        $t8, 0xffff\n" /* $t8 = 0xffff0000 */
			"lv.s       S100, 0(%[scale])\n" /* scale0 */
			"lv.s       S101, 4(%[scale])\n" /* scale1 */
			"vdiv.s     S102, S101, S100\n"  /* ratio = scale1 / scale0 */
			"1:\n"
			"ulv.q      C000, 0(%[mtx])\n"
			"ulv.q      C010, 16(%[mtx])\n"
			"ulv.q      C020, 32(%[mtx])\n"
			"ulv.q      C030, 48(%[mtx])\n"
			"vscl.q     C000, C000, S100\n"
			"vscl.q     C010, C010, S100\n"
			"vscl.q     C020, C020, S100\n"
			"vscl.q     C030, C030, S100\n"
			"vmul.s     S003, S003, S102\n"
			"vmul.s     S013, S013, S102\n"
			"vmul.s     S023, S023, S102\n"
			"vmul.s     S033, S033, S102\n"
			"vf2iz.q    C000, C000, 0\n"
			"vf2iz.q    C010, C010, 0\n"
			"vf2iz.q    C020, C020, 0\n"
			"vf2iz.q    C030, C030, 0\n"

			/* Rows 0/1 -> l[0] and l[2] */
			"mfv        $t0, S000\n"
			"mfv        $t1, S001\n"
			"mfv        $t2, S002\n"
			"mfv        $t3, S003\n"
			"mfv        $t4, S010\n"
			"mfv        $t5, S011\n"
			"mfv        $t6, S012\n"
			"mfv        $t7, S013\n"

			/* l[0][0], l[2][0] */
			"and        $t9, $t0, $t8\n"
			"srl        $a2, $t1, 16\n"
			"or         $t9, $t9, $a2\n"
			"sll        $a3, $t0, 16\n"
			"andi       $a2, $t1, 0xffff\n"
			"or         $a3, $a3, $a2\n"
			"sw         $t9, 0(%[mtx])\n"
			"sw         $a3, 32(%[mtx])\n"

			/* l[0][1], l[2][1] */
			"and        $t9, $t2, $t8\n"
			"srl        $a2, $t3, 16\n"
			"or         $t9, $t9, $a2\n"
			"sll        $a3, $t2, 16\n"
			"andi       $a2, $t3, 0xffff\n"
			"or         $a3, $a3, $a2\n"
			"sw         $t9, 4(%[mtx])\n"
			"sw         $a3, 36(%[mtx])\n"

			/* l[0][2], l[2][2] */
			"and        $t9, $t4, $t8\n"
			"srl        $a2, $t5, 16\n"
			"or         $t9, $t9, $a2\n"
			"sll        $a3, $t4, 16\n"
			"andi       $a2, $t5, 0xffff\n"
			"or         $a3, $a3, $a2\n"
			"sw         $t9, 8(%[mtx])\n"
			"sw         $a3, 40(%[mtx])\n"

			/* l[0][3], l[2][3] */
			"and        $t9, $t6, $t8\n"
			"srl        $a2, $t7, 16\n"
			"or         $t9, $t9, $a2\n"
			"sll        $a3, $t6, 16\n"
			"andi       $a2, $t7, 0xffff\n"
			"or         $a3, $a3, $a2\n"
			"sw         $t9, 12(%[mtx])\n"
			"sw         $a3, 44(%[mtx])\n"

			/* Rows 2/3 -> l[1] and l[3] */
			"mfv        $t0, S020\n"
			"mfv        $t1, S021\n"
			"mfv        $t2, S022\n"
			"mfv        $t3, S023\n"
			"mfv        $t4, S030\n"
			"mfv        $t5, S031\n"
			"mfv        $t6, S032\n"
			"mfv        $t7, S033\n"

			/* l[1][0], l[3][0] */
			"and        $t9, $t0, $t8\n"
			"srl        $a2, $t1, 16\n"
			"or         $t9, $t9, $a2\n"
			"sll        $a3, $t0, 16\n"
			"andi       $a2, $t1, 0xffff\n"
			"or         $a3, $a3, $a2\n"
			"sw         $t9, 16(%[mtx])\n"
			"sw         $a3, 48(%[mtx])\n"

			/* l[1][1], l[3][1] */
			"and        $t9, $t2, $t8\n"
			"srl        $a2, $t3, 16\n"
			"or         $t9, $t9, $a2\n"
			"sll        $a3, $t2, 16\n"
			"andi       $a2, $t3, 0xffff\n"
			"or         $a3, $a3, $a2\n"
			"sw         $t9, 20(%[mtx])\n"
			"sw         $a3, 52(%[mtx])\n"

			/* l[1][2], l[3][2] */
			"and        $t9, $t4, $t8\n"
			"srl        $a2, $t5, 16\n"
			"or         $t9, $t9, $a2\n"
			"sll        $a3, $t4, 16\n"
			"andi       $a2, $t5, 0xffff\n"
			"or         $a3, $a3, $a2\n"
			"sw         $t9, 24(%[mtx])\n"
			"sw         $a3, 56(%[mtx])\n"

			/* l[1][3], l[3][3] */
			"and        $t9, $t6, $t8\n"
			"srl        $a2, $t7, 16\n"
			"or         $t9, $t9, $a2\n"
			"sll        $a3, $t6, 16\n"
			"andi       $a2, $t7, 0xffff\n"
			"or         $a3, $a3, $a2\n"
			"sw         $t9, 28(%[mtx])\n"
			"sw         $a3, 60(%[mtx])\n"

			"addiu      %[count], %[count], -1\n"
			"bnez       %[count], 1b\n"
			" addiu     %[mtx], %[mtx], 64\n"
			".set pop\n"
			: [mtx] "+r"(mtx), [count] "+r"(count)
			: [scale] "r"(var8005ef10)
			: "t0",
			  "t1",
			  "t2",
			  "t3",
			  "t4",
			  "t5",
			  "t6",
			  "t7",
			  "t8",
			  "t9",
			  "a2",
			  "a3",
			  "memory");
	}
#else
	do {
		u32 m00 = (s32) (mtx->m[0][0] * var8005ef10[0]);
		u32 m01 = (s32) (mtx->m[0][1] * var8005ef10[0]);
		u32 m02 = (s32) (mtx->m[0][2] * var8005ef10[0]);
		u32 m03 = (s32) (mtx->m[0][3] * var8005ef10[1]);
		u32 m10 = (s32) (mtx->m[1][0] * var8005ef10[0]);
		u32 m11 = (s32) (mtx->m[1][1] * var8005ef10[0]);
		u32 m12 = (s32) (mtx->m[1][2] * var8005ef10[0]);
		u32 m13 = (s32) (mtx->m[1][3] * var8005ef10[1]);
		u32 m20 = (s32) (mtx->m[2][0] * var8005ef10[0]);
		u32 m21 = (s32) (mtx->m[2][1] * var8005ef10[0]);
		u32 m22 = (s32) (mtx->m[2][2] * var8005ef10[0]);
		u32 m23 = (s32) (mtx->m[2][3] * var8005ef10[1]);
		u32 m30 = (s32) (mtx->m[3][0] * var8005ef10[0]);
		u32 m31 = (s32) (mtx->m[3][1] * var8005ef10[0]);
		u32 m32 = (s32) (mtx->m[3][2] * var8005ef10[0]);
		u32 m33 = (s32) (mtx->m[3][3] * var8005ef10[1]);

		mtx->l[0][0] = (m00 & 0xffff0000) | m01 >> 16;
		mtx->l[0][1] = (m02 & 0xffff0000) | m03 >> 16;
		mtx->l[0][2] = (m10 & 0xffff0000) | m11 >> 16;
		mtx->l[0][3] = (m12 & 0xffff0000) | m13 >> 16;
		mtx->l[1][0] = (m20 & 0xffff0000) | m21 >> 16;
		mtx->l[1][1] = (m22 & 0xffff0000) | m23 >> 16;
		mtx->l[1][2] = (m30 & 0xffff0000) | m31 >> 16;
		mtx->l[1][3] = (m32 & 0xffff0000) | m33 >> 16;
		mtx->l[2][0] = m00 << 16 | (m01 & 0xffff);
		mtx->l[2][1] = m02 << 16 | (m03 & 0xffff);
		mtx->l[2][2] = m10 << 16 | (m11 & 0xffff);
		mtx->l[2][3] = m12 << 16 | (m13 & 0xffff);
		mtx->l[3][0] = m20 << 16 | (m21 & 0xffff);
		mtx->l[3][1] = m22 << 16 | (m23 & 0xffff);
		mtx->l[3][2] = m30 << 16 | (m31 & 0xffff);
		mtx->l[3][3] = m32 << 16 | (m33 & 0xffff);

		mtx++;

		count--;
	} while (count);
#endif
#else
	s32 i;
	s32 j;
	f32 scale0 = var8005ef10[0] * (1.0f / 65536.0f);
	f32 scale1 = var8005ef10[1] * (1.0f / 65536.0f);

	while (count > 0) {
		for (i = 0; i < 4; i++) {
			for (j = 0; j < 4; j++) {
				f32 scale = (j == 3) ? scale1 : scale0;
				mtx->m[i][j] *= scale;
			}
		}

		mtx++;
		count--;
	}
#endif
}
