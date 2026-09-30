// SPDX-License-Identifier: GPL-2.0-only
/*
 * super.c - the partitioned global index superblock and mount
 *
 * Mount validates the on-medium superblock, caches its geometry in sbi, settles this node's
 * identity, and starts the GC thread that also drives the liveness tick. Ordering between nodes
 * is the application's problem: nothing here arbitrates across hosts.
 */

#include <linux/blkdev.h>
#include <linux/crc32.h>
#include <linux/dax.h>
#include <linux/dma-buf.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/iversion.h>
#include <linux/log2.h>
#include <linux/module.h>
#include <linux/parser.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/statfs.h>
#include <linux/time64.h>
#include <linux/uio.h>
#include <linux/vmalloc.h>

#include <linux/pgtable.h>

#include "access/acl.h"
#include "access/daemon.h"
#include "compat.h"
#include "core.h"
#include "cxl/dax_area.h"
#include "layout/layout_access.h"
#include "lifecycle/bootstrap.h"
#include "lifecycle/gc.h"
#include "region/refs.h"
#include "region/region.h"
#include "sysfs/sysfs.h"
#include "test/test_hooks.h"
#include "vfs/dir.h"
#include "vfs/inode.h"

/*
 * Everything a mount allocates beyond the sbi itself, in one pair. Each subsystem still owns the
 * size and type of its own buffer; what lives here is the order the mount takes and drops them in.
 */
static s32 fs_alloc_mount_resources(struct fs_sb_info *sbi)
{
	sbi->shard_cache = kvmalloc_array(LAYOUT_NUM_SHARDS, sizeof(struct fs_shard_cache), GFP_KERNEL | __GFP_ZERO);
	if (!sbi->shard_cache)
		return -ENOMEM;

	s32 ret = region_alloc_scratch(sbi);
	if (ret)
		return ret;

	ret = region_alloc_refs(sbi);
	if (ret)
		return ret;

	return gc_init(sbi);
}

/* Every free below takes a NULL, so a half-built mount unwinds through this one call. */
static void fs_free_mount_resources(struct fs_sb_info *sbi)
{
	gc_exit(sbi);
	region_free_refs(sbi);
	region_free_scratch(sbi);
	kvfree(sbi->shard_cache);
	sbi->shard_cache = NULL;
}

enum SUPER_MOUNT_OPTION {
	OPTION_NODE_ID,
	OPTION_DAXDEV,
	OPTION_FORMAT,
	OPTION_GRANULE_SHIFT,
	OPTION_UC_MIB,
	OPTION_DAEMON_UID,
	OPTION_DAEMON_GID,
	OPTION_ERROR,
};

static const match_table_t super_mount_tokens = {
	{ OPTION_NODE_ID, "node_id=%d" },
	{ OPTION_DAXDEV, "daxdev=%s" },
	{ OPTION_FORMAT, "format" },
	{ OPTION_GRANULE_SHIFT, "granule_shift=%u" },
	{ OPTION_UC_MIB, "uc_mib=%u" },
	{ OPTION_DAEMON_UID, "daemon_uid=%u" },
	{ OPTION_DAEMON_GID, "daemon_gid=%u" },
	{ OPTION_ERROR, NULL },
};

/* @granule_shift and @uc_mib need format=, since a mount that assumed a split the device lacks
 * would map the pools somewhere else. */
struct super_mount_opts {
	s32 node_id; /* 0 = auto-mount (no node_id= given); ≥1 = manual */
	char daxdev[128];
	bool format; /* in-kernel format on mount */
	u32 granule_shift; /* 0 = the default */
	u32 uc_mib; /* uncached pool in MiB, 0 = the default */
	u32 daemon_uid; /* FS_TOOLS_ANY_ID unless daemon_uid_given names it */
	bool daemon_uid_given;
	u32 daemon_gid; /* FS_TOOLS_ANY_ID unless daemon_gid_given names it */
	bool daemon_gid_given;
};

