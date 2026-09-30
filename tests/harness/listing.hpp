// SPDX-License-Identifier: Apache-2.0
//
// listing.hpp -- what a mount's directory holds, narrowed to one case's own names.
//
// A mount is shared ground. The daemon's control region sits in the same directory, and so does
// anything another case is holding at that moment. So a case counts only the names it made itself,
// and what it compares is a set rather than a total.

#pragma once

#include <filesystem>
#include <set>
#include <string>
#include <system_error>
#include <utility>

namespace fsuser::tests
{

// Every name in @mount that begins with @prefix. An unreadable directory answers the same empty set
// as an empty one, so a caller that needs to tell those apart asks the mount something else too.
[[nodiscard]] inline std::set<std::string> listNames(const std::string& mount,
                                                     const std::string& prefix)
{
    std::set<std::string> found;
    // The error_code form, so a mount that went away mid-run reads as an empty listing rather than
    // throwing through a case that was asking about something else.
    std::error_code failed;
    for (const auto& entry : std::filesystem::directory_iterator{mount, failed})
    {
        auto named = entry.path().filename().string();
        if (named.rfind(prefix, 0) == 0)
        {
            found.insert(std::move(named));
        }
    }
    return found;
}

// Whether @mount resolves @name at all. A lookup and not the library's open, because what is being
// asked is the lookup and an open would also take a descriptor.
[[nodiscard]] inline bool nameExists(const std::string& mount, const std::string& name)
{
    std::error_code failed;
    return std::filesystem::exists(std::filesystem::path{mount} / name, failed);
}

}  // namespace fsuser::tests
