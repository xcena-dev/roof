// SPDX-License-Identifier: Apache-2.0
//
// mount_helper.cpp - the mount helper that owns the whole bring-up of one mount.
//
// mount(8) execs /sbin/mount.<type> with (device, dir, options), so fstab and a
// systemd .mount unit both arrive here. The kernel cannot finish a mount alone:
// mount(2) returns before the daemon can open the lock region, which lives on
// the filesystem being mounted.
//
//   1. mount(2), which claims this node's identity and binds the lock region to the helper's
//      account, both through the kernel's settle window
//   2. format the lock region's content if nobody has, and bring this node's helper up on it,
//      which is what puts the metadata domain in it
//
// A failure after stage 1 unmounts, so the caller sees a finished mount or none.
//
// The helper's own settings are not this helper's to choose. They live in the file
// `-o daemon_config=` names, which an administrator writes and this helper only hands over. Its
// owner is the daemon account, and the daemon reads the format arguments back out of it.
// These commands stay overridable, since they are how to invoke a tool rather than what to
// configure:
//   <FS>_DAEMON         the daemon binary, asked what the region needs
//   <FS>_DAEMON_START   how this node's helper comes up (systemctl start <daemon>@<node>)
//   <FS>_DAEMON_STOP    how it goes down again when it never greets (systemctl stop <daemon>@<node>)
//   <FS>_CME_FORMAT     how the lock region is formatted (cme-format)

#include <pwd.h>
#include <sys/mount.h>
#include <sys/stat.h>

#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "daemon_command.hpp"
#include "helper_common.hpp"
#include "mount_table.hpp"
#include "name.h"
#include "tools_uapi.h"

