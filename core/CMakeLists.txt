# SPDX-FileCopyrightText: 2026 Automattic Inc. and obs-videopress contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# videopress-core: the parts of the plugin that do not touch libobs (OAuth helpers,
# the VideoPress TUS uploader, settings, title templates). Builds either as part of
# the plugin (add_subdirectory) or standalone for unit tests:
#
#   cmake -S core -B build-core -G Ninja -DCMAKE_PREFIX_PATH=<Qt6 prefix>
#   cmake --build build-core && ctest --test-dir build-core --output-on-failure

cmake_minimum_required(VERSION 3.28...3.30)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(videopress-core LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_COMPILE_WARNING_AS_ERROR ON)
  set(_videopress_core_standalone ON)
else()
  set(_videopress_core_standalone OFF)
endif()

option(VIDEOPRESS_CORE_BUILD_TESTS "Build videopress-core unit tests" ${_videopress_core_standalone})

find_package(Qt6 REQUIRED COMPONENTS Core Network)

add_library(videopress-core STATIC)
target_sources(
  videopress-core
  PRIVATE
    src/api-error.cpp
    src/jetpack-site-client.cpp
    src/oauth.cpp
    src/settings-store.cpp
    src/title-template.cpp
    src/tus-upload.cpp
  PUBLIC
    src/api-error.hpp
    src/jetpack-site-client.hpp
    src/oauth.hpp
    src/settings-store.hpp
    src/title-template.hpp
    src/tus-upload.hpp
)
target_include_directories(videopress-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(videopress-core PUBLIC Qt6::Core Qt6::Network)
target_compile_features(videopress-core PUBLIC cxx_std_17)
set_target_properties(videopress-core PROPERTIES AUTOMOC ON POSITION_INDEPENDENT_CODE ON)

if(_videopress_core_standalone AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(videopress-core PRIVATE -Wall -Wextra -Wshadow -Wsign-compare)
endif()

if(VIDEOPRESS_CORE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
