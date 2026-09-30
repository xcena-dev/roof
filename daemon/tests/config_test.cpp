// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// tests/config_test.cpp -- daemon.yaml parsing, the backends block included.

#include "config/config.hpp"

#include <chrono>
#include <exception>
#include <string>

#include "config/internal/mount_table.hpp"
#include "harness/probe.hpp"
#include "harness/temp_file.hpp"
#include "name.hpp"
#include "tools_uapi.h"

namespace
{

using fsdaemon::config::Config;
using fsdaemon::probe::TempFile;

bool endsWith(const std::string& text, const std::string& tail)
{
    return text.size() >= tail.size() && text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
}

void checkMinimalLoad(fsdaemon::probe::Context& ctx)
{
    const TempFile file{"audit:\n  target: stdout\n", "daemon-config"};
    const Config config = Config::load(file.getPath());
    ctx.check(endsWith(config.getPolicyPath(), "/policy.rego"), "policy_path defaults under the conf dir");
    ctx.check(config.getNodeId() == 0, "node_id defaults to 0");
}

void refuseInlinePolicy(fsdaemon::probe::Context& ctx)
{
    const TempFile file{"policy:\n  package: x\n", "daemon-config"};
    bool threw = false;
    std::string message;
    try
    {
        static_cast<void>(Config::load(file.getPath()));
    }
    catch (const std::exception& error)
    {
        threw = true;
        message = error.what();
    }
    ctx.check(threw && message.find("inline 'policy:'") != std::string::npos,
              "an inline policy block is refused");
}

void checkOverrides(fsdaemon::probe::Context& ctx)
{
    const TempFile file{
        "policy_path: /custom/policy.rego\n"
        "identity_rules_path: /custom/rules.yaml\n"
        "node_id: 5\n",
        "daemon-config"};
    const Config config = Config::load(file.getPath());
    ctx.check(config.getPolicyPath() == "/custom/policy.rego", "policy_path is overridable");
    ctx.check(config.getIdentityRulesPath() == "/custom/rules.yaml", "identity_rules_path is overridable");
    ctx.check(config.getNodeId() == 5, "node_id is overridable");
    ctx.check(config.getChannelPath() == fsdaemon::name::channelPathForNode(5), "the device path follows node_id");
}

void checkNodeIdOverride(fsdaemon::probe::Context& ctx)
{
    const TempFile file{"node_id: 2\n", "daemon-config"};
    const Config config = Config::load(file.getPath(), 7);
    ctx.check(config.getNodeId() == 7, "a node-id override replaces the file's node_id");
}

void checkDurationUnits(fsdaemon::probe::Context& ctx)
{
    const TempFile file{"upcall:\n  request_timeout: 250ms\n", "daemon-config"};
    const Config config = Config::load(file.getPath());
    ctx.checkf(config.getRequestTimeout() == std::chrono::milliseconds{250},
               "250ms parses to a 250ms budget (%lld)",
               static_cast<long long>(config.getRequestTimeout().count()));
}

void checkChannelPathOverride(fsdaemon::probe::Context& ctx)
{
    const TempFile named{"node_id: 3\nchannel_path: /dev/somewhere-else\n", "daemon-config"};
    ctx.check(Config::load(named.getPath()).getChannelPath() == "/dev/somewhere-else",
              "channel_path wins over the path node_id would name");

    const TempFile derived{"node_id: 3\n", "daemon-config"};
    ctx.check(Config::load(derived.getPath()).getChannelPath() == fsdaemon::name::channelPathForNode(3),
              "without it the path follows node_id");
}

void checkTraceSwitch(fsdaemon::probe::Context& ctx)
{
    const TempFile asked{"upcall:\n  trace_turns: true\n", "daemon-config"};
    ctx.check(Config::load(asked.getPath()).isTracingStages(), "upcall.trace_turns reads");

    const TempFile bare{"audit:\n  target: stdout\n", "daemon-config"};
    ctx.check(!Config::load(bare.getPath()).isTracingStages(), "it is off unless a config asks");
}

void checkWorkerCount(fsdaemon::probe::Context& ctx)
{
    const TempFile file{"upcall:\n  workers: 8\n", "daemon-config"};
    ctx.check(Config::load(file.getPath()).getWorkerCount() == 8, "upcall.workers reads");

    const TempFile bare{"audit:\n  target: stdout\n", "daemon-config"};
    ctx.check(Config::load(bare.getPath()).getWorkerCount() == 4, "it defaults to 4");

    const TempFile none{"upcall:\n  workers: 0\n", "daemon-config"};
    bool threw = false;
    try
    {
        static_cast<void>(Config::load(none.getPath()));
    }
    catch (const std::exception&)
    {
        threw = true;
    }
    ctx.check(threw, "zero workers is refused, since a lock would never be answered");
}

void checkBackendsBlock(fsdaemon::probe::Context& ctx)
{
    const TempFile file{"backends:\n  identity: spire\n  policy: opa\n", "daemon-config"};
    const Config config = Config::load(file.getPath());
    ctx.check(config.getIdentityBackend() == "spire", "identity backend reads spire");
    ctx.check(config.getPolicyBackend() == "opa", "policy backend reads opa");
}

// A mount table as the kernel writes one: two nodes of this filesystem, and a mount that is not
// ours at all.
std::string makeMountTable()
{
    const std::string fsName{fsdaemon::name::FsName};
    return "/dev/sda1 / ext4 rw,relatime 0 0\n"
           "none /mnt/one " +
           fsName + " rw,relatime,node_id=1,daxdev=/dev/dax0.0 0 0\n" + "none /mnt/eleven " + fsName +
           " rw,relatime,node_id=11,daxdev=/dev/dax0.0 0 0\n";
}

void checkNodeRegionLookup(fsdaemon::probe::Context& ctx)
{
    using fsdaemon::config::internal::findRegionForNode;
    const TempFile mounts{makeMountTable(), "daemon-mounts"};
    const std::string region{FS_LOCK_REGION_NAME};

    ctx.check(findRegionForNode(1, mounts.getPath()) == "file:/mnt/one/" + region,
              "a node's own mount carries its region");
    ctx.check(findRegionForNode(11, mounts.getPath()) == "file:/mnt/eleven/" + region,
              "node 11 is not answered by the node 1 row");
    ctx.check(findRegionForNode(2, mounts.getPath()).empty(),
              "an id no mount claims carries no region");
    ctx.check(findRegionForNode(0, mounts.getPath()).empty(),
              "node 0 is the id a config left unset, and carries no region");
    ctx.check(findRegionForNode(1, "/nonexistent/mounts").empty(),
              "no mount table at all carries no region");
}

// The region sizing the mount helper asks this daemon for, since it cannot read this config itself.
void checkRegionSizing(fsdaemon::probe::Context& ctx)
{
    const TempFile bare{"audit:\n  target: stdout\n", "daemon-config"};
    const Config fallback = Config::load(bare.getPath());
    ctx.check(fallback.getTurnMaxPeers() == 8, "max_peers defaults to 8");
    ctx.check(fallback.getTurnMaxDomains() == 4,
              "max_domains defaults to 4, which leaves room beside cme's own");
    ctx.check(fallback.getTurnStrategy() == "peterson", "the strategy defaults to peterson");

    const TempFile named{"turn_region:\n  max_peers: 16\n  max_domains: 32\n  strategy: ticket\n",
                         "daemon-config"};
    const Config config = Config::load(named.getPath());
    ctx.check(config.getTurnMaxPeers() == 16, "max_peers reads");
    ctx.check(config.getTurnMaxDomains() == 32, "max_domains reads");
    ctx.check(config.getTurnStrategy() == "ticket", "the strategy reads");
}

void checkNamedRegionPriority(fsdaemon::probe::Context& ctx)
{
    const TempFile file{"turn_region:\n  uri: file:/somewhere/else.region\n", "daemon-config"};
    ctx.check(Config::load(file.getPath(), 1).getTurnUri() == "file:/somewhere/else.region",
              "a config naming a region wins over the mount the daemon runs on");
}

}  // namespace

int main()
{
    return fsdaemon::probe::runProbe(
        [](fsdaemon::probe::Context& ctx)
        {
            checkMinimalLoad(ctx);
            checkNodeRegionLookup(ctx);
            checkNamedRegionPriority(ctx);
            checkRegionSizing(ctx);
            refuseInlinePolicy(ctx);
            checkOverrides(ctx);
            checkNodeIdOverride(ctx);
            checkDurationUnits(ctx);
            checkBackendsBlock(ctx);
            checkWorkerCount(ctx);
            checkTraceSwitch(ctx);
            checkChannelPathOverride(ctx);
        });
}
