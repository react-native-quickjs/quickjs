#!/usr/bin/env bash
# AOT digest unit test.
#
# Checks that aot_digest() is:
#   1. deterministic        - same source, two runs => same digest
#   2. atom-id independent  - same function after unrelated atoms are interned
#                             first (different atom ids) => same digest
#   3. content sensitive    - a one-token edit => different digest
#
# Usage: tools/aot/digest-test.sh [path/to/qjsprof]
# The binary must be a QJS_AOT_PROFILE build (e.g. the qjs-bench linked with
# -DQJS_REGVM=1 -DQJS_AOT_PROFILE=1). Falls back to $QJS_AOT_PROF_BIN.

set -euo pipefail

BIN="${1:-${QJS_AOT_PROF_BIN:-}}"
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
    echo "usage: $0 <qjsprof-binary>   (or set QJS_AOT_PROF_BIN)" >&2
    echo "note: build with -DQJS_REGVM=1 -DQJS_AOT_PROFILE=1" >&2
    exit 2
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/base.js" <<'EOF'
function foo(o){ return o.x + 1; }
foo({x:2});
EOF

# Unrelated atoms interned before foo, shifting every atom id.
cat >"$tmp/atoms.js" <<'EOF'
var unrelated_atom_zzz = "hello";
var another_atom = "world";
function foo(o){ return o.x + 1; }
foo({x:2});
EOF

# One-token edit inside foo.
cat >"$tmp/edited.js" <<'EOF'
function foo(o){ return o.x + 2; }
foo({x:2});
EOF

# Extract the digest of the F line whose name=foo.
foo_digest() {
    QJS_AOT_PROFILE_OUT="$tmp/out.qjsprof" "$BIN" "$1" >/dev/null 2>&1
    awk '$1=="F" && $3=="name=foo"{print $2; exit}' "$tmp/out.qjsprof"
}

d_base=$(foo_digest "$tmp/base.js")
d_base2=$(foo_digest "$tmp/base.js")
d_atoms=$(foo_digest "$tmp/atoms.js")
d_edited=$(foo_digest "$tmp/edited.js")

fail=0
check_eq() {
    if [ "$1" = "$2" ]; then
        echo "ok   $3"
    else
        echo "FAIL $3: $1 != $2"
        fail=1
    fi
}
check_ne() {
    if [ "$1" != "$2" ]; then
        echo "ok   $3"
    else
        echo "FAIL $3: unexpectedly equal ($1)"
        fail=1
    fi
}

[ -n "$d_base" ] || { echo "FAIL: no F name=foo line produced"; exit 1; }

check_eq "$d_base" "$d_base2" "deterministic (same source, two runs)"
check_eq "$d_base" "$d_atoms" "atom-id independent"
check_ne "$d_base" "$d_edited" "content sensitive (one-token edit)"

exit "$fail"
