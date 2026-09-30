// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// serve/loop.hpp -- the main loop: wait on the channel, answer what arrives, until told to stop.
//
// One epoll covers the channel and the two eventfds a signal pokes. A pass reads what the channel
// has ready into whole frames and hands each to the handler, keeps reading for a short while in
// case the next request is already on its way, then sleeps again. stop() and requestReload() are
// the two pokes another thread or a signal handler makes; both only wake the wait, so a handler may
// call them.

#pragma once

#include <atomic>

#include "posix/epoll.hpp"
#include "posix/event_fd.hpp"

namespace fsdaemon::serve
{

class FsChannel;
class RequestHandler;

class ServeLoop
{
public:
    ServeLoop(FsChannel& channel, RequestHandler& handler);

    ServeLoop(const ServeLoop&) = delete;
    ServeLoop& operator=(const ServeLoop&) = delete;

    // Turns until stop(), or until the channel reports end of stream, which is how a killed kernel
    // side or a closed test socket ends it.
    void run();

    // Ends the loop from another thread or a signal handler.
    void stop() noexcept;

    // Asks the loop to re-read the identity backend at the top of its next pass, on a SIGHUP.
    void requestReload() noexcept;

private:
    // Reads and answers what the channel holds, and keeps reading for one budget after an answer.
    void serveChannel();

    FsChannel& channel_;
    RequestHandler& handler_;
    posix::EventFd stopSignal_;
    posix::EventFd reloadSignal_;
    posix::Epoll epoll_;
    std::atomic<bool> running_{true};
    // Set alongside reloadSignal_'s poke and cleared once handled, so a serveChannel() pass mid
    // request storm can notice a pending reload without waiting for the channel to fall idle.
    std::atomic<bool> reloadPending_{false};
};

}  // namespace fsdaemon::serve