static s32 super_parse_options(char *options, struct super_mount_opts *opts)
{
	opts->node_id = 0;
	opts->daxdev[0] = '\0';
	opts->format = false;
	opts->granule_shift = 0;
	opts->uc_mib = 0;
	opts->daemon_uid = FS_TOOLS_ANY_ID;
	opts->daemon_uid_given = false;
	opts->daemon_gid = FS_TOOLS_ANY_ID;
	opts->daemon_gid_given = false;

	if (!options)
		return 0;

	substring_t args[MAX_OPT_ARGS];
	char *part;
	while ((part = strsep(&options, ",")) != NULL) {
		if (!*part)
			continue;

		s32 token = match_token(part, super_mount_tokens, args);
		switch (token) {
		case OPTION_NODE_ID: {
			s32 option;
			if (match_int(&args[0], &option))
				return -EINVAL;
			if (option <= 0) {
				pr_err("node_id must be > 0 (got %d)\n", option);
				return -EINVAL;
			}
			opts->node_id = option;
			break;
		}
		case OPTION_DAXDEV:
			match_strlcpy(opts->daxdev, &args[0], sizeof(opts->daxdev));
			break;
		case OPTION_FORMAT:
			opts->format = true;
			break;
		case OPTION_GRANULE_SHIFT: {
			s32 option;
			if (match_int(&args[0], &option))
				return -EINVAL;
			if (!layout_check_granule_shift((u32)option)) {
				pr_err("granule_shift must be in [%u, %u] (got %d)\n",
				       LAYOUT_GRANULE_SHIFT_MIN, LAYOUT_GRANULE_SHIFT_MAX, option);
				return -EINVAL;
			}
			opts->granule_shift = (u32)option;
			break;
		}
		case OPTION_UC_MIB: {
			s32 option;
			if (match_int(&args[0], &option))
				return -EINVAL;
			if (option <= 0) {
				pr_err("uc_mib must be > 0 (got %d)\n", option);
				return -EINVAL;
			}
			opts->uc_mib = (u32)option;
			break;
		}
		/* match_uint and not match_int: an id is unsigned, and the sentinel that leaves the
		 * field beside it deciding is above INT_MAX. */
		case OPTION_DAEMON_UID: {
			u32 option;
			if (match_uint(&args[0], &option))
				return -EINVAL;
			opts->daemon_uid = option;
			opts->daemon_uid_given = true;
			break;
		}
		case OPTION_DAEMON_GID: {
			u32 option;
			if (match_uint(&args[0], &option))
				return -EINVAL;
			opts->daemon_gid = option;
			opts->daemon_gid_given = true;
			break;
		}
		default:
			pr_err("unrecognized mount option: %s\n", part);
			return -EINVAL;
		}
	}

	if (!opts->format && (opts->granule_shift || opts->uc_mib)) {
		pr_err("granule_shift= and uc_mib= describe the medium, so they need format\n");
		return -EINVAL;
	}

	/* Both left open would name everyone, which is what naming an account exists to end. */
	if ((opts->daemon_uid_given || opts->daemon_gid_given) &&
	    opts->daemon_uid == FS_TOOLS_ANY_ID &&
	    opts->daemon_gid == FS_TOOLS_ANY_ID) {
		pr_err("daemon_uid= and daemon_gid= cannot both be %u\n", FS_TOOLS_ANY_ID);
		return -EINVAL;
	}

	return 0;
}

static struct kmem_cache *inode_cachep;

/* ============================================================================
 * inode cache management
 * ============================================================================ */

static struct inode *super_alloc_inode(struct super_block *sb)
{
	struct fs_inode_info *xi = kmem_cache_alloc(inode_cachep, GFP_KERNEL);
	if (!xi)
		return NULL;

	fs_init_inode_info(xi);

	return &xi->vfs_inode;
}

static void super_free_inode(struct inode *inode)
{
	kmem_cache_free(inode_cachep, fs_get_inode_info(inode));
}

/* ============================================================================
 * Filesystem statistics
 * ============================================================================ */

static s32 super_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct fs_sb_info *sbi = fs_get_sb_info(dentry->d_sb);
	if (sbi == NULL || fs_is_fenced(sbi))
		return -EIO;

	struct region_pool_usage usage;
	s32 ret = region_get_pool_usage(sbi, &usage);
	if (ret)
		return ret;

	/* statfs has one set of fields, so the two pools are summed here. A pool that is full
	 * beside one that is empty shows only in the per-pool sysfs file. */
	u64 total = usage.uc_total + usage.wb_total;
	u64 used_size = usage.uc_used + usage.wb_used;

	buf->f_files = LAYOUT_MAX_RAT_ENTRIES;
	buf->f_ffree = LAYOUT_MAX_RAT_ENTRIES - usage.entries;

	buf->f_type = LAYOUT_MAGIC;
	buf->f_bsize = PAGE_SIZE;
	buf->f_blocks = total / PAGE_SIZE;
	if (used_size > total)
		buf->f_bfree = 0;
	else
		buf->f_bfree = (total - used_size) / PAGE_SIZE;
	buf->f_bavail = buf->f_bfree;
	buf->f_namelen = FS_NAME_MAX;

	return 0;
}

