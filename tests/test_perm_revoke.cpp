// SPDX-License-Identifier: Apache-2.0
//
// test_perm_revoke -- who may take an account delegation back off a region.
//
// One rule, in two halves. A row whose uid is the caller's own is given up and asks nobody. Any
// other row, one naming a group among them, needs fsuser::Permission::Admin. A row is named by
// three things together: the node that wrote it, and the uid and gid it carries.
//
// A revoke also takes off the rows the helper wrote for that account's processes, because those
// are the same grant in the shape an upcall writes it.
//
// Two mounts of one device, so this one process reaches the same region as two nodes. That is what
// lets the owner act twice: it created the region, so acl_is_owner keeps matching its pid, its start
// time and its executable for as long as the process lives. A helper that exits between two calls is
// never the owner on the second one.
//
//   test_perm_revoke <mount-a> <mount-b>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <system_error>

#include "fs/file.hpp"
#include "harness/harness.hpp"
#include "harness/mount.hpp"

namespace
{

// A row naming somebody this run is not. Nobody holds this uid, and nobody needs to: what is asked
// of such a row is who may take it off, not who may use it.
constexpr std::uint32_t OtherUid = 62100;

using fsuser::tests::caseName;
using fsuser::tests::PlacementSize;

// The account this process is, which is the account a row has to name for the self case to answer.
[[nodiscard]] std::uint32_t ownAccount()
{
    return static_cast<std::uint32_t>(::getuid());
}

[[nodiscard]] std::uint32_t ownGroup()
{
    return static_cast<std::uint32_t>(::getgid());
}

// The row the peer writes, which is the whole of what one setup varies from the next.
struct PeerRow_t
{
    std::uint32_t uid;
    std::uint32_t gid;
    fsuser::Permission perms;
};

// A row is only read on the node that wrote it, so the peer writes its own. GRANT is what lets it,
// and a grant hands on only what its holder has, so the door carries the row's bits too.
// The default ends at 0, so from here a row is the caller's whole authority.
[[nodiscard]] fsuser::File openWithPeerRow(fsuser::tests::Mount& owner, fsuser::tests::Mount& peer,
                                           const std::string& name, const PeerRow_t& row)
{
    auto file = fsuser::tests::placeFile(owner, name);

    fsuser::tests::runThroughOpenDoor(
        file, fsuser::Permission::Grant | row.perms,
        [&]
        {
            auto seen = peer.open(name, O_RDWR);
            seen.grantPermission(row.uid, row.gid, row.perms);
        });

    return file;
}

// The self case. The caller holds READ through the row and nothing else anywhere, and READ is not
// ADMIN, so a success here is the row naming the caller and not a permission.
void checkOwnRowRevoke(fsuser::tests::Report& report, fsuser::tests::Mount& owner,
                       fsuser::tests::Mount& peer, const std::string& name)
{
    auto file = openWithPeerRow(owner, peer, name, {ownAccount(), fsuser::AnyId, fsuser::Permission::Read});
    auto seen = peer.open(name, O_RDWR);

    report.section("a row that names the caller");
    report.checkAccepted("a caller holding only READ gives up the row that names it",
                         [&]
                         {
                             seen.revokePermission(ownAccount(), fsuser::AnyId);
                         });

    // ENOENT and not a second success: a caller has to be able to tell a row it removed from a row
    // that was never there.
    report.checkRefusedWith("the same row a second time answers ENOENT",
                            std::errc::no_such_file_or_directory,
                            [&]
                            {
                                seen.revokePermission(ownAccount(), fsuser::AnyId);
                            });

    // Both ids left open would name everyone, which is not a row. Refused on the way in, before any
    // permission is read.
    report.checkRefusedWith("naming neither id is refused", std::errc::invalid_argument,
                            [&]
                            {
                                seen.revokePermission(fsuser::AnyId, fsuser::AnyId);
                            });

    owner.unlink(name);
}

// A gid names everyone carrying it, so taking that row off takes from all of them. A member doing
// that without ADMIN reaches past its own grant, which is why the answer here is a refusal.
void refuseGroupRowWithoutAdmin(fsuser::tests::Report& report, fsuser::tests::Mount& owner,
                                fsuser::tests::Mount& peer, const std::string& name)
{
    auto file = openWithPeerRow(owner, peer, name, {fsuser::AnyId, ownGroup(), fsuser::Permission::Read});
    auto seen = peer.open(name, O_RDWR);

    report.section("a row that names only the caller's group");
    report.checkRefusedWith("a member of that group with no ADMIN cannot take it off",
                            std::errc::permission_denied,
                            [&]
                            {
                                seen.revokePermission(fsuser::AnyId, ownGroup());
                            });

    // The door stands open only long enough for the peer to write itself the row that answers, so
    // what lets the next attempt through is ADMIN and not the default.
    file.setDefaultPermission(fsuser::Permission::Admin | fsuser::Permission::Grant);
    seen.grantPermission(ownAccount(), fsuser::AnyId, fsuser::Permission::Admin);
    file.setDefaultPermission(fsuser::Permission::None);

    report.checkAccepted("the same member holding ADMIN takes it off",
                         [&]
                         {
                             seen.revokePermission(fsuser::AnyId, ownGroup());
                         });

    // ENOENT and not a second success, which is also what says the refusal above removed nothing.
    report.checkRefusedWith("the group row is gone and was there until ADMIN arrived",
                            std::errc::no_such_file_or_directory,
                            [&]
                            {
                                seen.revokePermission(fsuser::AnyId, ownGroup());
                            });

    owner.unlink(name);
}

// EACCES and not ENOENT: the row is there and reachable, and what is missing is ADMIN.
// The caller is also the one that wrote the row, and writing a row is not what lets one take it off.
void refuseOtherAccountRow(fsuser::tests::Report& report, fsuser::tests::Mount& owner,
                           fsuser::tests::Mount& peer, const std::string& name)
{
    auto file = openWithPeerRow(owner, peer, name, {OtherUid, fsuser::AnyId, fsuser::Permission::Read});
    auto seen = peer.open(name, O_RDWR);

    report.section("a row that names somebody else, and a caller holding nothing");
    report.checkRefusedWith("a caller with no ADMIN cannot take it off",
                            std::errc::permission_denied,
                            [&]
                            {
                                seen.revokePermission(OtherUid, fsuser::AnyId);
                            });

    report.note("that caller is the one that wrote the row, and writing it grants nothing back");

    owner.unlink(name);
}

// ADMIN reaching a row it does not name, twice: held by being the owner, and held through a row.
// It arrives as a row and not the default, which every node reads and would hand ADMIN to everyone.
void checkAdminRevokesOtherRow(fsuser::tests::Report& report, fsuser::tests::Mount& owner,
                               fsuser::tests::Mount& peer, const std::string& name)
{
    auto file = fsuser::tests::placeFile(owner, name);

    report.section("ADMIN, on a row of this node");
    file.grantPermission(OtherUid, fsuser::AnyId, fsuser::Permission::Read);
    report.checkAccepted("the owner takes off a row naming somebody else",
                         [&]
                         {
                             file.revokePermission(OtherUid, fsuser::AnyId);
                         });

    // The peer writes itself an ADMIN row while the door is open, and the door shuts before the
    // claim, so what answers below is that row.
    file.setDefaultPermission(fsuser::Permission::Admin | fsuser::Permission::Grant);
    auto seen = peer.open(name, O_RDWR);
    report.checkAccepted("the peer writes itself an ADMIN row",
                         [&]
                         {
                             seen.grantPermission(ownAccount(), fsuser::AnyId, fsuser::Permission::Admin);
                         });
    seen.grantPermission(OtherUid, fsuser::AnyId, fsuser::Permission::Read);
    file.setDefaultPermission(fsuser::Permission::None);

    report.checkAccepted("a caller holding ADMIN through a row of its own takes off another's",
                         [&]
                         {
                             seen.revokePermission(OtherUid, fsuser::AnyId);
                         });

    owner.unlink(name);
}

// The node is part of what names a row, and it is the part a caller cannot argue with. The peer
// holds ADMIN here, and the row it reaches for is one the other node wrote, so the answer is ENOENT:
// there is no such row on this node to take off.
void refuseCrossNodeRow(fsuser::tests::Report& report, fsuser::tests::Mount& owner,
                        fsuser::tests::Mount& peer, const std::string& name)
{
    auto file = fsuser::tests::placeFile(owner, name);

    // Written through mount A, so the row carries node A.
    file.grantPermission(OtherUid, fsuser::AnyId, fsuser::Permission::Read);

    auto seen = peer.open(name, O_RDWR);

    report.section("a row the other node wrote");
    fsuser::tests::runThroughOpenDoor(
        file, fsuser::Permission::Admin,
        [&]
        {
            report.checkRefusedWith(
                "ADMIN on this node does not reach it",
                std::errc::no_such_file_or_directory,
                [&]
                {
                    seen.revokePermission(OtherUid, fsuser::AnyId);
                });
        });

    // And it is still there, which is what says the refusal above removed nothing.
    report.checkAccepted("the node that wrote it still can",
                         [&]
                         {
                             file.revokePermission(OtherUid, fsuser::AnyId);
                         });

    owner.unlink(name);
}

// What a revoke does not do. It takes the row off. It does not take down a mapping that already
// stands, because tearing one down and rechecking on every fault are a pair, and only fencing does
// both.
//
// What it also does not do is refuse the next mapping. The row was one answer already given, and
// the helper is asked again once it is gone.
void checkStandingMappingStays(fsuser::tests::Report& report, fsuser::tests::Mount& owner,
                               fsuser::tests::Mount& peer, const std::string& name)
{
    auto file = openWithPeerRow(owner, peer, name, {ownAccount(), fsuser::AnyId, fsuser::Permission::Read});
    auto seen = peer.open(name, O_RDWR);

    report.section("a mapping that already stands");
    auto standing = seen.map(PlacementSize, PROT_READ);
    report.check("the row admits a mapping", standing.isMapped());

    // Touched before the revoke, so the page table entry exists and a later read of the same page
    // does not go back through the kernel. That is the whole claim.
    const auto* cell = static_cast<const volatile std::uint8_t*>(standing.get());
    static_cast<void>(*cell);

    report.checkAccepted("the caller gives up its own row",
                         [&]
                         {
                             seen.revokePermission(ownAccount(), fsuser::AnyId);
                         });

    static_cast<void>(*cell);
    report.check("the standing mapping still reads", standing.isMapped());

    // ENOENT is the row being gone, which is the whole of what a revoke changed on the record.
    report.checkRefusedWith("the row it took off is gone", std::errc::no_such_file_or_directory,
                            [&]
                            {
                                seen.revokePermission(ownAccount(), fsuser::AnyId);
                            });

    owner.unlink(name);
}

}  // namespace

int main(int argc, char** argv)
{
    const auto body = [](fsuser::tests::Report& report, fsuser::tests::Mounts_t& mounts)
    {
        auto owner = fsuser::tests::Mount(mounts.first());
        auto peer = fsuser::tests::Mount(mounts.second());

        checkOwnRowRevoke(report, owner, peer, caseName("revoke_own"));
        refuseGroupRowWithoutAdmin(report, owner, peer, caseName("revoke_group"));
        refuseOtherAccountRow(report, owner, peer, caseName("revoke_other"));
        checkAdminRevokesOtherRow(report, owner, peer, caseName("revoke_admin"));
        refuseCrossNodeRow(report, owner, peer, caseName("revoke_crossnode"));
        checkStandingMappingStays(report, owner, peer, caseName("revoke_mapped"));
    };
    return fsuser::tests::runCase(argc, argv, "test_perm_revoke", 2, body);
}
