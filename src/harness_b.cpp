// harness_b — the B1 load sweep (§6.4, §6.5).
//
// Replays a fixed slice of the capture at a series of fixed offered
// rates, through each queue arm in turn, and records the end-to-end
// latency distribution at each point. The output is the latency-vs-load
// curve §10 calls one of the two graphs that carry the repo, and the X
// and Y in the CV line come from it.
//
// What makes this a measurement rather than a throughput test is that the
// send schedule is computed before the clock starts and latency is
// measured from the *intended* send time (§6.4). A harness that pushed as
// fast as the consumer could drain would stall the producer whenever the
// system stalled, so the stall would never appear in any latency — that
// is coordinated omission, and it is the single most common way this
// benchmark is got wrong.
//
// Two arms, one interface. SpscRingBuffer and MutexQueue both expose
// try_push/try_pop with identical reject-newest semantics (§4), so the
// producer loop is the same code for both and the comparison is between
// implementations rather than between behaviours.
//
// ---------------------------------------------------------------------
// Two decisions taken here that depart from the plan as written
// ---------------------------------------------------------------------
//
// 1. Sample is 16 bytes, not the 24 §6.4b specifies, because queue depth
//    is not recorded at run time.
//
//    §6.4b calls depth "free — tail - head is already in registers at
//    dequeue". That is true only inside try_pop; from outside, reading
//    the producer's index is a cross-core load the consumer would
//    otherwise not make, once per message. It is not free, and it is
//    least free on the arm whose pop is a dozen instructions, so it would
//    tax the two arms unequally in the same way §replay_producer.hpp
//    refuses virtual dispatch for.
//
//    It is also unnecessary. Depth at any instant is (messages sent by t)
//    minus (messages dequeued by t), and both series are already
//    recoverable: the send schedule is a pure function of index and every
//    dequeue timestamp is recorded. The depth-vs-time graph §6.4b wants
//    is reconstructible offline, exactly, with no hot-path cost.
//
//    §6.4b needs amending to match. Recorded here rather than left as a
//    silent divergence.
//
// 2. The consumer updates a book rather than only timestamping.
//
//    try_pop copies all 80 bytes out of the slot regardless, so the
//    memory traffic is nearly the same either way. The reason to touch
//    the payload is that SpscRingBuffer::try_pop is a header template
//    that inlines completely, so a consumer that never reads the copied
//    record leaves the copy open to dead-store elimination at -O2 —
//    while MutexQueue::try_pop takes a lock, which constrains what the
//    optimiser can prove. The elision would therefore favour the fast arm
//    and would be invisible in the output.
//
//    Updating a top-of-book and accumulating a checksum through a
//    volatile sink makes the payload provably live in both arms. The cost
//    is a few ns per message, added identically to both, which compresses
//    the measured ratio slightly — the reason to keep the work small
//    rather than to skip it.
//
//    --consumer=timestamp exists to settle this empirically rather than
//    by argument: if the timestamp-only arm is dramatically faster, the
//    copy was being elided. It is a diagnostic, not a reported
//    configuration.

#include "capture_file.hpp"
#include "measurement_thread.hpp"
#include "mutex_queue.hpp"
#include "record.hpp"
#include "replay_producer.hpp"
#include "replay_schedule.hpp"
#include "spsc_ring_buffer.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>


