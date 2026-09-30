// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// tests/harness/creds.hpp -- the caller a probe stands in for, and what it resolved to.
//
// Every identity case attests this process against a rule it wrote, so the facts are always this
// process's own. Writing that inline is how two identical copies appeared.

#pragma once

#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "model.hpp"

namespace fsdaemon::probe
{

// When this process started, in the CLOCK_BOOTTIME nanoseconds an upcall carries. Parsed here
// rather than borrowed from the daemon, so a case is not checking one reader against itself.
[[nodiscard]] inline std::uint64_t ownStartBoottimeNs()
{
    constexpr std::uint64_t NanosPerSecond = 1000000000;
    // stat's 22nd field, which is the 20th of those that follow the comm.
    constexpr std::uint32_t StartTimePosition = 20;

    std::ifstream source{"/proc/self/stat"};
    std::string line;
    const auto perSecond = ::sysconf(_SC_CLK_TCK);
    if (!source || !std::getline(source, line) || perSecond <= 0)
    {
        return 0;
    }

    // The comm sits in parentheses and may hold one of its own, so counting starts after the last.
    const auto commEnd = line.rfind(')');
    if (commEnd == std::string::npos)
    {
        return 0;
    }

    std::istringstream fields{line.substr(commEnd + 1)};
    std::string field;
    for (std::uint32_t taken = 0; taken < StartTimePosition; ++taken)
    {
        if (!(fields >> field))
        {
            return 0;
        }
    }
    return std::stoull(field) * (NanosPerSecond / static_cast<std::uint64_t>(perSecond));
}

// The kernel's word about this process, as an upcall would carry it. A case that needs a group
// list or an exe fills those in on the returned value.
[[nodiscard]] inline Creds_t factsForSelf()
{
    Creds_t facts;
    facts.uid = ::getuid();
    facts.gid = ::getgid();
    facts.startBoottimeNs = ownStartBoottimeNs();
    return facts;
}

// Whether @labels holds @wanted. The labels a backend derives have no order a case should depend
// on, so a case asks about one rather than comparing the whole list.
[[nodiscard]] inline bool labelsContain(const std::vector<std::string>& labels, std::string_view wanted)
{
    return std::find(labels.begin(), labels.end(), wanted) != labels.end();
}

}  // namespace fsdaemon::probe
