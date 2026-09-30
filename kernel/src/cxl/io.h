/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * io.h - Primitives for accessing CXL shared memory.
 *
 * Bundles what every layer needs when touching the medium:
 *   - whole-line reads and writes, which is the granularity CXL resolves at
 *   - the two views of a row, little-endian on the medium and host order in a copy
 *   - cross-host visibility barriers, for the bulk writes that go around the views
 * Kept in one header so callers see a single, coherent CXL-I/O surface.
 */

#ifndef _CXL_IO_H
#define _CXL_IO_H

#include <linux/stddef.h>
#include <linux/types.h>

/*
 * CXL multi-node memory barriers
 *
 * The metadata area is mapped uncached, and dax_map_split_areas refuses the mount when the
 * mapping came back cached, so none of it sits in a cache line for a writeback to push out
 * or an invalidate to drop. These two order the accesses and leave the cache alone.
 *
 * A whole-line access carries its own, so these two are for what goes around them: a bulk memset
 * over an area, and the line access itself.
 *
 * The range stays in the signature to say at the call site what is being ordered. Neither
 * macro reads it.
 */
#define CXL_WMB(addr, len)    \
	do {                  \
		(void)(addr); \
		(void)(len);  \
		wmb();        \
	} while (0)

#define CXL_RMB(addr, len)    \
	do {                  \
		(void)(addr); \
		(void)(len);  \
		rmb();        \
	} while (0)

/*
 * The file data area is write-back instead, so a peer's write can sit behind this node's own
 * cached copy of the line. CXL 3.0 keeps the two coherent in hardware; on 2.0 the line has to
 * be dropped before the read. Build with -DCONFIG_FS_CXL2_COMPAT for a 2.0 host.
 */
#ifdef CONFIG_FS_CXL2_COMPAT

#include <linux/libnvdimm.h>

#define CXL_RMB_CACHED(addr, len)                            \
	do {                                                 \
		arch_invalidate_pmem((void *)(addr), (len)); \
		rmb();                                       \
	} while (0)

/* The write side of the same gap: a cached store this node made has to leave its cache before a
 * 2.0 peer reads the line. */
#define CXL_WMB_CACHED(addr, len)                          \
	do {                                               \
		arch_wb_cache_pmem((void *)(addr), (len)); \
		wmb();                                     \
	} while (0)

#else /* CXL 3.0: hardware coherence guaranteed across hosts */

#define CXL_RMB_CACHED(addr, len) CXL_RMB(addr, len)
#define CXL_WMB_CACHED(addr, len) CXL_WMB(addr, len)

#endif /* CONFIG_FS_CXL2_COMPAT */

/* One line, which is the unit CXL coherence works in. */
#define CXL_LINE_BYTES 64U

/*
 * cxl_is_mapping_uncached - does this kernel mapping actually bypass the cache?
 *
 * memtype_reserve grants the type it can rather than failing, so an uncached request over a
 * range another owner reserved write-back returns a cached mapping and no error at all.
 */
bool cxl_is_mapping_uncached(const void __iomem *addr);

/*
 * Whole-line access. One aligned 64-byte transaction rather than eight 8-byte
 * ones, which on an uncached mapping also avoids a read-modify-write in the
 * memory controller. Callers read a line into their own copy, edit the copy,
 * then write it back, so the shared line is touched exactly twice.
 */

/*
 * A caller's private copy of one line. The attribute holds for an automatic or
 * static instance but not for one kmalloc'd inside a larger struct, and it says
 * nothing about @line below. Only the shared side decides whether a peer can
 * see half a write, so that is the side cxl_get_cacheline and _set check.
 */
struct cxl_cacheline {
	u8 byte[CXL_LINE_BYTES];
} __aligned(CXL_LINE_BYTES);

/*
 * @line must be 64-byte aligned. Each call carries its own barrier, the read before its
 * load and the write after its store, so a caller does not repeat one around it.
 */
void cxl_get_cacheline(struct cxl_cacheline *copy, const void *line);
void cxl_set_cacheline(void *line, const struct cxl_cacheline *copy);

/*
 * Zeroes @bytes from @base, which both have to be a whole number of lines. One fence at the end
 * rather than one per line, because nothing reads an area while it is being cleared.
 */
void cxl_zero_cachelines(void *base, u64 bytes);

/*
 * One field list per row, expanded four ways: the medium's little-endian view, the host-order
 * view, and the two conversions between them. A field named once therefore reaches every use
 * of it, so the two views cannot drift and no per-field assert is needed.
 *
 * A row's list takes three callbacks. FIELD carries a width in bits, ARRAY that same width plus
 * an element count, and BYTES a length in bytes for a field that has no byte order to convert.
 */
