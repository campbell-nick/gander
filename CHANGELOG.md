# Changelog

All notable changes to gander are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.2.0] - 2026-11-19

### Added

- `--app` opens the UI in a chromeless Chromium-family window instead of a
  browser tab, using a throwaway profile; it falls back to a normal tab when no
  such browser is found. Also settable as `app = true` in the config file.
- `--webview` shows the UI in an experimental embedded window instead of a
  browser, built opt-in with `make WEBVIEW=1` (WebKitGTK on GTK3, or Qt Web
  Engine where GTK is absent). The window uses the bundled gander logo as its
  icon. A build without it says so and opens a browser instead. Also settable as
  `webview = true` in the config file.
- Content search: press `/` to search file contents with `git grep --untracked`
  and click a hit to open the file at that line (`/api/search`).
- A `gander(1)` manual page, installed by `make install`.
- Contributor and architecture notes: `CONTRIBUTING.md`, `docs/architecture.md`
  and `docs/webview.md`.
- A `./invoke` build runner: a `zig build`-style front end over the Makefile
  with `install` (default), `update`, `test`, `run`, `clean` and `uninstall`
  steps, plus `--prefix`, `--release` (optimized, stripped), `--experimental`
  (embedded webview) and `--pull` (fast-forward the checkout before updating).
- A portable live-reload backend. Linux still uses inotify; every other system
  (and any build with `make WATCH=poll`) falls back to a periodic scan-and-diff
  of the tree, so live reload now works off Linux too. The banner names the
  backend in use.

### Fixed

- `--app` now recognises Helium (`helium`) as a Chromium-family browser.
- The embedded webview no longer feels laggy: the toolkit runs its own event
  loop and the server services its sockets from a short periodic callback, so
  input and rendering are handled immediately instead of only after each server
  poll.

## [0.1.0] - 2026-11-18

The first release: an offline previewer for a local git repository.

### Added

- **File tree** with filtering and collapsible folders; previews refresh when
  the files on disk change.
- **Markdown preview** rendered by gander's own subset, with relative images
  and GIFs shown inline and a safe whitelist for raw HTML.
- **Media preview**: images, animated GIFs, SVG, `<video>` with byte-range
  seeking, and audio.
- **Code preview** with GitHub-style syntax highlighting and a line-number
  gutter.
- **Repo home card**: branch, latest tag, a CI-configured chip, license,
  commit and file counts, a language breakdown, the last commit and the
  rendered README — all read from disk, never the network.
- **Git panel**: working-tree changes grouped as staged/changes/untracked with
  status colours in the tree, a recent-commit log and a per-file unified diff.
- **Live reload** over Server-Sent Events, refreshing only when the git history
  changes; `--watch all` also refreshes on every file change.
- **Configuration** at `~/.config/gander/config`, with a user `style.css`
  layered over the bundled theme and a `[paths]` section of named shortcuts.
- **Command line**: `gander <path>`, `gander current`, `gander <name>` for a
  `[paths]` shortcut, bare `gander` for the command list, and `--version` with
  the ASCII goose.
- Bundled GitHub Dark theme.

### Changed

- gander previews git repositories only; pointed at a plain folder it prints a
  line and exits without serving.
- Quitting with Ctrl-C no longer echoes `^C`.
- The CI chip shows only whether CI is configured, never a live pass/fail
  result, which would mean going online.

[Unreleased]: https://github.com/campbell-nick/gander/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/campbell-nick/gander/releases/tag/v0.2.0
[0.1.0]: https://github.com/campbell-nick/gander/releases/tag/v0.1.0
