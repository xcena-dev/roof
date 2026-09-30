/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * core.h - the in-memory sbi and the accessors over it.
 *
 * The filesystem hands out CXL space and enforces per-region permissions. It does not serialise
 * nodes against each other, so an application that lets several nodes mutate the same metadata
 * provides that serialisation itself. Within one node, sbi->meta_lock covers the metadata
 * mutations that take more than one line write.
 */

#ifndef _FS_CORE_H
#define _FS_CORE_H

/* Kbuild puts KBUILD_MODNAME on every compile line, so the filesystem type, the sysfs
 * directory and every log prefix read the module's name there rather than as a literal. */
#undef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/fs.h>
#include <linux/list.h>
#include <linux/log2.h> /* const_ilog2, which ties a shift to its size */
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include "uapi.h" /* FS_MAX_NODE_ID, one stamp watch per slot */

/* Held by pointer, so the definition is the owning header's to provide. */
struct bootstrap_slot;
struct dax_areas;
struct gc_orphan_tracker;
struct layout_index_link;
struct layout_rat;
struct layout_shard_header;
struct layout_superblock;
struct region_span;
struct sysfs_mount;
struct daemon_channel;

/* ============================================================================
 * What this filesystem tells the VFS about itself
 * ============================================================================
 *
 * The shift cannot disagree with the size, because const_ilog2 derives it. i_blocks counts
 * 512-byte units by VFS definition, so that pair is the kernel's SECTOR_* and not ours.
 */

enum fs_vfs_config {
	FS_ROOT_INO = 1, /* the root directory's ino */
	FS_ROOT_DIR_MODE = 0755,

	FS_VFS_BLOCK_SIZE = 4096, /* sb->s_blocksize */
	FS_VFS_BLOCK_SIZE_BITS = const_ilog2(FS_VFS_BLOCK_SIZE),
};

/* The derivation above only holds for a power of two, and says nothing when it is not one. */
_Static_assert(1 << FS_VFS_BLOCK_SIZE_BITS == FS_VFS_BLOCK_SIZE,
	       "the block size must be a power of two");

/* ============================================================================
 * Local DRAM cache for shard metadata (read-only after format)
 * ============================================================================
 *
 * The cached fields never change after format, so holding them in DRAM takes a CXL read off
 * every index operation.
 */

struct fs_shard_cache {
	struct layout_index_link *buckets; /* CXL bucket array, one line per bucket */
	struct layout_index_link *pool; /* CXL lines this shard hands to a full bucket */
	struct layout_shard_header *header; /* CXL shard header */
};

/* One peer slot's stamp as this node last saw it, timed on this node's clock. */
struct bootstrap_stamp_watch {
	u64 token; /* the holder being watched, so a new mount in the slot starts over */
	u64 stamp; /* the heartbeat value last seen */
	u64 since_ns; /* ktime_get_ns() when that value was first seen, 0 before any read */
	bool moved; /* the value has changed while watched */
};

/* ============================================================================
 * In-memory superblock info
 * ============================================================================ */

struct fs_sb_info {
	/* The mount this belongs to. The GC thread holds only an sbi, and reaching the mount's
	 * inodes from there is what lets it take a live mapping down. */
	struct super_block *sb;

	/* ── The device ───────────────────────────────────────────────────── */
	char daxdev_path[128]; /* the DEV_DAX device this mount sits on */
	phys_addr_t phys_base; /* where that device starts in physical memory */
	u64 total_size; /* how much of it there is */

	/* ── Its two areas ────────────────────────────────────────────────
	 * A PAT reservation is exclusive per physical range, so one range cannot carry two cache
	 * policies. The data area comes from memremap_pages, which gives ZONE_DEVICE pages, and
	 * MEMORY_DEVICE_GENERIC supports the pinning a GPU DMA registration needs. Both belong to
	 * the device, so these two are copies of what @dax_areas holds.
	 */
	void *meta_base; /* uncached, below LAYOUT_DATA_OFFSET */
	void *data_base; /* write-back, from LAYOUT_DATA_OFFSET on */
	struct dax_areas *dax_areas;

	/* ── vm_ops wrapper (mprotect enforcement) ──────────────────────────
	 * Seeded at the first mmap by copying the underlying ops and overriding .mprotect. Every
	 * vma points at this one copy, so no vma carries an allocation of its own.
	 */
	struct vm_operations_struct vm_ops;
	bool vm_ops_seeded;
	struct mutex vm_ops_lock;

	/* ── Node identity ────────────────────────────────────────────────── */
	u32 node_id;

	/* ── Cached on-disk layout pointers (set at mount, valid until umount) */
	struct layout_superblock *gsb; /* Global superblock */
	struct bootstrap_slot *bootstrap_slots; /* slot base array */
	struct layout_rat *rat; /* Region allocation table array */

