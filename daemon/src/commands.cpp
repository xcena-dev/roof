// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// daemon/commands.cpp -- see commands.hpp.

#include "commands.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "backends.hpp"
#include "config/config.hpp"

namespace fsdaemon
{

namespace
{

bool hasFlag(const std::vector<std::string_view>& args, std::string_view flag)
{
    return std::find(args.begin(), args.end(), flag) != args.end();
}

// Runs @body and answers its exit status, reporting what it threw on stderr.
template <typename T_Body>
std::int32_t runReporting(const T_Body& body)
{
    try
    {
        body();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "daemon: %s\n", error.what());
        return 1;
    }
}

config::Config loadNamedConfig(const std::string& configPath)
{
    if (configPath.empty())
    {
        throw std::runtime_error{"this command reads a config, and --config names none"};
    }
    return config::Config::load(configPath);
}

// A deploy script reads a setting through the same parser the daemon serves with, one value to a
// line, rather than matching the file's text itself.
void writeSetting(const std::string& configPath, std::string_view key)
{
    const auto lines = loadNamedConfig(configPath).findSetting(key);
    if (!lines)
    {
        throw std::runtime_error{std::string{"--print-config has no key '"}.append(key).append("'")};
    }
    for (const auto& line : *lines)
    {
        std::fprintf(stdout, "%s\n", line.c_str());
    }
}

// The mount helper cannot read this daemon's config, so it runs the daemon to learn what laying out
// the lock region takes. One line on stdout, in the order cme-format takes its flags.
void writeRegionFormat(const std::string& configPath, const std::string& mountPoint)
{
    const auto config = loadNamedConfig(configPath);
    const auto uri = config.getTurnUri().empty() ? config::makeRegionUri(mountPoint) : config.getTurnUri();
    std::fprintf(stdout, "--uri %s --max-peers %u --max-domains %u --strategy %s\n", uri.c_str(),
                 config.getTurnMaxPeers(), config.getTurnMaxDomains(), config.getTurnStrategy().c_str());
}

// What a SIGHUP would load, built and checked without a channel, so an edit is tried before a
// running daemon is asked to take it.
void checkConfig(const std::string& configPath)
{
    const auto config = loadNamedConfig(configPath);
    const auto identity = makeIdentityProvider(config);
    const auto policy = makePolicyEngine(config, *identity);
    identity->checkReload();
    if (policy != nullptr)
    {
        policy->checkReload();
    }
    std::fprintf(stdout, "%s: every check passed\n", configPath.c_str());
}

}  // namespace

std::optional<std::int32_t> runCommand(const std::vector<std::string_view>& args)
{
    if (hasFlag(args, "--list-backends"))
    {
        for (const auto& name : getBackendNames())
        {
            std::fprintf(stdout, "%s\n", name.c_str());
        }
        return 0;
    }

    const auto configPath = config::findOptionValue(args, "--config");
    if (const auto key = config::findOptionValue(args, "--print-config"); !key.empty())
    {
        return runReporting([&]
                            {
                                writeSetting(configPath, key);
                            });
    }
    if (const auto target = config::findOptionValue(args, "--print-region-format"); !target.empty())
    {
        return runReporting([&]
                            {
                                writeRegionFormat(configPath, target);
                            });
    }
    if (hasFlag(args, "--check-config"))
    {
        return runReporting([&]
                            {
                                checkConfig(configPath);
                            });
    }
    return std::nullopt;
}

std::optional<std::uint32_t> readNodeId(const std::vector<std::string_view>& args)
{
    const auto nodeText = config::findOptionValue(args, "--node-id");
    if (nodeText.empty())
    {
        return std::nullopt;
    }
    std::uint32_t parsedNode = 0;
    const auto parsed = std::from_chars(nodeText.data(), nodeText.data() + nodeText.size(), parsedNode);
    if (parsed.ec != std::errc{} || parsed.ptr != nodeText.data() + nodeText.size())
    {
        throw std::runtime_error{"--node-id " + nodeText + " is not a node id"};
    }
    return parsedNode;
}

}  // namespace fsdaemon
