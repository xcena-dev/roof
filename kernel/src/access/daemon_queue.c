// SPDX-License-Identifier: GPL-2.0-only
/*
 * daemon_queue.c - the daemon upcall request queue.
 *
 * The pending-request list, sequence ID allocation, per-request completion and response
 * dispatch. Every entry point takes the channel's queue, so nothing here is shared between
 * mounts.
 */

#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/ktime.h>
#include <linux/random.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/wait.h>

#include "access/daemon_queue.h"
#include "test/test_hooks.h"

/* Which kind a timing sample belongs to, in the shape the test hook names its kinds. */
static enum test_upcall_kind daemon_get_upcall_kind(enum daemon_req_type type)
{
	switch (type) {
	case DAEMON_REQ_ACCESS:
		return TEST_UPCALL_ACCESS;
	case DAEMON_REQ_ATTEST:
		return TEST_UPCALL_ATTEST;
	case DAEMON_REQ_LOCK:
		return TEST_UPCALL_LOCK;
	case DAEMON_REQ_UNLOCK:
		return TEST_UPCALL_UNLOCK;
	}
	return TEST_UPCALL_KINDS;
}

/* ── Lifecycle ───────────────────────────────────────────────────────── */

/* The seq counter starts at a random value so a response from a previous helper cannot match a
 * fresh slot after a reconnect. */
void daemon_queue_init(struct daemon_queue *queue)
{
	spin_lock_init(&queue->lock);
	queue->fenced = false;
	INIT_LIST_HEAD(&queue->pending_list);
	INIT_LIST_HEAD(&queue->outbound_list);
	atomic_set(&queue->pending_count, 0);
	atomic64_set(&queue->seq_counter, (s64)get_random_u64());
	init_waitqueue_head(&queue->read_wq);
}

/* ── Internal helpers ────────────────────────────────────────────────── */

static u64 daemon_next_seq(struct daemon_queue *queue)
{
	return atomic64_inc_return(&queue->seq_counter);
}

static struct daemon_pending *daemon_find_pending_locked(struct daemon_queue *queue, u64 seq)
{
	struct daemon_pending *pending;

	list_for_each_entry(pending, &queue->pending_list, pending_node) {
		if (pending->req_frame.hdr.seq == seq)
			return pending;
	}
	return NULL;
}

/* ── Queue public API (called from daemon_dev.c) ──────────────────────── */

static void daemon_pending_release(struct kref *refs)
{
	kfree(container_of(refs, struct daemon_pending, refs));
}

void daemon_pending_put(struct daemon_pending *pending)
{
	if (pending)
		kref_put(&pending->refs, daemon_pending_release);
}

/* Take a slot, build its outbound frame and wake the helper. The caller waits on it next, or for a
 * type with no answer leaves it to the read path. Returns ERR_PTR on a full queue or a bad tag. */
struct daemon_pending *daemon_queue_enqueue(struct daemon_queue *queue, enum daemon_req_type type,
					    const union daemon_req_payload *req)
{
	if (!req || !daemon_check_type(type))
		return ERR_PTR(-EINVAL);

	if (atomic_read(&queue->pending_count) >= FS_DAEMON_QUEUE_MAX)
		return ERR_PTR(-EAGAIN);

	struct daemon_pending *pending = kzalloc(sizeof(*pending), GFP_KERNEL);
	if (!pending)
		return ERR_PTR(-ENOMEM);

	pending->type = type;
	pending->resp_status = -ENOSYS;
	pending->req_frame.hdr.magic = FS_DAEMON_MAGIC;
	pending->req_frame.hdr.version = 1;
	pending->req_frame.hdr.type = daemon_req_msg_type(type);
	pending->req_frame.hdr.seq = daemon_next_seq(queue);
	pending->req_frame.hdr.payload_len = daemon_get_req_size(type);
	pending->req_frame.payload = *req;
	init_completion(&pending->done);
	kref_init(&pending->refs);
	INIT_LIST_HEAD(&pending->pending_node);
	INIT_LIST_HEAD(&pending->outbound_node);

	unsigned long flags;
	spin_lock_irqsave(&queue->lock, flags);
	/* The fence and insertion share the lock, so a request cannot enter after its purge.
	 * UNLOCK stays admissible to return a turn whose acquire was interrupted by the fence. */
	if (queue->fenced && daemon_expects_reply(type)) {
		spin_unlock_irqrestore(&queue->lock, flags);
		kfree(pending);
		return ERR_PTR(-EIO);
	}
	/* re-check under lock to avoid race with purge raising count */
	if (atomic_read(&queue->pending_count) >= FS_DAEMON_QUEUE_MAX) {
		spin_unlock_irqrestore(&queue->lock, flags);
		kfree(pending);
		return ERR_PTR(-EAGAIN);
	}
	list_add_tail(&pending->pending_node, &queue->pending_list);
	list_add_tail(&pending->outbound_node, &queue->outbound_list);
	atomic_inc(&queue->pending_count);
	/* A request with no reply is the read path's to retire, so the slot can be freed the moment
	 * the lock drops. What the sample needs is read while it still cannot. */
	const u64 started_seq = pending->req_frame.hdr.seq;
	spin_unlock_irqrestore(&queue->lock, flags);

	test_start_upcall(started_seq, daemon_get_upcall_kind(type), ktime_get_ns());
	wake_up_interruptible(&queue->read_wq);
	return pending;
}

