// SPDX-License-Identifier: Apache-2.0
//
// test_unlink_live_refs -- what unlink does to a descriptor and to mappings that are still standing.
//
// POSIX takes the name and leaves the bytes to the last reference. Every node counts its own open
// references on an entry and shows one bit per entry on the medium, so unlink takes the name at once
// and frees the extent only when no node's bit is set. Until then the holders keep reading their own
// bytes, no placement lands on the extent, and it is freed once the last one goes.
//
// The peer half needs a second mount: the node that unlinks cannot see a mapping the other holds.
//
//   test_unlink_live_refs <mount-a> <mount-b>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

#include "fs/file.hpp"
#include "harness/harness.hpp"
#include "harness/listing.hpp"
#include "harness/mount.hpp"
#include "harness/pattern.hpp"
#include "harness/slot.hpp"
#include "harness/sysfs.hpp"

namespace
{

using fsuser::tests::PlacementSize;
constexpr std::uint64_t PlacedWords = PlacementSize / sizeof(std::uint32_t);

// One seed per file, so a stale mapping reads as the other file's pattern and not as a plausible value.
constexpr std::uint32_t FirstSeed = 0xC3C3C3C3U;
constexpr std::uint32_t SecondSeed = 0x3C3C3C3CU;

// How many words read() compares. The mapping check already covered the whole span.
constexpr std::uint64_t ReadWords = 4;

using fsuser::tests::caseName;
using fsuser::tests::findRowByName;
using fsuser::tests::nameExists;
using fsuser::tests::RegionRow_t;
using fsuser::tests::slotIsTaken;
using fsuser::tests::waitForSlotFree;

// A file placed and filled with FirstSeed, and its region_info row.
[[nodiscard]] fsuser::File placeFirst(fsuser::tests::Report& report, fsuser::tests::Mount& mount,
                                      const std::string& name, std::optional<RegionRow_t>& row)
{
    auto file = fsuser::tests::placeFile(mount, name);
    auto stored = file.map(PlacementSize, PROT_READ | PROT_WRITE);
    report.check("the file maps writably", stored.isMapped());
    fsuser::tests::fillPattern(static_cast<std::uint32_t*>(stored.get()), PlacedWords, FirstSeed);
    row = findRowByName(name);
    report.check("region_info lists the file", row.has_value());
    return file;
}

// A second file placed after the unlink, which is the moment a freed slot could be handed out again.
void checkSecondFileLandsElsewhere(fsuser::tests::Report& report, fsuser::tests::Mount& mount,
                                   const std::string& secondName, const std::optional<RegionRow_t>& first)
{
    auto second = fsuser::tests::placeFile(mount, secondName);
    {
        auto stored = second.map(PlacementSize, PROT_READ | PROT_WRITE);
        report.check("a second file places after the unlink", stored.isMapped());
        fsuser::tests::fillPattern(static_cast<std::uint32_t*>(stored.get()), PlacedWords, SecondSeed);
    }
    const auto row = findRowByName(secondName);
    report.check("the second file lands on another extent",
                 first && row && row->offset != first->offset && row->entry != first->entry);
    mount.unlink(secondName);
}

void checkDescriptorOutlivesName(fsuser::tests::Report& report, fsuser::tests::Mount& mount,
                                 const std::string& name)
{
    report.section("an open descriptor while the name goes");

    std::optional<RegionRow_t> row;
    {
        auto file = placeFirst(report, mount, name, row);

        report.checkAccepted("unlink goes through while a descriptor is open",
                             [&]
                             {
                                 mount.unlink(name);
                             });
        report.check("the name is gone", !nameExists(mount.getPoint(), name));
        report.check("the slot stays taken while the descriptor is open", row && slotIsTaken(row->entry));

        std::uint32_t taken[ReadWords] = {};
        const bool sought = ::lseek(file.get(), 0, SEEK_SET) == 0;
        const std::int64_t got = sought ? ::read(file.get(), taken, sizeof(taken)) : -1;
        report.checkErrno("read() on the descriptor still answers",
                          got == static_cast<std::int64_t>(sizeof(taken)), errno);
        if (got == static_cast<std::int64_t>(sizeof(taken)))
        {
            fsuser::tests::checkPattern(report, "and returns the file's own bytes", taken, ReadWords,
                                        FirstSeed);
        }
    }

    report.check("the slot is free once the descriptor closed",
                 row && waitForSlotFree(row->entry));
}

void checkMappingOutlivesName(fsuser::tests::Report& report, fsuser::tests::Mount& mount,
                              const std::string& name, const std::string& secondName)
{
    report.section("a standing mapping while the name goes");

    std::optional<RegionRow_t> row;
    {
        // The descriptor closes here and the mapping stays, which is the reference unlink has to see.
        auto standing = [&]
        {
            auto file = placeFirst(report, mount, name, row);
            return file.map(PlacementSize, PROT_READ);
        }();
        report.check("the mapping outlives its descriptor", standing.isMapped());
        const auto* words = static_cast<const std::uint32_t*>(standing.get());

        report.checkAccepted("unlink goes through while a mapping stands",
                             [&]
                             {
                                 mount.unlink(name);
                             });
        report.check("the name is gone", !nameExists(mount.getPoint(), name));
        report.check("the slot stays taken while the mapping stands", row && slotIsTaken(row->entry));
        fsuser::tests::checkPattern(report, "the mapping still reads its own bytes", words, PlacedWords,
                                    FirstSeed);

        checkSecondFileLandsElsewhere(report, mount, secondName, row);
        fsuser::tests::checkPattern(report, "and the mapping still reads its own bytes after that", words,
                                    PlacedWords, FirstSeed);
    }

    report.check("the slot is free once the mapping went", row && waitForSlotFree(row->entry));
}

void checkPeerMappingOutlivesName(fsuser::tests::Report& report, fsuser::tests::Mount& owner,
                                  fsuser::tests::Mount& peer, const std::string& name,
                                  const std::string& secondName)
{
    report.section("a peer's mapping while the owner unlinks");

    std::optional<RegionRow_t> row;
    {
        {
            auto file = placeFirst(report, owner, name, row);
        }

        // No grant: the peer's first map is refused on the record and that node's helper decides.
        auto standing = [&]
        {
            auto seen = peer.open(name, O_RDWR);
            return seen.map(PlacementSize, PROT_READ);
        }();
        report.check("the peer maps for reading", standing.isMapped());
        const auto* words = static_cast<const std::uint32_t*>(standing.get());
        fsuser::tests::checkPattern(report, "the peer's mapping holds the owner's pattern", words,
                                    PlacedWords, FirstSeed);

        report.checkAccepted("the owner's unlink goes through while the peer's mapping stands",
                             [&]
                             {
                                 owner.unlink(name);
                             });
        report.check("the name is gone from the peer's mount too", !nameExists(peer.getPoint(), name));
        report.check("the slot stays taken while the peer's mapping stands", row && slotIsTaken(row->entry));
        fsuser::tests::checkPattern(report, "the peer's mapping still reads its own bytes", words,
                                    PlacedWords, FirstSeed);

        checkSecondFileLandsElsewhere(report, owner, secondName, row);
        fsuser::tests::checkPattern(report, "and still reads its own bytes after that", words, PlacedWords,
                                    FirstSeed);
    }

    report.check("the slot is free once the peer's mapping went",
                 row && waitForSlotFree(row->entry));
}

}  // namespace

int main(int argc, char** argv)
{
    const auto body = [](fsuser::tests::Report& report, fsuser::tests::Mounts_t& mounts)
    {
        auto owner = fsuser::tests::Mount(mounts.first());
        auto peer = fsuser::tests::Mount(mounts.second());

        checkDescriptorOutlivesName(report, owner, caseName("unlink_fd"));
        checkMappingOutlivesName(report, owner, caseName("unlink_map"), caseName("unlink_map2"));
        checkPeerMappingOutlivesName(report, owner, peer, caseName("unlink_peer"),
                                     caseName("unlink_peer2"));
    };
    return fsuser::tests::runCase(argc, argv, "test_unlink_live_refs", 2, body);
}
