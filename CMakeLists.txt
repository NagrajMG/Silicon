cmake_minimum_required(VERSION 3.16)

if(APPLE)
  set(CMAKE_CXX_COMPILER "clang++" CACHE STRING "C++ compiler" FORCE)
endif()

project(SILICON LANGUAGES CXX)

if(NOT APPLE)
  message(FATAL_ERROR "This project targets macOS.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(SILICON
  core/agent.cpp
  core/scheduler.cpp
  core/session_manager.cpp

  policies/policy_loader.cpp

  enforcement/process_enforcer.cpp
  enforcement/filesystem_enforcer.cpp
  enforcement/network_enforcer.cpp
  debugs/debug.cpp
  logging/logger.cpp
)

target_include_directories(SILICON PRIVATE ${PROJECT_SOURCE_DIR})
