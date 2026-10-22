// SPDX-License-Identifier: GPL-2.0-only
/*
 * region.c - the region allocator
 *
 * Two-phase region allocation:
 *   1. open(O_CREAT): region_alloc_rat_entry() reserves a RAT entry (size=0)
 *   2. ftruncate(N):  region_init() finds contiguous space, inits header
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>

#include "access/acl.h"
#include "access/meta_lock.h"
#include "core.h"
#include "layout/layout_access.h"
#include "region/region.h"
#include "test/test_hooks.h"
#include "util/bitmap.h"

/* ============================================================================
 * RAT (Region Allocation Table) management
 * ============================================================================ */

struct region_span *region_alloc_spans(void)
{
	return kmalloc_array(LAYOUT_MAX_RAT_ENTRIES * LAYOUT_MAX_EXTENTS, sizeof(struct region_span), GFP_KERNEL);
}

void region_free_spans(struct region_span *spans)
{
	kfree(spans);
}

s32 region_get_pool_usage(struct fs_sb_info *sbi, struct region_pool_usage *out)
{
	if (!sbi || !out)
		return -EINVAL;

	memset(out, 0, sizeof(*out));
	out->uc_total = layout_get_primary_wb_start(sbi) - sbi->uc_start;
	for (u32 device = 0; device < sbi->device_count; device++)
		out->wb_total += sbi->devices[device].size - sbi->devices[device].wb_start;

	for (u32 index = 0; index < LAYOUT_MAX_RAT_ENTRIES; index++) {
		struct layout_rat_entry *entry = layout_get_rat_entry(sbi, index);
		if (!entry)
			return -EIO;

		union layout_rat_hot_copy hot;
		cxl_get_layout_rat_hot(&hot, &entry->hot);
		/* A region on its way out still holds its extent, so free space must not count it. */
		if (!layout_rat_state_is_standing(hot.local.state) &&
		    hot.local.state != LAYOUT_RAT_ENTRY_DELETING)
			continue;

		out->entries++;

		/* Every piece sits in the first one's pool. The lock region sits in neither. */
		struct region_extent extents[LAYOUT_MAX_EXTENTS];
		const s32 count = region_read_extents(sbi, index, &hot, extents);
		if (count <= 0 ||
		    (extents[0].device_id == FS_PRIMARY_DEVICE && extents[0].offset < sbi->uc_start))
			continue;

		const u64 taken = region_sum_extents(extents, (u32)count);
		if (layout_check_uncached_piece(sbi, extents[0].device_id, extents[0].offset))
			out->uc_used += taken;
		else {
			out->wb_used += taken;
			out->device_wb_used[extents[0].device_id] += taken;
		}
	}

	return 0;
}

static void region_read_map(struct fs_sb_info *sbi, union layout_rat_map_copy *map)
{
	cxl_get_layout_rat_map(map, &layout_get_rat(sbi)->map);
}

static void region_write_map(struct fs_sb_info *sbi, union layout_rat_map_copy *map)
{
	cxl_set_layout_rat_map(&layout_get_rat(sbi)->map, map);
}

/* Write @map as read, with its count at @placements and slot @freed (when below the entry count)
 * marked free. A write spends the copy it is given, so @map stays whole for the next step and
 * the line is read once per operation. */
static void region_write_map_stepped(struct fs_sb_info *sbi, const union layout_rat_map_copy *map, u64 placements,
				     u32 freed)
{
	union layout_rat_map_copy stepped = *map;
	stepped.local.placements = placements;
	if (freed < LAYOUT_MAX_RAT_ENTRIES)
		bits_clear(stepped.local.taken, freed);
	region_write_map(sbi, &stepped);
}

/* The count an extent change steps to as it begins: odd, and past whatever the line read, so a
 * list stamped with the old value stops matching even when that value was odd already. */
static u64 region_step_placements_odd(u64 placements)
{
	return (placements & 1) ? placements + 2 : placements + 1;
}

/* The uncached pool runs from uc_start to the primary's write-back start, and a write-back pool from
 * its device's boundary to the device's end. The uncached pool exists on the primary only. */
