/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * sysfs.h - registering and tearing down the module's sysfs tree.
 *
 * The knobs themselves live in the sysfs_* files beside this one. What is here
 * is what the module entry point and each mount call.
 */

#ifndef _FS_SYSFS_H
#define _FS_SYSFS_H

#include <linux/types.h>

struct fs_sb_info;

s32 sysfs_init(void);
void sysfs_exit(void);
s32 sysfs_register(struct fs_sb_info *sbi);
void sysfs_unregister(struct fs_sb_info *sbi);

#endif /* _FS_SYSFS_H */
