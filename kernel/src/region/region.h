/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * region.h - RAT (Region Allocation Table) and region init entry points.
 */

#ifndef _REGION_H
#define _REGION_H

#include <linux/types.h>

#include "daemon_uapi.h" /* struct fs_daemon_attest_response */

struct fs_sb_info;
struct layout_rat_entry;

/* One allocated extent, as the gap search sorts them. */
struct region_span {
	u64 offset;
	u64 end;
};

/* The gap search runs under meta_lock, so its scratch is the mount's rather than the call's. */
s32 region_alloc_scratch(struct fs_sb_info *sbi);
void region_free_scratch(struct fs_sb_info *sbi);

/* The first free slot as the RAT's summary reads now, or LAYOUT_MAX_RAT_ENTRIES when none is. Needs
 * no turn: it is a hint for region_alloc_rat_entry, which confirms it under the lock. */
u32 region_find_free_rat_entry(struct fs_sb_info *sbi);

/* Take a free slot under meta_lock. @hint is tried first when it still reads free, then the
 * summary's other free bits, and a scan of every entry is the fallback. */
s32 region_alloc_rat_entry(struct fs_sb_info *sbi, const char *name, u64 size, u64 offset, u32 hint, umode_t mode,
			   const struct fs_daemon_attest_response *identity, u32 *out_rat_entry_id);
bool region_free_rat_entry(struct fs_sb_info *sbi, struct layout_rat_entry *entry);

/* Bring the mount's extent list up to the RAT's placement count, reading entries only when that
 * count moved. Needs no turn, only the mutex half: region_init_locked confirms the list against
 * the count once the turn is held, so what a peer changed meanwhile is read again there. */
void region_survey_space(struct fs_sb_info *sbi, bool uncached);

/* True when the summary disagrees with @seen, the taken bits a walk over the entries collected, or
 * its placement count reads odd. Needs no turn, so a peer mid-operation can answer true; the repair
 * judges again under the turn. */
bool region_map_needs_repair(struct fs_sb_info *sbi, const union layout_rat_map_copy *seen);

/* Read every entry once and set whether this mount may search by the summary's bits. Until the
 * summary agrees with the entries, the extent list is built from every entry, so a device formatted
 * before the summary existed places nothing over an extent its bits do not show. */
void region_check_map(struct fs_sb_info *sbi);

/* Under the turn: rewrite the summary as the entries say it should read. The allocator repairs a
 * bit that reads free over a taken entry on its own, so what this catches is a bit left set over a
 * free one and a count a placer's death left odd. Returns how many bits moved, plus one for the
 * count, or -EIO. */
s32 region_repair_map(struct fs_sb_info *sbi);

/* What each pool holds, so a caller reports the two separately rather than one total that hides
 * a full pool beside an empty one. Bytes, and @entries counts the RAT rows behind them. */
struct region_pool_usage {
	u64 uc_total;
	u64 uc_used;
	u64 wb_total;
	u64 wb_used;
	u32 entries;
};

s32 region_get_pool_usage(struct fs_sb_info *sbi, struct region_pool_usage *out);

/* Region initialization (called from ftruncate path).
 *
 * @uncached takes the pool the device maps uncached, where a mapping carries no struct pages and
 * so cannot be pinned for DMA. */
s32 region_init_locked(struct fs_sb_info *sbi, u32 rat_entry_id, u64 data_size, bool uncached);

#endif /* _REGION_H */
