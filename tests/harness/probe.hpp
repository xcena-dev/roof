// SPDX-License-Identifier: Apache-2.0
//
// probe.hpp -- whether an open region admits a read-only mapping, from a child that has to answer
// in one exit code and no thrown exception.
//
// A forked child binds a delegation row by mapping once, and several cases only care whether that
// mapping was granted: the errno the kernel left behind, or 0 when it landed.

#pragma once

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <string>

#include "fs/testing.hpp"

namespace fsuser::tests
{

// Maps @descriptor read-only for @size bytes and answers 0 or the errno mmap left behind, tearing
// the mapping down again either way.
[[nodiscard]] inline std::int32_t probeReadMap(std::int32_t descriptor, std::uint64_t size)
{
    errno = 0;
    void* mapped = ::mmap(nullptr, size, PROT_READ, MAP_SHARED, descriptor, 0);
    if (mapped == MAP_FAILED)
    {
        return errno;
    }
    ::munmap(mapped, size);
    return 0;
}

// The same probe from a bare mount and name rather than an inherited descriptor, for a child that
// opens its own path to the region.
[[nodiscard]] inline std::int32_t openAndProbeReadMap(const std::string& mount,
                                                      const std::string& name, std::uint64_t size)
{
    const std::int32_t taken = fsuser::testing::openRaw(mount, name, O_RDWR | O_CLOEXEC);
    if (taken < 0)
    {
        return errno;
    }
    const auto answer = probeReadMap(taken, size);
    ::close(taken);
    return answer;
}

}  // namespace fsuser::tests
