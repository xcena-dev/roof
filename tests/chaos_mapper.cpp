// SPDX-License-Identifier: Apache-2.0
/*
 * chaos_mapper - hold a mapping of one region, and say when it stops answering.
 *
 * A revoke clears page table entries and signals nobody. It only becomes visible when somebody
 * touches the page afterwards and faults, so bootstrap_chaos needs a process doing exactly that.
 *
 * Prints one word per step on stdout, line buffered, because the case reads it as it goes:
 * "mapped" once the region is mapped and written, then "revoked" on the first SIGBUS.
 *
 * With --place it stops after placing the region. A case needing rows for a sweep to clear wants
 * that and not a process, and the shell cannot do it: truncate(1) opens without O_CLOEXEC.
 *
 * With --grant <perms> it places the region and then delegates those permissions to this account,
 * writing one row that carries the node the call was made from. That is the row a recovery clears,
 * and no daemon is involved: the account form of a grant answers to the owner alone.
 *
 * With --default <perms> the owner opens the region to every node instead. A delegation row is only
 * read on the node that wrote it, so this is what lets a second node grant a row of its own.
 *
 * --grant takes an optional uid, so a case can write a row that names somebody other than the
 * caller. --revoke <uid> takes such a row back off. Both name gid as ANY.
 *
 * --grant-role <perms> <group> <role> and --revoke-role <group> <role> do the same for a role row,
 * which names a workload and any node. --hold-read maps the region read-only and waits like the
 * default mode, so a node with no row for the caller asks its helper before the map lands.
 *
 * --own <perms> sets the default like --default and then maps and waits like the default mode, so
 * the region keeps a live owner, which a sweep never reclaims, for as long as the case needs it.
 */

#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string_view>

#include "fs/file.hpp"
#include "fs/testing.hpp"
#include "uapi.h"

namespace
{

constexpr std::uint64_t RegionSize = fsuser::DaxAlignment;
constexpr std::int32_t PollMs = 100;
constexpr std::int32_t GiveUpSeconds = 180;

sigjmp_buf g_faultReturn;

/* siglongjmp and not a flag: the faulting load restarts on return, so the handler cannot resume. */
void onBusError(std::int32_t signo)
{
    (void)signo;
    siglongjmp(g_faultReturn, 1);
}

void sleepPoll()
{
    const struct timespec slice = {.tv_sec = 0, .tv_nsec = PollMs * 1000L * 1000L};
    nanosleep(&slice, nullptr);
}

using FsPermReq = struct fs_perm_req;

/* What every failure here exits with. 1 stays for a mapping that outlived the wait. */
constexpr std::int32_t FailStatus = 2;

/* argv[1] and argv[2] are the mount and the name, argv[3] the flag, and its arguments follow. */
constexpr std::int32_t FlagArg = 3;
constexpr std::int32_t FirstValueArg = FlagArg + 1;

/* A number as the shell passes it, decimal or 0x hex. 0 for text that is not one. */
std::uint32_t parseNumber(std::string_view text)
{
    constexpr std::string_view HexPrefix = "0x";
    constexpr std::int32_t HexBase = 16;
    std::int32_t base = 10;
    if (text.substr(0, HexPrefix.size()) == HexPrefix)
    {
        text.remove_prefix(HexPrefix.size());
        base = HexBase;
    }
    std::uint32_t value = 0;
    std::from_chars(text.data(), text.data() + text.size(), value, base);
    return value;
}

/* An account row names its uid with the gid left open, so it outlives the process that asked. */
FsPermReq makeAccountRequest(std::uint32_t uid, std::uint32_t perms)
{
    FsPermReq request{};
    request.uid = uid;
    request.gid = FS_PERM_ANY_ID;
    request.perms = perms;
    return request;
}

/* A role row names a workload by its labels, outlives the process and answers on every node. */
FsPermReq makeRoleRequest(const char* group, const char* role, std::uint32_t perms)
{
    FsPermReq request{};
    request.kind = FS_PERM_KIND_ROLE;
    request.perms = perms;
    strncpy(request.group, group, sizeof(request.group) - 1);
    strncpy(request.role, role, sizeof(request.role) - 1);
    return request;
}

/* One permission ioctl. Prints @done, which the case reads, or the errno under @what. */
std::int32_t sendRequest(std::int32_t region, std::uint64_t command, FsPermReq request, const char* what,
                         const char* done)
{
    if (ioctl(region, command, &request) != 0)
    {
        fprintf(stderr, "%s: %s\n", what, strerror(errno));
        return FailStatus;
    }

    printf("%s\n", done);
    return 0;
}

/* What one run does. Hold maps read-write and waits, and is what no flag asks for. */
enum class Mode
{
    Hold,
    HoldRead,
    Place,
    Grant,
    Default,
    Own,
    Revoke,
    GrantRole,
    RevokeRole,
};

/* A flag and how many arguments may follow it. */
struct ModeFlag_t
{
    std::string_view flag;
    Mode mode;
    std::int32_t minArgs;
    std::int32_t maxArgs;
};

constexpr std::array<ModeFlag_t, 8> ModeFlags{{
    {"--place", Mode::Place, 0, 0},
    {"--hold-read", Mode::HoldRead, 0, 0},
    {"--grant", Mode::Grant, 1, 2},
    {"--default", Mode::Default, 1, 1},
    {"--own", Mode::Own, 1, 1},
    {"--revoke", Mode::Revoke, 1, 1},
    {"--grant-role", Mode::GrantRole, 3, 3},
    {"--revoke-role", Mode::RevokeRole, 2, 2},
}};

/* The mode argv names, or nullopt for an unknown flag or the wrong number of arguments after it. */
std::optional<Mode> parseMode(std::int32_t argc, char** argv)
{
    if (argc < FlagArg)
    {
        return std::nullopt;
    }
    if (argc == FlagArg)
    {
        return Mode::Hold;
    }

    const std::string_view flag{argv[FlagArg]};
    const auto found = std::find_if(ModeFlags.begin(), ModeFlags.end(),
                                    [flag](const ModeFlag_t& entry)
                                    {
                                        return entry.flag == flag;
                                    });
    const std::int32_t given = argc - FirstValueArg;
    if (found == ModeFlags.end() || given < found->minArgs || given > found->maxArgs)
    {
        return std::nullopt;
    }
    return found->mode;
}

/* The modes that end once their request is sent. nullopt for the ones that go on to map, which
 * --own does after it has opened the region to every node. */
std::optional<std::int32_t> runRequestMode(std::int32_t region, Mode mode, std::int32_t argc, char** argv)
{
    const char* const* values = argv + FirstValueArg;
    switch (mode)
    {
        case Mode::Place:
            printf("placed\n");
            return 0;
        case Mode::Grant:
        {
            const std::uint32_t uid = argc > FirstValueArg + 1 ? parseNumber(values[1]) : getuid();
            return sendRequest(region, FS_IOC_PERM_GRANT, makeAccountRequest(uid, parseNumber(values[0])), "perm_grant",
                               "granted");
        }
        case Mode::Default:
            /* Reaches every node, unlike a row. The owner is the only caller ADMIN admits. */
            return sendRequest(region, FS_IOC_PERM_SET_DEFAULT, makeAccountRequest(FS_PERM_ANY_ID, parseNumber(values[0])),
                               "perm_set_default", "defaulted");
        case Mode::Own:
        {
            const auto status = sendRequest(region, FS_IOC_PERM_SET_DEFAULT,
                                            makeAccountRequest(FS_PERM_ANY_ID, parseNumber(values[0])), "perm_set_default",
                                            "defaulted");
            return status == 0 ? std::nullopt : std::optional<std::int32_t>{status};
        }
        case Mode::Revoke:
            /* Own row or not is the module's question, not this one's. */
            return sendRequest(region, FS_IOC_PERM_REVOKE, makeAccountRequest(parseNumber(values[0]), 0), "perm_revoke",
                               "revoked");
        case Mode::GrantRole:
            return sendRequest(region, FS_IOC_PERM_GRANT, makeRoleRequest(values[1], values[2], parseNumber(values[0])),
                               "perm_grant role", "granted");
        case Mode::RevokeRole:
            return sendRequest(region, FS_IOC_PERM_REVOKE, makeRoleRequest(values[0], values[1], 0), "perm_revoke role",
                               "revoked");
        case Mode::Hold:
        case Mode::HoldRead:
            return std::nullopt;
    }
    return std::nullopt;
}

}  // namespace

