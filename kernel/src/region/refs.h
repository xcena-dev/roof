/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * refs.h - this node's open references on RAT entries, and whether any node holds one.
 *
 * An open takes one and the release of that struct file drops it, so a mapping that outlives its
 * descriptor is still counted until it goes. The count is this node's own; what reaches the medium
 * is one bit per entry, set while the count is above zero. unlink frees an entry only when no node's
 * bit is set. Otherwise the node that clears the last bit frees it, and the sweep is the fallback.
 */

#ifndef _REGION_REFS_H
#define _REGION_REFS_H

#include <linux/types.h>

struct fs_sb_info;

s32 region_alloc_refs(struct fs_sb_info *sbi);
void region_free_refs(struct fs_sb_info *sbi);

/* One more reference on @rat_entry_id, and the bit when this is the first. Needs no turn: the line
 * is this node's alone. */
void region_take_ref(struct fs_sb_info *sbi, u32 rat_entry_id);

/* One reference fewer, and the bit cleared when it was the last. True when it was. */
bool region_drop_ref(struct fs_sb_info *sbi, u32 rat_entry_id);

/* Whether any node's bit is set for @rat_entry_id. */
bool region_has_refs(struct fs_sb_info *sbi, u32 rat_entry_id);

/* Whether this node counts a reference on any entry. */
bool region_has_local_refs(const struct fs_sb_info *sbi);

/* Zeroes one node's line: this node's at mount, a dead node's at its eviction. */
void region_clear_node_refs(struct fs_sb_info *sbi, u32 node_id);

#endif /* _REGION_REFS_H */
