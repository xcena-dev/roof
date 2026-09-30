// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// tests/harness/temp_file.hpp -- a file that lives as long as the case that made it.
//
// Writes its content at construction and unlinks at destruction, so a case names a path and never
// cleans up. That matters on the failure paths: a case that returns early, or one whose check
// throws, still leaves nothing in /tmp.

#pragma once

#include <stdlib.h>
#include <unistd.h>

#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <utility>

#include "posix/unique_fd.hpp"

namespace fsdaemon::probe
{

class TempFile
{
public:
    // A file holding @content. @label only shapes the name, which helps when a run is inspected by
    // hand. Empty content makes an empty file, which is itself a case some probes want.
    explicit TempFile(std::string_view content, std::string_view label = "daemon")
    {
        path_ = "/tmp/" + std::string{label} + "-XXXXXX";
        const posix::UniqueFd held{::mkstemp(path_.data())};
        if (held.get() < 0)
        {
            path_.clear();
            return;
        }
        auto left = content.size();
        const char* cursor = content.data();
        while (left > 0)
        {
            const auto put = ::write(held.get(), cursor, left);
            if (put <= 0)
            {
                break;
            }
            cursor += put;
            left -= static_cast<std::string_view::size_type>(put);
        }
    }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    TempFile(TempFile&& other) noexcept
        : path_{std::exchange(other.path_, std::string{})}
    {
    }

    TempFile& operator=(TempFile&& other) noexcept
    {
        if (this != &other)
        {
            unlinkNow();
            path_ = std::exchange(other.path_, std::string{});
        }
        return *this;
    }

    ~TempFile()
    {
        unlinkNow();
    }

    // The path, or empty when the file could not be made.
    [[nodiscard]] const std::string& getPath() const noexcept
    {
        return path_;
    }

    // Replaces the content in place, which is how a case drives a reload over the same path.
    void rewrite(std::string_view content) const
    {
        std::ofstream file{path_, std::ios::binary | std::ios::trunc};
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

private:
    void unlinkNow() noexcept
    {
        if (!path_.empty())
        {
            ::unlink(path_.c_str());
        }
    }

    std::string path_;
};

}  // namespace fsdaemon::probe
