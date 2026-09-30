// SPDX-License-Identifier: Apache-2.0
//
// test_vm_protect_peer -- widening a peer's mapping goes back through the delegation, not just the
// descriptor.
//
// mmap checks the region's permissions once, and mprotect can be asked for more than mmap was. So a
// peer holding READ could reach a writable mapping in two steps if mprotect trusted the vma it was
// handed. What this case asks is which of the two guards answers, and the descriptor is what
// separates them: a read-only one is refused before the region is consulted at all, and a writable
// one leaves the delegation as the only thing that can refuse.
//
// The last section adds WRITE to the delegation while the mapping is still open, so the refusal is
// shown to follow the delegation rather than the moment the mapping was made.
//
//   test_vm_protect_peer <mount-a> <mount-b>

#include <fcntl.h>
#include <sys/mman.h>

#include <cerrno>
#include <cstdint>
#include <string>

#include "fs/file.hpp"
#include "harness/harness.hpp"
#include "harness/mount.hpp"

namespace
{

using fsuser::tests::caseName;
using fsuser::tests::PlacementSize;

// The descriptor arm. A read-only descriptor cannot carry a writable mapping whatever the region
// says, so this refusal happens without the delegation being read.
void checkReadOnlyDescriptorArm(fsuser::tests::Report& report, fsuser::tests::Mount& peer,
                                const std::string& name)
{
    report.section("a peer mapping taken from a read-only descriptor");

    auto seen = peer.open(name, O_RDONLY);
    auto mapping = seen.map(PlacementSize, PROT_READ);
    report.check("the peer maps for reading", mapping.isMapped());

    void* base = mapping.get();
    errno = 0;
    const std::int32_t answered = ::mprotect(base, PlacementSize, PROT_READ | PROT_WRITE);
    report.checkErrno("adding PROT_WRITE is refused",
                      answered < 0 && (errno == EACCES || errno == EPERM), errno);

    report.check("dropping to PROT_NONE is allowed", ::mprotect(base, PlacementSize, PROT_NONE) == 0);
    report.check("returning to PROT_READ is allowed",
                 ::mprotect(base, PlacementSize, PROT_READ) == 0);
}

// The record arm. A writable descriptor satisfies the first guard, so what refuses here is the
// region's own record, and adding the bit to that record has to change the answer.
void checkRecordArm(fsuser::tests::Report& report, fsuser::tests::Mount& peer,
                    fsuser::File& owned, const std::string& name)
{
    report.section("a peer mapping taken from a writable descriptor");

    auto seen = peer.open(name, O_RDWR);
    auto mapping = seen.map(PlacementSize, PROT_READ);
    report.check("the peer maps for reading", mapping.isMapped());

    void* base = mapping.get();
    errno = 0;
    const std::int32_t answered = ::mprotect(base, PlacementSize, PROT_READ | PROT_WRITE);
    report.checkErrno("adding PROT_WRITE is refused with EACCES while the region grants only READ",
                      answered < 0 && errno == EACCES, errno);

    report.checkAccepted("the owner adds WRITE to what the region grants", [&]
                         {
                             owned.setDefaultPermission(fsuser::Permission::Read | fsuser::Permission::Write);
                         });

    report.check("adding PROT_WRITE now succeeds on the same mapping",
                 ::mprotect(base, PlacementSize, PROT_READ | PROT_WRITE) == 0);
    report.check("narrowing back to PROT_READ still succeeds",
                 ::mprotect(base, PlacementSize, PROT_READ) == 0);
}

}  // namespace

int main(int argc, char** argv)
{
    const auto body = [](fsuser::tests::Report& report, fsuser::tests::Mounts_t& mounts)
    {
        const auto name = caseName("vm_protect_peer");

        auto owner = fsuser::tests::Mount(mounts.first());
        auto peer = fsuser::tests::Mount(mounts.second());

        auto owned = fsuser::tests::placeFile(owner, name);
        // What the region grants, and not a delegation: mprotect reads the record without asking
        // the helper, so this is the one lever that moves its answer mid-test.
        owned.setDefaultPermission(fsuser::Permission::Read);

        checkReadOnlyDescriptorArm(report, peer, name);
        checkRecordArm(report, peer, owned, name);

        owner.unlink(name);
    };
    return fsuser::tests::runCase(argc, argv, "test_vm_protect_peer", 2, body);
}
