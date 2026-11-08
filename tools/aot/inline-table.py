#!/usr/bin/env python3
"""Per-site inlining table of one generated unit: the C lines every inlined copy costs against how often its site ran.

  QJS_AOT_REGION_INSTS=1 QJS_AOT_INL_TABLE=1 QJS_AOT_SEL_LOG=1 qjsaot ... 2> gen.log
  inline-table.py u.c gen.log [--name PROG] [--rows FILE] [--saving S]

Rows (one per inlined copy): region copies come from the `region-inst` lines (instance r<id> in the C text), plain-body copies from the
`inl-copy` lines (the body carries a `/*@own pc*/` marker before every instruction). The lines of a copy are its own lines plus the exit
stubs its guards jump to. `sitecalls` is the call count of the site in the profile run, `rootent` the entries of the root function.
Summaries: waste buckets by site count, the worst lines per call, the what-if "keep a copy only if sitecalls * S >= lines * k", the
shared `ei_` inline bodies, and the selection reasons (--sel from the `sel` lines of the same log).
"""
import argparse, collections, importlib.util, os, re, sys

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('ub', os.path.join(here, 'unit-build.py'))
ub = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ub)

LABEL = re.compile(r'^\s*(rx|rp|vx|dx|ox|osr|ax|hx|g)_(\d+):;?\s*$')
BLOCK_LABEL = re.compile(r'^\s*(?:s_\d+|r\d+_B\d+):')


