// SPDX-License-Identifier: GPL-2.0-only
/*
 * acl.c - permission delegation
 *
 * A region is reached, cheapest first, through the owner the entry names, the entry's default_perms,
 * then its delegation rows. A process row and an account row answer on the node that wrote them.
 * A role row names a workload by its labels, so only the helper's answer to an access can match it.
 */

#include <linux/cred.h>
#include <linux/dcache.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/kdev_t.h>
#include <linux/kernel.h>
#include <linux/mm_types.h>
#include <linux/pid.h>
#include <linux/pid_namespace.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/string.h>

#include "access/acl.h"
#include "access/meta_lock.h"
#include "core.h"
#include "layout/layout_access.h"
#include "util/bitmap.h"

/* A bit no permission names, which no row may carry and no caller may ask a write to store. */
static bool acl_has_unknown_perms(u32 perms)
{
	return (perms & ~(u32)FS_PERM_ALL) != 0;
}

/* What an owner or a row binds to besides the pid, so an execve into another binary stops
 * matching. False for a caller with no exe, which is a kernel thread. */
bool acl_get_exe_id(u64 *out_ino, u32 *out_dev)
{
	struct mm_struct *mm = current->mm;
	struct file *exe = mm ? READ_ONCE(mm->exe_file) : NULL;

	if (!exe || !exe->f_inode)
		return false;

	*out_ino = exe->f_inode->i_ino;
	*out_dev = new_encode_dev(exe->f_inode->i_sb->s_dev);
	return true;
}

void acl_get_exe_path(char *out, u32 len)
{
	if (!out || len == 0)
		return;
	out[0] = '\0';

	struct mm_struct *mm = current->mm;
	struct file *exe = mm ? READ_ONCE(mm->exe_file) : NULL;
	if (!exe)
		return;

	/* d_path fills from the end and answers where it started, so the result is moved down to
	 * the front. A path longer than @len leaves the empty string a caller can screen on. */
	char *found = d_path(&exe->f_path, out, len);
	if (IS_ERR(found)) {
		out[0] = '\0';
		return;
	}
	memmove(out, found, strlen(found) + 1);
}

/* Every pid this module records is a tgid the initial namespace numbered, so a lookup of one names
 * that namespace. The caller's own would read the number as one of its container's. */
struct pid *acl_find_global_pid(u32 tgid)
{
	rcu_read_lock();
	struct pid *found = get_pid(find_pid_ns((s32)tgid, &init_pid_ns));
	rcu_read_unlock();

	return found;
}

/* Dead, or the pid now names a different process. */
bool acl_is_owner_dead(u32 owner_pid, u64 owner_birth_time)
{
	struct pid *pid_s;
	struct task_struct *task;
	bool dead = true;

	/* A record naming no owner is not a dead one, and answering otherwise would have a caller
	 * collect on an entry it knows nothing about. No caller reaches this today. */
	if (owner_pid == 0)
		return false;

	pid_s = acl_find_global_pid(owner_pid);
	if (!pid_s)
		return true;

	task = get_pid_task(pid_s, PIDTYPE_PID);
	if (task) {
		u64 bt = ktime_to_ns(task->start_boottime);
		if (bt == owner_birth_time)
			dead = false;
		put_task_struct(task);
	}
	put_pid(pid_s);
	return dead;
}

/* The leader's, because every record that stores it stores a tgid beside it. A thread's own would
 * pair a leader's pid with a time no lookup of that pid can produce. */
u64 acl_read_caller_birth_time(void)
{
	return ktime_to_ns(current->group_leader->start_boottime);
}

/* Node, tgid, start time and exe binary, all screened from the line @acl already holds. The last
 * two are what a reused pid and an execve fail on. */
static bool acl_is_owner(struct fs_sb_info *sbi, const struct layout_rat_acl_local *acl)
{
	/* The stored owner_pid is a tgid, which is what userspace calls a pid. */
	if (acl->owner_node_id != sbi->node_id ||
	    acl->owner_pid != current->tgid ||
	    acl->owner_birth_time != acl_read_caller_birth_time())
		return false;

	u64 exe_ino;
	u32 exe_dev;

	if (!acl_get_exe_id(&exe_ino, &exe_dev))
		return false;

	return exe_ino == acl->owner_exe_inode_ino && exe_dev == acl->owner_exe_inode_dev;
}

void acl_read_caller_identity(struct acl_caller_identity *out)
{
	out->tgid = (u32)current->tgid;

	/* The leader's account, read as one snapshot: authority is not divided below a process, so a
	 * thread carrying ids of its own is still the principal its leader names. */
	rcu_read_lock();
	const struct cred *leader_cred = __task_cred(current->group_leader);
	out->uid = leader_cred->uid.val;
	out->gid = leader_cred->gid.val;
	rcu_read_unlock();

	out->birth_time = acl_read_caller_birth_time();
	out->exec_id = READ_ONCE(current->group_leader->self_exec_id);
	out->exe_inode_ino = 0;
	out->exe_inode_dev = 0;
	(void)acl_get_exe_id(&out->exe_inode_ino, &out->exe_inode_dev);
}

