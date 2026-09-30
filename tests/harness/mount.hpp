// SPDX-License-Identifier: Apache-2.0
//
// mount.hpp -- one mount, and files named relative to it.
//
// The library takes a path, and every case here names a file relative to the mount it was pointed
// at, so the joining lives once instead of at each of the several hundred call sites.

#pragma once

#include <sys/types.h>

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "fs/file.hpp"

namespace fsuser::tests
{

class Mount
{
public:
    explicit Mount(std::string point)
        : point_{std::move(point)}
    {
    }

    // ── public methods ─────────────────────────────────────────────
    [[nodiscard]] std::string pathTo(std::string_view name) const
    {
        auto built = point_;
        if (!built.empty() && built.back() != '/')
        {
            built += '/';
        }
        built.append(name);
        return built;
    }

    [[nodiscard]] File open(std::string_view name, std::int32_t flags, ::mode_t mode = 0) const
    {
        return File::open(pathTo(name), flags, mode);
    }

    void unlink(std::string_view name) const
    {
        unlinkFile(pathTo(name));
    }

    // ── accessors ──────────────────────────────────────────────────
    [[nodiscard]] const std::string& getPoint() const noexcept
    {
        return point_;
    }

    // The node id the kernel gave this mount, read off the mount table, or 0 when nothing is
    // mounted here. The kernel picks it inside mount(2), so the mount point does not imply it.
    [[nodiscard]] std::uint32_t getNodeId() const
    {
        std::ifstream table{"/proc/self/mounts"};
        std::string line;
        while (std::getline(table, line))
        {
            std::istringstream fields{line};
            std::string device;
            std::string point;
            std::string type;
            std::string options;
            if (!(fields >> device >> point >> type >> options) || point != point_)
            {
                continue;
            }
            std::istringstream each{options};
            std::string option;
            while (std::getline(each, option, ','))
            {
                if (option.rfind("node_id=", 0) == 0)
                {
                    return static_cast<std::uint32_t>(std::stoul(option.substr(8)));
                }
            }
        }
        return 0;
    }

private:
    std::string point_;
};

}  // namespace fsuser::tests
