// SPDX-License-Identifier: GPL-2.0-only
/* gc.c - dead-process and crash-orphan reclaim */

#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/slab.h>

#include "access/acl.h"
#include "access/meta_lock.h"
#include "core.h"
#include "layout/layout_access.h"
#include "lifecycle/bootstrap.h"
#include "lifecycle/gc.h"
#include "region/index.h"
#include "region/refs.h"
#include "region/region.h"
#include "vfs/file.h"

/* The stale timeout widens the enum past 32 bits, so a printed member needs a cast back. */
enum gc_config {
	GC_INTERVAL_MS = 10000,
	GC_STALE_TIMEOUT_NS = (60ULL * NSEC_PER_SEC),
	GC_TICK_MS = 500,
	GC_LOCK_WAIT_MS = 50,
	GC_SELF_RECOVERY_PASSES = 3,
};

/*
 * A node_id==0 entry died mid-create or mid-delete, so no node's own node_id filter reaches it.
 * The admin node, the lowest node_id still ticking, is the only one that tracks and claims them.
 */

/* The entry's own stamp, which a free and a fresh create both move. */
static u64 gc_orphan_stamp(void *entry, enum gc_orphan_type type)
{
	switch (type) {
	case GC_ORPHAN_RAT: {
		struct layout_rat_entry *rat_entry = entry;
		union layout_rat_hot_copy hot;
		cxl_get_layout_rat_hot(&hot, &rat_entry->hot);
		return hot.local.alloc_time;
	}
	default:
		return 0;
	}
}

/*
 * Tracked in DRAM and never on the medium, so this node's discovery clobbers no line a live
 * writer elsewhere owns. Caller holds sbi->gc_sweep_lock: the bound check and the increment below
 * read the count separately, so a second sweeper between them moves the slot this one writes.
 */
static void gc_track_orphan(struct fs_sb_info *sbi, void *entry, enum gc_orphan_type type)
{
	if (!sbi->gc_orphans)
		return; /* a sweep driven from sysfs on a mount whose GC never started */

	for (u32 i = 0; i < sbi->gc_orphan_count; i++) {
		if (sbi->gc_orphans[i].entry == entry)
			return;
	}

	/* Full: the next cycle retries, once the sweep has freed slots. */
	if (sbi->gc_orphan_count >= GC_ORPHAN_MAX)
		return;

	struct gc_orphan_tracker *slot = &sbi->gc_orphans[sbi->gc_orphan_count++];
	slot->entry = entry;
	slot->seen_at = gc_orphan_stamp(entry, type);
	slot->discovered_at = ktime_get_real_ns();
	slot->type = type;
}

/* False once the entry has moved on, which is the tracker's cue to drop it. @seen_at is the stamp
 * the tracker recorded, so an entry freed and taken by another file fails on that alone. */
static bool gc_is_orphan_stuck(void *entry, enum gc_orphan_type type, u64 seen_at)
{
	switch (type) {
	case GC_ORPHAN_RAT: {
		struct layout_rat_entry *rat_entry = entry;
		union layout_rat_hot_copy hot;
		cxl_get_layout_rat_hot(&hot, &rat_entry->hot);
		if (hot.local.alloc_time != seen_at)
			return false;
		if (!layout_rat_state_is_transient(hot.local.state))
			return false;

		union layout_rat_acl_copy acl;
		cxl_get_layout_rat_acl(&acl, &rat_entry->acl);
		return acl.local.owner_node_id == 0;
	}
	default:
		return false;
	}
}

/*
 * Names this node and restamps, which puts the entry on this node's normal path for the next sweep.
 * Caller holds sbi->meta_lock: the ACL line is read here and put back.
 */
