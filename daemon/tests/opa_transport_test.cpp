// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// tests/opa_transport_test.cpp -- what the OPA transport reaches, over a server the probe stands up.
//
// The decision path believes whatever answers it, so what a deployment can rely on is where the
// request goes. A named unix socket is a path only root can create; a loopback port is not. These
// cases hold the transport to that: a socket carries the exchange, and naming one keeps the request
// off any port.

#include "policy/internal/opa_transport.hpp"

#include <netinet/in.h>
#include <stdlib.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "harness/probe.hpp"
#include "model.hpp"
#include "policy/base.hpp"
#include "policy/internal/opa.hpp"
#include "posix/unique_fd.hpp"
#include "util/text.hpp"
#include "wire/protocol.hpp"

namespace
{

using fsdaemon::policy::Decision_t;
using fsdaemon::policy::makeCurlTransport;
using fsdaemon::policy::OpaPolicyEngine;

constexpr std::chrono::milliseconds ExchangeTimeout{4000};

// How long the listener waits on one poll before it looks at the stop flag again.
constexpr std::int32_t PollMillis = 50;

// What a server that always allows would answer. The body is the real shape, so a case that gets an
// allow got it through a parse and not through an error path.
constexpr std::string_view AllowBody =
    R"({"result":{"allow":true,"granted_perms":["READ","WRITE"],"rule":"stood-up"}})";

fsdaemon::Identity_t consumerFixture()
{
    fsdaemon::Identity_t who;
    who.spiffeId = "spiffe://test.local/group/eng/role/reader";
    who.group = "eng";
    who.role = "reader";
    return who;
}

// Reads one HTTP request off @client and writes @body back as a 200. Content-Length and not a close
// to delimit: curl reads the length, so the exchange ends without waiting on a half-closed socket.
void serveOnce(fsdaemon::posix::Descriptor client, std::string_view body, std::string* captured)
{
    std::string request;
    char buffer[1024];
    std::uint64_t wanted = 0;
    while (true)
    {
        const auto got = ::recv(client, buffer, sizeof(buffer), 0);
        if (got <= 0)
        {
            break;
        }
        request.append(buffer, static_cast<std::string::size_type>(got));

        const auto headerEnd = request.find("\r\n\r\n");
        if (headerEnd == std::string::npos)
        {
            continue;
        }
        if (wanted == 0)
        {
            const auto named = request.find("Content-Length:");
            wanted = (named == std::string::npos)
                         ? 0
                         : std::strtoull(request.c_str() + named + sizeof("Content-Length:") - 1,
                                         nullptr, 10);
        }
        if (request.size() >= headerEnd + 4 + wanted)
        {
            break;
        }
    }
    *captured = request;

    const std::string answer = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                               std::to_string(body.size()) + "\r\n\r\n" + std::string{body};
    static_cast<void>(::send(client, answer.data(), answer.size(), MSG_NOSIGNAL));
}

// One server that answers a single request and then stops. Both address families, because a case
// needs a live port to show that naming a socket is what takes the request off it.
class OneShotServer
{
public:
    // The listener polls rather than blocking in accept, so a case where nobody ever connects ends
    // on the stop flag instead of waiting on a wakeup the address family may not deliver.
    OneShotServer(fsdaemon::posix::UniqueFd listener, std::string_view body)
        : listener_{std::move(listener)}
    {
        worker_ = std::thread(
            [this, body]()
            {
                while (!stopping_)
                {
                    pollfd waiting{listener_.get(), POLLIN, 0};
                    if (::poll(&waiting, 1, PollMillis) <= 0)
                    {
                        continue;
                    }
                    const fsdaemon::posix::UniqueFd client{::accept(listener_.get(), nullptr, nullptr)};
                    if (client.get() < 0)
                    {
                        continue;
                    }
                    serveOnce(client.get(), body, &request_);
                    served_ = true;
                    return;
                }
            });
    }

    OneShotServer(const OneShotServer&) = delete;
    OneShotServer& operator=(const OneShotServer&) = delete;

    ~OneShotServer()
    {
        stopping_ = true;
        if (worker_.joinable())
        {
            worker_.join();
        }
    }

    [[nodiscard]] bool hasServed() const noexcept
    {
        return served_;
    }

