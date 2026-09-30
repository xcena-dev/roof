// SPDX-License-Identifier: Apache-2.0
//
// test_unmount_drain_race -- a prepare that arrives while another is refusing waits its turn.
// The first prepare sees an open file and is held just before its refusal. The file closes, and a
// second prepare that ran now would clean the node and then see the first one reopen the mount.
//
//   test_unmount_drain_race <mount-a>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>
#include <thread>

#include "harness/harness.hpp"
#include "harness/mount.hpp"
#include "harness/slot.hpp"
#include "name.h"

namespace
{

using fsuser::tests::caseName;
using fsuser::tests::findRowByName;
using fsuser::tests::PlacementSize;

constexpr std::uint32_t RefusalDelayMs = 1500;
// Long enough for the first prepare to reach its delay, short against that delay.
constexpr std::chrono::milliseconds EnterWait{300};
constexpr const char* RefusalDelayPath =
    "/sys/module/" FS_NAME_STR "/parameters/unmount_prepare_refusal_delay_ms";

[[nodiscard]] std::string unmountPreparePath(std::uint32_t nodeId)
{
    return "/sys/fs/" FS_NAME_STR "/node" + std::to_string(nodeId) + "/unmount_prepare";
}

// The errno one write to unmount_prepare ends with, 0 when it was accepted, -1 when it never opened.
[[nodiscard]] std::int32_t writePrepare(const std::string& path)
{
    const std::int32_t descriptor = ::open(path.c_str(), O_WRONLY);
    if (descriptor < 0)
    {
        return -1;
    }
    errno = 0;
    const auto written = ::write(descriptor, "1", 1);
    const std::int32_t savedErrno = (written < 0) ? errno : 0;
    ::close(descriptor);
    return savedErrno;
}

[[nodiscard]] bool writeRefusalDelay(std::uint32_t delayMs)
{
    std::ofstream sink{RefusalDelayPath};
    if (!sink)
    {
        return false;
    }
    sink << delayMs;
    sink.close();
    return !sink.fail();
}

// Puts the delay back to none whichever way the case ends, so no later prepare is held.
class RefusalDelayGuard
{
public:
    explicit RefusalDelayGuard(std::uint32_t delayMs)
        : applied_{writeRefusalDelay(delayMs)}
    {
    }
    ~RefusalDelayGuard()
    {
        static_cast<void>(writeRefusalDelay(0));
    }
    RefusalDelayGuard(const RefusalDelayGuard&) = delete;
    RefusalDelayGuard& operator=(const RefusalDelayGuard&) = delete;
    RefusalDelayGuard(RefusalDelayGuard&&) = delete;
    RefusalDelayGuard& operator=(RefusalDelayGuard&&) = delete;

    [[nodiscard]] bool applied() const
    {
        return applied_;
    }

private:
    bool applied_;
};

}  // namespace

int main(int argc, char** argv)
{
    fsuser::tests::Mounts_t mounts;
    if (!fsuser::tests::takeMounts(argc, argv, 1, mounts))
    {
        return fsuser::tests::UsageStatus;
    }

    fsuser::tests::Report report;

    // unmount_prepare is mode 0200 and the module parameter 0600, so only root reaches either.
    if (::geteuid() != 0)
    {
        report.note("not root, so unmount_prepare cannot be reached: skipping");
        return fsuser::tests::SkipStatus;
    }
    if (::access(RefusalDelayPath, W_OK) != 0)
    {
        report.note("this module was built without the test knobs: skipping");
        return fsuser::tests::SkipStatus;
    }
    // The kernel attests the real uid, which the rules name as the invoking account.
    static_cast<void>(fsuser::tests::dropRealIdsToInvoker());

    auto mount = fsuser::tests::Mount(mounts.first());
    const auto name = caseName("unmount_drain_race");
    const auto path = unmountPreparePath(mount.getNodeId());

    report.section("a prepare held inside its refusal");

    const RefusalDelayGuard delay{RefusalDelayMs};
    report.check("the refusal delay is set", delay.applied());

    std::atomic<bool> firstReturned{false};
    std::int32_t firstErrno = -1;
    std::thread firstPrepare;
    {
        auto file = fsuser::tests::placeFile(mount, name);
        auto mapped = file.map(PlacementSize, PROT_READ | PROT_WRITE);
        report.check("the file maps writably", mapped.isMapped());

        firstPrepare = std::thread{[&path, &firstErrno, &firstReturned]
                                   {
                                       firstErrno = writePrepare(path);
                                       firstReturned.store(true);
                                   }};
        std::this_thread::sleep_for(EnterWait);
    }
    // The mapping and the descriptor are gone, so this node holds no reference from here.

    report.section("a second prepare while the first is still refusing");

    report.check("the first prepare has not returned yet", !firstReturned.load());
    const std::int32_t secondErrno = writePrepare(path);
    report.checkErrno("the second prepare is told to try again", secondErrno == EAGAIN, secondErrno);

    firstPrepare.join();
    report.checkErrno("the first prepare is refused with EBUSY", firstErrno == EBUSY, firstErrno);

    report.section("nothing either prepare cleared");

    report.check("the region is still listed", findRowByName(name).has_value());
    report.checkAccepted("an open of the same name succeeds",
                         [&]
                         {
                             auto reopened = mount.open(name, O_RDONLY);
                         });
    const auto secondName = caseName("unmount_drain_race_second");
    report.checkAccepted("a create and place of a new name succeeds",
                         [&]
                         {
                             auto second = fsuser::tests::placeFile(mount, secondName);
                         });
    // A cleared node has dropped the first name already, so each unlink is a check of its own.
    report.checkAccepted("the new name unlinks", [&]
                         {
                             mount.unlink(secondName);
                         });
    report.checkAccepted("the held name unlinks", [&]
                         {
                             mount.unlink(name);
                         });

    return report.summarise("test_unmount_drain_race");
}
