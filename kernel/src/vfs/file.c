// SPDX-License-Identifier: GPL-2.0-only
/* file.c - file operations (open, read, mmap, ioctl) */

#include <linux/bitops.h>
#include <linux/dax.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/highmem.h>
#include <linux/huge_mm.h>
#include <linux/io.h>
#include <linux/iomap.h>
#include <linux/mman.h>
#include <linux/pagemap.h>
#include <linux/pgtable.h>
#include <linux/prefetch.h>
#include <linux/uio.h>
#include <linux/writeback.h>

#include "access/acl.h"
#include "access/daemon.h"
#include "access/meta_lock.h"
#include "compat.h"
#include "core.h"
#include "cxl/io.h"
#include "layout/layout_access.h"
#include "region/refs.h"
#include "region/region.h"
#include "vfs/file.h"
#include "vfs/inode.h"

/* How long a release waits for the turn before it leaves an unlinked entry to the sweep. */
enum file_config {
	FILE_FREE_LOCK_WAIT_MS = 50,
};

/* vma hardening: block fork inherit, mremap grow, coredump leak */
#define FILE_VMA_HARDEN_FLAGS (VM_DONTCOPY | VM_DONTEXPAND | VM_DONTDUMP)

/* A bare fd survives execve, so a re-exec of the same binary reaches the data paths through it.
 * The check runs at access time because .open has not installed the fd yet, and it asks about every
 * descriptor the caller holds on the file, since dup2 clears the bit on the copy it makes.
 * The walk is current->files, bounded by RLIMIT_NOFILE, so a caller that opens many slows itself. */

/* The hint is an index + 1 in file->private_data, so an empty pointer is no hint. Nothing else in
 * this module uses that field of a regular file. */
static u32 file_get_fd_hint(const struct file *file)
{
	return (u32)(uintptr_t)READ_ONCE(file->private_data);
}

static void file_set_fd_hint(struct file *file, u32 idx)
{
	WRITE_ONCE(file->private_data, (void *)(uintptr_t)(idx + 1));
}

/* Whether @word's set bits hold @file, starting at @base. The hint is written on a match so the
 * caller's next call can answer the same question without reaching this. */
static bool file_find_slot_in_word(struct file *file, struct fdtable *fdt, u32 base, unsigned long word)
{
	while (word) {
		const u32 idx = base + (u32)__ffs(word);

		word &= word - 1;
		if (idx < fdt->max_fds && rcu_dereference_raw(fdt->fd[idx]) == file) {
			file_set_fd_hint(file, idx);
			return true;
		}
	}
	return false;
}

/* Whether @word's set bits hold @file at a slot without close-on-exec. The bitmap only narrows the
 * search, and a surviving slot's bit is read through the usual accessor. */
static bool file_find_bare_in_word(struct file *file, struct files_struct *files, struct fdtable *fdt,
				   u32 base, unsigned long word)
{
	while (word) {
		const u32 idx = base + (u32)__ffs(word);

		word &= word - 1;
		if (idx < fdt->max_fds && rcu_dereference_raw(fdt->fd[idx]) == file &&
		    !compat_close_on_exec(idx, files, fdt))
			return true;
	}
	return false;
}

/* Whether the caller holds @file and marked every descriptor it holds it at. One pass answers both,
 * since the bare bits are a subset of the open ones.
 *
 * The hint settles the first question outright, which is what keeps a repeated call off the length
 * of the table: the bare bits are none of them once a caller marks what it opens, while the open
 * bits are all of them. */
static bool file_require_cloexec(struct file *file)
{
	struct files_struct *files = current->files;
	if (!files)
		return false;

	rcu_read_lock();

	struct fdtable *fdt = files_fdtable(files);
	const u32 hint = file_get_fd_hint(file);
	bool held = hint != 0 && hint - 1 < fdt->max_fds && rcu_dereference_raw(fdt->fd[hint - 1]) == file;
	bool bare = false;
	const u32 words = (fdt->max_fds + BITS_PER_LONG - 1) / BITS_PER_LONG;

	for (u32 word = 0; word < words && !bare; word++) {
		const u32 base = word * (u32)BITS_PER_LONG;
		const unsigned long open = fdt->open_fds[word];

		if (!held)
			held = file_find_slot_in_word(file, fdt, base, open);
		bare = file_find_bare_in_word(file, files, fdt, base, open & ~fdt->close_on_exec[word]);
	}

	rcu_read_unlock();
	return held && !bare;
}