static bool gc_claim_orphan(struct fs_sb_info *sbi, void *entry, enum gc_orphan_type type)
{
	u64 now = ktime_get_real_ns();

	switch (type) {
	case GC_ORPHAN_RAT: {
		struct layout_rat_entry *rat_entry = entry;
		union layout_rat_acl_copy acl;
		cxl_get_layout_rat_acl(&acl, &rat_entry->acl);
		if (acl.local.owner_node_id != 0)
			return false; /* its own node came back and named itself */
		acl.local.owner_node_id = sbi->node_id;
		cxl_set_layout_rat_acl(&rat_entry->acl, &acl);

		/* The claim landed, so the stamp goes on a line this node now owns. */
		union layout_rat_hot_copy hot;
		cxl_get_layout_rat_hot(&hot, &rat_entry->hot);
		hot.local.alloc_time = now;
		cxl_set_layout_rat_hot(&rat_entry->hot, &hot);
		return true;
	}
	default:
		return false;
	}
}

/* Returns entries claimed, not entries freed: the freeing waits for the next cycle. */
static s32 gc_sweep_orphans(struct fs_sb_info *sbi)
{
	s32 reclaimed = 0;
	u32 i = 0;
	u64 now = ktime_get_real_ns();

	mutex_lock(&sbi->gc_sweep_lock);

	while (i < sbi->gc_orphan_count) {
		struct gc_orphan_tracker *tracker = &sbi->gc_orphans[i];
		if (!gc_is_orphan_stuck(tracker->entry, tracker->type, tracker->seen_at))
			goto remove;

		if ((now > tracker->discovered_at) &&
		    (now - tracker->discovered_at) > GC_STALE_TIMEOUT_NS) {
			/* The claim reads a line and puts it back, so a user path writing that line
			 * must not fall between the two. Stepping aside keeps it for the next sweep. */
			struct meta_lock lock;
			if (!meta_trylock(sbi, &lock, GC_LOCK_WAIT_MS, TEST_META_OP_SWEEP)) {
				i++;
				continue;
			}
			if (gc_claim_orphan(sbi, tracker->entry, tracker->type))
				reclaimed++;
			meta_unlock(sbi, &lock);
			goto remove;
		}

		i++;
		continue;
remove:
		sbi->gc_orphans[i] = sbi->gc_orphans[--sbi->gc_orphan_count];
	}

	mutex_unlock(&sbi->gc_sweep_lock);
	return reclaimed;
}

/*
 * Empties the rows whose delegated process died. This node's own rows only, because another
 * node's process liveness is not this node's to judge.
 */
static s32 gc_sweep_dead_delegations(struct fs_sb_info *sbi, struct layout_rat_entry *rat_entry)
{
	union layout_rat_hot_copy hot;
	cxl_get_layout_rat_hot(&hot, &rat_entry->hot);
	/* OWNER_DEAD too: an entry in that state is waiting for its rows to go, and a process row
	 * goes by being swept here. Leaving it out is what would make that wait forever. */
	if (!layout_rat_state_is_standing(hot.local.state))
		return 0;

	s32 cleaned = 0;

	for (u32 i = 0; i < ACL_DELEG_MAX_ENTRIES; i++) {
		struct acl_deleg_entry *deleg_entry = layout_get_deleg_entry(rat_entry, i);
		if (!deleg_entry)
			continue;

		union acl_deleg_entry_copy copy;
		cxl_get_acl_deleg_entry(&copy, deleg_entry);

		switch (copy.local.state) {
		case ACL_DELEG_EMPTY:
			break;

		case ACL_DELEG_ACTIVE: {
			u32 de_pid = copy.local.pid;
			u64 de_birth = copy.local.birth_time;

			if (copy.local.node_id != sbi->node_id || de_pid == 0)
				break;

			/* A row with no start time names no live process, because the write that
			 * makes one fills it. acl_is_owner_dead reads that as dead. */
			if (!acl_is_owner_dead(de_pid, de_birth))
				break;

			/* Re-read and re-judged under the lock: the copy above predates it, and a
			 * create clearing this table would get its zeros put back. */
			struct meta_lock lock;
			if (!meta_trylock(sbi, &lock, GC_LOCK_WAIT_MS, TEST_META_OP_SWEEP))
				break;

			cxl_get_acl_deleg_entry(&copy, deleg_entry);
			if (copy.local.state == ACL_DELEG_ACTIVE &&
			    copy.local.pid == de_pid &&
			    copy.local.birth_time == de_birth) {
				copy.local.state = ACL_DELEG_EMPTY;
				cxl_set_acl_deleg_entry(deleg_entry, &copy);
				cleaned++;
			}

			meta_unlock(sbi, &lock);
			break;
		}
		}
	}

	return cleaned;
}

