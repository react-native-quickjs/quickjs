#!/usr/bin/env python3
"""Merge one or more .qjsprof files into a single profile.

The engine's AOT profiler (build with -DQJS_AOT_PROFILE=1, run with
QJS_AOT_PROFILE_OUT=...) writes one `.qjsprof` per process. A workload is
usually several processes, and a benchmark is several workloads; this tool
folds them together by function digest so the report sees one hot set.

Counts are summed. Per-function facts are unioned:

  L  loop iteration counts      summed
  R  register type sets         bit-OR of the class names
  S  PIC site shape histograms  merged by shape pointer within a run; across
                                runs only the total count is meaningful, so
                                shapes are kept from the first run that saw
                                the site and the count is summed
  C  call sites                 summed; per-callee hits summed, argc range
                                widened, new callees appended

Usage:
    python3 tools/aot/profmerge.py -o merged.qjsprof a.qjsprof b.qjsprof ...
    python3 tools/aot/profmerge.py --append merged.qjsprof a.qjsprof
"""

import argparse
import sys
from collections import OrderedDict

ABI = None


class Func:
    __slots__ = ("digest", "name", "file", "line", "ops", "entries", "len",
                 "loops", "regs", "pcs", "sites", "calls")

    def __init__(self, digest):
        self.digest = digest
        self.name = "?"
        self.file = "?"
        self.line = 0
        self.ops = 0
        self.entries = 0
        self.len = 0
        self.loops = {}          # pc -> iters
        self.regs = {}           # reg -> set(types)
        self.pcs = {}            # defining pc -> {types, min, max, frac, negzero, src}
        self.sites = {}          # site -> [count, shape_map]
        self.calls = OrderedDict()  # pc -> Call


class Callee:
    __slots__ = ("kind", "name", "hits", "magic", "cproto", "fn")

    def __init__(self, kind, name):
        self.kind = kind
        self.name = name
        self.hits = 0
        self.magic = None
        self.cproto = None
        self.fn = None

    def key(self):
        return (self.kind, self.name, self.fn)


class Call:
    __slots__ = ("pc", "count", "argc_min", "argc_max", "callees", "src")

    def __init__(self, pc):
        self.pc = pc
        self.count = 0
        self.argc_min = 255
        self.argc_max = 0
        self.callees = OrderedDict()  # key -> Callee
        self.src = "?:?"


def parse_call_kv(tok):
    """`callee=bc:<hex>` or `callee=c:<sym>:fn=0x..:magic=0:cproto=1`."""
    # Strip the leading `callee=`.
    assert tok.startswith("callee=")
    body = tok[len("callee="):]
    if body.startswith("bc:"):
        return "bc", body[3:], {}
    if body.startswith("c:"):
        # The symbol itself may not contain ':', so split from the right on the
        # fn= that the emitter always writes.
        sym, _, rest = body[2:].partition(":fn=")
        kv = {}
        parts = (rest.split(":") if rest else [])
        if parts:
            kv["fn"] = parts[0]
            for p in parts[1:]:
                k, _, v = p.partition("=")
                kv[k] = v
        return "c", sym, kv
    return "other", body, {}


