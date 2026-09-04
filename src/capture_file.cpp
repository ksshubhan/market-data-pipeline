#include "capture_file.hpp"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// sysctlbyname is a BSD/Darwin interface. §5 keeps a Linux ARM64 VM for
// portability validation, so the sysctl cross-check is compiled only
// where it exists and page_size_agrees() reports false elsewhere.
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif


namespace {

// §7.6a trap 3: Apple Silicon uses 16 KiB pages. Both APIs are read and
// cross-checked rather than one being trusted, and the verified value is
// what the warming loop steps by. Stepping 4096 would be harmless but
// would do four times the work, and any future count of faults would be
// off by four.
std::size_t query_page_size(bool& agrees) noexcept
{
    const long from_sysconf = sysconf(_SC_PAGESIZE);

    std::size_t from_sysctl = 0;

#if defined(__APPLE__)
    std::size_t length = sizeof(from_sysctl);

    const bool sysctl_ok =
        sysctlbyname("hw.pagesize", &from_sysctl, &length, nullptr, 0) == 0;
#else
    const bool sysctl_ok = false;
#endif

    if (from_sysconf > 0 && sysctl_ok) {
        agrees = static_cast<std::size_t>(from_sysconf) == from_sysctl;
        return from_sysctl;
    }

    agrees = false;

    if (sysctl_ok && from_sysctl > 0) {
        return from_sysctl;
    }

    if (from_sysconf > 0) {
        return static_cast<std::size_t>(from_sysconf);
    }

    // Neither API answered. 16 KiB is this platform's value; the caller
    // can see page_size_agrees() is false.
    return 16384;
}


// Validates the 64-byte header in the order §7.6a specifies. magic and
// format_version are checked before anything else, including header_size:
// every later check is only meaningful once the file is known to be this
// format at this version.
CaptureFileError validate_header(
    const BinaryHeader& header,
    std::string_view expected_symbol,
    std::size_t file_size
) noexcept
{
    if (std::memcmp(header.magic, kCaptureMagic, sizeof(kCaptureMagic)) != 0) {
        return CaptureFileError::bad_magic;
    }

    if (header.format_version != kCaptureFormatVersion) {
        return CaptureFileError::unsupported_format_version;
    }

    if (header.header_size != kCaptureHeaderSize) {
        return CaptureFileError::bad_header_size;
    }

    if (header.record_size != sizeof(CaptureRecord)) {
        return CaptureFileError::record_size_mismatch;
    }

    if (header.scale_exponent != kCaptureScaleExponent) {
        return CaptureFileError::bad_scale_exponent;
    }

    if (header.market_type !=
            static_cast<std::uint8_t>(MarketType::futures) &&
        header.market_type !=
            static_cast<std::uint8_t>(MarketType::spot)) {
        return CaptureFileError::unknown_market_type;
    }

    if (header.stream_type !=
            static_cast<std::uint8_t>(StreamType::book_ticker) &&
        header.stream_type !=
            static_cast<std::uint8_t>(StreamType::depth)) {
        return CaptureFileError::unknown_stream_type;
    }

    // §7.1: depth is captured and kept on disk but deliberately not
    // parsed. A depth .bin is a recognised file this code refuses to
    // read, which is different from an unrecognised one.
    if (header.stream_type !=
        static_cast<std::uint8_t>(StreamType::book_ticker)) {
        return CaptureFileError::unsupported_stream_type;
    }

    if ((header.flags & ~kKnownHeaderFlags) != 0) {
        return CaptureFileError::unknown_flags;
    }

    if (header.reserved[0] != 0 || header.reserved[1] != 0) {
        return CaptureFileError::reserved_not_zero;
    }

    // The symbol must be null-terminated inside its 16 bytes, so at most
    // 15 usable characters (§7.6).
    std::size_t symbol_length = 0;

    while (symbol_length < sizeof(header.symbol) &&
           header.symbol[symbol_length] != '\0') {
        ++symbol_length;
    }

    if (symbol_length == sizeof(header.symbol)) {
        return CaptureFileError::symbol_not_terminated;
    }

    const std::string_view symbol(header.symbol, symbol_length);

    if (symbol != expected_symbol) {
        return CaptureFileError::symbol_mismatch;
    }

    // Rejected explicitly rather than falling out of the size check, so
    // an interrupted conversion reports as incomplete rather than as a
    // size mismatch (§7.6).
    if (header.record_count == kUnfinalizedRecordCount) {
        return CaptureFileError::unfinalized_record_count;
    }

    // Guard the multiply before performing it. record_count is attacker-
    // controlled in the sense that it comes off disk; the file-size check
    // below is only sound if the product cannot wrap.
    const std::uint64_t max_records =
        (std::numeric_limits<std::uint64_t>::max() - kCaptureHeaderSize) /
        sizeof(CaptureRecord);

    if (header.record_count > max_records) {
        return CaptureFileError::record_count_overflow;
    }

    const std::uint64_t expected_size =
        kCaptureHeaderSize +
        header.record_count * sizeof(CaptureRecord);

    if (expected_size != file_size) {
        return CaptureFileError::file_size_mismatch;
    }

    return CaptureFileError::none;
}

} // namespace


