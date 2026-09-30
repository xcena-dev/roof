// SPDX-License-Identifier: GPL-2.0-only
/*
 * bootstrap.c - mount-time election, the format gate, and the per-node liveness tick.
 *
 * A claim writes the whole slot, sleeps the settle window so a concurrent claimant finishes its
 * own write, then rereads: the slot still carrying our token is ours. No CAS is needed, because
 * a slot is one line and cross-host CXL writes resolve at last-store-to-line granularity.
 */

#include <linux/delay.h>
#include <linux/random.h>

#include "access/daemon.h"
#include "core.h"
#include "layout/layout_access.h"
#include "lifecycle/bootstrap.h"
#include "lifecycle/format.h"
#include "region/refs.h"
#include "test/test_hooks.h"

/* Milliseconds throughout, and none of it is on the medium, so it lives here. */
enum bootstrap_configuration {
	BOOTSTRAP_SETTLE_MS = 20,
	BOOTSTRAP_POLL_MS = 50,

	/* A format costs well under a millisecond and does not grow with the device, so nearly all of
	 * this covers a formatter descheduled mid-mount. */
	BOOTSTRAP_FORMAT_TIMEOUT_MS = 1000,

	/* Past this age a heartbeat says nothing: the admin scan skips the slot, and an eviction stops
	 * asking. No slot reuse turns on it. */
	BOOTSTRAP_HEARTBEAT_TIMEOUT_MS = 8000,
};

static struct bootstrap_slot *bootstrap_get_slot(struct fs_sb_info *sbi, s32 idx)
{
	if (WARN_ON_ONCE(idx < 0 || idx >= LAYOUT_BOOTSTRAP_MAX_SLOTS))
		return NULL;
	if (unlikely(!sbi || !sbi->bootstrap_slots))
		return NULL;
	return &sbi->bootstrap_slots[idx];
}

/* Freeing a slot writes zero here, and the tick's only identity test is this value, so a holder
 * that owned zero would go on reading a freed slot as its own. */
static u64 bootstrap_gen_nonzero_token(void)
{
	u64 token;

	do {
		token = get_random_u64();
	} while (token == 0);
	return token;
}

/*
 * Write the claim, settle, then reread to see whose token survived.
 * Returns 0 when ours did and -EAGAIN when a peer's did.
 *
 * @observed_token takes the token seen after the settle window, and may be NULL.
 */
static s32 bootstrap_claim_write_and_verify(struct bootstrap_slot *slot, u64 token, u64 *observed_token)
{
	union bootstrap_slot_copy claim = {};
	claim.local.magic = LAYOUT_BOOTSTRAP_MAGIC;
	claim.local.state = LAYOUT_BOOTSTRAP_STATE_HELD;
	claim.local.token = token;
	/* Stamped now rather than left at zero: a peer that sees the token must already see a
	 * tick, or it would probe this node as dead before the first GC tick lands. */
	claim.local.heartbeat = ktime_get_real_ns();
	cxl_set_bootstrap_slot(slot, &claim);

	msleep(BOOTSTRAP_SETTLE_MS);

	union bootstrap_slot_copy settled;
	cxl_get_bootstrap_slot(&settled, slot);
	if (observed_token)
		*observed_token = settled.local.token;
	return (settled.local.token == token) ? 0 : -EAGAIN;
}

/*
 * The slot answers to another token, so recovery gave this node_id to somebody else while this
 * mount was silent. Stop here rather than keep writing rows that name a node this is not.
 */
static void bootstrap_fence_mount(struct fs_sb_info *sbi)
{
	if (READ_ONCE(sbi->fenced))
		return;

	WRITE_ONCE(sbi->fenced, true);
	pr_warn("bootstrap: node %u lost slot %d, fencing this mount\n", sbi->node_id, sbi->bootstrap_slot_idx);

	/* A caller asleep on the daemon is woken now: its answer would be to a node id that is another
	 * node's, and the wait would otherwise run its whole length for it. */
	daemon_drop_waiters(sbi);
}

