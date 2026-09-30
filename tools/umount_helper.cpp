// SPDX-License-Identifier: Apache-2.0
//
// umount_helper.cpp - the other half of the bring-up the mount helper owns.
//
// umount(8) execs /sbin/umount.<type> with the mount point, so unmounting one arrives here
// the way `mount` arrives at the helper beside this file.
//
// It exists because the teardown has an order. The daemon maps the lock region, which lives on the
// filesystem being unmounted, and a live mapping holds the mount. So umount(2) alone answers EBUSY
// until somebody stops the daemon, and nothing tells the caller that is what happened.
//
//   1. have the kernel clear this node's rows while the daemon can still take the turn, and stop
//      here, daemon untouched, when a file is still open or the turn cannot be taken
//   2. stop the daemon serving this mount
//   3. umount(2)
//   4. on EBUSY, name the processes still mapping the lock region
//
// How the daemon goes down is a deployment's choice, the way <FS>_DAEMON_START is on the way up:
//   <FS>_DAEMON_STOP   default `systemctl stop <daemon>@<node>`

#include <dirent.h>
#include <sys/mount.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "helper_common.hpp"
#include "mount_table.hpp"
#include "tools_uapi.h"

namespace
{
constexpr std::uint32_t PrepareAttempts = 3;
constexpr std::chrono::seconds PrepareRetryWait{1};

// The errno the kernel refused the clean step with, or 0 once it ran. A mount with no such file
// has no clean step to ask for.
std::int32_t writePrepare(const std::string& preparePath)
{
    auto* prepare = std::fopen(preparePath.c_str(), "w");
    if (prepare == nullptr)
    {
        return 0;
    }
    const auto put = std::fputs("1", prepare) >= 0;
    const std::int32_t refused = put ? 0 : errno;
    const auto closed = std::fclose(prepare) == 0;
    if (refused != 0)
    {
        return refused;
    }
    return closed ? 0 : errno;
}

// Asks the kernel for the clean step and says whether the unmount may go on. A refusal leaves the
// mount and its daemon as they are, and says which of the two reasons it was.
bool clearNodeRows(const std::string& preparePath, const std::string& target)
{
    auto refused = writePrepare(preparePath);
    // EAGAIN is a turn held past the kernel's own retries, which a later try can still get.
    for (std::uint32_t attempt = 1; attempt < PrepareAttempts && refused == EAGAIN; ++attempt)
    {
        std::this_thread::sleep_for(PrepareRetryWait);
        refused = writePrepare(preparePath);
    }
    if (refused == EBUSY)
    {
        std::fprintf(stderr, FS_PROGRAM_NAME
                     ": %s: a file on %s is still open; nothing was cleared, "
                     "and the daemon is left running\n",
                     preparePath.c_str(), target.c_str());
        return false;
    }
    if (refused != 0)
    {
        std::fprintf(stderr, FS_PROGRAM_NAME
                     ": %s: %s; this node's turn could not be taken, so check "
                     "that its daemon is running; the daemon is left as it is\n",
                     preparePath.c_str(), std::strerror(refused));
        return false;
    }
    return true;
}

// A C handle closed by the call its own library names. A deleter type rather than a function
// pointer, because taking the address of one carries its nonnull attribute into the type.
template <typename T, int (*close)(T*)>
class CloseWith
{
public:
    void operator()(T* held) const noexcept
    {
        close(held);
    }
};

using DirHandle = std::unique_ptr<::DIR, CloseWith<::DIR, ::closedir>>;
using FileHandle = std::unique_ptr<std::FILE, CloseWith<std::FILE, std::fclose>>;

void printUsage()
{
    std::fprintf(stderr, "usage: " FS_PROGRAM_NAME " <dir> [-flnrv]\n");
}

// Whether the process at @procName still has @path mapped. "re" is fopen's close-on-exec mode, so
// nothing this helper runs later inherits the open /proc file.
[[nodiscard]] bool hasPathMapped(const char* procName, const std::string& path)
{
    const auto maps = std::string{"/proc/"} + procName + "/maps";
    // A process that ended, or one this caller may not read, maps nothing it can prove.
    const FileHandle opened{std::fopen(maps.c_str(), "re")};
    if (!opened)
    {
        return false;
    }

    constexpr std::uint32_t LineBytes = 4096;
    std::array<char, LineBytes> line{};
    while (std::fgets(line.data(), static_cast<int>(line.size()), opened.get()) != nullptr)
    {
        if (std::strstr(line.data(), path.c_str()) != nullptr)
        {
            return true;
        }
    }
    return false;
}

// Whoever still has @path mapped, by pid. Read from /proc rather than guessed, because the answer
// is what turns EBUSY from a verdict into something an operator can act on.
[[nodiscard]] std::vector<std::string> findMappers(const std::string& path)
{
    const DirHandle proc{::opendir("/proc")};
    if (!proc)
    {
        return {};
    }

    std::vector<std::string> holders;
    while (const auto* entry = ::readdir(proc.get()))
    {
        if (entry->d_name[0] < '1' || entry->d_name[0] > '9')
        {
            continue;
        }
        if (hasPathMapped(static_cast<const char*>(entry->d_name), path))
        {
            holders.emplace_back(static_cast<const char*>(entry->d_name));
        }
    }
    return holders;
}

}  // namespace

