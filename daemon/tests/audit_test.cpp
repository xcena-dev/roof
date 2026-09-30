// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// tests/audit_test.cpp -- the line an answered request goes out as.
//
// The sink is stdout or stderr, which a probe cannot read back, so the cases assert on the line the
// record turns into rather than on where it lands.

#include "audit/audit.hpp"

#include <chrono>
#include <ratio>
#include <string>

#include "harness/probe.hpp"

namespace
{

using fsdaemon::audit::Record_t;
using fsdaemon::audit::detail::makeSummaryLine;

bool holds(const std::string& line, const std::string& wanted)
{
    return line.find(wanted) != std::string::npos;
}

Record_t accessRecord()
{
    Record_t record;
    record.kind = Record_t::Kind::Access;
    record.seq = 9;
    record.pid = 4242;
    record.latency = std::chrono::duration<double, std::milli>{1.5};
    record.hasIdentity = true;
    record.group = "prod";
    record.role = "llm-worker";
    record.spiffeId = "spiffe://test.local/group/prod/role/llm-worker";
    record.hasDecision = true;
    record.allow = true;
    record.grantedPerms = 3;
    record.expiry = std::chrono::seconds{3600};
    record.rule = "same-group";
    return record;
}

void checkAllowDecision(fsdaemon::probe::Context& ctx)
{
    const std::string line = makeSummaryLine(accessRecord());
    ctx.check(holds(line, "audit access pid=4242"), "the kind and pid open the line");
    ctx.check(holds(line, "ALLOW perms=0x3"), "the granted perms go out in hex");
    ctx.check(holds(line, "rule=same-group"), "the rule that decided is named");
    ctx.check(holds(line, "id=prod/llm-worker"), "the identity is named");
    ctx.check(holds(line, "1.5ms"), "the latency is recorded");
}

void checkDenyReason(fsdaemon::probe::Context& ctx)
{
    Record_t record = accessRecord();
    record.allow = false;
    record.reason = "denied by policy";
    const std::string line = makeSummaryLine(record);
    ctx.check(holds(line, "DENY reason=denied by policy"), "a denial carries its reason");
    ctx.check(!holds(line, "ALLOW"), "a denial does not read as an allow");
}

void checkUnresolvedRequest(fsdaemon::probe::Context& ctx)
{
    Record_t record;
    record.kind = Record_t::Kind::Attest;
    record.pid = 7;
    const std::string line = makeSummaryLine(record);
    ctx.check(holds(line, "ERR"), "no identity and no decision reads as an error");
}

void checkNoteFlattening(fsdaemon::probe::Context& ctx)
{
    Record_t record;
    record.kind = Record_t::Kind::Attest;
    record.pid = 7;
    record.extra = std::string{"bell\x01 and\nnewline"};
    const std::string line = makeSummaryLine(record);
    ctx.check(holds(line, "note=bell"), "the note reaches the line");
    ctx.check(line.find('\n') == std::string::npos, "a newline in the note cannot split the record");
    ctx.check(line.find('\x01') == std::string::npos, "no raw control byte reaches the sink");
}

void checkNonAsciiPreserved(fsdaemon::probe::Context& ctx)
{
    // char is signed here, so a continuation byte of a UTF-8 sequence is negative. Without the
    // unsigned read every such byte would look like a control character and go out as a space.
    Record_t record;
    record.kind = Record_t::Kind::Attest;
    record.pid = 7;
    record.extra = "\xed\x95\x9c\xea\xb8\x80";  // "한글"
    const std::string line = makeSummaryLine(record);
    ctx.check(holds(line, "\xed\x95\x9c\xea\xb8\x80"), "a multi-byte character survives the note");
}

}  // namespace

int main()
{
    return fsdaemon::probe::runProbe(
        [](fsdaemon::probe::Context& ctx)
        {
            checkAllowDecision(ctx);
            checkDenyReason(ctx);
            checkUnresolvedRequest(ctx);
            checkNoteFlattening(ctx);
            checkNonAsciiPreserved(ctx);
        });
}
