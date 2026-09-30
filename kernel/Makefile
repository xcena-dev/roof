# SPDX-License-Identifier: GPL-2.0-only
#
# The kernel module: a WORM filesystem over CXL shared memory, on a partitioned global index.
#
# Build artifacts are collected in the 'build/' directory
#

# The tree names the filesystem once, at the git root. Kbuild turns MODULE_NAME into
# KBUILD_MODNAME, and the log prefix, fs type, sysfs directory and slab names come off that.
FS_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))..)
include $(FS_ROOT)/fsname
MODULE_NAME := $(FS_NAME)

# The channel device and its log prefix carry the daemon's name, which fsname keeps apart.
ccflags-y += -DFS_DAEMON_NAME='"$(DAEMON_NAME)"'

obj-m := $(MODULE_NAME).o

# Source files, one line per subsystem directory under src/.
$(MODULE_NAME)-objs :=                                                   \
	src/vfs/super.o src/vfs/inode.o src/vfs/file.o src/vfs/dir.o         \
	src/vfs/namei.o                                                      \
	src/region/region.o src/region/index.o src/region/refs.o             \
	src/lifecycle/bootstrap.o src/lifecycle/gc.o src/lifecycle/format.o  \
	src/cxl/io.o src/cxl/dax_area.o                                      \
	src/access/acl.o src/access/daemon_dev.o 							 \
	src/access/daemon_queue.o src/access/meta_lock.o     				 \
	src/sysfs/sysfs.o src/sysfs/sysfs_region.o src/sysfs/sysfs_acl.o     \
	src/sysfs/sysfs_gc.o src/sysfs/sysfs_daemon.o

# The knobs only a test turns. A production build passes FS_TEST_KNOBS=0 and test/ is neither
# compiled nor reachable, every hook folding to the value production already used.
FS_TEST_KNOBS ?= 1
ifeq ($(FS_TEST_KNOBS),1)
$(MODULE_NAME)-objs += test/test_hooks.o test/test_sysfs.o
ccflags-y += -DCONFIG_FS_TEST_KNOBS
endif

# Manual GC control: forcing a sweep, taking the thread down, pausing the reaper. Kept on its own
# switch so a host can carry the operator half without carrying fault injection with it.
FS_DEBUG_KNOBS ?= 1
ifeq ($(FS_DEBUG_KNOBS),1)
$(MODULE_NAME)-objs += debug/debug_sysfs.o
ccflags-y += -DCONFIG_FS_DEBUG_KNOBS
endif

# Kernel build directory (default to running kernel)
KDIR ?= /lib/modules/$(shell uname -r)/build

# The uapi headers this shares with userspace, rendered beside their templates by the rule below.
ccflags-y += -I$(src)/include

# Local headers are named from the src/ root, so a #include line says which
# subsystem owns the header rather than where the including file happens to sit.
ccflags-y += -I$(src)/src

# test/ and debug/ sit beside src/ rather than in it: neither is a subsystem, and a build can
# leave either one out. Naming them from here keeps their #include lines the same shape.
ccflags-y += -I$(src)

# CXL 2.0 compatibility: explicit cache flush for cross-host visibility
ccflags-y += -DCONFIG_FS_CXL2_COMPAT

# Build output directory
BUILD_DIR := build

# Every place a .o and its command file can land: one per subsystem, plus the two switchable
# directories beside src/.
OBJ_GLOBS := src/*/*.o src/*/.*.cmd test/*.o test/.*.cmd debug/*.o debug/.*.cmd

# The uapi headers carry the filesystem's name, so they are rendered from the templates beside
# them rather than checked in. A missing one is rendered and a drifted one stops the build.
UAPI_TEMPLATES := $(wildcard $(FS_ROOT)/kernel/include/*.h.in)

# Current directory (use CURDIR — immune to stale PWD env var)
PWD := $(CURDIR)

# Default target: build the module
all: collect-artifacts

# Create build directory
prepare:
	@mkdir -p $(BUILD_DIR)

# Not a pattern rule on the timestamps: a header edited by hand is newer than its template,
# which is exactly the case this has to catch.
uapi-headers:
	@for template in $(UAPI_TEMPLATES); do \
		$(FS_ROOT)/tools/render-fsname.sh --ensure $$template $${template%.in} || exit 1; \
	done

# Build kernel module in-place (depends on prepare)
build-module: prepare uapi-headers
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# Collect all build artifacts to build/ and clean source dirs (depends on build)
collect-artifacts: build-module
	@echo "Collecting build artifacts to $(BUILD_DIR)/"
	@mkdir -p $(BUILD_DIR)
	@cp -f *.o *.ko *.mod* .*.o .*.cmd Module.symvers modules.order $(BUILD_DIR)/ 2>/dev/null || true
	@cp -f $(OBJ_GLOBS) $(BUILD_DIR)/ 2>/dev/null || true
	@rm -f *.o *.ko *.mod *.mod.c *.mod.o .*.o .*.cmd Module.symvers modules.order 2>/dev/null || true
	@rm -f $(OBJ_GLOBS) 2>/dev/null || true
	@echo "Build artifacts collected in $(BUILD_DIR)/"

# Clean build artifacts
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	@sh -c 'rm -f *.o *.ko *.mod* .*.cmd Module.symvers modules.order 2>/dev/null; true'
	@sh -c 'rm -f $(OBJ_GLOBS) 2>/dev/null; true'
	@rm -rf $(BUILD_DIR)
	@echo "✓ Build artifacts cleaned"

# Install the module
install: all
	sudo insmod $(BUILD_DIR)/$(MODULE_NAME).ko

# Load the module
load: all
	sudo insmod $(BUILD_DIR)/$(MODULE_NAME).ko

# Unload the module
unload:
	sudo rmmod $(MODULE_NAME)

# Reload the module
reload: unload load

# Show module info
info: all
	modinfo $(BUILD_DIR)/$(MODULE_NAME).ko

# Check for coding style issues (requires kernel checkpatch.pl)
check:
	$(KDIR)/scripts/checkpatch.pl --no-tree -f src/*.h src/*/*.c src/*/*.h \
		test/*.c test/*.h debug/*.c debug/*.h

.PHONY: all clean install load unload reload info check uapi-headers
