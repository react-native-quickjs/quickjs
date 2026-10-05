#!/usr/bin/env python3
"""Measure each handler's fast path in a disassembly, and find stack traffic.

The P2 gate is stated as three static properties of the compiled handlers:

  * the `Mul` double path is <= 14 native instructions *including dispatch*
  * the `GetById` own-slot hit is <= 16
  * no `[sp, #...]` access on any fast path

Those are all read off the disassembly, so they need a script rather than a
person. After P2 every handler is its own function named `regvm_op_<name>`, so
each one is a real symbol; before P2 the monolith can still be inspected, but
only as a whole.

    objdump -d --no-show-raw-insn /tmp/rv/bref > bref.s
    tools/regvm/spillcheck.py bref.s --select regvm_op_ --top 30

Per symbol it reports:

    n_fast    instructions from the symbol's entry to the first unconditional
              control transfer -- the fall-through fast path as the compiler laid
              it out, dispatch included. This is the number the gate names.
    n_all     every instruction in the symbol, slow paths included.
    sp        instructions touching `[sp, ...]` or `[x29, ...]`.
    cbr       conditional branches inside the fast path: a fast path should reach
              its slow path through a branch, so a couple are expected; a large
              number means the branch-free assumption is wrong.

`--select` is a substring match on the symbol name (default `regvm_op_`).
`--budget NAME=N` asserts one handler's `n_fast` against the gate and exits 1 if
it is over. `--max-spill N` asserts the per-symbol stack-access count.
"""

import argparse
import collections
import re
import sys

SYM_LINE = re.compile(r"^([0-9a-f]+)\s+<([^>]+)>:")
INSN_LINE = re.compile(r"^\s*([0-9a-f]+):\s+(.*)$")
SECT_LINE = re.compile(r"^Disassembly of section")

# An unconditional transfer ends the fall-through fast path. `b.cond`, `cbz`,
# `tbz`, `cbnz` and `tbnz` are conditional and stay inside it.
TERMINATOR = re.compile(
    r"^(ret|br\s|braa|brab|blr\s|b\s|b\.tail|ud2|hlt|rfe|eret)\b")
SPILL = re.compile(r"\[(sp|x29)\b")
# A threaded handler must not save any register solely because the platform ABI
# marks it callee-saved. The ARM check is restricted to stack accesses so
# ordinary pairs used to copy JSValues do not count.
X86_CALLEE_SAVE = re.compile(
    r"\b(push|pop)[a-z]*\s+%(?:rbx|rbp|r1[2-5]|ebx|ebp|r(?:12|13|14|15)d)\b",
    re.IGNORECASE)
ARM_CALLEE_SAVE = re.compile(
    r"\b(?:stp|ldp|str|ldr)\s+[^\n]*\b(?:x(?:19|2[0-9])|w(?:19|2[0-9]))\b"
    r"[^\n]*\[sp\b", re.IGNORECASE)
# The conditional-select family and its aliases: `cinc` and `cset` are `csinc`,
# `cneg` is `csinv`. What matters is not that a handler contains one -- a
# comparison operator legitimately produces its boolean with `cset` -- but that
# one feeds the *instruction pointer*. `T_NEXT(SZ + (take ? off : 0))` lowers to
# `cmp; mov; csel; add x0`, which puts the compare and both operand loads on the
# critical path before the next opcode can even be fetched; two arms with a real
# branch start the fetch as soon as the compare resolves. That shape has now been
# introduced twice in this project, so it is checked rather than remembered --
# see T_BR_NEXT in quickjs.c.
SELECT = re.compile(r"\b(csel|csinc|csinv|csneg|cinc|cset|cneg)\b")
REG64 = re.compile(r"\b([xw])(\d{1,2})\b")
NO_DEST = {
    "str", "stp", "stur", "strb", "sturb", "sturh", "st1", "st2",
    "cmp", "cmn", "tst", "fcmp", "fccmp",
    "ret", "br", "bl", "blr", "nop", "ud2", "hlt",
    "dmb", "dsb", "isb", "b", "cbz", "cbnz", "tbz", "tbnz",
}
# A call returns its value in x0/x1, so it defines them even though nothing in
# the text names them.
CLOBBERS_X0 = {"bl", "blr"}


