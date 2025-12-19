CMAKE_MINIMUM_REQUIRED(VERSION 3.18)
PROJECT(zcoroutine LANGUAGES CXX)
SET(CMAKE_CXX_STANDARD 14)
SET(CMAKE_CXX_STANDARD_REQUIRED ON)

# 日志库
add_subdirectory(zlog)

# 收集源文件
# 工具类源文件
file(GLOB UTIL_SRCS 
    ${PROJECT_SOURCE_DIR}/src/util/*.cc
)

# 同步原语源文件
file(GLOB SYNC_SRCS 
    ${PROJECT_SOURCE_DIR}/src/sync/*.cc
)

# 运行时层源文件
file(GLOB RUNTIME_SRCS 
    ${PROJECT_SOURCE_DIR}/src/runtime/*.cc
)

# 调度层源文件
file(GLOB SCHEDULING_SRCS 
    ${PROJECT_SOURCE_DIR}/src/scheduling/*.cc
)

# IO层源文件
file(GLOB IO_SRCS 
    ${PROJECT_SOURCE_DIR}/src/io/*.cc
)

# 定时器层源文件
file(GLOB TIMER_SRCS 
    ${PROJECT_SOURCE_DIR}/src/timer/*.cc
)

# Hook层源文件
file(GLOB HOOK_SRCS 
    ${PROJECT_SOURCE_DIR}/src/hook/*.cc
)

# 合并所有源文件
set(ZCOROUTINE_SRCS
    ${UTIL_SRCS}
    ${SYNC_SRCS}
    ${RUNTIME_SRCS}
    ${SCHEDULING_SRCS}
    ${IO_SRCS}
    ${TIMER_SRCS}
    ${HOOK_SRCS}
)

# 创建共享库和静态库
add_library(zcoroutine_shared SHARED ${ZCOROUTINE_SRCS})
add_library(zcoroutine_static STATIC ${ZCOROUTINE_SRCS})
target_include_directories(zcoroutine_shared PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_include_directories(zcoroutine_static PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(zcoroutine_shared PUBLIC zlog_shared dl)
target_link_libraries(zcoroutine_static PUBLIC zlog_static dl)

# 输出信息
message(STATUS "zcoroutine library configuration:")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  Shared Library: zcoroutine_shared")
message(STATUS "  Static Library: zcoroutine_static")

# 收集测试源文件
file(GLOB TEST_UNIT_SRCS ${PROJECT_SOURCE_DIR}/tests/unit/*.cc)
file(GLOB TEST_INTEGRATION_SRCS ${PROJECT_SOURCE_DIR}/tests/integration/*.cc)

# 设置测试输出目录
set(TEST_OUTPUT_DIR ${CMAKE_BINARY_DIR}/bin/tests)
file(MAKE_DIRECTORY ${TEST_OUTPUT_DIR})

# 单元测试可执行文件
if(TEST_UNIT_SRCS)
    enable_testing()
    find_package(GTest REQUIRED)
    add_executable(zcoroutine_unit_tests ${TEST_UNIT_SRCS})
    target_include_directories(zcoroutine_unit_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(zcoroutine_unit_tests PRIVATE
            zcoroutine_shared
            GTest::gtest
            pthread
    )
    set_target_properties(zcoroutine_unit_tests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${TEST_OUTPUT_DIR}
    )
    include(GoogleTest)
    gtest_discover_tests(zcoroutine_unit_tests)
    message(STATUS "Configured unit tests: ${TEST_UNIT_SRCS}")
endif()

# 集成测试可执行文件
if(TEST_INTEGRATION_SRCS)
    enable_testing()
    find_package(GTest REQUIRED)
    add_executable(zcoroutine_integration_tests ${TEST_INTEGRATION_SRCS})
    target_include_directories(zcoroutine_integration_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(zcoroutine_integration_tests PRIVATE
            zcoroutine_shared
            GTest::gtest
            pthread
    )
    set_target_properties(zcoroutine_integration_tests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${TEST_OUTPUT_DIR}
    )
    include(GoogleTest)
    gtest_discover_tests(zcoroutine_integration_tests)
    message(STATUS "Configured integration tests: ${TEST_INTEGRATION_SRCS}")
endif()

# Benchmark 可执行文件
if(EXISTS ${PROJECT_SOURCE_DIR}/tests/benchmark/http_server.cc)
    add_executable(http_server_benchmark ${PROJECT_SOURCE_DIR}/tests/benchmark/http_server.cc)
    target_include_directories(http_server_benchmark PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(http_server_benchmark PRIVATE
            zcoroutine_shared
            pthread
    )
    set_target_properties(http_server_benchmark PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${TEST_OUTPUT_DIR}
    )
    message(STATUS "Configured benchmark: http_server")
endif()