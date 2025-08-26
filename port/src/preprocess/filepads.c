#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "preprocess/common.h"

#include "constants.h"

struct n64_header {
	s32 num_pads;
	s32 num_covers;
	u32 ptr_waypoints;
	u32 ptr_waygroups;
	u32 ptr_cover;
};

struct host_header {
	s32 num_pads;
	s32 num_covers;
	uintptr_t ptr_waypoints;
	uintptr_t ptr_waygroups;
	uintptr_t ptr_cover;
};

struct padheader {
	u32 flags : 18;
	u32 room : 10;
	u32 liftnum : 4;
};

struct n64_waypoint {
	s32 padnum;
	u32 ptr_neighbours;
	s32 groupnum;
	s32 step;
};

struct n64_waygroup {
	u32 ptr_neighbours;
	u32 ptr_waypoints;
	s32 step;
};

static u32 convertPads(u8 *dst, u32 dstpos, u8 *src, u32 srcpos, int num_pads)
{
	u16 *src_offsets = (u16 *) &src[srcpos];
	u16 *dst_offsets = (u16 *) &dst[dstpos];

	dstpos += num_pads * sizeof(u16);

	for (int i = 0; i < num_pads; i++) {
		srcpos = PD_BE16(src_offsets[i]);

		dst_offsets[i] = (dstpos);

		// Header
		u32 n64_padheader = PD_BE32(*(u32 *)&src[srcpos]);
		struct padheader *host_padheader = (struct padheader *)&dst[dstpos];
		u32 flags = (n64_padheader >> 14) & 0x3ffff;

		// safe unaligned store (no crash on PSP)
		memcpy(host_padheader, &n64_padheader, sizeof(u32));

		srcpos += sizeof(struct padheader);
		dstpos += sizeof(struct padheader);

		// Position
		if (flags & PADFLAG_INTPOS) {
			s16 *srcptr = (s16 *) &src[srcpos];
			s16 *dstptr = (s16 *) &dst[dstpos];

			dstptr[0] = PD_BE16(srcptr[0]);
			dstptr[1] = PD_BE16(srcptr[1]);
			dstptr[2] = PD_BE16(srcptr[2]);

			srcpos += 8;
			dstpos += 8;
		} else {
			u32 *srcptr = (u32 *) &src[srcpos];
			u32 *dstptr = (u32 *) &dst[dstpos];

			dstptr[0] = PD_BE32(srcptr[0]);
			dstptr[1] = PD_BE32(srcptr[1]);
			dstptr[2] = PD_BE32(srcptr[2]);

			srcpos += 12;
			dstpos += 12;
		}

		// Up
		if ((flags & (PADFLAG_UPALIGNTOX | PADFLAG_UPALIGNTOY | PADFLAG_UPALIGNTOZ)) == 0) {
   		 uint32_t tmp;

   		 memcpy(&tmp, &src[srcpos + 0], 4);
    	tmp = PD_BE32(tmp);
    	memcpy(&dst[dstpos + 0], &tmp, 4);

    	memcpy(&tmp, &src[srcpos + 4], 4);
    	tmp = PD_BE32(tmp);
    	memcpy(&dst[dstpos + 4], &tmp, 4);

    	memcpy(&tmp, &src[srcpos + 8], 4);
    	tmp = PD_BE32(tmp);
    	memcpy(&dst[dstpos + 8], &tmp, 4);

    	srcpos += 12;
    	dstpos += 12;
		}

// Look
if ((flags & (PADFLAG_LOOKALIGNTOX | PADFLAG_LOOKALIGNTOY | PADFLAG_LOOKALIGNTOZ)) == 0) {
    uint32_t tmp;

    memcpy(&tmp, &src[srcpos + 0], 4);
    tmp = PD_BE32(tmp);
    memcpy(&dst[dstpos + 0], &tmp, 4);

    memcpy(&tmp, &src[srcpos + 4], 4);
    tmp = PD_BE32(tmp);
    memcpy(&dst[dstpos + 4], &tmp, 4);

    memcpy(&tmp, &src[srcpos + 8], 4);
    tmp = PD_BE32(tmp);
    memcpy(&dst[dstpos + 8], &tmp, 4);

    srcpos += 12;
    dstpos += 12;
}

// Bbox
if (flags & PADFLAG_HASBBOXDATA) {
    uint32_t tmp;

    memcpy(&tmp, &src[srcpos +  0], 4);
    tmp = PD_BE32(tmp);
    memcpy(&dst[dstpos +  0], &tmp, 4);

    memcpy(&tmp, &src[srcpos +  4], 4);
    tmp = PD_BE32(tmp);
    memcpy(&dst[dstpos +  4], &tmp, 4);

    memcpy(&tmp, &src[srcpos +  8], 4);
    tmp = PD_BE32(tmp);
    memcpy(&dst[dstpos +  8], &tmp, 4);

    memcpy(&tmp, &src[srcpos + 12], 4);
    tmp = PD_BE32(tmp);
    memcpy(&dst[dstpos + 12], &tmp, 4);

    memcpy(&tmp, &src[srcpos + 16], 4);
    tmp = PD_BE32(tmp);
    memcpy(&dst[dstpos + 16], &tmp, 4);

    memcpy(&tmp, &src[srcpos + 20], 4);
    tmp = PD_BE32(tmp);
    memcpy(&dst[dstpos + 20], &tmp, 4);

    srcpos += 24;  // 4 * 6
    dstpos += 24;  // 4 * 6
}
	}

	return dstpos;
}

