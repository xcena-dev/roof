// SPDX-License-Identifier: Apache-2.0
//
// test_stamp_fallback -- a peer verdict that misses the wall-clock window falls back to whether its
// stamp moves. This puts one node's tick an hour behind its peer's clock, and separately a minute
// ahead, and checks that movement alone keeps its slot HELD well past the timeout and several
// sweeps: the old rule would have refused both on the wall clock alone and staked a live node.
//
// Root only, since the fault injection and the slot table both live behind test-only sysfs
// attributes. Runs alone against the pair's bootstrap state, so a case beside it would read a
// fault this one injected as its own.
//
//   test_stamp_fallback <mount-a> <mount-b>

#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
#include <string>
#include <thread>

#include "harness/harness.hpp"
#include "harness/mount.hpp"
#include "harness/sysfs.hpp"

namespace
{

using fsuser::tests::Report;

constexpr std::int64_t NanosPerSecond = 1'000'000'000;

// Far enough past the wall-clock window, either side, that only the movement fallback is left to
// call this node alive.
constexpr std::int64_t HourSlowOffsetNs = -3600 * NanosPerSecond;
constexpr std::int64_t MinuteFastOffsetNs = 60 * NanosPerSecond;

// Several sweeps' worth of wall-clock time, with room to spare, so a slower host is a slower run
// rather than a red one.
constexpr std::int32_t GraceSeconds = 45;

constexpr std::uint32_t BootstrapStateHeld = 1;

// Clears the offset when this goes out of scope, so a failed check here does not leave the next
// case on the host reading a peer this one mis-stamped.
class StampOffsetGuard
{
public:
    explicit StampOffsetGuard(std::uint32_t nodeId)
        : nodeId_{nodeId}
    {
    }
    ~StampOffsetGuard()
    {
        static_cast<void>(fsuser::tests::writeStampOffset(nodeId_, 0));
    }
    StampOffsetGuard(const StampOffsetGuard&) = delete;
    StampOffsetGuard& operator=(const StampOffsetGuard&) = delete;

private:
    std::uint32_t nodeId_;
};

[[nodiscard]] std::optional<std::uint32_t> slotStateOf(std::uint32_t readerNodeId,
                                                       std::uint32_t targetNodeId)
{
    const auto slots = fsuser::tests::readBootstrapSlots(readerNodeId);
    if (!slots)
    {
        return std::nullopt;
    }
    for (const auto& slot : *slots)
    {
        if (slot.node == targetNodeId)
        {
            return slot.state;
        }
    }
    return std::nullopt;
}

void checkMovingOutOfRangeStampStaysHeld(Report& report, std::uint32_t readerNodeId,
                                         std::uint32_t targetNodeId, std::int64_t offsetNs,
                                         const std::string& label)
{
    report.section("a moving stamp " + label + " this node's wall clock");

    const StampOffsetGuard guard{targetNodeId};
    report.check("the fault injection accepts the offset",
                 fsuser::tests::writeStampOffset(targetNodeId, offsetNs));

    std::this_thread::sleep_for(std::chrono::seconds(GraceSeconds));

    const auto state = slotStateOf(readerNodeId, targetNodeId);
    report.check("bootstrap_dump still carries this node's slot", state.has_value());
    if (state)
    {
        report.check("its state stays HELD, so movement alone kept it alive",
                     *state == BootstrapStateHeld);
    }
}

}  // namespace

int main(int argc, char** argv)
{
    fsuser::tests::Mounts_t mounts;
    if (!fsuser::tests::takeMounts(argc, argv, 2, mounts))
    {
        return 2;
    }

    if (::geteuid() != 0)
    {
        std::printf(
            "test_stamp_fallback: not root, so the fault injection and bootstrap_dump are "
            "unreadable: skipping\n");
        return fsuser::tests::SkipStatus;
    }

    const auto readerNodeId = fsuser::tests::Mount(mounts.first()).getNodeId();
    const auto targetNodeId = fsuser::tests::Mount(mounts.second()).getNodeId();

    fsuser::tests::Report report;
    try
    {
        checkMovingOutOfRangeStampStaysHeld(report, readerNodeId, targetNodeId, HourSlowOffsetNs,
                                            "an hour behind");
        checkMovingOutOfRangeStampStaysHeld(report, readerNodeId, targetNodeId, MinuteFastOffsetNs,
                                            "a minute ahead of");
    }
    catch (const std::exception& failure)
    {
        report.raised("test_stamp_fallback", failure);
    }

    return report.summarise("test_stamp_fallback");
}