namespace {

// ---------------------------------------------------------------------
// Fixed parameters, all decided before the first run so none of them can
// be chosen after seeing which values flatter a result (§6.4's rule for
// the lag threshold, applied to everything here).
// ---------------------------------------------------------------------

// §6.4b: 2,000,000 puts roughly 200 samples behind p99.99 and the sample
// buffer at 32 MB, small enough not to compete with the 734 MiB dataset
// for memory on a 16 GB machine.
constexpr std::size_t kSliceLength = 2'000'000;

// Capacity, decided 4 Sep before any B run.
//
// The criterion: the smallest power of two such that dropped_records is
// zero at every offered rate below the knee and the maximum reconstructed
// depth stays under half of capacity. Smallest, not largest, and the
// reason is not memory. A larger ring means the producer stores into
// slots that have fallen out of cache and the consumer reads cold ones;
// that cost is additive and near-identical in absolute ns for both arms,
// so it is a large fraction of a dozen-instruction push and a small
// fraction of one that takes a lock. Oversizing quietly narrows the gap
// that is the result. A4b saw exactly this, the cached-index advantage
// falling from 1.400 at 5 MB to 1.359 at 80 MB.
//
// 16384 slots is 1.31 MB of Record. At 1M/s it absorbs a 16 ms consumer
// stall before rejecting, against the ~9 us context switches §6.3 found
// and the ~1.3 us park/wake measured for the condvar. Three orders of
// margin.
//
// If the pilot shows depth above 8192 anywhere below the knee, the answer
// is 32768 and a re-run, not a softened criterion.
constexpr std::size_t kCapacity = 16384;

// Half-decade log spacing. All are far below measure_pacing_floor's ~50M
// records/s producer ceiling, so the producer is never the limiting
// factor anywhere in the sweep. Log spacing because the expected shape is
// flat then hockey-stick, and equal resolution per decade is what finds
// the knee. Points to resolve the knee get added after the pilot shows
// where it is, not guessed now.
constexpr double kRates[] = {
    100'000.0,
    250'000.0,
    500'000.0,
    1'000'000.0,
    2'500'000.0,
    5'000'000.0,
    10'000'000.0,
    20'000'000.0
};

constexpr std::size_t kRateCount = sizeof(kRates) / sizeof(kRates[0]);


enum class Arm {
    Spsc,
    Mutex
};


const char* arm_name(Arm arm) noexcept
{
    switch (arm) {
    case Arm::Spsc:
        return "spsc";
    case Arm::Mutex:
        return "mutex";
    }

    std::abort();
}


enum class ConsumerMode {
    Book,
    Timestamp
};


const char* consumer_mode_name(ConsumerMode mode) noexcept
{
    switch (mode) {
    case ConsumerMode::Book:
        return "book";
    case ConsumerMode::Timestamp:
        return "timestamp";
    }

    std::abort();
}


// Same clock, same call path, as §6.3 calibrated and harness_a uses.
std::uint64_t now_ns() noexcept
{
#if defined(__APPLE__)
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
    timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ull +
           static_cast<std::uint64_t>(ts.tv_nsec);
#endif
}


void cpu_relax() noexcept
{
#if defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#elif defined(__x86_64__)
    asm volatile("pause" ::: "memory");
#else
    asm volatile("" ::: "memory");
#endif
}


// ---------------------------------------------------------------------
// Consumer sample — raw, not derived (§6.4b)
// ---------------------------------------------------------------------
//
// The dequeue timestamp is stored rather than a computed latency. Latency
// is recoverable offline from `sequence` via the known schedule, and the
// raw timestamp additionally answers *where in the run* the tail samples
// fell — which is what turns "p99.9 is 40 us" into "the p99.9 samples
// cluster at 1.2 s intervals, which is thermal".
struct Sample {
    std::uint64_t sequence;
    std::uint64_t dequeue_ns;
};

static_assert(sizeof(Sample) == 16);
static_assert(std::is_trivially_copyable_v<Sample>);


// What a bookTicker consumer actually does. The stream is a complete
// statement of top-of-book (§7.1), so an update is four stores, not a
// merge into a book structure.
struct BookState {
    std::int64_t bid_price = 0;
    std::int64_t ask_price = 0;
    std::int64_t bid_qty = 0;
    std::int64_t ask_qty = 0;
    std::uint64_t update_count = 0;
    std::uint64_t checksum = 0;
};


// ---------------------------------------------------------------------
// Buffers, allocated once and reused across every datapoint
// ---------------------------------------------------------------------
//
// §6.4b: preallocated and pre-touched before the clock starts, with an
// observable side effect, or the touch loop is dead code at -O2 and the
// first traversal takes page faults inside the measured window.
struct RunBuffers {
    std::vector<Sample> samples;
    std::vector<std::uint32_t> lag_ns;

