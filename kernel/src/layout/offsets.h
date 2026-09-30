/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * offsets.h - the on-disk layout (umbrella header).
 *
 * Data access between nodes is the application's to serialise. A metadata record several nodes
 * may write is one cacheline published in one store, and a write spanning lines takes meta_lock.
 *
 * This header collects the master layout description: magic numbers,
 * the global offset table, and compile-time size validators. Per-domain
 * on-disk structs are defined in dedicated headers (superblock/index/rat),
 * each focused on one subsystem.
 *
 * CXL memory is packed in this order, and only the region area is 2MB-aligned.
 * Every offset up to the lock region is computed by enum layout_offset_table below.
 *
 *   Superblock → Bootstrap slots → Shard table → Bucket lines
 *     → Index line pool → RAT → (2MB align) → lock region → regions
 *
 * The regions split into two pools, uncached below the boundary and write-back above it. That
 * boundary and the granule they are handed out in come off the superblock, so they are runtime
 * values on the sbi rather than constants here.
 */

#ifndef _LAYOUT_OFFSETS_H
#define _LAYOUT_OFFSETS_H

#include <linux/align.h>
#include <linux/types.h>

#include "cxl/io.h"
#include "layout/hash.h"
#include "uapi.h"

#include "layout/bootstrap.h"
#include "layout/index.h"
#include "layout/rat.h"
#include "layout/refs.h"
#include "layout/superblock.h"

/* ── Magic Numbers / Version ───────────────────────────────────────── */
enum layout_magic {
	LAYOUT_MAGIC = 0x46475342, /* "FGSB", the global superblock */
	LAYOUT_BOOTSTRAP_MAGIC = 0x46425453, /* "FBTS" */
	LAYOUT_SHARD_MAGIC = 0x46534844, /* "FSHD" */
	LAYOUT_RAT_MAGIC = 0x46524154, /* "FRAT" */

	LAYOUT_VERSION = 1,
};

/* ── Region granule ───────────────────────────────────────────────────
 *
 * What a region's offset and size are rounded to, chosen at format and kept in the superblock so
 * every node aligns the same way. Distinct from LAYOUT_ALIGN_2MB, which places the areas
 * themselves and does not move.
 *
 * The floor is the PMD size: a region under it cannot take a huge-page fault, and on uncached
 * memory that is the expensive direction.
 */
enum layout_granule_config {
	LAYOUT_GRANULE_SHIFT_MIN = 21, /* 2MB */
	LAYOUT_GRANULE_SHIFT_MAX = 30, /* 1GB */
	LAYOUT_GRANULE_SHIFT_DEFAULT = LAYOUT_GRANULE_SHIFT_MIN,

	/* One granule at least, so a coarse granule leaves a pool rather than none. */
	LAYOUT_UC_POOL_DEFAULT_BYTES = 64 * 1024 * 1024,
};

static inline bool layout_check_granule_shift(u32 shift)
{
	return shift >= LAYOUT_GRANULE_SHIFT_MIN && shift <= LAYOUT_GRANULE_SHIFT_MAX;
}

/* Granules the uncached pool takes at @shift, never fewer than one. */
static inline u32 layout_get_default_uc_granules(u32 shift)
{
	u64 count = LAYOUT_UC_POOL_DEFAULT_BYTES >> shift;
	return count ? (u32)count : 1;
}

/* ── Global offset table ──────────────────────────────────────────── */
enum layout_offset_table {
	LAYOUT_ALIGN_2MB = 2 * 1024 * 1024,

	LAYOUT_SUPERBLOCK_OFFSET = 0,

	LAYOUT_BOOTSTRAP_AREA_OFFSET = LAYOUT_SUPERBLOCK_OFFSET + LAYOUT_SUPERBLOCK_SIZE,

