# gander architecture

gander is one small C program that serves a bundled web UI for a local git
repository. It has no dependencies beyond libc, binds to `127.0.0.1`, and
contains no network code: everything it shows is read from disk (and from the
local `git` CLI).

This page is a map of the source. For building and testing, see
[CONTRIBUTING.md](../CONTRIBUTING.md); for the opt-in window, see
[webview.md](webview.md).

## The shape of a run

```
argv ─▶ main.c ──▶ config.c ──▶ server_run()  (server.c)
                                 │
                                 ├─ web/  (bundled UI: index.html, app.js, style.css, logo.svg)
                                 ├─ repo.c ──▶ local git CLI + filesystem
                                 └─ watch.c (inotify/poll) ──▶ SSE ──▶ app.js
```

1. `main.c` parses the command line, resolves the target (`path`, a `[paths]`
   name, or `current`), reads the config file, and checks that the target is a
   git repository.
2. `server_run()` binds `127.0.0.1`, resolves the web root, prints the startup
   banner, installs signal handlers, and runs the event loop.
3. The loop serves the UI and its JSON endpoints, watches the repository, and
   pushes live-reload events to the page.

## Modules

| File | Role |
|---|---|
| `main.c` | Command-line parsing, target resolution, `--help` / `--version`, then calls `server_run()`. |
| `config.c` | The optional config file (`~/.config/gander/config`), its `[paths]` shortcuts, and the user `style.css`. |
| `server.c` | The HTTP server: a single-threaded `poll()` loop, routing, static assets, `/raw`, and Server-Sent Events. |
| `repo.c` | Repository metadata, entirely from disk: tree, status, log, diff, search, tags, languages, CI, license, README. |
| `walk.c` | Collect the file tree of the previewed directory. |
| `watch.c` | Live-reload watcher with debounce: `inotify` on Linux, a portable scan-and-diff (`WATCH=poll`) elsewhere. |
| `md.c` | The Markdown subset → HTML, with relative links/images rewritten to `/raw/<path>`. |
| `html.c` | A safe renderer for the raw HTML that READMEs embed (tag/attribute whitelist). |
| `highlight.c` | Source code → HTML spans, GitHub-style, for a handful of languages. |
| `util.c` | Helpers: `xmalloc`, a string buffer, JSON escaping, MIME types. |
| `webview.c` | The embedded webview's GTK backend, plus the inert stubs the default build compiles. |
| `webview_qt.cpp` | The embedded webview's Qt Web Engine backend (the only C++ unit). |
| `logo.h` | The ASCII goose printed by `--version`. |
| `gander.h` | Shared declarations for everything above. |
| `web/` | The bundled UI served to the browser: `index.html`, `app.js`, `style.css`, `logo.svg`. |

## The server loop

Everything runs on **one thread**. `server_step()` builds a `pollfd` set — the
listening socket, the watcher's fd (when the backend is event-driven), and one
entry per connection — and waits up to 40 ms. When it wakes it accepts new
connections, reads and answers requests, drains the watcher, polls the watcher
again (a no-op for `inotify`, a periodic scan for the polling backend), and
reaps closed connections.

Each connection is a small state machine:

- `C_READ` — accumulating a request until the header terminator arrives.
- `C_WRITE` — a response is queued in the connection's output buffer; written
  out on the next writable poll.
- `C_SSE` — a live-reload stream, kept open and written to whenever a change is
  broadcast.

A request flows `conn_read()` → `handle_request()` → (token gate) → a route
handler → response buffered into `c->out` → `conn_write()`.

## Routes

| Route | Handler |
|---|---|
| `/`, `/index.html`, `/app.js`, `/style.css`, `/logo.svg` | bundled assets from `web/` |
| `/api/tree`, `/api/meta`, `/api/status`, `/api/log` | repository facts from `repo.c` |
| `/api/search`, `/api/diff`, `/api/md`, `/api/code` | search, per-file diff, Markdown, highlighted code |
| `/raw/<path>` | a file from the previewed directory, with correct MIME and byte ranges |
| `/api/events` | Server-Sent Events for live reload |

Requests are confined to the previewed directory with `realpath` checks, and a
strict `Content-Security-Policy` stops the page loading anything remote.

## Live reload

`watch.c` has two backends behind one API. On Linux (or with
`-DGANDER_WATCH_INOTIFY`) it uses `inotify`: the kernel reports events on a fd,
`server.c` adds that fd to its `poll()` set, and `watch_drain()` reads them. On
every other system (or with `-DGANDER_WATCH_POLL`) it uses a portable polling
backend: `watch_fd()` returns `-1` and `server.c` calls `watch_poll()` each turn,
which scans the tree every `POLL_INTERVAL_MS` (500 ms) and diffs it against the
previous snapshot. The Makefile's `WATCH` knob selects the backend; the banner
names the one compiled in.

`--watch commit` (the default) reacts only to git-history changes; `--watch all`
reacts to any file change. Events are debounced (`DEBOUNCE_MS`, 120 ms) and then
drained with `watch_take()`, which `server.c` turns into an `event: change` SSE
frame for every `C_SSE` connection. The page (`web/app.js`) refetches the tree
and re-renders the current preview, keeping the selection and scroll position.

All git commands run with `GIT_OPTIONAL_LOCKS=0` so `git status` never rewrites
the index — otherwise that write would be seen as a change and the page would
refresh in a loop.

## The embedded webview

The window is an optional, opt-in layer (see [webview.md](webview.md)). Rather
than pumping toolkit events from the server's `poll()` loop, gander hands the
loop to the toolkit: `webview_run()` runs Qt's `exec()` or GTK's `gtk_main()`,
and a 4 ms timer calls back into `server_tick()`, which does a non-blocking
`server_step()`. That keeps input and rendering responsive while the sockets
keep moving on the same thread. The default build compiles inert stubs and never
links a toolkit.

## Build variants

- **Default** (`make`): libc only. `webview.c` compiles its inert stubs.
- **`make WEBVIEW=1`**: auto-selects WebKitGTK (`gtk+-3.0`, `webkit2gtk-4.1`)
  or, failing that, Qt Web Engine (`Qt6WebEngineWidgets`).
- **`make WEBVIEW=gtk` / `WEBVIEW=qt`**: force one backend.

The Qt backend links with `$(CXX)`; the Makefile always rebuilds the webview
glue so switching backends cannot reuse a stale object.

## Tests

`make check` runs the POSIX-sh suite in `tests/` (`cli`, `http`, `md`). Each
suite sources `tests/lib.sh`, builds throwaway git repositories under `$TMPDIR`,
and starts a real server on a free port. See
[CONTRIBUTING.md](../CONTRIBUTING.md#tests) for how to add a case.