    void prepare(std::size_t count)
    {
        samples.assign(count, Sample{});

        for (std::size_t i = 0; i < count; ++i) {
            samples[i].sequence = 1;
            asm volatile("" :: "r"(&samples[i]) : "memory");
        }

        std::fill(samples.begin(), samples.end(), Sample{});

        prepare_lag_buffer(lag_ns, count);
    }
};


// ---------------------------------------------------------------------
// The consumer
// ---------------------------------------------------------------------
//
// MutexQueue carries a blocking entry point that SpscRingBuffer does not,
// and that asymmetry is the point rather than a defect: §4's bounded spin
// then condvar wait is the tuning that makes the baseline fair, and it
// belongs to the baseline's *consumer*, not to try_pop. Detected here by
// interface rather than by an arm enum, so the two paths cannot drift
// apart from the queues they belong to.
template <typename Q>
concept HasWaitNonempty = requires(Q& q) {
    { q.wait_nonempty() } -> std::same_as<bool>;
};


// Only the baseline has a wait policy to instrument. The SPSC arm has no
// condvar, so signals and parks are meaningless there rather than zero.
template <typename Q>
concept HasWaitDiagnostics = requires(const Q& q) {
    { q.signals() } -> std::same_as<std::uint64_t>;
    { q.parks() } -> std::same_as<std::uint64_t>;
};


template <typename Queue>
void consume(
    Queue& queue,
    std::atomic<bool>& producer_done,
    ConsumerMode mode,
    std::vector<Sample>& samples,
    BookState& book,
    std::uint64_t& delivered_out
) noexcept
{
    std::uint64_t delivered = 0;

    Record record{};

    // One handling path, reached however the record was obtained.
    //
    // The first version of this loop had a separate final-drain branch
    // that recorded the sample but skipped the book update. That branch
    // is reachable: the top-of-loop pop can fail on a momentarily empty
    // queue, the producer can then push its last records and set
    // producer_done, and the drain pop succeeds. book.update_count would
    // then disagree with delivered and the consistency check below would
    // abort the run — rarely, and only under timing that a short test
    // would never produce.
    for (;;) {
        if (!queue.try_pop(record)) {
            if (producer_done.load(std::memory_order_acquire)) {
                // producer_done is released after the last push, so
                // observing it means every push is visible. One more
                // attempt covers a record published between the failed
                // pop above and this load.
                if (!queue.try_pop(record)) {
                    break;
                }
            } else {
                if constexpr (HasWaitNonempty<Queue>) {
                    queue.wait_nonempty();
                } else {
                    cpu_relax();
                }

                continue;
            }
        }

        const std::uint64_t dequeue_ns = now_ns();

        samples[delivered].sequence = record.sequence;
        samples[delivered].dequeue_ns = dequeue_ns;

        if (mode == ConsumerMode::Book) {
            book.bid_price = record.capture.bid_price;
            book.ask_price = record.capture.ask_price;
            book.bid_qty = record.capture.bid_qty;
            book.ask_qty = record.capture.ask_qty;
            ++book.update_count;

            book.checksum +=
                static_cast<std::uint64_t>(record.capture.bid_price) ^
                static_cast<std::uint64_t>(record.capture.ask_qty);
        }

        ++delivered;
    }

    // The payload must be provably live or the 80-byte copy in try_pop
    // becomes a candidate for elimination. This is the sink.
    asm volatile("" :: "r"(book.checksum) : "memory");

    delivered_out = delivered;
}


// ---------------------------------------------------------------------
// One datapoint
// ---------------------------------------------------------------------

struct Datapoint {
    Arm arm = Arm::Spsc;
    double rate_hz = 0.0;
    std::size_t pass = 0;

    std::uint64_t pushed = 0;
    std::uint64_t dropped_records = 0;
    std::uint64_t full_rejections = 0;
    std::uint64_t delivered = 0;

    std::uint64_t p50_lag_ns = 0;
    std::uint64_t p99_lag_ns = 0;
    std::uint64_t max_lag_ns = 0;

    std::uint64_t p50_ns = 0;
    std::uint64_t p90_ns = 0;
    std::uint64_t p99_ns = 0;
    std::uint64_t p999_ns = 0;
    std::uint64_t p9999_ns = 0;
    std::uint64_t max_ns = 0;

    std::uint64_t book_updates = 0;

    int spin_count = 0;
    std::uint64_t signals = 0;
    std::uint64_t parks = 0;

