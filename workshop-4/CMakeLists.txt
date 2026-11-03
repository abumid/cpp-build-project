cmake_minimum_required(VERSION 3.20)
project(workshop4_order_book CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

option(COVERAGE "Build with coverage instrumentation" OFF)

include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
  GIT_SHALLOW TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_library(order_book order_book.cpp)
target_include_directories(order_book PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(order_book PRIVATE -Wall -Wextra -Wpedantic -Werror)

if(COVERAGE)
  target_compile_options(order_book PUBLIC --coverage -O0 -fno-inline)
  target_link_options(order_book PUBLIC --coverage)
endif()

enable_testing()
add_executable(order_book_test order_book_test.cpp)
target_link_libraries(order_book_test PRIVATE order_book GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(order_book_test)
