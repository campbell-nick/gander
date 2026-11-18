#!/bin/sh
# tests/http.sh - endpoint smoke tests and security negatives.
set -u
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$here/lib.sh"

if ! command -v curl >/dev/null 2>&1; then
  echo "curl not found; skipping HTTP tests"
  exit 0
fi

setup

repo="$TMPROOT/repo"
make_repo "$repo"

if ! start_server "$repo"; then
  echo "server did not start"
  cat "$SERVER_OUT"
  exit 1
fi
base=$(server_base)

# --- the UI and its assets ----------------------------------------------------

hdr=$(curl -s -D - -o "$TMPROOT/body" "$base/")
assert_contains "/ serves html" "$hdr" "200"
assert_contains "/ is text/html" "$hdr" "Content-Type: text/html"
assert_contains "/ serves the app shell" "$(cat "$TMPROOT/body")" "<title>gander</title>"

code=$(curl -s -o /dev/null -w '%{http_code}' "$base/app.js")
assert_eq "/app.js is 200" 200 "$code"
code=$(curl -s -o /dev/null -w '%{http_code}' "$base/style.css")
assert_eq "/style.css is 200" 200 "$code"
code=$(curl -s -o /dev/null -w '%{http_code}' "$base/logo.svg")
assert_eq "/logo.svg is 200" 200 "$code"

# --- JSON endpoints -----------------------------------------------------------

for ep in tree meta status log; do
  code=$(curl -s -o "$TMPROOT/body" -w '%{http_code}' "$base/api/$ep")
  assert_eq "/api/$ep is 200" 200 "$code"
  if command -v python3 >/dev/null 2>&1; then
    if python3 -c 'import json,sys; json.load(open(sys.argv[1]))' "$TMPROOT/body" 2>/dev/null; then
      pass "/api/$ep is valid JSON"
    else
      fail "/api/$ep is valid JSON" "$(head -c 200 "$TMPROOT/body")"
    fi
  else
    assert_contains "/api/$ep looks like JSON" "$(cat "$TMPROOT/body")" "{"
  fi
done

# --- rendering and raw files --------------------------------------------------

code=$(curl -s -o "$TMPROOT/body" -w '%{http_code}' "$base/api/md?path=README.md")
assert_eq "/api/md is 200" 200 "$code"
assert_contains "/api/md renders a heading" "$(cat "$TMPROOT/body")" "<h1"

code=$(curl -s -o "$TMPROOT/body" -w '%{http_code}' "$base/raw/README.md")
assert_eq "/raw is 200" 200 "$code"
assert_contains "/raw serves the file bytes" "$(cat "$TMPROOT/body")" "quick brown fox"

# --- content search -----------------------------------------------------------

printf 'find ME_PLEASE in here\n' >"$repo/needle.txt"
printf 'also ME_PLEASE here\n' >>"$repo/README.md"

code=$(curl -s -o "$TMPROOT/body" -w '%{http_code}' "$base/api/search?q=ME_PLEASE")
assert_eq "/api/search is 200" 200 "$code"
body=$(cat "$TMPROOT/body")
assert_contains "search finds tracked matches" "$body" "README.md"
assert_contains "search finds untracked matches" "$body" "needle.txt"
assert_contains "search echoes the query" "$body" '"query":"ME_PLEASE"'
if command -v python3 >/dev/null 2>&1; then
  if python3 -c 'import json,sys; json.load(open(sys.argv[1]))' "$TMPROOT/body" 2>/dev/null; then
    pass "/api/search is valid JSON"
  else
    fail "/api/search is valid JSON" "$body"
  fi

  code=$(curl -s -o "$TMPROOT/body" -w '%{http_code}' "$base/api/search?q=ME_PLEASE&limit=1")
  n=$(python3 -c 'import json,sys; print(len(json.load(open(sys.argv[1]))["results"]))' "$TMPROOT/body")
  assert_eq "search honours limit" 1 "$n"
  t=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["truncated"])' "$TMPROOT/body")
  assert_eq "search reports truncation" "True" "$t"
fi

code=$(curl -s -o "$TMPROOT/body" -w '%{http_code}' "$base/api/search?q=zzz_no_such_needle")
assert_eq "search with no matches is 200" 200 "$code"
assert_contains "search with no matches is empty" "$(cat "$TMPROOT/body")" '"results":[]'

code=$(curl -s -o /dev/null -w '%{http_code}' "$base/api/search")
assert_eq "search without q is 400" 400 "$code"

# --- errors and confinement ---------------------------------------------------

code=$(curl -s -o /dev/null -w '%{http_code}' "$base/nope")
assert_eq "unknown route is 404" 404 "$code"

code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$base/api/tree")
assert_eq "POST is refused" 405 "$code"

code=$(curl -s --path-as-is -o /dev/null -w '%{http_code}' "$base/raw/../README.md")
assert_refused "traversal via .. is refused" "$code"

code=$(curl -s --path-as-is -o /dev/null -w '%{http_code}' "$base/raw/%2e%2e/%2e%2e/etc/passwd")
assert_refused "traversal via %2e%2e is refused" "$code"

stop_server

# --- token gate ---------------------------------------------------------------

if ! start_server "$repo" --token s3cret; then
  echo "token server did not start"
  exit 1
fi
base=$(server_base)

code=$(curl -s -o /dev/null -w '%{http_code}' "$base/")
assert_eq "token: / without a token is 403" 403 "$code"
code=$(curl -s -o /dev/null -w '%{http_code}' "$base/?token=s3cret")
assert_eq "token: / with the token is 200" 200 "$code"
code=$(curl -s -o /dev/null -w '%{http_code}' "$base/api/tree?token=s3cret")
assert_eq "token: api with the token is 200" 200 "$code"
code=$(curl -s -o /dev/null -w '%{http_code}' "$base/api/tree?token=wrong")
assert_eq "token: api with a wrong token is 403" 403 "$code"

stop_server

report http
