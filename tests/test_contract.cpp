// SPDX-License-Identifier: Apache-2.0
//
// test_contract -- refusals a correct caller can never provoke.
//
// Each case hands the kernel input the library does not emit: an mmap without O_CLOEXEC, an unknown
// ioctl, and a size change by path. Creation is not under test.
//
//   test_contract <mount-a>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <exception>
#include <string>

#include "fs/file.hpp"
#include "fs/testing.hpp"
#include "harness/harness.hpp"
#include "harness/mount.hpp"
#include "harness/slot.hpp"

namespace
{

using fsuser::tests::caseName;
using fsuser::tests::findRowByName;
using fsuser::tests::PlacementSize;

// mmap on a descriptor without O_CLOEXEC has to answer EACCES, and the same file mapped through the
// library has to succeed. The pair is what separates the guard from a file that was simply broken.
void checkCloexecGuard(fsuser::tests::Report& report, fsuser::tests::Mount& mount)
{
    report.section("mmap requires O_CLOEXEC");

    const auto name = caseName("contract_cloexec");

    try
    {
        auto file = fsuser::tests::placeFile(mount, name);

        const std::int32_t bare = fsuser::testing::openRaw(mount.getPoint(), name, O_RDWR);
        report.check("a bare open succeeds", bare >= 0);
        if (bare >= 0)
        {
            errno = 0;
            void* mapped = ::mmap(nullptr, PlacementSize, PROT_READ, MAP_SHARED, bare, 0);
            report.check("mmap on a non-cloexec fd is refused with EACCES",
                         mapped == MAP_FAILED && errno == EACCES);
            if (mapped != MAP_FAILED)
            {
                ::munmap(mapped, PlacementSize);
            }
            ::close(bare);
        }

        auto mapping = file.map(PlacementSize, PROT_READ);
        report.check("mmap through the library succeeds", mapping.isMapped());
    }
    catch (const std::exception& failure)
    {
        report.raised("cloexec guard", failure);
    }

    mount.unlink(name);
}

// An ioctl number no verb owns has to reach the driver's default arm and answer ENOTTY.
void checkUnknownIoctl(fsuser::tests::Report& report, fsuser::tests::Mount& mount)
{
    report.section("an unknown ioctl answers ENOTTY");

    const auto name = caseName("contract_unknown_ioctl");

    try
    {
        auto file = fsuser::tests::placeFile(mount, name);

        const std::int32_t taken = fsuser::testing::openRaw(mount.getPoint(), name, O_RDWR | O_CLOEXEC);
        report.check("a cloexec open succeeds", taken >= 0);
        if (taken >= 0)
        {
            errno = 0;
            const std::int32_t answered = fsuser::testing::sendUnknownIoctl(taken);
            report.checkErrno("an unknown ioctl returns -1 with ENOTTY",
                              answered == -1 && errno == ENOTTY, errno);
            ::close(taken);
        }
    }
    catch (const std::exception& failure)
    {
        report.raised("unknown ioctl", failure);
    }

    mount.unlink(name);
}

// A size change has to come through an open descriptor, so truncate(2) by path answers EOPNOTSUPP
// and leaves the file unplaced.
void checkTruncateNeedsDescriptor(fsuser::tests::Report& report, fsuser::tests::Mount& mount)
{
    report.section("a size change needs an open descriptor");

    const auto name = caseName("contract_truncate");
    const auto path = mount.pathTo(name);

    try
    {
        {
            auto file = mount.open(name, O_CREAT | O_RDWR, 0644);
        }

        errno = 0;
        const std::int32_t truncated = ::truncate(path.c_str(), static_cast<::off_t>(PlacementSize));
        report.checkErrno("truncate by path is refused with EOPNOTSUPP",
                          truncated == -1 && errno == EOPNOTSUPP, errno);

        const auto row = findRowByName(name);
        report.check("region_info still shows the entry unplaced",
                     row && row->size == 0 && row->offset == 0);

        auto file = mount.open(name, O_RDWR);
        report.check("the file reopens", file.isOpen());
        report.check("ftruncate on the descriptor places it",
                     ::ftruncate(file.get(), static_cast<::off_t>(PlacementSize)) == 0);
    }
    catch (const std::exception& failure)
    {
        report.raised("truncate needs a descriptor", failure);
    }

    mount.unlink(name);
}

}  // namespace

int main(int argc, char** argv)
{
    const auto body = [](fsuser::tests::Report& report, fsuser::tests::Mounts_t& mounts)
    {
        auto mount = fsuser::tests::Mount(mounts.first());
        checkCloexecGuard(report, mount);
        checkUnknownIoctl(report, mount);
        checkTruncateNeedsDescriptor(report, mount);
    };
    return fsuser::tests::runCase(argc, argv, "test_contract", 1, body);
}
