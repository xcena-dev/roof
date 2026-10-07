// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// tests/config_test.cpp -- config.yaml parsing, its list blocks and the account it names included.

#include "config/config.hpp"

#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <exception>
#include <optional>
#include <string>
#include <vector>

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

// Whether loading the file at @path throws.
bool refusesToLoadFile(const std::string& path)
{
    try
    {
        static_cast<void>(Config::load(path));
    }
    catch (const std::exception&)
    {
        return true;
    }
    return false;
}

// Whether loading @content throws, so a case asserts a refusal in one line.
bool refusesToLoad(const std::string& content)
{
    const TempFile file{content, "daemon-config"};
    return refusesToLoadFile(file.getPath());
}

void checkMinimalLoad(fsdaemon::probe::Context& ctx)
{
    const TempFile file{"audit:\n  target: stdout\n", "daemon-config"};
    const Config config = Config::load(file.getPath());
    const std::string fsName{fsdaemon::name::FsName};
    ctx.check(endsWith(config.getPolicyPath(), "/policy.rego"), "policy.path defaults under the conf dir");
    ctx.check(config.getIdentityRulesPath() == file.getPath(), "the rules are read from the config itself");
    ctx.check(config.getNodeId() == 0, "the node id is the unit's to give");
    ctx.check(config.findSetting("accounts.daemon") == std::vector<std::string>{fsName},
              "accounts.daemon defaults to the filesystem's name");
    ctx.check(config.getTrustDomain() == fsName + ".local", "the trust domain defaults under that name");
    ctx.check(config.findSetting("mounts") == std::vector<std::string>{}, "a config without mounts lists none");
}

void checkPolicyPackage(fsdaemon::probe::Context& ctx)
{
    const TempFile file{"policy:\n  package: acme.authz\n  path: /custom/policy.rego\n", "daemon-config"};
    const Config config = Config::load(file.getPath());
    ctx.check(config.getPolicyQuery() == "acme.authz", "policy.package is the query");
    ctx.check(endsWith(config.getPolicyUrl(), "/v1/data/acme/authz"), "and the OPA path, with slashes");
    ctx.check(config.getPolicyPath() == "/custom/policy.rego", "policy.path is overridable");
}

void checkNodeIdOverride(fsdaemon::probe::Context& ctx)
{
    const TempFile file{"audit:\n  target: stdout\n", "daemon-config"};
    const Config config = Config::load(file.getPath(), 7);
    ctx.check(config.getNodeId() == 7, "the node-id override is the node served");
    ctx.check(config.getChannelPath() == fsdaemon::name::channelPathForNode(7), "the device path follows it");
}

void checkDurationUnits(fsdaemon::probe::Context& ctx)
{
    const TempFile file{"upcall:\n  request_timeout: 250ms\n", "daemon-config"};
    const Config config = Config::load(file.getPath());
    ctx.checkf(config.getRequestTimeout() == std::chrono::milliseconds{250},
               "250ms parses to a 250ms budget (%lld)",
               static_cast<long long>(config.getRequestTimeout().count()));
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

    ctx.check(refusesToLoad("upcall:\n  workers: 0\n"), "zero workers is refused, since a lock would never be answered");
}

void checkBackends(fsdaemon::probe::Context& ctx)
{
    const TempFile file{"identity:\n  backend: spire\npolicy:\n  backend: opa\n", "daemon-config"};
    const Config config = Config::load(file.getPath());
    ctx.check(config.getIdentityBackend() == "spire", "identity.backend reads spire");
    ctx.check(config.getPolicyBackend() == "opa", "policy.backend reads opa");
}

// The two list blocks sit between maps of scalars, so a key after them shows the maps still read.
std::string makeBlockedConfig()
{
    return "accounts:\n"
           "  daemon: someone\n"
           "mounts:\n"
           "  - point: /mnt/one\n"
           "    device: /dev/dax0.0\n"
           "  - point: /mnt/two   # a second device\n"
           "    device: /dev/dax1.0\n"
           "rules:\n"
           "  - match:\n"
           "      uid: 1004\n"
           "    identity:\n"
           "      group: prod\n"
           "audit:\n"
           "  target: stderr\n";
}

void checkListBlocks(fsdaemon::probe::Context& ctx)
{
    const TempFile file{makeBlockedConfig(), "daemon-config"};
    const Config config = Config::load(file.getPath());
    const std::vector<std::string> mounts{"/mnt/one /dev/dax0.0", "/mnt/two /dev/dax1.0"};
    ctx.check(config.findSetting("mounts") == mounts,
              "each mount keeps its own device, and a comment is not part of the point");
    ctx.check(config.getAuditTarget() == "stderr", "a map after the list blocks still reads");
    ctx.check(config.findSetting("accounts.daemon") == std::vector<std::string>{"someone"},
              "and so does one before them");
}

void refuseBadMounts(fsdaemon::probe::Context& ctx)
{
    ctx.check(refusesToLoad("mounts:\n  - point: /mnt/one\n"), "a mount without a device is refused");
    ctx.check(refusesToLoad("mounts:\n  - point: /mnt/one\n    devise: /dev/dax0.0\n"),
              "a misspelled mount field is refused");
    ctx.check(refusesToLoad("mounts:\n  - point: /mnt/one\n    device: /dev/dax0.0\n"
                            "  - point: /mnt/one\n    device: /dev/dax1.0\n"),
              "a point listed twice is refused");
}

