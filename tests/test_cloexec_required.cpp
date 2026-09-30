// SPDX-License-Identifier: Apache-2.0
//
// test_cloexec_required -- every data path on this filesystem refuses a descriptor that lacks
// FD_CLOEXEC, checked ahead of anything the descriptor's own permissions would otherwise allow.
//
//   test_cloexec_required <mount-a>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <string>

#include "fs/testing.hpp"
#include "harness/harness.hpp"
#include "harness/mount.hpp"
#include "uapi.h"

namespace
{

using fsuser::tests::caseName;
using fsuser::tests::PlacementSize;

// The three data paths that read file_require_cloexec before anything else, and then the same
// descriptor once it carries the bit the kernel asks for.
void checkPathsRefuseThenAdmit(fsuser::tests::Report& report, const std::string& mountPoint,
                               const std::string& name)
{
    report.section("a descriptor with no FD_CLOEXEC");

    const std::int32_t raw = fsuser::testing::openRaw(mountPoint, name, O_RDWR);
    report.check("the raw open itself succeeds", raw >= 0);
    if (raw < 0)
    {
        return;
    }

    std::array<std::uint8_t, 64> buffer{};
    errno = 0;
    const ::ssize_t readWithout = ::read(raw, buffer.data(), buffer.size());
    report.checkErrno("read without close-on-exec is refused",
                      readWithout < 0 && errno == EACCES, errno);

    errno = 0;
    void* mappedWithout = ::mmap(nullptr, PlacementSize, PROT_READ, MAP_SHARED, raw, 0);
    report.checkErrno("mmap without close-on-exec is refused",
                      mappedWithout == MAP_FAILED && errno == EACCES, errno);
    if (mappedWithout != MAP_FAILED)
    {
        ::munmap(mappedWithout, PlacementSize);
    }

    fs_perm_req request{};
    request.uid = 0;
    request.gid = 0;
    request.perms = FS_PERM_READ;
    errno = 0;
    const std::int32_t ioctlWithout = ::ioctl(raw, FS_IOC_PERM_SET_DEFAULT, &request);
    report.checkErrno("the perm ioctl without close-on-exec is refused",
                      ioctlWithout < 0 && errno == EACCES, errno);

    report.check("fcntl sets FD_CLOEXEC on the same descriptor",
                 ::fcntl(raw, F_SETFD, FD_CLOEXEC) == 0);

    errno = 0;
    const ::ssize_t readWith = ::read(raw, buffer.data(), buffer.size());
    report.checkErrno("read succeeds once the descriptor carries close-on-exec",
                      readWith >= 0, errno);

    errno = 0;
    void* mappedWith = ::mmap(nullptr, PlacementSize, PROT_READ, MAP_SHARED, raw, 0);
    report.checkErrno("mmap succeeds once the descriptor carries close-on-exec",
                      mappedWith != MAP_FAILED, errno);
    if (mappedWith != MAP_FAILED)
    {
        ::munmap(mappedWith, PlacementSize);
    }

    ::close(raw);
}

}  // namespace

int main(int argc, char** argv)
{
    const auto body = [](fsuser::tests::Report& report, fsuser::tests::Mounts_t& mounts)
    {
        const auto name = caseName("cloexec_required");

        auto mount = fsuser::tests::Mount(mounts.first());
        auto file = fsuser::tests::placeFile(mount, name);

        checkPathsRefuseThenAdmit(report, mount.getPoint(), name);

        mount.unlink(name);
    };
    return fsuser::tests::runCase(argc, argv, "test_cloexec_required", 1, body);
}
