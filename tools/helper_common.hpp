// SPDX-License-Identifier: Apache-2.0
//
// helper_common.hpp - what the mount helper and the umount helper both need.
//
// Each is its own program and they share no runtime state, but mount(8) and umount(8) hand them the
// same kind of thing: an environment override for a command, a command to run, and the names one
// node's helper goes by. One definition of each keeps the two programs in agreement.

#pragma once

#include <sys/wait.h>

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

#include "name.h"

namespace fstools
{

// mount(8) and umount(8) both reserve this for an operation that did not happen.
inline constexpr std::int32_t HelperFailure = 32;

// The value $@name holds, or @fallback when it is unset. A deployment overrides how a tool is
// invoked this way, which is not the same as configuring the filesystem.
[[nodiscard]] inline std::string readEnv(const char* name, std::string_view fallback)
{
    const char* const value = ::getenv(name);
    return (value != nullptr) ? std::string{value} : std::string{fallback};
}

// The exit status of @command, or nullopt for one that never ran to completion.
[[nodiscard]] inline std::optional<std::int32_t> runCommand(const std::string& command)
{
    const std::int32_t status = std::system(command.c_str());
    if (status == -1 || !WIFEXITED(status))
    {
        return std::nullopt;
    }
    return WEXITSTATUS(status);
}

// The systemd instance attending node @nodeId.
[[nodiscard]] inline std::string nameHelperUnit(std::uint32_t nodeId)
{
    return std::string{DAEMON_NAME_STR "@"}.append(std::to_string(nodeId)).append(".service");
}

// How node @nodeId's helper goes down: the deployment's override, else systemctl.
[[nodiscard]] inline std::string readStopCommand(std::uint32_t nodeId)
{
    return readEnv(FS_NAME_UPPER_STR "_DAEMON_STOP", "systemctl stop " + nameHelperUnit(nodeId));
}

// The file @file in the sysfs directory the kernel keeps for node @nodeId.
[[nodiscard]] inline std::string nameNodeSysfsPath(std::uint32_t nodeId, std::string_view file)
{
    return std::string{"/sys/fs/" FS_NAME_STR "/node"}.append(std::to_string(nodeId)).append("/").append(file);
}

// A flag such as -f or -v, which mount(8) and umount(8) pass around the positional arguments. A lone
// - is positional.
[[nodiscard]] inline bool isFlagArgument(std::string_view argument)
{
    return argument.size() > 1 && argument.front() == '-';
}

}  // namespace fstools