void refuseUntrustedFile(fsdaemon::probe::Context& ctx)
{
    const TempFile file{"audit:\n  target: stdout\n", "daemon-config"};
    constexpr ::mode_t GroupWritable = 0620;
    ctx.check(::chmod(file.getPath().c_str(), GroupWritable) == 0, "the test can loosen its own file");
    ctx.check(refusesToLoadFile(file.getPath()), "a config its group can write is refused");
}

void checkFindSetting(fsdaemon::probe::Context& ctx)
{
    const TempFile file{makeBlockedConfig(), "daemon-config"};
    const Config config = Config::load(file.getPath());

    const auto mounts = config.findSetting("mounts");
    ctx.check(mounts && mounts->size() == 2 && (*mounts)[0] == "/mnt/one /dev/dax0.0",
              "a mount prints as its point and its device on one line");
    const auto account = config.findSetting("accounts.daemon");
    ctx.check(account && account->size() == 1 && account->front() == "someone", "a scalar prints as one line");
    const auto port = config.findSetting("spire.server_port");
    ctx.check(port && port->front() == "8081", "a default prints like a written value");
    ctx.check(!config.findSetting("audit.target"), "a key no script reads is not offered");
}

// Why checkReloadable refuses @edit over a daemon that loaded makeBlockedConfig, or nullopt when it
// takes the edit.
std::optional<std::string> findReloadRefusal(const std::string& edit)
{
    const TempFile file{makeBlockedConfig(), "daemon-config"};
    const Config running = Config::load(file.getPath());
    file.rewrite(edit);
    try
    {
        running.checkReloadable();
    }
    catch (const std::exception& error)
    {
        return error.what();
    }
    return std::nullopt;
}

void checkReloadableEdits(fsdaemon::probe::Context& ctx)
{
    const auto rulesEdit = findReloadRefusal(makeBlockedConfig() + "# a comment\n");
    ctx.check(!rulesEdit, "an edit no running key reads is taken");

    const auto restartEdit = findReloadRefusal(makeBlockedConfig() + "upcall:\n  workers: 7\n");
    ctx.check(restartEdit && restartEdit->find("upcall.workers") != std::string::npos,
              "an edit to a key the daemon was built from is refused, and named");

    const auto brokenEdit = findReloadRefusal("audit:\n\ttarget: stdout\n");
    ctx.check(brokenEdit.has_value(), "an edit that will not parse is refused");
}

std::string makeAccountConfig(const std::string& account)
{
    return "accounts:\n  daemon: " + account + "\n";
}

// Whether requireDaemonAccount accepts a config owned by this test that names @account.
bool acceptsAccount(const std::string& account)
{
    const TempFile file{makeAccountConfig(account), "daemon-config"};
    try
    {
        Config::load(file.getPath()).requireDaemonAccount();
    }
    catch (const std::exception&)
    {
        return false;
    }
    return true;
}

void checkDaemonAccount(fsdaemon::probe::Context& ctx)
{
    const auto* const self = ::getpwuid(::geteuid());
    ctx.check(self != nullptr && acceptsAccount(self->pw_name),
              "the account this daemon runs as, owning the file, is accepted");
    ctx.check(!acceptsAccount("no-such-account-here"), "an account that does not exist is refused");
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

// The region sizing the mount helper asks this daemon for.
void checkRegionSizing(fsdaemon::probe::Context& ctx)
{
    const TempFile bare{"audit:\n  target: stdout\n", "daemon-config"};
    const Config fallback = Config::load(bare.getPath());
    ctx.check(fallback.getTurnMaxPeers() == 8, "max_peers defaults to 8");
    ctx.check(fallback.getTurnMaxDomains() == 4,
              "max_domains defaults to 4, which leaves room beside cme's own");
    ctx.check(fallback.getTurnStrategy() == "peterson", "the strategy defaults to peterson");

    const TempFile named{"turn:\n  max_peers: 16\n  max_domains: 32\n  strategy: ticket\n", "daemon-config"};
    const Config config = Config::load(named.getPath());
    ctx.check(config.getTurnMaxPeers() == 16, "max_peers reads");
    ctx.check(config.getTurnMaxDomains() == 32, "max_domains reads");
    ctx.check(config.getTurnStrategy() == "ticket", "the strategy reads");
}

void checkNamedRegionPriority(fsdaemon::probe::Context& ctx)
{
    const TempFile file{"turn:\n  uri: file:/somewhere/else.region\n", "daemon-config"};
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
            checkPolicyPackage(ctx);
            checkNodeRegionLookup(ctx);
            checkNamedRegionPriority(ctx);
            checkRegionSizing(ctx);
            checkNodeIdOverride(ctx);
            checkDurationUnits(ctx);
            checkBackends(ctx);
            checkWorkerCount(ctx);
            checkTraceSwitch(ctx);
            checkListBlocks(ctx);
            refuseBadMounts(ctx);
            refuseUntrustedFile(ctx);
            checkFindSetting(ctx);
            checkReloadableEdits(ctx);
            checkDaemonAccount(ctx);
        });
}
