// SPDX-License-Identifier: GPL-2.0-only
/*
 * test_sysfs.c - the attributes only a test reaches.
 *
 * Each one asks the module to misbehave or to expose what no interface exposes, which is why they
 * are not in the debug group.
 *
 * The tick freeze is keyed on a node_id and so belongs to the module's own directory. The rest are
 * one mount's own state, so they hang under that mount instead.
 */

#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/mutex.h>
#include <linux/sysfs.h>

#include "core.h"
#include "layout/bootstrap.h"
#include "layout/layout_access.h"
#include "layout/rat.h"
#include "sysfs/sysfs_internal.h"
#include "test/test_hooks.h"
#include "test/test_sysfs.h"

/*
 * Takes "<node_id> <0|1>" and makes that node's bootstrap tick a no-op, so peers see a stalled
 * heartbeat. Scoped by node_id rather than a module-wide flag, so a local multi-mount setup can
 * freeze one node and leave its peers ticking.
 */
static ssize_t freeze_heartbeat_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	s32 n = 0;

	mutex_lock(&sysfs_lock);
	for (u32 i = 0; i < SYSFS_MAX_MOUNTS; i++) {
		struct fs_sb_info *sbi = sysfs_sbi_list[i];
		if (!sbi)
			continue;

		n += scnprintf(buf + n, PAGE_SIZE - n, "node=%u frozen=%d\n", sbi->node_id,
			       test_tick_is_frozen(sbi->node_id));
	}
	mutex_unlock(&sysfs_lock);
	return n;
}

static ssize_t freeze_heartbeat_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	u32 node_id, val;
	if (sscanf(buf, "%u %u", &node_id, &val) != 2)
		return -EINVAL;

	mutex_lock(&sysfs_lock);
	for (u32 i = 0; i < SYSFS_MAX_MOUNTS; i++) {
		struct fs_sb_info *sbi = sysfs_sbi_list[i];
		if (!sbi || sbi->node_id != node_id)
			continue;

		test_set_tick_frozen(sbi->node_id, val != 0);
	}
	mutex_unlock(&sysfs_lock);
	return count;
}

static struct kobj_attribute freeze_heartbeat_attr =
	__ATTR(freeze_heartbeat, 0600, freeze_heartbeat_show, freeze_heartbeat_store);

/*
 * Takes "<node_id> <offset_ns>", a signed nanosecond offset added to that node's tick instead of
 * freezing it. The stamp still moves with the real clock, so it is a peer's wall-clock check that
 * this fails and not its movement fallback.
 */
static ssize_t stamp_offset_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	s32 n = 0;

	mutex_lock(&sysfs_lock);
	for (u32 i = 0; i < SYSFS_MAX_MOUNTS; i++) {
		struct fs_sb_info *sbi = sysfs_sbi_list[i];
		if (!sbi)
			continue;

		n += scnprintf(buf + n, PAGE_SIZE - n, "node=%u offset_ns=%lld\n", sbi->node_id,
			       test_get_stamp_offset(sbi->node_id));
	}
	mutex_unlock(&sysfs_lock);
	return n;
}

static ssize_t stamp_offset_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	u32 node_id;
	s64 offset_ns;
	if (sscanf(buf, "%u %lld", &node_id, &offset_ns) != 2)
		return -EINVAL;

	mutex_lock(&sysfs_lock);
	for (u32 i = 0; i < SYSFS_MAX_MOUNTS; i++) {
		struct fs_sb_info *sbi = sysfs_sbi_list[i];
		if (!sbi || sbi->node_id != node_id)
			continue;

		test_set_stamp_offset(sbi->node_id, offset_ns);
	}
	mutex_unlock(&sysfs_lock);
	return count;
}

static struct kobj_attribute stamp_offset_attr = __ATTR(stamp_offset, 0600, stamp_offset_show, stamp_offset_store);

/* Takes "<node_id> <0|1>": a create on that node takes a fixed identity instead of asking the
 * daemon. For a mount brought up with no daemon, which is what the lifecycle cases run on. */
static ssize_t daemon_stub_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	s32 n = 0;

	mutex_lock(&sysfs_lock);
	for (u32 i = 0; i < SYSFS_MAX_MOUNTS; i++) {
		struct fs_sb_info *sbi = sysfs_sbi_list[i];
		if (!sbi)
			continue;

		n += scnprintf(buf + n, PAGE_SIZE - n, "node=%u stubbed=%d\n", sbi->node_id,
			       test_daemon_is_stubbed(sbi->node_id));
	}
	mutex_unlock(&sysfs_lock);
	return n;
}

