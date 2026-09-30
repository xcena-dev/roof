// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// serve/loop.cpp -- see loop.hpp.

#include "serve/loop.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>

#include "serve/channel.hpp"
#include "serve/handler.hpp"

namespace fsdaemon::serve
{

namespace
{

// How many ready descriptors one wait reports at most. Three go in, so this is room over rather
// than a limit: a wait that filled it would simply report the rest on the next pass.
constexpr std::uint32_t ReadyAtOnce = 8;

// How long the loop keeps reading after an answer before it sleeps. Waking a sleeping daemon is the
// largest single span of an upcall, and a create sends three in a row, so a request that arrives in
// this window is answered without that wake. One that does not costs this much of an idle core.
constexpr auto SpinAfterAnswer = std::chrono::microseconds{25};

}  // namespace

ServeLoop::ServeLoop(FsChannel& channel, RequestHandler& handler)
    : channel_{channel},
      handler_{handler},
      epoll_{ReadyAtOnce}
{
    epoll_.watchReadable(channel_.getDescriptor());
    epoll_.watchReadable(stopSignal_.getDescriptor());
    epoll_.watchReadable(reloadSignal_.getDescriptor());
}

void ServeLoop::run()
{
    while (running_.load(std::memory_order_acquire))
    {
        // No timeout: every reason this loop has to act arrives as one of the three descriptors it
        // watches, and a stop pokes its own. A periodic wake would find nothing to do.
        for (const auto readyDescriptor : epoll_.wait())
        {
            if (stopSignal_.consumeWake(readyDescriptor))
            {
                running_.store(false, std::memory_order_release);
                continue;
            }
            if (reloadSignal_.consumeWake(readyDescriptor))
            {
                reloadPending_.store(false, std::memory_order_release);
                handler_.reloadBackends();
                continue;
            }
            if (readyDescriptor == channel_.getDescriptor())
            {
                serveChannel();
            }
        }
    }
}

void ServeLoop::serveChannel()
{
    auto deadline = std::chrono::steady_clock::now() + SpinAfterAnswer;
    do
    {
        const auto frames = channel_.readReady();
        for (const auto& frame : frames)
        {
            handler_.handle(channel_, frame);
        }
        if (channel_.isEndOfStream())
        {
            running_.store(false, std::memory_order_release);
            return;
        }
        if (!frames.empty())
        {
            deadline = std::chrono::steady_clock::now() + SpinAfterAnswer;
        }
    } while (running_.load(std::memory_order_acquire) && !reloadPending_.load(std::memory_order_acquire) &&
             std::chrono::steady_clock::now() < deadline);
}

void ServeLoop::stop() noexcept
{
    running_.store(false, std::memory_order_release);
    stopSignal_.poke();
}

void ServeLoop::requestReload() noexcept
{
    reloadPending_.store(true, std::memory_order_release);
    reloadSignal_.poke();
}

}  // namespace fsdaemon::serve
