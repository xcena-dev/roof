/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * layout_access.h - reaching the records on the medium through a mount.
 *
 * The headers beside this one say what a record looks like. This one says how a
 * mount finds it: every accessor resolves an offset against the two mappings
 * sbi holds and fences the line it hands back.
 */

#ifndef _FS_LAYOUT_ACCESS_H
#define _FS_LAYOUT_ACCESS_H

#include <linux/string.h>
#include <linux/types.h>

#include "core.h"
#include "layout/offsets.h"
#include "tools_uapi.h"

/* ============================================================================
 * DAX abstraction API
 * ============================================================================
 *
 * All filesystem code uses these helpers to access device memory (DEV_DAX).
 */

/*
 * Which of the two mappings holds a device offset, and so which cache policy it carries. The
 * address decides it rather than the region's identity, and the boundary comes from the
 * superblock so that every node splits the device the same way.
 */
static inline bool layout_check_meta_offset(const struct fs_sb_info *sbi, u64 offset)
{
	return offset < sbi->wb_start;
}

/* Pointer into the uncached area. @offset is a device offset below sbi->wb_start. */
static inline void *layout_get_meta_ptr(struct fs_sb_info *sbi, u64 offset)
{
	return (void *)((char *)sbi->meta_base + offset);
}

/* Validate that a device offset is inside the device and the areas are mapped */
static inline bool layout_is_valid_phys_offset(struct fs_sb_info *sbi, u64 offset)
{
	return offset > 0 && offset < sbi->total_size && sbi->meta_base;
}

/*
 * GSB accessor: the address alone. fs_init_layout_ptrs caches the pointer, and
 * super_read_superblock computes it from meta_base itself rather than calling this.
 */
static inline struct layout_superblock *layout_get_superblock(struct fs_sb_info *sbi)
{
	if (unlikely(!sbi || !sbi->gsb))
		return NULL;

	return sbi->gsb;
}

/*
 * RAT accessor: the address alone. The header is read through cxl_get_layout_rat_head and an
 * entry through cxl_get_ on one of its lines, and each of those fences, so a fence here would
 * order nothing the caller has read. sbi->rat is set before any runtime caller reaches here.
 */
static inline struct layout_rat *layout_get_rat(struct fs_sb_info *sbi)
{
	if (unlikely(!sbi || !sbi->rat))
		return NULL;

	return sbi->rat;
}

/* The lock region sits outside the hash index, so every path that resolves a
 * name asks here before consulting a bucket. */
static inline bool layout_check_lock_region_name(const char *name, u32 len)
{
	return len == FS_LOCK_REGION_NAME_LEN && memcmp(name, FS_LOCK_REGION_NAME, len) == 0;
}

/*
 * RAT entry accessor: the address alone. Every read of the entry goes through cxl_get_ on one of
 * its lines, and each of those fences, so a fence here would order nothing the caller has read.
 *
 * NULL means the RAT is not mapped or @id is past the table. Both are this mount's own record
 * being wrong rather than a caller passing a bad argument, which is why callers answer -EIO.
 */
static inline struct layout_rat_entry *layout_get_rat_entry(struct fs_sb_info *sbi, u32 id)
{
	if (unlikely(!sbi || !sbi->rat || id >= LAYOUT_MAX_RAT_ENTRIES))
		return NULL;

	return &sbi->rat->entries[id];
}

/* Delegation row accessor: the address alone, for the same reason as the entry above. */
static inline struct acl_deleg_entry *layout_get_deleg_entry(struct layout_rat_entry *entry, u32 idx)
{
	if (unlikely(idx >= ACL_DELEG_MAX_ENTRIES))
		return NULL;

	return &entry->deleg_entries[idx];
}

/* ── Typed DAX region accessors ──────────────────────────────────────────── */

/*
 * Shard header accessor: the address alone, for the same reason as the RAT above.
 *
 * fs_init_layout_ptrs fills the cache and its header pointers at mount start, before format and
 * read_superblock, so this serves the format-side writes as well as later reads.
 */
static inline struct layout_shard_header *layout_get_shard_header(struct fs_sb_info *sbi, u32 shard_id)
{
	if (unlikely(!sbi || !sbi->shard_cache || shard_id >= LAYOUT_NUM_SHARDS))
		return NULL;

	return sbi->shard_cache[shard_id].header;
}