namespace
{

constexpr const char* FsType = FS_NAME_STR;

// The one option here that mount(2) never sees: which helper config serves this mount.
constexpr std::string_view DaemonConfigOption = "daemon_config=";

// Where the helper's config lives unless -o daemon_config= names another.
constexpr std::string_view DefaultDaemonConfig = "/etc/" FS_NAME_STR "/config.yaml";

// How long stage 2 waits on the daemon, and how often it looks.
constexpr std::chrono::milliseconds DaemonWait{5000};
constexpr std::chrono::milliseconds DaemonPoll{50};

// ── small types ─────────────────────────────────────────────────────────

struct Invocation_t
{
    std::string device;
    std::string target;
    std::string options;       // what reaches mount(2)
    std::string daemonConfig;  // daemon_config=, which never does
};

// ── environment and arguments ───────────────────────────────────────────

void printUsage()
{
    std::fprintf(stderr, "usage: " FS_PROGRAM_NAME " <device> <dir> [-sfnv] [-o options]\n");
}

// Comma-separated, the way mount(8) hands them over.
[[nodiscard]] std::vector<std::string_view> splitOptions(std::string_view options)
{
    std::vector<std::string_view> listed;

    while (!options.empty())
    {
        const std::uint64_t comma = options.find(',');
        if (comma == std::string_view::npos)
        {
            listed.push_back(options);
            break;
        }
        listed.push_back(options.substr(0, comma));
        options.remove_prefix(comma + 1);
    }
    return listed;
}

[[nodiscard]] bool hasPrefix(std::string_view text, std::string_view prefix)
{
    return text.substr(0, prefix.size()) == prefix;
}

// Options travel as one comma-separated string, so every one but the first has a comma before it.
void appendOption(std::string& joined, std::string_view option)
{
    if (!joined.empty())
    {
        joined += ",";
    }
    joined += option;
}

// mount(8) passes its own flags before and after the two positional arguments,
// and the ones it offers a helper carry no argument except -o.
[[nodiscard]] std::optional<Invocation_t> parseInvocation(std::int32_t argc, char** argv)
{
    Invocation_t parsed{};
    std::vector<std::string_view> positional;

    for (std::int32_t index = 1; index < argc; ++index)
    {
        const std::string_view argument{argv[index]};

        if (argument == "-o")
        {
            if (index + 1 >= argc)
            {
                return std::nullopt;
            }
            for (const auto option : splitOptions(argv[++index]))
            {
                if (option.size() > DaemonConfigOption.size() && hasPrefix(option, DaemonConfigOption))
                {
                    parsed.daemonConfig = option.substr(DaemonConfigOption.size());
                    continue;
                }
                appendOption(parsed.options, option);
            }
            continue;
        }
        // -s sloppy, -f fake, -n no mtab, -v verbose: nothing here acts on them.
        if (fstools::isFlagArgument(argument))
        {
            continue;
        }
        positional.push_back(argument);
    }

    if (positional.size() != 2)
    {
        return std::nullopt;
    }
    parsed.device = positional[0];
    parsed.target = positional[1];
    return parsed;
}

// ── splitting what mount(8) hands over ──────────────────────────────────

// What an option that is a mount(2) flag does to the flag word. Passing such an option through as
// data reaches super_parse_options, which answers -EINVAL on any token it does not know, so the
// split below is what lets this helper be reached from fstab and from systemd.
struct FlagEffect_t
{
    // Fixed width, though mount(2) takes unsigned long: the two are the same on this target, so
    // the call needs no cast and the field states its size.
    std::uint64_t set;    // bits this option turns on
    std::uint64_t clear;  // bits it turns off, for the pairs that mean the opposite
};

// A lookup rather than a scan, so adding an option costs a row and no reader has to believe the
// list is short enough to walk.
[[nodiscard]] const std::unordered_map<std::string_view, FlagEffect_t>& getFlagOptions()
{
    static const std::unordered_map<std::string_view, FlagEffect_t> ByWord{
        {"ro", {MS_RDONLY, 0}},
        {"rw", {0, MS_RDONLY}},
        {"suid", {0, MS_NOSUID}},
        {"nosuid", {MS_NOSUID, 0}},
        {"dev", {0, MS_NODEV}},
        {"nodev", {MS_NODEV, 0}},
        {"exec", {0, MS_NOEXEC}},
        {"noexec", {MS_NOEXEC, 0}},
        {"sync", {MS_SYNCHRONOUS, 0}},
        {"async", {0, MS_SYNCHRONOUS}},
        {"dirsync", {MS_DIRSYNC, 0}},
        {"mand", {MS_MANDLOCK, 0}},
        {"nomand", {0, MS_MANDLOCK}},
        {"atime", {0, MS_NOATIME}},
        {"noatime", {MS_NOATIME, 0}},
        {"diratime", {0, MS_NODIRATIME}},
        {"nodiratime", {MS_NODIRATIME, 0}},
        {"relatime", {MS_RELATIME, 0}},
        {"norelatime", {0, MS_RELATIME}},
        {"strictatime", {MS_STRICTATIME, 0}},
        {"lazytime", {MS_LAZYTIME, 0}},
        {"nolazytime", {0, MS_LAZYTIME}},
        {"silent", {MS_SILENT, 0}},
        {"loud", {0, MS_SILENT}},
    };
    return ByWord;
}

[[nodiscard]] bool isUserspaceOption(std::string_view option)
{
    // Options mount(8) and systemd keep for themselves. Neither the kernel nor this helper acts on
    // them, and forwarding one would fail the mount for a word the caller never meant for it.
    static const std::unordered_set<std::string_view> Theirs{
        "defaults",
        "auto",
        "noauto",
        "user",
        "nouser",
        "users",
        "owner",
        "group",
        "nofail",
        "_netdev",
    };
    if (Theirs.count(option) != 0)
    {
        return true;
    }
    // x-* and comment= are reserved for the caller by convention, so anything under them is not
    // this filesystem's to read either.
    return hasPrefix(option, "x-") || hasPrefix(option, "comment=");
}

struct SplitOptions_t
{
    std::uint64_t flags{0};
    std::string data;  // what reaches super_parse_options
};

[[nodiscard]] SplitOptions_t splitFlagsAndData(std::string_view options)
{
    SplitOptions_t split{};
    const auto& flagOptions = getFlagOptions();

    for (const auto option : splitOptions(options))
    {
        if (option.empty() || isUserspaceOption(option))
        {
            continue;
        }

        if (const auto known = flagOptions.find(option); known != flagOptions.end())
        {
            split.flags |= known->second.set;
            split.flags &= ~known->second.clear;
            continue;
        }

        appendOption(split.data, option);
    }
    return split;
}

// ── the two commands stage 2 runs ───────────────────────────────────────

// hello_done is the helper's own answer that it opened the lock region, put the metadata domain
// in it and then greeted the kernel: its start does all three before HELLO goes out.
[[nodiscard]] bool isHelperServing(std::uint32_t nodeId)
{
    std::ifstream state{fstools::nameNodeSysfsPath(nodeId, "daemon_state")};
    std::string line;
    return std::getline(state, line) && line.find("hello_done=1") != std::string::npos;
}

// Bring this node's helper up unless it already serves, then wait for it to greet the kernel. A
// start that never greets is taken back here, so the caller has nothing to undo.
[[nodiscard]] bool startHelper(std::uint32_t nodeId)
{
    if (isHelperServing(nodeId))
    {
        return true;
    }

    const auto startCommand =
        fstools::readEnv(FS_NAME_UPPER_STR "_DAEMON_START", "systemctl start " + fstools::nameHelperUnit(nodeId));
    if (const auto status = fstools::runCommand(startCommand); !status.has_value() || *status != 0)
    {
        std::fprintf(stderr, FS_PROGRAM_NAME ": `%s` did not start\n", startCommand.c_str());
        return false;
    }

    const auto until = std::chrono::steady_clock::now() + DaemonWait;
    while (std::chrono::steady_clock::now() < until)
    {
        if (isHelperServing(nodeId))
        {
            return true;
        }
        std::this_thread::sleep_for(DaemonPoll);
    }

    std::fprintf(stderr, FS_PROGRAM_NAME ": %s did not greet the kernel within %" PRId64 " ms\n",
                 fstools::nameHelperUnit(nodeId).c_str(), static_cast<std::int64_t>(DaemonWait.count()));
    static_cast<void>(fstools::runCommand(fstools::readStopCommand(nodeId)));
    return false;
}

// The account mount(2) closes the lock region to, which is therefore the only account that may
// write the region afterwards. The config at @configPath is owned by that account, and the daemon
// refuses to serve as anyone else, so the owner is the one answer both sides read.
[[nodiscard]] std::optional<fstools::DaemonAccount_t> resolveDaemonAccount(const std::string& configPath)
{
    struct ::stat info  // NOLINT(misc-include-cleaner) <sys/stat.h> above declares it
    {
    };
    if (::stat(configPath.c_str(), &info) != 0)
    {
        std::fprintf(stderr, FS_PROGRAM_NAME ": %s: %s\n", configPath.c_str(), std::strerror(errno));
        return std::nullopt;
    }
    if (info.st_uid == 0)
    {
        std::fprintf(stderr, FS_PROGRAM_NAME ": %s is owned by root, not by the daemon account\n",
                     configPath.c_str());
        return std::nullopt;
    }
    const auto* const found = ::getpwuid(info.st_uid);
    if (found == nullptr)
    {
        std::fprintf(stderr, FS_PROGRAM_NAME ": %s is owned by uid %u, which names no account\n",
                     configPath.c_str(), info.st_uid);
        return std::nullopt;
    }

    return fstools::DaemonAccount_t{found->pw_uid, found->pw_gid, found->pw_name};
}

// The daemon_uid=/daemon_gid= tokens that close the lock region to @account. uid alone, not its
// group: a second account may reach the helper without reaching the region behind it.
[[nodiscard]] std::string nameDaemonAccountOptions(const fstools::DaemonAccount_t& account)
{
    return std::string{"daemon_uid="}
        .append(std::to_string(account.uid))
        .append(",daemon_gid=")
        .append(std::to_string(FS_TOOLS_ANY_ID));
}

}  // namespace