def parse_insn(text):
    """(destinations, sources) as 64-bit register names, for the subset used."""
    parts = text.split(None, 1)
    if not parts:
        return set(), set()
    mn = parts[0]
    ops = parts[1] if len(parts) > 1 else ""
    toks = [m.group(1) + m.group(2) for m in REG64.finditer(ops)]
    if mn in CLOBBERS_X0:
        return {"x0", "x1"}, set(toks)
    if mn in NO_DEST or mn.startswith("b."):
        return set(), set(toks)
    if mn in ("ldp", "ldnp"):
        # ldp Rt, Rt2, [Rn, #imm] -- two destinations, address register read
        if len(toks) < 3:
            return set(), set(toks)
        return set(toks[:2]), set(toks[2:])
    if not toks:
        return set(), set()
    dst, srcs = {toks[0]}, set(toks[1:])
    # A writeback or post-index form writes its address register too:
    # `ldrb w8, [x0, #7]!` advances the instruction pointer in x0.
    if "]" in ops and ("]!" in ops or re.search(r"\],", ops)):
        m = re.search(r"\[([^\]]+)\]", ops)
        if m:
            inner = [x.group(1) + x.group(2) for x in REG64.finditer(m.group(1))]
            if inner:
                dst.add(inner[0])
    return dst, srcs


def ip_selects(insns):
    """The instructions whose value reaches x0, and the selects among them.

    A backward walk that follows each register to the definition that actually
    reaches this point, so a `cset` producing a comparison result is not
    confused with a `csel` producing the next instruction pointer.
    """
    parsed = [parse_insn(text) for (_addr, text) in insns]
    need = {"x0"}
    sel = []
    for i in range(len(parsed) - 1, -1, -1):
        dst, srcs = parsed[i]
        if not (dst & need):
            continue
        if SELECT.search(insns[i][1]):
            sel.append(i)
        need = (need - dst) | srcs
    return sel


def parse(path):
    """Ordered {symbol: [(addr, text), ...]} in file order."""
    funcs = collections.OrderedDict()
    cur = None
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if SECT_LINE.match(line):
                cur = None
                continue
            m = SYM_LINE.match(line)
            if m:
                cur = m.group(2)
                funcs.setdefault(cur, [])
                continue
            m = INSN_LINE.match(line)
            if m and cur is not None:
                funcs[cur].append((int(m.group(1), 16), m.group(2).strip()))
    return funcs


