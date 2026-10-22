// SPDX-License-Identifier: GPL-2.0-only
/*
 * dax_area.c - the primary device's two mappings, metadata uncached and file data write-back,
 * and every further device's single write-back mapping.
 */

#include <linux/dax.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/memremap.h>
#include <linux/mutex.h>

#include "compat.h"
#include "core.h"
#include "cxl/dax_area.h"
#include "cxl/io.h"
#include "layout/layout_access.h"

static s32 dax_read_sysfs_text(const char *path, char *buf, u32 size)
{
	struct file *file = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(file))
		return PTR_ERR(file);

	loff_t pos = 0;
	memset(buf, 0, size);
	const s64 len = kernel_read(file, buf, size - 1, &pos);
	filp_close(file, NULL);

	if (len <= 0)
		return -EIO;

	buf[len] = '\0';
	return 0;
}

static s32 dax_read_sysfs_u64(const char *path, u64 *out)
{
	char buf[64];

	s32 ret = dax_read_sysfs_text(path, buf, sizeof(buf));
	if (ret)
		return ret;

	return kstrtoull(buf, 0, out) < 0 ? -EINVAL : 0;
}

/*
 * The node the device's memory is attached to. NUMA_NO_NODE lets the allocator
 * place the vmemmap wherever it can, which is correct but not local.
 */
static s32 dax_resolve_target_node(const char *devname)
{
	char sysfs_path[256];
	u64 node;

	snprintf(sysfs_path, sizeof(sysfs_path), "/sys/bus/dax/devices/%s/target_node", devname);
	if (dax_read_sysfs_u64(sysfs_path, &node) || node >= MAX_NUMNODES)
		return NUMA_NO_NODE;

	return (s32)node;
}

/* One pgmap per run of one folio order. PUD-order folios need a range that starts and ends on a
 * physical 1 GiB boundary, and the write-back area's two ends need not sit on one. */
enum dax_area_config {
	DAX_WB_PGMAPS_MAX = 3,
};

/* Shared by every mount of one device, since memremap_pages refuses a second registration over a
 * range. @meta is NULL on a device that is not a primary. */
struct dax_areas {
	u64 phys_base;
	/* The device offset where the uncached mapping ends, read off the superblock. Zero on a device
	 * that is not a primary. */
	u64 wb_start;
	/* The granule the folio orders were chosen for, off the same read as @wb_start. */
	u32 granule_shift;
	void __iomem *meta;
	void *data;
	struct dev_pagemap pgmaps[DAX_WB_PGMAPS_MAX];
	u32 pgmap_count;
	u32 refs;
};

/* A host runs at most one mount per node, and a mount maps at most LAYOUT_MAX_DEVICES devices. */
static struct dax_areas dax_areas[FS_MAX_NODE_ID * LAYOUT_MAX_DEVICES];
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
static u64 dax_read_wb_start(const struct fs_sb_info *sbi, u64 phys_addr, u64 dev_size, u32 *granule_shift)
{
	/* What a format on this mount would write, which is where the areas split on a device that
	 * has no filesystem to read one off yet. */
	const u64 request_granule = 1ULL << sbi->format_granule_shift;
	const u64 fallback = ALIGN(LAYOUT_DATA_OFFSET, request_granule) +
			     (u64)sbi->format_uc_granules * request_granule;

	*granule_shift = sbi->format_granule_shift;
	/* A format on this mount replaces the superblock, so the split it publishes is the one to map. */
	if (sbi->format_on_mount)
		return fallback;

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

	*granule_shift = head.local.granule_shift;
	return start;
}

static void dax_unmap_wb_area(struct dax_areas *areas)
{
	while (areas->pgmap_count) {
		memunmap_pages(&areas->pgmaps[--areas->pgmap_count]);
	}
}

/* One half came from ioremap and the other from memremap_pages, so each goes back the way it came. */
static void dax_drop_areas(struct dax_areas *areas)
{
	dax_unmap_wb_area(areas);
	if (areas->meta)
		iounmap(areas->meta);
	memset(areas, 0, sizeof(*areas));
}

/*
 * The write-back area [@start, @end) as ZONE_DEVICE pages in PMD-order folios, or, under a granule
 * of 1 GiB or more, PUD-order ones over its 1 GiB-aligned middle. Nothing stays mapped on failure.
 */
