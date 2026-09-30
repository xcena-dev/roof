// SPDX-License-Identifier: GPL-2.0-only
/*
 * index.c - the global name index.
 *
 * A bucket is a chain of 64-byte lines, and a name is one slot of one line holding the RAT id of
 * its region. The caller holds the turn, so one node mutates at a time and every write is a
 * plain one.
 *
 * Every write here leaves a state the next reader can act on, so a crash needs no repair pass.
 * A whole-line write is one 64-byte operation, which is what lets a name appear or vanish at
 * once. Growing a bucket takes two more, and each is either invisible or complete.
 */

#include <linux/bitmap.h>
#include <linux/string.h>
#include <linux/types.h>

#include "core.h"
#include "layout/hash.h"
#include "layout/layout_access.h"
#include "region/index.h"
#include "uapi.h"

/*
 * One walk of one bucket: what it was told to look for, and the single line it held on to.
 *
 * One line is enough because no operation wants two. An insert that finds the name returns
 * -EEXIST and never touches the empty slot, and a find or a delete has no use for one. So the
 * walk keeps the line carrying the name when there is one, and otherwise the first with room.
 *
 * @copy leads because its union is 64-byte aligned, so any field before it would only buy
 * padding.
 */
struct index_walk {
	union layout_index_link_copy copy; /* that line, as the walk read it */

	struct layout_index_link *addr; /* where it lives */
	struct layout_index_link *prior_addr; /* the line naming @addr, when one does */
	struct layout_index_link *last_addr; /* the chain's final line */

	const char *name; /* the name this walk went looking for */
	u32 namelen;

	u32 shard_id; /* which shard the name hashes to */
	u32 bucket_idx; /* which bucket of that shard */
	u32 pool_idx; /* @addr's pool index, LAYOUT_LINK_END for a bucket's own line */
	u32 slot; /* the slot of interest, LAYOUT_LINK_SLOTS when the bucket is full */
	u16 tag; /* the hash bits a slot carries beside its RAT id */
	bool found; /* whether @slot carries the name rather than standing empty */
};

/* A line naming nothing: what a pool line looks like before a bucket puts a name in it. */
static void index_clear_line(union layout_index_link_copy *copy)
{
	memset(copy, 0, sizeof(*copy));
	copy->local.next_link = LAYOUT_LINK_END;
	for (u32 slot = 0; slot < LAYOUT_LINK_SLOTS; slot++)
		copy->local.region_id[slot] = LAYOUT_LINK_SLOT_FREE;
}

static bool index_is_line_empty(const union layout_index_link_copy *copy)
{
	for (u32 slot = 0; slot < LAYOUT_LINK_SLOTS; slot++)
		if (copy->local.region_id[slot] != LAYOUT_LINK_SLOT_FREE)
			return false;
	return true;
}

/*
 * index_read_bucket - read one bucket end to end and keep the one line that matters.
 *
 * Also records the chain's last line, which growth writes, and the line before the one kept,
 * which a delete emptying a pool line writes. Both are addresses only: those paths are rare
 * enough to pay a read, and a second copy on the stack is not.
 */
static s32 index_read_bucket(struct fs_sb_info *sbi, struct index_walk *walk)
{
	struct layout_index_link *cur_addr = layout_get_shard_bucket(sbi, walk->shard_id, walk->bucket_idx);
	if (unlikely(!cur_addr))
		return -EIO;

	walk->slot = LAYOUT_LINK_SLOTS;

	union layout_index_link_copy cur;
	struct layout_index_link *prior_addr = NULL;
	u32 cur_pool_idx = LAYOUT_LINK_END;
	u32 steps = 0;

	for (;;) {
		cxl_get_layout_index_link(&cur, cur_addr);

		for (u32 slot = 0; slot < LAYOUT_LINK_SLOTS; slot++) {
			u32 region_id = cur.local.region_id[slot];
			bool carries_name = false;

			if (region_id != LAYOUT_LINK_SLOT_FREE) {
				/* The tag filters and the stored name judges, so a tag matching by
				 * chance costs one RAT read and changes no answer. */
				if (walk->found || cur.local.tag[slot] != walk->tag)
					continue;
				if (!layout_check_rat_name(sbi, region_id, walk->name, walk->namelen))
					continue;
				carries_name = true;
			} else if (walk->slot != LAYOUT_LINK_SLOTS) {
				continue; /* a line is already held, and one is all any caller wants */
			}

			walk->copy = cur;
			walk->addr = cur_addr;
			walk->pool_idx = cur_pool_idx;
			walk->prior_addr = prior_addr;
			walk->slot = slot;
			walk->found = carries_name;
		}

		u32 next = cur.local.next_link;
		if (next == LAYOUT_LINK_END)
			break;
		if (unlikely(++steps > sbi->pool_lines_per_shard)) {
			pr_err("bucket %u of shard %u links more lines than the shard has\n",
			       walk->bucket_idx, walk->shard_id);
			return -EIO;
		}

		struct layout_index_link *next_addr = layout_get_shard_pool_line(sbi, walk->shard_id, next);
		if (unlikely(!next_addr)) {
			pr_err("bucket %u of shard %u names pool line %u\n",
			       walk->bucket_idx, walk->shard_id, next);
			return -EIO;
		}

		prior_addr = cur_addr;
		cur_addr = next_addr;
		cur_pool_idx = next;
	}

	walk->last_addr = cur_addr;
	return 0;
}

