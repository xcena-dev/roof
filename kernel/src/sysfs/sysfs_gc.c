// SPDX-License-Identifier: GPL-2.0-only
/*
 * sysfs_gc.c - gc_status and unmount_prepare.
 *
 * gc_status is a counter an operator watches and the suite waits on. unmount_prepare is the one
 * lifecycle step user space asks for, from the umount helper. The knob that pauses GC sits in the
 * debug group instead.
 */

#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sysfs.h>

#include "core.h"
#include "lifecycle/bootstrap.h"
#include "lifecycle/gc.h"
#include "region/refs.h"
#include "sysfs/sysfs_gc.h"
#include "sysfs/sysfs_internal.h"
#include "test/test_hooks.h"

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

/*
 * /sys/fs/<fs>/node<N>/unmount_prepare - a write clears every row and reference this node holds on
 * the medium while its daemon is still up to take the turn. The umount helper writes it before it
 * stops the daemon, because kill_sb runs the same step after the daemon is gone and can then only
 * leave the slot to the admin's timeout. The clean step is node death, so it runs only once open and
 * create are closed and no file here is open. -EBUSY says a file is still open and nothing was
 * cleared. -EAGAIN says another prepare holds the mount or some entry's turn could not be taken,
 * and the write can be repeated. Once one prepare finds no reference, open and create stay closed.
 */
static ssize_t unmount_prepare_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	struct fs_sb_info *sbi = sysfs_sbi_of(kobj);
	s32 err = 0;

	if (fs_is_fenced(sbi) || !bootstrap_has_slot(sbi))
		return count; /* the admin's to clean, and nothing here to add */

	/* A second prepare is the helper retrying, and the helper already backs off on -EAGAIN. */
	if (!mutex_trylock(&sbi->drain_mutex))
		return -EAGAIN;
	if (sbi->drain_latched)
		goto clean;

	WRITE_ONCE(sbi->draining, true);
	smp_mb();
	/* Every create that read the flag clear holds this mutex until its entry is written. */
	mutex_lock(&sbi->meta_lock);
	mutex_unlock(&sbi->meta_lock);

	if (region_has_local_refs(sbi)) {
		test_delay_before_drain_refusal();
		WRITE_ONCE(sbi->draining, false);
		err = -EBUSY;
		goto out;
	}
	/* Past here the mount stays closed: what it owned is being released, even if umount fails. */
	sbi->drain_latched = true;

clean:
	if (!gc_clear_own_node(sbi))
		err = -EAGAIN;
out:
	mutex_unlock(&sbi->drain_mutex);
	return err ? err : (ssize_t)count;
}

static struct kobj_attribute unmount_prepare_attr = __ATTR(unmount_prepare, 0200, NULL, unmount_prepare_store);

static struct attribute *gc_mount_attrs[] = {
	&unmount_prepare_attr.attr,
	NULL,
};

/* Unnamed, so the file sits directly in the mount's directory. */
static const struct attribute_group gc_mount_attr_group = {
	.attrs = gc_mount_attrs,
};

s32 gc_create_mount_attrs(struct kobject *kobj)
{
	return sysfs_create_group(kobj, &gc_mount_attr_group);
}

void gc_delete_mount_attrs(struct kobject *kobj)
{
	sysfs_remove_group(kobj, &gc_mount_attr_group);
}
