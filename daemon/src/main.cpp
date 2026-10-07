// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// main.cpp -- the daemon: open the channel, announce, and answer the kernel until a signal stops it.
//
// One epoll loop on this thread. A signal ends it cleanly rather than killing the process, so a peer
// slot this daemon holds goes back. The backends come from config by name, so this file names none.

#include <signal.h>  // NOLINT(modernize-deprecated-headers) sigaction is POSIX, not in <csignal>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <string_view>
#include <vector>

#include "audit/audit.hpp"
#include "backends.hpp"
#include "commands.hpp"
#include "config/config.hpp"
#include "observe/stat.hpp"
#include "posix/notify.hpp"
#include "serve/channel.hpp"
#include "serve/handler.hpp"
#include "serve/loop.hpp"
#include "serve/worker_pool.hpp"

namespace
{

// Where a signal handler reaches the loop. Set once before the loop runs and cleared after, so a
// signal landing outside that window finds nothing and does nothing.
std::atomic<fsdaemon::serve::ServeLoop*> g_runningLoop{nullptr};

// SIGHUP asks for a reload and the rest ask the loop to end. Both only poke an eventfd the loop
// already waits on, so either is safe from here.
void onSignal(std::int32_t signalNumber)
{
    auto* const loop = g_runningLoop.load(std::memory_order_acquire);
    if (loop == nullptr)
    {
        return;
    }
    if (signalNumber == SIGHUP)
    {
        loop->requestReload();
        return;
    }
    loop->stop();
}

void registerHandler(std::int32_t signalNumber)
{
    struct ::sigaction action
    {
    };
    action.sa_handler = onSignal;
    ::sigemptyset(&action.sa_mask);
    ::sigaction(signalNumber, &action, nullptr);
}

}  // namespace

int main(int argc, char** argv)
{
    const std::vector<std::string_view> args{argv, argv + argc};
    if (const auto status = fsdaemon::runCommand(args))
    {
        return *status;
    }

    try
    {
        // The effective settings come from config.yaml when --config is given, and from flags
        // otherwise, so a test or a hand run needs no config file.
        const auto nodeOverride = fsdaemon::readNodeId(args);
        const auto configPath = fsdaemon::config::findOptionValue(args, "--config");
        const auto config = configPath.empty()
                                ? fsdaemon::config::Config::fromArgs(args, nodeOverride)
                                : fsdaemon::config::Config::load(configPath, nodeOverride);
        config.requireDaemonAccount();

        const auto identity = fsdaemon::makeIdentityProvider(config);
        const auto policy = fsdaemon::makePolicyEngine(config, *identity);

        fsdaemon::audit::AuditLogger audit{config.getAuditTarget(), config.isLoggingDecisions()};
        fsdaemon::observe::Stat stat{config.isTracingStages()};

        const auto channelPath = config.getChannelPath();
        auto channel = fsdaemon::serve::FsChannel::makeChannel(channelPath);
        channel.observe(&stat);

        // Declared after the channel and the recorder a worker writes through, so its destructor
        // joins every worker while both are still alive. A worker answers when a backend on the
        // path waits on a socket, since the loop has to keep reading while it does.
        fsdaemon::serve::WorkerPool pool{config.getWorkerCount()};

        // After the channel for the same reason: the thread this holds answers a lock it waited for
        // by writing to the channel, so it has to be joined while the channel is still open.
        const auto turn = fsdaemon::makeTurnService(config);

        fsdaemon::serve::Services_t services{*identity};
        services.policy = policy.get();
        services.turn = turn.get();
        services.pool = &pool;
        services.audit = &audit;
        services.stat = &stat;
        if (!config.getSourcePath().empty())
        {
            services.checkSettings = [&config]
            {
                config.checkReloadable();
            };
        }
        fsdaemon::serve::RequestHandler handler{services};
        if (!channel.sendHello(handler.getCapabilities(), static_cast<std::uint32_t>(::getpid())))
        {
            std::fprintf(stderr, "daemon: could not send HELLO on %s\n", channelPath.c_str());
            return 1;
        }

        fsdaemon::serve::ServeLoop loop{channel, handler};
        g_runningLoop.store(&loop, std::memory_order_release);
        registerHandler(SIGTERM);
        registerHandler(SIGINT);
        registerHandler(SIGHUP);

        // After the HELLO and the handlers, so a supervisor that reads ready can already send the
        // daemon a signal and the kernel already knows what it serves.
        if (!fsdaemon::posix::sendReady())
        {
            std::fprintf(stderr, "daemon: could not tell the supervisor it is serving\n");
        }

        loop.run();
        g_runningLoop.store(nullptr, std::memory_order_release);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "daemon: %s\n", error.what());
        return 1;
    }
    return 0;
}
