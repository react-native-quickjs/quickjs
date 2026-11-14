#!/usr/bin/env python3
"""Fact census report: census_report.py CENSUS PROFILE ROW > row.md   (see census.py for the log)"""
import collections, re, sys
cen, prof, row = sys.argv[1:4]

runs = collections.defaultdict(list); S = {}; C = collections.defaultdict(list); A = {}; names = {}
for l in open(prof, errors='replace'):
    t = l[:2]
    if t == 'N ':
        p = l.split(); m = re.match(r'pc=(\d+)\.\.(\d+)', p[2]); runs[p[1]].append((int(m.group(1)), int(m.group(2)), int(p[3][2:])))
    elif t == 'F ':
        p = l.split(); mm = re.match(r'name=(.*) file=(.*):(\d+)$', ' '.join(p[2:4])) 
        names[p[1]] = (p[2][5:], p[3][5:])
    elif t == 'S ':
        p = l.split(); d = dict(x.split('=', 1) for x in p[2:] if '=' in x)
        n = int(d.get('shapes', 0)); slots = set(); hits = []
        for i in range(n):
            if 'slot%d' % i in d: slots.add(d['slot%d' % i])
            hits.append(int(d.get('hits%d' % i, d.get('hits', 0)) or 0)) if i == 0 else None
        S[(p[1], int(d['pc']))] = (n, slots, int(d.get('count', 0)))
    elif t == 'C ':
        p = l.split(); d = dict(x.split('=', 1) for x in p[2:] if '=' in x)
        C[(p[1], int(d['pc']))].append((d['callee'], int(d.get('hits', 0))))
    elif t == 'A ':
        p = l.split(); d = dict(x.split('=', 1) for x in p[2:] if '=' in x)
        A[(p[1], int(d['pc']))] = (int(d['own']), int(d['add']), int(d['other']))

def cnt(dg, pc):
    return sum(n for lo, hi, n in runs.get(dg, ()) if lo <= pc <= hi)

inst = collections.defaultdict(list)
for l in open(cen):
    p = l.split()
    if len(p) < 11 or p[0] != 'C': continue
    inst[(p[1], int(p[2]), p[3])].append((p[10], p[9]))

def outcome(rop, kind, marks):
    m = set(marks.split(','))
    if rop.startswith(('get_field', 'get_this_field', 'get_arg_field', 'get_local_field')):
        if kind == 'OPCALL': return 'runtime: IC read + step-op fallback' if 'q2_own_read_hit' in m else 'runtime: step op (no inline read)'
        if marks == '-': return 'eliminated (value forwarded, no code)'
        if 'q2_poly' in m: return 'poly guard (several shapes)'
        if 'q2_lmt_hit' in m or 'q2_slt_off' in m or 'aoff_' in m: return 'class/method table read (guarded by table)'
        if 'q2_slotpp' in m or 'q2_propp' in m: return 'grouped: one shape guard, offset reads'
        if 'q2_shape_ne' in m: return 'single shape guard + fixed offset'
        if 'q2_slotp' in m: return 'proven receiver, no guard at site'
        return 'other typed'
    if rop in ('call', 'call_method', 'construct'):
        if kind == 'OPCALL': return 'runtime: step-op call'
        if 'aot_n_' in m: return 'direct call to compiled target (guarded by function identity)'
        if 'JS_Call(' in m: return 'generic JS_Call (callee unresolved at compile time)'
        if marks == '-': return 'eliminated/inlined (no call code)'
        return 'other typed call'
    if rop in ('put_field', 'put_element', 'put_element_xfer', 'put_array_el'):
        if kind == 'OPCALL': return 'runtime: step-op store'
        if marks == '-': return 'eliminated (no code)'
        if 'q2_shape_ne' in m: return 'shape guard + fixed offset store'
        if 'q2_slotp' in m or 'q2_slotpp' in m: return 'proven receiver store, no guard at site'
        return 'other typed store'
    if kind == 'OPCALL': return 'runtime: step op'
    return 'typed'

