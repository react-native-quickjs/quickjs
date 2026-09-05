/*
 * 25-array-holes.js -- the dense-array representation cliff.
 *
 * WHAT THIS MODELS
 * ----------------
 * Not an application, a REPRESENTATION. Upstream QuickJS gives a JS_CLASS_ARRAY
 * a dense JSValue vector (`fast_array`) and abandons it -- permanently, for the
 * object's whole lifetime -- the first time a store lands past
 * u.array.count. One skipped index turns the array into a property hash table.
 * `a = Array(n); a[2] = x` is enough, and so is a matrix filled row-offset, a
 * ring buffer, an array indexed by a numeric id, anything written backwards,
 * and every `for (i = 1; ...)` fill.
 *
 * docs/general-javascript-standing.md FINDING 1 measured the cliff at
 * 1,000,000 elements against Hermes and found quickjs 1.36-1.42x AHEAD while
 * the array stays dense and 1.75x (writes) / 2.11x (reads) BEHIND the moment
 * one index is skipped. The four rows below are that measurement, in this
 * repository's harness so both engines run byte-identical source.
 *
 * THE ROWS, AND WHAT EACH ONE ISOLATES
 * ------------------------------------
 *   fill-from-0        control. Pure appends; never leaves the dense path on
 *                      any engine, before or after any change here.
 *   fill-from-2        THE CLIFF, write side. Identical work to fill-from-0
 *                      except the first store is at index 2 instead of 0.
 *   prefilled-from-2   the same skipped index, but onto an array that is
 *                      ALREADY dense because .fill() populated it. Every store
 *                      overwrites a present element, so no representation
 *                      change is possible. This is the row that proves
 *                      fill-from-2's cost is the representation and not the
 *                      loop bounds.
 *   read-back-from-2   THE CLIFF, read side: read every element of the array
 *                      fill-from-2 produced.
 *   read-back-from-0   control for read-back-from-2, same size, dense.
 *
 * FALSIFIABLE PREDICTION (recorded 2026-07-30, BEFORE the measurement, for
 * patch 0046-holey-fast-arrays)
 * ---------------------------------------------------------------------------
 * The patch gives fast arrays a hole representation, so fill-from-2 allocates
 * ONE extra vector slot pair and then appends exactly like fill-from-0.
 * Therefore, on qjs, after the patch:
 *
 *   P1  fill-from-2 / fill-from-0            <= 1.15    (was ~1.75 vs Hermes)
 *   P2  read-back-from-2 / read-back-from-0  <= 1.20    (was ~2.1  vs Hermes)
 *   P3  fill-from-0 unchanged within noise (+-4%): the dense path must not
 *       pay for the hole test. This is the regression check, and it is the
 *       one that matters -- a hole representation that taxes dense arrays is
 *       a net loss, because dense is the common case.
 *   P4  prefilled-from-2 unchanged within noise: this row creates no holes at
 *       all (verified by a holeyadmit=0 counter), so any movement here is
 *       measurement error and invalidates the other three rows.
 *
 * If P1 or P2 come out above their bound, the mechanism did not fire and the
 * hit-rate counters (holeyadmit / slowget in the instrumented build described
 * in docs/array-hole-representation.md) will say why.
 *
 * N is 100,000 rather than the 1,000,000 of the standing document so that one
 * unit of work stays a few milliseconds and the harness can calibrate; the
 * representation change happens at the first store and is size-independent.
 */

var N = 100000;

function fillFrom(start) {
  var a = new Array(N);
  for (var i = start; i < N; i++) a[i] = i;
  return a;
}

function sumOf(a) {
  var s = 0;
  for (var i = 0; i < N; i++) s += a[i] | 0;
  return s;
}

/* sum of 0..N-1 */
var SUM_ALL = (N * (N - 1)) / 2;
/* the same, minus the two skipped indices 0 and 1 (0 + 1 = 1) */
var SUM_FROM_2 = SUM_ALL - 1;

bench({
  name: 'arrayholes/fill-from-0',
  unit: 'element',
  run: function () {
    var a = fillFrom(0);
    return a[N - 1] + a[0];
  },
  expect: function (v) {
    /* N-1 + 0. Not a constant a broken loop could also return: a loop that
       never ran leaves a[N-1] undefined and this becomes NaN. */
    return v === N - 1;
  },
});

bench({
  name: 'arrayholes/fill-from-2',
  unit: 'element',
  run: function () {
    var a = fillFrom(2);
    return a[N - 1] + (0 in a ? 1000000 : 0);
  },
  expect: function (v) {
    /* index 0 must still be ABSENT after the fill -- if the engine ever
       materialises holes as undefined this gate fails. */
    return v === N - 1;
  },
});

bench({
  name: 'arrayholes/prefilled-from-2',
  unit: 'element',
  run: function () {
    var a = new Array(N).fill(0);
    for (var i = 2; i < N; i++) a[i] = i;
    return a[N - 1] + (0 in a ? 1 : 1000000);
  },
  expect: function (v) {
    /* here index 0 IS present (fill wrote it), so the +1 branch must be taken */
    return v === N;
  },
});

var denseArr = null;
var holeyArr = null;

bench({
  name: 'arrayholes/read-back-from-0',
  unit: 'element',
  setup: function () {
    denseArr = fillFrom(0);
  },
  run: function () {
    return sumOf(denseArr);
  },
  expect: SUM_ALL,
});

bench({
  name: 'arrayholes/read-back-from-2',
  unit: 'element',
  setup: function () {
    holeyArr = fillFrom(2);
  },
  run: function () {
    return sumOf(holeyArr);
  },
  /* holes read as undefined; `undefined | 0` is 0, so the two skipped indices
     contribute 0 and the total is SUM_ALL - (0 + 1). */
  expect: SUM_FROM_2,
});
