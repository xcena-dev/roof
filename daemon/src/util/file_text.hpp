// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// util/file_text.hpp -- a whole settings file as one string, with the verdict on who wrote it.
//
// The config and the rego policy are both a file this daemon reads end to end and then parses, and
// both decide what the daemon admits. So both pass one test of who may have written them.

#pragma once

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "posix/unique_fd.hpp"

namespace fsdaemon::util
{

// What one open of a settings file settles.
enum class Standing
{
    Absent,     // nothing there to read
    Untrusted,  // a writer this daemon may not take settings from
    Trusted,
};

struct TrustedText_t
{
    Standing standing{Standing::Absent};
    std::string text;
    std::uint32_t owner{0};
};

// The file at @path, judged on the descriptor it is read through so the verdict and the text are of
// one file. Trusted means owned by root or this euid, and writable by its owner alone. Throws
// std::runtime_error when a read fails partway, since a cut-off file is not the file.
[[nodiscard]] inline TrustedText_t readTrustedText(const std::string& path)
{
    TrustedText_t found;
    const posix::UniqueFd file{::open(path.c_str(), O_RDONLY | O_CLOEXEC)};
    if (!file)
    {
        return found;
    }
    struct ::stat info
    {
    };
    if (::fstat(file.get(), &info) != 0)
    {
        return found;
    }
    found.owner = info.st_uid;
    const bool ownerTrusted = info.st_uid == 0 || info.st_uid == ::geteuid();
    if (!ownerTrusted || (info.st_mode & (S_IWGRP | S_IWOTH)) != 0)
    {
        found.standing = Standing::Untrusted;
        return found;
    }

    constexpr std::uint64_t ChunkBytes = 4096;
    std::array<char, ChunkBytes> chunk{};
    for (;;)
    {
        const std::int64_t got = ::read(file.get(), chunk.data(), chunk.size());
        if (got == 0)
        {
            break;
        }
        if (got < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            throw std::runtime_error{path + ": read failed partway"};
        }
        found.text.append(chunk.data(), static_cast<std::uint64_t>(got));
    }
    found.standing = Standing::Trusted;
    return found;
}

}  // namespace fsdaemon::util
