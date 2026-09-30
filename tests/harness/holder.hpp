// SPDX-License-Identifier: Apache-2.0
//
// holder.hpp -- a process that opens and maps a name on one mount, and keeps the mapping until told.
//
// A reference the kernel counts is a struct file, and a mapping keeps one past close(). A case that
// asks what unlink does to other holders needs those holders to be other processes, on this mount or
// the other one, so the case forks them and drives each through a Handoff.

#pragma once

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <exception>
#include <string>

#include "fs/file.hpp"
#include "harness/harness.hpp"
#include "harness/mount.hpp"
#include "harness/pattern.hpp"

namespace fsuser::tests
{

class Holder
{
public:
    enum Command : std::int32_t
    {
        Check = 1,
        Leave = 2,
    };

    Holder() = default;
    Holder(const Holder&) = delete;
    Holder& operator=(const Holder&) = delete;
    ~Holder()
    {
        if (pid_ > 0)
        {
            leave();
        }
    }

    // Forks. The child opens @name through @mountPoint, maps PlacementSize for reading, and answers 1
    // when the mapping holds @seed's pattern. It then serves Check and Leave from the parent.
    void start(const std::string& mountPoint, const std::string& name, std::uint32_t seed)
    {
        pid_ = ::fork();
        if (pid_ == 0)
        {
            serve(mountPoint, name, seed);
        }
    }

    [[nodiscard]] bool started() const noexcept
    {
        return pid_ > 0;
    }

    // The child's answer to its own first read, 1 for the pattern and 0 for anything else.
    [[nodiscard]] std::int32_t mapped()
    {
        return handoff_.ack.take();
    }

    // Has the child read its mapping again now, and answers what it found the same way.
    [[nodiscard]] std::int32_t check()
    {
        handoff_.begin.pass(Check);
        return handoff_.ack.take();
    }

    // Ends the child, which is what releases its reference. Answers whether it exited cleanly.
    bool leave()
    {
        if (pid_ <= 0)
        {
            return false;
        }
        handoff_.begin.pass(Leave);
        std::int32_t status = 0;
        const bool reaped = ::waitpid(pid_, &status, 0) == pid_;
        pid_ = -1;
        return reaped && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }

private:
    [[nodiscard]] static std::int32_t holdsPattern(const std::uint32_t* words, std::uint32_t seed)
    {
        for (std::uint64_t index = 0; index < PlacementSize / sizeof(std::uint32_t); ++index)
        {
            if (words[index] != patternWord(seed, index))
            {
                return 0;
            }
        }
        return 1;
    }

    [[noreturn]] void serve(const std::string& mountPoint, const std::string& name, std::uint32_t seed)
    {
        try
        {
            const Mount mount{mountPoint};
            auto standing = [&]
            {
                auto file = mount.open(name, O_RDONLY);
                return file.map(PlacementSize, PROT_READ);
            }();
            const auto* words = static_cast<const std::uint32_t*>(standing.get());
            handoff_.ack.pass(holdsPattern(words, seed));
            while (handoff_.begin.take() == Check)
            {
                handoff_.ack.pass(holdsPattern(words, seed));
            }
        }
        catch (const std::exception&)
        {
            handoff_.ack.pass(0);
            ::_exit(1);
        }
        ::_exit(0);
    }

private:
    ::pid_t pid_{-1};
    Handoff_t handoff_;
};

}  // namespace fsuser::tests
