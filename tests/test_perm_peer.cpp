// SPDX-License-Identifier: Apache-2.0
//
// test_perm_peer -- what a node that does not own a file may do with it.
//
// Two mounts of one device, so the same process reaches the same region as two nodes. The peer is
// this process seen through the second mount, which is what makes the ladder below one program:
// the grant names (peer node, this pid), and the check goes through the other mount point.
//
// Every claim here is read through an ioctl and never through a mapping. A mapping the rows refuse
// goes on to ask the helper, and the helper answers on the caller's account rather than on what
// this region wrote down, so it cannot say which row decided.
//
//   test_perm_peer <mount-a> <mount-b>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <system_error>
#include <vector>

#include "fs/errors.hpp"
#include "fs/file.hpp"
#include "harness/harness.hpp"
#include "harness/mount.hpp"

namespace
{

// Who the peer names when it passes a permission on. Nobody holds this uid, and nobody needs to:
// what the escalation ladder asks is which masks the kernel writes down, not who ends up holding
// them.
constexpr std::uint32_t OnwardUid = 62000;

// A group no account on a test host holds, checked below rather than assumed. A row carrying it
// names a group the caller is not in, which is what the pair case needs.
constexpr std::uint32_t OutsideGid = 63000;

// An account no row in this case ever carries, so a revoke naming it removes nothing and answers
// only whether the caller could have reached a row at all.
constexpr std::uint32_t AbsentUid = 62500;

using fsuser::tests::caseName;

// getgroups and not a compare against getgid: a row's gid matches a supplementary group too, so
// only the whole set answers whether the caller is outside it.
[[nodiscard]] bool callerHoldsGroup(std::uint32_t group)
{
    const auto wanted = static_cast<::gid_t>(group);
    if (::getgid() == wanted || ::getegid() == wanted)
    {
        return true;
    }

    const std::int32_t count = ::getgroups(0, nullptr);
    if (count <= 0)
    {
        return false;
    }

    std::vector<::gid_t> held(static_cast<std::size_t>(count));
    if (::getgroups(static_cast<int>(count), held.data()) < 0)
    {
        return false;
    }

    return std::find(held.begin(), held.end(), wanted) != held.end();
}

// What the rows on the peer's node say about the caller, asked without changing any of them.
//
// A revoke naming an account no row carries answers ENOENT to a caller holding ADMIN and EACCES to
// one that does not, and removes nothing either way. ADMIN is the rung no policy hands out, so it
// is the one an ioctl can still read off the record alone.
//
// The errno is the answer rather than a bool, so a case that expected the other one says which it
// got.
[[nodiscard]] std::int32_t askForAdmin(fsuser::tests::Mount& peer, const std::string& name)
{
    try
    {
        auto file = peer.open(name, O_RDWR);
        file.revokePermission(AbsentUid, fsuser::AnyId);
    }
    catch (const fsuser::FsCodedError& answered)
    {
        return answered.code().value();
    }
    catch (const std::exception&)
    {
        return -1;
    }
    return 0;
}

// A peer reaches a region through an account row on its own node, because uid and gid are that
// node's numbers and mean nothing on another host. The owner cannot write that row from here, so
// it opens the door with the default once, the peer writes its own row, and the door shuts again.
void checkGrantLadder(fsuser::tests::Report& report, fsuser::tests::Mount& owner,
                      fsuser::tests::Mount& peer, const std::string& name)
{
    auto file = fsuser::tests::placeFile(owner, name);
    const auto account = static_cast<std::uint32_t>(::getuid());

    report.section("a peer with no row of its own");
    const auto bare = askForAdmin(peer, name);
    report.checkErrno("the peer holds no ADMIN", bare == EACCES, bare);

    report.section("the peer writes its own row");
    auto seen = peer.open(name, O_RDWR);
    fsuser::tests::runThroughOpenDoor(
        file, fsuser::Permission::Grant | fsuser::Permission::Read,
        [&]
        {
            report.checkAccepted(
                "the peer grants its own account READ",
                [&]
                {
                    seen.grantPermission(account, fsuser::AnyId,
                                         fsuser::Permission::Read);
                });
        });

    // The door the row was written through is shut again, so what answers below is the row alone.
    // READ is what it carries, and a row carrying READ is still a row carrying no ADMIN.
    const auto readOnly = askForAdmin(peer, name);
    report.checkErrno("a row carrying READ hands on no ADMIN", readOnly == EACCES, readOnly);

    // READ is not GRANT, so a peer cannot widen the row it holds. Only the owner or the helper on
    // that node reaches further.
    report.section("the row is a ceiling, not a foothold");
    report.checkRefusedWith("the peer cannot widen its own row", std::errc::permission_denied,
                            [&]
                            {
                                seen.grantPermission(account, fsuser::AnyId,
                                                     fsuser::Permission::Read | fsuser::Permission::Write);
                            });

    owner.unlink(name);
}

// A row carrying both ids names the pair and not either half. The caller's own uid is on the row,
// so a rule that let either half answer alone would open the region to it.
void checkTwoIdRow(fsuser::tests::Report& report, fsuser::tests::Mount& owner,
                   fsuser::tests::Mount& peer, const std::string& name)
{
    auto file = fsuser::tests::placeFile(owner, name);
    const auto account = static_cast<std::uint32_t>(::getuid());

    report.section("a row naming a uid and a group the caller is not in");
    report.check("the caller is outside that group", !callerHoldsGroup(OutsideGid));

    fsuser::tests::runThroughOpenDoor(
        file, fsuser::Permission::Grant | fsuser::Permission::Admin,
        [&]
        {
            auto seen = peer.open(name, O_RDWR);
            report.checkAccepted(
                "the peer writes a row naming its uid and that group",
                [&]
                {
                    seen.grantPermission(account, OutsideGid,
                                         fsuser::Permission::Admin);
                });
        });

    // The door the row was written through is shut again, so what answers below is the row alone.
    // ADMIN is what the row carries, so a caller the row answered for would hold it.
    const auto pair = askForAdmin(peer, name);
    report.checkErrno("the row does not answer for the caller", pair == EACCES, pair);

    owner.unlink(name);
}

// GRANT lets a peer hand on what it holds. ADMIN is what lets a peer hand on more, and the peer here
// is given the first and not the second, so its ceiling is its own set.
//
// The refusals name EPERM. A peer holding no delegation at all is refused with EACCES, so the number
// is what separates the ceiling from a delegation that never landed.
void refuseEscalation(fsuser::tests::Report& report, fsuser::tests::Mount& owner,
                      fsuser::tests::Mount& peer, const std::string& name)
{
    auto file = fsuser::tests::placeFile(owner, name);

    report.section("a peer holding GRANT but not ADMIN");
    report.checkAccepted("the owner lets everyone hold GRANT|READ|WRITE", [&]
                         {
                             file.setDefaultPermission(fsuser::Permission::Grant | fsuser::Permission::Read | fsuser::Permission::Write);
                         });

    auto delegated = peer.open(name, O_RDWR);
    report.checkRefusedWith("the peer cannot hand on ADMIN", std::errc::operation_not_permitted,
                            [&]
                            {
                                delegated.grantPermission(OnwardUid, fsuser::AnyId,
                                                          fsuser::Permission::Admin);
                            });
    report.checkRefusedWith("the peer cannot hand on GRANT", std::errc::operation_not_permitted,
                            [&]
                            {
                                delegated.grantPermission(OnwardUid, fsuser::AnyId,
                                                          fsuser::Permission::Grant);
                            });
    report.checkAccepted("the peer can hand on READ, which it holds", [&]
                         {
                             delegated.grantPermission(OnwardUid, fsuser::AnyId, fsuser::Permission::Read);
                         });

    // A grant for an account that already holds a row on this node rewrites that row. GRANT hands
    // on and takes nothing back, so a rewrite that drops a bit the row holds is ADMIN's alone.
    report.checkAccepted("the peer widens that account's row to READ|WRITE, both of which it holds", [&]
                         {
                             delegated.grantPermission(OnwardUid, fsuser::AnyId,
                                                       fsuser::Permission::Read | fsuser::Permission::Write);
                         });
    report.checkRefusedWith("the peer cannot rewrite it back to READ alone",
                            std::errc::operation_not_permitted,
                            [&]
                            {
                                delegated.grantPermission(OnwardUid, fsuser::AnyId, fsuser::Permission::Read);
                            });
    report.checkAccepted("the peer can rewrite it with the same bits and takes none", [&]
                         {
                             delegated.grantPermission(OnwardUid, fsuser::AnyId,
                                                       fsuser::Permission::Read | fsuser::Permission::Write);
                         });

    // The ceiling is the holder's own set, bit by bit: DELETE is outside GRANT|READ|WRITE, and a
    // row carrying it would let the peer widen its own reach through an account it names.
    report.checkRefusedWith("the peer cannot hand on DELETE, which it does not hold",
                            std::errc::operation_not_permitted,
                            [&]
                            {
                                delegated.grantPermission(OnwardUid, fsuser::AnyId,
                                                          fsuser::Permission::Read | fsuser::Permission::Delete);
                            });

    owner.unlink(name);
}

// The default covers everyone holding no delegation, so this runs on a file that was never granted
// anything and moves only that one value. It is read here through ADMIN and DELETE, the two rungs
// the default can carry that no mapping reaches.
void checkDefaultMove(fsuser::tests::Report& report, fsuser::tests::Mount& owner,
                      fsuser::tests::Mount& peer, const std::string& name)
{
    auto file = fsuser::tests::placeFile(owner, name);

    report.section("the default, with no delegation anywhere");
    const auto closed = askForAdmin(peer, name);
    report.checkErrno("the peer holds no ADMIN", closed == EACCES, closed);

    report.checkAccepted("setting the default to ADMIN succeeds",
                         [&]
                         {
                             file.setDefaultPermission(fsuser::Permission::Admin);
                         });
    const auto opened = askForAdmin(peer, name);
    report.checkErrno("the peer holds ADMIN through the default", opened == ENOENT, opened);

    report.checkAccepted("clearing the default succeeds",
                         [&]
                         {
                             file.setDefaultPermission(fsuser::Permission::None);
                         });
    const auto shut = askForAdmin(peer, name);
    report.checkErrno("the peer holds no ADMIN again", shut == EACCES, shut);

    report.section("a default carrying DELETE");
    report.checkAccepted("adding DELETE to the default succeeds",
                         [&]
                         {
                             file.setDefaultPermission(fsuser::Permission::Delete);
                         });
    report.checkAccepted("the peer can unlink", [&]
                         {
                             peer.unlink(name);
                         });
}

}  // namespace

int main(int argc, char** argv)
{
    const auto body = [](fsuser::tests::Report& report, fsuser::tests::Mounts_t& mounts)
    {
        auto owner = fsuser::tests::Mount(mounts.first());
        auto peer = fsuser::tests::Mount(mounts.second());

        checkGrantLadder(report, owner, peer, caseName("perm_peer"));
        checkTwoIdRow(report, owner, peer, caseName("perm_pair"));
        refuseEscalation(report, owner, peer, caseName("perm_escalate"));
        checkDefaultMove(report, owner, peer, caseName("perm_default"));
    };
    return fsuser::tests::runCase(argc, argv, "test_perm_peer", 2, body);
}
