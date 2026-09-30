// SPDX-License-Identifier: GPL-2.0-only
/*
 * test_hooks.c - the module parameters a test writes, and the only file that reads them.
 *
 * Each starts at 0, which reads as "production's own value stands", so a build that keeps this
 * file but leaves the parameters alone behaves exactly like one built without it.
 */

#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#include "core.h"
#include "cxl/io.h"
#include "layout/layout_access.h"
#include "layout/rat.h"
#include "layout/superblock.h"
#include "test/test_hooks.h"
#include "uapi.h"

/* ── Timeouts a test shortens so a slow path finishes inside a test run ── */

static u32 bootstrap_format_timeout_ms;
module_param(bootstrap_format_timeout_ms, uint, 0600);
MODULE_PARM_DESC(bootstrap_format_timeout_ms,
		 "TEST: stuck-formatter timeout in ms (0 = the built-in default)");

u32 test_format_timeout_ms(u32 production_ms)
{
	u32 wanted = READ_ONCE(bootstrap_format_timeout_ms);

	return wanted ? wanted : production_ms;
}

static u32 daemon_timeout_ms;
module_param(daemon_timeout_ms, uint, 0600);
MODULE_PARM_DESC(daemon_timeout_ms,
		 "TEST: per-request upcall timeout in ms (0 = the built-in default)");

u32 test_daemon_timeout_ms(u32 production_ms)
{
	u32 wanted = READ_ONCE(daemon_timeout_ms);

	return wanted ? wanted : production_ms;
}

/* ── Fault injection ────────────────────────────────────────────────────── */

/* A module parameter rather than a sysfs attribute because the mount path reads it before sbi
 * exists, and a per-mount attribute would appear too late to matter. */
static u32 bootstrap_inject_stuck_formatter;
module_param(bootstrap_inject_stuck_formatter, uint, 0600);
MODULE_PARM_DESC(bootstrap_inject_stuck_formatter,
		 "TEST: non-zero leaves slot[0] held with no layout behind it, so a joiner steals it");

bool test_should_inject_stuck_formatter(void)
{
	return READ_ONCE(bootstrap_inject_stuck_formatter) != 0;
}

/* The only way a test reaches the formatter election at all. Nothing in user space can clear the
 * superblock: the module maps the device itself, and device_dax, which owns the /dev node, has to be
 * unbound for that mapping to be uncached.
 *
 * It blanks the layout rather than answering a question differently, because a mount asks that
 * question in two places and only the medium can make both answers agree. */
static u32 bootstrap_inject_blank_device;
module_param(bootstrap_inject_blank_device, uint, 0600);
MODULE_PARM_DESC(bootstrap_inject_blank_device,
		 "TEST: non-zero makes the next mount clear the layout before it reads it");

void test_blank_device(struct fs_sb_info *sbi)
{
	if (!READ_ONCE(bootstrap_inject_blank_device))
		return;

	/* The superblock is where both the format decision and the joiner's wait look, so clearing
	 * it is what makes a device read as blank to both at once. */
	if (sbi->meta_base) {
		cxl_zero_cachelines(sbi->meta_base, LAYOUT_SUPERBLOCK_SIZE);
		pr_info("test: layout cleared, so the next mount reads the device as blank\n");
	}
}

/* One shot at the next mount: the slot table is zeroed before that mount claims, so a slot a stale
 * holder left behind is not one the case has to wait for the admin to recover. A format leaves the
 * table alone by design, since claims precede it. */
static u32 bootstrap_inject_blank_slots;
module_param(bootstrap_inject_blank_slots, uint, 0600);
MODULE_PARM_DESC(bootstrap_inject_blank_slots,
		 "TEST: non-zero makes the next mount zero the bootstrap slot table before it claims");

void test_blank_slots(struct fs_sb_info *sbi)
{
	if (!READ_ONCE(bootstrap_inject_blank_slots))
		return;
	WRITE_ONCE(bootstrap_inject_blank_slots, 0);

	if (sbi->meta_base) {
		cxl_zero_cachelines((char *)sbi->meta_base + LAYOUT_BOOTSTRAP_AREA_OFFSET, LAYOUT_BOOTSTRAP_AREA_SIZE);
		pr_info("test: slot table zeroed, so this mount claims on a table nobody holds\n");
	}
}

/* Widens the window between the free-slot scan and the claim write, so two mounters observe the same
 * free slot and the last-writer-wins resolution actually runs. */
