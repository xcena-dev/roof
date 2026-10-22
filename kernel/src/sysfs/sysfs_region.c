// SPDX-License-Identifier: GPL-2.0-only
/*
 * sysfs_region.c - region_info, pool_info and the rat/ and extents/ directories.
 *
 * Every attribute here reads the RAT through the first registered sbi, so each holds sysfs_lock
 * for the read and answers with what the medium held at that moment.
 */

#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/sysfs.h>

#include "core.h"
#include "layout/layout_access.h"
#include "region/region.h"
#include "sysfs/sysfs_internal.h"
#include "sysfs/sysfs_region.h"

static struct region_extent sysfs_read_region_columns(struct fs_sb_info *sbi, u32 idx,
						      const union layout_rat_hot_copy *hot)
{
	struct region_extent extents[LAYOUT_MAX_EXTENTS];
	const s32 count = region_read_extents(sbi, idx, hot, extents);
	if (count <= 0)
		return (struct region_extent){ 0 };
	return (struct region_extent){ .offset = extents[0].offset, .length = region_sum_extents(extents, (u32)count) };
}

/* /sys/fs/<fs>/region_info */
static ssize_t region_info_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	u32 len = 0;

	mutex_lock(&sysfs_lock);

	/* One exit, so the unlock below is the only one. Every refusal breaks out with its
	 * reason already in @buf. */
	do {
		struct fs_sb_info *sbi = sysfs_get_sbi();
		if (!sbi) {
			len = sysfs_emit(buf, "No filesystem mounted\n");
			break;
		}

		len += sysfs_emit_at(buf, len, "RAT_Entry\tNode\tPID\tState\tSize\tOffset\tName\n");

		for (u32 i = 0; i < LAYOUT_MAX_RAT_ENTRIES; i++) {
			struct layout_rat_entry *entry = layout_get_rat_entry(sbi, i);
			if (!entry)
				continue;

			union layout_rat_hot_copy hot;
			cxl_get_layout_rat_hot(&hot, &entry->hot);
			if (!layout_rat_state_is_standing(hot.local.state))
				continue;

			union layout_rat_acl_copy acl;
			cxl_get_layout_rat_acl(&acl, &entry->acl);

			struct cxl_cacheline line;
			const char *name = layout_read_rat_name(entry, &line);

			/* Where an operator sees a region outliving the node that made it, so it is
			 * named rather than left out. */
			const struct region_extent columns = sysfs_read_region_columns(sbi, i, &hot);
			len += sysfs_emit_at(buf, len, "%u\t%u\t%u\t%s\t%llu\t0x%llx\t%s\n", i,
					     acl.local.owner_node_id, acl.local.owner_pid,
					     hot.local.state == LAYOUT_RAT_ENTRY_OWNER_DEAD ? "OWNER_DEAD" : "ALLOCATED",
					     columns.length, columns.offset, name);
		}
	} while (false);

	mutex_unlock(&sysfs_lock);
	return len;
}
static struct kobj_attribute region_info_attr = __ATTR_RO(region_info);

/*
 * pool_info - what each pool holds, in bytes, which statfs can only report summed.
 *
 * One row per pool carrying its start, its size and what is free in it, so a reader sees a full
 * pool beside an empty one rather than the one total that hides it.
 */
static ssize_t pool_info_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	u32 len = 0;

	mutex_lock(&sysfs_lock);

	struct fs_sb_info *sbi = sysfs_get_sbi();
	struct region_pool_usage usage;
	if (!sbi) {
		len = sysfs_emit(buf, "No filesystem mounted\n");
	} else if (region_get_pool_usage(sbi, &usage)) {
		len = sysfs_emit(buf, "No RAT\n");
	} else {
		len += sysfs_emit_at(buf, len, "granule %llu\n", sbi->granule);
		len += sysfs_emit_at(buf, len, "uncached %llu %llu %llu\n", sbi->uc_start,
				     usage.uc_total, usage.uc_total - usage.uc_used);
		len += sysfs_emit_at(buf, len, "writeback %llu %llu %llu\n",
				     layout_get_primary_wb_start(sbi),
				     usage.wb_total, usage.wb_total - usage.wb_used);
		for (u32 device = 0; device < sbi->device_count; device++) {
			const struct fs_device *dev = &sbi->devices[device];
			const u64 total = dev->size - dev->wb_start;
			len += sysfs_emit_at(buf, len, "device %u %llu %llu %llu\n", device, dev->wb_start,
					     total, total - usage.device_wb_used[device]);
		}
	}

	mutex_unlock(&sysfs_lock);
	return len;
}
static struct kobj_attribute pool_info_attr = __ATTR_RO(pool_info);

static struct attribute *region_attrs[] = {
	&pool_info_attr.attr,
	&region_info_attr.attr,
	NULL,
};

/* Unnamed, so the files sit directly in the module's directory. */
static const struct attribute_group region_attr_group = {
	.attrs = region_attrs,
};

/*
 * /sys/fs/<fs>/rat/<slot>: one file per RAT slot, so a reader that wants every region loops over
 * the slots and never meets the one page a single show is cut at. The row is region_info's, and a
 * slot with no region answers "free".
 */
static struct kobj_attribute rat_slot_attrs[LAYOUT_MAX_RAT_ENTRIES];
static char rat_slot_names[LAYOUT_MAX_RAT_ENTRIES][4];
static struct attribute *rat_slot_attr_ptrs[LAYOUT_MAX_RAT_ENTRIES + 1];
_Static_assert(LAYOUT_MAX_RAT_ENTRIES <= 1000, "a slot's file name is at most three digits");

