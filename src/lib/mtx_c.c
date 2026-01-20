#include <ultra64.h>
#include "constants.h"
#include "bss.h"
#include "data.h"
#include "lib/mtx.h"
#include "types.h"

f32 var8005ef10[] __attribute__((aligned(16))) = {65536, 65536};

#ifdef __PSP__
static inline bool mtxIsAligned16(const void *ptr)
{
	return (((uintptr_t)ptr) & 0x0f) == 0;
}

static inline void mtxCopyF32x16(f32 *dst, const f32 *src)
{
	s32 i;

	for (i = 0; i < 16; i++) {
		dst[i] = src[i];
	}
}

static inline void mtxForceAffine(f32 *mtx)
{
	mtx[3] = 0.0f;
	mtx[7] = 0.0f;
	mtx[11] = 0.0f;
	mtx[15] = 1.0f;
}

static inline void mtxZeroTranslationColumn(f32 *mtx)
{
	mtx[12] = 0.0f;
	mtx[13] = 0.0f;
	mtx[14] = 0.0f;
}

// Manual VFPU transform that mirrors the scalar row-dot-vector math on column-major matrices.
static inline void mtxVfpuTransformVec(const ScePspFMatrix4 *mat, const struct coord *vec, f32 w, struct coord *dst)
{
	const f32 *mtx = (const f32 *)mat;
	f32 wval = w;

	__asm__ volatile(
		".set push\n"
		".set noreorder\n"
		"lv.s       S100, 0(%[vec])\n"
		"lv.s       S101, 4(%[vec])\n"
		"lv.s       S102, 8(%[vec])\n"
		"lv.s       S103, 0(%[w])\n"
		/* Row 0 */
		"lv.s       S000, 0(%[mtx])\n"
		"lv.s       S001, 16(%[mtx])\n"
		"lv.s       S002, 32(%[mtx])\n"
		"lv.s       S003, 48(%[mtx])\n"
		/* Row 1 */
		"lv.s       S010, 4(%[mtx])\n"
		"lv.s       S011, 20(%[mtx])\n"
		"lv.s       S012, 36(%[mtx])\n"
		"lv.s       S013, 52(%[mtx])\n"
		/* Row 2 */
		"lv.s       S020, 8(%[mtx])\n"
		"lv.s       S021, 24(%[mtx])\n"
		"lv.s       S022, 40(%[mtx])\n"
		"lv.s       S023, 56(%[mtx])\n"
		"vmul.q     C030, C000, C100\n"
		"vfad.q     S200, C030\n"
		"vmul.q     C030, C010, C100\n"
		"vfad.q     S201, C030\n"
		"vmul.q     C030, C020, C100\n"
		"vfad.q     S202, C030\n"
		"sv.s       S200, 0(%[dst])\n"
		"sv.s       S201, 4(%[dst])\n"
		"sv.s       S202, 8(%[dst])\n"
		".set pop\n"
		:
		: [mtx] "r"(mtx), [vec] "r"(vec), [dst] "r"(dst), [w] "r"(&wval)
		: "memory");
}
#endif

void mtx4LoadIdentity(Mtxf *mtx)
{
#ifdef __PSP__
	if (mtxIsAligned16(mtx)) {
		vfpu_identity_matrix((ScePspFMatrix4 *)mtx);
	} else {
		ScePspFMatrix4 tmp;

		vfpu_identity_matrix(&tmp);
		mtxCopyF32x16(&mtx->m[0][0], (const f32 *)&tmp);
	}
	return;
#endif
	mtx->m[0][0] = 1;
	mtx->m[0][1] = 0;
	mtx->m[0][2] = 0;
	mtx->m[0][3] = 0;

	mtx->m[1][0] = 0;
	mtx->m[1][1] = 1;
	mtx->m[1][2] = 0;
	mtx->m[1][3] = 0;

	mtx->m[2][0] = 0; 
	mtx->m[2][1] = 0;
	mtx->m[2][2] = 1;
	mtx->m[2][3] = 0;

	mtx->m[3][0] = 0;
	mtx->m[3][1] = 0;
	mtx->m[3][2] = 0;
	mtx->m[3][3] = 1;
}

