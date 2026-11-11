#!/usr/bin/env python3
"""Mutants of the RegExp pristine checks (patches 0531 and 0541).

Each mutant removes or weakens one check in js_regexp_proto_pristine /
js_regexp_ctor_pristine and must make at least one of the files below produce
output different from node's (or crash). A mutant that survives means the
check is not covered by a test.

  python3 tests/differential/mutants/regexp-flags-sabotage.py [--scratch DIR] [--only NAME] [--keep]

Needs node, the engine's other objects (libregexp.o libunicode.o dtoa.o and
qjs-run.o built the way tests/differential/README.md builds qjs-run) in
--objs, and tools/aot/ccsplit as the compiler. One mutant takes one compile of
quickjs.c.
"""
import argparse, os, shutil, subprocess, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
SRC = os.path.join(ROOT, 'engine', 'quickjs-ng')
FILES = [
    os.path.join(ROOT, 'tests', 'differential', 'corpus', 'regexp-flags-derivation.js'),
    os.path.join(ROOT, '.local', 'kit', 'regexp-proto-mutation.js'),
]

MUTANTS = {
    # the slot may now hold a different property of the same kind
    'proto-name': ("sp[ctx->re_snap_idx[i]].atom != ctx->re_snap_atom[i] ||", ""),
    # a getter turned into a data property (or back) is not noticed
    'proto-kind': ("((sp[ctx->re_snap_idx[i]].flags & JS_PROP_TMASK) == JS_PROP_GETSET) != (ctx->re_snap_getter[i] != 0))",
                   "0)"),
    # a replaced function is not noticed
    'proto-value': ("} else if (!js_regexp_same_obj(pr->u.value, ctx->re_snap[i])) {\n            return false;\n        }",
                    "}"),
    # a replaced getter is not noticed
    'proto-getter': ("if ((void *)pr->u.getset.getter != JS_VALUE_GET_PTR(ctx->re_snap[i]))\n                return false;",
                     ";"),
    # RegExp[Symbol.species] replaced
    'ctor-species': ("return (void *)pr->u.getset.getter == JS_VALUE_GET_PTR(ctx->re_species);", "return true;"),
}

def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)

def node_out(path):
    pre = 'var print = console.log;\n'
    tmp = os.path.join(args.scratch, 'n.js')
    open(tmp, 'w').write(pre + open(path).read())
    return run(['node', tmp]).stdout

def main():
    os.makedirs(args.scratch, exist_ok=True)
    src = open(os.path.join(SRC, 'quickjs.c')).read()
    want = {f: node_out(f) for f in FILES}
    cc = os.path.join(ROOT, 'tools', 'aot', 'ccsplit')
    survivors = []
    for name, (old, new) in MUTANTS.items():
        if args.only and name != args.only:
            continue
        if src.count(old) != 1:
            print('%-14s SKIP (pattern occurs %d times)' % (name, src.count(old)))
            survivors.append(name + ' (pattern)')
            continue
        d = os.path.join(args.scratch, name)
        os.makedirs(d, exist_ok=True)
        mc = os.path.join(d, 'quickjs-mutant.c')
        open(mc, 'w').write(src.replace(old, new))
        try:
            r = run([cc, '-O1', '-std=gnu11', '-D_GNU_SOURCE', '-DCONFIG_VERSION="x"', '-DJS_ENABLE_DEBUGGER=0',
                     '-DQJS_REGVM=1', '-I' + SRC, '-c', mc, '-o', os.path.join(d, 'quickjs.o')])
        finally:
            os.remove(mc)
        if r.returncode:
            print('%-14s BUILD FAILED\n%s' % (name, r.stderr[-400:]))
            survivors.append(name + ' (build)')
            continue
        exe = os.path.join(d, 'qjs-run')
        objs = [os.path.join(args.objs, o) for o in ('qjs-run.o', 'libregexp.o', 'libunicode.o', 'dtoa.o')]
        r = run([cc, os.path.join(d, 'quickjs.o')] + objs + ['-lm', '-lpthread', '-o', exe])
        if r.returncode:
            print('%-14s LINK FAILED\n%s' % (name, r.stderr[-400:]))
            survivors.append(name + ' (link)')
            continue
        killed_by = None
        for f in FILES:
            p = run([exe, f], timeout=120)
            if p.returncode != 0 or p.stdout != want[f]:
                killed_by = os.path.basename(f)
                break
        print('%-14s %s' % (name, 'killed by ' + killed_by if killed_by else 'SURVIVED'))
        if not killed_by:
            survivors.append(name)
        if not args.keep:
            shutil.rmtree(d, ignore_errors=True)
    if survivors:
        print('surviving mutants:', ', '.join(survivors))
        sys.exit(1)
    print('all mutants killed')

if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--scratch', default=os.path.join(ROOT, '.local', 'scratch', 'mutants'))
    ap.add_argument('--objs', default=os.path.join(ROOT, '.local', 'scratch', 'wt-R', 'scr', 'dt'))
    ap.add_argument('--only')
    ap.add_argument('--keep', action='store_true')
    args = ap.parse_args()
    main()