/* Indexed without the accessor's range check: the walk starts from a mask cut to the table's rows. */
bool acl_next_taken_row(struct fs_sb_info *sbi, struct layout_rat_entry *rat_entry, enum test_deleg_scan scan,
			struct acl_taken_row *pos)
{
	if (!pos->rows_left)
		return false;

	pos->idx = bits_take_lowest(&pos->rows_left);
	pos->row = &rat_entry->deleg_entries[pos->idx];
	cxl_get_cacheline(&pos->line, pos->row);
	test_count_deleg_read(sbi->node_id, scan);
	return true;
}

void acl_clear_taken_rows(struct layout_rat_entry *rat_entry, union layout_rat_acl_copy *acl, u32 rows)
{
	union layout_rat_hot_copy hot;
	cxl_get_layout_rat_hot(&hot, &rat_entry->hot);
	if (hot.local.region_type == LAYOUT_REGION_LOCK)
		rows &= ~(u32)ACL_DELEG_NODE_ROWS;
	if (!(acl->local.deleg_taken & rows))
		return;

	acl->local.deleg_taken &= ~rows;
	cxl_set_layout_rat_acl(&rat_entry->acl, acl);
}

/* An ACTIVE row this node wrote. node_id is exact, so a row a crash left at 0 matches nobody, and
 * one another node wrote answers for nobody here. */
static bool acl_is_row_active_on_node(const struct acl_deleg_process_local *head, u32 node_id)
{
	return head->state == ACL_DELEG_ACTIVE && head->node_id == node_id;
}

/* The task the row was written for, execve generation aside. Any other kind names no process and so
 * answers for no caller here. */
static bool acl_match_deleg_task(const struct acl_deleg_process_local *row,
				 const struct acl_caller_identity *caller)
{
	if (row->kind != ACL_DELEG_KIND_PROCESS || row->pid != caller->tgid)
		return false;
	if (row->birth_time != caller->birth_time)
		return false; /* PID reuse */
	if (row->exe_inode_ino != caller->exe_inode_ino || row->exe_inode_dev != caller->exe_inode_dev)
		return false; /* execve into another binary */
	return true;
}

/* Every field is compared, because the write that makes a process row fills all of them. The
 * generation is what a re-exec of the same binary changes, and nothing else in the row does. */
static bool acl_match_deleg_process(const struct acl_deleg_process_local *row,
				    const struct acl_caller_identity *caller)
{
	return acl_match_deleg_task(row, caller) &&
	       row->exec_id == caller->exec_id;
}

/* Every field that names somebody has to match, and ACL_DELEG_ANY_ID in one leaves the other to
 * decide. A row naming neither is not a delegation, and the grant path refuses to write one, so a
 * corrupt row reaching here matches nobody.
 *
 * in_group_p and not a compare against @caller->gid: a row's gid matches a supplementary group too,
 * and only the caller's own credentials answer that. */
static bool acl_match_deleg_account(const struct acl_deleg_account_local *row,
				    const struct acl_caller_identity *caller)
{
	if (row->uid == ACL_DELEG_ANY_ID && row->gid == ACL_DELEG_ANY_ID)
		return false;
	if (row->uid != ACL_DELEG_ANY_ID && caller->uid != row->uid)
		return false;
	if (row->gid != ACL_DELEG_ANY_ID && !in_group_p(make_kgid(&init_user_ns, row->gid)))
		return false;

	return true;
}

/* The read side: what a standing row grants the caller, by the rule the row's own kind picks. The
 * process view reads the head of @line, since every view's head sits in the same place. */
static bool acl_match_deleg_caller(const struct cxl_cacheline *line, u32 node_id,
				   const struct acl_caller_identity *caller)
{
	union acl_deleg_process_copy process;
	cxl_decode_acl_deleg_process(&process, line);
	if (!acl_is_row_active_on_node(&process.local, node_id))
		return false;

	if (process.local.kind == ACL_DELEG_KIND_PROCESS)
		return acl_match_deleg_process(&process.local, caller);

	/* A role row needs the caller's labels, which only the helper's answer carries. */
	if (process.local.kind != ACL_DELEG_KIND_ACCOUNT)
		return false;

	union acl_deleg_account_copy account;
	cxl_decode_acl_deleg_account(&account, line);
	return acl_match_deleg_account(&account.local, caller);
}

/* The holder a write scan looks for, so the write lands on that holder's row and not beside it.
 * Neither @process nor @role means an account row, which ids name and no process outlives. */
struct acl_deleg_target {
	const struct acl_caller_identity *process;
	const struct acl_role_key *role;
	u32 uid;
	u32 gid;
};

/* The write side: whether @row is the one already naming @want, so a rerun rewrites it instead of
 * taking a second slot. Exact where the read side is not, because ANY_ID here is a stored value to
 * find and the ids name a holder the caller may not be. */
