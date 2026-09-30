// SPDX-License-Identifier: Apache-2.0
//
// test_security_cloexec_alias -- the close-on-exec guard answers for the descriptor in the call,
// not for whichever alias of the same file the caller happens to hold.
//
// dup2 clears FD_CLOEXEC on the descriptor it creates, so a caller can hold one marked alias and
// one bare alias of one open file. A guard that stops at the first alias it finds admits calls
// through the bare one, and the bare one is what survives execve.
//
// Both directions are checked, because the guard has to reach the same verdict either way: the
// bare alias above the marked one, and the bare alias below it.
//
//   test_security_cloexec_alias <mount-a>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <string>

#include "fs/testing.hpp"
#include "harness/harness.hpp"
#include "harness/mount.hpp"

namespace
{

using fsuser::tests::caseName;
using fsuser::tests::PlacementSize;

// Far enough above the descriptor the open returned that nothing else in this process is sitting
// there, so dup2 creates a slot rather than replacing one.
constexpr std::int32_t AliasDistance = 50;

// Whether a one-byte read at offset 0 through @descriptor was admitted. errno is left as the read
// set it, so the caller reports which refusal answered.
[[nodiscard]] bool readsThrough(std::int32_t descriptor)
{
    std::array<std::uint8_t, 1> byte{};
    errno = 0;
    return ::pread(descriptor, byte.data(), byte.size(), 0) == 1;
}

// The same for a mapping, which is the path a consumer actually takes.
[[nodiscard]] bool mapsThrough(std::int32_t descriptor)
{
    errno = 0;
    void* mapped = ::mmap(nullptr, PlacementSize, PROT_READ, MAP_SHARED, descriptor, 0);
    if (mapped == MAP_FAILED)
    {
        return false;
    }
    ::munmap(mapped, PlacementSize);
    return true;
}

[[nodiscard]] bool carriesCloexec(std::int32_t descriptor)
{
    const std::int32_t flags = ::fcntl(descriptor, F_GETFD);
    return flags >= 0 && (flags & FD_CLOEXEC) != 0;
}

// The bare alias sits above the marked one, which is the order dup2 produces from a compliant
// open. The marked alias is used first, so the guard's own lookup is warm before the bare one runs.
void checkBareAliasAboveMarked(fsuser::tests::Report& report, const std::string& mountPoint,
                               const std::string& name)
{
    report.section("a bare alias created above a marked descriptor");

    const std::int32_t marked = fsuser::testing::openRaw(mountPoint, name, O_RDWR | O_CLOEXEC);
    report.check("the marked open succeeds", marked >= 0);
    if (marked < 0)
    {
        return;
    }

    report.checkErrno("the marked descriptor reads", readsThrough(marked), errno);

    const std::int32_t bare = ::dup2(marked, marked + AliasDistance);
    report.check("dup2 makes a second descriptor on the same file", bare >= 0);
    if (bare < 0)
    {
        ::close(marked);
        return;
    }
    // Without this the case would pass on a host where dup2 kept the bit, having tested nothing.
    report.check("dup2 leaves the new descriptor without close-on-exec", !carriesCloexec(bare));

    report.checkErrno("a read through the bare alias is refused", !readsThrough(bare), errno);
    report.checkErrno("a mapping through the bare alias is refused", !mapsThrough(bare), errno);

    report.check("fcntl marks the alias", ::fcntl(bare, F_SETFD, FD_CLOEXEC) == 0);
    report.checkErrno("the alias reads once it carries close-on-exec", readsThrough(bare), errno);

    ::close(bare);
    ::close(marked);
}

// The bare alias sits below the marked one, so a guard scanning upwards meets the bare one first.
// Closing it has to be what admits the marked descriptor, and nothing else.
void checkBareAliasBelowMarked(fsuser::tests::Report& report, const std::string& mountPoint,
                               const std::string& name)
{
    report.section("a marked alias created above a bare descriptor");

    const std::int32_t bare = fsuser::testing::openRaw(mountPoint, name, O_RDWR);
    report.check("the bare open succeeds", bare >= 0);
    if (bare < 0)
    {
        return;
    }

    const std::int32_t marked = ::dup(bare);
    report.check("dup makes a second descriptor on the same file", marked >= 0);
    if (marked < 0)
    {
        ::close(bare);
        return;
    }
    report.check("fcntl marks the alias", ::fcntl(marked, F_SETFD, FD_CLOEXEC) == 0);

    report.checkErrno("the marked alias is refused while the bare one is open",
                      !readsThrough(marked), errno);

    ::close(bare);
    report.checkErrno("the marked alias reads once the bare one is closed", readsThrough(marked),
                      errno);

    ::close(marked);
}

}  // namespace

int main(int argc, char** argv)
{
    // Nothing here needs a privileged account, and dup2 on a descriptor needs none either.
    static_cast<void>(fsuser::tests::dropRealIdsToInvoker());

    const auto body = [](fsuser::tests::Report& report, fsuser::tests::Mounts_t& mounts)
    {
        const auto name = caseName("security_cloexec_alias");

        auto mount = fsuser::tests::Mount(mounts.first());
        auto file = fsuser::tests::placeFile(mount, name);

        checkBareAliasAboveMarked(report, mount.getPoint(), name);
        checkBareAliasBelowMarked(report, mount.getPoint(), name);

        mount.unlink(name);
    };
    return fsuser::tests::runCase(argc, argv, "test_security_cloexec_alias", 1, body);
}
