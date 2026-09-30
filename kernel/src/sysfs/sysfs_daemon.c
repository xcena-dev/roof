// SPDX-License-Identifier: GPL-2.0-only
/*
 * sysfs_daemon.c - daemon sysfs attributes.
 */

#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/sysfs.h>

#include "access/daemon.h"
#include "sysfs/sysfs_daemon.h"
#include "sysfs/sysfs_internal.h"

/* /sys/fs/<fs>/node<N>/daemon_state - this mount's upcall channel in one line. */
static ssize_t daemon_state_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct daemon_state state;

	daemon_get_state(sysfs_sbi_of(kobj), &state);
	return sysfs_emit(buf, "reader_open=%d hello_done=%d helper_dead=%d queue_depth=%u asked=%llu\n",
			  state.reader_open, state.hello_done, state.helper_dead, state.queue_depth,
			  state.asked);
}

static struct kobj_attribute daemon_state_attr = __ATTR_RO(daemon_state);

static struct attribute *daemon_mount_attrs[] = {
	&daemon_state_attr.attr,
	NULL,
};

/* Unnamed, so the file sits directly in the mount's directory beside its peers. */
static const struct attribute_group daemon_mount_attr_group = {
	.attrs = daemon_mount_attrs,
};

s32 daemon_create_mount_attrs(struct kobject *kobj)
{
	return sysfs_create_group(kobj, &daemon_mount_attr_group);
}

void daemon_delete_mount_attrs(struct kobject *kobj)
{
	sysfs_remove_group(kobj, &daemon_mount_attr_group);
}
