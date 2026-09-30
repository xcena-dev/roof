// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// tests/serve_lock_test.cpp -- LOCK and UNLOCK through the serve loop and the worker pool.
//
// The handler routes a lock to the pool so the wait runs off the loop, then writes the response. A
// file-backed cme region stands in for the medium; the kernel and the channel are never touched.

#include <cerrno>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "cme/shared.hpp"
#include "harness/probe.hpp"
#include "harness/socket_pair.hpp"
#include "harness/temp_file.hpp"
#include "harness/wire_io.hpp"
#include "identity/internal/local.hpp"
#include "observe/stat.hpp"
#include "serve/channel.hpp"
#include "serve/handler.hpp"
#include "serve/loop.hpp"
#include "serve/worker_pool.hpp"
#include "turn/internal/cme_turn.hpp"
#include "wire/codec.hpp"
#include "wire/protocol.hpp"

namespace
{

using fsdaemon::FsDaemonLockRequest;
using fsdaemon::FsDaemonLockResponse;
using fsdaemon::FsDaemonUnlockRequest;
using fsdaemon::probe::readResponse;
using fsdaemon::probe::SocketPair;
using fsdaemon::probe::TempFile;
using fsdaemon::probe::writeFrame;
using fsdaemon::serve::FsChannel;
using fsdaemon::serve::RequestHandler;
using fsdaemon::serve::ServeLoop;
using fsdaemon::serve::Services_t;
using fsdaemon::serve::WorkerPool;
using fsdaemon::wire::writeName;

std::string makeRegion(const std::string& path)
{
    const std::string uri = std::string{"file:"} + path;
    cme::Session::FormatOpts_t opts;
    opts.maxPeers = 4;
    opts.maxDomains = 4;
    cme::Session::format(uri, opts);
    return uri;
}

// The loop runs on its own thread, so a frame it has read is not yet a frame it has served.
bool releasedWithin(fsdaemon::turn::CmeTurn& node, const std::string& domain, std::chrono::milliseconds within)
{
    const auto deadline = std::chrono::steady_clock::now() + within;
    while (node.holds(domain))
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

void checkLockUnlockLoop(fsdaemon::probe::Context& ctx)
{
    SocketPair pair;
    if (!ctx.check(pair.isOpen(), "a socketpair stands in for the channel"))
    {
        return;
    }

    const TempFile region{"", "daemon-serve-turn"};
    const std::string uri = makeRegion(region.getPath());
    fsdaemon::turn::CmeTurn turn{uri, cme::CoherencyMode::CacheCoherent, std::vector<std::string>{"turn.d"}};

    fsdaemon::identity::LocalIdentityProvider identity{"", std::vector<std::string>{}};
    WorkerPool pool{2};
    Services_t services{identity};
    services.turn = &turn;
    services.pool = &pool;
    RequestHandler handler{services};
    ctx.check((handler.getCapabilities() & FS_DAEMON_CAP_LOCK) != 0, "HELLO advertises the lock capability");

    auto channel = FsChannel::makeChannel(pair.releaseTheirs());
    ServeLoop loop{channel, handler};
    std::thread runner{[&loop]
                       {
                           loop.run();
                       }};

    FsDaemonLockRequest lock{};
    writeName(lock.domain, "turn.d");
    lock.timeout_ms = 1000;
    writeFrame(pair.getMine(), FS_DAEMON_MSG_LOCK_REQUEST, 41, lock);
    const auto lockBack = readResponse<FsDaemonLockResponse>(pair.getMine(), FS_DAEMON_MSG_LOCK_RESPONSE);
    ctx.check(lockBack.has_value() && lockBack->payload.status == 0, "a lock through the loop is granted");
    ctx.check(turn.holds("turn.d"), "the node now holds the domain");

    // An unlock has no response to wait on, so the release shows in the turn service alone.
    FsDaemonUnlockRequest unlock{};
    writeName(unlock.domain, "turn.d");
    writeFrame(pair.getMine(), FS_DAEMON_MSG_UNLOCK_REQUEST, 42, unlock);
    ctx.check(releasedWithin(turn, "turn.d", std::chrono::seconds{1}), "the domain is given back");

    loop.stop();
    runner.join();
}

// One try spends a slice and can miss on a loaded machine, so a case that needs the domain held
// asks the way the loop does, until the deadline.
bool holdWithin(fsdaemon::turn::CmeTurn& node, const std::string& domain, std::chrono::milliseconds within)
{
    const auto deadline = std::chrono::steady_clock::now() + within;
    do
    {
        if (node.tryAcquireHere(domain))
        {
            return true;
        }
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

// A domain another node holds goes to the waiter, which answers once the deadline passes. That
// answer is the one the recorder has to fold and count: the loop only handed it on.
void checkWaitedLockRecorded(fsdaemon::probe::Context& ctx)
{
    SocketPair pair;
    if (!ctx.check(pair.isOpen(), "a socketpair stands in for the channel"))
    {
        return;
    }

    const TempFile region{"", "daemon-serve-turn"};
    const std::string uri = makeRegion(region.getPath());
    const std::vector<std::string> domains{"turn.d"};
    fsdaemon::turn::CmeTurn other{uri, cme::CoherencyMode::CacheCoherent, domains};
    fsdaemon::turn::CmeTurn turn{uri, cme::CoherencyMode::CacheCoherent, domains};
    if (!ctx.check(holdWithin(other, "turn.d", std::chrono::seconds{2}), "another node holds the domain"))
    {
        return;
    }

    fsdaemon::identity::LocalIdentityProvider identity{"", std::vector<std::string>{}};
    WorkerPool pool{2};
    fsdaemon::observe::Stat stat{true};
    Services_t services{identity};
    services.turn = &turn;
    services.pool = &pool;
    services.stat = &stat;
    RequestHandler handler{services};

    auto channel = FsChannel::makeChannel(pair.releaseTheirs());
    ServeLoop loop{channel, handler};
    std::thread runner{[&loop]
                       {
                           loop.run();
                       }};

    FsDaemonLockRequest lock{};
    writeName(lock.domain, "turn.d");
    lock.timeout_ms = 200;
    writeFrame(pair.getMine(), FS_DAEMON_MSG_LOCK_REQUEST, 43, lock);
    const auto back = readResponse<FsDaemonLockResponse>(pair.getMine(), FS_DAEMON_MSG_LOCK_RESPONSE);
    ctx.check(back.has_value() && back->payload.status == -EBUSY, "a lock another node holds times out as busy");
    ctx.check(stat.getFoldCount("lock_acquire") == 1, "the wait is folded into lock_acquire once");
    ctx.check(stat.getAnswerCount() == 1, "the waited answer is counted once");

    loop.stop();
    runner.join();
    ctx.check(other.release("turn.d"), "the other node gives the domain back");
}

}  // namespace

int main()
{
    return fsdaemon::probe::runProbe(
        [](fsdaemon::probe::Context& ctx)
        {
            checkLockUnlockLoop(ctx);
            checkWaitedLockRecorded(ctx);
        });
}
