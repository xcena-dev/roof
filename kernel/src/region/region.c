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

/* ============================================================================
 * RAT (Region Allocation Table) management
 * ============================================================================ */

s32 region_alloc_scratch(struct fs_sb_info *sbi)
{
	sbi->span_scratch = kmalloc_array(LAYOUT_MAX_RAT_ENTRIES, sizeof(*sbi->span_scratch), GFP_KERNEL);
	sbi->span_count = 0;
	/* Odd, so it matches no quiet count and the first search reads the entries. */
	sbi->span_placements = ~0ULL;
	return sbi->span_scratch ? 0 : -ENOMEM;
}

void region_free_scratch(struct fs_sb_info *sbi)
{
	kfree(sbi->span_scratch);
	sbi->span_scratch = NULL;
}

s32 region_get_pool_usage(struct fs_sb_info *sbi, struct region_pool_usage *out)
{
	if (!sbi || !out)
		return -EINVAL;

	memset(out, 0, sizeof(*out));
	out->uc_total = sbi->wb_start - sbi->uc_start;
	out->wb_total = sbi->total_size - sbi->wb_start;

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

		if (hot.local.phys_offset < sbi->uc_start)
			continue;

		if (hot.local.phys_offset < sbi->wb_start)
			out->uc_used += hot.local.size;
		else
			out->wb_used += hot.local.size;
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
		layout_rat_map_mark(&stepped, freed, false);
	region_write_map(sbi, &stepped);
}

/* The count an extent change steps to as it begins: odd, and past whatever the line read, so a
 * list stamped with the old value stops matching even when that value was odd already. */
static u64 region_step_placements_odd(u64 placements)
{
	return (placements & 1) ? placements + 2 : placements + 1;
}

/* The pool one placement searches. The uncached pool runs from the metadata end to where the
 * write-back pool begins, and that one runs to the device end. uc_start is the metadata end rounded
 * up to a granule, so it never sits below the head's regions_start and keeps a placement off the
 * metadata. */
static void region_get_pool_bounds(struct fs_sb_info *sbi, bool uncached, u64 *start, u64 *end)
{
	*start = uncached ? sbi->uc_start : sbi->wb_start;
	*end = uncached ? sbi->wb_start : sbi->total_size;
}

/* Sorted by offset, so the gap walk below is one pass. */
static void region_insert_span(struct fs_sb_info *sbi, u64 offset, u64 end)
{
	struct region_span *spans = sbi->span_scratch;
	u32 at = sbi->span_count;
	while (at > 0 && spans[at - 1].offset > offset) {
		spans[at] = spans[at - 1];
		at--;
	}
	spans[at].offset = offset;
	spans[at].end = end;
	sbi->span_count++;
}

/* Rebuild the mount's extent list for @uncached's pool from the entries @map marks taken, and stamp
 * it with @map's placement count. @map was read before the entries, and a placement steps the count
 * after its write, so a list whose count still reads current missed nothing. */
static s32 region_collect_spans(struct fs_sb_info *sbi, bool uncached, const union layout_rat_map_copy *map)
{
	u64 regions_start;
	u64 regions_end;
	region_get_pool_bounds(sbi, uncached, &regions_start, &regions_end);

	sbi->span_count = 0;
	sbi->span_uncached = uncached;
	sbi->span_placements = map->local.placements;

	/* Every entry while the summary is not trusted, since a taken bit a device formatted
	 * before the summary existed never had set would hide a standing extent. */
	const bool by_bits = sbi->rat_map_trusted;
	for (u32 idx = by_bits ? layout_rat_map_next(map, 0, true) : 0; idx < LAYOUT_MAX_RAT_ENTRIES;
	     idx = by_bits ? layout_rat_map_next(map, idx + 1, true) : idx + 1) {
		struct layout_rat_entry *entry = layout_get_rat_entry(sbi, idx);
		if (!entry)
			return -EIO;

		union layout_rat_hot_copy hot;
		cxl_get_layout_rat_hot(&hot, &entry->hot);
		if (!layout_rat_state_is_standing(hot.local.state))
			continue;

		const u64 entry_offset = hot.local.phys_offset;
		const u64 entry_size = hot.local.size;
		if (entry_offset == 0 || entry_size == 0)
			continue;

		/* Skip corrupted entries (overflow protection) */
		if (entry_size > sbi->total_size || entry_offset > sbi->total_size - entry_size)
			continue;

		/* The other pool's, and no candidate here can reach it. */
		if (entry_offset + entry_size <= regions_start || entry_offset >= regions_end)
			continue;

		region_insert_span(sbi, entry_offset, entry_offset + entry_size);
	}
	return 0;
}

