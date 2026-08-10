.set noat

.text
.set push
.set noreorder

/*
 * void gfx_transform_vertices_vfpu_pd(LoadedVertex *dest, const Vtx *source,
 *                                     uint32_t count,
 *                                     const TransformSnapshot *transform,
 *                                     uint32_t clip_config);
 *
 * PD Vtx is 12 bytes. PSP LoadedVertex is 64 bytes, with object position at
 * offset 0, clip flags at offset 29, clip position at offset 32 and eye-space
 * position at offset 48. TransformSnapshot contains P, M and MP matrices at
 * offsets 0, 64 and 128. The matrices use row-vector layout. clip_config holds
 * the active plane mask in bits 0..7 and selects z-in-[0,w] near clipping with
 * bit 8.
 */
#define VTX_SIZE 12
#define LOADED_VERTEX_SIZE 64
#define LOADED_VERTEX_OBJECT_POS 0
#define LOADED_VERTEX_CLIP_REJ 29
#define LOADED_VERTEX_CLIP_POS 32
#define LOADED_VERTEX_EYE_POS 48
#define TRANSFORM_M 64
#define TRANSFORM_MP 128
#define CLIP_EPSILON_BITS 0x3727c5ac

.globl gfx_transform_vertices_vfpu_pd
gfx_transform_vertices_vfpu_pd:
	beq     $a2, $zero, .Lpd_transform_done
	nop

	/* PSP EABI passes the fifth argument in $t0. Preserve it before loading X. */
	or      $t3, $t0, $zero
	li      $t4, CLIP_EPSILON_BITS
	mtv     $t4, S700
	vone.q  C710
	vscl.q  C710, C710, S700
	vzero.q C720

	/* Load the four MP and modelview columns once for the whole batch. */
	lv.s    S000, TRANSFORM_MP+0($a3)
	lv.s    S001, TRANSFORM_MP+16($a3)
	lv.s    S002, TRANSFORM_MP+32($a3)
	lv.s    S003, TRANSFORM_MP+48($a3)
	lv.s    S010, TRANSFORM_MP+4($a3)
	lv.s    S011, TRANSFORM_MP+20($a3)
	lv.s    S012, TRANSFORM_MP+36($a3)
	lv.s    S013, TRANSFORM_MP+52($a3)
	lv.s    S020, TRANSFORM_MP+8($a3)
	lv.s    S021, TRANSFORM_MP+24($a3)
	lv.s    S022, TRANSFORM_MP+40($a3)
	lv.s    S023, TRANSFORM_MP+56($a3)
	lv.s    S030, TRANSFORM_MP+12($a3)
	lv.s    S031, TRANSFORM_MP+28($a3)
	lv.s    S032, TRANSFORM_MP+44($a3)
	lv.s    S033, TRANSFORM_MP+60($a3)

	lv.s    S400, TRANSFORM_M+0($a3)
	lv.s    S401, TRANSFORM_M+16($a3)
	lv.s    S402, TRANSFORM_M+32($a3)
	lv.s    S403, TRANSFORM_M+48($a3)
	lv.s    S410, TRANSFORM_M+4($a3)
	lv.s    S411, TRANSFORM_M+20($a3)
	lv.s    S412, TRANSFORM_M+36($a3)
	lv.s    S413, TRANSFORM_M+52($a3)
	lv.s    S420, TRANSFORM_M+8($a3)
	lv.s    S421, TRANSFORM_M+24($a3)
	lv.s    S422, TRANSFORM_M+40($a3)
	lv.s    S423, TRANSFORM_M+56($a3)
	lv.s    S430, TRANSFORM_M+12($a3)
	lv.s    S431, TRANSFORM_M+28($a3)
	lv.s    S432, TRANSFORM_M+44($a3)
	lv.s    S433, TRANSFORM_M+60($a3)

.Lpd_transform_vertex:
	lh      $t0, 0($a1)
	lh      $t1, 2($a1)
	lh      $t2, 4($a1)
	mtv     $t0, S100
	mtv     $t1, S101
	mtv     $t2, S102
	vi2f.s  S100, S100, 0
	vi2f.s  S101, S101, 0
	vi2f.s  S102, S102, 0
	vone.s  S103

	vdot.q  S200, C000, C100
	vdot.q  S201, C010, C100
	vdot.q  S202, C020, C100
	vdot.q  S203, C030, C100
	vdot.q  S500, C400, C100
	vdot.q  S501, C410, C100
	vdot.q  S502, C420, C100
	vdot.q  S503, C430, C100

	/*
	 * loaded_vertices is not guaranteed to begin on a 16-byte boundary. Its
	 * 64-byte stride preserves whatever alignment the allocation supplied, so
	 * use unaligned vector stores for all three vec4 results.
	 */
	usv.q   C100, LOADED_VERTEX_OBJECT_POS($a0)
	usv.q   C200, LOADED_VERTEX_CLIP_POS($a0)
	usv.q   C500, LOADED_VERTEX_EYE_POS($a0)

	/*
	 * Perfect Dark clip flags, evaluated entirely on VFPU:
	 *   left/right  = w +/- x
	 *   bottom/top  = w +/- y
	 *   near        = z or w + z
	 *   far         = w - z
	 * The positive epsilon matches the polygon clipper's >= -epsilon test.
	 */
	or      $t4, $zero, $zero
	vadd.s  S300, S203, S200
	vsub.s  S301, S203, S200
	vadd.s  S302, S203, S201
	vsub.s  S303, S203, S201
	vadd.q  C300, C300, C710
	vcmp.q  LT, C300, C720

	bvf     0, .Lpd_clip_no_left
	nop
	ori     $t4, $t4, 0x01
.Lpd_clip_no_left:
	bvf     1, .Lpd_clip_no_right
	nop
	ori     $t4, $t4, 0x02
.Lpd_clip_no_right:
	bvf     2, .Lpd_clip_no_bottom
	nop
	ori     $t4, $t4, 0x04
.Lpd_clip_no_bottom:
	bvf     3, .Lpd_clip_no_top
	nop
	ori     $t4, $t4, 0x08
.Lpd_clip_no_top:

	andi    $t5, $t3, 0x30
	beq     $t5, $zero, .Lpd_clip_store_mask
	nop

	andi    $t5, $t3, 0x100
	beq     $t5, $zero, .Lpd_clip_near_zw
	nop
	vmov.s  S320, S202
	b       .Lpd_clip_near_ready
	nop
.Lpd_clip_near_zw:
	vadd.s  S320, S203, S202
.Lpd_clip_near_ready:
	vadd.s  S320, S320, S710
	vcmp.s  LT, S320, S720
	bvf     0, .Lpd_clip_no_near
	nop
	ori     $t4, $t4, 0x10
.Lpd_clip_no_near:

	vsub.s  S321, S203, S202
	vadd.s  S321, S321, S710
	vcmp.s  LT, S321, S720
	bvf     0, .Lpd_clip_store_mask
	nop
	ori     $t4, $t4, 0x20

.Lpd_clip_store_mask:
	andi    $t5, $t3, 0xff
	and     $t4, $t4, $t5
	sb      $t4, LOADED_VERTEX_CLIP_REJ($a0)

	addiu   $a1, $a1, VTX_SIZE
	addiu   $a0, $a0, LOADED_VERTEX_SIZE
	addiu   $a2, $a2, -1
	bne     $a2, $zero, .Lpd_transform_vertex
	nop

.Lpd_transform_done:
	jr      $ra
	nop

.set pop