	/* ── Geometry (read from GSB at mount time) ───────────────────────── */
	u32 num_shards;
	u32 shard_mask; /* num_shards - 1 */
	u32 buckets_per_shard;
	u32 bucket_mask; /* buckets_per_shard - 1 */
	u32 pool_lines_per_shard;
	u64 shard_table_offset; /* both read once, by the superblock read */
	u64 rat_offset;

	/* ── The two region pools ─────────────────────────────────────────
	 * A region's cache policy is where it sits: below @wb_start the device is mapped uncached
	 * and above it write-back, and one mapping covers each side. @granule is what an offset and
	 * a size are rounded to, and every node reads it from the superblock so they agree.
	 */
	u64 granule;
	u64 uc_start; /* first offset a region may take, past the metadata */
	u64 wb_start; /* where the uncached pool ends and the write-back one begins */

	/* What a format on this mount is to write, off its options. Read before the areas are
	 * mapped, since the split they take has to be the one the format then publishes. */
	u32 format_granule_shift;
	u32 format_uc_granules;

	/* ── Per-shard DRAM cache (read-only after mount) ─────────────────── */
	struct fs_shard_cache *shard_cache;

	/* The gap search's working array, held by the mount so that search allocates nothing. It
	 * stays the pool's standing extents as of span_placements, so a search that finds the RAT's
	 * placement count unchanged reads no entry at all. Guarded by meta_lock's mutex half. */
	struct region_span *span_scratch;
	u32 span_count;
	bool span_uncached;
	u64 span_placements;
	/* Whether the RAT's summary line agreed with its entries when this mount last walked
	 * them. False keeps the extent search on the entries themselves. */
	bool rat_map_trusted;

	/* ── Open references ─────────────────────────────────────────────── */
	/* This node's count of struct file references per RAT entry. The medium carries one bit per
	 * entry built from these, and refs_publish_lock keeps two publishers from each storing a
	 * line the other's bit is missing from. */
	atomic_t *refs_local;
	struct mutex refs_publish_lock;

	/* ── Metadata serialisation ───────────────────────────────────────── */
	/* The node-local half of the metadata lock, taken before the helper's and interruptibly.
	 * It covers the read-then-write sequences no single CAS expresses. */
	struct mutex meta_lock;

	/* ── Garbage collector ────────────────────────────────────────────── */
	struct task_struct *gc_thread;
	struct mutex gc_sweep_lock;
	atomic_t gc_paused; /* 1 = GC temporarily paused */
	atomic_t gc_epoch; /* GC cycle counter (liveness check) */
	struct gc_orphan_tracker *gc_orphans; /* mount-lifetime, so a GC restart reuses it */
	u32 gc_orphan_count;

	/* ── Bootstrap auto-mount ─────────────────────────────────────────── */
	s32 bootstrap_slot_idx; /* -1 until this node claims one */
	u64 bootstrap_token; /* the token this node wrote into its slot */
	struct bootstrap_stamp_watch stamp_watch[FS_MAX_NODE_ID]; /* GC thread and mount only */

	/* Set by the tick when the slot answers to another token, so this node_id is somebody
	 * else's. Written once from the GC thread and read from every operation. */
	bool fenced;

	/* Set by unmount_prepare, which then refuses to clean while this node holds a reference. open
	 * and create refuse from here, so the count it read stays true until the mount goes. */
	bool draining;
	/* Set once a prepare found no reference. From then on the clean step has begun and draining stays up. */
	bool drain_latched;
	struct mutex drain_mutex; /* one unmount_prepare at a time */

	/* Set by a test's crash point. From then on this node takes no lock, releases none it
	 * holds and sweeps nothing, so the medium sees what a node that lost power there left. */
	bool test_dead;

	/* ── Admin role (lowest active node_id, refreshed dynamically) ───── */
	u32 cached_admin_node_id;

	/* This mount's own sysfs directory, or NULL when it has none. */
	struct sysfs_mount *sysfs_mount;
	/* This mount's upcall channel: one helper per node. */
	struct daemon_channel *daemon;
};

/* ============================================================================
 * sbi accessor
 * ============================================================================ */

static inline struct fs_sb_info *fs_get_sb_info(struct super_block *sb)
{
	return sb->s_fs_info;
}

/* A mount that deferred its node_id has none until the helper claims one under
 * the daemon's lock. Nothing may stamp node identity on disk until then. */
static inline bool fs_is_identity_pending(const struct fs_sb_info *sbi)
{
	return sbi->node_id == 0;
}

/* This mount's slot went to another node, which now answers to the node_id every RAT entry and
 * delegation row here names. Latched: what it wrote as that node stands, and umount is the only way out. */
static inline bool fs_is_fenced(const struct fs_sb_info *sbi)
{
	return READ_ONCE(sbi->fenced);
}

static inline bool fs_is_draining(const struct fs_sb_info *sbi)
{
	return READ_ONCE(sbi->draining);
}

#endif /* _FS_CORE_H */
