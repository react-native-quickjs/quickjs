#!/usr/bin/env python3
"""Disassemble the register VM's bytecode.

The register stream is what the compiler actually emits, and reading it is the only
way to tell whether the compiler is doing its job: how many instructions an operation
takes, and how many of them are loads, stores and moves that exist only because a
value was forced through the frame.

Input is the dump the engine writes when REGVM_DUMP=1 is set in a QJS_REGVM_COUNT
build (the encoder's dump hook):

    ##FUNC## insns=<n> values=<n> regs=<n> vars=<n>
    ##I## <index> <src_pc> <rop> <op>...

Usage:
    REGVM_DUMP=1 ./tool script.js 2>&1 >/dev/null | python3 tools/regvm/regvm-disasm.py

Output is one block per function, each instruction as `name op=value ...` with the
operand names taken from the format table, plus a per-function tally of the
instructions that are pure frame traffic (loads, stores, moves) -- the number the
compiler has to drive to zero.
"""
import os
import re
import sys

# The tree root: the script lives in <root>/tools/regvm. QJS_ROOT overrides it
# for a checkout elsewhere.
ROOT = os.environ.get('QJS_ROOT') or os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
REGVM_H = f'{ROOT}/engine/quickjs-ng/quickjs-regvm.h'
QUICKJS_C = f'{ROOT}/engine/quickjs-ng/quickjs.c'

OPERAND_NAME = {'r': 'reg', 'u': 'slot', 'a': 'atom', 's': 'site', 'b': 'subop',
                'i': 'imm', 'I': 'imm', 'c': 'const', 't': 'target'}

# what the instruction is for, so the tally can name the traffic
TRAFFIC = {
    'ROP_get_local': 'load', 'ROP_get_arg': 'load',
    'ROP_put_local': 'store', 'ROP_put_arg': 'store',
    'ROP_put_local_xfer': 'store', 'ROP_put_arg_xfer': 'store',
    'ROP_move': 'move', 'ROP_move_xfer': 'move',
    'ROP_get_var_ref': 'load', 'ROP_put_var_ref': 'store',
}


def opcode_names():
    src = open(REGVM_H).read()
    m = re.search(r'typedef\s+enum\s+JSRegOpcode\s*\{(.*?)\bROP_COUNT\b', src, re.S)
    body = re.sub(r'/\*.*?\*/', '', m.group(1), flags=re.S)
    body = re.sub(r'//[^\n]*', '', body)
    names, v = {}, 0
    for item in body.split(','):
        item = item.strip()
        if not item:
            continue
        q = re.match(r'(ROP_\w+)\s*(?:=\s*(\d+))?$', item)
        if not q:
            continue
        if q.group(2) is not None:
            v = int(q.group(2))
        names[v] = q.group(1)
        v += 1
    return names


def formats():
    src = open(QUICKJS_C).read()
    out = {}
    for m in re.finditer(r'\[(ROP_\w+)\]\s*=\s*"([a-zA-Z]*)",', src):
        out[m.group(1)] = m.group(2)
    return out


def main():
    names, fmts = opcode_names(), formats()
    total = {'load': 0, 'store': 0, 'move': 0}
    nfun = 0
    for ln in sys.stdin:
        if ln.startswith('##FUNC## '):
            nfun += 1
            kv = dict(x.split('=') for x in ln.split()[1:])
            print(f'\nfunction {nfun}: insns={kv.get("insns")} values={kv.get("values")} '
                  f'regs={kv.get("regs")} vars={kv.get("vars")}')
            continue
        if not ln.startswith('##I## '):
            continue
        parts = ln.split()[1:]
        idx, src_pc, rop = int(parts[0]), int(parts[1]), int(parts[2])
        ops = [int(x) for x in parts[3:]]
        name = names.get(rop, f'op{rop}')
        fmt = fmts.get(name, '')

        rendered, k = [], 0
        for k, ch in enumerate(fmt):
            if k >= len(ops):
                break
            rendered.append(f'{OPERAND_NAME.get(ch, ch)}={ops[k]}')
        if len(ops) > len(fmt):
            rendered += [str(x) for x in ops[len(fmt):]]

        kind = TRAFFIC.get(name)
        if kind:
            total[kind] += 1
        mark = f'   <- {kind}' if kind else ''
        print(f'  {idx:>3} pc={src_pc:<5} {name:<26} {" ".join(rendered)}{mark}')

    print(f'\n  frame traffic in this dump: '
          f'loads={total["load"]} stores={total["store"]} moves={total["move"]}')


if __name__ == '__main__':
    main()
