/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * The in-process half of the Intl benchmark harness.
 *
 * PURPOSE
 *   Produce a steady-state ns/op figure for one operation that is comparable
 *   between `intl-cli-apple` (QuickJS + this module) and `node` (V8 + ICU),
 *   given that the two engines share nothing but ECMAScript.
 *
 *   Motivated by: nobody had measured `modules/intl` against anything. See
 *   `docs/intl-vs-node.md`.
 *
 * CONTRACT
 *   `run.mjs` concatenates THIS FILE, then a workload from `workloads/`, then
 *   `harness-tail.js`, and writes the result to one temporary file which is fed
 *   byte-for-byte identically to every engine. There is no module loader in
 *   `intl-cli`, so concatenation is the mechanism; it also removes "were they
 *   running the same source" as a question.
 *
 *   A workload calls:
 *
 *     bench(name, body)              body(i) -> number, accumulated into a sink
 *     bench(name, body, {opts})      opts.minMs, opts.reps, opts.n, opts.sinkMayDiffer
 *     setup(fn)                      run once before calibration, value ignored
 *
 *   Output on stdout, one line per row, tab separated:
 *
 *     #ROW <name> <ns_per_op_min> <ns_per_op_median> <n> <reps> <sink> <flags>
 *     #END <rowcount>
 *
 *   `#END` is what makes a truncated or crashed run distinguishable from a fast
 *   one. `run.mjs` requires it AND exit status 0; a cost table in this module's
 *   history was once produced by runs that were exiting 1.
 *
 * HOW IT DEFEATS DEAD-CODE ELIMINATION
 *   Every body must return a number. The harness accumulates it into `sink`,
 *   and prints `sink` on the row. `run.mjs` asserts the sink is identical
 *   across repeated runs and across engines unless the row is explicitly
 *   flagged `sinkMayDiffer` (which is only correct for rows whose output
 *   depends on the platform's CLDR version). This is a free differential test:
 *   a row whose sink diverges between node and us is a correctness finding, and
 *   a row whose sink is 0 or NaN when it should not be is a harness bug.
 *
 * WHAT IT DELIBERATELY DOES NOT DO
 *   - It does not try to measure sub-microsecond operations in one shot. It
 *     calibrates `n` until a block takes at least `minMs` (default 50 ms) of
 *     `Date.now()`, so millisecond clock granularity contributes under 2%.
 *   - It does not subtract loop overhead. `__loop_overhead` is emitted as its
 *     own row so a reader can see it and decide; silently subtracting it would
 *     hide the fact that QuickJS's interpreted loop is not free and V8's is.
 *   - It does not measure startup, GC or RSS. Those are separate arms in
 *     `run.mjs`, because blending cold and warm answers neither question.
 *   - It does not report a mean. Min and median only; a mean over a shared
 *     machine is the noise, not the signal.
 */

var __rows = [];
var __sink = 0;

/*
 * How many iterations the correctness sink is computed over. Fixed, and
 * deliberately not a round number or a power of two, so that a body keyed on
 * `i % 4` or `i & 1` is exercised across all of its cases.
 */
var __VERIFY_N = 97;

if (typeof print === "undefined") {
  // node has no `print`. This is the ONLY environment adaptation in the
  // harness, and it is unconditional rather than wrapped in a silent
  // capability guard -- a guard of that shape once made every arm of an Intl
  // cost table exit 1 while still printing a plausible number.
  globalThis.print = function () {
    console.log(Array.prototype.join.call(arguments, " "));
  };
}

function setup(fn) {
  fn();
}

function __blockNs(body, n) {
  var s = 0;
  var t0 = Date.now();
  for (var i = 0; i < n; i++) s += body(i);
  var t1 = Date.now();
  __sink += s;
  return { ms: t1 - t0, ns: ((t1 - t0) * 1e6) / n, s: s };
}

function bench(name, body, opts) {
  opts = opts || {};
  var minMs = opts.minMs || 50;
  var reps = opts.reps || 5;

  // Warm up. Both engines need it, for different reasons: V8 to tier up, and
  // QuickJS to fill inline caches, grow the heap and reach a steady GC rhythm.
  __blockNs(body, Math.min(opts.n || 1000, 1000));

  var n = opts.n || 0;
  if (!n) {
    n = 64;
    for (;;) {
      var probe = __blockNs(body, n);
      if (probe.ms >= minMs || n >= 1 << 24) break;
      // Grow by the ratio needed, with a floor of 2x and a ceiling of 64x so a
      // zero-millisecond probe cannot make this loop forever or overshoot into
      // a minute-long block.
      var grow = probe.ms > 0 ? Math.ceil((minMs * 1.3) / probe.ms) : 8;
      if (grow < 2) grow = 2;
      if (grow > 64) grow = 64;
      n = n * grow;
    }
  }

  var samples = [];
  for (var r = 0; r < reps; r++) samples.push(__blockNs(body, n).ns);
  samples.sort(function (a, b) {
    return a - b;
  });

  /*
   * The reported sink is computed over a FIXED iteration count, outside the
   * timed region. It has to be: the calibrated `n` differs between engines and
   * between runs, so a sink accumulated over the timed loop would differ for
   * that reason alone and would say nothing about correctness. The first
   * version of this harness made exactly that mistake and reported 20 false
   * "sink divergences" -- the instrument measuring itself.
   */
  var vs = 0;
  for (var v = 0; v < __VERIFY_N; v++) vs += body(v);
  __sink += vs;
  var sinkStr =
    typeof vs === "number" && isFinite(vs)
      ? String(Math.round(vs * 1e6) / 1e6)
      : String(vs);

  __rows.push(
    "#ROW\t" +
      name +
      "\t" +
      samples[0].toFixed(3) +
      "\t" +
      samples[samples.length >> 1].toFixed(3) +
      "\t" +
      n +
      "\t" +
      reps +
      "\t" +
      sinkStr +
      "\t" +
      (opts.sinkMayDiffer ? "sinkMayDiffer" : "-")
  );
}
