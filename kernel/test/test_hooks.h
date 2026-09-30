/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * test_hooks.h - every knob only a test turns, behind one door.
 *
 * A hook hands back the value production would use unless a test wrote the matching module
 * parameter. Without CONFIG_FS_TEST_KNOBS test/ is not compiled and every call folds away.
 */

#ifndef _TEST_HOOKS_H
#define _TEST_HOOKS_H

#include <linux/string.h>
#include <linux/types.h>

struct fs_sb_info;

/* What became of the cross-node half of a metadata lock. Production keeps none of this, so a case
 * asserting that a path took the turn is the only reader. There is no third outcome: a writer that
 * cannot reach the peers is refused rather than let through on the mutex alone. */
enum test_meta_turn {
	TEST_META_TURN_TAKEN,
	TEST_META_TURN_REFUSED,
	TEST_META_TURN_KINDS,
};

struct test_meta_turns {
	u64 counts[TEST_META_TURN_KINDS];
};

/* Where a metadata write can be made to die between two of its lines, numbered as the knob names
 * them. Dying means the node writes nothing more, releases nothing it holds and stops its
 * heartbeat, which is what a peer sees of a node that lost power there. */
enum test_crash_point {
	TEST_CRASH_NONE = 0,
	TEST_CRASH_ALLOC_BEFORE_MARK = 1, /* entry taken, its summary bit still clear */
	TEST_CRASH_PLACE_BEFORE_EXTENT = 2, /* count odd, extent not written */
	TEST_CRASH_PLACE_BEFORE_EVEN = 3, /* extent written, count still odd */
	TEST_CRASH_FREE_BEFORE_UNMARK = 4, /* entry FREE, its summary bit still set */
	TEST_CRASH_CREATE_BEFORE_NAME = 5, /* entry taken and marked, no name in the index */
	TEST_CRASH_UNLINK_BEFORE_FREE = 6, /* name out of the index, entry still taken */
	TEST_CRASH_ALLOC_WHILE_ALLOCATING = 7, /* entry ALLOCATING with nothing else written */
	TEST_CRASH_FREE_WHILE_DELETING = 8, /* entry DELETING with its lines still intact */
};

/* Which metadata operation a stage sample belongs to, so a placement can be priced apart from an
 * unlink that took the same one turn. */
enum test_meta_op {
	TEST_META_OP_CREATE,
	TEST_META_OP_PLACE,
	TEST_META_OP_UNLINK,
	TEST_META_OP_PERM,
	TEST_META_OP_MAP,
	TEST_META_OP_SWEEP,
	TEST_META_OP_KINDS,
};

/* The four spans a metadata writer spends, and the whole. WORK is what the caller does with the
 * turn held, which is the only one no upcall accounts for. */
enum test_meta_stage {
	TEST_META_STAGE_MUTEX,
	TEST_META_STAGE_LOCK,
	TEST_META_STAGE_WORK,
	TEST_META_STAGE_UNLOCK,
	TEST_META_STAGE_WHOLE,
	TEST_META_STAGES,
};

/* Which upcall a timing sample belongs to, so a LOCK can be priced apart from an ATTEST. */
enum test_upcall_kind {
	TEST_UPCALL_ACCESS,
	TEST_UPCALL_ATTEST,
	TEST_UPCALL_LOCK,
	TEST_UPCALL_UNLOCK,
	TEST_UPCALL_KINDS,
};

/* The points one request passes between the caller queueing it and the trip ending, which is the
 * caller running again, or for a request with no answer the helper having read it. Stamping each is
 * what turns a round trip into segments rather than one number. */
enum test_upcall_point {
	TEST_UPCALL_QUEUED,
	TEST_UPCALL_READ_WOKE,
	TEST_UPCALL_READ_DONE,
	TEST_UPCALL_ANSWERED,
	TEST_UPCALL_MATCHED,
	TEST_UPCALL_RESUMED,
	TEST_UPCALL_POINTS,
};

/* Each pair of neighbouring points, plus the whole trip. A span is folded in only once its
 * request came back, so one that timed out contributes nothing rather than five seconds. */
enum test_upcall_span {
	TEST_SPAN_HELPER_WAKE,
	TEST_SPAN_READ_COPY,
	TEST_SPAN_HELPER_TURNAROUND,
	TEST_SPAN_RESPONSE_MATCH,
	TEST_SPAN_WAITER_WAKE,
	TEST_SPAN_WHOLE,
	TEST_UPCALL_SPANS,
};

/* Sums rather than a mean, so the reader divides at whatever precision it wants. */
struct test_upcall_total {
	u64 count;
	u64 total_ns;
	u64 worst_ns;
};

struct test_upcall_spans {
	struct test_upcall_total spans[TEST_UPCALL_SPANS];
	/* Samples thrown away because another request had taken the same tracking slot. */
	u64 dropped;
};

struct test_meta_stages {
	struct test_upcall_total stages[TEST_META_STAGES];
};

#ifdef CONFIG_FS_TEST_KNOBS

u32 test_format_timeout_ms(u32 production_ms);
u32 test_daemon_timeout_ms(u32 production_ms);
bool test_should_inject_stuck_formatter(void);
void test_blank_device(struct fs_sb_info *sbi);
void test_blank_summary(struct fs_sb_info *sbi);
void test_blank_slots(struct fs_sb_info *sbi);
void test_delay_before_claim_write(void);
void test_delay_before_drain_refusal(void);
bool test_crash_here(struct fs_sb_info *sbi, enum test_crash_point point);
bool test_tick_is_frozen(u32 node_id);
void test_set_tick_frozen(u32 node_id, bool frozen);
/* The stamp a tick would write for @node_id, offset by whatever a test last set: 0 leaves
 * @real_now_ns untouched. The offset moves with the clock, so the peer verdict's movement fallback
 * still sees this node as alive while a wall-clock read of the same stamp would not. */
