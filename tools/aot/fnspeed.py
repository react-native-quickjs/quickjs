#!/usr/bin/env python3
"""Join two QJS_FNSAMPLE_OUT tables and print the hot set with speedups.

    fnspeed.py INTERP.txt AOT.txt [--work-i X] [--work-a Y] --bin BINARY
               [--unit UNIT.c] [--names FILE]

Each table is one line per sampled function, plus a record of every sample's
program counter:

    T <total> <nojs>          sample counts
    W <seconds>               wall time the table covers
    B <hex>                   the executable's load base
    F <samples> <in_unit> key=<k> aot=<0|1> <name> <file>:<line>:<col>
    P <key> <pc hex>          one per sample, key 0xffffffff for no-JS
    O <dropped>               samples the PC ring could not hold

A function's time share is `samples(f) / total * work`, where work is the run's
cost per unit of finished work: 1/score for an Octane benchmark, ms per world
for box2d.

Why the program counters are kept as well as the function: "outside the
generated text" is not one thing. It is the AOT helper functions in the engine
(which a better emitter removes), the interpreter (which runs a function the
emitter refused or switched off, which more lowering fixes), and genuine engine work such
as GC or allocation (which no emitter reaches). Only the last is
"runtime-bound", so each sample's PC is resolved to a symbol and bucketed.

Samples inside the unit are charged to a function by address rather than by the
frame that `regvm_cur` points at, because a frameless function has no
frame of its own and its time would otherwise be charged to its caller.
"""
import argparse
import bisect
import collections
import re
import subprocess
import sys

ANON = '<anon>'

# The register interpreter's own code: its run loop, one handler per op, the
# frame setup and the step fallback. Time here is the interpreter executing a
# function the emitter did not compile, so it is "not compiled" rather than
# engine work. `regvm_aot_*` is the AOT helper path and is deliberately not in
# this list; it matches `aot-helper` first.
# What is actually the interpreter running a function the unit did not compile:
# the monolith and the threaded handlers. The step forms (`regvm_s_*`) are *not*
# here -- compiled code calls them for every op call -- and neither is the call
# entry, which has its own bucket.
INTERP_PREFIXES = ('regvm_t_', 'regvm_cmp_', 'regvm_iter_', 'regvm_light_',
                   'regvm_get_field_ic', 'regvm_put_field', 'regvm_own_',
                   'l_rop_')

# The register-VM compiler and the parser run once, at startup. They are not
# the interpreter executing a function the emitter skipped, so charging them to
# `interp` would mislabel startup as "this function is not compiled" — on box2d
# they are 31 % of the interpreter bucket. They are engine work the emitter
# does not touch, so they belong with the other engine internals.
COMPILER_PREFIXES = ('js_regvm_build', 'regvm_sim_', 'js_regvm_encode',
                     'regvm_encode', 'regvm_own_analyze', 'regvm_effect',
                     'regvm_slot_operand', 'regvm_transfer_', 'regvm_perm_of',
                     'regvm_cfg', 'js_parse_', 'parse_', 'next_token',
                     'resolve_labels', 'resolve_scope_var',
                     'resolve_variables', 'compute_stack_size', 'js_parse')
# A sample in the compiler is engine time in the same sense as a builtin.
COMPILER_BUCKET = 'builtin'

# Bucket rules, checked top to bottom; the first match wins. `unit` is decided
# by the symbol being a unit function, before any of these.
AOT_HELPER_PREFIXES = ('qjs_aot_', 'js_aot_', 'regvm_aot_')
GC_PREFIXES = ('gc_', 'js_drc_', 'mark_', 'free_', 'sweep_', 'js_gc_')
GC_EXACT = {'__JS_FreeValueRT', '__JS_FreeValueRT1'}
ALLOC_EXACT = {'malloc', 'free', 'realloc', 'calloc', 'memalign',
               'posix_memalign', 'aligned_alloc', 'JS_NewArray',
               'JS_NewPlainObject'}
ALLOC_PREFIXES = ('js_malloc', 'js_def_malloc', 'js_realloc', 'js_calloc',
                  'js_alloc', 'js_new_', 'JS_NewObject')