def measure(insns):
    n_fast = None
    cbr = 0
    sp = 0
    sp_fast = 0
    sel = 0
    for i, (_addr, text) in enumerate(insns):
        if SPILL.search(text):
            sp += 1
            if n_fast is None:
                sp_fast += 1
        if n_fast is None:
            if SELECT.search(text):
                sel += 1
            if TERMINATOR.match(text):
                n_fast = i + 1
            elif text.startswith("b.") or text.startswith("cb") or \
                    text.startswith("tb"):
                cbr += 1
    if n_fast is None:
        n_fast = len(insns)
    return n_fast, len(insns), sp, sp_fast, cbr, sel


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("disasm")
    ap.add_argument("--select", default="regvm_op_")
    ap.add_argument("--exclude", action="append", default=[],
                    help="exclude symbols containing this substring; repeatable")
    ap.add_argument("--top", type=int, default=0,
                    help="print only the N largest fast paths")
    ap.add_argument("--budget", action="append", default=[],
                    metavar="NAME=N", help="assert n_fast <= N for a handler")
    ap.add_argument("--max-spill", type=int, default=None,
                    help="assert every selected symbol has <= N stack accesses")
    ap.add_argument("--no-callee-saved", action="store_true",
                    help="assert selected handlers contain no x86 push/pop or "
                         "ARM stack save/restore of callee-saved registers")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--no-select", action="store_true",
                    help="assert no handler's fast path computes a branch "
                         "target with a conditional select: the next "
                         "instruction pointer must come from a real branch")
    ap.add_argument("--no-frame", action="store_true",
                    help="assert no handler's fast path touches the stack: the "
                         "phase-2 rule that every call a handler can reach is a "
                         "tail call, so nothing is live across a dispatch")
    ap.add_argument("--frame-exempt", action="append", default=[],
                    metavar="SUBSTR",
                    help="with --no-frame, allow symbols containing SUBSTR: a "
                         "handler whose *hot* path is itself a call cannot avoid "
                         "a frame, and the exception has to be named")
    args = ap.parse_args()

    funcs = parse(args.disasm)
    selected = [(name, ins) for name, ins in funcs.items()
                if args.select in name and ins and
                not any(x in name for x in args.exclude)]
    if not selected:
        print(f"no symbols matching {args.select!r} in {args.disasm}",
              file=sys.stderr)
        return 2

    rows = []
    for name, insns in selected:
        n_fast, n_all, sp, sp_fast, cbr, sel = measure(insns)
        rows.append((name, n_fast, n_all, sp, sp_fast, cbr, sel))

    rows.sort(key=lambda r: -r[1])
    shown = rows[: args.top] if args.top else rows
    if not args.quiet:
        print(f"{'handler':<34} {'n_fast':>7} {'n_all':>7} {'sp':>5} "
              f"{'sp_fast':>8} {'cbr':>5} {'sel':>4}")
        print(f"{'-'*34} {'-'*7} {'-'*7} {'-'*5} {'-'*8} {'-'*5} {'-'*4}")
        for name, n_fast, n_all, sp, sp_fast, cbr, sel in shown:
            print(f"{name:<34} {n_fast:>7} {n_all:>7} {sp:>5} "
                  f"{sp_fast:>8} {cbr:>5} {sel:>4}")
        if args.top and len(rows) > args.top:
            print(f"... {len(rows)-args.top} more")

    status = 0
    for spec in args.budget:
        name, _, limit = spec.rpartition("=")
        limit = int(limit)
        hit = [r for r in rows if r[0] == name]
        if not hit:
            print(f"BUDGET {name}: symbol not found", file=sys.stderr)
            status = 1
            continue
        n_fast = hit[0][1]  # noqa: the budget is the only consumer of n_fast
        ok = n_fast <= limit
        print(f"BUDGET {name}: n_fast={n_fast} limit={limit} "
              f"{'OK' if ok else 'OVER'}")
        if not ok:
            status = 1

    if args.no_frame:
        # The phase-2 discipline, as a check rather than a convention: a handler
        # whose fast path touches [sp, #...] has a frame, and a frame means the
        # compiler had to keep ip, fr, f and ctx somewhere callee-saved across
        # something -- which is a call, and a call on a path the handler can
        # reach. Each one costs a dozen instructions of save, restore and
        # shuffle on *every* dispatch of that opcode; this is what made the
        # first conversion of the empty loop 1.7x slower than the monolith it
        # replaced. `_slow` halves and named exceptions are cold or inherently
        # calling.
        exempt = tuple(args.frame_exempt)
        bad = [r for r in rows
               if r[4] > 0 and "_slow" not in r[0]
               and not any(e in r[0] for e in exempt)]
        print(f"FRAMECHECK: {len(rows)} handlers, {len(bad)} with a framed "
              f"fast path" + (f" (exempt: {' '.join(exempt)})" if exempt else ""))
        for name, n_fast, _na, _sp, sp_fast, _cbr, _sel in bad[:40]:
            print(f"  {name}: n_fast={n_fast} stack accesses in fast path="
                  f"{sp_fast}")
        if bad:
            status = 1

    if args.no_select:
        # `_slow` halves keep the select form: they run on a miss, where the
        # extra latency is not on any workload's critical path, and rewriting
        # them would only add branches to code already off the hot path.
        bad = []
        for name, insns in selected:
            if "_slow" in name:
                continue
            sel = ip_selects(insns)
            if sel:
                bad.append((name, len(sel)))
        print(f"SELECTCHECK: {len(rows)} handlers, {len(bad)} whose returned "
              f"instruction pointer comes through a select")
        for name, n in bad[:40]:
            print(f"  {name}: {n}")
        if bad:
            status = 1
    if args.max_spill is not None:
        bad = [r for r in rows if r[3] > args.max_spill]
        print(f"SPILLCHECK: {len(rows)} handlers, "
              f"{len(bad)} over {args.max_spill} stack accesses")
        for name, _nf, _na, sp, _spf, _cbr, _sel in bad[:20]:
            print(f"  {name}: {sp}")
        if bad:
            status = 1

    if args.no_callee_saved:
        bad = []
        for name, insns in selected:
            hits = [text for _addr, text in insns
                    if X86_CALLEE_SAVE.search(text) or
                    ARM_CALLEE_SAVE.search(text)]
            if hits:
                bad.append((name, hits))
        print(f"CALLEESAVECHECK: {len(rows)} handlers, {len(bad)} with "
              "callee-saved stack traffic")
        for name, hits in bad[:40]:
            print(f"  {name}: {hits[0]}")
        if bad:
            status = 1

    return status


if __name__ == "__main__":
    sys.exit(main())
