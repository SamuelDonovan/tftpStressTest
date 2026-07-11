cmake_minimum_required(VERSION 3.16)

project(tftp_test_harness
        VERSION 1.0.0
        DESCRIPTION "Local loopback TFTP conformance and robustness test harness"
        LANGUAGES CXX)

# --------------------------------------------------------------------------
# Language standard: C++17 (portable to older/embedded toolchains). C++20 is
# used only where explicitly gated and confirmed available.
# --------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# Export compile_commands.json for tooling.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --------------------------------------------------------------------------
# Warnings. Correctness-first, so warnings are loud but not fatal by default
# (a constrained toolchain may differ slightly); enable -Werror via
# -DTFTP_HARNESS_WERROR=ON in CI.
# --------------------------------------------------------------------------
option(TFTP_HARNESS_WERROR "Treat warnings as errors" OFF)
option(TFTP_HARNESS_BUILD_TESTS "Build the harness self-tests" ON)

add_library(tftp_harness_warnings INTERFACE)
if(MSVC)
    target_compile_options(tftp_harness_warnings INTERFACE /W4 /permissive-)
    if(TFTP_HARNESS_WERROR)
        target_compile_options(tftp_harness_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(tftp_harness_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
    if(TFTP_HARNESS_WERROR)
        target_compile_options(tftp_harness_warnings INTERFACE -Werror)
    endif()
endif()

# --------------------------------------------------------------------------
# Threads (std::thread) — required by the proxy and the runner.
# --------------------------------------------------------------------------
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# --------------------------------------------------------------------------
# Core harness library. Globs every implementation file under src/ so the long
# incremental build does not require editing this file per source added.
# CONFIGURE_DEPENDS re-globs on build when the file set changes.
# --------------------------------------------------------------------------
file(GLOB_RECURSE TFTP_HARNESS_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(tftp_harness STATIC ${TFTP_HARNESS_SOURCES})
target_include_directories(tftp_harness PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(tftp_harness
    PUBLIC Threads::Threads
    PRIVATE tftp_harness_warnings)
if(WIN32)
    target_link_libraries(tftp_harness PUBLIC ws2_32)
endif()

# --------------------------------------------------------------------------
# Adapter library (reference implementations + subprocess base + examples).
# Separate target so an integrator can build the harness without dragging in
# every example adapter.
# --------------------------------------------------------------------------
file(GLOB_RECURSE TFTP_ADAPTER_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/adapters/*.cpp")
if(TFTP_ADAPTER_SOURCES)
    add_library(tftp_adapters STATIC ${TFTP_ADAPTER_SOURCES})
    target_include_directories(tftp_adapters PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/adapters")
    target_link_libraries(tftp_adapters
        PUBLIC tftp_harness
        PRIVATE tftp_harness_warnings)
endif()

# --------------------------------------------------------------------------
# Applications: runner, report generator, comparison. Each apps/*_main.cpp
# becomes its own executable.
# --------------------------------------------------------------------------
file(GLOB TFTP_APP_MAINS CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/apps/*_main.cpp")
foreach(app_main ${TFTP_APP_MAINS})
    get_filename_component(app_stem "${app_main}" NAME_WE)
    add_executable(${app_stem} "${app_main}")
    target_link_libraries(${app_stem}
        PRIVATE tftp_harness tftp_harness_warnings)
    if(TARGET tftp_adapters)
        target_link_libraries(${app_stem} PRIVATE tftp_adapters)
    endif()
endforeach()

# --------------------------------------------------------------------------
# Self-tests. Each tests/test_*.cpp is a standalone executable registered with
# CTest.
# --------------------------------------------------------------------------
if(TFTP_HARNESS_BUILD_TESTS)
    enable_testing()
    file(GLOB TFTP_TEST_MAINS CONFIGURE_DEPENDS
         "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_*.cpp")
    foreach(test_main ${TFTP_TEST_MAINS})
        get_filename_component(test_stem "${test_main}" NAME_WE)
        add_executable(${test_stem} "${test_main}")
        target_include_directories(${test_stem} PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/tests")
        target_link_libraries(${test_stem}
            PRIVATE tftp_harness tftp_harness_warnings)
        if(TARGET tftp_adapters)
            target_link_libraries(${test_stem} PRIVATE tftp_adapters)
        endif()
        add_test(NAME ${test_stem} COMMAND ${test_stem})
        set_tests_properties(${test_stem} PROPERTIES TIMEOUT 300)
    endforeach()
endif()
