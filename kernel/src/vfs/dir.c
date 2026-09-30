// SPDX-License-Identifier: GPL-2.0-only
/*
 * dir.c - reading the one flat directory
 *
 * The root holds every file across every region and there are no subdirectories, so readdir walks
 * the RAT slots rather than the far larger index. Resolving a name to a file is namei.c.
 */

#include <linux/fs.h>
#include <linux/string.h>

#include "core.h"
#include "layout/layout_access.h"
#include "vfs/inode.h"

/* ============================================================================
 * dir_iterate - scan all shards for readdir
 * ============================================================================ */

static int dir_iterate(struct file *file, struct dir_context *ctx)
{
	/* "." entry */
	if (ctx->pos == 0) {
		if (!dir_emit_dot(file, ctx))
			return 0;
		ctx->pos = 1;
	}

	/* ".." entry */
	if (ctx->pos == 1) {
		if (!dir_emit_dotdot(file, ctx))
			return 0;
		ctx->pos = 2;
	}

	/*
	* RAT-based readdir: scan 256 RAT entries instead of 64*16384 index entries.
	*
	* ctx->pos encoding:
	*   0 = "."
	*   1 = ".."
	*   2 + i = RAT entry[i]  (i = 0..255)
	*
	* On VFS re-entry after buffer full, pos resumes from the next RAT slot.
	*/
	/* Not ctx: the readdir cursor this function was handed already owns that name. */
	struct inode_ctx dir;
	if (!inode_open_ctx(&dir, file_inode(file)))
		return -EIO;

	for (u32 i = ctx->pos - 2; i < LAYOUT_MAX_RAT_ENTRIES; i++) {
		/* Through the accessor and not the array: reaching the RAT in one place is what lets
		 * a single check decide whether it may be read at all. */
		struct layout_rat_entry *rat_e = layout_get_rat_entry(dir.sbi, i);
		if (!rat_e)
			break;

		/* OWNER_DEAD is listed: a name nobody can see is a name nobody can revoke a row on. */
		union layout_rat_hot_copy hot;
		cxl_get_layout_rat_hot(&hot, &rat_e->hot);
		if (!layout_rat_state_is_standing(hot.local.state))
			continue;

		struct cxl_cacheline line;
		const char *name_buf = layout_read_rat_name(rat_e, &line);

		u32 name_len = strnlen(name_buf, FS_NAME_MAX);
		if (name_len == 0)
			continue;

		unsigned long ino = inode_make_ino(i);

		if (!dir_emit(ctx, name_buf, name_len, ino, DT_REG))
			return 0;

		ctx->pos = i + 3; /* next iteration resumes from i+1 */
	}

	return 0;
}

/* ============================================================================
 * Operations tables
 * ============================================================================ */

const struct file_operations dir_fops = {
	.owner = THIS_MODULE,
	.llseek = generic_file_llseek,
	.read = generic_read_dir,
	.iterate_shared = dir_iterate,
	.fsync = noop_fsync,
};
