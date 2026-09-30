// SPDX-License-Identifier: Apache-2.0
//
// test_mmap_unplaced -- a file exists before it has any CXL bytes, and what that state answers.
//
// Creation and placement are two calls: open(O_CREAT) links the name and resize commits the extent.
// Between them the file has a RAT entry and no physical offset, and mapping it would hand out an
// address with nothing behind it. ENODATA is what says so, held apart from EACCES so a caller can
// tell "not placed yet" from "not yours".
//
//   test_mmap_unplaced <mount-a>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <string>
#include <system_error>

#include "fs/file.hpp"
#include "harness/harness.hpp"
#include "harness/mount.hpp"

namespace
{

using fsuser::tests::caseName;
using fsuser::tests::PlacementSize;

void checkUnplacedFile(fsuser::tests::Report& report, fsuser::tests::Mount& mount,
                       const std::string& name)
{
    auto file = mount.open(name, O_CREAT | O_RDWR, 0644);

    report.section("a file with no extent");

    struct ::stat fresh = {};
    report.check("fstat reports no size",
                 ::fstat(file.get(), &fresh) == 0 && fresh.st_size == 0);

    report.checkRefusedWith("mapping it answers ENODATA", std::errc::no_message_available,
                            [&]
                            {
                                auto mapping = file.map(PlacementSize, PROT_READ);
                            });

    // read() reaches the same missing offset by another path. The kernel may report it as the same
    // refusal or as an empty file, and either answer says there is nothing there yet.
    std::uint32_t taken[4] = {};
    errno = 0;
    const std::int64_t got = ::read(file.get(), taken, sizeof(taken));
    report.checkErrno("reading it returns nothing, either as ENODATA or as end of file",
                      got == 0 || (got < 0 && errno == ENODATA), errno);

    report.section("the same file once it is placed");
    report.checkAccepted("resize places the extent", [&]
                         {
                             file.resize(PlacementSize);
                         });
    report.checkAccepted("mapping it now succeeds", [&]
                         {
                             auto mapping = file.map(PlacementSize, PROT_READ);
                             if (!mapping.isMapped())
                             {
                                 throw fsuser::FsError{"map returned nothing"};
                             }
                         });
}

}  // namespace

int main(int argc, char** argv)
{
    const auto body = [](fsuser::tests::Report& report, fsuser::tests::Mounts_t& mounts)
    {
        const auto name = caseName("mmap_unplaced");

        auto mount = fsuser::tests::Mount(mounts.first());
        checkUnplacedFile(report, mount, name);
        mount.unlink(name);
    };
    return fsuser::tests::runCase(argc, argv, "test_mmap_unplaced", 1, body);
}
