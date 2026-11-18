#!/bin/sh
# tests/cli.sh - command-line behaviour: flags, errors, target resolution.
set -u
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$here/lib.sh"

setup

version=$(sed -n 's/^VERSION := //p' Makefile)

repo="$TMPROOT/repo"
make_repo "$repo"
real_repo=$(cd "$repo" && pwd -P)

# --- informational flags ------------------------------------------------------

run_gander --version
assert_eq "--version exits 0" 0 "$STATUS"
assert_contains "--version names gander" "$OUT" "gander $version"
assert_contains "--version shows the tagline" "$OUT" "offline previewer"

run_gander --help
assert_eq "--help exits 0" 0 "$STATUS"
assert_contains "--help shows Usage" "$OUT" "Usage:"

run_gander
assert_eq "bare gander exits 0" 0 "$STATUS"
assert_contains "bare gander shows help" "$OUT" "Usage:"

run_gander --port 8080
assert_eq "options without a target exit 0" 0 "$STATUS"
assert_contains "options without a target show help" "$OUT" "Usage:"

# --- errors -------------------------------------------------------------------

run_gander --bogus
assert_eq "unknown option exits 2" 2 "$STATUS"
assert_contains "unknown option is reported" "$ERR" "unknown option"

mkdir -p "$TMPROOT/plain"
run_gander "$TMPROOT/plain"
assert_eq "non-repo exits 1" 1 "$STATUS"
assert_contains "non-repo prints the goose line" "$ERR" "This isn't a grain, it's a bread!"
assert_contains "non-repo gives the reason" "$ERR" "not a git repository:"
assert_eq "non-repo starts no server" "" "$OUT"

run_gander "$TMPROOT/nope"
assert_eq "missing dir exits 1" 1 "$STATUS"
assert_contains "missing dir is reported" "$ERR" "no such directory"

# --- repository detection -----------------------------------------------------

# A .git *file* (a linked worktree) counts as a repository.
linked="$TMPROOT/linked"
mkdir -p "$linked"
printf 'gitdir: %s\n' "$TMPROOT/nowhere" >"$linked/.git"
if start_server "$linked"; then
  assert_contains ".git file counts as a repository" "$(cat "$SERVER_OUT")" "root:"
else
  fail ".git file counts as a repository" "server did not start"
fi
stop_server

# --- [paths] shortcuts --------------------------------------------------------

cfg="$TMPROOT/config"
export GANDER_CONFIG="$cfg"

printf '[paths]\nNexus = %s\n' "$repo" >"$cfg"
if start_server Nexus; then
  assert_contains "alias resolves (exact case)" "$(cat "$SERVER_OUT")" "root:   $real_repo"
else
  fail "alias resolves (exact case)" "server did not start"
fi
stop_server

if start_server nexus; then
  assert_contains "alias resolves (lower case)" "$(cat "$SERVER_OUT")" "root:   $real_repo"
else
  fail "alias resolves (lower case)" "server did not start"
fi
stop_server

# A leading ~ expands to $HOME.
old_home=$HOME
HOME="$TMPROOT/home"
export HOME
mkdir -p "$HOME"
make_repo "$HOME/proj"
printf '[paths]\ntilde = ~/proj\n' >"$cfg"
if start_server tilde; then
  assert_contains "alias expands ~ to \$HOME" "$(cat "$SERVER_OUT")" "root:   $HOME/proj"
else
  fail "alias expands ~ to \$HOME" "server did not start"
fi
stop_server
HOME=$old_home
export HOME

# An alias pointing at a missing directory is a clean error.
printf '[paths]\nBroken = %s\n' "$TMPROOT/nope" >"$cfg"
run_gander Broken
assert_eq "alias to missing dir exits 1" 1 "$STATUS"
assert_contains "alias to missing dir is reported" "$ERR" "is not a directory"

# A directory on disk wins over an alias of the same name (./web exists here).
printf '[paths]\nweb = %s\n' "$repo" >"$cfg"
run_gander web
assert_contains "existing directory wins over alias" "$ERR" "This isn't a grain"

unset GANDER_CONFIG

# --- current ------------------------------------------------------------------

if start_server_in "$repo" current; then
  assert_contains "current previews the working directory" "$(cat "$SERVER_OUT")" "root:   $real_repo"
else
  fail "current previews the working directory" "server did not start"
fi
stop_server

# --- --app --------------------------------------------------------------------

run_gander --help
assert_contains "--help lists --app" "$OUT" "--app"

# --app changes only how the UI is launched, so the server still comes up.
if start_server_in "$repo" . --app; then
  assert_contains "--app still serves the UI" "$(cat "$SERVER_OUT")" "root:   $real_repo"
else
  fail "--app still serves the UI" "server did not start"
fi
stop_server

# The app = true config key is accepted.
cfg2="$TMPROOT/config-app"
printf 'app = true\n' >"$cfg2"
run_gander --config "$cfg2" --help
assert_eq "config 'app = true' is accepted" 0 "$STATUS"

# --- --webview ----------------------------------------------------------------

run_gander --help
assert_contains "--help lists --webview" "$OUT" "--webview"

# The default build has no embedded webview: --webview says so and still serves.
SERVER_OUT=$(mktemp "$TMPROOT/server.XXXXXX")
(cd "$repo" && exec "$GANDER_BIN" . --webview --no-open --port 0) >"$SERVER_OUT" 2>&1 &
SERVER_PID=$!
if _wait_url; then
  assert_contains "--webview without a webview build explains itself" \
    "$(cat "$SERVER_OUT")" "no embedded webview"
  assert_contains "--webview still serves the UI" \
    "$(cat "$SERVER_OUT")" "root:   $real_repo"
else
  fail "--webview still serves the UI" "server did not start"
fi
stop_server

# The webview = true config key is accepted.
cfg3="$TMPROOT/config-webview"
printf 'webview = true\n' >"$cfg3"
run_gander --config "$cfg3" --help
assert_eq "config 'webview = true' is accepted" 0 "$STATUS"

# --- manual page --------------------------------------------------------------

if [ -f man/gander.1 ]; then
  assert_contains "man page names gander" "$(cat man/gander.1)" ".TH GANDER 1"
  assert_contains "man page documents --app" "$(cat man/gander.1)" "chromeless"
  assert_contains "man page documents --webview" "$(cat man/gander.1)" "embedded webview"
else
  fail "man page exists" "man/gander.1 not found (run make check from the repo root)"
fi

report cli
