/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * index.h - Global hash index on-disk format.
 *
 * A bucket is one cacheline naming the files that hash to it, and the shard table says where a
 * shard's buckets and its pool lines lie. Nothing sits between a bucket and the RAT: a slot
 * carries the RAT id itself, so publishing a name is one line write and has no half-done state.
 */

#ifndef _LAYOUT_INDEX_H
#define _LAYOUT_INDEX_H

#include <linux/types.h>

#include "cxl/io.h"
#include "layout/rat.h" /* LAYOUT_MAX_RAT_ENTRIES, which a slot has to hold */

/* ── Region index geometry and sizes ───────────────────────────────── */
enum layout_index_config {
	LAYOUT_NUM_SHARDS = 4,
	LAYOUT_BUCKETS_PER_SHARD = 32,
	/* Lines a shard keeps in reserve for a bucket that outgrows its own line. */
	LAYOUT_POOL_LINES_PER_SHARD = 24,

	LAYOUT_SHARD_HEADER_SIZE = 64,
	LAYOUT_INDEX_LINK_SIZE = 64,
	LAYOUT_LINK_SLOTS = 15, /* what fits beside next_link at 4 bytes a slot */
};

/* ── Sentinels ─────────────────────────────────────────────────────────
 * Each is every bit of the field that carries it, written the way ACL_DELEG_ANY_ID is.
 *
 * Macros rather than two more enumerators. All-ones in 32 bits lies outside int, and an
 * enumerator that wide turns the whole enum unsigned before C23. The marker below is compared
 * against a RAT constant, which -Wenum-compare reads as two enum types when both are
 * enumerators.
 */
#define LAYOUT_LINK_END ((u32)~0U) /* this bucket has no further line */
#define LAYOUT_LINK_SLOT_FREE ((u16)~0U) /* this slot names no file */

/* A slot names a RAT entry in 16 bits, and the free marker takes the top value. */
_Static_assert(LAYOUT_MAX_RAT_ENTRIES < LAYOUT_LINK_SLOT_FREE,
	       "a RAT id must fit a link slot and leave the free marker free");

/*
 * Every file may hash to one bucket, so that bucket alone has to be able to hold them all.
 * Sized this way the pool cannot run dry, and insert has no out-of-lines case to answer for.
 */
_Static_assert((1 + LAYOUT_POOL_LINES_PER_SHARD) * LAYOUT_LINK_SLOTS >= LAYOUT_MAX_RAT_ENTRIES,
	       "one bucket must be able to hold every file");

/*
 * Shard Header — one cacheline, one per shard, in the shard table. Says where that shard's
 * buckets and its pool lines lie.
 */
#define LAYOUT_SHARD_HEADER_FIELDS(FIELD, ARRAY, BYTES)                    \
	FIELD(32, magic) /* LAYOUT_SHARD_MAGIC */                          \
	FIELD(32, shard_id) /* index into [0, num_shards) */               \
	FIELD(32, num_buckets) /* hash bucket count, a power of two */     \
	FIELD(32, num_pool_lines) /* pool lines this shard can hand out */ \
	FIELD(64, bucket_array_offset) /* absolute device offset */        \
	FIELD(64, pool_array_offset) /* absolute device offset */          \
	BYTES(32, reserved)

CXL_DEFINE_LINE_VIEWS(layout_shard_header, LAYOUT_SHARD_HEADER_FIELDS)

/*
 * Index Link — one cacheline, and the whole of a bucket. A cell of the bucket array is a line of
 * this shape, and a bucket that outgrows it takes another from the shard's pool.
 *
 * @region_id is the RAT entry carrying the file, and LAYOUT_LINK_SLOT_FREE marks a slot holding
 * no file. @tag is bits 15..0 of the name's hash, which the shard and bucket selection does not
 * use, so those 16 bits still tell two names in one bucket apart.
 *
 * A tag decides nothing. layout_check_rat_name compares the stored name and is the only judge,
 * so a tag collision costs one RAT read and never an answer.
 *
 * A pool line is free when no bucket of its shard names it, and its own content says nothing
 * about that: a line already linked into a bucket may hold no file either. So the pool keeps no
 * free list and no marker, and taking or returning a line writes no shard header.
 */
#define LAYOUT_INDEX_LINK_FIELDS(FIELD, ARRAY, BYTES)                               \
	FIELD(32, next_link) /* further line, LAYOUT_LINK_END ends the bucket */    \
	ARRAY(16, LAYOUT_LINK_SLOTS, tag) /* bits 15..0 of that file's name hash */ \
	ARRAY(16, LAYOUT_LINK_SLOTS, region_id) /* RAT id, or LAYOUT_LINK_SLOT_FREE */

CXL_DEFINE_LINE_VIEWS(layout_index_link, LAYOUT_INDEX_LINK_FIELDS)

#endif /* _LAYOUT_INDEX_H */