def parse_file(path, verbose=False):
    global ABI
    funcs = OrderedDict()
    with open(path, "r", errors="replace") as fh:
        for lineno, raw in enumerate(fh, 1):
            line = raw.rstrip("\n")
            if not line:
                continue
            if line.startswith("qjsprof "):
                # qjsprof 1 abi=N
                toks = line.split()
                for t in toks:
                    if t.startswith("abi="):
                        abi = int(t[4:])
                        if ABI is None:
                            ABI = abi
                        elif ABI != abi:
                            raise SystemExit(
                                "%s: ABI %d does not match %d" % (path, abi, ABI))
                continue
            kind, _, rest = line.partition(" ")
            if not rest:
                continue
            digest, _, rest = rest.partition(" ")
            fr = funcs.get(digest)
            if fr is None:
                fr = funcs[digest] = Func(digest)
            if kind == "F":
                kv = dict(tok.split("=", 1) for tok in rest.split(" ") if "=" in tok)
                fr.name = kv.get("name", fr.name)
                f = kv.get("file", "?")
                # file has form <path>:<line>
                if ":" in f:
                    p, _, ln = f.rpartition(":")
                    fr.file = p
                    try:
                        fr.line = int(ln)
                    except ValueError:
                        fr.file = f
                else:
                    fr.file = f
                fr.ops += int(kv.get("ops", 0))
                fr.entries += int(kv.get("entries", 0))
                fr.len = int(kv.get("len", fr.len))
            elif kind == "L":
                kv = dict(tok.split("=", 1) for tok in rest.split(" ") if "=" in tok)
                pc = int(kv["pc"])
                ent = fr.loops.get(pc)
                if ent is None:
                    ent = fr.loops[pc] = [0, kv.get("src", "?:?")]
                ent[0] += int(kv["iters"])
            elif kind == "R":
                kv = dict(tok.split("=", 1) for tok in rest.split(" ") if "=" in tok)
                reg = int(kv["reg"])
                ent = fr.regs.get(reg)
                if ent is None:
                    ent = fr.regs[reg] = [set(), kv.get("src", "?:?")]
                ent[0].update(kv.get("types", "").split("|"))
            elif kind == "P":
                kv = dict(tok.split("=", 1) for tok in rest.split(" ") if "=" in tok)
                pc = int(kv["pc"])
                ent = fr.pcs.get(pc)
                if ent is None:
                    ent = fr.pcs[pc] = {"types": set(), "min": None, "max": None,
                                        "frac": 0, "negzero": 0,
                                        "src": kv.get("src", "?:?")}
                ent["types"].update(kv.get("types", "").split("|"))
                if "num" in kv:
                    lo, _, hi = kv["num"].partition("..")
                    lo, hi = float(lo), float(hi)
                    ent["min"] = lo if ent["min"] is None else min(ent["min"], lo)
                    ent["max"] = hi if ent["max"] is None else max(ent["max"], hi)
                if kv.get("frac") == "1":
                    ent["frac"] = 1
                if kv.get("negzero") == "1":
                    ent["negzero"] = 1
            elif kind == "S":
                toks = rest.split(" ")
                site = None
                count = 0
                shapes = []
                src = "?:?"
                for tok in toks:
                    if tok.startswith("site="):
                        site = int(tok[5:])
                    elif tok.startswith("count="):
                        count = int(tok[6:])
                    elif tok.startswith("src="):
                        src = tok[4:]
                    elif tok.startswith("shape") and "=" in tok:
                        # shapeN=0x.. then hits=N
                        kv = tok.split("=")
                        shapes.append(kv[1])
                ent = fr.sites.get(site)
                if ent is None:
                    ent = fr.sites[site] = [0, {}, src]
                ent[0] += count
                for si, sh in enumerate(shapes):
                    ent[1].setdefault(sh, 0)
            elif kind == "C":
                toks = rest.split(" ")
                pc = None
                count = 0
                argc_min, argc_max = 255, 0
                ck = None
                src = "?:?"
                for tok in toks:
                    if tok.startswith("pc="):
                        pc = int(tok[3:])
                    elif tok.startswith("count="):
                        count = int(tok[6:])
                    elif tok.startswith("argc="):
                        a = tok[5:]
                        lo, _, hi = a.partition("..")
                        argc_min = int(lo)
                        argc_max = int(hi)
                    elif tok.startswith("src="):
                        src = tok[4:]
                    elif tok.startswith("callee="):
                        ck = parse_call_kv(tok)
                    elif tok.startswith("hits="):
                        hits = int(tok[5:])
                        if ck is not None:
                            ckind, cname, ckv = ck
                            call = fr.calls.get(pc)
                            if call is None:
                                call = fr.calls[pc] = Call(pc)
                            key = (ckind, cname)
                            cal = call.callees.get(key)
                            if cal is None:
                                cal = call.callees[key] = Callee(ckind, cname)
                                cal.fn = ckv.get("fn")
                                cal.magic = ckv.get("magic")
                                cal.cproto = ckv.get("cproto")
                            cal.hits += hits
                call = fr.calls.get(pc)
                if call is None:
                    call = fr.calls[pc] = Call(pc)
                call.count += count
                call.argc_min = min(call.argc_min, argc_min)
                call.argc_max = max(call.argc_max, argc_max)
                if call.src == "?:?":
                    call.src = src
    return funcs


