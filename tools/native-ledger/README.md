# native-ledger

A compiler-assisted accountant for interpreter overhead. It answers a narrow,
quantitative question:

> How much **native work** does one engine do that another does not, and in what
> kind of work does the difference live?

It counts work. It does **not** attribute time: production cycle samples locate
expensive regions, and controlled structural changes establish achievable savings.
Keeping work attribution and time attribution separate is the point of the tool --
the two disagree more often than not, and conflating them is how "the profiler says
30% here" turns into a change that measures zero.

## What it is not

- Not a profiler. It emits instruction counts, never timings from an instrumented
  binary.
- Not a sampling tool. Samples say where the CPU was, not what it was doing.
- Not a source-level analyser. The unit is the machine instruction.

## Pipeline

```
inventory   disassemble the optimized interpreter (-O3 -g); per instruction record
            address, symbol, source line, mnemonic, operands
counters    run the same, uninstrumented,-O3 build under callgrind
            (--dump-instr=yes) and read the exact dynamic count per instruction
            address
classify    assign each instruction to one of eight cost categories, or to an
            explicit unknown/mixed row, with a confidence per instruction
ledger      dynamic work = sum over instructions of (retired count x category);
            normalise per workload iteration; subtract the two builds; run the
            reconciliation checks
```

The counter was `-fsanitize-coverage=trace-pc-guard` with a hook. That instrument
does not leave a guard at every basic block in an -O3 build, so a "block" was a
region of several basic blocks, and the ledger had no correct extent for it: walking
to the first terminator undercounted 2.3x, walking to the next guard overcounted (an
earlier ledger was inflated about 6x that way). Its totals were therefore a
coverage-dependent subset -- 35% of the program's instructions on the register build,
43% on the stack -- and totals and category shares moved with the build. Callgrind is
~100x slower, and it is exact, deterministic, and per instruction, so the total IS the
program's and coverage is ~100%. `--truth` holds it to that.

Timings still come from an uninstrumented run through `--ms`; callgrind never supplies
time.

## Usage

```sh
python3 tools/native-ledger/ledger.py \
    --workload path/to/workload.js \
    --bytecodes 953603563 992503972 \
    --ms 2963 1695 \
    --ghz 3.0 \
    --json /tmp/ledger.json
```

`--ghz` is optional and only labels the cycle columns. Cycles are **derived**, never
measured: the tool has no PMU here, so `cycles = ns x GHz` from the uninstrumented
`--ms`, and the per-category cycle split is the category's work share of that total
(which assumes uniform cost per native instruction -- the one thing this tool's own
introduction says not to assume). Read the cycle columns as an order-of-magnitude
ledger of where elapsed time would sit if cost were uniform, not as attribution. The
absolute block counts and the category totals are measured; the split is inferred.

`--bytecodes` and `--ms` are measured separately (an opcode counter build and an
uninstrumented timing run) and are used only for the per-instruction ratios. Both
engine variants are built from the same tree: register is `QJS_REGVM=1`, stack is
`QJS_REGVM=0`.

Requires `clang` with `-fsanitize-coverage=trace-pc-guard` and `llvm-objdump`
(override with `CC` and `LLVM_OBJDUMP`).

## Output

Per category, dynamic native instructions per world for each engine, the difference,
and each side's share; then natives per bytecode instruction and ns per native; then
the reconciliation block. With `--json` the same numbers land in a file for
downstream use.

## Reconciliation checks

The tool reports, and refuses to pretend otherwise:

- **Category totals must equal the inventoried dynamic total.** Printed as a check.
- **Coverage**: blocks used versus blocks counted, and the hook's invocation total.
- **Confidence**: the share of work attributed from known provenance (high), from a
  mnemonic (medium), and not attributable at all (low, the unknown/mixed row).
- **Excess left unknown**: how much of the register-minus-stack difference sits in
  the unknown row. If this is large, no ranking of causes can be trusted.

## Limitations, stated plainly

- **No compiler provenance per instruction.** Category assignment uses the macro a
  line belongs to, the helper an inlined body belongs to, and otherwise the
  mnemonic. An `ldr` cannot be told from a spilled interpreter pointer to a JS value
  this way, and `add` cannot be told between addressing and semantics at all; those
  land in `unknown_mixed` rather than being forced into a bucket. Closing this needs
  an LLVM backend pass over the register-allocated machine representation, or column
  information from the DWARF line table mapped back to source tokens. Until then the
  totals are sound and part of the category split is not.
- **Single-threaded only.** Counters are plain increments; an atomic per block would
  distort the measurement. Do not use this on a multi-threaded sample.
- **Full inlining only where the debug info says so.** An out-of-line call
  contributes the callee's whole body at the call site's count.
- **Processor Trace is available but not usable here.** On Apple Silicon it records
  only for a binary carrying `com.apple.security.get-task-allow`
  (`codesign --force --sign - --entitlements pt.entitlements <binary>`); with that it
  records fine, and `xctrace export --toc` lists `processor-trace-points` and
  `processor-trace-intervals`. Exporting either for this workload does not complete:
  a 2-second recording of roughly 500M instructions produced no XML in 10 minutes.
  PT is therefore useful for short windows and for control-flow questions, not for
  whole-workload accounting. Do not build the ledger around an assumed export
  capability.
- **Block spans end at the terminator**, not at the next instrumented address; hot
  and cold splitting leaves those far apart. Getting this wrong inflated an earlier
  ledger by about 6x.
