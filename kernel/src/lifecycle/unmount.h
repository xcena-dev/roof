/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * unmount.h - the clean step a departing node asks for while its daemon is still up.
 */

#ifndef _FS_UNMOUNT_H
#define _FS_UNMOUNT_H

#include <linux/types.h>

struct fs_sb_info;

/* Closes open and create, then clears every row and reference this node holds on the medium. 0
 * also covers a node that has nothing of its own to clear. -EBUSY and -EAGAIN can be retried. */
s32 unmount_prepare(struct fs_sb_info *sbi);

#endif /* _FS_UNMOUNT_H */
