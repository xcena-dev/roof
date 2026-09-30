// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// tests/harness/probe.hpp -- what one daemon probe is, and the exit code ctest reads.
//
// Each probe is its own binary, so a case that wedges or dies takes only itself down and ctest
// names it. A case says what it checks and nothing about how it starts or stops: runProbe prints a
// PASS/FAIL line per check, tallies, and derives the code from the tally, so a run that printed
// PASS cannot also return non-zero.
//
// Self-contained on purpose: daemon's units are pure, so a probe needs no region and no medium, and
// this stays apart from cme's region-coupled TestContext.

#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <exception>

namespace fsdaemon::probe
{

class Context
{
public:
    // Print a PASS/FAIL line and tally. Non-fatal, so one run reports every invariant it broke
    // rather than only the first. Returns @cond, so a case whose later steps depend on this one can
    // write `if (!ctx.check(...)) { return; }`.
    bool check(bool cond, const char* message) noexcept
    {
        std::printf("%s: %s\n", cond ? "PASS" : "FAIL", message);
        if (!cond)
        {
            ++failures_;
        }
        return cond;
    }

    // check() for a message carrying a runtime value. The format attribute makes the compiler
    // reject a %u handed a 64-bit value.
    [[gnu::format(printf, 3, 4)]] bool checkf(bool cond, const char* format, ...) noexcept
    {
        std::printf("%s: ", cond ? "PASS" : "FAIL");
        std::va_list args;
        va_start(args, format);
        // NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized) the analyzer does not follow va_start
        std::vprintf(format, args);
        va_end(args);
        std::printf("\n");
        if (!cond)
        {
            ++failures_;
        }
        return cond;
    }

    // For a run that printed its own failure line and only needs the tally.
    void recordFailure() noexcept
    {
        ++failures_;
    }

    [[nodiscard]] std::int32_t failures() const noexcept
    {
        return failures_;
    }

private:
    std::int32_t failures_{0};
};

// Runs one case body against a fresh Context and returns the code ctest reads. A body that throws
// still reports the checks it reached, and a throw on the way is a failure however few checks ran.
template <typename T>
int runProbe(T&& body) noexcept
{
    Context ctx;
    try
    {
        body(ctx);
    }
    catch (const std::exception& error)
    {
        std::printf("FAIL: uncaught exception: %s\n", error.what());
        ctx.recordFailure();
    }
    catch (...)
    {
        std::printf("FAIL: uncaught non-standard exception\n");
        ctx.recordFailure();
    }

    const std::int32_t failures = ctx.failures();
    std::printf("\nRESULT: %s (%d failure(s))\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}

}  // namespace fsdaemon::probe