static const char *region_read_rat_state_name(u32 state)
{
	switch (state) {
	case LAYOUT_RAT_ENTRY_ALLOCATING:
		return "ALLOCATING";
	case LAYOUT_RAT_ENTRY_ALLOCATED:
		return "ALLOCATED";
	case LAYOUT_RAT_ENTRY_DELETING:
		return "DELETING";
	case LAYOUT_RAT_ENTRY_OWNER_DEAD:
		return "OWNER_DEAD";
	default:
		return "free";
	}
}

static ssize_t rat_slot_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	const u32 slot = (u32)(attr - rat_slot_attrs);
	ssize_t len;

	mutex_lock(&sysfs_lock);
	do {
		struct fs_sb_info *sbi = sysfs_get_sbi();
		struct layout_rat_entry *entry = sbi ? layout_get_rat_entry(sbi, slot) : NULL;
		if (!entry) {
			len = sysfs_emit(buf, "No filesystem mounted\n");
			break;
		}

		union layout_rat_hot_copy hot;
		cxl_get_layout_rat_hot(&hot, &entry->hot);
		if (hot.local.state == LAYOUT_RAT_ENTRY_FREE) {
			len = sysfs_emit(buf, "free\n");
			break;
		}

		union layout_rat_acl_copy acl;
		cxl_get_layout_rat_acl(&acl, &entry->acl);
		struct cxl_cacheline line;
		const char *name = layout_read_rat_name(entry, &line);
		const struct region_extent columns = sysfs_read_region_columns(sbi, slot, &hot);
		len = sysfs_emit(buf, "%u\t%u\t%s\t%llu\t0x%llx\t%s\n", acl.local.owner_node_id, acl.local.owner_pid,
				 region_read_rat_state_name(hot.local.state), columns.length, columns.offset, name);
	} while (false);
	mutex_unlock(&sysfs_lock);
	return len;
}

static const struct attribute_group rat_slot_attr_group = {
	.name = "rat",
	.attrs = rat_slot_attr_ptrs,
};

/*
 * /sys/fs/<fs>/extents/<slot>: every piece of the slot's region, since rat/ shows a paged region
 * only by its first piece and its total.
 */
static struct kobj_attribute extent_slot_attrs[LAYOUT_MAX_RAT_ENTRIES];
static struct attribute *extent_slot_attr_ptrs[LAYOUT_MAX_RAT_ENTRIES + 1];

static ssize_t extent_slot_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	const u32 slot = (u32)(attr - extent_slot_attrs);
	u32 len = 0;

	mutex_lock(&sysfs_lock);
	struct fs_sb_info *sbi = sysfs_get_sbi();
	struct layout_rat_entry *entry = sbi ? layout_get_rat_entry(sbi, slot) : NULL;
	if (!entry) {
		len = sysfs_emit(buf, "No filesystem mounted\n");
	} else {
		union layout_rat_hot_copy hot;
		cxl_get_layout_rat_hot(&hot, &entry->hot);
		struct region_extent extents[LAYOUT_MAX_EXTENTS];
		const s32 count =
			layout_rat_state_is_standing(hot.local.state) ? region_read_extents(sbi, slot, &hot, extents) : 0;
		for (s32 piece = 0; piece < count; piece++)
			len += sysfs_emit_at(buf, len, "0x%llx\t%llu\t%u\n", extents[piece].offset,
					     extents[piece].length, extents[piece].device_id);
	}
	mutex_unlock(&sysfs_lock);
	return len;
}

static const struct attribute_group extent_slot_attr_group = {
	.name = "extents",
	.attrs = extent_slot_attr_ptrs,
};

static void region_name_rat_slots(void)
{
	for (u32 slot = 0; slot < LAYOUT_MAX_RAT_ENTRIES; slot++) {
		snprintf(rat_slot_names[slot], sizeof(rat_slot_names[slot]), "%u", slot);
		rat_slot_attrs[slot].attr.name = rat_slot_names[slot];
		rat_slot_attrs[slot].attr.mode = 0444;
		rat_slot_attrs[slot].show = rat_slot_show;
		rat_slot_attr_ptrs[slot] = &rat_slot_attrs[slot].attr;

		extent_slot_attrs[slot].attr.name = rat_slot_names[slot];
		extent_slot_attrs[slot].attr.mode = 0444;
		extent_slot_attrs[slot].show = extent_slot_show;
		extent_slot_attr_ptrs[slot] = &extent_slot_attrs[slot].attr;
	}
	rat_slot_attr_ptrs[LAYOUT_MAX_RAT_ENTRIES] = NULL;
	extent_slot_attr_ptrs[LAYOUT_MAX_RAT_ENTRIES] = NULL;
}

s32 region_create_sysfs_attrs(struct kobject *kobj)
{
	s32 ret = sysfs_create_group(kobj, &region_attr_group);
	if (ret)
		return ret;

	region_name_rat_slots();
	ret = sysfs_create_group(kobj, &rat_slot_attr_group);
	if (ret)
		goto out_region;
	ret = sysfs_create_group(kobj, &extent_slot_attr_group);
	if (ret)
		goto out_rat;
	return 0;

out_rat:
	sysfs_remove_group(kobj, &rat_slot_attr_group);
out_region:
	sysfs_remove_group(kobj, &region_attr_group);
	return ret;
}

void region_delete_sysfs_attrs(struct kobject *kobj)
{
	sysfs_remove_group(kobj, &extent_slot_attr_group);
	sysfs_remove_group(kobj, &rat_slot_attr_group);
	sysfs_remove_group(kobj, &region_attr_group);
}
