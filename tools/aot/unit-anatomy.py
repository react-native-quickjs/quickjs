#!/usr/bin/env python3
"""Size anatomy of a generated AOT unit: C lines and bytes by kind of top-level item, the largest functions, exit-stub labels.

  unit-anatomy.py u.c [--top N] [--tsv]

Uses the item parser of unit-build.py, so multi-line function heads and conditional braces are handled. Kinds, by symbol prefix:
  spec     aot_s_/aot_f_ bodies (the speculative and the region bodies)
  generic  aot_g_ bodies and the e_gfs_ flags
  inline   ei_ always-inline copies of callees
  tramp    aot_nx_/aot_nb_/aot_rx_/aot_nr_ trampolines
  entry    aot_n_ entries, aot_fs_
  region   rg_/rk_/rgc_ per-function support (caches, init, claims)
  helper   e_*/q2_* helpers of the prelude
  other    everything else at file scope (types, tables, macros, declarations)
"""
import collections, importlib.util, os, re, sys

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('ub', os.path.join(here, 'unit-build.py'))
ub = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ub)

PREFIX = (('aot_s_', 'spec'), ('aot_f_', 'spec'), ('aot_g_', 'generic'), ('e_gfs_', 'generic'), ('ei_', 'inline'),
          ('aot_nx_', 'tramp'), ('aot_nb_', 'tramp'), ('aot_rx_', 'tramp'), ('aot_nr_', 'tramp'), ('aot_n_', 'entry'),
          ('aot_fs_', 'entry'), ('rg_', 'region'), ('rk_', 'region'), ('rgc_', 'region'), ('rgga_', 'region'), ('rgp_', 'region'),
          ('e_', 'helper'), ('q2_', 'helper'), ('qjs_aot_', 'helper'))


def kind_of(name):
    for p, k in PREFIX:
        if name.startswith(p):
            return k
    return 'other'


def main():
    path = sys.argv[1]
    top = int(sys.argv[sys.argv.index('--top') + 1]) if '--top' in sys.argv else 10
    src = open(path, errors='replace').read().split('\n')
    items = ub.parse_items(src)
    lines = collections.Counter()
    nbytes = collections.Counter()
    nfn = collections.Counter()
    fns = []
    for it in items:
        if it.kind == 'code':
            ub.classify(it)
        if it.kind in ('fndef', 'data', 'proto'):
            k = kind_of(it.name or '')
        else:
            k = 'other'
        lines[k] += it.nlines
        nbytes[k] += len(it.text) + 1
        if it.kind == 'fndef':
            nfn[k] += 1
            fns.append((it.nlines, it.name))
    total = len(src)
    stubs = sum(1 for l in src if re.match(r'^\s*(?:[a-z]+_\d+|x_\d+|vx_\d+|dx_\d+|ix_\d+)\s*:', l))
    if '--tsv' in sys.argv:
        print('\t'.join([os.path.basename(os.path.dirname(os.path.abspath(path))), str(total), str(sum(nbytes.values()))] +
                        [str(lines[k]) for k in ('spec', 'generic', 'inline', 'tramp', 'entry', 'region', 'helper', 'other')]))
        return
    print('%s: %d lines, %.1f MB, %d C functions, %d exit-stub labels' % (path, total, sum(nbytes.values()) / 1e6, len(fns), stubs))
    print('%-10s %9s %6s %7s %6s' % ('kind', 'lines', '%', 'MB', 'fns'))
    for k, n in lines.most_common():
        print('%-10s %9d %5.1f%% %7.2f %6d' % (k, n, 100.0 * n / total, nbytes[k] / 1e6, nfn[k]))
    fns.sort(reverse=True)
    print('largest functions:')
    for n, nm in fns[:top]:
        print('  %7d lines  %s' % (n, nm))
    print('functions over 3000 lines: %d' % sum(1 for n, _ in fns if n > 3000))


main()
