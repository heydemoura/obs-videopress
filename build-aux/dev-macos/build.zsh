#!/usr/bin/env zsh
# SPDX-FileCopyrightText: 2026 Automattic Inc. and obs-videopress contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds the plugin against the installed OBS.app (no Xcode needed) and installs it into
# ~/Library/Application Support/obs-studio/plugins. Development use only.
#
#   build-aux/dev-macos/build.zsh            # build + install
#   build-aux/dev-macos/build.zsh --no-install
#   OBS_APP=/path/to/OBS.app build-aux/dev-macos/build.zsh

setopt ERR_EXIT PIPE_FAIL NO_UNSET

local repo_root=${0:A:h:h:h}
local obs_app=${OBS_APP:-/Applications/OBS.app}
local install=1
[[ ${1:-} == --no-install ]] && install=0

for tool (cmake ninja git curl tar codesign) {
  (( ${+commands[$tool]} )) || { print -u2 "missing required tool: $tool"; exit 1 }
}
[[ -d $obs_app ]] || { print -u2 "OBS not found at $obs_app (set OBS_APP)"; exit 1 }

local obs_version=$(defaults read "$obs_app/Contents/Info.plist" CFBundleShortVersionString)
local arch=$(uname -m)
local cache=$repo_root/.deps/dev-macos/$obs_version-$arch
mkdir -p $cache

# obs-studio headers at exactly the installed version.
local obs_src=$cache/obs-studio
if [[ ! -d $obs_src/libobs ]] {
  print "Fetching obs-studio $obs_version headers..."
  rm -rf $obs_src
  git clone --quiet --depth 1 --filter=blob:none --sparse --branch $obs_version \
    https://github.com/obsproject/obs-studio.git $obs_src
  git -C $obs_src sparse-checkout set libobs frontend/api
}

# obs-deps (SIMDe etc.) and Qt 6, the same release this OBS version was built with.
local deps_dir=$cache/deps
if [[ ! -d $deps_dir/lib/QtCore.framework ]] {
  # Sparse checkouts keep top-level files, so buildspec.json is there.
  local deps_version=$(python3 -c 'import json,sys; print(json.load(sys.stdin)["dependencies"]["qt6"]["version"])' \
    < $obs_src/buildspec.json)
  print "Fetching obs-deps $deps_version ($arch)..."
  mkdir -p $deps_dir
  local base=https://github.com/obsproject/obs-deps/releases/download/$deps_version
  for archive (macos-deps-$deps_version-$arch.tar.xz macos-deps-qt6-$deps_version-$arch.tar.xz) {
    curl --fail --location --silent --show-error -o $cache/$archive $base/$archive
    tar -xJf $cache/$archive -C $deps_dir
  }
}

local build_dir=$repo_root/build-dev-macos
cmake -S $repo_root/build-aux/dev-macos -B $build_dir -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_OSX_ARCHITECTURES=$arch \
  -DOBS_APP=$obs_app -DOBS_SOURCE_DIR=$obs_src -DOBS_DEPS_DIR=$deps_dir \
  -DCMAKE_INSTALL_PREFIX="$HOME/Library/Application Support/obs-studio/plugins"
cmake --build $build_dir

if (( install )) {
  rm -rf "$HOME/Library/Application Support/obs-studio/plugins/obs-videopress.plugin"
  cmake --install $build_dir
  print "Installed to ~/Library/Application Support/obs-studio/plugins/obs-videopress.plugin (OBS $obs_version)."
  print "Restart OBS to load it."
}
