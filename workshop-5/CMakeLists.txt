cmake_minimum_required(VERSION 3.20)
project(workshop5_order_books CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(COVERAGE "Build with coverage instrumentation" OFF)

include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
  GIT_SHALLOW TRUE
)
FetchContent_Declare(
  googlebenchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG v1.9.1
  GIT_SHALLOW TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest googlebenchmark)

add_library(order_books book_types.cpp tick_order_book.cpp map_order_book.cpp heap_order_book.cpp)
target_include_directories(order_books PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(order_books PRIVATE -Wall -Wextra -Wpedantic -Werror)

if(COVERAGE)
  target_compile_options(order_books PUBLIC --coverage -O0 -fno-inline)
  target_link_options(order_books PUBLIC --coverage)
endif()

enable_testing()
add_executable(order_book_test order_book_test.cpp)
target_link_libraries(order_book_test PRIVATE order_books GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(order_book_test)

add_executable(order_book_bench order_book_bench.cpp)
target_link_libraries(order_book_bench PRIVATE order_books benchmark::benchmark)
