#include <ultra64.h>
#include "constants.h"
#include "game/prop.h"
#include "game/bg.h"
#include "game/pad.h"
#include "game/padhalllv.h"
#include "bss.h"
#include "lib/collision.h"
#include "lib/rng.h"
#include "lib/anim.h"
#include "lib/libc/ll.h"
#include "data.h"
#include "types.h"
#include "unaligned.h"
#include <stddef.h>

/**
 * Path Finding
 *
 * Usage:
 * - The caller calls waypointFindClosestToPos() twice to find the from and to waypoints.
 * - The caller allocates an array of waypoint pointers (typically 6 elements).
 * - The caller calls navFindRoute() with the two waypoints, a pointer to the array and the array length.
 * - navFindRoute() writes the waypoint pointers into the array and returns the number of elements written.
 *
 * If there are more waypoints in the path than the array can fit, only the first <array size> waypoints are written.
 * In this case, the caller should re-run the path finding once some of the path has been traversed to get the next
 * waypoints. However, there is randomness involved in path finding. To ensure the same path is used, the caller can
 * preseed the random number generator for the path finder. It can do this by calling navSetSeed() with a random
 * seed before calling navFindRoute(), and it should call navSetSeed() with a zero seed to clear it for the
 * next caller.
 *
 * The algorithm is hierarchical, and uses Dijkstra's algorithm but with a cost of 1 for every segment and breaks ties
 * using randomness.
 *
 * More specifically:
 * Waypoints are grouped into clusters called waygroups. A waygroup might be the same thing as a room, but not
 * necessarily. The algorithm finds the path at the waygroup level, then a path is found through the waypoints in the
 * first group, then the second group, and so on until the results array is full or the destination is reached.
 *
 * To actually find a path, a step number (cost) is assigned to each node until the destination is discovered. It then
 * works backwards from the destination, choosing which node will be the one prior to it by looking for the neighbouring
 * node which is one step lower. If there are multiple nodes to choose from then the selected node will be random.
 */

#define WPSEGFLAG_OUTWARDSONLY 0x4000 // eg. top of ledge
#define WPSEGFLAG_INWARDSONLY  0x8000 // eg. bottom of ledge

#define IGNORE_NONE     0
#define IGNORE_OUTWARDS WPSEGFLAG_OUTWARDSONLY
#define IGNORE_INWARDS  WPSEGFLAG_INWARDSONLY

#define WPSEG_GET_ID(seg) (seg & (0xffff & ~(WPSEGFLAG_OUTWARDSONLY | WPSEGFLAG_INWARDSONLY)))

u32 g_NavSeed[2] = {0};

void navSetSeed(u32 upper, u32 lower)
{
	g_NavSeed[0] = upper;
	g_NavSeed[1] = lower;
}

/**
 * Given a position and the rooms it's in, find the closest waypoint, taking
 * into consideration neighbouring rooms and line of sight tests.
 *
 * This is typically called twice prior to route finding; once with the chr's
 * pos to get their starting waypoint and once with the player's pos to get the
 * ending waypoint.
 *
 * Only waypoints within the position's room and its neighbouring rooms are
 * considered. Because of this, any two directly connected waypoints must be
 * in the same or neighbouring rooms.
 *
 * The function will return NULL if there are no waypoints at all within the
 * position's room or its neighbours.
 */
struct waypoint *waypointFindClosestToPos(struct coord *pos, RoomNum *rooms)
{
	struct waypoint *closest = NULL;
#ifdef AVOID_UB
	 // prevent bgRoomGetNeighbours or roomsAppend from writing out of bounds
	RoomNum allrooms[31];
	RoomNum neighbours[11];
#else
	RoomNum allrooms[30];
	RoomNum neighbours[10];
#endif
	s32 candlen = 0;
	s32 i;
	s32 j;
	struct waypoint *candwaypoints[10];
	f32 candsqdists[10];
	bool checkmore[10];
	struct coord sp250[10];
	struct coord sp1d8[10];

	/* PSP-safe: pos might be unaligned; copy to a local aligned coord and cache components */
	struct coord pos_aligned;
	float px, py, pz;
	px = pd_load_f32_unaligned(&pos->x);
	py = pd_load_f32_unaligned(&pos->y);
	pz = pd_load_f32_unaligned(&pos->z);
	pos_aligned.x = px;
	pos_aligned.y = py;
	pos_aligned.z = pz;

	for (i = 0; rooms[i] != -1; i++) {
		allrooms[i] = rooms[i];
	}

	allrooms[i] = -1;

	for (i = 0; rooms[i] != -1; i++) {
		bgRoomGetNeighbours(rooms[i], neighbours, 10);
		roomsAppend(neighbours, allrooms, 30);
	}