/* Reads @slot into @seen and says whether it is still this mount's to write. */
static bool bootstrap_is_slot_ours(struct fs_sb_info *sbi, struct bootstrap_slot *slot,
				   union bootstrap_slot_copy *seen)
{
	cxl_get_bootstrap_slot(seen, slot);

	/* Our own release clears the index before the token, so an index still live here means
	 * somebody else wrote this slot: a claim after an eviction, or the eviction itself. */
	if (seen->local.token != sbi->bootstrap_token)
		return false;

	/* An admin took this node for dead and is clearing its rows. The token is still ours, which is
	 * what keeps the node_id off the market, so the state is the only thing that says to stop. */
	return seen->local.state != LAYOUT_BOOTSTRAP_STATE_RECOVERING;
}

/*
 * bootstrap_tick - restamp this node's slot.
 *
 * Only the node owning the slot calls this, and a caller holding no slot is a no-op, so a failed
 * mount cannot stamp somebody else's slot.
 */
void bootstrap_tick(struct fs_sb_info *sbi)
{
	if (!sbi)
		return;

	/* Read once: bootstrap_release drops this to -1 from another thread, and a second read
	 * between the test and the lookup would hand -1 to bootstrap_get_slot's WARN. */
	s32 idx = READ_ONCE(sbi->bootstrap_slot_idx);
	if (idx < 0)
		return;
	if (test_tick_is_frozen(sbi->node_id))
		return; /* fault injection: look crashed to peers */

	struct bootstrap_slot *slot = bootstrap_get_slot(sbi, idx);
	if (!slot)
		return;

	union bootstrap_slot_copy seen;
	if (!bootstrap_is_slot_ours(sbi, slot, &seen)) {
		bootstrap_fence_mount(sbi);
		return;
	}

	seen.local.heartbeat = test_tick_stamp(sbi->node_id, ktime_get_real_ns());
	cxl_set_bootstrap_slot(slot, &seen);
}

bool bootstrap_has_slot(struct fs_sb_info *sbi)
{
	s32 idx = READ_ONCE(sbi->bootstrap_slot_idx);
	if (idx < 0)
		return false;

	struct bootstrap_slot *slot = bootstrap_get_slot(sbi, idx);
	if (!slot)
		return false;

	union bootstrap_slot_copy seen;
	return bootstrap_is_slot_ours(sbi, slot, &seen);
}

enum peer_verdict {
	PEER_ALIVE,
	PEER_DEAD,
	PEER_UNKNOWN, /* not yet decided, so it stakes nobody and names no admin */
};

/*
 * Whether the stamp's value moves, timed on this node's clock and so whatever the holder's clock
 * reads. Refreshed on every read, so a value that stops has been timed since it was first seen.
 */
static enum peer_verdict bootstrap_watch_stamp(struct fs_sb_info *sbi, s32 idx, const union bootstrap_slot_copy *seen)
{
	struct bootstrap_stamp_watch *watch = &sbi->stamp_watch[idx];
	const u64 now = ktime_get_ns();

	if (watch->since_ns == 0 || watch->token != seen->local.token) {
		*watch = (struct bootstrap_stamp_watch){ .token = seen->local.token, .stamp = seen->local.heartbeat, .since_ns = now };
		return PEER_UNKNOWN;
	}

	if (watch->stamp != seen->local.heartbeat) {
		watch->stamp = seen->local.heartbeat;
		watch->since_ns = now;
		watch->moved = true;
		return PEER_ALIVE;
	}

	if (now - watch->since_ns >= (u64)BOOTSTRAP_HEARTBEAT_TIMEOUT_MS * NSEC_PER_MSEC)
		return PEER_DEAD;
	return watch->moved ? PEER_ALIVE : PEER_UNKNOWN;
}

/*
 * bootstrap_judge_slot - whether a slot's holder still ticks, without sleeping.
 *
 * A stamp within one timeout of this node's wall clock, either side, is what a live holder with a
 * synced clock writes, so it reads as alive. Any other stamp falls back to whether the value moves.
 */
