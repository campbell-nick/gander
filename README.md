<p align="center"><img src="web/logo.svg" width="128" height="128" alt="gander logo"></p>

# gander

**Take a gander at your repo.** An offline previewer for a local folder or git
repository: a file tree on the left, live previews on the right — Markdown,
images, animated GIFs, video, PDFs and source code — that refresh the moment
the files on disk change.

Everything is local. `gander` is a small, dependency-free C program that serves
a bundled web UI on `127.0.0.1` and opens it in your browser. No network by
default, no CDN, no telemetry — the one opt-in exception is `--ci-status`.

```
gander [path]
```

## Why

Most "preview this folder" tools either need a build step, a runtime, or the
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
- **Repo home card**: branch, latest tag (as the release), CI status, license,
  commit and file counts, a language breakdown, the last commit and the
  rendered README below — all read from disk, never the network (unless you opt
  into `--ci-status`).
- **Git panel**: working-tree changes grouped as staged/changes/untracked with
  status colours in the tree, a recent-commit log, and a per-file unified diff.
- **Live reload**: refresh only when you commit (or switch branch/tag/fetch),
  keeping your place; `--watch all` also refreshes as you edit files.
- **Offline by construction**: bound to `127.0.0.1`, strict CSP, all assets
  bundled. The only thing that may touch the network is the opt-in
  `--ci-status` flag.

## Build

Requires a C11 compiler and a POSIX system (Linux for live reload). No
dependencies beyond libc.

```sh
make
./gander .
```

Install (binary + UI assets):

```sh
sudo make install PREFIX=/usr/local
```

## Usage

```
gander [path] [options]

  --port <n>        port to listen on (default: pick a free one)
  --host <addr>     address to bind (default: 127.0.0.1)
  --web-root <dir>  directory holding the bundled UI assets
  --token <str>     require this token to open the UI
  --config <file>   read settings from this file instead of the default
  --no-open         do not launch a browser
  --watch <mode>    what live reload reacts to: commit (default), all or off
  --no-watch        disable live reload (same as --watch off)
  --ci-status       show real CI pass/fail via the `gh` CLI (uses the network)
  --no-ci-status    keep gander fully offline (default)
  -h, --help        show help and exit
  -V, --version     show the version and exit
```

The UI assets are found via, in order: `--web-root`, `$GANDER_WEB`,
`<exe-dir>/web`, `<exe-dir>/../share/gander/web`, then `./web`.

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
watch     = commit        # commit (default), all or off
ci-status = false         # opt in to the network `gh` lookup
web-root  = /usr/share/gander/web
token     = hunter2       # require this token to open the UI
style     = /etc/gander/style.css
```

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

`gander` watches the previewed directory with `inotify`, debounces bursts of
changes (~120 ms), and pushes them to the page over Server-Sent Events
(`/api/events`). The page refetches the tree and re-renders the current preview
when it changes, preserving your selection, expanded folders and scroll
position. Media is cache-busted so an updated GIF or image actually reloads.

By default it refreshes **only when the git history changes** — a commit, a tag,
a branch switch or a fetch (`.git/HEAD`, `refs/`, `logs/` and friends). Editing
or saving files does not refresh the page, so it stays put while you work.
`--watch all` restores the old behaviour (refresh on every file change);
`--watch off` (or `--no-watch`) turns live reload off entirely. In a plain
folder with no repository there is nothing to commit, so `commit` mode falls
back to watching files.

All git commands run with `GIT_OPTIONAL_LOCKS=0`, so `git status` never rewrites
the index — otherwise that write would be seen as a change and the page would
refresh in a loop.

Live reload is Linux-only for now; on other platforms `gander` still serves
everything, just without automatic refresh.

## CI status

Real pass/fail only exists on the forge, so by default `gander` does **not**
claim one — it just notes that CI is configured. Pass `--ci-status` to opt in,
and `gander` will ask the authenticated [`gh` CLI](https://cli.github.com/) for
the latest run of each workflow on the default branch. The home card then shows
a green *passing*, red *failing* or yellow *running* chip, and README CI badges
are upgraded from a neutral chip to the same pass/fail wording.

Status is fetched once at startup and again on every commit; if `gh` is missing,
not logged in, or offline, it falls back to `unknown` and `gander` keeps
working. Without the flag, `gander` never touches the network.

## Endpoints

| Route | Purpose |
|---|---|
| `/` | the bundled UI |
| `/api/tree` | `{name, root, is_git, branch, files:[...]}` |
| `/api/meta` | repo metadata: branch, tags, license, CI, languages, last commit, `ci_status`/`ci_workflows` |
| `/api/status` | working-tree changes from `git status --porcelain` |
| `/api/log` | recent commits (hash, author, when, subject) |
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
out, and relative images render normally. With `--ci-status`, a badge whose URL
names a known workflow is shown in the workflow's pass/fail colour (see above).

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
- The only outbound network request is the opt-in `--ci-status`, which shells
  out to the authenticated `gh` CLI; everything else stays local.

## Roadmap

- **M1** server, file tree, media preview — *done*
- **M2** live reload (inotify → SSE) — *done*
- **M3** Markdown → HTML with inline relative media — *done*
- **M4** code preview, syntax highlighting, line numbers — *done*
- **M5** git panel: status colours, branch, log, per-file diff — *done*
- **M6** repo home card — *done*; search, themes, `--app` webview window

## License

MIT — see [LICENSE](LICENSE). © campbell-nick
