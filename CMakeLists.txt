# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
cmake_minimum_required(VERSION 3.16)
project(oneconv VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(ONECONV_BUILD_TESTS "Build the test suite" ON)
option(ONECONV_STATIC "Link the C/C++ runtime statically (portable release binaries)" OFF)
set(ONECONV_VERSION_STRING "${PROJECT_VERSION}" CACHE STRING "Version reported by --version")

if(ONECONV_STATIC AND MSVC)
  # Must be set before targets are created; avoids depending on the VC++ redistributable
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(ONECONV_LIB_SOURCES
  src/util/guid.cpp
  src/util/text.cpp
  src/util/md5.cpp
  src/onestore/propset.cpp
  src/onestore/desktop.cpp
  src/onestore/packaging.cpp
  src/one/model.cpp
  src/one/math.cpp
  src/one/builder.cpp
  src/render/context.cpp
  src/render/markdown.cpp
  src/render/html.cpp
  src/render/export.cpp
  src/render/enex.cpp
  src/render/raster.cpp
  src/cab/cab.cpp
  src/cab/inflate.cpp
  src/cab/lzx.cpp
  src/app/loader.cpp
)

add_library(oneconv_core STATIC ${ONECONV_LIB_SOURCES})
target_include_directories(oneconv_core PUBLIC src)
target_compile_definitions(oneconv_core PUBLIC ONECONV_VERSION="${ONECONV_VERSION_STRING}")

if(MSVC)
  target_compile_options(oneconv_core PRIVATE /W4 /utf-8 /permissive- /EHsc)
  target_compile_definitions(oneconv_core PUBLIC _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
  target_compile_options(oneconv_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# std::filesystem needs an extra library on older GCC releases
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.1)
  target_link_libraries(oneconv_core PUBLIC stdc++fs)
endif()

add_executable(oneconv src/app/main.cpp)
target_link_libraries(oneconv PRIVATE oneconv_core)
if(MSVC)
  # MSVC picks the wmain() entry point automatically (Unicode command lines)
  target_compile_options(oneconv PRIVATE /utf-8 /EHsc)
elseif(MINGW)
  target_link_options(oneconv PRIVATE -municode)
endif()

if(ONECONV_STATIC AND NOT MSVC AND NOT APPLE)
  # Fully static executable: runs on any Linux distribution / Windows without extra DLLs
  target_link_options(oneconv PRIVATE -static)
endif()

install(TARGETS oneconv RUNTIME DESTINATION bin)

if(ONECONV_BUILD_TESTS)
  enable_testing()
  add_executable(oneconv_tests tests/unit_tests.cpp)
  target_link_libraries(oneconv_tests PRIVATE oneconv_core)
  if(MSVC)
    target_compile_options(oneconv_tests PRIVATE /utf-8 /EHsc)
  endif()
  if(MINGW)
    target_link_options(oneconv_tests PRIVATE -static)
  endif()
  add_test(NAME unit COMMAND oneconv_tests)
endif()
