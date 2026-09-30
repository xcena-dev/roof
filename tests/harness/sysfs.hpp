// SPDX-License-Identifier: Apache-2.0
//
// sysfs.hpp -- what a case reads under the filesystem's sysfs directory as an ordinary user.
//
// Each file here is the only surface for what it reports. region_info is where a RAT slot's physical
// extent becomes visible at all, gc_status is where the sweep counter is, which lets a case wait for
// a sweep instead of sleeping and hoping, and meta_lock is where a turn the kernel took becomes
// visible.
//
// Nothing here writes. The debug files that select a region or force a sweep are root's, so a case
// that needs one gates on it and reports a skip.

#pragma once

#include <chrono>
#include <cstdint>
#include <exception>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "name.h"

namespace fsuser::tests
{

constexpr const char* RegionInfoPath = "/sys/fs/" FS_NAME_STR "/region_info";
constexpr const char* GcStatusPath = "/sys/fs/" FS_NAME_STR "/gc_status";
constexpr const char* PermInfoPath = "/sys/fs/" FS_NAME_STR "/perm_info";
constexpr const char* PoolInfoPath = "/sys/fs/" FS_NAME_STR "/pool_info";

// Under the mount's own directory, so one file is one mount's reading of the table.
[[nodiscard]] inline std::string bootstrapDumpPath(std::uint32_t nodeId)
{
    return "/sys/fs/" FS_NAME_STR "/node" + std::to_string(nodeId) + "/test/bootstrap_dump";
}

[[nodiscard]] inline std::string metaLockPath(std::uint32_t nodeId)
{
    return "/sys/fs/" FS_NAME_STR "/node" + std::to_string(nodeId) + "/test/meta_lock";
}

// Unlike the test/ files above, this one is the daemon's own and needs no root.
[[nodiscard]] inline std::string daemonStatePath(std::uint32_t nodeId)
{
    return "/sys/fs/" FS_NAME_STR "/node" + std::to_string(nodeId) + "/daemon_state";
}

// Module-wide and not per-mount, like freeze_heartbeat: a fault is keyed on the node_id a test
// names, not on which mount happens to be reading it.
constexpr const char* StampOffsetPath = "/sys/fs/" FS_NAME_STR "/test/stamp_offset";

// Adds @offsetNs to what @nodeId's next tick would otherwise stamp, so the value keeps moving with
// the real clock but by an amount a peer's wall-clock check refuses. 0 clears the fault.
[[nodiscard]] inline bool writeStampOffset(std::uint32_t nodeId, std::int64_t offsetNs)
{
    std::ofstream sink{StampOffsetPath};
    if (!sink)
    {
        return false;
    }
    sink << nodeId << ' ' << offsetNs;
    sink.close();  // the kernel write happens at flush, so its error only shows after this
    return !sink.fail();
}

// The header region_info puts above its rows, which a case checks before trusting the fields below
// it: a reordered column would otherwise be read as a value.
constexpr const char* RegionInfoHeader = "RAT_Entry\tNode\tPID\tState\tSize\tOffset\tName";

struct RegionRow_t
{
    std::int64_t entry{-1};
    std::uint32_t node{0};
    std::uint32_t pid{0};
    std::string state;
    std::uint64_t size{0};
    std::uint64_t offset{0};
    std::string name;
};

// Tabs and not whitespace, since a name is free to hold a space and it is the last field.
[[nodiscard]] inline std::vector<std::string> splitOnTabs(const std::string& line)
{
    std::vector<std::string> fields;
    std::uint64_t cursor = 0;
    while (true)
    {
        const auto tab = line.find('\t', cursor);
        if (tab == std::string::npos)
        {
            fields.push_back(line.substr(cursor));
            return fields;
        }
        fields.push_back(line.substr(cursor, tab - cursor));
        cursor = tab + 1;
    }
}

// The number written after @key in @line, or nullopt when @key is absent or what follows it is not
// one. The offset is taken from the key itself, so no caller carries a length beside the word.
[[nodiscard]] inline std::optional<std::uint64_t> readTagged(const std::string& line,
                                                             std::string_view key, std::int32_t base = 10)
{
    const auto found = line.find(key);
    if (found == std::string::npos)
    {
        return std::nullopt;
    }
    try
    {
        return std::stoull(line.substr(found + key.size()), nullptr, base);
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

// The table region_info holds: the header row a case checks the columns against, and the rows under
// it. nullopt when the file is not there, which is what separates that from a region with no rows.
struct RegionInfo_t
{
    std::string header;
    std::vector<RegionRow_t> rows;
};

[[nodiscard]] inline std::optional<RegionInfo_t> readRegionInfo()
{
    std::ifstream source{RegionInfoPath};
    RegionInfo_t table;
    if (!source || !std::getline(source, table.header))
    {
        return std::nullopt;
    }
    std::string line;
    while (std::getline(source, line))
    {
        const auto fields = splitOnTabs(line);
        if (fields.size() < 7)
        {
            continue;
        }
        try
        {
            table.rows.push_back({std::stoll(fields[0]),
                                  static_cast<std::uint32_t>(std::stoul(fields[1])),
                                  static_cast<std::uint32_t>(std::stoul(fields[2])), fields[3],
                                  std::stoull(fields[4]), std::stoull(fields[5], nullptr, 0),
                                  fields[6]});
        }
        catch (const std::exception&)
        {
            continue;
        }
    }
    return table;
}

[[nodiscard]] inline std::vector<RegionRow_t> readRegionRows()
{
    auto table = readRegionInfo();
    return table ? std::move(table->rows) : std::vector<RegionRow_t>{};
}

// Where the two region pools sit and what is left in each. A pool row reads
// "<pool> <start> <total> <free>" and the granule has a line of its own above them.
struct PoolInfo_t
{
    std::uint64_t granule{0};
    std::uint64_t ucStart{0};
    std::uint64_t ucTotal{0};
    std::uint64_t ucFree{0};
    std::uint64_t wbStart{0};
    std::uint64_t wbTotal{0};
    std::uint64_t wbFree{0};
};

[[nodiscard]] inline std::optional<PoolInfo_t> readPoolInfo()
{
    std::ifstream source{PoolInfoPath};
    if (!source.is_open())
    {
        return std::nullopt;
    }

    PoolInfo_t into;
    std::string line;
    bool sawWriteback = false;
    while (std::getline(source, line))
    {
        std::istringstream fields{line};
        std::string label;
        if (!(fields >> label))
        {
            continue;
        }

        if (label == "granule")
        {
            fields >> into.granule;
        }
        else if (label == "uncached")
        {
            fields >> into.ucStart >> into.ucTotal >> into.ucFree;
        }
        else if (label == "writeback")
        {
            fields >> into.wbStart >> into.wbTotal >> into.wbFree;
            sawWriteback = true;
        }
    }

    // The boundary is what a placement check compares against, so its absence is what fails this.
    if (!sawWriteback)
    {
        return std::nullopt;
    }
    return into;
}

// How many rows carry @name. Two is the collision a race on one name would leave behind.
[[nodiscard]] inline std::int32_t countRegionsNamed(const std::string& name)
{
    std::int32_t found = 0;
    for (const auto& row : readRegionRows())
    {
        found += (row.name == name) ? 1 : 0;
    }
    return found;
}

// gc_status is one line with a field per mount, each "node<id>:<state> epoch=<n>". Answers -1 when
// the file or that node is not there.
[[nodiscard]] inline std::int64_t readSweepCount(std::uint32_t nodeId)
{
    std::ifstream source{GcStatusPath};
    std::string line;
    if (!source || !std::getline(source, line))
    {
        return -1;
    }
    const auto foundAt = line.find("node" + std::to_string(nodeId) + ":");
    if (foundAt == std::string::npos)
    {
        return -1;
    }
    const auto counted = readTagged(line.substr(foundAt), "epoch=");
    return counted ? static_cast<std::int64_t>(*counted) : -1;
}

// How many sweeps a case waits for before reading what one gave back. Two and not one, because a
// cycle already under way when the state changed may have read it before.
constexpr std::int32_t SweepsToWait = 2;
constexpr std::int32_t SweepDeadlineSeconds = 60;
constexpr std::int32_t SweepPollMillis = 250;

// Bounded, so a GC that stopped ends a case with a failed check instead of hanging it. False also
// when gc_status stops answering for @nodeId, which is the same thing from the caller's side.
[[nodiscard]] inline bool waitForSweeps(std::uint32_t nodeId, std::int64_t from)
{
    const auto until =
        std::chrono::steady_clock::now() + std::chrono::seconds(SweepDeadlineSeconds);
    while (std::chrono::steady_clock::now() < until)
    {
        const auto now = readSweepCount(nodeId);
        if (now < 0)
        {
            return false;
        }
        if (now >= from + SweepsToWait)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(SweepPollMillis));
    }
    return false;
}

// One line of bootstrap_dump. A slot nobody holds carries a zero token, which is what a release
// writes and what a scan reads as free.
struct BootstrapSlot_t
{
    std::int32_t slot{-1};
    std::uint32_t node{0};
    std::uint32_t state{0};  // enum layout_bootstrap_state: 0 free, 1 held, 2 recovering
    std::uint64_t token{0};
    std::uint64_t heartbeat{0};
    bool mine{false};
};

// Root's, so a caller that cannot read it separates that from a table with no slots. The mount named
// by @nodeId reads the whole table, and its own slot is the one carrying "<mine>".
[[nodiscard]] inline std::optional<std::vector<BootstrapSlot_t>> readBootstrapSlots(std::uint32_t nodeId)
{
    std::ifstream source{bootstrapDumpPath(nodeId)};
    if (!source)
    {
        return std::nullopt;
    }
    std::vector<BootstrapSlot_t> into;
    std::string line;
    while (std::getline(source, line))
    {
        if (line.rfind("slot[", 0) != 0)
        {
            continue;
        }
        const auto node = readTagged(line, "node_id=");
        const auto state = readTagged(line, "state=");
        const auto token = readTagged(line, "token=", 0);
        const auto heartbeat = readTagged(line, "heartbeat=");
        const auto slot = readTagged(line, "slot[");
        if (!node || !state || !token || !heartbeat || !slot)
        {
            continue;
        }
        BootstrapSlot_t row;
        row.mine = line.find("<mine>") != std::string::npos;
        row.slot = static_cast<std::int32_t>(*slot);
        row.node = static_cast<std::uint32_t>(*node);
        row.state = static_cast<std::uint32_t>(*state);
        row.token = *token;
        row.heartbeat = *heartbeat;
        into.push_back(row);
    }
    return into;
}

// What became of the cross-node half of @nodeId's metadata lock since its mount. A case reads this
// before and after an operation, so what it asserts on is the delta and never the total.
struct MetaTurns_t
{
    std::uint64_t taken{0};
    std::uint64_t refused{0};
};

// Answers false when the file is not there, which is a module built without its test attributes
// rather than a mount that took no turn.
[[nodiscard]] inline std::optional<MetaTurns_t> readMetaTurns(std::uint32_t nodeId)
{
    std::ifstream source{metaLockPath(nodeId)};
    std::string line;
    if (!source || !std::getline(source, line))
    {
        return std::nullopt;
    }
    const auto taken = readTagged(line, "taken=");
    const auto refused = readTagged(line, "refused=");
    if (!taken || !refused)
    {
        return std::nullopt;
    }
    return MetaTurns_t{*taken, *refused};
}

// Every request @nodeId's channel has queued, answered or not. A caller a standing row already
// answers never reaches it, so a rise across an interval is the helper deciding again.
[[nodiscard]] inline std::optional<std::uint64_t> readDaemonAsked(std::uint32_t nodeId)
{
    std::ifstream source{daemonStatePath(nodeId)};
    std::string line;
    if (!source || !std::getline(source, line))
    {
        return std::nullopt;
    }
    return readTagged(line, "asked=");
}

// The whole content of @path with trailing whitespace removed, or an empty string when it cannot be
// read. For the one-value files, where the value is the whole file.
[[nodiscard]] inline std::string readWholeLine(const char* path)
{
    std::ifstream source{path};
    std::string line;
    if (!source || !std::getline(source, line))
    {
        return {};
    }
    while (!line.empty() && (line.back() == '\n' || line.back() == ' '))
    {
        line.pop_back();
    }
    return line;
}

}  // namespace fsuser::tests
