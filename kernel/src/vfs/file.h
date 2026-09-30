/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * file.h - VFS file_operations / address_space_operations exports, and mapping revocation.
 */

#ifndef _FS_FILE_H
#define _FS_FILE_H

#include <linux/fs.h>
#include <linux/types.h>

#include "core.h"

extern const struct file_operations file_fops;
extern const struct address_space_operations file_aops;

/*
 * Take live mappings of @sbi down, so a holder's next access faults instead of reaching CXL. What
 * the fault then answers is decided elsewhere: a fenced mount SIGBUSes it in file_data_fault.
 *
 * The whole mount, or one process's share of it. The second is what a withdrawn grant wants, since
 * taking a grant back leaves the pages it authorised sitting in that process's page table.
 */
void file_revoke_all_mappings(struct fs_sb_info *sbi);
s32 file_revoke_task_mappings(struct fs_sb_info *sbi, pid_t tgid);

#endif /* _FS_FILE_H */
