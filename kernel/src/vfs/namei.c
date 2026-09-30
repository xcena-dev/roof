// SPDX-License-Identifier: GPL-2.0-only
/*
 * namei.c - resolving a name in the flat namespace to a file
 *
 * Lookup, create and unlink. The object of all three is a file, and the directory they are
 * reached through is the one flat root.
 */

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/string.h>

#include "access/acl.h"
#include "access/daemon.h"
#include "access/meta_lock.h"
#include "compat.h"
#include "core.h"
#include "layout/layout_access.h"
#include "lifecycle/gc.h"
#include "region/index.h"
#include "region/refs.h"
#include "region/region.h"
#include "test/test_hooks.h"
#include "vfs/file.h"
#include "vfs/inode.h"

/* ============================================================================
 * namei_lookup - Global index hash lookup
 * ============================================================================ */

static struct dentry *namei_lookup(struct inode *dir, struct dentry *dentry, u32 flags)
{
	struct inode *inode = NULL;

	struct inode_ctx parent;
	if (!inode_open_ctx(&parent, dir))
		return ERR_PTR(-EIO);

	if (dir->i_ino != FS_ROOT_INO)
		return ERR_PTR(-ENOENT);

	pr_debug("lookup '%pd' in global index\n", dentry);

	/* The lock region is not in the index: its name resolves to a fixed id. */
	if (layout_check_lock_region_name(dentry->d_name.name, dentry->d_name.len)) {
		inode = inode_iget_lock_region(parent.sb);
		if (IS_ERR(inode))
			return (PTR_ERR(inode) == -ENOENT) ? d_splice_alias(NULL, dentry) : ERR_CAST(inode);
		return d_splice_alias(inode, dentry);
	}

	u32 region_id;
	s32 ret = index_find(parent.sbi, dentry->d_name.name, dentry->d_name.len, &region_id);
	if (ret == 0) {
		inode = inode_iget(parent.sb, region_id);
		if (IS_ERR(inode))
			return ERR_CAST(inode);

		pr_debug("found '%pd' region=%u\n", dentry, region_id);
	}

	return d_splice_alias(inode, dentry);
}

/* ============================================================================
 * namei_create - lightweight file creation (two-phase model)
 * ============================================================================
 *
 * Reserves a RAT entry and inserts the name. No physical space yet: ftruncate is what places the
 * region and fills the offset and size in.
 */

/* Caller holds the metadata lock. A failed index insert hands the RAT entry back here.
 *
 * The slot, the name and the owner identity land under one lock: publishing the name first would
 * let a peer grant on a line that still names nobody.
 */
static s32 __namei_create_locked(struct fs_sb_info *sbi, struct dentry *dentry, umode_t mode,
				 const struct fs_daemon_attest_response *identity, u32 slot_hint,
				 u32 *out_rat_entry_id)
{
	/* Judged inside the lock: the wait for it sleeps, and a fence that landed meanwhile means
	 * the lines below carry a node_id that is another node's now. */
	if (fs_is_fenced(sbi))
		return -EIO;

	/* The mode, the caller's ids and the attested group and role ride in with the claim, so the
	 * hot and acl lines are each written once. */
	u32 rat_entry_id;
	s32 ret = region_alloc_rat_entry(sbi, dentry->d_name.name, 0, 0, slot_hint, mode, identity, &rat_entry_id);
	if (ret) {
		pr_err("RAT entry reservation failed: %d\n", ret);
		return ret;
	}

	if (test_crash_here(sbi, TEST_CRASH_CREATE_BEFORE_NAME))
		return -EIO;

	ret = index_insert(sbi, dentry->d_name.name, dentry->d_name.len, rat_entry_id);
	if (ret) {
		pr_err("index insert failed: %d\n", ret);
		region_free_rat_entry(sbi, layout_get_rat_entry(sbi, rat_entry_id));
		return ret;
	}

	*out_rat_entry_id = rat_entry_id;
	return 0;
}

/* Build VFS inode and bind fs_inode_info to the freshly reserved RAT slot. */
static struct inode *__namei_create_inode(struct super_block *sb, struct fs_sb_info *sbi, umode_t mode,
					  u32 rat_entry_id)
{
	struct inode *inode = inode_new(sb, mode);
	if (IS_ERR(inode))
		return inode;

	struct fs_inode_info *xi = fs_get_inode_info(inode);
	xi->region_id = rat_entry_id;
	xi->rat_entry_id = rat_entry_id;
	xi->region_offset = 0;
	xi->owner_node_id = sbi->node_id;
	xi->owner_pid = current->tgid;
	xi->owner_birth_time = acl_read_caller_birth_time();
	xi->data_phys_offset = 0; /* set by ftruncate */

	inode->i_ino = inode_make_ino(rat_entry_id);
	inode->i_size = 0;
	inode->i_op = &inode_iops;
	inode->i_fop = &file_fops;
	return inode;
}