static void *dax_map_wb_area(struct dax_areas *areas, u64 start, u64 end, u32 granule_shift, s32 node)
{
	u64 body_start = start;
	u64 body_end = start;
	if (granule_shift >= PUD_SHIFT &&
	    compat_can_map_pud() &&
	    ALIGN(start, PUD_SIZE) < ALIGN_DOWN(end, PUD_SIZE)) {
		body_start = ALIGN(start, PUD_SIZE);
		body_end = ALIGN_DOWN(end, PUD_SIZE);
	}

	const struct {
		u64 first;
		u64 last;
		u32 order;
	} parts[DAX_WB_PGMAPS_MAX] = {
		{ start, body_start, PMD_ORDER },
		{ body_start, body_end, PUD_ORDER },
		{ body_end, end, PMD_ORDER },
	};

	void *data = NULL;
	for (u32 idx = 0; idx < DAX_WB_PGMAPS_MAX; idx++) {
		if (parts[idx].first == parts[idx].last)
			continue;

		struct dev_pagemap *pgmap = &areas->pgmaps[areas->pgmap_count];
		memset(pgmap, 0, sizeof(*pgmap));
		pgmap->type = MEMORY_DEVICE_GENERIC;
		pgmap->nr_range = 1;
		pgmap->range.start = parts[idx].first;
		pgmap->range.end = parts[idx].last - 1;
		pgmap->vmemmap_shift = parts[idx].order;

		void *mapped = memremap_pages(pgmap, node);
		if (IS_ERR(mapped)) {
			dax_unmap_wb_area(areas);
			return mapped;
		}
		areas->pgmap_count++;
		/* The direct map is linear, so the first part's address carries on into the others. */
		if (!data)
			data = mapped;
	}
	return data;
}

/*
 * dax_map_split_areas - the uncached area below the boundary, write-back above, one device.
 *
 * device_dax reserves the whole device write-back while it is bound, and that reservation is
 * what silently turns the uncached half back into a cached one.
 */
static s32 dax_map_split_areas(struct fs_sb_info *sbi, u64 phys_addr, u64 dev_size, const char *devname)
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

	u32 granule_shift;
	const u64 wb_start = dax_read_wb_start(sbi, phys_addr, dev_size, &granule_shift);

	if (areas->refs) {
		if (!areas->meta) {
			pr_err("%s is mapped write-back only as a device of another mount, so it cannot be a primary\n",
			       devname);
			return -EBUSY;
		}
		/* The boundary comes off the medium, so two mounts of one device agree unless the
		 * superblock changed under them. Sharing a mapping split elsewhere would put the
		 * pools where this mount does not expect them. */
		if (areas->wb_start != wb_start || areas->granule_shift != granule_shift) {
			pr_err("%s is mapped with its boundary at 0x%llx granule 2^%u, not 0x%llx granule 2^%u\n",
			       devname, areas->wb_start, areas->granule_shift, wb_start, granule_shift);
			return -EBUSY;
		}
		areas->refs++;
		sbi->devices[FS_PRIMARY_DEVICE].areas = areas;
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

	data = dax_map_wb_area(areas, phys_addr + wb_start, phys_addr + dev_size, granule_shift,
			       dax_resolve_target_node(devname));
	if (IS_ERR(data)) {
		pr_err("ZONE_DEVICE pages refused for the data area of %s: %ld\n", devname, PTR_ERR(data));
		iounmap(meta);
		return PTR_ERR(data);
	}

	areas->phys_base = phys_addr;
	areas->wb_start = wb_start;
	areas->granule_shift = granule_shift;
	areas->meta = meta;
	areas->data = data;
	areas->refs = 1;

	sbi->devices[FS_PRIMARY_DEVICE].areas = areas;

	pr_info("%s uncached at 0x%llx for %llu bytes, write-back at 0x%llx in %u pgmap(s)\n",
		devname, phys_addr, wb_start, phys_addr + wb_start, areas->pgmap_count);

	return 0;
}

/*
 * dax_map_device_area - a further device's whole range write-back, or a share of the mapping
 * another mount made. Nothing is mapped uncached, so no PAT reservation conflicts with device_dax's.
 */
