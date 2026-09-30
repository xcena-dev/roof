// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// posix/notify.hpp -- telling the process supervisor that this daemon is serving.
//
// The unit is Type=notify, so systemd holds the start until this arrives. That is what makes the
// installer's is-active check mean the channel is open rather than only that a process forked.

#pragma once

namespace fsdaemon::posix
{

// Sends READY=1 to the socket $NOTIFY_SOCKET names, and answers whether it went. A hand run has no
// supervisor and no such variable, which is not a failure and answers true.
[[nodiscard]] bool sendReady() noexcept;

}  // namespace fsdaemon::posix
