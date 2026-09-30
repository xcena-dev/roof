/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * sysfs_internal.h - shared state and helpers across the sysfs sources.
 *
 * sysfs.c owns the registered-sbi list, the lock over it, and the directory each mount gets.
 * The domain-split files include this header rather than repeating the declarations.
 */

#ifndef _SYSFS_INTERNAL_H
#define _SYSFS_INTERNAL_H

#include <linux/kobject.h>
#include <linux/mutex.h>

#include "core.h"
#include "uapi.h" /* FS_MAX_NODE_ID, the ceiling below */

#define SYSFS_MAX_MOUNTS FS_MAX_NODE_ID

extern struct fs_sb_info *sysfs_sbi_list[SYSFS_MAX_MOUNTS];
extern struct mutex sysfs_lock;

/*
 * One mount's sysfs directory. An attribute hung here reaches its sbi from the kobject it is
 * handed, so it needs neither the list nor the lock over it.
 */
struct sysfs_mount {
	struct kobject kobj;
	struct fs_sb_info *sbi;
};

/* The mount an attribute in that directory belongs to. */
static inline struct fs_sb_info *sysfs_sbi_of(struct kobject *kobj)
{
	return container_of(kobj, struct sysfs_mount, kobj)->sbi;
}

/* Both walk the list, so the caller holds sysfs_lock, and the pointer is good only while it does. */
struct fs_sb_info *sysfs_get_sbi(void); /* any registered sbi, which is what a shared-CXL read wants */
struct fs_sb_info *sysfs_find_by_node(u32 node_id); /* NULL when no mount here holds that node */

#endif /* _SYSFS_INTERNAL_H */
