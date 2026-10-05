#!/bin/sh
# template-sync.sh -- guard the AOT Layer A templates against interpreter drift.
#
# Every template in tools/aot/qjsaot.c mirrors one interpreter handler in
# engine/quickjs-ng/quickjs.c (the manifest maps the ROP to the handler label).
# This script hashes each mirrored handler body and compares it with the hash
# recorded at the last review (template-sync.hashes). A change means the
# template may no longer match the handler, so the check fails and the template
# must be re-reviewed before the hash is refreshed.
#
# Usage:
#   tools/aot/template-sync.sh            # check; exit 1 if a handler changed
#   tools/aot/template-sync.sh --update   # re-record the hashes after review
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
manifest="$root/tools/aot/template-sync.manifest"
hashes="$root/tools/aot/template-sync.hashes"
engine="$root/engine/quickjs-ng/quickjs.c"

update=0
[ "${1:-}" = "--update" ] && update=1

if [ ! -f "$engine" ]; then
  echo "template-sync: missing $engine" >&2
  exit 1
fi

# body <label>: print the handler body, from its label to the next L_ROP_/exit.
# A line holding only one or more labels (the compiler groups aliases like
# `L_ROP_get_local_s0: L_ROP_get_local_s1:`) is kept; the group's body follows.
body() {
  awk -v label="$1" '
    $0 ~ "^[ \t]*(L_ROP_[A-Za-z0-9_]+:[ \t]*)*" label ":" { inb = 1; print; next }
    inb && /^[ \t]*L_ROP_/ {
      if ($0 ~ /^[ \t]*L_ROP_[A-Za-z0-9_]+:[ \t]*(L_ROP_[A-Za-z0-9_]+:[ \t]*)*$/) {
        print; next
      }
      exit
    }
    inb && /^[ \t]*(exception|done|done_generator|L_default):/ { exit }
    inb { print }
  ' "$engine"
}

tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
while read -r rop label; do
  case "$rop" in ''|\#*) continue ;; esac
  text=$(body "$label" | sed -e 's/^[ \t]*//' -e 's/[ \t]*$//' | grep -v '^$')
  if [ -z "$text" ]; then
    echo "template-sync: handler $label (for $rop) not found in $engine" >&2
    exit 1
  fi
  sum=$(printf '%s\n' "$text" | sha256sum | cut -d' ' -f1)
  printf '%s %s %s\n' "$rop" "$label" "$sum" >> "$tmp"
done < "$manifest"

if [ "$update" = 1 ]; then
  sort "$tmp" > "$hashes"
  echo "template-sync: recorded $(wc -l < "$hashes") handler hashes in $(basename "$hashes")"
  exit 0
fi

if [ ! -f "$hashes" ]; then
  echo "template-sync: no $hashes; run with --update after reviewing the templates" >&2
  exit 1
fi

sort "$tmp" > "$tmp.sorted"
if diff -u "$hashes" "$tmp.sorted" > "$tmp.diff"; then
  echo "template-sync: ok ($(wc -l < "$hashes") handlers unchanged)"
  exit 0
fi

echo "template-sync: a mirrored interpreter handler changed since the template was" >&2
echo "               last reviewed; re-check tools/aot/qjsaot.c and re-run --update." >&2
cat "$tmp.diff" >&2
exit 1