def kv(line):
    return dict(re.findall(r'(\w+)=(\S+)', line))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('unit')
    ap.add_argument('log')
    ap.add_argument('--name', default='')
    ap.add_argument('--rows', default=None)
    ap.add_argument('--saving', type=float, default=25.0, help='instructions a compiled call costs over an inlined one (per call)')
    args = ap.parse_args()
    src = open(args.unit, errors='replace').read().split('\n')
    items = ub.parse_items(src)
    for it in items:
        if it.kind == 'code':
            ub.classify(it)
    inst = collections.defaultdict(dict)    # region root hex -> id -> record
    copies = collections.defaultdict(dict)  # plain root hex -> k -> record
    sels = {}
    for l in open(args.log, errors='replace'):
        if l.startswith('region-inst '):
            d = kv(l)
            inst[d['root']][int(d['id'])] = d
        elif l.startswith('inl-copy '):
            d = kv(l)
            copies[d['root']][int(d['k'])] = d
        elif l.startswith('sel '):
            m = re.match(r'sel fn=(\w+) name=(\S+) line=(\d+) ops=(\d+) entries=(\d+) reason=(.*)$', l.strip())
            if m:
                sels[m.group(1)] = dict(name=m.group(2), line=int(m.group(3)), ops=int(m.group(4)), entries=int(m.group(5)), reason=m.group(6))
    # unit functions with bodies
    owned_lines = collections.Counter()
    bodies = {}
    ei_lines = {}
    for it in items:
        if it.kind == 'fndef' and it.owner:
            nm = it.name
            if nm.startswith(('aot_f_', 'aot_s_', 'aot_g_', 'aot_n_', 'aot_nx_', 'aot_nb_', 'aot_rx_', 'aot_nr_', 'aot_fs_')) or nm.startswith(('rg_', 'rgc_')):
                owned_lines[it.owner] += it.nlines
            if nm.startswith(('aot_f_', 'aot_s_')):
                bodies.setdefault(it.owner, []).append(it)
            if nm.startswith('ei_'):
                ei_lines[nm] = it.nlines
    in_unit = set(owned_lines)
    rows = []
    # ---- region copies
    for it_list in bodies.values():
        pass
    for root, its in bodies.items():
        for it in its:
            lines = it.lines
            has_reg = root in inst
            has_plain = root in copies
            if not has_reg and not has_plain:
                continue
            by_own = collections.Counter()
            own_stub = collections.Counter()
            stub_of = {}   # label index -> (kind, lines)
            cur_own = 0
            first_ref = {}  # stub index -> own of the line that first jumps to it
            stub_lines = []
            instub = None
            pc_own = {}
            # pass 1: attribute body lines
            last = 0
            body_owns = []
            for l in lines:
                if LABEL.match(l):
                    instub = LABEL.match(l)
                    stub_lines.append((instub.group(1), int(instub.group(2)), []))
                    continue
                if instub is not None:
                    if l.startswith('}') or BLOCK_LABEL.match(l):
                        instub = None
                    else:
                        stub_lines[-1][2].append(l)
                        continue
                if has_plain and not has_reg:
                    m = re.match(r'/\*@(\d+) (\d+)\*/', l)
                    if m:
                        cur_own = int(m.group(1)); pc_own[int(m.group(2))] = cur_own
                        continue
                    by_own[cur_own] += 1
                    for mm in re.finditer(r'\bgoto ((?:rx|rp|vx|dx|ox|osr|ax|hx|g)_\d+)\b', l):
                        first_ref.setdefault(mm.group(1), cur_own)
                else:
                    mm = re.search(r'\br(\d+)_', l)
                    if mm:
                        last = int(mm.group(1))
                    by_own[last] += 1
                    for mm in re.finditer(r'\bgoto ((?:rx|rp|vx|dx|ox|osr|ax|hx|g)_\d+)\b', l):
                        first_ref.setdefault(mm.group(1), last)
            for kind, idx, sl in stub_lines:
                if has_plain and not has_reg and kind not in ('rx', 'rp'):
                    owner = pc_own.get(idx, first_ref.get('%s_%d' % (kind, idx), 0))
                else:
                    owner = first_ref.get('%s_%d' % (kind, idx), 0)
                own_stub[owner] += len(sl) + 1
            recs = inst[root] if has_reg else copies[root]
            rootent = 0
            for k, d in recs.items():
                rootent = max(rootent, int(d.get('rootent', 0)))
            for k in sorted(set(by_own) | set(own_stub)):
                if k == 0 and not has_reg:
                    kind = 'root'
                elif has_reg and k == 0:
                    kind = 'root'
                else:
                    kind = 'region' if has_reg else 'plain'
                d = recs.get(k)
                if kind == 'root':
                    continue
                if d is None:
                    continue
                callee = d.get('fn') or d.get('callee')
                rows.append(dict(root=root, kind=kind, id=k, callee=callee, name=d.get('name'), line=int(d.get('line', 0)), depth=int(d.get('depth', 1)),
                                 own=by_own[k], stub=own_stub[k], lines=by_own[k] + own_stub[k],
                                 sitecalls=int(d.get('sitecalls', 0)), rootent=int(d.get('rootent', 0)), callee_entries=int(d.get('entries', 0)),
                                 in_unit=callee in in_unit, call_pc=int(d.get('call_pc', 0))))
    tot_lines = sum(r['lines'] for r in rows)
    print('== %s: %d lines in the unit; %d inlined copies (%d region, %d plain) holding %d lines (%.1f%%)' % (
        args.name, len(src), len(rows), sum(1 for r in rows if r['kind'] == 'region'), sum(1 for r in rows if r['kind'] == 'plain'), tot_lines,
        100.0 * tot_lines / max(1, len(src))))
    if args.rows:
        with open(args.rows, 'w') as f:
            f.write('\t'.join(['prog', 'root', 'kind', 'callee', 'line', 'depth', 'own', 'stub', 'lines', 'sitecalls', 'rootent', 'per_entry', 'callee_in_unit']) + '\n')
            for r in sorted(rows, key=lambda r: -r['lines']):
                f.write('\t'.join(str(x) for x in [args.name, r['root'][:8], r['kind'], r['callee'][:8], r['line'], r['depth'], r['own'], r['stub'], r['lines'],
                                                   r['sitecalls'], r['rootent'], '%.2f' % (r['sitecalls'] / max(1, r['rootent'])), int(r['in_unit'])]) + '\n')
    # ---- buckets
    edges = [(0, 0, '0'), (1, 10, '1-10'), (11, 100, '11-100'), (101, 1000, '101-1000'), (1001, 10**18, '>1000')]
    print('waste buckets (lines in copies by how often the site ran in the profile run):')
    for lo, hi, nm in edges:
        sel = [r for r in rows if lo <= r['sitecalls'] <= hi]
        print('  %-9s %5d copies %8d lines (%4.1f%% of copy lines)' % (nm, len(sel), sum(r['lines'] for r in sel), 100.0 * sum(r['lines'] for r in sel) / max(1, tot_lines)))
    cheap = [r for r in rows if r['sitecalls'] < r['lines']]
    print('  copies whose site ran fewer times than the copy has lines: %d copies, %d lines (%.1f%%)' % (len(cheap), sum(r['lines'] for r in cheap), 100.0 * sum(r['lines'] for r in cheap) / max(1, tot_lines)))
    print('top 20 by lines per call (worst first):')
    for r in sorted(rows, key=lambda r: -(r['lines'] / max(1, r['sitecalls'])))[:20]:
        print('  %-6s root %s callee %s@%d depth %d  %6d lines (own %d stub %d)  site %d  entries %d  %.1f lines/call%s' % (
            r['kind'], r['root'][:8], r['callee'][:8], r['line'], r['depth'], r['lines'], r['own'], r['stub'], r['sitecalls'], r['rootent'],
            r['lines'] / max(1, r['sitecalls']), '' if r['in_unit'] else '  [callee NOT in unit]'))
    print('what-if: keep a copy only if sitecalls * S >= lines * k   (S = %.0f instructions saved per inlined call)' % args.saving)
    for k in (1, 5, 20, 100):
        rem = [r for r in rows if r['sitecalls'] * args.saving < r['lines'] * k]
        print('  k=%-4d removes %5d of %d copies, %8d lines (%4.1f%% of the unit)' % (k, len(rem), len(rows), sum(r['lines'] for r in rem), 100.0 * sum(r['lines'] for r in rem) / max(1, len(src))))
    # ---- shared ei_ inline bodies
    ei_total = sum(ei_lines.values())
    print('shared ei_ inline bodies: %d functions, %d lines (%.1f%% of the unit); call sites in the text: %d' % (
        len(ei_lines), ei_total, 100.0 * ei_total / max(1, len(src)), sum(len(re.findall(r'\bei_[0-9a-f]{32}\w*\(', l)) for l in src)))
    # ---- selection
    if sels:
        print('selection (%d functions):' % len(sels))
        byr = collections.defaultdict(list)
        for h, d in sels.items():
            byr[d['reason'].split()[0]].append(h)
        copy_lines = collections.Counter()
        for r in rows:
            copy_lines[r['callee']] += r['lines']
        for rs, hs in sorted(byr.items()):
            own = sum(owned_lines[h] for h in hs)
            cp = sum(copy_lines[h] for h in hs)
            print('  %-8s %3d functions, own body lines %7d, lines of their inlined copies elsewhere %7d, ops %d' % (rs, len(hs), own, cp, sum(sels[h]['ops'] for h in hs)))
        print('  closure-added functions (worst first by own+copies lines):')
        add = [h for h, d in sels.items() if d['reason'].split()[0] != 'hot']
        for h in sorted(add, key=lambda h: -(owned_lines[h] + copy_lines[h]))[:12]:
            d = sels[h]
            print('    %s %-14s own %6d copies %6d  ops %-10d entries %-8d %s' % (h[:8], d['name'], owned_lines[h], copy_lines[h], d['ops'], d['entries'], d['reason']))


main()
