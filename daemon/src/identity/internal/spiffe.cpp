// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// identity/internal/spiffe.cpp -- see internal/spiffe.hpp.

#include "identity/internal/spiffe.hpp"

#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "identity/base.hpp"
#include "util/text.hpp"

namespace fsdaemon::identity
{

namespace
{

constexpr std::string_view SpiffePrefix = "spiffe://";

}  // namespace

std::pair<std::string, std::string> parseSpiffePath(std::string_view spiffeId)
{
    if (spiffeId.compare(0, SpiffePrefix.size(), SpiffePrefix) != 0)
    {
        return {"", ""};
    }
    const auto rest = spiffeId.substr(SpiffePrefix.size());
    constexpr std::string_view PathSeparator = "/";
    const std::uint64_t slash = rest.find(PathSeparator);
    if (slash == std::string_view::npos)
    {
        return {"", ""};
    }

    const auto segments = util::splitFields(rest.substr(slash), PathSeparator);
    constexpr std::string_view GroupKey = "group";
    constexpr std::string_view RoleKey = "role";
    // A key that appears twice is an id nobody issued, so it resolves to nothing rather than to
    // whichever value came last.
    std::optional<std::string> group;
    std::optional<std::string> role;
    for (auto key = segments.begin(); key != segments.end(); ++key)
    {
        const auto value = std::next(key);
        if (value == segments.end())
        {
            break;
        }
        if (*key == GroupKey)
        {
            if (group)
            {
                return {"", ""};
            }
            group = std::string{*value};
        }
        else if (*key == RoleKey)
        {
            if (role)
            {
                return {"", ""};
            }
            role = std::string{*value};
        }
        else
        {
            continue;
        }
        // A matched key consumed its value, so the next key starts past it.
        key = value;
    }
    return {group.value_or(""), role.value_or("")};
}

std::string assembleSpiffeId(std::string_view trustDomain, std::string_view path)
{
    if (trustDomain.empty())
    {
        throw BridgeError{"empty trust_domain"};
    }
    std::string assembled{SpiffePrefix};
    assembled += trustDomain;
    if (!path.empty())
    {
        if (path.front() != '/')
        {
            assembled += '/';
        }
        assembled += path;
    }
    return assembled;
}
}  // namespace fsdaemon::identity
