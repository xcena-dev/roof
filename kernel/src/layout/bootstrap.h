/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * bootstrap.h - the bootstrap slot area, as it sits on the medium.
 *
 * The area is a node-election table of one slot per node, and slot i belongs to node_id i+1.
 * A slot also carries its owner's liveness stamp, which decides both slot reuse and which node
 * holds the admin role.
 */

#ifndef _LAYOUT_BOOTSTRAP_H
#define _LAYOUT_BOOTSTRAP_H

#include <linux/types.h>

#include "cxl/io.h" /* the whole-line views this row is defined through */
#include "uapi.h" /* FS_MAX_NODE_ID, which is how many slots there are */

/* One slot per node that may claim one, sized so the whole area is 512B. */
enum layout_bootstrap_config {
	LAYOUT_BOOTSTRAP_SLOT_SIZE = 64,
	LAYOUT_BOOTSTRAP_MAX_SLOTS = FS_MAX_NODE_ID,
	LAYOUT_BOOTSTRAP_AREA_SIZE = LAYOUT_BOOTSTRAP_SLOT_SIZE * LAYOUT_BOOTSTRAP_MAX_SLOTS,
};

/* Whether a slot is anybody's, and if so whether its owner still answers for it. */
enum layout_bootstrap_state {
	LAYOUT_BOOTSTRAP_STATE_FREE = 0, /* claimable, and what a zeroed device reads as */
	LAYOUT_BOOTSTRAP_STATE_HELD = 1,
	LAYOUT_BOOTSTRAP_STATE_RECOVERING = 2, /* held, and an admin is clearing its rows */
};

/*
 * A slot is one line, so a claim publishes every field of it in one transaction. No peer can
 * therefore see a token without the stamp that says whether its holder still runs, and staking a
 * recovery cannot land half-written.
 *
 * @state says whether the slot is anybody's and @token only tells two claimants apart, so no reader
 * has to combine them. FREE goes on last, once the cleanup @recoverer names has finished: a scan
 * skips a free slot, so freeing one early would leave rows nothing comes back for.
 */
#define BOOTSTRAP_SLOT_FIELDS(FIELD, ARRAY, BYTES)                                      \
	FIELD(32, magic) /* LAYOUT_BOOTSTRAP_MAGIC once the slot is in use */           \
	FIELD(32, state) /* enum layout_bootstrap_state */                              \
	FIELD(64, token) /* per-claim nonce, and what a claim race is settled on */     \
	FIELD(64, heartbeat) /* wall clock at the owner's last tick */                  \
	FIELD(32, recoverer) /* node_id clearing this slot's rows, 0 while nobody is */ \
	BYTES(36, reserved0)

CXL_DEFINE_LINE_VIEWS(bootstrap_slot, BOOTSTRAP_SLOT_FIELDS)

#endif /* _LAYOUT_BOOTSTRAP_H */
