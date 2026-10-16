/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * bitmap.h - bit operations on the masks this module keeps, one bit per slot.
 *
 * A mask of up to 32 slots is a u32. A longer one is an array of u64 words with slot i at bit
 * i % 64 of word i / 64, as the medium stores it. Unlike <linux/bitmap.h>, no word is unsigned long.
 */

#ifndef _UTIL_BITMAP_H
#define _UTIL_BITMAP_H

#include <linux/bits.h>
#include <linux/types.h>

enum {
	BITS_WORD_WIDTH = BITS_PER_TYPE(u64),
	BITS_MASK_WIDTH = BITS_PER_TYPE(u32),
};

/* The lowest set slot of @bits, cleared from it, so a loop over a copy of a mask visits each slot
 * once. @bits must not be 0. */
static inline u32 bits_take_lowest(u32 *bits)
{
	const u32 idx = (u32)__builtin_ctz(*bits);
	*bits &= *bits - 1;
	return idx;
}

/* The lowest slot below @width that @bits leaves clear, or @width when every one is set. */
static inline u32 bits_find_lowest_clear(u32 bits, u32 width)
{
	const u32 slots = width < BITS_MASK_WIDTH ? (1U << width) - 1 : ~0U;
	const u32 clear = ~bits & slots;
	return clear ? (u32)__builtin_ctz(clear) : width;
}

static inline bool bits_has(const u64 *words, u32 idx)
{
	return (words[idx / BITS_WORD_WIDTH] >> (idx % BITS_WORD_WIDTH)) & 1U;
}

static inline void bits_set(u64 *words, u32 idx)
{
	words[idx / BITS_WORD_WIDTH] |= 1ULL << (idx % BITS_WORD_WIDTH);
}

static inline void bits_clear(u64 *words, u32 idx)
{
	words[idx / BITS_WORD_WIDTH] &= ~(1ULL << (idx % BITS_WORD_WIDTH));
}

/* The first slot at or past @from and below @count whose bit, XORed with @flip, reads set. */
static inline u32 __bits_find_flipped(const u64 *words, u32 count, u32 from, u64 flip)
{
	for (u32 idx = from; idx < count; idx += BITS_WORD_WIDTH - (idx % BITS_WORD_WIDTH)) {
		const u32 word_at = idx / BITS_WORD_WIDTH;
		/* Bits below @idx in its own word are behind us. */
		const u64 word = (words[word_at] ^ flip) & (~0ULL << (idx % BITS_WORD_WIDTH));
		if (word) {
			const u32 found = word_at * BITS_WORD_WIDTH + (u32)__builtin_ctzll(word);
			return found < count ? found : count;
		}
	}
	return count;
}

/* The first set slot at or past @from and below @count, or @count when none is. */
static inline u32 bits_find_next_set(const u64 *words, u32 count, u32 from)
{
	return __bits_find_flipped(words, count, from, 0);
}

/* The first clear slot at or past @from and below @count, or @count when none is. */
static inline u32 bits_find_next_clear(const u64 *words, u32 count, u32 from)
{
	return __bits_find_flipped(words, count, from, ~0ULL);
}

/* Each set slot below @count, lowest first, as @idx, which the loop declares. */
#define bits_for_each_set(idx, words, count)                                   \
	for (u32 idx = bits_find_next_set((words), (count), 0); idx < (count); \
	     idx = bits_find_next_set((words), (count), idx + 1))

#endif /* _UTIL_BITMAP_H */