static ssize_t daemon_stub_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	u32 node_id, val;
	if (sscanf(buf, "%u %u", &node_id, &val) != 2)
		return -EINVAL;

	mutex_lock(&sysfs_lock);
	for (u32 i = 0; i < SYSFS_MAX_MOUNTS; i++) {
		struct fs_sb_info *sbi = sysfs_sbi_list[i];
		if (!sbi || sbi->node_id != node_id)
			continue;

		test_set_daemon_stubbed(sbi->node_id, val != 0);
	}
	mutex_unlock(&sysfs_lock);
	return count;
}

static struct kobj_attribute daemon_stub_attr = __ATTR(daemon_stub, 0600, daemon_stub_show, daemon_stub_store);

/*
 * bootstrap_dump - read-only: the slot table as this mount's claim scan would read it.
 *
 * One mount per file, which is what keeps eight slots inside the single page a show is given. The
 * sbi comes from the directory rather than from the list, so no lock is taken.
 */
static ssize_t bootstrap_dump_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct fs_sb_info *sbi = sysfs_sbi_of(kobj);
	s32 n = 0;

	if (!sbi->bootstrap_slots)
		return scnprintf(buf, PAGE_SIZE, "(bootstrap not initialized)\n");

	for (s32 idx = 0; idx < LAYOUT_BOOTSTRAP_MAX_SLOTS && n < PAGE_SIZE - 1; idx++) {
		union bootstrap_slot_copy seen;
		cxl_get_bootstrap_slot(&seen, &sbi->bootstrap_slots[idx]);

		n += scnprintf(buf + n, PAGE_SIZE - n,
			       "slot[%d] node_id=%d magic=0x%08x state=%u token=0x%016llx heartbeat=%llu recoverer=%u%s\n",
			       idx, idx + 1, seen.local.magic, seen.local.state, seen.local.token,
			       seen.local.heartbeat, seen.local.recoverer,
			       (idx == sbi->bootstrap_slot_idx) ? " <mine>" : "");
	}
	return n;
}

static struct kobj_attribute bootstrap_dump_attr = __ATTR(bootstrap_dump, 0400, bootstrap_dump_show, NULL);

/*
 * meta_lock - read-only: what became of the cross-node half of this mount's metadata lock.
 *
 * A case asserts on the delta across one operation, so it can tell a path that took the turn from
 * one that was refused it. @refused rising instead of @taken is a mount with no helper.
 */
static ssize_t meta_lock_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct test_meta_turns turns;

	test_get_meta_turns(sysfs_sbi_of(kobj)->node_id, &turns);
	return sysfs_emit(buf, "taken=%llu refused=%llu\n",
			  turns.counts[TEST_META_TURN_TAKEN], turns.counts[TEST_META_TURN_REFUSED]);
}

static struct kobj_attribute meta_lock_attr = __ATTR_RO(meta_lock);

/*
 * meta_stages - read-only: where one metadata write on this mount spends its time.
 *
 * One row per operation and stage, carrying the count and the summed nanoseconds so a reader takes
 * its own delta and divides. WORK is the only stage no upcall accounts for: it is what the caller
 * does with the turn held.
 */
static const char *const meta_op_names[TEST_META_OP_KINDS] = {
	"create",
	"place",
	"unlink",
	"perm",
	"map",
	"sweep",
};

static const char *const meta_stage_names[TEST_META_STAGES] = {
	"mutex",
	"lock",
	"work",
	"unlock",
	"whole",
};

static ssize_t meta_stages_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	const u32 node_id = sysfs_sbi_of(kobj)->node_id;
	s32 len = 0;

	for (u32 op = 0; op < TEST_META_OP_KINDS; op++) {
		struct test_meta_stages stages;

		test_get_meta_stages(node_id, op, &stages);
		for (u32 stage = 0; stage < TEST_META_STAGES; stage++) {
			len += sysfs_emit_at(buf, len, "%s %s %llu %llu %llu\n", meta_op_names[op],
					     meta_stage_names[stage], stages.stages[stage].count,
					     stages.stages[stage].total_ns,
					     stages.stages[stage].worst_ns);
		}
	}
	return len;
}

static struct kobj_attribute meta_stages_attr = __ATTR_RO(meta_stages);

