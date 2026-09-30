// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// policy/internal/opa_transport.hpp -- the libcurl transport the OPA backend POSTs a decision request over.
//
// Held apart from the backend registration so a probe drives the real HTTP exchange against a
// server it stands up itself, without linking the registry or the config the registration reads.

#pragma once

#include <chrono>
#include <string>

#include "policy/internal/opa.hpp"

namespace fsdaemon::policy
{

// POSTs application/json with @timeout for the whole exchange. A non-empty @unixSocketPath is what
// the request travels over, and the url's host then only fills the Host header.
[[nodiscard]] OpaTransport makeCurlTransport(std::chrono::milliseconds timeout,
                                             std::string unixSocketPath);

}  // namespace fsdaemon::policy
