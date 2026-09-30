// SPDX-License-Identifier: Apache-2.0
//
// test_vm_protect -- what the vm_ops wrapper allows to happen to a mapping after mmap returns.
//
// The module installs its own vm_ops over the ones device_dax provides, and the flags it sets on the vma
// decide four things: which protections mprotect may move to, whether a fork inherits the mapping,
// whether mremap may grow it, and whether a partial mprotect may split it. None of the four is
// visible from the address a mapping hands back, so each one is a syscall on that address.
//
// mprotect and mremap have no verb on the library. Adding one would put public API on the surface
// for a test's sake, so the checks below call them on the address Mapping already exposes. The two
// mremap sections open their own mapping instead: mremap moves the address, and a Mapping holding
// the address from before the move would unmap the wrong span.
//
//   test_vm_protect <mount-a>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <string>

#include "fs/file.hpp"
#include "fs/testing.hpp"
#include "harness/harness.hpp"
#include "harness/mount.hpp"

namespace
{

// The DEV_DAX alignment floor, and device_dax refuses a split finer than that. A span of four such
// chunks is the smallest one where a slice can be carved out of the middle with a whole chunk left
// on either side.
constexpr std::uint64_t ChunkSize = fsuser::tests::PlacementSize;
constexpr std::uint64_t SplitSpan = ChunkSize * 4;

// A value the fork check writes before forking, so the parent can tell an intact mapping from one the
// child's death disturbed.
constexpr std::uint32_t ParentMark = 0xC3C3C3C3U;

// How many split-and-merge rounds the reference-count check runs. One round proves the calls answer;
// what a run of them shows is that the wrapper's open and close hooks stay in balance.
constexpr std::int32_t SplitRounds = 200;

using fsuser::tests::caseName;

// mprotect on the whole span never splits the vma, so the address and the length stay what Mapping
// holds and its own unmapping stays correct.
void checkProtectionMoves(fsuser::tests::Report& report, fsuser::File& file)
{
    report.section("mprotect over a whole owner mapping");

    auto mapping = file.map(SplitSpan, PROT_READ | PROT_WRITE);
    report.check("the owner maps writably", mapping.isMapped());

    void* base = mapping.get();
    report.check("mprotect to the protection it already has",
                 ::mprotect(base, SplitSpan, PROT_READ | PROT_WRITE) == 0);
    report.check("mprotect narrows to PROT_READ", ::mprotect(base, SplitSpan, PROT_READ) == 0);
    report.check("mprotect narrows to PROT_NONE", ::mprotect(base, SplitSpan, PROT_NONE) == 0);
    report.check("mprotect widens back to PROT_READ", ::mprotect(base, SplitSpan, PROT_READ) == 0);
    report.check("mprotect widens back to PROT_READ|PROT_WRITE",
                 ::mprotect(base, SplitSpan, PROT_READ | PROT_WRITE) == 0);
}

// A read-only descriptor is the other half of the guard: the owner holds every permission, so what
// refuses this is the descriptor and not the region.
void checkReadOnlyCannotWiden(fsuser::tests::Report& report, const std::string& mount,
                              const std::string& name)
{
    report.section("mprotect on a mapping taken from a read-only descriptor");

    const std::int32_t taken = fsuser::testing::openRaw(mount, name, O_RDONLY | O_CLOEXEC);
    report.check("a read-only open succeeds", taken >= 0);
    if (taken < 0)
    {
        return;
    }

    void* base = ::mmap(nullptr, SplitSpan, PROT_READ, MAP_SHARED, taken, 0);
    report.check("it maps for reading", base != MAP_FAILED);
    if (base != MAP_FAILED)
    {
        errno = 0;
        const std::int32_t answered = ::mprotect(base, SplitSpan, PROT_READ | PROT_WRITE);
        report.checkErrno("adding PROT_WRITE is refused",
                          answered < 0 && (errno == EACCES || errno == EPERM), errno);
        ::munmap(base, SplitSpan);
    }
    ::close(taken);
}

// The vma carries VM_DONTCOPY, so a fork leaves the child without it. The child touches the address
// anyway and has to die on it, which is the only way that absence is observable.
void checkForkDoesNotInherit(fsuser::tests::Report& report, fsuser::File& file)
{
    report.section("a fork and the mapping it does not inherit");

    auto mapping = file.map(SplitSpan, PROT_READ | PROT_WRITE);
    report.check("the owner maps writably", mapping.isMapped());

    auto* marked = static_cast<volatile std::uint32_t*>(mapping.get());
    marked[0] = ParentMark;

    const ::pid_t child = ::fork();
    if (child == 0)
    {
        const std::uint32_t seen = marked[0];
        static_cast<void>(seen);
        ::_exit(0);
    }
    report.check("fork succeeds", child > 0);
    if (child <= 0)
    {
        return;
    }

    std::int32_t status = 0;
    const bool reaped = ::waitpid(child, &status, 0) == child;
    report.check("the child dies on the address it inherited no mapping for",
                 reaped && (WIFSIGNALED(status) != 0) && WTERMSIG(status) == SIGSEGV);
    report.check("the parent's mapping is untouched", marked[0] == ParentMark);
}

// VM_DONTEXPAND, from a mapping this function owns outright: a successful mremap returns a different
// address, and letting Mapping hold that span would put the wrong one in its destructor.
void checkMremapCannotGrow(fsuser::tests::Report& report, const std::string& mount,
                           const std::string& name)
{
    report.section("mremap on a mapping of half the placed span");

    const std::int32_t taken = fsuser::testing::openRaw(mount, name, O_RDWR | O_CLOEXEC);
    report.check("a writable open succeeds", taken >= 0);
    if (taken < 0)
    {
        return;
    }

    // Half the span, so the file itself has room for the growth the kernel is being asked for and the
    // refusal comes from the vma rather than from the extent running out.
    const std::uint64_t half = SplitSpan / 2;
    void* base = ::mmap(nullptr, half, PROT_READ | PROT_WRITE, MAP_SHARED, taken, 0);
    report.check("it maps", base != MAP_FAILED);
    if (base != MAP_FAILED)
    {
        errno = 0;
        void* grew = ::mremap(base, half, SplitSpan, 0);
        report.checkErrno("growing in place is refused", grew == MAP_FAILED && errno == EFAULT,
                          errno);

        errno = 0;
        grew = ::mremap(base, half, SplitSpan, MREMAP_MAYMOVE);
        report.checkErrno("growing with MREMAP_MAYMOVE is refused",
                          grew == MAP_FAILED && errno == EFAULT, errno);

        // The same call at the same size is a relocation and not growth, so the flag that blocks
        // growth leaves it alone.
        void* moved = ::mremap(base, half, half, MREMAP_MAYMOVE);
        report.check("relocating at the same size succeeds", moved != MAP_FAILED);
        ::munmap(moved != MAP_FAILED ? moved : base, half);
    }
    ::close(taken);
}

// A partial mprotect splits the vma into three, and the wrapper's open and close hooks each run on
// the pieces. What a run of rounds asks is whether the file reference those hooks hold comes back to
// where it started every time.
void checkSplitAndMergeStayBalanced(fsuser::tests::Report& report, fsuser::File& file)
{
    report.section("a slice carved out of the middle and put back");

    auto mapping = file.map(SplitSpan, PROT_READ | PROT_WRITE);
    report.check("the owner maps writably", mapping.isMapped());

    auto* base = static_cast<char*>(mapping.get());
    char* middle = base + ChunkSize;

    report.check("narrowing the middle chunk to PROT_READ splits the mapping",
                 ::mprotect(middle, ChunkSize, PROT_READ) == 0);

    *reinterpret_cast<volatile char*>(base) = 'A';
    *reinterpret_cast<volatile char*>(base + ChunkSize * 2) = 'Z';
    report.check("the chunks on either side stay writable",
                 base[0] == 'A' && base[ChunkSize * 2] == 'Z');

    report.check("widening the whole span again merges it",
                 ::mprotect(base, SplitSpan, PROT_READ | PROT_WRITE) == 0);

    std::int32_t rounds = 0;
    while (rounds < SplitRounds)
    {
        if (::mprotect(middle, ChunkSize, PROT_READ) != 0 ||
            ::mprotect(base, SplitSpan, PROT_READ | PROT_WRITE) != 0)
        {
            break;
        }
        ++rounds;
    }
    report.check("every split and merge round answers", rounds == SplitRounds);
}

}  // namespace

int main(int argc, char** argv)
{
    const auto body = [](fsuser::tests::Report& report, fsuser::tests::Mounts_t& mounts)
    {
        const auto name = caseName("vm_protect");

        auto mount = fsuser::tests::Mount(mounts.first());
        auto file = mount.open(name, O_CREAT | O_RDWR, 0644);
        file.resize(SplitSpan);

        checkProtectionMoves(report, file);
        checkReadOnlyCannotWiden(report, mounts.first(), name);
        checkForkDoesNotInherit(report, file);
        checkMremapCannotGrow(report, mounts.first(), name);
        checkSplitAndMergeStayBalanced(report, file);

        mount.unlink(name);
    };
    return fsuser::tests::runCase(argc, argv, "test_vm_protect", 1, body);
}
