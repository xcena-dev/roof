// SPDX-License-Identifier: Apache-2.0
//
// fs/errors.hpp -- what this library throws.
//
// Its own hierarchy: an application here calls file verbs, so catching what those verbs throw
// should not put the kernel's headers in front of it.

#pragma once

#include <cerrno>
#include <stdexcept>
#include <string>
#include <system_error>

namespace fsuser
{

class FsError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// A failed syscall, carrying the errno it left. code() is the part a caller can branch on; the
// message alone cannot be compared against std::errc.
class FsCodedError : public FsError
{
public:
    explicit FsCodedError(const std::string& message, std::error_code code = {})
        : FsError{code ? message + ": " + code.message() : message},
          code_{code}
    {
    }

    [[nodiscard]] const std::error_code& code() const noexcept
    {
        return code_;
    }

private:
    std::error_code code_;
};

[[nodiscard]] inline std::error_code lastSystemError() noexcept
{
    return std::error_code{errno, std::system_category()};
}

}  // namespace fsuser
