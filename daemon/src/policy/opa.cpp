// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// policy/opa.cpp -- see internal/opa.hpp.

#include "policy/internal/opa.hpp"

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "model.hpp"
#include "policy/base.hpp"
#include "util/text.hpp"

namespace fsdaemon::policy
{

namespace
{

// The {"input": {...}} document a decision POSTs. The server reads its input from a top-level
// "input" member, so the shared input document is wrapped in one before it goes out.
std::string buildRequestBody(const fsdaemon::Identity_t& consumer, std::string_view ownerGroup,
                             std::string_view ownerRole)
{
    return R"({"input":)" + buildPolicyInput(consumer, ownerGroup, ownerRole) + "}";
}

// The member OPA wraps its decision under, which is what the reader descends into.
constexpr std::string_view ResultKey = "result";

}  // namespace

OpaPolicyEngine::OpaPolicyEngine(std::string url, OpaTransport transport)
    : url_{std::move(url)},
      transport_{std::move(transport)}
{
    const auto http = util::hasPrefix(url_, "http://");
    const auto https = util::hasPrefix(url_, "https://");
    if (!http && !https)
    {
        throw std::invalid_argument{"OPA url must be http:// or https://, got " + url_};
    }
}

Decision_t OpaPolicyEngine::decide(const fsdaemon::Identity_t& consumer, std::string_view ownerGroup,
                                   std::string_view ownerRole)
{
    const auto body = buildRequestBody(consumer, ownerGroup, ownerRole);
    std::string response;
    try
    {
        response = transport_(url_, body);
    }
    catch (const OpaTransportError& down)
    {
        return makeDenial(std::string{"OPA unreachable: "} + down.what());
    }
    return readDecision(response, ResultKey);
}

void OpaPolicyEngine::warm(const std::vector<fsdaemon::Identity_t>& /*identities*/)
{
}

void OpaPolicyEngine::reload()
{
}

}  // namespace fsdaemon::policy