int main(int argc, char** argv)
{
    std::string target;

    for (std::int32_t index = 1; index < argc; ++index)
    {
        const std::string_view argument{argv[index]};
        // -f force, -l lazy, -n no mtab, -r remount-ro, -v verbose: none change what happens here,
        // and the kernel sees the flags only through the umount(2) below.
        if (fstools::isFlagArgument(argument))
        {
            continue;
        }
        if (!target.empty())
        {
            printUsage();
            return 1;
        }
        target = argument;
    }

    if (target.empty())
    {
        printUsage();
        return 1;
    }

    // The instance is named after the node id the kernel picked, so it is read back off the mount.
    // A mount reporting none has no daemon of its own to stop.
    if (const auto nodeId = fstools::readNodeId(target); nodeId.has_value())
    {
        // While the daemon is still up: the rows this node wrote go under a turn only that daemon
        // can take, and kill_sb runs after the stop below with nobody to take it.
        const auto preparePath = fstools::nameNodeSysfsPath(*nodeId, "unmount_prepare");
        if (!clearNodeRows(preparePath, target))
        {
            return 1;
        }

        const auto stopCommand = fstools::readStopCommand(*nodeId);

        // A stop that fails is not fatal on its own: the daemon may already be down, and the
        // unmount below is the real test of whether anything still holds the mount.
        if (fstools::runCommand(stopCommand) != 0)
        {
            std::fprintf(stderr, FS_PROGRAM_NAME ": `%s` did not succeed; unmounting anyway\n",
                         stopCommand.c_str());
        }
    }

    if (::umount(target.c_str()) == 0)
    {
        return 0;
    }

    const std::int32_t failed = errno;
    std::fprintf(stderr, FS_PROGRAM_NAME ": umount %s: %s\n", target.c_str(), std::strerror(failed));

    if (failed == EBUSY)
    {
        const auto holders = findMappers(target + "/" FS_LOCK_REGION_NAME);
        if (holders.empty())
        {
            std::fprintf(stderr,
                         FS_PROGRAM_NAME
                         ": nothing maps the lock region, so the mount is "
                         "held by an open file or a working directory on it\n");
        }
        else
        {
            std::fprintf(stderr, FS_PROGRAM_NAME ": the lock region is still mapped by pid");
            for (const auto& pid : holders)
            {
                std::fprintf(stderr, " %s", pid.c_str());
            }
            std::fprintf(stderr, "\n");
        }
    }
    return fstools::HelperFailure;
}