#define CXL_VIEW_MEDIUM_FIELD(width, name) __le##width name;
#define CXL_VIEW_MEDIUM_ARRAY(width, count, name) __le##width name[count];
#define CXL_VIEW_MEDIUM_BYTES(size, name) __u8 name[size];

#define CXL_VIEW_LOCAL_FIELD(width, name) u##width name;
#define CXL_VIEW_LOCAL_ARRAY(width, count, name) u##width name[count];
#define CXL_VIEW_LOCAL_BYTES(size, name) u8 name[size];

#define CXL_VIEW_DECODE_FIELD(width, name) copy->local.name = le##width##_to_cpu(copy->medium.name);
#define CXL_VIEW_DECODE_ARRAY(width, count, name)  \
	for (u32 slot = 0; slot < (count); slot++) \
		copy->local.name[slot] = le##width##_to_cpu(copy->medium.name[slot]);
#define CXL_VIEW_DECODE_BYTES(size, name)

#define CXL_VIEW_ENCODE_FIELD(width, name) copy->medium.name = cpu_to_le##width(copy->local.name);
#define CXL_VIEW_ENCODE_ARRAY(width, count, name)  \
	for (u32 slot = 0; slot < (count); slot++) \
		copy->medium.name[slot] = cpu_to_le##width(copy->local.name[slot]);
#define CXL_VIEW_ENCODE_BYTES(size, name)

/*
 * Both views of one row, the union over them, their size checks and the two wrappers, out of
 * @FIELDS. @name is the medium view's own name; the rest are built from it as @name_local,
 * @name_copy, cxl_get_@name and cxl_set_@name. Invoke without a trailing semicolon.
 *
 * The wrappers convert where the bytes lie, so the two views have to agree on every offset.
 * One list gives them the same fields in the same order, and the three size checks give them
 * the same offsets: the medium view is packed, so its 64 bytes are the fields alone, and any
 * padding the local view took on to satisfy alignment would push it past 64.
 *
 * The barriers come from cxl_get_cacheline and cxl_set_cacheline, so neither these wrappers
 * nor their callers spell one out.
 *
 * cxl_set_ encodes into the same bytes it reads, because the two views share the union. A copy
 * is therefore spent by the write, and a caller wanting it again reads or builds it again.
 */
#define CXL_DEFINE_LINE_VIEWS(name, FIELDS)                                                 \
	struct name {                                                                       \
		FIELDS(CXL_VIEW_MEDIUM_FIELD, CXL_VIEW_MEDIUM_ARRAY, CXL_VIEW_MEDIUM_BYTES) \
	} __attribute__((packed));                                                          \
                                                                                            \
	struct name##_local {                                                               \
		FIELDS(CXL_VIEW_LOCAL_FIELD, CXL_VIEW_LOCAL_ARRAY, CXL_VIEW_LOCAL_BYTES)    \
	};                                                                                  \
                                                                                            \
	union name##_copy {                                                                 \
		struct cxl_cacheline line;                                                  \
		struct name medium;                                                         \
		struct name##_local local;                                                  \
	};                                                                                  \
                                                                                            \
	_Static_assert(sizeof(struct name) == CXL_LINE_BYTES,                               \
		       #name " must cover the whole line");                                 \
	_Static_assert(sizeof(struct name##_local) == CXL_LINE_BYTES,                       \
		       #name "_local must cover the whole line");                           \
	_Static_assert(sizeof(union name##_copy) == CXL_LINE_BYTES,                         \
		       #name "_copy must be exactly one cacheline");                        \
                                                                                            \
	static inline void cxl_get_##name(union name##_copy *copy, const struct name *row)  \
	{                                                                                   \
		cxl_get_cacheline(&copy->line, row);                                        \
		FIELDS(CXL_VIEW_DECODE_FIELD, CXL_VIEW_DECODE_ARRAY, CXL_VIEW_DECODE_BYTES) \
	}                                                                                   \
                                                                                            \
	static inline void cxl_set_##name(struct name *row, union name##_copy *copy)        \
	{                                                                                   \
		FIELDS(CXL_VIEW_ENCODE_FIELD, CXL_VIEW_ENCODE_ARRAY, CXL_VIEW_ENCODE_BYTES) \
		cxl_set_cacheline(row, &copy->line);                                        \
	}

#endif /* _CXL_IO_H */