    bool drop_gate_passed = false;
    bool lag_gate_passed = false;
    bool valid = false;
};


std::uint64_t percentile_of(
    const std::vector<std::uint64_t>& sorted,
    double fraction
) noexcept
{
    if (sorted.empty()) {
        return 0;
    }

    const std::size_t index = std::min(
        sorted.size() - 1,
        static_cast<std::size_t>(
            fraction * static_cast<double>(sorted.size())
        )
    );

    return sorted[index];
}


template <typename Queue>
Datapoint run_datapoint(
    std::span<const CaptureRecord> slice,
    double rate_hz,
    Arm arm,
    ConsumerMode mode,
    std::size_t pass,
    RunBuffers& buffers,
    const char* dump_path = nullptr
)
{
    Datapoint point;
    point.arm = arm;
    point.rate_hz = rate_hz;
    point.pass = pass;

    const ReplaySchedule schedule =
        build_fixed_rate_schedule(slice.size(), rate_hz);

    auto queue = std::make_unique<Queue>();

    std::atomic<bool> producer_done{false};

    BookState book;
    std::uint64_t delivered = 0;

    QosResult consumer_qos = QosResult::not_attempted;
    QosResult producer_qos = QosResult::not_attempted;

    ReplayStats stats;

    std::thread consumer([&] {
        consumer_qos = request_user_interactive_qos();

        consume(
            *queue,
            producer_done,
            mode,
            buffers.samples,
            book,
            delivered
        );
    });

    std::thread producer([&] {
        producer_qos = request_user_interactive_qos();

        stats = run_replay(
            *queue,
            slice,
            schedule,
            0,
            0,
            0,
            buffers.lag_ns
        );

        producer_done.store(true, std::memory_order_release);

        // MutexQueue's consumer may be parked on the condvar; without
        // this it waits for a producer that has finished. SpscRingBuffer
        // has no such entry point and needs nothing here.
        if constexpr (HasWaitNonempty<Queue>) {
            queue->close();
        }
    });

    producer.join();
    consumer.join();

    // §8.0b: a run whose QoS did not apply is not the run being reported.
    const QosResult qos = combine_qos(producer_qos, consumer_qos);

    if (qos != QosResult::applied) {
        std::cerr
            << "error: QoS not applied (" << qos_result_name(qos)
            << ") on " << arm_name(arm) << " at " << rate_hz << " Hz\n";
        std::exit(1);
    }

    point.pushed = stats.pushed;
    point.dropped_records = stats.dropped_records;
    point.full_rejections = stats.full_rejections;
    point.delivered = delivered;
    point.p50_lag_ns = stats.p50_lag_ns;
    point.p99_lag_ns = stats.p99_lag_ns;
    point.max_lag_ns = stats.max_lag_ns;
    point.book_updates = book.update_count;

    if constexpr (HasWaitDiagnostics<Queue>) {
        point.spin_count = Queue::spin_count();
        point.signals = queue->signals();
        point.parks = queue->parks();
    }

    // §7.7a: under drop-newest with no retry every rejection is a drop,
    // so these must agree. They are separate counters owned by different
    // layers, and this is the one place their equality is checkable.
    if (stats.full_rejections != stats.dropped_records) {
        std::cerr
            << "error: full_rejections (" << stats.full_rejections
            << ") != dropped_records (" << stats.dropped_records
            << ") — the two counters have diverged\n";
        std::exit(1);
    }

    if (mode == ConsumerMode::Book && book.update_count != delivered) {
        std::cerr
            << "error: book updates (" << book.update_count
            << ") != delivered (" << delivered
            << ") — the consumer did not do the work the methodology "
               "claims\n";
        std::exit(1);
    }

    // Latency, computed offline from raw timestamps (§6.4b). The schedule
    // is indexed by sequence because first_sequence is zero here; the
    // general mapping is §7.3a's.
    std::vector<std::uint64_t> latencies;
    latencies.reserve(delivered);

    for (std::uint64_t i = 0; i < delivered; ++i) {
        const Sample& sample = buffers.samples[i];

        if (sample.sequence >= schedule.intended_offset_ns.size()) {
            std::cerr
                << "error: delivered sequence " << sample.sequence
                << " is outside the schedule (size "
                << schedule.intended_offset_ns.size() << ")\n";
            std::exit(1);
        }

        const std::uint64_t intended =
            stats.t0_ns + schedule.intended_offset_ns[sample.sequence];

        // Cannot underflow: a record is delivered only after being
        // pushed, and the push happens at or after its intended time.
        latencies.push_back(sample.dequeue_ns - intended);
    }

    // §6.4b's actual reason for storing raw dequeue timestamps rather
    // than computed latencies: the raw form answers *where in the run*
    // the tail samples fell. Percentiles alone cannot distinguish a cost
    // that recurs every N messages from one that recurs every T
    // microseconds, and those have completely different causes.
    //
    // Only samples above the threshold are written. At p99.9 that is
    // ~2000 rows out of 2,000,000, which is a small file rather than a
    // 60 MB one, and the sub-threshold samples carry no information
    // about the tail.
    if (dump_path != nullptr) {
        std::ofstream dump(dump_path, std::ios::app);

        if (dump) {
            for (std::uint64_t i = 0; i < delivered; ++i) {
                if (latencies[i] < 1000) {
                    continue;
                }

                const Sample& sample = buffers.samples[i];

                dump
                    << arm_name(arm) << ','
                    << rate_hz << ','
                    << i << ','
                    << sample.sequence << ','
                    << (sample.dequeue_ns - stats.t0_ns) << ','
                    << latencies[i] << '\n';
            }
        }
    }

    std::sort(latencies.begin(), latencies.end());

    point.p50_ns = percentile_of(latencies, 0.50);
    point.p90_ns = percentile_of(latencies, 0.90);
    point.p99_ns = percentile_of(latencies, 0.99);
    point.p999_ns = percentile_of(latencies, 0.999);
    point.p9999_ns = percentile_of(latencies, 0.9999);
    point.max_ns = latencies.empty() ? 0 : latencies.back();

    // §6.4's two gates.
    //
    // Drop gate: any dropped record voids the datapoint (§7.7). Dropping
    // is cheaper than delivering, so an arm that drops looks faster.
    //
    // Lag gate: p99 producer lag must stay under one offered-rate period.
    // One period is the only threshold here with a physical meaning — a
    // producer a full inter-send interval behind has missed its slot, so
    // the rate on the x-axis is not the rate delivered. Max lag is
    // reported but is deliberately not a gate: a single scheduling stall
    // produces one large latency sample, which is data about the tail
    // rather than evidence the rate was not offered.
    const double period_ns = 1'000'000'000.0 / rate_hz;

    point.drop_gate_passed = (point.dropped_records == 0);
    point.lag_gate_passed =
        static_cast<double>(point.p99_lag_ns) <= period_ns;

    point.valid = point.drop_gate_passed && point.lag_gate_passed;

    return point;
}


// ---------------------------------------------------------------------
// Provenance — same convention as harness_a and convert_capture (§7.6)
// ---------------------------------------------------------------------

struct Options {
    std::string git_commit;
    bool dirty = false;
    std::string bin_path;
    std::string symbol;
    ConsumerMode consumer = ConsumerMode::Book;
    std::size_t passes = 3;
    bool spin_sweep = false;
    bool dump_samples = false;

