/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * dax_area.h - taking and dropping a hold on the mapped areas of a mount's devices.
 */

#ifndef _DAX_AREA_H
#define _DAX_AREA_H

#include <linux/types.h>

struct fs_sb_info;

/* On failure nothing is held. */
s32 dax_acquire_areas(struct fs_sb_info *sbi);

/* The areas belong to the device, so the last mount out is the one that unmaps them. */
void dax_release_areas(struct fs_sb_info *sbi);

/* Unmaps whatever is still held at unload. The table lives in this module, so an area left mapped
 * with the table gone is a device nothing on this host can map again. Call after the last mount. */
void dax_exit(void);

#endif /* _DAX_AREA_H */
