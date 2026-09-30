/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * compat.h - kernel version compatibility shim
 *
 * Consolidates all LINUX_VERSION_CODE checks.
 * Covers kernel API changes from 5.x through 6.5+.
 */

#ifndef _FS_COMPAT_H
#define _FS_COMPAT_H

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/version.h>

/* VFS idmap parameter abstraction (5.12, 6.3) */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
#define COMPAT_IDMAP_PARAM_COMMA struct mnt_idmap *idmap,
#define COMPAT_IDMAP_ARG_COMMA idmap,
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
#define COMPAT_IDMAP_PARAM_COMMA struct user_namespace *mnt_userns,
#define COMPAT_IDMAP_ARG_COMMA mnt_userns,
#else
#define COMPAT_IDMAP_PARAM_COMMA /* empty */
#define COMPAT_IDMAP_ARG_COMMA /* empty */
#endif

/*
 * generic_fillattr() wrapper — inline function to avoid preprocessor
 * argument counting issues with COMPAT_IDMAP_ARG_COMMA trailing comma macro.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static inline void compat_fill_attr(struct mnt_idmap *idmap, u32 req_mask, struct inode *inode, struct kstat *stat)
{
	generic_fillattr(idmap, req_mask, inode, stat);
}
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
static inline void compat_fill_attr(struct user_namespace *mnt_userns, u32 req_mask, struct inode *inode, struct kstat *stat)
{
	generic_fillattr(mnt_userns, inode, stat);
}
#else
static inline void compat_fill_attr(u32 req_mask, struct inode *inode, struct kstat *stat)
{
	generic_fillattr(inode, stat);
}
#endif

/* setattr_prepare() wrapper */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static inline int compat_prepare_setattr(struct mnt_idmap *idmap, struct dentry *dentry, struct iattr *attr)
{
	return setattr_prepare(idmap, dentry, attr);
}
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
static inline int compat_prepare_setattr(struct user_namespace *mnt_userns, struct dentry *dentry, struct iattr *attr)
{
	return setattr_prepare(mnt_userns, dentry, attr);
}
#else
static inline int compat_prepare_setattr(struct dentry *dentry, struct iattr *attr)
{
	return setattr_prepare(dentry, attr);
}
#endif

/* SLAB_MEM_SPREAD removed in 6.8 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
#define COMPAT_SLAB_MEM_SPREAD 0
#else
#define COMPAT_SLAB_MEM_SPREAD SLAB_MEM_SPREAD
#endif

/* set_page_dirty() removed in 6.8, replaced by folio_mark_dirty() */
static inline void compat_set_page_dirty(struct page *page)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
	folio_mark_dirty(page_folio(page));
#else
	set_page_dirty(page);
#endif
}

/* d_revalidate() signature changed in 6.12:
 *   old: int (*)(struct dentry *, unsigned int)
 *   new: int (*)(struct inode *, const struct qstr *, struct dentry *, unsigned int)
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
#define COMPAT_D_REVALIDATE_ARGS struct inode *dir, const struct qstr *name, struct dentry *dentry, unsigned int flags
#else
#define COMPAT_D_REVALIDATE_ARGS struct dentry *dentry, unsigned int flags
#endif

/* s_d_op: use set_default_d_op() on 6.17+ (direct __s_d_op write
 * skips DCACHE_OP_REVALIDATE flag), fall back to direct s_d_op on older. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
#define COMPAT_SET_D_OP(sb, ops) set_default_d_op(sb, ops)
#else
#define COMPAT_SET_D_OP(sb, ops) ((sb)->s_d_op = (ops))
#endif

/* call_mmap() renamed to vfs_mmap() in 6.17 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
#define compat_call_mmap(file, vma) vfs_mmap(file, vma)
#else
#define compat_call_mmap(file, vma) call_mmap(file, vma)
#endif

/* close_on_exec() takes the files_struct from 6.11 and the fdtable before it, so a caller holds
 * both and lets this pick. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
#define compat_close_on_exec(fd, files, fdt) close_on_exec(fd, files)
#else
#define compat_close_on_exec(fd, files, fdt) close_on_exec(fd, fdt)
#endif

/* The unaligned accessors live under linux/ from 6.12 and under asm/ before it. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
#include <linux/unaligned.h>
#else
#include <asm/unaligned.h>
#endif

/* Before 6.15 no call both inserts a struct page and forces the PTE writable: a shared vma
 * carries write in vm_page_prot, and a private one does not. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
#define compat_vmf_insert_page_mkwrite(vmf, page, write) vmf_insert_page_mkwrite(vmf, page, write)
#else
static inline vm_fault_t
compat_vmf_insert_page_mkwrite(struct vm_fault *vmf, struct page *page, bool write)
{
	if (write && !(vmf->vma->vm_flags & VM_SHARED))
		return VM_FAULT_SIGBUS;
	return vmf_insert_page(vmf->vma, vmf->address, page);
}
#endif

/* __iget() is an inline from 6.12 and a function no module may link before it. Both take the
 * same increment, and both want inode->i_lock held. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
#define compat_iget(inode) __iget(inode)
#else
static inline void compat_iget(struct inode *inode)
{
	atomic_inc(&inode->i_count);
}
#endif

/* The drop_inode that never caches: an inode goes as soon as its last reference does. 6.18 spells
 * it inode_just_drop, and earlier kernels spell it generic_delete_inode. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
#define COMPAT_DROP_INODE_ALWAYS inode_just_drop
#else
#define COMPAT_DROP_INODE_ALWAYS generic_delete_inode
#endif

/* A mount that carries no block device. 6.18 has no mount_nodev, since fs_context is the path it
 * steers a filesystem to, but it keeps ->mount and every part that helper stands on. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
static inline struct dentry *compat_mount_nodev(struct file_system_type *fs_type, int flags, void *data,
						int (*fill_super)(struct super_block *, void *, int))
{
	struct super_block *sb = sget(fs_type, NULL, set_anon_super, flags, NULL);
	if (IS_ERR(sb))
		return ERR_CAST(sb);

	const int filled = fill_super(sb, data, (flags & SB_SILENT) ? 1 : 0);
	if (filled) {
		deactivate_locked_super(sb);
		return ERR_PTR(filled);
	}

	sb->s_flags |= SB_ACTIVE;
	return dget(sb->s_root);
}
#else
#define compat_mount_nodev(fs_type, flags, data, fill_super) mount_nodev(fs_type, flags, data, fill_super)
#endif

/*
 * Drops the page table entries behind @vma, so its owner faults on the next touch.
 *
 * Up to 6.17 the reach is that one vma. From 6.18 the only per-vma zap a module may call is
 * zap_vma_ptes, which does nothing at all on anything but VM_PFNMAP and returns void while it does
 * so, and a file region here is VM_MIXEDMAP. So the reach widens to the whole file range: every
 * process mapping it loses its entries, and one whose permission still holds faults them back.
 * Over-revoking costs those processes a fault; under-revoking would leave access the caller
 * believes it took away.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
static inline void compat_zap_vma(struct vm_area_struct *vma)
{
	struct file *mapped = vma->vm_file;
	if (mapped == NULL || mapped->f_mapping == NULL)
		return;

	/* even_cows, so a MAP_PRIVATE reader's copies go with the shared pages. */
	unmap_mapping_range(mapped->f_mapping, (loff_t)vma->vm_pgoff << PAGE_SHIFT,
			    (loff_t)(vma->vm_end - vma->vm_start), 1);
}
#else
static inline void compat_zap_vma(struct vm_area_struct *vma)
{
	zap_vma_pages(vma);
}
#endif

#endif /* _FS_COMPAT_H */
