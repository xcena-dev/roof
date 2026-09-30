// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// identity/spire_backend.cpp -- the name "spire" and the gRPC fetcher it answers with.
//
// Compiles only where gRPC and protobuf do, so a build without them carries no "spire" name and
// refuses a config asking for one. The provider core beside this needs neither, so its mock test
// builds everywhere.

#include <grpcpp/grpcpp.h>  // IWYU pragma: keep
#include <grpcpp/security/credentials.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "backends.hpp"
#include "config/config.hpp"
#include "delegatedidentity.grpc.pb.h"
#include "delegatedidentity.pb.h"
#include "identity/base.hpp"
#include "identity/internal/spiffe.hpp"
#include "identity/internal/spire.hpp"
#include "selector.pb.h"
#include "spiffeid.pb.h"

namespace fsdaemon::identity
{

namespace
{

namespace delegated = spire::api::agent::delegatedidentity::v1;

// The (type, value) a unix selector label splits into on its first colon, e.g. "unix:uid:1000" to
// ("unix", "uid:1000"). An empty half is a malformed label the agent would reject.
std::pair<std::string, std::string> splitLabel(std::string_view label)
{
    const std::uint64_t colon = label.find(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 == label.size())
    {
        throw BridgeError{std::string{"malformed selector label: "}.append(label)};
    }
    return {std::string{label.substr(0, colon)}, std::string{label.substr(colon + 1)}};
}

// Reads the first stream message that carries a SVID and returns its SPIFFE id, or throws. The first
// message can be an empty subscription ack, so this reads until one has a SVID or the stream ends.
std::string readFirstSvid(delegated::DelegatedIdentity::Stub& stub, const std::vector<std::string>& labels,
                          std::chrono::milliseconds timeout)
{
    delegated::SubscribeToX509SVIDsRequest request{};
    for (const auto& label : labels)
    {
        auto [type, value] = splitLabel(label);
        auto* selector = request.add_selectors();
        selector->set_type(type);
        selector->set_value(value);
    }

    grpc::ClientContext context{};  // NOLINT(misc-include-cleaner) grpcpp.h above provides it, since the header that declares it is gRPC's implementation detail
    context.set_deadline(std::chrono::system_clock::now() + timeout);
    const auto reader = stub.SubscribeToX509SVIDs(&context, request);

    delegated::SubscribeToX509SVIDsResponse response{};
    while (reader->Read(&response))
    {
        if (response.x509_svids_size() > 0)
        {
            context.TryCancel();
            const auto& svid = response.x509_svids(0).x509_svid().id();
            return assembleSpiffeId(svid.trust_domain(), svid.path());
        }
    }

    const auto status = reader->Finish();
    if (!status.ok())
    {
        throw BridgeError{"SPIRE agent rpc failed: " + status.error_message()};
    }
    throw BridgeError{"SPIRE agent returned no SVID"};
}

// A fetcher over a persistent channel to @socketPath, the unix socket the agent listens on.
// @timeout bounds the whole subscribe. A SVID whose own trust domain is empty is an error.
SvidFetcher makeGrpcSvidFetcher(std::string_view socketPath, std::chrono::milliseconds timeout)
{
    const auto channel =
        grpc::CreateChannel(std::string{"unix:"}.append(socketPath), grpc::InsecureChannelCredentials());
    const auto stub =
        std::shared_ptr<delegated::DelegatedIdentity::Stub>{delegated::DelegatedIdentity::NewStub(channel)};

    return [stub, timeout](const std::vector<std::string>& labels)
    {
        return readFirstSvid(*stub, labels, timeout);
    };
}

const bool RegisteredSpireIdentity = registerIdentityBackend(
    "spire",
    [](const config::Config& config) -> std::unique_ptr<IdentityProvider>
    {
        return std::make_unique<SpireIdentityProvider>(
            config.getSelectors(), makeGrpcSvidFetcher(config.getSpireSocket(), config.getRequestTimeout()));
    });

}  // namespace

}  // namespace fsdaemon::identity
