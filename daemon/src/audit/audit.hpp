// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// audit/audit.hpp -- the record of who asked what, and what they were told.
//
// One sink: a line per answered request, on stdout or stderr. With logging off the whole thing is a
// no-op, so a deployment that does not audit pays nothing. Workers record in parallel, so this is
// locked.
//
// Header-only, so the serving code can record without spreading a link dependency across the probes.

#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ios>
#include <mutex>
#include <ratio>
#include <sstream>
#include <string>
#include <string_view>

#include "util/text.hpp"

namespace fsdaemon::audit
{

// One answered request, as much of it as the caller filled. The identity and decision are optional
// because an attest carries no decision and a failure carries neither.
struct Record_t
{
    enum class Kind : std::uint8_t
    {
        Attest,
        Access,
    };

    Kind kind{Kind::Attest};
    std::uint64_t seq{0};
    std::uint32_t pid{0};
    std::chrono::duration<double, std::milli> latency{0};

    bool hasIdentity{false};
    std::string group;
    std::string role;
    std::string spiffeId;

    bool hasDecision{false};
    bool allow{false};
    std::uint32_t grantedPerms{0};
    std::chrono::seconds expiry{0};
    std::string rule;
    std::string reason;

    // A free note, e.g. the error a failure carried. Empty when there is none.
    std::string extra;
};

namespace detail
{

// The word a line names @kind by.
[[nodiscard]] constexpr std::string_view nameKind(Record_t::Kind kind) noexcept
{
    switch (kind)
    {
        case Record_t::Kind::Attest:
            return "attest";
        case Record_t::Kind::Access:
            return "access";
    }
    return "unknown";
}

// The one line an answered request goes out as, whatever the caller managed to fill in.
[[nodiscard]] inline std::string makeSummaryLine(const Record_t& entry)
{
    std::ostringstream out;
    out << "audit " << nameKind(entry.kind) << " pid=" << entry.pid << " -> ";
    if (entry.hasDecision)
    {
        if (entry.allow)
        {
            std::ostringstream perms;
            perms << "0x" << std::hex << entry.grantedPerms;
            out << "ALLOW perms=" << perms.str() << " rule=" << (entry.rule.empty() ? "-" : util::flatten(entry.rule));
        }
        else
        {
            out << "DENY reason=" << (entry.reason.empty() ? "-" : util::flatten(entry.reason));
        }
    }
    else
    {
        out << (entry.hasIdentity ? "OK" : "ERR");
    }
    if (!entry.extra.empty())
    {
        out << " note=" << util::flatten(entry.extra);
    }
    if (entry.hasIdentity)
    {
        out << " id=" << util::flatten(entry.group) << "/" << util::flatten(entry.role);
    }
    out << " (" << entry.latency.count() << "ms)";
    return out.str();
}

}  // namespace detail

class AuditLogger
{
public:
    // @target "stdout" writes to stdout; anything else writes through stderr. With @logDecisions
    // false nothing is written.
    AuditLogger(std::string_view target, bool logDecisions)
        : sink_{target == "stdout" ? stdout : stderr},
          logDecisions_{logDecisions}
    {
    }

    AuditLogger(const AuditLogger&) = delete;
    AuditLogger& operator=(const AuditLogger&) = delete;

    // Writes @entry out when logging is on. Safe to call from several worker threads.
    void record(const Record_t& entry)
    {
        if (!logDecisions_)
        {
            return;
        }
        const std::lock_guard guard{mutex_};
        const auto summary = detail::makeSummaryLine(entry);
        std::fprintf(sink_, "%s\n", summary.c_str());
        std::fflush(sink_);
    }

private:
    std::FILE* sink_;
    bool logDecisions_;
    std::mutex mutex_;
};

}  // namespace fsdaemon::audit
