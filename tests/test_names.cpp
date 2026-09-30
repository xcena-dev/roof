// SPDX-License-Identifier: Apache-2.0
//
// test_names -- creating, finding, removing and reusing a name on one mount.
//
// A name is an index bucket and a RAT slot, and neither is visible from the directory. What is
// visible is whether a lookup resolves, so the checks below are lookups around each call that should
// have changed one.
//
// The names live on a mount that stays open for the whole case. A name's owner is the process that
// created it and the GC reclaims a name whose owner is gone, so a short-lived process per name would
// have the sweep removing entries underneath the checks.
//
//   test_names <mount-a>

#include <fcntl.h>
#include <sys/stat.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <system_error>
#include <vector>

#include "fs/file.hpp"
#include "harness/harness.hpp"
#include "harness/listing.hpp"
#include "harness/mount.hpp"

namespace
{

// The index splits an entry at 63 characters, so the boundary is where a name stops fitting.
constexpr std::uint64_t LongestName = 63;

// How many names the counting checks make. Enough that a listing bug shows and few enough to leave
// the RAT to the case that fills it on purpose.
constexpr std::int32_t Batch = 10;

constexpr std::int32_t Cycles = 10;

using fsuser::tests::caseName;
using fsuser::tests::listNames;
using fsuser::tests::nameExists;

void checkOneNameComesAndGoes(fsuser::tests::Report& report, fsuser::tests::Mount& mount,
                              const std::string& stem)
{
    report.section("one name");

    const std::string name = stem + "_one";
    report.check("the name is not there to begin with", !nameExists(mount.getPoint(), name));

    {
        auto file = mount.open(name, O_CREAT | O_RDWR, 0644);
        report.check("creating it opens a descriptor", file.isOpen());
        report.check("the lookup now resolves", nameExists(mount.getPoint(), name));

        struct ::stat fresh = {};
        report.check("it arrives as a regular file with no size",
                     ::stat(mount.pathTo(name).c_str(), &fresh) == 0 &&
                         S_ISREG(fresh.st_mode) && fresh.st_size == 0);

        report.checkAccepted("opening it again without O_CREAT succeeds",
                             [&]
                             {
                                 auto again = mount.open(name, O_RDONLY);
                             });
        report.checkAccepted("opening it again with O_CREAT adds nothing",
                             [&]
                             {
                                 auto again = mount.open(name, O_CREAT | O_RDWR, 0644);
                             });
        report.check("only one entry carries the name",
                     listNames(mount.getPoint(), name).size() == 1);
    }

    report.checkAccepted("removing it succeeds", [&]
                         {
                             mount.unlink(name);
                         });
    report.check("the lookup stops resolving", !nameExists(mount.getPoint(), name));
    report.checkRefusedWith("opening it without O_CREAT answers ENOENT",
                            std::errc::no_such_file_or_directory,
                            [&]
                            {
                                auto gone = mount.open(name, O_RDONLY);
                            });
}

void checkABatchIsListed(fsuser::tests::Report& report, fsuser::tests::Mount& mount,
                         const std::string& stem)
{
    report.section("a batch of names");

    const std::string prefix = stem + "_batch_";
    std::vector<fsuser::File> held;
    held.reserve(static_cast<std::size_t>(Batch));
    for (std::int32_t index = 0; index < Batch; ++index)
    {
        held.push_back(mount.open(prefix + std::to_string(index), O_CREAT | O_RDWR, 0644));
    }
    report.check("the directory lists every one of them",
                 listNames(mount.getPoint(), prefix).size() == static_cast<std::size_t>(Batch));

    mount.unlink(prefix + "0");
    report.check("removing one leaves the rest",
                 listNames(mount.getPoint(), prefix).size() == static_cast<std::size_t>(Batch) - 1);

    for (std::int32_t index = 1; index < Batch; ++index)
    {
        mount.unlink(prefix + std::to_string(index));
    }
    report.check("removing the rest empties the prefix", listNames(mount.getPoint(), prefix).empty());
}

void checkTheNameLengthBoundary(fsuser::tests::Report& report, fsuser::tests::Mount& mount)
{
    report.section("how long a name may be");

    const std::string longest(LongestName, 'a');
    const std::string overlong(LongestName + 1, 'a');

    report.checkAccepted("a name of the longest length is taken",
                         [&]
                         {
                             auto file = mount.open(longest, O_CREAT | O_RDWR, 0644);
                         });
    report.check("and it resolves", nameExists(mount.getPoint(), longest));
    report.checkAccepted("removing it succeeds", [&]
                         {
                             mount.unlink(longest);
                         });

    report.checkRefusedWith("one character more is refused", std::errc::filename_too_long,
                            [&]
                            {
                                auto file = mount.open(overlong, O_CREAT | O_RDWR, 0644);
                            });
    report.check("and nothing by that name was left behind", !nameExists(mount.getPoint(), overlong));

    // Two names that agree on every character but the last, which is where an index keyed on a
    // truncated name would put them in one bucket.
    const std::string sharedStem(LongestName - 1, 'b');
    auto first = mount.open(sharedStem + "A", O_CREAT | O_RDWR, 0644);
    auto second = mount.open(sharedStem + "B", O_CREAT | O_RDWR, 0644);
    report.check("two names differing only in the last character are two entries",
                 listNames(mount.getPoint(), sharedStem).size() == 2);
    mount.unlink(sharedStem + "A");
    mount.unlink(sharedStem + "B");
}

void checkANameIsReusable(fsuser::tests::Report& report, fsuser::tests::Mount& mount,
                          const std::string& stem)
{
    report.section("a name taken and given back again");

    const std::string name = stem + "_cycle";
    std::int32_t completed = 0;
    for (std::int32_t round = 0; round < Cycles; ++round)
    {
        try
        {
            auto file = mount.open(name, O_CREAT | O_RDWR, 0644);
            mount.unlink(name);
            ++completed;
        }
        catch (const std::exception&)
        {
            break;
        }
    }
    report.check("every round took the name and gave it back: " + std::to_string(completed) +
                     " of " + std::to_string(Cycles),
                 completed == Cycles);
    report.check("the name is free at the end", !nameExists(mount.getPoint(), name));
}

}  // namespace

int main(int argc, char** argv)
{
    const auto body = [](fsuser::tests::Report& report, fsuser::tests::Mounts_t& mounts)
    {
        const auto stem = caseName("names");

        auto mount = fsuser::tests::Mount(mounts.first());
        checkOneNameComesAndGoes(report, mount, stem);
        checkABatchIsListed(report, mount, stem);
        checkTheNameLengthBoundary(report, mount);
        checkANameIsReusable(report, mount, stem);
    };
    return fsuser::tests::runCase(argc, argv, "test_names", 1, body);
}
