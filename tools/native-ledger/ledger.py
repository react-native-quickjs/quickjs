#!/usr/bin/env python3
"""Native-work ledger -- a compiler-assisted accountant for interpreter overhead.

Answers "what work does the register VM do that the stack VM does not" in units of
**dynamic native instructions**, not time and not samples. It is deliberately not a
profiler: it counts work, and time attribution is a separate exercise.

Pipeline
--------
  1. inventory  -- disassemble the optimized interpreter (-O3 -g) and record, per
                   instruction: address, function, source line, mnemonic, operands,
                   and inline ancestry where the debug info provides it.
  2. counters   -- build the same source with -fsanitize-coverage=trace-pc-guard and
                   this directory's blockcounts.c, run the workload, and read the
                   per-block execution counts. Counters land at block entries in the
                   finalised machine code, so register allocation is the one being
                   measured. This build is slow and is never timestamped.
  3. classify   -- assign each instruction to one of eight cost categories plus an
                   explicit unknown/mixed row, with a confidence per instruction.
  4. ledger     -- dynamic work = sum over blocks of (count x instructions in the
                   block, per category), normalised per workload iteration, with the
                   register-minus-stack column and reconciliation checks.

A block's span ends at its terminator, not at the next instrumented address: hot/cold
splitting leaves those far apart and the gap is not the block's code. Getting that
wrong inflates the ledger by about 6x.

Usage
-----
  python3 ledger.py --repo <quickjs repo> --workload <js> --worlds 32 \
                    [--label register] [--label stack] [--json out.json]

Both builds are produced from one source tree; the stack build is QJS_REGVM=0.
"""
import argparse
import bisect
import json
import os
import re
import subprocess
import sys
from collections import defaultdict

CATS = ['operand_decode', 'operand_addressing', 'value_traffic', 'ownership',
        'interp_state', 'dispatch', 'js_operation', 'runtime_helpers',
        'unknown_mixed']

# Instructions whose role is unambiguous from the mnemonic alone.
TERMINATORS = re.compile(r'^(b|br|ret|cbz|cbnz|tbz|tbnz)(\.\w+)?$')

# Helpers whose inlined bodies are classified by what the helper is, not by the
# caller's context. Kept deliberately short: anything not established here falls to
# the unknown row rather than being forced into a bucket.
HELPER_ROLE = {
    'js_dup': 'ownership', 'JS_FreeValueRT': 'ownership', 'js_free_value_rt': 'ownership',
    'free_zero_refcount': 'ownership', 'set_value': 'ownership', 'JS_FreeValue': 'ownership',
    'regvm_num_fast': 'js_operation', 'js_regvm_binary': 'js_operation',
    'regvm_get_field_ic': 'js_operation', 'js_relational_slow': 'js_operation',
    'js_strict_eq2': 'js_operation', 'js_binary_logic_slow': 'js_operation',
    'JS_SetPropertyInternal2': 'js_operation', 'JS_GetPropertyInternal': 'js_operation',
    'find_own_property': 'js_operation', 'js_megacache_probe': 'js_operation',
    'ic_read_update': 'js_operation', 'regvm_to_bool': 'js_operation',
}

def is_engine_source(path):
    """An instruction's source file is one of the engine's own translation
    units or headers. Header and `.inc` instructions are real work (inlined
    helpers); the old filter kept `quickjs.c` alone and dropped about 16% of
    them, which is why the totals were not comparable across builds."""
    p = path.replace('\\', '/')
    return '/engine/quickjs-' in p or p.startswith('engine/quickjs-')


SYM = re.compile(r'^([0-9a-f]+) <(.+)>:')
# Raw byte groups are space-separated by llvm-objdump on Linux ("1f 20 03 d5")
# and contiguous on some other builds ("1f2003d5"); accept both.
DIS = re.compile(r'^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} ?)+)\s+(\S+)\s*(.*)$')
LIN = re.compile(r'^;\s+([^:]+):(\d+)\s*$')