static u32 bootstrap_debug_pre_write_delay_us;
module_param(bootstrap_debug_pre_write_delay_us, uint, 0600);
MODULE_PARM_DESC(bootstrap_debug_pre_write_delay_us,
		 "TEST: delay in us before the claim write (0 = none)");

void test_delay_before_claim_write(void)
{
	u32 delay_us = READ_ONCE(bootstrap_debug_pre_write_delay_us);

	if (delay_us)
		usleep_range(delay_us, delay_us + 100);
}

/* Widens the window between a prepare seeing a reference and its refusal, so a second prepare can
 * arrive after the file is closed and before the first one returns. */
static u32 unmount_prepare_refusal_delay_ms;
module_param(unmount_prepare_refusal_delay_ms, uint, 0600);
MODULE_PARM_DESC(unmount_prepare_refusal_delay_ms,
		 "TEST: delay in ms before unmount_prepare refuses with -EBUSY (0 = none)");

void test_delay_before_drain_refusal(void)
{
	u32 delay_ms = READ_ONCE(unmount_prepare_refusal_delay_ms);

	if (delay_ms)
		msleep(delay_ms);
}

/*
 * A frozen node stops stamping its slot, so peers see a stalled heartbeat and take the
 * crashed-owner path. Kept by node_id and not per-mount, so a host running several mounts can
 * freeze one node without killing the peers beside it.
 */
static unsigned long frozen_nodes;

/* One shot: the next write that reaches the named point dies there, and the knob reads 0 again so
 * the same mount cannot die twice. Numbered as enum test_crash_point. */
static u32 region_inject_crash_point;
module_param(region_inject_crash_point, uint, 0600);
MODULE_PARM_DESC(region_inject_crash_point,
		 "TEST: the metadata write that reaches this point dies there and the node stays dead");

bool test_crash_here(struct fs_sb_info *sbi, enum test_crash_point point)
{
	if (point == TEST_CRASH_NONE || READ_ONCE(region_inject_crash_point) != point)
		return false;

	WRITE_ONCE(region_inject_crash_point, TEST_CRASH_NONE);
	/* Dead before the heartbeat stops, so no path slips a write in between the two. */
	WRITE_ONCE(sbi->test_dead, true);
	test_set_tick_frozen(sbi->node_id, true);
	pr_warn("test: node %u died at crash point %u; it writes nothing more\n", sbi->node_id, point);
	return true;
}

/* One shot at the next mount: the RAT summary line is zeroed before that mount checks it, which is
 * the line a device formatted before the summary existed shows. */
static u32 region_inject_blank_summary;
module_param(region_inject_blank_summary, uint, 0600);
MODULE_PARM_DESC(region_inject_blank_summary,
		 "TEST: non-zero makes the next mount zero the RAT summary line before reading it");

void test_blank_summary(struct fs_sb_info *sbi)
{
	if (!READ_ONCE(region_inject_blank_summary))
		return;
	WRITE_ONCE(region_inject_blank_summary, 0);

	struct layout_rat *rat = layout_get_rat(sbi);
	if (!rat)
		return;
	cxl_zero_cachelines(&rat->map, sizeof(rat->map));
	pr_info("test: RAT summary line zeroed, so this mount reads it as stale\n");
}

bool test_tick_is_frozen(u32 node_id)
{
	if (node_id >= BITS_PER_LONG)
		return false;
	return test_bit(node_id, &frozen_nodes);
}

void test_set_tick_frozen(u32 node_id, bool frozen)
{
	if (node_id >= BITS_PER_LONG)
		return;
	if (frozen)
		set_bit(node_id, &frozen_nodes);
	else
		clear_bit(node_id, &frozen_nodes);
}

/*
 * Kept by node_id, mirroring the tick freeze: a host running several mounts can mis-stamp one
 * node's heartbeat without touching its peers. The offset rides on the real clock rather than
 * replacing it, so the stamp still advances and only a peer's movement-based read tells this node
 * apart from a frozen one.
 */
static s64 stamp_offset_ns[FS_MAX_NODE_ID + 1];

u64 test_tick_stamp(u32 node_id, u64 real_now_ns)
{
	if (node_id > FS_MAX_NODE_ID)
		return real_now_ns;
	return (u64)((s64)real_now_ns + READ_ONCE(stamp_offset_ns[node_id]));
}

void test_set_stamp_offset(u32 node_id, s64 offset_ns)
{
	if (node_id > FS_MAX_NODE_ID)
		return;
	WRITE_ONCE(stamp_offset_ns[node_id], offset_ns);
}