void mtx4MultMtx4InPlace(Mtxf *multmtx, Mtxf *subject)
{
	mtx4MultMtx4(multmtx, subject, subject);
}

void mtx4MultMtx4(Mtxf *mtx1, Mtxf *mtx2, Mtxf *dst)
{
#ifdef __PSP__
	ScePspFMatrix4 aligned_mtx1;
	ScePspFMatrix4 aligned_mtx2;
	ScePspFMatrix4 aligned_dst;
	const ScePspFMatrix4 *a = (const ScePspFMatrix4 *)mtx1;
	const ScePspFMatrix4 *b = (const ScePspFMatrix4 *)mtx2;
	ScePspFMatrix4 *o = (ScePspFMatrix4 *)dst;

	if (!mtxIsAligned16(mtx1)) {
		mtxCopyF32x16((f32 *)&aligned_mtx1, &mtx1->m[0][0]);
		a = &aligned_mtx1;
	}

	if (!mtxIsAligned16(mtx2)) {
		mtxCopyF32x16((f32 *)&aligned_mtx2, &mtx2->m[0][0]);
		b = &aligned_mtx2;
	}

	if (!mtxIsAligned16(dst) || dst == mtx1 || dst == mtx2) {
		o = &aligned_dst;
	}

	__asm__ __volatile__(
		"lv.q C000, 0(%[a])\n"
		"lv.q C010, 16(%[a])\n"
		"lv.q C020, 32(%[a])\n"
		"lv.q C030, 48(%[a])\n"
		"lv.q C100, 0(%[b])\n"
		"lv.q C110, 16(%[b])\n"
		"lv.q C120, 32(%[b])\n"
		"lv.q C130, 48(%[b])\n"
		"vmmul.q M200, M000, M100\n"
		"sv.q C200, 0(%[o])\n"
		"sv.q C210, 16(%[o])\n"
		"sv.q C220, 32(%[o])\n"
		"sv.q C230, 48(%[o])\n"
		:
		: [a] "r"(a), [b] "r"(b), [o] "r"(o)
		: "memory");

	if (o != (ScePspFMatrix4 *)dst) {
		mtxCopyF32x16(&dst->m[0][0], (const f32 *)o);
	}
	return;
#endif
	s32 i;
	f32 m00 = mtx2->m[0][0];
	f32 m01 = mtx2->m[0][1];
	f32 m02 = mtx2->m[0][2];
	f32 m03 = mtx2->m[0][3];
	f32 m10 = mtx2->m[1][0];
	f32 m11 = mtx2->m[1][1];
	f32 m12 = mtx2->m[1][2];
	f32 m13 = mtx2->m[1][3];
	f32 m20 = mtx2->m[2][0];
	f32 m21 = mtx2->m[2][1];
	f32 m22 = mtx2->m[2][2];
	f32 m23 = mtx2->m[2][3];
	f32 m30 = mtx2->m[3][0];
	f32 m31 = mtx2->m[3][1];
	f32 m32 = mtx2->m[3][2];
	f32 m33 = mtx2->m[3][3];

	for (i = 0; i < 4; i++) {
		dst->m[0][i] = mtx1->m[0][i] * m00 + mtx1->m[1][i] * m01 + mtx1->m[2][i] * m02 + mtx1->m[3][i] * m03;
		dst->m[1][i] = mtx1->m[0][i] * m10 + mtx1->m[1][i] * m11 + mtx1->m[2][i] * m12 + mtx1->m[3][i] * m13;
		dst->m[2][i] = mtx1->m[0][i] * m20 + mtx1->m[1][i] * m21 + mtx1->m[2][i] * m22 + mtx1->m[3][i] * m23;
		dst->m[3][i] = mtx1->m[0][i] * m30 + mtx1->m[1][i] * m31 + mtx1->m[2][i] * m32 + mtx1->m[3][i] * m33;
	}
}

void mtx4RotateVecInPlace(Mtxf *mtx, struct coord *vec)
{
	mtx4RotateVec(mtx, vec, vec);
}