	if (g_StageSetup.waypoints != NULL) {
		// Build the candidates list, sorted by distance (closest first)
		for (i = 0; allrooms[i] != -1; i++) {
			if (g_Rooms[allrooms[i]].numwaypoints != 0) {
				for (j = 0; j < g_Rooms[allrooms[i]].numwaypoints; j++) {
					s32 index = g_Vars.waypointnums[g_Rooms[allrooms[i]].firstwaypoint + j];
					struct waypoint *waypoint = g_StageSetup.waypoints + index;
					f32 sqdist;
					s32 k;
					u32 stack;
					struct pad pad;

					/* PSP-safe: waypoint->padnum may be unaligned in some builds */
					s32 wp_padnum;
					wp_padnum = pd_load_s32_unaligned((const u8 *)waypoint + offsetof(struct waypoint, padnum));
					padUnpack(wp_padnum, PADFIELD_POS, &pad);

					sqdist = (px - pad.pos.f[0]) * (px - pad.pos.f[0])
						+ (py - pad.pos.f[1]) * (py - pad.pos.f[1])
						+ (pz - pad.pos.f[2]) * (pz - pad.pos.f[2]);

					// Find the index where this waypoint should go
					// into the candidates list
					for (index = 0; index < candlen; index++) {
						if (sqdist < candsqdists[index]) {
							break;
						}
					}

					// Insert the new candidate
					if (index < ARRAYCOUNT(candwaypoints)) {
						k = candlen - 1;

						if (k > ARRAYCOUNT(candwaypoints) - 2) {
							k = ARRAYCOUNT(candwaypoints) - 2;
						}

						while (k >= index) {
							candwaypoints[k + 1] = candwaypoints[k];
							candsqdists[k + 1] = candsqdists[k];
							k--;
						}

						candwaypoints[index] = waypoint;
						candsqdists[index] = sqdist;

						if (candlen < ARRAYCOUNT(candwaypoints)) {
							candlen++;
						}
					}
				}
			}
		}

		// Check which candidates have line of sight
		for (i = 0; i < candlen; i++) {
			struct pad pad;
			RoomNum padrooms[8];

			{
				s32 wp_padnum2;
				wp_padnum2 = pd_load_s32_unaligned((const u8 *)candwaypoints[i] + offsetof(struct waypoint, padnum));
				padUnpack(wp_padnum2, PADFIELD_POS | PADFIELD_ROOM, &pad);
			}

			padrooms[0] = pad.room;
			padrooms[1] = -1;

			if (cdTestLos05(&pos_aligned, rooms, &pad.pos, padrooms, CDTYPE_BG, GEOFLAG_FLOOR1 | GEOFLAG_FLOOR2) != CDRESULT_COLLISION) {
				s32 cdresult = cdExamCylMove05(&pos_aligned, rooms, &pad.pos, padrooms, CDTYPE_BG | CDTYPE_PATHBLOCKER, true, 0.0f, 0.0f);

				if (cdresult == CDRESULT_ERROR) {
					checkmore[i] = false;
				} else if (cdresult == CDRESULT_COLLISION) {
					checkmore[i] = true;
					cdGetEdge(&sp250[i], &sp1d8[i], 441, "padhalllv.c");
				} else {
					closest = candwaypoints[i];
					break;
				}
			} else {
				checkmore[i] = false;
			}
		}

		if (closest == NULL) {
			// If this is reached, the chr has no line of sight to any waypoint.
			// This should be pretty rare, but it can happen if the pos is
			// inside a lift or out of bounds.
			// This is selecting the first (closest) waypoint that matches some
			// crtieria relating to collisions.
			for (i = 0; i < candlen; i++) {
				if (checkmore[i] && (sp250[i].x != sp1d8[i].x || sp250[i].z != sp1d8[i].z)) {
					struct pad pad;
					RoomNum padrooms[8];
					struct coord sp98;
					struct coord tmppos;
					RoomNum tmprooms[8];
					f32 mult;

					{
						s32 wp_padnum3;
						wp_padnum3 = pd_load_s32_unaligned((const u8 *)candwaypoints[i] + offsetof(struct waypoint, padnum));
						padUnpack(wp_padnum3, PADFIELD_POS | PADFIELD_ROOM, &pad);
					}

					padrooms[0] = pad.room;
					padrooms[1] = -1;

					sp98.f[0] = sp250[i].x - sp1d8[i].x;
					sp98.f[1] = 0.0f;
					sp98.f[2] = sp250[i].z - sp1d8[i].z;

					mult = 10.0f / pspFpuSqrt(sp98.f[0] * sp98.f[0] + sp98.f[2] * sp98.f[2]);

					sp98.x *= mult;
					sp98.z *= mult;

					tmppos.x = sp250[i].f[0] + sp98.f[0];
					tmppos.y = py;
					tmppos.z = sp250[i].f[2] + sp98.f[2];

					if (cdTestCylMove04(&pos_aligned, rooms, &tmppos, tmprooms, CDTYPE_BG | CDTYPE_PATHBLOCKER, 1, 0.0f, 0.0f) != CDRESULT_COLLISION) {
						closest = candwaypoints[i];
						break;
					}

					tmppos.x = sp1d8[i].x - sp98.x;
					tmppos.y = py;
					tmppos.z = sp1d8[i].z - sp98.z;

					if (cdTestCylMove04(&pos_aligned, rooms, &tmppos, tmprooms, CDTYPE_BG | CDTYPE_PATHBLOCKER, 1, 0.0f, 0.0f) != CDRESULT_COLLISION) {
						closest = candwaypoints[i];
						break;
					}
				}
			}

			// If the above criteria didn't match anything,
			// just choose the closest waypoint
			if (closest == NULL && candlen > 0) {
				closest = candwaypoints[0];
			}
		}
	}

	return closest;
}

/**
 * Given a group that is known to be on the path, groupnums is a pointer to that
 * group's neighbours. At least one of these groups should have the given step
 * number.
 *
 * The function chooses which neighbour will be routed through.
 *
 * If padrandomroutes is false, the first neighbour with the correct step is used.
 * If padrandomroutes is true, the algorithm has a 50% chance of searching for
 * another neighbour.
 *
 * padrandomroutes is always true.
 */
struct waygroup *waygroupChooseNeighbour(s32 *groupnums, s32 step, u32 ignoremask)
{
	struct waygroup *groups = g_StageSetup.waygroups;
	struct waygroup *best = NULL;

