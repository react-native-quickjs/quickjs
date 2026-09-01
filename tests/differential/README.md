# Differential corpus

Every file in `corpus/` is executed by this engine and by node, and the two
outputs are compared byte for byte. `run.mjs` is the runner; `qjs-run.c` is the
minimal shell it drives, exposing `print` and nothing else.

Byte-for-byte against node rather than a written expectation, because an
expectation written by the same person who wrote the code encodes the same
misunderstanding. node is an independent implementation of the same
specification, so a difference is evidence.

`pending/` holds fixtures that do not pass yet. `run.mjs` does not look there,
so they do not gate the build. See `pending/README.md` for what each one waits
on.

## Reading a corpus file's header

Most of these files were written to guard a specific engine optimization, and
their headers name it. Two kinds of reference in those headers cannot be
followed from this repository:

- **A bare patch number** — `patch 0048`, `patch 0023` — names a patch in the
  engine-optimization series, which is not in this tree. Those patches arrive
  later, each with a header describing what it does in plain words; the corpus
  header should be rewritten to match at that point.
- **A `docs/…md` path** names a measurement write-up that is not in this
  repository either.

The tests themselves do not depend on any of it. A corpus file asserts only
JavaScript semantics, so it passes against any conformant engine and will keep
passing when the optimization it guards lands. The reference tells you *why*
the file exists, not what it checks.

## Adding a file

Plain ES5, prints with `print()`, and deterministic: no `Math.random`, no
`Date.now`, and no iteration over host objects whose key set differs between
engines.