/* ============================================================================
 * Superblock operations table
 * ============================================================================ */

static s32 super_show_options(struct seq_file *m, struct dentry *root)
{
	struct fs_sb_info *sbi = root->d_sb->s_fs_info;

	seq_printf(m, ",node_id=%u", sbi->node_id);

	if (sbi->daxdev_path[0])
		seq_printf(m, ",daxdev=%s", sbi->daxdev_path);
	else if (sbi->meta_base != NULL)
		seq_puts(m, ",dax");

	return 0;
}

/* ============================================================================
 * Dentry operations - dcache invalidation for cross-node visibility
 * ============================================================================
 *
 * Another node can create or delete a file at any time, so revalidation always fails and the VFS
 * calls lookup again rather than trusting a cached dentry.
 */

static s32 super_d_revalidate(COMPAT_D_REVALIDATE_ARGS)
{
	return 0;
}

static const struct dentry_operations super_dentry_ops = {
	.d_revalidate = super_d_revalidate,
};

/* ============================================================================
 * Read and validate Global Superblock
 * ============================================================================ */

static s32 super_read_superblock(struct fs_sb_info *sbi, struct super_block *sb, s32 silent)
{
	struct layout_superblock *gsb = layout_get_superblock(sbi);
	if (!gsb)
		return -EINVAL;

	/* One read for everything this validates and then caches. */
	union layout_superblock_head_copy head;
	cxl_get_layout_superblock_head(&head, &gsb->head);
	if (head.local.magic != LAYOUT_MAGIC) {
		if (!silent)
			pr_err("invalid magic 0x%x (expected 0x%x)\n", head.local.magic, LAYOUT_MAGIC);
		return -EINVAL;
	}

	if (head.local.version != LAYOUT_VERSION) {
		if (!silent)
			pr_err("unsupported version %u (expected %u, reformat required)\n",
			       head.local.version, LAYOUT_VERSION);
		return -EINVAL;
	}

	u32 stored = head.local.checksum;
	u32 computed = layout_compute_superblock_checksum(&head);
	if (stored != computed) {
		if (!silent)
			pr_err("superblock checksum mismatch (stored=0x%x, computed=0x%x)\n", stored, computed);
		return -EINVAL;
	}

	sbi->total_size = head.local.total_size;
	sbi->num_shards = head.local.num_shards;
	if (sbi->num_shards == 0 || !is_power_of_2(sbi->num_shards)) {
		pr_err("num_shards %u is not a power of 2\n", sbi->num_shards);
		return -EINVAL;
	}
	sbi->shard_mask = sbi->num_shards - 1;
	sbi->buckets_per_shard = head.local.buckets_per_shard;
	if (sbi->buckets_per_shard == 0 || !is_power_of_2(sbi->buckets_per_shard)) {
		pr_err("buckets_per_shard %u is not a power of 2\n", sbi->buckets_per_shard);
		return -EINVAL;
	}
	sbi->bucket_mask = sbi->buckets_per_shard - 1;
	sbi->pool_lines_per_shard = head.local.pool_lines_per_shard;
	sbi->shard_table_offset = head.local.shard_table_offset;
	sbi->rat_offset = head.local.rat_offset;

	if (!layout_check_granule_shift(head.local.granule_shift)) {
		pr_err("granule shift %u is outside [%u, %u]\n", head.local.granule_shift,
		       LAYOUT_GRANULE_SHIFT_MIN, LAYOUT_GRANULE_SHIFT_MAX);
		return -EINVAL;
	}
	sbi->granule = 1ULL << head.local.granule_shift;
	/* The metadata sits below both pools, so the first region starts at the granule past it. */
	sbi->uc_start = layout_align_up(LAYOUT_DATA_OFFSET, sbi->granule);

	u64 boundary = sbi->uc_start + (u64)head.local.uc_granules * sbi->granule;
	if (boundary >= sbi->total_size) {
		pr_err("an uncached pool of %u granules leaves no write-back area\n",
		       head.local.uc_granules);
		return -EINVAL;
	}
	/* The areas were mapped from a read of this same line, so a difference means another node
	 * reformatted the device in between and the pools sit outside what is mapped. */
	if (boundary != sbi->wb_start) {
		pr_err("the areas are split at 0x%llx but the superblock says 0x%llx\n",
		       sbi->wb_start, boundary);
		return -EINVAL;
	}

	pr_debug("superblock validated\n");
	pr_debug("  total_size=%llu, shards=%u, buckets/shard=%u, pool/shard=%u\n",
		 sbi->total_size, sbi->num_shards,
		 sbi->buckets_per_shard, sbi->pool_lines_per_shard);

	return 0;
}

