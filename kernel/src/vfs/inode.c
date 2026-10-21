// SPDX-License-Identifier: GPL-2.0-only
/*
 * inode.c - inode operations
 *
 * VFS inode operations over the partitioned global index.
 * Maps index entries to VFS inodes and handles metadata updates.
 *
 * Two-phase model:
 *   inode_iget:    reads RAT entry, and the pieces with the size once the region is placed
 *   inode_set_attr: ftruncate triggers region_init (first-time only, WORM)
 */

#include <linux/blk_types.h> /* SECTOR_SIZE, SECTOR_SHIFT: the units i_blocks counts */
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/time.h>

#include "access/acl.h"
#include "access/meta_lock.h"
#include "compat.h"
#include "core.h"
#include "layout/layout_access.h"
#include "region/region.h"
#include "vfs/dir.h"
#include "vfs/file.h"
#include "vfs/inode.h"

/* ============================================================================
 * Operation admission
 * ============================================================================ */

/*
 * One chain, walked once, and the answer to whether it may be walked at all. A fenced mount is
 * refused here because its node_id names shared state a live peer answers to now.
 */
bool inode_open_ctx(struct inode_ctx *ctx, struct inode *inode)
{
	ctx->inode = inode;
	ctx->sb = inode->i_sb;
	ctx->xi = fs_get_inode_info(inode);
	ctx->sbi = fs_get_sb_info(ctx->sb);

	if (ctx->sbi == NULL || ctx->xi == NULL)
		return false;

	return !fs_is_fenced(ctx->sbi);
}

/* ============================================================================
 * inode read operations (index entry → VFS inode)
 * ============================================================================ */

/*
 * inode_resolve_rat_entry - read RAT entry fields into inode info
 * @sbi: superblock info
 * @xi: inode info to populate
 * @rat_entry_id: RAT entry ID to resolve
 *
 * Reads ownership from the RAT entry, and the pieces with the file's size.
 *
 * Return: 0 on success, -ENOENT if the RAT entry holds no region, -EINVAL if missing
 */
static s32 inode_resolve_rat_entry(struct fs_sb_info *sbi, struct fs_inode_info *xi, u32 rat_entry_id)
{
	struct layout_rat_entry *rat_entry = layout_get_rat_entry(sbi, rat_entry_id);
	if (!rat_entry)
		return -EINVAL;

	/* OWNER_DEAD resolves too: its readers still reach it by name, and taking a row off it needs
	 * an fd. Whether the caller may do that is the permission check's answer, not this one. */
	union layout_rat_hot_copy hot;
	cxl_get_layout_rat_hot(&hot, &rat_entry->hot);
	if (!layout_rat_state_is_standing(hot.local.state))
		return -ENOENT;

	union layout_rat_acl_copy acl;

	cxl_get_layout_rat_acl(&acl, &rat_entry->acl);
	xi->owner_node_id = acl.local.owner_node_id;
	xi->owner_pid = acl.local.owner_pid;
	xi->owner_birth_time = acl.local.owner_birth_time;

	inode_load_extents(sbi, xi, &hot);
	return 0;
}

void inode_load_extents(struct fs_sb_info *sbi, struct fs_inode_info *xi, const union layout_rat_hot_copy *hot)
{
	const s32 count = region_read_extents(sbi, xi->rat_entry_id, hot, xi->extents);
	xi->extent_count = count > 0 ? (u32)count : 0;

	/* The pieces are whole granules, and the file is as long as they are together. */
	struct inode *inode = &xi->vfs_inode;
	const u64 size = region_sum_extents(xi->extents, xi->extent_count);
	i_size_write(inode, size);
	inode->i_blocks = (size + SECTOR_SIZE - 1) >> SECTOR_SHIFT;
}

/*
 * inode_fill_from_entry - populate VFS inode fields from the RAT entry
 * @inode: target inode
 * @rat_e: RAT entry
 *
 * Every field a peer node can change, so a cached inode refreshed through here
 * follows that node's writes. modified_at is the last size decision and not the
 * last data write: mmap is the only way to write, and it takes no hook.
 */
static void inode_fill_from_entry(struct inode *inode, struct layout_rat_entry *rat_e)
{
	union layout_rat_hot_copy copy;

	cxl_get_layout_rat_hot(&copy, &rat_e->hot);

	u64 mod_time = copy.local.modified_at;
	struct timespec64 stamp = { .tv_sec = mod_time / NSEC_PER_SEC, .tv_nsec = mod_time % NSEC_PER_SEC };

	inode->i_mode = S_IFREG | copy.local.mode;
	inode->i_uid = make_kuid(&init_user_ns, copy.local.uid);
	inode->i_gid = make_kgid(&init_user_ns, copy.local.gid);
	inode_set_mtime_to_ts(inode, stamp);
	inode_set_ctime_to_ts(inode, stamp);
	set_nlink(inode, 1);
}