static bool acl_match_deleg_target(const struct cxl_cacheline *line, u32 node_id,
				   const struct acl_deleg_target *want)
{
	union acl_deleg_process_copy process;
	cxl_decode_acl_deleg_process(&process, line);
	if (!acl_is_row_active_on_node(&process.local, node_id))
		return false;

	/* The task and not the generation: a re-exec rewrites the row it already has rather than
	 * leaving the old one to sit until the process dies. */
	if (want->process)
		return acl_match_deleg_task(&process.local, want->process);

	if (want->role) {
		if (process.local.kind != ACL_DELEG_KIND_ROLE)
			return false;

		union acl_deleg_role_copy role;
		cxl_decode_acl_deleg_role(&role, line);
		return role.local.target_node == want->role->target_node &&
		       memcmp(role.local.group, want->role->group, sizeof(role.local.group)) == 0 &&
		       memcmp(role.local.role, want->role->role, sizeof(role.local.role)) == 0;
	}

	if (process.local.kind != ACL_DELEG_KIND_ACCOUNT)
		return false;

	union acl_deleg_account_copy account;
	cxl_decode_acl_deleg_account(&account, line);
	return account.local.uid == want->uid && account.local.gid == want->gid;
}

/* @want among these credentials' own and supplementary groups, which is what in_group_p answers for
 * the running task and nothing answers for another one. */
static bool acl_cred_has_group(const struct cred *cred, kgid_t want)
{
	if (gid_eq(cred->fsgid, want))
		return true;
	if (!cred->group_info)
		return false;

	for (s32 slot = 0; slot < cred->group_info->ngroups; slot++) {
		if (gid_eq(cred->group_info->gid[slot], want))
			return true;
	}

	return false;
}

/* Whether the process @row was written for carries @gid now. The row holds the one gid its holder
 * ran under, so a supplementary group of that holder is read off the holder or nowhere. */
static bool acl_holder_has_group(const struct acl_deleg_process_local *row, u32 gid)
{
	if (row->gid == gid)
		return true;

	struct pid *holder = acl_find_global_pid(row->pid);
	if (!holder)
		return false;

	bool carries = false;
	struct task_struct *task = get_pid_task(holder, PIDTYPE_PID);
	if (task) {
		/* The birth time as well: a pid the kernel handed out again names another process, and
		 * that process's groups answer for this row not at all. */
		if (ktime_to_ns(task->start_boottime) == row->birth_time) {
			const struct cred *cred = get_task_cred(task);

			carries = acl_cred_has_group(cred, make_kgid(&init_user_ns, gid));
			put_cred(cred);
		}
		put_task_struct(task);
	}
	put_pid(holder);

	return carries;
}

/* A process row belonging to the account @uid and @gid name, so an account revoke reaches the rows
 * the helper wrote for that account's processes. Either id left open decides nothing. */
static bool acl_match_deleg_process_account(const struct acl_deleg_process_local *row, u32 node_id,
					    u32 uid, u32 gid)
{
	if (!acl_is_row_active_on_node(row, node_id) || row->kind != ACL_DELEG_KIND_PROCESS)
		return false;
	if (uid != ACL_DELEG_ANY_ID && row->uid != uid)
		return false;
	/* The holder and not the row, and only a revoke naming a gid pays for that lookup. */
	if (gid != ACL_DELEG_ANY_ID && !acl_holder_has_group(row, gid))
		return false;

	return true;
}

/* The scan with both lines already in hand, so a writer under meta_lock reads them once for the
 * decision and for the row it goes on to write. @hot must be standing for this to mean anything. */
static u32 acl_read_granted(struct fs_sb_info *sbi, struct layout_rat_entry *rat_entry,
			    const union layout_rat_hot_copy *hot, const union layout_rat_acl_copy *acl,
			    u32 candidate)
{
	/* The state and not the id: a recovery that died between its two writes leaves this state over
	 * an id that still names a node, and a later holder of that id is not the owner. */
	if (hot->local.state != LAYOUT_RAT_ENTRY_OWNER_DEAD &&
	    acl_is_owner(sbi, &acl->local))
		return candidate;

	u32 have = acl->local.default_perms & candidate;
	if (have == candidate)
		return have;

	/* Read before the loop: every process row compares against the same identity. */
	struct acl_caller_identity caller;
	acl_read_caller_identity(&caller);

	acl_for_each_taken_row(sbi, rat_entry, acl->local.deleg_taken, TEST_DELEG_SCAN_CHECK, pos) {
		if (!acl_match_deleg_caller(&pos.line, sbi->node_id, &caller))
			continue;

		union acl_deleg_process_copy head;
		cxl_decode_acl_deleg_process(&head, &pos.line);
		u32 grant = head.local.perms & candidate;
		if (!grant)
			continue;

		have |= grant;
		if (have == candidate)
			break;
	}

	return have;
}

/* The rights the caller holds within @candidate, so a caller can branch on which bits matched.
 * An entry that is not ALLOCATED answers 0 with nothing granted. */