static enum peer_verdict bootstrap_judge_slot(struct fs_sb_info *sbi, s32 idx, const union bootstrap_slot_copy *seen,
					      u64 now)
{
	const enum peer_verdict moving = bootstrap_watch_stamp(sbi, idx, seen);
	const u64 stamp = seen->local.heartbeat;
	const u64 timeout_ns = (u64)BOOTSTRAP_HEARTBEAT_TIMEOUT_MS * NSEC_PER_MSEC;
	const u64 distance = (stamp > now) ? stamp - now : now - stamp;

	return (distance < timeout_ns) ? PEER_ALIVE : moving;
}

/*
 * bootstrap_get_current_admin_node_id - the lowest node_id whose slot is held and ticking, or 0
 * when none qualifies, which reads as "this node is not the admin".
 *
 * Staleness only and no probe: a wrong verdict costs a second admin, and both admins reclaim the
 * same orphans, so the answer is worth no sleep.
 */
u32 bootstrap_get_current_admin_node_id(struct fs_sb_info *sbi)
{
	if (!sbi->meta_base)
		return 0;

	struct bootstrap_slot *slots = bootstrap_get_slot(sbi, 0);
	if (!slots)
		return 0;

	u64 now = ktime_get_real_ns();

	for (s32 i = 0; i < LAYOUT_BOOTSTRAP_MAX_SLOTS; i++) {
		union bootstrap_slot_copy seen;
		cxl_get_bootstrap_slot(&seen, &slots[i]);
		if (seen.local.state != LAYOUT_BOOTSTRAP_STATE_HELD)
			continue;
		if (bootstrap_judge_slot(sbi, i, &seen, now) != PEER_ALIVE)
			continue;

		return (u32)(i + 1); /* ascending slot order, so the first hit is the lowest */
	}

	return 0;
}

/*
 * Reads what mount cached and each GC sweep refreshes, so no path rescans the slot table. A sysfs
 * write sweeps too, so this is read off the GC thread as well, and neither side is annotated: a
 * stale answer costs a second admin, and two admins reclaim the same orphans.
 */
bool bootstrap_is_admin_node(struct fs_sb_info *sbi)
{
	return sbi->cached_admin_node_id == sbi->node_id;
}

/*
 * bootstrap_claim - take the first free slot, and report which one in @out_slot_idx.
 *
 * Held is held whatever the stamp says: a node_id names its owner in every RAT entry and delegation
 * row that owner wrote, so a second node taking it would answer to those grants. Only a release or
 * an eviction frees a slot.
 *
 * Returns -EAGAIN when a peer won the same slot, which the caller answers by scanning again, and
 * -EBUSY when every slot is held.
 */
static s32 bootstrap_claim(struct fs_sb_info *sbi, s32 *out_slot_idx)
{
	struct bootstrap_slot *slots = bootstrap_get_slot(sbi, 0);
	if (!slots)
		return -EINVAL;

	s32 free_idx = -1;

	for (s32 i = 0; i < LAYOUT_BOOTSTRAP_MAX_SLOTS; i++) {
		union bootstrap_slot_copy seen;
		cxl_get_bootstrap_slot(&seen, &slots[i]);

		/* An uninitialised slot on a fresh device */
		if (seen.local.magic != LAYOUT_BOOTSTRAP_MAGIC) {
			free_idx = i;
			break;
		}
		/* Nobody holds it: never claimed, handed back by a graceful umount, or recovered */
		if (seen.local.state == LAYOUT_BOOTSTRAP_STATE_FREE) {
			free_idx = i;
			break;
		}
	}

	if (free_idx < 0)
		return -EBUSY;

	test_delay_before_claim_write();

	struct bootstrap_slot *tgt = &slots[free_idx];
	u64 observed;
	u64 token = bootstrap_gen_nonzero_token();
	s32 ret = bootstrap_claim_write_and_verify(tgt, token, &observed);

	if (ret) {
		pr_info("bootstrap: claim race lost on slot %d (my_token=0x%016llx observed=0x%016llx)\n", free_idx, token,
			observed);
		return -EAGAIN;
	}

	sbi->bootstrap_slot_idx = free_idx;
	sbi->bootstrap_token = token;
	*out_slot_idx = free_idx;
	return 0;
}