static void region_get_pool_bounds(struct fs_sb_info *sbi, u32 device, bool uncached, u64 *start, u64 *end)
{
	const struct fs_device *dev = &sbi->devices[device];
	*start = uncached ? sbi->uc_start : dev->wb_start;
	*end = uncached ? dev->wb_start : dev->size;
}

/* A region's pool is judged off its first piece alone. */
static bool region_check_extents(struct fs_sb_info *sbi, const struct region_extent *extents, u32 count)
{
	const bool uncached = layout_check_uncached_piece(sbi, extents[0].device_id, extents[0].offset);
	for (u32 idx = 0; idx < count; idx++) {
		const struct region_extent *extent = &extents[idx];
		if (extent->device_id >= sbi->device_count)
			return false;
		if (extent->length == 0 || !layout_is_valid_range(sbi, extent->device_id, extent->offset, extent->length))
			return false;
		/* On the primary the metadata is below the first pool. A device is all pool. */
		if (extent->device_id == FS_PRIMARY_DEVICE && extent->offset < sbi->uc_start)
			return false;
		/* A write-back piece starts on the placing node's physical granule boundary, which a peer
		 * may map elsewhere, so its start is held to a page only. */
		if (!IS_ALIGNED(extent->offset, uncached ? sbi->granule : PAGE_SIZE) ||
		    !IS_ALIGNED(extent->length, sbi->granule))
			return false;
		if (layout_check_uncached_piece(sbi, extent->device_id, extent->offset) != uncached)
			return false;
	}
	return true;
}

s32 region_read_extents(struct fs_sb_info *sbi, u32 rat_entry_id, const union layout_rat_hot_copy *hot,
			struct region_extent *out)
{
	const u32 count = hot->local.extent_count;
	if (count == 0)
		return 0;
	if (count > LAYOUT_MAX_EXTENTS)
		return -EIO;

	/* The lock region's place is fixed and off any granule boundary. */
	if (hot->local.region_type == LAYOUT_REGION_LOCK) {
		out[0].offset = LAYOUT_LOCK_REGION_OFFSET;
		out[0].length = LAYOUT_LOCK_REGION_SIZE;
		out[0].device_id = 0;
		return 1;
	}

	struct layout_rat_entry *entry = layout_get_rat_entry(sbi, rat_entry_id);
	if (!entry)
		return -EIO;
	union layout_rat_extent_copy copy;
	cxl_get_layout_rat_extent(&copy, &entry->extents);
	for (u32 piece = 0; piece < count; piece++) {
		out[piece].offset = copy.local.offset[piece];
		out[piece].length = (u64)copy.local.granules[piece] * sbi->granule;
		out[piece].device_id = copy.local.device_id[piece];
	}

	return region_check_extents(sbi, out, count) ? (s32)count : -EIO;
}

struct region_pool {
	struct region_span *spans;
	u32 count;
	u32 device_id; /* the device the pool is on */
	u64 start;
	u64 end;
	u64 granule;
	/* The device's physical base for a write-back pool, where a huge entry needs a physical granule
	 * boundary and the device need not sit on one. Zero for the uncached pool. */
	u64 align_base;
};

/* Sorted by offset, so the gap walk below is one pass. */
static void region_insert_span(struct region_pool *pool, u64 offset, u64 end)
{
	struct region_span *spans = pool->spans;
	u32 at = pool->count;
	while (at > 0 && spans[at - 1].offset > offset) {
		spans[at] = spans[at - 1];
		at--;
	}
	spans[at].offset = offset;
	spans[at].end = end;
	pool->count++;
}

/* Read afresh by every placement, since another node may have placed or freed a region since this
 * node last looked. */
