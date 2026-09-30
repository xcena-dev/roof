/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * dax_area.h - taking and dropping a hold on the device's two mapped areas.
 */

#ifndef _DAX_AREA_H
#define _DAX_AREA_H

struct fs_sb_info;

/* Reads the device's address and size from sysfs, then maps both areas. On return
 * sbi->meta_base, data_base, phys_base and total_size are set. */
int dax_acquire_areas(struct fs_sb_info *sbi);

/* Drops this mount's hold. The areas belong to the device, so the last mount out
 * is the one that unmaps them. */
void dax_release_areas(struct fs_sb_info *sbi);

/* Unmaps whatever is still held at unload. The table lives in this module, so an area left mapped
 * with the table gone is a device nothing on this host can map again. Call after the last mount. */
void dax_exit(void);

#endif /* _DAX_AREA_H */