/*
 * Core mm then treats a mapping of the file as dax: a huge fault whatever the THP setting, and an
 * unmap that drops the folio references huge entries hold. GUP refuses FOLL_LONGTERM pins on it.
 */
static void inode_mark_dax(struct inode *inode)
{
	inode->i_flags |= S_DAX;
}

/*
 * iget_by_region - create or refresh the inode for RAT entry @region_id
 * @sb: Superblock
 * @region_id: RAT entry id, which is also what the ino is made from
 *
 * Every field comes from the RAT entry, read fresh so another node's write is
 * visible. A cached inode is refreshed too, because a reused RAT entry carries
 * a different file behind the same ino.
 *
 * Two-phase awareness:
 *   extent_count > 0: region placed, the pieces and the size are read
 *   extent_count == 0: region not yet placed (ftruncate pending)
 *
 * Return: inode pointer on success, ERR_PTR on error
 */
static struct inode *iget_by_region(struct super_block *sb, u32 region_id)
{
	struct fs_sb_info *sbi = fs_get_sb_info(sb);

	struct inode *inode = iget_locked(sb, inode_make_ino(region_id));
	if (!inode)
		return ERR_PTR(-ENOMEM);

	struct fs_inode_info *xi = fs_get_inode_info(inode);
	xi->region_id = region_id;
	xi->rat_entry_id = region_id;

	if (!(inode->i_state & I_NEW)) {
		/*
		 * Already cached — refresh from CXL memory for cross-node
		 * visibility.  When a RAT entry is reused (file deleted then
		 * new file allocated to the same entry), the cached inode
		 * carries stale pieces / owner fields.  Re-read
		 * everything that can change between uses of the same ino.
		 */
		struct layout_rat_entry *cached = layout_get_rat_entry(sbi, region_id);
		if (cached)
			inode_fill_from_entry(inode, cached);

		/* Re-validate RAT entry: GC may have freed it since caching */
		s32 ret = inode_resolve_rat_entry(sbi, xi, region_id);
		if (ret == -ENOENT) {
			iput(inode);
			return ERR_PTR(-ESTALE);
		}

		return inode;
	}

	/* Restore fields from RAT entry */
	s32 ret = inode_resolve_rat_entry(sbi, xi, region_id);
	if (ret == -ENOENT) {
		iget_failed(inode);
		return ERR_PTR(-ENOENT);
	}
	if (ret == -EINVAL) {
		/* Invalid RAT entry ID */
		xi->owner_node_id = 0;
		xi->owner_pid = 0;
		xi->owner_birth_time = 0;
		xi->extent_count = 0;
	}

	struct layout_rat_entry *rat_e = layout_get_rat_entry(sbi, region_id);
	if (rat_e) {
		union layout_rat_hot_copy hot;

		cxl_get_layout_rat_hot(&hot, &rat_e->hot);

		u64 alloc_time = hot.local.alloc_time;
		struct timespec64 born = { .tv_sec = alloc_time / NSEC_PER_SEC, .tv_nsec = alloc_time % NSEC_PER_SEC };

		inode_fill_from_entry(inode, rat_e);

		/* atime only. The other two come from the fill above, which the
		 * refresh path shares, so a peer's write reaches both. Nothing
		 * advances atime afterwards: reads take no hook here. */
		inode_set_atime_to_ts(inode, born);
	}

	inode->i_op = &inode_iops;
	inode->i_fop = &file_fops;
	inode->i_mapping->a_ops = &file_aops;
	inode_mark_dax(inode);

	unlock_new_inode(inode);
	return inode;
}

void inode_read_entry(struct inode *inode)
{
	struct fs_sb_info *sbi = fs_get_sb_info(inode->i_sb);
	struct fs_inode_info *xi = fs_get_inode_info(inode);
	struct layout_rat_entry *entry = layout_get_rat_entry(sbi, xi->rat_entry_id);
	if (!entry)
		return;

	inode_fill_from_entry(inode, entry);
	inode_resolve_rat_entry(sbi, xi, xi->rat_entry_id);
}

/* inode_iget - inode for a name found in the global index. */
struct inode *inode_iget(struct super_block *sb, u32 region_id)
{
	return iget_by_region(sb, region_id);
}