BUILTIN_PREFIXES = ('js_', 'JS_', 'lre_', 'lre', 'unicode_')
# A sample inside libc/libm but between the functions their symbol tables name.
# Those objects are the allocator and the maths library, so the bucket follows
# the object rather than a symbol.
OBJECT_BUCKET = {'__libc_internal': 'alloc', '__libm_internal': 'builtin'}


def bucket_for(sym, in_unit):
    """The bucket a symbol belongs to, by symbol name."""
    if in_unit:
        return 'unit'
    if sym is None:
        return 'other'
    if sym in OBJECT_BUCKET:
        return OBJECT_BUCKET[sym]
    if sym.startswith(STEP_PREFIXES):
        # One label per opcode: an op's share here is its op-call weight, which
        # is the number that ranks which ops are worth lowering next.
        return 'aot-helper'
    if sym in CALL_EXACT or sym.startswith(CALL_PREFIXES):
        return 'call'
    if sym.startswith(AOT_HELPER_PREFIXES):
        return 'aot-helper'
    if sym in GC_EXACT or sym.startswith(GC_PREFIXES):
        return 'gc'
    if sym in ALLOC_EXACT or sym.startswith(ALLOC_PREFIXES):
        return 'alloc'
    if sym.startswith(COMPILER_PREFIXES):
        return COMPILER_BUCKET
    if sym.startswith(INTERP_PREFIXES):
        return 'interp'
    if sym.startswith(BUILTIN_PREFIXES):
        return 'builtin'
    return 'other'


BUCKETS = ('unit', 'aot-helper', 'call', 'interp', 'gc', 'alloc', 'builtin',
           'other')

# The step forms: one per opcode, entered from compiled code for every op it
# lowers as an op call. Their share *is* the op-call weight, reported per op as
# `step:<op>`.
STEP_PREFIXES = ('regvm_s_', 'regvm_step')

# Call entry and the frame machinery around it, split from `interp`: a call is
# not interpreter work, and lumping the two hid where the time went.
CALL_EXACT = ('js_regvm_execute', 'JS_CallInternal', 'regvm_call',
              'regvm_return')
CALL_PREFIXES = ('regvm_call', 'qjs_aot_call', 'js_call_internal')

# A unit symbol names the function it belongs to: aot_f_HEX, aot_g_HEX,
# aot_n_HEX (the native entry) and aot_nx_HEX (a frameless cold exit).
UNIT_SYM = re.compile(r'^aot_(?:f|g|s|n|nx)_([0-9a-f]{32})$')


def load(path):
    """Parse one sample table."""
    table = collections.defaultdict(lambda: [0, 0])
    meta = {'total': 0, 'nojs': 0, 'wall': 0.0, 'base': 0, 'drop': 0,
            'objs': [], 'keys': {}, 'pcs': []}
    with open(path) as f:
        for line in f:
            p = line.split()
            if not p:
                continue
            k = p[0]
            if k == 'W':
                meta['wall'] = float(p[1])
            elif k == 'T':
                meta['total'] = int(p[1])
                meta['nojs'] = int(p[2]) if len(p) > 2 else 0
            elif k == 'B':
                # One line per mapped object: the executable first, then each
                # shared library, so a sample in libc resolves against libc.
                meta['objs'].append((int(p[1], 16),
                                     p[2] if len(p) > 2 else 'exe'))
                if not meta['base']:
                    meta['base'] = int(p[1], 16)
            elif k == 'O':
                meta['drop'] = int(p[1])
            elif k == 'F':
                samples, in_unit = int(p[1]), int(p[2])
                key = 0
                state = 'unknown'
                rest = p[3:]
                while rest and '=' in rest[0]:
                    n, v = rest[0].split('=', 1)
                    if n == 'key':
                        key = int(v)
                    elif n == 'state':
                        state = v
                    elif n == 'aot':          # an older table
                        state = 'bound' if v == '1' else 'absent'
                    rest = rest[1:]
                name = rest[0] if rest else ANON
                pos = rest[1] if len(rest) > 1 else ''
                ent = table[(name, pos)]
                ent[0] += samples
                ent[1] += in_unit
                meta['keys'][key] = dict(name=name, pos=pos, state=state)
            elif k == 'P':
                meta['pcs'].append((int(p[1]), int(p[2], 16)))
    return table, meta


