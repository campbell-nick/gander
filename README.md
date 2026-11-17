# gander

**Take a gander at your repo.** An offline previewer for a local folder or git
repository: a file tree on the left, live previews on the right — Markdown,
images, animated GIFs, video, PDFs and source code — that refresh the moment
the files on disk change.

Everything is local. `gander` is a small, dependency-free C program that serves
a bundled web UI on `127.0.0.1` and opens it in your browser. No network, no
CDN, no telemetry, ever.

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
- **Repo home card**: branch, latest tag (as the release), CI config, license,
  commit and file counts, a language breakdown and the last commit — all read
  from disk, never the network.
- **Git panel**: working-tree changes grouped as staged/changes/untracked with
  status colours in the tree, a recent-commit log, and a per-file unified diff.
- **Live reload**: edits on disk update the page instantly, keeping your place.
- **Offline by construction**: bound to `127.0.0.1`, strict CSP, all assets
  bundled.

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
  --no-open         do not launch a browser
  --no-watch        disable live reload
  -h, --help        show help and exit
  -V, --version     show the version and exit
```

The UI assets are found via, in order: `--web-root`, `$GANDER_WEB`,
`<exe-dir>/web`, `<exe-dir>/../share/gander/web`, then `./web`.

## Live reload

`gander` watches the previewed directory recursively with `inotify`, debounces
bursts of changes (~120 ms), and pushes them to the page over Server-Sent
Events (`/api/events`). The page refetches the tree and re-renders the current
preview when it changes, preserving your selection, expanded folders and scroll
position. Media is cache-busted so an updated GIF or image actually reloads.

Live reload is Linux-only for now; on other platforms `gander` still serves
everything, just without automatic refresh. Use `--no-watch` to disable it.

## Endpoints

| Route | Purpose |
|---|---|
| `/` | the bundled UI |
| `/api/tree` | `{name, root, is_git, branch, files:[...]}` |
| `/api/meta` | repo metadata: branch, tags, license, CI, languages, last commit |
| `/api/status` | working-tree changes from `git status --porcelain` |
| `/api/log` | recent commits (hash, author, when, subject) |
| `/api/diff?path=` | a unified diff of one file against `HEAD` |
| `/api/md?path=` | Markdown rendered to an HTML fragment |
| `/api/code?path=` | source highlighted to an HTML code blob |
| `/raw/<path>` | a file from the previewed directory (correct MIME, byte ranges) |
| `/api/events` | Server-Sent Events for live reload |

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
`<p align="center"><img src="logo.svg" width="128"></p>`, `<details>`,
`<kbd>` and friends render, while scripts, event handlers and `javascript:`
URLs are dropped. Relative `src`/`href` are rewritten to `/raw/<path>` just
like Markdown links.

## Theme

gander ships a dark theme (GitHub Dark) by default. Every colour is a CSS
variable at the top of `web/style.css`, so you can re-theme it by editing that
one block; a light palette is included there, commented out.

## Security

- Binds to `127.0.0.1` by default; nothing is exposed to the network.
- Requests are confined to the previewed directory via `realpath` checks.
- A strict `Content-Security-Policy` prevents the page from loading anything
  remote.
- Raw HTML in Markdown is sanitised against a tag/attribute whitelist; scripts,
  event handlers and `javascript:` URLs are removed.
- `--token` gates access behind a cookie for shared machines.

## Roadmap

- **M1** server, file tree, media preview — *done*
- **M2** live reload (inotify → SSE) — *done*
- **M3** Markdown → HTML with inline relative media — *done*
- **M4** code preview, syntax highlighting, line numbers — *done*
- **M5** git panel: status colours, branch, log, per-file diff — *done*
- **M6** repo home card — *done*; search, themes, `--app` webview window

## License

MIT — see [LICENSE](LICENSE). © campbell-nick
