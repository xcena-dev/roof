// SPDX-License-Identifier: Apache-2.0
//
// probe_huge_bytes -- bytes written through a huge-entry mapping come back the same every other way:
// read(2) in pieces that cross folio ends, a second mapping, one at a page offset, and the first one
// after a page is cut out of its middle. It also prints the faults the write took, the PMD-mapped file
// memory the system counts meanwhile, and whether io_uring may pin a buffer in the mapping.
//
//   probe_huge_bytes <mount> <mib> <tag>

#include <fcntl.h>
#include <linux/io_uring.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>  // IWYU pragma: keep (the __NR_ numbers, which live in an arch header under it)
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace
{

constexpr std::uint64_t PageBytes = 4096;
constexpr std::uint64_t PmdBytes = 2ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t PudBytes = 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t WordBytes = sizeof(std::uint64_t);

[[nodiscard]] std::uint64_t computeWord(std::uint64_t wordIndex)
{
    constexpr std::uint64_t MixFactor = 0x9E3779B97F4A7C15ULL;
    constexpr std::uint32_t MixShift = 17;
    return ((wordIndex + 1) * MixFactor) ^ (wordIndex << MixShift);
}

[[nodiscard]] std::optional<std::uint64_t> parseUnsigned(std::string_view text)
{
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
    {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::uint64_t readMinorFaults()
{
    struct rusage usage = {};
    ::getrusage(RUSAGE_SELF, &usage);
    return static_cast<std::uint64_t>(usage.ru_minflt);
}

// One /proc/meminfo field in kB, or 0 when it is missing.
[[nodiscard]] std::int64_t readMeminfoKib(std::string_view field)
{
    std::ifstream meminfo{"/proc/meminfo"};
    std::string line;
    while (std::getline(meminfo, line))
    {
        if (line.rfind(field, 0) != 0 || line.size() <= field.size() || line[field.size()] != ':')
        {
            continue;
        }
        const auto digits = line.find_first_of("0123456789", field.size());
        const auto end = line.find_first_not_of("0123456789", digits);
        if (digits == std::string::npos)
        {
            return 0;
        }
        const auto value = parseUnsigned(std::string_view{line}.substr(digits, end - digits));
        return value ? static_cast<std::int64_t>(*value) : 0;
    }
    return 0;
}

// Words of [firstWord, firstWord + words) that do not hold the pattern, read through @base.
[[nodiscard]] std::uint64_t countMismatches(const volatile std::uint64_t* base, std::uint64_t firstWord,
                                            std::uint64_t words)
{
    std::uint64_t mismatched = 0;
    for (std::uint64_t word = 0; word < words; ++word)
    {
        if (base[word] != computeWord(firstWord + word))
        {
            ++mismatched;
        }
    }
    return mismatched;
}

// An odd size, so every read but the first starts inside a folio and crosses its end, which is where
// the kernel has to split its copy.
[[nodiscard]] std::uint64_t countReadMismatches(std::int32_t handle, std::uint64_t bytes)
{
    constexpr std::uint64_t ReadBytes = (3 * 1024ULL * 1024ULL) + PageBytes;
    std::vector<std::uint64_t> chunk(ReadBytes / WordBytes);
    std::uint64_t mismatched = 0;
    for (std::uint64_t offset = 0; offset < bytes; offset += ReadBytes)
    {
        const std::uint64_t want = std::min(bytes - offset, ReadBytes);
        const std::int64_t got = ::pread(handle, chunk.data(), want, static_cast<::off_t>(offset));
        if (got != static_cast<std::int64_t>(want))
        {
            std::printf("read2 short at offset %llu: %lld (%s)\n", static_cast<unsigned long long>(offset),
                        static_cast<long long>(got), std::strerror(errno));
            return bytes / WordBytes;
        }
        mismatched += countMismatches(chunk.data(), offset / WordBytes, want / WordBytes);
    }
    return mismatched;
}

// A shared mapping that unmaps whatever of it is still mapped on every way out. cutPage takes one
// page out of the middle, so a huge entry over it has to split.
class Mapped
{
public:
    Mapped(std::int32_t handle, std::uint64_t bytes, std::int32_t protection, std::uint64_t offset)
        : bytes_{bytes}
    {
        void* const mapped =
            ::mmap(nullptr, static_cast<std::size_t>(bytes), protection, MAP_SHARED, handle, static_cast<::off_t>(offset));
        if (mapped != MAP_FAILED)
        {
            base_ = static_cast<std::uint8_t*>(mapped);
        }
    }
    Mapped(const Mapped&) = delete;
    Mapped& operator=(const Mapped&) = delete;
    ~Mapped()
    {
        if (base_ == nullptr)
        {
            return;
        }
        if (!hole_)
        {
            ::munmap(base_, static_cast<std::size_t>(bytes_));
            return;
        }
        ::munmap(base_, static_cast<std::size_t>(*hole_));
        ::munmap(base_ + *hole_ + PageBytes, static_cast<std::size_t>(bytes_ - *hole_ - PageBytes));
    }

    [[nodiscard]] bool isMapped() const noexcept
    {
        return base_ != nullptr;
    }
    [[nodiscard]] volatile std::uint64_t* getWords() const noexcept
    {
        return reinterpret_cast<volatile std::uint64_t*>(base_);
    }

    [[nodiscard]] bool cutPage(std::uint64_t offset)
    {
        if (hole_ || ::munmap(base_ + offset, static_cast<std::size_t>(PageBytes)) != 0)
        {
            return false;
        }
        hole_ = offset;
        return true;
    }

    void print(const char* label) const
    {
        const auto address = reinterpret_cast<std::uintptr_t>(base_);
        std::printf("%s addr=%p mod2m=%llu mod1g=%llu\n", label, static_cast<void*>(base_),
                    static_cast<unsigned long long>(address % PmdBytes),
                    static_cast<unsigned long long>(address % PudBytes));
    }

private:
    std::uint8_t* base_{nullptr};
    std::uint64_t bytes_{0};
    std::optional<std::uint64_t> hole_;
};

// Whether io_uring may register a fixed buffer over @address, which takes a FOLL_LONGTERM pin.
void reportLongtermPin(volatile std::uint64_t* address)
{
    struct io_uring_params params = {};  // NOLINT(misc-const-correctness): the kernel fills it in
    const auto ring = static_cast<std::int32_t>(::syscall(__NR_io_uring_setup, 1, &params));
    if (ring < 0)
    {
        std::printf("longterm_pin io_uring_setup=-1 errno=%s\n", std::strerror(errno));
        return;
    }
    // The buffer io_uring pins is one huge entry's worth, the smallest one this mapping has.
    struct iovec buffer = {const_cast<std::uint64_t*>(address), static_cast<std::size_t>(PmdBytes)};
    const std::int64_t registered = ::syscall(__NR_io_uring_register, ring, IORING_REGISTER_BUFFERS, &buffer, 1);
    const std::int32_t savedErrno = errno;
    std::printf("longterm_pin io_uring_register=%lld errno=%s\n", static_cast<long long>(registered),
                registered == 0 ? "0" : std::strerror(savedErrno));
    if (registered == 0)
    {
        ::syscall(__NR_io_uring_register, ring, IORING_UNREGISTER_BUFFERS, nullptr, 0);
    }
    ::close(ring);
}

// Every check against one placed file. Returns how many words came back wrong, or nullopt when a
// mapping could not be made.
[[nodiscard]] std::optional<std::uint64_t> checkFile(std::int32_t handle, std::uint64_t bytes)
{
    const std::int64_t pmdBefore = readMeminfoKib("FilePmdMapped");
    const std::int64_t mappedBefore = readMeminfoKib("Mapped");
    Mapped writer{handle, bytes, PROT_READ | PROT_WRITE, 0};
    if (!writer.isMapped())
    {
        std::perror("mmap writer");
        return std::nullopt;
    }
    writer.print("map_a");

    const std::uint64_t words = bytes / WordBytes;
    volatile std::uint64_t* const writeWords = writer.getWords();
    const std::uint64_t faultsBefore = readMinorFaults();
    for (std::uint64_t word = 0; word < words; ++word)
    {
        writeWords[word] = computeWord(word);
    }
    std::printf("write minflt=%llu file_pmd_kib_delta=%lld mapped_kib_delta=%lld\n",
                static_cast<unsigned long long>(readMinorFaults() - faultsBefore),
                static_cast<long long>(readMeminfoKib("FilePmdMapped") - pmdBefore),
                static_cast<long long>(readMeminfoKib("Mapped") - mappedBefore));

    const std::uint64_t readMismatches = countReadMismatches(handle, bytes);
    std::printf("read2 mismatched_words=%llu\n", static_cast<unsigned long long>(readMismatches));

    const Mapped second{handle, bytes, PROT_READ, 0};
    const Mapped shifted{handle, bytes - PageBytes, PROT_READ, PageBytes};
    if (!second.isMapped() || !shifted.isMapped())
    {
        std::perror("mmap reader");
        return std::nullopt;
    }
    second.print("map_b");
    shifted.print("map_c");
    const std::uint64_t readerFaultsBefore = readMinorFaults();
    const std::uint64_t secondMismatches = countMismatches(second.getWords(), 0, words);
    std::printf("map_b minflt=%llu mismatched_words=%llu\n",
                static_cast<unsigned long long>(readMinorFaults() - readerFaultsBefore),
                static_cast<unsigned long long>(secondMismatches));
    constexpr std::uint64_t PageWords = PageBytes / WordBytes;
    const std::uint64_t shiftedMismatches = countMismatches(shifted.getWords(), PageWords, words - PageWords);
    std::printf("map_c offset=%llu mismatched_words=%llu\n", static_cast<unsigned long long>(PageBytes),
                static_cast<unsigned long long>(shiftedMismatches));

    reportLongtermPin(writeWords);

    // Past the middle by a page, so the hole lies inside a huge entry rather than on its edge.
    const std::uint64_t holeOffset = (bytes / 2) + PageBytes;
    if (!writer.cutPage(holeOffset))
    {
        std::perror("munmap hole");
        return std::nullopt;
    }
    const std::uint64_t splitFaultsBefore = readMinorFaults();
    const std::uint64_t holeWord = holeOffset / WordBytes;
    const std::uint64_t afterHole = holeWord + PageWords;
    const std::uint64_t splitMismatches = countMismatches(writeWords, 0, holeWord) +
                                          countMismatches(writeWords + afterHole, afterHole, words - afterHole);
    std::printf("split hole_offset=%llu minflt=%llu mismatched_words=%llu\n",
                static_cast<unsigned long long>(holeOffset),
                static_cast<unsigned long long>(readMinorFaults() - splitFaultsBefore),
                static_cast<unsigned long long>(splitMismatches));

    return readMismatches + secondMismatches + shiftedMismatches + splitMismatches;
}

}  // namespace

int main(int argc, char** argv)
{
    constexpr std::int32_t UsageStatus = 2;
    if (argc < 4)
    {
        std::fprintf(stderr, "usage: %s <mount> <mib> <tag>\n", argv[0]);
        return UsageStatus;
    }
    // Line by line, so what was printed before a fault kills the process is still in the log.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    constexpr std::uint64_t MiBBytes = 1024ULL * 1024ULL;
    const auto mib = parseUnsigned(argv[2]);
    if (!mib || *mib * MiBBytes < 2 * PmdBytes)
    {
        std::fprintf(stderr, "mib must be a whole number of at least 4\n");
        return UsageStatus;
    }
    const std::uint64_t bytes = *mib * MiBBytes;
    const std::string path = std::string{argv[1]}.append("/").append(argv[3]).append(".bytes");

    constexpr ::mode_t CreateMode = 0644;
    const std::int32_t handle = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, CreateMode);
    if (handle < 0)
    {
        std::perror("open");
        return EXIT_FAILURE;
    }
    std::optional<std::uint64_t> mismatches;
    if (::ftruncate(handle, static_cast<::off_t>(bytes)) == 0)
    {
        mismatches = checkFile(handle, bytes);
    }
    else
    {
        std::perror("ftruncate");
    }
    ::close(handle);
    ::unlink(path.c_str());

    const bool passed = mismatches && *mismatches == 0;
    std::printf("result=%s\n", passed ? "PASS" : "FAIL");
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
