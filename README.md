<p align="center"><img src="web/logo.svg" width="128" height="128" alt="gander logo"></p>

# gander

**Take a gander at your repo.** An offline previewer for a local git
repository: a file tree on the left, live previews on the right — Markdown,
images, animated GIFs, video, PDFs and source code — that refresh the moment
the files on disk change.

Everything is local. `gander` is a small, dependency-free C program that serves
a bundled web UI on `127.0.0.1` and opens it in your browser. No network, no
CDN, no telemetry — nothing leaves your machine.

```
gander <path>        preview the repository at <path>
gander current       preview the current directory
gander <name>        preview a name from the config's [paths]
```

## Why

Most "preview this repo" tools either need a build step, a runtime, or the
internet. `gander` is one tiny binary plus a handful of bundled assets. It is
the sibling of [`nexus`](https://github.com/campbell-nick/nexus): same spirit,
same minimal C, a different view of your work.

## Features

- **File tree** of any directory, with filtering and collapsible folders.
- **Markdown preview** rendered by gander's own Markdown subset, with relative
  images and GIFs shown inline.
- **Media preview**: images, animated GIFs, SVG, `<video>` (with byte-range
  seeking) and audio.
- **Code and text** preview with GitHub-style syntax highlighting and a
  line-number gutter.
- **Content search**: press `/` to full-text search the working tree with
  `git grep`; click a hit to open the file at that line.
- **Repo home card**: branch, latest tag (as the release), whether CI is
  configured, license, commit and file counts, a language breakdown, the last
  commit and the rendered README below — all read from disk, never the network.
- **Git panel**: working-tree changes grouped as staged/changes/untracked with
  status colours in the tree, a recent-commit log, and a per-file unified diff.
- **Live reload**: refresh only when you commit (or switch branch/tag/fetch),
  keeping your place; `--watch all` also refreshes as you edit files.
- **Embedded webview** *(experimental)*: an opt-in `make WEBVIEW=1` build lets
  `--webview` show the UI in gander's own window — WebKitGTK, or Qt WebEngine
  where GTK is absent — instead of a browser tab.
- **Offline by construction**: bound to `127.0.0.1`, strict CSP, all assets
  bundled, and no network code at all.

## Build

Requires a C11 compiler and a POSIX system (Linux for live reload). No
dependencies beyond libc.

```sh
make
./gander .
```

An experimental embedded webview is available as an opt-in build. `make
WEBVIEW=1` links against a browser engine and lets `--webview` show the UI in
its own window instead of a browser. It prefers WebKitGTK (on GTK3) and falls
back to Qt WebEngine when GTK is not present; `make WEBVIEW=gtk` or `make
WEBVIEW=qt` forces one. It is off by default, so a plain `make` stays
dependency-free. See [docs/webview.md](docs/webview.md) for the details.

## Install

`make install` places the binary in `$PREFIX/bin`, the UI assets in
`$PREFIX/share/gander/web` (found automatically at runtime), and the manual
page in `$PREFIX/share/man/man1`, so `man gander` works after installing. The
recommended setup is a per-user install into `~/.local`:

```sh
make
make install PREFIX=$HOME/.local
```

A small build runner, `./invoke`, wraps that with a `zig build`-style CLI:

```sh
./invoke install --prefix ~/.local   # install (the default step)
./invoke update --prefix ~/.local    # rebuild + reinstall over the install
./invoke update --pull               # git pull --ff-only, then rebuild + reinstall
./invoke test                        # run the test suite
./invoke run                         # build and preview the repo
./invoke install --release           # -O2 -DNDEBUG, and strip the binary
./invoke install --experimental      # embedded webview (auto backend)
./invoke uninstall --prefix ~/.local # remove what install placed
```

For system-wide installs, packaging, uninstall and troubleshooting, see
[docs/install.md](docs/install.md).

## Usage

```
gander <path>        preview the repository at <path>
gander <name>        preview a name from the config's [paths]
gander current       preview the current directory
gander               show this help

  --port <n>        port to listen on (default: pick a free one)
  --host <addr>     address to bind (default: 127.0.0.1)
  --web-root <dir>  directory holding the bundled UI assets
  --token <str>     require this token to open the UI
  --config <file>   read settings from this file instead of the default
  --no-open         do not launch a browser
  --app             open a chromeless app window instead of a tab
  --webview         show the UI in an embedded window (experimental; needs a
                    build made with 'make WEBVIEW=1')
  --watch <mode>    what live reload reacts to: commit (default), all or off
  --no-watch        disable live reload (same as --watch off)
  -h, --help        show help and exit
  -V, --version     show the version and exit
```

`path` must be a git repository — a `.git` directory, or a `.git` file for a
linked worktree. Given a plain folder, gander prints a line and exits. Use
`current` (or `.`) to preview the directory you are in.

The UI assets are found via, in order: `--web-root`, `$GANDER_WEB`,
`<exe-dir>/web`, `<exe-dir>/../share/gander/web`, then `./web`.

`--app` opens the UI in a chromeless window rather than a tab. It needs no extra
toolkit: it looks for a Chromium-family browser on `PATH` (`chromium`,
`google-chrome`, `brave`, `helium`, `microsoft-edge`, `vivaldi`, …) and launches
it with `--app=` and a throwaway profile under `$XDG_CACHE_HOME/gander/app`, so
it never touches your normal browser data. Firefox has no equivalent, so if no
Chromium-family browser is found, gander says so and opens a normal tab instead.
Set `app = true` in the config file to make it the default.

`--webview` is the embedded alternative: rather than handing the URL to a
browser, gander shows it in its own window (WebKitGTK, or Qt WebEngine where GTK
is absent) and stops when you close that window. The window is titled `gander`
and carries the bundled logo as its icon. It is experimental and only present in
a `make WEBVIEW=1` build; without one, gander says so and opens a browser
instead. Set `webview = true` in the config file to make it the default. If you
would rather not build an engine, `--app` gives you a chromeless window with no
extra dependencies at all.

## Configuration

Settings can live in a config file instead of on the command line. It is read
from the first of these that exists:

1. `--config <file>` (or `$GANDER_CONFIG`)
2. `$XDG_CONFIG_HOME/gander/config`
3. `$HOME/.config/gander/config`

It is plain `key = value` lines; blank lines and `#`/`;` comments are ignored,
and a value may be quoted. A missing file is fine — every key has a built-in
default — and an unknown key is warned about and skipped. Command-line options
always win over the file.

```ini
# ~/.config/gander/config
port      = 8080          # 0 picks a free port (default)
host      = 127.0.0.1     # interface to bind
open      = true          # launch a browser on start
app       = false         # open a chromeless app window (Chromium family)
webview   = false         # show the UI in an embedded window (WEBVIEW=1 build)
watch     = commit        # commit (default), all or off
web-root  = /usr/share/gander/web
token     = hunter2       # require this token to open the UI
style     = /etc/gander/style.css

[paths]                   # named shortcuts: `gander <name>` opens them
Nexus     = ~/Projects/Nexus
Illution  = ~/Projects/Illution
```

### Named paths

The `[paths]` section maps a short name to a repository, so `gander Nexus`
opens it from anywhere. Names are matched case-insensitively (`gander nexus`
works too) and a leading `~` expands to `$HOME`. A path that exists on disk
still wins, so `gander .` and `gander ./Nexus` behave exactly as before.

### User stylesheet

A `style.css` next to the config file is appended after the bundled
`web/style.css`, so a short override is enough to re-skin the UI — no need to
fork the theme. For example, to square everything off:

```css
/* ~/.config/gander/style.css */
:root { --radius: 0; --radius-sm: 0; --radius-pill: 0; }
```

The bundled theme exposes every colour *and* its corner radii as variables, so
you can change just a few values and leave the rest alone. Use the `style` key
above if your override lives somewhere else.

## Live reload

`gander` watches the previewed directory, debounces bursts of changes (~120 ms),
and pushes them to the page over Server-Sent Events (`/api/events`). The page
refetches the tree and re-renders the current preview when it changes, preserving
your selection, expanded folders and scroll position. Media is cache-busted so an
updated GIF or image actually reloads.

On Linux it uses `inotify`; on every other system it falls back to a portable
scan-and-diff of the tree, so live reload works wherever gander builds. A build
can force either backend with `make WATCH=poll` or `make WATCH=inotify`.

By default it refreshes **only when the git history changes** — a commit, a tag,
a branch switch or a fetch (`.git/HEAD`, `refs/`, `logs/` and friends). Editing
or saving files does not refresh the page, so it stays put while you work.
`--watch all` restores the old behaviour (refresh on every file change);
`--watch off` (or `--no-watch`) turns live reload off entirely.

All git commands run with `GIT_OPTIONAL_LOCKS=0`, so `git status` never rewrites
the index — otherwise that write would be seen as a change and the page would
refresh in a loop.

## CI status

`gander` reads the workflow files in `.github/workflows` from disk and shows
whether CI is configured — never a pass/fail result, which only exists on the
forge and would mean going online. The home card carries a neutral *CI
configured* chip (with the workflow names in its tooltip) or a grey *no CI
configured* one; README badges are shown as chips too, since a remote image
can't be fetched offline.

## Search

Press `/` (or use the search box in the top bar) to search file **contents**,
not just names. The sidebar's filter narrows the file tree; the search box runs
a full-text search over the working tree and lists every hit as `path:line`
with the match highlighted. Clicking a hit opens the file and jumps to the line.

Search is backed by `git grep --untracked -I`, so it respects `.gitignore`,
skips binary files, includes untracked files, and is case-insensitive. Results
are capped (100 by default, 500 max) and the response says when it truncated.
Nothing is indexed and nothing leaves the machine.

## Endpoints

| Route | Purpose |
|---|---|
| `/` | the bundled UI |
| `/api/tree` | `{name, root, is_git, branch, files:[...]}` |
| `/api/meta` | repo metadata: branch, tags, license, CI workflows, languages, last commit |
| `/api/status` | working-tree changes from `git status --porcelain` |
| `/api/log` | recent commits (hash, author, when, subject) |
| `/api/search?q=&limit=` | content search (`git grep`): `{query, truncated, results:[{path, line, col, text}]}` |
| `/api/diff?path=` | a unified diff of one file against `HEAD` |
| `/api/md?path=` | Markdown rendered to an HTML fragment |
| `/api/code?path=` | source highlighted to an HTML code blob |
| `/raw/<path>` | a file from the previewed directory (correct MIME, byte ranges) |
| `/api/events` | Server-Sent Events for live reload |
| `/style.css` | the bundled theme plus your user `style.css`, if any |

## Markdown support

A deliberate subset, not full CommonMark: ATX and setext headings, paragraphs,
fenced code, block quotes, nested ordered/unordered lists, GFM task lists, GFM
tables, horizontal rules, and inline emphasis, strong, strikethrough, code
spans, links, images and autolinks. Relative links and images are rewritten to
`/raw/<path>` so they resolve against the repository. Fenced code blocks are
syntax-highlighted when they name a language we know. Raw HTML is passed
through a safe whitelist (see below).

README badges are usually remote images. Since `gander` never touches the
network, a remote image is shown as a small chip carrying its alt text instead
of a broken image; a linked badge such as `[![CI](badge.svg)](url)` still links
out, and relative images render normally.

Raw HTML is supported too, with a safe whitelist: a centred logo such as
`<p align="center"><img src="web/logo.svg" width="128"></p>`, `<details>`,
`<kbd>` and friends render, while scripts, event handlers and `javascript:`
URLs are dropped. Relative `src`/`href` are rewritten to `/raw/<path>` just
like Markdown links.

## Theme

gander ships a dark theme (GitHub Dark) by default. Every colour *and* corner
radius is a CSS variable at the top of `web/style.css`, so you can re-theme it
by editing that one block; a light palette is included there, commented out.
To re-skin without touching the bundled file, drop a `style.css` in your config
directory — see [Configuration](#configuration).

## Security

- Binds to `127.0.0.1` by default; nothing is exposed to the network.
- Requests are confined to the previewed directory via `realpath` checks.
- A strict `Content-Security-Policy` prevents the page from loading anything
  remote.
- Raw HTML in Markdown is sanitised against a tag/attribute whitelist; scripts,
  event handlers and `javascript:` URLs are removed.
- `--token` gates access behind a cookie for shared machines.
- `gander` makes no outbound network requests at all.

## Roadmap

- **M1** server, file tree, media preview — *done*
- **M2** live reload (inotify → SSE) — *done*
- **M3** Markdown → HTML with inline relative media — *done*
- **M4** code preview, syntax highlighting, line numbers — *done*
- **M5** git panel: status colours, branch, log, per-file diff — *done*
- **M6** repo home card — *done*
- **M7** `--app` chromeless window (Chromium family) — *done*
- **M8** content search over the working tree — *done*
- **M9** `gander(1)` manual page — *done*
- **M10** experimental embedded webview (WebKitGTK, else Qt Web Engine; opt-in) — *done*
- **M11** portable (non-Linux) live-reload backend — *done*

## Contributing

Bug reports, patches and questions are welcome. Start with
[CONTRIBUTING.md](CONTRIBUTING.md) for building and testing, and
[docs/architecture.md](docs/architecture.md) for how the source fits together.
The embedded webview has its own notes in [docs/webview.md](docs/webview.md).

## License

MIT — see [LICENSE](LICENSE). © campbell-nick

The logo is gander's own: `web/logo.svg` (the README header and the SVG
favicon) and the ASCII goose in `src/logo.h` are original artwork by
campbell-nick, covered by the same MIT licence.