void mtx4RotateVec(Mtxf *mtx, struct coord *vec, struct coord *dst)
{
#ifdef __PSP__
	ScePspFMatrix4 aligned_mtx;
	ScePspFMatrix4 *mat = (ScePspFMatrix4 *)mtx;
	bool needs_copy = !mtxIsAligned16(mtx)
		|| mtx->m[0][3] != 0.0f
		|| mtx->m[1][3] != 0.0f
		|| mtx->m[2][3] != 0.0f
		|| mtx->m[3][3] != 1.0f
		|| mtx->m[3][0] != 0.0f
		|| mtx->m[3][1] != 0.0f
		|| mtx->m[3][2] != 0.0f;

	if (needs_copy) {
		mtxCopyF32x16((f32 *)&aligned_mtx, &mtx->m[0][0]);
		mtxForceAffine((f32 *)&aligned_mtx);
		mtxZeroTranslationColumn((f32 *)&aligned_mtx);
		mat = &aligned_mtx;
	}

	mtxVfpuTransformVec(mat, vec, 0.0f, dst);
	return;
#endif
	f32 x = vec->x;
	f32 y = vec->y;
	f32 z = vec->z;

	dst->x = mtx->m[0][0] * x + mtx->m[1][0] * y + mtx->m[2][0] * z;
	dst->y = mtx->m[0][1] * x + mtx->m[1][1] * y + mtx->m[2][1] * z;
	dst->z = mtx->m[0][2] * x + mtx->m[1][2] * y + mtx->m[2][2] * z;
}

void mtx4TransformVecInPlace(Mtxf *mtx, struct coord *vec)
{
	mtx4TransformVec(mtx, vec, vec);
}

void mtx4TransformVec(Mtxf *mtx, struct coord *vec, struct coord *dst)
{
#ifdef __PSP__
	ScePspFMatrix4 aligned_mtx;
	ScePspFMatrix4 *mat = (ScePspFMatrix4 *)mtx;
	bool needs_copy = !mtxIsAligned16(mtx)
		|| mtx->m[0][3] != 0.0f
		|| mtx->m[1][3] != 0.0f
		|| mtx->m[2][3] != 0.0f
		|| mtx->m[3][3] != 1.0f;

	if (needs_copy) {
		mtxCopyF32x16((f32 *)&aligned_mtx, &mtx->m[0][0]);
		mtxForceAffine((f32 *)&aligned_mtx);
		mat = &aligned_mtx;
	}

	mtxVfpuTransformVec(mat, vec, 1.0f, dst);
	return;
#endif
	f32 x = vec->x;
	f32 y = vec->y;
	f32 z = vec->z;

	dst->x = mtx->m[0][0] * x + mtx->m[1][0] * y + mtx->m[2][0] * z;
	dst->y = mtx->m[0][1] * x + mtx->m[1][1] * y + mtx->m[2][1] * z;
	dst->z = mtx->m[0][2] * x + mtx->m[1][2] * y + mtx->m[2][2] * z;

	dst->x += mtx->m[3][0];
	dst->y += mtx->m[3][1];
	dst->z += mtx->m[3][2];
}

void mtx00015be0(Mtxf *matrix1, Mtxf *matrix2)
{
	mtx00015be4(matrix1, matrix2, matrix2);
}

