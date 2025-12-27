cmake_minimum_required(VERSION 3.18)
project(zcoroutine LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
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


# 查找GTest
if(TEST_UNIT_SRCS OR TEST_INTEGRATION_SRCS)
    enable_testing()
    find_package(GTest REQUIRED)
endif()

# 为每个单元测试文件创建独立的测试目标
if(TEST_UNIT_SRCS)
    foreach(test_source ${TEST_UNIT_SRCS})
        # 获取文件名（不含路径和扩展名）
        get_filename_component(test_name ${test_source} NAME_WE)
        # 创建可执行文件
        add_executable(${test_name} ${test_source})
        # 链接库
        target_link_libraries(${test_name} PRIVATE
                zcoroutine_shared
                GTest::gtest
                pthread
        )
        # 添加为CTest测试
        add_test(NAME ${test_name} COMMAND ${test_name})
        message(STATUS "Configured unit test: ${test_name}")
    endforeach()
endif()

# 为每个集成测试文件创建独立的测试目标
if(TEST_INTEGRATION_SRCS)
    foreach(test_source ${TEST_INTEGRATION_SRCS})
        # 获取文件名（不含路径和扩展名）
        get_filename_component(test_name ${test_source} NAME_WE)
        # 创建可执行文件
        add_executable(${test_name} ${test_source})
        # 链接库
        target_link_libraries(${test_name} PRIVATE
                zcoroutine_shared
                GTest::gtest
                pthread
        )
        # 添加为CTest测试
        add_test(NAME ${test_name} COMMAND ${test_name})
        message(STATUS "Configured integration test: ${test_name}")
    endforeach()
endif()

# Benchmark 可执行文件
if(EXISTS ${PROJECT_SOURCE_DIR}/tests/benchmark/http_server.cc)
    add_executable(http_server_benchmark ${PROJECT_SOURCE_DIR}/tests/benchmark/http_server.cc)
    target_link_libraries(http_server_benchmark PRIVATE
            zcoroutine_shared
            pthread
    )
    message(STATUS "Configured benchmark: http_server")
endif()