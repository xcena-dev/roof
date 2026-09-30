/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * rat.h - Region Allocation Table on-disk format. One entry tracks one region file and carries
 * that region's delegation table inside itself.
 */

#ifndef _LAYOUT_RAT_H
#define _LAYOUT_RAT_H

#include <linux/string.h>
#include <linux/types.h>

#include "cxl/io.h" /* the whole-line views this row is defined through */
#include "daemon_uapi.h" /* FS_DAEMON_IDENT_LEN */
#include "uapi.h" /* FS_NAME_MAX, FS_DELEG_MAX */

enum layout_region_type {
	LAYOUT_REGION_DATA = 0,
	/* The daemon's lock area. No process owns it and GC never reclaims it,
	 * so a daemon that restarts under a new pid keeps mapping it. */
	LAYOUT_REGION_LOCK = 1,
};

enum layout_rat_entry_state {
	LAYOUT_RAT_ENTRY_FREE = 0,
	LAYOUT_RAT_ENTRY_ALLOCATING = 1,
	LAYOUT_RAT_ENTRY_ALLOCATED = 2,
	LAYOUT_RAT_ENTRY_DELETING = 3,
	/* The owning node is gone and a reader is still here. Nobody owns it, no new grant lands on
	 * it, and the rows and open references already on it are what keep it until they go. Not
	 * transient: it can stand for as long as somebody reads it. */
	LAYOUT_RAT_ENTRY_OWNER_DEAD = 4,
};

/* One being made or unmade. Paired with owner_node_id == 0 it says the node running that
 * operation died with neither FREE nor ALLOCATED published. */
static inline bool layout_rat_state_is_transient(u32 state)
{
	return state == LAYOUT_RAT_ENTRY_ALLOCATING || state == LAYOUT_RAT_ENTRY_DELETING;
}

/* A region that exists and a caller may act on, whether or not the node that made it is still
 * here. FREE carries none, and the transient pair is one still on its way in or out. */
static inline bool layout_rat_state_is_standing(u32 state)
{
	return state == LAYOUT_RAT_ENTRY_ALLOCATED || state == LAYOUT_RAT_ENTRY_OWNER_DEAD;
}

enum acl_deleg_state {
	ACL_DELEG_EMPTY = 0,
	ACL_DELEG_ACTIVE = 1,
};

enum {
	ACL_DELEG_MAX_ENTRIES = FS_DELEG_MAX,
};

enum layout_rat_config {
	LAYOUT_MAX_RAT_ENTRIES = 256,

	ACL_DELEG_ENTRY_SIZE = 64,
	LAYOUT_RAT_ENTRY_SIZE = 2048,
	LAYOUT_RAT_HEADER_SIZE = 128,

	/* The summary line's bitmap: one word per this many entries, and what the line has left
	 * once the words and the placement count are in it. */
	LAYOUT_RAT_MAP_WORD_BITS = 64,
	LAYOUT_RAT_MAP_WORDS = LAYOUT_MAX_RAT_ENTRIES / LAYOUT_RAT_MAP_WORD_BITS,
	LAYOUT_RAT_MAP_RESERVED = CXL_LINE_BYTES - LAYOUT_RAT_MAP_WORDS * 8 - 8,
};

_Static_assert(LAYOUT_MAX_RAT_ENTRIES % LAYOUT_RAT_MAP_WORD_BITS == 0,
	       "the summary bitmap covers the entries in whole words");
_Static_assert(LAYOUT_RAT_MAP_WORDS * 8 + 8 <= CXL_LINE_BYTES,
	       "the summary bitmap and the placement count must fit one line");

/* An id field that does not narrow the row. Not zero, which is root and gid 0. */
#define ACL_DELEG_ANY_ID ((__u32)~0U)

/*
 * Delegation Entry - one line of a RAT entry's delegation table, in two shapes told apart by pid.
 * A process row (pid != 0) binds to that process's start time, exe binary and execve generation,
 * so a reused pid or an execve stops matching, and carries the account it runs as. An account row
 * (pid == 0) names a node-local uid or gid, either of which may be ACL_DELEG_ANY_ID, for a grant
 * that outlives the process, and leaves birth_time and the exe fields unused.
 */
#define ACL_DELEG_FIELDS(FIELD, ARRAY, BYTES)                                 \
	FIELD(32, state) /* enum acl_deleg_state */                           \
	FIELD(32, node_id) /* Target node, exact: 0 matches nothing */        \
	FIELD(32, pid) /* Target PID, or 0 for an account row */              \
	FIELD(32, perms) /* Permission bitmask (FS_PERM_*) */                 \
	FIELD(64, birth_time) /* PID reuse protection (process rows only) */  \
	FIELD(64, granted_at) /* Grant timestamp (ns since epoch) */          \
	FIELD(64, exec_id) /* execve generation (process rows only) */        \
	FIELD(64, exe_inode_ino) /* exe binary inode # (process rows only) */ \
	FIELD(32, exe_inode_dev) /* exe binary fs dev (huge_encode_dev) */    \
	FIELD(32, uid) /* target uid, or the process row's own */             \
	FIELD(32, gid) /* target gid, or the process row's own */             \
	BYTES(4, reserved)

