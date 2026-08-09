.set noat

.text
.set push
.set noreorder

/*
 * void gfx_transform_vertices_vfpu_pd(LoadedVertex *dest, const Vtx *source,
 *                                     uint32_t count, const float mp[4][4]);
 *
 * PD Vtx is 12 bytes. PSP LoadedVertex is 48 bytes, with object position at
 * offset 0 and clip position at offset 32. The matrix uses row-vector layout.
 */
#define VTX_SIZE 12
#define LOADED_VERTEX_SIZE 48
#define LOADED_VERTEX_OBJECT_POS 0
#define LOADED_VERTEX_CLIP_POS 32

.globl gfx_transform_vertices_vfpu_pd
gfx_transform_vertices_vfpu_pd:
	beq     $a2, $zero, .Lpd_transform_done
	nop

	/* Load the four MP matrix columns once for the whole batch. */
	lv.s    S000, 0($a3)
	lv.s    S001, 16($a3)
	lv.s    S002, 32($a3)
	lv.s    S003, 48($a3)
	lv.s    S010, 4($a3)
	lv.s    S011, 20($a3)
	lv.s    S012, 36($a3)
	lv.s    S013, 52($a3)
	lv.s    S020, 8($a3)
	lv.s    S021, 24($a3)
	lv.s    S022, 40($a3)
	lv.s    S023, 56($a3)
	lv.s    S030, 12($a3)
	lv.s    S031, 28($a3)
	lv.s    S032, 44($a3)
	lv.s    S033, 60($a3)

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

	/*
	 * loaded_vertices is not guaranteed to begin on a 16-byte boundary. Its
	 * 48-byte stride preserves whatever alignment the allocation supplied, so
	 * use unaligned vector stores for both vec4 results.
	 */
	usv.q   C100, LOADED_VERTEX_OBJECT_POS($a0)
	usv.q   C200, LOADED_VERTEX_CLIP_POS($a0)

	addiu   $a1, $a1, VTX_SIZE
	addiu   $a0, $a0, LOADED_VERTEX_SIZE
	addiu   $a2, $a2, -1
	bne     $a2, $zero, .Lpd_transform_vertex
	nop

.Lpd_transform_done:
	jr      $ra
	nop

.set pop
