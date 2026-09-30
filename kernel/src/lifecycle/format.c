// SPDX-License-Identifier: GPL-2.0-only
/*
 * format.c - lay a filesystem onto a device that does not carry one yet.
 */

#include <linux/fs.h>
#include <linux/ktime.h>
#include <linux/string.h>

#include "core.h"
#include "cxl/io.h"
#include "layout/layout_access.h"
#include "lifecycle/format.h"
#include "test/test_hooks.h"

/*
 * format_write_lock_entry - RAT entry 0, the lock region, written without the index.
 *
 * The index is not usable this early and this name needs none: namei_lookup resolves it to the
 * fixed id. No owner either, since an owner is a process and the helper restarts under a new pid.
 * The bytes go out blank because cme-format leaves a region that already answers alone, so a
 * header from the filesystem that used this device before would otherwise survive into this one.
 */
static void format_write_lock_entry(struct fs_sb_info *sbi)
{
	struct layout_rat_entry *entry = &sbi->rat->entries[FS_LOCK_REGION_RAT_ID];

	cxl_zero_cachelines(layout_get_meta_ptr(sbi, LAYOUT_LOCK_REGION_OFFSET),
			    LAYOUT_LOCK_REGION_SIZE);

	/* Whole lines, not fields: state landing with the rest is what makes the entry valid. */
	union layout_rat_hot_copy hot;
	union layout_rat_acl_copy acl;
	cxl_get_layout_rat_hot(&hot, &entry->hot);
	cxl_get_layout_rat_acl(&acl, &entry->acl);

	layout_write_rat_name(entry, FS_LOCK_REGION_NAME, strlen(FS_LOCK_REGION_NAME));

	const u64 stamp = ktime_get_real_ns();
	hot.local.region_type = LAYOUT_REGION_LOCK;
	hot.local.phys_offset = LAYOUT_LOCK_REGION_OFFSET;
	hot.local.size = LAYOUT_LOCK_REGION_SIZE;
	hot.local.alloc_time = stamp;
	hot.local.modified_at = stamp;
	hot.local.mode = 0666;
	hot.local.state = LAYOUT_RAT_ENTRY_ALLOCATED;
	acl.local.default_perms = FS_PERM_READ | FS_PERM_WRITE;
	/* One slot per node, keyed by node_id, so the bound is the ceiling and never moves. */
	acl.local.deleg_bound = FS_MAX_NODE_ID;

	cxl_set_layout_rat_acl(&entry->acl, &acl);
	cxl_set_layout_rat_hot(&entry->hot, &hot);

	pr_info("format: lock region '%s' rat=%u at 0x%llx size %llu\n",
		FS_LOCK_REGION_NAME, FS_LOCK_REGION_RAT_ID,
		(u64)LAYOUT_LOCK_REGION_OFFSET, (u64)LAYOUT_LOCK_REGION_SIZE);
}

