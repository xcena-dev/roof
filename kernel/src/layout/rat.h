/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * rat.h - Region Allocation Table on-disk format. One entry tracks one region file and carries
 * that region's delegation table inside itself.
 */

#ifndef _LAYOUT_RAT_H
#define _LAYOUT_RAT_H

#include <linux/limits.h>
#include <linux/string.h>
#include <linux/types.h>

#include "cxl/io.h" /* the whole-line views this row is defined through */
#include "daemon_uapi.h" /* FS_DAEMON_IDENT_LEN */
#include "uapi.h" /* FS_NAME_MAX, FS_DELEG_MAX */
#include "util/bitmap.h" /* BITS_WORD_WIDTH, BITS_MASK_WIDTH */

enum layout_region_type {
	LAYOUT_REGION_DATA = 0,
	/* The daemon's lock area. No process owns it and GC never reclaims it,
	 * so a daemon that restarts under a new pid keeps mapping it. */
	LAYOUT_REGION_LOCK = 1,
};

/* 0 is CONTIGUOUS, so a zeroed hot line reads as one. */
enum layout_placement {
	LAYOUT_PLACEMENT_CONTIGUOUS = 0,
	LAYOUT_PLACEMENT_PAGED = 1,
};

enum layout_rat_entry_state {
	LAYOUT_RAT_ENTRY_FREE = 0,
	LAYOUT_RAT_ENTRY_ALLOCATING = 1,
	LAYOUT_RAT_ENTRY_ALLOCATED = 2,
	LAYOUT_RAT_ENTRY_DELETING = 3,
	/* The owning node is gone and a reader is still here. Nobody owns it, no new grant lands on
	 * it, and the process rows and open references already on it keep it until they go. Not
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

/* What a row names, which decides how the bytes past its shared head read. 0 is no kind, and is what
 * a row a create cleared carries. */
enum acl_deleg_kind {
	ACL_DELEG_KIND_PROCESS = 1,
	ACL_DELEG_KIND_ACCOUNT = 2,
	ACL_DELEG_KIND_ROLE = 3,
};

enum {
	ACL_DELEG_MAX_ENTRIES = FS_DELEG_MAX,
	/* Every row the table has, so a mask read off the medium names none past its end. */
	ACL_DELEG_ALL_ROWS = (1U << ACL_DELEG_MAX_ENTRIES) - 1,
	/* The lock region's slot per node. Format marks them all, because a node writes its own at
	 * bring-up with no turn to mark it under. */
	ACL_DELEG_NODE_ROWS = (1U << FS_MAX_NODE_ID) - 1,
};

_Static_assert((u32)ACL_DELEG_MAX_ENTRIES < (u32)BITS_MASK_WIDTH, "deleg_taken holds one bit per row in one u32");
_Static_assert((u32)FS_MAX_NODE_ID <= (u32)ACL_DELEG_MAX_ENTRIES, "the lock region holds a row per node");

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
_Static_assert((u32)LAYOUT_RAT_MAP_WORD_BITS == (u32)BITS_WORD_WIDTH, "the summary words are the bitmap util's");
_Static_assert(LAYOUT_RAT_MAP_WORDS * 8 + 8 <= CXL_LINE_BYTES,
	       "the summary bitmap and the placement count must fit one line");

/* An id field that does not narrow the row. Not zero, which is root and gid 0. */
#define ACL_DELEG_ANY_ID ((__u32)~0U)

/* The bytes every kind of row shares, at the same offsets and widths in each kind's view, so a scan
 * reads state, kind, node and perms off any row before it knows which kind it holds. */
#define ACL_DELEG_HEAD_FIELDS(FIELD, ARRAY, BYTES)                                     \
	FIELD(16, state) /* enum acl_deleg_state */                                    \
	FIELD(16, kind) /* enum acl_deleg_kind */                                      \
	FIELD(16, node_id) /* the node that wrote the row, exact: 0 matches nothing */ \
	FIELD(16, perms) /* Permission bitmask (FS_PERM_*) */

/* A process row binds to that process's exe binary, start time and execve generation, so a reused
 * pid or an execve stops matching, and carries the account it runs as. */
#define ACL_DELEG_PROCESS_FIELDS(FIELD, ARRAY, BYTES)                      \
	ACL_DELEG_HEAD_FIELDS(FIELD, ARRAY, BYTES)                         \
	FIELD(32, pid) /* the process's tgid */                            \
	FIELD(32, uid) /* the account the process runs as */               \
	FIELD(32, gid)                                                     \
	FIELD(32, exe_inode_dev) /* exe binary fs dev (huge_encode_dev) */ \
	FIELD(64, exe_inode_ino) /* exe binary inode # */                  \
	FIELD(64, exec_id) /* execve generation */                         \
	FIELD(64, birth_time) /* PID reuse protection */                   \
	BYTES(16, reserved)

CXL_DEFINE_LINE_VIEWS(acl_deleg_process, ACL_DELEG_PROCESS_FIELDS)

/* An account row names a node-local uid or gid, either of which may be ACL_DELEG_ANY_ID. */
#define ACL_DELEG_ACCOUNT_FIELDS(FIELD, ARRAY, BYTES) \
	ACL_DELEG_HEAD_FIELDS(FIELD, ARRAY, BYTES)    \
	FIELD(32, uid)                                \
	FIELD(32, gid)                                \
	BYTES(48, reserved)

CXL_DEFINE_LINE_VIEWS(acl_deleg_account, ACL_DELEG_ACCOUNT_FIELDS)

/* A workload by its attested group and role, on target_node or on every node with 0. node_id is the
 * writer's, so the writer's unmount or death takes the row as it takes an account row. */
#define ACL_DELEG_ROLE_FIELDS(FIELD, ARRAY, BYTES)                                   \
	ACL_DELEG_HEAD_FIELDS(FIELD, ARRAY, BYTES)                                   \
	BYTES(FS_DAEMON_IDENT_LEN, group) /* NUL-padded, as the helper attests it */ \
	BYTES(FS_DAEMON_IDENT_LEN, role)                                             \
	FIELD(16, target_node) /* the node it answers on, 0 for every node */        \
	BYTES(22, reserved)

CXL_DEFINE_LINE_VIEWS(acl_deleg_role, ACL_DELEG_ROLE_FIELDS)

_Static_assert((u32)FS_LABEL_LEN == (u32)FS_DAEMON_IDENT_LEN,
	       "a role grant's labels must be the ones the helper attests");
_Static_assert(FS_PERM_ALL <= U16_MAX && FS_MAX_NODE_ID <= U16_MAX, "a row holds perms and node ids in 16 bits");

/* One slot of a RAT entry's delegation table, written and read through the view its kind names. The
 * head sits at the same offsets in every view, so a reader wanting only the head may use any. */
union acl_deleg_entry {
	struct acl_deleg_process process;
	struct acl_deleg_account account;
	struct acl_deleg_role role;
};

/* CL0 of a RAT entry: what an open, a read and a stat all reach for. */
#define LAYOUT_RAT_HOT_FIELDS(FIELD, ARRAY, BYTES)           \
	FIELD(32, state) /* enum layout_rat_entry_state */   \
	FIELD(32, region_type) /* enum layout_region_type */ \
	FIELD(64, alloc_time) /* ns */                       \
	FIELD(64, modified_at) /* ns */                      \
	FIELD(32, uid) /* POSIX owner UID */                 \
	FIELD(32, gid) /* POSIX owner GID */                 \
	FIELD(16, mode) /* POSIX mode bits */                \
	FIELD(16, placement) /* enum layout_placement */     \
	FIELD(16, align_shift) /* log2, 0 for the granule */ \
	FIELD(16, extent_count) /* 0 until placed */         \
	BYTES(24, reserved0)

CXL_DEFINE_LINE_VIEWS(layout_rat_hot, LAYOUT_RAT_HOT_FIELDS)

enum layout_extents_config {
	LAYOUT_MAX_EXTENTS = 4,
};

/* CL1: the pieces in file order. Written before the hot line counts them, so a line no hot line
 * counts is never read and needs no clearing. */
#define LAYOUT_RAT_EXTENT_FIELDS(FIELD, ARRAY, BYTES)                                 \
	ARRAY(64, LAYOUT_MAX_EXTENTS, offset) /* bytes from the device start */       \
	ARRAY(32, LAYOUT_MAX_EXTENTS, granules) /* in the superblock's granules */    \
	ARRAY(16, LAYOUT_MAX_EXTENTS, device_id) /* 0 while a mount has one device */ \
	BYTES(8, reserved)

CXL_DEFINE_LINE_VIEWS(layout_rat_extent, LAYOUT_RAT_EXTENT_FIELDS)

_Static_assert(LAYOUT_MAX_EXTENTS <= U16_MAX, "the hot line counts extents in 16 bits");

/*
 * CL3 of a RAT entry: who owns the region, what a non-owner may do to it, and which delegation rows
 * a scan reads. An owner check reads most of these at once, which is why they share a line.
 */
#define LAYOUT_RAT_ACL_FIELDS(FIELD, ARRAY, BYTES)                                           \
	FIELD(16, default_perms) /* default non-owner perms (FS_PERM_*) */                   \
	FIELD(16, owner_node_id) /* the node the owner runs on */                            \
	FIELD(32, owner_pid)                                                                 \
	FIELD(64, owner_birth_time) /* PID reuse protection */                               \
	FIELD(64, owner_exe_inode_ino) /* owner exe inode # (post-exec retention defense) */ \
	FIELD(32, owner_exe_inode_dev) /* owner exe binary fs dev */                         \
	BYTES(FS_DAEMON_IDENT_LEN, owner_group)                                              \
	BYTES(FS_DAEMON_IDENT_LEN, owner_role)                                               \
	FIELD(32, deleg_taken) /* bit i set before row i is written, cleared after it is emptied */

CXL_DEFINE_LINE_VIEWS(layout_rat_acl, LAYOUT_RAT_ACL_FIELDS)

/* One region file's entry. open(O_CREAT) leaves extent_count 0, and ftruncate places the region. */
struct layout_rat_entry {
	struct layout_rat_hot hot;

	struct layout_rat_extent extents;

	/* Cold, reached only on a hash match. A name of exactly FS_NAME_MAX bytes keeps no
	 * terminator here, so a reader supplies one. */
	char name[FS_NAME_MAX + 1];

	struct layout_rat_acl acl;

	union acl_deleg_entry deleg_entries[ACL_DELEG_MAX_ENTRIES];
} __attribute__((packed));

/* Which line of an entry to fence, and what the asserts below hold the struct to. */
enum layout_rat_cl {
	LAYOUT_RAT_CL_HOT = 0,
	LAYOUT_RAT_CL_EXTENTS = 1,
	LAYOUT_RAT_CL_NAME = 2,
	LAYOUT_RAT_CL_ACL = 3,
};

_Static_assert(offsetof(struct layout_rat_entry, hot) == LAYOUT_RAT_CL_HOT * 64, "CL_HOT enum must match hot offset");
_Static_assert(offsetof(struct layout_rat_entry, extents) == LAYOUT_RAT_CL_EXTENTS * 64,
	       "CL_EXTENTS enum must match extents offset");
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
	FIELD(32, version) /* LAYOUT_RAT_VERSION */             \
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
