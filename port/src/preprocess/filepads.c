#include <stdio.h>
#include <stdlib.h>

#include "preprocess/common.h"

#include "constants.h"
#include "unaligned.h"

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

static inline void copy_be32_words_to_host(u8 *dst, const u8 *src, int count)
{
	for (int i = 0; i < count; i++) {
		u32 value = pd_load_be32_unaligned(src + i * 4);
		pd_store_u32_unaligned(dst + i * 4, value);
	}
}

static u32 convertPads(u8 *dst, u32 dstpos, u8 *src, u32 srcpos, int num_pads)
{
	u32 src_offsets_pos = srcpos;
	u32 dst_offsets_pos = dstpos;

	dstpos += (u32)num_pads * sizeof(u16);

	for (int i = 0; i < num_pads; i++) {
		srcpos = pd_load_be16_unaligned(&src[src_offsets_pos + (u32)i * sizeof(u16)]);

		pd_store_u16_unaligned(&dst[dst_offsets_pos + (u32)i * sizeof(u16)], (u16)dstpos);

		// Header
		u32 n64_padheader = pd_load_be32_unaligned(&src[srcpos]);
		u32 flags = (n64_padheader >> 14) & 0x3ffffu;

		pd_store_u32_unaligned(&dst[dstpos], n64_padheader);

		srcpos += sizeof(struct padheader);
		dstpos += sizeof(struct padheader);

		// Position
		if (flags & PADFLAG_INTPOS) {
			for (int j = 0; j < 3; j++) {
				u16 value = pd_load_be16_unaligned(&src[srcpos + j * 2]);
				pd_store_u16_unaligned(&dst[dstpos + j * 2], value);
			}

			srcpos += 8;
			dstpos += 8;
		} else {
			copy_be32_words_to_host(&dst[dstpos], &src[srcpos], 3);

			srcpos += 12;
			dstpos += 12;
		}

		// Up
		if ((flags & (PADFLAG_UPALIGNTOX | PADFLAG_UPALIGNTOY | PADFLAG_UPALIGNTOZ)) == 0) {
			copy_be32_words_to_host(&dst[dstpos], &src[srcpos], 3);
			srcpos += 12;
			dstpos += 12;
		}

		// Look
		if ((flags & (PADFLAG_LOOKALIGNTOX | PADFLAG_LOOKALIGNTOY | PADFLAG_LOOKALIGNTOZ)) == 0) {
			copy_be32_words_to_host(&dst[dstpos], &src[srcpos], 3);
			srcpos += 12;
			dstpos += 12;
		}

		// Bbox
		if (flags & PADFLAG_HASBBOXDATA) {
			copy_be32_words_to_host(&dst[dstpos], &src[srcpos], 6);
			srcpos += 24; // 4 * 6
			dstpos += 24; // 4 * 6
		}
	}

	return dstpos;
}

