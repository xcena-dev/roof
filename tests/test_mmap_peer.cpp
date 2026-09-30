// SPDX-License-Identifier: Apache-2.0
//
// test_mmap_peer -- bytes one node stores in a region and the other node finds.
//
// Two mounts of one device, so the same process reaches the same region as two nodes. The owner
// writes through the first mount and the peer reads through the second, which is the whole point of
// the region: neither mount has its own copy of the bytes.
//
// The peer needs a delegation before it may map at all. What that delegation admits is
// test_perm_peer's subject, so here the grant is setup and the bytes are the claim.
//
// No flush stands between the two halves. Both mounts are this host's, so bytes a mapping writes are
// in this host's cache before the other mount reads them.
//
//   test_mmap_peer <mount-a> <mount-b>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <string>

#include "fs/file.hpp"
#include "harness/harness.hpp"
#include "harness/mount.hpp"
#include "harness/pattern.hpp"

namespace
{

using fsuser::tests::PlacementSize;
constexpr std::uint64_t PlacedWords = PlacementSize / sizeof(std::uint32_t);

// One seed for what the owner stores and another for what the peer stores over it, so a stale read
// on either side reads as the other side's pattern rather than as a plausible value.
constexpr std::uint32_t OwnerSeed = 0xB7B7B7B7U;
constexpr std::uint32_t PeerSeed = 0x1D1D1D1DU;

// How many words the peer writes back. The owner only has to find that the peer's bytes arrived, and
// the direction was already covered a whole span at a time on the way out.
constexpr std::uint64_t WrittenBackWords = 4;

using fsuser::tests::caseName;

void checkBytesCrossTheMounts(fsuser::tests::Report& report, fsuser::tests::Mount& owner,
                              fsuser::tests::Mount& peer, const std::string& name)
{
    auto file = fsuser::tests::placeFile(owner, name);
    // No grant here. The region starts owner-only, so the peer's first map is refused on the
    // record and the helper on that node decides, which is the only way a peer gets in.

    report.section("the owner stores a pattern");
    {
        auto stored = file.map(PlacementSize, PROT_READ | PROT_WRITE);
        report.check("the owner maps writably", stored.isMapped());
        fsuser::tests::fillPattern(static_cast<std::uint32_t*>(stored.get()), PlacedWords,
                                   OwnerSeed);
    }

    report.section("the peer finds it through the other mount");
    auto seen = peer.open(name, O_RDWR);

    struct ::stat placed = {};
    report.check("the peer sees the placed size",
                 ::fstat(seen.get(), &placed) == 0 &&
                     static_cast<std::uint64_t>(placed.st_size) == PlacementSize);
    {
        auto reading = seen.map(PlacementSize, PROT_READ);
        report.check("the peer maps for reading", reading.isMapped());
        fsuser::tests::checkPattern(report, "the peer's mapping holds the owner's pattern",
                                    static_cast<const std::uint32_t*>(reading.get()), PlacedWords,
                                    OwnerSeed);
    }

    std::uint32_t taken[WrittenBackWords] = {};
    report.check("the peer seeks to the start", ::lseek(seen.get(), 0, SEEK_SET) == 0);
    const std::int64_t got = ::read(seen.get(), taken, sizeof(taken));
    report.check("the peer's read returns the bytes it asked for",
                 got == static_cast<std::int64_t>(sizeof(taken)));
    if (got == static_cast<std::int64_t>(sizeof(taken)))
    {
        fsuser::tests::checkPattern(report, "the peer's read holds the owner's pattern", taken,
                                    WrittenBackWords, OwnerSeed);
    }

    report.section("the peer stores over it and the owner finds that");
    {
        auto writing = seen.map(PlacementSize, PROT_READ | PROT_WRITE);
        report.check("the peer maps writably", writing.isMapped());
        fsuser::tests::fillPattern(static_cast<std::uint32_t*>(writing.get()), WrittenBackWords,
                                   PeerSeed);
    }
    {
        auto back = file.map(PlacementSize, PROT_READ);
        report.check("the owner maps again", back.isMapped());
        fsuser::tests::checkPattern(report, "the owner's mapping holds the peer's pattern",
                                    static_cast<const std::uint32_t*>(back.get()),
                                    WrittenBackWords, PeerSeed);
    }
}

}  // namespace

int main(int argc, char** argv)
{
    const auto body = [](fsuser::tests::Report& report, fsuser::tests::Mounts_t& mounts)
    {
        const auto name = caseName("mmap_peer");

        auto owner = fsuser::tests::Mount(mounts.first());
        auto peer = fsuser::tests::Mount(mounts.second());
        checkBytesCrossTheMounts(report, owner, peer, name);
        owner.unlink(name);
    };
    return fsuser::tests::runCase(argc, argv, "test_mmap_peer", 2, body);
}
