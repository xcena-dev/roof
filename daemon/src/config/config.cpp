// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// config/config.cpp -- see config.hpp.

#include "config/config.hpp"

#include <pwd.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iterator>
#include <optional>
#include <ratio>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/kv_config.hpp"
#include "config/internal/mount_table.hpp"
#include "name.hpp"
#include "tools_uapi.h"
#include "util/file_text.hpp"
#include "util/text.hpp"

namespace fsdaemon::config
{

namespace
{

constexpr std::string_view RulesKey = "rules";
constexpr std::string_view MountsKey = "mounts";

// A relative policy path is read next to the config file itself, so the shipped example can name a
// bare filename. An absolute path passes through unchanged.
std::string resolveNextToConfig(std::string_view configPath, const std::string& value)
{
    const std::filesystem::path candidate{value};
    if (value.empty() || candidate.is_absolute())
    {
        return value;
    }
    return (std::filesystem::path{configPath}.parent_path() / candidate).string();
}

// A duration written as a number with an "ms" or "s" suffix, or as a bare number of seconds. Throws
// on a value that will not parse, so a typo is loud.
std::chrono::duration<double> parseDuration(std::string_view text)
{
    constexpr std::string_view MillisSuffix = "ms";

    const auto body = util::trimSpace(text);
    try
    {
        if (body.size() > MillisSuffix.size() && body.substr(body.size() - MillisSuffix.size()) == MillisSuffix)
        {
            const auto millis = body.substr(0, body.size() - MillisSuffix.size());
            return std::chrono::duration<double, std::milli>{std::stod(std::string{millis})};
        }
        if (body.size() > 1 && body.back() == 's')
        {
            return std::chrono::duration<double>{std::stod(std::string{body.substr(0, body.size() - 1)})};
        }
        return std::chrono::duration<double>{std::stod(std::string{body})};
    }
    catch (const std::exception&)
    {
        throw std::runtime_error{std::string{"upcall.request_timeout: '"}.append(text).append("' is not a duration")};
    }
}

bool readBool(const kvconfig::KeyValueConfig& source, const std::string& key, bool fallback)
{
    const auto text = source.getString(key, fallback ? "true" : "false");
    return text == "true" || text == "1" || text == "yes";
}

// The lines of @text with the empty ones kept, so each keeps the number it has in the file.
std::vector<std::string_view> readLines(std::string_view text)
{
    std::vector<std::string_view> lines;
    std::uint64_t start = 0;
    for (;;)
    {
        const std::uint64_t stop = text.find('\n', start);
        if (stop == std::string_view::npos)
        {
            lines.push_back(text.substr(start));
            return lines;
        }
        lines.push_back(text.substr(start, stop - start));
        start = stop + 1;
    }
}

// The key a top-level line opens a nested block with, or empty for any other line.
std::string_view findBlockKey(std::string_view line)
{
    const std::uint64_t colon = line.find(':');
    if (colon == std::string_view::npos)
    {
        return {};
    }
    const auto rest = util::trimSpace(line.substr(colon + 1));
    if (!rest.empty() && rest.front() != '#')
    {
        return {};
    }
    return util::trimSpace(line.substr(0, colon));
}

// The file with its rules and mounts blocks blanked, and the mounts block's own lines. kvconfig holds
// no list of maps, and blanking rather than cutting keeps its line numbers right.
struct SplitText_t
{
    std::string settings;
    std::vector<std::string_view> mountRows;
};

SplitText_t takeListBlocks(std::string_view text)
{
    SplitText_t split;
    std::string_view openBlock;
    for (const auto line : readLines(text))
    {
        const auto trimmed = util::trimSpace(line);
        const bool topLevel = !trimmed.empty() && trimmed.front() != '#' && line.front() != ' ' &&
                              line.front() != '\t' && line.front() != '-';
        if (topLevel)
        {
            const auto key = findBlockKey(line);
            openBlock = key == RulesKey || key == MountsKey ? key : std::string_view{};
        }
        if (openBlock.empty())
        {
            split.settings.append(line);
        }
        else if (openBlock == MountsKey && !topLevel)
        {
            split.mountRows.push_back(line);
        }
        split.settings.push_back('\n');
    }
    return split;
}

// The mounts block's rows as mounts. Each '-' opens one, and point and device are its only fields, so
// a misspelling is refused rather than read as a mount missing one.
std::vector<Mount_t> parseMounts(const std::vector<std::string_view>& rows, const std::string& path)
{
    const auto refuse = [&path](std::string_view why)
    {
        return std::runtime_error{std::string{path}.append(": mounts: ").append(why)};
    };

    std::vector<Mount_t> mounts;
    for (auto row : rows)
    {
        if (const std::uint64_t hash = row.find(" #"); hash != std::string_view::npos)
        {
            row = row.substr(0, hash);
        }
        auto field = util::trimSpace(row);
        if (field.empty() || field.front() == '#')
        {
            continue;
        }
        if (field.front() == '-')
        {
            mounts.emplace_back();
            field = util::trimSpace(field.substr(1));
            if (field.empty())
            {
                continue;
            }
        }
        const std::uint64_t colon = field.find(':');
        if (mounts.empty() || colon == std::string_view::npos)
        {
            throw refuse(std::string{"no mount item holds '"}.append(field).append("'"));
        }
        const auto key = util::trimSpace(field.substr(0, colon));
        std::string value{util::trimSpace(field.substr(colon + 1))};
        if (key == "point")
        {
            mounts.back().point = std::move(value);
        }
        else if (key == "device")
        {
            mounts.back().device = std::move(value);
        }
        else
        {
            throw refuse(std::string{"a mount has no field '"}.append(key).append("'"));
        }
    }

    std::unordered_set<std::string> seen;
    for (const auto& mount : mounts)
    {
        if (mount.point.empty() || mount.device.empty())
        {
            throw refuse("every mount names a point and a device");
        }
        if (!seen.insert(mount.point).second)
        {
            throw refuse(mount.point + " is listed twice");
        }
    }
    return mounts;
}

// The OPA data API path for @package, which names with dots what the URL names with slashes.
std::string makePolicyUrl(std::string package)
{
    std::replace(package.begin(), package.end(), '.', '/');
    return "http://localhost/v1/data/" + package;
}

}  // namespace

Config Config::load(const std::string& path, std::optional<std::uint32_t> nodeIdOverride)
{
    const auto found = util::readTrustedText(path);
    if (found.standing == util::Standing::Absent)
    {
        throw std::runtime_error{path + ": cannot read"};
    }
    if (found.standing == util::Standing::Untrusted)
    {
        throw std::runtime_error{path + ": must be owned by root or this daemon and writable by its owner alone"};
    }
    const auto split = takeListBlocks(found.text);
    std::istringstream settings{split.settings};
    const auto source = kvconfig::KeyValueConfig::parse(settings, path);
    const std::string fsName{name::FsName};

    Config config{};
    config.nodeId_ = nodeIdOverride.value_or(0U);
    config.sourcePath_ = path;
    config.fileOwner_ = found.owner;
    config.daemonAccount_ = source.getString("accounts.daemon", fsName);
    config.workloadAccount_ = source.getString("accounts.workload", "");
    config.mounts_ = parseMounts(split.mountRows, path);

    config.identityRulesPath_ = path;
    config.identityBackend_ = source.getString("identity.backend", config.identityBackend_);
    config.selectors_ = source.getList("identity.selectors");

    config.trustDomain_ = source.getString("spire.trust_domain", fsName + ".local");
    config.spireSocket_ = source.getString("spire.admin_socket", "/run/spire-agent/admin/api.sock");
    config.spireServerAddress_ = source.getString("spire.server_address", "127.0.0.1");
    config.spireServerPort_ = source.get<std::uint32_t>("spire.server_port", config.spireServerPort_);

    config.policyBackend_ = source.getString("policy.backend", config.policyBackend_);
    config.policyPath_ = resolveNextToConfig(path, source.getString("policy.path", "policy.rego"));
    config.policyQuery_ = source.getString("policy.package", fsName + ".authz");
    config.policyUrl_ = makePolicyUrl(config.policyQuery_);
    // A path and not a port: a port above 1024 is anyone's to hold first, and nothing in the
    // exchange tells the real server from whoever answered.
    config.policySocket_ = source.getString("policy.opa_socket", "/run/" + fsName + "-opa/api.sock");

    // A config that names a region wins. Otherwise this node's own mount carries it.
    config.turnUri_ = source.getString("turn.uri", "");
    if (config.turnUri_.empty())
    {
        config.turnUri_ = internal::findRegionForNode(config.nodeId_);
    }
    config.turnMaxPeers_ = source.get<std::uint32_t>("turn.max_peers", config.turnMaxPeers_);
    config.turnMaxDomains_ = source.get<std::uint32_t>("turn.max_domains", config.turnMaxDomains_);
    config.turnStrategy_ = source.getString("turn.strategy", config.turnStrategy_);

    config.auditTarget_ = source.getString("audit.target", config.auditTarget_);
    config.auditLogDecisions_ = readBool(source, "audit.log_decisions", config.auditLogDecisions_);

    config.upcallTraceTurns_ = readBool(source, "upcall.trace_turns", config.upcallTraceTurns_);
    config.upcallWorkers_ = source.get<std::uint32_t>("upcall.workers", config.upcallWorkers_);
    if (config.upcallWorkers_ == 0)
    {
        // No worker means a lock request is queued and never answered, so refuse rather than hang.
        throw std::runtime_error{path + ": upcall.workers must be at least 1"};
    }

    const auto requestTimeout = source.getString("upcall.request_timeout", "");
    if (!requestTimeout.empty())
    {
        config.upcallRequestTimeout_ = parseDuration(requestTimeout);
    }

    return config;
}

Config Config::fromArgs(const std::vector<std::string_view>& args,
                        std::optional<std::uint32_t> nodeIdOverride)
{
    Config config{};
    config.nodeId_ = nodeIdOverride.value_or(0U);

    config.identityRulesPath_ = findOptionValue(args, "--rules-path");
    config.policyPath_ = findOptionValue(args, "--policy-path");
    const auto selectorText = findOptionValue(args, "--selectors");
    const auto selectorNames = util::splitFields(selectorText, ",");
    config.selectors_.assign(selectorNames.begin(), selectorNames.end());
    config.spireSocket_ = findOptionValue(args, "--spire-socket");
    if (const auto named = findOptionValue(args, "--identity-backend"); !named.empty())
    {
        config.identityBackend_ = named;
    }
    if (const auto named = findOptionValue(args, "--policy-backend"); !named.empty())
    {
        config.policyBackend_ = named;
    }
    config.turnUri_ = findOptionValue(args, "--turn-uri");
    config.policyUrl_ = findOptionValue(args, "--opa-url");
    config.policySocket_ = findOptionValue(args, "--opa-socket");
    config.channelPathOverride_ = findOptionValue(args, "--channel");
    return config;
}

std::optional<std::vector<std::string>> Config::findSetting(std::string_view key) const
{
    if (key == MountsKey)
    {
        std::vector<std::string> lines;
        lines.reserve(mounts_.size());
        for (const auto& mount : mounts_)
        {
            lines.push_back(mount.point + " " + mount.device);
        }
        return lines;
    }

    const std::pair<std::string_view, std::string> scalars[] = {
        {"accounts.daemon", daemonAccount_},
        {"accounts.workload", workloadAccount_},
        {"identity.backend", identityBackend_},
        {"policy.backend", policyBackend_},
        {"policy.path", policyPath_},
        {"policy.opa_socket", policySocket_},
        {"spire.trust_domain", trustDomain_},
        {"spire.admin_socket", spireSocket_},
        {"spire.server_address", spireServerAddress_},
        {"spire.server_port", std::to_string(spireServerPort_)},
    };
    for (const auto& [name, value] : scalars)
    {
        if (name == key)
        {
            return std::vector<std::string>{value};
        }
    }
    return std::nullopt;
}

std::vector<std::string> Config::findRestartChanges(const Config& staged) const
{
    std::vector<std::string> changed;
    const auto note = [&changed](std::string_view key, bool differs)
    {
        if (differs)
        {
            changed.emplace_back(key);
        }
    };
    note("accounts.daemon", daemonAccount_ != staged.daemonAccount_);
    note("identity.backend", identityBackend_ != staged.identityBackend_);
    note("identity.selectors", selectors_ != staged.selectors_);
    note("policy.backend", policyBackend_ != staged.policyBackend_);
    note("policy.path", policyPath_ != staged.policyPath_);
    note("policy.package", policyQuery_ != staged.policyQuery_);
    note("policy.opa_socket", policySocket_ != staged.policySocket_);
    note("spire.trust_domain", trustDomain_ != staged.trustDomain_);
    note("spire.admin_socket", spireSocket_ != staged.spireSocket_);
    note("audit.target", auditTarget_ != staged.auditTarget_);
    note("audit.log_decisions", auditLogDecisions_ != staged.auditLogDecisions_);
    note("upcall.request_timeout", upcallRequestTimeout_ != staged.upcallRequestTimeout_);
    note("upcall.trace_turns", upcallTraceTurns_ != staged.upcallTraceTurns_);
    note("upcall.workers", upcallWorkers_ != staged.upcallWorkers_);
    note("turn.uri", turnUri_ != staged.turnUri_);
    note("turn.max_peers", turnMaxPeers_ != staged.turnMaxPeers_);
    note("turn.max_domains", turnMaxDomains_ != staged.turnMaxDomains_);
    note("turn.strategy", turnStrategy_ != staged.turnStrategy_);
    return changed;
}

std::string findOptionValue(const std::vector<std::string_view>& args, std::string_view flag)
{
    const auto found = std::find(args.begin(), args.end(), flag);
    if (found == args.end() || std::next(found) == args.end())
    {
        return "";
    }
    return std::string{*std::next(found)};
}

std::string makeRegionUri(std::string_view mountPoint)
{
    return std::string{"file:"}.append(mountPoint).append("/").append(FS_LOCK_REGION_NAME);
}

std::chrono::milliseconds Config::getRequestTimeout() const
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(upcallRequestTimeout_);
}