/* Claims slot[node_id-1] for a manual mount, and refuses with -EBUSY while anyone holds it. */
static s32 bootstrap_claim_explicit(struct fs_sb_info *sbi, u32 node_id)
{
	if (node_id == 0 || node_id > LAYOUT_BOOTSTRAP_MAX_SLOTS)
		return -EINVAL;

	s32 idx = (s32)(node_id - 1);
	struct bootstrap_slot *tgt = bootstrap_get_slot(sbi, idx);

	if (!tgt)
		return -EINVAL;

	union bootstrap_slot_copy seen;
	cxl_get_bootstrap_slot(&seen, tgt);

	/* Naming a node_id asks for that node's identity, and the rows it wrote still carry it. */
	if (seen.local.magic == LAYOUT_BOOTSTRAP_MAGIC && seen.local.state != LAYOUT_BOOTSTRAP_STATE_FREE) {
		pr_err("bootstrap: slot %d (node_id %u) is held\n", idx, node_id);
		return -EBUSY;
	}

	u64 token = bootstrap_gen_nonzero_token();

	if (bootstrap_claim_write_and_verify(tgt, token, NULL))
		return -EAGAIN;

	sbi->bootstrap_slot_idx = idx;
	sbi->bootstrap_token = token;
	return 0;
}

void bootstrap_release(struct fs_sb_info *sbi)
{
	s32 idx = sbi->bootstrap_slot_idx;
	if (idx < 0)
		return;

	/* The index goes before the token, and not after: the tick reads both, and a zeroed token
	 * under a live index is what it reads as a peer having taken the slot. */
	WRITE_ONCE(sbi->bootstrap_slot_idx, -1);

	/* Zeroing the token is the whole release: it is what a scan reads as free. Only ours goes,
	 * because an eviction may have freed this slot already and a later mount taken it. */
	struct bootstrap_slot *tgt = bootstrap_get_slot(sbi, idx);
	if (tgt) {
		union bootstrap_slot_copy seen;
		cxl_get_bootstrap_slot(&seen, tgt);
		/* RECOVERING keeps the token until the admin has cleared every row naming this id. */
		if (seen.local.token == sbi->bootstrap_token &&
		    seen.local.state == LAYOUT_BOOTSTRAP_STATE_HELD) {
			seen.local.state = LAYOUT_BOOTSTRAP_STATE_FREE;
			seen.local.token = 0;
			cxl_set_bootstrap_slot(tgt, &seen);
			pr_info("bootstrap: node %u slot %d released\n", sbi->node_id, idx);
		}
	}
}

/* True while a live node other than this one is cleaning that slot up. */
static bool bootstrap_recovery_is_held(struct fs_sb_info *sbi, u32 recoverer, u64 now)
{
	if (recoverer == 0 || recoverer > LAYOUT_BOOTSTRAP_MAX_SLOTS || recoverer == sbi->node_id)
		return false;

	struct bootstrap_slot *slot = bootstrap_get_slot(sbi, (s32)(recoverer - 1));
	if (!slot)
		return false;

	union bootstrap_slot_copy seen;
	cxl_get_bootstrap_slot(&seen, slot);
	if (seen.local.state == LAYOUT_BOOTSTRAP_STATE_FREE)
		return false; /* it left, so the cleanup it staked is nobody's */

	return bootstrap_judge_slot(sbi, (s32)(recoverer - 1), &seen, now) == PEER_ALIVE;
}

/*
 * bootstrap_next_recovery - what this node's sweep should do next, and to which node_id.
 *
 * A slot qualifies when its owner's heartbeat has timed out and no other live node has staked it,
 * so a casualty nobody has touched and one whose recoverer died mid-cleanup come back from the same
 * scan. No step is remembered between sweeps: the slot itself says which step is next.
 */
enum bootstrap_recovery_step bootstrap_next_recovery(struct fs_sb_info *sbi, u32 *node_id)
{
	struct bootstrap_slot *slots = bootstrap_get_slot(sbi, 0);
	if (!slots)
		return BOOTSTRAP_RECOVERY_NONE;

	u64 now = ktime_get_real_ns();

