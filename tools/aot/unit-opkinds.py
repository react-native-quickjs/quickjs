#!/usr/bin/env python3
"""Why one function is thousands of C lines: lines per bytecode instruction and per kind of op, from a unit generated with QJS_AOT_OP_MARK=1.

  QJS_AOT_OP_MARK=1 qjsaot ... --out u.c
  unit-opkinds.py u.c [--top N] [--fn NAMEPREFIX ...]

Every instruction of an ordinary (aot_s_/aot_f_) body is preceded by a `/*#pc ropname*/` marker. The lines up to the next marker are that
instruction's own code; an exit stub (the text after a `vx_N:`/`rx_N:`/... label) is charged to the op that first jumps to it. Lines are also
split by what they are: guard (a test that jumps to an exit), exit (stub), boxing (value construction/stores), other. Functions without markers
(region bodies) are classified by text only.
"""
import collections, importlib.util, os, re, sys

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('ub', os.path.join(here, 'unit-build.py'))
ub = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ub)

MARK = re.compile(r'^\s*/\*#(\d+) (\S+)\*/')
LABEL = re.compile(r'^\s*(rx|rp|vx|dx|ox|osr|ax|hx|g)_(\d+):;?\s*$')
BLOCK_LABEL = re.compile(r'^\s*(?:s_\d+|r\d+_B\d+|osr_\d+|osr_fail):')
GOTO = re.compile(r'\bgoto ((?:rx|rp|vx|dx|ox|osr|ax|hx|g)_\d+)\b')

KINDS = [
    ('field get/put', r'(get_field|put_field|get_this_field|put_this_field|get_length|get_var_ref|put_var_ref|define_field|get_private|put_private)'),
    ('element', r'(array_el|get_el|put_el|get_array|put_array|append|array_from|define_array|ta_|typed)'),
    ('call/construct', r'(call|construct|apply|new_|eval|tail)'),
    ('arith', r'^(add|sub|mul|div|mod|pow|neg|plus|inc|dec|shl|sar|shr|and|or|xor|not|lnot|typeof|fma|imul|math)'),
    ('compare/branch', r'(lt|lte|gt|gte|eq|neq|strict|branch|if_|goto|jump|cmp|select|instanceof|in_)'),
    ('local/const/move', r'(local|move|load|const|arg|get_loc|put_loc|undefined|null|true|false|this|dup|drop|swap|nip|set_)'),
    ('return/throw/other', r'(return|throw|yield|await|ret|catch|finally|gosub|with|for_|iterator|enum|special|check|init)'),
]


def kind_of(rop):
    r = rop.lower().replace('rop_', '')
    for name, pat in KINDS:
        if re.search(pat, r):
            return name
    return 'other'


def line_class(l, in_stub):
    if in_stub:
        return 'exit stub'
    if re.search(r'goto (?:rx|rp|vx|dx|ox|osr|ax|hx|g)_\d+', l):
        return 'guard'
    if re.search(r'AOT_SET_B|JS_MKVAL|JS_NewFloat64|q2_box|JS_NewInt|__JS_NewFloat64|AOT_REG\(|q2_dup|q2_free|JS_FreeValue|q2_put', l):
        return 'boxing/ownership'
    return 'other'


