// SPDX-License-Identifier: Apache-2.0
//
// test_rat_exhaustion -- the region runs out of RAT slots, says so, and takes them back.
//
// A name costs one region allocation table entry, and the table has a fixed number of them. What
// matters is not the number, which the format decides, but that the refusal at the end is ENOSPC and
// that removing the names makes the slots available again.
//
// This case takes every free slot in the region, so nothing else can create a name while it runs.
// Every descriptor is held until the removal loop, because a slot whose owner has gone is one the GC
// may take back mid-run and the refusal being measured would stop arriving.
//
//   test_rat_exhaustion <mount-a>

#include <fcntl.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <system_error>
#include <vector>

#include "fs/errors.hpp"
#include "fs/file.hpp"
#include "harness/harness.hpp"
#include "harness/listing.hpp"
#include "harness/mount.hpp"

namespace
{

// An upper bound on the loop and not a claim about the table: the format sets the real number, and
// this only has to be past it so the loop ends on the region's refusal rather than on its own count.
constexpr std::int32_t MoreThanTheTable = 4096;

using fsuser::tests::caseName;
using fsuser::tests::listNames;

void checkTheTableFillsAndEmpties(fsuser::tests::Report& report, fsuser::tests::Mount& mount,
                                  const std::string& stem)
{
    const std::string prefix = stem + "_";
    std::vector<fsuser::File> held;
    std::string lastRefusal;
    std::error_code refusedWith;

    report.section("taking every free slot");

    for (std::int32_t index = 0; index < MoreThanTheTable; ++index)
    {
        try
        {
            held.push_back(mount.open(prefix + std::to_string(index), O_CREAT | O_RDWR, 0644));
        }
        catch (const fsuser::FsCodedError& refusal)
        {
            refusedWith = refusal.code();
            lastRefusal = refusal.what();
            break;
        }
        catch (const std::exception& failure)
        {
            lastRefusal = failure.what();
            break;
        }
    }

    const auto taken = static_cast<std::int32_t>(held.size());
    report.check("the loop ended on the region and not on its own bound: " +
                     std::to_string(taken) + " taken",
                 taken > 0 && taken < MoreThanTheTable);
    report.checkErrno("the slot past the last one answers ENOSPC (" + lastRefusal + ")",
                      refusedWith == std::errc::no_space_on_device, refusedWith.value());
    report.check("every name taken is listed",
                 listNames(mount.getPoint(), prefix).size() == static_cast<std::size_t>(taken));

    report.section("giving them back");

    held.clear();
    std::int32_t removed = 0;
    for (std::int32_t index = 0; index < taken; ++index)
    {
        try
        {
            mount.unlink(prefix + std::to_string(index));
            ++removed;
        }
        catch (const std::exception& failure)
        {
            report.raised("removing " + prefix + std::to_string(index), failure);
            break;
        }
    }
    report.check("every name is removed: " + std::to_string(removed) + " of " +
                     std::to_string(taken),
                 removed == taken);
    report.check("none of them is listed any more", listNames(mount.getPoint(), prefix).empty());

    report.checkAccepted("a name can be taken again", [&]
                         {
                             auto file = mount.open(prefix + "again", O_CREAT | O_RDWR, 0644);
                             mount.unlink(prefix + "again");
                         });
}

}  // namespace

int main(int argc, char** argv)
{
    const auto body = [](fsuser::tests::Report& report, fsuser::tests::Mounts_t& mounts)
    {
        const auto stem = caseName("rat_exhaust");

        auto mount = fsuser::tests::Mount(mounts.first());
        checkTheTableFillsAndEmpties(report, mount, stem);
    };
    return fsuser::tests::runCase(argc, argv, "test_rat_exhaustion", 1, body);
}
