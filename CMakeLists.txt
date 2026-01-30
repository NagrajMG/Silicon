cmake_minimum_required(VERSION 3.20)
project(SILICON LANGUAGES CXX)

if(NOT APPLE)
    message(FATAL_ERROR "SILICON is macOS-only. Build aborted.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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

add_executable(silicon_agent agent/main.cpp)
target_link_libraries(silicon_agent PRIVATE silicon_logger)

add_executable(agent_skeleton tests/agent_skeleton.cpp)
target_link_libraries(agent_skeleton PRIVATE silicon_logger)

enable_testing()
# Register the tests 
add_test(NAME SILICON_LOGGER COMMAND logger)
add_test(NAME SILICON_AGENT_SKELETON COMMAND agent_skeleton)