/*
 * index_claim_pool_line - a line of @shard_id's pool that no bucket of that shard names.
 *
 * Reachability decides and content says nothing, because a line already linked into a bucket may
 * hold no name either. So only a walk of every bucket tells the two apart. The cost lands on
 * bucket growth alone, which the geometry keeps out of reach at this file count.
 */
static noinline s32 index_claim_pool_line(struct fs_sb_info *sbi, u32 shard_id, u32 *out_line_idx)
{
	DECLARE_BITMAP(taken, LAYOUT_POOL_LINES_PER_SHARD);
	bitmap_zero(taken, LAYOUT_POOL_LINES_PER_SHARD);

	for (u32 bucket_idx = 0; bucket_idx < sbi->buckets_per_shard; bucket_idx++) {
		struct layout_index_link *addr = layout_get_shard_bucket(sbi, shard_id, bucket_idx);
		if (unlikely(!addr))
			return -EIO;

		union layout_index_link_copy copy;
		u32 steps = 0;

		for (;;) {
			cxl_get_layout_index_link(&copy, addr);

			u32 next = copy.local.next_link;
			if (next == LAYOUT_LINK_END)
				break;
			if (unlikely(next >= sbi->pool_lines_per_shard ||
				     ++steps > sbi->pool_lines_per_shard))
				return -EIO;

			/* __ rather than set_bit: @taken is this frame's own, so a LOCK
			 * prefix would order it against nothing. */
			__set_bit(next, taken);
			addr = layout_get_shard_pool_line(sbi, shard_id, next);
			if (unlikely(!addr))
				return -EIO;
		}
	}

	u32 line_idx = find_first_zero_bit(taken, sbi->pool_lines_per_shard);
	if (unlikely(line_idx >= sbi->pool_lines_per_shard))
		return -ENOSPC;

	*out_line_idx = line_idx;
	return 0;
}

/*
 * index_link_line - give this bucket one more line and let @walk hold it instead.
 *
 * Two writes, and each leaves a state a reader can act on. Clearing the pool line is invisible
 * because nothing names it yet, and linking it adds a line carrying no name. A crash between the
 * two therefore loses this insert and nothing else.
 */
static noinline s32 index_link_line(struct fs_sb_info *sbi, struct index_walk *walk)
{
	u32 shard_id = walk->shard_id;
	u32 line_idx;
	s32 ret = index_claim_pool_line(sbi, shard_id, &line_idx);
	if (ret)
		return ret;

	struct layout_index_link *addr = layout_get_shard_pool_line(sbi, shard_id, line_idx);
	if (unlikely(!addr))
		return -EIO;

	union layout_index_link_copy line;
	index_clear_line(&line);
	cxl_set_layout_index_link(addr, &line);

	cxl_get_layout_index_link(&line, walk->last_addr);
	line.local.next_link = line_idx;
	cxl_set_layout_index_link(walk->last_addr, &line);

	walk->addr = addr;
	walk->pool_idx = line_idx;
	walk->prior_addr = walk->last_addr;
	walk->slot = 0;
	walk->found = false;
	index_clear_line(&walk->copy);
	return 0;
}

/* The line before @walk's, with its successor set to @next. Rare, so it reads before it writes. */
static noinline void index_unlink_line(struct index_walk *walk, u32 next)
{
	union layout_index_link_copy prior;

	cxl_get_layout_index_link(&prior, walk->prior_addr);
	prior.local.next_link = next;
	cxl_set_layout_index_link(walk->prior_addr, &prior);
}

