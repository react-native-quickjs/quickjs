#!/usr/bin/env python3
"""Name the `##ROP##` dispatch-count lines a QJS_REGVM_COUNT build emits.

    QJS_REGVM_COUNTS=1 ./bcnt workload.js 2> cnt.err
    tools/regvm/ropcounts.py cnt.err [--top N] [--json out.json]

The interpreter prints `##ROP## <opcode> <count>` once per executed register
instruction and a handful of named counter lines (`##GETFIELDIC##`, `##PUTIC##`,
`##MOVES##`, `##GETFIELD##`, `##SLOT##`). The opcode is the numeric value of the
`JSRegOpcode` enum in `engine/quickjs-ng/quickjs-regvm.h`, so the names are read
out of that header rather than duplicated here -- renumbering the enum changes
this script's output and nothing else.

Counts are per ROP *id*, not per specialised form: `ROP_binary`, `ROP_binary_imm`
and so on have distinct ids and are reported separately, which is what the
"which form is hot" question needs.
"""

import argparse
import collections
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
HEADER = os.path.join(REPO, "engine", "quickjs-ng", "quickjs-regvm.h")

ROP_RE = re.compile(r"^##ROP##\s+(\d+)\s+(\d+)\s*$")
NAMED_RE = re.compile(r"^##([A-Z]+)##\s*(.*)$")


def parse_enum(path, typedef="JSRegOpcode"):
    """Return {value: name} for one `typedef enum <typedef>` block, plus the
    value of its `*_COUNT` sentinel if it has one.

    Names are collected positionally rather than by prefix, because the header
    has more than one such enum (`JSRegOpcode`, `JSRegBinaryOp`,
    `JSRegUnaryOp`) and their member prefixes differ. An explicit `= <int>`
    resets the counter, so a partially-numbered enum still reads correctly.

    The enum bodies carry long explanatory block comments that name other
    enumerators (`ROP_binary_imm_s carry a sub operand`, `as ROP_binary_* has`),
    so comment state is tracked across lines: stripping `/*...*/` per line
    silently swallows those names as if they were enumerators and shifts every
    later number.
    """
    names = {}
    value = 0
    total = None
    seen_enum = False
    in_block = False
    marker = "typedef enum " + typedef

    def strip_comments(text):
        nonlocal in_block
        out = []
        i = 0
        while i < len(text):
            if in_block:
                end = text.find("*/", i)
                if end < 0:
                    return "".join(out)
                in_block = False
                i = end + 2
                continue
            start = text.find("/*", i)
            line_comment = text.find("//", i)
            if line_comment >= 0 and (start < 0 or line_comment < start):
                out.append(text[i:line_comment])
                return "".join(out)
            if start < 0:
                out.append(text[i:])
                return "".join(out)
            out.append(text[i:start])
            in_block = True
            i = start + 2
        return "".join(out)

    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            text = strip_comments(line)
            if not seen_enum:
                if marker in text:
                    seen_enum = True
                continue
            if text.strip().startswith("}"):
                break
            # `quickjs-regvm.h` puts `#define` lines (REG_RMW_UNARY,
            # REG_CMP_INVERT) inside the enum bodies. They are not enumerators,
            # and counting them shifts every later member by one.
            if text.lstrip().startswith("#"):
                continue
            tokens = text.replace(",", " ").split()
            for i, token in enumerate(tokens):
                if not re.fullmatch(r"[A-Za-z_]\w*", token):
                    continue
                explicit = None
                if i + 2 < len(tokens) and tokens[i + 1] == "=":
                    try:
                        explicit = int(tokens[i + 2], 0)
                    except ValueError:
                        explicit = None
                if explicit is not None:
                    value = explicit
                if token.endswith("_COUNT"):
                    total = value
                    continue
                names[value] = token
                value += 1
    return names, total


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("counts", help="stderr file from a QJS_REGVM_COUNTS=1 run")
    ap.add_argument("--top", type=int, default=30)
    ap.add_argument("--json", help="also write the parsed table here")
    ap.add_argument("--header", default=HEADER)
    args = ap.parse_args()

    names, total_ops = parse_enum(args.header)
    bin_names, bin_count = parse_enum(args.header, "JSRegBinaryOp")
    un_names, un_count = parse_enum(args.header, "JSRegUnaryOp")

    rops = {}
    named = collections.OrderedDict()
    with open(args.counts, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = ROP_RE.match(line)
            if m:
                op = int(m.group(1))
                rops[op] = rops.get(op, 0) + int(m.group(2))
                continue
            m = NAMED_RE.match(line)
            if m:
                named.setdefault(m.group(1), []).append(m.group(2).strip())

    executed = sum(rops.values())
    if not executed:
        print("no ##ROP## lines found", file=sys.stderr)
        return 1

    print(f"executed register instructions: {executed:,}")
    if total_ops:
        print(f"opcodes defined: {total_ops}, distinct executed: {len(rops)}")
    print()
    print(f"{'id':>5}  {'rop':<32} {'count':>14}  {'%':>6}")
    print(f"{'-'*5}  {'-'*32} {'-'*14}  {'-'*6}")
    ranked = sorted(rops.items(), key=lambda kv: -kv[1])
    for op, count in ranked[: args.top]:
        name = names.get(op, f"<unknown {op}>")
        print(f"{op:>5}  {name:<32} {count:>14,}  {100.0*count/executed:>5.2f}%")
    shown = sum(c for _, c in ranked[: args.top])
    if shown != executed:
        print(f"{'':>5}  {'(remaining %d opcodes)' % (len(ranked)-args.top):<32} "
              f"{executed-shown:>14,}  {100.0*(executed-shown)/executed:>5.2f}%")
    print()
    for key, lines in named.items():
        if key in ("BINSUB", "UNSUB"):
            continue
        print(f"##{key}##")
        for ln in lines:
            print(f"    {ln}")

    # ROP_binary and ROP_unary carry their kind as an operand, so which kinds a
    # workload executes is invisible in the ROP counts. These two histograms are
    # what decides which kinds are worth an opcode of their own.
    for key, table, count in (("BINSUB", bin_names, bin_count),
                              ("UNSUB", un_names, un_count)):
        rows = []
        for ln in named.get(key, []):
            parts = ln.split()
            if len(parts) == 2:
                rows.append((int(parts[0]), int(parts[1])))
        if not rows:
            continue
        tot = sum(c for _, c in rows)
        print()
        print(f"##{key}##  ({tot:,} sub-operations)")
        print(f"{'id':>5}  {'sub-op':<28} {'count':>14}  {'%':>6}")
        for op, c in sorted(rows, key=lambda kv: -kv[1]):
            print(f"{op:>5}  {table.get(op, f'<unknown {op}>'):<28} "
                  f"{c:>14,}  {100.0*c/tot:>5.2f}%")
        missing = sorted(set(range(count or 0)) - {op for op, _ in rows})
        if missing:
            print(f"      (never executed: "
                  f"{', '.join(table.get(m, str(m)) for m in missing)})")

    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(
                {
                    "executed": executed,
                    "rops": [
                        {"id": op, "name": names.get(op, f"<unknown {op}>"),
                         "count": c, "share": c / executed}
                        for op, c in ranked
                    ],
                    "named": named,
                },
                fh,
                indent=2,
            )
        print(f"\nwrote {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())