static s32 region_collect_spans(struct fs_sb_info *sbi, u32 device, bool uncached,
				const union layout_rat_map_copy *map, struct region_pool *pool)
{
	const struct fs_device *dev = &sbi->devices[device];
	region_get_pool_bounds(sbi, device, uncached, &pool->start, &pool->end);
	pool->device_id = device;
	pool->granule = sbi->granule;
	pool->align_base = uncached ? 0 : dev->phys_base;
	pool->count = 0;
	const u64 regions_start = pool->start;
	const u64 regions_end = pool->end;

	/* Every entry while the summary is not trusted, since a taken bit a device formatted
	 * before the summary existed never had set would hide a standing extent. */
	static const u64 every_entry[LAYOUT_RAT_MAP_WORDS] = { [0 ... LAYOUT_RAT_MAP_WORDS - 1] = ~0ULL };
	const u64 *taken = sbi->rat_map_trusted ? map->local.taken : every_entry;
	bits_for_each_set(idx, taken, LAYOUT_MAX_RAT_ENTRIES) {
		struct layout_rat_entry *entry = layout_get_rat_entry(sbi, idx);
		if (!entry)
			return -EIO;

		union layout_rat_hot_copy hot;
		cxl_get_layout_rat_hot(&hot, &entry->hot);
		if (!layout_rat_state_is_standing(hot.local.state))
			continue;

		struct region_extent extents[LAYOUT_MAX_EXTENTS];
		const s32 count = region_read_extents(sbi, idx, &hot, extents);
		for (s32 piece = 0; piece < count; piece++) {
			if (extents[piece].device_id != device)
				continue;
			const u64 piece_offset = extents[piece].offset;
			const u64 piece_length = extents[piece].length;
			const u64 device_size = dev->size;
			if (piece_length > device_size || piece_offset > device_size - piece_length)
				continue;
			const u64 piece_end = piece_offset + piece_length;

			/* The other pool's, and no candidate here can reach it. */
			if (piece_end <= regions_start || piece_offset >= regions_end)
				continue;

			region_insert_span(pool, piece_offset, piece_end);
		}
	}
	return 0;
}

/* How many slots @have and @want mark differently. */
static s32 region_count_map_moves(const union layout_rat_map_copy *have, const union layout_rat_map_copy *want)
{
	s32 moves = 0;
	for (u32 word = 0; word < LAYOUT_RAT_MAP_WORDS; word++)
		moves += hweight64(have->local.taken[word] ^ want->local.taken[word]);
	return moves;
}

/* The taken bits as the entries read now, one line read per entry. */
static s32 region_read_taken_bits(struct fs_sb_info *sbi, union layout_rat_map_copy *seen)
{
	memset(seen, 0, sizeof(*seen));
	for (u32 idx = 0; idx < LAYOUT_MAX_RAT_ENTRIES; idx++) {
		struct layout_rat_entry *entry = layout_get_rat_entry(sbi, idx);
		if (!entry)
			return -EIO;

		union layout_rat_hot_copy hot;
		cxl_get_layout_rat_hot(&hot, &entry->hot);
		if (hot.local.state != LAYOUT_RAT_ENTRY_FREE)
			bits_set(seen->local.taken, idx);
	}
	return 0;
}

bool region_map_needs_repair(struct fs_sb_info *sbi, const union layout_rat_map_copy *seen)
{
	if (!layout_get_rat(sbi) || !seen)
		return false;

	union layout_rat_map_copy map;
	region_read_map(sbi, &map);
	return (map.local.placements & 1) != 0 || region_count_map_moves(&map, seen) != 0;
}

void region_check_map(struct fs_sb_info *sbi)
{
	union layout_rat_map_copy seen;
	sbi->rat_map_trusted = layout_get_rat(sbi) && region_read_taken_bits(sbi, &seen) == 0 &&
			       !region_map_needs_repair(sbi, &seen);
}

s32 region_repair_map(struct fs_sb_info *sbi)
{
	if (!layout_get_rat(sbi))
		return -EIO;

	union layout_rat_map_copy want;
	s32 ret = region_read_taken_bits(sbi, &want);
	if (ret)
		return ret;

	union layout_rat_map_copy map;
	region_read_map(sbi, &map);
	s32 moves = region_count_map_moves(&map, &want);
	/* Nothing places while this node holds the turn, so an odd count is a placer that died
	 * between its two steps, and the extent it wrote is in the entries this walk just read. */
	const bool odd = (map.local.placements & 1) != 0;
	if (moves == 0 && !odd) {
		sbi->rat_map_trusted = true;
		return 0;
	}

	want.local.placements = map.local.placements + (odd ? 1 : 0);
	region_write_map(sbi, &want);
	sbi->rat_map_trusted = true;
	return moves + (odd ? 1 : 0);
}

