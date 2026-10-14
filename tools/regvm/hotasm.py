#!/usr/bin/env python3
"""Join callgrind's per-instruction counts with llvm-objdump -l disassembly.

It shows which native instruction inside `js_regvm_execute` actually retires,
and which source line (that is, which handler) it belongs to.

    valgrind --tool=callgrind --dump-instr=yes --callgrind-out-file=ref.cg \
        ./gref workload.js
    llvm-objdump-14 -d -l --no-show-raw-insn \
        --disassemble-symbols=js_regvm_execute ./gref > exec.s
    tools/regvm/hotasm.py ref.cg exec.s js_regvm_execute --top 40

Warning -- the by-source-line modes are unreliable for `js_regvm_execute` in a
threaded (`-DQJS_REGVM_THREAD=1`) build. That function is now mostly a prologue,
a dispatch loop and an epilogue with the handlers compiled out of it, and its
line table no longer separates them: addresses land on whichever line survived,
so the rollup can charge a path that never ran. Use the per-instruction view
and the activation count instead: every prologue instruction retires once per
activation, so the prologue's counts are the activation count.

Modes:
    (default)   hot instructions, then the same counts rolled up by source line
    --spill     only instructions that touch the stack (`[sp, ...]` / `[sp]`),
                rolled up by source line: the register-allocator cost the
                redesign is trying to remove
    --lines     only the by-source-line rollup
    --json F    also write the parsed data

Notes:
  * callgrind's instruction position lines are `<addr> <line> <Ir>`; a `calls=`
    line is followed by the *callee's* cost line, which must not be attributed
    to the caller, so the cost line after `calls=` is skipped.
  * llvm-objdump prints addresses without an `0x` prefix, one section at a time,
    and emits `; /path/file.c:NNN` (sometimes `:NNN:CC`) before the instructions
    of that source line. Compiler-internal labels (`L_ROP_*`) are not printed,
    which is why the source line is the handler key here.
"""

import argparse
import collections
import json
import re
import sys

ADDR_LINE = re.compile(r"^\s*([0-9a-f]+):\s+(.*)$")
SRC_LINE = re.compile(r"^\s*;\s+(/\S+?):(\d+)(?::\d+)?\s*$")
SECT_LINE = re.compile(r"^Disassembly of section")
SYM_LINE = re.compile(r"^([0-9a-f]+)\s+<([^>]+)>:")
COST_LINE = re.compile(r"^(0x[0-9a-fA-F]+|[+\-]\d+|\*)\s+(\S+)\s+(\d+)\s*$")
SKIP_PREFIX = ("cfn=", "cfi=", "cfl=", "cob=", "ob=", "fl=", "fi=", "fe=",
               "jump=", "jcnd=", "fn=", "version:", "creator:", "pid:",
               "cmd:", "part:", "desc:", "positions:", "events:", "summary:",
               "totals:")


def parse_callgrind(path, fnfilter):
    """{address: retired Ir} for instructions inside functions matching fnfilter."""
    counts = collections.Counter()
    fnmap = {}
    cur_fn = ""
    last_addr = 0
    skip_cost = False
    total_matched = 0

    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            line = raw.strip()
            if line.startswith("fn="):
                m = re.match(r"fn=\((\d+)\)(?:\s+(.*))?$", line)
                if m:
                    if m.group(2):
                        fnmap[m.group(1)] = m.group(2)
                    cur_fn = fnmap.get(m.group(1), "")
                skip_cost = False
                continue
            if line.startswith("cfn="):
                m = re.match(r"cfn=\((\d+)\)(?:\s+(.*))?$", line)
                if m and m.group(2):
                    fnmap[m.group(1)] = m.group(2)
                continue
            if line.startswith("calls="):
                skip_cost = True
                continue
            if line.startswith(SKIP_PREFIX):
                continue
            m = COST_LINE.match(line)
            if not m:
                continue
            a, _srcline, ir = m.group(1), m.group(2), int(m.group(3))
            if a.startswith("0x"):
                addr = int(a, 16)
            elif a == "*":
                addr = last_addr
            else:
                addr = last_addr + int(a)
            last_addr = addr
            if skip_cost:
                skip_cost = False
                continue
            if cur_fn and fnfilter in cur_fn:
                counts[addr] += ir
                total_matched += ir
    return counts, total_matched