static s32 acl_check_permission_any(struct fs_sb_info *sbi, u32 rat_entry_id, u32 candidate, u32 *out_granted)
{
	struct layout_rat_entry *rat_entry = layout_get_rat_entry(sbi, rat_entry_id);
	if (!rat_entry)
		return -EIO;
	if (!out_granted || candidate == 0)
		return -EINVAL;

	/* Grant nothing until a path says otherwise. Every return below this line
	 * answers 0, so a caller reading @out_granted must find a written value. */
	*out_granted = 0;

	union layout_rat_hot_copy hot;
	cxl_get_layout_rat_hot(&hot, &rat_entry->hot);
	if (!layout_rat_state_is_standing(hot.local.state))
		return 0;

	/* One read for all three the scan asks of the line: whether the caller owns the entry, the
	 * baseline everyone gets, and the bound it stops at. */
	union layout_rat_acl_copy acl;
	cxl_get_layout_rat_acl(&acl, &rat_entry->acl);

	*out_granted = acl_read_granted(sbi, rat_entry, &hot, &acl, candidate);
	return 0;
}

/* Every bit in @required_perms or -EACCES, where the helper above answers with a subset. */
s32 acl_check_permission(struct fs_sb_info *sbi, u32 rat_entry_id, u32 required_perms)
{
	u32 have;
	s32 ret = acl_check_permission_any(sbi, rat_entry_id, required_perms, &have);
	if (ret)
		return ret;
	return have == required_perms ? 0 : -EACCES;
}

/* Sets @row's taken bit before the caller writes the row. @acl must be the copy read under meta_lock,
 * since the mask shares its line with default_perms and the owner fields. */
static void acl_mark_taken_row(struct layout_rat_entry *rat_entry, union layout_rat_acl_copy *acl,
			       const union acl_deleg_entry *row)
{
	const u32 bit = 1U << (u32)(row - rat_entry->deleg_entries);
	if (acl->local.deleg_taken & bit)
		return;

	acl->local.deleg_taken |= bit;
	cxl_set_layout_rat_acl(&rat_entry->acl, acl);
}

/* The slot for a row naming @want: its holder's own row, else a taken EMPTY row, else the lowest untaken
 * one, or NULL. @out_line is that row as read, zeros for an untaken one, so its state tells them apart. */
static union acl_deleg_entry *
acl_find_deleg_slot(struct fs_sb_info *sbi, struct layout_rat_entry *rat_entry,
		    u32 taken, const struct acl_deleg_target *want,
		    struct cxl_cacheline *out_line)
{
	union acl_deleg_entry *free_row = NULL;
	struct cxl_cacheline free_line = {};

	acl_for_each_taken_row(sbi, rat_entry, taken, TEST_DELEG_SCAN_SLOT, pos) {
		union acl_deleg_process_copy head;
		cxl_decode_acl_deleg_process(&head, &pos.line);
		if (head.local.state == ACL_DELEG_EMPTY) {
			if (!free_row) {
				free_row = pos.row;
				free_line = pos.line;
			}
			continue;
		}

		if (!acl_match_deleg_target(&pos.line, sbi->node_id, want))
			continue;

		*out_line = pos.line;
		return pos.row;
	}

	/* Not read: no ACTIVE row sits under a clear bit, and the caller writes the whole line. A full
	 * table names the slot past its end, which the accessor answers with NULL. */
	if (!free_row)
		free_row = layout_get_deleg_entry(rat_entry, bits_find_lowest_clear(taken, ACL_DELEG_MAX_ENTRIES));

	*out_line = free_line;
	return free_row;
}

/* What the role rows standing for @labels grant on this node. Any node's role row counts, since its
 * target_node and not its writer says where it answers. NULL @labels earns nothing. */
static u32 acl_read_role_grants(struct fs_sb_info *sbi, struct layout_rat_entry *rat_entry, u32 taken,
				const struct acl_role_key *labels)
{
	if (!labels)
		return 0;

	u32 granted = 0;
	acl_for_each_taken_row(sbi, rat_entry, taken, TEST_DELEG_SCAN_ROLE, pos) {
		union acl_deleg_role_copy row;
		cxl_decode_acl_deleg_role(&row, &pos.line);
		if (row.local.state != ACL_DELEG_ACTIVE || row.local.kind != ACL_DELEG_KIND_ROLE)
			continue;
		if (row.local.target_node != 0 && row.local.target_node != sbi->node_id)
			continue;
		if (memcmp(row.local.group, labels->group, sizeof(row.local.group)) != 0 ||
		    memcmp(row.local.role, labels->role, sizeof(row.local.role)) != 0)
			continue;

		granted |= row.local.perms;
	}

	return granted;
}

/* The row names the calling process, because the helper answers about that caller and no other.
 * An owner naming somebody else reaches acl_grant_deleg_account or acl_grant_deleg_role. */