static bool gc_has_active_delegations(struct layout_rat_entry *rat_entry)
{
	for (u32 i = 0; i < ACL_DELEG_MAX_ENTRIES; i++) {
		struct acl_deleg_entry *deleg_entry = layout_get_deleg_entry(rat_entry, i);
		if (!deleg_entry)
			continue;

		union acl_deleg_entry_copy copy;
		cxl_get_acl_deleg_entry(&copy, deleg_entry);
		if (copy.local.state == ACL_DELEG_ACTIVE)
			return true;
	}

	return false;
}

static bool gc_has_no_name(const struct layout_rat_entry *entry)
{
	struct cxl_cacheline line;
	return layout_read_rat_name(entry, &line)[0] == '\0';
}

/*
 * Nobody stands behind the entry: its owner is dead or its name was unlinked, and no delegation
 * row is left. A row is what a consumer stakes to keep a region past its owner, so reaching one
 * through default_perms alone buys access and not lifetime.
 */
static bool gc_is_unclaimed(struct layout_rat_entry *entry)
{
	union layout_rat_hot_copy hot;
	cxl_get_layout_rat_hot(&hot, &entry->hot);
	/* No owner by design, and its users hold no row, so nothing here would ever say to keep it. */
	if (hot.local.region_type == LAYOUT_REGION_LOCK)
		return false;

	/* The state answers the owner question already, so the rows are the whole test. */
	if (hot.local.state == LAYOUT_RAT_ENTRY_OWNER_DEAD)
		return !gc_has_active_delegations(entry);

	/* Unlinked while somebody still held it: the name is gone and the holders are what keep it. */
	if (hot.local.state == LAYOUT_RAT_ENTRY_ALLOCATED && gc_has_no_name(entry))
		return true;

	/* The owner lands whole, and the write that clears it sets OWNER_DEAD first, so an entry that
	 * still names a state below has an owner to ask about. */
	union layout_rat_acl_copy acl;
	cxl_get_layout_rat_acl(&acl, &entry->acl);

	/* A claimed orphan: the admin named itself on an entry whose create died before the owner
	 * was written, so there is no pid to ask and the claim itself is the verdict. */
	if (acl.local.owner_pid == 0 && layout_rat_state_is_transient(hot.local.state))
		return !gc_has_active_delegations(entry);

	if (!acl_is_owner_dead(acl.local.owner_pid, acl.local.owner_birth_time))
		return false;

	if (gc_has_active_delegations(entry))
		return false;

	return true;
}

/* Unclaimed and unreferenced: an open descriptor or a mapping on any node keeps the bytes. */
static bool gc_is_orphaned(struct fs_sb_info *sbi, struct layout_rat_entry *entry)
{
	if (!gc_is_unclaimed(entry))
		return false;

	const u32 index = (u32)(entry - layout_get_rat(sbi)->entries);
	return !region_has_refs(sbi, index);
}

/* Lets unlink take down a file whose owner is dead without DELETE permission. */
bool gc_can_force_unlink(struct fs_sb_info *sbi, u32 rat_entry_id)
{
	struct layout_rat_entry *entry = layout_get_rat_entry(sbi, rat_entry_id);
	if (!entry)
		return false;

	union layout_rat_acl_copy acl;
	cxl_get_layout_rat_acl(&acl, &entry->acl);
	if (acl.local.owner_node_id != sbi->node_id)
		return false;

	return gc_is_unclaimed(entry);
}