	for (;;) {
    s32 seg;
    seg = pd_load_s32_unaligned(groupnums);
    if (seg < 0) break;

    if ((seg & ignoremask) == 0) {
        struct waygroup *group = &groups[WPSEG_GET_ID(seg)];
        s32 gstep;
        gstep = pd_load_s32_unaligned((const u8 *)group + offsetof(struct waygroup, step));

        if (gstep == step) {
            best = group;

				if (!g_Vars.padrandomroutes) {
					break;
				}

				if (!g_NavSeed[0] && !g_NavSeed[1]) {
					if (rngRandom() % 2 == 0) {
						break;
					}
				} else {
					u64 seed = ((u64)g_NavSeed[0] << 32) | g_NavSeed[1];

					if (rngRotateSeed(&seed) % 2 == 0) {
						break;
					}
				}
			}
		}

		groupnums++;
	}

	return best;
}

/**
 * Iterate the given groupnums and set their step if they don't already have one.
 */
void waygroupSetStepIfUndiscovered(s32 *groupnums, s32 step, u32 ignoremask)
{
	struct waygroup *groups = g_StageSetup.waygroups;

	for (;;) {
    s32 seg;
    seg = pd_load_s32_unaligned(groupnums);
    if (seg < 0) break;

    if ((seg & ignoremask) == 0) {
        struct waygroup *group = &groups[WPSEG_GET_ID(seg)];
        s32 gstep;
        gstep = pd_load_s32_unaligned((const u8 *)group + offsetof(struct waygroup, step));
        if (gstep < 0) {
            pd_store_s32_unaligned((u8 *)group + offsetof(struct waygroup, step), step);
        }
    }
    groupnums++;
	}
}

/**
 * Do one scan of all waygroups, finding ones at the given step.
 * Set their neighbours to step + 1 if they haven't been discovered yet.
 */
bool waygroupDiscoverOneStep(struct waygroup *group, s32 step, u32 ignoremask)
{
	bool discovered = false;

	for (;;) {
    void *neigh;
    neigh = (s32 *)(uintptr_t)pd_load_uptr_unaligned((const u8 *)group + offsetof(struct waygroup, neighbours));
    if (neigh == NULL) break;

    s32 gstep;
    gstep = pd_load_s32_unaligned((const u8 *)group + offsetof(struct waygroup, step));
    if (gstep == step) {
        discovered = true;
        waygroupSetStepIfUndiscovered((s32 *)neigh, step + 1, ignoremask);
    }
    group = (struct waygroup *)((u8 *)group + sizeof(struct waygroup));
	}

	return discovered;
}

/**
 * Figure out every group's step from the starting group.
 *
 * It does this by resetting all steps to -1, then setting the from group's step
 * to 0, then repeatedly iterating the entire group list and expanding the step
 * each time.
 *
 * If discoverall is false, the discovery stops once the to group is discovered.
 * If discoverall is true, groups beyond the to group are also discovered.
 */
bool waygroupDiscoverSteps(struct waygroup *from, struct waygroup *to, struct waygroup *groups, bool discoverall, u32 ignoremask)
{
    bool result = true;
    struct waygroup *group;
    s32 step;

    /* Reset all steps to -1 by iterating until neighbours == NULL (unaligned-safe) */
    group = groups;
    for (;;) {
        void *neigh;
        neigh = (s32 *)(uintptr_t)pd_load_uptr_unaligned((const u8 *)group + offsetof(struct waygroup, neighbours));
        if (neigh == NULL) {
            break;
        }
        {
            s32 neg1 = -1;
            pd_store_s32_unaligned((u8 *)group + offsetof(struct waygroup, step), neg1);
        }
        group = (struct waygroup *)((u8 *)group + sizeof(struct waygroup));
    }

    /* from->step = 0 */
    {
        s32 zero = 0;
        pd_store_s32_unaligned((u8 *)from + offsetof(struct waygroup, step), zero);
    }

    /* for (step = 0; (discoverall || to->step < 0) && result; step++) */
    for (step = 0; ; step++) {
        if (!discoverall) {
            s32 tostep;
            tostep = pd_load_s32_unaligned((const u8 *)to + offsetof(struct waygroup, step));
            if (tostep >= 0) {
                break;
            }
        }
        if (!result) {
            break;
        }
        result = waygroupDiscoverOneStep(groups, step, ignoremask);
    }

    return result;
}

/**
 * Find a route at the group level between from and to.
 *
 * The groups along the chosen route will have their step numbers set >= 10000.
 */
bool waygroupFindRoute(struct waygroup *from, struct waygroup *to, struct waygroup *groups)
{
    u32 stack[2];
    bool result = waygroupDiscoverSteps(from, to, groups, false, IGNORE_INWARDS);

    if (result) {
        struct waygroup *curto = to;
        s32 step;
        /* load curto->step safely */
        step = pd_load_s32_unaligned((const u8 *)curto + offsetof(struct waygroup, step));
        step -= 1;

        while (step >= 0) {
            /* curto->step += 10000; */
            s32 curstep;
            curstep = pd_load_s32_unaligned((const u8 *)curto + offsetof(struct waygroup, step));
            curstep += 10000;
            pd_store_s32_unaligned((u8 *)curto + offsetof(struct waygroup, step), curstep);

            /* curto = waygroupChooseNeighbour(curto->neighbours, step, IGNORE_OUTWARDS); */
            void *neigh;
            neigh = (s32 *)(uintptr_t)pd_load_uptr_unaligned((const u8 *)curto + offsetof(struct waygroup, neighbours));
            curto = waygroupChooseNeighbour((s32 *)neigh, step, IGNORE_OUTWARDS);

            step--;
        }

        /* final: curto->step += 10000; */
        {
            s32 curstep;
            curstep = pd_load_s32_unaligned((const u8 *)curto + offsetof(struct waygroup, step));
            curstep += 10000;
            pd_store_s32_unaligned((u8 *)curto + offsetof(struct waygroup, step), curstep);
        }
    }

    return result;
}

