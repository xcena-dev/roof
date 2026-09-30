// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// identity/internal/selector.cpp -- see internal/selector.hpp.

#include "identity/internal/selector.hpp"

#include <fcntl.h>
#include <openssl/evp.h>
#include <openssl/types.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "identity/base.hpp"
#include "model.hpp"
#include "posix/unique_fd.hpp"

namespace fsdaemon::identity
{

namespace
{

// The 32-bit device id an upcall carries for a filesystem, which is new_encode_dev of the
// superblock's dev_t. A glibc dev_t is wider and packs its halves elsewhere, so it is rebuilt.
std::uint32_t encodeDev(::dev_t device) noexcept
{
    constexpr std::uint32_t MinorLowMask = 0xFF;
    constexpr std::uint32_t MajorShift = 8;
    constexpr std::uint32_t MinorHighShift = 12;

    const auto majorPart = static_cast<std::uint32_t>(::major(device));
    const auto minorPart = static_cast<std::uint32_t>(::minor(device));
    return (minorPart & MinorLowMask) | (majorPart << MajorShift) | ((minorPart & ~MinorLowMask) << MinorHighShift);
}

// SHA-256 over a byte stream. Every OpenSSL call lives in here, so a caller feeds bytes and reads
// hex without holding a context of its own.
class Digest
{
public:
    // Throws BridgeError when OpenSSL will not start a context, so a built Digest is always usable.
    Digest()
        : context_{EVP_MD_CTX_new()}
    {
        if (context_ == nullptr || EVP_DigestInit_ex(context_, EVP_sha256(), nullptr) != 1)
        {
            EVP_MD_CTX_free(context_);
            throw BridgeError{"sha256 selector: cannot start a digest"};
        }
    }

    Digest(const Digest&) = delete;
    Digest& operator=(const Digest&) = delete;
    Digest(Digest&&) = delete;
    Digest& operator=(Digest&&) = delete;

    ~Digest() noexcept
    {
        EVP_MD_CTX_free(context_);
    }

    void update(const std::uint8_t* first, const std::uint8_t* last)
    {
        if (EVP_DigestUpdate(context_, first, static_cast<std::size_t>(last - first)) != 1)
        {
            throw BridgeError{"sha256 selector: digest failed"};
        }
    }

    // The digest as 64 lowercase hex characters, the form a rule spells a digest in. Ends the
    // stream, so no update follows.
    [[nodiscard]] std::string finish()
    {
        std::array<std::uint8_t, EVP_MAX_MD_SIZE> raw{};
        std::uint32_t rawBytes = 0;
        if (EVP_DigestFinal_ex(context_, raw.data(), &rawBytes) != 1)
        {
            throw BridgeError{"sha256 selector: digest failed"};
        }

        static constexpr std::uint64_t NibblesPerByte = 2;
        std::string text;
        text.reserve(rawBytes * NibblesPerByte);
        static constexpr std::string_view Digits = "0123456789abcdef";
        static constexpr std::uint32_t NibbleBits = 4;
        static constexpr std::uint32_t NibbleMask = 0x0F;
        std::for_each(raw.begin(), std::next(raw.begin(), rawBytes),
                      [&text](std::uint32_t byte)
                      {
                          text.push_back(Digits[byte >> NibbleBits]);
                          text.push_back(Digits[byte & NibbleMask]);
                      });
        return text;
    }

private:
    EVP_MD_CTX* context_;
};

// Whether the upcall named an exe: a path, and the inode the kernel read at it.
bool hasExe(const Creds_t& creds) noexcept
{
    return !creds.exePath.empty() && creds.exeInodeIno != 0;
}

// Whether @info is the inode the kernel read for the exe in @creds.
bool isKernelInode(const struct ::stat& info, const Creds_t& creds) noexcept
{
    return static_cast<std::uint64_t>(info.st_ino) == creds.exeInodeIno &&
           encodeDev(info.st_dev) == creds.exeInodeDev;
}

// Whether the exe path still names the inode the kernel read. A caller picks the name through its
// own mount namespace, but not the inode this daemon finds at that name in its own.
bool isSameExeInode(const Creds_t& creds)
{
    if (!hasExe(creds))
    {
        return false;
    }
    struct ::stat info
    {
    };
    if (::stat(creds.exePath.c_str(), &info) != 0)
    {
        return false;
    }
    return isKernelInode(info, creds);
}

// What tells one version of an exe from another without reading it. ctime is in the stamp because
// utimensat puts mtime back but raises ctime, so a rewrite cannot wear the stamp it replaced.
struct ExeStamp_t
{
    std::uint64_t device{};
    std::uint64_t inode{};
    std::chrono::nanoseconds written{};
    std::chrono::nanoseconds changed{};
    std::uint64_t bytes{};