s64 test_get_stamp_offset(u32 node_id)
{
	if (node_id > FS_MAX_NODE_ID)
		return 0;
	return READ_ONCE(stamp_offset_ns[node_id]);
}

static unsigned long daemon_stubbed_nodes;

bool test_daemon_is_stubbed(u32 node_id)
{
	if (node_id >= BITS_PER_LONG)
		return false;
	return test_bit(node_id, &daemon_stubbed_nodes);
}

void test_set_daemon_stubbed(u32 node_id, bool stubbed)
{
	if (node_id >= BITS_PER_LONG)
		return;
	if (stubbed)
		set_bit(node_id, &daemon_stubbed_nodes);
	else
		clear_bit(node_id, &daemon_stubbed_nodes);
}

/*
 * Kept by node_id and not per-mount, so a count survives the mount that made it and a host running
 * several mounts reads one node without disturbing its peers. Atomic because every metadata writer
 * on that node bumps one of these, and they are read without any lock.
 */
static atomic64_t meta_turns[FS_MAX_NODE_ID + 1][TEST_META_TURN_KINDS];

void test_count_meta_turn(u32 node_id, enum test_meta_turn outcome)
{
	if (node_id > FS_MAX_NODE_ID || outcome >= TEST_META_TURN_KINDS)
		return;
	atomic64_inc(&meta_turns[node_id][outcome]);
}

void test_get_meta_turns(u32 node_id, struct test_meta_turns *out)
{
	memset(out, 0, sizeof(*out));
	if (node_id > FS_MAX_NODE_ID)
		return;
	for (u32 kind = 0; kind < TEST_META_TURN_KINDS; kind++)
		out->counts[kind] = (u64)atomic64_read(&meta_turns[node_id][kind]);
}

/* ── What one round trip to a helper spends, segment by segment ── */

/*
 * A request is tracked by its sequence id, direct-mapped, because the slot it travels on belongs to
 * production and carries no room for stamps. Two channels number their requests independently, so a
 * second request can land on a tracked slot: every stamp re-checks the id it finds there and the
 * loser is counted as dropped rather than folded into another request's spans.
 */
#define UPCALL_TRACKS 1024

struct upcall_track {
	u64 seq;
	enum test_upcall_kind kind;
	bool live;
	u64 stamps[TEST_UPCALL_POINTS];
};

/* One lock for the table and the totals both: every metadata writer on the host takes it six times
 * per round trip, and a second lock would buy nothing against a hold of a few writes. */
static DEFINE_SPINLOCK(upcall_lock);
static struct upcall_track upcall_tracks[UPCALL_TRACKS];
static struct test_upcall_total upcall_totals[TEST_UPCALL_KINDS][TEST_UPCALL_SPANS];
static u64 upcall_dropped;

/* Where each span starts and ends, in the order enum test_upcall_span names them. */
static const enum test_upcall_point upcall_span_from[TEST_UPCALL_SPANS] = {
	TEST_UPCALL_QUEUED,
	TEST_UPCALL_READ_WOKE,
	TEST_UPCALL_READ_DONE,
	TEST_UPCALL_ANSWERED,
	TEST_UPCALL_MATCHED,
	TEST_UPCALL_QUEUED,
};
static const enum test_upcall_point upcall_span_to[TEST_UPCALL_SPANS] = {
	TEST_UPCALL_READ_WOKE,
	TEST_UPCALL_READ_DONE,
	TEST_UPCALL_ANSWERED,
	TEST_UPCALL_MATCHED,
	TEST_UPCALL_RESUMED,
	TEST_UPCALL_RESUMED,
};

/* Caller holds upcall_lock. A span with either end unstamped is left out, which is how a request
 * the helper never read stays out of the totals. */
static void upcall_fold_locked(const struct upcall_track *track)
{
	for (u32 span = 0; span < TEST_UPCALL_SPANS; span++) {
		const u64 began = track->stamps[upcall_span_from[span]];
		const u64 ended = track->stamps[upcall_span_to[span]];
		if (!began || ended < began)
			continue;

		struct test_upcall_total *total = &upcall_totals[track->kind][span];
		const u64 took_ns = ended - began;
		total->count++;
		total->total_ns += took_ns;
		if (took_ns > total->worst_ns)
			total->worst_ns = took_ns;
	}
}