    [[nodiscard]] const std::string& getRequest() const noexcept
    {
        return request_;
    }

private:
    fsdaemon::posix::UniqueFd listener_;
    std::thread worker_;
    std::string request_;
    std::atomic<bool> served_{false};
    std::atomic<bool> stopping_{false};
};

// A directory of this probe's own, unlinked with the socket inside it. mkdtemp and not a fixed name,
// so two runs on one host do not take each other's endpoint.
class TempDir
{
public:
    TempDir()
    {
        if (::mkdtemp(path_.data()) == nullptr)
        {
            path_.clear();
        }
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    ~TempDir()
    {
        if (!path_.empty())
        {
            static_cast<void>(::unlink(socketPath().c_str()));
            static_cast<void>(::rmdir(path_.c_str()));
        }
    }

    [[nodiscard]] std::string socketPath() const
    {
        return path_.empty() ? std::string{} : path_ + "/api.sock";
    }

private:
    std::string path_{"/tmp/daemon-opa-XXXXXX"};
};

[[nodiscard]] fsdaemon::posix::UniqueFd listenOnPath(const std::string& path)
{
    fsdaemon::posix::UniqueFd listener{::socket(AF_UNIX, SOCK_STREAM, 0)};
    if (listener.get() < 0)
    {
        return {};
    }
    sockaddr_un where{};
    where.sun_family = AF_UNIX;
    if (path.size() + 1 > sizeof(where.sun_path))
    {
        return {};
    }
    std::memcpy(where.sun_path, path.c_str(), path.size() + 1);
    if (::bind(listener.get(), reinterpret_cast<const sockaddr*>(&where), sizeof(where)) != 0 ||
        ::listen(listener.get(), 1) != 0)
    {
        return {};
    }
    return listener;
}

// The port the kernel picked, so nothing here reserves a number another process may hold.
[[nodiscard]] fsdaemon::posix::UniqueFd listenOnLoopback(std::uint16_t* port)
{
    fsdaemon::posix::UniqueFd listener{::socket(AF_INET, SOCK_STREAM, 0)};
    if (listener.get() < 0)
    {
        return {};
    }
    sockaddr_in where{};
    where.sin_family = AF_INET;
    where.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    where.sin_port = 0;
    if (::bind(listener.get(), reinterpret_cast<const sockaddr*>(&where), sizeof(where)) != 0 ||
        ::listen(listener.get(), 1) != 0)
    {
        return {};
    }
    sockaddr_in chosen{};
    socklen_t chosenSize = sizeof(chosen);
    if (::getsockname(listener.get(), reinterpret_cast<sockaddr*>(&chosen), &chosenSize) != 0)
    {
        return {};
    }
    *port = ::ntohs(chosen.sin_port);
    return listener;
}

void checkUnixSocketExchange(fsdaemon::probe::Context& ctx)
{
    const TempDir home;
    auto listener = listenOnPath(home.socketPath());
    if (!ctx.check(listener.get() >= 0, "a unix socket endpoint can be stood up"))
    {
        return;
    }

    const OneShotServer server{std::move(listener), AllowBody};
    OpaPolicyEngine engine{"http://localhost/v1/data/authz",
                           makeCurlTransport(ExchangeTimeout, home.socketPath())};
    const Decision_t decision = engine.decide(consumerFixture(), "eng", "owner");

    ctx.check(decision.allow, "a decision answered over the socket is read back");
    ctx.check(decision.grantedPerms == (FS_PERM_READ | FS_PERM_WRITE),
              "the perms the socket answered become the bits");
    ctx.check(fsdaemon::util::hasPrefix(server.getRequest(), "POST /v1/data/authz"),
              "the url's path is what the request asks for");
    ctx.check(server.getRequest().find(R"({"input":)") != std::string::npos,
              "the input envelope travels over the socket");
}

// The property E5 turns on. A port anyone may hold answers an allow, and naming a socket is what
// keeps the request away from it.
void checkNamedSocketBypassesPort(fsdaemon::probe::Context& ctx)
{
    std::uint16_t port = 0;
    auto listener = listenOnLoopback(&port);
    if (!ctx.check(listener.get() >= 0, "a loopback responder can be stood up"))
    {
        return;
    }

    const OneShotServer impostor{std::move(listener), AllowBody};
    const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/v1/data/authz";

    const TempDir home;
    OpaPolicyEngine engine{url, makeCurlTransport(ExchangeTimeout, home.socketPath())};
    const Decision_t decision = engine.decide(consumerFixture(), "eng", "owner");

    ctx.check(!decision.allow, "a named socket with nothing behind it is a denial, not a port call");
    ctx.check(!impostor.hasServed(), "the responder on the port was never asked");
}

// The control for the case above: without a socket the same responder is reached and believed, so
// the denial there is the socket and not a broken fixture.
void checkUnnamedSocketFallsBackToPort(fsdaemon::probe::Context& ctx)
{
    std::uint16_t port = 0;
    auto listener = listenOnLoopback(&port);
    if (!ctx.check(listener.get() >= 0, "a loopback responder can be stood up for the control"))
    {
        return;
    }

    const OneShotServer server{std::move(listener), AllowBody};
    const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/v1/data/authz";
    OpaPolicyEngine engine{url, makeCurlTransport(ExchangeTimeout, std::string{})};
    const Decision_t decision = engine.decide(consumerFixture(), "eng", "owner");

    ctx.check(decision.allow, "with no socket named the port is reached and its answer believed");
}

}  // namespace

int main()
{
    return fsdaemon::probe::runProbe(
        [](fsdaemon::probe::Context& ctx)
        {
            checkUnixSocketExchange(ctx);
            checkNamedSocketBypassesPort(ctx);
            checkUnnamedSocketFallsBackToPort(ctx);
        });
}