void mtx00015be4(Mtxf *arg0, Mtxf *arg1, Mtxf *dst)
{
#ifdef __PSP__
	ScePspFMatrix4 aligned_arg0;
	ScePspFMatrix4 aligned_arg1;
	ScePspFMatrix4 aligned_dst;
	const ScePspFMatrix4 *a = (const ScePspFMatrix4 *)arg0;
	const ScePspFMatrix4 *b = (const ScePspFMatrix4 *)arg1;
	ScePspFMatrix4 *o = (ScePspFMatrix4 *)dst;
	bool b_needs_copy = !mtxIsAligned16(arg1)
		|| arg1->m[0][3] != 0.0f
		|| arg1->m[1][3] != 0.0f
		|| arg1->m[2][3] != 0.0f
		|| arg1->m[3][3] != 1.0f;

	if (!mtxIsAligned16(arg0)) {
		mtxCopyF32x16((f32 *)&aligned_arg0, &arg0->m[0][0]);
		a = &aligned_arg0;
	}

	if (b_needs_copy) {
		mtxCopyF32x16((f32 *)&aligned_arg1, &arg1->m[0][0]);
		mtxForceAffine((f32 *)&aligned_arg1);
		b = &aligned_arg1;
	}

	if (!mtxIsAligned16(dst) || dst == arg0 || dst == arg1) {
		o = &aligned_dst;
	}

	__asm__ __volatile__(
		"lv.q C000, 0(%[a])\n"
		"lv.q C010, 16(%[a])\n"
		"lv.q C020, 32(%[a])\n"
		"lv.q C030, 48(%[a])\n"
		"lv.q C100, 0(%[b])\n"
		"lv.q C110, 16(%[b])\n"
		"lv.q C120, 32(%[b])\n"
		"lv.q C130, 48(%[b])\n"
		"vmmul.q M200, M000, M100\n"
		"sv.q C200, 0(%[o])\n"
		"sv.q C210, 16(%[o])\n"
		"sv.q C220, 32(%[o])\n"
		"sv.q C230, 48(%[o])\n"
		:
		: [a] "r"(a), [b] "r"(b), [o] "r"(o)
		: "memory");

	if (o != (ScePspFMatrix4 *)dst) {
		mtxCopyF32x16(&dst->m[0][0], (const f32 *)o);
	}

	dst->m[0][3] = 0.0f;
	dst->m[1][3] = 0.0f;
	dst->m[2][3] = 0.0f;
	dst->m[3][3] = 1.0f;
	return;
#endif
	f32 m00 = arg1->m[0][0];
	f32 m01 = arg1->m[0][1];
	f32 m02 = arg1->m[0][2];
	f32 m03 = arg1->m[0][3];
	f32 m10 = arg1->m[1][0];
	f32 m11 = arg1->m[1][1];
	f32 m12 = arg1->m[1][2];
	f32 m13 = arg1->m[1][3];
	f32 m20 = arg1->m[2][0];
	f32 m21 = arg1->m[2][1];
	f32 m22 = arg1->m[2][2];
	f32 m23 = arg1->m[2][3];
	f32 m30 = arg1->m[3][0];
	f32 m31 = arg1->m[3][1];
	f32 m32 = arg1->m[3][2];
	f32 m33 = arg1->m[3][3];

	dst->m[0][0] = arg0->m[0][0] * m00 + arg0->m[1][0] * m01 + arg0->m[2][0] * m02;
	dst->m[0][1] = arg0->m[0][1] * m00 + arg0->m[1][1] * m01 + arg0->m[2][1] * m02;
	dst->m[0][2] = arg0->m[0][2] * m00 + arg0->m[1][2] * m01 + arg0->m[2][2] * m02;
	dst->m[0][3] = 0;

	dst->m[1][0] = arg0->m[0][0] * m10 + arg0->m[1][0] * m11 + arg0->m[2][0] * m12;
	dst->m[1][1] = arg0->m[0][1] * m10 + arg0->m[1][1] * m11 + arg0->m[2][1] * m12;
	dst->m[1][2] = arg0->m[0][2] * m10 + arg0->m[1][2] * m11 + arg0->m[2][2] * m12;
	dst->m[1][3] = 0;

	dst->m[2][0] = arg0->m[0][0] * m20 + arg0->m[1][0] * m21 + arg0->m[2][0] * m22;
	dst->m[2][1] = arg0->m[0][1] * m20 + arg0->m[1][1] * m21 + arg0->m[2][1] * m22;
	dst->m[2][2] = arg0->m[0][2] * m20 + arg0->m[1][2] * m21 + arg0->m[2][2] * m22;
	dst->m[2][3] = 0;

	dst->m[3][0] = arg0->m[0][0] * m30 + arg0->m[1][0] * m31 + arg0->m[2][0] * m32 + arg0->m[3][0];
	dst->m[3][1] = arg0->m[0][1] * m30 + arg0->m[1][1] * m31 + arg0->m[2][1] * m32 + arg0->m[3][1];
	dst->m[3][2] = arg0->m[0][2] * m30 + arg0->m[1][2] * m31 + arg0->m[2][2] * m32 + arg0->m[3][2];
	dst->m[3][3] = 1;
}

