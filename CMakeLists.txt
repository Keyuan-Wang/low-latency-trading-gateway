cmake_minimum_required(VERSION 3.20)
project(low-latency-trading-gateway LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(LLTG_BUILD_TESTS "Build tests" ON)

if(LLTG_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(core/SPSC)
add_subdirectory(core/order_entry)