/* Whether the process that made the entry is dead, so nobody is left to clear its bytes. */
static bool gc_has_dead_owner(struct layout_rat_entry *entry)
{
	union layout_rat_hot_copy hot;
	cxl_get_layout_rat_hot(&hot, &entry->hot);
	if (hot.local.state == LAYOUT_RAT_ENTRY_OWNER_DEAD)
		return true;

	union layout_rat_acl_copy acl;
	cxl_get_layout_rat_acl(&acl, &entry->acl);
	return acl.local.owner_pid == 0 || acl_is_owner_dead(acl.local.owner_pid, acl.local.owner_birth_time);
}

/*
 * Zeroes the extent before the slot goes back. A live owner clears what it wants cleared before it
 * unlinks; a dead one cleared nothing, and the next tenant must not read what it left.
 */
static void gc_clear_extent(struct fs_sb_info *sbi, struct layout_rat_entry *entry)
{
	union layout_rat_hot_copy hot;
	cxl_get_layout_rat_hot(&hot, &entry->hot);
	if (hot.local.phys_offset == 0 ||
	    hot.local.size == 0 ||
	    !layout_is_valid_region_addr(sbi, hot.local.phys_offset, hot.local.size))
		return;

	void *base = layout_get_file_data_ptr(sbi, hot.local.phys_offset, 0);
	cxl_zero_cachelines(base, hot.local.size);
	/* The write-back pool's lines sit in this node's cache after the byte fallback. */
	if (!layout_check_meta_offset(sbi, hot.local.phys_offset))
		CXL_WMB_CACHED(base, hot.local.size);
}

/*
 * False when the index kept the name, and false when the slot was already FREE, which is an
 * unlink having finished it between this sweep's judgement and its lock. @zero_extent is whether
 * the bytes go too, which is the dead owner's case and not the unlinked one's.
 */
static bool gc_cleanup_rat_entry(struct fs_sb_info *sbi, struct layout_rat_entry *entry, bool zero_extent)
{
	struct cxl_cacheline line;
	const char *name_buf = layout_read_rat_name(entry, &line);
	u32 name_len = (u32)strnlen(name_buf, FS_NAME_MAX);

	if (name_len > 0) {
		s32 ret = index_delete(sbi, name_buf, name_len);
		if (ret && ret != -ENOENT) {
			/* Keep the slot: freed now, the next create takes it while the index still
			 * names it, and a lookup then answers with the wrong file. */
			pr_warn("gc index_delete failed for '%s': %d\n", name_buf, ret);
			return false;
		}
	}

	if (zero_extent)
		gc_clear_extent(sbi, entry);

	return region_free_rat_entry(sbi, entry);
}

/*
 * Empties the rows @node_id wrote into one entry's delegation table. Caller holds sbi->meta_lock,
 * because a create clearing this table would otherwise get its zeros put back.
 */
static u32 gc_drop_node_delegations(struct layout_rat_entry *entry, u32 node_id)
{
	u32 dropped = 0;

	for (u32 slot = 0; slot < ACL_DELEG_MAX_ENTRIES; slot++) {
		struct acl_deleg_entry *deleg_entry = layout_get_deleg_entry(entry, slot);
		if (!deleg_entry)
			continue;

		union acl_deleg_entry_copy copy;
		cxl_get_acl_deleg_entry(&copy, deleg_entry);
		if (copy.local.state != ACL_DELEG_ACTIVE || copy.local.node_id != node_id)
			continue;

		/* The bound stays: lowering it would hide a row a peer just wrote above. */
		copy.local.state = ACL_DELEG_EMPTY;
		cxl_set_acl_deleg_entry(deleg_entry, &copy);
		dropped++;
	}

	return dropped;
}

