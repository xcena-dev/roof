// SPDX-License-Identifier: GPL-2.0-only
/*
 * sysfs_gc.c - gc_status.
 *
 * gc_status is a counter an operator watches and the suite waits on. The knob that pauses GC sits in
 * the debug group instead.
 */

#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sysfs.h>

#include "core.h"
#include "sysfs/sysfs_gc.h"
#include "sysfs/sysfs_internal.h"

/*
 * GC status - show per-node GC thread liveness and epoch counter.
 *   cat gc_status  =>  "node1:alive epoch=42 node2:dead epoch=0"
 */
static ssize_t gc_status_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	s32 len = 0;

	mutex_lock(&sysfs_lock);
	for (u32 i = 0; i < SYSFS_MAX_MOUNTS; i++) {
		struct fs_sb_info *sbi = sysfs_sbi_list[i];
		if (!sbi)
			continue;

		bool has_thread = sbi->gc_thread != NULL;
		s32 epoch = atomic_read(&sbi->gc_epoch);

		len += sysfs_emit_at(buf, len, "node%u:%s epoch=%d ", sbi->node_id, has_thread ? "running" : "stopped", epoch);
	}
	mutex_unlock(&sysfs_lock);

	if (len == 0)
		return sysfs_emit(buf, "no mounts\n");

	len += sysfs_emit_at(buf, len, "\n");
	return len;
}

static struct kobj_attribute gc_status_attr = __ATTR(gc_status, 0444, gc_status_show, NULL);

static struct attribute *gc_attrs[] = {
	&gc_status_attr.attr,
	NULL,
};

/* Unnamed, so the file sits directly in the module's directory. */
static const struct attribute_group gc_attr_group = {
	.attrs = gc_attrs,
};

s32 gc_create_sysfs_attrs(struct kobject *kobj)
{
	return sysfs_create_group(kobj, &gc_attr_group);
}

void gc_delete_sysfs_attrs(struct kobject *kobj)
{
	sysfs_remove_group(kobj, &gc_attr_group);
}
