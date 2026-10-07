// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// config/config.hpp -- the daemon's settings, and every source they come from.
//
// One file holds what a host deploys with. Its maps of scalars go through the house key-value reader
// (cme's kvconfig). Its two lists of maps, rules and mounts, are lifted out and read beside it.

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

// One mount the host carries: where it is mounted, and the DAX device it lays out on.
struct Mount_t
{
    std::string point;
    std::string device;
};

class Config
{
public:
    // Loads config.yaml. Throws std::runtime_error on a parse error, a missing file, or one this
    // daemon may not trust. @nodeIdOverride is the node this instance serves, which the unit names.
    [[nodiscard]] static Config load(const std::string& path,
                                     std::optional<std::uint32_t> nodeIdOverride = std::nullopt);

    // The settings @args names, for a hand run or a test with no config file. A value no flag
    // reaches keeps its default.
    [[nodiscard]] static Config fromArgs(const std::vector<std::string_view>& args,
                                         std::optional<std::uint32_t> nodeIdOverride = std::nullopt);

    // @key as the lines a deploy script reads, one value to a line, or nullopt for a key it never
    // asks for. A mount is one line, its point and its device.
    [[nodiscard]] std::optional<std::vector<std::string>> findSetting(std::string_view key) const;

    // Throws unless this euid, the config file's owner and accounts.daemon name one account. The
    // mount helper takes the daemon account from that owner, so a daemon running as anyone else is
    // refused. Settings from a hand run's flags have no owner to compare.
    void requireDaemonAccount() const;

    // Throws when the file these settings came from fails to load now, or holds a new value for a
    // key the running daemon was built from. What a SIGHUP checks before any backend re-reads.
    void checkReloadable() const;

    // The file the settings came from, empty for a hand run's flags.
    [[nodiscard]] const std::string& getSourcePath() const noexcept
    {
        return sourcePath_;
    }

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
    // The file the rules are read from: the config itself, or --rules-path on a hand run.
    [[nodiscard]] const std::string& getIdentityRulesPath() const noexcept
    {
        return identityRulesPath_;
    }

    [[nodiscard]] const std::vector<std::string>& getSelectors() const noexcept
    {
        return selectors_;
    }

    [[nodiscard]] const std::string& getTrustDomain() const noexcept
    {
        return trustDomain_;
    }

    [[nodiscard]] const std::string& getSpireSocket() const noexcept
    {
        return spireSocket_;
    }

    [[nodiscard]] const std::string& getPolicyPath() const noexcept
    {
        return policyPath_;
    }

    // The package the decision lives in. Empty leaves the policy backend on its own default.
    [[nodiscard]] const std::string& getPolicyQuery() const noexcept
    {
        return policyQuery_;
    }

    [[nodiscard]] const std::string& getPolicyUrl() const noexcept
    {
        return policyUrl_;
    }

    // The unix socket the policy server answers on. A path here carries the request, and the url's
    // host then only fills the Host header.
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
    // helper asks it for these, so one file answers for both.
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
    // The char device this node's helper opens its channel on: --channel when a hand run names one,
    // otherwise /dev/<daemon>-<node_id>.
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

    // The keys this daemon was built from that @staged holds differently.
    [[nodiscard]] std::vector<std::string> findRestartChanges(const Config& staged) const;

    std::uint32_t nodeId_{0};
    std::string sourcePath_;

    std::string daemonAccount_;
    std::string workloadAccount_;
    std::vector<Mount_t> mounts_;
    std::optional<std::uint32_t> fileOwner_;

    std::string identityBackend_{"local"};
    std::string policyBackend_{"local"};

    std::string identityRulesPath_;
    std::vector<std::string> selectors_;
    std::string trustDomain_;
    std::string spireSocket_;
    std::string spireServerAddress_;
    std::uint32_t spireServerPort_{8081};

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