void mtx3Copy(f32 src[3][3], f32 dst[3][3])
{
	dst[0][0] = src[0][0];
	dst[0][1] = src[0][1];
	dst[0][2] = src[0][2];

	dst[1][0] = src[1][0];
	dst[1][1] = src[1][1];
	dst[1][2] = src[1][2];

	dst[2][0] = src[2][0];
	dst[2][1] = src[2][1];
	dst[2][2] = src[2][2];
}

void mtx4Copy(Mtxf *src, Mtxf *dst)
{
	*dst = *src;
}

void mtx3ToMtx4(f32 src[3][3], Mtxf *dst)
{
	dst->m[0][0] = src[0][0];
	dst->m[0][1] = src[0][1];
	dst->m[0][2] = src[0][2];
	dst->m[0][3] = 0;

	dst->m[1][0] = src[1][0];
	dst->m[1][1] = src[1][1];
	dst->m[1][2] = src[1][2];
	dst->m[1][3] = 0;

	dst->m[2][0] = src[2][0];
	dst->m[2][1] = src[2][1];
	dst->m[2][2] = src[2][2];
	dst->m[2][3] = 0;

	dst->m[3][0] = 0;
	dst->m[3][1] = 0;
	dst->m[3][2] = 0;
	dst->m[3][3] = 1;
}

void mtx4ToMtx3(Mtxf *src, f32 dst[3][3])
{
	dst[0][0] = src->m[0][0];
	dst[0][1] = src->m[0][1];
	dst[0][2] = src->m[0][2];

	dst[1][0] = src->m[1][0];
	dst[1][1] = src->m[1][1];
	dst[1][2] = src->m[1][2];

	dst[2][0] = src->m[2][0];
	dst[2][1] = src->m[2][1];
	dst[2][2] = src->m[2][2];
}

void mtx4SetTranslation(struct coord *pos, Mtxf *mtx)
{
	mtx->m[3][0] = pos->x;
	mtx->m[3][1] = pos->y;
	mtx->m[3][2] = pos->z;
}

void mtx00015df0(f32 mult, Mtxf *mtx)
{
	mtx->m[0][0] *= mult;
	mtx->m[0][1] *= mult;
	mtx->m[0][2] *= mult;
	mtx->m[0][3] *= mult;
}

void mtx00015e24(f32 mult, Mtxf *mtx)
{
	mtx->m[0][0] *= mult;
	mtx->m[0][1] *= mult;
	mtx->m[0][2] *= mult;
}

void mtx00015e4c(f32 mult, Mtxf *mtx)
{
	mtx->m[1][0] *= mult;
	mtx->m[1][1] *= mult;
	mtx->m[1][2] *= mult;
	mtx->m[1][3] *= mult;
}

void mtx00015e80(f32 mult, Mtxf *mtx)
{
	mtx->m[1][0] *= mult;
	mtx->m[1][1] *= mult;
	mtx->m[1][2] *= mult;
}

void mtx00015ea8(f32 mult, Mtxf *mtx)
{
	mtx->m[2][0] *= mult;
	mtx->m[2][1] *= mult;
	mtx->m[2][2] *= mult;
	mtx->m[2][3] *= mult;
}

void mtx00015edc(f32 mult, Mtxf *mtx)
{
	mtx->m[2][0] *= mult;
	mtx->m[2][1] *= mult;
	mtx->m[2][2] *= mult;
}

void mtx00015f04(f32 mult, Mtxf *mtx)
{
	mtx->m[0][0] *= mult;
	mtx->m[0][1] *= mult;
	mtx->m[0][2] *= mult;
	mtx->m[0][3] *= mult;

	mtx->m[1][0] *= mult;
	mtx->m[1][1] *= mult;
	mtx->m[1][2] *= mult;
	mtx->m[1][3] *= mult;

	mtx->m[2][0] *= mult;
	mtx->m[2][1] *= mult;
	mtx->m[2][2] *= mult;
	mtx->m[2][3] *= mult;
}

