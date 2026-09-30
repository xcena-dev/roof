// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// posix/unique_fd.hpp -- one owned file descriptor, closed when it goes out of scope.
//
// A descriptor is a number the kernel reuses the moment it is closed, so ownership has to be
// unambiguous. This holds exactly one and closes it once; a borrowed descriptor (a test's socket,
// the caller's stdout) is held with ownership off so the same type names both without closing what
// it does not own.

#pragma once

#include <unistd.h>

#include <cstdint>
#include <utility>

namespace fsdaemon::posix
{

// What open() and its kin hand back: a small number indexing this process's open files, not a
// quantity to compute with. The assert holds this to the width the syscalls take.
using Descriptor = std::int32_t;
static_assert(sizeof(Descriptor) == sizeof(int), "a descriptor is what open() returns");

class UniqueFd
{
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(Descriptor descriptor) noexcept
        : descriptor_{descriptor}
    {
    }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept
        : descriptor_{std::exchange(other.descriptor_, NoFd)}
    {
    }

    UniqueFd& operator=(UniqueFd&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            descriptor_ = std::exchange(other.descriptor_, NoFd);
        }
        return *this;
    }

    ~UniqueFd() noexcept
    {
        reset();
    }

    [[nodiscard]] Descriptor get() const noexcept
    {
        return descriptor_;
    }

    // Hands the descriptor to a caller that will close it, leaving this holding nothing. The one
    // way ownership leaves without a close, which is how a factory taking a descriptor is fed.
    [[nodiscard]] Descriptor release() noexcept
    {
        return std::exchange(descriptor_, NoFd);
    }

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return descriptor_ != NoFd;
    }

private:
    // What a UniqueFd holds when it holds nothing. Every caller asks with operator bool instead, so
    // the value itself never leaves the class.
    static constexpr Descriptor NoFd = -1;

    // The destructor's body, and the first half of a move assignment. Both are members, so this
    // closes at most once and nothing outside can close what the object still reports as live.
    void reset() noexcept
    {
        if (descriptor_ != NoFd)
        {
            ::close(descriptor_);
            descriptor_ = NoFd;
        }
    }

    Descriptor descriptor_{NoFd};
};

}  // namespace fsdaemon::posix