/* Bounded by the turn round trip: past it the poll only burns CPU, short of it the sleep's own
 * wakeup costs more than polling. */
#define DAEMON_SPIN_US 25

/* True when the answer arrived within the bound, which consumes the completion. */
static bool daemon_poll_for_answer(struct daemon_pending *pending)
{
	const ktime_t give_up_at = ktime_add_us(ktime_get(), DAEMON_SPIN_US);

	do {
		if (try_wait_for_completion(&pending->done))
			return true;
		cpu_relax();
	} while (ktime_before(ktime_get(), give_up_at));

	return false;
}

/* Take a slot off both lists. True when it was still on pending_list, which is when this call is
 * the one that removes it from the count: a purge that got there first has already done both. */
static bool daemon_unlink_pending(struct daemon_queue *queue, struct daemon_pending *pending)
{
	unsigned long flags;
	spin_lock_irqsave(&queue->lock, flags);
	const bool was_on_pending = !list_empty(&pending->pending_node);
	if (was_on_pending)
		list_del_init(&pending->pending_node);
	if (!list_empty(&pending->outbound_node))
		list_del_init(&pending->outbound_node);
	if (was_on_pending)
		atomic_dec(&queue->pending_count);
	spin_unlock_irqrestore(&queue->lock, flags);
	return was_on_pending;
}

/* Block until the helper answers, or the timeout or a signal ends the wait. The slot is off the
 * lists and freed whatever the outcome, so the caller must not touch @pending after this. */
s32 daemon_queue_wait(struct daemon_queue *queue, struct daemon_pending *pending,
		      union daemon_resp_payload *resp_out, u32 timeout_ms)
{
	if (!pending || !resp_out)
		return -EINVAL;

	/* Nonzero for the poll's own answer, which is what "ended with time to spare" reads as. */
	long jiffies_left = 1;

	if (!daemon_poll_for_answer(pending))
		jiffies_left = wait_for_completion_interruptible_timeout(
			&pending->done, msecs_to_jiffies(timeout_ms));
	const u64 resumed_ns = ktime_get_ns();

	daemon_unlink_pending(queue, pending);

	s32 ret;
	if (jiffies_left == 0) {
		ret = -ETIMEDOUT;
	} else if (jiffies_left < 0) {
		ret = -EINTR;
	} else {
		test_stamp_upcall(pending->req_frame.hdr.seq, TEST_UPCALL_RESUMED, resumed_ns);
		ret = pending->resp_status;
		if (ret == 0)
			*resp_out = pending->resp;
	}

	daemon_pending_put(pending);
	return ret;
}

/* The enqueue reference goes only with the unlink, since a purge that unlinked first has dropped
 * it already and the read path's own reference is the caller's to put. */
void daemon_queue_retire(struct daemon_queue *queue, struct daemon_pending *pending)
{
	if (!pending)
		return;
	if (daemon_unlink_pending(queue, pending))
		daemon_pending_put(pending);
}

/* The next request for the helper to read, or NULL when it has read them all. */
struct daemon_pending *daemon_queue_dequeue_outbound(struct daemon_queue *queue)
{
	struct daemon_pending *pending = NULL;
	unsigned long flags;

	spin_lock_irqsave(&queue->lock, flags);
	if (!list_empty(&queue->outbound_list)) {
		pending = list_first_entry(&queue->outbound_list, struct daemon_pending, outbound_node);
		list_del_init(&pending->outbound_node);
		/* Held across the read path's copy, since the slot stays on pending_list where a
		 * waiter that times out can remove and free it. */
		kref_get(&pending->refs);
	}
	spin_unlock_irqrestore(&queue->lock, flags);
	return pending;
}