/**
 * Given a waypoint that is known to be on the path, groupnums is a pointer to
 * that waypoint's neighbours. At least one of these waypoints should have the
 * given step number.
 *
 * The function chooses which neighbour will be routed through.
 *
 * If padrandomroutes is false, the first neighbour with the correct step is used.
 * If padrandomroutes is true, the algorithm has a 50% chance of searching for
 * another neighbour.
 *
 * padrandomroutes is always true.
 */
struct waypoint *waypointChooseNeighbour(s32 *pointnums, s32 step, s32 groupnum, u32 ignoremask)
{
	struct waypoint *points = g_StageSetup.waypoints;
	struct waypoint *best = NULL;

	for (;;) {
		s32 seg;
		seg = pd_load_s32_unaligned(pointnums);
		if (seg < 0) break;

		if ((seg & ignoremask) == 0) {
			struct waypoint *point = &points[WPSEG_GET_ID(seg)];
			s32 grpnum_tmp, step_tmp;
			grpnum_tmp = pd_load_s32_unaligned((const u8 *)point + offsetof(struct waypoint, groupnum));
			step_tmp = pd_load_s32_unaligned((const u8 *)point + offsetof(struct waypoint, step));

			if (grpnum_tmp == groupnum && step_tmp == step) {
				best = point;

				if (!g_Vars.padrandomroutes) {
					break;
				}

				if (!g_NavSeed[0] && !g_NavSeed[1]) {
					if (rngRandom() % 2 == 0) {
						break;
					}
				} else {
					u64 seed = ((u64)g_NavSeed[0] << 32) | g_NavSeed[1];
					if (rngRotateSeed(&seed) % 2 == 0) {
						break;
					}
				}
			}
		}

		pointnums++;
	}

	return best;
}

/**
 * Iterate the given pointnums and set their step if they don't already have one.
 */
void waypointSetStepIfUndiscovered(s32 *pointnums, s32 value, s32 groupnum, u32 ignoremask)
{
	struct waypoint *waypoints = g_StageSetup.waypoints;

	for (;;) {
    s32 seg;
    seg = pd_load_s32_unaligned(pointnums);
    if (seg < 0) break;

    if ((seg & ignoremask) == 0) {
        struct waypoint *waypoint = &waypoints[WPSEG_GET_ID(seg)];
        {
            s32 grp, stp;
            grp = pd_load_s32_unaligned((const u8 *)waypoint + offsetof(struct waypoint, groupnum));
            stp = pd_load_s32_unaligned((const u8 *)waypoint + offsetof(struct waypoint, step));
            if (grp == groupnum && stp < 0) {
                pd_store_s32_unaligned((u8 *)waypoint + offsetof(struct waypoint, step), value);
            }
        }
    }

    pointnums++;
	}
}

/**
 * Scan the waypoints in the given list, finding ones at the given step.
 * Set their neighbours to step + 1 if they haven't been discovered yet.
 */
bool waypointDiscoverOneStep(s32 *pointnums, s32 step, s32 groupnum, u32 ignoremask)
{
	bool result = false;
	struct waypoint *points = g_StageSetup.waypoints;

for (;;) {
    s32 seg;
    seg = pd_load_s32_unaligned(pointnums);
    if (seg < 0) break;

    struct waypoint *point = &points[seg];

    {
        s32 pstep;
        void *pneigh;
        pstep = pd_load_s32_unaligned((const u8 *)point + offsetof(struct waypoint, step));
        pneigh = (s32 *)(uintptr_t)pd_load_uptr_unaligned((const u8 *)point + offsetof(struct waypoint, neighbours));
        if (step == pstep && pneigh) {
            result = true;
            waypointSetStepIfUndiscovered((s32 *)pneigh, step + 1, groupnum, ignoremask);
        }
    }

    pointnums++;
}

	return result;
}

/**
 * Figure out every point's step within the waygroup.
 *
 * The from and to points MUST be in the same waygroup.
 *
 * It does this by resetting all steps to -1, then setting the from point's step
 * to 0, then repeatedly iterating the group's waypoints and expanding the step
 * each time.
 *
 * If discoverall is false, the discovery stops once the to point is discovered.
 * If discoverall is true, points beyond the to point are also discovered.
 */
void waypointDiscoverSteps(struct waypoint *from, struct waypoint *to, bool discoverall, u32 ignoremask)
{
	struct waygroup *groups = g_StageSetup.waygroups;
	struct waypoint *points = g_StageSetup.waypoints;
	struct waypoint *point;
	s32 from_groupnum;
	from_groupnum = pd_load_s32_unaligned((const u8 *)from + offsetof(struct waypoint, groupnum));
	s32 *pointnums; {
		void *wps_ptr;
		wps_ptr = (s32 *)(uintptr_t)pd_load_uptr_unaligned((const u8 *)&groups[from_groupnum] + offsetof(struct waygroup, waypoints));
		pointnums = (s32 *)wps_ptr;
	}
	s32 i;
	bool more;

	for (;;) {
		s32 idx;
		idx = pd_load_s32_unaligned(pointnums);
		if (idx < 0) break;
		point = &points[idx];
		{
			s32 neg1 = -1;
			pd_store_s32_unaligned((u8 *)point + offsetof(struct waypoint, step), neg1);
		}
		pointnums++;
	}

	{
	    s32 zero = 0;
	    pd_store_s32_unaligned((u8 *)from + offsetof(struct waypoint, step), zero);
	}

	more = true;

	for (i = 0; ; i++) {
		if (!discoverall) {
			s32 to_step_val;
			to_step_val = pd_load_s32_unaligned((const u8 *)to + offsetof(struct waypoint, step));
			if (to_step_val >= 0) {
				break;
			}
		}
		if (!more) {
			break;
		}
		{
			void *wps_ptr2;
			wps_ptr2 = (s32 *)(uintptr_t)pd_load_uptr_unaligned((const u8 *)&groups[from_groupnum] + offsetof(struct waygroup, waypoints));
			more = waypointDiscoverOneStep((s32 *)wps_ptr2, i, from_groupnum, ignoremask);
		}
	}
}