int main(int argc, char** argv)
{
    const auto parsed = parseInvocation(argc, argv);
    if (!parsed.has_value())
    {
        printUsage();
        return 1;
    }

    // mount(8) hands a helper the VFS flags and the filesystem's own options in one string, and
    // the module refuses any token it does not know, so the two have to be told apart here.
    auto split = splitFlagsAndData(parsed->options);

    // The kernel binds the lock region to this node's daemon account from inside mount(2) itself,
    // so the account has to travel with the mount rather than through a later call.
    const auto configPath = parsed->daemonConfig.empty() ? std::string{DefaultDaemonConfig} : parsed->daemonConfig;
    const auto daemonAccount = resolveDaemonAccount(configPath);
    if (!daemonAccount.has_value())
    {
        // Mounting without it would leave the region open to every account on this node.
        return fstools::HelperFailure;
    }
    appendOption(split.data, nameDaemonAccountOptions(*daemonAccount));

    if (::mount(parsed->device.c_str(), parsed->target.c_str(), FsType, split.flags, split.data.c_str()) != 0)
    {
        // The string too, because the failure is usually a token rather than the mount point, and
        // the kernel's own reason goes to dmesg where the caller is not looking.
        std::fprintf(stderr,
                     FS_PROGRAM_NAME ": mount %s: %s (flags 0x%" PRIx64 ", options '%s')\n",
                     parsed->target.c_str(), std::strerror(errno), split.flags,
                     split.data.c_str());
        return fstools::HelperFailure;
    }

    // The kernel picked this node's identity inside mount(2), and everything below is named
    // after it: the channel, the unit, and the sysfs file that says whether it is attended.
    const auto nodeId = fstools::readNodeId(parsed->target);

    // mount(2) above bound the lock region to the daemon account, so whatever touches the region
    // from here on has to be that account. Two steps remain.
    //
    //   format   brings this node's region content up to date, idempotent on every mount, and run
    //            as the daemon account because the binding is what admits the write
    //   helper   started through systemctl, which the unit's own User= puts on that account, and
    //            maps the region to put the metadata domain in it before it greets the kernel
    if (nodeId.has_value())
    {
        const auto formatArgs = fstools::regionFormatArgs(configPath, parsed->target, daemonAccount);
        if (formatArgs.has_value() && fstools::formatLockRegion(*formatArgs, *daemonAccount) && startHelper(*nodeId))
        {
            return 0;
        }
    }
    else
    {
        std::fprintf(stderr, FS_PROGRAM_NAME ": %s reports no node id\n", parsed->target.c_str());
    }

    // Leave nothing behind: the caller asked for a mount, not for a stage of one. Nothing to take
    // down first, since startHelper undoes its own start.
    if (::umount(parsed->target.c_str()) != 0)
    {
        std::fprintf(stderr, FS_PROGRAM_NAME ": umount %s after failure: %s\n",
                     parsed->target.c_str(), std::strerror(errno));
    }
    return fstools::HelperFailure;
}
