// SPDX-License-Identifier: Apache-2.0
//
// testing.hpp -- the states the library refuses to produce, so the kernel's refusal can be shown.
//
// Not installed, and built only when FS_TESTING is on. Everything here exists to hand the
// kernel input a correct caller never sends: the point is the rejection, not the call.
//
// These take and return a plain descriptor rather than a File. A File is what the library opens
// correctly, and giving it a way to be opened incorrectly would put the hole inside the contract.

#pragma once

#include <sys/types.h>

#include <cstdint>
#include <string_view>

namespace fsuser::testing
{

// Opens a file with exactly @flags, which is how a descriptor the library would never hand out is
// reached: without O_CLOEXEC, the module refuses to map it. @mode is read only when @flags carries
// O_CREAT. Returns the descriptor, or -1 with errno set. The caller closes it.
[[nodiscard]] std::int32_t openRaw(std::string_view mountPoint, std::string_view name,
                                   std::int32_t flags, ::mode_t mode = 0);

// Sends a request no verb of this filesystem owns, so an unhandled number answers ENOTTY.
// Returns what ioctl returned, with errno left as ioctl set it.
[[nodiscard]] std::int32_t sendUnknownIoctl(std::int32_t file);

}  // namespace fsuser::testing
