// SPDX-License-Identifier: Apache-2.0
//
// test_thread_account -- which account a thread is judged under.
//
// A row naming a uid is a grant to that account, and the account it names may take it back without
// ADMIN. Every other caller needs ADMIN for the same revoke, so the answer says which uid the
// kernel read.
//
// A thread can change its own uid while its leader keeps the account the process runs as. Reading
// the thread's would give one thread of a process an account the rest of it does not have, and
// threads sharing an address space cannot keep that apart.
//
//   test_thread_account <mount-a>

#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <thread>

#include "fs/errors.hpp"
#include "fs/file.hpp"
#include "harness/harness.hpp"
#include "harness/mount.hpp"

namespace
{

using fsuser::tests::caseName;

// An account the invoker is not, so a caller judged under it holds no row on this region.
constexpr ::uid_t StrangerUid = 65534;
constexpr ::uid_t SecondStrangerUid = 65533;

// What a child reports back, since a child cannot throw across the fork.
constexpr std::int32_t RevokeAllowed = 0;
constexpr std::int32_t NoPrivilegeToChange = 254;
constexpr std::int32_t UnexpectedFailure = 255;

[[nodiscard]] ::uid_t pickStrangerUid()
{
    return ::getuid() == StrangerUid ? SecondStrangerUid : StrangerUid;
}

// The raw call and not the library's, which carries the change to every thread of the process.
[[nodiscard]] bool changeThisThreadsUid(::uid_t wanted)
{
    const auto asked = static_cast<std::int64_t>(wanted);
    return ::syscall(SYS_setresuid, asked, asked, asked) == 0;
}

// Whether this run may separate a thread's account from its leader's at all. Without it the thread
// below would keep the leader's uid for want of privilege and the check would claim nothing.
[[nodiscard]] bool canChangeOneThreadsUid(::uid_t wanted)
{
    bool allowed = false;
    std::thread prober{[&allowed, wanted]
                       {
                           allowed = changeThisThreadsUid(wanted);
                       }};
    prober.join();
    return allowed;
}

// 0 when the kernel let the revoke through, and the errno otherwise.
[[nodiscard]] std::int32_t attemptRevoke(fsuser::File& file, std::uint32_t rowUid)
{
    try
    {
        file.revokePermission(rowUid, fsuser::AnyId);
    }
    catch (const fsuser::FsCodedError& refused)
    {
        return refused.code().value();
    }
    catch (const std::exception&)
    {
        return UnexpectedFailure;
    }
    return RevokeAllowed;
}

// The control: one account for the whole process, and not the one the row names.
[[nodiscard]] std::int32_t revokeAsStranger(const std::string& path, std::uint32_t rowUid,
                                            ::uid_t strangerUid)
{
    auto file = fsuser::File::open(path, O_RDWR | O_CLOEXEC);
    if (!changeThisThreadsUid(strangerUid))
    {
        return NoPrivilegeToChange;
    }
    return attemptRevoke(file, rowUid);
}

// The claim: the leader keeps the account the row names and the thread that asks has taken another.
[[nodiscard]] std::int32_t revokeFromChangedThread(const std::string& path, std::uint32_t rowUid,
                                                   ::uid_t strangerUid)
{
    // Opened while the process still has one account, so what the kernel judges is the revoke and
    // not the open.
    auto file = fsuser::File::open(path, O_RDWR | O_CLOEXEC);

    std::int32_t answered = NoPrivilegeToChange;
    std::thread caller{[&answered, &file, rowUid, strangerUid]
                       {
                           if (changeThisThreadsUid(strangerUid))
                           {
                               answered = attemptRevoke(file, rowUid);
                           }
                       }};
    caller.join();
    return answered;
}

// In a child, because the owner reaches the region by a path no delegation is consulted on. Answers
// what the child left, or -1 when it did not exit on its own.
template <typename T_Attempt>
[[nodiscard]] std::int32_t runInChild(T_Attempt&& attempt)
{
    const ::pid_t child = ::fork();
    if (child == 0)
    {
        std::int32_t answered = UnexpectedFailure;
        try
        {
            answered = attempt();
        }
        catch (const std::exception&)
        {
            answered = UnexpectedFailure;
        }
        ::_exit(answered);
    }
    if (child < 0)
    {
        return -1;
    }

    std::int32_t status = 0;
    if (::waitpid(child, &status, 0) != child || WIFEXITED(status) == 0)
    {
        return -1;
    }
    return WEXITSTATUS(status);
}

void checkAThreadHoldsItsLeadersAccount(fsuser::tests::Report& report, fsuser::tests::Mount& mount,
                                        ::uid_t strangerUid)
{
    report.section("a thread that took an account of its own");

    const auto name = caseName("thread_account");
    const auto rowUid = static_cast<std::uint32_t>(::getuid());

    try
    {
        auto file = fsuser::tests::placeFile(mount, name);
        file.grantPermission(rowUid, fsuser::AnyId, fsuser::Permission::Read);
        report.check("the owner writes a row naming the invoking account", true);

        const std::int32_t control = runInChild(
            [&]
            {
                return revokeAsStranger(mount.pathTo(name), rowUid, strangerUid);
            });
        report.checkErrno("a caller under another account cannot take that row off",
                          control == EACCES, control);

        const std::int32_t fromThread = runInChild(
            [&]
            {
                return revokeFromChangedThread(mount.pathTo(name), rowUid, strangerUid);
            });
        report.checkErrno("a thread that changed only its own uid still holds its leader's row",
                          fromThread == RevokeAllowed, fromThread);
    }
    catch (const std::exception& failure)
    {
        report.raised("thread account", failure);
    }

    mount.unlink(name);
}

}  // namespace

int main(int argc, char** argv)
{
    fsuser::tests::Mounts_t mounts;
    if (!fsuser::tests::takeMounts(argc, argv, 1, mounts))
    {
        return fsuser::tests::UsageStatus;
    }

    // Under sudo the rules name the invoking account and the kernel attests the real uid, so the
    // real ids drop to it while the effective ones stay root.
    static_cast<void>(fsuser::tests::dropRealIdsToInvoker());

    const ::uid_t strangerUid = pickStrangerUid();
    if (!canChangeOneThreadsUid(strangerUid))
    {
        std::printf(
            "test_thread_account: separating one thread's uid from its leader's needs "
            "CAP_SETUID, which this run does not carry\n");
        return fsuser::tests::SkipStatus;
    }

    fsuser::tests::Report report;
    try
    {
        auto mount = fsuser::tests::Mount(mounts.first());
        checkAThreadHoldsItsLeadersAccount(report, mount, strangerUid);
    }
    catch (const std::exception& failure)
    {
        report.raised("test_thread_account", failure);
    }

    return report.summarise("test_thread_account");
}
