// SPDX-License-Identifier: Apache-2.0
//
// daemon_command.hpp - asking the daemon what a region needs, and running a command as its
// account, both by execvp and never by a shell.
//
// The mount helper and a test double share this: neither hands a joined string to /bin/sh, so a
// character in a path or in the daemon's own answer is a character and never a command separator.

#pragma once

#include <grp.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "helper_common.hpp"
#include "name.h"

namespace fstools
{

// What a child reports when it never reached the program it was forked for. The shell's own value
// for the same thing, so it stays distinct from anything cme-format itself exits with.
inline constexpr std::int32_t CommandNotRun = 126;

// cme-format's answer for a region that is already formatted, which is the state
// every joiner and every remount is in.
inline constexpr std::int32_t FormatAlreadyLive = 3;

// mount(8) clears the environment before it runs a type helper, and execvp with no PATH searches
// only /bin and /usr/bin, which is not where either program this helper runs is installed.
inline constexpr const char* FallbackSearchPath = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";

// This node's daemon account, resolved from its passwd entry.
struct DaemonAccount_t
{
    ::uid_t uid;
    ::gid_t gid;
    std::string name;
};

// Which groups a child moved onto the account carries besides the account's own.
enum class Groups : std::uint8_t
{
    // None: what the child execs needs the account's uid and nothing more.
    OwnOnly,
    // The account's supplementary set, the one systemd's User= gives the daemon itself.
    Membership,
};

// The words of @line, split on whitespace and nothing else. What the daemon prints is an argument
// list, and a shell would also expand globs and quotes in it.
[[nodiscard]] inline std::vector<std::string> splitWords(std::string_view line)
{
    constexpr std::string_view Whitespace{" \t\n\v\f\r"};

    std::vector<std::string> words;
    for (;;)
    {
        // find_first_not_of answers npos when only whitespace is left, which remove_prefix cannot take.
        line.remove_prefix(std::min(line.find_first_not_of(Whitespace), line.size()));
        if (line.empty())
        {
            return words;
        }
        words.emplace_back(line.substr(0, line.find_first_of(Whitespace)));
        line.remove_prefix(words.back().size());
    }
}

// Fill in the fallback only when the child inherited no PATH of its own, since a PATH an
// administrator set is the one that decides which binary a name resolves to.
inline void fillSearchPath()
{
    const auto* const inherited = ::getenv("PATH");
    if (inherited != nullptr && inherited[0] != '\0')
    {
        return;
    }
    static_cast<void>(::setenv("PATH", FallbackSearchPath, 1));
}

// The child side of a fork, ended by the exec. An argument reaches the program as it is and never
// through a shell, so a character in one is a character and not a command separator.
[[noreturn]] inline void execArgv(const std::vector<std::string>& argv)
{
    fillSearchPath();
    std::vector<char*> raw;
    raw.reserve(argv.size() + 1);
    for (const std::string& word : argv)
    {
        raw.push_back(const_cast<char*>(word.c_str()));
    }
    raw.push_back(nullptr);
    ::execvp(raw[0], raw.data());
    std::fprintf(stderr, FS_PROGRAM_NAME ": exec %s: %s\n", raw[0], std::strerror(errno));
    ::_exit(CommandNotRun);
}

// The child side of a fork, moved onto @account or ended. Both drops are irreversible, so what the
// child execs cannot take the privilege back.
inline void dropToAccount(const DaemonAccount_t& account, Groups groups)
{
    const bool grouped = (groups == Groups::Membership)
                             ? ::initgroups(account.name.c_str(), account.gid) == 0
                             : ::setgroups(1, &account.gid) == 0;
    if (!grouped ||
        ::setresgid(account.gid, account.gid, account.gid) != 0 ||
        ::setresuid(account.uid, account.uid, account.uid) != 0)
    {
        std::fprintf(stderr, FS_PROGRAM_NAME ": cannot run as uid %u: %s\n",
                     static_cast<std::uint32_t>(account.uid), std::strerror(errno));
        ::_exit(CommandNotRun);
    }
}

// @argv as @account, or as this helper when there is none, with its first line of output returned.
[[nodiscard]] inline std::optional<std::string>
readFirstLine(const std::vector<std::string>& argv,
              const std::optional<DaemonAccount_t>& account)
{
    std::array<std::int32_t, 2> ends{};
    if (::pipe(ends.data()) != 0)
    {
        std::fprintf(stderr, FS_PROGRAM_NAME ": pipe: %s\n", std::strerror(errno));
        return std::nullopt;
    }
    const auto readEnd = ends[0];
    const auto writeEnd = ends[1];

    const auto child = ::fork();
    if (child < 0)
    {
        std::fprintf(stderr, FS_PROGRAM_NAME ": fork: %s\n", std::strerror(errno));
        ::close(readEnd);
        ::close(writeEnd);
        return std::nullopt;
    }

    if (child == 0)
    {
        ::close(readEnd);
        if (::dup2(writeEnd, STDOUT_FILENO) < 0)
        {
            ::_exit(CommandNotRun);
        }
        ::close(writeEnd);
        if (account.has_value())
        {
            // The daemon reads its config through any group systemd gives it, so this query does too.
            dropToAccount(*account, Groups::Membership);
        }
        execArgv(argv);
    }

    ::close(writeEnd);
    std::string line;
    constexpr std::uint32_t ChunkBytes = 512;
    // Read past the first line to EOF, so a child that prints more is not killed by SIGPIPE.
    // The cap bounds how long a runaway child can keep this helper reading.
    constexpr std::uint64_t DrainLimitBytes = std::uint64_t{64} * 1024;
    std::uint64_t drained = 0;
    std::array<char, ChunkBytes> buffer{};
    while (drained < DrainLimitBytes)
    {
        const std::int64_t got = ::read(readEnd, buffer.data(), buffer.size());
        if (got <= 0)
        {
            break;
        }
        drained += static_cast<std::uint64_t>(got);
        if (line.find('\n') == std::string::npos)
        {
            line.append(buffer.data(), static_cast<std::uint64_t>(got));
        }
    }
    ::close(readEnd);

    // No '\n' is left after the cut, so only the '\r' of a CRLF can trail.
    line = line.substr(0, line.find('\n'));
    while (!line.empty() && line.back() == '\r')
    {
        line.pop_back();
    }

    // An empty first line is no answer either, since the arguments it would hand on are none.
    std::int32_t status = 0;
    const auto waited = ::waitpid(child, &status, 0) == child;
    if (!waited || !WIFEXITED(status) || WEXITSTATUS(status) != 0 || line.empty())
    {
        std::fprintf(stderr, FS_PROGRAM_NAME ": `%s` said nothing about the region\n",
                     argv[0].c_str());
        return std::nullopt;
    }
    return line;
}

// What cme-format needs for this mount's region. This helper cannot read the daemon's YAML, so it
// asks the daemon as @account, since the daemon trusts a config that only its own account or root owns.
[[nodiscard]] inline std::optional<std::string>
regionFormatArgs(std::string_view configPath,
                 std::string_view target,
                 const std::optional<DaemonAccount_t>& account)
{
    const std::vector<std::string> argv{
        fstools::readEnv(FS_NAME_UPPER_STR "_DAEMON", DAEMON_NAME_STR),
        "--config", std::string{configPath}, "--print-region-format", std::string{target}};

    return readFirstLine(argv, account);
}

// @command as @account rather than as this helper, which mount(8) runs privileged.
[[nodiscard]] inline std::optional<std::int32_t> runCommandAs(const std::vector<std::string>& argv,
                                                              const DaemonAccount_t& account)
{
    const auto child = ::fork();
    if (child < 0)
    {
        std::fprintf(stderr, FS_PROGRAM_NAME ": fork: %s\n", std::strerror(errno));
        return std::nullopt;
    }

    if (child == 0)
    {
        // The lock region admits the account by uid, and a group the formatter does not need
        // is a group it must not carry into the exec.
        dropToAccount(account, Groups::OwnOnly);
        execArgv(argv);
    }

    std::int32_t status = 0;
    if (::waitpid(child, &status, 0) != child || !WIFEXITED(status))
    {
        return std::nullopt;
    }
    return WEXITSTATUS(status);
}

// Format the region, unless it already answers. As the daemon account and not as the helper's own
// root: mount(2) above closed the region to that account, and root holds no row to fall back on.
[[nodiscard]] inline bool formatLockRegion(std::string_view formatArgs, const DaemonAccount_t& account)
{
    std::vector<std::string> argv{fstools::readEnv(FS_NAME_UPPER_STR "_CME_FORMAT", "cme-format")};
    for (std::string& word : splitWords(formatArgs))
    {
        argv.push_back(std::move(word));
    }

    const auto status = runCommandAs(argv, account);
    if (status == 0 || status == FormatAlreadyLive)
    {
        return true;
    }
    if (!status.has_value() || status == CommandNotRun)
    {
        std::fprintf(stderr, FS_PROGRAM_NAME ": cme-format did not run\n");
        return false;
    }
    std::fprintf(stderr, FS_PROGRAM_NAME ": cme-format exited %d\n", *status);
    return false;
}

}  // namespace fstools
