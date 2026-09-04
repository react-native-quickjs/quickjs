# The `Intl` benchmark suite

Answers one question, repeatably, in one command: **how far is `modules/intl`
from node, per service and per operation kind?**

```sh
node modules/intl/bench/run.mjs --runs 5
```

Node is the bar, not Hermes. Node is V8 with ICU compiled in — C++ all the way
down — and it is the honest ceiling for an architecture that is a JavaScript
layer over a platform backend. Hermes ships four ECMA-402 services to this
module's ten, so on six of them the Hermes answer is "throws" and there is no
comparison to make.

## Layout

| path | what it is |
| --- | --- |
| `run.mjs` | the runner: spawns engines, aggregates, validates, compares against a baseline |
| `ab.mjs` | the A/B comparator: two of *our* binaries, interleaved, min-of-mins, with a sink diff |
| `harness.js` | the in-process half: calibration, timing, the sink |
| `harness-tail.js` | emits the loop-overhead row and the `#END` sentinel |
| `workloads/*.js` | one file per service; each file's header says what it models |

`run.mjs --help` prints the full contract.

## How a run is assembled

There is no module loader in `intl-cli`, so `run.mjs` **concatenates**
`harness.js` + the workload + `harness-tail.js` into one temporary file and
hands that exact file to every engine. That is the mechanism, and it also
removes "were they running the same source" as a question anyone has to ask.

## Writing a workload

```js
bench("fmt-currency", function (i) {
  return nfCur.format(i + 0.5).length;             // must return a number
});

bench("ctor-compact", function () {
  return new Intl.NumberFormat("de", { notation: "compact" }).format(1).length;
}, { sinkMayDiffer: true });                        // output is CLDR data
```

- The body **must return a number**. The harness sums it, prints it, and
  compares it — that is what stops an engine folding the call away.
- `minMs` (default 50), `reps` (default 5) and `n` (default: calibrated) are
  the only other options.
- Start the file with a comment saying **what real code the rows model**. A row
  nobody can motivate is a row nobody can act on.

## What the runner validates on every run

A run that fails any of these fails the whole invocation with a non-zero exit
status. They exist because each one has caught something here.

| check | what it caught |
| --- | --- |
| exit status 0 | `04-locale.js` used `zh-cmn-Hans-CN`, which *both* engines reject; the run was exiting 1 while printing a plausible partial table |
| a `#END` sentinel | a workload that throws halfway prints a complete-looking table; the sentinel is the only way to tell |
| identical row set between runs and engines | a row that only appears on one engine is a missing feature, not a fast one |
| per-row sink identical **across runs of one engine** | a non-deterministic body has no meaningful timing |
| per-row sink identical **across engines**, unless `sinkMayDiffer` | a free differential test against ICU. It found that Apple renders de-DE `50%` where ICU 77 renders `50 %` — deviation D4 |

The sink is computed over a **fixed 97 iterations outside the timed region**.
The first version accumulated it inside the timed loop, where the calibrated
iteration count differs between engines, and reported 20 false divergences.
That is the instrument measuring itself, and it is why the count is fixed.

## The three arms, and why they are separate

```sh
node modules/intl/bench/run.mjs --arm warm     # steady state, ns/op
node modules/intl/bench/run.mjs --arm cold     # startup / TTI, whole process
node modules/intl/bench/run.mjs --arm mem      # peak RSS
```

**warm** — N separate *process* invocations per engine. The table reports the
median of the per-run medians, and beside it `(max − min) / median` across
runs. On a shared machine that spread has reached 300% on a sub-10 ns row.
**A difference smaller than the spread is not a result**, and `--baseline`
enforces that: it labels a row FASTER or SLOWER only when the change exceeds
the larger of 5% and either run's own spread.

**cold** — whole-process wall time for a script that touches `Intl` once,
*net of the same script with the Intl line removed*. Process spawn and engine
boot are enormous and completely different between the two engines, so they are
differenced away rather than reported. Every case asserts the probe printed
`ok`; a cold arm that measures a program that did not run is the easiest
mistake in this whole file.

**mem** — peak RSS through `/usr/bin/time`. A 1.1x speedup that doubles memory
is a regression on a phone. Note the units: macOS reports **bytes** and GNU
reports **kilobytes**; `peakRssKb` documents that, because the first version
got it wrong by 1024x.

`--arm all` (the default) runs all three.

## `__loop_overhead`

Every table carries a `__loop_overhead` row — an empty `return i & 1` body
through the same harness. It is **reported and never subtracted**. QuickJS
interprets the harness loop at about 17 ns and V8 compiles it to about 4 ns;
on the sub-100 ns rows that gap is most of the difference and on the microsecond
rows it is noise. Silently subtracting it would hide which of those a reader is
looking at.

## Counters: proving a fast path is reached