u64 test_tick_stamp(u32 node_id, u64 real_now_ns);
void test_set_stamp_offset(u32 node_id, s64 offset_ns);
s64 test_get_stamp_offset(u32 node_id);
/* A create on that node takes a fixed identity instead of asking the daemon, so a mount brought up
 * without one can still place regions. */
bool test_daemon_is_stubbed(u32 node_id);
void test_set_daemon_stubbed(u32 node_id, bool stubbed);
void test_count_meta_turn(u32 node_id, enum test_meta_turn outcome);
void test_get_meta_turns(u32 node_id, struct test_meta_turns *out);
void test_start_upcall(u64 seq, enum test_upcall_kind kind, u64 when_ns);
void test_stamp_upcall(u64 seq, enum test_upcall_point point, u64 when_ns);
void test_get_upcall_spans(enum test_upcall_kind kind, struct test_upcall_spans *out);
void test_fold_meta_stages(u32 node_id, enum test_meta_op op, u64 began_ns, u64 after_mutex_ns,
			   u64 after_lock_ns, u64 before_unlock_ns, u64 ended_ns);
void test_get_meta_stages(u32 node_id, enum test_meta_op op, struct test_meta_stages *out);

#else

static inline u32 test_format_timeout_ms(u32 production_ms)
{
	return production_ms;
}

static inline u32 test_daemon_timeout_ms(u32 production_ms)
{
	return production_ms;
}

/* Nothing to inject, so the formatter never pretends to hang. */
static inline bool test_should_inject_stuck_formatter(void)
{
	return false;
}

/* Nothing blanks it, so the layout a mount finds is the one that was there. */
static inline void test_blank_device(struct fs_sb_info *sbi)
{
	(void)sbi;
}

/* Nothing blanks it, so the summary a mount finds is the one its peers kept. */
static inline void test_blank_summary(struct fs_sb_info *sbi)
{
	(void)sbi;
}

/* Nothing blanks it, so every slot a mount finds held is held. */
static inline void test_blank_slots(struct fs_sb_info *sbi)
{
	(void)sbi;
}

/* No widened window, so a claim writes its slot as soon as it picks one. */
static inline void test_delay_before_claim_write(void)
{
}

/* No widened window, so a prepare refuses as soon as it sees a reference. */
static inline void test_delay_before_drain_refusal(void)
{
}

/* Nothing dies on purpose, so every write runs to its end. */
static inline bool test_crash_here(struct fs_sb_info *sbi, enum test_crash_point point)
{
	(void)sbi;
	(void)point;
	return false;
}

/* No node can be frozen, so every claimed slot keeps stamping. */
static inline bool test_tick_is_frozen(u32 node_id)
{
	(void)node_id;
	return false;
}

static inline void test_set_tick_frozen(u32 node_id, bool frozen)
{
	(void)node_id;
	(void)frozen;
}

/* No offset, so a tick always writes the real clock. */
static inline u64 test_tick_stamp(u32 node_id, u64 real_now_ns)
{
	(void)node_id;
	return real_now_ns;
}

static inline void test_set_stamp_offset(u32 node_id, s64 offset_ns)
{
	(void)node_id;
	(void)offset_ns;
}

static inline s64 test_get_stamp_offset(u32 node_id)
{
	(void)node_id;
	return 0;
}

/* No stub, so every create asks the daemon. */
static inline bool test_daemon_is_stubbed(u32 node_id)
{
	(void)node_id;
	return false;
}

static inline void test_set_daemon_stubbed(u32 node_id, bool stubbed)
{
	(void)node_id;
	(void)stubbed;
}

/* Nobody counts, so the lock path costs what it did before this file existed. */
static inline void test_count_meta_turn(u32 node_id, enum test_meta_turn outcome)
{
	(void)node_id;
	(void)outcome;
}

static inline void test_get_meta_turns(u32 node_id, struct test_meta_turns *out)
{
	(void)node_id;
	memset(out, 0, sizeof(*out));
}

/* Nobody stamps, so a round trip carries no clock reads of its own. */
static inline void test_start_upcall(u64 seq, enum test_upcall_kind kind, u64 when_ns)
{
	(void)seq;
	(void)kind;
	(void)when_ns;
}

static inline void test_stamp_upcall(u64 seq, enum test_upcall_point point, u64 when_ns)
{
	(void)seq;
	(void)point;
	(void)when_ns;
}

static inline void test_get_upcall_spans(enum test_upcall_kind kind, struct test_upcall_spans *out)
{
	(void)kind;
	memset(out, 0, sizeof(*out));
}

/* Nobody folds, so a writer takes its two locks with no clock read between them. */
static inline void test_fold_meta_stages(u32 node_id, enum test_meta_op op, u64 began_ns,
					 u64 after_mutex_ns, u64 after_lock_ns,
					 u64 before_unlock_ns, u64 ended_ns)
{
	(void)node_id;
	(void)op;
	(void)began_ns;
	(void)after_mutex_ns;
	(void)after_lock_ns;
	(void)before_unlock_ns;
	(void)ended_ns;
}

static inline void test_get_meta_stages(u32 node_id, enum test_meta_op op,
					struct test_meta_stages *out)
{
	(void)node_id;
	(void)op;
	memset(out, 0, sizeof(*out));
}

#endif /* CONFIG_FS_TEST_KNOBS */

#endif /* _TEST_HOOKS_H */