/* The first offset at or past @offset a piece may start at. */
static u64 region_align_candidate(const struct region_pool *pool, u64 offset)
{
	return layout_align_up(pool->align_base + offset, pool->granule) - pool->align_base;
}

/* The last offset at or before @limit a piece may end at. */
static u64 region_align_limit(const struct region_pool *pool, u64 limit)
{
	return ALIGN_DOWN(pool->align_base + limit, pool->granule) - pool->align_base;
}

/* The first gap of @size in the sorted list, or -ENOSPC. */
static s32 region_pick_gap(const struct region_pool *pool, u64 size, u64 *out_offset)
{
	const struct region_span *spans = pool->spans;
	u64 candidate = region_align_candidate(pool, pool->start);
	for (u32 idx = 0; idx < pool->count; idx++) {
		if (candidate + size <= spans[idx].offset) {
			*out_offset = candidate;
			return 0;
		}
		if (spans[idx].end > candidate)
			candidate = region_align_candidate(pool, spans[idx].end);
	}

	if (candidate + size <= pool->end) {
		*out_offset = candidate;
		return 0;
	}

	pr_err("no contiguous space for size %llu in [0x%llx, 0x%llx)\n", size, pool->start, pool->end);
	return -ENOSPC;
}

static bool region_is_gap_taken(u32 device, u64 offset, const struct region_extent *taken, u32 count)
{
	for (u32 idx = 0; idx < count; idx++) {
		if (taken[idx].device_id == device && taken[idx].offset == offset)
			return true;
	}
	return false;
}

/* A zero length when no gap of @need bytes is left. */
static struct region_extent region_find_gap(const struct region_pool *pool, u64 need, bool largest,
					    const struct region_extent *taken, u32 taken_count)
{
	const struct region_span *spans = pool->spans;
	struct region_extent best = { 0 };
	u64 candidate = region_align_candidate(pool, pool->start);
	for (u32 idx = 0; idx <= pool->count; idx++) {
		const u64 limit = idx < pool->count ? spans[idx].offset : pool->end;
		const u64 end = region_align_limit(pool, limit);
		const u64 length = end > candidate ? end - candidate : 0;
		const bool better = !best.length || (largest ? length >= best.length : length <= best.length);
		if (length >= need && length > 0 && better && !region_is_gap_taken(pool->device_id, candidate, taken, taken_count)) {
			best.offset = candidate;
			best.length = length;
			best.device_id = (u16)pool->device_id;
		}
		if (idx < pool->count && spans[idx].end > candidate)
			candidate = region_align_candidate(pool, spans[idx].end);
	}
	return best;
}

static s32 region_fill_gaps(const struct region_pool *pool, u64 size, bool largest_first,
			    struct region_extent *out)
{
	u64 left = size;
	for (u32 count = 0; count < LAYOUT_MAX_EXTENTS; count++) {
		const struct region_extent fit = region_find_gap(pool, left, false, out, count);
		if (fit.length) {
			/* The high end, since a contiguous placement takes the low end of a gap. */
			out[count].offset = fit.offset + fit.length - left;
			out[count].length = left;
			out[count].device_id = (u16)pool->device_id;
			return (s32)count + 1;
		}

		const struct region_extent whole = region_find_gap(pool, 1, largest_first, out, count);
		if (!whole.length || count + 1 == LAYOUT_MAX_EXTENTS)
			return -ENOSPC;
		out[count] = whole;
		left -= whole.length;
	}
	return -ENOSPC;
}

/* Small gaps go first so the large ones stay for contiguous regions. The largest go first only when
 * that runs out of pieces. */
static s32 region_pick_gaps(const struct region_pool *pool, u64 size, struct region_extent *out)
{
	s32 count = region_fill_gaps(pool, size, false, out);
	if (count < 0)
		count = region_fill_gaps(pool, size, true, out);
	if (count < 0)
		pr_err("no space for paged size %llu in %u pieces\n", size, (u32)LAYOUT_MAX_EXTENTS);
	return count;
}