/* Both are read without the lock, so a hit is rechecked under it and a miss costs no lock at all. */
static bool gc_has_node_owner(struct layout_rat_entry *entry, u32 node_id)
{
	union layout_rat_acl_copy acl;
	cxl_get_layout_rat_acl(&acl, &entry->acl);
	return acl.local.owner_node_id == node_id;
}

/* A node's rows sit in the delegation table of whatever region it was granted, not only its own. */
static bool gc_has_node_deleg(struct layout_rat_entry *entry, u32 node_id)
{
	for (u32 slot = 0; slot < ACL_DELEG_MAX_ENTRIES; slot++) {
		struct acl_deleg_entry *deleg_entry = layout_get_deleg_entry(entry, slot);
		if (!deleg_entry)
			continue;

		union acl_deleg_entry_copy copy;
		cxl_get_acl_deleg_entry(&copy, deleg_entry);
		if (copy.local.state == ACL_DELEG_ACTIVE && copy.local.node_id == node_id)
			return true;
	}

	return false;
}

/*
 * Marks an entry whose owner's node is gone, so that its readers can finish.
 *
 * Nobody takes it over: ownership carries a policy context of the process that made the region, and
 * there is no process here to carry it. The whole identity goes with the state, because a node id
 * nobody holds is what stops a later holder of that id reading itself as the owner.
 */
static void gc_mark_owner_dead(struct layout_rat_entry *entry, u32 index, u32 node_id)
{
	/* The state first and the identity after, since the two lines cannot be written as one. A
	 * crash here leaves an entry the admin still sweeps and a rerun still recognises. */
	union layout_rat_hot_copy hot;
	cxl_get_layout_rat_hot(&hot, &entry->hot);
	hot.local.state = LAYOUT_RAT_ENTRY_OWNER_DEAD;
	cxl_set_layout_rat_hot(&entry->hot, &hot);

	union layout_rat_acl_copy acl;
	cxl_get_layout_rat_acl(&acl, &entry->acl);
	acl.local.owner_node_id = 0;
	acl.local.owner_pid = 0;
	acl.local.owner_birth_time = 0;
	acl.local.owner_exe_inode_ino = 0;
	acl.local.owner_exe_inode_dev = 0;
	memset(acl.local.owner_group, 0, sizeof(acl.local.owner_group));
	memset(acl.local.owner_role, 0, sizeof(acl.local.owner_role));
	cxl_set_layout_rat_acl(&entry->acl, &acl);

	pr_info("gc: RAT entry %u kept for its readers, node %u gone\n", index, node_id);
}

/*
 * gc_recover_dead_node - clear every row naming @node_id, and say whether any is left.
 *
 * No pid is consulted. The slot said that node is gone, which is a stronger statement than a local
 * process table can make about a remote pid, and it is the statement the eviction rests on.
 *
 * An entry the dead node owned goes only when no row is left on it, because a row is what a
 * consumer stakes to keep a region past its owner. An entry that still carries one is marked
 * OWNER_DEAD: nothing new starts on it, and the ordinary sweep takes it when the last row goes.
 * Rows a live node wrote are left alone, since a grant nobody revoked is not this sweep's to
 * withdraw.
 */