	for (s32 idx = 0; idx < LAYOUT_BOOTSTRAP_MAX_SLOTS; idx++) {
		if (idx == READ_ONCE(sbi->bootstrap_slot_idx))
			continue;

		union bootstrap_slot_copy seen;
		cxl_get_bootstrap_slot(&seen, &slots[idx]);

		if (seen.local.state == LAYOUT_BOOTSTRAP_STATE_FREE)
			continue;
		/* Only a stamp that stopped advancing stakes a peer, by either clock. */
		if (bootstrap_judge_slot(sbi, idx, &seen, now) != PEER_DEAD)
			continue;
		if (bootstrap_recovery_is_held(sbi, seen.local.recoverer, now))
			continue;

		/* Only a stake in this node's own name says the rows are this node's to clear. Picking
		 * up an abandoned one therefore goes through the stake again, and one cleaner stays one. */
		*node_id = (u32)(idx + 1);
		bool mine = seen.local.state == LAYOUT_BOOTSTRAP_STATE_RECOVERING &&
			    seen.local.recoverer == sbi->node_id;
		return mine ? BOOTSTRAP_RECOVERY_CLEAN : BOOTSTRAP_RECOVERY_STAKE;
	}

	return BOOTSTRAP_RECOVERY_NONE;
}

/*
 * bootstrap_stake_recovery - put this node's name on the cleanup of @node_id's slot.
 *
 * The token carries over untouched, so the slot stays held for the whole cleanup and nothing claims
 * the node_id while rows still name it. Two admins reaching here at once settle it the way two
 * claimants do. Returns -EAGAIN to the one that lost, which finds another target next sweep.
 */
s32 bootstrap_stake_recovery(struct fs_sb_info *sbi, u32 node_id)
{
	if (node_id == 0 || node_id > LAYOUT_BOOTSTRAP_MAX_SLOTS)
		return -EINVAL;

	struct bootstrap_slot *tgt = bootstrap_get_slot(sbi, (s32)(node_id - 1));
	if (!tgt)
		return -EINVAL;

	union bootstrap_slot_copy stake;
	cxl_get_bootstrap_slot(&stake, tgt);
	if (stake.local.state == LAYOUT_BOOTSTRAP_STATE_FREE)
		return -ENOENT;

	stake.local.state = LAYOUT_BOOTSTRAP_STATE_RECOVERING;
	stake.local.recoverer = sbi->node_id;
	cxl_set_bootstrap_slot(tgt, &stake);

	msleep(BOOTSTRAP_SETTLE_MS);

	union bootstrap_slot_copy settled;
	cxl_get_bootstrap_slot(&settled, tgt);
	if (settled.local.recoverer != sbi->node_id)
		return -EAGAIN;

	pr_warn("bootstrap: node %u took node %u for dead, recovering slot %d\n", sbi->node_id, node_id,
		(s32)(node_id - 1));
	return 0;
}

/*
 * bootstrap_evict_node - free the slot @node_id holds, now that nothing points back at it.
 *
 * The last write of a recovery, and the first moment the node_id is claimable again. It is last
 * because a scan skips a free slot: an admin dying before this leaves the slot recoverable, while
 * one dying after a premature free would leave rows no scan reaches.
 */
s32 bootstrap_evict_node(struct fs_sb_info *sbi, u32 node_id)
{
	if (node_id == 0 || node_id > LAYOUT_BOOTSTRAP_MAX_SLOTS)
		return -EINVAL;

	s32 idx = (s32)(node_id - 1);
	struct bootstrap_slot *tgt = bootstrap_get_slot(sbi, idx);
	if (!tgt)
		return -EINVAL;

	union bootstrap_slot_copy seen;
	cxl_get_bootstrap_slot(&seen, tgt);
	if (seen.local.state == LAYOUT_BOOTSTRAP_STATE_FREE)
		return -ENOENT;

	seen.local.state = LAYOUT_BOOTSTRAP_STATE_FREE;
	seen.local.token = 0;
	seen.local.recoverer = 0;
	cxl_set_bootstrap_slot(tgt, &seen);

	pr_warn("bootstrap: node %u freed slot %d, node %u recovered\n", sbi->node_id, idx, node_id);
	return 0;
}

