// SPDX-License-Identifier: GPL-2.0-only
/*
 * sysfs.c - the module's sysfs tree: its directory, one directory per mount, and the sbi list the
 * attributes read through.
 *
 * The attributes themselves live in the sysfs_*.c files beside this one, one file per subject, and
 * each hangs its own group on the directory this file hands it.
 */

#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/sysfs.h>

#include "core.h"
#include "debug/debug_sysfs.h"
#include "sysfs/sysfs.h"
#include "sysfs/sysfs_acl.h"
#include "sysfs/sysfs_daemon.h"
#include "sysfs/sysfs_gc.h"
#include "sysfs/sysfs_internal.h"
#include "sysfs/sysfs_region.h"
#include "test/test_sysfs.h"

static struct kobject *sysfs_kobj;

/*
 * Nothing but the wrapper. The sbi it points at outlives this directory, because kobject_del waits
 * for a show already inside an attribute and super_kill_sb frees the sbi after that.
 */
static void sysfs_mount_release(struct kobject *kobj)
{
	kfree(container_of(kobj, struct sysfs_mount, kobj));
}

static const struct kobj_type sysfs_mount_type = {
	.sysfs_ops = &kobj_sysfs_ops,
	.release = sysfs_mount_release,
};

/*
 * A directory of this mount's own, named for the node it holds. An attribute about one mount is one
 * file there, which is what keeps it inside the single page a show is given.
 */
static s32 sysfs_create_mount_dir(struct fs_sb_info *sbi)
{
	struct sysfs_mount *mount = kzalloc(sizeof(*mount), GFP_KERNEL);

	if (!mount)
		return -ENOMEM;

	mount->sbi = sbi;

	s32 ret = kobject_init_and_add(&mount->kobj, &sysfs_mount_type, sysfs_kobj, "node%u",
				       sbi->node_id);
	if (ret)
		goto err_put;

	ret = gc_create_mount_attrs(&mount->kobj);
	if (ret)
		goto err_put;

	ret = daemon_create_mount_attrs(&mount->kobj);
	if (ret)
		goto err_gc;

	ret = test_create_mount_attrs(&mount->kobj);
	if (ret)
		goto err_daemon;

	sbi->sysfs_mount = mount;
	return 0;

err_daemon:
	daemon_delete_mount_attrs(&mount->kobj);
err_gc:
	gc_delete_mount_attrs(&mount->kobj);
err_put:
	kobject_put(&mount->kobj);
	return ret;
}

/* Deleted before the put, so an attribute still inside a show finishes before the sbi goes. */
static void sysfs_delete_mount_dir(struct fs_sb_info *sbi)
{
	struct sysfs_mount *mount = sbi->sysfs_mount;

	if (!mount)
		return;

	sbi->sysfs_mount = NULL;
	test_delete_mount_attrs(&mount->kobj);
	daemon_delete_mount_attrs(&mount->kobj);
	gc_delete_mount_attrs(&mount->kobj);
	kobject_del(&mount->kobj);
	kobject_put(&mount->kobj);
}

/* Shared with the sysfs_*.c files via sysfs_internal.h. */
struct fs_sb_info *sysfs_sbi_list[SYSFS_MAX_MOUNTS];

/* Covers the list, and with it the lifetime of an sbi found there: super_kill_sb unregisters before
 * it frees, so every listed sbi stays alive for as long as this is held. */
DEFINE_MUTEX(sysfs_lock);

struct fs_sb_info *sysfs_get_sbi(void)
{
	for (u32 i = 0; i < SYSFS_MAX_MOUNTS; i++) {
		if (sysfs_sbi_list[i])
			return sysfs_sbi_list[i];
	}
	return NULL;
}

struct fs_sb_info *sysfs_find_by_node(u32 node_id)
{
	for (u32 i = 0; i < SYSFS_MAX_MOUNTS; i++) {
		if (sysfs_sbi_list[i] && sysfs_sbi_list[i]->node_id == node_id)
			return sysfs_sbi_list[i];
	}
	return NULL;
}

/*
 * Each step unwinds what the ones before it made, so a failure leaves nothing under fs_kobj. The
 * caller has no handle to clean up with: a failed init means the module never loads.
 */
s32 sysfs_init(void)
{
	sysfs_kobj = kobject_create_and_add(KBUILD_MODNAME, fs_kobj);
	if (!sysfs_kobj)
		return -ENOMEM;

	s32 ret = region_create_sysfs_attrs(sysfs_kobj);
	if (ret)
		goto err_kobj;

	ret = acl_create_sysfs_attrs(sysfs_kobj);
	if (ret)
		goto err_region;

	ret = gc_create_sysfs_attrs(sysfs_kobj);
	if (ret)
		goto err_acl;

	ret = debug_create_sysfs_group(sysfs_kobj);
	if (ret)
		goto err_gc;

	ret = test_create_sysfs_group(sysfs_kobj);
	if (ret)
		goto err_debug;

	return 0;

err_debug:
	debug_delete_sysfs_group(sysfs_kobj);
err_gc:
	gc_delete_sysfs_attrs(sysfs_kobj);
err_acl:
	acl_delete_sysfs_attrs(sysfs_kobj);
err_region:
	region_delete_sysfs_attrs(sysfs_kobj);
err_kobj:
	kobject_put(sysfs_kobj);
	sysfs_kobj = NULL;
	return ret;
}

void sysfs_exit(void)
{
	if (sysfs_kobj) {
		test_delete_sysfs_group(sysfs_kobj);
		debug_delete_sysfs_group(sysfs_kobj);
		gc_delete_sysfs_attrs(sysfs_kobj);
		acl_delete_sysfs_attrs(sysfs_kobj);
		region_delete_sysfs_attrs(sysfs_kobj);
		kobject_put(sysfs_kobj);
		sysfs_kobj = NULL;
	}
}

s32 sysfs_register(struct fs_sb_info *sbi)
{
	bool listed = false;

	mutex_lock(&sysfs_lock);
	for (u32 i = 0; i < SYSFS_MAX_MOUNTS; i++) {
		if (!sysfs_sbi_list[i]) {
			sysfs_sbi_list[i] = sbi;
			listed = true;
			break;
		}
	}
	mutex_unlock(&sysfs_lock);

	if (!listed)
		pr_warn("sysfs: max %d mounts reached, sysfs may not track this mount\n",
			SYSFS_MAX_MOUNTS);

	/* A mount with no node_id yet has no name for a directory and nothing to publish in one. */
	if (sbi->node_id != 0) {
		s32 ret = sysfs_create_mount_dir(sbi);

		if (ret)
			pr_warn("sysfs: no directory for node %u: %d\n", sbi->node_id, ret);
	}

	return 0; /* Non-fatal: mount still succeeds */
}

void sysfs_unregister(struct fs_sb_info *sbi)
{
	sysfs_delete_mount_dir(sbi);

	mutex_lock(&sysfs_lock);
	for (u32 i = 0; i < SYSFS_MAX_MOUNTS; i++) {
		if (sysfs_sbi_list[i] == sbi) {
			sysfs_sbi_list[i] = NULL;
			mutex_unlock(&sysfs_lock);
			return;
		}
	}
	mutex_unlock(&sysfs_lock);
}