/**
 * Find a route at the waypoint level between from and to.
 *
 * The steps along the chosen route will have their step numbers set >= 10000.
 *
 * The from and to points should be in the same waygroup.
 */
void waypointFindRoute(struct waypoint *from, struct waypoint *to)
{
	struct waypoint *curto;
	s32 value;

	waypointDiscoverSteps(from, to, false, IGNORE_INWARDS);

	{
	    s32 to_step;
	    to_step = pd_load_s32_unaligned((const u8 *)to + offsetof(struct waypoint, step));
	    value = to_step - 1;
	}
	curto = to;

	while (value >= 0) {
		{
    s32 s;
    s = pd_load_s32_unaligned((const u8 *)curto + offsetof(struct waypoint, step));
    s += 10000;
    pd_store_s32_unaligned((u8 *)curto + offsetof(struct waypoint, step), s);
	}
		{
		    void *neigh;
		    s32 from_groupnum;
		    neigh = (s32 *)(uintptr_t)pd_load_uptr_unaligned((const u8 *)curto + offsetof(struct waypoint, neighbours));
		    from_groupnum = pd_load_s32_unaligned((const u8 *)from + offsetof(struct waypoint, groupnum));
		    curto = waypointChooseNeighbour((s32 *)neigh, value, from_groupnum, IGNORE_OUTWARDS);
		}

		value--;
	}

	{
    s32 s;
    s = pd_load_s32_unaligned((const u8 *)curto + offsetof(struct waypoint, step));
    s += 10000;
    pd_store_s32_unaligned((u8 *)curto + offsetof(struct waypoint, step), s);
	}
}

/**
 * Find the route between the from and to waypoints and write their pointers to
 * the supplied array.
 *
 * The from and to points should be in the same waygroup.
 */
s32 waypointCollectLocal(struct waypoint *from, struct waypoint *to, struct waypoint **arr, s32 arrlen)
{
	struct waypoint **arrptr = arr;
	struct waypoint *curfrom;
	s32 step;

	if (arrlen >= 2) {
		waypointFindRoute(from, to);

		*arr = from;
		arrptr++;

		curfrom = from;
	arrlen += 9999;
	step = 10001;

	s32 to_step;
	to_step = pd_load_s32_unaligned((const u8 *)to + offsetof(struct waypoint, step));
	while (step <= to_step && step < arrlen) {
   {
       void *neigh;
       s32 from_groupnum;
       neigh = (s32 *)(uintptr_t)pd_load_uptr_unaligned((const u8 *)curfrom + offsetof(struct waypoint, neighbours));
       from_groupnum = pd_load_s32_unaligned((const u8 *)from + offsetof(struct waypoint, groupnum));
       curfrom = waypointChooseNeighbour((s32 *)neigh, step, from_groupnum, IGNORE_INWARDS);
   }
    *arrptr = curfrom;
    arrptr++;
    step++;
	}
	}

	*arrptr = NULL;
	arrptr++;

	return arrptr - arr;
}

/**
 * Given two neighbouring waygroups, find the path segment that connects the two
 * groups. Write pointers to those two waypoints to **frompoint and **topoint.
 *
 * If there are multiple paths between the two waygroups, choose one at random.
 */
void waypointFindSegmentIntoGroup(struct waygroup *fromgroup, struct waygroup *togroup, struct waypoint **frompoint, struct waypoint **topoint)
{
	struct waypoint *points = g_StageSetup.waypoints;
	struct waygroup *groups = g_StageSetup.waygroups;
	void *wps_ptr;
	wps_ptr = (s32 *)(uintptr_t)pd_load_uptr_unaligned((const u8 *)fromgroup + offsetof(struct waygroup, waypoints));
	s32 *fromwpptr = (s32 *)wps_ptr;
	s32 stack;

	*topoint = NULL;
	*frompoint = NULL;

	for (;;) {
    s32 fromidx; fromidx = pd_load_s32_unaligned(fromwpptr);
    if (fromidx < 0) break;

    struct waypoint *fromwp = &points[fromidx];
    void *neigh_ptr;
    neigh_ptr = (s32 *)(uintptr_t)pd_load_uptr_unaligned((const u8 *)fromwp + offsetof(struct waypoint, neighbours));
    s32 *neighbournums = (s32 *)neigh_ptr;

		for (;;) {
    	s32 seg; seg = pd_load_s32_unaligned(neighbournums);
    	if (seg < 0) break;

    	if ((seg & IGNORE_INWARDS) == 0) {
        struct waypoint *neighbour = &points[WPSEG_GET_ID(seg)];

				              s32 ng;
              		ng = pd_load_s32_unaligned((const u8 *)neighbour + offsetof(struct waypoint, groupnum));
              		if (togroup == &groups[ng]) {
					*frompoint = fromwp;
					*topoint = neighbour;

					if (!g_Vars.padrandomroutes) {
						break;
					}

					if (!g_NavSeed[0] && !g_NavSeed[1]) {
						if (rngRandom() % 2 == 0) {
							break;
						}
					} else {
						u64 seed = ((u64)g_NavSeed[0] << 32) | g_NavSeed[1];

						if ((rngRotateSeed(&seed) % 2) == 0) {
							break;
						}
					}
				}
			}

			neighbournums++;
		}

		fromwpptr++;
	}
}

