// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// daemon/backends.cpp -- see backends.hpp.

#include "backends.hpp"

#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "config/config.hpp"
#include "identity/base.hpp"
#include "policy/base.hpp"
#include "turn/service.hpp"

namespace fsdaemon
{

namespace
{

// Function-local, so the map exists before the first registration runs whatever order the
// translation units initialize in.
template <typename T>
std::map<std::string, T, std::less<>>& getRegistry()
{
    static std::map<std::string, T, std::less<>> registry;
    return registry;
}

template <typename T>
T findBackend(std::string_view name)
{
    const auto& registry = getRegistry<T>();
    const auto found = registry.find(name);
    return found == registry.end() ? nullptr : found->second;
}

template <typename T>
void appendNames(std::vector<std::string>& names, std::string_view layer)
{
    // The factory half is not read here, only the name it answers to.
    for ([[maybe_unused]] const auto& [name, make] : getRegistry<T>())
    {
        names.push_back(std::string{layer}.append(":").append(name));
    }
}

// The error a name no registration answers to raises. It lists what this build did carry, so a
// deployment tells a backend the build left out from one the config misspelled.
std::runtime_error makeMissingError(std::string_view layer, std::string_view name)
{
    std::string carried;
    for (const auto& carriedName : getBackendNames())
    {
        carried.append(carried.empty() ? "" : ", ").append(carriedName);
    }
    return std::runtime_error{std::string{layer}
                                  .append(" backend '")
                                  .append(name)
                                  .append("' is not in this build (carries: ")
                                  .append(carried)
                                  .append(")")};
}

}  // namespace

bool registerIdentityBackend(std::string name, IdentityFactory make)
{
    getRegistry<IdentityFactory>().emplace(std::move(name), make);
    return true;
}

bool registerPolicyBackend(std::string name, PolicyFactory make)
{
    getRegistry<PolicyFactory>().emplace(std::move(name), make);
    return true;
}

bool registerTurnBackend(std::string name, TurnFactory make)
{
    getRegistry<TurnFactory>().emplace(std::move(name), make);
    return true;
}

std::unique_ptr<identity::IdentityProvider> makeIdentityProvider(const config::Config& config)
{
    const auto make = findBackend<IdentityFactory>(config.getIdentityBackend());
    if (make == nullptr)
    {
        throw makeMissingError("identity", config.getIdentityBackend());
    }
    return make(config);
}

std::unique_ptr<policy::PolicyEngine> makePolicyEngine(const config::Config& config,
                                                       const identity::IdentityProvider& identity)
{
    const auto make = findBackend<PolicyFactory>(config.getPolicyBackend());
    if (make == nullptr)
    {
        throw makeMissingError("policy", config.getPolicyBackend());
    }
    auto engine = make(config);
    if (engine)
    {
        // One pass over every consumer-owner pair here turns the first real request into a cache
        // hit. A backend that enumerates nothing gets an empty list and does nothing with it.
        engine->warm(identity.getKnownIdentities());
    }
    return engine;
}

std::unique_ptr<turn::TurnService> makeTurnService(const config::Config& config)
{
    // Config names a region, not a turn backend, so the build carrying one is the selection.
    const auto& registry = getRegistry<TurnFactory>();
    if (config.getTurnUri().empty() || registry.empty())
    {
        return nullptr;
    }
    return registry.begin()->second(config);
}

std::vector<std::string> getBackendNames()
{
    std::vector<std::string> names;
    appendNames<IdentityFactory>(names, "identity");
    appendNames<PolicyFactory>(names, "policy");
    appendNames<TurnFactory>(names, "turn");
    return names;
}

}  // namespace fsdaemon