s32 acl_grant_deleg_caller(struct fs_sb_info *sbi, u32 rat_entry_id, u32 policy_perms,
			   const struct acl_role_key *labels, u32 required_perms)
{
	if (!sbi)
		return -EINVAL;

	if (acl_has_unknown_perms(policy_perms))
		return -EINVAL;

	struct layout_rat_entry *rat_entry = layout_get_rat_entry(sbi, rat_entry_id);
	if (!rat_entry)
		return -EIO;

	/* Before the turn, so a caller neither the helper nor a role row covers is refused without
	 * waiting for one. */
	union layout_rat_acl_copy acl;
	cxl_get_layout_rat_acl(&acl, &rat_entry->acl);
	u32 earned = policy_perms | acl_read_role_grants(sbi, rat_entry, acl.local.deleg_taken, labels);
	if ((earned & required_perms) != required_perms)
		return -EACCES;

	/* Read here and not later: without these the row would match any process inheriting the
	 * pid, and a zero exe is the row such a caller would then match. */
	struct acl_caller_identity caller;
	acl_read_caller_identity(&caller);
	if (caller.exe_inode_ino == 0)
		return -ESRCH;

	/* Two concurrent grants would settle on the same free slot, and marking a fresh row reads
	 * and writes a line three other writers share. */
	struct meta_lock lock;
	s32 ret = meta_lock(sbi, &lock, TEST_META_OP_MAP);
	if (ret)
		return ret;

	/* ── Critical section (meta_lock held) ────────────────────── */

	union layout_rat_hot_copy hot;
	cxl_get_layout_rat_hot(&hot, &rat_entry->hot);
	if (hot.local.state != LAYOUT_RAT_ENTRY_ALLOCATED) {
		ret = -EINVAL;
		goto unlock;
	}

	/* Read once for the rows the scan below reads and for the mark a fresh row takes. The role rows
	 * are read again too, since one can be revoked while the caller waits for the turn. */
	cxl_get_layout_rat_acl(&acl, &rat_entry->acl);
	earned = policy_perms | acl_read_role_grants(sbi, rat_entry, acl.local.deleg_taken, labels);
	if ((earned & required_perms) != required_perms) {
		ret = -EACCES;
		goto unlock;
	}

	const struct acl_deleg_target want = { .process = &caller };
	struct cxl_cacheline line;
	union acl_deleg_entry *deleg_entry =
		acl_find_deleg_slot(sbi, rat_entry, acl.local.deleg_taken, &want, &line);
	if (!deleg_entry) {
		ret = -ENOSPC;
		goto unlock;
	}

	/* A row of an earlier generation carries what that image was granted, so the new image
	 * gets what this grant says and not the union of the two. */
	union acl_deleg_process_copy copy;
	cxl_decode_acl_deleg_process(&copy, &line);
	const bool widens_own_row = (copy.local.state == ACL_DELEG_ACTIVE &&
				     copy.local.exec_id == caller.exec_id);

	if (widens_own_row) {
		/* Edited in the copy and put back whole, which is the only shape this row is
		 * written in. */
		copy.local.perms |= earned;
	} else {
		/* The assignment zeroes what it does not name. */
		copy.local = (struct acl_deleg_process_local){
			.state = ACL_DELEG_ACTIVE,
			.kind = ACL_DELEG_KIND_PROCESS,
			.node_id = sbi->node_id,
			.pid = caller.tgid,
			.perms = earned,
			.birth_time = caller.birth_time,
			.exe_inode_ino = caller.exe_inode_ino,
			.exe_inode_dev = caller.exe_inode_dev,
			.exec_id = caller.exec_id,
		};
	}

	/* The account the process runs as, which is what an account revoke names it by. Written on
	 * both paths, since a setuid between two grants moves the process to another account. */
	copy.local.uid = caller.uid;
	copy.local.gid = caller.gid;

	acl_mark_taken_row(rat_entry, &acl, deleg_entry);
	if (test_crash_here(sbi, TEST_CRASH_GRANT_BEFORE_ROW)) {
		ret = -EIO;
		goto unlock;
	}

	/* One 64-byte transaction, so a reader sees the slot as it was or the finished row. */
	cxl_set_acl_deleg_process(&deleg_entry->process, &copy);
	ret = 0;

unlock:
	meta_unlock(sbi, &lock);
	return ret;
}

/* What a revoke for @want takes: the row naming that holder and, for an account, every process row
 * the helper wrote for its processes, since leaving those behind would be half a revoke. */
static bool acl_match_revoked_row(const struct cxl_cacheline *line, u32 node_id, const struct acl_deleg_target *want)
{
	if (acl_match_deleg_target(line, node_id, want))
		return true;
	if (want->role || want->process)
		return false;

	union acl_deleg_process_copy row;
	cxl_decode_acl_deleg_process(&row, line);
	return acl_match_deleg_process_account(&row.local, node_id, want->uid, want->gid);
}

/* Empties every row a revoke for @want takes, then their taken bits, under the turn the caller holds.
 * -ENOENT when none matched. */