/* The lock region holds one row per node at a slot its node_id names, written from the daemon_uid=
 * mount option. A grant or an upcall row here would take the slot of a node that has yet to mount. */
static bool file_is_lock_region(struct fs_sb_info *sbi, u32 rat_entry_id)
{
	struct layout_rat_entry *rat_entry = layout_get_rat_entry(sbi, rat_entry_id);
	if (!rat_entry)
		return false;

	union layout_rat_hot_copy hot;
	cxl_get_layout_rat_hot(&hot, &rat_entry->hot);
	return hot.local.region_type == LAYOUT_REGION_LOCK;
}

/*
 * Whether the entry still carries the name the caller opened: standing, and not unlinked out from
 * under its holders. The name and not any name, because an inode is keyed on its slot alone, so a
 * slot freed and given to another file between the lookup and this read is the same inode.
 */
static bool file_has_this_name(struct fs_sb_info *sbi, u32 rat_entry_id, const struct dentry *dentry)
{
	struct layout_rat_entry *rat_entry = layout_get_rat_entry(sbi, rat_entry_id);
	if (!rat_entry)
		return false;

	union layout_rat_hot_copy hot;
	cxl_get_layout_rat_hot(&hot, &rat_entry->hot);
	if (!layout_rat_state_is_standing(hot.local.state))
		return false;

	return layout_check_rat_name(sbi, rat_entry_id, dentry->d_name.name, dentry->d_name.len);
}

/* Standing with its name gone, which only an unlink leaves behind. */
static bool file_is_unlinked(struct layout_rat_entry *entry)
{
	union layout_rat_hot_copy hot;
	cxl_get_layout_rat_hot(&hot, &entry->hot);
	if (hot.local.state != LAYOUT_RAT_ENTRY_ALLOCATED)
		return false;

	struct cxl_cacheline line;
	return layout_read_rat_name(entry, &line)[0] == '\0';
}

/*
 * Frees an unlinked entry whose last reference on this node just went, when no other node holds
 * one. The bits are read under the turn, as unlink reads them: a peer opening right now either set
 * its bit first and keeps the entry, or reads the name after this and finds none. A turn not taken
 * within the wait leaves the entry to the owner's sweep.
 */
static void file_free_unlinked(struct fs_sb_info *sbi, u32 rat_entry_id)
{
	struct layout_rat_entry *entry = layout_get_rat_entry(sbi, rat_entry_id);
	if (!entry || !file_is_unlinked(entry))
		return;

	struct meta_lock lock;
	if (!meta_trylock(sbi, &lock, FILE_FREE_LOCK_WAIT_MS, TEST_META_OP_UNLINK))
		return;

	/* Judged again under the turn: a sweep or a peer's release can have freed it in between. */
	if (file_is_unlinked(entry) && !region_has_refs(sbi, rat_entry_id))
		region_free_rat_entry(sbi, entry);

	meta_unlock(sbi, &lock);
}

/*
 * Permission is checked at data access time, not here. What open does is count: a reference on
 * this node is what keeps the entry past an unlink on any node. The lock region is left out, since
 * it is never unlinked and the daemon holds it for the mount's whole life.
 */
static int file_open(struct inode *inode, struct file *file)
{
	s32 ret = generic_file_open(inode, file);
	if (ret)
		return ret;

	struct inode_ctx ctx;
	if (!inode_open_ctx(&ctx, inode))
		return -EIO;
	if (file_is_lock_region(ctx.sbi, ctx.xi->rat_entry_id))
		return 0;
	/* No node id yet means no line to count in, and a reference nobody can see is none. */
	if (fs_is_identity_pending(ctx.sbi))
		return -EAGAIN;

	region_take_ref(ctx.sbi, ctx.xi->rat_entry_id);

	/* The count is raised before the flag is read, and a drain raises the flag before it reads
	 * the counts, so either this open sees the drain or the drain sees this open. */
	smp_mb();
	if (fs_is_draining(ctx.sbi)) {
		if (region_drop_ref(ctx.sbi, ctx.xi->rat_entry_id))
			file_free_unlinked(ctx.sbi, ctx.xi->rat_entry_id);
		return -EBUSY;
	}

	/* The bit is out before the name is read, and unlink writes the name away before it reads
	 * the bits, so an unlink that missed this reference is one this open sees. The unlink that
	 * saw the bit left the entry to it, so the drop frees what that unlink could not. */
	if (!file_has_this_name(ctx.sbi, ctx.xi->rat_entry_id, file_dentry(file))) {
		if (region_drop_ref(ctx.sbi, ctx.xi->rat_entry_id))
			file_free_unlinked(ctx.sbi, ctx.xi->rat_entry_id);
		return -ENOENT;
	}

	/* The reference pins the slot from here, so what the entry holds now is what this open
	 * reads and maps. The lookup's copy can predate a free and a new placement on the slot. */
	inode_read_entry(inode);
	return 0;
}