/* True when the list is @uncached's pool as of @map, and no placement is in flight: the count is
 * odd from a placer's first step to its last, and a list built across one may have missed it. */
static bool region_spans_are_current(const struct fs_sb_info *sbi, bool uncached,
				     const union layout_rat_map_copy *map)
{
	return sbi->rat_map_trusted &&
	       sbi->span_uncached == uncached &&
	       sbi->span_placements == map->local.placements &&
	       (map->local.placements & 1) == 0;
}

void region_survey_space(struct fs_sb_info *sbi, bool uncached)
{
	if (!layout_get_rat(sbi) || !sbi->span_scratch)
		return;

	union layout_rat_map_copy map;
	region_read_map(sbi, &map);
	if (!region_spans_are_current(sbi, uncached, &map))
		region_collect_spans(sbi, uncached, &map);
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
			layout_rat_map_mark(seen, idx, true);
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

/* The first gap of @size in the sorted list, or -ENOSPC. */
static s32 region_pick_gap(struct fs_sb_info *sbi, bool uncached, u64 size, u64 *out_offset)
{
	u64 regions_start;
	u64 regions_end;
	region_get_pool_bounds(sbi, uncached, &regions_start, &regions_end);

	const struct region_span *spans = sbi->span_scratch;
	u64 candidate = regions_start;
	for (u32 idx = 0; idx < sbi->span_count; idx++) {
		if (candidate + size <= spans[idx].offset) {
			*out_offset = candidate;
			return 0;
		}
		if (spans[idx].end > candidate)
			candidate = layout_align_up(spans[idx].end, sbi->granule);
	}

	if (candidate + size <= regions_end) {
		*out_offset = candidate;
		return 0;
	}

	pr_err("no contiguous space for size %llu in [0x%llx, 0x%llx)\n", size, regions_start, regions_end);
	return -ENOSPC;
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
 * @offset: physical offset
 * @size: region size
 *
 * Runs while the slot reads ALLOCATING, so no peer reads what these writes pass through. The name
 * and acl lines are written here, the acl with the calling task as owner and @identity as its group
 * and role; the hot line is only built into @hot, with @mode and the caller's ids, and the caller
 * writes it once with ALLOCATED so the fields land together with the state.
 */
static void region_fill_rat_entry(struct layout_rat_entry *entry, struct fs_sb_info *sbi, const char *name, u32 name_len,
				  u64 offset, u64 size, umode_t mode, const struct fs_daemon_attest_response *identity,
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
	hot->local.phys_offset = offset;
	hot->local.size = size;
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
	return layout_rat_map_next(&map, 0, false);
}

/* Take slot @idx if it reads free. -EBUSY when it does not, which for a hint is a peer having
 * taken it since the caller looked. */
static s32 region_claim_rat_entry(struct fs_sb_info *sbi, u32 idx, const char *name, u32 name_len,
				  u64 offset, u64 size, umode_t mode, const struct fs_daemon_attest_response *identity)
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
	region_fill_rat_entry(entry, sbi, name, name_len, offset, size, mode, identity, &hot);

	/* Publish: the hot line's fields and ALLOCATED in one write, so the read above and the
	 * one write here are all the traffic this line sees. */
	hot.local.state = LAYOUT_RAT_ENTRY_ALLOCATED;
	cxl_set_layout_rat_hot(&entry->hot, &hot);

	pr_debug("allocated RAT entry %u for '%s' at offset %llu size %llu\n", idx, name, offset, size);
	return 0;
}

s32 region_alloc_rat_entry(struct fs_sb_info *sbi, const char *name, u64 size, u64 offset, u32 hint, umode_t mode,
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
	if (idx >= LAYOUT_MAX_RAT_ENTRIES || layout_rat_map_has(&map, idx))
		idx = layout_rat_map_next(&map, 0, false);
	while (idx < LAYOUT_MAX_RAT_ENTRIES) {
		s32 ret = region_claim_rat_entry(sbi, idx, name, name_len, offset, size, mode, identity);
		if (ret == 0)
			break;
		if (ret != -EBUSY)
			return ret;
		/* Taken with its bit clear is what a crash between the claim and the mark leaves, so
		 * the bit is set on the way past. */
		layout_rat_map_mark(&map, idx, true);
		idx = layout_rat_map_next(&map, idx + 1, false);
	}

	/* Every bit set: a slot a crash left marked while free is found by the entries themselves. */
	if (idx >= LAYOUT_MAX_RAT_ENTRIES) {
		for (idx = 0; idx < LAYOUT_MAX_RAT_ENTRIES; idx++) {
			s32 ret = region_claim_rat_entry(sbi, idx, name, name_len, offset, size, mode, identity);
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

	layout_rat_map_mark(&map, idx, true);
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
	const bool placed = hot.local.phys_offset != 0;
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

/*
 * region_init - allocate physical space and initialize region header
 * @sbi: superblock info
 * @rat_entry_id: RAT entry ID (must be pre-allocated in reservation mode)
 * @data_size: user-requested data size in bytes
 *
 * Called from inode_set_attr() when ftruncate sets file size for the first time.
 * Finds contiguous space, initializes region header with name table,
 * and updates the RAT entry with physical offset/size.
 *
 * Region layout (v2: header in pool, data starts at phys_offset):
 *   [Data area (data_size, 2MB aligned)]
 *   Total region_size = align_2MB(data_size)
 *
 * Returns 0 on success, negative error code on failure.
 *
 * PRECONDITION: the caller holds the metadata lock. The gap search reads the standing extents
 * and then writes the winner, so two callers would place overlapping regions.
 */
s32 region_init_locked(struct fs_sb_info *sbi, u32 rat_entry_id, u64 data_size, bool uncached)
{
	struct layout_rat_entry *entry = layout_get_rat_entry(sbi, rat_entry_id);
	if (!entry || data_size == 0)
		return -EINVAL;

	/* Reject obviously oversized requests */
	if (data_size > sbi->total_size)
		return -ENOSPC;

	/* The granule is the unit both pools hand out, so a region takes whole ones. */
	u64 region_size = layout_align_up(data_size, sbi->granule);
	if (region_size > sbi->total_size)
		return -ENOSPC;

	/* Judged under the caller's lock: its wait sleeps, so a fence can land inside it, and the
	 * extent placed below would carry another node's id. */
	s32 ret = 0;

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
	if (hot.local.phys_offset != 0) {
		ret = -EEXIST; /* Already initialized */
		goto out;
	}

	/* The list a survey left is used as it stands when the placement count says nothing moved
	 * since; otherwise it is read again here, under the turn, where nothing can move. */
	if (!sbi->span_scratch) {
		ret = -EINVAL;
		goto out;
	}
	union layout_rat_map_copy map;
	region_read_map(sbi, &map);
	if (!region_spans_are_current(sbi, uncached, &map)) {
		ret = region_collect_spans(sbi, uncached, &map);
		if (ret)
			goto out;
	}

	u64 region_offset;
	ret = region_pick_gap(sbi, uncached, region_size, &region_offset);
	if (ret)
		goto out;

	/* Validate region fits in DAX mapping */
	if (!layout_is_valid_range(sbi, region_offset, region_size)) {
		pr_err("region_init: region 0x%llx+%llu exceeds DAX mapping\n", region_offset, region_size);
		ret = -ENOSPC;
		goto out;
	}

	/* Odd while the extent lands: a peer's list built across this write is not one to trust. */
	const u64 placements = region_step_placements_odd(map.local.placements) + 1;
	region_write_map_stepped(sbi, &map, placements - 1, LAYOUT_MAX_RAT_ENTRIES);
	if (test_crash_here(sbi, TEST_CRASH_PLACE_BEFORE_EXTENT)) {
		ret = -EIO;
		goto out;
	}

	/* Commit: write phys_offset + size to RAT entry */
	hot.local.phys_offset = region_offset;
	hot.local.size = region_size;
	cxl_set_layout_rat_hot(&entry->hot, &hot);

	if (test_crash_here(sbi, TEST_CRASH_PLACE_BEFORE_EVEN)) {
		ret = -EIO;
		goto out;
	}

	region_write_map_stepped(sbi, &map, placements, LAYOUT_MAX_RAT_ENTRIES);

	/* The list now carries this extent too, so the next placement on this node reads no entry. */
	region_insert_span(sbi, region_offset, region_offset + region_size);
	sbi->span_placements = placements;

out:
	if (ret) {
		pr_err("region_init failed for rat_entry=%u (err=%d)\n", rat_entry_id, ret);
		return ret;
	}

	pr_debug("region_init rat=%u region_offset=0x%llx region_size=%llu data_size=%llu\n",
		 rat_entry_id, region_offset, region_size, data_size);

	return 0;
}
