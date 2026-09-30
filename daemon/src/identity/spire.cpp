// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// identity/spire.cpp -- see internal/spire.hpp.

#include "identity/internal/spire.hpp"

#include <sys/types.h>

#include <string>
#include <utility>
#include <vector>

#include "identity/base.hpp"
#include "identity/internal/selector.hpp"
#include "identity/internal/spiffe.hpp"
#include "model.hpp"

namespace fsdaemon::identity
{

namespace
{

// Every label the chosen kinds derive from @creds, in the order config named them. The agent
// matches on these, so a caller sends them as one list.
std::vector<std::string> deriveLabels(const Selector& selectors, const Creds_t& creds)
{
    std::vector<std::string> labels;
    for (const auto kind : selectors.getKinds())
    {
        const auto derived = Selector::deriveKindLabels(kind, creds);
        labels.insert(labels.end(), derived.begin(), derived.end());
    }
    return labels;
}

}  // namespace

SpireIdentityProvider::SpireIdentityProvider(const std::vector<std::string>& selectorNames, SvidFetcher fetcher)
    : IdentityProvider{selectorNames},
      fetcher_{std::move(fetcher)}
{
}

Identity_t SpireIdentityProvider::attest(pid_t pid, const Creds_t& facts)
{
    const auto creds = resolveCreds(pid, facts);
    if (!creds)
    {
        throw BridgeError{"no identity for pid=" + std::to_string(pid) + ": /proc read failed"};
    }

    const auto labels = deriveLabels(getSelectors(), *creds);
    if (labels.empty())
    {
        throw BridgeError{"no identity for pid=" + std::to_string(pid) + ": no selector labels"};
    }

    // The fetcher throws BridgeError on an agent error or an empty answer, which the attest path
    // turns into a denial, so no other exception type escapes here.
    const auto spiffeId = fetcher_(labels);
    auto [group, role] = parseSpiffePath(spiffeId);
    return Identity_t{spiffeId, std::move(group), std::move(role), labels};
}

void SpireIdentityProvider::reload()
{
}

}  // namespace fsdaemon::identity
