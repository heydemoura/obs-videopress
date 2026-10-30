# SPDX-FileCopyrightText: 2026 Automattic Inc. and obs-videopress contributors
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(NOT ENABLE_FRONTEND_API OR NOT ENABLE_QT)
  message(FATAL_ERROR "obs-videopress requires ENABLE_FRONTEND_API and ENABLE_QT")
endif()

find_package(obs-frontend-api REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets Qt6::Network)
target_compile_options(
  ${CMAKE_PROJECT_NAME}
  PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
)
set_target_properties(
  ${CMAKE_PROJECT_NAME}
  PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
)

# Platform-independent logic (OAuth, TUS uploads, settings) lives in a Qt-only
# static library so it can be unit-tested without libobs. See core/README.md.
set(VIDEOPRESS_CORE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(core)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE videopress-core)
if(APPLE)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE "-framework Security" "-framework CoreFoundation")
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/add-site-dialog.cpp
    src/add-site-dialog.hpp
    src/credential-store.cpp
    src/credential-store.hpp
    src/dock-icons.cpp
    src/dock-icons.hpp
    src/dock-theme.cpp
    src/dock-theme.hpp
    src/dock-widgets.cpp
    src/dock-widgets.hpp
    src/plugin-main.cpp
    src/videopress-dock.cpp
    src/videopress-dock.hpp
    src/recording-watcher.cpp
    src/recording-watcher.hpp
    src/upload-controller.cpp
    src/upload-controller.hpp
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

# OBS ships no Qt TLS backend (it uses curl), so bundle one for Qt networking to do HTTPS;
# plugin-main.cpp adds the folder to Qt's library paths at load. macOS: Secure Transport in
# Contents/PlugIns/tls. Windows: Schannel in bin/64bit/qt-plugins/tls. Linux uses the
# distribution's Qt, which ships the OpenSSL backend.
if(APPLE AND TARGET Qt6::QSecureTransportBackendPlugin)
  add_custom_command(
    TARGET ${CMAKE_PROJECT_NAME}
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:${CMAKE_PROJECT_NAME}>/PlugIns/tls"
    COMMAND
      "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:Qt6::QSecureTransportBackendPlugin>"
      "$<TARGET_BUNDLE_CONTENT_DIR:${CMAKE_PROJECT_NAME}>/PlugIns/tls/"
    VERBATIM
  )
endif()
if(WIN32)
  if(TARGET Qt6::QSchannelBackendPlugin)
    install(
      FILES "$<TARGET_FILE:Qt6::QSchannelBackendPlugin>"
      DESTINATION "${CMAKE_PROJECT_NAME}/bin/64bit/qt-plugins/tls"
      COMPONENT Runtime
    )
  else()
    message(WARNING "Qt6::QSchannelBackendPlugin not found: Windows builds will have no HTTPS support")
  endif()
endif()
