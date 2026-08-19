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
    double ns_per_tick = (double)tb.numer / tb.denom;
    std::cout << "Result: " << ns_per_tick << '\n';

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

    for (size_t i = 0; i < 10; ++i) {
        uint64_t tick = ticks[i];
        std::cout << "tick: " << tick << '\n';
    }
    std::cout << '\n';

    

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

    double window = (double)nonzero / ticks.size() * ns_per_tick;
    double filtered_window = (double)single_boundary / ticks.size() * ns_per_tick;

    std::cout << "window: " << window << '\n';
    std::cout << "filtered_window: " << filtered_window << '\n';

    uint64_t bucket_sum = single_boundary + multi_tick + microsecond;
    std::cout << "bucket sum: " << bucket_sum << '\n';

    return 0;
}

