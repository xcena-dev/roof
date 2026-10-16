// SPDX-License-Identifier: GPL-2.0-only
/*
 * sysfs_unmount.c - unmount_prepare, the one lifecycle step user space asks for, from the umount
 * helper.
 */

#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/sysfs.h>

#include "lifecycle/unmount.h"
#include "sysfs/sysfs_internal.h"
#include "sysfs/sysfs_unmount.h"

/* /sys/fs/<fs>/node<N>/unmount_prepare - the umount helper writes it before it stops the daemon. */
static ssize_t unmount_prepare_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	s32 err = unmount_prepare(sysfs_sbi_of(kobj));
	return err ? err : (ssize_t)count;
}

static struct kobj_attribute unmount_prepare_attr = __ATTR(unmount_prepare, 0200, NULL, unmount_prepare_store);

static struct attribute *unmount_mount_attrs[] = {
	&unmount_prepare_attr.attr,
	NULL,
};

/* Unnamed, so the file sits directly in the mount's directory beside its peers. */
static const struct attribute_group unmount_mount_attr_group = {
	.attrs = unmount_mount_attrs,
};

s32 unmount_create_mount_attrs(struct kobject *kobj)
{
	return sysfs_create_group(kobj, &unmount_mount_attr_group);
}

void unmount_delete_mount_attrs(struct kobject *kobj)
{
	sysfs_remove_group(kobj, &unmount_mount_attr_group);
}