static void region_write_extents(struct fs_sb_info *sbi, struct layout_rat_entry *entry,
				 const struct region_extent *extents, u32 count)
{
	union layout_rat_extent_copy copy;
	memset(&copy.local, 0, sizeof(copy.local));
	for (u32 piece = 0; piece < count; piece++) {
		copy.local.offset[piece] = extents[piece].offset;
		copy.local.granules[piece] = (u32)(extents[piece].length / sbi->granule);
		copy.local.device_id[piece] = extents[piece].device_id;
	}
	cxl_set_layout_rat_extent(&entry->extents, &copy);
}

/*
 * The delegation rows a previous tenant left. Nothing else clears them, and a slot handed to a
 * new file with them alive would hand that file to the old one's delegates.
 */
static void region_clear_deleg_rows(struct layout_rat_entry *entry)
{
	cxl_zero_cachelines(entry->deleg_entries, sizeof(entry->deleg_entries));
}

/*
 * region_fill_rat_entry - the new tenant's values over a slot cleared of the last one's.
 * @entry: RAT entry the caller has already claimed as ALLOCATING
 * @sbi: superblock info
 * @name: filename
 * @name_len: filename length
 *
 * Runs while the slot reads ALLOCATING, so no peer reads what these writes pass through. The name
 * and acl lines are written here, the acl with the calling task as owner and @identity as its group
 * and role; the hot line is only built into @hot, with @mode and the caller's ids, and the caller
 * writes it once with ALLOCATED so the fields land together with the state.
 */
static void region_fill_rat_entry(struct layout_rat_entry *entry, struct fs_sb_info *sbi, const char *name, u32 name_len,
				  umode_t mode, const struct fs_daemon_attest_response *identity,
				  union layout_rat_hot_copy *hot)
{
	region_clear_deleg_rows(entry);

	u64 exe_ino = 0;
	u32 exe_dev = 0;
	(void)acl_get_exe_id(&exe_ino, &exe_dev);

	union layout_rat_acl_copy acl;
	memset(&hot->local, 0, sizeof(hot->local));
	memset(&acl.local, 0, sizeof(acl.local));

	u64 now = ktime_get_real_ns();
	hot->local.alloc_time = now;
	hot->local.modified_at = now;
	hot->local.uid = current_uid().val;
	hot->local.gid = current_gid().val;
	hot->local.mode = mode & 0777;
	acl.local.owner_node_id = sbi->node_id;
	acl.local.owner_pid = current->tgid;
	acl.local.owner_birth_time = acl_read_caller_birth_time();
	acl.local.owner_exe_inode_ino = exe_ino;
	acl.local.owner_exe_inode_dev = exe_dev;
	/* Bounded copies over a zeroed line, so a shorter id leaves no previous tenant's tail. */
	if (identity) {
		memcpy(acl.local.owner_group, identity->group,
		       strnlen(identity->group, sizeof(acl.local.owner_group)));
		memcpy(acl.local.owner_role, identity->role, strnlen(identity->role, sizeof(acl.local.owner_role)));
	}

	layout_write_rat_name(entry, name, name_len);
	cxl_set_layout_rat_acl(&entry->acl, &acl);
}

u32 region_find_free_rat_entry(struct fs_sb_info *sbi)
{
	if (!layout_get_rat(sbi))
		return LAYOUT_MAX_RAT_ENTRIES;

	union layout_rat_map_copy map;
	region_read_map(sbi, &map);
	return bits_find_next_clear(map.local.taken, LAYOUT_MAX_RAT_ENTRIES, 0);
}

/* Take slot @idx if it reads free. -EBUSY when it does not, which for a hint is a peer having
 * taken it since the caller looked. */