static u32 convertWayPoints(u8 *dst, u32 dstpos, u8 *src, u32 srcpos)
{
	// Count waypoints by scanning padnum until 0xFFFFFFFF (terminator)
	int num_waypoints = 0;

	for (;;) {
		size_t sbase = srcpos + (size_t)num_waypoints * sizeof(struct n64_waypoint);
		u32 pad = pd_load_be32_unaligned(&src[sbase + offsetof(struct n64_waypoint, padnum)]);

		if (pad == 0xffffffffu) {
			break;
		}

		num_waypoints++;
	}

	// Reserve space for host waypoint table (+1 terminator)
	u32 host_tbl_pos = dstpos;
	dstpos += (u32)(num_waypoints + 1) * sizeof(struct waypoint);

	// Where we will write the flattened neighbours arrays
	u8 *neigh_write = &dst[dstpos];

	// Fill host waypoints and copy neighbour lists
	for (int i = 0; i < num_waypoints; i++) {
		size_t sbase = srcpos + (size_t)i * sizeof(struct n64_waypoint);
		u8 *entry = &dst[host_tbl_pos + (u32)i * sizeof(struct waypoint)];
		u32 pad_le = pd_load_be32_unaligned(&src[sbase + offsetof(struct n64_waypoint, padnum)]);
		u32 grp_le = pd_load_be32_unaligned(&src[sbase + offsetof(struct n64_waypoint, groupnum)]);
		u32 ptr_le = pd_load_be32_unaligned(&src[sbase + offsetof(struct n64_waypoint, ptr_neighbours)]);

		pd_store_s32_unaligned(entry + offsetof(struct waypoint, padnum), (s32)pad_le);
		pd_store_uptr_unaligned(entry + offsetof(struct waypoint, neighbours), (uintptr_t)dstpos);
		pd_store_s32_unaligned(entry + offsetof(struct waypoint, groupnum), (s32)grp_le);
		pd_store_s32_unaligned(entry + offsetof(struct waypoint, step), 0);

		// Copy neighbour list (32-bit entries ending with 0xFFFFFFFF), unaligned-safe
		const u8 *nsrc = &src[ptr_le];

		for (;;) {
			u32 nb_le = pd_load_be32_unaligned(nsrc);

			pd_store_u32_unaligned(neigh_write, nb_le);
			neigh_write += 4;
			dstpos += 4;
			nsrc += 4;

			if (nb_le == 0xffffffffu) {
				break;
			}
		}
	}

	// Terminator waypoint
	{
		u8 *entry = &dst[host_tbl_pos + (u32)num_waypoints * sizeof(struct waypoint)];

		pd_store_u32_unaligned(entry + offsetof(struct waypoint, padnum), 0xffffffffu);
		pd_store_uptr_unaligned(entry + offsetof(struct waypoint, neighbours), 0);
		pd_store_s32_unaligned(entry + offsetof(struct waypoint, groupnum), 0);
		pd_store_s32_unaligned(entry + offsetof(struct waypoint, step), 0);
	}

	return dstpos;
}

static u32 convertWayGroups(u8 *dst, u32 dstpos, u8 *src, u32 srcpos)
{
	// Count waygroups until ptr_neighbours == 0 (terminator)
	int num_waygroups = 0;

	for (;;) {
		size_t sbase = srcpos + (size_t)num_waygroups * sizeof(struct n64_waygroup);
		u32 pn_le = pd_load_be32_unaligned(&src[sbase + offsetof(struct n64_waygroup, ptr_neighbours)]);

		if (pn_le == 0) {
			break;
		}

		num_waygroups++;
	}

	// Reserve space for host waygroup table (+1 terminator)
	u32 host_tbl_pos = dstpos;
	dstpos += (u32)(num_waygroups + 1) * sizeof(struct waygroup);

	// Waygroups and child waypoints
	for (int i = 0; i < num_waygroups; i++) {
		size_t sbase = srcpos + (size_t)i * sizeof(struct n64_waygroup);
		u8 *entry = &dst[host_tbl_pos + (u32)i * sizeof(struct waygroup)];
		u32 wp_le = pd_load_be32_unaligned(&src[sbase + offsetof(struct n64_waygroup, ptr_waypoints)]);

		pd_store_uptr_unaligned(entry + offsetof(struct waygroup, waypoints), (uintptr_t)dstpos);
		pd_store_s32_unaligned(entry + offsetof(struct waygroup, step), 0);

		// Copy waypoint list (u32s) until 0xFFFFFFFF, endian-fixing each
		const u8 *sp = &src[wp_le];

		for (;;) {
			u32 value = pd_load_be32_unaligned(sp);

			pd_store_u32_unaligned(&dst[dstpos], value);
			dstpos += 4;
			sp += 4;

			if (value == 0xffffffffu) {
				break;
			}
		}
	}

	// Terminator waygroup
	{
		u8 *entry = &dst[host_tbl_pos + (u32)num_waygroups * sizeof(struct waygroup)];

		pd_store_uptr_unaligned(entry + offsetof(struct waygroup, neighbours), 0);
		pd_store_uptr_unaligned(entry + offsetof(struct waygroup, waypoints), 0);
		pd_store_s32_unaligned(entry + offsetof(struct waygroup, step), 0);
	}

	// Waygroup neighbours
	for (int i = 0; i < num_waygroups; i++) {
		size_t sbase = srcpos + (size_t)i * sizeof(struct n64_waygroup);
		u8 *entry = &dst[host_tbl_pos + (u32)i * sizeof(struct waygroup)];
		u32 nbptr_le = pd_load_be32_unaligned(&src[sbase + offsetof(struct n64_waygroup, ptr_neighbours)]);

		pd_store_uptr_unaligned(entry + offsetof(struct waygroup, neighbours), (uintptr_t)dstpos);

		// Copy neighbour list (u32s) until 0xFFFFFFFF, endian-fixing each
		const u8 *sp = &src[nbptr_le];

		for (;;) {
			u32 value = pd_load_be32_unaligned(sp);

			pd_store_u32_unaligned(&dst[dstpos], value);
			dstpos += 4;
			sp += 4;

			if (value == 0xffffffffu) {
				break;
			}
		}
	}

	return dstpos;
}

