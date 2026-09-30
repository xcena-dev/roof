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
 */

#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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

/* Reaches every node, unlike a row. The owner is the only caller ADMIN admits. */
std::int32_t setDefaultPerms(std::int32_t region, std::uint32_t perms)
{
    struct fs_perm_req request = {};
    request.uid = FS_PERM_ANY_ID;
    request.gid = FS_PERM_ANY_ID;
    request.perms = perms;

    if (ioctl(region, FS_IOC_PERM_SET_DEFAULT, &request) != 0)
    {
        fprintf(stderr, "perm_set_default: %s\n", strerror(errno));
        return 2;
    }

    printf("defaulted\n");
    return 0;
}

/* Names an account, so the row outlives the process that asked for it. */
std::int32_t grantAccount(std::int32_t region, std::uint32_t perms, std::uint32_t uid)
{
    struct fs_perm_req request = {};
    request.uid = uid;
    request.gid = FS_PERM_ANY_ID;
    request.perms = perms;

    if (ioctl(region, FS_IOC_PERM_GRANT, &request) != 0)
    {
        fprintf(stderr, "perm_grant: %s\n", strerror(errno));
        return 2;
    }

    printf("granted\n");
    return 0;
}

/* Takes the row naming @uid back off. Own row or not is the module's question, not this one's. */
std::int32_t revokeAccount(std::int32_t region, std::uint32_t uid)
{
    struct fs_perm_req request = {};
    request.uid = uid;
    request.gid = FS_PERM_ANY_ID;

    if (ioctl(region, FS_IOC_PERM_REVOKE, &request) != 0)
    {
        fprintf(stderr, "perm_revoke: %s\n", strerror(errno));
        return 2;
    }

    printf("revoked\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    static const char* const Usage =
        "usage: chaos_mapper <mount> <name> "
        "[--place | --grant <perms> [uid] | --default <perms> | --revoke <uid>]\n";

    if (argc < 3 || argc > 6)
    {
        fprintf(stderr, "%s", Usage);
        return 2;
    }

    const bool placeOnly = argc == 4 && strcmp(argv[3], "--place") == 0;
    const bool grantOnly = argc >= 5 && strcmp(argv[3], "--grant") == 0;
    const bool defaultOnly = argc == 5 && strcmp(argv[3], "--default") == 0;
    const bool revokeOnly = argc == 5 && strcmp(argv[3], "--revoke") == 0;
    if (argc > 3 && !placeOnly && !grantOnly && !defaultOnly && !revokeOnly)
    {
        fprintf(stderr, "%s", Usage);
        return 2;
    }

    /* O_CLOEXEC is not optional here: the module refuses a descriptor without it. */
    const auto region = fsuser::testing::openRaw(argv[1], argv[2], O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (region < 0)
    {
        fprintf(stderr, "open: %s\n", strerror(errno));
        return 2;
    }

    /* Placement is once and for all: a second ftruncate is what WORM refuses, so an already
     * placed region is opened and left alone. */
    struct stat placed = {};
    if (fstat(region, &placed) != 0)
    {
        fprintf(stderr, "fstat: %s\n", strerror(errno));
        return 2;
    }

    if (placed.st_size == 0 && ftruncate(region, static_cast<::off_t>(RegionSize)) != 0)
    {
        fprintf(stderr, "ftruncate: %s\n", strerror(errno));
        return 2;
    }

    if (placeOnly)
    {
        printf("placed\n");
        return 0;
    }

    if (grantOnly)
    {
        const auto perms = static_cast<std::uint32_t>(strtoul(argv[4], nullptr, 0));
        const auto uid = argc == 6 ? static_cast<std::uint32_t>(strtoul(argv[5], nullptr, 0)) : getuid();
        return grantAccount(region, perms, uid);
    }

    if (defaultOnly)
    {
        return setDefaultPerms(region, static_cast<std::uint32_t>(strtoul(argv[4], nullptr, 0)));
    }

    if (revokeOnly)
    {
        return revokeAccount(region, static_cast<std::uint32_t>(strtoul(argv[4], nullptr, 0)));
    }

    auto* const mapping = mmap(nullptr, static_cast<::size_t>(RegionSize), PROT_READ | PROT_WRITE, MAP_SHARED, region, 0);
    if (mapping == MAP_FAILED)
    {
        fprintf(stderr, "mmap: %s\n", strerror(errno));
        return 2;
    }

    auto* const cell = static_cast<volatile std::uint8_t*>(mapping);
    *cell = 0x5a;

    struct sigaction action = {};
    action.sa_handler = onBusError;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGBUS, &action, nullptr) != 0)
    {
        fprintf(stderr, "sigaction: %s\n", strerror(errno));
        return 2;
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