/*
 * inode_iget_lock_region - inode for the daemon's lock region.
 *
 * That name is not in the index, so there are no shard coordinates to pass:
 * format writes the entry at a fixed id and lookup resolves the name to it.
 */
struct inode *inode_iget_lock_region(struct super_block *sb)
{
	struct fs_sb_info *sbi = fs_get_sb_info(sb);
	struct layout_rat_entry *entry = layout_get_rat_entry(sbi, FS_LOCK_REGION_RAT_ID);

	if (!entry)
		return ERR_PTR(-ENOENT);

	union layout_rat_hot_copy hot;

	cxl_get_layout_rat_hot(&hot, &entry->hot);
	if (hot.local.region_type != LAYOUT_REGION_LOCK)
		return ERR_PTR(-ENOENT);

	return iget_by_region(sb, FS_LOCK_REGION_RAT_ID);
}

/* ============================================================================
 * New inode creation
 * ============================================================================ */

/*
 * inode_new - create new VFS inode
 * @sb: Superblock
 * @mode: file mode (S_IFREG | permissions)
 *
 * Creates new in-memory inode before writing to index. Caller must fill in
 * region_id and rat_entry_id.
 *
 * Return: inode pointer on success, ERR_PTR on error
 */
struct inode *inode_new(struct super_block *sb, umode_t mode)
{
	struct inode *inode = new_inode(sb);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	struct fs_inode_info *xi = fs_get_inode_info(inode);
	fs_init_inode_info(xi);

	inode->i_ino = 0;
	inode->i_mode = mode;
	inode->i_uid = current_fsuid();
	inode->i_gid = current_fsgid();

	{
		struct timespec64 ts = current_time(inode);
		inode_set_atime_to_ts(inode, ts);
		inode_set_mtime_to_ts(inode, ts);
		inode_set_ctime_to_ts(inode, ts);
	}

	inode->i_blocks = 0;
	set_nlink(inode, 1);

	if (S_ISREG(mode)) {
		inode->i_op = &inode_iops;
		inode->i_fop = &file_fops;
		inode->i_mapping->a_ops = &file_aops;
		inode_mark_dax(inode);
	} else if (S_ISDIR(mode)) {
		inode->i_op = &dir_iops;
		inode->i_fop = &dir_fops;
		set_nlink(inode, 2);
	}

	return inode;
}

/* ============================================================================
 * inode eviction
 * ============================================================================ */

/*
 * inode_evict - evict inode from cache
 * @inode: inode to evict
 *
 * Called when inode is removed from cache. Cleans up page cache and marks
 * inode as clean.
 */
void inode_evict(struct inode *inode)
{
	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
}

/* ============================================================================
 * File attribute operations
 * ============================================================================ */

/*
 * inode_get_attr - get file attributes
 *
 * Reads latest file size from index entry to ensure cross-node consistency,
 * then fills stat structure.
 */
static s32 inode_get_attr(COMPAT_IDMAP_PARAM_COMMA const struct path *path, struct kstat *stat, u32 request_mask, u32 query_flags)
{
	struct inode_ctx ctx;
	/* The size below comes from the RAT, and a fenced mount's entry answers to another node. */
	if (!inode_open_ctx(&ctx, d_inode(path->dentry)))
		return -EIO;

	compat_fill_attr(COMPAT_IDMAP_ARG_COMMA request_mask, ctx.inode, stat);

	/* A placed region's size does not change, so only an unplaced copy can be behind: a peer may
	 * have placed the region since, which the RAT says. */
	if (ctx.inode->i_ino == FS_ROOT_INO || ctx.xi->extent_count != 0)
		return 0;

	struct layout_rat_entry *rat_e = layout_get_rat_entry(ctx.sbi, ctx.xi->rat_entry_id);
	if (!rat_e)
		return 0;
	union layout_rat_hot_copy hot;
	cxl_get_layout_rat_hot(&hot, &rat_e->hot);
	if (hot.local.extent_count == 0)
		return 0;

	inode_lock(ctx.inode);
	inode_load_extents(ctx.sbi, ctx.xi, &hot);
	inode_unlock(ctx.inode);
	stat->size = i_size_read(ctx.inode);
	stat->blocks = ctx.inode->i_blocks;
	return 0;
}

/* The span buffer is allocated before the lock, so the placement never waits on memory while it
 * holds the turn. */
