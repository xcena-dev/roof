// SPDX-License-Identifier: Apache-2.0
//
// test_paged_extents -- where paged and contiguous placements land, read back off extents/<slot>.
//
// Each section fills the write-back pool around the gaps it wants free. A paged region takes one
// whole gap when one holds it, and pieces of several only when none does.
//
//   test_paged_extents <mount-a>

#include <fcntl.h>
#include <sys/mman.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "fs/file.hpp"
#include "harness/harness.hpp"
#include "harness/mount.hpp"
#include "harness/pool.hpp"
#include "harness/sysfs.hpp"

namespace
{

using fsuser::tests::caseName;
using fsuser::tests::Mount;
using fsuser::tests::PagedSeed;
using fsuser::tests::Piece_t;
using fsuser::tests::Report;

// The sizes the run sections place in, large enough that a granule up to 1 GiB divides them.
constexpr std::uint64_t UnitBytes = 1ULL << 30;

// One free run and nothing else free in the write-back pool, and the fillers around it.
struct Gap_t
{
    std::uint64_t start{0};
    std::vector<std::string> fillers;
};

[[nodiscard]] std::vector<Piece_t> sortByOffset(std::vector<Piece_t> pieces)
{
    std::sort(pieces.begin(), pieces.end(),
              [](const Piece_t& left, const Piece_t& right)
              {
                  return left.offset < right.offset;
              });
    return pieces;
}

[[nodiscard]] std::optional<std::uint64_t> readFirstOffset(const std::string& name)
{
    const auto pieces = fsuser::tests::readPiecesOf(name);
    if (pieces.empty())
    {
        return std::nullopt;
    }
    return pieces.front().offset;
}

void placePaged(const Mount& mount, const std::string& name, std::uint64_t bytes)
{
    auto file = mount.open(name, O_CREAT | O_EXCL | O_RDWR, 0644);
    file.setRegionAttr(fsuser::RegionAttr_t{fsuser::CachePolicy::Writeback, fsuser::Placement::Paged});
    file.resize(bytes);
}

void checkContiguousRefused(Report& report, const Mount& mount, const std::string& name, std::uint64_t bytes)
{
    report.checkRefusedWith("a contiguous region of that size answers ENOSPC", std::errc::no_space_on_device,
                            [&]
                            {
                                mount.open(name, O_CREAT | O_RDWR, 0644).resize(bytes);
                            });
    mount.unlink(name);
}

void unlinkAll(const Mount& mount, const std::vector<std::string>& names)
{
    for (const auto& name : names)
    {
        mount.unlink(name);
    }
}

// A contiguous region of @bytes placed first marks where the run will be, then the rest of the pool
// is filled and the marker unlinked.
[[nodiscard]] std::optional<Gap_t> carveGap(const Mount& mount, const std::string& stem, std::uint64_t bytes)
{
    const auto marker = stem + "carve";
    if (!fsuser::tests::placeContiguous(mount, marker, bytes))
    {
        return std::nullopt;
    }
    const auto start = readFirstOffset(marker);
    Gap_t gap;
    gap.fillers = fsuser::tests::fillWritebackPool(mount, stem + "fill");
    mount.unlink(marker);
    if (!start || !fsuser::tests::waitForWritebackFree(bytes))
    {
        unlinkAll(mount, gap.fillers);
        return std::nullopt;
    }
    gap.start = *start;
    return gap;
}

// Writes through one mapping, then reads back across each granule boundary through read() and
// through a second mapping.
void checkBytes(Report& report, const Mount& mount, const std::string& name, std::uint64_t bytes,
                std::uint64_t granule)
{
    auto file = mount.open(name, O_RDWR);
    {
        auto mapping = file.map(bytes, PROT_READ | PROT_WRITE);
        fsuser::tests::writePattern(mapping.get(), bytes, PagedSeed);
    }
    report.check("read() returns the right bytes across every granule boundary",
                 fsuser::tests::readsAcrossGranules(file, bytes, granule));
    auto again = mount.open(name, O_RDONLY);
    auto mapping = again.map(bytes, PROT_READ);
    report.check("a second mapping reads what the first wrote",
                 fsuser::tests::hasPattern(mapping.get(), bytes, PagedSeed));
}

void checkHoles(Report& report, const Mount& mount)
{
    // Three holes is one short of the most pieces a region holds, so the placement never runs out.
    constexpr std::uint32_t HoleCount = 3;

    report.section("a paged region fills holes that no one of them holds alone");
    const auto fragments = fsuser::tests::fragmentWritebackPool(mount, caseName("paged_h_"), HoleCount);
    if (!fragments)
    {
        report.check("the write-back pool could be cut into holes", false);
        return;
    }
    fsuser::tests::writeKeepers(mount, *fragments);
    checkContiguousRefused(report, mount, caseName("paged_h_contig"), fragments->holeBytes);

    const auto pagedName = caseName("paged_h_region");
    placePaged(mount, pagedName, fragments->holeBytes);
    const auto landed = mount.open(pagedName, O_RDONLY).readRegionAttr();
    report.check("the region reads back as paged", landed.placement == fsuser::Placement::Paged);
    report.check("it lies in one piece per run of holes", landed.pieces == fragments->holes.size());
    report.check("its pieces are exactly the holes",
                 sortByOffset(fsuser::tests::readPiecesOf(pagedName)) == fragments->holes);
    const auto pools = fsuser::tests::readPoolInfo();
    report.check("the write-back pool has nothing left", pools && pools->wbFree == 0);

    checkBytes(report, mount, pagedName, fragments->holeBytes, fragments->granule);
    {
        auto file = mount.open(pagedName, O_RDONLY);
        report.check("one read() of the whole region returns its bytes",
                     fsuser::tests::readsWhole(file, fragments->holeBytes));
    }
    report.check("the neighbours kept their bytes", fsuser::tests::keepersHold(mount, *fragments));

    mount.unlink(pagedName);
    fsuser::tests::releaseFragments(mount, *fragments);
}

// A and B side by side at the front of a five-unit run, then A freed. A paged region the tail holds
// takes it whole from the high end, and only one that fits nowhere whole takes A's place as well.
void checkWholeBeforePieces(Report& report, const Mount& mount)
{
    constexpr std::uint64_t RunUnits = 5;

    report.section("a paged region takes a whole gap before it takes pieces");
    const auto stem = caseName("paged_w_");
    const auto gap = carveGap(mount, stem, RunUnits * UnitBytes);
    if (!gap)
    {
        report.check("the pool could be left with one free run of five units", false);
        return;
    }
    const std::uint64_t start = gap->start;
    const auto first = stem + "a";
    const auto second = stem + "b";
    report.check("A and B land side by side at the front of the run",
                 fsuser::tests::placeContiguous(mount, first, UnitBytes) &&
                     fsuser::tests::placeContiguous(mount, second, UnitBytes) && readFirstOffset(first) == start &&
                     readFirstOffset(second) == start + UnitBytes);
    mount.unlink(first);

    const auto twoUnits = stem + "paged2";
    placePaged(mount, twoUnits, 2 * UnitBytes);
    report.check("a paged region of two units takes one piece at the high end of the tail",
                 fsuser::tests::readPiecesOf(twoUnits) == std::vector<Piece_t>{{start + 3 * UnitBytes, 2 * UnitBytes}});
    const auto oneUnit = stem + "contig1";
    report.check("a contiguous region of one unit then takes A's place",
                 fsuser::tests::placeContiguous(mount, oneUnit, UnitBytes) && readFirstOffset(oneUnit) == start);
    mount.unlink(twoUnits);
    mount.unlink(oneUnit);

    report.section("only a paged region that fits no gap whole takes A's place and the tail");
    checkContiguousRefused(report, mount, stem + "contig4", 4 * UnitBytes);
    const auto fourUnits = stem + "paged4";
    placePaged(mount, fourUnits, 4 * UnitBytes);
    const std::vector<Piece_t> expected{{start, UnitBytes}, {start + 2 * UnitBytes, 3 * UnitBytes}};
    report.check("a paged region of four units takes A's place and the whole tail",
                 sortByOffset(fsuser::tests::readPiecesOf(fourUnits)) == expected);

    mount.unlink(fourUnits);
    mount.unlink(second);
    unlinkAll(mount, gap->fillers);
}

// One unit taken in the middle of a five-unit run. A paged region bigger than either side lies in
// pieces of both, and a contiguous one of the same size answers ENOSPC.
void checkBlockInMiddle(Report& report, const Mount& mount)
{
    constexpr std::uint64_t RunUnits = 5;

    report.section("a block in the middle of a run splits a paged region and refuses a contiguous one");
    const auto stem = caseName("paged_m_");
    const auto gap = carveGap(mount, stem, RunUnits * UnitBytes);
    if (!gap)
    {
        report.check("the pool could be left with one free run of five units", false);
        return;
    }
    const std::uint64_t start = gap->start;
    const auto front = stem + "front";
    const auto block = stem + "block";
    report.check("the block lands in the middle of the run",
                 fsuser::tests::placeContiguous(mount, front, 2 * UnitBytes) &&
                     fsuser::tests::placeContiguous(mount, block, UnitBytes) &&
                     readFirstOffset(block) == start + 2 * UnitBytes);
    mount.unlink(front);

    checkContiguousRefused(report, mount, stem + "contig3", 3 * UnitBytes);
    const auto paged = stem + "paged3";
    placePaged(mount, paged, 3 * UnitBytes);
    const auto pieces = fsuser::tests::readPiecesOf(paged);
    report.check("the paged region lies in two pieces", pieces.size() == 2);
    const auto isBesideBlock = [start](const Piece_t& piece)
    {
        const std::uint64_t end = piece.offset + piece.length;
        return (piece.offset >= start && end <= start + 2 * UnitBytes) ||
               (piece.offset >= start + 3 * UnitBytes && end <= start + RunUnits * UnitBytes);
    };
    report.check("each piece lies on one side of the block", std::all_of(pieces.begin(), pieces.end(), isBesideBlock));

    const auto pools = fsuser::tests::readPoolInfo();
    checkBytes(report, mount, paged, 3 * UnitBytes, pools ? pools->granule : UnitBytes);

    mount.unlink(paged);
    mount.unlink(block);
    unlinkAll(mount, gap->fillers);
}

}  // namespace

int main(int argc, char** argv)
{
    return fsuser::tests::runCase(argc, argv, "test_paged_extents", 1,
                                  [](Report& report, const fsuser::tests::Mounts_t& mounts)
                                  {
                                      const Mount mount{mounts.first()};
                                      const auto before = fsuser::tests::readPoolInfo();
                                      if (!before)
                                      {
                                          report.check("pool_info is readable", false);
                                          return;
                                      }
                                      checkHoles(report, mount);
                                      checkWholeBeforePieces(report, mount);
                                      checkBlockInMiddle(report, mount);
                                      report.check("the pool is back to what it held before",
                                                   fsuser::tests::waitForWritebackFree(before->wbFree));
                                  });
}
