#!/usr/bin/env python3
"""Census of the region bodies of a generated unit: which inlined callees make up each big region function, the exit stubs
(count, lines, how many are identical once pcs and ids are masked), and identical inlined copies of one callee.

  QJS_AOT_REGION_INSTS=1 qjsaot ... 2> gen.log        (prints one `region-inst` line per instance)
  inline-census.py u.c gen.log [--top N] [--min-lines N]

A line of a region body belongs to the instance named by the first r<id>_ register it mentions (lines without one, such as
labels and gotos, to the line before). `copies` is the number of instances of one callee (function digest) in the region,
`bytes-identical` the number of those whose lines, with instance ids and numbers masked, are the same text.
"""
import collections, importlib.util, os, re, sys

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('ub', os.path.join(here, 'unit-build.py'))
ub = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ub)

LABEL = re.compile(r'^\s*([a-z]+)_(\d+):;?\s*$')


def mask(l):
    l = re.sub(r'\br\d+_', 'rN_', l)
    l = re.sub(r'\b(?:v|b|g|rx|vx|dx|rp|ox|osr|ax|hx|c|s|k|n)_?\d+\b', 'X', l)
    return re.sub(r'\d+', 'N', l)


def mask_ids(l):
    """only the instance ids: what must be equal for two copies to be one function"""
    return re.sub(r'\br\d+_', 'rN_', l)


def main():
    path, log = sys.argv[1], sys.argv[2]
    top = int(sys.argv[sys.argv.index('--top') + 1]) if '--top' in sys.argv else 10
    minl = int(sys.argv[sys.argv.index('--min-lines') + 1]) if '--min-lines' in sys.argv else 3000
    insts = collections.defaultdict(dict)
    for l in open(log, errors='replace'):
        m = re.match(r'region-inst root=(\w+) id=(\d+) fn=(\w+) name=(\S+) line=(\d+) par=(-?\d+) depth=(\d+)', l)
        if m:
            insts[m.group(1)][int(m.group(2))] = (m.group(3), m.group(4), int(m.group(5)), int(m.group(6)))
    items = ub.parse_items(open(path, errors='replace').read().split('\n'))
    stub_total = collections.Counter()
    stub_lines = collections.Counter()
    stub_texts = collections.Counter()
    body_lines = 0
    fold_save = 0
    fold_copies = 0
    fold_ident = 0
    exact_save = 0
    exact_ident = 0
    for it in items:
        if it.kind == 'code':
            ub.classify(it)
        if it.kind != 'fndef' or not (it.name or '').startswith(('aot_f_', 'aot_s_')):
            continue
        root = it.owner
        body_lines += it.nlines
        # exit stubs: label up to the next label
        lines = it.lines
        i = 0
        cur = None
        for l in lines:
            m = LABEL.match(l)
            if m and m.group(1) in ('rx', 'rp', 'vx', 'dx', 'ox', 'osr', 'ax', 'hx'):
                if cur:
                    stub_total[cur[0]] += 1
                    stub_lines[cur[0]] += len(cur[1])
                    stub_texts[(cur[0], '\n'.join(mask(x) for x in cur[1]))] += 1
                cur = (m.group(1), [])
            elif m or l.startswith('}'):
                if cur:
                    stub_total[cur[0]] += 1
                    stub_lines[cur[0]] += len(cur[1])
                    stub_texts[(cur[0], '\n'.join(mask(x) for x in cur[1]))] += 1
                cur = None
            elif cur is not None:
                cur[1].append(l)
        if it.nlines < minl or root not in insts:
            continue
        by = collections.Counter()
        last = 0
        per_inst_text = collections.defaultdict(list)
        per_inst_exact = collections.defaultdict(list)
        for l in lines:
            m = re.search(r'\br(\d+)_', l)
            if m:
                last = int(m.group(1))
            by[last] += 1
            per_inst_text[last].append(mask(l))
            per_inst_exact[last].append(mask_ids(l))
        agg = collections.Counter()
        copies = collections.Counter()
        ident = collections.defaultdict(collections.Counter)
        exact = collections.defaultdict(collections.Counter)
        for iid, n in by.items():
            fn = insts[root].get(iid)
            key = (fn[1] + '@' + str(fn[2]) + ' ' + fn[0][:8]) if fn else 'root/unknown(%d)' % iid
            agg[key] += n
            copies[key] += 1
            ident[key][hash('\n'.join(per_inst_text[iid]))] += 1
            exact[key][hash('\n'.join(per_inst_exact[iid]))] += 1
            if fn:
                fold_copies += 1
        for key in agg:
            c, d = copies[key], len(ident[key])
            if c > d:
                fold_save += agg[key] * (c - d) // c
                fold_ident += c - d
            e = len(exact[key])
            if c > e:
                exact_save += agg[key] * (c - e) // c
                exact_ident += c - e
        print('%s: %d lines, %d instances' % (it.name, it.nlines, len(insts[root])))
        for key, n in agg.most_common(top):
            print('  %7d lines (%4.1f%%)  %-46s %3d copies, %d distinct texts' % (n, 100.0 * n / it.nlines, key, copies[key], len(ident[key])))
    print('EXACT (only instance ids masked): %d copies identical, about %d lines' % (exact_ident, exact_save))
    print('region bodies total: %d lines; instances in big regions %d, copies whose text equals another copy of the same callee: %d (folding them into one shared function saves about %d lines)' % (body_lines, fold_copies, fold_ident, fold_save))
    print('exit stubs in region bodies (label to next label):')
    tot_s = tot_l = 0
    for k, n in stub_total.most_common():
        print('  %-4s %6d stubs, %7d lines, %.1f lines/stub' % (k, n, stub_lines[k], stub_lines[k] / n))
        tot_s += n
        tot_l += stub_lines[k]
    distinct = len(stub_texts)
    dup_lines = sum(len(t.split('\n')) * (c - 1) for (k, t), c in stub_texts.items())
    print('  all  %6d stubs, %7d lines (%.1f%% of region bodies); %d distinct masked texts; folding identical ones saves %d lines' % (
        tot_s, tot_l, 100.0 * tot_l / max(1, body_lines), distinct, dup_lines))


main()
