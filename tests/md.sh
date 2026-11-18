#!/bin/sh
# tests/md.sh - the Markdown subset: headings, tables, task lists, rewrites.
set -u
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$here/lib.sh"

if ! command -v curl >/dev/null 2>&1; then
  echo "curl not found; skipping markdown tests"
  exit 0
fi

setup

repo="$TMPROOT/repo"
make_repo "$repo"

cat >"$repo/doc.md" <<'EOF'
# Title

Some **bold** text and `code`.

| a | b |
|---|---|
| 1 | 2 |

- [x] done
- [ ] todo

![pic](img/pic.png)

<script>alert(1)</script>
EOF

if ! start_server "$repo"; then
  echo "server did not start"
  cat "$SERVER_OUT"
  exit 1
fi
base=$(server_base)

code=$(curl -s -o "$TMPROOT/body" -w '%{http_code}' "$base/api/md?path=doc.md")
body=$(cat "$TMPROOT/body")

assert_eq "/api/md is 200" 200 "$code"
assert_contains "heading renders" "$body" "<h1>"
assert_contains "strong renders" "$body" "<strong>"
assert_contains "table renders" "$body" "<table><thead>"
assert_contains "task list renders" "$body" "type=\"checkbox\""
assert_contains "relative image is rewritten" "$body" "/raw/img/pic.png"
assert_not_contains "raw script is dropped" "$body" "<script"

stop_server

report md
