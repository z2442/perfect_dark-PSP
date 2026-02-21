#include <ultra64.h>
#include "constants.h"
#include "game/bondhead.h"
#include "game/bg.h"
#include "game/pad.h"
#include "game/setup.h"
#include "bss.h"
#include "lib/collision.h"
#include "lib/mtx.h"
#include "lib/anim.h"
#include "lib/model.h"
#include "data.h"
#include "types.h"
#include "platform.h"
#include "unaligned.h"

/**
 * The function assumes that a pad file's data has been loaded from the ROM
 * and is pointed to by g_StageSetup.padfiledata. These pads are in a packed
 * format. During gameplay, the game uses padUnpack as needed to temporarily
 * populate pad structs from this data.
 *
 * setupPreparePads prepares the packed data by doing the following:
 * - populates the room field (if -1)
 * - multiplies each pad's bounding box by 1 (this is effectively a no op)
 * - sets the g_StageSetup pad/waygroup/waypoint/cover pointers
 * - promotes file offsets to RAM pointers
 * - does similar things for cover by calling setupPrepareCover()
 */
void setupPreparePads(void)
{
	struct packedpad *packedpad;
	RoomNum *roomsptr;
	s32 padnum;
	s32 numpads;
	s32 roomnum;
	struct pad pad;
	struct waypoint *waypoint;
	struct waygroup *waygroup;
	RoomNum inrooms[24];
	RoomNum aboverooms[22];
	s32 offset;

	g_PadsFile = (struct padsfileheader *)g_StageSetup.padfiledata;
#ifdef PLATFORM_64BIT
	g_PadOffsets = (u16 *)(g_StageSetup.padfiledata + 0x20);
#else
	g_PadOffsets = (u16 *)(g_StageSetup.padfiledata + 0x14);
#endif
	padnum = 0;
	numpads = g_PadsFile->numpads;

	for (; padnum < numpads; padnum++) {
		offset = g_PadOffsets[padnum];
		u8 *p = &g_StageSetup.padfiledata[offset];
		packedpad = (struct packedpad *)p; /* keep type info, avoid direct derefs */
		padUnpack(padnum, PADFIELD_POS | PADFIELD_BBOX, &pad);

		/* Load header and room safely (unaligned OK). Room is a bit-field in the header. */
		u32 header_val = pd_load_u32_unaligned(p);
		/* room occupies bits [13:4] as signed 10-bit; extract like padUnpack: (hdr<<18)>>22 */
		s32 packed_room = (s32)(header_val << 18) >> 22;

		/* If room is negative (ie. not specified) */
		if (packed_room < 0) {
			roomsptr = NULL;
			bgFindRoomsByPos(&pad.pos, inrooms, aboverooms, 20, NULL);

			if (inrooms[0] != -1) {
				roomsptr = inrooms;
			} else if (aboverooms[0] != -1) {
				roomsptr = aboverooms;
			}

			if (roomsptr != NULL) {
				roomnum = cdFindFloorRoomAtPos(&pad.pos, roomsptr);
				s32 newroom = (roomnum > 0) ? roomnum : roomsptr[0];
				/* Clamp to signed 10-bit range (-512..511) */
				if (newroom < -512) newroom = -512;
				if (newroom >  511) newroom =  511;
				/* Clear room bits [13:4] and set them */
				header_val &= ~0x00003FF0u;
				header_val |= ((u32)(newroom & 0x3FF) << 4);
				pd_store_u32_unaligned(p, header_val);
			}
		}

		/* Scale the bbox by 1 and save it back into the packed pad data. (no-op) */
		if ((header_val >> 14) & PADFLAG_HASBBOXDATA) {
			f32 scale = 1;

			pad.bbox.xmin *= scale;
			pad.bbox.xmax *= scale;
			pad.bbox.ymin *= scale;
			pad.bbox.ymax *= scale;
			pad.bbox.zmin *= scale;
			pad.bbox.zmax *= scale;

			padCopyBboxFromPad(padnum, &pad);
		}
	}

	g_StageSetup.waypoints = (struct waypoint *) ((uintptr_t)g_StageSetup.padfiledata + g_PadsFile->waypointsoffset);
	g_StageSetup.waygroups = (struct waygroup *) ((uintptr_t)g_StageSetup.padfiledata + g_PadsFile->waygroupsoffset);
	g_StageSetup.cover = (void *) ((intptr_t)g_StageSetup.padfiledata + g_PadsFile->coversoffset);

	if (g_StageSetup.cover != NULL) {
		setupPrepareCover();
	}

	// Promote offsets to pointers in waypoints (unaligned-safe)
	waypoint = g_StageSetup.waypoints;
	for (;;) {
		s32 padnum_val = pd_load_s32_unaligned((u8 *)waypoint + offsetof(struct waypoint, padnum));
		if (padnum_val < 0) {
			break;
		}

		uintptr_t neiptr = pd_load_uptr_unaligned((u8 *)waypoint + offsetof(struct waypoint, neighbours));
		neiptr = (uintptr_t)g_StageSetup.padfiledata + neiptr;
		pd_store_uptr_unaligned((u8 *)waypoint + offsetof(struct waypoint, neighbours), neiptr);

		waypoint = (struct waypoint *)((u8 *)waypoint + sizeof(struct waypoint));
	}

	// Promote offsets to pointers in waygroups (unaligned-safe)
	waygroup = g_StageSetup.waygroups;
	for (;;) {
		uintptr_t neigh_val = pd_load_uptr_unaligned((u8 *)waygroup + offsetof(struct waygroup, neighbours));
		if (neigh_val == NULL) {
			break;
		}

		uintptr_t new_neigh = (uintptr_t)g_StageSetup.padfiledata + neigh_val;
		pd_store_uptr_unaligned((u8 *)waygroup + offsetof(struct waygroup, neighbours), new_neigh);

		uintptr_t wps_val = pd_load_uptr_unaligned((u8 *)waygroup + offsetof(struct waygroup, waypoints));
		uintptr_t new_wps = (uintptr_t)g_StageSetup.padfiledata + wps_val;
		pd_store_uptr_unaligned((u8 *)waygroup + offsetof(struct waygroup, waypoints), new_wps);

		waygroup = (struct waygroup *)((u8 *)waygroup + sizeof(struct waygroup));
	}
}
