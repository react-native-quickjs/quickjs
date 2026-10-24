#!/bin/bash
# Fails when a binary was linked with ThinLTO but without the flags this tree's
# numbers depend on.
#
#   lto-check.sh BINARY...
#
# LTO defers code generation to the link, so -mbranches-within-32B-boundaries and
# -ffp-contract=off have to appear on the LINK command, not only on the compile
# command that produced the bitcode. A link that loses the JCC flag produces a
# binary that runs 2.2% slower in cycles at 4.4% FEWER instructions, and its
# conditional branches crossing a 32-byte boundary go from 0.05% to 8.3%. Any
# benchmark number taken from such a binary is incomparable to the rest of the
# tree, and nothing about the build fails to say so.
#
# The test is made on the binary, not on the command line: a command-line check
# passes for a stale binary left behind by an earlier run, and cannot see a link
# performed by a build system this script did not write.
set -u

# A correctly built binary sits near 0.05%; one built without the flag sat at
# 8.26%. The limit is far from both, so it does not fire on linker noise.
MAX_CROSS_PCT=1.0

[ $# -gt 0 ] || { echo "usage: lto-check.sh BINARY..." >&2; exit 2; }

rc=0
for bin in "$@"; do
  if [ ! -f "$bin" ]; then
    echo "lto-check: no such binary: $bin" >&2
    rc=1
    continue
  fi
  if out=$(python3 - "$bin" "$MAX_CROSS_PCT" <<'PY'
import re, subprocess, sys

path, limit = sys.argv[1], float(sys.argv[2])
LINE = re.compile(r'^\s+([0-9a-f]+):\t([0-9a-f ]+)\t(\S+)')
# -mbranches-within-32B-boundaries governs the conditional jumps, which are what
# the JCC erratum affects. Unconditional jmp and call are outside its scope, and
# counting them would report a correct binary as broken.
COND = re.compile(r'^j(n?[a-z]+|rcxz|ecxz)$')

bad = total = 0
dis = subprocess.run(["objdump", "-d", path], capture_output=True, text=True)
if dis.returncode != 0:
    print("cannot disassemble %s" % path)
    sys.exit(2)
for line in dis.stdout.splitlines():
    m = LINE.match(line)
    if not m:
        continue
    mnemonic = m.group(3)
    if not COND.match(mnemonic) or mnemonic == "jmp":
        continue
    size = len(m.group(2).split())
    total += 1
    # The branch is misaligned when its last byte falls into the next block.
    if (int(m.group(1), 16) % 32) + size > 32:
        bad += 1

pct = (bad / total * 100.0) if total else 0.0
print("%d/%d (%.2f%%) conditional branches cross a 32B boundary" % (bad, total, pct))
sys.exit(0 if pct <= limit else 1)
PY
  ); then
    echo "lto-check: ok   $bin: $out"
  else
    echo "lto-check: FAIL $bin: $out" >&2
    echo "lto-check: the link is missing -mbranches-within-32B-boundaries (and" >&2
    echo "lto-check: probably -ffp-contract=off). LTO generates code at link time," >&2
    echo "lto-check: so both flags must be on the link command too." >&2
    rc=1
  fi
done
exit $rc
