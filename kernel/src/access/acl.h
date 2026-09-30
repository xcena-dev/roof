/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * acl.h - Region-level access control entry points.
 */

#ifndef _ACL_H
#define _ACL_H

#include <linux/types.h>

struct fs_sb_info;
struct layout_rat_entry;
struct pid;

/* ================================== What the calling task is ================================== */
/* Read once and carried to the helper, so what it judges is the task itself and not a later /proc. */
struct acl_caller_identity {
	u32 tgid;
	u32 uid;
	u32 gid;
	u64 birth_time;
	u64 exe_inode_ino;
	u32 exe_inode_dev;
	u64 exec_id;
};

/* A kernel thread gets zeros in the exe fields, which no row carries and no grant may write. */
void acl_read_caller_identity(struct acl_caller_identity *out);

/* The thread group leader's, which is the task a lookup of the stored tgid answers with. */
u64 acl_read_caller_birth_time(void);

/* What a reused pid and an execve fail on. False for a kernel thread. */
bool acl_get_exe_id(u64 *out_ino, u32 *out_dev);

/* Read here rather than by the helper, which would need ptrace for /proc/<pid>/exe. */
void acl_get_exe_path(char *out, u32 len);

/* ================================ Whether an access is allowed ================================ */
/* Every bit in @required_perms, or -EACCES. */
s32 acl_check_permission(struct fs_sb_info *sbi, u32 rat_entry_id, u32 required_perms);

/* ================================ Writing what a region grants =============================== */
/* What everyone holding no row of their own gets. 0 is a value and not an omission. */
s32 acl_set_default_perms(struct fs_sb_info *sbi, u32 rat_entry_id, u32 perms);

/* The helper upcall decides the bits, and the caller's identity is read here rather than passed. */
s32 acl_grant_deleg_caller(struct fs_sb_info *sbi, u32 rat_entry_id, u32 perms);

/* An account row asked for by a caller that has to earn it: EACCES holds neither ADMIN nor GRANT,
 * and EPERM holds GRANT and reaches past it. One node, one account, no process to outlive. */
s32 acl_grant_deleg_account(struct fs_sb_info *sbi, u32 rat_entry_id, u32 uid, u32 gid, u32 perms);

/* This node's row on the lock region, at the slot its node_id names. Reached on a capability,
 * before any turn exists, so the slot is computed rather than scanned for. */
s32 acl_set_lock_region_account(struct fs_sb_info *sbi, struct layout_rat_entry *rat_entry,
				u32 uid, u32 gid, u32 perms);

/* A row naming the caller asks nothing. Any other row needs FS_PERM_ADMIN. */
s32 acl_revoke_deleg_account(struct fs_sb_info *sbi, u32 rat_entry_id, u32 uid, u32 gid);

/* ========================== What a recorded identity is still worth =========================== */
/* The task a stored tgid names, looked up in the namespace that numbered it rather than in the
 * caller's. NULL when no task carries that number. The reference is the caller's to put. */
struct pid *acl_find_global_pid(u32 tgid);

/* Dead, or the pid now names a different process. */
bool acl_is_owner_dead(u32 owner_pid, u64 owner_birth_time);

#endif /* _ACL_H */
