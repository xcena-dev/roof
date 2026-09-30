// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// policy/internal/opa.hpp -- the OPA REST policy backend: POST an input, read a decision.
//
// The rego runs on an external OPA server, so this backend builds the input, hands it to a transport
// that speaks HTTP, and reads the decision back. The transport is a seam, so a test drives the whole
// decide path with a canned response and the real libcurl transport stays behind DAEMON_ENABLE_OPA.

#pragma once

#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "model.hpp"
#include "policy/base.hpp"

namespace fsdaemon::policy
{

// The transport could not reach the server or the server answered with no usable body. decide turns
// this into a denial rather than letting it escape, so an unreachable OPA is fail-closed.
class OpaTransportError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// POSTs @requestBody to @url and returns the response body, or throws OpaTransportError. A test
// supplies a canned one. The default, behind DAEMON_ENABLE_OPA, speaks HTTP with libcurl.
using OpaTransport = std::function<std::string(const std::string& url, const std::string& requestBody)>;

class OpaPolicyEngine : public PolicyEngine
{
public:
    // @url must be http:// or https://, or the ctor throws std::invalid_argument. @transport carries
    // the request; the request assembly and response reading do not depend on it.
    OpaPolicyEngine(std::string url, OpaTransport transport);

    [[nodiscard]] Decision_t decide(const fsdaemon::Identity_t& consumer, std::string_view ownerGroup,
                                    std::string_view ownerRole) override;

    // OPA holds no local cache to precompute into, so this does nothing.
    void warm(const std::vector<fsdaemon::Identity_t>& identities) override;

    // An HTTP round trip to the policy server, so the answer waits on a socket.
    [[nodiscard]] bool needsWorker() const noexcept override
    {
        return true;
    }

    // OPA owns its own bundle lifecycle over HTTP, so the daemon carries no policy state to refresh.
    void reload() override;

private:
    std::string url_;
    OpaTransport transport_;
};

}  // namespace fsdaemon::policy