    bool operator==(const ExeStamp_t& other) const noexcept
    {
        return device == other.device &&
               inode == other.inode &&
               written == other.written &&
               changed == other.changed &&
               bytes == other.bytes;
    }
};

// Whether a later change to this file is bound to land a different ctime. A filesystem stamps ctime
// from a coarse clock, so a change within the stamp's own second can reproduce it exactly.
bool isStampSettled(const ExeStamp_t& stamp) noexcept
{
    ::timespec now{};
    if (::clock_gettime(CLOCK_REALTIME_COARSE, &now) != 0)
    {
        return false;
    }
    return std::chrono::duration_cast<std::chrono::seconds>(stamp.changed) < std::chrono::seconds{now.tv_sec};
}

// A filesystem holds times past what a nanosecond count reaches, and a stamp only ever tests
// equality, so one that far out saturates instead of overflowing the count.
constexpr std::chrono::nanoseconds makeFileTime(const ::timespec& when) noexcept
{
    constexpr auto Countable = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::nanoseconds::max());
    // The count runs out partway through its last second, so that second is checked to the nanosecond.
    constexpr auto LastPart = std::chrono::nanoseconds::max() - Countable;

    const std::chrono::seconds whole{when.tv_sec};
    const std::chrono::nanoseconds part{when.tv_nsec};
    if (whole > Countable || (whole == Countable && part > LastPart))
    {
        return std::chrono::nanoseconds::max();
    }
    // tv_nsec is never negative, so a whole second inside the count cannot be pushed under it.
    if (whole < -Countable)
    {
        return std::chrono::nanoseconds::min();
    }
    return whole + part;
}

ExeStamp_t makeStamp(const struct ::stat& info) noexcept
{
    return ExeStamp_t{static_cast<std::uint64_t>(info.st_dev),
                      static_cast<std::uint64_t>(info.st_ino),
                      makeFileTime(info.st_mtim),
                      makeFileTime(info.st_ctim),
                      static_cast<std::uint64_t>(info.st_size)};
}

struct CachedDigest_t
{
    ExeStamp_t stamp;
    std::string hex;
};

// A digest costs a full read of the exe, and a caller that maps the same region twice asks for the
// same answer.
std::mutex g_digestLock;
// Keyed on the inode, so a rewritten exe replaces its entry rather than adding one beside it. An
// inode number repeats across devices, so the stamp stored beside the digest is what decides a hit.
std::unordered_map<std::uint64_t, CachedDigest_t> g_digestByInode;

// The exe digest a sha256 selector matches on. The upcall names the exe and carries its inode, so
// the file is re-opened by path and refused unless it is still the inode the kernel read.
std::string hashExe(const Creds_t& creds)
{
    if (!hasExe(creds))
    {
        throw BridgeError{"sha256 selector: the caller has no exe"};
    }

    // O_NONBLOCK so a FIFO left at this path opens instead of stalling this worker on a writer; a
    // regular file's read() below is unaffected by the flag either way.
    const posix::UniqueFd exeFile{::open(creds.exePath.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK)};
    if (!exeFile)
    {
        throw BridgeError{"sha256 selector: cannot open " + creds.exePath};
    }
    struct ::stat info
    {
    };
    if (::fstat(exeFile.get(), &info) != 0)
    {
        throw BridgeError{"sha256 selector: cannot stat " + creds.exePath};
    }
    if (!S_ISREG(info.st_mode) || !isKernelInode(info, creds))
    {
        throw BridgeError{"sha256 selector: " + creds.exePath + " is not the exe the kernel read"};
    }
    // A request must not turn into an unbounded read. An exe past this size is refused rather than
    // hashed, which is the same answer a rule gets when the digest does not match.
    constexpr std::uint64_t DigestSizeLimit = 512ULL * 1024 * 1024;
    if (static_cast<std::uint64_t>(info.st_size) > DigestSizeLimit)
    {
        throw BridgeError{"sha256 selector: " + creds.exePath + " is too large to digest"};
    }

    const auto stamp = makeStamp(info);
    {
        const std::lock_guard guard{g_digestLock};
        const auto known = g_digestByInode.find(stamp.inode);
        if (known != g_digestByInode.end() && known->second.stamp == stamp)
        {
            return known->second.hex;
        }
    }

    // 64 KiB a read. An exe runs to megabytes, so a page at a time would cost hundreds of syscalls.
    constexpr std::uint64_t ChunkBytes = 64ULL * 1024;
    std::array<std::uint8_t, ChunkBytes> chunk{};
    Digest digest{};
    std::uint64_t readBytes = 0;
    for (;;)
    {
        const std::int64_t got = ::read(exeFile.get(), chunk.data(), chunk.size());
        if (got < 0)
        {
            throw BridgeError{"sha256 selector: cannot read " + creds.exePath};
        }
        if (got == 0)
        {
            break;
        }
        readBytes += static_cast<std::uint64_t>(got);
        // The size the stat call saw bounds nothing while a writer is growing the file underneath.
        if (readBytes > DigestSizeLimit)
        {
            throw BridgeError{"sha256 selector: " + creds.exePath + " is too large to digest"};
        }
        digest.update(chunk.data(), chunk.data() + got);
    }

    struct ::stat afterRead
    {
    };
    if (::fstat(exeFile.get(), &afterRead) != 0)
    {
        throw BridgeError{"sha256 selector: cannot stat " + creds.exePath};
    }
    // A digest taken across a rewrite is of no version of the file, so it is refused rather than
    // returned or cached.
    if (!(makeStamp(afterRead) == stamp))
    {
        throw BridgeError{"sha256 selector: " + creds.exePath + " changed while it was read"};
    }

    auto hex = digest.finish();
    if (!isStampSettled(stamp))
    {
        return hex;
    }

    // The cache is dropped whole rather than aged, because a worker reaching this size is serving
    // many exes and no one of them is worth keeping over another.
    constexpr std::uint64_t DigestCacheEntries = 256;
    const std::lock_guard guard{g_digestLock};
    if (g_digestByInode.size() >= DigestCacheEntries)
    {
        g_digestByInode.clear();
    }
    g_digestByInode.insert_or_assign(stamp.inode, CachedDigest_t{stamp, hex});
    return hex;
}

}  // namespace