def mapped_size(binary):
    """The address span of an object's loadable segments, or 0.

    A symbol table understates the object: libc's text runs past its last sized
    function, so a sample there would look like it belonged to the next object.
    The program headers give the span the loader mapped."""
    try:
        out = subprocess.run(['readelf', '-lW', binary], capture_output=True,
                             text=True, check=True).stdout
    except (subprocess.CalledProcessError, FileNotFoundError):
        return 0
    hi = 0
    for line in out.splitlines():
        p = line.split()
        if len(p) > 5 and p[0] == 'LOAD':
            try:
                vaddr, _memsz = int(p[2], 16), int(p[5], 16)
            except ValueError:
                continue
            hi = max(hi, vaddr + _memsz)
    return hi


def symbol_table(binary, dynamic=False):
    """[(start, end, name)] for every defined symbol, sorted by address.

    A shared library keeps only its dynamic symbols, so it needs `-D`; the
    executable has a full table and keeps its local (static) functions, which
    is what lets an engine helper be named at all."""
    if dynamic:
        # A shared library's dynamic symbols may carry no size, so an interval
        # built from consecutive addresses charges a sample to the previous
        # symbol. readelf reports the size the linker recorded, which makes the
        # lookup exact.
        try:
            out = subprocess.run(['readelf', '-sW', binary], capture_output=True,
                                 text=True, check=True).stdout
        except (subprocess.CalledProcessError, FileNotFoundError):
            return []
        syms = []
        for line in out.splitlines():
            p = line.split()
            if len(p) < 8 or p[3] != 'FUNC':
                continue
            try:
                addr = int(p[1], 16)
                size = int(p[2])
            except ValueError:
                continue
            name = p[7].split('@')[0]
            syms.append((addr, addr + max(size, 0), name))
        syms.sort()
        return syms
    cmd = ['nm', '-n', '--defined-only', binary]
    try:
        out = subprocess.run(cmd, capture_output=True, text=True,
                             check=True).stdout
    except (subprocess.CalledProcessError, FileNotFoundError) as e:
        print('fnspeed: cannot read symbols from %s: %s' % (binary, e),
              file=sys.stderr)
        return []
    syms = []
    for line in out.splitlines():
        p = line.split()
        if len(p) < 3:
            continue
        try:
            addr = int(p[0], 16)
        except ValueError:
            continue
        syms.append((addr, p[2]))
    syms.sort()
    out = []
    for i, (addr, name) in enumerate(syms):
        end = syms[i + 1][0] if i + 1 < len(syms) else addr + 0x100000
        out.append((addr, end, name))
    return out


def resolve(syms, starts, addr):
    """The symbol containing `addr`, or None."""
    i = bisect.bisect_right(starts, addr) - 1
    if i < 0:
        return None
    s, e, name = syms[i]
    return name if s <= addr < e else None


def object_for(objs, pc, extents):
    """The mapped object a program counter falls in, and its offset.

    Choosing the highest base at or below the PC is wrong: the loader can map a
    shared library below the executable, and then an executable address looks
    like a library one. An object's extent — its base plus the highest symbol
    offset — decides instead."""
    for base, name in objs:
        if base <= pc < base + extents.get(name, 0):
            return name, pc - base
    # Nothing claims it: fall back to the nearest base below, so the sample is
    # still attributed to an object rather than silently dropped.
    best = None
    for base, name in objs:
        if pc >= base and (best is None or base > best[0]):
            best = (base, name)
    if best is None:
        return None, 0
    return best[1], pc - best[0]