`Intl.__rnqjsPerf.stats()` reports the hit and miss counts for every fast path
in `js/intl.js` — the implicit-formatter memo, the NumberFormat pre-rounding
fast path, the PluralRules operand fast path, the BCP-47 canonicalization memo
(`canonHits` / `canonMisses`), the Segmenter boundary memo (`segmentHits` /
`segmentMisses`) and the NumberFormat exact-double route (`exactDoubleHits` /
`exactDoubleMisses`). It is always on and costs an integer increment.

For **native** fast paths there is a second instrument, off by default:
`-DRNQJS_INTL_ABLATION` compiles in runtime-switched arms selected by
`RNQJS_INTL_ABL`, so a layer's cost can be measured by removing it rather than
inferred by subtracting other numbers. The contract is in
`cpp/IntlPlatform.h`; the arms are documented at their `if`s; and the first use
of it replaced a subtracted 303 ns with a measured 20-35 ns
(`docs/intl-string-seam.md`). An ablation build produces deliberately wrong
answers on every arm but 0 and must never be scored for correctness.

**No timing in `docs/intl-vs-node.md` is quoted without the counter beside
it.** This project has shipped a fast path with zero hits that survived a
spike, an audit and a relay. `modules/intl/test/invariants.js` asserts the hit
counts, so a change that silently stops taking a fast path fails a test rather
than merely getting slower.

`Intl.__rnqjsPerf.setEnabled(false)` turns the memo off. That is how a control
binary is built: flip the initializer of `memoEnabled` in `js/intl.js`, rebuild,
and score both — a binary that differs from the shipping one by exactly one
token. `modules/intl/` is not under version control in its own right, so
"the binary from before the change" is not otherwise reconstructable.

## `ab.mjs` — deciding whether a change helped

```sh
node modules/intl/bench/ab.mjs \
     --a /path/to/intl-cli-apple-before \
     --b build-intl/modules/intl/intl-cli-apple \
     --runs 7 --workload 06-
```

`run.mjs` answers "how far are we from node?". `ab.mjs` answers the narrower
question an optimization actually needs: **is B faster than A on this row, and
by how much?** It exists because the two questions want different statistics,
and because the machine this module is developed on is shared.

Three differences from `run.mjs`, each of which earned its place:

- **Interleaved.** A and B alternate process invocations, and the leading arm
  alternates too, so a load spike lands on both arms. `run.mjs` runs each
  engine's N processes as a block, which is right for its purpose and wrong for
  this one.
- **Min-of-mins.** Each process already reports the minimum of its `reps`
  blocks; this takes the minimum again across processes. Interference makes a
  measurement slower and never faster, so the minimum converges under load where
  the median does not. The median-of-medians is printed beside it and a row
  whose two estimators disagree in direction is flagged `unclear` rather than
  given a verdict. **On this machine at `loadavg` 17.1, `run.mjs` reported
  per-row spreads of 195% and 382% on rows whose min-of-mins was stable to
  1.3%.**
- **It is a differential test.** Every row's sink is compared between A and B
  and a mismatch fails the run, whatever the flags say. `sinkMayDiffer` exists
  because Apple and ICU legitimately disagree about a few renderings; two builds
  of *this* module have no such licence.

**What it caught the day it was written.** A `NumberFormat` change that improved
`fmt-integer` by 2.42x and regressed `fmt-large-grouped` by 1.27x, because the
fast path changed a cached mode in the backend and made that cache thrash for
every call that did *not* take the fast path. In the against-node table that row
would simply have looked like one that failed to improve.

Exit status: 0 if every run exited 0, printed `#END`, and every sink matched;
1 on a sink divergence; 2 on a failed run or a missing binary.

The final number in a document should still come from `run.mjs` on the quietest
machine available. `ab.mjs` decides whether to keep a change; `run.mjs` reports
where the module stands.

## Comparing against a stored run

```sh
node modules/intl/bench/run.mjs --runs 5 --json modules/intl/bench/results/now.json
node modules/intl/bench/run.mjs --runs 5 --baseline modules/intl/bench/results/2026-07-27-after.json
```

Every stored result carries its environment in `meta`: machine, load average,
node version and its ICU version, the engine path, and — read back from the
binary itself, not assumed — which backend answered.

## What this suite deliberately does not do

- **It does not run on a device.** Everything here is the Apple backend on
  macOS. The per-call JNI cost on Android is unmeasured; the measurement that
  would settle it is one formatter and 100,000 `format()` calls on a physical
  mid-range device with a counter on the native entry point.
- **It does not profile.** It reports where time goes at the granularity of one
  public API call. `workloads/07-decomposition.js` goes one level finer by
  differencing carefully chosen probes, and its header explains which
  differences are honest and which are traps; anything finer than that belongs
  in a profiler and its answer belongs in a document.
- **It does not check correctness beyond the sink.** The correctness
  instruments are listed in [the module README](../README.md#testing) and they
  are not optional after a change measured here.
