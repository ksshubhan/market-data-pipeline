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

add_executable(test_mutex_queue
    src/test_mutex_queue.cpp
)

add_executable(test_spsc_ring_buffer
    src/test_spsc_ring_buffer.cpp
)

add_executable(check_spsc_assembly
    src/check_spsc_assembly.cpp
)