def unit_functions(unit_c):
    """HEX -> source position, from the position comments the emitter writes."""
    out = {}
    if not unit_c:
        return out
    try:
        text = open(unit_c, errors='replace').read()
    except OSError:
        return out
    # The emitter writes each function's position above its definition:
    #   /* with_call callnil.js:5:1 */
    #   E_TEXT static JSValue aot_f_<hex>(...)
    pat = (r'/\*\s*\S+\s+(\S+:\d+:\d+)\s*\*/'
           r'(?:[^\n]*\n)*?\s*(?:E_TEXT\s+)?static\s+JSValue'
           r'\s+aot_[fgns]_([0-9a-f]{32})\(')
    for m in re.finditer(pat, text):
        out.setdefault(m.group(2), m.group(1))
    return out


def poskey(pos):
    """A source position key that ignores the path prefix.

    The sampler records the bytecode's `file:line:col` with whatever path the
    script was loaded by, and the emitter writes the basename it was invoked
    with, so the two differ by a prefix. Line and column are what identify the
    function; a script compiled twice under different paths would collide, and
    one unit is built per script, so that cannot happen here."""
    m = re.search(r'([^/:\\]+):(\d+):(\d+)$', pos)
    return '%s:%s:%s' % (m.group(1), m.group(2), m.group(3)) if m else pos


def parse_stats(path):
    """The `QJS_AOT_STATS=1` line, as a name -> int dict, or {}."""
    try:
        text = open(path).read()
    except OSError:
        return {}
    m = re.search(r'unit=(\d+) table=(\d+) bound=(\d+) enter=(\d+).*?'
                  r'disabled=(\d+) deopts=(\d+)', text)
    if not m:
        return {}
    keys = ('unit', 'table', 'bound', 'enter', 'disabled', 'deopts')
    return dict(zip(keys, (int(g) for g in m.groups())))


