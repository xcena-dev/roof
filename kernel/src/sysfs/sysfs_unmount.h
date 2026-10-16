/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * sysfs_unmount.h - unmount sysfs attributes.
 *
 * One file per mount, since the clean step is one node's own.
 */

#ifndef _SYSFS_UNMOUNT_H
#define _SYSFS_UNMOUNT_H

#include <linux/kobject.h>
#include <linux/types.h>

s32 unmount_create_mount_attrs(struct kobject *kobj);
void unmount_delete_mount_attrs(struct kobject *kobj);

#endif /* _SYSFS_UNMOUNT_H */
