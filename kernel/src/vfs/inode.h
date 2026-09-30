/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * inode.h - In-memory inode state and inode entry points.
 *
 * `fs_inode_info` lives here so any layer needing per-file metadata
 * (region/shard/owner) can include just this header. `vfs_inode` MUST
 * remain the last field (container_of contract).
 */

#ifndef _FS_INODE_H
#define _FS_INODE_H

#include <linux/fs.h>
#include <linux/types.h>

#include "compat.h"
#include "core.h"

struct fs_inode_info {
	u32 region_id; /* Region where data is stored (= RAT entry ID) */
	u32 rat_entry_id;
	u64 region_offset;
	u32 owner_node_id;
	u32 owner_pid;
	u64 owner_birth_time;
	u64 data_phys_offset; /* Cached: region phys_offset (= data start) */
	/* Which pool the placement is to take, set while the file has no region and read once by
	 * it. Once placed, the offset is what says which pool holds the region. */
	bool want_uncached;
	struct inode vfs_inode; /* VFS inode (must be last!) */
};

static inline struct fs_inode_info *fs_get_inode_info(struct inode *inode)
{
	return container_of(inode, struct fs_inode_info, vfs_inode);
}

/* Zero-initialize the fields of inode_info this filesystem owns. */
static inline void fs_init_inode_info(struct fs_inode_info *xi)
{
	xi->region_id = 0;
	xi->rat_entry_id = 0;
	xi->region_offset = 0;
	xi->owner_node_id = 0;
	xi->owner_pid = 0;
	xi->owner_birth_time = 0;
	xi->data_phys_offset = 0;
	xi->want_uncached = false;
}

/* One chain, walked once: an inode names a superblock, and each of those names what this
 * filesystem keeps beside it. Every operation here starts by walking it, so it is walked in one
 * place rather than two lines at the top of each. */
struct inode_ctx {
	struct inode *inode;
	struct super_block *sb;
	struct fs_inode_info *xi;
	struct fs_sb_info *sbi;
};

/*
 * Answers whether an operation may act on @inode at all, and fills @ctx when it may. Every reason
 * to refuse a whole operation lives in its one definition, so a new entry point gets them all.
 */
bool __must_check inode_open_ctx(struct inode_ctx *ctx, struct inode *inode);

struct inode *inode_iget(struct super_block *sb, u32 region_id);
struct inode *inode_iget_lock_region(struct super_block *sb);
struct inode *inode_new(struct super_block *sb, umode_t mode);
void inode_evict(struct inode *inode);

/* Rereads the inode's fields from its RAT entry. An inode is keyed on its slot, so a caller that
 * has just pinned the slot with a reference reads here what that slot holds now. */
void inode_read_entry(struct inode *inode);

extern const struct inode_operations inode_iops;
extern const struct inode_operations dir_iops;

/*
 * Shared permission op: defers all access decisions to acl_check_permission()
 * at the data path. See inode.c for rationale.
 */
s32 inode_check_permission(COMPAT_IDMAP_PARAM_COMMA struct inode *inode, s32 mask);

#endif /* _FS_INODE_H */
