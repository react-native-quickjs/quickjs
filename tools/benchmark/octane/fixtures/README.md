# Octane 2.0 — vendored benchmark corpus

Third-party source, unmodified. **Copyright the V8 project authors**, released
under the three-clause BSD licence reproduced at the top of every file. Nothing
in this directory is our code and nothing in it should be edited; if a file here
differs from upstream Octane, that is a defect.

## Why it is vendored

Every Octane number in `docs/` was produced by
[`bench/spikes/octane-flatten.mjs`](../../spikes/octane-flatten.mjs), which used
to read the corpus from a sibling project on one machine. That made every one of
those pages **unreproducible from this repository alone** — and the sibling tree
is one this project is not permitted to read. The defect was found on
2026-07-30 during the inline-cache investigation and is recorded in
`docs/octane-property-access-and-inline-caches.md`.

Vendoring is 10 MB, which is a real cost, paid because a benchmark whose corpus
cannot be reproduced cannot be used to defend a performance claim.

## What is here

The 20 raw source files, in the load order `octane-flatten.mjs` expects:

| group | files |
| --- | --- |
| harness | `base.js` |
| richards | `richards.js` |
| deltablue | `deltablue.js` |
| crypto | `crypto.js` |
| raytrace | `raytrace.js` |
| earleyboyer | `earley-boyer.js` |
| regexp | `regexp.js` |
| splay | `splay.js` |
| navierstokes | `navier-stokes.js` |
| pdfjs | `pdfjs.js` |
| mandreel | `mandreel.js` |
| gbemu | `gbemu-part1.js`, `gbemu-part2.js` |
| codeload | `code-load.js` |
| box2d | `box2d.js` |
| zlib | `zlib.js`, `zlib-data.js` |
| typescript | `typescript.js`, `typescript-input.js`, `typescript-compiler.js` |

These are the **raw** sources. They are not runnable on `qjs-bench` as they
stand, because the stock drivers call `load()`. Generate self-contained,
byte-identical-across-engines files instead:

```sh
node bench/spikes/octane-flatten.mjs /tmp/octane            # all groups
node bench/spikes/octane-flatten.mjs /tmp/octane crypto     # one group
```

`OCTANE_DIR` overrides this directory, for comparing against a differently
patched corpus.

## Two things to know before quoting a number from this corpus

**`zlib` returns `NaN` on both QuickJS and Hermes.** That is a corpus defect,
not an engine result. Exclude it rather than counting it as a tie.

**`crypto`'s dispatch count is not bit-deterministic** even in deterministic
mode — three base runs spanned 6.2 M dispatches (MEASURED 2026-07-30). Do not
read a small crypto delta as signal.

`codeload` is deliberately excluded from this project's Octane geomean: it
measures parse-and-compile throughput, and React Native ships precompiled
lazily-loaded bytecode, so the row does not correspond to anything the product
pays. See `docs/octane-rescore.md`.