std::string Config::getChannelPath() const
{
    return channelPathOverride_.empty() ? name::channelPathForNode(nodeId_) : channelPathOverride_;
}

void Config::requireDaemonAccount() const
{
    if (!fileOwner_)
    {
        return;
    }
    const auto* const entry = ::getpwnam(daemonAccount_.c_str());
    if (entry == nullptr)
    {
        throw std::runtime_error{"accounts.daemon: no account is named '" + daemonAccount_ + "'"};
    }
    const std::uint32_t self = ::geteuid();
    if (entry->pw_uid != self || *fileOwner_ != self)
    {
        throw std::runtime_error{std::string{"accounts.daemon '"}
                                     .append(daemonAccount_)
                                     .append("' is uid ")
                                     .append(std::to_string(entry->pw_uid))
                                     .append(", the config is owned by uid ")
                                     .append(std::to_string(*fileOwner_))
                                     .append(", and this daemon runs as uid ")
                                     .append(std::to_string(self))
                                     .append("; all three must be one account")};
    }
}

void Config::checkReloadable() const
{
    const auto staged = load(sourcePath_, nodeId_);
    const auto changed = findRestartChanges(staged);
    if (changed.empty())
    {
        return;
    }
    std::string names;
    for (const auto& key : changed)
    {
        names.append(names.empty() ? "" : ", ").append(key);
    }
    throw std::runtime_error{names + " changed, which takes a restart"};
}

}  // namespace fsdaemon::config