static u32 convertWayPoints(u8 *dst, u32 dstpos, u8 *src, u32 srcpos)
{
    // Count waypoints by scanning padnum until 0xFFFFFFFF (terminator)
    int num_waypoints = 0;
    for (;;) {
        uint32_t pad_be, pad;
        memcpy(&pad_be, &src[srcpos + num_waypoints * sizeof(struct n64_waypoint)
                             + offsetof(struct n64_waypoint, padnum)], 4);
        pad = PD_BE32(pad_be);
        if (pad == 0xFFFFFFFFu) break;
        num_waypoints++;
    }

    // Reserve space for host waypoint table (+1 terminator)
    u32 host_tbl_pos = dstpos;
    dstpos += (num_waypoints + 1) * sizeof(struct waypoint);

    // Where we will write the flattened neighbours arrays
    u8 *neigh_write = &dst[dstpos];

    // Fill host waypoints and copy neighbour lists
    for (int i = 0; i < num_waypoints; i++) {
        size_t sbase = srcpos + i * sizeof(struct n64_waypoint);

        uint32_t pad_be, grp_be, ptr_be;
        uint32_t pad_le, grp_le, ptr_le;

        memcpy(&pad_be, &src[sbase + offsetof(struct n64_waypoint, padnum)], 4);
        memcpy(&grp_be, &src[sbase + offsetof(struct n64_waypoint, groupnum)], 4);
        memcpy(&ptr_be, &src[sbase + offsetof(struct n64_waypoint, ptr_neighbours)], 4);

        pad_le = PD_BE32(pad_be);
        grp_le = PD_BE32(grp_be);
        ptr_le = PD_BE32(ptr_be);

        // Write host waypoint entry (use memcpy to avoid unaligned stores)
        struct waypoint *host_waypoints = (struct waypoint *)&dst[host_tbl_pos];

        memcpy(&host_waypoints[i].padnum, &pad_le, 4);

        void *neiptr = (void *)(uintptr_t)dstpos;  // neighbours field points to current dstpos
        memcpy(&host_waypoints[i].neighbours, &neiptr, sizeof(host_waypoints[i].neighbours));

        memcpy(&host_waypoints[i].groupnum, &grp_le, 4);
        memset(&host_waypoints[i].step, 0, sizeof(host_waypoints[i].step));

        // Copy neighbour list (32-bit entries ending with 0xFFFFFFFF), unaligned-safe
        const u8 *nsrc = &src[ptr_le];
        for (;;) {
            uint32_t nb_be, nb_le;
            memcpy(&nb_be, nsrc, 4);
            nb_le = PD_BE32(nb_be);

            memcpy(neigh_write, &nb_le, 4);
            neigh_write += 4;
            dstpos += 4;
            nsrc += 4;

            if (nb_le == 0xFFFFFFFFu)
                break;
        }
    }

    // Terminator waypoint
    {
        struct waypoint *host_waypoints = (struct waypoint *)&dst[host_tbl_pos];
        uint32_t term = 0xFFFFFFFFu, zero = 0;
        void *nullp = NULL;

        memcpy(&host_waypoints[num_waypoints].padnum, &term, 4);
        memcpy(&host_waypoints[num_waypoints].neighbours, &nullp,
               sizeof(host_waypoints[num_waypoints].neighbours));
        memcpy(&host_waypoints[num_waypoints].groupnum, &zero, 4);
        memset(&host_waypoints[num_waypoints].step, 0,
               sizeof(host_waypoints[num_waypoints].step));
    }

    return dstpos;
}

