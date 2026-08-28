#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>


// Fields that come directly from the captured market-data message.
struct CaptureRecord {

    // Time the capture process received the message.
    // Unix epoch wall-clock nanoseconds from time.time_ns().
    std::uint64_t capture_wall_time_ns;

    // Exchange event timestamp E, in Unix epoch milliseconds.
    std::uint64_t event_time_ms;

    // Exchange transaction timestamp T, in Unix epoch milliseconds.
    std::uint64_t transaction_time_ms;

    // Prices stored as fixed-point integers scaled by 10^8.
    std::int64_t bid_price;
    std::int64_t ask_price;

    // Quantities stored as fixed-point integers scaled by 10^8.
    std::int64_t bid_qty;
    std::int64_t ask_qty;
};


struct Record {
    // Assigned by the replay producer.
    std::uint64_t sequence;

    // Intended send time for this replay run.
    // Nanoseconds from the replay process's monotonic clock origin.
    std::uint64_t replay_intended_send_ns;

    // Facts originating from the captured market-data message.
    CaptureRecord capture;

    // Assigned by the replay producer.
    // 0 = BTCUSDT, 1 = another symbol, etc.
    std::uint16_t symbol_id;

    // Explicit tail bytes so Record has no implicit uninitialised padding.
    // These must be zeroed by the producer.
    std::uint8_t reserved[6];
};


static_assert(std::is_trivially_copyable_v<CaptureRecord>);
static_assert(std::is_trivially_copyable_v<Record>);

static_assert(std::is_standard_layout_v<CaptureRecord>);
static_assert(std::is_standard_layout_v<Record>);

static_assert(sizeof(CaptureRecord) == 56);
static_assert(sizeof(Record) == 80);
static_assert(alignof(Record) == 8);
static_assert(offsetof(Record, capture) == 16);