    // Which baseline configuration the full sweep runs. Both are
    // reported: 8192 is the tuned baseline §4 requires, 1000 is the
    // parking configuration §3 describes as catastrophic on the tail.
    // Neither alone is the result.
    int spin = 8192;
};


bool parse_options(int argc, char* argv[], Options& out)
{
    if (argc < 5 || argc > 8) {
        std::cerr
            << "Usage:\n"
            << "  " << argv[0]
            << " <git-commit-40-hex> <dirty:0|1> <capture.bin> <SYMBOL>"
               " [consumer:book|timestamp]"
               " [passes|spin-sweep|dump] [spin:1000|8192]\n";
        return false;
    }

    const std::string commit = argv[1];

    if (commit.size() != 40 ||
        commit.find_first_not_of("0123456789abcdef") != std::string::npos) {
        std::cerr << "error: git commit must be 40 lowercase hex characters\n";
        return false;
    }

    const std::string dirty_text = argv[2];

    if (dirty_text != "0" && dirty_text != "1") {
        std::cerr << "error: dirty flag must be either 0 or 1\n";
        return false;
    }

    out.git_commit = commit;
    out.dirty = (dirty_text == "1");
    out.bin_path = argv[3];
    out.symbol = argv[4];

    if (argc >= 6) {
        const std::string consumer_text = argv[5];

        if (consumer_text == "book") {
            out.consumer = ConsumerMode::Book;
        } else if (consumer_text == "timestamp") {
            out.consumer = ConsumerMode::Timestamp;
        } else {
            std::cerr << "error: consumer must be book or timestamp\n";
            return false;
        }
    }

    if (argc == 7) {
        const std::string sixth = argv[6];

        if (sixth == "spin-sweep") {
            out.spin_sweep = true;
            out.passes = 1;
        } else if (sixth == "dump") {
            out.dump_samples = true;
            out.passes = 1;
        } else {
            const long passes = std::strtol(argv[6], nullptr, 10);

            if (passes < 1 || passes > 20) {
                std::cerr
                    << "error: passes must be 1-20, or \"spin-sweep\"\n";
                return false;
            }

            out.passes = static_cast<std::size_t>(passes);
        }
    }

    if (argc == 8) {
        const std::string spin_text = argv[7];

        if (spin_text == "1000") {
            out.spin = 1000;
        } else if (spin_text == "8192") {
            out.spin = 8192;
        } else {
            std::cerr
                << "error: spin must be 1000 (parking baseline) or 8192"
                   " (tuned baseline)\n";
            return false;
        }
    }

    return true;
}


std::string utc_timestamp()
{
    const std::time_t now = std::time(nullptr);
    std::tm utc = {};

#if defined(_WIN32)
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif

    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y%m%d_%H%M%S", &utc);

    return buffer;
}

} // namespace