/* The last reference to the struct file, so after the last mapping over it went as well. */
static int file_release(struct inode *inode, struct file *file)
{
	struct inode_ctx ctx;
	if (!inode_open_ctx(&ctx, inode))
		return 0;
	if (file_is_lock_region(ctx.sbi, ctx.xi->rat_entry_id))
		return 0;
	if (region_drop_ref(ctx.sbi, ctx.xi->rat_entry_id))
		file_free_unlinked(ctx.sbi, ctx.xi->rat_entry_id);
	return 0;
}

/*
 * file_daemon_ask_access - ask this mount's helper what the caller may reach.
 *
 * Sends ACCESS_REQUEST, waits for the answer, and writes a delegation row into the RAT on a grant.
 *
 * Returns 0 if every bit in @required_perms is granted, negative errno otherwise. -ENOSYS is a
 * helper that is not connected, -EAGAIN a timeout or a full queue, and -EACCES a denial.
 */
static int file_daemon_ask_access(struct fs_sb_info *sbi, struct fs_inode_info *xi, u32 required_perms)
{
	/* Defensive NULL guards */
	if (!sbi || !xi)
		return -EIO;

	struct layout_rat_entry *rat_entry = layout_get_rat_entry(sbi, xi->rat_entry_id);
	if (!rat_entry)
		return -EIO;

	union daemon_req_payload req = {};
	daemon_fill_task(DAEMON_REQ_ACCESS, &req);

	/* The owner identity the helper judges against, out of the RAT entry's ACL line. */
	union layout_rat_acl_copy acl;
	cxl_get_layout_rat_acl(&acl, &rat_entry->acl);
	memcpy(req.access.owner_group, acl.local.owner_group, sizeof(acl.local.owner_group));
	memcpy(req.access.owner_role, acl.local.owner_role, sizeof(acl.local.owner_role));

	pr_debug("access upcall enqueue: pid=%d rat_entry=%u\n", current->tgid, xi->rat_entry_id);

	union daemon_resp_payload resp = {};
	s32 ret = daemon_request(sbi, DAEMON_REQ_ACCESS, &req, &resp);
	if (ret) {
		pr_debug("access upcall failed: pid=%d rat_entry=%u ret=%d\n", current->tgid, xi->rat_entry_id, ret);
		return ret;
	}

	pr_debug("access upcall response: pid=%d rat_entry=%u status=%d granted_perms=0x%x\n", current->tgid, xi->rat_entry_id,
		 resp.access.status, resp.access.granted_perms);

	if (resp.access.status != 0)
		return resp.access.status;

	/* Mask granted_perms to known bits — defense against helper bugs sending
	 * unexpected flags that could be reinterpreted by future bits */
	u32 known_mask = FS_PERM_READ | FS_PERM_WRITE | FS_PERM_GRANT | FS_PERM_ADMIN;
	u32 granted = resp.access.granted_perms & known_mask;
	if (granted == 0) {
		pr_debug("access upcall: helper returned no usable perms 0x%x\n", resp.access.granted_perms);
		return -EACCES;
	}

	/* Intersect granted with requested op to enforce least privilege */
	if ((granted & required_perms) != required_perms) {
		pr_debug("access upcall: granted_perms=0x%x insufficient for required=0x%x\n", granted, required_perms);
		return -EACCES;
	}

	/* The row daemon's answer earns, written whole. */
	ret = acl_grant_deleg_caller(sbi, xi->rat_entry_id, granted);
	if (ret) {
		pr_warn("access upcall: deleg_grant failed rat_entry=%u ret=%d\n", xi->rat_entry_id, ret);
		return ret;
	}

	pr_debug("access upcall: delegation written pid=%d rat_entry=%u perms=0x%x\n", current->tgid, xi->rat_entry_id, granted);
	return 0;
}

/*
 * Every bit of @required_perms, from the RAT rows or from the helper that decides whether a row
 * should say so. A row already covering the caller is a decision that has been made, so the
 * helper is asked only where none does.
 */
