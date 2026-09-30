// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// wire/protocol.hpp -- the kernel contract, as the daemon reaches it.
//
// The wire format is daemon_uapi.h. This header includes it and the perm bits it names, and gives
// each wire record a C++ name, so the rest of the daemon reaches the kernel's own definitions rather
// than re-deriving them. Compiling against the kernel's own header leaves nothing to keep in sync.

#pragma once

// daemon_uapi.h asserts its struct sizes with C11 _Static_assert, which g++ does not accept in C++
// mode. Mapping it to the C++ keyword lets the shared kernel header compile here unmodified; the two
// spellings take the same (expr, message) arguments.
#ifndef _Static_assert
#define _Static_assert static_assert
#endif

#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>

// The rendered uapi headers arrive on the include path from the build tree, not from kernel/. This
// header hands both on to whoever includes it.
#include "daemon_uapi.h"  // IWYU pragma: export
#include "uapi.h"         // IWYU pragma: export

// The wire records under this tree's C++ names. daemon_uapi.h keeps the kernel's C spelling.
namespace fsdaemon
{
using FsDaemonHdr = struct fs_daemon_hdr;
using FsDaemonHello = struct fs_daemon_hello;
using FsDaemonAttestRequest = struct fs_daemon_attest_request;
using FsDaemonAttestResponse = struct fs_daemon_attest_response;
using FsDaemonAccessRequest = struct fs_daemon_access_request;
using FsDaemonResponse = struct fs_daemon_response;
using FsDaemonLockRequest = struct fs_daemon_lock_request;
using FsDaemonLockResponse = struct fs_daemon_lock_response;
using FsDaemonUnlockRequest = struct fs_daemon_unlock_request;
}  // namespace fsdaemon

namespace fsdaemon::wire
{

// The bit a permission name stands for, or nullopt for a name no policy may grant. Case-sensitive:
// the rego and the audit line both spell them upper-case, and the bits are uapi.h's.
[[nodiscard]] inline std::optional<std::uint32_t> resolvePermBit(std::string_view name)
{
    // Views into string literals, which outlive the map.
    static const std::unordered_map<std::string_view, std::uint32_t> PermBits{
        {"READ", FS_PERM_READ},
        {"WRITE", FS_PERM_WRITE},
        {"DELETE", FS_PERM_DELETE},
        {"ADMIN", FS_PERM_ADMIN},
        {"IOCTL", FS_PERM_IOCTL},
        {"GRANT", FS_PERM_GRANT},
        {"ALL", FS_PERM_ALL},
    };
    const auto found = PermBits.find(name);
    if (found == PermBits.end())
    {
        return std::nullopt;
    }
    return found->second;
}

}  // namespace fsdaemon::wire
