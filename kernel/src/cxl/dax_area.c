// SPDX-License-Identifier: GPL-2.0-only
/*
 * dax_area.c - one device, two mappings: metadata uncached and file data write-back.
 */

#include <linux/dax.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/memremap.h>
#include <linux/mutex.h>

#include "core.h"
#include "cxl/dax_area.h"
#include "cxl/io.h"
#include "layout/layout_access.h"

/* ============================================================================
 * Unified DAX abstraction layer
 * ============================================================================
 *
 * dax_acquire_areas() / dax_release_areas()
 *
 * DEV_DAX (character device) produces:
 *   sbi->meta_base   = mapped memory pointer
 *   sbi->total_size = total size
 *
 * After that, all filesystem logic references only sbi->meta_base.
 */

/* Helper: read u64 value from sysfs */
static int dax_read_sysfs_u64(const char *path, u64 *out)
{
	struct file *f;
	char buf[64];
	loff_t pos = 0;
	ssize_t len;

	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f))
		return PTR_ERR(f);

	memset(buf, 0, sizeof(buf));
	len = kernel_read(f, buf, sizeof(buf) - 1, &pos);
	filp_close(f, NULL);

	if (len <= 0)
		return -EIO;

	buf[len] = '\0';
	return kstrtoull(buf, 0, out) < 0 ? -EINVAL : 0;
}

/*
 * The node the device's memory is attached to. NUMA_NO_NODE lets the allocator
 * place the vmemmap wherever it can, which is correct but not local.
 */
static int dax_resolve_target_node(const char *devname)
{
	char sysfs_path[256];
	u64 node;

	snprintf(sysfs_path, sizeof(sysfs_path), "/sys/bus/dax/devices/%s/target_node", devname);
	if (dax_read_sysfs_u64(sysfs_path, &node) || node >= MAX_NUMNODES)
		return NUMA_NO_NODE;

	return (int)node;
}

/*
 * The two mappings belong to the device rather than to a mount. memremap_pages registers the
 * vmemmap for a physical range once and refuses a second registration over it, so mounts of
 * one device share one of these and the last one out unmaps.
 */
struct dax_areas {
	u64 phys_base;
	/* A device offset and not an address, unlike the field above it: where the uncached mapping
	 * ends and the write-back one begins, read off the superblock. Kept here because the
	 * mapping it describes belongs to the device rather than to a mount. */
	u64 wb_start;
	void __iomem *meta;
	void *data;
	struct dev_pagemap pgmap;
	u32 refs;
};

/* One device per possible mount is the ceiling, since a mount takes one device. */
static struct dax_areas dax_areas[FS_MAX_NODE_ID];
static DEFINE_MUTEX(dax_areas_lock);

/* The entry already serving @phys_base, else a free one, else NULL. */
static struct dax_areas *dax_find_areas_slot(u64 phys_base)
{
	struct dax_areas *vacant = NULL;

	for (u32 idx = 0; idx < ARRAY_SIZE(dax_areas); idx++) {
		struct dax_areas *areas = &dax_areas[idx];

		if (areas->refs && areas->phys_base == phys_base)
			return areas;
		if (!areas->refs && !vacant)
			vacant = areas;
	}
	return vacant;
}

/*
 * Where the uncached area ends, off the superblock this device already carries.
 *
 * The split has to be known before either mapping exists, so this takes a small uncached window
 * over the superblock and gives it back. A device with no filesystem on it yet answers with the
 * default, which is what the format about to run will write.
 */
static u64 dax_read_wb_start(const struct fs_sb_info *sbi, u64 phys_addr, u64 dev_size)
{
	/* What a format on this mount would write, which is where the areas split on a device that
	 * has no filesystem to read one off yet. */
	const u64 request_granule = 1ULL << sbi->format_granule_shift;
	const u64 fallback = ALIGN(LAYOUT_DATA_OFFSET, request_granule) +
			     (u64)sbi->format_uc_granules * request_granule;

	void __iomem *window = ioremap_uc(phys_addr, LAYOUT_SUPERBLOCK_SIZE);
	if (!window)
		return fallback;

	union layout_superblock_head_copy head;
	cxl_get_layout_superblock_head(&head, (const struct layout_superblock_head *)window);
	iounmap(window);

	if (head.local.magic != LAYOUT_MAGIC || head.local.version != LAYOUT_VERSION)
		return fallback;
	if (head.local.checksum != layout_compute_superblock_checksum(&head))
		return fallback;
	if (!layout_check_granule_shift(head.local.granule_shift))
		return fallback;

	u64 granule = 1ULL << head.local.granule_shift;
	u64 start = layout_align_up(LAYOUT_DATA_OFFSET, granule) + (u64)head.local.uc_granules * granule;

	/* A boundary the device cannot hold would map the whole write-back area away. */
	if (start == 0 || start >= dev_size)
		return fallback;

	return start;
}

/*
 * dax_map_split_areas - the uncached area below the boundary, write-back above, one device.
 *
 * device_dax reserves the whole device write-back while it is bound, and that reservation is
 * what silently turns the uncached half back into a cached one.
 */
