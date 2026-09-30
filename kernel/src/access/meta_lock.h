/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * meta_lock.h - the two locks a metadata writer holds.
 *
 * This node's writers are serialised by the mount's own mutex, and the other nodes by a lock the
 * helper holds on its behalf. Taking them together, always in that order, is what keeps one asker
 * per mount at the helper: it excludes nodes, not the askers queued behind one.
 *
 * The two acquires differ only in how they wait.
 */

#ifndef _ACCESS_META_LOCK_H
#define _ACCESS_META_LOCK_H

#include <linux/types.h>

#include "access/daemon.h"
#include "test/test_hooks.h"

struct fs_sb_info;

/* What a taken lock leaves the caller to give back. An acquire either takes both halves or takes
 * neither; between meta_lock_begin and meta_lock_finish the mutex is held and the peers' half is
 * asked for, and finish either completes the pair or gives the mutex back.
 *
 * The stamps ride here because this is what a caller already carries from its acquire to its
 * release, so nothing else has to be threaded through the scope between the two. */
struct meta_lock {
	enum test_meta_op op;
	u64 began_ns;
	u64 after_mutex_ns;
	u64 after_lock_ns;
	struct daemon_lock_wait wait;
};

/* Wait for both halves, and let a signal abort the wait. What almost every writer wants. @op is
 * which operation the stage counters attribute this hold to. */
s32 meta_lock(struct fs_sb_info *sbi, struct meta_lock *taken, enum test_meta_op op);

/* meta_lock in two halves, for a writer with reads that need no exclusion: it asks for the peers'
 * half, reads while the helper takes the turn, then waits. What it read is a hint to confirm once
 * finish returns 0, since a peer may have written between the read and the grant. */
s32 meta_lock_begin(struct fs_sb_info *sbi, struct meta_lock *taken, enum test_meta_op op);
s32 meta_lock_finish(struct fs_sb_info *sbi, struct meta_lock *taken);

/* Step aside rather than queue, on either half. A poll thread cannot afford to wait for this
 * node's writers, and what it does not get now it gets on its next pass. */
bool meta_trylock(struct fs_sb_info *sbi, struct meta_lock *taken, u32 within_ms,
		  enum test_meta_op op);

/* Gives back whichever halves the lock got. */
void meta_unlock(struct fs_sb_info *sbi, const struct meta_lock *taken);

#endif /* _ACCESS_META_LOCK_H */
