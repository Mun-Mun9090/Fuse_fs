#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
MOUNT="$TMP/mnt"
mkdir -p "$MOUNT/docs"
printf 'original contents\n' > "$MOUNT/docs/notes.txt"

"$ROOT/bin/fusefs-version" snapshot "$MOUNT" docs/notes.txt > "$TMP/first.out"
FIRST_ID="$(awk '{print $3}' "$TMP/first.out")"
printf 'changed contents\n' > "$MOUNT/docs/notes.txt"
"$ROOT/bin/fusefs-version" snapshot "$MOUNT" docs/notes.txt > "$TMP/second.out"
SECOND_ID="$(awk '{print $3}' "$TMP/second.out")"

"$ROOT/bin/fusefs-version" list "$MOUNT" docs/notes.txt > "$TMP/list.out"
grep -q "$FIRST_ID" "$TMP/list.out"
grep -q "$SECOND_ID" "$TMP/list.out"
"$ROOT/bin/fusefs-version" restore "$MOUNT" docs/notes.txt "$FIRST_ID"
grep -q '^original contents$' "$MOUNT/docs/notes.txt"

if "$ROOT/bin/fusefs-version" snapshot "$MOUNT" /etc/passwd >/dev/null 2>&1; then
    echo "ERROR: accepted a file outside the mount point" >&2
    exit 1
fi

echo "Version history test passed."
