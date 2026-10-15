// SPDX-License-Identifier: Apache-2.0
//
// role_consumer -- the process test_perm_role maps a file as, under a role of its own.
//
// A separate executable because the daemon attests a process by the binary it runs, and the identity
// rules give this path a different role than the case that launches it.
//
// It exits with the errno the kernel gave, or 0 when the access landed, which is the same convention
// the forked children in the suite use.
//
//   role_consumer <mount> <name> map|read

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <string>

#include "fs/testing.hpp"
#include "harness/harness.hpp"
#include "harness/probe.hpp"

namespace
{

constexpr std::int32_t UsageStatus = 2;

// Reads the first byte through read(2), so the access check that decides is the one on the file and
// not on a mapping.
[[nodiscard]] std::int32_t openAndProbeRead(const std::string& mount, const std::string& name)
{
    const std::int32_t taken = fsuser::testing::openRaw(mount, name, O_RDWR | O_CLOEXEC);
    if (taken < 0)
    {
        return errno;
    }

    char first = 0;
    errno = 0;
    const std::int64_t got = ::pread(taken, &first, sizeof(first), 0);
    const std::int32_t answer = (got == static_cast<std::int64_t>(sizeof(first))) ? 0 : errno;
    ::close(taken);
    return answer;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        std::fprintf(stderr, "usage: %s <mount> <name> map|read\n", argv[0]);
        return UsageStatus;
    }

    const std::string mount{argv[1]};
    const std::string name{argv[2]};
    const std::string action{argv[3]};
    if (action == "read")
    {
        return openAndProbeRead(mount, name);
    }
    if (action == "map")
    {
        return fsuser::tests::openAndProbeReadMap(mount, name, fsuser::tests::PlacementSize);
    }

    std::fprintf(stderr, "%s: unknown action %s\n", argv[0], argv[3]);
    return UsageStatus;
}