/* ============================================================================
 * Initialize Shard Table
 * ============================================================================ */

static s32 super_init_shard_table(struct fs_sb_info *sbi)
{
	u64 shard_table_offset = sbi->shard_table_offset;
	if (!layout_is_valid_range(sbi, shard_table_offset, (u64)sbi->num_shards * LAYOUT_SHARD_HEADER_SIZE)) {
		pr_err("shard_table_offset 0x%llx out of range\n", shard_table_offset);
		return -EINVAL;
	}

	WARN_ON_ONCE(shard_table_offset != LAYOUT_SHARD_TABLE_OFFSET);

	/* The header pointers are already in place, so this loop validates each header and fills
	 * the bucket and pool pointers that only a validated header can supply. */
	for (u32 i = 0; i < sbi->num_shards; i++) {
		struct layout_shard_header *sh = layout_get_shard_header(sbi, i);
		if (!sh) {
			pr_err("shard_cache header NULL at %u\n", i);
			return -EINVAL;
		}

		/* One read for the four this loop checks, on a header that is uncached. */
		union layout_shard_header_copy copy;
		cxl_get_layout_shard_header(&copy, sh);
		if (copy.local.magic != LAYOUT_SHARD_MAGIC) {
			pr_err("bad shard magic at %u (0x%x)\n", i, copy.local.magic);
			return -EINVAL;
		}
		if (copy.local.shard_id != i) {
			pr_err("shard_id mismatch at %u (got %u)\n", i, copy.local.shard_id);
			return -EINVAL;
		}

		u32 buckets = copy.local.num_buckets;
		u32 pool_lines = copy.local.num_pool_lines;
		if (!layout_is_valid_shard_geometry(buckets, pool_lines)) {
			pr_err("shard %u geometry invalid: buckets=%u pool=%u (superblock says %u and %u)\n",
			       i, buckets, pool_lines, sbi->buckets_per_shard, sbi->pool_lines_per_shard);
			return -EINVAL;
		}

		u64 bucket_off = copy.local.bucket_array_offset;
		u64 pool_off = copy.local.pool_array_offset;

		if (!layout_is_valid_range(sbi, bucket_off, (u64)buckets * LAYOUT_INDEX_LINK_SIZE) ||
		    !layout_is_valid_range(sbi, pool_off, (u64)pool_lines * LAYOUT_INDEX_LINK_SIZE)) {
			pr_err("shard %u: offset out of bounds (bucket=%llu pool=%llu total=%llu)\n",
			       i, bucket_off, pool_off, sbi->total_size);
			return -EINVAL;
		}

		sbi->shard_cache[i].buckets = layout_get_meta_ptr(sbi, bucket_off);
		sbi->shard_cache[i].pool = layout_get_meta_ptr(sbi, pool_off);
	}

	pr_debug("shard table initialized (%u shards validated, RAM counters + shard cache allocated)\n", sbi->num_shards);

	return 0;
}

/* ============================================================================
 * Load and validate RAT (Region Allocation Table)
 * ============================================================================ */

