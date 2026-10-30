# Contributing

## Licensing of contributions

This project is licensed under **GPL-3.0-or-later**. By contributing, you agree that your
contribution is licensed under the same terms.

Sign off every commit to certify the [Developer Certificate of Origin](https://developercertificate.org):

```sh
git commit -s
```

New source files start with an SPDX header:

```cpp
// SPDX-FileCopyrightText: 2026 Automattic Inc. and obs-videopress contributors
// SPDX-License-Identifier: GPL-3.0-or-later
```

Do not copy code under licenses that are incompatible with GPL-3.0-or-later. List any new
third-party component in [NOTICE.md](NOTICE.md).

## Development

- Keep libobs-independent logic in `core/` with unit tests (`core/tests`). The plugin layer
  in `src/` should stay thin: OBS events, Qt widgets, wiring.
- Never block the OBS UI thread or touch libobs output/graphics threads from plugin code;
  network and file I/O run on worker threads.
- Never log or persist WordPress.com access tokens outside the OS keychain.
- Format with `./build-aux/run-clang-format` and `./build-aux/run-gersemi` (clang-format 19,
  gersemi from the `obsproject/tools` Homebrew tap) before pushing.