static u32 convertWayGroups(u8 *dst, u32 dstpos, u8 *src, u32 srcpos)
{
    // Count waygroups until ptr_neighbours == 0 (terminator)
    int num_waygroups = 0;
    for (;;) {
        uint32_t pn_be, pn_le;
        memcpy(&pn_be, &src[srcpos + num_waygroups * sizeof(struct n64_waygroup)
                             + offsetof(struct n64_waygroup, ptr_neighbours)], 4);
        pn_le = PD_BE32(pn_be);
        if (pn_le == 0) break;
        num_waygroups++;
    }

    // Reserve space for host waygroup table (+1 terminator)
    u32 host_tbl_pos = dstpos;
    dstpos += (num_waygroups + 1) * sizeof(struct waygroup);

    // Waygroups and child waypoints
    for (int i = 0; i < num_waygroups; i++) {
        struct waygroup *host_waygroups = (struct waygroup *)&dst[host_tbl_pos];

        // waypoints pointer -> current dstpos
        void *wpp = (void *)(uintptr_t)dstpos;
        memcpy(&host_waygroups[i].waypoints, &wpp, sizeof(host_waygroups[i].waypoints));
        memset(&host_waygroups[i].step, 0, sizeof(host_waygroups[i].step));

        // Read source ptr to waypoints list
        size_t sbase = srcpos + i * sizeof(struct n64_waygroup);
        uint32_t wp_be, wp_le;
        memcpy(&wp_be, &src[sbase + offsetof(struct n64_waygroup, ptr_waypoints)], 4);
        wp_le = PD_BE32(wp_be);

        // Copy waypoint list (u32s) until 0xFFFFFFFF, endian-fixing each
        const u8 *sp = &src[wp_le];
        for (;;) {
            uint32_t v_be, v_le;
            memcpy(&v_be, sp, 4);
            v_le = PD_BE32(v_be);
            memcpy(&dst[dstpos], &v_le, 4);
            dstpos += 4;
            sp += 4;
            if (v_le == 0xFFFFFFFFu) break;
        }
    }

    // Terminator waygroup
    {
        struct waygroup *host_waygroups = (struct waygroup *)&dst[host_tbl_pos];
        void *nullp = NULL;
        memcpy(&host_waygroups[num_waygroups].neighbours, &nullp,
               sizeof(host_waygroups[num_waygroups].neighbours));
        memcpy(&host_waygroups[num_waygroups].waypoints, &nullp,
               sizeof(host_waygroups[num_waygroups].waypoints));
        memset(&host_waygroups[num_waygroups].step, 0,
               sizeof(host_waygroups[num_waygroups].step));
    }

    // Waygroup neighbours
    for (int i = 0; i < num_waygroups; i++) {
        struct waygroup *host_waygroups = (struct waygroup *)&dst[host_tbl_pos];

        // neighbours pointer -> current dstpos
        void *nbp = (void *)(uintptr_t)dstpos;
        memcpy(&host_waygroups[i].neighbours, &nbp, sizeof(host_waygroups[i].neighbours));

        // Read source ptr to neighbours list
        size_t sbase = srcpos + i * sizeof(struct n64_waygroup);
        uint32_t nbptr_be, nbptr_le;
        memcpy(&nbptr_be, &src[sbase + offsetof(struct n64_waygroup, ptr_neighbours)], 4);
        nbptr_le = PD_BE32(nbptr_be);

        // Copy neighbour list (u32s) until 0xFFFFFFFF, endian-fixing each
        const u8 *sp = &src[nbptr_le];
        for (;;) {
            uint32_t v_be, v_le;
            memcpy(&v_be, sp, 4);
            v_le = PD_BE32(v_be);
            memcpy(&dst[dstpos], &v_le, 4);
            dstpos += 4;
            sp += 4;
            if (v_le == 0xFFFFFFFFu) break;
        }
    }

    return dstpos;
}

