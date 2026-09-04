/*
 * Attribution rows: where does a format() call actually go?
 *
 * WHAT THIS MODELS
 *   Nothing an application does. Every row here is a *probe*, chosen so that
 *   the difference between two rows isolates one layer of the stack. It exists
 *   because the first attempt at optimizing `NumberFormat.format` was aimed at
 *   the wrong layer, on the strength of exactly one of these differences read
 *   in isolation.
 *
 *   The honest reading, and the trap in it:
 *
 *     fmt-nan        `preRound` returns null for NaN, so this is the JS
 *                    wrapper plus one backend call and NOTHING else. It is
 *                    tempting to call this "the floor" and attribute
 *                    (fmt-integer − fmt-nan) to our JavaScript. THAT IS WRONG,
 *                    and it was wrong here: NaN is also the cheapest possible
 *                    *backend* call, because NSNumberFormatter has no digits to
 *                    render. The difference is JS work AND backend work
 *                    together.
 *     fmt-scientific `preRound` also returns null (the scaled notations are the
 *                    backend's own rounding), but the backend does real work.
 *                    So this is a much better estimate of "one real backend
 *                    call with no JS digit arithmetic".
 *     getter-only    reads `nf.format` and does not call it. ECMA-402 makes
 *                    `format` an accessor returning a bound function, so every
 *                    `nf.format(x)` pays a WeakMap lookup for the internal
 *                    slots before anything else happens.
 *     bound-*        the same call with the accessor hoisted out of the loop.
 *                    (bound-integer − fmt-integer) is what the accessor costs.
 *
 *   Read `fmt-scientific` against `bound-integer`, not `fmt-nan` against
 *   `fmt-integer`. The full attribution, including the Objective-C side that
 *   this file cannot see, is in docs/intl-vs-node.md.
 */

setup(function () {
  new Intl.NumberFormat("de-DE").format(1);
});

var nf = new Intl.NumberFormat("de-DE");
var bound = nf.format;
var nfSci = new Intl.NumberFormat("de-DE", { notation: "scientific" });
var boundSci = nfSci.format;
var nfCur = new Intl.NumberFormat("de-DE", { style: "currency", currency: "EUR" });
var boundCur = nfCur.format;

bench("getter-only", function () {
  return nf.format ? 1 : 0;
});

bench("fmt-integer", function (i) {
  return nf.format(i).length;
});

bench("bound-integer", function (i) {
  return bound(i).length;
});

bench("bound-fraction-1dp", function (i) {
  return bound(i + 0.5).length;
});

bench("bound-nan", function (i) {
  return bound(NaN).length + (i & 0);
});

bench("bound-scientific", function (i) {
  return boundSci(i).length;
}, { sinkMayDiffer: true });

/*
 * Currency pins minimumFractionDigits === maximumFractionDigits === 2, so the
 * digit count handed to the backend is the SAME on every call. Decimal style
 * with maximumFractionDigits 3 varies it between 0 and 3. The pair is what
 * shows whether the Objective-C fraction-digit setter memo is earning its keep
 * — it hits every time here and only sometimes above.
 */
bench("bound-currency-fixed-frac", function (i) {
  return boundCur(i + 0.5).length;
});

bench("bound-alternating-frac", function (i) {
  return bound(i % 2 ? i : i + 0.25).length;
});

/*
 * String(x) is the input to the whole pre-rounding pipeline and the value the
 * fast path returns verbatim. It is here as the absolute floor: no
 * implementation of format() can be cheaper than producing this string.
 */
bench("String(x)-floor", function (i) {
  return String(i).length;
});