static u32 convertCover(u8 *dst, u32 dstpos, u8 *src, u32 srcpos, int num_covers)
{
	for (int i = 0; i < num_covers; i++) {
		size_t sbase = srcpos + (size_t)i * sizeof(struct coverdefinition);
		size_t dbase = dstpos + (size_t)i * sizeof(struct coverdefinition);
		u16 flags = pd_load_be16_unaligned(&src[sbase + offsetof(struct coverdefinition, flags)]);

		copy_be32_words_to_host(
			&dst[dbase + offsetof(struct coverdefinition, pos)],
			&src[sbase + offsetof(struct coverdefinition, pos)],
			3);

		copy_be32_words_to_host(
			&dst[dbase + offsetof(struct coverdefinition, look)],
			&src[sbase + offsetof(struct coverdefinition, look)],
			3);

		pd_store_u16_unaligned(&dst[dbase + offsetof(struct coverdefinition, flags)], flags);
	}

	dstpos += (u32)(num_covers * sizeof(struct coverdefinition));
	return dstpos;
}


static u32 convertPadsFile(u8 *dst, u8 *src)
{
	u32 dstpos = 0;
	u8 *host_header = &dst[dstpos];
	u32 num_pads = pd_load_be32_unaligned(src + offsetof(struct n64_header, num_pads));
	u32 num_covers = pd_load_be32_unaligned(src + offsetof(struct n64_header, num_covers));
	u32 waypoints_srcpos = pd_load_be32_unaligned(src + offsetof(struct n64_header, ptr_waypoints));
	u32 waygroups_srcpos = pd_load_be32_unaligned(src + offsetof(struct n64_header, ptr_waygroups));
	u32 cover_srcpos = pd_load_be32_unaligned(src + offsetof(struct n64_header, ptr_cover));

	pd_store_s32_unaligned(host_header + offsetof(struct host_header, num_pads), (s32)num_pads);
	pd_store_s32_unaligned(host_header + offsetof(struct host_header, num_covers), (s32)num_covers);

	dstpos += sizeof(struct host_header);

	// Pads
	dstpos = convertPads(dst, dstpos, src, sizeof(struct n64_header), (int)num_pads);

	// Waypoints
	pd_store_uptr_unaligned(host_header + offsetof(struct host_header, ptr_waypoints), (uintptr_t)dstpos);
	dstpos = convertWayPoints(dst, dstpos, src, waypoints_srcpos);

	// Waygroups
	pd_store_uptr_unaligned(host_header + offsetof(struct host_header, ptr_waygroups), (uintptr_t)dstpos);
	dstpos = convertWayGroups(dst, dstpos, src, waygroups_srcpos);

	// Cover
	pd_store_uptr_unaligned(host_header + offsetof(struct host_header, ptr_cover), (uintptr_t)dstpos);
	dstpos = convertCover(dst, dstpos, src, cover_srcpos, (int)num_covers);

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

	pd_copy_bytes_unaligned(data, dst, newSize);
	sysMemFree(dst);

	*outSize = newSize;
	return 0;
}
