cmake_minimum_required(VERSION 3.24)
project(world_generator VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(WG_BUILD_VIEWER "Build the raylib-based world viewer" ON)
option(WG_BUILD_TESTS "Build unit tests" ON)

include(cmake/Dependencies.cmake)

add_subdirectory(src)
add_subdirectory(apps/wgen)
if(WG_BUILD_VIEWER)
  add_subdirectory(apps/wview)
endif()
if(WG_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