void test_start_upcall(u64 seq, enum test_upcall_kind kind, u64 when_ns)
{
	if (kind >= TEST_UPCALL_KINDS)
		return;

	unsigned long flags;
	spin_lock_irqsave(&upcall_lock, flags);
	struct upcall_track *track = &upcall_tracks[seq % UPCALL_TRACKS];
	if (track->live)
		upcall_dropped++;
	memset(track->stamps, 0, sizeof(track->stamps));
	track->seq = seq;
	track->kind = kind;
	track->live = true;
	track->stamps[TEST_UPCALL_QUEUED] = when_ns;
	spin_unlock_irqrestore(&upcall_lock, flags);
}

void test_stamp_upcall(u64 seq, enum test_upcall_point point, u64 when_ns)
{
	if (point >= TEST_UPCALL_POINTS)
		return;

	unsigned long flags;
	spin_lock_irqsave(&upcall_lock, flags);
	struct upcall_track *track = &upcall_tracks[seq % UPCALL_TRACKS];
	if (track->live && track->seq == seq) {
		track->stamps[point] = when_ns;
		/* The caller running again is the last point, so this is where the trip is complete
		 * and the slot is free for the next request that maps to it. */
		if (point == TEST_UPCALL_RESUMED) {
			upcall_fold_locked(track);
			track->live = false;
		}
	} else {
		upcall_dropped++;
	}
	spin_unlock_irqrestore(&upcall_lock, flags);
}

/* ── What a metadata writer spends, stage by stage ── */

/*
 * Kept by node_id for the reason the turn counter beside it is: a probe on one mount reads its own
 * node without a sweep on another mount landing in the same total. One lock rather than atomics,
 * because a fold writes fifteen words and a reader takes fifteen more.
 */
static DEFINE_SPINLOCK(meta_stage_lock);
static struct test_upcall_total meta_stages[FS_MAX_NODE_ID + 1][TEST_META_OP_KINDS][TEST_META_STAGES];

static void meta_fold_one_locked(u32 node_id, enum test_meta_op op, enum test_meta_stage stage,
				 u64 began, u64 ended)
{
	if (!began || !ended || ended < began)
		return;

	struct test_upcall_total *total = &meta_stages[node_id][op][stage];
	const u64 took_ns = ended - began;
	total->count++;
	total->total_ns += took_ns;
	if (took_ns > total->worst_ns)
		total->worst_ns = took_ns;
}

void test_fold_meta_stages(u32 node_id, enum test_meta_op op, u64 began_ns, u64 after_mutex_ns,
			   u64 after_lock_ns, u64 before_unlock_ns, u64 ended_ns)
{
	if (node_id > FS_MAX_NODE_ID || op >= TEST_META_OP_KINDS)
		return;

	unsigned long flags;
	spin_lock_irqsave(&meta_stage_lock, flags);
	meta_fold_one_locked(node_id, op, TEST_META_STAGE_MUTEX, began_ns, after_mutex_ns);
	meta_fold_one_locked(node_id, op, TEST_META_STAGE_LOCK, after_mutex_ns, after_lock_ns);
	meta_fold_one_locked(node_id, op, TEST_META_STAGE_WORK, after_lock_ns, before_unlock_ns);
	meta_fold_one_locked(node_id, op, TEST_META_STAGE_UNLOCK, before_unlock_ns, ended_ns);
	meta_fold_one_locked(node_id, op, TEST_META_STAGE_WHOLE, began_ns, ended_ns);
	spin_unlock_irqrestore(&meta_stage_lock, flags);
}

void test_get_meta_stages(u32 node_id, enum test_meta_op op, struct test_meta_stages *out)
{
	memset(out, 0, sizeof(*out));
	if (node_id > FS_MAX_NODE_ID || op >= TEST_META_OP_KINDS)
		return;

	unsigned long flags;
	spin_lock_irqsave(&meta_stage_lock, flags);
	for (u32 stage = 0; stage < TEST_META_STAGES; stage++)
		out->stages[stage] = meta_stages[node_id][op][stage];
	spin_unlock_irqrestore(&meta_stage_lock, flags);
}

void test_get_upcall_spans(enum test_upcall_kind kind, struct test_upcall_spans *out)
{
	memset(out, 0, sizeof(*out));
	if (kind >= TEST_UPCALL_KINDS)
		return;

	unsigned long flags;
	spin_lock_irqsave(&upcall_lock, flags);
	for (u32 span = 0; span < TEST_UPCALL_SPANS; span++)
		out->spans[span] = upcall_totals[kind][span];
	out->dropped = upcall_dropped;
	spin_unlock_irqrestore(&upcall_lock, flags);
}
