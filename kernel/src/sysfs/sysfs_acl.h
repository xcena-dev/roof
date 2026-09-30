/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * sysfs_acl.h - what the module publishes about permissions.
 *
 * perm_info is every region's default and bound, readable by anyone. deleg_info is one region's
 * rows, which name other accounts' ids and so are root's alone.
 */

#ifndef _SYSFS_ACL_H
#define _SYSFS_ACL_H

#include <linux/kobject.h>
#include <linux/types.h>

s32 acl_create_sysfs_attrs(struct kobject *kobj);
void acl_delete_sysfs_attrs(struct kobject *kobj);

#endif /* _SYSFS_ACL_H */