static s32 super_load_rat(struct fs_sb_info *sbi)
{
	u64 rat_offset = sbi->rat_offset;
	if (rat_offset == 0) {
		pr_err("RAT not present! rat_offset=0 (filesystem too old or corrupted)\n");
		return -EINVAL;
	}

	if (!layout_is_valid_range(sbi, rat_offset, sizeof(struct layout_rat))) {
		pr_err("rat_offset 0x%llx out of range\n", rat_offset);
		return -EINVAL;
	}

	WARN_ON_ONCE(rat_offset != LAYOUT_RAT_OFFSET);

	/* sbi->rat already cached by fs_init_layout_ptrs at mount start. */
	struct layout_rat *rat = layout_get_rat(sbi);
	if (!rat)
		return -EINVAL;

	/* One read for the header this mount validates and then logs. */
	union layout_rat_head_copy head;
	cxl_get_layout_rat_head(&head, &rat->head);
	if (head.local.magic != LAYOUT_RAT_MAGIC) {
		pr_err("invalid RAT magic 0x%x (expected 0x%x) at offset 0x%llx\n", head.local.magic, LAYOUT_RAT_MAGIC,
		       rat_offset);
		return -EINVAL;
	}

	if (head.local.version != 1) {
		pr_err("unsupported RAT version %u (expected 1)\n", head.local.version);
		return -EINVAL;
	}

	pr_debug("RAT loaded at offset 0x%llx, RAT pointer=%p\n", rat_offset, rat);
	pr_debug("  device_size=%llu, regions_start=0x%llx\n", head.local.device_size, head.local.regions_start);

	test_blank_summary(sbi);
	region_check_map(sbi);
	if (!sbi->rat_map_trusted)
		pr_warn("RAT summary disagrees with its entries; searching entries until GC repairs it\n");

	return 0;
}

/* ============================================================================
 * Create root directory inode
 * ============================================================================ */

static struct inode *super_make_root_inode(struct super_block *sb)
{
	struct inode *inode = new_inode(sb);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	inode->i_ino = FS_ROOT_INO;
	inode->i_mode = S_IFDIR | FS_ROOT_DIR_MODE;
	inode->i_uid = GLOBAL_ROOT_UID;
	inode->i_gid = GLOBAL_ROOT_GID;

	inode_set_atime_to_ts(inode, current_time(inode));
	inode_set_mtime_to_ts(inode, current_time(inode));
	inode_set_ctime_to_ts(inode, current_time(inode));

	inode->i_op = &dir_iops;
	inode->i_fop = &dir_fops;

	set_nlink(inode, 2);

	struct fs_inode_info *xi = fs_get_inode_info(inode);
	xi->region_id = 0;

	return inode;
}

/* Narrows the lock region to daemon_uid=/daemon_gid=. A device carrying no lock region yet leaves
 * this a warning rather than a mount failure, because a remount is the way in once one stands. */
static s32 super_bind_daemon_account(struct fs_sb_info *sbi, const struct super_mount_opts *opts)
{
	if (!opts->daemon_uid_given && !opts->daemon_gid_given)
		return 0;

	struct layout_rat_entry *rat_entry = layout_get_rat_entry(sbi, FS_LOCK_REGION_RAT_ID);
	if (!rat_entry) {
		pr_warn("lock region: daemon_uid=%u gid=%u given, and no entry stands at its slot\n",
			opts->daemon_uid, opts->daemon_gid);
		return 0;
	}

	union layout_rat_hot_copy hot;
	cxl_get_layout_rat_hot(&hot, &rat_entry->hot);
	if (hot.local.region_type != LAYOUT_REGION_LOCK) {
		pr_warn("lock region: daemon_uid=%u gid=%u given, and its slot carries region type %u\n",
			opts->daemon_uid, opts->daemon_gid, hot.local.region_type);
		return 0;
	}

	s32 ret = acl_set_lock_region_account(sbi, rat_entry, opts->daemon_uid, opts->daemon_gid,
					      FS_PERM_READ | FS_PERM_WRITE);
	if (ret) {
		pr_err("lock region: account row for uid=%u gid=%u refused: %d\n", opts->daemon_uid,
		       opts->daemon_gid, ret);
		return ret;
	}

	/* READ and not 0: every later bring-up reads this region's header, and that read is
	 * nobody's delegate. A joining node finds it narrowed already, so this runs once. */
	union layout_rat_acl_copy acl;
	cxl_get_layout_rat_acl(&acl, &rat_entry->acl);
	if (acl.local.default_perms != FS_PERM_READ) {
		acl.local.default_perms = FS_PERM_READ;
		cxl_set_layout_rat_acl(&rat_entry->acl, &acl);
	}

	pr_info("lock region bound to uid=%u gid=%u on node %u\n",
		opts->daemon_uid, opts->daemon_gid, sbi->node_id);
	return 0;
}