static s32 acl_drop_revoked_rows(struct fs_sb_info *sbi, struct layout_rat_entry *rat_entry,
				 const struct acl_deleg_target *want)
{
	/* Read under the turn, since the emptied rows come off this copy's mask and it goes back. */
	union layout_rat_acl_copy acl;
	cxl_get_layout_rat_acl(&acl, &rat_entry->acl);

	u32 emptied = 0;
	acl_for_each_taken_row(sbi, rat_entry, acl.local.deleg_taken, TEST_DELEG_SCAN_REVOKE, pos) {
		if (!acl_match_revoked_row(&pos.line, sbi->node_id, want))
			continue;

		/* Only the head changes, and every kind's head sits where the process view reads it. */
		union acl_deleg_process_copy row;
		cxl_decode_acl_deleg_process(&row, &pos.line);
		row.local.state = ACL_DELEG_EMPTY;
		cxl_set_acl_deleg_process(&pos.row->process, &row);
		emptied |= 1U << pos.idx;
	}

	if (!emptied)
		return -ENOENT;
	if (test_crash_here(sbi, TEST_CRASH_REVOKE_BEFORE_UNMARK))
		return -EIO;

	acl_clear_taken_rows(rat_entry, &acl, emptied);
	return 0;
}

s32 acl_revoke_deleg_account(struct fs_sb_info *sbi, u32 rat_entry_id, u32 uid, u32 gid)
{
	if (!sbi)
		return -EINVAL;
	/* Both ids left open would name every row rather than one. */
	if (uid == ACL_DELEG_ANY_ID && gid == ACL_DELEG_ANY_ID)
		return -EINVAL;

	struct layout_rat_entry *rat_entry = layout_get_rat_entry(sbi, rat_entry_id);
	if (!rat_entry)
		return -EIO;

	/* OWNER_DEAD as well: such a region waits on its process rows, so taking one off is what frees
	 * it. */
	union layout_rat_hot_copy hot;
	cxl_get_layout_rat_hot(&hot, &rat_entry->hot);
	if (!layout_rat_state_is_standing(hot.local.state))
		return -EINVAL;

	/* Giving up a row that names the caller alone takes nothing from anybody, so it asks for
	 * nothing. A gid alone names a whole group, and taking that away from everyone needs ADMIN. */
	struct acl_caller_identity caller;
	acl_read_caller_identity(&caller);

	const struct acl_deleg_account_local named = { .uid = uid, .gid = gid };
	const bool names_the_caller_alone = uid != ACL_DELEG_ANY_ID &&
					    acl_match_deleg_account(&named, &caller);

	/* Before the turn, so a caller with no standing is refused without waiting for one. */
	if (!names_the_caller_alone) {
		u32 have;
		s32 checked = acl_check_permission_any(sbi, rat_entry_id, FS_PERM_ADMIN, &have);
		if (checked)
			return checked;
		if (!have)
			return -EACCES;
	}

	struct meta_lock lock;
	s32 ret = meta_lock(sbi, &lock, TEST_META_OP_PERM);
	if (ret)
		return ret;

	/* And again under the turn: the ADMIN this rests on can be revoked while the caller waits. */
	if (!names_the_caller_alone) {
		u32 have;
		ret = acl_check_permission_any(sbi, rat_entry_id, FS_PERM_ADMIN, &have);
		if (ret)
			goto unlock;
		if (!have) {
			ret = -EACCES;
			goto unlock;
		}
	}

	const struct acl_deleg_target want = { .uid = uid, .gid = gid };
	ret = acl_drop_revoked_rows(sbi, rat_entry, &want);

unlock:
	meta_unlock(sbi, &lock);
	return ret;
}

/* One 64-byte transaction carrying ACTIVE, so a fresh slot needs no state between empty and live. */
static void acl_set_account_row(struct fs_sb_info *sbi, union acl_deleg_entry *deleg_entry, u32 uid, u32 gid,
				u32 perms)
{
	union acl_deleg_account_copy copy;
	copy.local = (struct acl_deleg_account_local){
		.state = ACL_DELEG_ACTIVE,
		.kind = ACL_DELEG_KIND_ACCOUNT,
		.node_id = sbi->node_id,
		.perms = perms,
		.uid = uid,
		.gid = gid,
	};
	cxl_set_acl_deleg_account(&deleg_entry->account, &copy);
}

/* An account row or a role row, for a grant that has to outlive the process holding it. Either
 * names the node that wrote it. A rerun for the same holder rewrites that row; another takes a slot. */
