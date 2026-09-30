/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * index.h - Global name index entry points.
 *
 * A name resolves to the RAT id of the region carrying it and to nothing else, because that id
 * is the whole of what the index stores about it.
 */

#ifndef _INDEX_H
#define _INDEX_H

#include <linux/types.h>

struct fs_sb_info;

s32 index_insert(struct fs_sb_info *sbi, const char *name, u32 namelen, u32 region_id);
s32 index_find(struct fs_sb_info *sbi, const char *name, u32 namelen, u32 *out_region_id);
s32 index_delete(struct fs_sb_info *sbi, const char *name, u32 namelen);

#endif /* _INDEX_H */
