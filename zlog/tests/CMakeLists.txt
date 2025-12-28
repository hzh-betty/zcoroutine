cmake_minimum_required(VERSION 3.18)

# Find GTest
find_package(GTest REQUIRED)

# Find Benchmark
find_package(benchmark REQUIRED)

# Find spdlog
find_package(spdlog REQUIRED)

# Find glog
find_package(glog REQUIRED)

# Unit Tests
file(GLOB UNIT_TEST_SOURCES "unit/*.cc")
message(STATUS "Project source dir: ${PROJECT_SOURCE_DIR}")
foreach(TEST_SRC ${UNIT_TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)
    add_executable(${TEST_NAME} ${TEST_SRC})
    target_link_libraries(${TEST_NAME} PRIVATE zlog_static GTest::gtest_main GTest::gmock)
    target_include_directories(${TEST_NAME} PRIVATE ${PROJECT_SOURCE_DIR}/include)
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()

# Benchmarks
add_executable(zlog_benchmark bench/bench_main.cc)
target_link_libraries(zlog_benchmark PRIVATE 
    zlog_static 
    benchmark::benchmark 
    benchmark::benchmark_main
    spdlog::spdlog
    glog::glog
    Threads::Threads
)
target_include_directories(zlog_benchmark PRIVATE ${PROJECT_SOURCE_DIR}/include)
