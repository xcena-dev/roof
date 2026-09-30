// SPDX-License-Identifier: Apache-2.0
//
// test_mount_helper_argv -- whether the mount helper hands a child its arguments one at a time
// instead of through a shell.
//
// Both regionFormatArgs and formatLockRegion exec a stub in place of the real daemon or
// cme-format, and the stub logs its own argc and argv. A value carrying a semicolon and a space
// stays inert data to that stub, because execvp never hands the joined string to /bin/sh.

#include <stdlib.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "daemon_command.hpp"
#include "name.h"

namespace
{

std::int32_t g_failureCount = 0;

void check(const std::string& label, bool passed)
{
    std::printf("%s: %s\n", passed ? "PASS" : "FAIL", label.c_str());
    if (!passed)
    {
        ++g_failureCount;
    }
}

void skip(const std::string& label, const std::string& reason)
{
    std::printf("SKIP: %s (%s)\n", label.c_str(), reason.c_str());
}

// Every path this run creates, removed once on the way out regardless of which check failed.
class ScratchDir
{
public:
    ScratchDir()
        : path_{std::filesystem::temp_directory_path() /
                ("mount_helper_argv_test_" + std::to_string(::getpid()))}
    {
        std::filesystem::create_directory(path_);
    }

    ~ScratchDir()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] std::string join(const std::string& name) const
    {
        return (path_ / name).string();
    }

private:
    std::filesystem::path path_;
};

// A logging stub in place of the real daemon or cme-format: it records its own argv to @logPath,
// answers with @stdoutLines, and exits with @exitCode.
void writeStub(const std::string& scriptPath, const std::string& logPath,
               const std::vector<std::string>& stdoutLines, std::int32_t exitCode)
{
    std::string body = "#!/usr/bin/env bash\nset -u\n";
    body += R"({ printf 'argc=%d\n' "$#"; for word in "$@"; do printf '%s\n' "$word"; done; } >> ')" +
            logPath + "'\n";
    for (const auto& line : stdoutLines)
    {
        body += "printf '%s\\n' '" + line + "'\n";
    }
    body += "exit " + std::to_string(exitCode) + "\n";

    std::ofstream script{scriptPath};
    script << body;
    script.close();
    std::filesystem::permissions(scriptPath,
                                 std::filesystem::perms::owner_all |
                                     std::filesystem::perms::group_read |
                                     std::filesystem::perms::group_exec);
}

[[nodiscard]] std::vector<std::string> readLines(const std::string& path)
{
    std::vector<std::string> lines;
    std::ifstream stream{path};
    std::string line;
    while (std::getline(stream, line))
    {
        lines.push_back(line);
    }
    return lines;
}

void checkSplitWordsKeepsSemicolonInsideAWord()
{
    const auto words = fstools::splitWords("--region 3 attack;touch /tmp/pwned_split_marker");
    check("splitWords splits only on whitespace",
          words.size() == 4 && words[0] == "--region" && words[1] == "3" &&
              words[2] == "attack;touch" && words[3] == "/tmp/pwned_split_marker");
}

void checkFormatLockRegionArgvArrivesWholeAndNothingRuns(const ScratchDir& scratch)
{
    // runCommandAs calls setgroups() before it execs, and that call needs CAP_SETGID even to set
    // the group this process already carries: there is no same-value exception like setresuid's.
    if (::geteuid() != 0)
    {
        skip("formatLockRegion argv checks", "setgroups() in runCommandAs needs root");
        return;
    }

    const auto logPath = scratch.join("cme_format.log");
    const std::string markerPath = "/tmp/pwned_cme_marker";
    std::filesystem::remove(markerPath);

    writeStub(scratch.join("cme-format-stub"), logPath, {}, 0);
    ::setenv(FS_NAME_UPPER_STR "_CME_FORMAT", scratch.join("cme-format-stub").c_str(), 1);

    const fstools::DaemonAccount_t self{::getuid(), ::getgid(), ""};
    const auto formatted =
        fstools::formatLockRegion("--region 3 --tag attack;touch /tmp/pwned_cme_marker", self);
    check("formatLockRegion reports the stub's own success", formatted);

    const auto logged = readLines(logPath);
    check("cme-format stub saw 5 arguments",
          !logged.empty() && logged[0] == "argc=5");
    check("the semicolon word arrived as one argument",
          logged.size() >= 6 && logged[4] == "attack;touch" && logged[5] == "/tmp/pwned_cme_marker");
    check("no command hid behind the semicolon", !std::filesystem::exists(markerPath));
}

