#pragma once

#include "record.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>


// MDCAPBIN v1 is deliberately a little-endian native-layout format.
// Any incompatible layout or encoding change requires a new format version.
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
static_assert(
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
    "MDCAPBIN v1 requires a little-endian host"
);
#elif !defined(_WIN32)
#error "MDCAPBIN v1 requires a known little-endian host"
#endif


inline constexpr char kCaptureMagic[8] = {
    'M', 'D', 'C', 'A', 'P', 'B', 'I', 'N'
};

inline constexpr std::uint16_t kCaptureFormatVersion = 1;
inline constexpr std::uint16_t kCaptureHeaderSize = 64;
inline constexpr std::uint16_t kCaptureRecordSize = 56;
inline constexpr std::uint8_t kCaptureScaleExponent = 8;

inline constexpr std::uint64_t kUnfinalizedRecordCount =
    std::numeric_limits<std::uint64_t>::max();


enum class MarketType : std::uint8_t {
    invalid = 0,
    futures = 1,
    spot = 2,
};


enum class StreamType : std::uint8_t {
    invalid = 0,
    book_ticker = 1,
    depth = 2,
};


inline constexpr std::uint8_t kHeaderFlagDirtyBuild = 1u << 0;
inline constexpr std::uint8_t kKnownHeaderFlags =
    kHeaderFlagDirtyBuild;


struct BinaryHeader {
    char magic[8];                 // offset 0

    std::uint64_t record_count;    // offset 8

    std::uint16_t format_version;  // offset 16
    std::uint16_t header_size;     // offset 18
    std::uint16_t record_size;     // offset 20

    std::uint8_t scale_exponent;   // offset 22
    std::uint8_t market_type;      // offset 23
    std::uint8_t stream_type;      // offset 24
    std::uint8_t flags;            // offset 25

    char symbol[16];               // offset 26
    std::uint8_t git_commit[20];   // offset 42

    std::uint8_t reserved[2];      // offset 62
};


static_assert(
    std::is_standard_layout_v<BinaryHeader>,
    "BinaryHeader must be standard layout"
);

static_assert(
    std::is_trivially_copyable_v<BinaryHeader>,
    "BinaryHeader must be trivially copyable"
);

static_assert(
    sizeof(BinaryHeader) == kCaptureHeaderSize,
    "BinaryHeader must be exactly 64 bytes"
);

static_assert(
    alignof(BinaryHeader) == 8,
    "BinaryHeader must have 8-byte alignment"
);


static_assert(offsetof(BinaryHeader, magic) == 0);
static_assert(offsetof(BinaryHeader, record_count) == 8);
static_assert(offsetof(BinaryHeader, format_version) == 16);
static_assert(offsetof(BinaryHeader, header_size) == 18);
static_assert(offsetof(BinaryHeader, record_size) == 20);
static_assert(offsetof(BinaryHeader, scale_exponent) == 22);
static_assert(offsetof(BinaryHeader, market_type) == 23);
static_assert(offsetof(BinaryHeader, stream_type) == 24);
static_assert(offsetof(BinaryHeader, flags) == 25);
static_assert(offsetof(BinaryHeader, symbol) == 26);
static_assert(offsetof(BinaryHeader, git_commit) == 42);
static_assert(offsetof(BinaryHeader, reserved) == 62);

// ---------------------------------------------------------------------
// CaptureFile — §7.6a
// ---------------------------------------------------------------------
//
// A validator with an accessor, not an abstraction layer.
//
// The reason it exists is that something must validate the header exactly
// once, at open, and that something must not be the replay producer's hot
// loop. If the validation lives inline in the producer, the next tool
// that opens a .bin will skip it and §7.6's entire 64-byte header will
// have bought nothing.
//
// Deliberately absent, per §7.6a: no read_next(), no iterator, no virtual
// dispatch over stream types, no caching. The producer takes the span and
// indexes it directly — a span index is a multiply-add, where a
// read_next() is a call with state to maintain on a loop that runs 13.7
// million times. Depth is out of parser scope (§7.1), so StreamType::depth
// is a recognised value that gets rejected, not a case to dispatch on.
// mmap is already backed by the page cache, so a cache here would be a
// second copy of what the kernel is holding.

#include <cstdio>
#include <span>
#include <string_view>


enum class CaptureFileError {
    none,

    // File-level failures.
    cannot_open,
    cannot_stat,
    file_smaller_than_header,
    mmap_failed,

    // Header failures, checked in the order listed in open(). magic and
    // format_version come first: a field checked after the fields it
    // protects is decoration.
    bad_magic,
    unsupported_format_version,
    bad_header_size,
    record_size_mismatch,
    bad_scale_exponent,
    unknown_market_type,
    unknown_stream_type,
    unsupported_stream_type,
    unknown_flags,
    reserved_not_zero,
    symbol_not_terminated,
    symbol_mismatch,

    // record_count still holds the poison placeholder, so the writer did
    // not reach its seek-back-and-patch step. The file is incomplete
    // rather than empty, and §7.6 chose UINT64_MAX precisely so the two
    // are distinguishable.
    unfinalized_record_count,

    record_count_overflow,
    file_size_mismatch,

    // The mapped record array is not suitably aligned. Cannot happen with
    // a page-aligned mapping and a 64-byte header, but §7.6a says assert
    // it rather than rely on it.
    records_misaligned
};


const char* capture_file_error_name(CaptureFileError error) noexcept;


class CaptureFile {
public:
    CaptureFile() = default;
    ~CaptureFile();

    CaptureFile(const CaptureFile&) = delete;
    CaptureFile& operator=(const CaptureFile&) = delete;

    CaptureFile(CaptureFile&& other) noexcept;
    CaptureFile& operator=(CaptureFile&& other) noexcept;

    // Validates everything and either fills `out` or returns an error.
    // `expected_symbol` is supplied by the caller and never inferred from
    // the filename — filename inference is how a BTC dataset ends up
    // labelled ETHW (§7.6).
    static CaptureFileError open(
        const char* path,
        std::string_view expected_symbol,
        CaptureFile& out
    ) noexcept;

    bool is_open() const noexcept { return base_ != nullptr; }

    const BinaryHeader& header() const noexcept;

    std::span<const CaptureRecord> records() const noexcept;

    // §6.4a: the BTC dataset is ~734 MiB and is not in the page cache on
    // first traversal, so the producer would take demand-paging faults
    // inside the measured window. Warming lives here because it needs the
    // page size and the mapping, both of which this object owns; deriving
    // them again in the producer is how a hard-coded 4096 gets in.
    //
    // The touched value is accumulated and returned rather than
    // discarded. A loop that touches pages and throws the result away is
    // dead code at -O2 and clang will delete it.
    std::uint64_t warm() const noexcept;

    // Verified once at open and cross-checked between the two APIs.
    // Apple Silicon is 16 KiB, not 4 KiB (§7.6a).
    std::size_t page_size() const noexcept { return page_size_; }
    bool page_size_agrees() const noexcept { return page_size_agrees_; }

private:
    void reset() noexcept;

    const std::uint8_t* base_ = nullptr;
    std::size_t mapped_size_ = 0;
    std::size_t page_size_ = 0;
    bool page_size_agrees_ = false;
};


// §7.6a trap 2: the record array begins at header_size, so that offset
// must be a multiple of CaptureRecord's alignment for the mapped records
// to be suitably aligned off a page-aligned base.
static_assert(kCaptureHeaderSize % alignof(CaptureRecord) == 0);