static s32 region_claim_rat_entry(struct fs_sb_info *sbi, u32 idx, const char *name, u32 name_len,
				  umode_t mode, const struct fs_daemon_attest_response *identity)
{
	struct layout_rat_entry *entry = layout_get_rat_entry(sbi, idx);
	if (!entry)
		return -EIO;

	union layout_rat_hot_copy hot;
	cxl_get_layout_rat_hot(&hot, &entry->hot);
	if (hot.local.state != LAYOUT_RAT_ENTRY_FREE)
		return -EBUSY;

	/* Claim: FREE → ALLOCATING, out of the line this read already holds. */
	hot.local.state = LAYOUT_RAT_ENTRY_ALLOCATING;
	cxl_set_layout_rat_hot(&entry->hot, &hot);

	if (test_crash_here(sbi, TEST_CRASH_ALLOC_WHILE_ALLOCATING))
		return -EIO;

	/* The other lines land while the slot reads ALLOCATING, which no reader accepts. */
	region_fill_rat_entry(entry, sbi, name, name_len, mode, identity, &hot);

	/* Publish: the hot line's fields and ALLOCATED in one write, so the read above and the
	 * one write here are all the traffic this line sees. */
	hot.local.state = LAYOUT_RAT_ENTRY_ALLOCATED;
	cxl_set_layout_rat_hot(&entry->hot, &hot);

	pr_debug("allocated RAT entry %u for '%s'\n", idx, name);
	return 0;
}

s32 region_alloc_rat_entry(struct fs_sb_info *sbi, const char *name, u32 hint, umode_t mode,
			   const struct fs_daemon_attest_response *identity, u32 *out_rat_entry_id)
{
	if (!sbi || !name || !out_rat_entry_id)
		return -EINVAL;

	u32 name_len = strlen(name);
	if (name_len > FS_NAME_MAX)
		return -ENAMETOOLONG;
	if (!layout_get_rat(sbi))
		return -EIO;

	union layout_rat_map_copy map;
	region_read_map(sbi, &map);

	u32 idx = hint;
	if (idx >= LAYOUT_MAX_RAT_ENTRIES || bits_has(map.local.taken, idx))
		idx = bits_find_next_clear(map.local.taken, LAYOUT_MAX_RAT_ENTRIES, 0);
	while (idx < LAYOUT_MAX_RAT_ENTRIES) {
		s32 ret = region_claim_rat_entry(sbi, idx, name, name_len, mode, identity);
		if (ret == 0)
			break;
		if (ret != -EBUSY)
			return ret;
		/* Taken with its bit clear is what a crash between the claim and the mark leaves, so
		 * the bit is set on the way past. */
		bits_set(map.local.taken, idx);
		idx = bits_find_next_clear(map.local.taken, LAYOUT_MAX_RAT_ENTRIES, idx + 1);
	}

	/* Every bit set: a slot a crash left marked while free is found by the entries themselves. */
	if (idx >= LAYOUT_MAX_RAT_ENTRIES) {
		for (idx = 0; idx < LAYOUT_MAX_RAT_ENTRIES; idx++) {
			s32 ret = region_claim_rat_entry(sbi, idx, name, name_len, mode, identity);
			if (ret == 0)
				break;
			if (ret != -EBUSY)
				return ret;
		}
	}
	if (idx >= LAYOUT_MAX_RAT_ENTRIES) {
		pr_err("no free RAT entries\n");
		return -ENOSPC;
	}

	if (test_crash_here(sbi, TEST_CRASH_ALLOC_BEFORE_MARK))
		return -EIO;

	bits_set(map.local.taken, idx);
	region_write_map(sbi, &map);
	*out_rat_entry_id = idx;
	return 0;
}

/*
 * region_free_rat_entry - hand a slot back, in the allocator's order reversed.
 * @entry: RAT entry pointer (caller holds sbi->meta_lock)
 *
 * Claim DELETING, clear, publish FREE. Neither DELETING nor the state it replaces is one a
 * lookup accepts, so nobody reads the entry between the claim and the publish, and no create
 * takes the slot before it is clear. Accepts ALLOCATED, ALLOCATING or DELETING.
 *
 * Returns false when the entry was already FREE, which is another path having finished it.
 */
