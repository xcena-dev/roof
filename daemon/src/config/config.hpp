// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// config/config.hpp -- the daemon's settings, and every source they come from.
//
// The file is a nested map of scalars and inline lists, which the house key-value reader (cme's
// kvconfig) covers, so this reads it with that rather than a parser of its own.

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fsdaemon::config
{

// The value after @flag in @args, or empty when the flag is absent or ends the line. A caller that
// must read a flag before the settings exist, such as --config itself, reaches it here.
[[nodiscard]] std::string findOptionValue(const std::vector<std::string_view>& args, std::string_view flag);

// The URI of the lock region the mount at @mountPoint carries.
[[nodiscard]] std::string makeRegionUri(std::string_view mountPoint);

class Config
{
public:
    // Loads daemon.yaml. Throws std::runtime_error on a parse error, an unreadable-but-present file,
    // or an inline `policy:` block (policy must be a file path). @nodeIdOverride replaces node_id
    // when it holds one, since the config is identical on every node and the unit knows the mount.
    [[nodiscard]] static Config load(const std::string& path,
                                     std::optional<std::uint32_t> nodeIdOverride = std::nullopt);

    // The settings @args names, for a hand run or a test with no config file. A value no flag
    // reaches keeps its default.
    [[nodiscard]] static Config fromArgs(const std::vector<std::string_view>& args,
                                         std::optional<std::uint32_t> nodeIdOverride = std::nullopt);

    // ── What the deployment chose ───────────────────────────────────────
    [[nodiscard]] std::uint32_t getNodeId() const noexcept
    {
        return nodeId_;
    }

    // ── Which backend answers, by the name the registry knows it under ──
    [[nodiscard]] const std::string& getIdentityBackend() const noexcept
    {
        return identityBackend_;
    }

    [[nodiscard]] const std::string& getPolicyBackend() const noexcept
    {
        return policyBackend_;
    }

    // ── What a backend builds itself from ───────────────────────────────
    [[nodiscard]] const std::string& getIdentityRulesPath() const noexcept
    {
        return identityRulesPath_;
    }

    [[nodiscard]] const std::vector<std::string>& getSelectors() const noexcept
    {
        return selectors_;
    }

    [[nodiscard]] const std::string& getSpireSocket() const noexcept
    {
        return spireSocket_;
    }

    [[nodiscard]] const std::string& getPolicyPath() const noexcept
    {
        return policyPath_;
    }

    // Empty leaves the policy backend on its own default query.
    [[nodiscard]] const std::string& getPolicyQuery() const noexcept
    {
        return policyQuery_;
    }

    [[nodiscard]] const std::string& getPolicyUrl() const noexcept
    {
        return policyUrl_;
    }

    // The unix socket the policy server answers on. A path here carries the request, and the url's
    // host then only fills the Host header. Empty puts the request on TCP, which authenticates
    // nothing about whoever holds that port.
    [[nodiscard]] const std::string& getPolicySocket() const noexcept
    {
        return policySocket_;
    }

    // The cme lock region this node joins, or empty to serve no lock.
    [[nodiscard]] const std::string& getTurnUri() const noexcept
    {
        return turnUri_;
    }

    // What laying that region out takes. This daemon attaches rather than formats, but the mount
    // helper cannot read this config and asks for these, so one file answers for both.
    [[nodiscard]] std::uint32_t getTurnMaxPeers() const noexcept
    {
        return turnMaxPeers_;
    }

    [[nodiscard]] std::uint32_t getTurnMaxDomains() const noexcept
    {
        return turnMaxDomains_;
    }

    [[nodiscard]] const std::string& getTurnStrategy() const noexcept
    {
        return turnStrategy_;
    }

    // ── What the audit trail does ───────────────────────────────────────
    [[nodiscard]] const std::string& getAuditTarget() const noexcept
    {
        return auditTarget_;
    }

    [[nodiscard]] bool isLoggingDecisions() const noexcept
    {
        return auditLogDecisions_;
    }

    // ── Derived, so a caller never assembles these itself ───────────────
    // The char device this node's helper opens its channel on: channel_path or --channel when one
    // names it, otherwise /dev/<daemon>-<node_id>.
    [[nodiscard]] std::string getChannelPath() const;

    // How many worker threads answer off the serve loop. A lock acquire waits on a peer for as
    // long as that peer holds it, so the loop hands the wait to one of these and reads the next
    // request meanwhile.
    [[nodiscard]] std::uint32_t getWorkerCount() const noexcept
    {
        return upcallWorkers_;
    }

    // Whether to time each stage of an answer and report means. Off by default, and off means no
    // clock is read on the serving path.
    [[nodiscard]] bool isTracingStages() const noexcept
    {
        return upcallTraceTurns_;
    }

    // The upcall budget as a transport takes it. The file writes seconds and every backend bounds
    // one request in milliseconds, so the conversion lives with the value.
    [[nodiscard]] std::chrono::milliseconds getRequestTimeout() const;

private:
    // The two factories are the only way in, so a Config a caller holds is always one a source was
    // read into and checked.
    Config() = default;

    std::uint32_t nodeId_{0};

    std::string identityBackend_{"local"};
    std::string policyBackend_{"local"};

    std::string identityRulesPath_;
    std::vector<std::string> selectors_;
    std::string spireSocket_;

    std::string policyPath_;
    std::string policyQuery_;
    std::string policyUrl_;
    std::string policySocket_;

    std::string turnUri_;
    std::uint32_t turnMaxPeers_{8};
    std::uint32_t turnMaxDomains_{4};
    std::string turnStrategy_{"peterson"};

    std::string auditTarget_{"stdout"};
    bool auditLogDecisions_{true};

    // Neither is read from outside: the two accessors above are what they exist for.
    std::string channelPathOverride_;
    std::chrono::duration<double> upcallRequestTimeout_{4.0};
    std::uint32_t upcallWorkers_{4};
    bool upcallTraceTurns_{false};
};

}  // namespace fsdaemon::config