def main():
    path = sys.argv[1]
    top = int(sys.argv[sys.argv.index('--top') + 1]) if '--top' in sys.argv else 8
    only = []
    if '--fn' in sys.argv:
        i = sys.argv.index('--fn') + 1
        while i < len(sys.argv) and not sys.argv[i].startswith('--'):
            only.append(sys.argv[i]); i += 1
    src = open(path, errors='replace').read().split('\n')
    items = ub.parse_items(src)
    fns = []
    for it in items:
        if it.kind == 'code':
            ub.classify(it)
        if it.kind == 'fndef' and (it.name or '').startswith(('aot_s_', 'aot_f_')):
            fns.append(it)
    fns.sort(key=lambda it: -it.nlines)
    if only:
        fns = [it for it in fns if any(it.name.startswith(o) for o in only)]
    G_own = collections.Counter(); G_stub = collections.Counter(); G_n = collections.Counter()
    for it in (fns if '--byrop' in sys.argv else fns[:top]):
        lines = it.lines
        cur = None            # (pc, rop) of the op whose code we are in
        n_marks = 0
        by_op = collections.Counter()      # (pc, rop) -> own lines
        owner = {}                          # stub label -> (pc, rop)
        stub_lines = collections.Counter()  # (pc, rop) -> stub lines
        cls = collections.Counter()
        in_stub = None
        stub_label = None
        unattributed = 0
        pro = collections.Counter()
        for l in lines:
            m = MARK.match(l)
            if m:
                cur = (int(m.group(1)), m.group(2)); n_marks += 1
                in_stub = None
                continue
            ml = LABEL.match(l)
            if ml:
                in_stub = '%s_%s' % (ml.group(1), ml.group(2))
                continue
            if in_stub is not None:
                if l.startswith('}') or BLOCK_LABEL.match(l):
                    in_stub = None
                else:
                    stub_lines[owner.get(in_stub, (-1, 'unowned'))] += 1
                    cls['exit stub'] += 1
                    continue
            for g in GOTO.findall(l):
                owner.setdefault(g, cur if cur else (-1, 'unowned'))
            if cur is None:
                unattributed += 1
                if re.match(r'\s*JSValue v\d+ = ', l): pro['JSValue vN = JS_UNDEFINED'] += 1
                elif re.match(r'\s*const unsigned char \*pp\d+', l): pro['const unsigned char *ppN'] += 1
                elif re.match(r'\s*const void \*pme\d+', l): pro['const void *pmeN'] += 1
                elif re.match(r'\s*(int32_t|int64_t|double|uint32_t|int|JSValue|const void \*|void \*)\s*\**[a-z_]*\d+( = [^;]*)?;', l): pro['typed local decls (iN/dN/...)'] += 1
                else: pro['other prologue (entry, PIC, holders, fills)'] += 1
            by_op[cur if cur else (-1, 'prologue')] += 1
            cls[line_class(l, False)] += 1
        for (pc, rop), n in by_op.items():
            if pc >= 0: G_own[rop] += n; G_n[rop] += 1
        for (pc, rop), n in stub_lines.items():
            if pc >= 0: G_stub[rop] += n
        kinds = collections.Counter()
        for (pc, rop), n in by_op.items():
            kinds[kind_of(rop) if pc >= 0 else 'prologue/entry'] += n
        skinds = collections.Counter()
        for (pc, rop), n in stub_lines.items():
            skinds[kind_of(rop) if pc >= 0 else 'unowned'] += n
        total = it.nlines
        prologue_report = '   prologue (lines before the first instruction marker): ' + ', '.join('%s %d' % (k, v) for k, v in pro.most_common()) if pro else ''
        if '--byrop' in sys.argv: continue
        print('%s: %d lines, %d marked bytecode instructions%s' % (it.name, total, n_marks, (' => %.1f C lines per instruction' % (total / n_marks)) if n_marks else ' (no markers: region body)'))
        print('   by line class: ' + ', '.join('%s %d (%.0f%%)' % (k, v, 100.0 * v / max(1, sum(cls.values()))) for k, v in cls.most_common()))
        if n_marks:
            print('   by op kind (own code | exit stubs charged to it | total, lines per instruction):')
            ninsn = collections.Counter()
            for (pc, rop) in set(list(by_op.keys()) + list(stub_lines.keys())):
                if pc >= 0:
                    ninsn[kind_of(rop)] += 1
            for k in sorted(set(kinds) | set(skinds), key=lambda k: -(kinds[k] + skinds[k])):
                t = kinds[k] + skinds[k]
                print('     %-20s %6d | %6d | %6d  (%2d%%)  %d insns, %.1f lines/insn' % (k, kinds[k], skinds[k], t, 100.0 * t / total, ninsn[k], t / max(1, ninsn[k])))
            print(prologue_report) if prologue_report else None
            worst = collections.Counter()
            for key, n in by_op.items(): worst[key] += n
            for key, n in stub_lines.items(): worst[key] += n
            print('   worst ops: ' + ', '.join('%s@%d %d' % (r, p, n) for (p, r), n in worst.most_common(6)))


    if '--byrop' in sys.argv:
        print('%-22s %6s %8s %8s %8s' % ('op', 'insns', 'own', 'stubs', 'lines/insn'))
        for r, n in sorted(G_n.items(), key=lambda kv: -(G_own[kv[0]] + G_stub[kv[0]]))[:25]:
            print('%-22s %6d %8d %8d %8.1f' % (r, n, G_own[r], G_stub[r], (G_own[r] + G_stub[r]) / n))


main()
