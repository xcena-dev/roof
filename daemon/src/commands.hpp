// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// daemon/commands.hpp -- what the command line asks the daemon for besides serving.
//
// A one-shot command reads the config, prints and exits, so main serves only when none is named.

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace fsdaemon
{

// The exit status of the one-shot command @args name, or nullopt when they name none.
[[nodiscard]] std::optional<std::int32_t> runCommand(const std::vector<std::string_view>& args);

// The node --node-id names, or nullopt when the flag is absent. Throws std::runtime_error on a value
// that is not a node id, since a silent fallback would serve the wrong channel.
[[nodiscard]] std::optional<std::uint32_t> readNodeId(const std::vector<std::string_view>& args);

}  // namespace fsdaemon