static s32 dax_map_device_area(struct fs_sb_info *sbi, u32 device_idx, u64 phys_addr, u64 dev_size,
			       u32 granule_shift, const char *devname)
{
	guard(mutex)(&dax_areas_lock);

	struct dax_areas *areas = dax_find_areas_slot(phys_addr);
	if (!areas) {
		pr_err("no room to track the areas of %s\n", devname);
		return -ENOSPC;
	}

	if (areas->refs) {
		if (areas->meta) {
			pr_err("%s is mapped with an uncached split as the primary of another mount, so it cannot be device %u\n",
			       devname, device_idx);
			return -EBUSY;
		}
		if (areas->granule_shift != granule_shift) {
			pr_err("%s is mapped for granule 2^%u, not 2^%u\n", devname, areas->granule_shift,
			       granule_shift);
			return -EBUSY;
		}
		areas->refs++;
		pr_info("%s device %u shared, now %u mounts\n", devname, device_idx, areas->refs);
	} else {
		void *data = dax_map_wb_area(areas, phys_addr, phys_addr + dev_size, granule_shift,
					     dax_resolve_target_node(devname));
		if (IS_ERR(data)) {
			pr_err("ZONE_DEVICE pages refused for device %u %s: %ld\n", device_idx, devname,
			       PTR_ERR(data));
			return PTR_ERR(data);
		}

		areas->phys_base = phys_addr;
		areas->wb_start = 0;
		areas->granule_shift = granule_shift;
		areas->meta = NULL;
		areas->data = data;
		areas->refs = 1;
		pr_info("%s device %u write-back at 0x%llx in %u pgmap(s)\n", devname, device_idx, phys_addr,
			areas->pgmap_count);
	}

	sbi->devices[device_idx].areas = areas;
	return 0;
}

static const char *dax_get_devname(const char *devpath)
{
	const char *devname = strrchr(devpath, '/');
	return devname ? devname + 1 : devpath;
}

/* The device's physical base and size, off its sysfs entries. */
static s32 dax_read_device(const char *devname, u64 *phys_addr, u64 *dev_size)
{
	char sysfs_path[256];
	snprintf(sysfs_path, sizeof(sysfs_path), "/sys/bus/dax/devices/%s/resource", devname);
	s32 ret = dax_read_sysfs_u64(sysfs_path, phys_addr);
	if (ret) {
		pr_err("cannot read resource for %s (%d)\n", devname, ret);
		return ret;
	}

	snprintf(sysfs_path, sizeof(sysfs_path), "/sys/bus/dax/devices/%s/size", devname);
	ret = dax_read_sysfs_u64(sysfs_path, dev_size);
	if (ret || *dev_size == 0) {
		pr_err("cannot read size for %s (%d)\n", devname, ret);
		return ret ? ret : -EINVAL;
	}

	pr_debug("%s phys=0x%llx size=%llu (%llu MB)\n", devname, *phys_addr, *dev_size, *dev_size >> 20);
	return 0;
}

__printf(2, 3) static s32 dax_read_sysfs_u64_fmt(u64 *out, const char *fmt, ...)
{
	char path[256];
	va_list args;

	va_start(args, fmt);
	vsnprintf(path, sizeof(path), fmt, args);
	va_end(args);

	return dax_read_sysfs_u64(path, out);
}

/*
 * A dax device is the memdev serial and the device offset of its range. The serial is the same
 * on every host and the offset tells apart dax devices carved from one memdev.
 */
static s32 dax_read_identity(const char *devname, u64 dax_start, u64 *serial, u64 *device_offset)
{
	char path[256];
	snprintf(path, sizeof(path), "/sys/bus/dax/devices/%s/../../target0", devname);

	char decoder[64];
	if (dax_read_sysfs_text(path, decoder, sizeof(decoder)))
		goto no_identity;

	strim(decoder);
	if (!*decoder || strchr(decoder, '/') || decoder[0] == '.')
		goto no_identity;

	u64 region_start;
	u64 dpa_start;
	if (dax_read_sysfs_u64_fmt(&region_start, "/sys/bus/dax/devices/%s/../../resource", devname) ||
	    dax_read_sysfs_u64_fmt(&dpa_start, "/sys/bus/cxl/devices/%s/dpa_resource", decoder) ||
	    dax_read_sysfs_u64_fmt(serial, "/sys/bus/cxl/devices/%s/../uport/serial", decoder) ||
	    dax_start < region_start)
		goto no_identity;

	*device_offset = dpa_start + (dax_start - region_start);
	return 0;

no_identity:
	pr_err("%s has no CXL region to identify it by, which a mount of several devices needs\n", devname);
	return -ENODEV;
}

/* One device as the mount was given it, before it has a device number. */
struct dax_given {
	u64 phys_addr;
	u64 size;
	u64 serial;
	u64 device_offset;
};

