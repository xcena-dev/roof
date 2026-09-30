// SPDX-License-Identifier: Apache-2.0
//
// test_unmount_drain -- a drain refused while this node still holds a reference clears nothing.
// The descriptor stays open across the write, so the drain can only refuse and never closes the
// mount the rest of the suite uses.
//
//   test_unmount_drain <mount-a>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <string>

#include "fs/file.hpp"
#include "harness/harness.hpp"
#include "harness/mount.hpp"
#include "harness/pattern.hpp"
#include "harness/slot.hpp"
#include "harness/sysfs.hpp"
#include "name.h"

namespace
{

using fsuser::tests::caseName;
using fsuser::tests::findRowByName;
using fsuser::tests::PlacementSize;
using fsuser::tests::RegionRow_t;

constexpr std::uint64_t PlacedWords = PlacementSize / sizeof(std::uint32_t);
constexpr std::uint32_t FillSeed = 0x5A5A5A5AU;

// Directly under the mount's node directory: gc_mount_attr_group carries no name of its own.
[[nodiscard]] std::string unmountPreparePath(std::uint32_t nodeId)
{
    return "/sys/fs/" FS_NAME_STR "/node" + std::to_string(nodeId) + "/unmount_prepare";
}

// Root-only and write-only, so a non-root run has no way to reach the path under test.
void writeRefusedWithBusy(fsuser::tests::Report& report, const std::string& path)
{
    errno = 0;
    const std::int32_t descriptor = ::open(path.c_str(), O_WRONLY);
    report.check("unmount_prepare opens for writing", descriptor >= 0);
    if (descriptor < 0)
    {
        return;
    }

    errno = 0;
    const auto written = ::write(descriptor, "1", 1);
    const std::int32_t savedErrno = errno;
    report.checkErrno("the write is refused with EBUSY", written < 0 && savedErrno == EBUSY, savedErrno);
    ::close(descriptor);
}

}  // namespace

int main(int argc, char** argv)
{
    fsuser::tests::Mounts_t mounts;
    if (!fsuser::tests::takeMounts(argc, argv, 1, mounts))
    {
        return fsuser::tests::UsageStatus;
    }

    fsuser::tests::Report report;

    // unmount_prepare is mode 0200, so only root can even open it.
    if (::geteuid() != 0)
    {
        report.note("not root, so unmount_prepare cannot be reached: skipping");
        return fsuser::tests::SkipStatus;
    }
    // The kernel attests the real uid, which the rules name as the invoking account.
    static_cast<void>(fsuser::tests::dropRealIdsToInvoker());

    auto mount = fsuser::tests::Mount(mounts.first());
    const auto name = caseName("unmount_drain");

    report.section("a region held open across the drain attempt");

    auto file = fsuser::tests::placeFile(mount, name);
    auto mapped = file.map(PlacementSize, PROT_READ | PROT_WRITE);
    report.check("the file maps writably", mapped.isMapped());
    fsuser::tests::fillPattern(static_cast<std::uint32_t*>(mapped.get()), PlacedWords, FillSeed);

    const auto before = findRowByName(name);
    report.check("region_info lists the file before the drain attempt", before.has_value());

    report.section("the drain this node's own reference refuses");

    const auto path = unmountPreparePath(mount.getNodeId());
    writeRefusedWithBusy(report, path);

    report.section("nothing the refused drain touched");

    const auto after = findRowByName(name);
    report.check("the region is still listed", after.has_value());
    if (before && after)
    {
        report.check("the same slot", before->entry == after->entry);
        report.check("the same state", before->state == after->state);
        report.check("the same size", before->size == after->size);
        report.check("the same offset", before->offset == after->offset);
    }
    fsuser::tests::checkPattern(report, "the mapping still reads its own bytes",
                                static_cast<const std::uint32_t*>(mapped.get()), PlacedWords, FillSeed);

    report.section("the flag the refusal lowered again");

    report.checkAccepted("a second open of the same name succeeds",
                         [&]
                         {
                             auto reopened = mount.open(name, O_RDONLY);
                         });

    const auto secondName = caseName("unmount_drain_second");
    report.checkAccepted("a create and place of a new name succeeds",
                         [&]
                         {
                             auto second = fsuser::tests::placeFile(mount, secondName);
                         });
    mount.unlink(secondName);
    mount.unlink(name);

    return report.summarise("test_unmount_drain");
}