/**
 * Find a route from frompoint to topoint. The arr argument will be populated
 * with pointers to the route's waypoints. If arr is not big enough then only
 * the first part of the route will be populated into the array.
 *
 * The return value is the number of elements populated into the array.
 */
s32 navFindRoute(struct waypoint *frompoint, struct waypoint *topoint, struct waypoint **arr, s32 arrlen)
{
    struct waypoint **arrptr = arr;
    struct waygroup *groups = g_StageSetup.waygroups;

    if (groups && frompoint && topoint) {
        /* PSP-safe: frompoint/topoint may be unaligned; read groupnum via memcpy */
        s32 fromgroupnum, togroupnum;
        fromgroupnum = pd_load_s32_unaligned((const u8 *)frompoint + offsetof(struct waypoint, groupnum));
        togroupnum = pd_load_s32_unaligned((const u8 *)topoint + offsetof(struct waypoint, groupnum));

        struct waygroup *fromgroup = &groups[fromgroupnum];
        struct waygroup *togroup   = &groups[togroupnum];

        if (waygroupFindRoute(fromgroup, togroup, groups)) {
            struct waypoint *curfrompoint = frompoint;
            struct waygroup *curfromgroup = fromgroup;
            s32 step;

            s32 from_step, to_step;
            from_step = pd_load_s32_unaligned((const u8 *)fromgroup + offsetof(struct waygroup, step));
            to_step = pd_load_s32_unaligned((const u8 *)togroup + offsetof(struct waygroup, step));
            for (step = from_step + 1; step <= to_step && arrlen >= 2; step++) {
                struct waygroup *nextfromgroup;
                struct waypoint *curgrouplastwp;
                struct waypoint *nextgroupfirstwp;
                s32 numwritten;
                {
                    void *neigh;
                    neigh = (s32 *)(uintptr_t)pd_load_uptr_unaligned((const u8 *)curfromgroup + offsetof(struct waygroup, neighbours));
                    nextfromgroup = waygroupChooseNeighbour((s32 *)neigh, step, IGNORE_INWARDS);
                }

                waypointFindSegmentIntoGroup(curfromgroup, nextfromgroup, &curgrouplastwp, &nextgroupfirstwp);
                numwritten = waypointCollectLocal(curfrompoint, curgrouplastwp, arrptr, arrlen) - 1;

                arrlen -= numwritten;
                arrptr += numwritten;

                curfrompoint = nextgroupfirstwp;
                curfromgroup = nextfromgroup;
            }

            arrptr += waypointCollectLocal(curfrompoint, topoint, arrptr, arrlen) - 1;
        }
    }

    *arrptr = NULL;
    arrptr++;

    return arrptr - arr;
}

void waypointResetAllSteps(void)
{
struct waypoint *waypoint = g_StageSetup.waypoints;
for (;;) {
    s32 padnum;
    padnum = pd_load_s32_unaligned((const u8 *)waypoint + offsetof(struct waypoint, padnum));
    if (padnum < 0) break;
    {
        s32 neg1 = -1;
        pd_store_s32_unaligned((u8 *)waypoint + offsetof(struct waypoint, step), neg1);
    }
    waypoint++;
}
}

struct waypoint *waypointFindRandomAtStep(s32 *pointnums, s32 step)
{
	s32 len = 0;
	s32 randomindex;
	s32 i;

for (;;) {
    s32 seg;
    seg = pd_load_s32_unaligned(pointnums + len);
    if (seg < 0) break;
    len++;
}

	randomindex = rngRandom() % len;

	for (i = randomindex; i < len; i++) {
		s32 seg; seg = pd_load_s32_unaligned(pointnums + i);
		struct waypoint *point = &g_StageSetup.waypoints[WPSEG_GET_ID(seg)];
		if (pd_load_s32_unaligned((const u8 *)point + offsetof(struct waypoint, step)) == step) {
			return point;
		}
	}

	for (i = 0; i < randomindex; i++) {
		s32 seg; seg = pd_load_s32_unaligned(pointnums + i);
		struct waypoint *point = &g_StageSetup.waypoints[WPSEG_GET_ID(seg)];

		if (pd_load_s32_unaligned((const u8 *)point + offsetof(struct waypoint, step)) == step) {
			return point;
		}
	}

	return NULL;
}

struct waygroup *waygroupFindRandomAtStep(s32 *groupnums, s32 step)
{
	s32 len = 0;
	s32 randomindex;
	s32 i;

	for (;;) {
    s32 seg; seg = pd_load_s32_unaligned(groupnums + len);
    if (seg < 0) break;
    len++;
	}

	randomindex = rngRandom() % len;

	for (i = randomindex; i < len; i++) {
		s32 seg; seg = pd_load_s32_unaligned(groupnums + i);
		struct waygroup *group = &g_StageSetup.waygroups[WPSEG_GET_ID(seg)];

		if (pd_load_s32_unaligned((const u8 *)group + offsetof(struct waygroup, step)) == step) {
			return group;
		}
	}