/* Whether the device starts with a valid superblock head, and if so that superblock's device list. */
static s32 dax_probe_superblock(u64 phys_addr, bool *found, union layout_superblock_devices_copy *list)
{
	void __iomem *window = ioremap_uc(phys_addr, LAYOUT_SUPERBLOCK_SIZE);
	if (!window)
		return -ENOMEM;

	const struct layout_superblock *probe = (const struct layout_superblock *)window;
	union layout_superblock_head_copy head;
	cxl_get_layout_superblock_head(&head, &probe->head);

	*found = head.local.magic == LAYOUT_MAGIC && head.local.checksum == layout_compute_superblock_checksum(&head);
	if (*found)
		layout_get_superblock_devices(list, &probe->devices);

	iounmap(window);
	return 0;
}

/* The one given device that holds a superblock, with its device list. @found stays false on none. */
static s32 dax_find_primary(const struct fs_sb_info *sbi, const struct dax_given *given, u32 *primary,
			    bool *found, union layout_superblock_devices_copy *list)
{
	*found = false;
	for (u32 idx = 0; idx < sbi->daxdev_count; idx++) {
		union layout_superblock_devices_copy scratch;
		bool holds;

		s32 ret = dax_probe_superblock(given[idx].phys_addr, &holds, &scratch);
		if (ret)
			return ret;
		if (!holds)
			continue;
		if (*found) {
			pr_err("%s and %s both hold a superblock\n", sbi->daxdev_paths[*primary],
			       sbi->daxdev_paths[idx]);
			return -ENODEV;
		}
		*found = true;
		*primary = idx;
		*list = scratch;
	}
	return 0;
}

/* Gives every given device the index its identity has in the primary's device list. */
static s32 dax_order_by_list(const struct fs_sb_info *sbi, const struct dax_given *given, u32 primary,
			     const union layout_superblock_devices_copy *list, u32 *order)
{
	const char *primary_path = sbi->daxdev_paths[primary];
	bool filled[LAYOUT_MAX_DEVICES] = {};

	if (list->local.device_count == 0 || list->local.device_count > LAYOUT_MAX_DEVICES ||
	    list->local.checksum != layout_compute_devices_checksum(list)) {
		pr_err("%s holds a superblock whose device list is invalid\n", primary_path);
		return -ENODEV;
	}
	if (list->local.serial[0] != given[primary].serial ||
	    list->local.device_offset[0] != given[primary].device_offset ||
	    list->local.size[0] != given[primary].size) {
		pr_err("%s holds the superblock but is not the device 0 its list names\n", primary_path);
		return -ENODEV;
	}

	order[0] = primary;
	filled[0] = true;
	for (u32 idx = 0; idx < sbi->daxdev_count; idx++) {
		if (idx == primary)
			continue;

		u32 slot = 1;
		while (slot < list->local.device_count &&
		       (list->local.serial[slot] != given[idx].serial ||
			list->local.device_offset[slot] != given[idx].device_offset))
			slot++;
		if (slot == list->local.device_count) {
			pr_err("%s (serial 0x%llx offset 0x%llx) is not in the device list of %s\n",
			       sbi->daxdev_paths[idx], given[idx].serial, given[idx].device_offset, primary_path);
			return -ENODEV;
		}
		if (list->local.size[slot] != given[idx].size) {
			pr_err("%s is %llu bytes and the device list says %llu\n", sbi->daxdev_paths[idx],
			       given[idx].size, list->local.size[slot]);
			return -ENODEV;
		}
		order[slot] = idx;
		filled[slot] = true;
	}

	for (u32 slot = 0; slot < list->local.device_count; slot++) {
		if (!filled[slot]) {
			pr_err("device %u of %s (serial 0x%llx offset 0x%llx) was not given\n", slot, primary_path,
			       list->local.serial[slot], list->local.device_offset[slot]);
			return -ENODEV;
		}
	}
	return 0;
}

/* The device holding the superblock is the primary and its list numbers the rest. With none, or
 * under a format, the given order stands. */
