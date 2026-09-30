cmake_minimum_required(VERSION 3.21)
project(OrderBook LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ORDERBOOK_BUILD_TESTS "Build unit tests" ON)
option(ORDERBOOK_BUILD_BENCH "Build benchmark harness" ON)

add_subdirectory(src)

if(ORDERBOOK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(ORDERBOOK_BUILD_BENCH)
    add_subdirectory(bench)
endif()