/* Put a slot back at the head after a failed copy_to_user, so the helper retries it. */
void daemon_queue_requeue_outbound(struct daemon_queue *queue, struct daemon_pending *pending)
{
	if (!pending)
		return;

	unsigned long flags;
	spin_lock_irqsave(&queue->lock, flags);
	/* Only a slot still on pending_list, and only if nothing else linked it. A cancelled one
	 * would go back to the helper forever with no waiter left to take its answer. */
	if (!list_empty(&pending->pending_node) && list_empty(&pending->outbound_node))
		list_add(&pending->outbound_node, &queue->outbound_list);
	spin_unlock_irqrestore(&queue->lock, flags);

	wake_up_interruptible(&queue->read_wq);
}

bool daemon_queue_has_outbound(struct daemon_queue *queue)
{
	unsigned long flags;

	spin_lock_irqsave(&queue->lock, flags);
	bool empty = list_empty(&queue->outbound_list);
	spin_unlock_irqrestore(&queue->lock, flags);
	return !empty;
}

/* Hand a response to the slot that asked for it. -ENOENT when no slot matches, which is a
 * response that arrived after its request timed out, one of the wrong type, or a second answer
 * to a request the first answer already took. */
s32 daemon_queue_dispatch_response(struct daemon_queue *queue, u64 seq, enum daemon_req_type type,
				   const union daemon_resp_payload *resp)
{
	if (!resp || !daemon_check_type(type))
		return -EINVAL;

	test_stamp_upcall(seq, TEST_UPCALL_ANSWERED, ktime_get_ns());

	unsigned long flags;
	spin_lock_irqsave(&queue->lock, flags);
	struct daemon_pending *pending = daemon_find_pending_locked(queue, seq);
	if (pending && pending->type == type) {
		pending->resp = *resp;
		__s32 status = daemon_get_resp_status(type, resp);
		pending->resp_status = (status == 0) ? 0 : status;
		test_stamp_upcall(seq, TEST_UPCALL_MATCHED, ktime_get_ns());

		/* Taking the slot off the lists here is what makes accepting an answer a one-time
		 * transition: a later answer carrying the same seq finds nothing to attach to.
		 * daemon_unlink_pending in the waiter is idempotent, so its own call still works. */
		list_del_init(&pending->pending_node);
		if (!list_empty(&pending->outbound_node))
			list_del_init(&pending->outbound_node);
		atomic_dec(&queue->pending_count);

		complete(&pending->done);
		spin_unlock_irqrestore(&queue->lock, flags);
		return 0;
	}
	spin_unlock_irqrestore(&queue->lock, flags);
	return -ENOENT;
}

/* Fail every slot on this channel with @err and wake whoever waits on it. */
void daemon_queue_purge(struct daemon_queue *queue, s32 err)
{
	struct daemon_pending *pending, *tmp;
	unsigned long flags;
	LIST_HEAD(victims);

	/* pending_list is canonical; outbound is strict subset.
	 * Process EVERYTHING under lock — concurrent wait_* / dispatch_* also
	 * take this lock, so they cannot race with our list mutation.
	 * complete() inside spinlock is safe (uses its own internal lock).
	 */
	spin_lock_irqsave(&queue->lock, flags);
	list_splice_init(&queue->pending_list, &victims);
	INIT_LIST_HEAD(&queue->outbound_list);
	list_for_each_entry_safe(pending, tmp, &victims, pending_node) {
		INIT_LIST_HEAD(&pending->outbound_node);
		list_del_init(&pending->pending_node);
		/* Splice owns every slot we iterate — wait() will see empty
		 * pending_node and skip its own dec. Unconditional dec keeps
		 * the symmetry inc-on-enqueue ↔ dec-on-removal honest, and a
		 * future underflow surfaces as a WARN instead of silently
		 * masked by a defensive guard. */
		atomic_dec(&queue->pending_count);
		pending->resp_status = err;
		/* A slot with no waiter has nobody to wake, so the unlink frees it here instead. */
		if (daemon_expects_reply(pending->type))
			complete(&pending->done);
		else
			daemon_pending_put(pending);
	}
	WARN_ON_ONCE(atomic_read(&queue->pending_count) < 0);
	spin_unlock_irqrestore(&queue->lock, flags);
}

u32 daemon_queue_get_depth(const struct daemon_queue *queue)
{
	return (u32)atomic_read(&queue->pending_count);
}

/* Close admission before purging, including a submit paused before taking the queue lock. */
void daemon_queue_fence(struct daemon_queue *queue)
{
	unsigned long flags;
	spin_lock_irqsave(&queue->lock, flags);
	queue->fenced = true;
	spin_unlock_irqrestore(&queue->lock, flags);
	daemon_queue_purge(queue, -EIO);
}
