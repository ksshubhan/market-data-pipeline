cmake_minimum_required(VERSION 3.20)

project(market_data_pipeline LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(calibrate src/calibrate.cpp)

add_executable(check_record src/check_record.cpp)

add_executable(test_parser
    src/test_parser.cpp
    src/parser.cpp
)

add_executable(convert_capture
    src/convert_capture.cpp
    src/parser.cpp
)

add_executable(test_capture_file
    src/test_capture_file.cpp
    src/capture_file.cpp
)

add_executable(test_replay_schedule
    src/test_replay_schedule.cpp
    src/replay_schedule.cpp
)

add_executable(measure_pacing_floor
    src/measure_pacing_floor.cpp
    src/replay_schedule.cpp
)

add_executable(test_replay_producer
    src/test_replay_producer.cpp
    src/replay_schedule.cpp
)

add_executable(verify_capture
    src/verify_capture.cpp
    src/capture_file.cpp
)

add_executable(test_mutex_queue
    src/test_mutex_queue.cpp
)

add_executable(test_spsc_ring_buffer
    src/test_spsc_ring_buffer.cpp
)

add_executable(check_spsc_assembly
    src/check_spsc_assembly.cpp
)

add_executable(check_a2b_assembly
    src/check_a2b_assembly.cpp
)

add_executable(environment_probe
    src/environment_probe.cpp
)

add_executable(harness_a
    src/harness_a.cpp
)

add_executable(c1_relaxed_publication
    src/c1_relaxed_publication.cpp
)

add_executable(harness_c
    src/harness_c.cpp
)

# One command that runs every correctness test: `ctest --test-dir <build>`.
# Part of the fix for the disarmed parser suite: the defect survived partly
# because there was no single invocation whose exit status covered the tests.
#
# None of these targets uses assert(), so none of them depends on NDEBUG
# being absent. Harness C and C1 are excluded deliberately: Harness C is a
# multi-minute stress run, and C1 is intentionally invalid C++ whose
# non-deterministic failure is evidence rather than a regression signal.
enable_testing()

add_test(NAME parser COMMAND test_parser)
add_test(NAME mutex_queue COMMAND test_mutex_queue)
add_test(NAME spsc_ring_buffer COMMAND test_spsc_ring_buffer)
add_test(NAME capture_file COMMAND test_capture_file)
add_test(NAME replay_schedule COMMAND test_replay_schedule)
add_test(NAME replay_producer COMMAND test_replay_producer)
