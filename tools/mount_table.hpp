// SPDX-License-Identifier: Apache-2.0
//
// mount_table.hpp - what the kernel's own mount table says about one mount of this filesystem.
//
// Both helpers name the daemon after the node id, and the kernel picks that id inside mount(2), so
// neither can know it any other way than by reading it back.

#pragma once

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <ios>
#include <istream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include "name.h"

namespace fstools
{

// The node id the mount at @target claimed in the mount table @mounts, or nothing when there is no
// such mount or its claim did not land. show_options prints the live value, and ids start at 1.
[[nodiscard]] inline std::optional<std::uint32_t> findNodeId(std::istream& mounts, std::string_view target)
{
    std::string device;
    std::string where;
    std::string type;
    std::string options;

    while (mounts >> device >> where >> type >> options)
    {
        mounts.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        if (where != target || type != FS_NAME_STR)
        {
            continue;
        }

        constexpr std::string_view NodeOption{"node_id="};
        std::uint64_t found = 0;
        while ((found = options.find(NodeOption, found)) != std::string::npos)
        {
            if (found == 0 || options[found - 1] == ',')
            {
                const auto claimed = static_cast<std::uint32_t>(
                    std::strtoul(options.c_str() + found + NodeOption.size(), nullptr, 10));
                if (claimed == 0)
                {
                    return std::nullopt;
                }
                return claimed;
            }
            found += NodeOption.size();
        }
    }
    return std::nullopt;
}

// The same, read off the kernel's own table.
[[nodiscard]] inline std::optional<std::uint32_t> readNodeId(std::string_view target)
{
    std::ifstream mounts{"/proc/mounts"};
    return findNodeId(mounts, target);
}

}  // namespace fstools
