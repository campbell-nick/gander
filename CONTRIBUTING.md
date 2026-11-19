# Contributing to gander

Thanks for taking a look. gander is a small, deliberately boring C program: one
binary, no dependencies beyond libc, and no network code. Patches that keep it
that way are the easiest to accept.

## Getting started

```sh
git clone https://github.com/campbell-nick/gander
cd gander
make
./gander .          # preview this repository
```

`make` needs a C11 compiler and a POSIX system. Live reload uses `inotify` on
Linux and a portable scan-and-diff fallback elsewhere; everything works anywhere
POSIX.

## Building

| Command | What it does |
|---|---|
| `make` | the default build — libc only |
| `make CC=clang` | build with a specific compiler |
| `make WATCH=poll` | force the portable polling live-reload backend |
| `make WATCH=inotify` | force the inotify backend (Linux only) |
| `make WEBVIEW=1` | experimental embedded webview (auto-picks a backend) |
| `make WEBVIEW=gtk` | force the WebKitGTK backend |
| `make WEBVIEW=qt` | force the Qt Web Engine backend |
| `make clean` | remove the object files and the binary |

There is also a small build runner, `./invoke`, that calls `make` for you with a
`zig build`-style CLI: `./invoke install --prefix ~/.local` (the default step),
`./invoke update` (rebuild + reinstall), `./invoke test`, `./invoke run`,
`./invoke clean`, `./invoke uninstall`, plus `--release` (optimized, stripped),
`--experimental` (embedded webview) and `--pull` (fast-forward the checkout from
the remote before updating — the only step that touches the network). See
`./invoke --help`.

The default build must stay **libc-only**. If you touch the webview, keep it
behind the `WEBVIEW` flag so a plain `make` still links only libc.

## Tests

```sh
make check
```

This builds `gander` and runs the POSIX-sh suite under `tests/`:

- `tests/cli.sh` — command-line behaviour: flags, errors, target resolution.
- `tests/http.sh` — endpoint smoke tests and security negatives.
- `tests/md.sh` — the Markdown subset: headings, tables, task lists, rewrites.

Each suite is plain `sh`, sources `tests/lib.sh` for its assertions
(`assert_eq`, `assert_contains`, `assert_refused`, …), makes throwaway git
repos under `$TMPDIR`, and starts a real server on a free port. Nothing touches
the network, and nothing is left behind.

Adding a case is usually a few lines:

```sh
run_gander --help
assert_eq "--help exits 0" 0 "$STATUS"
assert_contains "--help shows usage" "$OUT" "Usage:"
```

Please add or extend a test alongside any behaviour change. The suite should
pass with both `gcc` and `clang`.

## Code layout

See [docs/architecture.md](docs/architecture.md) for how the pieces fit
together. In short: `main.c` parses the command line, `config.c` reads the
config file, and `server.c` runs the single-threaded `poll()` loop that serves
the bundled UI in `web/` and the JSON endpoints.

## Style

- C11, `-Wall -Wextra`, no warnings.
- Keep the default build libc-only.
- Match the surrounding code: two-space indent, `snake_case`, a short comment at
  the top of each file and above anything non-obvious.
- No new runtime dependencies, and nothing that talks to the network.

## Committing

- Small, focused commits with a `prefix: summary` subject line — `server:`,
  `webview:`, `repo:`, `docs:`, `tests:`, and so on.
- Explain *why* in the body when it is not obvious.
- Keep [`CHANGELOG.md`](CHANGELOG.md) current under `[Unreleased]`.

## Reporting bugs

Open an issue with your platform, the gander version (`gander --version`), and
the exact command you ran. For the embedded webview, mention whether you built
with `WEBVIEW=gtk` or `WEBVIEW=qt` and whether the window appeared at all.
