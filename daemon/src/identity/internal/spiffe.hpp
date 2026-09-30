// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// identity/internal/spiffe.hpp -- the two directions of a SPIFFE id string.
//
// internal/ means the identity backends and their probes only. A serving layer reads the group and
// role off an Identity_t and never touches the id they came from.

#pragma once

#include <string>
#include <string_view>
#include <utility>

namespace fsdaemon::identity
{

// The (group, role) a SPIFFE id names, or a pair of empty strings when it is not one this daemon
// reads. Either half may be empty on its own: a rule that names only a group leaves the role empty.
[[nodiscard]] std::pair<std::string, std::string> parseSpiffePath(std::string_view spiffeId);

// spiffe://<trustDomain><path>, with a leading slash added to @path when it lacks one. Throws
// BridgeError on an empty trust domain, since an id without one names nothing.
[[nodiscard]] std::string assembleSpiffeId(std::string_view trustDomain, std::string_view path);

}  // namespace fsdaemon::identity