/* Writes the daemon_uid=/daemon_gid= row on MS_REMOUNT, which is the way in for a device whose
 * lock region did not exist when this node first mounted. node_id is already settled on sbi. */
static s32 super_remount_fs(struct super_block *sb, s32 *mount_flags, char *data)
{
	struct fs_sb_info *sbi = fs_get_sb_info(sb);
	if (!sbi)
		return -EINVAL;

	struct super_mount_opts opts;
	s32 ret = super_parse_options(data, &opts);
	if (ret)
		return ret;

	return super_bind_daemon_account(sbi, &opts);
}

/*
 * No .write_inode: ftruncate is the only operation that decides a new size, and it
 * publishes to the RAT itself. A writeback pass would re-publish what that call
 * already wrote, on a thread no cross-node turn can cover.
 */
static const struct super_operations super_sb_ops = {
	.alloc_inode = super_alloc_inode,
	.free_inode = super_free_inode,
	.evict_inode = inode_evict,
	.drop_inode = COMPAT_DROP_INODE_ALWAYS,
	.statfs = super_statfs,
	.show_options = super_show_options,
	.remount_fs = super_remount_fs,
};

/* ============================================================================
 * Common mount handling for DEV_DAX
 * ============================================================================ */

static s32 super_fill_super_common(struct super_block *sb, struct fs_sb_info *sbi, s32 silent)
{
	s32 ret = super_read_superblock(sbi, sb, silent);
	if (ret) {
		pr_err("failed to read superblock\n");
		pr_err("mount with 'format' option to initialize: mount -t %s -o daxdev=...,node_id=%d,format none /mnt/...\n",
		       KBUILD_MODNAME, sbi->node_id);
		return ret;
	}

	ret = super_init_shard_table(sbi);
	if (ret) {
		pr_err("failed to initialize shard table\n");
		goto err_free_gsb;
	}

	ret = super_load_rat(sbi);
	if (ret) {
		pr_err("failed to load RAT\n");
		goto err_free_gsb;
	}

	/* No region setup here: a RAT entry is taken when a file is created. */
	sb->s_magic = LAYOUT_MAGIC;
	sb->s_blocksize = FS_VFS_BLOCK_SIZE;
	sb->s_blocksize_bits = FS_VFS_BLOCK_SIZE_BITS;
	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_op = &super_sb_ops;
	COMPAT_SET_D_OP(sb, &super_dentry_ops);
	sb->s_time_gran = 1;

	struct inode *root_inode = super_make_root_inode(sb);
	if (IS_ERR(root_inode)) {
		ret = PTR_ERR(root_inode);
		goto err_free_gsb;
	}

	struct dentry *root_dentry = d_make_root(root_inode);
	if (!root_dentry) {
		ret = -ENOMEM;
		goto err_free_gsb;
	}

	sb->s_root = root_dentry;

	ret = sysfs_register(sbi);
	if (ret)
		pr_warn("failed to register sysfs: %d\n", ret);

	/* The GC thread also drives the liveness tick. A mount still waiting for its identity has
	 * no slot to stamp and no node_id to filter on, so the claim ioctl starts it instead. */
	if (!fs_is_identity_pending(sbi)) {
		ret = gc_start(sbi);
		if (ret) {
			/* Fatal, not a warning: with no tick this slot goes stale, a peer takes the
			 * node_id, and the tick that would have noticed is the one that never ran. */
			pr_err("failed to start gc thread: %d\n", ret);
			goto err_unregister_sysfs;
		}
	}

	pr_info("filesystem mounted (node=%u, shards=%u)\n", sbi->node_id, sbi->num_shards);
	return 0;

err_unregister_sysfs:
	/* The first failure that can follow sysfs_register, so it is the first that has to undo it:
	 * the list would otherwise keep an entry the caller is about to free. */
	sysfs_unregister(sbi);
err_free_gsb:
	fs_free_mount_resources(sbi);

	return ret;
}

/* ============================================================================
 * Auto-mount helpers (called from super_fill_super)
 * ============================================================================ */

/*
 * Every offset here is fixed at compile time, so all three pointers are good the moment
 * sbi->meta_base is, which is before a format or a superblock read has put anything there. A
 * caller dereferencing one still has to establish that the content is initialised.
 */