	for (i = 0; i < randomindex; i++) {
		s32 seg; seg = pd_load_s32_unaligned(groupnums + i);
		struct waygroup *group = &g_StageSetup.waygroups[WPSEG_GET_ID(seg)];

		if (pd_load_s32_unaligned((const u8 *)group + offsetof(struct waygroup, step)) == step) {
			return group;
		}
	}

	return NULL;
}

/**
 * Try to find a waypoint not on the route towards the target, and return it.
 */
struct waypoint *navChooseRetreatPoint(struct waypoint *chrpoint, struct waypoint *tarpoint)
{
	if (g_StageSetup.waygroups) {
		s32 chr_groupnum, tar_groupnum;
		chr_groupnum = pd_load_s32_unaligned((const u8 *)chrpoint + offsetof(struct waypoint, groupnum));
		tar_groupnum = pd_load_s32_unaligned((const u8 *)tarpoint + offsetof(struct waypoint, groupnum));
		struct waygroup *chrgroup = &g_StageSetup.waygroups[chr_groupnum];
		struct waygroup *targroup = &g_StageSetup.waygroups[tar_groupnum];
		struct waypoint *result;
		s32 stack;

		if (chrgroup == targroup) {
			waypointResetAllSteps();

			// Mark steps from target to chr
			waypointDiscoverSteps(tarpoint, chrpoint, true, IGNORE_NONE);

			// If the chr has a neighbouring waypoint into another group (room), select it
			result = waypointFindRandomAtStep(
					(s32 *)(uintptr_t)pd_load_uptr_unaligned((const u8 *)chrpoint + offsetof(struct waypoint, neighbours)),
					-1);

			if (result) {
				return result;
			}

			// Otherwise, choose a waypoint not between the two points
			s32 chr_step; chr_step = pd_load_s32_unaligned((const u8 *)chrpoint + offsetof(struct waypoint, step));
			result = waypointFindRandomAtStep(
					(s32 *)(uintptr_t)pd_load_uptr_unaligned((const u8 *)chrpoint + offsetof(struct waypoint, neighbours)),
					chr_step + 1);

			if (result) {
				return result;
			}
		} else {
			waygroupDiscoverSteps(targroup, chrgroup, g_StageSetup.waygroups, false, IGNORE_INWARDS);

			s32 chrg_step; chrg_step = pd_load_s32_unaligned((const u8 *)chrgroup + offsetof(struct waygroup, step));
			if (chrg_step >= 0) {
				// Find a neighbouring group not in the route to target
				struct waygroup *safetygroup = waygroupFindRandomAtStep(
						(s32 *)(uintptr_t)pd_load_uptr_unaligned((const u8 *)chrgroup + offsetof(struct waygroup, neighbours)),
						-1);

				if (safetygroup) {
					struct waypoint *segfrompoint;
					struct waypoint *segtopoint;
					struct waypoint *route[3];

					waypointFindSegmentIntoGroup(chrgroup, safetygroup, &segfrompoint, &segtopoint);

					// Return the entry waypoint in safetygroup
					if (segfrompoint == chrpoint) {
						return segtopoint;
					}

					// Return first waypoint towards safetygroup
					if (waypointCollectLocal(chrpoint, segfrompoint, route, 3) >= 3) {
						return route[1];
					}
				} else {
					// There are no waygroups outside of the route between the two points.
					// ie. The chr and target are at opposite ends of the level, and the level is mostly linear.

					// Choose a group one step closer to the target
					s32 chrg_step2; chrg_step2 = pd_load_s32_unaligned((const u8 *)chrgroup + offsetof(struct waygroup, step));
					struct waygroup *safetygroup = waygroupChooseNeighbour(
							(s32 *)(uintptr_t)pd_load_uptr_unaligned((const u8 *)chrgroup + offsetof(struct waygroup, neighbours)),
							chrg_step2 - 1,
							IGNORE_INWARDS);

					if (safetygroup) {
						struct waypoint *segfrompoint;
						struct waypoint *segtopoint;

						waypointFindSegmentIntoGroup(chrgroup, safetygroup, &segfrompoint, &segtopoint);
						waypointDiscoverSteps(segfrompoint, chrpoint, true, IGNORE_NONE);

						// Return first waypoint towards safetygroup
						s32 chr_step2, chr_groupnum2;
						chr_step2 = pd_load_s32_unaligned((const u8 *)chrpoint + offsetof(struct waypoint, step));
						chr_groupnum2 = pd_load_s32_unaligned((const u8 *)chrpoint + offsetof(struct waypoint, groupnum));
						result = waypointChooseNeighbour(
								(s32 *)(uintptr_t)pd_load_uptr_unaligned((const u8 *)chrpoint + offsetof(struct waypoint, neighbours)),
								chr_step2 + 1,
								chr_groupnum2,
								IGNORE_INWARDS);

						if (result) {
							return result;
						}
					}
				}
			}
		}
	}

	return NULL;
}

/**
 * Disable the segment from A to B.
 *
 * This works by removing B from A's neighbour list. If B isn't a neighbour of
 * A (ie. segment is already disabled) then no operation is performed.
 *
 * Once B is removed from A's list, the function then updates the group
 * neighbours too. If the segment being removed is the last link between
 * A's group and B's group then group B is removed from group A's neighbour
 * list.
 */
