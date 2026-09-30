// SPDX-License-Identifier: Apache-2.0
//
// test_mmap_cuda -- whether a GPU can be pointed at a region a mapping of this filesystem hands out.
//
// cudaHostRegister pins host pages and builds a device-side mapping for them. That needs real
// struct pages behind the address, which this module has only on the DEV_DAX path where it delegates mmap
// to the device_dax driver. So what this case measures is that the delegation survives: an address
// from File::map has to be registrable, and an address from a fallback path would not be.
//
// The runtime arrives through dlopen rather than a link, so the suite builds on a host with no CUDA
// toolkit and this case reports a skip there instead of failing to compile.
//
// What permissions a mapping needs is not the subject. The peer arms of that question are
// test_perm_peer's and test_vm_protect_peer's, and only the protection of the owner's own mapping is
// varied here, because that is what cudaHostRegister reads.
//
// cudaHostRegisterReadOnly is recorded rather than claimed. Whether a device accepts that flag is a
// device attribute the runtime publishes on its own, so an outcome either way says nothing about the
// mapping it was handed.
//
//   test_mmap_cuda <mount-a>

#include <dlfcn.h>
#include <sys/mman.h>

#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>

#include "fs/file.hpp"
#include "harness/harness.hpp"
#include "harness/mount.hpp"

namespace
{

// From driver_types.h, so the case names them rather than pulling the toolkit's headers in.
constexpr std::uint32_t RegisterDefault = 0x00;
constexpr std::uint32_t RegisterReadOnly = 0x08;
constexpr std::int32_t CudaSuccess = 0;

using fsuser::tests::caseName;
using fsuser::tests::PlacementSize;

// The entry points this case needs, resolved together: a runtime missing any of them is a runtime
// this case cannot use, which is the same situation as no runtime at all.
class CudaRuntime
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────
    CudaRuntime() noexcept
    {
        for (const char* candidate : {"libcudart.so", "libcudart.so.13", "libcudart.so.12"})
        {
            loaded_ = ::dlopen(candidate, RTLD_LAZY);
            if (loaded_ != nullptr)
            {
                break;
            }
        }
        if (loaded_ == nullptr)
        {
            return;
        }
        registerPages_ = resolve<RegisterFn>("cudaHostRegister");
        releasePages_ = resolve<ReleaseFn>("cudaHostUnregister");
        countDevices_ = resolve<CountFn>("cudaGetDeviceCount");
        describe_ = resolve<DescribeFn>("cudaGetErrorString");
    }
    CudaRuntime(const CudaRuntime&) = delete;
    ~CudaRuntime()
    {
        if (loaded_ != nullptr)
        {
            ::dlclose(loaded_);
        }
    }

    // ── operator= ──────────────────────────────────────────────────
    CudaRuntime& operator=(const CudaRuntime&) = delete;

    // ── public methods ─────────────────────────────────────────────
    [[nodiscard]] std::int32_t registerPages(void* base, std::uint64_t bytes,
                                             std::uint32_t flags) const
    {
        return registerPages_(base, bytes, flags);
    }
    void releasePages(void* base) const
    {
        static_cast<void>(releasePages_(base));
    }
    [[nodiscard]] std::string describe(std::int32_t code) const
    {
        return describe_ != nullptr ? std::string{describe_(code)} : std::to_string(code);
    }

    // ── accessors ──────────────────────────────────────────────────
    // A runtime with no device answers every call with an error, so the device count is part of
    // whether this case has anything to measure.
    [[nodiscard]] bool isUsable() const
    {
        if (registerPages_ == nullptr || releasePages_ == nullptr || countDevices_ == nullptr)
        {
            return false;
        }
        std::int32_t found = 0;
        return countDevices_(&found) == CudaSuccess && found > 0;
    }

private:
    using RegisterFn = std::int32_t (*)(void*, std::uint64_t, std::uint32_t);
    using ReleaseFn = std::int32_t (*)(void*);
    using CountFn = std::int32_t (*)(std::int32_t*);
    using DescribeFn = const char* (*)(std::int32_t);

    template <typename T_Entry>
    [[nodiscard]] T_Entry resolve(const char* named) const noexcept
    {
        return reinterpret_cast<T_Entry>(::dlsym(loaded_, named));
    }

private:
    void* loaded_{nullptr};
    RegisterFn registerPages_{nullptr};
    ReleaseFn releasePages_{nullptr};
    CountFn countDevices_{nullptr};
    DescribeFn describe_{nullptr};
};

// Releases whatever it pinned, so a caller has only the code to read.
[[nodiscard]] std::int32_t tryRegister(const CudaRuntime& cuda, void* base, std::uint32_t flags)
{
    const std::int32_t answered = cuda.registerPages(base, PlacementSize, flags);
    if (answered == CudaSuccess)
    {
        cuda.releasePages(base);
    }
    return answered;
}

[[nodiscard]] std::string outcomeOf(const CudaRuntime& cuda, std::int32_t answered)
{
    return answered == CudaSuccess ? std::string{"accepted"} : cuda.describe(answered);
}

void checkGpuReachesTheRegion(fsuser::tests::Report& report, const CudaRuntime& cuda,
                              fsuser::tests::Mount& mount, const std::string& name)
{
    auto file = fsuser::tests::placeFile(mount, name);

    report.section("a writable mapping");
    {
        auto writable = file.map(PlacementSize, PROT_READ | PROT_WRITE);
        report.check("the placed file maps writably", writable.isMapped());

        const std::int32_t plain = tryRegister(cuda, writable.get(), RegisterDefault);
        report.check("cudaHostRegister takes it: " + outcomeOf(cuda, plain),
                     plain == CudaSuccess);
        report.note("cudaHostRegisterReadOnly on it: " +
                    outcomeOf(cuda, tryRegister(cuda, writable.get(), RegisterReadOnly)));
    }

    report.section("a read-only mapping");
    {
        auto readable = file.map(PlacementSize, PROT_READ);
        report.check("the placed file maps for reading", readable.isMapped());

        const std::int32_t plain = tryRegister(cuda, readable.get(), RegisterDefault);
        report.check("cudaHostRegister refuses it, having no page it may write to: " +
                         outcomeOf(cuda, plain),
                     plain != CudaSuccess);
        report.note("cudaHostRegisterReadOnly on it: " +
                    outcomeOf(cuda, tryRegister(cuda, readable.get(), RegisterReadOnly)));
    }
}

}  // namespace

int main(int argc, char** argv)
{
    fsuser::tests::Mounts_t mounts;
    if (!fsuser::tests::takeMounts(argc, argv, 1, mounts))
    {
        return 2;
    }

    const CudaRuntime cuda;
    if (!cuda.isUsable())
    {
        std::printf("test_mmap_cuda: skipped, no CUDA runtime with a device behind it\n");
        return fsuser::tests::SkipStatus;
    }

    fsuser::tests::Report report;
    const auto name = caseName("mmap_cuda");

    try
    {
        auto mount = fsuser::tests::Mount(mounts.first());
        checkGpuReachesTheRegion(report, cuda, mount, name);
        mount.unlink(name);
    }
    catch (const std::exception& failure)
    {
        report.raised("test_mmap_cuda setup", failure);
    }

    return report.summarise("test_mmap_cuda");
}
