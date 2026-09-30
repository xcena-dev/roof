// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// identity/base.cpp -- see base.hpp.

#include "identity/base.hpp"

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

#include "identity/internal/selector.hpp"
#include "model.hpp"
#include "posix/unique_fd.hpp"
#include "util/text.hpp"

namespace fsdaemon::identity
{

namespace
{

// The whole of @name opened relative to @dirfd, or nullopt when the open or a read fails. A task's
// /proc file has no size a single read can assume, so this reads to end of file.
std::optional<std::string> readWholeFile(posix::Descriptor dirfd, const char* name)
{
    const posix::UniqueFd file{::openat(dirfd, name, O_RDONLY | O_CLOEXEC)};
    if (!file)
    {
        return std::nullopt;
    }
    std::string body;
    // One page, which holds a task's status with room over. Only a loop bound, not a limit: the
    // loop below reads to end of file, so a longer one costs another read rather than being cut.
    constexpr std::uint64_t ChunkBytes = 4ULL * 1024;
    std::array<char, ChunkBytes> chunk{};
    for (;;)
    {
        const std::int64_t got = ::read(file.get(), chunk.data(), chunk.size());
        if (got < 0)
        {
            return std::nullopt;
        }
        if (got == 0)
        {
            return body;
        }
        body.append(chunk.data(), static_cast<std::size_t>(got));
    }
}

// The separators between the fields of a /proc line.
constexpr std::string_view Blanks = " \t";

// Every gid on the "Groups:" line of a status body.
std::unordered_set<std::uint32_t> parseGroupsLine(std::string_view status)
{
    constexpr std::string_view GroupsKey = "Groups:";

    const std::uint64_t keyAt = status.find(GroupsKey);
    if (keyAt == std::string_view::npos)
    {
        return {};
    }
    auto row = status.substr(keyAt + GroupsKey.size());
    // The length is bounded rather than left as npos, so an optimizer inlining this sees a
    // memchr bound it can prove.
    const std::uint64_t lineEnd = row.find('\n');
    row = row.substr(0, lineEnd == std::string_view::npos ? row.size() : lineEnd);

    std::unordered_set<std::uint32_t> groups;
    for (const auto field : util::splitFields(row, Blanks))
    {
        std::uint32_t value = 0;
        if (std::from_chars(field.data(), field.data() + field.size(), value).ec == std::errc{})
        {
            groups.insert(value);
        }
    }
    return groups;
}

// The start time on a task's stat line, in clock ticks since boot. Counted from the last ')',
// because the comm between the parentheses is free to hold a space and a parenthesis of its own.
std::optional<std::uint64_t> parseStartTicks(std::string_view stat)
{
    const std::uint64_t commEnd = stat.rfind(')');
    if (commEnd == std::string_view::npos)
    {
        return std::nullopt;
    }

    const auto fields = util::splitFields(stat.substr(commEnd + 1), Blanks);
    // stat's 22nd field, which is the 20th of those that follow the comm.
    constexpr std::uint64_t StartTimePosition = 20;
    if (fields.size() < StartTimePosition)
    {
        return std::nullopt;
    }

    const auto field = fields[StartTimePosition - 1];
    std::uint64_t ticks = 0;
    if (std::from_chars(field.data(), field.data() + field.size(), ticks).ec != std::errc{})
    {
        return std::nullopt;
    }
    return ticks;
}

// Whether @stat belongs to the task whose start time the upcall carried as @wireNs. /proc counts
// in clock ticks, so the comparison is in ticks and forgives the one the rounding can move it by.
bool isSameStartTime(std::string_view stat, std::uint64_t wireNs)
{
    const std::int64_t perSecond = ::sysconf(_SC_CLK_TCK);
    const auto found = parseStartTicks(stat);
    if (perSecond <= 0 || !found)
    {
        return false;
    }

    constexpr std::uint64_t NanosPerSecond = 1000000000;
    const auto wireTicks = wireNs / (NanosPerSecond / static_cast<std::uint64_t>(perSecond));
    return (wireTicks > *found ? wireTicks - *found : *found - wireTicks) <= 1;
}

}  // namespace

IdentityProvider::IdentityProvider(const std::vector<std::string>& selectorNames)
    : selectors_{std::make_unique<Selector>(selectorNames)}
{
}

IdentityProvider::~IdentityProvider() = default;

const Selector& IdentityProvider::getSelectors() const noexcept
{
    return *selectors_;
}

std::optional<Creds_t> IdentityProvider::resolveCreds(pid_t pid, const Creds_t& facts) const
{
    if (!selectors_->isSupported(Selector::SupplementaryGid))
    {
        return facts;
    }

    // An O_PATH dirfd pins the task, so a pid reused after it opens cannot redirect the read
    // through it.
    const auto procPath = "/proc/" + std::to_string(pid);
    const posix::UniqueFd taskDir{::open(procPath.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC)};
    if (!taskDir)
    {
        return std::nullopt;
    }

    // The pid alone names whoever holds it now. Both reads go through the one dirfd, so the start
    // time that admits this task is the start time of the task the groups are read from.
    const auto stat = readWholeFile(taskDir.get(), "stat");
    if (!stat || !isSameStartTime(*stat, facts.startBoottimeNs))
    {
        return std::nullopt;
    }

    const auto status = readWholeFile(taskDir.get(), "status");
    if (!status)
    {
        return std::nullopt;
    }

    auto creds = facts;
    creds.supplementaryGids = parseGroupsLine(*status);
    return creds;
}

}  // namespace fsdaemon::identity