int main(int argc, char** argv)
{
    static const char* const Usage =
        "usage: chaos_mapper <mount> <name> "
        "[--place | --hold-read | --grant <perms> [uid] | --default <perms> | --own <perms> | --revoke <uid> |\n"
        "                      --grant-role <perms> <group> <role> | --revoke-role <group> <role>]\n";

    const auto mode = parseMode(argc, argv);
    if (!mode)
    {
        fprintf(stderr, "%s", Usage);
        return FailStatus;
    }

    /* O_CLOEXEC is not optional here: the module refuses a descriptor without it. */
    const auto region = fsuser::testing::openRaw(argv[1], argv[2], O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (region < 0)
    {
        fprintf(stderr, "open: %s\n", strerror(errno));
        return FailStatus;
    }

    /* Placement is once and for all: a second ftruncate is what WORM refuses, so an already
     * placed region is opened and left alone. */
    struct stat placed = {};
    if (fstat(region, &placed) != 0)
    {
        fprintf(stderr, "fstat: %s\n", strerror(errno));
        return FailStatus;
    }

    if (placed.st_size == 0 && ftruncate(region, static_cast<::off_t>(RegionSize)) != 0)
    {
        fprintf(stderr, "ftruncate: %s\n", strerror(errno));
        return FailStatus;
    }

    if (const auto done = runRequestMode(region, *mode, argc, argv))
    {
        return *done;
    }

    const bool holdRead = *mode == Mode::HoldRead;
    const std::int32_t protection = holdRead ? PROT_READ : (PROT_READ | PROT_WRITE);
    auto* const mapping = mmap(nullptr, static_cast<::size_t>(RegionSize), protection, MAP_SHARED, region, 0);
    if (mapping == MAP_FAILED)
    {
        fprintf(stderr, "mmap: %s\n", strerror(errno));
        return FailStatus;
    }

    auto* const cell = static_cast<volatile std::uint8_t*>(mapping);
    if (!holdRead)
    {
        *cell = 0x5a;
    }

    struct sigaction action = {};
    action.sa_handler = onBusError;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGBUS, &action, nullptr) != 0)
    {
        fprintf(stderr, "sigaction: %s\n", strerror(errno));
        return FailStatus;
    }

    printf("mapped\n");
    fflush(stdout);

    for (std::int32_t elapsed = 0; elapsed < GiveUpSeconds * (1000 / PollMs); elapsed++)
    {
        if (sigsetjmp(g_faultReturn, 1) != 0)
        {
            printf("revoked\n");
            fflush(stdout);
            return 0;
        }

        (void)*cell;
        sleepPoll();
    }

    printf("still mapped\n");
    fflush(stdout);
    return 1;
}