CXL_DEFINE_LINE_VIEWS(acl_deleg_entry, ACL_DELEG_FIELDS)

/* CL0 of a RAT entry: what an open, a read and a stat all reach for. */
#define LAYOUT_RAT_HOT_FIELDS(FIELD, ARRAY, BYTES)                                  \
	FIELD(32, state) /* enum layout_rat_entry_state */                          \
	FIELD(32, region_type) /* enum layout_region_type */                        \
	FIELD(64, phys_offset) /* 0 = not yet allocated (open-without-ftruncate) */ \
	FIELD(64, size) /* 2MB aligned */                                           \
	FIELD(64, alloc_time) /* ns */                                              \
	FIELD(64, modified_at) /* ns */                                             \
	FIELD(32, uid) /* POSIX owner UID */                                        \
	FIELD(32, gid) /* POSIX owner GID */                                        \
	FIELD(16, mode) /* POSIX mode bits */                                       \
	BYTES(14, reserved0)

CXL_DEFINE_LINE_VIEWS(layout_rat_hot, LAYOUT_RAT_HOT_FIELDS)

/*
 * CL2 of a RAT entry: who owns the region, what a non-owner may do to it, and where a delegation
 * scan stops. An owner check reads most of these at once, which is why they share a line.
 */
#define LAYOUT_RAT_ACL_FIELDS(FIELD, ARRAY, BYTES)                                            \
	FIELD(16, default_perms) /* default non-owner perms (FS_PERM_*) */                    \
	FIELD(16, owner_node_id) /* the node the owner runs on */                             \
	FIELD(32, owner_pid)                                                                  \
	FIELD(64, owner_birth_time) /* PID reuse protection */                                \
	FIELD(64, owner_exe_inode_ino) /* owner exe inode # (post-exec retention defense) */  \
	FIELD(32, owner_exe_inode_dev) /* owner exe binary fs dev */                          \
	BYTES(FS_DAEMON_IDENT_LEN, owner_group)                                               \
	BYTES(FS_DAEMON_IDENT_LEN, owner_role)                                                \
	FIELD(16, deleg_bound) /* slots a delegation scan must cover: one past the highest */ \
	BYTES(2, reserved2)

CXL_DEFINE_LINE_VIEWS(layout_rat_acl, LAYOUT_RAT_ACL_FIELDS)

/*
 * One region file's entry, whose allocation is physically contiguous. It fills in two steps:
 * open(O_CREAT) leaves phys_offset and size 0, and ftruncate then places the region.
 */
struct layout_rat_entry {
	struct layout_rat_hot hot;

	/* Cold, reached only on a hash match. A name of exactly FS_NAME_MAX bytes keeps no
	 * terminator here, so a reader supplies one. */
	char name[FS_NAME_MAX + 1];

	struct layout_rat_acl acl;

	struct acl_deleg_entry deleg_entries[ACL_DELEG_MAX_ENTRIES];
} __attribute__((packed));

/* Which line of an entry to fence, and what the asserts below hold the struct to. */
enum layout_rat_cl {
	LAYOUT_RAT_CL_HOT = 0,
	LAYOUT_RAT_CL_NAME = 1,
	LAYOUT_RAT_CL_ACL = 2,
};

_Static_assert(offsetof(struct layout_rat_entry, hot) == LAYOUT_RAT_CL_HOT * 64, "CL_HOT enum must match hot offset");
_Static_assert(offsetof(struct layout_rat_entry, name) == LAYOUT_RAT_CL_NAME * 64, "CL_NAME enum must match name offset");
_Static_assert(FS_NAME_MAX + 1 == CXL_LINE_BYTES, "the name must fill its line exactly");
_Static_assert(offsetof(struct layout_rat_entry, acl) == LAYOUT_RAT_CL_ACL * 64, "CL_ACL enum must match acl offset");

/* Every entry then starts on a line boundary, so a whole-line access to one is aligned
 * whenever the table's own base is. */
_Static_assert(LAYOUT_RAT_ENTRY_SIZE % CXL_LINE_BYTES == 0,
	       "a RAT entry must be a whole number of cachelines");
_Static_assert(LAYOUT_RAT_HEADER_SIZE % CXL_LINE_BYTES == 0,
	       "the RAT header must be a whole number of cachelines");
_Static_assert(ACL_DELEG_ENTRY_SIZE == CXL_LINE_BYTES,
	       "a delegation entry is exactly one cacheline");

