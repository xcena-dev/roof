// SPDX-License-Identifier: GPL-2.0-only
/*
 * io.c - Whole-cacheline load and store on CXL shared memory.
 *
 * The two directions do not use the same instruction. Reading takes a zmm move and so needs the
 * FPU section the kernel requires around the vector registers. Writing takes MOVDIR64B, which
 * uses general registers and carries the single 64-byte-operation guarantee a zmm store does not.
 */

#include <asm/cpufeature.h>
#include <asm/fpu/api.h>
#include <asm/pgtable.h>
#include <linux/align.h>
#include <linux/printk.h>
#include <linux/string.h>

#include "cxl/io.h"

/* ── Private to this file ─────────────────────────────────────────── */

/*
 * Both instructions below live in the template rather than behind -mavx512f or -mmovdir64b: the
 * assembler knows the mnemonics and the compiler needs no knowledge of them. zmm0 is left out of
 * the clobber list on purpose, because naming it would demand the AVX-512 target and a kernel
 * built -mno-avx never holds a value there for us to destroy.
 */

/* One 64-byte write, and weakly ordered, so the caller's write barrier supplies the fence. */
static void cxl_store_cacheline_movdir64b(void *dst, const void *src)
{
	asm volatile("movdir64b (%1), %0" : : "r"(dst), "r"(src) : "memory");
}

/*
 * Moves the line either way through zmm0. True when it ran, false where the caller has to
 * fall back to bytes.
 *
 * The FPU section is opened per line, so a caller walking many of them pays its entry cost
 * each time. It also disables softirq processing, so nothing between begin and end may
 * sleep, fault, or take long. irq_fpu_usable() is false in the contexts where kernel-mode
 * FPU is not allowed at all.
 */
static bool cxl_copy_cacheline_avx512(void *dst, const void *src)
{
	if (!cpu_feature_enabled(X86_FEATURE_AVX512F) || !irq_fpu_usable())
		return false;

	kernel_fpu_begin();
	asm volatile("vmovdqu64 (%1), %%zmm0\n\t"
		     "vmovdqu64 %%zmm0, (%0)\n\t"
		     "vzeroupper"
		     :
		     : "r"(dst), "r"(src)
		     : "memory");
	kernel_fpu_end();
	return true;
}

/*
 * A misaligned line still copies correctly but straddles two of them, which
 * costs a second transaction and lets a peer observe half a write. Nothing
 * downstream reports that, so it is caught here.
 */
static bool cxl_is_cacheline_aligned(const void *line)
{
	return !WARN_ON_ONCE(!IS_ALIGNED((unsigned long)line, CXL_LINE_BYTES));
}

/*
 * Three ways down, in the order of what they promise. MOVDIR64B is the one 64-byte operation the
 * architecture guarantees. A zmm store is one instruction and usually one transaction, but
 * nothing promises a peer cannot observe it half done. The byte copy promises neither and is what
 * is left where the FPU is unavailable. No fence here, so a caller writing many lines pays one.
 */
static void cxl_store_cacheline(void *line, const void *src)
{
	if (cpu_feature_enabled(X86_FEATURE_MOVDIR64B))
		cxl_store_cacheline_movdir64b(line, src);
	else if (!cxl_copy_cacheline_avx512(line, src))
		memcpy(line, src, CXL_LINE_BYTES);
}

/* ── What the rest of the tree calls ──────────────────────────────── */

bool cxl_is_mapping_uncached(const void __iomem *addr)
{
	unsigned int level;
	pte_t *pte = lookup_address((unsigned long)addr, &level);

	return pte && (pte_val(*pte) & _PAGE_PCD);
}

/* Fences before the load, so a caller reads the line rather than repeating the barrier. */
void cxl_get_cacheline(struct cxl_cacheline *copy, const void *line)
{
	if (!cxl_is_cacheline_aligned(line))
		return;

	CXL_RMB(line, CXL_LINE_BYTES);
	if (!cxl_copy_cacheline_avx512(copy->byte, line))
		memcpy(copy->byte, line, CXL_LINE_BYTES);
}

/* Fences after the store, for the same reason the read fences before its load. */
void cxl_set_cacheline(void *line, const struct cxl_cacheline *copy)
{
	if (!cxl_is_cacheline_aligned(line))
		return;

	cxl_store_cacheline(line, copy->byte);
	CXL_WMB(line, CXL_LINE_BYTES);
}

/*
 * MOVDIR64B is the only store here a loop can take, because the zmm path opens an FPU section per
 * line and disables softirqs inside it. So a host without MOVDIR64B clears the range in bytes,
 * and loses nothing by it: an area being cleared is an area nobody reads.
 */
void cxl_zero_cachelines(void *base, u64 bytes)
{
	if (!cxl_is_cacheline_aligned(base) || WARN_ON_ONCE(bytes % CXL_LINE_BYTES))
		return;

	if (cpu_feature_enabled(X86_FEATURE_MOVDIR64B)) {
		const struct cxl_cacheline zero = {};
		for (u64 done = 0; done < bytes; done += CXL_LINE_BYTES)
			cxl_store_cacheline_movdir64b((char *)base + done, zero.byte);
	} else {
		memset(base, 0, bytes);
	}

	CXL_WMB(base, bytes);
}
