#!/bin/sh
# First-run setup for the Nix-installed gander: the build cannot write into
# $HOME, so copy the bundled sample config the first time gander is run.
cfg="${XDG_CONFIG_HOME:-$HOME/.config}/gander/config"
if [ ! -e "$cfg" ]; then
  root="$(dirname "$(dirname "$0")")"
  mkdir -p "$(dirname "$cfg")" || exit 1
  cp "$root/share/gander/config.example" "$cfg"
fi
