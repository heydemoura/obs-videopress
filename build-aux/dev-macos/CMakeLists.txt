# SPDX-FileCopyrightText: 2026 Automattic Inc. and obs-videopress contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Local macOS development build that does not need Xcode. It compiles against the OBS
# headers and obs-deps Qt matching an installed OBS.app and links against that app's
# libobs and obs-frontend-api. Release builds use the obs-plugintemplate presets instead.
# Driven by build-aux/dev-macos/build.zsh.

cmake_minimum_required(VERSION 3.28...3.30)

set(_repo_root "${CMAKE_CURRENT_LIST_DIR}/../..")
file(READ "${_repo_root}/buildspec.json" _buildspec)
string(JSON _name GET "${_buildspec}" name)
string(JSON _version GET "${_buildspec}" version)
string(JSON _bundle_id GET "${_buildspec}" platformConfig macos bundleId)

project(${_name} VERSION ${_version} LANGUAGES C CXX)

set(OBS_APP "/Applications/OBS.app" CACHE PATH "Installed OBS.app to link against")
set(OBS_SOURCE_DIR "" CACHE PATH "obs-studio sources at the installed OBS version (libobs, frontend/api)")
set(OBS_DEPS_DIR "" CACHE PATH "Extracted obs-deps and obs-deps Qt6 for the installed OBS version")

foreach(_var IN ITEMS OBS_SOURCE_DIR OBS_DEPS_DIR)
  if(NOT IS_DIRECTORY "${${_var}}")
    message(FATAL_ERROR "${_var} must point at a directory (got '${${_var}}')")
  endif()
endforeach()

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET 12.0)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

list(PREPEND CMAKE_PREFIX_PATH "${OBS_DEPS_DIR}")
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Widgets Network)

# libobs and obs-frontend-api come from the installed app; their install names are
# @rpath-relative, so the plugin resolves them inside OBS at runtime.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/obs-config/obsconfig.h" "#pragma once\n#define OBS_RELEASE_CANDIDATE 0\n#define OBS_BETA 0\n")

add_library(OBS::libobs SHARED IMPORTED)
set_target_properties(
  OBS::libobs
  PROPERTIES
    IMPORTED_LOCATION "${OBS_APP}/Contents/Frameworks/libobs.framework/Versions/A/libobs"
    INTERFACE_INCLUDE_DIRECTORIES "${OBS_SOURCE_DIR}/libobs;${CMAKE_CURRENT_BINARY_DIR}/obs-config;${OBS_DEPS_DIR}/include"
)

add_library(OBS::obs-frontend-api SHARED IMPORTED)
set_target_properties(
  OBS::obs-frontend-api
  PROPERTIES
    IMPORTED_LOCATION "${OBS_APP}/Contents/Frameworks/obs-frontend-api.dylib"
    INTERFACE_INCLUDE_DIRECTORIES "${OBS_SOURCE_DIR}/frontend/api"
    INTERFACE_LINK_LIBRARIES OBS::libobs
)

add_subdirectory("${_repo_root}/core" core)

configure_file("${_repo_root}/src/plugin-support.c.in" plugin-support.c @ONLY)

add_library(${PROJECT_NAME} MODULE)
target_sources(
  ${PROJECT_NAME}
  PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/plugin-support.c"
    "${_repo_root}/src/add-site-dialog.cpp"
    "${_repo_root}/src/add-site-dialog.hpp"
    "${_repo_root}/src/credential-store.cpp"
    "${_repo_root}/src/credential-store.hpp"
    "${_repo_root}/src/dock-icons.cpp"
    "${_repo_root}/src/dock-icons.hpp"
    "${_repo_root}/src/dock-theme.cpp"
    "${_repo_root}/src/dock-theme.hpp"
    "${_repo_root}/src/dock-widgets.cpp"
    "${_repo_root}/src/dock-widgets.hpp"
    "${_repo_root}/src/plugin-main.cpp"
    "${_repo_root}/src/recording-watcher.cpp"
    "${_repo_root}/src/recording-watcher.hpp"
    "${_repo_root}/src/upload-controller.cpp"
    "${_repo_root}/src/upload-controller.hpp"
    "${_repo_root}/src/videopress-dock.cpp"
    "${_repo_root}/src/videopress-dock.hpp"
)
target_include_directories(${PROJECT_NAME} PRIVATE "${_repo_root}/src")
target_link_libraries(
  ${PROJECT_NAME}
  PRIVATE
    OBS::libobs
    OBS::obs-frontend-api
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
    videopress-core
    "-framework Security"
    "-framework CoreFoundation"
)
target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wno-unused-parameter)

set_target_properties(
  ${PROJECT_NAME}
  PROPERTIES
    AUTOMOC ON
    BUNDLE TRUE
    BUNDLE_EXTENSION plugin
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_LIST_DIR}/Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER "${_bundle_id}"
    MACOSX_BUNDLE_BUNDLE_NAME "${PROJECT_NAME}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    INSTALL_RPATH "@executable_path/../Frameworks"
    BUILD_WITH_INSTALL_RPATH ON
)

# OBS.app ships no Qt TLS backend (OBS itself uses curl), so Qt networking cannot do HTTPS
# inside OBS unless the plugin brings one. The plugin adds Contents/PlugIns to Qt's library
# paths at load time (plugin-main.cpp).
set(_tls_dir "$<TARGET_BUNDLE_CONTENT_DIR:${PROJECT_NAME}>/PlugIns/tls")

# Locale and other resources, the TLS backend, then an ad-hoc signature so dyld accepts
# the bundle (nested code first, then the bundle seal).
add_custom_command(
  TARGET ${PROJECT_NAME}
  POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_repo_root}/data" "$<TARGET_BUNDLE_CONTENT_DIR:${PROJECT_NAME}>/Resources"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${_tls_dir}"
  COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:Qt6::QSecureTransportBackendPlugin>" "${_tls_dir}/"
  COMMAND codesign --force --sign - "${_tls_dir}/$<TARGET_FILE_NAME:Qt6::QSecureTransportBackendPlugin>"
  COMMAND codesign --force --sign - "$<TARGET_BUNDLE_DIR:${PROJECT_NAME}>"
  VERBATIM
)

install(TARGETS ${PROJECT_NAME} LIBRARY DESTINATION .)
