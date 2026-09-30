// SPDX-License-Identifier: Apache-2.0
//
// overlap -- whether two mounts of one device can be made to share physical bytes.
//
// Two processes create files on their own mount, meet at a barrier, and place them at the same
// moment. Placement scans every extent already committed and then commits into the gap it found,
// so two nodes doing that without exclusion pick the same gap and end up sharing bytes.
//
// Then it reads the filesystem's sysfs region_info and says whether any two regions overlap. --raw
// does the same rounds with plain syscalls instead of this library, which is the arm that shows
// where the exclusion lives: the kernel takes the lock on the placement path either way.
//
//   <fs>-overlap <mount-a> <mount-b> --rounds 10
//   <fs>-overlap <mount-a> <mount-b> --rounds 10 --raw

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "fs/errors.hpp"
#include "fs/file.hpp"
#include "name.h"

namespace
{

constexpr std::uint64_t PlacementSize = fsuser::DaxAlignment;
constexpr const char* RegionInfo = "/sys/fs/" FS_NAME_STR "/region_info";

struct Extent_t
{
    std::uint64_t offset{0};
    std::uint64_t size{0};
    std::string name;
};

struct Options_t
{
    std::string mountA;
    std::string mountB;
    std::int32_t rounds{10};
    std::int32_t files{3};
    bool raw{false};
};

// Which mount, and the file-name tag one node's round runs under.
struct NodeRoute_t
{
    std::string mount;
    std::string tag;
};

void printUsage()
{
    std::fprintf(stderr,
                 "usage: " FS_PROGRAM_NAME
                 " <mount-a> <mount-b> [--rounds N] [--files N] [--raw]\n");
}

[[nodiscard]] std::optional<Options_t> parseOptions(std::int32_t argc, char** argv)
{
    Options_t into;
    std::vector<std::string> positional;

    for (std::int32_t index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];

        if (argument == "--raw")
        {
            into.raw = true;
            continue;
        }
        if (argument.rfind("--", 0) == 0)
        {
            if (index + 1 >= argc)
            {
                return std::nullopt;
            }
            const std::string value = argv[++index];

            if (argument == "--rounds")
            {
                into.rounds = std::stoi(value);
            }
            else if (argument == "--files")
            {
                into.files = std::stoi(value);
            }
            else
            {
                return std::nullopt;
            }
            continue;
        }
        positional.push_back(argument);
    }

    if (positional.size() != 2 || into.rounds < 1 || into.files < 1)
    {
        return std::nullopt;
    }
    into.mountA = positional[0];
    into.mountB = positional[1];
    return into;
}

// ── the placement map ───────────────────────────────────────────────────

// Every region the kernel has placed, read from sysfs. A region with no offset has been created but
// not yet placed, and two of those are not a collision.
[[nodiscard]] std::vector<Extent_t> readExtents()
{
    std::vector<Extent_t> found;
    std::ifstream source{RegionInfo};
    std::string line;

    std::getline(source, line);  // the header
    while (std::getline(source, line))
    {
        std::istringstream fields{line};
        std::string entryId;
        std::string node;
        std::string pid;
        std::string state;
        std::string size;
        std::string offset;
        std::string name;

        if (!(fields >> entryId >> node >> pid >> state >> size >> offset >> name))
        {
            continue;
        }
        const std::uint64_t placed = std::stoull(offset, nullptr, 0);
        if (placed == 0)
        {
            continue;
        }
        found.push_back({placed, std::stoull(size), name});
    }
    return found;
}

// Pairs sharing physical bytes. Reported rather than counted, because which two collided is what
// tells a reader whether the run reproduced the case.
[[nodiscard]] std::int32_t reportOverlaps(const std::vector<Extent_t>& extents)
{
    std::int32_t collisions = 0;

    for (std::size_t left = 0; left < extents.size(); ++left)
    {
        for (std::size_t right = left + 1; right < extents.size(); ++right)
        {
            const auto& one = extents[left];
            const auto& two = extents[right];
            const auto begin = std::max(one.offset, two.offset);
            const auto end = std::min(one.offset + one.size, two.offset + two.size);
            if (begin < end)
            {
                std::printf("  OVERLAP [0x%" PRIx64 ", 0x%" PRIx64 ") %s vs %s\n", begin, end,
                            one.name.c_str(), two.name.c_str());
                ++collisions;
            }
        }
    }
    return collisions;
}