static void fs_init_layout_ptrs(struct fs_sb_info *sbi)
{
	sbi->gsb = layout_get_meta_ptr(sbi, LAYOUT_SUPERBLOCK_OFFSET);
	sbi->bootstrap_slots = layout_get_meta_ptr(sbi, LAYOUT_BOOTSTRAP_AREA_OFFSET);
	sbi->rat = layout_get_meta_ptr(sbi, LAYOUT_RAT_OFFSET);

	/* Only the header pointers are derivable from an offset. The bucket and pool pointers come
	 * out of a validated header, so they are filled once the shard table is read. */
	for (u32 i = 0; i < LAYOUT_NUM_SHARDS; i++) {
		u64 off = LAYOUT_SHARD_TABLE_OFFSET + (u64)i * LAYOUT_SHARD_HEADER_SIZE;
		sbi->shard_cache[i].header = layout_get_meta_ptr(sbi, off);
	}
}

/* ============================================================================
 * Mount handling - super_fill_super (DEV_DAX)
 * ============================================================================ */

static s32 super_fill_super(struct super_block *sb, void *data, s32 silent)
{
	struct super_mount_opts opts;
	s32 ret = super_parse_options((char *)data, &opts);
	if (ret)
		return ret;

	struct fs_sb_info *sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;

	sb->s_fs_info = sbi;
	sbi->sb = sb;
	sbi->bootstrap_slot_idx = -1; /* not yet claimed */
	mutex_init(&sbi->vm_ops_lock);
	mutex_init(&sbi->meta_lock);
	mutex_init(&sbi->refs_publish_lock);
	mutex_init(&sbi->drain_mutex);

	/* Only an explicit node_id is checked here. Auto-mount takes its own from the claim below. */
	if (opts.node_id != 0) {
		if (opts.node_id > FS_MAX_NODE_ID) {
			pr_err("node_id=%d invalid (valid range: 1..%d)\n", opts.node_id, FS_MAX_NODE_ID);
			ret = -EINVAL;
			goto err_free_sbi;
		}
		sbi->node_id = opts.node_id;
	}
	if (!opts.daxdev[0]) {
		pr_err("must specify daxdev= mount option\n");
		ret = -EINVAL;
		goto err_free_sbi;
	}
	strscpy(sbi->daxdev_path, opts.daxdev, sizeof(sbi->daxdev_path));
	pr_debug("fill_super DEV_DAX %s (node_id=%d)\n", opts.daxdev, opts.node_id);

	/* Before the areas are mapped, because a format about to run decides where they split. */
	sbi->format_granule_shift = opts.granule_shift ? opts.granule_shift : LAYOUT_GRANULE_SHIFT_DEFAULT;
	if (opts.uc_mib) {
		u64 bytes = (u64)opts.uc_mib << 20;
		u64 count = bytes >> sbi->format_granule_shift;
		sbi->format_uc_granules = count ? (u32)count : 1;
	} else {
		sbi->format_uc_granules = layout_get_default_uc_granules(sbi->format_granule_shift);
	}

	ret = dax_acquire_areas(sbi);
	if (ret) {
		pr_err("DEV_DAX acquisition failed for %s\n", opts.daxdev);
		goto err_free_sbi;
	}

	/* Before the pointers, because one of the buffers is what they are cached into. */
	ret = fs_alloc_mount_resources(sbi);
	if (ret) {
		pr_err("mount allocation failed: %d\n", ret);
		goto err_release_dax;
	}
	fs_init_layout_ptrs(sbi);

	if (opts.node_id == 0)
		ret = bootstrap_auto_mount(sbi);
	else
		ret = bootstrap_manual_mount(sbi, (u32)opts.node_id, opts.format);
	if (ret)
		goto err_release_dax;

	/* Before fill_super_common, because GC starts inside it and the row this writes is what
	 * the region admits from then on. */
	ret = super_bind_daemon_account(sbi, &opts);
	if (ret)
		goto err_bs_release;

	/* Seed admin role cache before fill_super_common (GC starts inside). */
	sbi->cached_admin_node_id = bootstrap_get_current_admin_node_id(sbi);

	/* Before fill_super_common, because GC starts inside it and can upcall. The node id is
	 * only final once the bootstrap above has run. */
	ret = daemon_create_channel(sbi);
	if (ret)
		goto err_bs_release;

	ret = super_fill_super_common(sb, sbi, silent);
	if (ret)
		goto err_daemon;

	return 0;

err_daemon:
	daemon_delete_channel(sbi);
err_bs_release:
	bootstrap_release(sbi);
err_release_dax:
	dax_release_areas(sbi);
err_free_sbi:
	fs_free_mount_resources(sbi);
	kfree(sbi);
	sb->s_fs_info = NULL;
	return ret;
}