/*
 * layout_get_file_data_ptr - file data pointer at (data_phys_offset + pos).
 *
 * data_phys_offset is the per-inode physical base; pos is the read offset
 * within that region.  Caller must RMB before accessing the returned pointer.
 */
static inline void *layout_get_file_data_ptr(struct fs_sb_info *sbi, u64 data_phys_offset, loff_t pos)
{
	u64 offset = data_phys_offset + (u64)pos;

	if (layout_check_meta_offset(sbi, offset))
		return layout_get_meta_ptr(sbi, offset);

	/* The write-back area is its own mapping starting where the uncached one ends, so a
	 * device offset becomes an offset into that mapping. */
	return (void *)((char *)sbi->data_base + (offset - sbi->wb_start));
}

/* Validate that [offset, offset+size) is within DAX mapping (overflow-safe) */
static inline bool layout_is_valid_range(struct fs_sb_info *sbi, u64 offset, u64 size)
{
	if (unlikely(!sbi->meta_base || size == 0))
		return false;
	if (unlikely(offset >= sbi->total_size))
		return false;
	if (unlikely(size > sbi->total_size - offset)) /* overflow-safe subtraction */
		return false;
	return true;
}

/*
 * True when region @region_id carries exactly @name. A stored name is null-terminated unless it
 * fills the field, so a prefix match has to see the terminator too.
 */
static inline bool layout_check_rat_name(struct fs_sb_info *sbi, u32 region_id, const char *name, u32 namelen)
{
	struct layout_rat_entry *rat_e = layout_get_rat_entry(sbi, region_id);
	if (!rat_e)
		return false;

	struct cxl_cacheline line;
	const char *stored = layout_read_rat_name(rat_e, &line);

	if (strncmp(stored, name, namelen) != 0)
		return false;
	return namelen >= FS_NAME_MAX || stored[namelen] == '\0';
}

/* Validate data_phys_offset + access range is within DAX mapping */
static inline bool layout_is_valid_region_addr(struct fs_sb_info *sbi, u64 data_phys_offset, u64 access_size)
{
	if (unlikely(data_phys_offset == 0))
		return false;
	return layout_is_valid_range(sbi, data_phys_offset, access_size);
}

/* ============================================================================
 * Shard helpers
 * ============================================================================ */

/* Validate shard geometry is non-zero and within what this build lays out */
static inline bool layout_is_valid_shard_geometry(u32 num_buckets, u32 num_pool_lines)
{
	return num_buckets > 0 && num_buckets <= LAYOUT_BUCKETS_PER_SHARD &&
	       num_pool_lines > 0 && num_pool_lines <= LAYOUT_POOL_LINES_PER_SHARD;
}

/*
 * A bucket's own line, or NULL when the shard or the bucket is out of range.
 *
 * The two below hand back an address and fence nothing, the way the RAT accessors do: every read
 * of a line goes through cxl_get_layout_index_link, which carries its own barrier, so a fence
 * here would order nothing the caller has read.
 */
static inline struct layout_index_link *layout_get_shard_bucket(struct fs_sb_info *sbi, u32 shard_id, u32 bucket_idx)
{
	if (unlikely(shard_id >= sbi->num_shards))
		return NULL;
	struct fs_shard_cache *sc = &sbi->shard_cache[shard_id];
	if (unlikely(bucket_idx >= sbi->buckets_per_shard || !sc->buckets))
		return NULL;

	return &sc->buckets[bucket_idx];
}

/* One of the shard's pool lines, by index into that shard's pool. */
static inline struct layout_index_link *layout_get_shard_pool_line(struct fs_sb_info *sbi, u32 shard_id, u32 line_idx)
{
	if (unlikely(shard_id >= sbi->num_shards))
		return NULL;
	struct fs_shard_cache *sc = &sbi->shard_cache[shard_id];
	if (unlikely(line_idx >= sbi->pool_lines_per_shard || !sc->pool))
		return NULL;

	return &sc->pool[line_idx];
}

#endif /* _FS_LAYOUT_ACCESS_H */
