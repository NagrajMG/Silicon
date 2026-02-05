cmake_minimum_required(VERSION 3.20)
project(SILICON LANGUAGES CXX)

if(NOT APPLE)
    message(FATAL_ERROR "SILICON is macOS-only. Build aborted.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
enable_testing()

# Find files
find_package(nlohmann_json 3.11.2 QUIET)
if(NOT nlohmann_json_FOUND)
    message(STATUS "nlohmann_json not found, downloading v3.11.2...")
    include(FetchContent)
    FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

    # Disable tests and installation for faster build
    set(JSON_BuildTests OFF CACHE BOOL "")
    set(JSON_Install OFF CACHE BOOL "")
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# Interface, shared, static
add_library(silicon_logger INTERFACE)

# Any target that links to silicon_logger should automatically use this directory as an include path
target_include_directories(
    silicon_logger
    INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

add_executable(logger tests/logging.cpp)
target_link_libraries(logger PRIVATE silicon_logger)

add_library(silicon_config STATIC src/config/config_manager.cpp)
target_include_directories(
    silicon_config
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(silicon_config PUBLIC nlohmann_json::nlohmann_json silicon_logger)

add_library(silicon_process STATIC src/process/process_enforcer.cpp)
target_include_directories(
    silicon_process
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(silicon_process PUBLIC silicon_config silicon_logger)

# Main agent executable
add_executable(silicon_agent agent/main.cpp)
target_link_libraries(silicon_agent PRIVATE silicon_logger silicon_process)

# skeleton test
add_executable(agent_skeleton tests/agent_skeleton.cpp)
target_link_libraries(agent_skeleton PRIVATE silicon_logger)

# Config Manager tests
add_executable(config_manager_test tests/config_manager_test.cpp)
target_include_directories(
    config_manager_test
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(config_manager_test PRIVATE silicon_config silicon_logger)

add_executable(process_enforcer_test tests/process_enforcer_test.cpp)
target_include_directories(
    process_enforcer_test
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(process_enforcer_test PRIVATE silicon_process silicon_logger)

# Register the tests 
add_test(NAME SILICON_LOGGER_TEST COMMAND logger)
add_test(NAME SILICON_AGENT_SKELETON_TEST COMMAND agent_skeleton)
add_test(NAME SILICON_CONFIG_MANAGER_TEST COMMAND config_manager_test)
add_test(NAME SILICON_PROCESS_ENFORCER_TEST COMMAND process_enforcer_test)

# Add test properties for better output
set_tests_properties(SILICON_CONFIG_MANAGER_TEST
    PROPERTIES
    LABELS "config;unit"
    TIMEOUT 30
)

set_tests_properties(SILICON_LOGGER_TEST
    PROPERTIES
    LABELS "logging;unit"
)

set_tests_properties(SILICON_AGENT_SKELETON_TEST
    PROPERTIES
    LABELS "agent;integration"
)

set_tests_properties(SILICON_PROCESS_ENFORCER_TEST
    PROPERTIES
    LABELS "process;integration"
    TIMEOUT 60
)
