// SPDX-License-Identifier: GPL-2.0-only
/*
 * meta_lock.c - taking this node's metadata mutex and the peers' lock as one.
 */

#include "access/meta_lock.h"

#include <linux/errno.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/printk.h>

#include "access/daemon.h"
#include "core.h"
#include "test/test_hooks.h"

/* What the helper answered to a lock, read as the one of three answers a writer acts on. */
static s32 meta_settle_peers(struct fs_sb_info *sbi, s32 ret)
{
	if (ret == 0) {
		test_count_meta_turn(sbi->node_id, TEST_META_TURN_TAKEN);
		return 0;
	}
	test_count_meta_turn(sbi->node_id, TEST_META_TURN_REFUSED);
	/* A helper that stopped answering is its own case, as it is for attest. */
	if (ret == -ETIMEDOUT)
		return ret;

	/* No helper, or one that cannot lock: nothing else on this node can exclude a peer, so the
	 * writer is refused rather than let through on the mutex alone. */
	if (ret == -ENOSYS || ret == -EOPNOTSUPP)
		return -EAGAIN;

	/* Everything else is a peer still holding it, or a transport that will recover. Transient,
	 * so the caller sees the one answer it already retries on. */
	pr_debug_ratelimited("meta lock refused: %d\n", ret);
	return -EAGAIN;
}

/* Internal to the locks below: nothing else takes one half on its own. */
static s32 meta_lock_peers(struct fs_sb_info *sbi, u32 within_ms)
{
	/* A stubbed daemon grants the turn it would have taken: a test mount with no helper still
	 * excludes its own writers through the mutex above this. */
	if (test_daemon_is_stubbed(sbi->node_id))
		return 0;
	return meta_settle_peers(sbi, daemon_lock_domain(sbi, FS_DOMAIN_META, within_ms));
}

s32 meta_lock_begin(struct fs_sb_info *sbi, struct meta_lock *taken, enum test_meta_op op)
{
	if (READ_ONCE(sbi->test_dead))
		return -EIO;

	taken->op = op;
	taken->began_ns = ktime_get_ns();

	s32 ret = mutex_lock_interruptible(&sbi->meta_lock);
	if (ret)
		return ret;
	taken->after_mutex_ns = ktime_get_ns();

	taken->wait = (struct daemon_lock_wait){};
	if (test_daemon_is_stubbed(sbi->node_id))
		return 0;

	ret = daemon_lock_domain_begin(sbi, FS_DOMAIN_META, 0, &taken->wait);
	if (ret) {
		mutex_unlock(&sbi->meta_lock);
		return meta_settle_peers(sbi, ret);
	}
	return 0;
}

s32 meta_lock_finish(struct fs_sb_info *sbi, struct meta_lock *taken)
{
	if (test_daemon_is_stubbed(sbi->node_id)) {
		taken->after_lock_ns = ktime_get_ns();
		return 0;
	}

	s32 ret = meta_settle_peers(sbi, daemon_lock_domain_finish(sbi, &taken->wait));
	if (ret) {
		mutex_unlock(&sbi->meta_lock);
		return ret;
	}
	taken->after_lock_ns = ktime_get_ns();
	return 0;
}

s32 meta_lock(struct fs_sb_info *sbi, struct meta_lock *taken, enum test_meta_op op)
{
	s32 ret = meta_lock_begin(sbi, taken, op);
	return ret ? ret : meta_lock_finish(sbi, taken);
}

bool meta_trylock(struct fs_sb_info *sbi, struct meta_lock *taken, u32 within_ms,
		  enum test_meta_op op)
{
	if (READ_ONCE(sbi->test_dead))
		return false;

	taken->op = op;
	taken->began_ns = ktime_get_ns();

	if (!mutex_trylock(&sbi->meta_lock))
		return false;
	taken->after_mutex_ns = ktime_get_ns();

	if (meta_lock_peers(sbi, within_ms)) {
		mutex_unlock(&sbi->meta_lock);
		return false;
	}

	taken->after_lock_ns = ktime_get_ns();
	return true;
}

void meta_unlock(struct fs_sb_info *sbi, const struct meta_lock *taken)
{
	/* A node that died mid-write took both halves with it, so its peers get the turn back the
	 * way they would from a real death: through the helper's recovery, not from here. */
	if (READ_ONCE(sbi->test_dead))
		return;

	/* Read before the release, so what lands in WORK is the caller's own and not the upcall
	 * this function is about to make. */
	const u64 before_unlock_ns = ktime_get_ns();

	if (!test_daemon_is_stubbed(sbi->node_id))
		daemon_unlock_domain(sbi, FS_DOMAIN_META);
	mutex_unlock(&sbi->meta_lock);

	test_fold_meta_stages(sbi->node_id, taken->op, taken->began_ns, taken->after_mutex_ns,
			      taken->after_lock_ns, before_unlock_ns, ktime_get_ns());
}