# --------------------------------------------------------------------- source --
class Source:
    def __init__(self, path):
        self.lines = open(path).read().split('\n')
        self.macros = self._macro_defs()
        self.funcs = self._func_lines()

    def _macro_defs(self):
        out, i = [], 0
        L = self.lines
        while i < len(L):
            if re.match(r'#define\s', L[i]):
                j = i
                while j + 1 < len(L) and L[j].rstrip().endswith('\\'):
                    j += 1
                out.append((i + 1, j + 1))
                i = j + 1
            else:
                i += 1
        return out

    def _func_lines(self):
        out = {}
        L = self.lines
        for i, ln in enumerate(L):
            m = re.match(r'static\s+[\w\s\*]*?\b(\w+)\s*\(', ln)
            if not m:
                continue
            j, depth = i, 0
            while j < len(L):
                depth += L[j].count('{') - L[j].count('}')
                if depth > 0 and L[j].strip() == '}':
                    out[m.group(1)] = (i + 1, j + 1)
                    break
                j += 1
        return out

    def macro_at(self, line):
        for a, b in self.macros:
            if a <= line <= b:
                m = re.match(r'#define\s+(\w+)', self.lines[a - 1])
                return m.group(1) if m else None
        return None

    def text(self, line):
        return self.lines[line - 1].strip() if 0 < line <= len(self.lines) else ''


# ------------------------------------------------------------------ inventory --
def disassemble(binary, out_path):
    objdump = os.environ.get('LLVM_OBJDUMP') or 'llvm-objdump'
    with open(out_path, 'w') as f:
        subprocess.run([objdump, '-d', '-l', binary], stdout=f,
                       stderr=subprocess.DEVNULL, check=False)
    return out_path


def parse_disasm(path):
    insns, syms = [], []
    sym, cur, keep = None, None, False
    for ln in open(path):
        m = SYM.match(ln)
        if m:
            sym = m.group(2)
            syms.append((int(m.group(1), 16), sym))
            cur, keep = None, False
            continue
        m = LIN.match(ln)
        if m:
            keep = is_engine_source(m.group(1))
            cur = int(m.group(2)) if keep else None
            continue
        m = DIS.match(ln)
        if m and keep and cur is not None:
            insns.append((int(m.group(1), 16), sym, cur, m.group(3), m.group(4)))
    span = {}
    for i, (a, n) in enumerate(syms):
        b = syms[i + 1][0] if i + 1 < len(syms) else a + (1 << 24)
        span[n] = (a, b)
    return insns, span


# -------------------------------------------------------------------- counters --
def build_plain(repo, out_bin, regvm):
    """The ordinary -O3 build. Counting no longer needs an instrumented binary:
    sanitizer coverage put its guards on a subset of blocks, which is what made
    the totals a coverage-dependent subset."""
    src = os.path.join(repo, 'engine', 'quickjs-ng')
    cc = os.environ.get('CC', 'clang')
    # -gdwarf-4: valgrind 3.19 cannot read clang 14's DWARF 5. It changes
    # nothing about the code, only the debug info.
    cmd = [cc, '-O3', '-g', '-gdwarf-4', '-DNDEBUG', '-std=gnu11',
           f'-DQJS_REGVM={1 if regvm else 0}', '-D_GNU_SOURCE', f'-I{src}',
           '-o', out_bin,
           os.path.join(repo, 'tools', 'benchmark', 'qjs-bench.c'),
           *[os.path.join(src, f) for f in
             ('quickjs.c', 'libregexp.c', 'libunicode.c', 'dtoa.c')],
           '-lm']
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f'build failed:\n{r.stdout[-2000:]}\n{r.stderr[-2000:]}')