static s32 file_authorize_or_ask(struct fs_sb_info *sbi, struct fs_inode_info *xi, u32 required_perms)
{
	s32 ret = acl_check_permission(sbi, xi->rat_entry_id, required_perms);
	if (ret != -EACCES)
		return ret;

	/* The lock region admits the helper's own account row and nobody else. The helper is what a
	 * row there admits, so it is not asked to write one. */
	if (file_is_lock_region(sbi, xi->rat_entry_id))
		return -EACCES;

	ret = file_daemon_ask_access(sbi, xi, required_perms);
	/* -ENOSYS would have userspace read the syscall itself as unsupported, and what is missing
	 * is a helper that a retry may find. */
	if (ret == -ENOSYS)
		return -EAGAIN;
	if (ret)
		return ret;

	/* The row the grant earned, read back: the answer is only good once it is on the record the
	 * fault path and every peer read from. */
	return acl_check_permission(sbi, xi->rat_entry_id, required_perms);
}

/*
 * Direct CXL read — bypasses page cache, copies to user buffer.
 *
 * This is a DAX filesystem: primary data access is via mmap (zero-copy).
 * read_iter supports read() for debugging/operational tools (cat, hexdump)
 * while maintaining full permission checks and CXL cache coherence (RMB).
 *
 * Page cache path (read_folio) is intentionally blocked (-EIO) because:
 *  - No permission check interface at folio fill time
 *  - DRAM page cache copies break cross-node CXL coherence
 *  - sendfile/splice are not part of the KV cache access model
 */
static ssize_t file_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct inode_ctx ctx;
	if (!inode_open_ctx(&ctx, file_inode(iocb->ki_filp)))
		return -EIO;

	if (!file_require_cloexec(iocb->ki_filp))
		return -EACCES;

	s32 ret = file_authorize_or_ask(ctx.sbi, ctx.xi, FS_PERM_READ);
	if (ret)
		return ret;

	/*
	 * Cross-node i_size sync: remote ftruncate updates RAT but not our
	 * DRAM i_size (d_revalidate=0 only affects new lookups, not open fds).
	 */
	struct layout_rat_entry *rat_e = layout_get_rat_entry(ctx.sbi, ctx.xi->rat_entry_id);
	if (rat_e) {
		union layout_rat_hot_copy hot;
		cxl_get_layout_rat_hot(&hot, &rat_e->hot);
		if (hot.local.size != (u64)ctx.inode->i_size) {
			inode_lock(ctx.inode);
			i_size_write(ctx.inode, hot.local.size);
			inode_unlock(ctx.inode);
		}

		if (ctx.xi->data_phys_offset == 0 && hot.local.phys_offset != 0)
			ctx.xi->data_phys_offset = hot.local.phys_offset;
	}

	loff_t pos = iocb->ki_pos;
	size_t count = iov_iter_count(to);
	if (pos >= ctx.inode->i_size)
		return 0;
	if (pos + count > ctx.inode->i_size)
		count = ctx.inode->i_size - pos;
	if (count == 0)
		return 0;

	if (unlikely(ctx.xi->data_phys_offset == 0)) {
		pr_debug("read on uninitialized region (ftruncate pending)\n");
		return 0;
	}

	if (unlikely(!layout_is_valid_region_addr(ctx.sbi, ctx.xi->data_phys_offset, pos + count))) {
		pr_err("read range invalid slot_base=0x%llx pos=%lld count=%zu\n", ctx.xi->data_phys_offset, pos, count);
		return -EIO;
	}

	/* The one range this filesystem reads through a write-back mapping, so a peer's write
	 * can sit behind this node's own cached copy of the line. */
	void *data_ptr = layout_get_file_data_ptr(ctx.sbi, ctx.xi->data_phys_offset, pos);
	CXL_RMB_CACHED(data_ptr, count);

	size_t copied = copy_to_iter(data_ptr, count, to);
	if (copied == 0)
		return -EFAULT;

	iocb->ki_pos += copied;
	return copied;
}

/* write() rejected — data writes only via mmap(PROT_WRITE) */
static ssize_t file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	return -EACCES;
}

/*
 * vm_ops wrapper hooks. vma->vm_private_data holds a reference to this
 * filesystem's own file, and everything this module needs comes from there: the
 * inode, and sbi through its superblock.
 *
 * The reference is what keeps the mount alive under a live mapping. An inode
 * pinned on its own does not: umount would tear the superblock down and warn
 * about a busy inode, and this close would then iput into freed memory.
 */

