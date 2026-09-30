/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * daemon.h - public kernel API for the daemon upcall subsystem.
 *
 * The rest of the module reaches a mount's helper through this header alone. The wire format is
 * the uapi the helper compiles against, not anything declared here.
 */

#ifndef _DAEMON_H
#define _DAEMON_H

#include <linux/types.h>

#include "access/daemon_queue.h" /* enum daemon_req_type, union payload types */
#include "daemon_uapi.h"

struct fs_sb_info;

/* ── Lifecycle ───────────────────────────────────────────────────────── */

/* A channel of this mount's own, at /dev/<daemon>-<node_id>. */
s32 daemon_create_channel(struct fs_sb_info *sbi);
void daemon_delete_channel(struct fs_sb_info *sbi);

/* ── Requests ────────────────────────────────────────────────────────── */

/* Enqueue @req on this mount's channel and sleep until its helper answers. @type picks the union
 * arm of @req and @resp, and must be one the helper answers. */
s32 daemon_request(struct fs_sb_info *sbi, enum daemon_req_type type, const union daemon_req_payload *req,
		   union daemon_resp_payload *resp);

/* Fill the arm @type names from the calling task, so every request carries the same fields. */
void daemon_fill_task(enum daemon_req_type type, union daemon_req_payload *req);

/* Wake every caller waiting on this mount's channel with -EIO. For a mount that has just fenced
 * itself, whose answers would be to a node id that is another node's now. */
void daemon_drop_waiters(struct fs_sb_info *sbi);

/* ── Locks ───────────────────────────────────────────────────────────── */

/* Hold @domain until daemon_unlock_domain. @timeout_ms bounds the acquire and 0 asks for the cap.
 * -EOPNOTSUPP means this mount's helper cannot take a lock at all. */
s32 daemon_lock_domain(struct fs_sb_info *sbi, const char *domain, u32 timeout_ms);

struct daemon_pending;

/* A lock asked for and not yet answered, carried on the caller's stack between the two halves. */
struct daemon_lock_wait {
	struct daemon_pending *pending;
	const char *domain;
	u32 timeout_ms;
	s64 answered_before; /* the channel's answer count when this was queued */
};

/* The two halves of daemon_lock_domain. Between them the caller may read what needs no exclusion
 * while the helper takes the turn; finish answers what daemon_lock_domain would have. */
s32 daemon_lock_domain_begin(struct fs_sb_info *sbi, const char *domain, u32 timeout_ms,
			     struct daemon_lock_wait *wait);
s32 daemon_lock_domain_finish(struct fs_sb_info *sbi, struct daemon_lock_wait *wait);

/* Give @domain back. A domain the helper does not hold answers 0, so a repeat is not an error. */
s32 daemon_unlock_domain(struct fs_sb_info *sbi, const char *domain);

/* ── Introspection ───────────────────────────────────────────────────── */

u32 daemon_get_queue_depth(struct fs_sb_info *sbi);

/* Each field is an independent atomic read, not a coherent snapshot. */
struct daemon_state {
	bool reader_open;
	bool hello_done;
	bool helper_dead;
	u32 queue_depth;
	u64 asked;
};

void daemon_get_state(struct fs_sb_info *sbi, struct daemon_state *out);

#endif /* _DAEMON_H */
