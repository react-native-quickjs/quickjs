#!/usr/bin/env python3
"""Per-function self interpreter time from a QJS_FNSAMPLE table.

    selfweights.py TABLE.txt --bin RUN-BINARY > weights.txt

TABLE comes from a plain (non-AOT) interpreter build run with QJS_FNSAMPLE_OUT.
Each output line is `<digest hex> <samples>`: the samples in which that
function was the innermost bytecode function and the program counter was in the
interpreter's own code (op handlers and the call machinery). Time in native
builtins, GC and the allocator is left out, because compiling the function
removes none of it. The result is what qjsaot --self-weights ranks by.
"""
import argparse
import collections
import sys

import fnspeed

SELF_BUCKETS = ('interp', 'call')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('table')
    ap.add_argument('--bin', required=True)
    ap.add_argument('--summary', action='store_true',
                    help='print bucket totals to stderr')
    args = ap.parse_args()
    table, ma = fnspeed.load(args.table)
    exe = fnspeed.symbol_table(args.bin)
    tabs = {'exe': (exe, [s[0] for s in exe])}
    extents = {'exe': fnspeed.mapped_size(args.bin)}
    dg_of = {}
    with open(args.table) as f:
        for line in f:
            p = line.split()
            if p and p[0] == 'F':
                key = None
                dg = None
                for t in p[3:]:
                    if t.startswith('key='):
                        key = int(t[4:])
                    elif t.startswith('dg='):
                        dg = t[3:]
                    elif '=' not in t:
                        break
                if key is not None and dg:
                    dg_of[key] = dg
    weights = collections.Counter()
    buckets = collections.Counter()
    for key, pc in ma['pcs']:
        objname, rel = fnspeed.object_for(ma['objs'], pc, extents)
        sym = None
        if objname == 'exe':
            tab, starts = tabs['exe']
            sym = fnspeed.resolve(tab, starts, rel)
        b = fnspeed.bucket_for(sym, False) if objname == 'exe' else 'other'
        buckets[b] += 1
        if b in SELF_BUCKETS and key in dg_of:
            weights[dg_of[key]] += 1
    for dg, n in weights.most_common():
        print(dg, n)
    if args.summary:
        tot = sum(buckets.values()) or 1
        for b, n in buckets.most_common():
            print('%-10s %8d %5.1f%%' % (b, n, 100.0 * n / tot), file=sys.stderr)


if __name__ == '__main__':
    main()
