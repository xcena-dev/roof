// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// tests/security_identity_test.cpp -- the identity facts a caller can choose for itself.
//
// Every case here states a property the daemon needs and does not yet have, so a red line is the
// finding and a green line is the day it was closed.

#include <fcntl.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "harness/creds.hpp"
#include "harness/probe.hpp"
#include "harness/temp_file.hpp"
#include "identity/base.hpp"
#include "identity/internal/selector.hpp"
#include "identity/internal/spiffe.hpp"
#include "model.hpp"
#include "posix/unique_fd.hpp"

namespace
{

using fsdaemon::Creds_t;
using fsdaemon::Identity_t;
using fsdaemon::identity::parseSpiffePath;
using fsdaemon::identity::Selector;
using fsdaemon::probe::factsForSelf;
using fsdaemon::probe::TempFile;

// The argv word the re-exec below answers to, and the path it reports through.
constexpr const char* ReportWord = "--report-exe";

// A backend that resolves nothing, so a case drives the credential read without a rules file.
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

// A1 of this probe: the path selector should not honour a name the daemon cannot confirm.
void refuseUnverifiedExePath(fsdaemon::probe::Context& ctx)
{
    Creds_t claimed;
    claimed.uid = ::getuid();
    claimed.exePath = "/usr/bin/trusted-worker-that-does-not-exist";
    claimed.exeInodeIno = 0;
    claimed.exeInodeDev = 0;

    const Selector::MatchRule_t rule{Selector::Path, std::string{claimed.exePath}};
    const bool matched = Selector::matches(rule, claimed);

    ctx.check(!matched,
              "a path selector refuses an exe path with no inode behind it (open: it matches)");
}

// The path selector's own comparison: a claimed exe path is refused when its current inode is not
// the one the claim names, and matches once it is.
void checkPathMatchesClaimedInode(fsdaemon::probe::Context& ctx)
{
    const TempFile carrier{"daemon-e1", "daemon-security-path"};
    if (!ctx.check(!carrier.getPath().empty(), "a file for the path case is written"))
    {
        return;
    }

    struct ::stat otherInfo
    {
    };
    if (!ctx.check(::stat("/proc/self/exe", &otherInfo) == 0, "a different file's inode reads"))
    {
        return;
    }

    Creds_t wrong;
    wrong.exePath = carrier.getPath();
    wrong.exeInodeIno = static_cast<std::uint64_t>(otherInfo.st_ino);
    wrong.exeInodeDev = static_cast<std::uint32_t>(otherInfo.st_dev);
    const Selector::MatchRule_t rule{Selector::Path, std::string{wrong.exePath}};
    ctx.check(!Selector::matches(rule, wrong),
              "a path selector refuses a claimed exe whose inode belongs to a different file");

    struct ::stat carrierInfo
    {
    };
    if (!ctx.check(::stat(carrier.getPath().c_str(), &carrierInfo) == 0, "the carrier file stats"))
    {
        return;
    }
    Creds_t right;
    right.exePath = carrier.getPath();
    right.exeInodeIno = static_cast<std::uint64_t>(carrierInfo.st_ino);
    right.exeInodeDev = static_cast<std::uint32_t>(carrierInfo.st_dev);
    ctx.check(Selector::matches(rule, right),
              "the same path selector matches once the claimed inode is the carrier's own");
}

// The contrast: the digest selector reopens the named file and holds it to the inode the kernel
// read, so the same forgery does not reach it.
void refuseForgedInodeDigest(fsdaemon::probe::Context& ctx)
{
    const TempFile carrier{"abc", "daemon-security"};
    if (!ctx.check(!carrier.getPath().empty(), "a file for the digest case is written"))
    {
        return;
    }

    Creds_t claimed;
    claimed.exePath = carrier.getPath();
    claimed.exeInodeIno = 1;
    claimed.exeInodeDev = 1;

    bool refused = false;
    try
    {
        static_cast<void>(Selector::matches({Selector::Sha256, std::string(64, 'a')}, claimed));
    }
    catch (const fsdaemon::identity::BridgeError&)
    {
        refused = true;
    }
    ctx.check(refused, "a digest selector refuses a file that is not the inode the kernel read");
}

// The digest is recomputed on every call, so the same path with new bytes answers differently. That
// is what makes a large exe a cost the caller chooses.
void checkDigestRecomputedEachCall(fsdaemon::probe::Context& ctx)
{
    const TempFile carrier{"abc", "daemon-security"};
    if (!ctx.check(!carrier.getPath().empty(), "a file for the recompute case is written"))
    {
        return;
    }

    Creds_t facts;
    facts.exePath = carrier.getPath();
    struct ::stat info
    {
    };
    if (!ctx.check(::stat(carrier.getPath().c_str(), &info) == 0, "the carrier file stats"))
    {
        return;
    }
    facts.exeInodeIno = static_cast<std::uint64_t>(info.st_ino);
    facts.exeInodeDev = static_cast<std::uint32_t>(info.st_dev);

    const auto first = Selector::deriveKindLabels(Selector::Sha256, facts);
    carrier.rewrite("abcd");
    const auto second = Selector::deriveKindLabels(Selector::Sha256, facts);

    ctx.check(first.size() == 1 && second.size() == 1 && first.front() != second.front(),
              "the digest selector reads the whole file on every request, so its cost is the caller's");
}

// A2 of this probe: the group list is read only for a pid whose start time the facts confirm, so a
// pid naming any other process carries no groups into the identity.
void refuseUnnamedPid(fsdaemon::probe::Context& ctx)
{
    // Far enough past the tick the comparison rounds to that a wrong value cannot land inside it.
    constexpr std::uint64_t OneSecondNs = 1000000000;

    const Creds_t facts = factsForSelf();
    const CredsProbe reader{{"uid", "supplementary_gid"}};

    if (!ctx.check(facts.startBoottimeNs != 0, "this process's own start time reads"))
    {
        return;
    }
    if (!ctx.check(reader.resolveCreds(::getpid(), facts).has_value(),
                   "the group list of the pid the facts name reads"))
    {
        return;
    }

    ctx.check(!reader.resolveCreds(1, facts).has_value(),
              "a live pid the facts do not name is refused");

    Creds_t stale = facts;
    stale.startBoottimeNs += OneSecondNs;
    ctx.check(!reader.resolveCreds(::getpid(), stale).has_value(),
              "the caller's own pid is refused once the start time no longer matches");
}

// The shape of the pid reuse this closes: a pid whose process is gone answers for nobody, whatever
// took the number afterwards.
void refuseExitedPid(fsdaemon::probe::Context& ctx)
{
    const Creds_t facts = factsForSelf();
    const CredsProbe reader{{"uid", "supplementary_gid"}};

    const auto departed = ::fork();
    if (departed == 0)
    {
        ::_exit(0);
    }
    if (!ctx.check(departed > 0, "a child for the departed pid case forks"))
    {
        return;
    }
    std::int32_t status = 0;
    static_cast<void>(::waitpid(departed, &status, 0));

    ctx.check(!reader.resolveCreds(departed, facts).has_value(),
              "a pid whose process has been reaped carries no group list");
}

// Which task the group list belongs to when the two disagree. An upcall names the thread group by
// its leader's pid, so the list is the process's and a thread's own setgroups does not narrow it.
void checkProcessGroupExcludesThreads(fsdaemon::probe::Context& ctx)
{
    const Creds_t facts = factsForSelf();
    const CredsProbe reader{{"uid", "supplementary_gid"}};

    const auto whole = reader.resolveCreds(::getpid(), facts);
    if (!ctx.check(whole.has_value(), "the group list of the calling process reads") || !whole)
    {
        return;
    }
    if (whole->supplementaryGids.empty())
    {
        std::printf(
            "SKIP: this account carries no supplementary group, so a thread has none "
            "to drop\n");
        return;
    }

    bool narrowed = false;
    std::optional<Creds_t> fromThread;
    std::thread caller{[&narrowed, &fromThread, &reader, &facts]
                       {
                           // The raw call, because glibc's wrapper broadcasts the change to every
                           // thread and one thread differing from its leader is the case.
                           narrowed = ::syscall(SYS_setgroups, 0, nullptr) == 0 &&
                                      ::getgroups(0, nullptr) == 0;
                           if (narrowed)
                           {
                               fromThread = reader.resolveCreds(::getpid(), facts);
                           }
                       }};
    caller.join();

    if (!narrowed)
    {
        std::printf(
            "SKIP: dropping one thread's groups needs CAP_SETGID, which this run has "
            "not\n");
        return;
    }

    ctx.check(fromThread && fromThread->supplementaryGids == whole->supplementaryGids,
              "a thread that dropped its own groups is still read as the process it belongs to");
}

// A3 of this probe: a repeated key in a SPIFFE id should be refused rather than resolved by
// position.
void refuseDuplicateSpiffeKey(fsdaemon::probe::Context& ctx)
{
    const auto [group, role] = parseSpiffePath("spiffe://test.local/group/eng/role/reader/role/admin");

    // An id with a repeated key was issued by nobody, so no half of it is an identity.
    ctx.check(group.empty(), "a repeated key leaves no group either");
    ctx.check(role != "admin",
              "a repeated role key is refused rather than taken from the last segment (open: the "
              "last one wins)");
}

// What the re-exec reports about itself: the name the kernel renders for its exe, and the inode
// that name resolves to inside the child's own namespace.
struct ForgedFacts_t
{
    std::string path;
    std::uint64_t inode{};
    std::uint32_t device{};
};

// Reports what the kernel renders as this process's exe path, and the inode behind that name.
void reportOwnExeFacts(const char* writeFdText)
{
    std::array<char, 4096> rendered{};
    const auto got = ::readlink("/proc/self/exe", rendered.data(), rendered.size() - 1);
    if (got <= 0)
    {
        return;
    }
    struct ::stat info
    {
    };
    if (::stat("/proc/self/exe", &info) != 0)
    {
        return;
    }

    const std::string reply = std::string{rendered.data(), static_cast<std::uint64_t>(got)} + "\n" +
                              std::to_string(info.st_ino) + "\n" + std::to_string(info.st_dev) + "\n";
    const std::int32_t writeFd = std::atoi(writeFdText);
    const auto put = ::write(writeFd, reply.data(), reply.size());
    static_cast<void>(put);
}

// The three lines the child wrote, or nullopt when it wrote anything else.
[[nodiscard]] std::optional<ForgedFacts_t> parseForgedFacts(const std::string& text)
{
    const auto pathEnd = text.find('\n');
    if (pathEnd == std::string::npos)
    {
        return std::nullopt;
    }
    const auto inodeEnd = text.find('\n', pathEnd + 1);
    if (inodeEnd == std::string::npos)
    {
        return std::nullopt;
    }
    const auto deviceEnd = text.find('\n', inodeEnd + 1);
    if (deviceEnd == std::string::npos)
    {
        return std::nullopt;
    }

    ForgedFacts_t facts;
    facts.path = text.substr(0, pathEnd);
    facts.inode = std::stoull(text.substr(pathEnd + 1, inodeEnd - pathEnd - 1));
    facts.device =
        static_cast<std::uint32_t>(std::stoull(text.substr(inodeEnd + 1, deviceEnd - inodeEnd - 1)));
    return facts;
}

// The child half: take a private user and mount namespace, put this binary over @target, and exec
// that name. False when the kernel refuses the namespace, which is a host with those disabled.
bool runForgedExec(const std::string& target, std::int32_t writeFd)
{
    const auto callerUid = ::getuid();
    const auto callerGid = ::getgid();

    if (::unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0)
    {
        return false;
    }

    // open(2) and not fopen, because the id maps refuse the truncate a "w" stream asks for.
    const auto writeMap = [](const char* path, const std::string& line)
    {
        const fsdaemon::posix::UniqueFd held{::open(path, O_WRONLY | O_CLOEXEC)};
        if (!held)
        {
            return false;
        }
        const auto put = ::write(held.get(), line.data(), line.size());
        return put > 0 && static_cast<std::uint64_t>(put) == line.size();
    };

    static_cast<void>(writeMap("/proc/self/setgroups", "deny"));
    if (!writeMap("/proc/self/uid_map", "0 " + std::to_string(callerUid) + " 1") ||
        !writeMap("/proc/self/gid_map", "0 " + std::to_string(callerGid) + " 1"))
    {
        return false;
    }

    // A mount that propagates back out would change the host, so the tree is made private first.
    if (::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0)
    {
        return false;
    }
    // The resolved name and not the procfs link, which a bind refuses as a source.
    std::array<char, 4096> own{};
    if (::readlink("/proc/self/exe", own.data(), own.size() - 1) <= 0)
    {
        return false;
    }
    if (::mount(own.data(), target.c_str(), nullptr, MS_BIND, nullptr) != 0)
    {
        return false;
    }

    const std::string fdText = std::to_string(writeFd);
    ::execl(target.c_str(), target.c_str(), ReportWord, fdText.c_str(), nullptr);
    return false;
}

// A4 of this probe: the exe path the kernel renders should not be a name the caller chose.
void refuseCallerChosenExePath(fsdaemon::probe::Context& ctx)
{
    const TempFile target{"", "daemon-security-target"};
    if (!ctx.check(!target.getPath().empty(), "a bind target for the exec case is written"))
    {
        return;
    }

    std::array<std::int32_t, 2> pipeFds{};
    if (!ctx.check(::pipe(pipeFds.data()) == 0, "a pipe for the child's answer opens"))
    {
        return;
    }

    const auto child = ::fork();
    if (child < 0)
    {
        ctx.check(false, "a child for the exec case forks");
        ::close(pipeFds[0]);
        ::close(pipeFds[1]);
        return;
    }
    if (child == 0)
    {
        ::close(pipeFds[0]);
        static_cast<void>(runForgedExec(target.getPath(), pipeFds[1]));
        ::_exit(1);
    }

    ::close(pipeFds[1]);
    std::array<char, 4096> answer{};
    const auto got = ::read(pipeFds[0], answer.data(), answer.size() - 1);
    ::close(pipeFds[0]);
    std::int32_t status = 0;
    static_cast<void>(::waitpid(child, &status, 0));

    if (got <= 0)
    {
        std::printf(
            "SKIP: this host refuses an unprivileged user namespace, so the forgery could "
            "not be driven here\n");
        return;
    }

    const auto reported = parseForgedFacts(std::string{answer.data(), static_cast<std::uint64_t>(got)});
    ctx.check(reported.has_value(), "the re-exec reports its rendered path and its own inode");
    if (!reported.has_value())
    {
        return;
    }
    const ForgedFacts_t& facts = reported.value();

    // The kernel renders the name the caller bound, which is the forgery this case exists to drive.
    // What holds the rule against it is the inode, checked below, and not the rendered name.
    if (facts.path != target.getPath())
    {
        std::printf("SKIP: the kernel rendered another name, so the forgery below was not driven\n");
        return;
    }

    // What the forged path runs into. The name is the rule's own, and the daemon resolves it in its
    // own namespace, where it is still the file this process wrote there.
    Creds_t forged;
    forged.exePath = facts.path;
    forged.exeInodeIno = facts.inode;
    forged.exeInodeDev = facts.device;

    const Selector::MatchRule_t rule{Selector::Path, target.getPath()};
    ctx.check(!Selector::matches(rule, forged),
              "a path rule naming that name refuses the caller that bound its own binary over it");
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc == 3 && std::string{argv[1]} == ReportWord)
    {
        reportOwnExeFacts(argv[2]);
        return 0;
    }

    return fsdaemon::probe::runProbe(
        [](fsdaemon::probe::Context& ctx)
        {
            refuseUnverifiedExePath(ctx);
            checkPathMatchesClaimedInode(ctx);
            refuseForgedInodeDigest(ctx);
            checkDigestRecomputedEachCall(ctx);
            refuseUnnamedPid(ctx);
            refuseExitedPid(ctx);
            checkProcessGroupExcludesThreads(ctx);
            refuseDuplicateSpiffeKey(ctx);
            refuseCallerChosenExePath(ctx);
        });
}