/*
 * The identity this create stamps, asked for before anything is allocated. Outside the lock on
 * purpose: the upcall sleeps, and holding the region while a policy engine answers would make
 * every peer wait for it.
 */
static s32 __namei_attest_identity(struct fs_sb_info *sbi, struct fs_daemon_attest_response *identity)
{
	if (test_daemon_is_stubbed(sbi->node_id)) {
		identity->status = 0;
		strscpy(identity->group, "test", sizeof(identity->group));
		strscpy(identity->role, "test", sizeof(identity->role));
		return 0;
	}

	union daemon_req_payload areq = {};
	union daemon_resp_payload aresp = {};

	daemon_fill_task(DAEMON_REQ_ATTEST, &areq);

	pr_debug("attest enqueue: pid=%d\n", current->tgid);

	s32 ret = daemon_request(sbi, DAEMON_REQ_ATTEST, &areq, &aresp);
	if (ret == -ENOSYS) {
		pr_warn_ratelimited("attest: no helper on this mount, so create is rejected\n");
		return -EAGAIN;
	}
	if (ret == -ETIMEDOUT) {
		pr_warn_ratelimited("attest: helper response timed out — pid=%d\n", current->tgid);
		return -ETIMEDOUT;
	}
	if (ret) {
		/* Transport-layer failure (enqueue OOM, late wakeup, etc.).
		 * Treat as transient: retry once the helper or queue recovers. */
		pr_warn("attest failed: pid=%d ret=%d\n", current->tgid, ret);
		return -EAGAIN;
	}
	if (aresp.attest.status) {
		/* A denial is permanent for this caller, unlike the transient cases above. */
		pr_warn("attest deny: pid=%d status=%d\n", current->tgid, aresp.attest.status);
		return -EACCES;
	}

	*identity = aresp.attest;
	pr_debug("attest ok: pid=%d group='%.16s' role='%.16s'\n", current->tgid, identity->group,
		 identity->role);
	return 0;
}

static s32 namei_create(COMPAT_IDMAP_PARAM_COMMA struct inode *dir, struct dentry *dentry, umode_t mode, bool excl)
{
	struct inode_ctx parent;
	if (!inode_open_ctx(&parent, dir))
		return -EIO;

	if (dir->i_ino != FS_ROOT_INO)
		return -ENOENT;
	if (dentry->d_name.len > FS_NAME_MAX)
		return -ENAMETOOLONG;
	/* Reserved: an index entry under this name would shadow the lock region,
	 * which lookup resolves without ever reading a bucket. */
	if (layout_check_lock_region_name(dentry->d_name.name, dentry->d_name.len))
		return -EPERM;
	/* A create stamps owner_node_id, and 0 is the encoding for a crash orphan,
	 * so the admin node would reclaim what we just made. */
	if (fs_is_identity_pending(parent.sbi))
		return -EAGAIN;

	if (!parent.sbi->rat) {
		pr_err("RAT not initialized\n");
		return -ENOSPC;
	}

	pr_debug("creating file '%pd' (lightweight, no physical space)\n", dentry);

	/* The identity first, and with no lock held: a denial then costs nothing to undo. */
	struct fs_daemon_attest_response identity = {};
	s32 ret = __namei_attest_identity(parent.sbi, &identity);
	if (ret)
		return ret;

	/* One lock for what a peer sees as one change: the slot, its name and its owner. The free
	 * slot is looked for while the helper takes the turn, since that read excludes nobody and
	 * the allocator confirms the slot once the turn is held. */
	struct meta_lock lock;
	ret = meta_lock_begin(parent.sbi, &lock, TEST_META_OP_CREATE);
	if (ret)
		return ret;
	const u32 slot_hint = region_find_free_rat_entry(parent.sbi);
	ret = meta_lock_finish(parent.sbi, &lock);
	if (ret)
		return ret;

	/* Read under the mutex, which a drain takes once after raising the flag, so a create that
	 * missed the flag has written its entry before the drain's scan starts. */
	if (fs_is_draining(parent.sbi)) {
		meta_unlock(parent.sbi, &lock);
		return -EBUSY;
	}

	u32 rat_entry_id;
	ret = __namei_create_locked(parent.sbi, dentry, mode, &identity, slot_hint, &rat_entry_id);
	if (ret) {
		meta_unlock(parent.sbi, &lock);
		return ret;
	}

	/* The VFS inode, not yet d_instantiated, and still under the lock: the undo below must not
	 * let a create take the slot between the index delete and the RAT free, and holding what we
	 * already have is what makes that undo unable to fail. A slab allocation is small next to
	 * the CXL work above it. */
	struct inode *inode = __namei_create_inode(parent.sb, parent.sbi, mode, rat_entry_id);
	if (IS_ERR(inode)) {
		index_delete(parent.sbi, dentry->d_name.name, dentry->d_name.len);
		region_free_rat_entry(parent.sbi, layout_get_rat_entry(parent.sbi, rat_entry_id));
		meta_unlock(parent.sbi, &lock);
		pr_err("inode creation failed: %ld\n", PTR_ERR(inode));
		return PTR_ERR(inode);
	}

	meta_unlock(parent.sbi, &lock);

	pr_debug("reserved RAT entry %u for '%pd' (ftruncate pending)\n", rat_entry_id, dentry);
	d_instantiate(dentry, inode);

	return 0;
}