static u32 convertCover(u8 *dst, u32 dstpos, u8 *src, u32 srcpos, int num_covers)
{
    struct coverdefinition *n64_covers  = (struct coverdefinition *)&src[srcpos];
    struct coverdefinition *host_covers = (struct coverdefinition *)&dst[dstpos];

    for (int i = 0; i < num_covers; i++) {
        uint32_t v32;

        // pos
        memcpy(&v32, &n64_covers[i].pos, sizeof(v32));
        v32 = PD_SWAPPED_VAL(v32);
        memcpy(&host_covers[i].pos, &v32, sizeof(v32));

        // look
        memcpy(&v32, &n64_covers[i].look, sizeof(v32));
        v32 = PD_SWAPPED_VAL(v32);
        memcpy(&host_covers[i].look, &v32, sizeof(v32));

        // flags (16-bit)
        uint16_t v16;
        memcpy(&v16, &n64_covers[i].flags, sizeof(v16));
        v16 = PD_BE16(v16);
        memcpy(&host_covers[i].flags, &v16, sizeof(v16));
    }

    dstpos += (u32)(num_covers * sizeof(struct coverdefinition));
    return dstpos;
}


static u32 convertPadsFile(u8 *dst, u8 *src)
{
    u32 dstpos = 0;

    struct n64_header *n64_header  = (struct n64_header *)src;
    struct host_header *host_header = (struct host_header *)&dst[dstpos];

    uint32_t tmp32;

    // num_pads
    memcpy(&tmp32, &n64_header->num_pads, 4);
    uint32_t num_pads = PD_BE32(tmp32);
    memcpy(&host_header->num_pads, &num_pads, 4);

    // num_covers
    memcpy(&tmp32, &n64_header->num_covers, 4);
    uint32_t num_covers = PD_BE32(tmp32);
    memcpy(&host_header->num_covers, &num_covers, 4);

    dstpos += sizeof(struct host_header);

    // Pads
    dstpos = convertPads(dst, dstpos, src, sizeof(struct n64_header), num_pads);

    // Waypoints
    memcpy(&tmp32, &n64_header->ptr_waypoints, 4);
    tmp32 = PD_BE32(tmp32);
    memcpy(&host_header->ptr_waypoints, &dstpos, 4);
    dstpos = convertWayPoints(dst, dstpos, src, tmp32);

    // Waygroups
    memcpy(&tmp32, &n64_header->ptr_waygroups, 4);
    tmp32 = PD_BE32(tmp32);
    memcpy(&host_header->ptr_waygroups, &dstpos, 4);
    dstpos = convertWayGroups(dst, dstpos, src, tmp32);

    // Cover
    memcpy(&tmp32, &n64_header->ptr_cover, 4);
    tmp32 = PD_BE32(tmp32);
    memcpy(&host_header->ptr_cover, &dstpos, 4);
    dstpos = convertCover(dst, dstpos, src, tmp32, num_covers);

    return dstpos;
}

u8* preprocessPadsFile(u8 *data, u32 size, u32 *outSize)
{
    u32 newSizeEstimated = romdataFileGetEstimatedSize(size, LOADTYPE_PADS);
    u8 *dst = sysMemZeroAlloc(newSizeEstimated);

    u32 newSize = convertPadsFile(dst, data);

    if (newSize > newSizeEstimated) {
        sysFatalError("overflow when trying to preprocess a pads file, size %d newsize %d", size, newSize);
    }

    memcpy(data, dst, newSize);
    sysMemFree(dst);

    *outSize = newSize;
    return 0;  // (kept same return contract as your original)
}