int main(int argc, char* argv[])
{
    Options options;

    if (!parse_options(argc, argv, options)) {
        return 2;
    }

    CaptureFile capture;

    const CaptureFileError error =
        CaptureFile::open(options.bin_path.c_str(), options.symbol, capture);

    if (error != CaptureFileError::none) {
        std::cerr
            << "error: cannot open " << options.bin_path << ": "
            << capture_file_error_name(error) << '\n';
        return 1;
    }

    const std::span<const CaptureRecord> all = capture.records();

    if (all.size() < kSliceLength) {
        std::cerr
            << "error: dataset has " << all.size()
            << " records, need at least " << kSliceLength << '\n';
        return 1;
    }

    const std::span<const CaptureRecord> slice = all.subspan(0, kSliceLength);

    // §6.4a. Warm, then prove the warming worked by comparing two full
    // traversals. The measured floor for that ratio is ~1.2, not 1.0 —
    // the residual is cache warming on the first pass, which no amount of
    // page-touching removes. A threshold of 1.0 would send you chasing a
    // phantom.
    const std::uint64_t warm_begin = now_ns();
    volatile std::uint64_t warm_sink = capture.warm();
    const std::uint64_t warm_end = now_ns();
    (void)warm_sink;

    const auto traverse = [&]() noexcept {
        std::uint64_t accumulator = 0;

        for (const CaptureRecord& record : slice) {
            accumulator += static_cast<std::uint64_t>(record.bid_price);
        }

        asm volatile("" :: "r"(accumulator) : "memory");
    };

    const std::uint64_t lap1_begin = now_ns();
    traverse();
    const std::uint64_t lap1_end = now_ns();

    traverse();
    const std::uint64_t lap2_end = now_ns();

    const double lap1_ms =
        static_cast<double>(lap1_end - lap1_begin) / 1e6;
    const double lap2_ms =
        static_cast<double>(lap2_end - lap1_end) / 1e6;
    const double lap_ratio = lap2_ms > 0.0 ? lap1_ms / lap2_ms : 0.0;

    RunBuffers buffers;
    buffers.prepare(kSliceLength);

    std::vector<Datapoint> results;
    results.reserve(options.passes * kRateCount * 2);

    using SpscArm = SpscRingBuffer<Record, kCapacity>;
    using MutexParking = MutexQueue<Record, kCapacity, 1000>;
    using MutexTuned = MutexQueue<Record, kCapacity, 8192>;

    // ---------------------------------------------------------------
    // Spin-sweep diagnostic
    // ---------------------------------------------------------------
    //
    // The first pilot showed the baseline arm failing §6.4's p99 lag gate
    // at every rate at or above 500k, with lag roughly constant at 2.7-5.3
    // us while the gate shrinks with the period. A roughly fixed cost on a
    // small fraction of pushes is the shape of an occasional syscall, and
    // the candidate is notify_one waking a parked consumer: that is a
    // __ulock_wake on the producer's critical path, charged to the
    // producer's own schedule.
    //
    // If that is the mechanism, raising the spin budget should reduce
    // parks, reduce signals-that-block, and reduce producer lag together.
    // If lag does not move while parks collapse, the cost is lock
    // contention rather than wakeups and the baseline's ceiling is real.
    //
    // Either answer is worth having before spending three passes. The
    // counters make this direct evidence rather than an inference from
    // the lag distribution.
    //
    // Note this also bears on §4's spin derivation. measure_condvar_wakeup
    // optimised the *waiter's* cost in isolation and omitted the cost
    // blocking imposes on the *signaller*. If parking is expensive for the
    // producer, the ski-rental balance point is higher than 1000 and the
    // constant needs revising with that term included.
    // ---------------------------------------------------------------
    // Tail-sample dump
    // ---------------------------------------------------------------
    //
    // The first three-pass sweep showed the SPSC arm at 83 ns p50, 125 ns
    // p99, and ~12,000 ns p99.9 — two orders of magnitude between
    // adjacent percentiles, at the same value whether the run lasted 20 s
    // or 0.2 s. Nothing in a wait-free push and pop costs 12 us on one
    // message in a thousand, so the cost is outside the queue. The same
    // floor appears in both baseline configurations, so it is added to
    // everything and it is what compresses the p99.9 comparison to 1.6x
    // while p99 shows 3-27x.
    //
    // Two hypotheses with different signatures, which is why this dumps
    // rather than guesses:
    //
    //   Per-message  — something recurring every N records, e.g. the
    //                  sample buffer crossing a 16 KiB page every 1024
    //                  entries, which is 0.0977% of messages and lands
    //                  suspiciously close to p99.9. Slow samples would
    //                  then be evenly spaced by *index* and the spacing
    //                  would not change with offered rate.
    //
    //   Per-time     — a scheduler tick or timer interrupt. Slow samples
    //                  would be evenly spaced in *time* and their index
    //                  spacing would scale with the rate.
    //
    // Running the same arm at 100k and 1M separates them: a 10x change in
    // rate leaves index spacing unchanged under the first hypothesis and
    // changes it 10x under the second.
    if (options.dump_samples) {
        const std::string dump_path =
            "results/tail_samples_" + utc_timestamp() + ".csv";

        {
            std::ofstream header(dump_path);
            header << "arm,rate_hz,index,sequence,dequeue_offset_ns,latency_ns\n";
        }

        for (const double rate : {100'000.0, 1'000'000.0}) {
            std::cerr << "dump  rate " << rate << "  arm spsc\n";
            results.push_back(run_datapoint<SpscArm>(
                slice, rate, Arm::Spsc, options.consumer, 0, buffers,
                dump_path.c_str()
            ));

            std::cerr << "dump  rate " << rate << "  arm mutex-tuned\n";
            results.push_back(run_datapoint<MutexTuned>(
                slice, rate, Arm::Mutex, options.consumer, 0, buffers,
                dump_path.c_str()
            ));
        }

        std::cout << "wrote " << dump_path << '\n';
    }

    if (options.spin_sweep) {
        constexpr double kDiagnosticRates[] = {
            500'000.0,
            1'000'000.0,
            5'000'000.0
        };

        for (const double rate : kDiagnosticRates) {
            std::cerr << "spin-sweep  rate " << rate << "  arm spsc\n";

            results.push_back(run_datapoint<SpscArm>(
                slice, rate, Arm::Spsc, options.consumer, 0, buffers
            ));

            std::cerr << "spin-sweep  rate " << rate << "  spin 1000\n";
            results.push_back(run_datapoint<MutexQueue<Record, kCapacity, 1000>>(
                slice, rate, Arm::Mutex, options.consumer, 0, buffers
            ));

            std::cerr << "spin-sweep  rate " << rate << "  spin 8192\n";
            results.push_back(run_datapoint<MutexQueue<Record, kCapacity, 8192>>(
                slice, rate, Arm::Mutex, options.consumer, 0, buffers
            ));

            std::cerr << "spin-sweep  rate " << rate << "  spin 65536\n";
            results.push_back(run_datapoint<MutexQueue<Record, kCapacity, 65536>>(
                slice, rate, Arm::Mutex, options.consumer, 0, buffers
            ));
        }
    }

    const std::size_t sweep_passes =
        (options.spin_sweep || options.dump_samples) ? 0 : options.passes;

    for (std::size_t pass = 0; pass < sweep_passes; ++pass) {
        for (std::size_t r = 0; r < kRateCount; ++r) {
            // §5: alternate which arm runs first on each pass, so
            // thermal drift on a fanless M2 does not load onto whichever
            // arm always ran second.
            const bool spsc_first = (pass % 2 == 0);

            for (int which = 0; which < 2; ++which) {
                const bool run_spsc = (which == 0) == spsc_first;

                std::cerr
                    << "pass " << pass << "  rate " << kRates[r]
                    << "  arm " << (run_spsc ? "spsc" : "mutex")
                    << '\n';

                if (run_spsc) {
                    results.push_back(run_datapoint<SpscArm>(
                        slice, kRates[r], Arm::Spsc,
                        options.consumer, pass, buffers
                    ));
                } else if (options.spin == 1000) {
                    results.push_back(run_datapoint<MutexParking>(
                        slice, kRates[r], Arm::Mutex,
                        options.consumer, pass, buffers
                    ));
                } else {
                    results.push_back(run_datapoint<MutexTuned>(
                        slice, kRates[r], Arm::Mutex,
                        options.consumer, pass, buffers
                    ));
                }
            }
        }
    }

    const std::string path =
        options.spin_sweep
            ? "results/spin_sweep_" + utc_timestamp() + ".csv"
            : "results/harness_b_spin" + std::to_string(options.spin) +
              "_" + utc_timestamp() + ".csv";

    std::ofstream out(path);

    if (!out) {
        std::cerr << "error: cannot write " << path << '\n';
        return 1;
    }

    out
        << "# experiment: " << (options.spin_sweep ? "b1_spin_sweep" : "b1")
        << '\n'
        << "# git_commit: " << options.git_commit << '\n'
        << "# git_dirty: " << (options.dirty ? "yes" : "no") << '\n'
        << "# utc: " << utc_timestamp() << '\n'
        << "# dataset: " << options.bin_path << '\n'
        << "# symbol: " << options.symbol << '\n'
        << "# dataset_records: " << all.size() << '\n'
        << "# slice_length: " << kSliceLength << '\n'
        << "# capacity: " << kCapacity << '\n'
        << "# consumer: " << consumer_mode_name(options.consumer) << '\n'
        << "# passes: " << options.passes << '\n'
        << "# qos_class: user_interactive\n"
        << "# page_size: " << capture.page_size()
        << " (apis_agree: " << (capture.page_size_agrees() ? "yes" : "no")
        << ")\n"
        << "# warm_seconds: "
        << static_cast<double>(warm_end - warm_begin) / 1e9 << '\n'
        << "# lap1_ms: " << lap1_ms << '\n'
        << "# lap2_ms: " << lap2_ms << '\n'
        << "# lap_ratio: " << lap_ratio
        << "  (see note below)\n"
        << "#\n"
        << "# §6.4a records a lap1/lap2 floor of ~1.2, but that came from\n"
        << "# traversing all 13.7M records. A 2M-record slice is ~112 MB,\n"
        << "# far beyond the 16 MB L2, so both laps stream from DRAM and\n"
        << "# there is no first-pass cache benefit to recover. A ratio at\n"
        << "# 1.0 here means no residual paging, which is what warming\n"
        << "# was for. It is not a failed check against the 1.2 floor.\n"
        << "# spin_count: " << options.spin
        << (options.spin == 1000
                ? " (parking baseline — §3's blocking condvar)\n"
                : " (tuned baseline — §4, spins past the inter-arrival"
                  " gap)\n")
        << "#\n"
        << "# Latency is intended-send to consumer-dequeue. A datapoint is\n"
        << "# valid only if dropped_records == 0 and p99 producer lag is\n"
        << "# within one offered-rate period. Invalid rows are written\n"
        << "# rather than deleted, with the gate that failed recorded.\n";

    out
        << "pass,arm,rate_hz,pushed,delivered,dropped_records,"
           "full_rejections,book_updates,spin_count,signals,parks,"
           "p50_lag_ns,p99_lag_ns,max_lag_ns,"
           "p50_ns,p90_ns,p99_ns,p999_ns,p9999_ns,max_ns,"
           "drop_gate,lag_gate,valid\n";

    for (const Datapoint& point : results) {
        out
            << point.pass << ','
            << arm_name(point.arm) << ','
            << point.rate_hz << ','
            << point.pushed << ','
            << point.delivered << ','
            << point.dropped_records << ','
            << point.full_rejections << ','
            << point.book_updates << ','
            << point.spin_count << ','
            << point.signals << ','
            << point.parks << ','
            << point.p50_lag_ns << ','
            << point.p99_lag_ns << ','
            << point.max_lag_ns << ','
            << point.p50_ns << ','
            << point.p90_ns << ','
            << point.p99_ns << ','
            << point.p999_ns << ','
            << point.p9999_ns << ','
            << point.max_ns << ','
            << (point.drop_gate_passed ? "pass" : "FAIL") << ','
            << (point.lag_gate_passed ? "pass" : "FAIL") << ','
            << (point.valid ? "yes" : "no") << '\n';
    }

    out.close();

    std::size_t valid_count = 0;

    for (const Datapoint& point : results) {
        if (point.valid) {
            ++valid_count;
        }
    }

    std::cout
        << "wrote " << path << '\n'
        << "datapoints: " << results.size()
        << "  valid: " << valid_count
        << "  discarded: " << (results.size() - valid_count) << '\n'
        << "lap1/lap2 ratio: " << lap_ratio << '\n';

    return 0;
}
