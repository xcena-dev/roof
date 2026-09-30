/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * sysfs_gc.h - what the module publishes about a mount's life on the medium.
 *
 * gc_status in the module's directory covers every mount. unmount_prepare is one mount's own, so it
 * hangs in that mount's directory.
 */

#ifndef _SYSFS_GC_H
#define _SYSFS_GC_H

#include <linux/kobject.h>
#include <linux/types.h>

s32 gc_create_sysfs_attrs(struct kobject *kobj);
void gc_delete_sysfs_attrs(struct kobject *kobj);

s32 gc_create_mount_attrs(struct kobject *kobj);
void gc_delete_mount_attrs(struct kobject *kobj);

#endif /* _SYSFS_GC_H */