// ── one node's round ────────────────────────────────────────────────────

[[nodiscard]] std::string buildName(const std::string& tag, std::int32_t round, std::int32_t index)
{
    return tag + "_r" + std::to_string(round) + "_" + std::to_string(index);
}

[[nodiscard]] std::string buildPath(const std::string& mount, const std::string& tag,
                                    std::int32_t round, std::int32_t index)
{
    return mount + "/" + buildName(tag, round, index);
}

void syncSignal(std::int32_t descriptor)
{
    const char one = 'G';
    if (::write(descriptor, &one, 1) != 1)
    {
        std::perror(FS_PROGRAM_NAME ": barrier write");
    }
}

// False when the other side is gone: its end of the pipe closed, so the read sees the end of the
// stream instead of the byte it signals with.
[[nodiscard]] bool syncWait(std::int32_t descriptor)
{
    char one = 0;
    const auto got = ::read(descriptor, &one, 1);
    if (got < 0)
    {
        std::perror(FS_PROGRAM_NAME ": barrier read");
    }
    return got == 1;
}

// What a node does when the other left the barrier: it stops the round rather than placing alone,
// since a placement with nobody to collide with measures nothing.
void requireBarrier(std::int32_t waitOn)
{
    if (!syncWait(waitOn))
    {
        throw std::runtime_error{"the other node left the barrier"};
    }
}

// Create every file, meet the other node at the barrier, then place them all. Creating first keeps
// the collision in the placement, which is the operation that reads the whole extent map.
//
// Placing is where this returns. What follows is the second handshake, which the two sides do not do
// symmetrically and which therefore belongs to the caller.
void runNode(const Options_t& options, const NodeRoute_t& route, std::int32_t round,
             std::int32_t signalTo, std::int32_t waitOn)
{
    if (options.raw)
    {
        std::vector<std::int32_t> held;
        for (std::int32_t index = 0; index < options.files; ++index)
        {
            const auto path = buildPath(route.mount, route.tag, round, index);
            held.push_back(::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644));
        }

        syncSignal(signalTo);
        requireBarrier(waitOn);

        for (const auto one : held)
        {
            if (one >= 0)
            {
                if (::ftruncate(one, static_cast<::off_t>(PlacementSize)) != 0)
                {
                    std::perror(FS_PROGRAM_NAME ": ftruncate");
                }
                ::close(one);
            }
        }
        return;
    }

    std::vector<fsuser::File> held;

    held.reserve(static_cast<std::size_t>(options.files));
    for (std::int32_t index = 0; index < options.files; ++index)
    {
        held.push_back(fsuser::File::open(buildPath(route.mount, route.tag, round, index),
                                          O_CREAT | O_RDWR, 0644));
    }

    syncSignal(signalTo);
    requireBarrier(waitOn);

    for (auto& one : held)
    {
        one.resize(PlacementSize);
    }
}

// Each side removes what it created. A file's owner is the process that created it, so a node that
// tried to remove the other's would be refused and the round would leave its files behind.
void removeRound(const Options_t& options, const NodeRoute_t& route, std::int32_t round)
{
    if (options.raw)
    {
        for (std::int32_t index = 0; index < options.files; ++index)
        {
            ::unlink(buildPath(route.mount, route.tag, round, index).c_str());
        }
        return;
    }

    for (std::int32_t index = 0; index < options.files; ++index)
    {
        try
        {
            fsuser::unlinkFile(buildPath(route.mount, route.tag, round, index));
        }
        catch (const fsuser::FsCodedError&)
        {
            // @expected: a round that failed to create leaves nothing to remove.
        }
    }
}

// What one round came to: measured with or without an overlap, or left by a node before placing.
enum class RoundOutcome : std::int32_t
{
    Clean,
    Overlapped,
    Broken,
};

