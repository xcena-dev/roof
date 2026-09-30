// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// config/internal/mount_table.cpp -- see mount_table.hpp.

#include "config/internal/mount_table.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include "config/config.hpp"
#include "name.hpp"
#include "util/text.hpp"

namespace fsdaemon::config::internal
{

namespace
{

// Whether the comma separated @options carry @want as a whole item. A substring match would let
// node_id=1 answer for node_id=11.
bool hasOption(std::string_view options, std::string_view want)
{
    const auto items = util::splitFields(options, ",");
    return std::find(items.begin(), items.end(), want) != items.end();
}

}  // namespace

std::string findRegionForNode(std::uint32_t nodeId, const std::string& mountsPath)
{
    // Node 0 is the id a config that named none leaves behind, and no mount claims it.
    if (nodeId == 0)
    {
        return {};
    }

    const auto want = "node_id=" + std::to_string(nodeId);
    std::ifstream mounts{mountsPath};
    for (std::string line; std::getline(mounts, line);)
    {
        std::istringstream fields{line};
        std::string backing;
        std::string target;
        std::string kind;
        std::string options;
        if (!(fields >> backing >> target >> kind >> options))
        {
            continue;
        }
        if (std::string_view{kind} == name::FsName && hasOption(options, want))
        {
            return makeRegionUri(target);
        }
    }
    return {};
}

}  // namespace fsdaemon::config::internal
