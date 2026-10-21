// SPDX-License-Identifier: Apache-2.0
//
// pool.hpp -- the write-back pool filled up except for holes a case cuts into it.
//
// A paged placement lies in pieces only when no single gap holds it, so a case that wants pieces
// fills every gap first and then frees granules that something taken sits next to.

#pragma once

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <vector>

#include "fs/errors.hpp"
#include "fs/file.hpp"
#include "harness/mount.hpp"
#include "harness/sysfs.hpp"

namespace fsuser::tests
{

// What a paged region over the holes writes, apart from every seed a neighbour gets.
constexpr std::uint64_t PagedSeed = 0x9E3779B97F4A7C15ULL;

// The names a fragmented pool stands on, and the free runs a paged placement of holeBytes fills,
// one piece each. Holes that adjoin make one run.
struct Fragments_t
{
    std::vector<std::string> fillers;
    std::vector<std::string> keepers;
    std::uint64_t granule{0};
    std::uint64_t holeBytes{0};
    std::vector<Piece_t> holes;
};

// Created and placed contiguous, or false with the name gone again when no gap holds @bytes.
[[nodiscard]] inline bool placeContiguous(const Mount& mount, const std::string& name, std::uint64_t bytes)
{
    {
        auto file = mount.open(name, O_CREAT | O_EXCL | O_RDWR, 0644);
        try
        {
            file.resize(bytes);
            return true;
        }
        catch (const FsCodedError& failure)
        {
            if (failure.code() != std::errc::no_space_on_device)
            {
                throw;
            }
        }
    }
    mount.unlink(name);
    return false;
}

// Bounded, so a free that never lands ends the case with a failed check instead of hanging it.
[[nodiscard]] inline bool waitForWritebackFree(std::uint64_t bytes)
{
    constexpr std::chrono::seconds Deadline{60};
    constexpr std::chrono::milliseconds Poll{100};
    const auto until = std::chrono::steady_clock::now() + Deadline;
    while (std::chrono::steady_clock::now() < until)
    {
        const auto pools = readPoolInfo();
        if (pools && pools->wbFree == bytes)
        {
            return true;
        }
        std::this_thread::sleep_for(Poll);
    }
    return false;
}

// Contiguous fillers until not one granule is free. Each takes the largest size that still lands, so
// a pool with few gaps takes few names.
[[nodiscard]] inline std::vector<std::string> fillWritebackPool(const Mount& mount, const std::string& stem)
{
    std::vector<std::string> fillers;
    const auto pools = readPoolInfo();
    if (!pools || pools->granule == 0)
    {
        return fillers;
    }

    const std::uint64_t granule = pools->granule;
    std::uint64_t size = pools->wbFree / granule * granule;
    while (size >= granule)
    {
        const auto name = stem + std::to_string(fillers.size());
        if (!placeContiguous(mount, name, size))
        {
            size = size / 2 / granule * granule;
            continue;
        }
        fillers.push_back(name);
        const auto left = readPoolInfo();
        size = left ? std::min(size, left->wbFree / granule * granule) : 0;
    }
    return fillers;
}

// Fills the pool around 2 x @holes granule-sized names and frees every other one, so each hole has a
// taken neighbour. Two freed names may still adjoin, so the pieces are counted off where they sat.
[[nodiscard]] inline std::optional<Fragments_t> fragmentWritebackPool(const Mount& mount, const std::string& stem,
                                                                      std::uint32_t holes)
{
    const auto pools = readPoolInfo();
    if (!pools || pools->granule == 0)
    {
        return std::nullopt;
    }

    Fragments_t made;
    made.granule = pools->granule;
    std::vector<std::string> cut;
    for (std::uint32_t pair = 0; pair < holes; ++pair)
    {
        const auto freed = stem + "hole" + std::to_string(pair);
        const auto kept = stem + "keep" + std::to_string(pair);
        if (!placeContiguous(mount, freed, made.granule) || !placeContiguous(mount, kept, made.granule))
        {
            return std::nullopt;
        }
        cut.push_back(freed);
        made.keepers.push_back(kept);
    }
    made.fillers = fillWritebackPool(mount, stem + "fill");

    const std::unordered_set<std::string> cutNames{cut.begin(), cut.end()};
    std::vector<std::uint64_t> offsets;
    for (const auto& row : readRegionRows())
    {
        if (cutNames.count(row.name) != 0)
        {
            offsets.push_back(row.offset);
        }
    }
    if (offsets.size() != cut.size())
    {
        return std::nullopt;
    }
    std::sort(offsets.begin(), offsets.end());
    for (const std::uint64_t offset : offsets)
    {
        if (!made.holes.empty() && made.holes.back().offset + made.holes.back().length == offset)
        {
            made.holes.back().length += made.granule;
            continue;
        }
        made.holes.push_back({offset, made.granule});
    }

    for (const auto& name : cut)
    {
        mount.unlink(name);
    }
    made.holeBytes = std::uint64_t{holes} * made.granule;
    if (!waitForWritebackFree(made.holeBytes))
    {
        return std::nullopt;
    }
    return made;
}

// Every name the fragmentation made that still stands, so the space goes back before the case ends.
inline void releaseFragments(const Mount& mount, const Fragments_t& fragments)
{
    for (const auto& name : fragments.keepers)
    {
        mount.unlink(name);
    }
    for (const auto& name : fragments.fillers)
    {
        mount.unlink(name);
    }
}

// One word per 8 bytes, each its file offset mixed with @seed, so a word read from the wrong place
// or from another region does not pass.
inline void writePattern(void* base, std::uint64_t bytes, std::uint64_t seed)
{
    auto* const words = static_cast<std::uint64_t*>(base);
    const std::uint64_t count = bytes / sizeof(std::uint64_t);
    for (std::uint64_t word = 0; word < count; ++word)
    {
        words[word] = seed ^ (word * sizeof(std::uint64_t));
    }
}

// @fileOffset is where @base starts in the file, for a buffer a read filled from the middle.
[[nodiscard]] inline bool hasPattern(const void* base, std::uint64_t bytes, std::uint64_t seed,
                                     std::uint64_t fileOffset = 0)
{
    const auto* const words = static_cast<const std::uint64_t*>(base);
    const std::uint64_t count = bytes / sizeof(std::uint64_t);
    for (std::uint64_t word = 0; word < count; ++word)
    {
        if (words[word] != (seed ^ (fileOffset + word * sizeof(std::uint64_t))))
        {
            return false;
        }
    }
    return true;
}

// A seed per neighbour, apart from PagedSeed, so no neighbour's word reads as the paged region's.
[[nodiscard]] inline std::uint64_t seedOfKeeper(std::uint64_t index)
{
    return PagedSeed + index + 1;
}

inline void writeKeepers(const Mount& mount, const Fragments_t& fragments)
{
    std::uint64_t index = 0;
    for (const auto& name : fragments.keepers)
    {
        auto file = mount.open(name, O_RDWR);
        auto mapping = file.map(fragments.granule, PROT_READ | PROT_WRITE);
        writePattern(mapping.get(), fragments.granule, seedOfKeeper(index++));
    }
}

[[nodiscard]] inline bool keepersHold(const Mount& mount, const Fragments_t& fragments)
{
    std::uint64_t index = 0;
    for (const auto& name : fragments.keepers)
    {
        auto file = mount.open(name, O_RDONLY);
        auto mapping = file.map(fragments.granule, PROT_READ);
        if (!hasPattern(mapping.get(), fragments.granule, seedOfKeeper(index++)))
        {
            return false;
        }
    }
    return true;
}

// read() across each granule boundary, a page on either side, since that is where one piece may end
// and the next begin anywhere on the device.
[[nodiscard]] inline bool readsAcrossGranules(const File& file, std::uint64_t bytes, std::uint64_t granule)
{
    constexpr std::uint64_t PageBytes = 4096;
    constexpr std::uint64_t SpanBytes = 2 * PageBytes;
    std::vector<std::uint64_t> buffer(SpanBytes / sizeof(std::uint64_t));
    for (std::uint64_t boundary = granule; boundary < bytes; boundary += granule)
    {
        const std::uint64_t from = boundary - PageBytes;
        const std::int64_t got = ::pread(file.get(), buffer.data(), SpanBytes, static_cast<::off_t>(from));
        if (got != static_cast<std::int64_t>(SpanBytes) || !hasPattern(buffer.data(), SpanBytes, PagedSeed, from))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool readsWhole(const File& file, std::uint64_t bytes)
{
    std::vector<std::uint64_t> buffer(bytes / sizeof(std::uint64_t));
    const std::int64_t got = ::pread(file.get(), buffer.data(), bytes, 0);
    return got == static_cast<std::int64_t>(bytes) && hasPattern(buffer.data(), bytes, PagedSeed);
}

}  // namespace fsuser::tests