// The child's half of a round. It places beside the parent, holds its files until the parent has read
// the extent map, and removes them whatever ended the round.
[[noreturn]] void runChildNode(const Options_t& options, const NodeRoute_t& route, std::int32_t round,
                               std::int32_t signalTo, std::int32_t waitOn)
{
    std::int32_t status = 0;
    try
    {
        runNode(options, route, round, signalTo, waitOn);
        // Placed. The other node reads the extent map before either side gives its bytes back, so
        // nothing is removed until it says so.
        syncSignal(signalTo);
        requireBarrier(waitOn);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, FS_PROGRAM_NAME ": %s: %s\n", route.tag.c_str(), error.what());
        status = 3;
    }
    ::close(signalTo);
    removeRound(options, route, round);
    ::_exit(status);
}

// The parent's half. It places beside the child, reads the extent map while both hold their files,
// and closes its signalling end on the way out, which is what tells a child still at its barrier
// that nobody is coming.
[[nodiscard]] RoundOutcome runParentNode(const Options_t& options, const NodeRoute_t& route,
                                         std::int32_t round, std::int32_t signalTo, std::int32_t waitOn)
{
    bool placed = true;
    try
    {
        runNode(options, route, round, signalTo, waitOn);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, FS_PROGRAM_NAME ": %s: %s\n", route.tag.c_str(), error.what());
        placed = false;
    }
    if (!placed || !syncWait(waitOn))
    {
        ::close(signalTo);
        return RoundOutcome::Broken;
    }

    const std::int32_t found = reportOverlaps(readExtents());
    if (found != 0)
    {
        std::printf("round %d: %d overlapping pairs\n", round, found);
    }
    syncSignal(signalTo);
    ::close(signalTo);
    return found == 0 ? RoundOutcome::Clean : RoundOutcome::Overlapped;
}

}  // namespace

int main(int argc, char** argv)
{
    const auto parsed = parseOptions(argc, argv);
    if (!parsed)
    {
        printUsage();
        return 1;
    }
    const Options_t& options = *parsed;

    // A write to a node that already left would otherwise end this side before it removed its files
    // and reaped the child.
    ::signal(SIGPIPE, SIG_IGN);

    std::int32_t collisions = 0;
    std::int32_t brokenRounds = 0;
    const NodeRoute_t routeA{options.mountA, "nodeA"};
    const NodeRoute_t routeB{options.mountB, "nodeB"};

    for (std::int32_t round = 0; round < options.rounds; ++round)
    {
        std::array<std::int32_t, 2> toChild{-1, -1};
        std::array<std::int32_t, 2> toParent{-1, -1};
        if (::pipe(toChild.data()) != 0 || ::pipe(toParent.data()) != 0)
        {
            std::perror(FS_PROGRAM_NAME ": pipe");
            return 2;
        }

        const ::pid_t child = ::fork();
        if (child < 0)
        {
            std::perror(FS_PROGRAM_NAME ": fork");
            return 2;
        }

        if (child == 0)
        {
            ::close(toChild[1]);
            ::close(toParent[0]);
            runChildNode(options, routeB, round, toParent[1], toChild[0]);
        }

        ::close(toChild[0]);
        ::close(toParent[1]);
        const auto outcome = runParentNode(options, routeA, round, toChild[1], toParent[0]);
        if (outcome == RoundOutcome::Overlapped)
        {
            ++collisions;
        }
        if (outcome == RoundOutcome::Broken)
        {
            std::printf("round %d: a node left before placing, so the round measures nothing\n", round);
            ++brokenRounds;
        }

        removeRound(options, routeA, round);

        std::int32_t status = 0;
        ::waitpid(child, &status, 0);
        ::close(toParent[0]);
    }

    std::printf("%s: %d of %d rounds had an overlap, %d could not be measured\n",
                options.raw ? "raw" : "through the library", collisions, options.rounds, brokenRounds);
    if (brokenRounds != 0)
    {
        return 5;
    }
    return collisions == 0 ? 0 : 4;
}