def label(key, names):
    name, pos = key
    if name not in (ANON, '<null>', ''):
        return name
    return names.get(pos, pos or ANON)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('interp')
    ap.add_argument('aot')
    ap.add_argument('--work-i', type=float, default=None)
    ap.add_argument('--work-a', type=float, default=None)
    ap.add_argument('--bin', default=None,
                    help='the timed binary, for nm; enables the buckets')
    ap.add_argument('--unit', default=None,
                    help='the unit source, to map a HEX to its position')
    ap.add_argument('--names', default=None,
                    help='optional `pos name` lines, to label anonymous functions')
    ap.add_argument('--min-share', type=float, default=0.01)
    ap.add_argument('--cover', type=float, default=0.80)
    ap.add_argument('--top', type=int, default=20)
    ap.add_argument('--buckets', action='store_true',
                    help='print every bucket for the hot functions')
    ap.add_argument('--stats', default=None,
                    help='a file holding the QJS_AOT_STATS=1 line, to check the '
                         'number of disabled functions against')
    args = ap.parse_args()

    names = {}
    if args.names:
        with open(args.names) as f:
            for line in f:
                p = line.split()
                if len(p) >= 2:
                    names[p[0]] = p[1]

    ti, mi = load(args.interp)
    ta, ma = load(args.aot)
    if mi['total'] == 0 or ma['total'] == 0:
        print('fnspeed: empty sample table; is QJS_FNSAMPLE_OUT set and the '
              'build made with QJS_FNSAMPLE=1?', file=sys.stderr)
        return 2
    wi = args.work_i if args.work_i is not None else 1.0
    wa = args.work_a if args.work_a is not None else wi

    # ---- buckets and PC attribution -------------------------------------
    # Resolve against every mapped object: the executable with its full
    # symbol table, and each shared library with its dynamic one. Allocator
    # time is in libc, so leaving shared libraries out would hide it under
    # "other" and make the allocation bucket read low.
    tabs = []
    extents = {}
    if args.bin:
        exe = symbol_table(args.bin)
        tabs.append(('exe', exe, [s[0] for s in exe]))
        for base, name in ma['objs'][1:]:
            # `exe` is the executable itself, already tabulated from --bin; a
            # vDSO has no file on disk to read symbols from.
            if name in ('exe', 'linux-vdso.so.1') or name.startswith('['):
                continue
            tab = symbol_table(name, dynamic=True)
            if tab:
                tabs.append((name, tab, [s[0] for s in tab]))
        # The executable's span comes from its own segments too.
        extents = {'exe': mapped_size(args.bin)}
        for name, tab, _ in tabs:
            if name != 'exe':
                extents[name] = mapped_size(name) or (tab[-1][1] if tab else 0)
        tabs.sort(key=lambda t: 0 if t[0] == 'exe' else 1)
    hexpos = unit_functions(args.unit)
    pos_to_hex = {poskey(v): k for k, v in hexpos.items()}
    per_key = collections.defaultdict(collections.Counter)
    sym_count = collections.Counter()   # resolved symbol, for the top lists
    pc_hex = collections.Counter()      # HEX the PC named
    pc_frame = collections.Counter()    # HEX the frame named, same samples
    unresolved = shared_lib = 0
    for key, pc in ma['pcs']:
        sym = None
        objname, rel = object_for(ma['objs'], pc, extents)
        for want, tab, starts in tabs:
            if want != objname:
                continue
            sym = resolve(tab, starts, rel)
            break
        if sym is None and tabs:
            # Inside a mapped object but between the functions its symbol table
            # names: libc's unsized internals and its ifunc variants are the
            # usual case. The object's own kind is the best answer available,
            # and it is a better one than "other".
            base_name = (objname or '').split('/')[-1]
            if base_name.startswith('libc') or base_name.startswith('libm'):
                sym = ('__libc_internal' if base_name.startswith('libc')
                       else '__libm_internal')
            elif objname not in (None, 'exe'):
                shared_lib += 1
            else:
                unresolved += 1
        m = UNIT_SYM.match(sym) if sym else None
        b = bucket_for(sym, m is not None)
        sym_count[(sym or '?', b)] += 1
        if key == 0xffffffff:
            per_key['<no-js>'][b] += 1
            continue
        metakey = ma['keys'].get(key)
        if metakey is None:
            continue
        tk = (metakey['name'], metakey['pos'])
        per_key[tk][b] += 1
        if m is not None:
            pc_hex[m.group(1)] += 1
            fh = pos_to_hex.get(poskey(metakey['pos']))
            if fh is not None:
                pc_frame[fh] += 1

    rows = []
    for key, (s, _u) in ti.items():
        sa, _ua = ta.get(key, (0, 0))
        ua = per_key.get(key, {}).get('unit', 0)  # by the sampled PC's symbol
        sh = s / mi['total']
        sh_a = sa / ma['total'] if ma['total'] else 0.0
        rows.append((key, s, sa, sh, sh_a, sh * wi, sh_a * wa, ua))
    rows.sort(key=lambda r: -r[3])

    hot, acc = [], 0.0
    for r in rows:
        if acc >= args.cover or len(hot) >= args.top:
            break
        if r[3] < args.min_share:
            continue
        hot.append(r)
        acc += r[3]

    def classify(key, sa):
        """(label, bucket fractions) for one function."""
        if sa == 0:
            return None, {}
        c = per_key.get(key)
        if not c:
            return None, {}
        frac = {b: c[b] / sa for b in BUCKETS}
        if frac['gc'] + frac['alloc'] + frac['builtin'] > 0.5:
            return 'runtime-bound', frac
        if frac['interp'] > 0.5:
            return 'not-compiled', frac
        return None, frac

    def state_of(key):
        for k, v in ma['keys'].items():
            if (v['name'], v['pos']) == key:
                return v['state']
        return None

    print('%5s %8s %8s %8s %7s  %-30s %s' % (
        'share', 'T_interp', 'T_aot', 'speedup', 'in-unit', 'function',
        'label'))
    speeds = []
    rb = nc = 0
    for key, s, sa, sh, sh_a, ti_f, ta_f, ua in hot:
        if sa == 0:
            speed = float('inf')
            inu = '-'
        else:
            speed = ti_f / ta_f if ta_f > 0 else float('inf')
            inu = '%.0f%%' % (100.0 * ua / sa)
        lab, frac = classify(key, sa)
        if lab == 'runtime-bound':
            rb += 1
        elif lab == 'not-compiled':
            # Still in the target: the emitter has to make this one fast, so its
            # current speedup counts toward the median as it stands.
            nc += 1
            if speed != float('inf'):
                speeds.append(speed)
        else:
            if speed != float('inf'):
                speeds.append(speed)
        labtxt = ''
        if lab == 'runtime-bound':
            labtxt = '  [runtime-bound]'
        elif lab == 'not-compiled':
            st = state_of(key)
            labtxt = '  [not compiled%s]' % ('' if st is None else ' %s' % st)
        lab_f = label(key, names)
        where = ' ' + key[1]
        print('%5.1f%% %8.3f %8.3f %8s %7s  %-30s%s%s' % (
            100.0 * sh, ti_f, ta_f,
            ('%.2fx' % speed) if speed != float('inf') else 'n/a',
            inu, lab_f[:30], where, labtxt))
        if args.buckets and frac:
            print('        ' + '  '.join(
                '%s %.0f%%' % (b, 100.0 * frac[b])
                for b in BUCKETS if frac[b] > 0))

    extra = [(k, v) for k, v in ta.items() if k not in ti and v[0] > 0]
    if extra:
        extra.sort(key=lambda kv: -kv[1][0])
        print()
        print('compiled-only (not in the interpreter\'s table):')
        for k, v in extra[:6]:
            lab_f = label(k, names)
            where = '' if lab_f == k[1] else ' ' + k[1]
            print('        %6d samples %6.0f%% in unit  %s%s'
                  % (v[0], 100.0 * v[1] / v[0], lab_f, where))

    # ---- the binding states against the engine's own count --------------
    by_state = collections.Counter(v['state'] for v in ma['keys'].values())
    stats = parse_stats(args.stats) if args.stats else {}
    line = 'states: ' + '  '.join('%s %d' % (k, by_state[k]) for k in
                                  ('bound', 'disabled', 'absent', 'unbound')
                                  if by_state[k])
    if stats:
        line += '  | stats: table=%d bound=%d disabled=%d deopts=%d' % (
            stats['table'], stats['bound'], stats['disabled'], stats['deopts'])
        # The engine counts every function it disabled; the sampler only knows
        # the ones it sampled, so it can never report more. It must not report
        # a disabled function the engine does not have, and the gap is the
        # disabled functions that were never sampled — both are stated.
        # The engine's count comes from a separate process and can differ:
        # a deopt race decides how many functions reach the limit, so two runs
        # of the same script report 2 and then 3 (measured). A small excess is
        # therefore expected, not a fault. A large one would mean the state
        # labels disagree with the engine, which is what this checks.
        slack = max(2, stats['disabled'] // 4)
        if by_state['disabled'] > stats['disabled'] + slack:
            line += ('  -> IMPOSSIBLE (sampler sees %d, engine %d)'
                     % (by_state['disabled'], stats['disabled']))
        else:
            note = ''
            if by_state['disabled'] > stats['disabled']:
                note = '; the engine\'s own count varies between runs ' \
                       '(measured 2 and 3 on crypto), so the sampler may ' \
                       'legitimately exceed one sample by a little'
            line += ('  -> consistent (%d of the engine\'s %d disabled '
                     'functions were sampled; %d of %d unit functions were '
                     'sampled at all%s)'
                     % (by_state['disabled'], stats['disabled'],
                        sum(by_state.values()), stats['table'], note))
    print(line)

    # ---- summaries ------------------------------------------------------
    tot_b = collections.Counter()
    for k, c in per_key.items():
        if k == '<no-js>':
            continue
        for b, n in c.items():
            tot_b[b] += n
    seen = sum(tot_b.values())
    print()
    if seen:
        print('buckets over %d compiled-build JS samples: %s'
              % (seen, '  '.join('%s %.1f%%' % (b, 100.0 * tot_b[b] / seen)
                                 for b in BUCKETS if tot_b[b])))
        # Every sample gets exactly one bucket, so the per-function fractions
        # sum to 100 % whenever the PC ring covered that function. What can be
        # short is coverage: a JS sample with no PC record (pc == 0) or a ring
        # that overflowed leaves samples unbucketed, so say so.
        js_total = ma['total'] - ma['nojs']
        print('bucket coverage: %d of %d JS samples have a PC (%.1f%%); '
              'per-function buckets sum to 100%% by construction'
              % (seen, js_total, 100.0 * seen / js_total if js_total else 0.0))
        cov = [(k, sum(c.values()) / ta[k][0]) for k, c in per_key.items()
               if k != '<no-js>' and ta.get(k) and ta[k][0] > 0]
        short = [(k, f) for k, f in cov if f < 0.999]
        if short:
            print('functions with unbucketed samples: %d (worst %s)'
                  % (len(short), min(short, key=lambda x: x[1])[0][1]))
    if ma['drop']:
        print('PC ring overflow: %d samples dropped' % ma['drop'])
    if shared_lib:
        print('samples in shared libraries: %d of %d (bucketed as other)'
              % (shared_lib, len(ma['pcs'])))
    if unresolved:
        print('unresolved PCs inside the binary: %d of %d'
              % (unresolved, len(ma['pcs'])))
    agree = sum((pc_hex & pc_frame).values())
    both = sum(pc_hex.values())
    if both:
        print('PC vs frame attribution: agree on %d of %d in-unit samples '
              '(%.1f%%)' % (agree, both, 100.0 * agree / both))

    speeds = [s for s in speeds if s != float('inf')]
    med = sorted(speeds)[len(speeds) // 2] if speeds else float('nan')
    print('total samples %d interp (%d no-JS), %d compiled (%d no-JS); '
          'hot set %d (%d runtime-bound, %d not compiled)'
          % (mi['total'], mi['nojs'], ma['total'], ma['nojs'], len(hot), rb, nc))
    if mi['wall'] > 0 and ma['wall'] > 0:
        print('sample period %.0f us interp, %.0f us compiled '
              '(%d samples over %.2f/%.2f s)'
              % (1e6 * mi['wall'] / mi['total'], 1e6 * ma['wall'] / ma['total'],
                 mi['total'], mi['wall'], ma['wall']))
    print('median speedup %.2fx over %d hot functions excluding the %d '
          'runtime-bound ones (not-compiled functions are included: the emitter '
          'must fix them)' % (med, len(speeds), rb))

    # The helper and interpreter symbols, by sample count: the input for
    # deciding which to replace with compiled code first.
    js_total = ms = sum(sym_count.values())

    # The op-call weight table: each `regvm_s_ROP_x` is the step form compiled
    # code enters for one op call, so its share *is* the weight of that op's op
    # call. It ranks which ops are worth lowering next, measured rather than
    # guessed.
    steps = [(sym, n) for (sym, b), n in sym_count.items()
             if b == 'aot-helper' and sym.startswith('regvm_s_')]
    steps.sort(key=lambda r: -r[1])
    if steps:
        stot = sum(n for _, n in steps)
        print()
        print('op-call weight (%d samples, %.1f%% of JS) -- step:<op>:'
              % (stot, 100.0 * stot / ms if ms else 0))
        for sym, n in steps[:20]:
            op = sym[len('regvm_s_ROP_'):] if sym.startswith('regvm_s_ROP_') \
                else sym[len('regvm_s_'):]
            print('  %6.1f%%  step:%-28s %s' % (100.0 * n / ms, op, sym))
        # One line a gate can read directly: the ops that would
        # repay a lowering, cumulatively.
        cum = 0.0
        for sym, n in steps:
            cum += 100.0 * n / ms
            if cum >= 50.0:
                print('  (the top ops above cover %.1f%% of all samples)' % cum)
                break

    for want, title in (('aot-helper', 'AOT-helper'),
                        ('call', 'call entry'),
                        ('interp', 'interpreter')):
        rows = [(sym, n) for (sym, b), n in sym_count.items()
                if b == want and sym != '?']
        rows.sort(key=lambda r: -r[1])
        if not rows:
            continue
        print()
        print('top %s symbols (%d samples, %.1f%% of JS):'
              % (title, sum(n for _, n in rows),
                 100.0 * sum(n for _, n in rows) / ms if ms else 0))
        for sym, n in rows[:15]:
            print('  %6.1f%%  %s' % (100.0 * n / ms, sym))
    return 0


if __name__ == '__main__':
    sys.exit(main())
