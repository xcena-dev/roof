// SPDX-License-Identifier: Apache-2.0
//
// harness.hpp -- what every case in this directory shares: a check that counts, and the mounts.
//
// No framework. A case is a binary that takes its mount points on the command line, runs its
// checks, and returns the number that failed, which is what ctest reads.

#pragma once

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "fs/errors.hpp"
#include "fs/file.hpp"
#include "harness/mount.hpp"

namespace fsuser::tests
{

constexpr std::uint64_t PlacementSize = DaxAlignment;

// What ctest reads as a skip rather than a pass or a failure, per SKIP_RETURN_CODE in this
// directory's CMakeLists.
constexpr std::int32_t SkipStatus = 77;

class Report
{
public:
    // @what names the claim, not the step: what a reader sees on a red line is this string.
    void check(std::string_view what, bool passed)
    {
        ++total_;
        if (!passed)
        {
            ++failed_;
        }
        std::printf("  %-4s %s\n", passed ? "ok" : "FAIL", std::string{what}.c_str());
    }

    // For a claim about a specific errno, where the number the kernel actually chose is the whole
    // diagnosis and a bare FAIL sends the reader back to the source.
    void checkErrno(std::string_view what, bool passed, std::int32_t saved)
    {
        ++total_;
        if (!passed)
        {
            ++failed_;
        }
        std::printf("  %-4s %s (errno=%d %s)\n", passed ? "ok" : "FAIL",
                    std::string{what}.c_str(), saved, ::strerror(saved));
    }

    // A refusal the library reports by throwing. Passing @work as a callable keeps the try out of
    // the case, where a dozen of them would bury what each one is claiming.
    template <typename T_Work>
    void checkRefused(std::string_view what, T_Work&& work)
    {
        try
        {
            work();
        }
        catch (const std::exception&)
        {
            check(what, true);
            return;
        }
        check(what, false);
    }

    // A refusal that has to arrive as one particular errno. Two guards on the same verb both throw,
    // so the number is the only thing that says which one answered.
    template <typename T_Work>
    void checkRefusedWith(std::string_view what, std::errc wanted, T_Work&& work)
    {
        try
        {
            work();
        }
        catch (const FsCodedError& refusal)
        {
            checkErrno(what, refusal.code() == wanted, refusal.code().value());
            return;
        }
        catch (const std::exception& other)
        {
            raised(what, other);
            return;
        }
        check(what, false);
    }

    // The other half: work that has to go through, reported by name when it does not.
    template <typename T_Work>
    void checkAccepted(std::string_view what, T_Work&& work)
    {
        try
        {
            work();
        }
        catch (const std::exception& failure)
        {
            raised(what, failure);
            return;
        }
        check(what, true);
    }

    // A case that threw where it should not is one failure, not a crashed run: the cases after it
    // still have something to say.
    void raised(std::string_view what, const std::exception& failure)
    {
        ++total_;
        ++failed_;
        std::printf("  FAIL %s: %s\n", std::string{what}.c_str(), failure.what());
    }

    // An observation the case makes without claiming an outcome, for a question whose answer is what
    // the run is there to find out. It counts towards neither total.
    void note(std::string_view what) const
    {
        std::printf("  --   %s\n", std::string{what}.c_str());
    }

    void section(std::string_view title) const
    {
        std::printf("\n=== %s ===\n", std::string{title}.c_str());
    }

    // A run that reached no check measured nothing, and nothing is a skip rather than a pass: the
    // green it would otherwise report reads as a claim the case never made.
    [[nodiscard]] std::int32_t summarise(std::string_view name) const
    {
        std::printf("\n%s: %d of %d passed\n", std::string{name}.c_str(), total_ - failed_, total_);
        if (failed_ == 0 && total_ == 0)
        {
            return SkipStatus;
        }
        return failed_;
    }

    // For a case that ends up with nothing to measure: it may report a skip only while everything it
    // did reach still holds.
    [[nodiscard]] bool anyFailed() const noexcept
    {
        return failed_ > 0;
    }

private:
    std::int32_t total_ = 0;
    std::int32_t failed_ = 0;
};

// A case that needs root for one step still creates under the account the daemon's rules name,
// because the kernel attests the real uid. The effective ids stay root so the step still works.
[[nodiscard]] inline bool dropRealIdsToInvoker()
{
    const char* const uidText = std::getenv("SUDO_UID");
    const char* const gidText = std::getenv("SUDO_GID");
    if (uidText == nullptr || gidText == nullptr)
    {
        return false;
    }
    const auto realUid = static_cast<::uid_t>(std::atoi(uidText));
    const auto realGid = static_cast<::gid_t>(std::atoi(gidText));
    return ::setregid(realGid, 0) == 0 && ::setreuid(realUid, 0) == 0;
}

// One value handed between a parent and a forked child, for the moment one of them waits on
// something it cannot see: a grant the parent has not written yet, or a result from a child that
// will not live to return one.
class Baton
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────
    // Closed on exec, so a child that execs leaves neither end open in the image that follows.
    Baton() noexcept
    {
        if (::pipe2(ends_.data(), O_CLOEXEC) != 0)
        {
            ends_.fill(-1);
        }
    }
    Baton(const Baton&) = delete;
    ~Baton()
    {
        shut(ends_[0]);
        shut(ends_[1]);
    }