FAM = collections.OrderedDict([
    ('property reads', ('get_field', 'get_field2', 'get_this_field', 'get_this_field2', 'get_arg_field', 'get_field_chain', 'get_local_field_0', 'get_local_field_1', 'get_local_field_2', 'get_local_field_3', 'get_local_field')),
    ('element/length reads', ('get_element', 'get_length', 'get_arg_el')),
    ('stores', ('put_field', 'put_element', 'put_element_xfer', 'put_array_el')),
    ('calls', ('call', 'call_method', 'construct')),
    ('closure variable reads', ('get_var_ref',)),
])
tabs = {k: collections.Counter() for k in FAM}; pot = {k: collections.defaultdict(collections.Counter) for k in FAM}
tot_all = 0; fam_tot = collections.Counter(); site_rows = collections.defaultdict(list)
for (dg, pc, rop), L in inst.items():
    w = cnt(dg, pc)
    if w == 0: continue
    tot_all += w
    for fam, ops in FAM.items():
        if rop in ops or (fam == 'property reads' and rop.startswith('get_field')):
            fam_tot[fam] += w
            for kind, marks in L:
                oc = outcome(rop, kind, marks)
                tabs[fam][oc] += w / len(L)
                # profile resolvability of the same site
                if fam in ('property reads',):
                    s = S.get((dg, pc))
                    pk = 'no S record' if s is None else ('1 shape' if s[0] == 1 else ('2-4 shapes, same slot' if 2 <= s[0] <= 4 and len(s[1]) == 1 else ('2-4 shapes, different slots' if 2 <= s[0] <= 4 else ('>4 shapes (megamorphic)' if s[0] > 4 else '0 shapes'))))
                    pot[fam][oc][pk] += w / len(L)
                    if oc.startswith('runtime'): site_rows[fam].append((w / len(L), dg, pc, rop, oc, pk))
                if fam == 'calls':
                    cs = C.get((dg, pc), [])
                    tot_h = sum(h for _, h in cs) or 1
                    top = max(cs, key=lambda x: x[1]) if cs else None
                    if top is None: pk = 'no C record'
                    elif top[0].startswith('c:'): pk = 'builtin C target' if top[1] * 100 >= 99 * tot_h else 'polymorphic (builtin dominant)'
                    elif top[0].startswith('thru'): pk = 'thru'
                    elif top[1] * 100 >= 99 * tot_h: pk = '1 bytecode target (>=99%)'
                    elif len(cs) <= 4: pk = '2-4 bytecode targets'
                    else: pk = '>4 targets (megamorphic)'
                    pot[fam][oc][pk] += w / len(L)
                    if oc.startswith(('runtime', 'generic')): site_rows[fam].append((w / len(L), dg, pc, rop, oc, pk))
                if fam == 'stores' and (dg, pc) in A:
                    o, a, x = A[(dg, pc)]; tt = (o + a + x) or 1
                    pot[fam][oc]['site adds %.0f%%' % (100 * a / tt // 25 * 25)] += w / len(L)
            break

def pct(x, t): return '%5.1f%%' % (100.0 * x / t) if t else '   n/a'
out = []
out.append('# Fact census: %s' % row)
out.append('Executed-op weights come from the profile N records (per-pc execution counts) over every lowered instruction in the unit '
           '(hot-cover 0.99), %.3e executed ops in lowered functions. A site lowered in several bodies splits its weight equally. '
           '"Outcome" is what the generator emitted (read from its own output text); "profile" is what the run observed at the same site.' % tot_all)
out.append('Fact sources NOT separable here: TS types are not wired into the generator (no TS input found in qjsaot sources); the bundle class (rg_recv_origin / qjs_aot_bundle_site_field) lives in the region IR only, so "proven receiver, no guard at site" is the footprint of bundle/region facts plus guards established earlier in the same group.')
for fam in FAM:
    t = fam_tot[fam]
    if not t: continue
    out.append('\n## %s  (%.1f%% of executed ops, %.3e)' % (fam, 100 * t / tot_all, t))
    out.append('| outcome | share of family | profile view of the same sites |')
    out.append('|---|---:|---|')
    for oc, v in tabs[fam].most_common():
        sub = ', '.join('%s %s' % (k, pct(x, v).strip()) for k, x in pot[fam][oc].most_common(5)) if pot[fam][oc] else ''
        out.append('| %s | %s | %s |' % (oc, pct(v, t), sub))
    rows = sorted(site_rows[fam], reverse=True)[:20]
    if rows:
        out.append('\nTop 20 unresolved/runtime sites (weight = executed ops, %% of family):')
        out.append('| %% | function | pc | op | outcome | profile |'); out.append('|--:|---|--:|---|---|---|')
        for w, dg, pc, rop, oc, pk in rows:
            nm = names.get(dg, ('?', '?'))
            out.append('| %.2f | %s %s | %d | %s | %s | %s |' % (100 * w / t, nm[0], nm[1], pc, rop, oc, pk))
# builtin callees by hits
bc = collections.Counter()
for (dg, pc), cs in C.items():
    for cal, h in cs:
        if cal.startswith('c:'): bc[cal.split(':')[1]] += h
tot_b = sum(bc.values())
out.append('\n## builtin calls by name (profile call hits; time inside = perf symbol share if given in the perf file)')
out.append('| builtin | call hits | share of builtin calls |'); out.append('|---|--:|--:|')
for n, h in bc.most_common(25): out.append('| %s | %d | %s |' % (n, h, pct(h, tot_b)))
print('\n'.join(out))
