/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * test_sysfs.h - the debug attributes only a test reads or writes.
 *
 * The module's own sit in a test/ directory beside the operator group. The one that describes a
 * single mount sits in a test/ group under that mount's own directory, which is what keeps its
 * table inside the one page a show is given. Without CONFIG_FS_TEST_KNOBS neither is created.
 */

#ifndef _TEST_SYSFS_H
#define _TEST_SYSFS_H

#include <linux/types.h>

struct kobject;

#ifdef CONFIG_FS_TEST_KNOBS

s32 test_create_sysfs_group(struct kobject *kobj);
void test_delete_sysfs_group(struct kobject *kobj);
s32 test_create_mount_attrs(struct kobject *kobj);
void test_delete_mount_attrs(struct kobject *kobj);

#else

/* No attributes, so neither directory is created. */
static inline s32 test_create_sysfs_group(struct kobject *kobj)
{
	(void)kobj;
	return 0;
}

static inline void test_delete_sysfs_group(struct kobject *kobj)
{
	(void)kobj;
}

static inline s32 test_create_mount_attrs(struct kobject *kobj)
{
	(void)kobj;
	return 0;
}

static inline void test_delete_mount_attrs(struct kobject *kobj)
{
	(void)kobj;
}

#endif /* CONFIG_FS_TEST_KNOBS */

#endif /* _TEST_SYSFS_H */
