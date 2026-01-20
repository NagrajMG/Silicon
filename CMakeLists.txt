cmake_minimum_required(VERSION 3.16)

# If building on macOS, force usage of clang++
# Apple’s toolchain is clang-based and tightly integrated with macOS SDKs.
if(APPLE)
    set(CMAKE_CXX_COMPILER "clang++" CACHE STRING "C++ compiler" FORCE)
endif()

project(SILICON LANGUAGES CXX)

if(NOT APPLE)
    message(FATAL_ERROR "This project targets macOS.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(SILICON
    core/agent.cpp
    logging/logger.cpp
)

target_include_directories(SILICON PRIVATE ${PROJECT_SOURCE_DIR})