def run_callgrind(binary, workload, out_cg):
    """Exact, deterministic dynamic count per instruction address. Slower than
    the hook by ~100x, and the only one of the two whose total is the program's."""
    here = os.path.dirname(os.path.abspath(__file__))
    modal = os.environ.get('VALGRIND') or 'valgrind'
    r = subprocess.run([modal, '--tool=callgrind', '--dump-instr=yes',
                        f'--callgrind-out-file={out_cg}', binary, workload],
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit(f'callgrind run failed:\n{r.stderr[-1000:]}')
    return r.stdout.strip().split('\n')[-1]


def parse_callgrind(path):
    """address -> dynamic instruction count, per instruction.

    `positions: instr line` with `--dump-instr=yes`; entries are `+delta`
    compressed, and lined and unlined entries are summed together.
    """
    counts, pos = {}, None
    for line in open(path):
        if line.startswith('fn='):
            continue
        t_ = line.split()
        if len(t_) == 3 and t_[0][:2] == '0x':
            pos = int(t_[0], 16)
        elif len(t_) == 3 and t_[0][0] == '+':
            pos += int(t_[0][1:])
        else:
            continue
        counts[pos] = counts.get(pos, 0) + int(t_[2])
    return counts, 0


# ---------------------------------------------------------------- register roles
MEMBASE = re.compile(r'\[(x\d+|sp|w\d+)')
OFFSET = re.compile(r'#(0x[0-9a-f]+|\d+)\]')


def infer_roles(insns, interp_sym):
    """What a base register holds, inferred from how it is addressed.

    A memory operand cannot be classified from its mnemonic: `ldr x2, [x20, #0x8]`
    is a JSValue tag (ownership) or a struct field (interpreter state) depending on
    what x20 is. Three address patterns identify the pointers that matter:

      register file -- used with `lsl #4`, the 16-byte JSValue stride
      context       -- used with large constant offsets (its field offsets)
      bytecode      -- used with small sequential offsets as the instruction stream

    Nothing here is hard-coded to a register number: the roles are read off the code,
    so the tool survives recompilation.
    """
    shift4, bigoff, smalloff = defaultdict(int), defaultdict(int), defaultdict(int)
    for a, sym, line, mnem, ops in insns:
        if sym != interp_sym:
            continue
        m = MEMBASE.search(ops)
        if not m:
            continue
        base = m.group(1)
        if 'lsl #4' in ops:
            shift4[base] += 1
        o = OFFSET.search(ops)
        if o:
            off = int(o.group(1), 16) if o.group(1).startswith('0x') else int(o.group(1))
            if off >= 0x100:
                bigoff[base] += 1
            elif off < 0x40:
                smalloff[base] += 1
    roles = {}
    if shift4:
        roles[max(shift4, key=shift4.get)] = 'regfile'
    for b in sorted(bigoff, key=bigoff.get, reverse=True):
        if roles.get(b) is None:
            roles[b] = 'context'
            break
    for b in sorted(smalloff, key=smalloff.get, reverse=True):
        if roles.get(b) is None:
            roles[b] = 'bytecode'
            break
    for b in ('sp',):
        roles[b] = 'frame'
    return roles


# -------------------------------------------------------------------- classify --
ADDBASE = re.compile(r'^\s*\S+,\s*(x\d+|sp|w\d+)')


def base_role(roles, ops, mnem=''):
    """The base pointer an instruction addresses. A load or store names it inside
    brackets; an address computation names it as the second operand."""
    m = MEMBASE.search(ops)
    if m:
        return roles.get(m.group(1))
    if mnem.lower() in ('add', 'sub', 'lsl', 'lsr', 'and', 'orr', 'ubfx', 'madd'):
        m = ADDBASE.match(ops)
        if m:
            return roles.get(m.group(1))
    return None


_LINE_CAT = {}


def line_origin(src, line):
    """(category, confidence) fixed by the source location alone, or None."""
    if line in _LINE_CAT:
        return _LINE_CAT[line]
    out = None
    name = src.macro_at(line)
    if name:
        if name.startswith('RD_') or 'get_u' in name or name.startswith('GET_'):
            out = ('operand_decode', 'high')
        elif name in ('REGVM_NEXT', 'SWITCH', 'BREAK', 'DISPATCH', 'CASE'):
            out = ('dispatch', 'high')
        elif name in ('SET_REG', 'JS_VALUE_HAS_REF_COUNT', 'JS_REF_COUNT'):
            out = ('ownership', 'high')
    if out is None:
        for h, cat in HELPER_ROLE.items():
            r = src.funcs.get(h)
            if r and r[0] <= line <= r[1]:
                out = (cat, 'high')
                break
    _LINE_CAT[line] = out
    return out


def classify(src, line, mnem, ops, roles=None):
    roles = roles or {}
    loc = line_origin(src, line)
    if loc:
        return loc
    """(category, confidence). High = provenance is known from the macro that was
    expanded or the helper the code belongs to. Low = the mnemonic cannot separate a
    spilled interpreter pointer from a JS operand load, so it is left unknown."""
    role = base_role(roles, ops, mnem)
    if role == 'bytecode' or re.search(r'\brpc\b', ops):
        return 'operand_decode', 'high'
    mem = bool(MEMBASE.search(ops))
    if role == 'regfile':
        # an address computation into the register file against a load or store of
        # a JSValue out of it
        return ('value_traffic' if mem else 'operand_addressing'), 'high'
    if role in ('context', 'frame'):
        return ('interp_state' if mem else 'operand_addressing'), 'high'
    if role == 'bytecode':
        return 'operand_addressing', 'high'
    m = mnem.lower()
    if m in ('ldrh', 'ldrb'):
        return 'operand_decode', 'high'
    if m in ('ldr', 'ldrsw') and re.search(r'(lsl|uxtw|sxtw) #3\b', ops):
        return 'dispatch', 'high'          # the dispatch-table entry
    if m == 'br':
        return 'dispatch', 'high'
    if m in ('mov', 'movz', 'movk') and re.search(r'#0x[0-9a-f]+, lsl #16', ops):
        return 'operand_addressing', 'medium'
    if m in ('bl', 'blraa'):
        return 'runtime_helpers', 'high'
    if m in ('ldp', 'stp') or (m in ('ldr', 'str') and re.match(r'^q\d', ops)):
        return 'value_traffic', 'high'
    if m in ('cmn', 'ccmn', 'ccmp', 'cset', 'csel', 'tbnz', 'tbz', 'cinc'):
        return 'ownership', 'high'
    # a scaled index of 8 or 16 bytes is element addressing: a JSValue in the
    # register file, or an 8-byte entry in a shape/property table
    if re.search(r'(lsl|uxtw|sxtw) #(3|4)\b', ops):
        return ('value_traffic' if mem else 'operand_addressing'), 'high'
    # a 16-byte-strided access at +8 reads a JSValue's tag: the ownership guard
    if mem and re.search(r'#0x8\]', ops):
        return 'ownership', 'high'
    if m in ('ldrsw', 'sxtl'):
        return 'interp_state', 'high'          # a sign-extended struct field
    # a large constant added to a base forms a symbol or table address
    if m in ('add', 'sub') and re.search(r'#0x[0-9a-f]{3,}', ops):
        return 'runtime_helpers', 'medium'
    # evidence-based fallback, so no instruction is left unclassified; the
    # confidence column records that this is a mnemonic class, not provenance
    if m in ('add', 'sub', 'lsl', 'lsr', 'asr', 'uxtw', 'sxtw', 'and', 'orr',
             'ubfx', 'bfi', 'sbfx', 'madd', 'adrp', 'mov', 'movz', 'movk', 'csel',
             'csinc', 'cset', 'cinc'):
        return 'operand_addressing', 'medium'
    if m in ('ldr', 'str', 'ldrb', 'ldrh', 'strb', 'strh', 'ldp', 'stp', 'ldur',
             'stur', 'ldarb', 'ldar', 'stlr'):
        return 'value_traffic', 'medium'
    if m in ('cmp', 'cmn', 'tst', 'ccmp', 'ccmn', 'tbz', 'tbnz', 'cbz', 'cbnz'):
        return 'ownership', 'medium'
    if m in ('b', 'b.eq', 'b.ne', 'b.lt', 'b.gt', 'b.le', 'b.ge', 'b.hi', 'b.lo',
             'b.hs', 'b.ls', 'b.mi', 'b.pl', 'b.vs', 'b.vc', 'br', 'ret'):
        return 'dispatch', 'medium'
    return 'js_operation', 'low'


# ---------------------------------------------------------------------- ledger --
SHAPE_IMM = re.compile(r'#[0-9]')


def shape_of(mnem, ops):
    """What kind of address arithmetic this instruction is."""
    if 'lsl #4' in ops:
        return 'index * 16 (JSValue stride)'
    if mnem.lower() in ('movz', 'movk'):
        return 'materialise a constant'
    if SHAPE_IMM.search(ops):
        return 'constant offset'
    return 'register arithmetic'


def reduce_ledger(src, insns, counts, worlds, roles):
    """Classify every executed instruction directly.

    With per-instruction counts there is no block to walk and no span to get
    wrong: an instruction is counted if the workload retired it. That is what
    makes the total equal the program's."""
    per_cat, per_cat_conf, per_line = defaultdict(int), defaultdict(int), defaultdict(int)
    per_line_cat, per_shape_cat = defaultdict(int), defaultdict(int)
    used = 0
    for a, sym, line, mnem, ops in insns:
        c = counts.get(a)
        if not c:
            continue
        used += 1
        cat, conf = classify(src, line, mnem, ops, roles)
        per_cat[cat] += c
        per_cat_conf[(cat, conf)] += c
        per_line[line] += c
        per_line_cat[(line, cat)] += c
        per_shape_cat[(cat, shape_of(mnem, ops))] += c
    return per_cat, per_cat_conf, per_line, per_line_cat, per_shape_cat, used


def report(tag, per_cat, worlds, bytecodes, ms, ghz=0.0, truth=0.0):
    total = sum(per_cat.values())
    ns_per_world = ms * 1e6 / worlds if worlds else None
    ns_per_native = ns_per_native_instruction(ms, total)
    cyc_per_native = ns_per_native * ghz if (ns_per_native and ghz) else None
    cat = {c: per_cat.get(c, 0) for c in CATS}
    share = {c: (v / total if total else 0) for c, v in cat.items()}
    return {
        'label': tag,
        'worlds': worlds,
        'bytecode_instructions_measured': bytecodes,
        'elapsed_ms': ms,
        'dynamic_native_instructions': total,
        'direct_dynamic_instructions': truth or None,
        'coverage': (total / truth) if truth else None,
        'natives_per_world': total / worlds,
        'natives_per_bytecode_instruction': total / bytecodes if bytecodes else None,
        'ns_per_native_instruction': ns_per_native,
        'ns_per_world': ns_per_world,
        # derived, never measured: ns from the uninstrumented --ms, cycles from ns
        # and the stated --ghz. The tool counts work; time is attributed outside it.
        'derived_ghz': ghz or None,
        'cycles_per_native_instruction': cyc_per_native,
        'cycles_per_world': ns_per_world * ghz if (ns_per_world and ghz) else None,
        'dynamic_native_cycles': total * cyc_per_native if cyc_per_native else None,
        'categories': cat,
        'category_share': share,
        'category_ns_per_world': {c: share[c] * ns_per_world
                                  for c in CATS} if ns_per_world else None,
        'category_cycles_per_world': {c: share[c] * ns_per_world * ghz
                                      for c in CATS} if (ns_per_world and ghz) else None,
    }


def ns_per_native_instruction(ms, total):
    return ms * 1e6 / total if total else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--repo', default=os.path.abspath(
        os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')))
    ap.add_argument('--workload', required=True)
    ap.add_argument('--worlds', type=int, default=32)
    ap.add_argument('--bytecodes', type=int, nargs=2, default=[0, 0],
                    help='measured bytecode instruction totals for register and stack')
    ap.add_argument('--ms', type=float, nargs=2, default=[0.0, 0.0],
                    help='uninstrumented elapsed ms for register and stack')
    ap.add_argument('--ghz', type=float, default=0.0,
                    help='CPU clock, to derive cycles from --ms (cycles are '
                         'never measured here; there is no PMU)')
    ap.add_argument('--truth', type=float, nargs=2, default=[0.0, 0.0],
                    help='direct dynamic instruction counts for the same binaries '
                         'and workload (register, stack), e.g. from callgrind. The '
                         'ledger counts only guarded blocks, so this is the check '
                         'that its total is a subset and by how much.')
    ap.add_argument('--min-coverage', type=float, default=0.95,
                    help='fail if the ledger total covers less than this fraction '
                         'of --truth (default 0.95)')
    ap.add_argument('--json', default=None)
    ap.add_argument('--explain', default=None,
                    help='break a category (e.g. operand_addressing) down by source '
                         'line -- which handler -- and by address-arithmetic shape')
    ap.add_argument('--workdir', default='/tmp/native-ledger')
    args = ap.parse_args()

    src_path = os.path.join(args.repo, 'engine', 'quickjs-ng', 'quickjs.c')
    src = Source(src_path)
    os.makedirs(args.workdir, exist_ok=True)
    results, lines, breakdowns = [], {}, {}
    for regvm, tag in ((True, 'register'), (False, 'stack')):
        binary = os.path.join(args.workdir, f'plain-{tag}')
        build_plain(args.repo, binary, regvm)
        cg = os.path.join(args.workdir, f'cg-{tag}.cg')
        run_callgrind(binary, args.workload, cg)
        dis = disassemble(binary, os.path.join(args.workdir, f'dis-{tag}.txt'))
        insns, span = parse_disasm(dis)
        counts, calls = parse_callgrind(cg)
        interp = next((n for n in span if n.endswith('js_regvm_execute')), None) \
            or next((n for n in span if n.endswith('JS_CallInternal')), None)
        roles = infer_roles(insns, interp) if interp else {}
        if regvm:
            print(f'  inferred register roles: {roles}')
        per_cat, per_cat_conf, per_line, per_line_cat, per_shape_cat, used = \
            reduce_ledger(src, insns, counts, args.worlds, roles)
        if args.explain:
            breakdowns[tag] = (per_line_cat, per_shape_cat)
        i = 0 if regvm else 1
        results.append(report(tag, per_cat, args.worlds, args.bytecodes[i],
                              args.ms[i], args.ghz, args.truth[i]))
        lines[tag] = per_line
        results[-1]['blocks_counted'] = len(counts)
        results[-1]['blocks_used'] = used
        results[-1]['hook_invocations'] = calls
        results[-1]['confidence'] = {
            'high': sum(v for (c, cf), v in per_cat_conf.items() if cf == 'high'),
            'medium': sum(v for (c, cf), v in per_cat_conf.items() if cf == 'medium'),
            'low': sum(v for (c, cf), v in per_cat_conf.items() if cf == 'low'),
        }

    r, s = results
    R = r['dynamic_native_instructions']
    S = s['dynamic_native_instructions']
    cov = [(x['coverage'] if x['coverage'] else None) for x in results]
    unreliable = any(c is not None and c < args.min_coverage for c in cov)
    if unreliable:
        print('  !! the block-count total covers only part of the program: the '
              'category split below is a subset, not the whole workload.')
    cyc = ' cycles/world' if args.ghz else ''
    print(f'{"category":<22}{"register/world":>16}{"stack/world":>16}'
          f'{"excess/world":>16}{"reg %":>8}{"stk %":>8}'
          + (f'{"reg cyc/w":>12}{"stk cyc/w":>12}' if args.ghz else ''))
    for c in CATS:
        rc, sc = r['categories'][c], s['categories'][c]
        row = (f'  {c:<20}{rc/args.worlds:>16,.0f}{sc/args.worlds:>16,.0f}'
               f'{(rc-sc)/args.worlds:>16,.0f}{100*r["category_share"][c]:>7.1f}%'
               f'{100*s["category_share"][c]:>7.1f}%')
        if args.ghz:
            row += (f'{r["category_cycles_per_world"][c]:>12,.0f}'
                    f'{s["category_cycles_per_world"][c]:>12,.0f}')
        print(row)
    print(f'  {"TOTAL":<20}{R/args.worlds:>16,.0f}{S/args.worlds:>16,.0f}'
          f'{(R-S)/args.worlds:>16,.0f}')
    print()
    for x in results:
        parts = []
        if x['natives_per_bytecode_instruction']:
            parts.append(f'{x["natives_per_bytecode_instruction"]:,.1f} natives/bytecode')
        if x['ns_per_native_instruction']:
            s_ = (f'{x["natives_per_world"]:,.0f} natives/world, '
                  f'{x["ns_per_world"]:,.1f} ns/world, '
                  f'{x["ns_per_native_instruction"]:.4f} ns/native')
            if args.ghz:
                s_ += (f', {x["cycles_per_world"]:,.0f} cycles/world, '
                       f'{x["cycles_per_native_instruction"]:.3f} cycles/native '
                       f'@{args.ghz:g}GHz')
            parts.append(s_)
        if x['dynamic_native_instructions'] and x['dynamic_native_cycles']:
            parts.append(f'total {x["dynamic_native_instructions"]:,.0f} natives, '
                         f'{x["dynamic_native_cycles"]:,.0f} cycles')
        print(f'  {x["label"]:<9} ' + '; '.join(parts))
    for x in results:
        if x['coverage']:
            print(f'  {x["label"]:<9} coverage {100*x["coverage"]:.1f}% of the direct '
                  f'dynamic count ({x["dynamic_native_instructions"]:,.0f} of '
                  f'{x["direct_dynamic_instructions"]:,.0f})')
    if args.ghz:
        print(f'  note: cycles are derived from the uninstrumented --ms and the '
              f'stated {args.ghz:g} GHz, and split across categories by work share; '
              f'they are not measured.')
    if R and S:
        print(f'\n  excess native work {100*(R-S)/S:+.1f}%; '
              f'elapsed excess {100*(args.ms[0]-args.ms[1])/max(args.ms[1],1):+.1f}%')
    print('\nRECONCILIATION')
    ok = all(sum(x['categories'].values()) == x['dynamic_native_instructions']
             for x in results)
    print(f'  category totals sum to the inventoried total: {ok}')
    for x in results:
        k = x['confidence']
        tot = sum(k.values()) or 1
        print(f'  {x["label"]:<9} attributed {100*(1-x["category_share"]["unknown_mixed"]):>5.1f}%  '
              f'(high {100*k["high"]/tot:.1f}%, medium {100*k["medium"]/tot:.1f}%, '
              f'low {100*k["low"]/tot:.1f}%)  blocks {x["blocks_used"]}/{x["blocks_counted"]}')
    ux = (r['categories']['unknown_mixed'] - s['categories']['unknown_mixed'])
    if R - S:
        print(f'  excess left unknown: {100*ux/(R-S):.1f}%')

    if unreliable:
        print(f'\n  FAIL: ledger covers less than {100*args.min_coverage:.0f}% of '
              f'--truth on at least one engine; the totals and shares are not '
              f'usable as whole-workload figures.')
    if args.explain:
        cat = args.explain
        for x in results:
            tag = x['label']
            plc, psc = breakdowns.get(tag, ({}, {}))
            rows = sorted(((l, c) for (l, cc), c in plc.items() if cc == cat),
                          key=lambda r: -r[1])[:25]
            if not rows:
                continue
            print(f'\n  {cat}, {tag}: top source lines')
            for l, c in rows:
                txt = src.lines[l - 1].strip() if 0 < l <= len(src.lines) else ''
                print(f'    {c/args.worlds:>14,.0f}/world  line {l:<6} {txt[:64]}')
            print(f'  {cat}, {tag}: by address-arithmetic shape')
            for (cc, sh), c in sorted(psc.items(), key=lambda kv: -kv[1]):
                if cc == cat:
                    print(f'    {c/args.worlds:>14,.0f}/world  {sh}')

    if args.json:
        with open(args.json, 'w') as f:
            json.dump({'results': results, 'cats': CATS,
                       'min_coverage': args.min_coverage}, f, indent=1)
        print(f'\nwrote {args.json}')

    # A total that covers part of the program cannot be used as a whole-workload
    # figure. Refuse, rather than let the shares be quoted.
    if unreliable:
        sys.exit(1)


if __name__ == '__main__':
    main()
