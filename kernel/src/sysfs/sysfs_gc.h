/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * sysfs_gc.h - GC sysfs attributes.
 *
 * gc_status sits in the module's directory, since it covers every mount.
 */

#ifndef _SYSFS_GC_H
#define _SYSFS_GC_H

#include <linux/kobject.h>
#include <linux/types.h>

s32 gc_create_sysfs_attrs(struct kobject *kobj);
void gc_delete_sysfs_attrs(struct kobject *kobj);

#endif /* _SYSFS_GC_H */