static bool gc_recover_dead_node(struct fs_sb_info *sbi, u32 node_id)
{
	bool clear = true;

	mutex_lock(&sbi->gc_sweep_lock);

	/* The slot said the node is gone, so nothing it counted is still open. */
	region_clear_node_refs(sbi, node_id);

	for (u32 i = 0; i < LAYOUT_MAX_RAT_ENTRIES; i++) {
		struct layout_rat_entry *entry = layout_get_rat_entry(sbi, i);
		if (!entry) {
			clear = false;
			break;
		}

		union layout_rat_hot_copy hot;
		cxl_get_layout_rat_hot(&hot, &entry->hot);
		if (hot.local.state == LAYOUT_RAT_ENTRY_FREE)
			continue;

		bool owned = gc_has_node_owner(entry, node_id);
		if (!owned && !gc_has_node_deleg(entry, node_id))
			continue;

		/* A user path or a peer holding this entry's lines is working on them, so the next
		 * sweep takes it rather than this one waiting. */
		struct meta_lock lock;
		if (!meta_trylock(sbi, &lock, GC_LOCK_WAIT_MS, TEST_META_OP_SWEEP)) {
			clear = false;
			continue;
		}

		/* The entry can be unlinked and its slot handed to a live owner between the read above
		 * and this lock, so the sweep acts on what the lock covers, not on what it saw. */
		cxl_get_layout_rat_hot(&hot, &entry->hot);
		owned = gc_has_node_owner(entry, node_id);
		if (hot.local.state == LAYOUT_RAT_ENTRY_FREE ||
		    (!owned && !gc_has_node_deleg(entry, node_id))) {
			meta_unlock(sbi, &lock);
			continue;
		}

		gc_drop_node_delegations(entry, node_id);

		if (owned) {
			if (gc_has_active_delegations(entry) || region_has_refs(sbi, i))
				gc_mark_owner_dead(entry, i, node_id);
			else if (!gc_cleanup_rat_entry(sbi, entry, true))
				clear = false;
		}

		meta_unlock(sbi, &lock);
	}

	mutex_unlock(&sbi->gc_sweep_lock);
	return clear;
}

/*
 * gc_clear_own_node - the clean step, run by the departing node on its own id.
 *
 * An unmount is the one node death that announces itself, and it is also the one no admin recovers:
 * a released slot is skipped by every scan, and the ordinary sweep reads only entries whose owner is
 * the sweeping node. A row this mount left behind would therefore be read as the next holder of
 * the id. A pass that could not lock every entry is retried, since a peer's turn ends in
 * milliseconds and the caller is about to give the id up for good.
 */
bool gc_clear_own_node(struct fs_sb_info *sbi)
{
	for (u32 pass = 0; pass < GC_SELF_RECOVERY_PASSES; pass++) {
		if (gc_recover_dead_node(sbi, sbi->node_id))
			return true;
		msleep(GC_LOCK_WAIT_MS);
	}

	return false;
}

/*
 * One recovery step per sweep, so a node staked here has a whole interval to read its own slot and
 * fence itself before the step below starts deleting what it owns.
 */
static void gc_run_recovery(struct fs_sb_info *sbi)
{
	u32 node_id = 0;

	switch (bootstrap_next_recovery(sbi, &node_id)) {
	case BOOTSTRAP_RECOVERY_STAKE:
		bootstrap_stake_recovery(sbi, node_id);
		break;
	case BOOTSTRAP_RECOVERY_CLEAN:
		if (gc_recover_dead_node(sbi, node_id))
			bootstrap_evict_node(sbi, node_id);
		break;
	case BOOTSTRAP_RECOVERY_NONE:
		break;
	}
}

/*
 * A region goes only once its owner is dead and every node's GC has emptied its own rows. So the
 * delegation sweep runs on all regions, while the reclaim below takes only this node's.
 */
