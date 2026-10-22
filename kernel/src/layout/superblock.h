/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * superblock.h - Global Superblock on-disk format.
 */

#ifndef _LAYOUT_SUPERBLOCK_H
#define _LAYOUT_SUPERBLOCK_H

#include <linux/crc32.h>
#include <linux/types.h>

#include "cxl/io.h" /* the whole-line views this row is defined through */
#include "uapi.h" /* FS_MAX_DEVICES */

enum layout_superblock_config {
	LAYOUT_SUPERBLOCK_SIZE = 256, /* Global superblock total size */
	LAYOUT_MAX_DEVICES = FS_MAX_DEVICES, /* the device list is sized for this many */
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

/* Each device the filesystem spans, by CXL memdev serial and range offset. The index is the
 * device_id a piece names, and the primary is index 0. */
#define LAYOUT_SUPERBLOCK_DEVICES_FIELDS(FIELD, ARRAY, BYTES) \
	FIELD(32, device_count)                               \
	FIELD(32, checksum)                                   \
	ARRAY(64, LAYOUT_MAX_DEVICES, serial)                 \
	ARRAY(64, LAYOUT_MAX_DEVICES, device_offset)          \
	ARRAY(64, LAYOUT_MAX_DEVICES, size) /* bytes */       \
	BYTES(16, reserved)

enum layout_superblock_devices_config {
	LAYOUT_SUPERBLOCK_DEVICES_LINES = 3,
};

struct layout_superblock_devices {
	LAYOUT_SUPERBLOCK_DEVICES_FIELDS(CXL_VIEW_MEDIUM_FIELD, CXL_VIEW_MEDIUM_ARRAY, CXL_VIEW_MEDIUM_BYTES)
} __attribute__((packed));

struct layout_superblock_devices_local {
	LAYOUT_SUPERBLOCK_DEVICES_FIELDS(CXL_VIEW_LOCAL_FIELD, CXL_VIEW_LOCAL_ARRAY, CXL_VIEW_LOCAL_BYTES)
};

union layout_superblock_devices_copy {
	struct cxl_cacheline lines[LAYOUT_SUPERBLOCK_DEVICES_LINES];
	struct layout_superblock_devices medium;
	struct layout_superblock_devices_local local;
};

_Static_assert(sizeof(struct layout_superblock_devices) == LAYOUT_SUPERBLOCK_DEVICES_LINES * CXL_LINE_BYTES,
	       "the device list must fill its lines");
_Static_assert(sizeof(struct layout_superblock_devices_local) == LAYOUT_SUPERBLOCK_DEVICES_LINES * CXL_LINE_BYTES,
	       "the host-order device list must fill its lines");
_Static_assert(sizeof(union layout_superblock_devices_copy) == LAYOUT_SUPERBLOCK_DEVICES_LINES * CXL_LINE_BYTES,
	       "the device list copy must be exactly its lines");
_Static_assert(offsetof(struct layout_superblock_devices_local, serial) == 8 &&
		       offsetof(struct layout_superblock_devices_local, device_offset) == 8 + 8 * LAYOUT_MAX_DEVICES &&
		       offsetof(struct layout_superblock_devices_local, size) == 8 + 16 * LAYOUT_MAX_DEVICES,
	       "the arrays follow the count and the checksum");

static inline void layout_get_superblock_devices(union layout_superblock_devices_copy *copy,
						 const struct layout_superblock_devices *row)
{
	for (u32 line = 0; line < LAYOUT_SUPERBLOCK_DEVICES_LINES; line++)
		cxl_get_cacheline(&copy->lines[line], (const u8 *)row + line * CXL_LINE_BYTES);
	LAYOUT_SUPERBLOCK_DEVICES_FIELDS(CXL_VIEW_DECODE_FIELD, CXL_VIEW_DECODE_ARRAY, CXL_VIEW_DECODE_BYTES)
}

static inline void layout_set_superblock_devices(struct layout_superblock_devices *row,
						 union layout_superblock_devices_copy *copy)
{
	LAYOUT_SUPERBLOCK_DEVICES_FIELDS(CXL_VIEW_ENCODE_FIELD, CXL_VIEW_ENCODE_ARRAY, CXL_VIEW_ENCODE_BYTES)
	for (u32 line = 0; line < LAYOUT_SUPERBLOCK_DEVICES_LINES; line++)
		cxl_set_cacheline((u8 *)row + line * CXL_LINE_BYTES, &copy->lines[line]);
}

/* The head line and the device list fill the superblock. */
struct layout_superblock {
	struct layout_superblock_head head;
	struct layout_superblock_devices devices;
} __attribute__((packed));

_Static_assert(offsetof(struct layout_superblock, devices) == CXL_LINE_BYTES,
	       "the device list follows the head line");

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

/* Over every field of the device list except the checksum, which sits between them. */
static inline u32 layout_compute_devices_checksum(const union layout_superblock_devices_copy *list)
{
	u32 crc = crc32(~0U, (const u8 *)&list->local.device_count, sizeof(list->local.device_count));
	crc = crc32(crc, (const u8 *)list->local.serial, sizeof(list->local.serial));
	crc = crc32(crc, (const u8 *)list->local.device_offset, sizeof(list->local.device_offset));
	return crc32(crc, (const u8 *)list->local.size, sizeof(list->local.size));
}

#endif /* _LAYOUT_SUPERBLOCK_H */