static s32 acl_write_deleg_named(struct fs_sb_info *sbi, struct layout_rat_entry *rat_entry,
				 union layout_rat_acl_copy *acl, const struct acl_deleg_target *want, u32 perms,
				 bool caller_has_admin)
{
	if (perms == 0 || acl_has_unknown_perms(perms))
		return -EINVAL;
	/* Both ids left open would name everyone, which is not a delegation. */
	if (!want->role && want->uid == ACL_DELEG_ANY_ID && want->gid == ACL_DELEG_ANY_ID)
		return -EINVAL;

	struct cxl_cacheline line;
	union acl_deleg_entry *target = acl_find_deleg_slot(sbi, rat_entry, acl->local.deleg_taken, want, &line);
	if (!target)
		return -ENOSPC;

	/* A rewrite replaces the row, so a narrower mask is a revocation. GRANT hands on and takes
	 * nothing back: only ADMIN may leave the holder with less than it had. */
	union acl_deleg_process_copy found;
	cxl_decode_acl_deleg_process(&found, &line);
	if (!caller_has_admin &&
	    found.local.state == ACL_DELEG_ACTIVE &&
	    (found.local.perms & ~perms))
		return -EPERM;

	acl_mark_taken_row(rat_entry, acl, target);
	if (test_crash_here(sbi, TEST_CRASH_GRANT_BEFORE_ROW))
		return -EIO;

	if (want->role) {
		union acl_deleg_role_copy copy;
		copy.local = (struct acl_deleg_role_local){
			.state = ACL_DELEG_ACTIVE,
			.kind = ACL_DELEG_KIND_ROLE,
			.node_id = sbi->node_id,
			.perms = perms,
			.target_node = want->role->target_node,
		};
		memcpy(copy.local.group, want->role->group, sizeof(copy.local.group));
		memcpy(copy.local.role, want->role->role, sizeof(copy.local.role));
		cxl_set_acl_deleg_role(&target->role, &copy);
	} else {
		acl_set_account_row(sbi, target, want->uid, want->gid, perms);
	}

	return 0;
}

/*
 * Both lines are read once inside the lock and serve the state check, the permission decision, the
 * rows a scan reads and the mark a new row takes. Reading them before it would put a stale mask back.
 */
static s32 acl_enter_deleg_write(struct fs_sb_info *sbi, struct layout_rat_entry *rat_entry,
				 u32 candidate, u32 *out_granted, union layout_rat_acl_copy *out_acl)
{
	union layout_rat_hot_copy hot;
	cxl_get_layout_rat_hot(&hot, &rat_entry->hot);
	/* No new row on an entry whose owner is gone: its readers are being let finish. */
	if (hot.local.state != LAYOUT_RAT_ENTRY_ALLOCATED)
		return -EACCES;

	cxl_get_layout_rat_acl(out_acl, &rat_entry->acl);
	*out_granted = acl_read_granted(sbi, rat_entry, &hot, out_acl, candidate);
	return 0;
}

s32 acl_set_lock_region_account(struct fs_sb_info *sbi, struct layout_rat_entry *rat_entry,
				u32 uid, u32 gid, u32 perms)
{
	if (!sbi || !rat_entry)
		return -EINVAL;
	if (perms == 0 || acl_has_unknown_perms(perms))
		return -EINVAL;
	/* Both ids left open would name everyone, which is not a delegation. */
	if (uid == ACL_DELEG_ANY_ID && gid == ACL_DELEG_ANY_ID)
		return -EINVAL;
	if (sbi->node_id == 0 || sbi->node_id > FS_MAX_NODE_ID)
		return -EINVAL;

	/* The node's own slot and not one a scan picked. Every node reaches this before any turn
	 * exists, so a scan would have two of them settling on the same free index. */
	union acl_deleg_entry *row = layout_get_deleg_entry(rat_entry, sbi->node_id - 1);
	if (!row)
		return -EIO;

	/* No lock: the row is this node's own slot, written in one transaction, and the entry it
	 * sits in is the formatter's. GC spares the LOCK type and unlink refuses its name, so
	 * nothing on this node writes these lines while the bring-up runs. */
	union layout_rat_hot_copy hot;
	cxl_get_layout_rat_hot(&hot, &rat_entry->hot);
	if (hot.local.state != LAYOUT_RAT_ENTRY_ALLOCATED)
		return -EACCES;

	/* Format marked every node's slot, so nothing here touches the acl line every node shares. */
	acl_set_account_row(sbi, row, uid, gid, perms);

	return 0;
}

/*
 * A named row on any region, asked for by a caller that has to earn it. The lock-region writer above
 * takes a capability instead, and its region hands out one slot per node rather than a scanned one.
 */
static s32 acl_grant_deleg_named(struct fs_sb_info *sbi, u32 rat_entry_id, const struct acl_deleg_target *want,
				 u32 perms)
{
	if (!sbi)
		return -EINVAL;

	struct layout_rat_entry *rat_entry = layout_get_rat_entry(sbi, rat_entry_id);
	if (!rat_entry)
		return -EIO;

	struct meta_lock lock;
	s32 ret = meta_lock(sbi, &lock, TEST_META_OP_PERM);
	if (ret)
		return ret;

	/* ── Critical section (meta_lock held) ────────────────────── */

	u32 have;
	union layout_rat_acl_copy acl;
	ret = acl_enter_deleg_write(sbi, rat_entry, FS_PERM_ALL, &have, &acl);
	if (ret)
		goto unlock;

	/* GRANT hands on what its holder has and nothing beyond it, so a delegate cannot widen its
	 * own reach through a row it writes. Only ADMIN hands on ADMIN, GRANT, or bits it lacks. */
	if (!(have & (FS_PERM_ADMIN | FS_PERM_GRANT))) {
		ret = -EACCES;
		goto unlock;
	}
	if (!(have & FS_PERM_ADMIN) && ((perms & (FS_PERM_ADMIN | FS_PERM_GRANT)) || (perms & ~have))) {
		ret = -EPERM;
		goto unlock;
	}

	ret = acl_write_deleg_named(sbi, rat_entry, &acl, want, perms, (have & FS_PERM_ADMIN) != 0);

unlock:
	meta_unlock(sbi, &lock);
	return ret;
}

