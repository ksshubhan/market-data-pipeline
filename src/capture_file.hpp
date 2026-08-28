#pragma once

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