// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// tests/backends_test.cpp -- the backend registry: a name config spells resolves to what registered
// under it, and a name nothing registered under is refused rather than defaulted.

#include "backends.hpp"

#include <sys/types.h>  // IWYU pragma: keep

#include <algorithm>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "config/config.hpp"
#include "harness/probe.hpp"
#include "identity/base.hpp"
#include "model.hpp"

namespace
{

// Resolves nothing. The registry's job is to hand back what registered, not to run it, so the
// probe registers the cheapest thing the interface allows.
class ProbeProvider : public fsdaemon::identity::IdentityProvider
{
public:
    explicit ProbeProvider(const std::vector<std::string>& selectorNames)
        : IdentityProvider{selectorNames}
    {
    }

    // NOLINTNEXTLINE(misc-include-cleaner) pid_t is <sys/types.h>'s, mapped to a libc inner header
    [[nodiscard]] fsdaemon::Identity_t attest(pid_t, const fsdaemon::Creds_t&) override
    {
        return {};
    }

    void reload() override
    {
    }
};

[[maybe_unused]] const bool RegisteredProbeIdentity = fsdaemon::registerIdentityBackend(
    "probe",
    [](const fsdaemon::config::Config& config) -> std::unique_ptr<fsdaemon::identity::IdentityProvider>
    {
        return std::make_unique<ProbeProvider>(config.getSelectors());
    });

fsdaemon::config::Config settingsNaming(const std::string& identityBackend, const std::string& policyBackend)
{
    return fsdaemon::config::Config::fromArgs(
        {"daemon", "--identity-backend", identityBackend, "--policy-backend", policyBackend});
}

bool threwRuntimeError(const std::function<void()>& call)
{
    try
    {
        call();
    }
    catch (const std::runtime_error&)
    {
        return true;
    }
    return false;
}

void checkRegisteredList(fsdaemon::probe::Context& ctx)
{
    const std::vector<std::string> names = fsdaemon::getBackendNames();
    ctx.check(std::find(names.begin(), names.end(), "identity:probe") != names.end(),
              "a registration shows in the carried list");
}

void checkRegisteredNameResolution(fsdaemon::probe::Context& ctx)
{
    const auto provider = fsdaemon::makeIdentityProvider(settingsNaming("probe", "local"));
    ctx.check(provider != nullptr, "a registered name resolves to its backend");
}

void refuseUnregisteredName(fsdaemon::probe::Context& ctx)
{
    ctx.check(threwRuntimeError([]
                                {
                                    static_cast<void>(fsdaemon::makeIdentityProvider(settingsNaming("nope", "local")));
                                }),
              "an identity name nothing registered under is refused");
    ctx.check(threwRuntimeError(
                  []
                  {
                      const ProbeProvider provider{{}};
                      static_cast<void>(fsdaemon::makePolicyEngine(settingsNaming("probe", "nope"), provider));
                  }),
              "a policy name nothing registered under is refused");
}

void refuseLockWithoutRegion(fsdaemon::probe::Context& ctx)
{
    // fromArgs leaves turn_region.uri empty, so the settings name no region to join.
    ctx.check(fsdaemon::makeTurnService(settingsNaming("probe", "local")) == nullptr,
              "settings naming no region serve no lock");
}

}  // namespace

int main()
{
    return fsdaemon::probe::runProbe(
        [](fsdaemon::probe::Context& ctx)
        {
            checkRegisteredList(ctx);
            checkRegisteredNameResolution(ctx);
            refuseUnregisteredName(ctx);
            refuseLockWithoutRegion(ctx);
        });
}
