// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// tests/selector_test.cpp -- the selector kinds and the /proc group read.
//
// The cases read this process's own uid, gid and groups, so they assert against what the test can
// itself name with getuid/getgid/getgroups.

#include "identity/internal/selector.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "harness/creds.hpp"
#include "harness/probe.hpp"
#include "harness/temp_file.hpp"
#include "identity/base.hpp"
#include "model.hpp"

namespace
{

using fsdaemon::Creds_t;
using fsdaemon::Identity_t;
using fsdaemon::identity::BridgeError;
using fsdaemon::identity::Selector;
using fsdaemon::probe::factsForSelf;
using fsdaemon::probe::labelsContain;
using fsdaemon::probe::TempFile;

std::vector<std::uint32_t> ownGroups()
{
    const auto count = ::getgroups(0, nullptr);
    std::vector<::gid_t> raw(count > 0 ? static_cast<std::size_t>(count) : 0);
    if (count > 0)
    {
        const auto filled = ::getgroups(count, raw.data());
        raw.resize(filled > 0 ? static_cast<std::size_t>(filled) : 0);
    }
    std::vector<std::uint32_t> groups;
    groups.reserve(raw.size());
    for (const ::gid_t gid : raw)
    {
        groups.push_back(static_cast<std::uint32_t>(gid));
    }
    std::sort(groups.begin(), groups.end());
    return groups;
}

// Whether @creds satisfies every rule, and the labels they stamp - the AND a rule's match block
// means, as the local backend evaluates it.
// Every label @selectors derive from @creds, the fold a backend runs over its chosen kinds.
std::vector<std::string> labelsFor(const Selector& selectors, const Creds_t& creds)
{
    std::vector<std::string> labels;
    for (const Selector::Type kind : selectors.getKinds())
    {
        auto some = Selector::deriveKindLabels(kind, creds);
        labels.insert(labels.end(), some.begin(), some.end());
    }
    return labels;
}

std::optional<std::vector<std::string>> matchLabels(const std::vector<Selector::MatchRule_t>& rules,
                                                    const Creds_t& creds)
{
    std::vector<std::string> labels;
    for (const Selector::MatchRule_t& rule : rules)
    {
        if (!Selector::matches(rule, creds))
        {
            return std::nullopt;
        }
        labels.push_back(Selector::makeLabel(rule));
    }
    return labels;
}

// A backend that resolves nothing. The credential read belongs to IdentityProvider now, so the
// cases drive it through the smallest concrete backend rather than through a rules file.
class CredsProbe : public fsdaemon::identity::IdentityProvider
{
public:
    explicit CredsProbe(const std::vector<std::string>& names)
        : IdentityProvider{names}
    {
    }

    // NOLINTNEXTLINE(misc-include-cleaner) pid_t is <sys/types.h>'s, mapped to a libc inner header
    [[nodiscard]] Identity_t attest(pid_t, const Creds_t&) override
    {
        return {};
    }

    void reload() override
    {
    }