/*
 * rat_map - read-only: the RAT's summary line as this mount reads it, beside what its entries say.
 *
 * A crash case leaves the two apart on purpose and then watches the sweep bring them back, so both
 * are printed from one read each: the line as it stands, and the taken bits a walk over the
 * entries finds now.
 */
static ssize_t rat_map_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct fs_sb_info *sbi = sysfs_sbi_of(kobj);
	struct layout_rat *rat = layout_get_rat(sbi);
	if (!rat)
		return sysfs_emit(buf, "no RAT\n");

	union layout_rat_map_copy map;
	cxl_get_layout_rat_map(&map, &rat->map);

	union layout_rat_map_copy seen = {};
	for (u32 idx = 0; idx < LAYOUT_MAX_RAT_ENTRIES; idx++) {
		union layout_rat_hot_copy hot;
		cxl_get_layout_rat_hot(&hot, &rat->entries[idx].hot);
		if (hot.local.state != LAYOUT_RAT_ENTRY_FREE)
			layout_rat_map_mark(&seen, idx, true);
	}

	s32 len = sysfs_emit(buf, "placements=%llu trusted=%u\n", map.local.placements, sbi->rat_map_trusted ? 1 : 0);
	for (u32 word = 0; word < LAYOUT_RAT_MAP_WORDS; word++)
		len += sysfs_emit_at(buf, len, "word%u line=%016llx entries=%016llx\n", word, map.local.taken[word],
				     seen.local.taken[word]);
	return len;
}

static struct kobj_attribute rat_map_attr = __ATTR_RO(rat_map);

static struct attribute *test_mount_attrs[] = {
	&bootstrap_dump_attr.attr,
	&meta_lock_attr.attr,
	&meta_stages_attr.attr,
	&rat_map_attr.attr,
	NULL,
};

/* Named, so a mount's directory says which of its files only a test reads. */
static const struct attribute_group test_mount_attr_group = {
	.name = "test",
	.attrs = test_mount_attrs,
};

s32 test_create_mount_attrs(struct kobject *kobj)
{
	return sysfs_create_group(kobj, &test_mount_attr_group);
}

void test_delete_mount_attrs(struct kobject *kobj)
{
	sysfs_remove_group(kobj, &test_mount_attr_group);
}

/*
 * upcall_latency - read-only: what one round trip to a helper spends, segment by segment.
 *
 * One row per request kind and span, carrying the count and the summed nanoseconds rather than a
 * mean, so the reader takes a delta across its own probe and divides itself. Module-wide, because
 * the channel a request travels on has no back-pointer to its mount.
 */
static const char *const upcall_kind_names[TEST_UPCALL_KINDS] = {
	"access",
	"attest",
	"lock",
	"unlock",
};

static const char *const upcall_span_names[TEST_UPCALL_SPANS] = {
	"helper_wake",
	"read_copy",
	"helper_turnaround",
	"response_match",
	"waiter_wake",
	"whole",
};

static ssize_t upcall_latency_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	s32 len = 0;
	u64 dropped = 0;

	for (u32 kind = 0; kind < TEST_UPCALL_KINDS; kind++) {
		struct test_upcall_spans spans;
		test_get_upcall_spans(kind, &spans);
		dropped = spans.dropped;
		for (u32 span = 0; span < TEST_UPCALL_SPANS; span++) {
			len += sysfs_emit_at(buf, len, "%s %s %llu %llu %llu\n", upcall_kind_names[kind],
					     upcall_span_names[span], spans.spans[span].count,
					     spans.spans[span].total_ns, spans.spans[span].worst_ns);
		}
	}
	len += sysfs_emit_at(buf, len, "dropped %llu\n", dropped);
	return len;
}

static struct kobj_attribute upcall_latency_attr = __ATTR_RO(upcall_latency);

static struct attribute *test_sysfs_attrs[] = {
	&freeze_heartbeat_attr.attr,
	&stamp_offset_attr.attr,
	&daemon_stub_attr.attr,
	&upcall_latency_attr.attr,
	NULL,
};

/* Its own directory, so the operator attributes next door stay a separate build switch. */
static const struct attribute_group test_sysfs_attr_group = {
	.name = "test",
	.attrs = test_sysfs_attrs,
};

s32 test_create_sysfs_group(struct kobject *kobj)
{
	return sysfs_create_group(kobj, &test_sysfs_attr_group);
}

void test_delete_sysfs_group(struct kobject *kobj)
{
	sysfs_remove_group(kobj, &test_sysfs_attr_group);
}
