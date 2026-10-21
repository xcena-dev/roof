// SPDX-License-Identifier: Apache-2.0
//
// test_loadable_abi -- the C entry points of the shared library, reached the way a program with no
// build-time dependency on it reaches them: dlopen the built object and resolve the names.
//
//   test_loadable_abi <mount-a>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <string>

#include "fs/abi.hpp"
#include "fs/file.hpp"
#include "harness/harness.hpp"
#include "harness/mount.hpp"

namespace
{

using fsuser::tests::caseName;

constexpr const char* NotRoofFile = "/dev/null";
constexpr std::uint32_t OtherAccount = 1001;
constexpr const char* RoleGroup = "travel";
constexpr const char* RoleName = "hotel";

// The table under the development form of the entry point name, or null.
[[nodiscard]] const fsuser::CApi_t* loadTable(void* library, std::uint32_t version)
{
    const auto getTable = reinterpret_cast<fsuser::CApiGetFn>(::dlsym(library, FSUSER_C_API_SYMBOL));
    return (getTable == nullptr) ? nullptr : getTable(version);
}

void checkRegionAttr(fsuser::tests::Report& report, const fsuser::CApi_t& entry, fsuser::File& file)
{
    report.section("region attributes");

    const std::int32_t descriptor = file.get();
    const fsuser::RegionAttr_t asked{fsuser::CachePolicy::Uncached, fsuser::Placement::Contiguous, 0};

    report.check("set region attr answers 0 before placement", entry.setRegionAttr(descriptor, asked) == 0);

    fsuser::RegionAttr_t taken{fsuser::CachePolicy::Writeback, fsuser::Placement::Paged, 1};
    report.check("read region attr answers 0", entry.readRegionAttr(descriptor, &taken) == 0);
    report.check("read returns what set asked for", taken.cache == asked.cache &&
                                                        taken.placement == asked.placement && taken.pieces == 0);

    file.resize(fsuser::tests::PlacementSize);
    report.check("set region attr answers -EBUSY once the file is placed",
                 entry.setRegionAttr(descriptor, asked) == -EBUSY);
}

void checkPermissions(fsuser::tests::Report& report, const fsuser::CApi_t& entry, const fsuser::File& file)
{
    report.section("permissions");

    const std::int32_t descriptor = file.get();
    const auto readAndWrite = fsuser::Permission::Read | fsuser::Permission::Write;

    report.check("set default permission answers 0", entry.setDefaultPermission(descriptor, readAndWrite) == 0);

    report.check("grant to an account answers 0",
                 entry.grantPermission(descriptor, OtherAccount, fsuser::AnyId, fsuser::Permission::Read) == 0);
    report.check("revoke of that account answers 0",
                 entry.revokePermission(descriptor, OtherAccount, fsuser::AnyId) == 0);

    report.check("grant to a role answers 0", entry.grantRole(descriptor, RoleGroup, RoleName,
                                                              fsuser::Permission::Read, fsuser::AnyNode) == 0);
    report.check("revoke of that role answers 0",
                 entry.revokeRole(descriptor, RoleGroup, RoleName, fsuser::AnyNode) == 0);

    const std::string tooLong(fsuser::MaxLabelBytes + 1, 'r');
    report.check("a role past the label limit answers -EINVAL on grant",
                 entry.grantRole(descriptor, RoleGroup, tooLong, fsuser::Permission::Read, fsuser::AnyNode) == -EINVAL);
    report.check("a group past the label limit answers -EINVAL on revoke",
                 entry.revokeRole(descriptor, tooLong, RoleName, fsuser::AnyNode) == -EINVAL);

    report.check("ask permission answers 0", entry.askPermission(descriptor, readAndWrite) == 0);
    void* const mapped = ::mmap(nullptr, fsuser::tests::PlacementSize, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
    report.checkErrno("a plain mmap follows the ask", mapped != MAP_FAILED, errno);
    if (mapped != MAP_FAILED)
    {
        ::munmap(mapped, fsuser::tests::PlacementSize);
    }

    report.check("the entry points left the descriptor open", file.isOpen() && ::fcntl(descriptor, F_GETFD) >= 0);
}

void checkOnOtherFile(fsuser::tests::Report& report, const fsuser::CApi_t& entry)
{
    report.section("a descriptor of a file that is not on the mount");

    const std::int32_t other = ::open(NotRoofFile, O_RDWR | O_CLOEXEC);
    report.check("the other file opens", other >= 0);
    if (other < 0)
    {
        return;
    }

    const auto read = fsuser::Permission::Read;
    fsuser::RegionAttr_t attr;
    report.check("set default permission answers -ENOTTY", entry.setDefaultPermission(other, read) == -ENOTTY);
    report.check("set region attr answers -ENOTTY", entry.setRegionAttr(other, attr) == -ENOTTY);
    report.check("read region attr answers -ENOTTY", entry.readRegionAttr(other, &attr) == -ENOTTY);
    report.check("grant permission answers -ENOTTY", entry.grantPermission(other, OtherAccount, fsuser::AnyId, read) == -ENOTTY);
    report.check("revoke permission answers -ENOTTY", entry.revokePermission(other, OtherAccount, fsuser::AnyId) == -ENOTTY);
    report.check("grant role answers -ENOTTY",
                 entry.grantRole(other, RoleGroup, RoleName, read, fsuser::AnyNode) == -ENOTTY);
    report.check("revoke role answers -ENOTTY", entry.revokeRole(other, RoleGroup, RoleName, fsuser::AnyNode) == -ENOTTY);
    report.check("ask permission answers -ENOTTY", entry.askPermission(other, read) == -ENOTTY);

    ::close(other);
}

}  // namespace

int main(int argc, char** argv)
{
    const auto body = [](fsuser::tests::Report& report, fsuser::tests::Mounts_t& mounts)
    {
        void* library = ::dlopen(ABI_LIBRARY_PATH, RTLD_NOW | RTLD_LOCAL);
        report.check("the shared library loads", library != nullptr);
        if (library == nullptr)
        {
            return;
        }

        report.check("it answers null for another version", loadTable(library, fsuser::AbiVersion + 1) == nullptr);

        const auto* const table = loadTable(library, fsuser::AbiVersion);
        report.check("the entry point answers for the version the case was written against", table != nullptr);
        if (table != nullptr)
        {
            report.check("the table names its size and version",
                         table->size >= sizeof(fsuser::CApi_t) && table->abiVersion == fsuser::AbiVersion);

            auto mount = fsuser::tests::Mount(mounts.first());
            const auto name = caseName("loadable_abi");
            auto file = mount.open(name, O_CREAT | O_RDWR, 0644);
            checkRegionAttr(report, *table, file);
            checkPermissions(report, *table, file);
            checkOnOtherFile(report, *table);
            mount.unlink(name);
        }

        ::dlclose(library);
    };
    return fsuser::tests::runCase(argc, argv, "test_loadable_abi", 1, body);
}