Selector::Selector(const std::vector<std::string>& names)
{
    if (names.empty())
    {
        addKind(Uid);
        return;
    }
    kinds_.reserve(names.size());
    for (const auto& name : names)
    {
        const auto kind = findType(name);
        if (!kind)
        {
            throw BridgeError{"selector: unknown kind '" + name + "'"};
        }
        addKind(*kind);
    }
}

void Selector::addKind(Type kind)
{
    if (isSupported(kind))
    {
        return;
    }
    kinds_.push_back(kind);
    supported_ |= static_cast<std::uint8_t>(1U << kind);
}

bool Selector::isSupported(Type kind) const noexcept
{
    return (supported_ & (1U << kind)) != 0;
}

std::optional<Selector::Type> Selector::parseType(std::string_view name) const
{
    const auto kind = findType(name);
    if (!kind || !isSupported(*kind))
    {
        return std::nullopt;
    }
    return kind;
}

bool Selector::isKnownName(std::string_view name) noexcept
{
    return findType(name).has_value();
}

std::optional<Selector::Type> Selector::findType(std::string_view name) noexcept
{
    // The keys are views into Rows' literals, which outlive the map.
    static const auto ByWord = []
    {
        std::unordered_map<std::string_view, Type> built;
        built.reserve(std::size(Rows));
        for (const auto& [kind, word] : Rows)
        {
            built.emplace(word, kind);
        }
        return built;
    }();

    const auto found = ByWord.find(name);
    if (found == ByWord.end())
    {
        return std::nullopt;
    }
    return found->second;
}

std::string Selector::makeLabel(const MatchRule_t& rule)
{
    const auto text = isIntType(rule.kind) ? std::to_string(std::get<std::uint32_t>(rule.expected))
                                           : std::get<std::string>(rule.expected);
    // Rows sit in enumerator order, so the kind is the index of its own row.
    return std::string{"unix:"}.append(Rows[rule.kind].second).append(":").append(text);
}

bool Selector::matches(const MatchRule_t& rule, const Creds_t& creds)
{
    const auto& expected = rule.expected;
    switch (rule.kind)
    {
        case Uid:
            return creds.uid == std::get<std::uint32_t>(expected);
        case Gid:
            return creds.gid == std::get<std::uint32_t>(expected);
        case SupplementaryGid:
            return creds.supplementaryGids.count(std::get<std::uint32_t>(expected)) != 0;
        case Path:
            return creds.exePath == std::get<std::string>(expected) && isSameExeInode(creds);
        case Sha256:
            return hashExe(creds) == std::get<std::string>(expected);
    }
    throw BridgeError{"unknown selector kind"};
}

std::vector<std::string> Selector::deriveKindLabels(Type kind, const Creds_t& creds)
{
    std::vector<std::string> labels;
    switch (kind)
    {
        case Uid:
            labels.push_back(makeLabel({kind, creds.uid}));
            break;
        case Gid:
            labels.push_back(makeLabel({kind, creds.gid}));
            break;
        case SupplementaryGid:
            labels.reserve(creds.supplementaryGids.size());
            for (const auto gid : creds.supplementaryGids)
            {
                labels.push_back(makeLabel({kind, gid}));
            }
            break;
        case Path:
            // A path the daemon cannot tie to the kernel's inode derives nothing, not a label.
            if (isSameExeInode(creds))
            {
                labels.push_back(makeLabel({kind, creds.exePath}));
            }
            break;
        case Sha256:
            labels.push_back(makeLabel({kind, hashExe(creds)}));
            break;
    }
    return labels;
}

}  // namespace fsdaemon::identity
