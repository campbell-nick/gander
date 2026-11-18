# tests/lib.sh - shared helpers for the gander test suite (POSIX sh).
#
# Each suite sources this, calls `setup`, defines its assertions, and ends with
# `report <name>` (whose exit status is the suite's). The `GANDER_BIN` env var
# selects the binary; otherwise ./gander is assumed (run from the repo root).

TESTS_RUN=0
TESTS_FAILED=0

if [ -t 1 ]; then
  C_OK=$(printf '\033[32m')
  C_BAD=$(printf '\033[31m')
  C_OFF=$(printf '\033[0m')
else
  C_OK=
  C_BAD=
  C_OFF=
fi

pass() {
  TESTS_RUN=$((TESTS_RUN + 1))
  printf '  %s%s%s %s\n' "$C_OK" ok "$C_OFF" "$1"
}

fail() {
  TESTS_RUN=$((TESTS_RUN + 1))
  TESTS_FAILED=$((TESTS_FAILED + 1))
  printf '  %s%s%s %s\n' "$C_BAD" FAIL "$C_OFF" "$1"
  shift
  for line in "$@"; do printf '       %s\n' "$line"; done
}

assert_eq() { # desc expected actual
  if [ "$2" = "$3" ]; then
    pass "$1"
  else
    fail "$1" "expected: $2" "actual:   $3"
  fi
}

assert_contains() { # desc haystack needle
  case "$2" in
    *"$3"*) pass "$1" ;;
    *) fail "$1" "expected to contain: $3" "in: $2" ;;
  esac
}

assert_not_contains() { # desc haystack needle
  case "$2" in
    *"$3"*) fail "$1" "expected NOT to contain: $3" ;;
    *) pass "$1" ;;
  esac
}

assert_refused() { # desc http-code  (a request that must not succeed)
  case "$2" in
    403 | 404) pass "$1" ;;
    *) fail "$1" "expected 403/404, got $2" ;;
  esac
}

report() { # suite name
  printf '\n%s: %d run, %d failed\n' "$1" "$TESTS_RUN" "$TESTS_FAILED"
  [ "$TESTS_FAILED" -eq 0 ]
}

# ---------------------------------------------------------------- fixtures --

GANDER_BIN=${GANDER_BIN:-./gander}
case "$GANDER_BIN" in
  /*) ;;
  *) GANDER_BIN="$PWD/$GANDER_BIN" ;;
esac

TMPROOT=
SERVER_PID=
SERVER_OUT=

cleanup() {
  stop_server
  [ -n "$TMPROOT" ] && rm -rf "$TMPROOT"
}

setup() {
  TMPROOT=$(mktemp -d "${TMPDIR:-/tmp}/gander-test.XXXXXX") || exit 1
  trap cleanup EXIT HUP INT TERM
}

# make_repo <dir>: a tiny git repo with one commit and a tag.
make_repo() {
  _d=$1
  mkdir -p "$_d"
  git -C "$_d" init -q
  git -C "$_d" config user.email test@example.com
  git -C "$_d" config user.name "gander test"
  printf '# hello\n\nthe quick brown fox\n' >"$_d/README.md"
  printf 'int main(void) { return 0; }\n' >"$_d/code.c"
  git -C "$_d" add -A
  git -C "$_d" commit -qm "initial commit"
  git -C "$_d" tag v0.0.1
}

# run_gander [args...]: sets STATUS, OUT (stdout) and ERR (stderr).
run_gander() {
  "$GANDER_BIN" "$@" >"$TMPROOT/out" 2>"$TMPROOT/err"
  STATUS=$?
  OUT=$(cat "$TMPROOT/out")
  ERR=$(cat "$TMPROOT/err")
}

_wait_url() {
  _i=0
  while [ "$_i" -lt 200 ]; do
    grep -q '^  url: ' "$SERVER_OUT" 2>/dev/null && return 0
    kill -0 "$SERVER_PID" 2>/dev/null || return 1
    sleep 0.05
    _i=$((_i + 1))
  done
  return 1
}

# start_server_in <workdir> <target> [extra args...]
start_server_in() {
  _wd=$1
  shift
  SERVER_OUT=$(mktemp "$TMPROOT/server.XXXXXX")
  (cd "$_wd" && exec "$GANDER_BIN" "$@" --no-open --port 0) >"$SERVER_OUT" 2>&1 &
  SERVER_PID=$!
  _wait_url
}

start_server() { start_server_in "$PWD" "$@"; }

stop_server() {
  if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill "$SERVER_PID" 2>/dev/null
    wait "$SERVER_PID" 2>/dev/null
  fi
  SERVER_PID=
}

server_url() { sed -n 's/^  url: *//p' "$SERVER_OUT" | head -1; }
# The banner masks a token as "?token=***"; keep only scheme://host:port.
server_base() { server_url | sed -e 's:?.*$::' -e 's:/$::'; }