/* ============================================================================
 * Mount/unmount callbacks
 * ============================================================================ */

static struct dentry *super_mount(struct file_system_type *fs_type, s32 flags, const char *dev_name, void *data)
{
	/* DEV_DAX carries no block device, so the mount is over an anonymous super. */
	return compat_mount_nodev(fs_type, flags, data, super_fill_super);
}

static void super_kill_sb(struct super_block *sb)
{
	struct fs_sb_info *sbi = fs_get_sb_info(sb);
	if (sbi) {
		/* First, and not last: it takes sysfs_lock, so it both waits out a sysfs write already
		 * working on this sbi and stops the next one from finding it. Everything below assumes
		 * no other context still holds this pointer. */
		sysfs_unregister(sbi);

		/* GC first: it is the only in-kernel caller left, and stopping it before the
		 * channel goes is what keeps it from holding a freed one. */
		gc_stop_thread(sbi);

		/* The rows this node wrote go with it, since no admin recovers a slot that was released.
		 * A fenced mount, or one whose slot an admin already staked, is that admin's to clean. */
		bool slot_is_ours = !fs_is_fenced(sbi) && bootstrap_has_slot(sbi);
		bool rows_gone = slot_is_ours && gc_clear_own_node(sbi);
		daemon_delete_channel(sbi);

		/* A slot kept with its rows still standing is what the admin's timeout recovers, which is
		 * the crash path and reaches every row. A slot released with them would hand the rows to
		 * the id's next holder. */
		if (slot_is_ours && rows_gone)
			bootstrap_release(sbi);
		else if (slot_is_ours)
			pr_warn("node %u leaves its slot held: some rows naming it could not be cleared\n",
				sbi->node_id);

		/* Regions outlive the mount, and so does everything gsb and the shard headers point
		 * at, because those are CXL. Only the DRAM over them is this mount's to free. */
		fs_free_mount_resources(sbi);
		dax_release_areas(sbi);

		kfree(sbi);
	}

	kill_anon_super(sb);

	pr_info("filesystem unmounted\n");
}

/* ============================================================================
 * Filesystem type registration
 * ============================================================================ */

static struct file_system_type super_fs_type = {
	.owner = THIS_MODULE,
	.name = KBUILD_MODNAME,
	.mount = super_mount,
	.kill_sb = super_kill_sb,
	.fs_flags = 0, /* Not FS_REQUIRES_DEV: the mount is over DEV_DAX, not a block device. */
};

/* ============================================================================
 * Inode cache initialization
 * ============================================================================ */

static void super_inode_init_once(void *obj)
{
	struct fs_inode_info *xi = obj;
	inode_init_once(&xi->vfs_inode);
}

/* ============================================================================
 * Module init/exit
 * ============================================================================ */

static s32 __init fs_init(void)
{
	inode_cachep = kmem_cache_create(KBUILD_MODNAME "_inode_cache", sizeof(struct fs_inode_info), 0,
					 SLAB_RECLAIM_ACCOUNT | COMPAT_SLAB_MEM_SPREAD, super_inode_init_once);
	if (!inode_cachep)
		return -ENOMEM;

	s32 ret = register_filesystem(&super_fs_type);
	if (ret)
		goto err_cache;

	ret = sysfs_init();
	if (ret) {
		pr_err("failed to initialize sysfs: %d\n", ret);
		goto err_fs;
	}

	pr_info("Partitioned Global Index filesystem loaded\n");
	return 0;

err_fs:
	unregister_filesystem(&super_fs_type);
err_cache:
	kmem_cache_destroy(inode_cachep);
	return ret;
}

static void __exit fs_exit(void)
{
	pr_info("unloading module\n");

	sysfs_exit();

	unregister_filesystem(&super_fs_type);
	rcu_barrier();

	dax_exit();

	kmem_cache_destroy(inode_cachep);

	pr_info("module unloaded\n");
}

module_init(fs_init);
module_exit(fs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Moonchan Park <mc.park@xcena.com>");
MODULE_DESCRIPTION("Shared filesystem over CXL memory for multi-node access with identity-aware permission delegation");
MODULE_VERSION("1.0");
