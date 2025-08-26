#include <ultra64.h>
#include "constants.h"
#include "game/bg.h"
#include "game/pad.h"
#include "bss.h"
#include "lib/collision.h"
#include "lib/memp.h"
#include "lib/anim.h"
#include "data.h"
#include "types.h"
#include <string.h>

void coverAllocateSpecial(u16 *specialcovernums)
{
	s32 i;

	g_SpecialCoverNums = mempAlloc(ALIGN16(g_NumSpecialCovers * sizeof(g_SpecialCoverNums[0])), MEMPOOL_STAGE);

	if (g_SpecialCoverNums != NULL) {
		for (i = 0; i < g_NumSpecialCovers; i++) {
			g_SpecialCoverNums[i] = specialcovernums[i];
		}
	}
}

void setupPrepareCover(void)
{
	s32 i;
	s32 numcovers = g_PadsFile->numcovers;
	RoomNum *roomsptr;
	f32 scale = 1;
	struct coord aimpos;
	struct cover cover;
	u16 specialcovernums[1024];
	RoomNum inrooms[21];
	RoomNum aboverooms[21];

	g_CoverFlags = mempAlloc(ALIGN16(numcovers * sizeof(g_CoverFlags[0])), MEMPOOL_STAGE);
	g_CoverRooms = mempAlloc(ALIGN16(numcovers * sizeof(g_CoverRooms[0])), MEMPOOL_STAGE);
	g_CoverCandidates = mempAlloc(ALIGN16(numcovers * sizeof(g_CoverCandidates[0])), MEMPOOL_STAGE);

	g_NumSpecialCovers = 0;
	g_SpecialCoverNums = NULL;

	if (g_CoverFlags && g_CoverRooms && g_CoverCandidates) {
		for (i = 0; i < numcovers; i++) {
			roomsptr = NULL;
			g_CoverFlags[i] = 0;

			if (coverUnpack(i, &cover)) {
				/* Safely load look vector (may be unaligned) */
				float lx, ly, lz;
				memcpy(&lx, &cover.look->x, 4);
				memcpy(&ly, &cover.look->y, 4);
				memcpy(&lz, &cover.look->z, 4);

				/* Skip covers with zero look vector */
				if (lx != 0.0f || ly != 0.0f || lz != 0.0f) {
					if (coverIsSpecial(&cover)) {
						specialcovernums[g_NumSpecialCovers] = i;
						g_NumSpecialCovers++;
					}

					/* Scale position safely */
					float px, py, pz;
					memcpy(&px, &cover.pos->x, 4);
					memcpy(&py, &cover.pos->y, 4);
					memcpy(&pz, &cover.pos->z, 4);
					px *= scale; py *= scale; pz *= scale;
					memcpy(&cover.pos->x, &px, 4);
					memcpy(&cover.pos->y, &py, 4);
					memcpy(&cover.pos->z, &pz, 4);

					/* Omnidirectional or normalize look (write back safely) */
					if (lx == 1.0f && ly == 1.0f && lz == 1.0f) {
						g_CoverFlags[i] |= COVERFLAG_OMNIDIRECTIONAL;
					} else if (!coverIsSpecial(&cover)) {
						ly = 0.0f;
						/* Normalize (manual to avoid taking addresses of packed fields) */
						float len = sqrtf(lx*lx + ly*ly + lz*lz);
						if (len != 0.0f) {
							float inv = 1.0f / len;
							lx *= inv; ly *= inv; lz *= inv;
						}
						memcpy(&cover.look->x, &lx, 4);
						memcpy(&cover.look->y, &ly, 4);
						memcpy(&cover.look->z, &lz, 4);
					}

					/* Find room for position (external fn expects coord*, original code passes cover.pos) */
					bgFindRoomsByPos(cover.pos, inrooms, aboverooms, 20, NULL);

					if (inrooms[0] != -1) {
						roomsptr = inrooms;
					} else if (aboverooms[0] != -1) {
						roomsptr = aboverooms;
					}

					g_CoverRooms[i] = -1;

					if (roomsptr != NULL) {
						s32 room = cdFindFloorRoomAtPos(cover.pos, roomsptr);

						if (room > 0) {
							g_CoverRooms[i] = (RoomNum)room;
						} else {
							g_CoverRooms[i] = roomsptr[0];
						}
					}

					/* Determine if aim is in the same room or not */
					if (g_CoverRooms[i] < 0) {
						g_CoverFlags[i] |= COVERFLAG_AIMSAMEROOM;
					} else if ((g_CoverFlags[i] & COVERFLAG_OMNIDIRECTIONAL) == 0) {
						aimpos.x = px + lx * 600.0f;
						aimpos.y = py;
						aimpos.z = pz + lz * 600.0f;

						bgFindRoomsByPos(&aimpos, inrooms, aboverooms, 20, NULL);

						if (inrooms[0] != -1) {
							roomsptr = inrooms;
						} else if (aboverooms[0] != -1) {
							roomsptr = aboverooms;
						}

						if (roomsptr) {
							s32 aimroom = cdFindFloorRoomAtPos(&aimpos, roomsptr);

							if (aimroom > 0) {
								g_CoverFlags[i] |= (g_CoverRooms[i] == (RoomNum)aimroom) ? COVERFLAG_AIMSAMEROOM : COVERFLAG_AIMDIFFROOM;
							} else {
								g_CoverFlags[i] |= (g_CoverRooms[i] == roomsptr[0]) ? COVERFLAG_AIMSAMEROOM : COVERFLAG_AIMDIFFROOM;
							}
						} else {
							g_CoverFlags[i] |= COVERFLAG_AIMDIFFROOM;
						}
					}
				}
			}
		}

		coverAllocateSpecial(specialcovernums);
	}
}