void checkRegionFormatArgsCarriesOneArgumentWithASpaceAndASemicolon(const ScratchDir& scratch)
{
    const auto logPath = scratch.join("daemon_one_line.log");
    const std::string markerPath = "/tmp/pwned_daemon_marker";
    std::filesystem::remove(markerPath);

    writeStub(scratch.join("daemon-one-line-stub"), logPath,
              {"region-format-line", "second-line-should-be-discarded"}, 0);
    ::setenv(FS_NAME_UPPER_STR "_DAEMON", scratch.join("daemon-one-line-stub").c_str(), 1);

    const std::string configPath = "/tmp/region cfg;touch /tmp/pwned_daemon_marker";
    const auto answer = fstools::regionFormatArgs(configPath, "/mnt/example", std::nullopt);
    check("regionFormatArgs keeps only the first line",
          answer.has_value() && *answer == "region-format-line");

    const auto logged = readLines(logPath);
    check("daemon stub saw the 4 words mount_helper builds",
          !logged.empty() && logged[0] == "argc=4");
    check("the config path with a space and a semicolon stayed one argument",
          logged.size() >= 4 && logged[2] == configPath);
    check("no command hid inside the config path", !std::filesystem::exists(markerPath));
}

void checkRegionFormatArgsGivesUpOnANonzeroExit(const ScratchDir& scratch)
{
    const auto logPath = scratch.join("daemon_failing.log");
    writeStub(scratch.join("daemon-failing-stub"), logPath, {"never used"}, 7);
    ::setenv(FS_NAME_UPPER_STR "_DAEMON", scratch.join("daemon-failing-stub").c_str(), 1);

    const auto answer = fstools::regionFormatArgs("/etc/rooffs/daemon.yaml", "/mnt/example", std::nullopt);
    check("regionFormatArgs answers nothing for a failing daemon", !answer.has_value());
}

// A daemon that exits 0 with an empty first line has named no arguments for cme-format.
void checkRegionFormatArgsGivesUpOnAnEmptyFirstLine(const ScratchDir& scratch)
{
    const auto logPath = scratch.join("daemon_empty.log");
    writeStub(scratch.join("daemon-empty-stub"), logPath, {"", "a second line is not the first"}, 0);
    ::setenv(FS_NAME_UPPER_STR "_DAEMON", scratch.join("daemon-empty-stub").c_str(), 1);

    const auto answer = fstools::regionFormatArgs("/etc/rooffs/daemon.yaml", "/mnt/example", std::nullopt);
    check("regionFormatArgs answers nothing for an empty first line", !answer.has_value());
}

}  // namespace

int main()
{
    checkSplitWordsKeepsSemicolonInsideAWord();

    const ScratchDir scratch{};
    checkFormatLockRegionArgvArrivesWholeAndNothingRuns(scratch);
    checkRegionFormatArgsCarriesOneArgumentWithASpaceAndASemicolon(scratch);
    checkRegionFormatArgsGivesUpOnANonzeroExit(scratch);
    checkRegionFormatArgsGivesUpOnAnEmptyFirstLine(scratch);

    std::printf("%d check(s) failed\n", g_failureCount);
    return g_failureCount == 0 ? 0 : 1;
}