def parse_disasm(path):
    """[(address, source_line_string, text)] in file order."""
    out = []
    cur = ""
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if SECT_LINE.match(line) or SYM_LINE.match(line):
                cur = ""
                continue
            m = SRC_LINE.match(line)
            if m:
                cur = f"{m.group(1)}:{m.group(2)}"
                continue
            m = ADDR_LINE.match(line)
            if m:
                out.append((int(m.group(1), 16), cur, m.group(2).strip()))
    return out


def is_spill(text):
    """A frame access: `[sp, #N]`, `[sp]`, `[sp, #N]!` (and the x29 forms)."""
    return bool(re.search(r"\[(sp|x29)\b", text))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("callgrind")
    ap.add_argument("disasm")
    ap.add_argument("function", help="substring of the function name to select")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--threshold", type=float, default=0.0005,
                    help="fraction of the function's Ir an instruction needs to print")
    ap.add_argument("--per-line", type=float, default=0.002,
                    help="fraction for the by-source-line table")
    ap.add_argument("--lines", action="store_true")
    ap.add_argument("--spill", action="store_true")
    ap.add_argument("--json")
    args = ap.parse_args()

    counts, total = parse_callgrind(args.callgrind, args.function)
    if not total:
        print(f"no instruction counts matched function {args.function!r}",
              file=sys.stderr)
        return 1
    insns = parse_disasm(args.disasm)

    by_line = collections.Counter()
    spill_by_line = collections.Counter()
    spill_total = 0
    matched = 0
    for addr, srcline, text in insns:
        c = counts.get(addr, 0)
        if not c:
            continue
        matched += c
        key = srcline or "?"
        by_line[key] += c
        if is_spill(text):
            spill_by_line[key] += c
            spill_total += c

    print(f"function {args.function!r}: {total:,} Ir retired")
    print(f"instructions in disassembly matched by address: {matched:,} "
          f"({100.0*matched/total:.1f}%)")
    print(f"stack (sp/x29) accesses: {spill_total:,} "
          f"({100.0*spill_total/total:.1f}% of the function)")
    print()

    if not args.lines and not args.spill:
        print(f"{'address':>10}  {'Ir':>14}  {'%':>6}  {'source':<28} instruction")
        print(f"{'-'*10}  {'-'*14}  {'-'*6}  {'-'*28} {'-'*40}")
        floor = total * args.threshold
        for addr, srcline, text in insns:
            c = counts.get(addr, 0)
            if c <= floor:
                continue
            src = srcline.split("/")[-1] if srcline else "?"
            print(f"{addr:>10x}  {c:>14,}  {100.0*c/total:>5.2f}%  {src:<28} {text}")
        print()

    print("by source line (i.e. by handler):")
    print(f"{'Ir':>14}  {'%':>6}  {'spill':>14}  source")
    print(f"{'-'*14}  {'-'*6}  {'-'*14}  {'-'*40}")
    floor = total * args.per_line
    for key, c in by_line.most_common():
        if c <= floor:
            break
        sp = spill_by_line.get(key, 0)
        print(f"{c:>14,}  {100.0*c/total:>5.2f}%  {sp:>14,}  {key}")

    if args.spill:
        print()
        print("spill instructions, by source line:")
        for key, c in spill_by_line.most_common(40):
            print(f"{c:>14,}  {100.0*c/total:>5.2f}%  {key}")

    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump({
                "function": args.function,
                "total": total,
                "matched": matched,
                "spill_total": spill_total,
                "by_line": dict(by_line),
                "spill_by_line": dict(spill_by_line),
                "instructions": [
                    {"addr": hex(a), "line": l, "text": t, "ir": counts.get(a, 0)}
                    for a, l, t in insns if counts.get(a, 0)
                ],
            }, fh, indent=1)
        print(f"\nwrote {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())