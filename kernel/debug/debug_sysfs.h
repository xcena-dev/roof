/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * debug_sysfs.h - manual GC control, merged into the debug group.
 *
 * Forcing a sweep, taking the thread down and pausing the reaper are what an operator reaches for
 * during a diagnosis. Steady-state operation turns none of them.
 *
 * The read-only counters an operator watches live in the sysfs_*.c files beside sysfs.c, outside
 * the debug group, because reading them changes nothing.
 */

#ifndef _DEBUG_SYSFS_H
#define _DEBUG_SYSFS_H

#include <linux/types.h>

struct kobject;

#ifdef CONFIG_FS_DEBUG_KNOBS

s32 debug_create_sysfs_group(struct kobject *kobj);
void debug_delete_sysfs_group(struct kobject *kobj);

#else

/* No attributes, so the directory is never created. */
static inline s32 debug_create_sysfs_group(struct kobject *kobj)
{
	(void)kobj;
	return 0;
}

static inline void debug_delete_sysfs_group(struct kobject *kobj)
{
	(void)kobj;
}

#endif /* CONFIG_FS_DEBUG_KNOBS */

#endif /* _DEBUG_SYSFS_H */