static struct file *file_get_vma_owner(struct vm_area_struct *vma)
{
	return vma->vm_private_data;
}

static void file_vma_open(struct vm_area_struct *vma)
{
	struct file *own = file_get_vma_owner(vma);
	if (own)
		get_file(own);
}

static void file_vma_close(struct vm_area_struct *vma)
{
	struct file *own = file_get_vma_owner(vma);
	if (own)
		fput(own);
}

/* mprotect: block escalation past RAT delegation. */
static int file_vma_mprotect(struct vm_area_struct *vma, unsigned long start, unsigned long end, unsigned long newflags)
{
	if (!(newflags & (VM_READ | VM_WRITE | VM_EXEC)))
		return 0; /* PROT_NONE */

	struct file *own = file_get_vma_owner(vma);
	if (!own || !vma->vm_ops)
		return 0;

	if ((newflags & VM_WRITE) && vma->vm_file && !(vma->vm_file->f_mode & FMODE_WRITE))
		return -EACCES;

	struct inode_ctx ctx;
	if (!inode_open_ctx(&ctx, file_inode(own)))
		return -EIO;

	/* The rows and not the helper: this gate holds a live mapping to the delegation it was
	 * given, and a caller wanting more asks for it by mapping again. */
	u32 required_perms = FS_PERM_READ;
	if (newflags & VM_WRITE)
		required_perms |= FS_PERM_WRITE;

	return acl_check_permission(ctx.sbi, ctx.xi->rat_entry_id, required_perms);
}

/*
 * Lazy-seed sbi->vm_ops from underlying driver's ops, override hooks,
 * point vma at it. @own stashed in vm_private_data with a reference, which is
 * what holds the mount under this mapping; .open/.close keep that count
 * balanced across vma split and clone.
 */
/*
 * The split's data half faults in through its ZONE_DEVICE pages rather than as a raw pfn
 * range, because a GPU DMA registration pins pages and VM_PFNMAP hands GUP none to pin.
 */
static vm_fault_t file_data_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct file *own = vma->vm_private_data;
	if (!own)
		return VM_FAULT_SIGBUS;

	struct inode_ctx ctx;
	if (!inode_open_ctx(&ctx, file_inode(own)) || ctx.xi->data_phys_offset == 0)
		return VM_FAULT_SIGBUS;

	/* The uncached half has no struct pages, and mmap gives what lives there a populated
	 * VM_PFNMAP instead, so no fault on such a region should reach here. */
	if (layout_check_meta_offset(ctx.sbi, ctx.xi->data_phys_offset))
		return VM_FAULT_SIGBUS;

	u64 offset = ((u64)vma->vm_pgoff << PAGE_SHIFT) + (vmf->address - vma->vm_start);
	/* Subtraction for the same reason mmap uses it: a sum here would wrap and pass. */
	const u64 size = (u64)i_size_read(ctx.inode);
	if (offset >= size || size - offset < PAGE_SIZE)
		return VM_FAULT_SIGBUS;

	u64 pfn = (ctx.sbi->phys_base + ctx.xi->data_phys_offset + offset) >> PAGE_SHIFT;
	if (!pfn_valid(pfn))
		return VM_FAULT_SIGBUS;

	/* The page and not the pfn: on an arch with special PTEs, inserting a pfn marks the
	 * entry special, and then vm_normal_page finds nothing for GUP to pin. */
	return compat_vmf_insert_page_mkwrite(vmf, pfn_to_page(pfn), vmf->flags & FAULT_FLAG_WRITE);
}

static s32 file_attach_vm_ops(struct vm_area_struct *vma, struct fs_sb_info *sbi, struct file *own)
{
	mutex_lock(&sbi->vm_ops_lock);
	if (!sbi->vm_ops_seeded) {
		if (vma->vm_ops)
			sbi->vm_ops = *vma->vm_ops;
		else
			memset(&sbi->vm_ops, 0, sizeof(sbi->vm_ops));
		sbi->vm_ops.open = file_vma_open;
		sbi->vm_ops.close = file_vma_close;
		sbi->vm_ops.mprotect = file_vma_mprotect;
		sbi->vm_ops.fault = file_data_fault;
		sbi->vm_ops_seeded = true;
	}
	mutex_unlock(&sbi->vm_ops_lock);

	vma->vm_private_data = get_file(own);
	vma->vm_ops = &sbi->vm_ops;
	return 0;
}

