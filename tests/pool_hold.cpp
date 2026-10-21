// SPDX-License-Identifier: Apache-2.0
//
// pool_hold -- a fragmented write-back pool held by one process, for a script to place into.
//
// A region goes when the process that made it does, so the pool stays cut only while this runs.
//   pool_hold serve <mount> <stem> <holes>   then one reply line per command read from stdin
//   pool_hold place <path> <bytes>           one paged placement, exit 0 when it landed

#include <fcntl.h>
#include <sys/mman.h>

#include <cstdint>
#include <cstdio>
#include <exception>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "common/timing.hpp"
#include "fs/errors.hpp"
#include "fs/file.hpp"
#include "harness/mount.hpp"
#include "harness/pool.hpp"
#include "harness/sysfs.hpp"

namespace
{

using fsuser::tests::Fragments_t;
using fsuser::tests::Mount;

// Flushed per line, since the script reading it waits on each answer before its next step.
void reply(std::string_view text)
{
    std::printf("%.*s\n", static_cast<int>(text.size()), text.data());
    std::fflush(stdout);
}

// "placed <pieces>" or "refused <errno>". EAGAIN is retried until @patience runs out, since that is a
// turn a dead peer still holds.
[[nodiscard]] std::string placePaged(std::string_view path, std::uint64_t bytes, timing::Secs patience)
{
    constexpr timing::Millis Retry{200};
    const timing::Deadline until{patience};
    std::optional<fsuser::File> file;
    bool asked = false;
    while (true)
    {
        try
        {
            if (!file)
            {
                file = fsuser::File::open(path, O_CREAT | O_EXCL | O_RDWR, 0644);
            }
            if (!asked)
            {
                file->setRegionAttr(fsuser::RegionAttr_t{fsuser::CachePolicy::Writeback, fsuser::Placement::Paged});
                asked = true;
            }
            file->resize(bytes);
            return "placed " + std::to_string(file->readRegionAttr().pieces);
        }
        catch (const fsuser::FsCodedError& failure)
        {
            if (failure.code() == std::errc::resource_unavailable_try_again &&
                !until.expired())
            {
                std::this_thread::sleep_for(Retry);
                continue;
            }
            return "refused " + std::to_string(failure.code().value());
        }
    }
}

// Writes the pattern through a mapping and reads it back through read(), as test_paged_extents does.
[[nodiscard]] bool verifyPaged(const Mount& mount, std::string_view name, const Fragments_t& fragments)
{
    auto file = mount.open(name, O_RDWR);
    {
        auto mapping = file.map(fragments.holeBytes, PROT_READ | PROT_WRITE);
        fsuser::tests::writePattern(mapping.get(), fragments.holeBytes, fsuser::tests::PagedSeed);
    }
    return fsuser::tests::readsAcrossGranules(file, fragments.holeBytes, fragments.granule) &&
           fsuser::tests::readsWhole(file, fragments.holeBytes);
}

[[nodiscard]] std::string unlinkName(const Mount& mount, std::string_view name)
{
    try
    {
        mount.unlink(name);
        return "ok";
    }
    catch (const fsuser::FsCodedError& failure)
    {
        return "refused " + std::to_string(failure.code().value());
    }
}

// One command line: place <name> <patience-s>, verify <name>, keepers, free, unlink <name>.
[[nodiscard]] std::string runCommand(const Mount& mount, const Fragments_t& fragments, const std::string& line,
                                     std::vector<std::string>& placed)
{
    std::istringstream words{line};
    std::string verb;
    std::string name;
    std::string extra;
    words >> verb >> name >> extra;

    if (verb == "place")
    {
        const auto patience = fsuser::tests::parseUnsigned(extra).value_or(0);
        placed.push_back(name);
        return placePaged(mount.pathTo(name), fragments.holeBytes,
                          timing::Secs{static_cast<std::int64_t>(patience)});
    }
    if (verb == "verify")
    {
        return verifyPaged(mount, name, fragments) ? "ok" : "bad";
    }
    if (verb == "keepers")
    {
        return fsuser::tests::keepersHold(mount, fragments) ? "ok" : "bad";
    }
    if (verb == "free")
    {
        const auto pools = fsuser::tests::readPoolInfo();
        return pools ? std::to_string(pools->wbFree) : "unknown";
    }
    if (verb == "unlink")
    {
        return unlinkName(mount, name);
    }
    return "unknown";
}

[[nodiscard]] std::int32_t serve(const Mount& mount, const std::string& stem, std::uint32_t holes)
{
    const auto fragments = fsuser::tests::fragmentWritebackPool(mount, stem, holes);
    if (!fragments)
    {
        reply("failed");
        return 1;
    }
    fsuser::tests::writeKeepers(mount, *fragments);
    reply(std::string{"ready "}
              .append(std::to_string(fragments->holeBytes))
              .append(" ")
              .append(std::to_string(fragments->holes.size())));

    std::vector<std::string> placed;
    std::string line;
    while (std::getline(std::cin, line) && line != "quit")
    {
        reply(runCommand(mount, *fragments, line, placed));
    }

    // A name the script already unlinked answers ENOENT here, which is the outcome either way.
    for (const auto& name : placed)
    {
        (void)unlinkName(mount, name);
    }
    fsuser::tests::releaseFragments(mount, *fragments);
    reply("done");
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    constexpr std::int32_t UsageStatus = 2;
    const std::vector<std::string_view> args{argv + 1, argv + argc};

    try
    {
        if (args.size() == 4 && args[0] == "serve")
        {
            const auto holes = fsuser::tests::parseUnsigned(args[3]);
            if (!holes)
            {
                return UsageStatus;
            }
            return serve(Mount{std::string{args[1]}}, std::string{args[2]}, static_cast<std::uint32_t>(*holes));
        }
        if (args.size() == 3 && args[0] == "place")
        {
            const auto bytes = fsuser::tests::parseUnsigned(args[2]);
            if (!bytes)
            {
                return UsageStatus;
            }
            const auto answer = placePaged(args[1], *bytes, timing::Secs{0});
            reply(answer);
            return answer.rfind("placed", 0) == 0 ? 0 : 1;
        }
    }
    catch (const std::exception& failure)
    {
        std::fprintf(stderr, "pool_hold: %s\n", failure.what());
        return 1;
    }

    std::fprintf(stderr, "usage: pool_hold serve <mount> <stem> <holes> | pool_hold place <path> <bytes>\n");
    return UsageStatus;
}
