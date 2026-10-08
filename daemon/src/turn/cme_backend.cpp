// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// turn/cme_backend.cpp -- the lock service over a cme region.
//
// A region the daemon cannot open is a startup failure. Serving without the lock would leave the
// mount looking healthy while every metadata write on it is refused.

#include <memory>
#include <string>
#include <vector>

#include "backends.hpp"
#include "config/config.hpp"
#include "turn/internal/cme_turn.hpp"
#include "turn/service.hpp"
#include "wire/protocol.hpp"

namespace
{

const auto RegisteredCmeTurn = fsdaemon::registerTurnBackend(
    "cme",
    [](const fsdaemon::config::Config& config) -> std::unique_ptr<fsdaemon::turn::TurnService>
    {
        return std::make_unique<fsdaemon::turn::CmeTurn>(
            config.getTurnUri(), std::vector<std::string>{std::string{FS_DOMAIN_META}});
    });

}  // namespace
