// SPDX-License-Identifier: GPL-2.0-only
/*
 * debug_sysfs.c - the GC pause knob, in a debug group of its own.
 *
 * The store takes CAP_SYS_ADMIN. The read-only counters an operator watches are in the sysfs_*.c
 * files and outside this group.
 *
 * This attribute reaches its sbi through sysfs_sbi_list, so it holds sysfs_lock. What it does under
 * that lock is one atomic per mount, so the lock covers the list and nothing that sleeps.
 */

#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/sysfs.h>

#include "core.h"
#include "debug/debug_sysfs.h"
#include "sysfs/sysfs_internal.h"

/*
 * Pauses the sweep without taking the thread down, so the liveness tick keeps running. Takes
 * "0"/"1" for every mount, or "<node_id>:0"/"<node_id>:1" for one.
 */
static ssize_t gc_pause_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	s32 len = 0;

	mutex_lock(&sysfs_lock);
	for (u32 i = 0; i < SYSFS_MAX_MOUNTS; i++) {
		if (sysfs_sbi_list[i])
			len += sysfs_emit_at(buf, len, "node%u:%d ", sysfs_sbi_list[i]->node_id,
					     atomic_read(&sysfs_sbi_list[i]->gc_paused));
	}
	mutex_unlock(&sysfs_lock);

	if (len == 0)
		return sysfs_emit(buf, "0\n");

	len += sysfs_emit_at(buf, len, "\n");
	return len;
}

static ssize_t gc_pause_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	const char *colon = strchr(buf, ':');
	if (colon) {
		/* kstrtou32 reads to the end of its string, so the node id is parsed from its own copy. */
		char node_text[12];
		const u32 node_len = (u32)(colon - buf);
		if (node_len == 0 || node_len >= sizeof(node_text))
			return -EINVAL;
		memcpy(node_text, buf, node_len);
		node_text[node_len] = '\0';

		u32 node_id;
		if (kstrtou32(node_text, 0, &node_id))
			return -EINVAL;

		s32 val;
		if (kstrtoint(colon + 1, 0, &val))
			return -EINVAL;

		mutex_lock(&sysfs_lock);
		struct fs_sb_info *sbi = sysfs_find_by_node(node_id);
		if (!sbi) {
			mutex_unlock(&sysfs_lock);
			return -ENOENT;
		}

		atomic_set(&sbi->gc_paused, val ? 1 : 0);
		mutex_unlock(&sysfs_lock);
		pr_debug("GC %s for node %u via sysfs\n", val ? "paused" : "resumed", node_id);
	} else {
		bool pause;
		if (kstrtobool(buf, &pause))
			return -EINVAL;

		mutex_lock(&sysfs_lock);
		for (u32 i = 0; i < SYSFS_MAX_MOUNTS; i++) {
			if (sysfs_sbi_list[i])
				atomic_set(&sysfs_sbi_list[i]->gc_paused, pause ? 1 : 0);
		}
		mutex_unlock(&sysfs_lock);
		pr_debug("GC %s (all) via sysfs\n", pause ? "paused" : "resumed");
	}

	return count;
}

static struct kobj_attribute gc_pause_attr = __ATTR(gc_pause, 0600, gc_pause_show, gc_pause_store);

static struct attribute *debug_sysfs_attrs[] = {
	&gc_pause_attr.attr,
	NULL,
};

static const struct attribute_group debug_sysfs_attr_group = {
	.name = "debug",
	.attrs = debug_sysfs_attrs,
};

s32 debug_create_sysfs_group(struct kobject *kobj)
{
	return sysfs_create_group(kobj, &debug_sysfs_attr_group);
}

void debug_delete_sysfs_group(struct kobject *kobj)
{
	sysfs_remove_group(kobj, &debug_sysfs_attr_group);
}