/* ============================================================================
 * namei_unlink - taking a name out of the global index
 * ============================================================================ */

static s32 namei_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode_ctx ctx;
	if (!inode_open_ctx(&ctx, d_inode(dentry)))
		return -EIO;

	pr_debug("unlink '%pd' rat_entry=%u\n", dentry, ctx.xi->rat_entry_id);

	/* The lock region has no index entry to delete, and every node's daemon
	 * is mapping it. Refused ahead of the permission check to say why. */
	if (layout_check_lock_region_name(dentry->d_name.name, dentry->d_name.len))
		return -EPERM;

	/* Freeing a RAT slot without an identity would let this node hand back
	 * what its own GC cannot then be held to. */
	if (fs_is_identity_pending(ctx.sbi))
		return -EAGAIN;

	s32 ret = acl_check_permission(ctx.sbi, ctx.xi->rat_entry_id, FS_PERM_DELETE);
	if (ret) {
		if (!gc_can_force_unlink(ctx.sbi, ctx.xi->rat_entry_id)) {
			pr_err("no delete permission for rat_entry %u\n", ctx.xi->rat_entry_id);
			return -EACCES;
		}
	}

	/* Taking the name out and freeing the RAT entry must not interleave with a create that
	 * could re-allocate the entry between the two halves. */
	struct meta_lock lock;
	ret = meta_lock(ctx.sbi, &lock, TEST_META_OP_UNLINK);
	if (ret)
		return ret;

	ret = index_delete(ctx.sbi, dentry->d_name.name, dentry->d_name.len);
	if (ret) {
		meta_unlock(ctx.sbi, &lock);
		pr_err("index delete failed: %d\n", ret);
		return ret;
	}

	if (test_crash_here(ctx.sbi, TEST_CRASH_UNLINK_BEFORE_FREE))
		return -EIO;

	/* The name goes before the bits are read, and an open sets its bit before it reads the
	 * name, so whichever of the two came second sees the other. */
	struct layout_rat_entry *entry = layout_get_rat_entry(ctx.sbi, ctx.xi->rat_entry_id);
	if (entry)
		layout_write_rat_name(entry, "", 0);

	/* A reference on any node keeps the extent, and the owner's sweep frees it when the last one
	 * goes. A false answer from the free is GC having freed the slot already. */
	if (!region_has_refs(ctx.sbi, ctx.xi->rat_entry_id))
		region_free_rat_entry(ctx.sbi, entry);
	else
		pr_debug("unlink '%pd' leaves rat_entry %u to its holders\n",
			 dentry, ctx.xi->rat_entry_id);

	meta_unlock(ctx.sbi, &lock);

	drop_nlink(ctx.inode);
	inode_set_ctime_to_ts(ctx.inode, current_time(ctx.inode));

	pr_debug("unlinked '%pd'\n", dentry);

	return 0;
}

const struct inode_operations dir_iops = {
	.permission = inode_check_permission,
	.lookup = namei_lookup,
	.create = namei_create,
	.unlink = namei_unlink,
};
