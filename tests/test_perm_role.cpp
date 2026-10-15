// SPDX-License-Identifier: Apache-2.0
//
// test_perm_role -- a grant that names a workload by its role, and not an account.
//
// The owner shares one file with the workload the helper attests as group travel, role hotel, and
// keeps a second file back. Both workloads run under the owner's account, so only the role can tell
// them apart: this binary attests as travel/planner and role_consumer as travel/hotel.
//
//   test_perm_role <mount-a> <mount-b>

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <string>
#include <system_error>

#include "fs/file.hpp"
#include "harness/harness.hpp"
#include "harness/mount.hpp"
#include "harness/probe.hpp"

namespace
{

// The exit status of a consumer that never ran its probe, outside the errno range.
constexpr std::int32_t ExecFailed = 251;

// What the identity rules attest for role_consumer.
constexpr const char* ConsumerGroup = "travel";
constexpr const char* ConsumerRole = "hotel";

// A node id past the last one the slot table holds.
constexpr std::uint32_t BeyondLastNode = 9;

using fsuser::tests::caseName;
using fsuser::tests::PlacementSize;

// Reaps @child and answers the status it exited with, or -1 when it did not exit.
[[nodiscard]] std::int32_t reap(::pid_t child)
{
    std::int32_t status = 0;
    if (::waitpid(child, &status, 0) != child || (WIFEXITED(status) == 0))
    {
        return -1;
    }
    return WEXITSTATUS(status);
}

// The errno role_consumer's @action left on @name through @mount, or 0 when it landed. A fresh
// process each time, so no row an earlier one earned can answer for this one.
[[nodiscard]] std::int32_t runConsumer(const std::string& mount, const std::string& name, const char* action)
{
    const ::pid_t child = ::fork();
    if (child < 0)
    {
        return -1;
    }
    if (child == 0)
    {
        ::execl(FS_ROLE_CONSUMER, FS_ROLE_CONSUMER, mount.c_str(), name.c_str(), action,
                static_cast<char*>(nullptr));
        ::_exit(ExecFailed);
    }
    return reap(child);
}

// The same probe from a child of this binary, which attests as this binary's own role.
[[nodiscard]] std::int32_t runPlannerChild(const std::string& mount, const std::string& name)
{
    const ::pid_t child = ::fork();
    if (child < 0)
    {
        return -1;
    }
    if (child == 0)
    {
        ::_exit(fsuser::tests::openAndProbeReadMap(mount, name, PlacementSize));
    }
    return reap(child);
}

void checkRoleGrant(fsuser::tests::Report& report, fsuser::tests::Mount& peer,
                    const std::string& shared, const std::string& kept)
{
    report.section("a workload the role names, and files it does not");

    const auto mapped = runConsumer(peer.getPoint(), shared, "map");
    report.checkErrno("the hotel workload maps the shared file READ", mapped == 0, mapped);

    const auto read = runConsumer(peer.getPoint(), shared, "read");
    report.checkErrno("and reads it", read == 0, read);

    // The same account and the same role as above, but no grant on this file names that role.
    const auto refused = runConsumer(peer.getPoint(), kept, "map");
    report.checkErrno("the same workload is refused the file nothing was granted on", refused == EACCES, refused);
}

void checkOtherWorkload(fsuser::tests::Report& report, fsuser::tests::Mount& peer,
                        const std::string& shared)
{
    report.section("another workload of the same account");

    // Planner is not hotel, and a child of the owner's binary is another process, so not the owner.
    const auto planner = runPlannerChild(peer.getPoint(), shared);
    report.checkErrno("a travel/planner process is refused the file shared with hotel", planner == EACCES, planner);
}

void checkTargetNode(fsuser::tests::Report& report, fsuser::tests::Mount& owner,
                     fsuser::tests::Mount& peer, fsuser::File& file, const std::string& name)
{
    report.section("a role grant aimed at one node");

    const auto ownerNode = owner.getNodeId();
    report.check("the owner's mount has a node id", ownerNode != 0);
    report.checkAccepted("the owner grants hotel READ on its own node only",
                         [&]
                         {
                             file.grantRole(ConsumerGroup, ConsumerRole, fsuser::Permission::Read, ownerNode);
                         });

    const auto elsewhere = runConsumer(peer.getPoint(), name, "map");
    report.checkErrno("the hotel workload on the other node is refused", elsewhere == EACCES, elsewhere);

    const auto here = runConsumer(owner.getPoint(), name, "map");
    report.checkErrno("and the one on the owner's node maps it", here == 0, here);
}

void checkRevoke(fsuser::tests::Report& report, fsuser::tests::Mount& peer, fsuser::File& file,
                 const std::string& name)
{
    report.section("the role grant taken back");

    report.checkAccepted("the owner revokes the hotel grant",
                         [&]
                         {
                             file.revokeRole(ConsumerGroup, ConsumerRole);
                         });

    const auto after = runConsumer(peer.getPoint(), name, "map");
    report.checkErrno("a new hotel process is refused", after == EACCES, after);
}

void checkLabels(fsuser::tests::Report& report, fsuser::File& file)
{
    report.section("labels and nodes the kernel will not write");

    report.checkRefusedWith("an empty group", std::errc::invalid_argument,
                            [&]
                            {
                                file.grantRole("", ConsumerRole, fsuser::Permission::Read);
                            });

    // The record holds 16 bytes, so one more than MaxLabelBytes leaves no room for the terminator.
    const std::string tooLong(fsuser::MaxLabelBytes + 1, 'r');
    report.checkRefusedWith("a role of 16 bytes", std::errc::invalid_argument,
                            [&]
                            {
                                file.grantRole(ConsumerGroup, tooLong, fsuser::Permission::Read);
                            });

    report.checkRefusedWith("a node past the last one", std::errc::invalid_argument,
                            [&]
                            {
                                file.grantRole(ConsumerGroup, ConsumerRole, fsuser::Permission::Read,
                                               BeyondLastNode);
                            });
}

}  // namespace

int main(int argc, char** argv)
{
    const auto body = [](fsuser::tests::Report& report, fsuser::tests::Mounts_t& mounts)
    {
        auto owner = fsuser::tests::Mount(mounts.first());
        auto peer = fsuser::tests::Mount(mounts.second());

        const auto shared = caseName("role_booking");
        const auto kept = caseName("role_private");
        const auto aimed = caseName("role_node");

        auto sharedFile = fsuser::tests::placeFile(owner, shared);
        auto keptFile = fsuser::tests::placeFile(owner, kept);
        auto aimedFile = fsuser::tests::placeFile(owner, aimed);
        for (auto* file : {&sharedFile, &keptFile, &aimedFile})
        {
            file->setDefaultPermission(fsuser::Permission::None);
        }

        report.checkAccepted("the owner grants hotel READ on the shared file",
                             [&]
                             {
                                 sharedFile.grantRole(ConsumerGroup, ConsumerRole, fsuser::Permission::Read);
                             });

        checkRoleGrant(report, peer, shared, kept);
        checkOtherWorkload(report, peer, shared);
        checkTargetNode(report, owner, peer, aimedFile, aimed);
        checkRevoke(report, peer, sharedFile, shared);
        checkLabels(report, sharedFile);

        owner.unlink(shared);
        owner.unlink(kept);
        owner.unlink(aimed);
    };
    return fsuser::tests::runCase(argc, argv, "test_perm_role", 2, body);
}