static int file_mmap(struct file *file, struct vm_area_struct *vma)
{
	if (!file_require_cloexec(file))
		return -EACCES;

	/* Reject VM_WRITE on O_RDONLY fd */
	if ((vma->vm_flags & VM_WRITE) && !(file->f_mode & FMODE_WRITE))
		return -EACCES;

	struct inode_ctx ctx;
	if (!inode_open_ctx(&ctx, file_inode(file)))
		return -EIO;

	struct inode *inode = ctx.inode;
	struct fs_sb_info *sbi = ctx.sbi;
	struct fs_inode_info *xi = ctx.xi;

	/* Fail-secure: an inode whose RAT entry is unreachable has no record we
	 * can authorise against, so it must not be mappable. */
	if (!layout_get_rat_entry(sbi, xi->rat_entry_id))
		return -EIO;

	u32 required_perms = FS_PERM_READ;
	if (vma->vm_flags & VM_WRITE)
		required_perms |= FS_PERM_WRITE;

	s32 ret = file_authorize_or_ask(sbi, xi, required_perms);
	if (ret)
		return ret;

	/* The upcall above sleeps to the helper's timeout, so the fence can land while it waits, and
	 * the revoke that follows a fence has then already walked past this inode. */
	if (fs_is_fenced(sbi))
		return -EIO;

	if (xi->data_phys_offset == 0 || inode->i_size == 0)
		return -ENODATA;

	/*
	 * A file region faults in one page at a time, so the mapping keeps the struct pages a
	 * GPU DMA registration has to pin. VM_PFNMAP would hand GUP none.
	 *
	 * The lock region is the exception: it lives in the uncached metadata area, which has
	 * no struct pages to insert, and its users want cross-node visibility rather than DMA.
	 */

	/* Bounds check. Subtraction and not a sum: pgoff is the caller's, so the shift below can
	 * carry past 64 bits and a sum would put a wrapped value up against the size. */
	const u64 size = (u64)i_size_read(inode);
	u64 user_offset = (u64)vma->vm_pgoff << PAGE_SHIFT;
	unsigned long map_size = vma->vm_end - vma->vm_start;
	if (user_offset > size || map_size > size - user_offset)
		return -EINVAL;

	if (unlikely(!layout_is_valid_region_addr(sbi, xi->data_phys_offset, user_offset + map_size)))
		return -EIO;

	if (layout_check_meta_offset(sbi, xi->data_phys_offset)) {
		phys_addr_t phys = sbi->phys_base + xi->data_phys_offset + user_offset;

		vm_flags_set(vma, VM_PFNMAP | FILE_VMA_HARDEN_FLAGS);
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

		ret = remap_pfn_range(vma, vma->vm_start, phys >> PAGE_SHIFT, map_size, vma->vm_page_prot);
		if (ret) {
			pr_err("remap_pfn_range failed: %d\n", ret);
			return ret;
		}

		pr_debug("mmap uncached inode=%lu phys=0x%llx\n", inode->i_ino, (u64)phys);
	} else {
		vm_flags_set(vma, VM_MIXEDMAP | FILE_VMA_HARDEN_FLAGS);
		pr_debug("mmap inode=%lu pgoff=%lu\n", inode->i_ino, vma->vm_pgoff);
	}

	return file_attach_vm_ops(vma, sbi, file);
}

static loff_t file_llseek(struct file *file, loff_t offset, int whence)
{
	return generic_file_llseek(file, offset, whence);
}

/*
 * Page cache path not supported — DAX FS uses mmap for data access
 * and read_iter for read(). sendfile/splice will get -EIO.
 */
static int file_read_folio(struct file *file, struct folio *folio)
{
	folio_unlock(folio);
	return -EIO;
}

const struct address_space_operations file_aops = {
	.read_folio = file_read_folio,
	.dirty_folio = filemap_dirty_folio,
};

/* ============================================================================
 * Mapping revocation
 *
 * Refusing an operation reaches only a caller that asks. A process holding a mapping asks nobody:
 * the metadata half is populated at mmap by remap_pfn_range and never faults, and a data page stays
 * in the page table once faulted. Clearing the PTEs is what ends those stores.
 *
 * Not mf_dax_kill_procs, which the dax failure path uses: that marks the pages poisoned and
 * unusable for the rest of the boot, and these pages are sound and wanted by whoever holds the
 * slot now. Nor does anything here signal. The refault does that, through file_data_fault.
 * ============================================================================ */

