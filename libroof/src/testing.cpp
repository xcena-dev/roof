// SPDX-License-Identifier: Apache-2.0
//
// testing.cpp -- see testing.hpp. Built only under FS_TESTING and never installed.

#include "fs/testing.hpp"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/types.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace fsuser::testing
{

namespace
{

// 'X' is this filesystem's ioctl letter and 99 is outside the numbers it defines, so the driver reaches its
// default arm rather than any verb.
constexpr std::uint64_t UnknownRequest = _IOW('X', 99, std::int32_t);

[[nodiscard]] std::string joinPath(std::string_view mountPoint, std::string_view name)
{
    std::string joined{mountPoint};
    if (!joined.empty() && joined.back() != '/')
    {
        joined.push_back('/');
    }
    joined.append(name);
    return joined;
}

}  // namespace

std::int32_t openRaw(std::string_view mountPoint, std::string_view name, std::int32_t flags, ::mode_t mode)
{
    const auto path = joinPath(mountPoint, name);
    return ::open(path.c_str(), flags, mode);
}

std::int32_t sendUnknownIoctl(std::int32_t file)
{
    // The driver reaches its default arm before it reads this, so its value never matters.
    std::int32_t argument = 0;
    return ::ioctl(file, UnknownRequest, &argument);
}

}  // namespace fsuser::testing