	LAYOUT_SHARD_TABLE_OFFSET = LAYOUT_BOOTSTRAP_AREA_OFFSET + LAYOUT_BOOTSTRAP_AREA_SIZE,
	LAYOUT_SHARD_TABLE_SIZE = LAYOUT_NUM_SHARDS * LAYOUT_SHARD_HEADER_SIZE,

	LAYOUT_INDEX_BUCKET_OFFSET = LAYOUT_SHARD_TABLE_OFFSET + LAYOUT_SHARD_TABLE_SIZE,
	LAYOUT_INDEX_BUCKET_SIZE = LAYOUT_NUM_SHARDS * LAYOUT_BUCKETS_PER_SHARD * LAYOUT_INDEX_LINK_SIZE,

	LAYOUT_INDEX_POOL_OFFSET = LAYOUT_INDEX_BUCKET_OFFSET + LAYOUT_INDEX_BUCKET_SIZE,
	LAYOUT_INDEX_POOL_SIZE = LAYOUT_NUM_SHARDS * LAYOUT_POOL_LINES_PER_SHARD * LAYOUT_INDEX_LINK_SIZE,

	LAYOUT_RAT_OFFSET = LAYOUT_INDEX_POOL_OFFSET + LAYOUT_INDEX_POOL_SIZE,
	LAYOUT_RAT_SIZE = LAYOUT_RAT_HEADER_SIZE + LAYOUT_MAX_RAT_ENTRIES * LAYOUT_RAT_ENTRY_SIZE,

	/* The reference table follows the RAT: one row of lines per node. */
	LAYOUT_REFS_OFFSET = LAYOUT_RAT_OFFSET + LAYOUT_RAT_SIZE,
	LAYOUT_REFS_END_OFFSET = LAYOUT_REFS_OFFSET + LAYOUT_REFS_SIZE,

	/* What the lock region's alignment leaves behind the tables. Named so that an area taking it
	 * later moves nothing after it. */
	LAYOUT_RAT_PAD_SIZE = ALIGN(LAYOUT_REFS_END_OFFSET, LAYOUT_ALIGN_2MB) - LAYOUT_REFS_END_OFFSET,

	/* The daemon's lock area, in the metadata half because its users want cross-node visibility
	 * rather than a cache or GPU DMA. One alignment unit holds it: 64 domains x 64 peers under
	 * its widest strategy measures 1MB + 192B. */
	LAYOUT_LOCK_REGION_OFFSET = LAYOUT_REFS_END_OFFSET + LAYOUT_RAT_PAD_SIZE,
	LAYOUT_LOCK_REGION_SIZE = LAYOUT_ALIGN_2MB,

	LAYOUT_DATA_OFFSET = LAYOUT_LOCK_REGION_OFFSET + LAYOUT_LOCK_REGION_SIZE,
};

/* ── Compile-time size validation ────────────────────────────────── */

#define LAYOUT_BUILD_BUG_ON(cond) ((void)sizeof(char[1 - 2 * !!(cond)]))

static inline void __layout_verify_structs(void)
{
	LAYOUT_BUILD_BUG_ON(sizeof(struct layout_superblock) != LAYOUT_SUPERBLOCK_SIZE);
	LAYOUT_BUILD_BUG_ON(sizeof(struct bootstrap_slot) != LAYOUT_BOOTSTRAP_SLOT_SIZE);
	LAYOUT_BUILD_BUG_ON(sizeof(struct layout_shard_header) != LAYOUT_SHARD_HEADER_SIZE);
	LAYOUT_BUILD_BUG_ON(sizeof(struct layout_index_link) != LAYOUT_INDEX_LINK_SIZE);
	LAYOUT_BUILD_BUG_ON(sizeof(struct acl_deleg_entry) != ACL_DELEG_ENTRY_SIZE);
	LAYOUT_BUILD_BUG_ON(sizeof(struct layout_rat_entry) != LAYOUT_RAT_ENTRY_SIZE);
}

#endif /* _LAYOUT_OFFSETS_H */