/*
 * index_read_name - work out where @name lands, then read that bucket.
 *
 * Return: 0, -ENAMETOOLONG on a name this index cannot hold, -EIO on a shard that does not
 * describe itself. The two callers that answer -ENOENT for an absent name map the first.
 */
static s32 index_read_name(struct fs_sb_info *sbi, const char *name, u32 namelen, struct index_walk *walk)
{
	if (unlikely(!sbi || !name))
		return -EINVAL;
	if (namelen == 0 || namelen > FS_NAME_MAX)
		return -ENAMETOOLONG;
	if (unlikely(!layout_is_valid_shard_geometry(sbi->buckets_per_shard, sbi->pool_lines_per_shard))) {
		pr_err("shard geometry needs a reformat: buckets=%u pool=%u\n",
		       sbi->buckets_per_shard, sbi->pool_lines_per_shard);
		return -EIO;
	}

	u64 hash = hash_name(name, namelen);

	memset(walk, 0, sizeof(*walk));
	walk->name = name;
	walk->namelen = namelen;
	walk->shard_id = hash_shard_idx(hash, sbi->shard_mask);
	walk->bucket_idx = hash_bucket_idx(hash, sbi->bucket_mask);
	walk->tag = hash_name_tag(hash);

	return index_read_bucket(sbi, walk);
}

/*
 * index_insert - let @name resolve to the region carrying it.
 *
 * Return: 0, -EEXIST if the name is taken, -ENOSPC if the shard's pool is spent.
 */
s32 index_insert(struct fs_sb_info *sbi, const char *name, u32 namelen, u32 region_id)
{
	if (unlikely(region_id >= LAYOUT_MAX_RAT_ENTRIES))
		return -EINVAL;

	struct index_walk walk;
	s32 ret = index_read_name(sbi, name, namelen, &walk);
	if (ret)
		return ret;
	if (walk.found)
		return -EEXIST;
	if (walk.slot == LAYOUT_LINK_SLOTS) {
		ret = index_link_line(sbi, &walk);
		if (ret)
			return ret;
	}

	/* One write publishes the name. Either the line carries it or the line is as it was. */
	walk.copy.local.tag[walk.slot] = walk.tag;
	walk.copy.local.region_id[walk.slot] = (u16)region_id;
	cxl_set_layout_index_link(walk.addr, &walk.copy);

	return 0;
}

/*
 * index_find - the region carrying @name.
 *
 * Return: 0 with @out_region_id set, -ENOENT if no slot of the bucket carries the name.
 */
s32 index_find(struct fs_sb_info *sbi, const char *name, u32 namelen, u32 *out_region_id)
{
	if (unlikely(!out_region_id))
		return -EINVAL;

	struct index_walk walk;
	s32 ret = index_read_name(sbi, name, namelen, &walk);
	if (ret)
		return (ret == -ENAMETOOLONG) ? -ENOENT : ret;
	if (!walk.found)
		return -ENOENT;

	*out_region_id = walk.copy.local.region_id[walk.slot];

	return 0;
}

/*
 * index_delete - take @name out of the index.
 *
 * Return: 0, -ENOENT if no slot of the bucket carries the name.
 */
s32 index_delete(struct fs_sb_info *sbi, const char *name, u32 namelen)
{
	struct index_walk walk;
	s32 ret = index_read_name(sbi, name, namelen, &walk);
	if (ret)
		return (ret == -ENAMETOOLONG) ? -ENOENT : ret;
	if (!walk.found)
		return -ENOENT;

	walk.copy.local.tag[walk.slot] = 0;
	walk.copy.local.region_id[walk.slot] = LAYOUT_LINK_SLOT_FREE;

	/* Read out of the copy before the write spends it. */
	bool line_emptied = index_is_line_empty(&walk.copy);
	u32 successor = walk.copy.local.next_link;

	/* One write takes the name away. */
	cxl_set_layout_index_link(walk.addr, &walk.copy);

	/* A pool line the last name just left returns to the pool by being named no more. Losing
	 * this write leaves the line linked and empty, which the next insert here fills. */
	if (walk.pool_idx != LAYOUT_LINK_END && walk.prior_addr && line_emptied)
		index_unlink_line(&walk, successor);

	return 0;
}
