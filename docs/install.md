# Installing gander

`gander` is a single binary plus a handful of bundled UI assets. There is
nothing to fetch at runtime and no dependencies beyond libc.

## Requirements

- A C11 compiler (`cc`, `gcc` or `clang`).
- A POSIX system. Live reload uses `inotify` on Linux and a portable
  scan-and-diff fallback elsewhere, so it works everywhere gander builds.
- `make` (optional — see the one-liner below).

## Build

```sh
make
```

That produces `./gander`. You can run it straight from the build directory,
with no install at all — the UI assets are found at `./web`:

```sh
./gander .
```

Or without `make`:

```sh
cc -O2 -std=c11 -D_GNU_SOURCE -o gander src/*.c
```

## Install

`make install` copies the binary to `$(PREFIX)/bin/gander`, the UI assets to
`$(PREFIX)/share/gander/web/` (which the binary locates automatically at
runtime — see [How the assets are found](#how-the-assets-are-found)), and the
manual page to `$(PREFIX)/share/man/man1/gander.1`, and a sample config to
`$(PREFIX)/share/gander/config.example`.

If no config exists yet, `make install` also copies that sample to
`$XDG_CONFIG_HOME/gander/config` (or `~/.config/gander/config`). It never
overwrites an existing config, and skips this step entirely when `DESTDIR` is
set or `GANDER_CONFIG` points somewhere else.

`PREFIX` defaults to `/usr/local`. There are two sensible choices:

### Per user (recommended)

No root needed, nothing outside your home, easy to remove:

```sh
make
make install PREFIX=$HOME/.local
```

This gives you `~/.local/bin/gander`. Make sure `~/.local/bin` is on your
`PATH` — most distributions add it automatically once it exists; otherwise add
this to your shell rc:

```sh
export PATH="$HOME/.local/bin:$PATH"
```

A per-user install also lines up with the per-user config directory
(`~/.config/gander`).

### System-wide

Use this when every account on the machine should have gander:

```sh
make
sudo make install PREFIX=/usr/local
```

**Build first, then `sudo`.** `sudo make install` in a fresh tree builds as
root and leaves root-owned `*.o` files and `gander` behind, so your next plain
`make` fails with permission errors. The recipe above avoids that by running
`make` as yourself.

### Packaging / staging

To install into a package root instead of the live filesystem, use `DESTDIR`:

```sh
make
make install DESTDIR="$pkgdir" PREFIX=/usr
```

This writes to `$pkgdir/usr/bin/gander`, `$pkgdir/usr/share/gander/web/` and
`$pkgdir/usr/share/man/man1/gander.1`.

## Verify

```sh
gander --version
```

Then run it against a git repository and open the printed URL:

```sh
gander ~/some/project
```

The startup banner shows the resolved root, URL and (if present) the user
`style.css` it picked up.

## How the assets are found

At startup the web root is resolved from the first of these that exists:

1. `--web-root <dir>`
2. `$GANDER_WEB`
3. `<exe-dir>/web`
4. `<exe-dir>/../share/gander/web`  ← what `make install` sets up
5. `./web`

So a build tree works in place, and an installed binary finds its assets next
to it.

## Uninstall

```sh
# per-user
make uninstall PREFIX=$HOME/.local

# system-wide
sudo make uninstall PREFIX=/usr/local
```

## Troubleshooting

- **`gander: warning: web root ... not found`** — the UI assets aren't where
  the binary expects. Either reinstall (so `share/gander/web` sits beside
  `bin/`), run from the source tree (so `./web` exists), or point at them with
  `--web-root` / `$GANDER_WEB`.
- **`command not found: gander`** — `$PREFIX/bin` isn't on your `PATH`; see the
  per-user section above.
- **Permission denied when rebuilding after a `sudo make install`** — the
  previous install left root-owned build artefacts. Remove them and rebuild:
  `sudo make clean && make`.