static int dax_map_split_areas(struct fs_sb_info *sbi, u64 phys_addr, u64 dev_size, const char *devname)
{
	struct dax_areas *areas;
	void __iomem *meta;
	void *data;

	if (dev_size <= LAYOUT_DATA_OFFSET) {
		pr_err("%s is too small to hold a region area\n", devname);
		return -EINVAL;
	}

	guard(mutex)(&dax_areas_lock);

	areas = dax_find_areas_slot(phys_addr);
	if (!areas) {
		pr_err("no room to track the areas of %s\n", devname);
		return -ENOSPC;
	}

	const u64 wb_start = dax_read_wb_start(sbi, phys_addr, dev_size);

	if (areas->refs) {
		/* The boundary comes off the medium, so two mounts of one device agree unless the
		 * superblock changed under them. Sharing a mapping split elsewhere would put the
		 * pools where this mount does not expect them. */
		if (areas->wb_start != wb_start) {
			pr_err("%s is mapped with its boundary at 0x%llx, not 0x%llx\n", devname,
			       areas->wb_start, wb_start);
			return -EBUSY;
		}
		areas->refs++;
		sbi->dax_areas = areas;
		sbi->meta_base = (void *)areas->meta;
		sbi->data_base = areas->data;
		pr_info("%s areas shared, now %u mounts\n", devname, areas->refs);
		return 0;
	}

	meta = ioremap_uc(phys_addr, wb_start);
	if (!meta) {
		pr_err("uncached mapping refused for the metadata area of %s\n", devname);
		return -ENOMEM;
	}

	if (!cxl_is_mapping_uncached(meta)) {
		pr_err("metadata area of %s came back cached; unbind it from device_dax first\n", devname);
		iounmap(meta);
		return -EBUSY;
	}

	areas->pgmap.type = MEMORY_DEVICE_GENERIC;
	areas->pgmap.nr_range = 1;
	areas->pgmap.range.start = phys_addr + wb_start;
	areas->pgmap.range.end = phys_addr + dev_size - 1;
	/* Compound struct pages at PMD order, which is the granularity the region
	 * layout is already aligned to. */
	areas->pgmap.vmemmap_shift = PMD_ORDER;

	data = memremap_pages(&areas->pgmap, dax_resolve_target_node(devname));
	if (IS_ERR(data)) {
		pr_err("ZONE_DEVICE pages refused for the data area of %s: %ld\n", devname, PTR_ERR(data));
		iounmap(meta);
		return PTR_ERR(data);
	}

	areas->phys_base = phys_addr;
	areas->wb_start = wb_start;
	areas->meta = meta;
	areas->data = data;
	areas->refs = 1;

	sbi->dax_areas = areas;
	sbi->meta_base = (void *)meta;
	sbi->data_base = data;

	pr_info("%s uncached at 0x%llx for %llu bytes, write-back at 0x%llx\n",
		devname, phys_addr, wb_start, phys_addr + wb_start);

	return 0;
}

/*
 * dax_acquire_areas - take the DEV_DAX range and map its two areas.
 *
 * Reads phys addr and size from sysfs for the device sbi->daxdev_path names.
 * After return, sbi->meta_base / data_base / phys_base / total_size are populated.
 */
int dax_acquire_areas(struct fs_sb_info *sbi)
{
	const char *devpath = sbi->daxdev_path;
	const char *devname;
	char sysfs_path[256];
	u64 phys_addr, dev_size;
	int ret;

	devname = strrchr(devpath, '/');
	devname = devname ? devname + 1 : devpath;

	pr_debug("acquiring DEV_DAX device %s via memremap\n", devname);

	snprintf(sysfs_path, sizeof(sysfs_path), "/sys/bus/dax/devices/%s/resource", devname);
	ret = dax_read_sysfs_u64(sysfs_path, &phys_addr);
	if (ret) {
		pr_err("cannot read resource for %s (%d)\n", devname, ret);
		return ret;
	}

	snprintf(sysfs_path, sizeof(sysfs_path), "/sys/bus/dax/devices/%s/size", devname);
	ret = dax_read_sysfs_u64(sysfs_path, &dev_size);
	if (ret || dev_size == 0) {
		pr_err("cannot read size for %s (%d)\n", devname, ret);
		return ret ? ret : -EINVAL;
	}

	pr_debug("%s phys=0x%llx size=%llu (%llu MB)\n", devname, phys_addr, dev_size, dev_size >> 20);

	ret = dax_map_split_areas(sbi, phys_addr, dev_size, devname);
	if (ret)
		return ret;

	sbi->phys_base = phys_addr; /* Store physical address for DAX mmap */
	sbi->total_size = dev_size;
	sbi->wb_start = sbi->dax_areas->wb_start;

	pr_debug("DEV_DAX %s acquired (%llu bytes)\n", devname, dev_size);
	return 0;
}

/* dax_release_areas - drop this mount's hold on the device's areas. */
void dax_release_areas(struct fs_sb_info *sbi)
{
	struct dax_areas *areas = sbi->dax_areas;

	sbi->meta_base = NULL;
	sbi->data_base = NULL;
	sbi->dax_areas = NULL;
	if (!areas)
		return;

	guard(mutex)(&dax_areas_lock);

	if (--areas->refs)
		return;

	/* One half came from ioremap and the other from memremap_pages, so each goes
	 * back the way it came. */
	memunmap_pages(&areas->pgmap);
	iounmap(areas->meta);
	memset(areas, 0, sizeof(*areas));
}

void dax_exit(void)
{
	guard(mutex)(&dax_areas_lock);

	for (u32 idx = 0; idx < ARRAY_SIZE(dax_areas); idx++) {
		/* refs is stamped only once both halves mapped, so a held entry has both to give
		 * back. Nonzero here is a mount teardown that never reached its release. */
		struct dax_areas *areas = &dax_areas[idx];
		if (!areas->refs)
			continue;

		pr_warn("dax: area at 0x%llx still held by %u mount(s) at unload\n", areas->phys_base, areas->refs);
		memunmap_pages(&areas->pgmap);
		iounmap(areas->meta);
		memset(areas, 0, sizeof(*areas));
	}
}
