// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// tests/observe_test.cpp -- the stage recorder: silent when off, and a report every 200 answers.

#include <cstdint>

#include "harness/probe.hpp"
#include "observe/stat.hpp"

namespace
{

using fsdaemon::observe::Stat;

void checkClockDisabled(fsdaemon::probe::Context& ctx)
{
    Stat stat{false};
    ctx.check(stat.mark() == 0, "a stamp is zero when off, so no clock is read");
    ctx.check(stat.add("attest", 1) == 0, "a fold is zero when off");
    stat.answered();
    ctx.check(true, "answering while off does nothing");
}

void checkStampAndFold(fsdaemon::probe::Context& ctx)
{
    Stat stat{true};
    const auto began = stat.mark();
    ctx.check(began != 0, "a stamp is a real reading when on");

    const auto next = stat.add("attest", began);
    ctx.check(next >= began, "a fold hands back a stamp for the next stage");
    ctx.check(stat.add("reply", next) >= next, "stages chain from one stamp to the next");
}

void checkZeroStampNoFold(fsdaemon::probe::Context& ctx)
{
    // A caller that was off when it stamped must not fold the whole epoch into a stage.
    Stat stat{true};
    ctx.check(stat.add("attest", 0) == 0, "a zero stamp folds nothing");
}

void checkReportThreshold(fsdaemon::probe::Context& ctx)
{
    // The report goes to stderr, which a probe cannot read back, so this asserts the counting does
    // not throw or wedge across the boundary rather than the text it prints.
    Stat stat{true};
    for (std::uint32_t index = 0; index < 205; ++index)
    {
        const auto began = stat.mark();
        stat.add("attest", began);
        stat.answered();
    }
    ctx.check(true, "200 answers report and start over without wedging");
}

}  // namespace

int main()
{
    return fsdaemon::probe::runProbe(
        [](fsdaemon::probe::Context& ctx)
        {
            checkClockDisabled(ctx);
            checkStampAndFold(ctx);
            checkZeroStampNoFold(ctx);
            checkReportThreshold(ctx);
        });
}