/*
 * bootstrap_wait_for_format - a joiner polls the GSB magic until the formatter publishes it.
 *
 * Returns 0 once the magic is valid, and -EAGAIN when it never appeared inside the timeout, which
 * is the caller's cue to steal slot[0] and format it itself.
 */
static s32 bootstrap_wait_for_format(struct fs_sb_info *sbi)
{
	struct layout_superblock *gsb = layout_get_superblock(sbi);

	if (!gsb)
		return -EINVAL;

	pr_info("bootstrap: node %u (slot %d) waiting for formatter\n", sbi->node_id, sbi->bootstrap_slot_idx);

	u64 local_start = ktime_get_ns();
	u64 t_max_ns = (u64)test_format_timeout_ms(BOOTSTRAP_FORMAT_TIMEOUT_MS) * NSEC_PER_MSEC;

	while (1) {
		union layout_superblock_head_copy head;
		cxl_get_layout_superblock_head(&head, &gsb->head);
		if (head.local.magic == LAYOUT_MAGIC) {
			pr_info("bootstrap: format detected, node %u proceeding\n", sbi->node_id);
			return 0;
		}

		u64 elapsed_ns = ktime_get_ns() - local_start;

		if (elapsed_ns > t_max_ns) {
			pr_warn("bootstrap: no format after %llu ms, entering recovery\n", elapsed_ns / NSEC_PER_MSEC);
			return -EAGAIN;
		}

		msleep_interruptible(BOOTSTRAP_POLL_MS);
	}
}

/*
 * bootstrap_steal_stuck_slot0 - take slot[0] from a formatter that never finished.
 *
 * Returns 0 once this node holds slot[0], and -EAGAIN when another node won the same steal.
 */
static s32 bootstrap_steal_stuck_slot0(struct fs_sb_info *sbi)
{
	struct bootstrap_slot *slot0 = bootstrap_get_slot(sbi, 0);
	if (!slot0)
		return -EINVAL;

	u64 token = bootstrap_gen_nonzero_token();
	if (bootstrap_claim_write_and_verify(slot0, token, NULL)) {
		pr_info("bootstrap: lost slot[0] steal race, retry\n");
		return -EAGAIN;
	}

	pr_info("bootstrap: node %u stole slot[0] for recovery\n", sbi->node_id);

	/* Clear what a format writes and leave the bootstrap area alone, so the re-format starts
	 * from nothing rather than from a half-written layout. */
	void *base = sbi->meta_base;
	u64 regions_start = LAYOUT_DATA_OFFSET;

	cxl_zero_cachelines(base, LAYOUT_SUPERBLOCK_SIZE);
	cxl_zero_cachelines(layout_get_meta_ptr(sbi, LAYOUT_SHARD_TABLE_OFFSET), regions_start - LAYOUT_SHARD_TABLE_OFFSET);

	sbi->bootstrap_slot_idx = 0;
	sbi->bootstrap_token = token;
	return 0;
}

/* ── Mount-time election ─────────────────────────────────────────────────── */

/*
 * Formats the device, and hands the slot back when that fails so it stops blocking joiners.
 *
 * The GSB magic format_device writes is the publication, so the slot needs no further write. The
 * restamp only refreshes an age: nothing ticks while a format runs, and a peer scanning right
 * after a slow one would otherwise read this node as dead.
 */
static s32 bootstrap_run_format(struct fs_sb_info *sbi)
{
	s32 ret = format_device(sbi);
	if (ret) {
		pr_err("bootstrap: format failed: %d\n", ret);
		bootstrap_release(sbi);
		return ret;
	}

	bootstrap_tick(sbi);
	return 0;
}

/*
 * The formatter's own path, for slot[0] on a device carrying no layout.
 *
 * Returns -EOWNERDEAD under stuck-formatter injection, which leaves slot[0] held with no magic
 * for joiners to time out on, and the caller must not release the slot in that case.
 */
static s32 bootstrap_run_formatter(struct fs_sb_info *sbi)
{
	pr_info("bootstrap: formatter elected (slot=0, node_id=%u)\n", sbi->node_id);

	if (test_should_inject_stuck_formatter()) {
		pr_info("bootstrap: stuck-formatter injection active, leaving slot[0] unformatted (test-only)\n");
		return -EOWNERDEAD;
	}

	return bootstrap_run_format(sbi);
}

