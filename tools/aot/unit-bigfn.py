#!/usr/bin/env python3
"""Largest generated C functions of a unit: size, inlined instances, top callees by lines.

  QJS_AOT_REGION_INSTS=1 QJS_AOT_INL_TABLE=1 QJS_AOT_SEL_LOG=1 qjsaot ... 2> gen.log
  inline-table.py u.c gen.log --name P --rows rows.tsv > /dev/null
  unit-bigfn.py u.c gen.log rows.tsv [--top 10]

For every C function of the unit (brace-depth items of unit-build.py): lines, and for a region/inlined root the number of inlined copies
and the three callees holding most lines in it. Prints the counts of functions over 1k, 3k and 10k lines.
"""
import collections, importlib.util, os, re, sys

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('ub', os.path.join(here, 'unit-build.py'))
ub = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ub)


def main():
    path, log, rows = sys.argv[1], sys.argv[2], sys.argv[3]
    top = int(sys.argv[sys.argv.index('--top') + 1]) if '--top' in sys.argv else 10
    names = {}
    for l in open(log, errors='replace'):
        m = re.match(r'sel fn=(\w+) name=(\S+) ', l)
        if m:
            names[m.group(1)] = m.group(2)
        m = re.match(r'region-inst root=(\w+) id=\d+ fn=(\w+) name=(\S+)', l)
        if m and m.group(2) not in names:
            names[m.group(2)] = m.group(3)
    by_root = collections.defaultdict(lambda: [0, collections.Counter()])
    first = True
    for l in open(rows):
        if first:
            first = False
            continue
        f = l.rstrip('\n').split('\t')
        root, callee, lines = f[1], f[3], int(f[8])
        by_root[root][0] += 1
        by_root[root][1][callee] += lines
    src = open(path, errors='replace').read().split('\n')
    items = ub.parse_items(src)
    fns = []
    for it in items:
        if it.kind == 'code':
            ub.classify(it)
        if it.kind == 'fndef':
            fns.append(it)
    fns.sort(key=lambda it: -it.nlines)
    n1 = sum(1 for it in fns if it.nlines > 1000)
    n3 = sum(1 for it in fns if it.nlines > 3000)
    n10 = sum(1 for it in fns if it.nlines > 10000)
    print('%s: %d lines, %d C functions; over 1k lines: %d, over 3k: %d, over 10k: %d' % (path, len(src), len(fns), n1, n3, n10))
    for it in fns[:top]:
        own = it.owner or ''
        info = by_root.get(own[:8]) if own else None
        extra = ''
        if info:
            tc = ', '.join('%s %d' % (names.get(next((h for h in names if h.startswith(c)), ''), c), n) for c, n in info[1].most_common(3))
            extra = '  inlined copies %d; top callees by lines: %s' % (info[0], tc)
        print('  %7d lines  %-44s %s%s' % (it.nlines, it.name, names.get(own, '') if own else '', extra))


main()
