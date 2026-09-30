// SPDX-License-Identifier: Apache-2.0
//
// test_names_peer -- one namespace reached through two mounts.
//
// The index and the RAT live in the region, not in either mount, so a name one node links is a name
// the other node resolves with no message passing between them. What this case asks is that the two
// mounts never disagree: about which names exist, about what stat says of one, or about who may
// remove it.
//
// Removing is where the two mounts differ on purpose. A file's owner is the node and process that
// created it, so the same process reaching the file through the other mount is not its owner and has
// to be refused.
//
//   test_names_peer <mount-a> <mount-b>

#include <fcntl.h>
#include <sys/stat.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "fs/file.hpp"
#include "harness/harness.hpp"
#include "harness/listing.hpp"
#include "harness/mount.hpp"

namespace
{

constexpr std::int32_t Batch = 10;

using fsuser::tests::caseName;
using fsuser::tests::listNames;
using fsuser::tests::nameExists;

void checkANameCrossesBothWays(fsuser::tests::Report& report, fsuser::tests::Mount& first,
                               fsuser::tests::Mount& second, const fsuser::tests::Mounts_t& mounts,
                               const std::string& stem)
{
    report.section("a name each node links");

    const std::string& mountA = mounts.first();
    const std::string& mountB = mounts.second();

    const std::string fromA = stem + "_from_a";
    const std::string fromB = stem + "_from_b";

    auto madeOnA = first.open(fromA, O_CREAT | O_RDWR, 0644);
    report.check("the second mount resolves what the first linked", nameExists(mountB, fromA));

    auto madeOnB = second.open(fromB, O_CREAT | O_RDWR, 0644);
    report.check("the first mount resolves what the second linked", nameExists(mountA, fromB));

    report.check("both mounts list the same two names",
                 listNames(mountA, stem) == listNames(mountB, stem));

    first.unlink(fromA);
    report.check("a removal on the first mount is gone from the second",
                 !nameExists(mountB, fromA));
    second.unlink(fromB);
    report.check("a removal on the second mount is gone from the first",
                 !nameExists(mountA, fromB));
}

void checkStatAgrees(fsuser::tests::Report& report, fsuser::tests::Mount& first,
                     const std::string& mountA, const std::string& mountB,
                     const std::string& stem)
{
    report.section("what stat says through either mount");

    const std::string name = stem + "_stat";
    auto file = first.open(name, O_CREAT | O_RDWR, 0644);

    struct ::stat here = {};
    struct ::stat there = {};
    const bool bothRead = ::stat((mountA + "/" + name).c_str(), &here) == 0 &&
                          ::stat((mountB + "/" + name).c_str(), &there) == 0;
    report.check("stat succeeds through both mounts", bothRead);

    if (bothRead)
    {
        report.check("the mode agrees", here.st_mode == there.st_mode);
        report.check("it is a regular file on both", S_ISREG(here.st_mode) != 0);
        report.check("the owning uid agrees", here.st_uid == there.st_uid);
        report.check("the owning gid agrees", here.st_gid == there.st_gid);
        report.check("the size agrees and is zero before placement",
                     here.st_size == there.st_size && here.st_size == 0);
    }

    first.unlink(name);
}

void checkOnlyTheOwnerMayRemove(fsuser::tests::Report& report, fsuser::tests::Mount& first,
                                fsuser::tests::Mount& second, const std::string& mountA,
                                const std::string& stem)
{
    report.section("who may remove a name");

    const std::string ownedByA = stem + "_owned_a";
    auto file = first.open(ownedByA, O_CREAT | O_RDWR, 0644);

    report.checkRefused("the other mount cannot remove it",
                        [&]
                        {
                            second.unlink(ownedByA);
                        });
    report.check("and it is still there", nameExists(mountA, ownedByA));

    report.checkAccepted("the mount that linked it can remove it",
                         [&]
                         {
                             first.unlink(ownedByA);
                         });
    report.check("and now it is not", !nameExists(mountA, ownedByA));
}

void checkABatchCrosses(fsuser::tests::Report& report, fsuser::tests::Mount& first,
                        const std::string& mountA, const std::string& mountB,
                        const std::string& stem)
{
    report.section("a batch, counted from the other side");

    const std::string prefix = stem + "_batch_";
    std::vector<fsuser::File> held;
    held.reserve(static_cast<std::size_t>(Batch));
    for (std::int32_t index = 0; index < Batch; ++index)
    {
        held.push_back(first.open(prefix + std::to_string(index), O_CREAT | O_RDWR, 0644));
    }
    report.check("the second mount lists every one of them",
                 listNames(mountB, prefix).size() == static_cast<std::size_t>(Batch));
    report.check("and lists exactly what the first mount lists",
                 listNames(mountA, prefix) == listNames(mountB, prefix));

    for (std::int32_t index = 0; index < Batch; ++index)
    {
        first.unlink(prefix + std::to_string(index));
    }
    report.check("the second mount lists none of them afterwards",
                 listNames(mountB, prefix).empty());
}

}  // namespace

int main(int argc, char** argv)
{
    const auto body = [](fsuser::tests::Report& report, fsuser::tests::Mounts_t& mounts)
    {
        const auto stem = caseName("names_peer");

        auto first = fsuser::tests::Mount(mounts.first());
        auto second = fsuser::tests::Mount(mounts.second());

        checkANameCrossesBothWays(report, first, second, mounts, stem);
        checkStatAgrees(report, first, mounts.first(), mounts.second(), stem);
        checkOnlyTheOwnerMayRemove(report, first, second, mounts.first(), stem);
        checkABatchCrosses(report, first, mounts.first(), mounts.second(), stem);
    };
    return fsuser::tests::runCase(argc, argv, "test_names_peer", 2, body);
}