bool region_free_rat_entry(struct fs_sb_info *sbi, struct layout_rat_entry *entry)
{
	struct layout_rat *rat = layout_get_rat(sbi);
	if (!rat || !entry)
		return false;
	const u32 idx = (u32)(entry - rat->entries);

	union layout_rat_hot_copy hot;
	cxl_get_layout_rat_hot(&hot, &entry->hot);
	if (hot.local.state == LAYOUT_RAT_ENTRY_FREE)
		return false;

	/* An extent going away is a placement change, so the count goes odd before the entry
	 * changes and even after, the way a placement steps it. */
	const bool placed = hot.local.extent_count != 0;
	union layout_rat_map_copy map;
	region_read_map(sbi, &map);
	u64 placements = map.local.placements;
	if (placed) {
		placements = region_step_placements_odd(placements);
		region_write_map_stepped(sbi, &map, placements, LAYOUT_MAX_RAT_ENTRIES);
		placements++;
	}

	/* Claim: whatever the caller found → DELETING, so a create scanning for FREE passes it
	 * by while the clearing runs. */
	hot.local.state = LAYOUT_RAT_ENTRY_DELETING;
	cxl_set_layout_rat_hot(&entry->hot, &hot);

	if (test_crash_here(sbi, TEST_CRASH_FREE_WHILE_DELETING))
		return true;

	/* Clear before the publish (H-S1): a slot handed back with a previous tenant's rows or
	 * name alive would carry them into the next file to land here. */
	region_clear_deleg_rows(entry);

	union layout_rat_acl_copy acl;
	memset(&acl.local, 0, sizeof(acl.local));
	cxl_set_layout_rat_acl(&entry->acl, &acl);

	layout_write_rat_name(entry, "", 0);

	/* Publish: the entry is clear, so the slot is takeable. */
	memset(&hot.local, 0, sizeof(hot.local));
	hot.local.state = LAYOUT_RAT_ENTRY_FREE;
	cxl_set_layout_rat_hot(&entry->hot, &hot);

	if (test_crash_here(sbi, TEST_CRASH_FREE_BEFORE_UNMARK))
		return true;

	/* After the publish, so a bit clear with the entry taken is the only stale shape a crash
	 * here leaves, and the allocator repairs that one. */
	region_write_map_stepped(sbi, &map, placements, idx);

	return true;
}

/* ============================================================================
 * Region initialization (called from ftruncate path)
 * ============================================================================ */

/* The devices a placement tries, as [@first, @last), or -EINVAL when @opts names one that cannot
 * hold the pool. Only the primary has an uncached pool. */
static s32 region_get_device_range(const struct fs_sb_info *sbi, const struct region_placement_opts *opts,
				   u32 *first, u32 *last)
{
	if (opts->device == REGION_DEVICE_ANY) {
		*first = FS_PRIMARY_DEVICE;
		*last = opts->uncached ? FS_PRIMARY_DEVICE + 1 : sbi->device_count;
		return 0;
	}
	if (opts->device >= sbi->device_count || (opts->uncached && opts->device != FS_PRIMARY_DEVICE))
		return -EINVAL;

	*first = opts->device;
	*last = opts->device + 1;
	return 0;
}

/* The first piece list of @region_size bytes that fits on device @device, or the error that says
 * why none does. @pool carries its spans. */
static s32 region_place_on_device(struct fs_sb_info *sbi, u32 device, const struct region_placement_opts *opts,
				  u64 region_size, const union layout_rat_map_copy *map, struct region_pool *pool,
				  struct region_extent *extents)
{
	s32 ret = region_collect_spans(sbi, device, opts->uncached, map, pool);
	if (ret)
		return ret;

	/* The uncached pool stays contiguous, since mmap maps a region there with one remap. */
	if (opts->paged && opts->uncached)
		return -EINVAL;
	if (opts->paged)
		return region_pick_gaps(pool, region_size, extents);

	ret = region_pick_gap(pool, region_size, &extents[0].offset);
	if (ret)
		return ret;
	extents[0].length = region_size;
	extents[0].device_id = (u16)device;
	return 1;
}

/* The caller holds the metadata lock. The gap search reads the standing extents and then writes
 * the winner, so two callers would place overlapping regions. */
