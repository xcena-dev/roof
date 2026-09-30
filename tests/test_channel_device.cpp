// SPDX-License-Identifier: Apache-2.0
//
// test_channel_device -- the daemon's upcall channel is the daemon's own, not this mount's.
//
// The device node an unprivileged caller finds under /dev names the channel the kernel asks for
// an access decision, and nothing but the daemon that owns it may open it.
//
//   test_channel_device <mount-a>

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <string>

#include "harness/harness.hpp"
#include "harness/mount.hpp"
#include "name.h"

namespace
{

void checkChannelIsNotOpenable(fsuser::tests::Report& report, std::uint32_t nodeId)
{
    report.section("the upcall channel device");

    const std::string path = "/dev/" DAEMON_NAME_STR "-" + std::to_string(nodeId);

    if (::geteuid() == 0)
    {
        report.note("running as root, so the device's own owner check was skipped");
        return;
    }

    errno = 0;
    const std::int32_t opened = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    report.checkErrno("an unprivileged process cannot open the channel device",
                      opened < 0 && errno == EACCES, errno);
    if (opened >= 0)
    {
        ::close(opened);
    }
}

}  // namespace

int main(int argc, char** argv)
{
    fsuser::tests::Mounts_t mounts;
    if (!fsuser::tests::takeMounts(argc, argv, 1, mounts))
    {
        return 2;
    }

    const auto mount = fsuser::tests::Mount(mounts.first());
    const std::uint32_t nodeId = mount.getNodeId();
    if (nodeId == 0)
    {
        std::printf(
            "test_channel_device: the mount table named no node id for this mount, so the "
            "channel path could not be built\n");
        return fsuser::tests::SkipStatus;
    }

    fsuser::tests::Report report;
    checkChannelIsNotOpenable(report, nodeId);
    return report.summarise("test_channel_device");
}
