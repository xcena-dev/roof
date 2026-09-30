// SPDX-License-Identifier: Apache-2.0
//
// test_security_group_revoke -- who may take a group's row off a region.
//
// A row naming a group is a grant to everyone in it. Removing it takes access from all of them, so
// it asks for the same standing that writing it did. Only a uid narrows a row to one caller, and
// the self-revoke rule that asks for nothing reaches no further than that.
//
// The child is the principal under test: it is not the owner, so it holds no ADMIN of its own.
//
//   test_security_group_revoke <mount-a>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <exception>
#include <string>

#include "fs/errors.hpp"
#include "fs/file.hpp"
#include "harness/harness.hpp"
#include "harness/mount.hpp"

namespace
{

using fsuser::tests::caseName;

// What the child reports back, since a child cannot throw across the fork.
constexpr std::int32_t RevokeAllowed = 0;

// Opens the region by name and asks for the group row to come off. Answers 0 when the kernel let it
// through, and the errno otherwise.
[[noreturn]] void runGroupRevoke(const std::string& path, std::uint32_t groupId)
{
    std::int32_t answered = RevokeAllowed;
    try
    {
        auto file = fsuser::File::open(path, O_RDWR | O_CLOEXEC);
        file.revokePermission(fsuser::AnyId, groupId);
    }
    catch (const fsuser::FsCodedError& refused)
    {
        answered = refused.code().value();
    }
    catch (const std::exception&)
    {
        answered = 255;
    }
    ::_exit(answered);
}

void checkGroupRowNeedsAdmin(fsuser::tests::Report& report, fsuser::tests::Mount& mount)
{
    report.section("a group member taking the group's row off");

    const auto name = caseName("security_group_revoke");
    const auto groupId = static_cast<std::uint32_t>(::getgid());

    try
    {
        auto file = fsuser::tests::placeFile(mount, name);

        // The owner writes the row, which is the grant the whole group rides.
        file.grantPermission(fsuser::AnyId, groupId, fsuser::Permission::Read);
        report.check("the owner writes a row naming the group", true);

        const ::pid_t child = ::fork();
        if (child == 0)
        {
            runGroupRevoke(mount.pathTo(name), groupId);
        }
        report.check("fork succeeds", child > 0);
        if (child <= 0)
        {
            mount.unlink(name);
            return;
        }

        std::int32_t status = 0;
        const bool reaped = ::waitpid(child, &status, 0) == child;
        report.check("the child exits rather than being signalled",
                     reaped && (WIFEXITED(status) != 0));
        if (!reaped || (WIFEXITED(status) == 0))
        {
            mount.unlink(name);
            return;
        }

        // EACCES and not merely a refusal: a child that could not open the name would also fail,
        // and that answers nothing about who may take the row off.
        const std::int32_t left = WEXITSTATUS(status);
        report.checkErrno("a member with no ADMIN cannot take the group's row off",
                          left == EACCES, left);
    }
    catch (const std::exception& failure)
    {
        report.raised("security group revoke", failure);
    }

    mount.unlink(name);
}

}  // namespace

int main(int argc, char** argv)
{
    fsuser::tests::Mounts_t mounts;
    if (!fsuser::tests::takeMounts(argc, argv, 1, mounts))
    {
        return 2;
    }

    // The group the row names is this process's real gid, and the child that tries to take the row
    // off inherits it. Dropping here makes both the invoking account rather than root.
    static_cast<void>(fsuser::tests::dropRealIdsToInvoker());

    fsuser::tests::Report report;
    try
    {
        auto mount = fsuser::tests::Mount(mounts.first());
        checkGroupRowNeedsAdmin(report, mount);
    }
    catch (const std::exception& failure)
    {
        report.raised("test_security_group_revoke", failure);
    }

    return report.summarise("test_security_group_revoke");
}
