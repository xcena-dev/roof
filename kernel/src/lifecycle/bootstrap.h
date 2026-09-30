/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * bootstrap.h - the bootstrap slot API.
 *
 * Bootstrap owns mount-time election, the format gate, and the per-node liveness tick that
 * decides the admin role. A caller runs one of the two mounts below and never writes a slot field
 * itself, so what each field means stays inside this subsystem.
 */

#ifndef _FS_BOOTSTRAP_H
#define _FS_BOOTSTRAP_H

#include <linux/types.h>

#include "core.h"
#include "layout/bootstrap.h"

/*
 * The two ways a mount takes a slot. Each runs its whole path, claim through format or wait, and
 * returns with @sbi carrying this node's slot and node_id.
 *
 * A held slot is held whatever its stamp says, so -EBUSY from either one means an owner has not
 * released it. Only a release, or an eviction standing in for one, frees a slot.
 */
s32 bootstrap_auto_mount(struct fs_sb_info *sbi);
s32 bootstrap_manual_mount(struct fs_sb_info *sbi, u32 node_id, bool want_format);

/* Hands the slot back so the next mounter can take it at once. No thread to stop. */
void bootstrap_release(struct fs_sb_info *sbi);

/*
 * Recovering a node that stopped answering, one sweep step at a time. Nothing is remembered between
 * calls: the slot carries which step is next, so any admin can pick up where a dead one stopped.
 *
 * The stake keeps the slot held, which is what stops the node_id being claimed while rows still
 * name it, and fences the owner if it wakes. Only bootstrap_evict_node frees the slot, and it goes
 * last, once the caller has cleared every row that named @node_id.
 */
enum bootstrap_recovery_step {
	BOOTSTRAP_RECOVERY_NONE, /* every slot is either live or already free */
	BOOTSTRAP_RECOVERY_STAKE, /* a dead node nobody is clearing up after */
	BOOTSTRAP_RECOVERY_CLEAN, /* staked, so its rows are this node's to clear */
};

enum bootstrap_recovery_step bootstrap_next_recovery(struct fs_sb_info *sbi, u32 *node_id);
s32 bootstrap_stake_recovery(struct fs_sb_info *sbi, u32 node_id);
s32 bootstrap_evict_node(struct fs_sb_info *sbi, u32 node_id);

/*
 * bootstrap_tick - advance this node's liveness counter and stamp.
 * A no-op until a slot is claimed, and it stops once the slot stops being ours.
 */
void bootstrap_tick(struct fs_sb_info *sbi);

/* Whether the slot still answers to this mount's token with no admin recovering it. A frozen
 * tick never learns that it lost the slot, so a path that acts as this node asks here first. */
bool bootstrap_has_slot(struct fs_sb_info *sbi);

/*
 * Admin role: the lowest node_id whose slot is claimed and still ticking.
 * bootstrap_is_admin_node reads the value cached at mount and per GC sweep.
 */
u32 bootstrap_get_current_admin_node_id(struct fs_sb_info *sbi);
bool bootstrap_is_admin_node(struct fs_sb_info *sbi);

#endif /* _FS_BOOTSTRAP_H */
