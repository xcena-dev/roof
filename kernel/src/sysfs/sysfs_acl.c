// SPDX-License-Identifier: GPL-2.0-only
/*
 * sysfs_acl.c - perm_info and deleg_info.
 *
 * Both read the RAT through the first registered sbi under sysfs_lock. deleg_info takes the region
 * to read as a write, since one page holds one region's rows and not the table's.
 */

#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/sysfs.h>

#include "core.h"
#include "layout/layout_access.h"
#include "sysfs/sysfs_acl.h"
#include "sysfs/sysfs_internal.h"

/* /sys/fs/<fs>/perm_info */
static ssize_t perm_info_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	u32 len = 0;

	mutex_lock(&sysfs_lock);

	do {
		struct fs_sb_info *sbi = sysfs_get_sbi();
		if (!sbi) {
			len = sysfs_emit(buf, "No filesystem mounted\n");
			break;
		}

		if (!sbi->rat) {
			len = sysfs_emit(buf, "No RAT\n");
			break;
		}

		len += sysfs_emit_at(buf, len, "RAT_Entry\tDefault\tDeleg_bound\n");

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
			len += sysfs_emit_at(buf, len, "%u\t0x%04x\t%u\n", i,
					     acl.local.default_perms, acl.local.deleg_bound);
		}
	} while (false);

	mutex_unlock(&sysfs_lock);
	return len;
}
static struct kobj_attribute perm_info_attr = __ATTR_RO(perm_info);

/*
 * /sys/fs/<fs>/deleg_info - per-region delegation detail
 * Write a region_id (RAT index), then read back all delegation entries.
 */
static u32 deleg_info_region_id;

static ssize_t deleg_info_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
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

		u32 rid = deleg_info_region_id;
		struct layout_rat_entry *entry = layout_get_rat_entry(sbi, rid);
		if (!entry) {
			len = sysfs_emit(buf, "Invalid region_id %u\n", rid);
			break;
		}

		union layout_rat_hot_copy hot;
		cxl_get_layout_rat_hot(&hot, &entry->hot);
		/* OWNER_DEAD as well: that is the state whose rows an operator most needs to read. */
		if (!layout_rat_state_is_standing(hot.local.state)) {
			len = sysfs_emit(buf, "region %u holds no rows (state=%u)\n", rid, hot.local.state);
			break;
		}

		struct cxl_cacheline line;
		const char *name = layout_read_rat_name(entry, &line);
		len += sysfs_emit_at(buf, len, "region: %u  name: %s\n", rid, name);

		for (u32 i = 0; i < ACL_DELEG_MAX_ENTRIES; i++) {
			union acl_deleg_entry_copy copy;
			cxl_get_acl_deleg_entry(&copy, &entry->deleg_entries[i]);
			if (copy.local.state == ACL_DELEG_EMPTY)
				continue;

			/* pid 0 is an account row, judged on uid and gid. Anything else is a
			 * process row, whose uid and gid say which account it belongs to. */
			const char *kind = (copy.local.pid == 0) ? "account" : "process";
			len += sysfs_emit_at(buf, len,
					     "  deleg[%u]: %s state=%u node=%u pid=%u uid=%u gid=%u "
					     "perms=0x%x birth_time=%llu exec_id=%llu\n",
					     i, kind, copy.local.state, copy.local.node_id, copy.local.pid,
					     copy.local.uid, copy.local.gid, copy.local.perms, copy.local.birth_time,
					     copy.local.exec_id);
		}
	} while (false);

	mutex_unlock(&sysfs_lock);
	return len;
}

static ssize_t deleg_info_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	u32 rid;
	if (kstrtou32(buf, 0, &rid))
		return -EINVAL;
	if (rid >= LAYOUT_MAX_RAT_ENTRIES)
		return -EINVAL;

	deleg_info_region_id = rid;
	return count;
}

/* The rows name uids, gids and pids of other accounts, which is not a list any user may read. */
static struct kobj_attribute deleg_info_attr = __ATTR(deleg_info, 0600, deleg_info_show, deleg_info_store);

static struct attribute *acl_attrs[] = {
	&perm_info_attr.attr,
	&deleg_info_attr.attr,
	NULL,
};

/* Unnamed, so the files sit directly in the module's directory. */
static const struct attribute_group acl_attr_group = {
	.attrs = acl_attrs,
};

s32 acl_create_sysfs_attrs(struct kobject *kobj)
{
	return sysfs_create_group(kobj, &acl_attr_group);
}

void acl_delete_sysfs_attrs(struct kobject *kobj)
{
	sysfs_remove_group(kobj, &acl_attr_group);
}