void navDisableSegmentInDirection(struct waypoint *a, struct waypoint *b)
{
	s32 agroupnum = pd_load_s32_unaligned((const u8 *)a + offsetof(struct waypoint, groupnum));
	struct waygroup *agroup = &g_StageSetup.waygroups[agroupnum];
	s32 *aneighbours = (s32 *)(uintptr_t)pd_load_uptr_unaligned((const u8 *)a + offsetof(struct waypoint, neighbours));
	s32 *agroup_waypoints = (s32 *)(uintptr_t)pd_load_uptr_unaligned((const u8 *)agroup + offsetof(struct waygroup, waypoints));
	s32 *agroup_neighbours = (s32 *)(uintptr_t)pd_load_uptr_unaligned((const u8 *)agroup + offsetof(struct waygroup, neighbours));
	s32 bindex = b - g_StageSetup.waypoints;
	s32 bgroupnum = pd_load_s32_unaligned((const u8 *)b + offsetof(struct waypoint, groupnum));
	bool foundlink = false;
	s32 i;
	s32 j;
	s32 tmp;

	// Find index of the neighbour point to remove, or index of end if not found
	for (i = 0; (tmp = pd_load_s32_unaligned(aneighbours + i)) >= 0 && WPSEG_GET_ID(tmp) != bindex; i++);

	// If neighbour was found, shuffle the rest of the neighbour list back by
	// one, effectively removing it.
	if (WPSEG_GET_ID(tmp) == bindex) {
		for (; pd_load_s32_unaligned(aneighbours + i) >= 0; i++) {
			pd_store_s32_unaligned(aneighbours + i, pd_load_s32_unaligned(aneighbours + i + 1));
		}
	}

	// Check if group A still contains any waypoints who have neighbours in
	// group B.
	for (i = 0; (tmp = pd_load_s32_unaligned(agroup_waypoints + i)) >= 0 && !foundlink; i++) {
		struct waypoint *apoint = &g_StageSetup.waypoints[tmp];
		s32 *apoint_neighbours = (s32 *)(uintptr_t)pd_load_uptr_unaligned((const u8 *)apoint + offsetof(struct waypoint, neighbours));

		for (j = 0; (tmp = pd_load_s32_unaligned(apoint_neighbours + j)) >= 0 && !foundlink; j++) {
			struct waypoint *neighbour = &g_StageSetup.waypoints[WPSEG_GET_ID(tmp)];

			if (pd_load_s32_unaligned((const u8 *)neighbour + offsetof(struct waypoint, groupnum)) == bgroupnum) {
				foundlink = true;
			}
		}
	}

	// If no link was found, remove group B from group A's neighbour list
	if (!foundlink) {
		for (i = 0; (tmp = pd_load_s32_unaligned(agroup_neighbours + i)) >= 0 && WPSEG_GET_ID(tmp) != bgroupnum; i++);

		if (WPSEG_GET_ID(tmp) == bgroupnum) {
			for (; pd_load_s32_unaligned(agroup_neighbours + i) >= 0; i++) {
				pd_store_s32_unaligned(agroup_neighbours + i, pd_load_s32_unaligned(agroup_neighbours + i + 1));
			}
		}
	}
}

/**
 * Enable the segment from A to B.
 *
 * This works by adding B to A's neighbour list. If B is already a neighbour of
 * A (ie. segment is already enabled) then no operation is performed.
 *
 * This code assumes that A's neighbours array is big enough to add the new
 * neighbour, which it will be if B was disabled previously.
 */
void navEnableSegmentInDirection(struct waypoint *a, struct waypoint *b)
{
	s32 agroupnum = pd_load_s32_unaligned((const u8 *)a + offsetof(struct waypoint, groupnum));
	struct waygroup *agroup = &g_StageSetup.waygroups[agroupnum];
	s32 *aneighbours = (s32 *)(uintptr_t)pd_load_uptr_unaligned((const u8 *)a + offsetof(struct waypoint, neighbours));
	s32 *agroup_neighbours = (s32 *)(uintptr_t)pd_load_uptr_unaligned((const u8 *)agroup + offsetof(struct waygroup, neighbours));
	s32 bpointnum = b - g_StageSetup.waypoints;
	s32 bgroupnum = pd_load_s32_unaligned((const u8 *)b + offsetof(struct waypoint, groupnum));
	s32 neighbournum;
	s32 i;

	// Find index in A's neighbour list where B can be added.
	// This will either be at the -1 terminator, or if B already exists in the
	// list then the index of B.
	for (i = 0; (neighbournum = pd_load_s32_unaligned(aneighbours + i)) >= 0 && WPSEG_GET_ID(neighbournum) != bpointnum; i++);

	// Add B to A's neighbour list if it doesn't exist
	if (WPSEG_GET_ID(neighbournum) != bpointnum) {
		pd_store_s32_unaligned(aneighbours + i, bpointnum);
		pd_store_s32_unaligned(aneighbours + i + 1, -1);
	}

	// Now the same for groups. Make sure B's group is a neighbour of A's group.
	for (i = 0; (neighbournum = pd_load_s32_unaligned(agroup_neighbours + i)) >= 0 && WPSEG_GET_ID(neighbournum) != bgroupnum; i++);

	if (bgroupnum != WPSEG_GET_ID(neighbournum)) {
		pd_store_s32_unaligned(agroup_neighbours + i, bgroupnum);
		pd_store_s32_unaligned(agroup_neighbours + i + 1, -1);
	}
}

void navDisableSegment(struct waypoint *a, struct waypoint *b)
{
	navDisableSegmentInDirection(a, b);
	navDisableSegmentInDirection(b, a);
}

void navEnableSegment(struct waypoint *a, struct waypoint *b)
{
	navEnableSegmentInDirection(a, b);
	navEnableSegmentInDirection(b, a);
}
