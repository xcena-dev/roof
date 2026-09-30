/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * sysfs_region.h - what the module publishes about regions and the pools they are placed in.
 *
 * region_info and pool_info in the module's directory, and one file per RAT slot under rat/.
 */

#ifndef _SYSFS_REGION_H
#define _SYSFS_REGION_H

#include <linux/kobject.h>
#include <linux/types.h>

s32 region_create_sysfs_attrs(struct kobject *kobj);
void region_delete_sysfs_attrs(struct kobject *kobj);

#endif /* _SYSFS_REGION_H */