    using IdentityProvider::resolveCreds;
};

// The FIPS 180-4 digest of "abc". A published constant, so the check does not lean on another call
// of the same library that produced the label.
constexpr std::string_view AbcDigest = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

// The creds a caller running @file would carry. The exe path comes back empty when the stat fails.
Creds_t factsForExe(const TempFile& file)
{
    Creds_t facts;
    struct ::stat info
    {
    };
    if (!file.getPath().empty() && ::stat(file.getPath().c_str(), &info) == 0)
    {
        facts.exePath = file.getPath();
        facts.exeInodeIno = static_cast<std::uint64_t>(info.st_ino);
        // The low 32 bits of a glibc dev_t, which is the form an upcall carries.
        facts.exeInodeDev = static_cast<std::uint32_t>(info.st_dev);
    }
    return facts;
}

// The bytes read() has handed this process so far, or nothing when the kernel does not account
// them. Empty is not a failure, and a case that cannot read the counter skips its cache check.
std::optional<std::uint64_t> readCharsOfSelf()
{
    std::ifstream stream{"/proc/self/io"};
    std::string name;
    std::uint64_t value = 0;
    while (stream >> name >> value)
    {
        if (name == "rchar:")
        {
            return value;
        }
    }
    return std::nullopt;
}

// Waits until the clock leaves the second the exe's ctime names. The selector caches a digest only
// past that point, because a change inside that second could stamp the same ctime again. Bounded,
// so a realtime clock stepped back leaves a failed check and not a probe that never ends.
bool waitForASettledStamp(const std::string& path)
{
    struct ::stat info
    {
    };
    if (::stat(path.c_str(), &info) != 0)
    {
        return false;
    }
    constexpr std::int32_t NapsAllowed = 600;
    for (std::int32_t naps = 0; naps < NapsAllowed; ++naps)
    {
        struct ::timespec now
        {
        };
        if (::clock_gettime(CLOCK_REALTIME_COARSE, &now) != 0)
        {
            return false;
        }
        if (now.tv_sec > info.st_ctim.tv_sec)
        {
            return true;
        }
        const struct ::timespec nap
        {
            0, 5'000'000
        };
        ::nanosleep(&nap, nullptr);
    }
    return false;
}

namespace sha256
{

void checkExeDigest(fsdaemon::probe::Context& ctx)
{
    const TempFile file{"abc", "daemon-sha256"};
    const Creds_t facts = factsForExe(file);
    if (ctx.check(!facts.exePath.empty(), "a temp exe for the digest is written"))
    {
        const Selector sha256Only{{"sha256"}};
        const auto labels = labelsFor(sha256Only, facts);
        ctx.check(labels.size() == 1 && labels.front() == "unix:sha256:" + std::string{AbcDigest},
                  "the sha256 label carries the digest of the exe");
    }
}

void refuseInodeMismatch(fsdaemon::probe::Context& ctx)
{
    const TempFile file{"abc", "daemon-sha256"};
    Creds_t facts = factsForExe(file);
    if (ctx.check(!facts.exePath.empty(), "a temp exe for the refusal is written"))
    {
        facts.exeInodeIno += 1;
        const Selector sha256Only{{"sha256"}};
        bool threw = false;
        try
        {
            labelsFor(sha256Only, facts);
        }
        catch (const BridgeError&)
        {
            threw = true;
        }
        ctx.check(threw, "a file that is not the inode the kernel read is refused");
    }
}

// A rewrite that keeps the length and puts mtime back still raises ctime, and the digest cache keys
// on ctime too, so the next read of the same path digests the content that is there now.
void checkRewriteUnderOldMtime(fsdaemon::probe::Context& ctx)
{
    const TempFile file{"abc", "daemon-sha256-restamp"};
    const Creds_t facts = factsForExe(file);
    if (!ctx.check(!facts.exePath.empty(), "a temp exe for the rewrite case is written"))
    {
        return;
    }
    if (!ctx.check(waitForASettledStamp(file.getPath()), "the exe's stamp settles, so the first digest is cached"))
    {
        return;
    }

    const Selector sha256Only{{"sha256"}};
    const auto firstLabels = labelsFor(sha256Only, facts);
    ctx.check(firstLabels.size() == 1 && firstLabels.front() == "unix:sha256:" + std::string{AbcDigest},
              "the first read digests the exe");

    struct ::stat before
    {
    };
    if (!ctx.check(::stat(file.getPath().c_str(), &before) == 0, "the exe's stamp is read before rewrite"))
    {
        return;
    }

    file.rewrite("abd");
    struct ::timespec sameStamp[2] = {before.st_atim, before.st_mtim};
    ctx.check(::utimensat(AT_FDCWD, file.getPath().c_str(), sameStamp, 0) == 0,
              "the mtime is put back to what the first read saw");

    const auto rewrittenLabels = labelsFor(sha256Only, facts);
    ctx.check(rewrittenLabels.size() == 1 && rewrittenLabels.front() != "unix:sha256:" + std::string{AbcDigest},
              "a rewrite under the old mtime digests to the content that is there now");
}

// What a stale digest would buy an attacker: the rule that admitted the original content keeps
// admitting the file after its content is swapped for another of the same length.
void checkRewrittenExeStopsMatching(fsdaemon::probe::Context& ctx)
{
    const TempFile file{"abc", "daemon-sha256-rule"};
    const Creds_t facts = factsForExe(file);
    if (!ctx.check(!facts.exePath.empty(), "a temp exe for the rule case is written"))
    {
        return;
    }
    if (!ctx.check(waitForASettledStamp(file.getPath()), "the exe's stamp settles, so the rule's digest is cached"))
    {
        return;
    }

    const Selector::MatchRule_t rule{Selector::Sha256, std::string{AbcDigest}};
    ctx.check(Selector::matches(rule, facts), "the rule admits the exe it was written for");

    struct ::stat before
    {
    };
    if (!ctx.check(::stat(file.getPath().c_str(), &before) == 0, "the exe's stamp is read before rewrite"))
    {
        return;
    }

    file.rewrite("abd");
    struct ::timespec sameStamp[2] = {before.st_atim, before.st_mtim};
    ctx.check(::utimensat(AT_FDCWD, file.getPath().c_str(), sameStamp, 0) == 0,
              "the mtime is put back to what the rule's first read saw");

    ctx.check(!Selector::matches(rule, facts), "the rule refuses the exe once its content differs");
}

// A file owner can stamp an mtime further out than a nanosecond count reaches. The stamp saturates
// there, so the selector still reads the exe and still tells one content from another.
void checkExeDigestAtStamp(fsdaemon::probe::Context& ctx, const struct ::timespec& when,
                           const char* named)
{
    const TempFile file{"abc", "daemon-sha256-far"};
    const Creds_t facts = factsForExe(file);
    if (!ctx.checkf(!facts.exePath.empty(), "a temp exe for the %s stamp case is written", named))
    {
        return;
    }

    const struct ::timespec stamp[2] = {when, when};
    if (::utimensat(AT_FDCWD, file.getPath().c_str(), stamp, 0) != 0)
    {
        std::printf("NOTE: this filesystem will not hold an mtime that far out, skipping\n");
        return;
    }

    const Selector sha256Only{{"sha256"}};
    const auto farLabels = labelsFor(sha256Only, facts);
    ctx.checkf(farLabels.size() == 1 && farLabels.front() == "unix:sha256:" + std::string{AbcDigest},
               "an exe with the %s stamp still digests its content", named);

    file.rewrite("abd");
    ctx.checkf(::utimensat(AT_FDCWD, file.getPath().c_str(), stamp, 0) == 0,
               "the %s mtime is put back after the rewrite", named);

    const auto rewrittenLabels = labelsFor(sha256Only, facts);
    ctx.checkf(rewrittenLabels.size() == 1 && rewrittenLabels.front() != "unix:sha256:" + std::string{AbcDigest},
               "a rewrite under the %s mtime digests to the content that is there now", named);
}

void checkExeDigestPastNanosecondLimit(fsdaemon::probe::Context& ctx)
{
    // The year 2413, which a nanosecond count cannot hold and ext4 stores anyway.
    constexpr ::time_t FarSeconds = 14'000'000'000;
    checkExeDigestAtStamp(ctx, {FarSeconds, 0}, "far");

    // The count's last second, read to the nanosecond on both sides of where it runs out.
    constexpr ::time_t LastSeconds = 9'223'372'036;
    constexpr auto LastHeld = 854'775'807L;
    checkExeDigestAtStamp(ctx, {LastSeconds, LastHeld}, "last countable");
    checkExeDigestAtStamp(ctx, {LastSeconds, LastHeld + 1}, "first uncountable");
}

// A digest the cache answers reads none of the exe, so the kernel's read counter for this process
// separates a hit from a second full read of the same content.
void checkCachedUntouchedExe(fsdaemon::probe::Context& ctx)
{
    constexpr std::uint64_t ContentBytes = 4ULL * 1024 * 1024;
    const TempFile file{std::string(ContentBytes, 'a'), "daemon-sha256-cache"};
    const Creds_t facts = factsForExe(file);
    if (!ctx.check(!facts.exePath.empty(), "a temp exe for the cache case is written"))
    {
        return;
    }
    if (!ctx.check(waitForASettledStamp(file.getPath()), "the exe's stamp settles into the past"))
    {
        return;
    }

    const Selector sha256Only{{"sha256"}};
    const auto firstLabels = labelsFor(sha256Only, facts);
    if (!ctx.check(firstLabels.size() == 1, "the first read digests the exe"))
    {
        return;
    }

    const auto beforeCount = readCharsOfSelf();
    const auto cachedLabels = labelsFor(sha256Only, facts);
    const auto afterCount = readCharsOfSelf();
    ctx.check(cachedLabels == firstLabels, "an exe nobody touched digests to what it did before");

    if (!beforeCount || !afterCount)
    {
        std::printf("NOTE: /proc/self/io does not report read bytes here, skipping the cache check\n");
        return;
    }
    const std::uint64_t readDelta = *afterCount - *beforeCount;
    ctx.checkf(readDelta < ContentBytes, "the second digest read %llu bytes, not the exe's %llu",
               static_cast<unsigned long long>(readDelta), static_cast<unsigned long long>(ContentBytes));
}

// hashExe checks the size cap before it touches the cache, so a sparse file just past the cap is
// refused without reading it.
void refuseOverSizeLimit(fsdaemon::probe::Context& ctx)
{
    const TempFile file{"", "daemon-sha256-oversize"};
    if (!ctx.check(!file.getPath().empty(), "a temp exe for the size cap case is written"))
    {
        return;
    }

    constexpr std::uint64_t DigestSizeLimit = 512ULL * 1024 * 1024;
    if (::truncate(file.getPath().c_str(), static_cast<::off_t>(DigestSizeLimit + 1)) != 0)
    {
        std::printf("NOTE: cannot grow a sparse file past the digest size limit here, skipping\n");
        return;
    }

    const Creds_t facts = factsForExe(file);
    if (!ctx.check(!facts.exePath.empty(), "the oversize exe's creds are read"))
    {
        return;
    }

    const Selector sha256Only{{"sha256"}};
    bool threw = false;
    try
    {
        labelsFor(sha256Only, facts);
    }
    catch (const BridgeError&)
    {
        threw = true;
    }
    ctx.check(threw, "an exe past the digest size limit is refused before it is read");
}

}  // namespace sha256

void checkGroupListOnRequest(fsdaemon::probe::Context& ctx)
{
    const Creds_t facts = factsForSelf();
    const auto withGroups = CredsProbe{{"uid", "supplementary_gid"}}.resolveCreds(::getpid(), facts);
    ctx.check(withGroups.has_value(), "a snapshot with groups reads");
    if (withGroups)
    {
        std::vector<std::uint32_t> got{withGroups->supplementaryGids.begin(), withGroups->supplementaryGids.end()};
        std::sort(got.begin(), got.end());
        ctx.check(got == ownGroups(), "the group list matches getgroups()");
    }

    const auto withoutGroups = CredsProbe{{"uid", "path"}}.resolveCreds(::getpid(), facts);
    ctx.check(withoutGroups && withoutGroups->supplementaryGids.empty(),
              "a spec without supplementary_gid reads no group list");
}

void checkIdsMatchKernelRegardless(fsdaemon::probe::Context& ctx)
{
    const Creds_t facts = factsForSelf();
    for (const std::vector<std::string>& names : {std::vector<std::string>{"uid", "supplementary_gid"},
                                                  std::vector<std::string>{"uid", "path"}})
    {
        const auto creds = CredsProbe{names}.resolveCreds(::getpid(), facts);
        ctx.check(creds && creds->uid == facts.uid && creds->gid == facts.gid,
                  "the ids are the kernel's whether or not groups are read");
    }
}

void checkSnapshotIdsMatchKernel(fsdaemon::probe::Context& ctx)
{
    Creds_t facts;
    facts.uid = ::getuid();
    facts.gid = ::getgid();
    const auto creds = CredsProbe{{"uid", "path"}}.resolveCreds(::getpid(), facts);
    ctx.check(creds && creds->uid == ::getuid() && creds->gid == ::getgid(),
              "the ids a snapshot carries are the ones the kernel sent");
}

void checkGroupSelectorLabel(fsdaemon::probe::Context& ctx)
{
    const Creds_t facts = factsForSelf();
    const Selector spec{{"uid", "supplementary_gid"}};
    const auto creds = CredsProbe{{"uid", "supplementary_gid"}}.resolveCreds(::getpid(), facts);
    if (!ctx.check(creds.has_value(), "a snapshot for label derivation reads") || !creds)
    {
        return;
    }
    const std::vector<std::string> labels = labelsFor(spec, *creds);
    ctx.check(labelsContain(labels, "unix:uid:" + std::to_string(::getuid())), "the uid label is derived");
    bool everyGroup = true;
    for (const std::uint32_t gid : ownGroups())
    {
        everyGroup = everyGroup && labelsContain(labels, "unix:supplementary_gid:" + std::to_string(gid));
    }
    ctx.check(everyGroup, "a label is derived for every supplementary gid");
}

void checkPathSelector(fsdaemon::probe::Context& ctx)
{
    const Selector pathOnly{{"path"}};

    const TempFile file{"abc", "daemon-path"};
    Creds_t withPath = factsForExe(file);
    if (ctx.check(!withPath.exePath.empty(), "a temp exe for the path label is written"))
    {
        const auto labels = labelsFor(pathOnly, withPath);
        ctx.check(labels.size() == 1 && labels.front() == "unix:path:" + file.getPath(),
                  "a path the inode confirms derives its label");

        withPath.exeInodeIno += 1;
        ctx.check(labelsFor(pathOnly, withPath).empty(), "a path naming another inode derives no label");
    }

    const Creds_t noPath;
    ctx.check(labelsFor(pathOnly, noPath).empty(), "no exe path derives no label");
}

void checkUidGidMatch(fsdaemon::probe::Context& ctx)
{
    Creds_t creds;
    creds.uid = 1004;
    creds.gid = 100;
    creds.supplementaryGids = {10, 27, 100};

    const auto uidLabels = matchLabels({{Selector::Uid, std::uint32_t{1004}}}, creds);
    ctx.check(uidLabels && uidLabels->size() == 1 && uidLabels->front() == "unix:uid:1004",
              "a matching uid gives its label");
    ctx.check(!matchLabels({{Selector::Uid, std::uint32_t{5}}}, creds), "a wrong uid gives none");

    const auto gidLabels = matchLabels({{Selector::SupplementaryGid, std::uint32_t{27}}}, creds);
    ctx.check(gidLabels && gidLabels->size() == 1 && gidLabels->front() == "unix:supplementary_gid:27",
              "a supplementary gid matches by membership");
    ctx.check(!matchLabels({{Selector::SupplementaryGid, std::uint32_t{99}}}, creds),
              "a gid not in the list does not match");

    const auto bothHold = matchLabels(
        {{Selector::Uid, std::uint32_t{1004}}, {Selector::SupplementaryGid, std::uint32_t{27}}}, creds);
    ctx.check(bothHold && bothHold->size() == 2, "two rules that both hold give both labels");
    ctx.check(!matchLabels({{Selector::Uid, std::uint32_t{1004}}, {Selector::SupplementaryGid, std::uint32_t{99}}},
                           creds),
              "one rule failing drops the whole set");
}

}  // namespace

int main()
{
    return fsdaemon::probe::runProbe(
        [](fsdaemon::probe::Context& ctx)
        {
            checkGroupListOnRequest(ctx);
            checkIdsMatchKernelRegardless(ctx);
            checkSnapshotIdsMatchKernel(ctx);
            checkGroupSelectorLabel(ctx);
            checkPathSelector(ctx);
            sha256::checkExeDigest(ctx);
            sha256::refuseInodeMismatch(ctx);
            sha256::checkRewriteUnderOldMtime(ctx);
            sha256::checkRewrittenExeStopsMatching(ctx);
            sha256::checkExeDigestPastNanosecondLimit(ctx);
            sha256::checkCachedUntouchedExe(ctx);
            sha256::refuseOverSizeLimit(ctx);
            checkUidGidMatch(ctx);
        });
}
