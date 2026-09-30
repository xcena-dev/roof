/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc.h - Dead-process and crash-orphan GC types and entry points.
 *
 * Orphan tracker stored in DRAM on each sbi (gc_orphans[]). GC sweeps RAT entries a process
 * left ALLOCATING or DELETING when it died mid-operation.
 */

#ifndef _FS_GC_H
#define _FS_GC_H

#include <linux/types.h>

#define GC_ORPHAN_MAX 64

enum gc_orphan_type {
	GC_ORPHAN_RAT, /* a RAT entry stuck ALLOCATING or DELETING */
};

/* @seen_at is the entry's own stamp when it was tracked, which is what tells the same entry from
 * whatever took its place: the address alone would hand a new tenant the old one's clock. */
struct gc_orphan_tracker {
	void *entry;
	u64 seen_at;
	u64 discovered_at;
	enum gc_orphan_type type;
};

struct fs_sb_info;

s32 gc_init(struct fs_sb_info *sbi);
void gc_exit(struct fs_sb_info *sbi);
s32 gc_reclaim_dead_regions(struct fs_sb_info *sbi);
bool gc_can_force_unlink(struct fs_sb_info *sbi, u32 rat_entry_id);
s32 gc_start(struct fs_sb_info *sbi);
void gc_stop_thread(struct fs_sb_info *sbi);

/* Clears every row and reference naming this node, as the admin would for a dead one. False when
 * some entry stayed locked through every pass, so the caller must not hand the id back. */
bool gc_clear_own_node(struct fs_sb_info *sbi);

#endif /* _FS_GC_H */