/*
 * A joiner's path: wait for the formatter, and recover from one that never finished.
 *
 * Every exit but the successful one leaves this node holding no slot, so -EAGAIN is the caller's
 * cue to claim again and nothing else.
 */
static s32 bootstrap_run_joiner(struct fs_sb_info *sbi)
{
	pr_info("bootstrap: joiner waiting for format (slot=%d, node_id=%u)\n", sbi->bootstrap_slot_idx, sbi->node_id);

	s32 ret = bootstrap_wait_for_format(sbi);
	if (ret == 0)
		return 0;

	/* Nothing below wants this slot: the steal overwrites the held index and recovery becomes
	 * node 1 on slot[0], and every other exit is giving up. */
	bootstrap_release(sbi);

	if (ret != -EAGAIN) {
		pr_err("bootstrap: wait_for_format error: %d\n", ret);
		return ret;
	}

	ret = bootstrap_steal_stuck_slot0(sbi);
	if (ret == -EAGAIN) {
		pr_info("bootstrap: steal lost, retrying mount\n");
		return -EAGAIN;
	}
	if (ret) {
		pr_err("bootstrap: steal failed: %d\n", ret);
		return ret;
	}

	/* Slot[0] is ours now, so this node formats it and takes node_id 1. */
	sbi->node_id = 1;
	region_clear_node_refs(sbi, sbi->node_id);
	return bootstrap_run_format(sbi);
}

/*
 * bootstrap_auto_mount - claim a slot, then format or wait, until this node holds one.
 *
 * Two things send it around again: a lost claim write race, and a lost race to steal a stuck
 * slot[0]. It ends on -EBUSY when every slot is held and live, or on a format error.
 */
s32 bootstrap_auto_mount(struct fs_sb_info *sbi)
{
	test_blank_slots(sbi);
	test_blank_device(sbi);

	s32 slot_idx = -1;

	for (;;) {
		s32 ret = bootstrap_claim(sbi, &slot_idx);
		if (ret == -EAGAIN) {
			pr_info("bootstrap: claim race lost, retrying\n");
			continue;
		}
		if (ret) {
			pr_err("bootstrap: claim failed: %d\n", ret);
			return ret;
		}

		sbi->node_id = slot_idx + 1;
		pr_info("bootstrap: node_id=%u (slot %d)\n", sbi->node_id, slot_idx);
		/* Nothing is open yet, so whatever a previous holder of this id left set is stale. */
		region_clear_node_refs(sbi, sbi->node_id);

		/* Slot[0] on a device already formatted needs nothing more: the claim published it. */
		if (slot_idx == 0 && format_is_needed(sbi)) {
			ret = bootstrap_run_formatter(sbi);
			if (ret)
				return ret;
		} else if (slot_idx != 0) {
			ret = bootstrap_run_joiner(sbi);
			if (ret == -EAGAIN)
				continue;
			if (ret)
				return ret;
		}

		return 0;
	}
}

/*
 * bootstrap_manual_mount - take slot[@node_id - 1] for an explicit node_id= mount.
 *
 * A slot this node does not end up holding is fatal, whether a live owner refused it or a peer won
 * the verify: with no slot there is no tick, and with no tick nothing ever fences this mount.
 * Anything else only warns, since an old image may carry no bootstrap area at all.
 */
s32 bootstrap_manual_mount(struct fs_sb_info *sbi, u32 node_id, bool want_format)
{
	if (want_format) {
		s32 formatted = format_device(sbi);
		if (formatted) {
			pr_err("in-kernel format failed\n");
			return formatted;
		}
	}

	s32 ret = bootstrap_claim_explicit(sbi, node_id);
	if (ret == -EBUSY) {
		pr_err("bootstrap: slot for node_id=%u already active\n", node_id);
		return ret;
	}
	if (ret == -EAGAIN) {
		pr_err("bootstrap: lost the claim for node_id=%u to a peer\n", node_id);
		return ret;
	}
	if (ret)
		pr_warn("bootstrap: explicit claim for node_id=%u: %d\n", node_id, ret);

	return 0;
}
