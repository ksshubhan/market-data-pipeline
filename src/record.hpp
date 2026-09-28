// The two record types: CaptureRecord, one captured message as stored in a
// .bin, and Record, what one slot of the queue carries during a replay.
//
// The split follows who owns each field. The parser (parser.cpp) fills
// only capture facts and has nowhere to put a sequence number; the replay
// producer (replay_producer.hpp) fills the run-owned fields and copies a
// CaptureRecord in whole. The static_asserts at the bottom pin both
// layouts. See ARCHITECTURE.md.

#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>


// What the capture recorded about one message: the capture tool's receive
// time plus six fields parsed from the message.
//
// These bytes are the .bin record format. convert_capture writes each
// record's raw bytes and CaptureFile maps the file and reads them in
// place, so changing a field's type or order changes the format. The
// header's record_size check catches a size change; it does not catch two
// same-sized fields swapped.
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
    // In a replay, assigned by run_replay before each push attempt, so a
    // record the queue rejects still uses a number and the gaps the
    // consumer sees add up exactly to the drop count.
    std::uint64_t sequence;

    // When run_replay intended to send this record: the run's start time t0
    // plus the record's scheduled offset, as a reading of the monotonic
    // clock (CLOCK_UPTIME_RAW on macOS, CLOCK_MONOTONIC elsewhere), not a
    // time since the run began.
    std::uint64_t replay_intended_send_ns;

    // Embedded whole, so the producer copies one 56-byte member and the
    // field order cannot drift from CaptureRecord's.
    CaptureRecord capture;

    // Passed in by run_replay's caller and not otherwise interpreted. No
    // code maps a symbol to an id: harness_b passes 0 for whatever symbol
    // it replays, and the other programs that build Records set their own
    // values.
    std::uint16_t symbol_id;

    // Explicit tail bytes so Record has no implicit uninitialised padding.
    // The default member initialiser zeroes them even under default
    // initialisation (`Record r;`), so they can never be read as garbage
    // by a memcmp, hash, or byte-wise copy of a Record. It does not force
    // the other 74 bytes to be zeroed, so it does not bring back the cost
    // of value-initialising a fresh Record per iteration and immediately
    // overwriting most of it.
    std::uint8_t reserved[6]{};
};


// Trivially copyable: a CaptureRecord is written to and read from the .bin
// as raw bytes, and both types are copied by plain assignment through the
// queue's slots. Standard layout: offsetof below is only well-defined on
// standard-layout types. The sizes pin the layout, and because the members
// add up to exactly 56 and 80 bytes they also rule out padding anywhere in
// either type.
static_assert(std::is_trivially_copyable_v<CaptureRecord>);
static_assert(std::is_trivially_copyable_v<Record>);

static_assert(std::is_standard_layout_v<CaptureRecord>);
static_assert(std::is_standard_layout_v<Record>);

static_assert(sizeof(CaptureRecord) == 56);
static_assert(sizeof(Record) == 80);
static_assert(alignof(Record) == 8);
static_assert(offsetof(Record, capture) == 16);