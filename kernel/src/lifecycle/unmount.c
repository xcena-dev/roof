// SPDX-License-Identifier: GPL-2.0-only
/*
 * unmount.c - the clean step a departing node asks for while its daemon is still up.
 *
 * The umount helper asks for it before it stops the daemon, because kill_sb runs the same clean
 * after the daemon is gone and can then only leave the slot to the admin's timeout.
 */

#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/sched/signal.h>

#include "core.h"
#include "daemon_uapi.h"
#include "lifecycle/bootstrap.h"
#include "lifecycle/gc.h"
#include "lifecycle/unmount.h"
#include "region/refs.h"
#include "test/test_hooks.h"

/* Milliseconds. A create holds meta_lock across one lock upcall, which the reply deadline bounds,
 * so twice that leaves its own work room and outlasts no create that is still making progress. */
enum unmount_configuration {
	UNMOUNT_CREATE_WAIT_MS = 2 * FS_DAEMON_DEFAULT_TIMEOUT_MS,
	UNMOUNT_CREATE_POLL_MS = 10,
};

/* Waits out every create that read draining clear, since each holds meta_lock until its entry is
 * written. False for a holder that never lets go, and for a caller being killed. */
static bool unmount_wait_for_creates(struct fs_sb_info *sbi)
{
	const u64 deadline_ns = ktime_get_ns() + (u64)UNMOUNT_CREATE_WAIT_MS * NSEC_PER_MSEC;

	while (!mutex_trylock(&sbi->meta_lock)) {
		if (ktime_get_ns() >= deadline_ns || fatal_signal_pending(current))
			return false;
		msleep(UNMOUNT_CREATE_POLL_MS);
	}
	mutex_unlock(&sbi->meta_lock);
	return true;
}

/*
 * unmount_prepare - clear every row and reference this node holds on the medium, while its daemon
 * is still up to take the turn. The clean step is node death, so it runs only once open and create
 * are closed and no file here is open. -EBUSY says a file is still open and nothing was cleared.
 * -EAGAIN says another prepare or a writer still holds the mount or a turn was refused, and the call
 * can be repeated. Once one prepare finds no reference, open and create stay closed.
 */
s32 unmount_prepare(struct fs_sb_info *sbi)
{
	s32 err = 0;

	/* A node a crash point killed is one that lost power: it takes no lock and leaves its rows. */
	if (fs_is_fenced(sbi) || READ_ONCE(sbi->test_dead) || !bootstrap_has_slot(sbi))
		return 0; /* the admin's to clean, and nothing here to add */

	/* A second prepare is the helper retrying, and the helper already backs off on -EAGAIN. */
	if (!mutex_trylock(&sbi->drain_mutex))
		return -EAGAIN;
	if (sbi->drain_latched)
		goto clean;

	WRITE_ONCE(sbi->draining, true);
	smp_mb();
	if (!unmount_wait_for_creates(sbi)) {
		WRITE_ONCE(sbi->draining, false);
		err = -EAGAIN;
		goto out;
	}

	if (region_has_local_refs(sbi)) {
		test_delay_before_drain_refusal();
		WRITE_ONCE(sbi->draining, false);
		err = -EBUSY;
		goto out;
	}
	/* Past here the mount stays closed: what it owned is being released, even if umount fails. */
	sbi->drain_latched = true;

clean:
	if (!gc_clear_own_node(sbi))
		err = -EAGAIN;
out:
	mutex_unlock(&sbi->drain_mutex);
	return err;
}
