# VideoPress for OBS

An [OBS Studio](https://obsproject.com) plugin that publishes your recordings straight to
[VideoPress](https://videopress.com) on your WordPress.com and Jetpack-connected sites.

> **Status: early development.**

## What it does

- Connect your WordPress.com account from inside OBS (OAuth in OBS's built-in browser, or
  your system browser on Linux/Wayland).
- Pick the site recordings go to, and switch between all your VideoPress-capable sites
  (Jetpack, WordPress.com) from the VideoPress dock. Each OBS profile remembers its own site.
- When a recording stops, upload it, automatically or after a confirmation. Uploads are
  resumable: they survive network drops and OBS restarts, and they pause while you stream.

## Building

The plugin uses the [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate)
build system (CMake presets for macOS, Windows and Ubuntu; OBS 31.1+, Qt 6). CI builds all three
platforms on every push.

```sh
cmake --preset macos        # or windows-x64, ubuntu-x86_64
cmake --build --preset macos
```

On macOS without Xcode, you can build against your installed OBS and install the plugin into
`~/Library/Application Support/obs-studio/plugins` for testing:

```sh
build-aux/dev-macos/build.zsh   # fetches matching OBS headers + obs-deps Qt into .deps/
```

The platform-independent code (OAuth helpers, the VideoPress TUS uploader, settings) lives in
[`core/`](core) and builds without libobs, so it can be unit-tested with only Qt installed:

```sh
cmake -S core -B build-core -G Ninja -DCMAKE_PREFIX_PATH="$(brew --prefix qtbase)"
cmake --build build-core
ctest --test-dir build-core --output-on-failure
```

Formatting is enforced in CI with clang-format 19 and gersemi:
`./build-aux/run-clang-format` and `./build-aux/run-gersemi`.

## Releasing

Publishing a GitHub release builds the plugin for **Windows** (x64), **macOS** (universal)
and **Linux** (Ubuntu 24.04 x86_64) and attaches the packages and their SHA-256 checksums to
the release ([`.github/workflows/release.yaml`](.github/workflows/release.yaml)):

| | System package | Manual install (portable) | Copy into |
|---|---|---|---|
| Windows | — | `…-windows-x64-portable.zip` | `C:\ProgramData\obs-studio\plugins\` |
| macOS | `…-macos-universal.pkg` | `…-macos-universal-portable.zip` | `~/Library/Application Support/obs-studio/plugins/` |
| Linux | `…-x86_64-linux-gnu.deb` | `…-x86_64-linux-gnu-portable.tar.xz` | `~/.config/obs-studio/plugins/` |

Each portable archive is self-contained and carries an `INSTALL.txt` with the steps. A source
tarball is attached as well.

1. Bump `version` in [`buildspec.json`](buildspec.json) and commit it on `main`.
2. Publish a release whose tag is that version, e.g. `gh release create 0.2.0 --generate-notes`
   (or *Draft a new release* on GitHub). Tags like `0.2.0-beta1` / `0.2.0-rc1` work for
   pre-releases. A tag that does not match `buildspec.json` fails the workflow before building.

macOS packages are signed and notarized when the `MACOS_*` signing secrets from
obs-plugintemplate are configured on the repository; without them the `.pkg` is unsigned.

## License

VideoPress for OBS is free software, licensed under the
**GNU General Public License, version 3 or (at your option) any later version**
(`GPL-3.0-or-later`). See [LICENSE](LICENSE) for the full text and [NOTICE.md](NOTICE.md) for
third-party components. Every source file carries an SPDX license identifier.

The plugin links against libobs and the OBS frontend API (GPL-2.0-or-later, used here under
GPL-3.0), so derived works must remain GPL-compatible.
