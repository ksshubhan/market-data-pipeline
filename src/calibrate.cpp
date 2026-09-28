// Timer calibration: how finely CLOCK_UPTIME_RAW resolves a very short
// interval on this machine, measured before any timing result relies on it.
//
// Reads the clock twice back to back, 1,000,000 times, and records each
// difference in nanoseconds. The clock is driven by a counter that steps
// once per timebase tick, so almost every difference is either 0 (both
// reads inside one tick) or about one tick. Assuming the reads fall at a
// random point within a tick, the fraction of non-zero differences times
// the tick length estimates how long the two reads take: a duration
// shorter than the clock's own resolution, recovered from how often the
// clock steps.
//
// Output: results/timer_calibration.csv, one difference per line, which
// scripts/plot_calibration.py draws as a histogram; bucket counts and the
// window estimates on stdout. Run it from the repository root, because the
// output path is relative. macOS only: uses mach_time.h and
// clock_gettime_nsec_np. See ARCHITECTURE.md.

#include <iostream>
#include <mach/mach_time.h>
#include <vector>
#include <cstdint>
#include <time.h>
#include <fstream>

int main() {
    mach_timebase_info_data_t tb;
    kern_return_t ret = mach_timebase_info(&tb);

    if (ret != KERN_SUCCESS) {
        std::cerr << "Error: mach_timebase_info failed with error code " << ret << '\n';
        return 1;
    } 

    std::cout << "Timebase info: numerator = " << tb.numer << ", denominator = " << tb.denom << '\n';
    // numer / denom converts the counter's ticks to nanoseconds, so it is
    // the length of one tick in ns: the step size of CLOCK_UPTIME_RAW.
    double ns_per_tick = (double)tb.numer / tb.denom;
    std::cout << "Result: " << ns_per_tick << '\n';

    // Despite the name, each entry is a difference in nanoseconds, not a
    // count of ticks. Sized up front so the timing loop never allocates.
    std::vector<uint64_t> ticks(1000000, 0);
    
    for (size_t i = 0; i < ticks.size(); ++i) {
        uint64_t t1 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        uint64_t t2 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        ticks[i] = t2 - t1;
    }

    std::ofstream file("results/timer_calibration.csv");
    if (!file.is_open()) {
        std::cerr << "Could not open results.csv\n";
        return 1;
    }

    for (size_t i = 0; i < ticks.size(); ++i) {
        file << ticks[i] << '\n';
    }

    // Spot check: the first few samples should be 0 or about one tick.
    for (size_t i = 0; i < 10; ++i) {
        uint64_t tick = ticks[i];
        std::cout << "tick: " << tick << '\n';
    }
    std::cout << '\n';

    

    // Buckets for the non-zero samples. If the two reads straddle at most one
    // tick boundary, a difference is about one tick, so anything from 70 ns
    // (between one tick and two) means something delayed the thread between
    // the reads. 1 us separates short delays, such as memory stalls, from
    // ones long enough to be the thread losing the CPU.
    const uint64_t max_single_boundary_ns = 70;
    const uint64_t max_multi_tick_boundary_ns = 1000;
    
    uint64_t nonzero = 0;
    uint64_t single_boundary = 0;
    uint64_t multi_tick = 0;
    uint64_t microsecond = 0;

    for (size_t i = 0; i < ticks.size(); i++) {
        uint64_t tick = ticks[i];
        if (tick != 0) {
            nonzero++;
        }
        if (tick != 0 && tick < max_single_boundary_ns) {
            single_boundary++;
        }
        if (tick != 0 && tick >= max_single_boundary_ns && tick < max_multi_tick_boundary_ns) {
            multi_tick++;
        }
        if (tick != 0 && tick >= max_multi_tick_boundary_ns) {
            microsecond++;
        }
    }
    std::cout << "nonzero: " << nonzero << '\n';
    std::cout << "single boundary: " << single_boundary << '\n';
    std::cout << "multi tick: " << multi_tick << '\n';
    std::cout << "microsecond: " << microsecond << '\n';

    // window counts every non-zero sample; filtered_window only those that
    // fit the one-boundary model, so delayed samples cannot inflate it.
    double window = (double)nonzero / ticks.size() * ns_per_tick;
    double filtered_window = (double)single_boundary / ticks.size() * ns_per_tick;

    std::cout << "window: " << window << '\n';
    std::cout << "filtered_window: " << filtered_window << '\n';

    // The three buckets split the non-zero samples between them, so this must
    // equal nonzero. Printed as a self-check.
    uint64_t bucket_sum = single_boundary + multi_tick + microsecond;
    std::cout << "bucket sum: " << bucket_sum << '\n';

    return 0;
}