void file_revoke_all_mappings(struct fs_sb_info *sbi)
{
	struct super_block *sb = sbi->sb;
	if (sb == NULL)
		return;

	u32 revoked = 0;
	struct inode *held = NULL;

	spin_lock(&sb->s_inode_list_lock);

	struct inode *inode;
	list_for_each_entry(inode, &sb->s_inodes, i_sb_list) {
		spin_lock(&inode->i_lock);
		if (inode->i_state & (I_NEW | I_FREEING | I_WILL_FREE)) {
			spin_unlock(&inode->i_lock);
			continue;
		}
		compat_iget(inode);
		spin_unlock(&inode->i_lock);

		/* The unmap and the iput below can both sleep, so the list lock goes first. The
		 * reference just taken is what keeps this inode's place in the list meanwhile. */
		spin_unlock(&sb->s_inode_list_lock);

		/* even_cows, so a MAP_PRIVATE reader's copies go with the shared pages. */
		unmap_mapping_range(inode->i_mapping, 0, 0, 1);
		revoked++;

		/* Held one iteration longer, since dropping it before the walk resumes would let
		 * this inode leave the list and take the cursor with it. */
		iput(held);
		held = inode;

		spin_lock(&sb->s_inode_list_lock);
	}

	spin_unlock(&sb->s_inode_list_lock);
	iput(held);

	pr_warn("node %u: %u inode mapping(s) revoked\n", sbi->node_id, revoked);
}

s32 file_revoke_task_mappings(struct fs_sb_info *sbi, pid_t tgid)
{
	struct pid *pid = acl_find_global_pid((u32)tgid);
	if (pid == NULL)
		return -ESRCH;

	struct task_struct *task = get_pid_task(pid, PIDTYPE_TGID);
	put_pid(pid);
	if (task == NULL)
		return -ESRCH;

	struct mm_struct *mm = get_task_mm(task);
	put_task_struct(task);
	if (mm == NULL)
		return -ESRCH; /* a kernel thread, or one already past exit_mm */

	u32 revoked = 0;

	mmap_write_lock(mm);

	VMA_ITERATOR(iter, mm, 0);
	struct vm_area_struct *vma;
	for_each_vma(iter, vma) {
		/* Every vma this module attaches points at that one copy, so the test names this
		 * mount and not merely this filesystem. */
		if (vma->vm_ops != &sbi->vm_ops)
			continue;

		compat_zap_vma(vma);
		revoked++;
	}

	mmap_write_unlock(mm);
	mmput(mm);

	pr_info("node %u: %u mapping(s) of pid %d revoked\n", sbi->node_id, revoked, tgid);
	return (s32)revoked;
}

/* ── ioctl handlers: the ones that take fs_perm_req ──────────────────── */
/* Each unpacks the record and nothing more. Screening the fields and logging the outcome belong
 * beside the write, so a caller reaching acl_* another way gets both. */
static s32 file_ioctl_perm_grant(struct fs_sb_info *sbi, struct fs_inode_info *xi, struct fs_perm_req *preq)
{
	if (file_is_lock_region(sbi, xi->rat_entry_id))
		return -EPERM;

	return acl_grant_deleg_account(sbi, xi->rat_entry_id, preq->uid, preq->gid, preq->perms);
}

static s32 file_ioctl_perm_revoke(struct fs_sb_info *sbi, struct fs_inode_info *xi, struct fs_perm_req *preq)
{
	if (file_is_lock_region(sbi, xi->rat_entry_id))
		return -EPERM;

	return acl_revoke_deleg_account(sbi, xi->rat_entry_id, preq->uid, preq->gid);
}

/* No lock-region guard here, unlike the two above: the default names no slot, and the daemon's own
 * formatter moves it on the region it is about to serve. */
static s32 file_ioctl_perm_set_default(struct fs_sb_info *sbi, struct fs_inode_info *xi, struct fs_perm_req *preq)
{
	return acl_set_default_perms(sbi, xi->rat_entry_id, preq->perms);
}

/*
 * The question mmap and read ask, asked ahead of them. mmap(2) holds the caller's mmap_lock while
 * it runs, so a daemon it waits on there stalls every fault of that process. A caller that asks
 * here first finds its row written by the time it maps, and waits with nothing held.
 */
static s32 file_ioctl_perm_ask(struct fs_sb_info *sbi, struct fs_inode_info *xi, struct fs_perm_req *preq)
{
	const u32 asked = preq->perms & (FS_PERM_READ | FS_PERM_WRITE);
	if (asked == 0 || asked != preq->perms)
		return -EINVAL;

	return file_authorize_or_ask(sbi, xi, asked);
}