    // ── operator= ──────────────────────────────────────────────────
    Baton& operator=(const Baton&) = delete;

    // ── public methods ─────────────────────────────────────────────
    void pass(std::int32_t value) noexcept
    {
        if (ends_[1] >= 0)
        {
            const std::int64_t wrote = ::write(ends_[1], &value, sizeof(value));
            static_cast<void>(wrote);
        }
    }

    // Closes this side's writing end first, so a peer that died without passing ends the read here
    // instead of leaving it blocked. Answers -1 when nothing arrived.
    [[nodiscard]] std::int32_t take() noexcept
    {
        shut(ends_[1]);
        std::int32_t value = -1;
        if (ends_[0] < 0 || ::read(ends_[0], &value, sizeof(value)) != sizeof(value))
        {
            return -1;
        }
        return value;
    }

    // ── accessors ──────────────────────────────────────────────────
    [[nodiscard]] bool isOpen() const noexcept
    {
        return ends_[0] >= 0;
    }

private:
    static void shut(std::int32_t& end) noexcept
    {
        if (end >= 0)
        {
            ::close(end);
            end = -1;
        }
    }

private:
    std::array<std::int32_t, 2> ends_{-1, -1};
};

// Two Batons used as one duplex channel between a parent and a child: begin releases the child into
// its next step, ack answers back with what it saw.
struct Handoff_t
{
    Baton begin;
    Baton ack;

    [[nodiscard]] bool isOpen() const noexcept
    {
        return begin.isOpen() && ack.isOpen();
    }
};

// A name no other run of this case is using, so two of them on one mount do not collide.
[[nodiscard]] inline std::string caseName(const char* stem)
{
    return std::string{stem} + "_" + std::to_string(::getpid());
}

// The mounts a case was pointed at. Two of them is the shape that shows anything cross-node, and a
// case that needs only one uses the first.
struct Mounts_t
{
    std::vector<std::string> points;

    [[nodiscard]] const std::string& first() const
    {
        return points.front();
    }
    [[nodiscard]] const std::string& second() const
    {
        return points.at(1);
    }
};

// Returns false and says so when too few mounts were named, since a case that guesses a path
// silently tests nothing. @argc keeps main's own type, which the language fixes as int.
[[nodiscard]] inline bool takeMounts(int argc, char** argv, std::uint64_t wanted, Mounts_t& into)
{
    for (std::int32_t index = 1; index < argc; ++index)
    {
        into.points.emplace_back(argv[index]);
    }
    if (into.points.size() < wanted)
    {
        std::fprintf(stderr, "usage: %s <mount> [<mount2> ...]   (%zu needed, %zu given)\n",
                     argv[0], static_cast<std::size_t>(wanted), into.points.size());
        return false;
    }
    return true;
}

// A name created and placed to PlacementSize in one call. @extraFlags rides beside O_CREAT |
// O_RDWR for a caller that needs O_EXCL too.
[[nodiscard]] inline File placeFile(Mount& mount, const std::string& name,
                                    std::int32_t extraFlags = 0)
{
    auto file = mount.open(name, O_CREAT | O_RDWR | extraFlags, 0644);
    file.resize(PlacementSize);
    return file;
}

// The door a peer's own row gets written through: the owner cannot write that row itself, so it
// stands the default open for @work and shuts it again right after.
template <typename T_Work>
void runThroughOpenDoor(File& file, Permission doorPerms, T_Work&& work)
{
    file.setDefaultPermission(doorPerms);
    work();
    file.setDefaultPermission(Permission::None);
}

// What ctest reads when a case was given too few mounts to say anything.
constexpr std::int32_t UsageStatus = 2;

// Runs @body under @name against the mounts the command line named, and answers the code ctest
// reads. A throw @body did not catch is one failure rather than a terminate, which the runner
// would report only as an aborted subprocess.
template <typename T_Body>
[[nodiscard]] std::int32_t runCase(int argc, char** argv, const char* name, std::uint64_t mountsNeeded,
                                   T_Body&& body)
{
    Mounts_t mounts;
    if (!takeMounts(argc, argv, mountsNeeded, mounts))
    {
        return UsageStatus;
    }

    Report report;
    try
    {
        body(report, mounts);
    }
    catch (const std::exception& failure)
    {
        report.raised(std::string{name} + " setup", failure);
    }
    return report.summarise(name);
}

}  // namespace fsuser::tests
