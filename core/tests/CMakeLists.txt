# SPDX-FileCopyrightText: 2026 Automattic Inc. and obs-videopress contributors
# SPDX-License-Identifier: GPL-3.0-or-later

find_package(Qt6 REQUIRED COMPONENTS Test)

add_library(videopress-test-support STATIC mock-http-server.cpp mock-http-server.hpp)
target_include_directories(videopress-test-support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(videopress-test-support PUBLIC Qt6::Core Qt6::Network)
set_target_properties(videopress-test-support PROPERTIES AUTOMOC ON)

foreach(
  _test
  IN
  ITEMS test-api-error test-jetpack-site-client test-oauth test-settings-store test-tus-upload
)
  add_executable(${_test} ${_test}.cpp)
  target_link_libraries(${_test} PRIVATE videopress-core videopress-test-support Qt6::Test)
  set_target_properties(${_test} PROPERTIES AUTOMOC ON)
  add_test(NAME ${_test} COMMAND ${_test})
  set_tests_properties(${_test} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endforeach()
