#!/usr/bin/env python3
"""Join a QJS_AOT_CENSUS log with a profile's execution counts.

    census.py CENSUS.txt PROFILE.qjsprof [--op get_field,...] [--top N]

Prints, per op, the executed-op share of each (emitter kind, marker signature),
weighting each (function, pc) by its profile execution count. A site lowered in
several bodies (inlined copies, spec and generic bodies) contributes its weight
split equally between the bodies.
"""
import argparse, bisect, collections, re, sys

def load_counts(path):
    runs = collections.defaultdict(list)
    names = {}
    with open(path, errors='replace') as f:
        for l in f:
            if l.startswith('N '):
                p = l.split()
                m = re.match(r'pc=(\d+)\.\.(\d+)', p[2])
                n = int(p[3][2:])
                runs[p[1]].append((int(m.group(1)), int(m.group(2)), n))
            elif l.startswith('F '):
                p = l.split()
                names[p[1]] = (p[2][5:], p[3][5:])
    return runs, names

def count_at(runs, dg, pc):
    t = 0
    for lo, hi, n in runs.get(dg, ()):
        if lo <= pc <= hi: t += n
    return t

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('census'); ap.add_argument('profile')
    ap.add_argument('--op', default=None); ap.add_argument('--top', type=int, default=12)
    ap.add_argument('--dump', default=None)
    a = ap.parse_args()
    runs, names = load_counts(a.profile)
    inst = collections.defaultdict(list)
    for l in open(a.census):
        p = l.split()
        if len(p) < 10 or p[0] != 'C': continue
        dg, pc, rop, own, fl, inl, nb, took, marks, kind = p[1], int(p[2]), p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10]
        inst[(dg, pc, rop)].append((kind, marks, own))
    agg = collections.defaultdict(float); tot = collections.defaultdict(float)
    for (dg, pc, rop), L in inst.items():
        w = count_at(runs, dg, pc)
        if w == 0: continue
        for kind, marks, own in L:
            agg[(rop, kind, marks)] += w / len(L); tot[rop] += w / len(L)
    grand = sum(tot.values())
    ops = a.op.split(',') if a.op else [o for o, _ in sorted(tot.items(), key=lambda x: -x[1])[:25]]
    print('total weighted lowered ops %.3e' % grand)
    for o in ops:
        print('\n== %s  (%.2f%% of executed ops, %.3e)' % (o, 100 * tot[o] / grand, tot[o]))
        rows = sorted(((v, k) for k, v in agg.items() if k[0] == o), reverse=True)
        for v, (rop, kind, marks) in rows[:a.top]:
            print('  %6.2f%%  %-7s %s' % (100 * v / tot[o], kind, marks[:110]))
if __name__ == '__main__': main()
