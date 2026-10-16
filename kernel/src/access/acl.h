/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * acl.h - Region-level access control entry points.
 */

#ifndef _ACL_H
#define _ACL_H

#include <linux/types.h>

#include "cxl/io.h" /* struct cxl_cacheline, which a taken row carries by value */
#include "daemon_uapi.h" /* FS_DAEMON_IDENT_LEN */
#include "test/test_hooks.h" /* enum test_deleg_scan */

struct fs_sb_info;
struct layout_rat_entry;
struct pid;
union acl_deleg_entry;
union layout_rat_acl_copy;

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

/* =================================== Who a role row names ==================================== */
/* Two NUL-padded labels and the node the row answers on, 0 for every node. */
struct acl_role_key {
	char group[FS_DAEMON_IDENT_LEN];
	char role[FS_DAEMON_IDENT_LEN];
	u32 target_node;
};

/* @src as a row holds a label: 1 to FS_DAEMON_IDENT_LEN - 1 bytes, NUL-padded. False for an empty or
 * unterminated one, which names no workload. */
bool acl_copy_label(char *dst, const char *src);

/* ================================= Which rows a scan reads =================================== */
/* One taken row as a scan visits it: its slot, its address, and its line read in one load, so state,
 * kind and node come from the same instant. @rows_left is the walk's own. */
struct acl_taken_row {
	u32 rows_left;
	u32 idx;
	union acl_deleg_entry *row;
	struct cxl_cacheline line;
};

/* Reads the next row @pos has left, counted for @scan. False once none is left. */
bool acl_next_taken_row(struct fs_sb_info *sbi, struct layout_rat_entry *rat_entry, enum test_deleg_scan scan,
			struct acl_taken_row *pos);

/* Each row @taken marks, lowest first, and no other: no ACTIVE row sits under a clear bit. */
#define acl_for_each_taken_row(sbi, rat_entry, taken, scan, pos)                       \
	for (struct acl_taken_row pos = { .rows_left = (taken) & ACL_DELEG_ALL_ROWS }; \
	     acl_next_taken_row((sbi), (rat_entry), (scan), &(pos));)

/* Takes @rows off @acl's mask, once the caller has emptied them under the turn, and puts the line back.
 * The lock region keeps its node slots: bring-up rewrites one there with no turn to mark it under. */
void acl_clear_taken_rows(struct layout_rat_entry *rat_entry, union layout_rat_acl_copy *acl, u32 rows);

/* ================================ Whether an access is allowed ================================ */
/* Every bit in @required_perms, or -EACCES. */
s32 acl_check_permission(struct fs_sb_info *sbi, u32 rat_entry_id, u32 required_perms);

/* ================================ Writing what a region grants =============================== */
/* What everyone holding no row of their own gets. 0 is a value and not an omission. */
s32 acl_set_default_perms(struct fs_sb_info *sbi, u32 rat_entry_id, u32 perms);

/* The helper's bits and the role rows standing for @labels decide, or -EACCES when together they miss
 * a bit of @required_perms. The caller's identity is read here rather than passed. */
s32 acl_grant_deleg_caller(struct fs_sb_info *sbi, u32 rat_entry_id, u32 policy_perms,
			   const struct acl_role_key *labels, u32 required_perms);

/* An account row asked for by a caller that has to earn it: EACCES holds neither ADMIN nor GRANT,
 * and EPERM holds GRANT and reaches past it. One node, one account, no process to outlive. */
s32 acl_grant_deleg_account(struct fs_sb_info *sbi, u32 rat_entry_id, u32 uid, u32 gid, u32 perms);

/* A role row, earned as an account row is. */
s32 acl_grant_deleg_role(struct fs_sb_info *sbi, u32 rat_entry_id, const struct acl_role_key *key, u32 perms);

/* This node's row on the lock region, at the slot its node_id names. Reached on a capability,
 * before any turn exists, so the slot is computed rather than scanned for. */
s32 acl_set_lock_region_account(struct fs_sb_info *sbi, struct layout_rat_entry *rat_entry,
				u32 uid, u32 gid, u32 perms);

/* A row naming the caller asks nothing. Any other row needs FS_PERM_ADMIN. */
s32 acl_revoke_deleg_account(struct fs_sb_info *sbi, u32 rat_entry_id, u32 uid, u32 gid);

/* The role row this node wrote for @key. A process row it earned stays. Needs FS_PERM_ADMIN. */
s32 acl_revoke_deleg_role(struct fs_sb_info *sbi, u32 rat_entry_id, const struct acl_role_key *key);

/* ========================== What a recorded identity is still worth =========================== */
/* The task a stored tgid names, looked up in the namespace that numbered it rather than in the
 * caller's. NULL when no task carries that number. The reference is the caller's to put. */
struct pid *acl_find_global_pid(u32 tgid);

/* Dead, or the pid now names a different process. */
bool acl_is_owner_dead(u32 owner_pid, u64 owner_birth_time);

#endif /* _ACL_H */