s32 region_init_locked(struct fs_sb_info *sbi, u32 rat_entry_id, u64 data_size,
		       const struct region_placement_opts *opts, struct region_span *spans)
{
	struct layout_rat_entry *entry = layout_get_rat_entry(sbi, rat_entry_id);
	if (!entry || data_size == 0 || !spans)
		return -EINVAL;
	const bool uncached = opts->uncached;
	const bool paged = opts->paged;

	u32 first_device;
	u32 last_device;
	s32 ret = region_get_device_range(sbi, opts, &first_device, &last_device);
	if (ret)
		return ret;

	/* Reject requests no candidate pool can hold. */
	u64 capacity = 0;
	for (u32 device = first_device; device < last_device; device++) {
		u64 start;
		u64 end;
		region_get_pool_bounds(sbi, device, uncached, &start, &end);
		capacity = max(capacity, end - start);
	}
	if (data_size > capacity)
		return -ENOSPC;

	/* The granule is the unit both pools hand out, so a region takes whole ones. */
	u64 region_size = layout_align_up(data_size, sbi->granule);
	if (region_size > capacity)
		return -ENOSPC;

	/* Judged under the caller's lock: its wait sleeps, so a fence can land inside it, and the
	 * extent placed below would carry another node's id. */
	if (fs_is_fenced(sbi)) {
		ret = -EIO;
		goto out;
	}

	/* Read under the lock and nowhere else: the gap search below places an extent off this
	 * read, so a second caller committing on a stale one would leak what the first placed. */
	union layout_rat_hot_copy hot;
	cxl_get_layout_rat_hot(&hot, &entry->hot);
	if (hot.local.state != LAYOUT_RAT_ENTRY_ALLOCATED) {
		ret = -EINVAL;
		goto out;
	}
	if (hot.local.extent_count != 0) {
		ret = -EEXIST; /* Already initialized */
		goto out;
	}

	/* Read here, under the turn, where nothing can move. */
	union layout_rat_map_copy map;
	region_read_map(sbi, &map);
	struct region_pool pool = { .spans = spans };

	struct region_extent extents[LAYOUT_MAX_EXTENTS];
	s32 count = -ENOSPC;
	for (u32 device = first_device; device < last_device && count == -ENOSPC; device++)
		count = region_place_on_device(sbi, device, opts, region_size, &map, &pool, extents);
	if (count < 0) {
		ret = count;
		goto out;
	}

	/* Validate region fits in DAX mapping */
	for (s32 piece = 0; piece < count; piece++) {
		if (!layout_is_valid_range(sbi, extents[piece].device_id, extents[piece].offset,
					   extents[piece].length)) {
			pr_err("region_init: region 0x%llx+%llu exceeds DAX mapping\n", extents[piece].offset,
			       extents[piece].length);
			ret = -ENOSPC;
			goto out;
		}
	}

	/* Odd while the extent lands: a peer's list built across this write is not one to trust. */
	const u64 placements = region_step_placements_odd(map.local.placements) + 1;
	region_write_map_stepped(sbi, &map, placements - 1, LAYOUT_MAX_RAT_ENTRIES);

	/* The extent line before the hot line, which is what makes it read. A crash between leaves a
	 * line no hot line counts, and its pieces free. */
	region_write_extents(sbi, entry, extents, (u32)count);
	if (test_crash_here(sbi, TEST_CRASH_PLACE_BEFORE_COMMIT)) {
		ret = -EIO;
		goto out;
	}

	/* Commit: the piece count is what makes the extent line read. */
	hot.local.placement = paged ? LAYOUT_PLACEMENT_PAGED : LAYOUT_PLACEMENT_CONTIGUOUS;
	hot.local.extent_count = (u16)count;
	hot.local.modified_at = ktime_get_real_ns();
	cxl_set_layout_rat_hot(&entry->hot, &hot);

	if (test_crash_here(sbi, TEST_CRASH_PLACE_BEFORE_EVEN)) {
		ret = -EIO;
		goto out;
	}

	region_write_map_stepped(sbi, &map, placements, LAYOUT_MAX_RAT_ENTRIES);

out:
	if (ret) {
		pr_err("region_init failed for rat_entry=%u (err=%d)\n", rat_entry_id, ret);
		return ret;
	}

	pr_debug("region_init rat=%u first=0x%llx pieces=%d region_size=%llu data_size=%llu\n",
		 rat_entry_id, extents[0].offset, count, region_size, data_size);

	return 0;
}
