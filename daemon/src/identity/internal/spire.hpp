// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// identity/internal/spire.hpp -- the SPIRE-backed identity backend: a task's selectors to a SVID.
//
// The provider reads the caller's creds, derives the unix selector labels, and asks a fetcher for the
// SVID those labels resolve to. The fetch is a seam: a test supplies a canned one, and the real gRPC
// fetcher to the SPIRE agent is compiled only into a build that carries gRPC.

#pragma once

#include <sys/types.h>

#include <functional>
#include <string>
#include <vector>

#include "identity/base.hpp"
#include "model.hpp"

namespace fsdaemon::identity
{

// Resolves the derived selector labels to a SPIFFE id, or throws BridgeError. The labels are
// unix:<kind>:<value> strings; the fetcher splits and subscribes on them. A test returns a fixed id.
using SvidFetcher = std::function<std::string(const std::vector<std::string>& labels)>;

class SpireIdentityProvider : public IdentityProvider
{
public:
    // @selectorNames is the config list of kinds to derive at attest time, empty for the default
    // set. @fetcher resolves them to a SVID. The SPIRE agent narrows to what its unix attestor
    // knows, so the default set is uid alone.
    SpireIdentityProvider(const std::vector<std::string>& selectorNames, SvidFetcher fetcher);

    [[nodiscard]] Identity_t attest(pid_t pid, const Creds_t& facts) override;

    // A directory server does not name a finite identity set, so this stays the empty default.
    // A round trip to the SPIRE agent, so the answer waits on a socket.
    [[nodiscard]] bool needsWorker() const noexcept override
    {
        return true;
    }

    void reload() override;

private:
    SvidFetcher fetcher_;
};

}  // namespace fsdaemon::identity
