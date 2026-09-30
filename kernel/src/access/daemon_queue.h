/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * daemon_queue.h - Internal queue types shared between daemon_dev.c and daemon_queue.c.
 */

#ifndef _DAEMON_QUEUE_H
#define _DAEMON_QUEUE_H

#include <linux/completion.h>
#include <linux/errno.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/wait.h>

#include "daemon_uapi.h"

enum daemon_req_type {
	DAEMON_REQ_ACCESS,
	DAEMON_REQ_ATTEST,
	DAEMON_REQ_LOCK,
	DAEMON_REQ_UNLOCK,
};

/* Tagged-union request payload (kernel → helper). */
union daemon_req_payload {
	struct fs_daemon_access_request access;
	struct fs_daemon_attest_request attest;
	struct fs_daemon_lock_request lock;
	struct fs_daemon_unlock_request unlock;
};

/* Tagged-union response payload (helper → kernel). */
union daemon_resp_payload {
	struct fs_daemon_response access;
	struct fs_daemon_attest_response attest;
	struct fs_daemon_lock_response lock;
};

/* Header immediately followed by payload, so the read path is one copy_to_user of the slot. */
struct daemon_req_frame {
	struct fs_daemon_hdr hdr;
	union daemon_req_payload payload;
} __attribute__((packed));

struct daemon_pending {
	/* A slot sits on two lists at once, so it needs a list_head for each: pending_node on the
	 * list a response is matched against, outbound_node on what the helper has not read. One
	 * shared head for both corrupts pointers and crashes on purge. */
	struct list_head pending_node;
	struct list_head outbound_node;
	enum daemon_req_type type;

	/* Built once at enqueue and copied out verbatim. */
	struct daemon_req_frame req_frame;
	union daemon_resp_payload resp;

	s32 resp_status;
	struct completion done;
	/* One ref is the waiter's, taken at enqueue; a slot with no answer has no waiter, so whoever
	 * unlinks it from pending_list drops that ref. The read path takes a second across the copy
	 * to the helper, so a waiter that gives up in that window frees the memory only once the
	 * copy is done with it. */
	struct kref refs;
};

/* The slots in flight on one channel and the helper waiting to read them. A mount owns one, so
 * one node's helper never sees another node's requests. */
struct daemon_queue {
	spinlock_t lock;
	bool fenced; /* protected by lock; only cleanup notifications remain admissible */
	struct list_head pending_list; /* every slot in flight */
	struct list_head outbound_list; /* those the helper has not read yet */
	atomic_t pending_count;
	atomic64_t seq_counter;
	wait_queue_head_t read_wq;
};

/* ── Type-tag helpers ────────────────────────────────────────────────
 * The tag→size/msg/status mapping, written once here so daemon_dev.c and
 * daemon_queue.c cannot disagree about what a tag means.
 */
static inline bool daemon_check_type(enum daemon_req_type type)
{
	switch (type) {
	case DAEMON_REQ_ACCESS:
	case DAEMON_REQ_ATTEST:
	case DAEMON_REQ_LOCK:
	case DAEMON_REQ_UNLOCK:
		return true;
	}
	return false;
}

/* UNLOCK carries no answer: the helper's release is local and the caller reads nothing back, so the
 * slot is done once the helper has read it and the caller need not wait for that either. */
static inline bool daemon_expects_reply(enum daemon_req_type type)
{
	return type != DAEMON_REQ_UNLOCK;
}

static inline u32 daemon_get_req_size(enum daemon_req_type type)
{
	switch (type) {
	case DAEMON_REQ_ACCESS:
		return sizeof(struct fs_daemon_access_request);
	case DAEMON_REQ_ATTEST:
		return sizeof(struct fs_daemon_attest_request);
	case DAEMON_REQ_LOCK:
		return sizeof(struct fs_daemon_lock_request);
	case DAEMON_REQ_UNLOCK:
		return sizeof(struct fs_daemon_unlock_request);
	}
	return 0;
}

static inline u32 daemon_get_resp_size(enum daemon_req_type type)
{
	switch (type) {
	case DAEMON_REQ_ACCESS:
		return sizeof(struct fs_daemon_response);
	case DAEMON_REQ_ATTEST:
		return sizeof(struct fs_daemon_attest_response);
	case DAEMON_REQ_LOCK:
		return sizeof(struct fs_daemon_lock_response);
	case DAEMON_REQ_UNLOCK:
		break;
	}
	return 0;
}

static inline u32 daemon_req_msg_type(enum daemon_req_type type)
{
	switch (type) {
	case DAEMON_REQ_ACCESS:
		return FS_DAEMON_MSG_ACCESS_REQUEST;
	case DAEMON_REQ_ATTEST:
		return FS_DAEMON_MSG_ATTEST_REQUEST;
	case DAEMON_REQ_LOCK:
		return FS_DAEMON_MSG_LOCK_REQUEST;
	case DAEMON_REQ_UNLOCK:
		return FS_DAEMON_MSG_UNLOCK_REQUEST;
	}
	return 0;
}

static inline __s32 daemon_get_resp_status(enum daemon_req_type type, const union daemon_resp_payload *resp)
{
	switch (type) {
	case DAEMON_REQ_ACCESS:
		return resp->access.status;
	case DAEMON_REQ_ATTEST:
		return resp->attest.status;
	case DAEMON_REQ_LOCK:
		return resp->lock.status;
	case DAEMON_REQ_UNLOCK:
		break;
	}
	return -EINVAL;
}

/* Queue API used by daemon_dev.c. One entry point per stage, with @type
 * selecting which arm of the union payload is read. */
void daemon_queue_init(struct daemon_queue *queue);

struct daemon_pending *daemon_queue_enqueue(struct daemon_queue *queue, enum daemon_req_type type,
					    const union daemon_req_payload *req);

s32 daemon_queue_wait(struct daemon_queue *queue, struct daemon_pending *pending,
		      union daemon_resp_payload *resp_out, u32 timeout_ms);

/* Finish a slot nobody waits on, once the helper has read it. */
void daemon_queue_retire(struct daemon_queue *queue, struct daemon_pending *pending);

s32 daemon_queue_dispatch_response(struct daemon_queue *queue, u64 seq, enum daemon_req_type type,
				   const union daemon_resp_payload *resp);

struct daemon_pending *daemon_queue_dequeue_outbound(struct daemon_queue *queue);
void daemon_queue_requeue_outbound(struct daemon_queue *queue, struct daemon_pending *pending);
bool daemon_queue_has_outbound(struct daemon_queue *queue);

/* Drop one reference to a slot, freeing it when the last goes. */
void daemon_pending_put(struct daemon_pending *pending);
void daemon_queue_purge(struct daemon_queue *queue, s32 err);
void daemon_queue_fence(struct daemon_queue *queue);
u32 daemon_queue_get_depth(const struct daemon_queue *queue);

#endif /* _DAEMON_QUEUE_H */