s32 gc_reclaim_dead_regions(struct fs_sb_info *sbi)
{
	if (!sbi)
		return -EINVAL;

	s32 reclaimed = 0;

	mutex_lock(&sbi->gc_sweep_lock);

	/* The taken bits as this walk sees them, so a summary a crash left stale is noticed without
	 * a second pass over the entries. */
	union layout_rat_map_copy seen = {};
	bool walked = true;

	for (u32 i = 0; i < LAYOUT_MAX_RAT_ENTRIES; i++) {
		struct layout_rat_entry *entry = layout_get_rat_entry(sbi, i);
		if (!entry) {
			pr_info_ratelimited("gc: no RAT loaded, skipping dead-process scan\n");
			reclaimed = 0;
			walked = false;
			break;
		}

		union layout_rat_hot_copy hot;
		cxl_get_layout_rat_hot(&hot, &entry->hot);

		u32 state = hot.local.state;

		if (state == LAYOUT_RAT_ENTRY_FREE)
			continue;
		layout_rat_map_mark(&seen, i, true);

		gc_sweep_dead_delegations(sbi, entry);

		/* Orphan with no owner node: a create or a delete died with the line unwritten */
		union layout_rat_acl_copy acl;
		cxl_get_layout_rat_acl(&acl, &entry->acl);

		u16 owner_node = acl.local.owner_node_id;
		if (owner_node == 0 && layout_rat_state_is_transient(state)) {
			if (bootstrap_is_admin_node(sbi))
				gc_track_orphan(sbi, entry, GC_ORPHAN_RAT);
			continue;
		}

		/* The entry belongs to nobody and names nobody, so no owner filter reaches it. The admin
		 * is what stands in for the node that is gone. */
		if (state == LAYOUT_RAT_ENTRY_OWNER_DEAD) {
			if (!bootstrap_is_admin_node(sbi))
				continue;
		} else if (owner_node != sbi->node_id) {
			continue;
		}

		if (!gc_is_orphaned(sbi, entry))
			continue;

		/* A user path holding the lock is taking an entry down right now, and this sweep
		 * comes back in ten seconds, so it steps aside rather than queueing behind it. */
		/* The cleanup below takes a name out of the shared index, whose unit is a line other
		 * nodes also write, so both halves have to be in hand. */
		struct meta_lock lock;
		if (!meta_trylock(sbi, &lock, GC_LOCK_WAIT_MS, TEST_META_OP_SWEEP))
			continue;

		/* Judged again under the lock, because a grant that landed since the judgement above
		 * holds a live row and freeing the region would take it from that caller. */
		if (gc_is_orphaned(sbi, entry)) {
			pr_info("gc reclaiming RAT entry %u (state=%u, pid=%u)\n", i, state, acl.local.owner_pid);
			if (gc_cleanup_rat_entry(sbi, entry, gc_has_dead_owner(entry)))
				reclaimed++;
		}

		meta_unlock(sbi, &lock);
	}

	/* The admin's, like the orphans: one node repairing is enough, and the judgement above was
	 * made without the turn, so it is made again under it before anything is written. */
	if (walked) {
		const bool stale = region_map_needs_repair(sbi, &seen);
		/* Every node judges for its own searches; a peer mid-operation can read as stale for
		 * one sweep, which costs that node a full search and nothing else. */
		sbi->rat_map_trusted = !stale;
		if (stale && bootstrap_is_admin_node(sbi)) {
			struct meta_lock lock;
			if (meta_trylock(sbi, &lock, GC_LOCK_WAIT_MS, TEST_META_OP_SWEEP)) {
				const s32 moved = region_repair_map(sbi);
				if (moved > 0)
					pr_info("gc: RAT summary repaired (%d moves)\n", moved);
				meta_unlock(sbi, &lock);
			}
		}
	}

	mutex_unlock(&sbi->gc_sweep_lock);
	return reclaimed;
}

/*
 * Ticking inside the wait rather than beside the sweep keeps liveness running while GC is paused,
 * because a paused GC must not let peers conclude this node died and steal its bootstrap slot.
 *
 * schedule_timeout_interruptible and not msleep_interruptible: only a signal cuts the latter
 * short, so kthread_stop's wakeup would be slept through and a caller waiting on it would pay a
 * whole slice.
 */
static bool gc_sleep_ticking(struct fs_sb_info *sbi, u32 total_ms)
{
	u32 slept_ms = 0;

	while (slept_ms < total_ms) {
		u32 slice = min_t(u32, GC_TICK_MS, total_ms - slept_ms);

		schedule_timeout_interruptible(msecs_to_jiffies(slice));
		if (kthread_should_stop())
			return false;

		bootstrap_tick(sbi);

		/* The tick is what notices, and once it has there is no slot of ours to stamp and no
		 * entry of ours to sweep. Both of this thread's jobs are over. */
		if (fs_is_fenced(sbi))
			return false;

		slept_ms += slice;
	}
	return true;
}