def merge(into, fr):
    g = into.get(fr.digest)
    if g is None:
        into[fr.digest] = fr
        return
    g.ops += fr.ops
    g.entries += fr.entries
    g.len = fr.len or g.len
    if fr.name not in ("?", "<null>") and g.name in ("?", "<null>"):
        g.name = fr.name
    if fr.file not in ("?",) and g.file in ("?",):
        g.file, g.line = fr.file, fr.line
    for pc, (n, src) in fr.loops.items():
        ent = g.loops.get(pc)
        if ent is None:
            ent = g.loops[pc] = [0, src]
        ent[0] += n
    for reg, (ts, src) in fr.regs.items():
        ent = g.regs.get(reg)
        if ent is None:
            ent = g.regs[reg] = [set(), src]
        ent[0].update(ts)
    for site, (count, shapes, src) in fr.sites.items():
        ent = g.sites.get(site)
        if ent is None:
            ent = g.sites[site] = [0, {}, src]
        ent[0] += count
        for sh in shapes:
            ent[1].setdefault(sh, 0)
    for pc, call in fr.calls.items():
        gc = g.calls.get(pc)
        if gc is None:
            gc = g.calls[pc] = Call(pc)
        gc.count += call.count
        gc.argc_min = min(gc.argc_min, call.argc_min)
        gc.argc_max = max(gc.argc_max, call.argc_max)
        if gc.src == "?:?":
            gc.src = call.src
        for key, cal in call.callees.items():
            gcal = gc.callees.get(key)
            if gcal is None:
                gc.callees[key] = cal
            else:
                gcal.hits += cal.hits


def write(out, funcs):
    out.write("qjsprof 1 abi=%s\n" % (ABI if ABI is not None else "?"))
    for fr in funcs.values():
        name = fr.name if fr.name else "<null>"
        out.write("F %s name=%s file=%s:%d ops=%d entries=%d len=%d\n" % (
            fr.digest, name, fr.file, fr.line, fr.ops, fr.entries, fr.len))
        for pc in sorted(fr.loops):
            iters, src = fr.loops[pc]
            out.write("L %s pc=%d iters=%d src=%s\n" % (fr.digest, pc, iters, src))
        for reg in sorted(fr.regs):
            ts, src = fr.regs[reg]
            if ts:
                out.write("R %s reg=%d types=%s src=%s\n" % (
                    fr.digest, reg, "|".join(sorted(ts)), src))
        for site in sorted(fr.sites):
            count, shapes, src = fr.sites[site]
            out.write("S %s site=%d count=%d src=%s shapes=%d" % (
                fr.digest, site, count, src, len(shapes)))
            for sh in shapes:
                out.write(" shape=%s" % sh)
            out.write("\n")
        for pc in sorted(fr.calls):
            call = fr.calls[pc]
            lo = 0 if call.argc_min == 255 else call.argc_min
            for cal in call.callees.values():
                if cal.kind == "c":
                    out.write(
                        "C %s pc=%d count=%d argc=%d..%d src=%s callee=c:%s:fn=%s:magic=%s:cproto=%s hits=%d\n"
                        % (fr.digest, pc, call.count, lo, call.argc_max, call.src,
                           cal.name, cal.fn or "0x0", cal.magic or 0,
                           cal.cproto or 0, cal.hits))
                elif cal.kind == "bc":
                    out.write(
                        "C %s pc=%d count=%d argc=%d..%d src=%s callee=bc:%s hits=%d\n"
                        % (fr.digest, pc, call.count, lo, call.argc_max, call.src,
                           cal.name, cal.hits))
                else:
                    out.write(
                        "C %s pc=%d count=%d argc=%d..%d src=%s callee=other hits=%d\n"
                        % (fr.digest, pc, call.count, lo, call.argc_max, call.src, cal.hits))


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inputs", nargs="+")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--append", action="store_true",
                    help="merge into an existing --out file instead of overwriting")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)

    funcs = OrderedDict()
    if args.append:
        try:
            funcs = parse_file(args.out)
        except FileNotFoundError:
            pass
    for path in args.inputs:
        for fr in parse_file(path).values():
            merge(funcs, fr)
    with open(args.out, "w") as out:
        write(out, funcs)
    if not args.quiet:
        total = sum(f.ops for f in funcs.values())
        sys.stderr.write("merged %d file(s): %d functions, %d ops\n"
                         % (len(args.inputs), len(funcs), total))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