static s32 dax_order_devices(struct fs_sb_info *sbi)
{
	const u32 count = sbi->daxdev_count;
	const bool several = count > 1;
	struct dax_given given[LAYOUT_MAX_DEVICES] = {};
	u32 order[LAYOUT_MAX_DEVICES];

	for (u32 idx = 0; idx < count; idx++) {
		const char *devname = dax_get_devname(sbi->daxdev_paths[idx]);

		s32 ret = dax_read_device(devname, &given[idx].phys_addr, &given[idx].size);
		if (ret)
			return ret;

		if (several) {
			ret = dax_read_identity(devname, given[idx].phys_addr, &given[idx].serial,
						&given[idx].device_offset);
			if (ret)
				return ret;
		}

		for (u32 prev = 0; prev < idx; prev++) {
			if (given[prev].phys_addr == given[idx].phys_addr ||
			    (several && given[prev].serial == given[idx].serial &&
			     given[prev].device_offset == given[idx].device_offset)) {
				pr_err("%s and %s are one device\n", sbi->daxdev_paths[prev], sbi->daxdev_paths[idx]);
				return -ENODEV;
			}
		}
	}

	u32 primary = FS_PRIMARY_DEVICE;
	bool listed = false;
	if (several && !sbi->format_on_mount) {
		union layout_superblock_devices_copy list;

		s32 ret = dax_find_primary(sbi, given, &primary, &listed, &list);
		if (ret)
			return ret;
		if (listed) {
			ret = dax_order_by_list(sbi, given, primary, &list, order);
			if (ret)
				return ret;
		}
	}
	if (!listed) {
		for (u32 idx = 0; idx < count; idx++)
			order[idx] = idx;
	}

	char(*paths)[sizeof(sbi->daxdev_paths[0])] __free(kfree) =
		kmemdup(sbi->daxdev_paths, sizeof(sbi->daxdev_paths), GFP_KERNEL);
	if (!paths)
		return -ENOMEM;

	for (u32 idx = 0; idx < count; idx++) {
		struct fs_device *dev = &sbi->devices[idx];

		strscpy(sbi->daxdev_paths[idx], paths[order[idx]], sizeof(sbi->daxdev_paths[idx]));
		dev->phys_base = given[order[idx]].phys_addr;
		dev->size = given[order[idx]].size;
		dev->serial = given[order[idx]].serial;
		dev->device_offset = given[order[idx]].device_offset;
	}
	return 0;
}

s32 dax_acquire_areas(struct fs_sb_info *sbi)
{
	s32 ret = dax_order_devices(sbi);
	if (ret)
		return ret;

	for (u32 idx = 0; idx < sbi->daxdev_count; idx++) {
		const char *devname = dax_get_devname(sbi->daxdev_paths[idx]);
		struct fs_device *dev = &sbi->devices[idx];

		pr_debug("acquiring DEV_DAX device %s as device %u via memremap\n", devname, idx);

		if (idx == FS_PRIMARY_DEVICE)
			ret = dax_map_split_areas(sbi, dev->phys_base, dev->size, devname);
		else
			ret = dax_map_device_area(sbi, idx, dev->phys_base, dev->size,
						  sbi->devices[FS_PRIMARY_DEVICE].areas->granule_shift, devname);
		if (ret)
			goto err_release;

		dev->wb_start = dev->areas->wb_start;
		dev->data_base = dev->areas->data;
		if (idx == FS_PRIMARY_DEVICE)
			sbi->meta_base = (void *)dev->areas->meta;

		pr_debug("DEV_DAX %s acquired (%llu bytes)\n", devname, dev->size);
	}

	sbi->device_count = sbi->daxdev_count;
	return 0;

err_release:
	dax_release_areas(sbi);
	return ret;
}

void dax_release_areas(struct fs_sb_info *sbi)
{
	struct dax_areas *held[LAYOUT_MAX_DEVICES];
	u32 count = 0;

	for (u32 idx = 0; idx < LAYOUT_MAX_DEVICES; idx++) {
		if (sbi->devices[idx].areas)
			held[count++] = sbi->devices[idx].areas;
	}

	sbi->meta_base = NULL;
	memset(sbi->devices, 0, sizeof(sbi->devices));
	sbi->device_count = 0;

	guard(mutex)(&dax_areas_lock);

	for (u32 idx = 0; idx < count; idx++) {
		if (--held[idx]->refs == 0)
			dax_drop_areas(held[idx]);
	}
}

void dax_exit(void)
{
	guard(mutex)(&dax_areas_lock);

	for (u32 idx = 0; idx < ARRAY_SIZE(dax_areas); idx++) {
		/* refs is stamped only once the mapping is whole, so a held entry has all of it to give
		 * back. Nonzero here is a mount teardown that never reached its release. */
		struct dax_areas *areas = &dax_areas[idx];
		if (!areas->refs)
			continue;

		pr_warn("dax: area at 0x%llx still held by %u mount(s) at unload\n", areas->phys_base, areas->refs);
		dax_drop_areas(areas);
	}
}