void mtx00015f4c(f32 mult, Mtxf *mtx)
{
	mtx->m[0][0] *= mult;
	mtx->m[0][1] *= mult;
	mtx->m[0][2] *= mult;

	mtx->m[1][0] *= mult;
	mtx->m[1][1] *= mult;
	mtx->m[1][2] *= mult;

	mtx->m[2][0] *= mult;
	mtx->m[2][1] *= mult;
	mtx->m[2][2] *= mult;
}

void mtx00015f88(f32 mult, Mtxf *mtx)
{
	mtx->m[0][0] *= mult;
	mtx->m[0][1] *= mult;
	mtx->m[0][2] *= mult;

	mtx->m[1][0] *= mult;
	mtx->m[1][1] *= mult;
	mtx->m[1][2] *= mult;

	mtx->m[2][0] *= mult;
	mtx->m[2][1] *= mult;
	mtx->m[2][2] *= mult;

	mtx->m[3][0] *= mult;
	mtx->m[3][1] *= mult;
	mtx->m[3][2] *= mult;
}

u32 mtxGetObfuscatedRomBase(void)
{
#ifdef PLATFORM_N64
	u32 value;

	osRecvMesg(&__osPiAccessQueue, NULL, OS_MESG_BLOCK * 0x10000);

	while (IO_READ(PI_STATUS_REG) & (PI_STATUS_DMA_BUSY | PI_STATUS_IO_BUSY));

	// osRomBase is 0xb0000000
	// load address is 0xb0000a5c
	// value is 0x1740fff9
	value = *(u32 *) (((uintptr_t) osRomBase | 0xb764b4fd) ^ 0x0764bea1);

	osSendMesg(&__osPiAccessQueue, 0, 0);

	return value;
#else
	return 0;
#endif
}

