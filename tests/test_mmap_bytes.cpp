// SPDX-License-Identifier: Apache-2.0
//
// test_mmap_bytes -- the bytes a mapping writes, and every way of finding them again.
//
// Placement is not the subject here. That one file gets one extent is what test_perm_grants asks,
// and this case takes a placed file and asks what happens to what is stored in it.
//
// read() has no verb on the library. The module refuses write() outright and serves data through
// mappings, so a read verb beside map() would suggest a second way in that does not exist. The
// descriptor a File already exposes is what the two syscall checks below use.
//
//   test_mmap_bytes <mount-a>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
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

// Two seeds, so the second mapping's bytes are distinguishable from the first's.
constexpr std::uint32_t FirstSeed = 0xA5A5A5A5U;

// How many words the read() checks compare. read() walks the region a word at a time through the
// kernel's own mapping, and the whole span would only repeat what the mapping check already covered.
constexpr std::uint64_t ReadWords = 4;

using fsuser::tests::caseName;

void checkStoredBytes(fsuser::tests::Report& report, fsuser::tests::Mount& mount,
                      const std::string& name)
{
    auto file = fsuser::tests::placeFile(mount, name);

    report.section("a mapping stores and returns its own bytes");
    {
        auto mapping = file.map(PlacementSize, PROT_READ | PROT_WRITE);
        report.check("the placed file maps writably", mapping.isMapped());
        report.check("and spans what was asked for", mapping.getSize() == PlacementSize);

        auto* words = static_cast<std::uint32_t*>(mapping.get());
        fsuser::tests::fillPattern(words, PlacedWords, FirstSeed);
        fsuser::tests::checkPattern(report, "every word reads back what was written", words,
                                    PlacedWords, FirstSeed);
    }

    report.section("the descriptor beside the mapping");

    struct ::stat placed = {};
    report.check("fstat reports the placed size",
                 ::fstat(file.get(), &placed) == 0 &&
                     static_cast<std::uint64_t>(placed.st_size) == PlacementSize);

    std::uint32_t taken[ReadWords] = {};
    report.check("lseek to the start succeeds", ::lseek(file.get(), 0, SEEK_SET) == 0);
    const std::int64_t got = ::read(file.get(), taken, sizeof(taken));
    report.check("read returns the bytes it was asked for",
                 got == static_cast<std::int64_t>(sizeof(taken)));
    if (got == static_cast<std::int64_t>(sizeof(taken)))
    {
        fsuser::tests::checkPattern(report, "read returns what the mapping wrote", taken, ReadWords,
                                    FirstSeed);
    }

    // Data reaches a region only through a writable mapping, so the descriptor refuses this even for
    // the owner.
    errno = 0;
    const std::int64_t put = ::write(file.get(), taken, sizeof(taken));
    report.checkErrno("write on the descriptor is refused with EACCES",
                      put < 0 && errno == EACCES, errno);

    report.section("a second mapping of the same file");
    {
        auto again = file.map(PlacementSize, PROT_READ);
        report.check("the placed file maps again", again.isMapped());
        fsuser::tests::checkPattern(report, "the bytes outlive the first mapping",
                                    static_cast<const std::uint32_t*>(again.get()), PlacedWords,
                                    FirstSeed);
    }
}

}  // namespace

int main(int argc, char** argv)
{
    const auto body = [](fsuser::tests::Report& report, fsuser::tests::Mounts_t& mounts)
    {
        const auto name = caseName("mmap_bytes");

        auto mount = fsuser::tests::Mount(mounts.first());
        checkStoredBytes(report, mount, name);
        mount.unlink(name);
    };
    return fsuser::tests::runCase(argc, argv, "test_mmap_bytes", 1, body);
}
