/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * sysfs_daemon.h - daemon sysfs attributes.
 *
 * One file per mount, since a channel belongs to one node.
 */

#ifndef _SYSFS_DAEMON_H
#define _SYSFS_DAEMON_H

#include <linux/kobject.h>
#include <linux/types.h>

s32 daemon_create_mount_attrs(struct kobject *kobj);
void daemon_delete_mount_attrs(struct kobject *kobj);

#endif /* _SYSFS_DAEMON_H */
