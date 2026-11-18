# The embedded webview (experimental)

By default gander serves its UI on `127.0.0.1` and opens it in your browser.
The **embedded webview** is an opt-in alternative: gander shows the same UI in
its own window, using a real browser engine, and stops when you close that
window.

It is experimental. The default build stays libc-only; a toolkit is only linked
when you ask for it.

## Building

```sh
make WEBVIEW=1
./gander . --webview
```

`WEBVIEW=1` picks a backend automatically:

1. **WebKitGTK** — the `gtk+-3.0` and `webkit2gtk-4.1` development packages.
2. **Qt Web Engine** — `Qt6WebEngineWidgets` (Gentoo: `dev-qt/qtwebengine`,
   Debian/Ubuntu: `qt6-webengine-dev`), used when GTK is not present.

Force one with `make WEBVIEW=gtk` or `make WEBVIEW=qt`. If neither is installed,
`make` says so and stops rather than building a broken binary.

The Qt backend is the only C++ translation unit in the project, so it is linked
with `$(CXX)`; the default build never compiles it.

## Using it

`--webview` shows the UI in an embedded window instead of a browser, and the
window's title is `gander` with the bundled logo as its icon. Closing the window
stops the server. Set `webview = true` in the config file to make it the default.

Without a `WEBVIEW=1` build, `--webview` prints a note and opens a browser
instead, so the flag is always safe to pass.

### `--webview` or `--app`?

- `--app` needs **no extra toolkit**: it launches a Chromium-family browser
  already on your `PATH` with `--app=` and a throwaway profile. The window is
  chromeless but it is still your browser.
- `--webview` embeds a browser engine in gander itself. It needs a build with
  `WEBVIEW=1`, but the window belongs to gander and the server stops when you
  close it.

If you would rather not build an engine, use `--app`.

## How it stays responsive

The webview must not stall the server, and the server must not stall the
webview, yet both share one thread. gander resolves this by giving the toolkit
the event loop while the window is open:

- `webview_run()` runs Qt's `exec()` or GTK's `gtk_main()`, so input and
  rendering are handled the moment they arrive.
- A 4 ms timer calls back into `server_tick()`, which does a **non-blocking**
  turn of the server loop (`server_step()` with a zero timeout) to service the
  sockets.

The opposite approach — pumping toolkit events from the server's `poll()` loop —
makes every click wait for the next poll (up to the 40 ms timeout, and longer
while a `git` call runs). Handing over the loop trades a few milliseconds of
request latency and roughly 1% idle CPU for immediate input.

On the author's machine, with the UI idle:

- rendering holds a steady 60 fps (16.67 ms per frame, no dropped frames);
- click-to-JS latency dropped from a 7 ms median / 20 ms worst case to about
  1 ms.

Those numbers are machine-specific; treat them as a shape, not a promise.

## Troubleshooting

- **`--webview` opens a browser tab** — the binary has no backend. Rebuild with
  `make WEBVIEW=1` and the WebKitGTK or Qt Web Engine development packages.
- **`make WEBVIEW=1` stops with "found neither"** — install one of the two
  backends, or force the one you have with `make WEBVIEW=gtk` / `WEBVIEW=qt`.
- **The window does not appear (no display)** — a headless session has no X11 or
  Wayland display to open a window on; gander says so and falls back to a
  browser.
- **The window icon is missing** — the bundled `logo.svg` is loaded at runtime
  and needs an SVG loader (the Qt SVG plugin, or the `gdk-pixbuf` SVG loader on
  GTK). A missing loader just keeps the toolkit's default icon.

## Continuous integration

CI builds the webview in two experimental, allowed-to-fail jobs — one with
WebKitGTK, one with Qt Web Engine — since not every environment has the engines.
They prove the opt-in builds compile; they do not run the GUI.
