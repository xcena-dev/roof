// SPDX-License-Identifier: GPL-2.0-only
/* refs.c - the reference table: this node's counts, and one bit per entry on the medium */

#include <linux/atomic.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "core.h"
#include "cxl/io.h"
#include "layout/layout_access.h"
#include "layout/refs.h"
#include "region/refs.h"

static struct layout_refs_line *region_get_refs_line(struct fs_sb_info *sbi, u32 node_id)
{
	return layout_get_meta_ptr(sbi, LAYOUT_REFS_OFFSET + (u64)node_id * CXL_LINE_BYTES);
}

s32 region_alloc_refs(struct fs_sb_info *sbi)
{
	sbi->refs_local = kcalloc(LAYOUT_MAX_RAT_ENTRIES, sizeof(*sbi->refs_local), GFP_KERNEL);
	return sbi->refs_local ? 0 : -ENOMEM;
}

void region_free_refs(struct fs_sb_info *sbi)
{
	kfree(sbi->refs_local);
	sbi->refs_local = NULL;
}

static bool region_can_count_refs(const struct fs_sb_info *sbi, u32 rat_entry_id)
{
	return sbi->refs_local && rat_entry_id < LAYOUT_MAX_RAT_ENTRIES && !fs_is_identity_pending(sbi);
}

/*
 * The whole line comes off the counts as they read now, under the lock, so the publish that lands
 * last carries every change that came before it took the lock.
 */
static void region_publish_refs(struct fs_sb_info *sbi)
{
	mutex_lock(&sbi->refs_publish_lock);

	union layout_refs_line_copy line;
	memset(&line.local, 0, sizeof(line.local));
	for (u32 entry = 0; entry < LAYOUT_MAX_RAT_ENTRIES; entry++) {
		if (atomic_read(&sbi->refs_local[entry]) > 0)
			line.local.held[entry / LAYOUT_REFS_WORD_BITS] |= 1ULL << (entry % LAYOUT_REFS_WORD_BITS);
	}
	cxl_set_layout_refs_line(region_get_refs_line(sbi, sbi->node_id), &line);

	mutex_unlock(&sbi->refs_publish_lock);
}

void region_take_ref(struct fs_sb_info *sbi, u32 rat_entry_id)
{
	if (!region_can_count_refs(sbi, rat_entry_id))
		return;

	if (atomic_inc_return(&sbi->refs_local[rat_entry_id]) == 1)
		region_publish_refs(sbi);
}

bool region_drop_ref(struct fs_sb_info *sbi, u32 rat_entry_id)
{
	if (!region_can_count_refs(sbi, rat_entry_id))
		return false;

	atomic_t *held = &sbi->refs_local[rat_entry_id];
	if (WARN_ON_ONCE(atomic_read(held) == 0))
		return false;
	if (atomic_dec_return(held) != 0)
		return false;

	region_publish_refs(sbi);
	return true;
}

bool region_has_refs(struct fs_sb_info *sbi, u32 rat_entry_id)
{
	if (rat_entry_id >= LAYOUT_MAX_RAT_ENTRIES)
		return false;

	const u64 mask = 1ULL << (rat_entry_id % LAYOUT_REFS_WORD_BITS);
	for (u32 node = 1; node <= FS_MAX_NODE_ID; node++) {
		union layout_refs_line_copy line;
		cxl_get_layout_refs_line(&line, region_get_refs_line(sbi, node));
		if (line.local.held[rat_entry_id / LAYOUT_REFS_WORD_BITS] & mask)
			return true;
	}
	return false;
}

bool region_has_local_refs(const struct fs_sb_info *sbi)
{
	if (!sbi->refs_local)
		return false;

	for (u32 entry = 0; entry < LAYOUT_MAX_RAT_ENTRIES; entry++) {
		if (atomic_read(&sbi->refs_local[entry]) > 0)
			return true;
	}
	return false;
}

void region_clear_node_refs(struct fs_sb_info *sbi, u32 node_id)
{
	if (node_id == 0 || node_id > FS_MAX_NODE_ID)
		return;

	mutex_lock(&sbi->refs_publish_lock);
	cxl_zero_cachelines(region_get_refs_line(sbi, node_id), CXL_LINE_BYTES);
	if (node_id == sbi->node_id && sbi->refs_local) {
		for (u32 entry = 0; entry < LAYOUT_MAX_RAT_ENTRIES; entry++)
			atomic_set(&sbi->refs_local[entry], 0);
	}
	mutex_unlock(&sbi->refs_publish_lock);
}