static s32 gc_thread_fn(void *data)
{
	struct fs_sb_info *sbi = data;
	pr_info("gc thread started for node %u\n", sbi->node_id);

	while (1) {
		if (!gc_sleep_ticking(sbi, GC_INTERVAL_MS))
			break;

		if (atomic_read(&sbi->gc_paused) || READ_ONCE(sbi->test_dead))
			continue;

		/* Refreshed per sweep, so the role this node acts on can be one cycle stale. */
		sbi->cached_admin_node_id = bootstrap_get_current_admin_node_id(sbi);

		gc_reclaim_dead_regions(sbi);
		if (kthread_should_stop())
			break;

		gc_sweep_orphans(sbi);
		if (kthread_should_stop())
			break;

		/* The admin is the one node that cleans up after a peer, so that two do not race over
		 * the same rows. A second admin costs one duplicated step, not a wrong one. */
		if (bootstrap_is_admin_node(sbi))
			gc_run_recovery(sbi);
		if (kthread_should_stop())
			break;

		atomic_inc(&sbi->gc_epoch);
	}

	/* The tick above is what fences, and it only latches. Taking the live mappings down is the
	 * rest of it, and this thread is the sleepable context that can. */
	if (fs_is_fenced(sbi))
		file_revoke_all_mappings(sbi);

	/* Parked and not returned, with the sweep and the tick both over. kthread_stop takes a
	 * reference on this task, and a threadfn that returned first leaves it none to take. */
	while (!kthread_should_stop())
		schedule_timeout_interruptible(msecs_to_jiffies(GC_TICK_MS));

	pr_info("gc thread exiting for node %u\n", sbi->node_id);
	return 0;
}

/*
 * The tracker's lifetime is the mount's and not the thread's, so a restart neither reallocates it
 * nor allocates under whatever lock the caller of that restart is holding.
 */
s32 gc_init(struct fs_sb_info *sbi)
{
	mutex_init(&sbi->gc_sweep_lock);

	sbi->gc_orphans = kcalloc(GC_ORPHAN_MAX, sizeof(*sbi->gc_orphans), GFP_KERNEL);
	if (!sbi->gc_orphans)
		return -ENOMEM;

	sbi->gc_orphan_count = 0;
	return 0;
}

void gc_exit(struct fs_sb_info *sbi)
{
	kfree(sbi->gc_orphans);
	sbi->gc_orphans = NULL;
	sbi->gc_orphan_count = 0;
}

s32 gc_start(struct fs_sb_info *sbi)
{
	if (!sbi)
		return -EINVAL;

	if (sbi->gc_thread) {
		pr_warn("gc thread already running\n");
		return -EEXIST;
	}

	/* Hold module reference so rmmod waits for GC exit */
	if (!try_module_get(THIS_MODULE))
		return -ENODEV;

	atomic_set(&sbi->gc_paused, 0);
	atomic_set(&sbi->gc_epoch, 0);
	sbi->gc_orphan_count = 0;

	sbi->gc_thread = kthread_run(gc_thread_fn, sbi, KBUILD_MODNAME "-gc-%u", sbi->node_id);
	if (IS_ERR(sbi->gc_thread)) {
		s32 ret = PTR_ERR(sbi->gc_thread);
		sbi->gc_thread = NULL;
		module_put(THIS_MODULE);
		pr_err("failed to start gc thread: %d\n", ret);
		return ret;
	}

	pr_info("gc thread started (interval=%ums)\n", (u32)GC_INTERVAL_MS);
	return 0;
}

void gc_stop_thread(struct fs_sb_info *sbi)
{
	if (!sbi || !sbi->gc_thread)
		return;

	pr_info("stopping gc thread for node %u\n", sbi->node_id);
	kthread_stop(sbi->gc_thread);
	sbi->gc_thread = NULL;
	module_put(THIS_MODULE);
}