void mtxF2L(Mtxf *src, Mtxf *dst)
{
#ifndef GBI_FLOATS
#ifdef __PSP__
	if (mtxIsAligned16(src)) {
		__asm__ volatile(
			".set push\n"
			".set noreorder\n"
			"lui        $t8, 0xffff\n" /* $t8 = 0xffff0000 */
			"lv.s       S100, 0(%[scale])\n" /* scale0 */
			"lv.s       S101, 4(%[scale])\n" /* scale1 */
			"vdiv.s     S102, S101, S100\n"  /* ratio = scale1 / scale0 */
			"lv.q       C000, 0(%[src])\n"
			"lv.q       C010, 16(%[src])\n"
			"lv.q       C020, 32(%[src])\n"
			"lv.q       C030, 48(%[src])\n"
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
			"sw         $t9, 0(%[dst])\n"
			"sw         $a3, 32(%[dst])\n"

			/* l[0][1], l[2][1] */
			"and        $t9, $t2, $t8\n"
			"srl        $a2, $t3, 16\n"
			"or         $t9, $t9, $a2\n"
			"sll        $a3, $t2, 16\n"
			"andi       $a2, $t3, 0xffff\n"
			"or         $a3, $a3, $a2\n"
			"sw         $t9, 4(%[dst])\n"
			"sw         $a3, 36(%[dst])\n"

			/* l[0][2], l[2][2] */
			"and        $t9, $t4, $t8\n"
			"srl        $a2, $t5, 16\n"
			"or         $t9, $t9, $a2\n"
			"sll        $a3, $t4, 16\n"
			"andi       $a2, $t5, 0xffff\n"
			"or         $a3, $a3, $a2\n"
			"sw         $t9, 8(%[dst])\n"
			"sw         $a3, 40(%[dst])\n"

			/* l[0][3], l[2][3] */
			"and        $t9, $t6, $t8\n"
			"srl        $a2, $t7, 16\n"
			"or         $t9, $t9, $a2\n"
			"sll        $a3, $t6, 16\n"
			"andi       $a2, $t7, 0xffff\n"
			"or         $a3, $a3, $a2\n"
			"sw         $t9, 12(%[dst])\n"
			"sw         $a3, 44(%[dst])\n"

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
			"sw         $t9, 16(%[dst])\n"
			"sw         $a3, 48(%[dst])\n"

			/* l[1][1], l[3][1] */
			"and        $t9, $t2, $t8\n"
			"srl        $a2, $t3, 16\n"
			"or         $t9, $t9, $a2\n"
			"sll        $a3, $t2, 16\n"
			"andi       $a2, $t3, 0xffff\n"
			"or         $a3, $a3, $a2\n"
			"sw         $t9, 20(%[dst])\n"
			"sw         $a3, 52(%[dst])\n"

			/* l[1][2], l[3][2] */
			"and        $t9, $t4, $t8\n"
			"srl        $a2, $t5, 16\n"
			"or         $t9, $t9, $a2\n"
			"sll        $a3, $t4, 16\n"
			"andi       $a2, $t5, 0xffff\n"
			"or         $a3, $a3, $a2\n"
			"sw         $t9, 24(%[dst])\n"
			"sw         $a3, 56(%[dst])\n"

			/* l[1][3], l[3][3] */
			"and        $t9, $t6, $t8\n"
			"srl        $a2, $t7, 16\n"
			"or         $t9, $t9, $a2\n"
			"sll        $a3, $t6, 16\n"
			"andi       $a2, $t7, 0xffff\n"
			"or         $a3, $a3, $a2\n"
			"sw         $t9, 28(%[dst])\n"
			"sw         $a3, 60(%[dst])\n"
			".set pop\n"
			:
			: [src] "r"(src), [dst] "r"(dst), [scale] "r"(var8005ef10)
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
			"ulv.q      C000, 0(%[src])\n"
			"ulv.q      C010, 16(%[src])\n"
			"ulv.q      C020, 32(%[src])\n"
			"ulv.q      C030, 48(%[src])\n"
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
			"sw         $t9, 0(%[dst])\n"
			"sw         $a3, 32(%[dst])\n"

			/* l[0][1], l[2][1] */
			"and        $t9, $t2, $t8\n"
			"srl        $a2, $t3, 16\n"
			"or         $t9, $t9, $a2\n"
			"sll        $a3, $t2, 16\n"
			"andi       $a2, $t3, 0xffff\n"
			"or         $a3, $a3, $a2\n"
			"sw         $t9, 4(%[dst])\n"
			"sw         $a3, 36(%[dst])\n"

			/* l[0][2], l[2][2] */
			"and        $t9, $t4, $t8\n"
			"srl        $a2, $t5, 16\n"
			"or         $t9, $t9, $a2\n"
			"sll        $a3, $t4, 16\n"
			"andi       $a2, $t5, 0xffff\n"
			"or         $a3, $a3, $a2\n"
			"sw         $t9, 8(%[dst])\n"
			"sw         $a3, 40(%[dst])\n"

			/* l[0][3], l[2][3] */
			"and        $t9, $t6, $t8\n"
			"srl        $a2, $t7, 16\n"
			"or         $t9, $t9, $a2\n"
			"sll        $a3, $t6, 16\n"
			"andi       $a2, $t7, 0xffff\n"
			"or         $a3, $a3, $a2\n"
			"sw         $t9, 12(%[dst])\n"
			"sw         $a3, 44(%[dst])\n"

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
			"sw         $t9, 16(%[dst])\n"
			"sw         $a3, 48(%[dst])\n"

			/* l[1][1], l[3][1] */
			"and        $t9, $t2, $t8\n"
			"srl        $a2, $t3, 16\n"
			"or         $t9, $t9, $a2\n"
			"sll        $a3, $t2, 16\n"
			"andi       $a2, $t3, 0xffff\n"
			"or         $a3, $a3, $a2\n"
			"sw         $t9, 20(%[dst])\n"
			"sw         $a3, 52(%[dst])\n"

			/* l[1][2], l[3][2] */
			"and        $t9, $t4, $t8\n"
			"srl        $a2, $t5, 16\n"
			"or         $t9, $t9, $a2\n"
			"sll        $a3, $t4, 16\n"
			"andi       $a2, $t5, 0xffff\n"
			"or         $a3, $a3, $a2\n"
			"sw         $t9, 24(%[dst])\n"
			"sw         $a3, 56(%[dst])\n"

			/* l[1][3], l[3][3] */
			"and        $t9, $t6, $t8\n"
			"srl        $a2, $t7, 16\n"
			"or         $t9, $t9, $a2\n"
			"sll        $a3, $t6, 16\n"
			"andi       $a2, $t7, 0xffff\n"
			"or         $a3, $a3, $a2\n"
			"sw         $t9, 28(%[dst])\n"
			"sw         $a3, 60(%[dst])\n"
			".set pop\n"
			:
			: [src] "r"(src), [dst] "r"(dst), [scale] "r"(var8005ef10)
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
	return;
#endif
	u32 src00 = (s32) (src->m[0][0] * var8005ef10[0]);
	u32 src01 = (s32) (src->m[0][1] * var8005ef10[0]);
	u32 src02 = (s32) (src->m[0][2] * var8005ef10[0]);
	u32 src03 = (s32) (src->m[0][3] * var8005ef10[1]);
	u32 src10 = (s32) (src->m[1][0] * var8005ef10[0]);
	u32 src11 = (s32) (src->m[1][1] * var8005ef10[0]);
	u32 src12 = (s32) (src->m[1][2] * var8005ef10[0]);
	u32 src13 = (s32) (src->m[1][3] * var8005ef10[1]);
	u32 src20 = (s32) (src->m[2][0] * var8005ef10[0]);
	u32 src21 = (s32) (src->m[2][1] * var8005ef10[0]);
	u32 src22 = (s32) (src->m[2][2] * var8005ef10[0]);
	u32 src23 = (s32) (src->m[2][3] * var8005ef10[1]);
	u32 src30 = (s32) (src->m[3][0] * var8005ef10[0]);
	u32 src31 = (s32) (src->m[3][1] * var8005ef10[0]);
	u32 src32 = (s32) (src->m[3][2] * var8005ef10[0]);
	u32 src33 = (s32) (src->m[3][3] * var8005ef10[1]);

	dst->l[0][0] = (src00 & 0xffff0000) | src01 >> 16;
	dst->l[0][1] = (src02 & 0xffff0000) | src03 >> 16;
	dst->l[0][2] = (src10 & 0xffff0000) | src11 >> 16;
	dst->l[0][3] = (src12 & 0xffff0000) | src13 >> 16;
	dst->l[1][0] = (src20 & 0xffff0000) | src21 >> 16;
	dst->l[1][1] = (src22 & 0xffff0000) | src23 >> 16;
	dst->l[1][2] = (src30 & 0xffff0000) | src31 >> 16;
	dst->l[1][3] = (src32 & 0xffff0000) | src33 >> 16;

	dst->l[2][0] = src00 << 16 | (src01 & 0xffff);
	dst->l[2][1] = src02 << 16 | (src03 & 0xffff);
	dst->l[2][2] = src10 << 16 | (src11 & 0xffff);
	dst->l[2][3] = src12 << 16 | (src13 & 0xffff);
	dst->l[3][0] = src20 << 16 | (src21 & 0xffff);
	dst->l[3][1] = src22 << 16 | (src23 & 0xffff);
	dst->l[3][2] = src30 << 16 | (src31 & 0xffff);
	dst->l[3][3] = src32 << 16 | (src33 & 0xffff);
#else
	s32 i;
	s32 j;
	f32 scale0 = var8005ef10[0] * (1.0f / 65536.0f);
	f32 scale1 = var8005ef10[1] * (1.0f / 65536.0f);

	for (i = 0; i < 4; i++) {
		for (j = 0; j < 4; j++) {
			f32 scale = (j == 3) ? scale1 : scale0;
			dst->m[i][j] = src->m[i][j] * scale;
		}
	}
#endif
}