_Static_assert(offsetof(struct layout_rat_entry, deleg_entries) % CXL_LINE_BYTES == 0,
	       "the delegation array must start on a cacheline boundary");

/*
 * CL0 of the RAT header: what a mount validates before it trusts the table, plus the two bounds
 * a region has to be placed inside. A mount reads all four at once, so they share a line.
 */
#define LAYOUT_RAT_HEAD_FIELDS(FIELD, ARRAY, BYTES)             \
	FIELD(32, magic) /* LAYOUT_RAT_MAGIC */                 \
	FIELD(32, version) /* 1 */                              \
	FIELD(64, device_size)                                  \
	FIELD(64, regions_start) /* where region files start */ \
	BYTES(40, reserved0)

CXL_DEFINE_LINE_VIEWS(layout_rat_head, LAYOUT_RAT_HEAD_FIELDS)

/*
 * CL1 of the RAT header: a summary a scan reads instead of the entries. The entries stay the
 * truth: a caller acting on a bit re-reads the entry, so a bit a crash left stale costs a fallback
 * scan and not a wrong answer. Written only under the turn, so a whole-line read-modify-write of
 * it loses nothing.
 */
#define LAYOUT_RAT_MAP_FIELDS(FIELD, ARRAY, BYTES)                                         \
	ARRAY(64, LAYOUT_RAT_MAP_WORDS, taken) /* bit i set while entry i is not FREE */   \
	FIELD(64, placements) /* stepped as an extent is placed or freed, odd meanwhile */ \
	BYTES(LAYOUT_RAT_MAP_RESERVED, reserved0)

CXL_DEFINE_LINE_VIEWS(layout_rat_map, LAYOUT_RAT_MAP_FIELDS)

static inline bool layout_rat_map_has(const union layout_rat_map_copy *map, u32 idx)
{
	return (map->local.taken[idx / LAYOUT_RAT_MAP_WORD_BITS] >> (idx % LAYOUT_RAT_MAP_WORD_BITS)) & 1U;
}

static inline void layout_rat_map_mark(union layout_rat_map_copy *map, u32 idx, bool taken)
{
	const u64 bit = 1ULL << (idx % LAYOUT_RAT_MAP_WORD_BITS);
	if (taken)
		map->local.taken[idx / LAYOUT_RAT_MAP_WORD_BITS] |= bit;
	else
		map->local.taken[idx / LAYOUT_RAT_MAP_WORD_BITS] &= ~bit;
}

/* The first slot at or past @from whose bit reads @taken, or LAYOUT_MAX_RAT_ENTRIES when none. */
static inline u32 layout_rat_map_next(const union layout_rat_map_copy *map, u32 from, bool taken)
{
	for (u32 idx = from; idx < LAYOUT_MAX_RAT_ENTRIES;
	     idx += LAYOUT_RAT_MAP_WORD_BITS - (idx % LAYOUT_RAT_MAP_WORD_BITS)) {
		const u32 word_at = idx / LAYOUT_RAT_MAP_WORD_BITS;
		u64 word = map->local.taken[word_at];
		if (!taken)
			word = ~word;
		/* Bits below @from in its own word are behind us. */
		word &= ~0ULL << (idx % LAYOUT_RAT_MAP_WORD_BITS);
		if (word)
			return word_at * LAYOUT_RAT_MAP_WORD_BITS + (u32)__builtin_ctzll(word);
	}
	return LAYOUT_MAX_RAT_ENTRIES;
}

/* The table itself: one header line, the summary line, then an entry per region file. */
struct layout_rat {
	struct layout_rat_head head;
	struct layout_rat_map map;
	struct layout_rat_entry entries[LAYOUT_MAX_RAT_ENTRIES];
} __attribute__((packed));

_Static_assert(offsetof(struct layout_rat, entries) == LAYOUT_RAT_HEADER_SIZE,
	       "the entry array must start where the header ends");

/*
 * The name line, in one read, as a null-terminated string. @line is the caller's storage for it:
 * the load wants a line-aligned destination, which a bare char array does not promise.
 */
static inline const char *layout_read_rat_name(const struct layout_rat_entry *entry, struct cxl_cacheline *line)
{
	cxl_get_cacheline(line, entry->name);
	line->byte[FS_NAME_MAX] = '\0';
	return (const char *)line->byte;
}

/* One line write, so the bytes past @len go to 0 rather than keeping a longer name's tail. */
static inline void layout_write_rat_name(struct layout_rat_entry *entry, const char *name, u32 len)
{
	struct cxl_cacheline line;
	const u32 kept = (len > FS_NAME_MAX) ? FS_NAME_MAX : len;
	memset(line.byte, 0, sizeof(line.byte));
	memcpy(line.byte, name, kept);
	cxl_set_cacheline(entry->name, &line);
}

#endif /* _LAYOUT_RAT_H */
