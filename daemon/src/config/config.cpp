// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// config/config.cpp -- see config.hpp.

#include "config/config.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <ratio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "common/kv_config.hpp"
#include "config/internal/mount_table.hpp"
#include "name.hpp"
#include "tools_uapi.h"
#include "util/text.hpp"

namespace fsdaemon::config
{

namespace
{

// A relative rules or policy path is read next to the config file itself, so the shipped example
// can name a bare filename. An absolute path passes through unchanged.
std::string resolveNextToConfig(std::string_view configPath, const std::string& value)
{
    const std::filesystem::path candidate{value};
    if (value.empty() || candidate.is_absolute())
    {
        return value;
    }
    return (std::filesystem::path{configPath}.parent_path() / candidate).string();
}

// A `policy:` line opening a nested map, which this daemon refuses: the policy is a file path, not
// something inlined into the config, so the two cannot drift.
bool hasInlinePolicyBlock(const std::string& path)
{
    std::ifstream file{path};
    std::string line;
    while (std::getline(file, line))
    {
        if (util::trimSpace(line) == "policy:")
        {
            return true;
        }
    }
    return false;
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

}  // namespace

Config Config::load(const std::string& path, std::optional<std::uint32_t> nodeIdOverride)
{
    if (hasInlinePolicyBlock(path))
    {
        throw std::runtime_error{path + ": an inline 'policy:' block is not allowed; use policy_path"};
    }

    const auto source = kvconfig::KeyValueConfig::load(path);

    Config config{};
    config.nodeId_ = nodeIdOverride.value_or(source.get<std::uint32_t>("node_id", config.nodeId_));

    config.channelPathOverride_ = source.getString("channel_path", "");
    config.identityRulesPath_ = resolveNextToConfig(
        path, source.getString("identity_rules_path", "identity-rules.yaml"));
    config.policyPath_ = resolveNextToConfig(path, source.getString("policy_path", "policy.rego"));

    config.identityBackend_ = source.getString("backends.identity", config.identityBackend_);
    config.policyBackend_ = source.getString("backends.policy", config.policyBackend_);

    config.selectors_ = source.getList("selectors");
    config.spireSocket_ = source.getString("spire_workload_api_socket",
                                           "/run/spire-agent/admin/api.sock");

    // A config that names a region wins. Otherwise this node's own mount carries it, which is what
    // a deployment that only mounts and starts the daemon relies on.
    config.turnUri_ = source.getString("turn_region.uri", "");
    if (config.turnUri_.empty())
    {
        config.turnUri_ = internal::findRegionForNode(config.nodeId_);
    }
    config.turnMaxPeers_ = source.get<std::uint32_t>("turn_region.max_peers", config.turnMaxPeers_);
    config.turnMaxDomains_ = source.get<std::uint32_t>("turn_region.max_domains", config.turnMaxDomains_);
    config.turnStrategy_ = source.getString("turn_region.strategy", config.turnStrategy_);

    // The host in the url only fills the Host header while the socket below carries the request,
    // so the default names no port and a deployment that wants TCP has to write one.
    config.policyUrl_ = source.getString("policy_url",
                                         "http://localhost/v1/data/" + std::string{name::FsName} + "/authz");
    // A path and not a port by default: a port above 1024 is anyone's to hold first, and nothing in
    // the exchange tells the real server from whoever answered.
    config.policySocket_ = source.getString("policy_socket",
                                            "/run/" + std::string{name::FsName} + "-opa/api.sock");
    config.policyQuery_ = source.getString("policy_query", "");

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

}  // namespace fsdaemon::config
