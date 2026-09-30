/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * superblock.h - Global Superblock on-disk format.
 */

#ifndef _LAYOUT_SUPERBLOCK_H
#define _LAYOUT_SUPERBLOCK_H

#include <linux/crc32.h>
#include <linux/types.h>

#include "cxl/io.h" /* the whole-line views this row is defined through */

enum layout_superblock_config {
	LAYOUT_SUPERBLOCK_SIZE = 256, /* Global superblock total size */
};

/*
 * CL0 of the superblock: what the filesystem is, where its parts lie, and the checksum over both.
 * A format publishes the line in one transaction, so a joiner never sees the magic without the
 * geometry that magic vouches for.
 */
#define LAYOUT_SUPERBLOCK_HEAD_FIELDS(FIELD, ARRAY, BYTES)                            \
	FIELD(32, magic) /* LAYOUT_MAGIC */                                           \
	FIELD(32, uuid) /* per-format, and 0 until a format writes it */              \
	FIELD(32, version) /* LAYOUT_VERSION */                                       \
	BYTES(4, reserved0) /* keeps total_size 8-byte aligned */                     \
	FIELD(64, total_size)                                                         \
	FIELD(64, shard_table_offset)                                                 \
	FIELD(64, rat_offset)                                                         \
	FIELD(32, num_shards) /* a power of two */                                    \
	FIELD(32, buckets_per_shard) /* a power of two */                             \
	FIELD(32, pool_lines_per_shard)                                               \
	FIELD(32, granule_shift) /* region alignment, LAYOUT_GRANULE_SHIFT_MIN.. */   \
	FIELD(32, uc_granules) /* granules of the uncached pool, past the metadata */ \
	FIELD(32, checksum)

CXL_DEFINE_LINE_VIEWS(layout_superblock_head, LAYOUT_SUPERBLOCK_HEAD_FIELDS)

/*
 * The superblock: one head line, and the rest held back so the bootstrap area starts where the
 * offset table says it does.
 */
struct layout_superblock {
	struct layout_superblock_head head;
	__u8 reserved[LAYOUT_SUPERBLOCK_SIZE - CXL_LINE_BYTES];
} __attribute__((packed));

_Static_assert(sizeof(struct layout_superblock) == LAYOUT_SUPERBLOCK_SIZE,
	       "the superblock must fill the space the offset table gives it");

/*
 * The checksum covers the head's fields up to itself, so a writer and a reader agree without
 * either one owning the rule. It runs over the host-order view, which makes the stored value a
 * property of this byte order and not of the medium: two hosts of opposite endianness would each
 * read the other's filesystem as needing a format.
 */
static inline u32 layout_compute_superblock_checksum(const union layout_superblock_head_copy *head)
{
	return crc32(~0U, (const u8 *)&head->local, offsetof(struct layout_superblock_head_local, checksum));
}

#endif /* _LAYOUT_SUPERBLOCK_H */