s32 acl_grant_deleg_account(struct fs_sb_info *sbi, u32 rat_entry_id, u32 uid, u32 gid, u32 perms)
{
	const struct acl_deleg_target want = { .uid = uid, .gid = gid };
	s32 ret = acl_grant_deleg_named(sbi, rat_entry_id, &want, perms);
	if (ret == 0)
		pr_debug("granted perms=0x%x to uid=%u gid=%u on rat_entry %u\n", perms, uid, gid, rat_entry_id);
	return ret;
}

s32 acl_grant_deleg_role(struct fs_sb_info *sbi, u32 rat_entry_id, const struct acl_role_key *key, u32 perms)
{
	if (!key)
		return -EINVAL;

	const struct acl_deleg_target want = { .role = key };
	s32 ret = acl_grant_deleg_named(sbi, rat_entry_id, &want, perms);
	if (ret == 0)
		pr_debug("granted perms=0x%x to role %s/%s on node %u, rat_entry %u\n", perms, key->group, key->role,
			 key->target_node, rat_entry_id);
	return ret;
}

/* A process row the role row earned stays: it holds no label to find it by, and goes with its process. */
s32 acl_revoke_deleg_role(struct fs_sb_info *sbi, u32 rat_entry_id, const struct acl_role_key *key)
{
	if (!sbi || !key)
		return -EINVAL;

	struct layout_rat_entry *rat_entry = layout_get_rat_entry(sbi, rat_entry_id);
	if (!rat_entry)
		return -EIO;

	union layout_rat_hot_copy hot;
	cxl_get_layout_rat_hot(&hot, &rat_entry->hot);
	if (!layout_rat_state_is_standing(hot.local.state))
		return -EINVAL;

	/* Before the turn, so a caller with no standing is refused without waiting for one. */
	s32 ret = acl_check_permission(sbi, rat_entry_id, FS_PERM_ADMIN);
	if (ret)
		return ret;

	struct meta_lock lock;
	ret = meta_lock(sbi, &lock, TEST_META_OP_PERM);
	if (ret)
		return ret;

	/* And again under the turn: the ADMIN this rests on can be revoked while the caller waits. */
	ret = acl_check_permission(sbi, rat_entry_id, FS_PERM_ADMIN);
	if (ret)
		goto unlock;

	const struct acl_deleg_target want = { .role = key };
	ret = acl_drop_revoked_rows(sbi, rat_entry, &want);

unlock:
	meta_unlock(sbi, &lock);
	return ret;
}

bool acl_copy_label(char *dst, const char *src)
{
	const u32 used = (u32)strnlen(src, FS_DAEMON_IDENT_LEN);
	if (used == 0 || used == FS_DAEMON_IDENT_LEN)
		return false;

	memset(dst, 0, FS_DAEMON_IDENT_LEN);
	memcpy(dst, src, used);
	return true;
}

/* What the region grants to everyone holding no row of their own. 0 is a value and not an omission,
 * so this takes any subset of FS_PERM_ALL including none of it. */
s32 acl_set_default_perms(struct fs_sb_info *sbi, u32 rat_entry_id, u32 perms)
{
	if (!sbi || acl_has_unknown_perms(perms))
		return -EINVAL;

	struct layout_rat_entry *rat_entry = layout_get_rat_entry(sbi, rat_entry_id);
	if (!rat_entry)
		return -EIO;

	/* Before the turn, so a caller with no standing is refused without waiting for one. */
	s32 ret = acl_check_permission(sbi, rat_entry_id, FS_PERM_ADMIN);
	if (ret)
		return ret;

	/* deleg_taken and the owner fields share this line, so the read and the write below must not
	 * have another writer's put-back between them. */
	struct meta_lock lock;
	ret = meta_lock(sbi, &lock, TEST_META_OP_PERM);
	if (ret)
		return ret;

	/* And again under the turn: the row this caller's ADMIN rests on can be revoked while the
	 * caller waits, and the check from before the wait would let the write through. */
	ret = acl_check_permission(sbi, rat_entry_id, FS_PERM_ADMIN);
	if (ret)
		goto unlock;

	union layout_rat_acl_copy acl;
	cxl_get_layout_rat_acl(&acl, &rat_entry->acl);
	if (acl.local.default_perms != perms) {
		acl.local.default_perms = perms;
		cxl_set_layout_rat_acl(&rat_entry->acl, &acl);
		pr_debug("set default_perms=0x%x on rat_entry %u\n", perms, rat_entry_id);
	}

unlock:
	meta_unlock(sbi, &lock);
	return ret;
}
