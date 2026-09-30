/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * refs.h - the reference table: which RAT entries each node holds a struct file reference on.
 *
 * One line per node, one bit per entry. A node's own count lives in its memory, and the bit says
 * only whether that count is above zero, which is all another node ever asks. Every line has one
 * writer, so no two nodes ever store into the same line. Row 0 is unused, since node ids start at 1.
 */

#ifndef _LAYOUT_REFS_H
#define _LAYOUT_REFS_H

#include <linux/types.h>

#include "cxl/io.h"
#include "layout/rat.h" /* LAYOUT_MAX_RAT_ENTRIES */
#include "uapi.h" /* FS_MAX_NODE_ID */

enum layout_refs_config {
	LAYOUT_REFS_WORD_BITS = 64,
	LAYOUT_REFS_WORDS = LAYOUT_MAX_RAT_ENTRIES / LAYOUT_REFS_WORD_BITS,
	LAYOUT_REFS_RESERVED = CXL_LINE_BYTES - LAYOUT_REFS_WORDS * sizeof(u64),
	LAYOUT_REFS_ROWS = FS_MAX_NODE_ID + 1,
	LAYOUT_REFS_SIZE = LAYOUT_REFS_ROWS * CXL_LINE_BYTES,
};

_Static_assert(LAYOUT_MAX_RAT_ENTRIES % LAYOUT_REFS_WORD_BITS == 0,
	       "a node's line covers the entries in whole words");
_Static_assert(LAYOUT_REFS_WORDS * sizeof(u64) <= CXL_LINE_BYTES, "a node's bits must fit one line");

#define LAYOUT_REFS_FIELDS(FIELD, ARRAY, BYTES) \
	ARRAY(64, LAYOUT_REFS_WORDS, held)      \
	BYTES(LAYOUT_REFS_RESERVED, reserved)

CXL_DEFINE_LINE_VIEWS(layout_refs_line, LAYOUT_REFS_FIELDS)

#endif /* _LAYOUT_REFS_H */
