/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * format.h - writing the initial layout, and deciding whether it is needed.
 */

#ifndef _FS_FORMAT_H
#define _FS_FORMAT_H

#include <linux/types.h>

struct fs_sb_info;

/* Writes superblock, shard table, bucket and entry arrays, and the RAT. The GSB
 * magic goes last, which is the fence a joiner polls for. */
s32 format_device(struct fs_sb_info *sbi);

/* True when the device carries no usable layout. A stale layout leaves the magic
 * valid and the checksum wrong, and that counts as needing one too. */
bool format_is_needed(struct fs_sb_info *sbi);

#endif /* _FS_FORMAT_H */
