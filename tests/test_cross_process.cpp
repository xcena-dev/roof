// SPDX-License-Identifier: Apache-2.0
//
// test_cross_process -- one file's whole life, watched from another process through the other mount.
//
// Two processes and not two opens in one. Ownership and every delegation key on the tgid, so a
// second open inside one process is still the owner's tgid and reaches the region by the owner's
// fast path. The observer here is a separate process, which is the only way a non-owner is a real
// non-owner.
//
// What lets it read at all is the region's default permission, set once by the owner. That covers
// everyone holding no delegation of their own, so no row is ever written for the observer and what is
// being watched stays the file rather than the grant.
//
// The observer takes no turn and holds no claim. open is a lookup and the turn exists for the calls
// that mutate shared state, none of which an observer makes.
//
//   test_cross_process <mount-a> <mount-b>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <exception>
#include <string>
#include <string_view>

#include "fs/file.hpp"
#include "fs/testing.hpp"
#include "harness/harness.hpp"
#include "harness/mount.hpp"
#include "harness/pattern.hpp"

namespace
{

using fsuser::tests::PlacementSize;
constexpr std::uint64_t PlacedWords = PlacementSize / sizeof(std::uint32_t);
constexpr std::uint32_t OwnerSeed = 0x5EED5EEDU;

// What one observation answers. Zero is the outcome the step was waiting for, and anything else is
// what the observer saw instead, so one channel carries both the verdict and the reason.
constexpr std::int32_t AsExpected = 0;
constexpr std::int32_t NothingSaid = -1;
constexpr std::int32_t WrongSize = 240;
constexpr std::int32_t WrongBytes = 241;
constexpr std::int32_t StillThere = 242;

using fsuser::tests::caseName;

[[nodiscard]] std::string explain(std::int32_t code)
{
    switch (code)
    {
        case AsExpected:
            return "as expected";
        case NothingSaid:
            return "the observer said nothing";
        case WrongSize:
            return "the size did not match";
        case WrongBytes:
            return "the bytes did not match";
        case StillThere:
            return "the name is still there";
        default:
            return ::strerror(code);
    }
}

// ── the observer, which runs in the child ───────────────────────────────

[[nodiscard]] std::string joinPath(const std::string& mount, const std::string& name)
{
    return mount + "/" + name;
}

[[nodiscard]] std::int32_t seeSize(const std::string& path, std::uint64_t wanted)
{
    struct ::stat seen = {};
    errno = 0;
    if (::stat(path.c_str(), &seen) != 0)
    {
        return errno;
    }
    return static_cast<std::uint64_t>(seen.st_size) == wanted ? AsExpected : WrongSize;
}

// Opens and maps for reading, and answers whether the words behind it are @seed's. Passing zero words
// asks only that the mapping was granted.
[[nodiscard]] std::int32_t seeBytes(const std::string& mount, const std::string& name,
                                    std::uint64_t words, std::uint32_t seed)
{
    const std::int32_t taken = fsuser::testing::openRaw(mount, name, O_RDONLY | O_CLOEXEC);
    if (taken < 0)
    {
        return errno;
    }
    errno = 0;
    void* mapped = ::mmap(nullptr, PlacementSize, PROT_READ, MAP_SHARED, taken, 0);
    if (mapped == MAP_FAILED)
    {
        const std::int32_t refused = errno;
        ::close(taken);
        return refused;
    }

    std::int32_t answer = AsExpected;
    const auto* reading = static_cast<const std::uint32_t*>(mapped);
    for (std::uint64_t index = 0; index < words; ++index)
    {
        if (reading[index] != fsuser::tests::patternWord(seed, index))
        {
            answer = WrongBytes;
            break;
        }
    }
    ::munmap(mapped, PlacementSize);
    ::close(taken);
    return answer;
}

[[nodiscard]] std::int32_t seeItGone(const std::string& path)
{
    struct ::stat seen = {};
    errno = 0;
    if (::stat(path.c_str(), &seen) == 0)
    {
        return StillThere;
    }
    return errno == ENOENT ? AsExpected : errno;
}

// Each step waits for the owner to finish its half, looks, and answers. Nothing here returns into the
// case: the owner's File is still constructed in this process and running its
// destructors would speak on the owner's connection to the daemon.
[[noreturn]] void observe(fsuser::tests::Baton& begin, fsuser::tests::Baton& back,
                          const std::string& mount, const std::string& name)
{
    const std::string path = joinPath(mount, name);

    static_cast<void>(begin.take());
    back.pass(seeSize(path, 0));

    static_cast<void>(begin.take());
    back.pass(seeSize(path, PlacementSize));
    back.pass(seeBytes(mount, name, 0, OwnerSeed));

    static_cast<void>(begin.take());
    back.pass(seeBytes(mount, name, PlacedWords, OwnerSeed));

    static_cast<void>(begin.take());
    back.pass(seeItGone(path));

    ::_exit(0);
}

// ── the owner, which runs in the parent ─────────────────────────────────

void checkTheObserverKeepsUp(fsuser::tests::Report& report, fsuser::tests::Baton& begin,
                             fsuser::tests::Baton& back, fsuser::tests::Mount& mount,
                             const std::string& name)
{
    auto reportStep = [&report, &back](std::string_view what)
    {
        const std::int32_t code = back.take();
        report.check(std::string{what} + ": " + explain(code), code == AsExpected);
    };

    report.section("the owner creates the name");
    auto file = mount.open(name, O_CREAT | O_RDWR, 0644);
    file.setDefaultPermission(fsuser::Permission::Read);
    begin.pass(1);
    reportStep("the observer sees a name with no size");

    report.section("the owner places the extent");
    file.resize(PlacementSize);
    begin.pass(1);
    reportStep("the observer sees the placed size");
    reportStep("the observer maps it under the region's default READ");

    report.section("the owner stores a pattern");
    {
        auto stored = file.map(PlacementSize, PROT_READ | PROT_WRITE);
        report.check("the owner maps writably", stored.isMapped());
        fsuser::tests::fillPattern(static_cast<std::uint32_t*>(stored.get()), PlacedWords,
                                   OwnerSeed);
    }
    begin.pass(1);
    reportStep("the observer finds the owner's pattern");

    report.section("the owner unlinks the name");
    mount.unlink(name);
    begin.pass(1);
    reportStep("the observer sees the name gone");
}

}  // namespace

int main(int argc, char** argv)
{
    fsuser::tests::Mounts_t mounts;
    if (!fsuser::tests::takeMounts(argc, argv, 2, mounts))
    {
        return 2;
    }

    fsuser::tests::Report report;
    const auto name = caseName("cross_process");

    fsuser::tests::Baton begin;
    fsuser::tests::Baton back;
    if (!begin.isOpen() || !back.isOpen())
    {
        report.check("the handoff pipes open", false);
        return report.summarise("test_cross_process");
    }

    const ::pid_t observer = ::fork();
    if (observer == 0)
    {
        observe(begin, back, mounts.second(), name);
    }
    report.check("fork succeeds", observer > 0);
    if (observer <= 0)
    {
        return report.summarise("test_cross_process");
    }

    try
    {
        auto mount = fsuser::tests::Mount(mounts.first());
        checkTheObserverKeepsUp(report, begin, back, mount, name);
    }
    catch (const std::exception& failure)
    {
        report.raised("test_cross_process owner", failure);
    }

    std::int32_t status = 0;
    const bool reaped = ::waitpid(observer, &status, 0) == observer;
    report.check("the observer exits rather than being signalled",
                 reaped && (WIFEXITED(status) != 0) && WEXITSTATUS(status) == 0);

    return report.summarise("test_cross_process");
}