static s32 inode_place_region(struct inode_ctx *ctx, u64 size)
{
	struct region_span *spans = region_alloc_spans();
	if (!spans)
		return -ENOMEM;

	struct meta_lock lock;
	s32 ret = meta_lock_begin(ctx->sbi, &lock, TEST_META_OP_PLACE);
	if (ret)
		goto out_free;
	ret = meta_lock_finish(ctx->sbi, &lock);
	if (ret)
		goto out_free;

	const struct region_placement_opts opts = { .uncached = ctx->xi->want_uncached, .paged = ctx->xi->want_paged };
	ret = region_init_locked(ctx->sbi, ctx->xi->rat_entry_id, size, &opts, spans);
	if (ret) {
		pr_err("region_init failed for rat_entry %u: %d\n", ctx->xi->rat_entry_id, ret);
		goto out_unlock;
	}

	struct layout_rat_entry *rat_entry = layout_get_rat_entry(ctx->sbi, ctx->xi->rat_entry_id);
	if (!rat_entry) {
		ret = -EIO;
		goto out_unlock;
	}
	union layout_rat_hot_copy hot;
	cxl_get_layout_rat_hot(&hot, &rat_entry->hot);
	inode_load_extents(ctx->sbi, ctx->xi, &hot);

	struct timespec64 now = current_time(ctx->inode);
	inode_set_mtime_to_ts(ctx->inode, now);
	inode_set_ctime_to_ts(ctx->inode, now);

out_unlock:
	meta_unlock(ctx->sbi, &lock);
out_free:
	region_free_spans(spans);
	return ret;
}

/*
 * inode_set_attr - set file attributes (ftruncate triggers region allocation)
 *
 * WORM enforcement:
 *   - First ftruncate (i_size == 0 -> new size): allocates physical region
 *   - Second ftruncate (i_size > 0): rejected with -EACCES
 *
 * This is the key function in the two-phase create model:
 *   open(O_CREAT) creates lightweight entry (i_size=0)
 *   ftruncate(size) triggers region_init() here
 */
static s32 inode_set_attr(COMPAT_IDMAP_PARAM_COMMA struct dentry *dentry, struct iattr *attr)
{
	struct inode_ctx ctx;
	if (!inode_open_ctx(&ctx, d_inode(dentry)))
		return -EIO;

	s32 ret = compat_prepare_setattr(COMPAT_IDMAP_ARG_COMMA dentry, attr);
	if (ret)
		return ret;

	/* CXL FS: only ATTR_SIZE (ftruncate) is meaningful — silently
	 * succeed for other attrs (chmod/chown/utimes) since CXL memory
	 * does not persist POSIX attributes.
	 */
	if (!(attr->ia_valid & ATTR_SIZE))
		return 0;

	if (attr->ia_valid & ATTR_SIZE) {
		/* Placing a region is one of the three mutations another node must be
		 * excluded from, and that exclusion arrives with the identity. */
		if (fs_is_identity_pending(ctx.sbi))
			return -EAGAIN;

		/* permission check before region allocation */
		ret = acl_check_permission(ctx.sbi, ctx.xi->rat_entry_id, FS_PERM_WRITE);
		if (ret)
			return ret;

		/* WORM: reject size changes if file already has data */
		if (ctx.inode->i_size > 0)
			return -EACCES;

		/* No-op if setting to 0 */
		if (attr->ia_size == 0)
			return 0;

		/* Placement only through a descriptor: its open counted a reference and re-read the
		 * slot, so an unmount drain sees it and a path lookup's cached slot is never placed. */
		if (!(attr->ia_valid & ATTR_FILE))
			return -EOPNOTSUPP;

		ret = inode_place_region(&ctx, attr->ia_size);
		if (ret)
			return ret;

		pr_debug("ftruncate rat=%u size=%lld first=0x%llx\n", ctx.xi->rat_entry_id, ctx.inode->i_size,
			 ctx.xi->extents[0].offset);
	}

	return 0;
}

/* ============================================================================
 * Permission gate
 *
 * This filesystem uses a delegated identity model: VFS-level access checks
 * (open / lookup / traversal) are always permitted. Actual access
 * control happens at the data path (mmap / read / write / ioctl)
 * via acl_check_permission() against helper-driven RAT delegations.
 * POSIX mode bits on a file here are decorative — the helper's policy
 * is authoritative.
 *
 * Without this override, generic_permission() would reject cross-uid
 * opens when the file's mode bits do not grant the operation, blocking
 * the helper upcall before policy evaluation could even run.
 * ============================================================================ */
s32 inode_check_permission(COMPAT_IDMAP_PARAM_COMMA struct inode *inode, s32 mask)
{
	(void)inode;
	(void)mask;
	return 0;
}

/* ============================================================================
 * File inode operations table
 * ============================================================================ */

const struct inode_operations inode_iops = {
	.permission = inode_check_permission,
	.getattr = inode_get_attr,
	.setattr = inode_set_attr,
};
