// SPDX-License-Identifier: Apache-2.0
//
// test_deleg_scan -- a delegation scan reads the rows a region holds and skips the slots it does not.
//
// perm_info shows which rows the region marks taken, and node<N>/test/deleg_reads counts the rows
// each kind of scan read. Revoking rows in the middle leaves holes that a scan bounded by the
// highest row would still read.
//
//   test_deleg_scan <mount-a>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <exception>
#include <fstream>
#include <optional>
#include <string>

#include "fs/errors.hpp"
#include "fs/file.hpp"
#include "harness/harness.hpp"
#include "harness/mount.hpp"
#include "harness/sysfs.hpp"

namespace
{

using fsuser::tests::caseName;
using fsuser::tests::DelegReads_t;
using fsuser::tests::readDelegReads;

// Well clear of any account this host has, so a row here names nobody who could then use it.
constexpr std::uint32_t FirstUid = 62000;

// The owner writes these rows into a fresh table, then revokes two of them to leave holes below
// the highest.
constexpr std::uint32_t GrantedRows = 6;
constexpr std::uint32_t FirstHole = 1;
constexpr std::uint32_t SecondHole = 3;

constexpr std::uint32_t AllGranted = (1U << GrantedRows) - 1;
constexpr std::uint32_t WithHoles = AllGranted & ~(1U << FirstHole) & ~(1U << SecondHole);
constexpr std::uint32_t HoleRefilled = WithHoles | (1U << FirstHole);
constexpr std::uint32_t RowsWithHoles = GrantedRows - 2;

// A refused grant's errno fits an exit status, and this one stands for every other way the child
// could have ended.
constexpr std::int32_t ChildBroke = 255;

constexpr const char* PermInfoHeader = "RAT_Entry\tDefault\tDeleg_taken";

// The Deleg_taken column of perm_info's row for @ratEntry. nullopt when the file, its header or the
// row is not what this case reads.
[[nodiscard]] std::optional<std::uint32_t> readTakenRows(std::int64_t ratEntry)
{
    std::ifstream source{fsuser::tests::PermInfoPath};
    std::string line;
    if (!source || !std::getline(source, line) || line != PermInfoHeader)
    {
        return std::nullopt;
    }
    while (std::getline(source, line))
    {
        const auto fields = fsuser::tests::splitOnTabs(line);
        if (fields.size() < 3 || fields[0] != std::to_string(ratEntry))
        {
            continue;
        }
        try
        {
            return static_cast<std::uint32_t>(std::stoul(fields[2], nullptr, 0));
        }
        catch (const std::exception&)
        {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::int64_t findRatEntry(const std::string& name)
{
    for (const auto& row : fsuser::tests::readRegionRows())
    {
        if (row.name == name)
        {
            return row.entry;
        }
    }
    return -1;
}

// What each scan read across @work, as the difference of two readings.
template <typename T_Work>
[[nodiscard]] DelegReads_t countScanReads(std::uint32_t nodeId, T_Work&& work)
{
    const auto before = readDelegReads(nodeId).value_or(DelegReads_t{});
    work();
    const auto after = readDelegReads(nodeId).value_or(DelegReads_t{});
    return {after.check - before.check, after.role - before.role, after.slot - before.slot,
            after.revoke - before.revoke, after.sweep - before.sweep};
}

// A forked child holds the owner's descriptor but not its standing, so its grant is refused by
// the permission scan. The exit status is the errno of that refusal, and 0 is a grant that landed.
[[nodiscard]] std::int32_t grantAsChild(fsuser::File& file)
{
    const ::pid_t child = ::fork();
    if (child < 0)
    {
        return ChildBroke;
    }
    if (child == 0)
    {
        try
        {
            file.grantPermission(FirstUid + GrantedRows, fsuser::AnyId, fsuser::Permission::Read);
            ::_exit(0);
        }
        catch (const fsuser::FsCodedError& refusal)
        {
            ::_exit(refusal.code().value() & ChildBroke);
        }
        catch (const std::exception&)
        {
            ::_exit(ChildBroke);
        }
    }

    std::int32_t status = 0;
    if (::waitpid(child, &status, 0) != child || WIFEXITED(status) == 0)
    {
        return ChildBroke;
    }
    return WEXITSTATUS(status);
}

// A fresh table takes each grant in the lowest free slot, and the slot search reads only the rows
// written before it.
void checkGrantsReadOnlyTakenRows(fsuser::tests::Report& report, fsuser::File& file, std::uint32_t nodeId,
                                  std::int64_t ratEntry)
{
    report.section("a grant reads only the rows already taken");

    report.check("a fresh region marks no row", readTakenRows(ratEntry) == 0U);

    bool everyGrantReadItsPredecessors = true;
    for (std::uint32_t row = 0; row < GrantedRows; ++row)
    {
        const auto reads = countScanReads(nodeId, [&]
                                          {
                                              report.checkAccepted("grant to uid " + std::to_string(FirstUid + row),
                                                                   [&]
                                                                   {
                                                                       file.grantPermission(FirstUid + row, fsuser::AnyId,
                                                                                            fsuser::Permission::Read);
                                                                   });
                                          });
        if (reads.slot != row)
        {
            everyGrantReadItsPredecessors = false;
            report.note("grant " + std::to_string(row) + " read " + std::to_string(reads.slot) + " rows");
        }
    }

    report.check("each grant's slot search read the rows before it and no more", everyGrantReadItsPredecessors);
    report.check("the rows are taken in order from slot 0", readTakenRows(ratEntry) == AllGranted);
}

// The holes stay below the highest row, which a bound would still cover and the marks do not.
void checkRevokesLeaveHolesUnread(fsuser::tests::Report& report, fsuser::File& file, std::uint32_t nodeId,
                                  std::int64_t ratEntry)
{
    report.section("a revoke reads only the taken rows and unmarks the one it empties");

    const auto first = countScanReads(nodeId, [&]
                                      {
                                          file.revokePermission(FirstUid + FirstHole, fsuser::AnyId);
                                      });
    report.check("the first revoke read every taken row", first.revoke == GrantedRows);

    const auto second = countScanReads(nodeId, [&]
                                       {
                                           file.revokePermission(FirstUid + SecondHole, fsuser::AnyId);
                                       });
    report.check("the second revoke read the rows the first left", second.revoke == GrantedRows - 1);
    report.note("revoke reads: " + std::to_string(first.revoke) + ", " + std::to_string(second.revoke));

    report.check("the two emptied rows are unmarked", readTakenRows(ratEntry) == WithHoles);

    const auto missed = countScanReads(nodeId, [&]
                                       {
                                           report.checkRefused("revoking an account with no row is refused", [&]
                                                               {
                                                                   file.revokePermission(FirstUid + SecondHole,
                                                                                         fsuser::AnyId);
                                                               });
                                       });
    report.check("a revoke that finds nothing still reads only the taken rows", missed.revoke == RowsWithHoles);
}

// The child holds no row and the region grants nothing by default, so the scan reads every taken
// row looking for one and stops at none of the holes.
void checkPermissionScanSkipsHoles(fsuser::tests::Report& report, fsuser::File& file, std::uint32_t nodeId)
{
    report.section("a permission check reads only the taken rows");

    std::int32_t refusal = 0;
    const auto reads = countScanReads(nodeId, [&]
                                      {
                                          refusal = grantAsChild(file);
                                      });
    report.checkErrno("a child with no standing is refused its grant", refusal == EACCES, refusal);
    report.check("its permission scan read the taken rows and none of the holes", reads.check == RowsWithHoles);
    report.note("check reads: " + std::to_string(reads.check));
}

void checkGrantRefillsLowestHole(fsuser::tests::Report& report, fsuser::File& file, std::uint32_t nodeId,
                                 std::int64_t ratEntry)
{
    report.section("the next grant fills the lowest hole");

    const auto reads = countScanReads(nodeId, [&]
                                      {
                                          report.checkAccepted("grant to a new uid", [&]
                                                               {
                                                                   file.grantPermission(FirstUid + GrantedRows + 1,
                                                                                        fsuser::AnyId,
                                                                                        fsuser::Permission::Read);
                                                               });
                                      });
    report.check("its slot search read the taken rows and none of the holes", reads.slot == RowsWithHoles);
    report.check("the row landed in the first hole", readTakenRows(ratEntry) == HoleRefilled);
}

}  // namespace

int main(int argc, char** argv)
{
    fsuser::tests::Mounts_t mounts;
    if (!fsuser::tests::takeMounts(argc, argv, 1, mounts))
    {
        return fsuser::tests::UsageStatus;
    }

    fsuser::tests::Report report;
    auto mount = fsuser::tests::Mount(mounts.first());
    const auto nodeId = mount.getNodeId();
    if (!readDelegReads(nodeId))
    {
        report.note("no test/deleg_reads under the mount, so no scan is countable");
        return fsuser::tests::SkipStatus;
    }

    const auto name = caseName("deleg_scan");
    try
    {
        auto file = fsuser::tests::placeFile(mount, name);
        const auto ratEntry = findRatEntry(name);
        report.check("region_info names the file's RAT entry", ratEntry >= 0);
        report.check("perm_info carries the entry's taken rows", readTakenRows(ratEntry).has_value());

        checkGrantsReadOnlyTakenRows(report, file, nodeId, ratEntry);
        checkRevokesLeaveHolesUnread(report, file, nodeId, ratEntry);
        checkPermissionScanSkipsHoles(report, file, nodeId);
        checkGrantRefillsLowestHole(report, file, nodeId, ratEntry);
    }
    catch (const std::exception& failure)
    {
        report.raised("test_deleg_scan setup", failure);
    }

    mount.unlink(name);
    return report.summarise("test_deleg_scan");
}