const char* capture_file_error_name(CaptureFileError error) noexcept
{
    switch (error) {
    case CaptureFileError::none:
        return "none";
    case CaptureFileError::cannot_open:
        return "cannot_open";
    case CaptureFileError::cannot_stat:
        return "cannot_stat";
    case CaptureFileError::file_smaller_than_header:
        return "file_smaller_than_header";
    case CaptureFileError::mmap_failed:
        return "mmap_failed";
    case CaptureFileError::bad_magic:
        return "bad_magic";
    case CaptureFileError::unsupported_format_version:
        return "unsupported_format_version";
    case CaptureFileError::bad_header_size:
        return "bad_header_size";
    case CaptureFileError::record_size_mismatch:
        return "record_size_mismatch";
    case CaptureFileError::bad_scale_exponent:
        return "bad_scale_exponent";
    case CaptureFileError::unknown_market_type:
        return "unknown_market_type";
    case CaptureFileError::unknown_stream_type:
        return "unknown_stream_type";
    case CaptureFileError::unsupported_stream_type:
        return "unsupported_stream_type";
    case CaptureFileError::unknown_flags:
        return "unknown_flags";
    case CaptureFileError::reserved_not_zero:
        return "reserved_not_zero";
    case CaptureFileError::symbol_not_terminated:
        return "symbol_not_terminated";
    case CaptureFileError::symbol_mismatch:
        return "symbol_mismatch";
    case CaptureFileError::unfinalized_record_count:
        return "unfinalized_record_count";
    case CaptureFileError::record_count_overflow:
        return "record_count_overflow";
    case CaptureFileError::file_size_mismatch:
        return "file_size_mismatch";
    case CaptureFileError::records_misaligned:
        return "records_misaligned";
    }

    return "<unrecognised CaptureFileError>";
}


CaptureFile::~CaptureFile()
{
    reset();
}


CaptureFile::CaptureFile(CaptureFile&& other) noexcept
    : base_(other.base_),
      mapped_size_(other.mapped_size_),
      page_size_(other.page_size_),
      page_size_agrees_(other.page_size_agrees_)
{
    other.base_ = nullptr;
    other.mapped_size_ = 0;
}


CaptureFile& CaptureFile::operator=(CaptureFile&& other) noexcept
{
    if (this != &other) {
        reset();

        base_ = other.base_;
        mapped_size_ = other.mapped_size_;
        page_size_ = other.page_size_;
        page_size_agrees_ = other.page_size_agrees_;

        other.base_ = nullptr;
        other.mapped_size_ = 0;
    }

    return *this;
}


void CaptureFile::reset() noexcept
{
    if (base_ != nullptr) {
        ::munmap(const_cast<std::uint8_t*>(base_), mapped_size_);
        base_ = nullptr;
        mapped_size_ = 0;
    }
}


CaptureFileError CaptureFile::open(
    const char* path,
    std::string_view expected_symbol,
    CaptureFile& out
) noexcept
{
    out.reset();

    const int fd = ::open(path, O_RDONLY);

    if (fd < 0) {
        return CaptureFileError::cannot_open;
    }

    struct stat status = {};

    if (::fstat(fd, &status) != 0) {
        ::close(fd);
        return CaptureFileError::cannot_stat;
    }

    const std::size_t file_size = static_cast<std::size_t>(status.st_size);

    if (file_size < kCaptureHeaderSize) {
        ::close(fd);
        return CaptureFileError::file_smaller_than_header;
    }

    void* mapping = ::mmap(
        nullptr,
        file_size,
        PROT_READ,
        MAP_PRIVATE,
        fd,
        0
    );

    // The mapping outlives the descriptor.
    ::close(fd);

    if (mapping == MAP_FAILED) {
        return CaptureFileError::mmap_failed;
    }

    const std::uint8_t* base = static_cast<const std::uint8_t*>(mapping);

    BinaryHeader header = {};
    std::memcpy(&header, base, sizeof(header));

    const CaptureFileError error =
        validate_header(header, expected_symbol, file_size);

    if (error != CaptureFileError::none) {
        ::munmap(mapping, file_size);
        return error;
    }

    // §7.6a trap 2: assert the alignment rather than relying on it. mmap
    // returns a page-aligned address and the header is a multiple of
    // alignof(CaptureRecord), so this holds — as a checked fact.
    if (reinterpret_cast<std::uintptr_t>(base + kCaptureHeaderSize) %
            alignof(CaptureRecord) != 0) {
        ::munmap(mapping, file_size);
        return CaptureFileError::records_misaligned;
    }

    out.base_ = base;
    out.mapped_size_ = file_size;
    out.page_size_ = query_page_size(out.page_size_agrees_);

    return CaptureFileError::none;
}


const BinaryHeader& CaptureFile::header() const noexcept
{
    // §7.6a trap 1: forming a BinaryHeader reference over mapped bytes is
    // formally undefined in C++20 because no object lifetime has begun
    // there. std::start_lifetime_as is the C++23 fix. It works on every
    // real implementation, and the correct move is to do it and name the
    // problem rather than pretend it is not there.
    return *reinterpret_cast<const BinaryHeader*>(base_);
}


std::span<const CaptureRecord> CaptureFile::records() const noexcept
{
    // Same C++20 lifetime caveat as header() above.
    const CaptureRecord* first =
        reinterpret_cast<const CaptureRecord*>(base_ + kCaptureHeaderSize);

    return std::span<const CaptureRecord>(
        first,
        static_cast<std::size_t>(header().record_count)
    );
}


std::uint64_t CaptureFile::warm() const noexcept
{
    if (base_ == nullptr) {
        return 0;
    }

    std::uint64_t sink = 0;

    for (std::size_t offset = 0;
         offset < mapped_size_;
         offset += page_size_) {
        sink += base_[offset];
    }

    // Touch the final byte too: the last page is only partly covered by
    // the stride above when the file is not a whole number of pages.
    sink += base_[mapped_size_ - 1];

    return sink;
}