// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// tests/security_audit_test.cpp -- what a field carrying a newline does to the audit trail.
//
// Four fields on an audit line come from outside this daemon: the group and role a SPIFFE id names,
// and the rule and reason a policy answers with. A newline in any of them ends the record early.

#include <chrono>
#include <ratio>
#include <string>

#include "audit/audit.hpp"
#include "harness/probe.hpp"

namespace
{

using fsdaemon::audit::Record_t;
using fsdaemon::audit::detail::makeSummaryLine;

// The shape a forged second line takes, so a check names what an operator would then read.
constexpr const char* ForgedLine = "\naudit access pid=1 -> ALLOW perms=0x3f rule=forged";

Record_t allowRecord()
{
    Record_t record;
    record.kind = Record_t::Kind::Access;
    record.seq = 1;
    record.pid = 4242;
    record.latency = std::chrono::duration<double, std::milli>{1.0};
    record.hasIdentity = true;
    record.group = "prod";
    record.role = "llm-worker";
    record.hasDecision = true;
    record.allow = true;
    record.grantedPerms = 3;
    record.rule = "same-group";
    return record;
}

void checkGroupOneLine(fsdaemon::probe::Context& ctx)
{
    Record_t record = allowRecord();
    record.group = std::string{"prod"} + ForgedLine;
    const std::string line = makeSummaryLine(record);
    ctx.check(line.find('\n') == std::string::npos,
              "a newline in the group cannot split the record (open: it can)");
}

void checkRoleOneLine(fsdaemon::probe::Context& ctx)
{
    Record_t record = allowRecord();
    record.role = std::string{"llm-worker"} + ForgedLine;
    const std::string line = makeSummaryLine(record);
    ctx.check(line.find('\n') == std::string::npos,
              "a newline in the role cannot split the record (open: it can)");
}

void checkRuleOneLine(fsdaemon::probe::Context& ctx)
{
    Record_t record = allowRecord();
    record.rule = std::string{"same-group"} + ForgedLine;
    const std::string line = makeSummaryLine(record);
    ctx.check(line.find('\n') == std::string::npos,
              "a newline in the policy rule cannot split the record (open: it can)");
}

void checkReasonOneLine(fsdaemon::probe::Context& ctx)
{
    Record_t record = allowRecord();
    record.allow = false;
    record.reason = std::string{"denied by policy"} + ForgedLine;
    const std::string line = makeSummaryLine(record);
    ctx.check(line.find('\n') == std::string::npos,
              "a newline in the policy reason cannot split the record (open: it can)");
}

// The one field that is already flattened, kept here so a regression on it shows up beside the four
// that are not.
void checkNoteOneLine(fsdaemon::probe::Context& ctx)
{
    Record_t record;
    record.kind = Record_t::Kind::Attest;
    record.pid = 7;
    record.extra = std::string{"failed"} + ForgedLine;
    const std::string line = makeSummaryLine(record);
    ctx.check(line.find('\n') == std::string::npos, "a newline in the note is already flattened");
}

}  // namespace

int main()
{
    return fsdaemon::probe::runProbe(
        [](fsdaemon::probe::Context& ctx)
        {
            checkGroupOneLine(ctx);
            checkRoleOneLine(ctx);
            checkRuleOneLine(ctx);
            checkReasonOneLine(ctx);
            checkNoteOneLine(ctx);
        });
}
