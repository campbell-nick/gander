#!/bin/sh
# tests/install.sh - install-time sample config and user config generation.
set -u
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$here/lib.sh"
setup

root=$(CDPATH= cd -- "$here/.." && pwd)
dest="$TMPROOT/dest"
fakehome="$TMPROOT/home"
prefix="$TMPROOT/prefix"
mkdir -p "$dest" "$fakehome"

# Packaging installs must never write into a user's home.
HOME="$fakehome" XDG_CONFIG_HOME= GANDER_CONFIG= \
  make -C "$root" -s install DESTDIR="$dest" PREFIX=/usr >/dev/null
assert_contains "DESTDIR install ships the sample config" \
  "$(cat "$dest/usr/share/gander/config.example")" "webview   = true"
assert_eq "DESTDIR install skips the user config" 0 \
  "$([ -f "$fakehome/.config/gander/config" ] && printf 1 || printf 0)"

# A normal install creates the user config from the sample.
HOME="$fakehome" XDG_CONFIG_HOME= GANDER_CONFIG= \
  make -C "$root" -s install PREFIX="$prefix" >/dev/null
cfg="$fakehome/.config/gander/config"
assert_eq "first install creates the user config" 0 \
  "$([ -f "$cfg" ] && printf 0 || printf 1)"
assert_contains "generated config has the host setting" \
  "$(cat "$cfg")" "host      = 127.0.0.1"
assert_contains "generated config enables webview mode" \
  "$(cat "$cfg")" "webview   = true"
assert_contains "sample config is installed under the prefix" \
  "$(cat "$prefix/share/gander/config.example")" "watch     = commit"

# Reinstalling must not clobber an existing config.
printf '# keep me\n' >"$cfg"
HOME="$fakehome" XDG_CONFIG_HOME= GANDER_CONFIG= \
  make -C "$root" -s install PREFIX="$prefix" >/dev/null
assert_eq "reinstall leaves an existing config alone" "# keep me" "$(cat "$cfg")"

# XDG_CONFIG_HOME is honoured over ~/.config.
xdg="$TMPROOT/xdg"
HOME="$fakehome" XDG_CONFIG_HOME="$xdg" GANDER_CONFIG= \
  make -C "$root" -s install PREFIX="$prefix" >/dev/null
assert_eq "XDG_CONFIG_HOME is honoured" 0 \
  "$([ -f "$xdg/gander/config" ] && printf 0 || printf 1)"

report install