/* Superblock, shard table, bucket lines, index pool and RAT, written onto the mapping itself. */
s32 format_device(struct fs_sb_info *sbi)
{
	void *base = sbi->meta_base;
	u64 total = sbi->total_size;
	u32 num_shards = LAYOUT_NUM_SHARDS;
	u32 pool_lines_per_shard = LAYOUT_POOL_LINES_PER_SHARD;
	u32 buckets_per_shard = LAYOUT_BUCKETS_PER_SHARD;

	pr_debug("formatting DEV_DAX device (%llu bytes, %llu MB)\n", total, total >> 20);

	u64 bucket_array_start = LAYOUT_INDEX_BUCKET_OFFSET;
	u64 total_bucket_bytes = (u64)num_shards * buckets_per_shard * LAYOUT_INDEX_LINK_SIZE;
	u64 pool_array_start = bucket_array_start + total_bucket_bytes;
	u64 total_pool_bytes = (u64)num_shards * pool_lines_per_shard * LAYOUT_INDEX_LINK_SIZE;
	u64 rat_offset = pool_array_start + total_pool_bytes;
	u64 rat_size = sizeof(struct layout_rat);
	u64 rat_end = rat_offset + rat_size;
	u64 regions_start = LAYOUT_DATA_OFFSET;

	if (regions_start >= total) {
		pr_err("device too small for metadata (%llu < %llu)\n", total, regions_start);
		return -ENOSPC;
	}

	if (pool_array_start != LAYOUT_INDEX_POOL_OFFSET ||
	    rat_offset != LAYOUT_RAT_OFFSET ||
	    rat_end > regions_start) {
		pr_err("FORMAT FAILED — layout mismatched (rat_end=0x%llx region=0x%llx)\n",
		       rat_end, regions_start);
		return -EIO;
	}

	/* Two ranges, so the bootstrap area between them keeps its slots: a claimant that is
	 * mounting right now would otherwise lose the one it holds across a formatter restart. */
	cxl_zero_cachelines(base, LAYOUT_SUPERBLOCK_SIZE);
	cxl_zero_cachelines((char *)base + LAYOUT_SHARD_TABLE_OFFSET, regions_start - LAYOUT_SHARD_TABLE_OFFSET);

	/* Cached at mount start, so the pointer is good long before the write at the end. */
	struct layout_superblock *gsb = layout_get_superblock(sbi);
	if (!gsb)
		return -EINVAL;

	for (u32 i = 0; i < num_shards; i++) {
		struct layout_shard_header *sh = layout_get_shard_header(sbi, i);
		if (!sh)
			return -EINVAL;

		union layout_shard_header_copy head = {};
		head.local.magic = LAYOUT_SHARD_MAGIC;
		head.local.shard_id = i;
		head.local.num_buckets = buckets_per_shard;
		head.local.num_pool_lines = pool_lines_per_shard;
		head.local.bucket_array_offset = bucket_array_start + (u64)i * buckets_per_shard * LAYOUT_INDEX_LINK_SIZE;
		head.local.pool_array_offset = pool_array_start + (u64)i * pool_lines_per_shard * LAYOUT_INDEX_LINK_SIZE;
		cxl_set_layout_shard_header(sh, &head);
	}

	/* A zeroed slot reads as RAT entry 0, which a mount would find as a file, so every line
	 * says FREE instead. The bucket and pool arrays run back to back, so one pass covers both. */
	struct layout_index_link *lines = (struct layout_index_link *)((char *)base + bucket_array_start);
	u32 total_lines = num_shards * (buckets_per_shard + pool_lines_per_shard);

	for (u32 i = 0; i < total_lines; i++) {
		union layout_index_link_copy empty = {};
		empty.local.next_link = LAYOUT_LINK_END;
		for (u32 slot = 0; slot < LAYOUT_LINK_SLOTS; slot++)
			empty.local.region_id[slot] = LAYOUT_LINK_SLOT_FREE;

		cxl_set_layout_index_link(&lines[i], &empty);
	}

	struct layout_rat *rat = layout_get_rat(sbi);
	if (!rat)
		return -EINVAL;

	union layout_rat_head_copy rat_head = {};
	rat_head.local.magic = LAYOUT_RAT_MAGIC;
	rat_head.local.version = 1;
	rat_head.local.device_size = total;
	rat_head.local.regions_start = regions_start;
	cxl_set_layout_rat_head(&rat->head, &rat_head);

	/* The lock region is the one entry a format leaves taken. */
	union layout_rat_map_copy rat_map = {};
	layout_rat_map_mark(&rat_map, FS_LOCK_REGION_RAT_ID, true);
	cxl_set_layout_rat_map(&rat->map, &rat_map);

	/* Read back through the mapping, which is the only thing that catches an unflushed write. */
	for (u32 i = 0; i < num_shards; i++) {
		struct layout_shard_header *vsh = layout_get_shard_header(sbi, i);
		if (!vsh)
			return -EINVAL;

		union layout_shard_header_copy check;
		cxl_get_layout_shard_header(&check, vsh);
		pr_debug("format verify shard %u: magic=0x%x buckets=%u pool=%u\n",
			 i, check.local.magic, check.local.num_buckets, check.local.num_pool_lines);

		if (check.local.magic != LAYOUT_SHARD_MAGIC || check.local.num_buckets != buckets_per_shard) {
			pr_err("FORMAT VERIFICATION FAILED — WC memory not flushed\n");
			return -EIO;
		}
	}

	union layout_rat_head_copy check_head;
	union layout_rat_hot_copy check_hot;
	cxl_get_layout_rat_head(&check_head, &rat->head);
	cxl_get_layout_rat_hot(&check_hot, &rat->entries[0].hot);
	pr_debug("format verify RAT: magic=0x%x entry[0].state=%u\n",
		 check_head.local.magic, check_hot.local.state);

	/* Before the fence below, so a joiner never finds the layout without it. */
	format_write_lock_entry(sbi);

	/* Last and whole: its magic is the fence a joiner polls, published with the geometry it
	 * vouches for in one line write. */
	union layout_superblock_head_copy head = {};
	head.local.magic = LAYOUT_MAGIC;
	head.local.version = LAYOUT_VERSION;
	head.local.total_size = total;
	head.local.shard_table_offset = LAYOUT_SHARD_TABLE_OFFSET;
	head.local.rat_offset = rat_offset;
	head.local.num_shards = num_shards;
	head.local.buckets_per_shard = buckets_per_shard;
	head.local.pool_lines_per_shard = pool_lines_per_shard;
	head.local.granule_shift = sbi->format_granule_shift;
	head.local.uc_granules = sbi->format_uc_granules;
	head.local.checksum = layout_compute_superblock_checksum(&head);
	cxl_set_layout_superblock_head(&gsb->head, &head);

	pr_info("format complete (shards=%u, buckets/shard=%u, pool/shard=%u, rat@0x%llx, regions@0x%llx)\n",
		num_shards, buckets_per_shard, pool_lines_per_shard, rat_offset, regions_start);

	return 0;
}

bool format_is_needed(struct fs_sb_info *sbi)
{
	struct layout_superblock *gsb = layout_get_superblock(sbi);
	if (!gsb)
		return true;

	union layout_superblock_head_copy head;
	cxl_get_layout_superblock_head(&head, &gsb->head);
	if (head.local.magic != LAYOUT_MAGIC)
		return true;

	/* A layout from another build leaves the magic valid and the checksum wrong, and that
	 * counts as needing a format rather than as a mount failure. */
	u32 stored = head.local.checksum;
	u32 computed = layout_compute_superblock_checksum(&head);
	if (stored != computed) {
		pr_warn("bootstrap: GSB checksum mismatch (stored=0x%x computed=0x%x), reformatting\n", stored,
			computed);
		return true;
	}

	return false;
}
