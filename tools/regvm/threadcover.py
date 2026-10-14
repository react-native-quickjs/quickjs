#!/usr/bin/env python3
"""Which executed opcodes still leave the dedicated threaded handlers?

The threaded register VM gives an opcode a dedicated handler only when it is
listed in `REGVM_T_BINARY_LIST`, `REGVM_T_UNARY_LIST`, or (with a handler other
than `regvm_t_cold`) in `REGVM_T_EXPLICIT_LIST`. Everything else runs through
one of two shared dispatchers:

  * `regvm_t_cold` -- still reached by musttail from the threaded chain, but it
    decodes the opcode and switches again before doing the work;
  * `regvm_t_to_loop` -- hands the instruction back to the monolithic loop,
    which re-dispatches from scratch.

Both are correct, but they cost an extra branch per executed instruction, which
is why an opcode that is a few percent of a workload is worth its own handler.
This script joins a `QJS_REGVM_COUNTS=1` run (the `##ROP##` lines, one per
executed register instruction) with the three lists above and reports, per
workload, the unthreaded share and the opcodes above a threshold.

    tools/regvm/threadcover.py /tmp/rv/*.cnt
    tools/regvm/threadcover.py --check --threshold 0.5 /tmp/rv/*.cnt

`--check` exits non-zero when an unthreaded opcode is at or above the threshold,
so the threaded set cannot silently regress.
"""

import argparse
import collections
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
HEADER = os.path.join(REPO, "engine", "quickjs-ng", "quickjs-regvm.h")
QUICKJS = os.path.join(REPO, "engine", "quickjs-ng", "quickjs.c")

sys.path.insert(0, HERE)
from ropcounts import parse_enum, ROP_RE  # noqa: E402


def parse_lists(path):
    """Return (dedicated, cold, to_loop) sets of ROP names.

    The lists are `#define REGVM_T_..._LIST(X) \\` blocks with backslash
    continuations, so a block is read until the first line that does not end in
    a backslash.
    """
    text = open(path, "r", encoding="utf-8").read()

    def block(name):
        m = re.search(r"#define\s+" + name + r"\(X\)\s*\\\n", text)
        if not m:
            raise SystemExit("no #define %s in %s" % (name, path))
        out = []
        i = m.end()
        while True:
            j = text.find("\n", i)
            line = text[i:j]
            out.append(line)
            if not line.rstrip().endswith("\\"):
                break
            i = j + 1
        return "\n".join(out)

    dedicated, cold = set(), set()
    for name, handler in re.findall(r"X\((\w+),\s*(\w+)\)", block("REGVM_T_EXPLICIT_LIST")):
        (cold if handler == "regvm_t_cold" else dedicated).add(name)
    dedicated.update(re.findall(r"X\((\w+),", block("REGVM_T_BINARY_LIST")))
    dedicated.update(re.findall(r"X\((\w+),", block("REGVM_T_UNARY_LIST")))

    to_loop = set(re.findall(r"X\((\w+)\)", block("REGVM_T_FALLBACK_LIST")))
    # call/call_method are dedicated under QJS_REGVM_RECORD, back to the loop
    # otherwise; the record build is the default, so treat them as dedicated.
    to_loop -= {"call", "call_method"}
    return dedicated, cold, to_loop


def parse_counts(path):
    rops = collections.Counter()
    for line in open(path, "r", encoding="utf-8", errors="replace"):
        m = ROP_RE.match(line)
        if m:
            rops[int(m.group(1))] += int(m.group(2))
    return rops


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("counts", nargs="+")
    ap.add_argument("--threshold", type=float, default=0.5,
                    help="percent at or above which an opcode must be threaded")
    ap.add_argument("--check", action="store_true",
                    help="exit non-zero if an unthreaded opcode is >= threshold")
    ap.add_argument("--top", type=int, default=12)
    args = ap.parse_args()

    names, _ = parse_enum(HEADER)
    dedicated, cold, to_loop = parse_lists(QUICKJS)
    unthreaded = cold | to_loop

    failures = 0
    for path in args.counts:
        rops = parse_counts(path)
        executed = sum(rops.values())
        if not executed:
            print("%s: no ##ROP## lines" % path, file=sys.stderr)
            continue
        rows = []
        for op, count in rops.items():
            name = names.get(op, "<unknown %d>" % op)
            member = name[4:] if name.startswith("ROP_") else name
            rows.append((count, op, name, "cold" if member in cold
                         else ("loop" if member in to_loop else "dedicated")))
        rows.sort(reverse=True)
        share = lambda c: 100.0 * c / executed
        cold_share = sum(share(c) for c, _, _, k in rows if k != "dedicated")
        print("== %s: %s executed, unthreaded %.2f%%"
              % (os.path.basename(path), f"{executed:,}", cold_share))
        shown = 0
        for count, op, name, kind in rows:
            if kind == "dedicated":
                continue
            pct = share(count)
            if pct < args.threshold:
                break
            shown += 1
            if shown <= args.top:
                flag = " <-- cold" if kind == "cold" else ""
                print("   %5d  %-30s %14s  %5.2f%%  %s%s"
                      % (op, name, f"{count:,}", pct, kind, flag))
            failures += 1
        if shown == 0:
            print("   (none above %.2f%%)" % args.threshold)
    if args.check and failures:
        print("\n%d unthreaded opcode(s) at or above %.2f%%"
              % (failures, args.threshold), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