/* Which pool the placement is to take. Refused once there is a region, because the pool a region
 * sits in is the bytes it sits on. */
static s32 file_ioctl_cache_set(struct fs_inode_info *xi, const struct fs_cache_req *req)
{
	if (req->policy != FS_CACHE_WRITEBACK && req->policy != FS_CACHE_UNCACHED)
		return -EINVAL;
	if (xi->data_phys_offset != 0)
		return -EBUSY;

	xi->want_uncached = req->policy == FS_CACHE_UNCACHED;
	return 0;
}

static s32 file_ioctl_cache_get(struct fs_sb_info *sbi, const struct fs_inode_info *xi,
				struct fs_cache_req *req)
{
	bool uncached = xi->data_phys_offset != 0 ? layout_check_meta_offset(sbi, xi->data_phys_offset) :
						    xi->want_uncached;

	req->policy = uncached ? FS_CACHE_UNCACHED : FS_CACHE_WRITEBACK;
	req->reserved = 0;
	return 0;
}

/* Required perm for an ioctl precheck. Zero where none belongs here: the permission writers check
 * next to their own write, the ask is itself the check, and the pool query names only the pool. */
static u16 file_ioctl_required_perm(unsigned int cmd)
{
	switch (cmd) {
	case FS_IOC_PERM_GRANT:
	case FS_IOC_PERM_REVOKE:
	case FS_IOC_PERM_SET_DEFAULT:
	case FS_IOC_PERM_ASK:
	case FS_IOC_CACHE_GET:
		return 0;
	default:
		return FS_PERM_IOCTL;
	}
}

static long file_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	if (!file_require_cloexec(file)) {
		pr_warn_ratelimited("ioctl 0x%x refused: the fd is not FD_CLOEXEC\n", cmd);
		return -EACCES;
	}

	struct inode_ctx ctx;
	if (!inode_open_ctx(&ctx, file_inode(file)))
		return -EIO;

	union {
		struct fs_perm_req perm;
		struct fs_cache_req cache;
		__u32 node_id;
	} payload;

	size_t req_size = _IOC_SIZE(cmd);
	if (req_size > sizeof(payload))
		return -ENOTTY;
	if (copy_from_user(&payload, (void __user *)arg, req_size))
		return -EFAULT;

	u16 req_perm = file_ioctl_required_perm(cmd);
	s32 ret = 0;
	if (req_perm) {
		ret = acl_check_permission(ctx.sbi, ctx.xi->rat_entry_id, req_perm);
		if (ret) {
			pr_warn_ratelimited("ioctl 0x%x on rat=%u wants perm 0x%x: %d\n", cmd, ctx.xi->rat_entry_id, req_perm, ret);
			return ret;
		}
	}

	switch (cmd) {
	case FS_IOC_PERM_GRANT:
		ret = file_ioctl_perm_grant(ctx.sbi, ctx.xi, &payload.perm);
		break;

	case FS_IOC_PERM_REVOKE:
		ret = file_ioctl_perm_revoke(ctx.sbi, ctx.xi, &payload.perm);
		break;

	case FS_IOC_PERM_SET_DEFAULT:
		ret = file_ioctl_perm_set_default(ctx.sbi, ctx.xi, &payload.perm);
		break;

	case FS_IOC_PERM_ASK:
		ret = file_ioctl_perm_ask(ctx.sbi, ctx.xi, &payload.perm);
		break;

	case FS_IOC_CACHE_SET:
		ret = file_ioctl_cache_set(ctx.xi, &payload.cache);
		break;

	case FS_IOC_CACHE_GET:
		ret = file_ioctl_cache_get(ctx.sbi, ctx.xi, &payload.cache);
		break;

	default:
		return -ENOTTY;
	}

	/* Centralized copy_to_user for _IOWR ioctls */
	if (ret == 0 && (_IOC_DIR(cmd) & _IOC_READ))
		if (copy_to_user((void __user *)arg, &payload, req_size))
			ret = -EFAULT;

	return ret;
}

const struct file_operations file_fops = {
	.owner = THIS_MODULE,
	.llseek = file_llseek,
	.read_iter = file_read_iter,
	.write_iter = file_write_iter,
	.mmap = file_mmap,
	.open = file_open,
	.release = file_release,
	.fsync = noop_fsync,
	.unlocked_ioctl = file_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};
