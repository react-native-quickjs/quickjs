#!/usr/bin/env python3
"""Report a `.qjsprof` profile: hot set, coverage, and call graph.

Consumes the output of the engine's P0 profiler (see tools/aot/profmerge.py for
the format) and answers the questions P1+ need:

  * Which functions dominate executed ops, and how concentrated is the set?
    (the `F ... ops=` total, i.e. interpreter dispatches)
  * Which functions are tiny and hot -- the best inline candidates?
  * Which call sites matter, to which JS functions and which C builtins?

Usage:
    python3 tools/aot/profreport.py profile.qjsprof
    python3 tools/aot/profreport.py --top 40 --dot hot.dot profile.qjsprof
    python3 tools/aot/profreport.py --csv hot.csv profile.qjsprof
"""

import argparse
import os
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import profmerge  # noqa: E402


def human(n):
    if n >= 1_000_000_000:
        return "%.2fG" % (n / 1e9)
    if n >= 1_000_000:
        return "%.2fM" % (n / 1e6)
    if n >= 1_000:
        return "%.1fK" % (n / 1e3)
    return str(n)


def main(argv):
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("profile")
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--dot", metavar="FILE", help="write call graph as Graphviz dot")
    ap.add_argument("--csv", metavar="FILE", help="write the hot-set table as CSV")
    ap.add_argument("--min-edges", type=int, default=1,
                    help="drop call-graph edges with fewer total hits")
    args = ap.parse_args(argv)

    funcs = profmerge.parse_file(args.profile)
    if not funcs:
        sys.stderr.write("no functions in %s\n" % args.profile)
        return 1

    order = sorted(funcs.values(), key=lambda f: f.ops, reverse=True)
    total = sum(f.ops for f in order)
    nfunc = sum(1 for f in order if f.ops)

    out = sys.stdout
    out.write("qjsprof report: %s\n" % args.profile)
    out.write("  abi=%s  functions-with-ops=%d  total ops=%s (%d)\n"
              % (profmerge.ABI, nfunc, human(total), total))

    # Cumulative coverage is the whole point of a profile-guided AOT: how few
    # functions do we need to compile to cover most dispatches?
    cum = 0
    marks = {}
    for i, f in enumerate(order):
        cum += f.ops
        for pct in (50, 80, 90, 95, 99):
            if pct not in marks and cum >= total * pct / 100:
                marks[pct] = (i + 1, cum)
    out.write("  coverage:")
    for pct in (50, 80, 90, 95, 99):
        if pct in marks:
            n, c = marks[pct]
            out.write(" %d%%=%d fn" % (pct, n))
    out.write("\n\n")

    out.write("top %d by ops\n" % args.top)
    out.write("  %-4s %-34s %-28s %10s %8s %5s %8s\n"
              % ("#", "digest", "name", "ops", "entries", "len", "ops/ent"))
    for i, f in enumerate(order[:args.top]):
        name = (f.name or "<null>")[:27]
        loc = "%s:%d" % (f.file, f.line)
        ope = (f.ops / f.entries) if f.entries else 0
        out.write("  %-4d %-34s %-28s %10s %8d %5d %8.1f   %s\n"
                  % (i + 1, f.digest, name, human(f.ops), f.entries, f.len,
                     ope, loc))

    # "Tiny and hot": small bytecode that runs a lot is the easiest to inline.
    tiny = [f for f in order if f.len and f.len <= 64 and f.ops > 0]
    tiny.sort(key=lambda f: f.ops, reverse=True)
    out.write("\ntiny hot functions (len <= 64)\n")
    for f in tiny[:12]:
        out.write("  %-34s %-24s ops=%-9s len=%-4d entries=%d\n"
                  % (f.digest, (f.name or "<null>")[:23], human(f.ops),
                     f.len, f.entries))

    # Call graph aggregates.
    bc_edges = Counter()
    c_edges = Counter()
    for caller in order:
        for call in caller.calls.values():
            for cal in call.callees.values():
                if cal.kind == "bc":
                    bc_edges[(caller.digest, cal.name)] += cal.hits
                elif cal.kind == "c":
                    c_edges[(caller.digest, cal.name)] += cal.hits
    out.write("\ncall graph: %d JS->JS edges, %d JS->C edges\n"
              % (len(bc_edges), len(c_edges)))
    out.write("  hottest C builtins:\n")
    cbycallee = Counter()
    for (_caller, name), n in c_edges.items():
        cbycallee[name] += n
    for name, n in cbycallee.most_common(15):
        out.write("    %-30s %s\n" % (name, human(n)))

    if args.csv:
        with open(args.csv, "w") as fh:
            fh.write("rank,digest,name,file,line,ops,entries,len,ops_per_entry\n")
            for i, f in enumerate(order):
                fh.write("%d,%s,%s,%s,%d,%d,%d,%d,%.3f\n"
                         % (i + 1, f.digest, (f.name or "").replace(",", "_"),
                            f.file, f.line, f.ops, f.entries, f.len,
                            (f.ops / f.entries) if f.entries else 0))
        out.write("\nwrote %s\n" % args.csv)

    if args.dot:
        nodes = set()
        lines = ["digraph qjsprof {", "  rankdir=LR;",
                 '  node [shape=box,fontsize=9];']
        all_edges = []
        for (a, b), n in bc_edges.items():
            if n >= args.min_edges:
                all_edges.append((a, b, n, "bc"))
        for (a, b), n in c_edges.items():
            if n >= args.min_edges:
                all_edges.append((a, b, n, "c"))
        # Label nodes by their best-known function, digest otherwise.
        labels = {}
        for f in order:
            if f.ops:
                labels[f.digest] = "%s\\n%s" % ((f.name or "?"),
                                                f.digest[:8])
        for a, b, n, kind in all_edges:
            for x in (a, b):
                if x not in nodes:
                    nodes.add(x)
                    lines.append('  "%s" [label="%s"];'
                                 % (x, labels.get(x, x[:12])))
            style = ",style=dashed" if kind == "c" else ""
            lines.append('  "%s" -> "%s" [label="%s"%s];'
                         % (a, b, human(n), style))
        lines.append("}")
        with open(args.dot, "w") as fh:
            fh.write("\n".join(lines) + "\n")
        out.write("wrote %s\n" % args.dot)

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
