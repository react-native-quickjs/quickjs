/* aot_bench.js -- kernel acceptance for the AOT compiler.
 *
 * WHY THIS EXISTS. compiler_bench.js and jit_bench.js name compiler decisions
 * for the SHIPPING (stack) engine and the NG JIT. Neither exercises the AOT
 * frame/entry/exit/unwind/resume design, which must be right independently of
 * any per-opcode template. These kernels are run in
 * STEP-ONLY Layer A (every op through qjs_aot_step) and must produce the SAME
 * checksum as the interpreter at both sizes. Slow is allowed here; wrong is
 * not.
 *
 * CONVENTIONS (same as compiler_bench.js; see that file's four rules):
 *   - every kernel returns a checksum, asserted at n/8 and n;
 *   - the expected value is derived from the two-size relation, never hard-coded;
 *   - fixed iteration counts, never a time box;
 *   - nothing printed from inside a timed region.
 *
 * OUTPUT: one `##CB2## <name> <ms> <n> <checksum> <score> <inner> <span>` per
 * kernel, then `##CBSUM##`; a `Score:` line so the project's row classifier
 * accepts the file.
 *
 * ASYNC. An async/await kernel cannot be timed from a synchronous harness: the
 * continuation after `await` is a microtask, so the harness is driven by an
 * async main and the whole report is emitted after the promise queue drains
 * (qjs-bench drains pending jobs after eval). If the AOT async resume switch is
 * broken the report never prints -- a loud failure, not a silent fast number.
 */

var CB_REPS = 5;
var CB_SCALE = 1;
var CB_TARGET_MS = 25;
var CB_FILTER = "";
/* Pinned so both arms (interpreter and AOT) use the same inner repeat count;
   see compiler_bench.js's CB_INNER comment.  1 means one call per timed rep. */
var CB_INNER = {
  "call.native": 1, "call.js_leaf": 1, "call.ab": 1, "call.host": 1,
  "elem.f64": 1, "elem.dense_hole": 1, "gen.loop": 1, "gen.finally": 1,
  "try.catch": 1, "throw.leaf": 1, "unwind.nested": 1, "unwind.finally": 1,
  "finally.return": 1, "unwind.cross": 1,
  "callee.change": 1, "shape.change": 1,
  "call.math_change": 1, "proto.method_change": 1, "call.recursion": 1,
  "call.arity": 1, "math.edge": 1, "call.cfunc_change": 1,
  "call.stack": 1,
  "num.fma": 1, "num.negzero": 1, "num.nan": 1, "num.overflow": 1,
  "async.await": 8, "async.forawait": 8
};

var now = (typeof performance !== "undefined" && performance.now)
  ? function () { return performance.now(); }
  : function () { return Date.now(); };
var results = [], failures = [], warns = [];

/* A host function registered by the bench driver. Under a driver that does not
   register one, fall back to a JS leaf so the file still runs. */
var host_add = (typeof host_add === "function")
  ? host_add
  : function (a, b) { return (a + b) | 0; };
/* Reads its second formal even when unsupplied; the fallback mirrors that so
   the arity kernel still runs under a driver that does not register host_opt. */
var host_opt = (typeof host_opt === "function")
  ? host_opt
  : function (a, b) { return (a + (b === void 0 ? 0 : b)) | 0; };

function linear(full, small) {
  return isFinite(full) && isFinite(small) && full !== small;
}
function exact8(full, small) { return full === 8 * small; }
function anyv(full, small) { return full === full; }
function wrapsum(full, small, n) {
  var e = (n * (n - 1) / 2) | 0, m = ((n / 8) | 0) || 1, es = (m * (m - 1) / 2) | 0;
  return full === e && small === es;
}
function linear_f(full, small) {
  return isFinite(full) && isFinite(small) && Math.abs(full) !== Math.abs(small);
}

function bench(name, fn, n, check) {
  if (CB_FILTER && name.indexOf(CB_FILTER) < 0) return;
  n = Math.max(1, (n * CB_SCALE) | 0);
  var small = fn((n / 8) | 0 || 1), full = fn(n);
  if (!check(full, small, n)) {
    failures.push(name + ": checksum full=" + full + " small=" + small);
    results.push({ name: name, ms: -1, n: n, sum: full, inner: 0, bad: true });
    return;
  }
  fn(n);
  fn(n);
  var inner = CB_INNER[name] | 0;
  if (inner < 1) {
    var pr = 1, pd = 0;
    for (;;) {
      var pt = now();
      for (var pq = 0; pq < pr; pq++) fn(n);
      pd = now() - pt;
      if (pd >= CB_TARGET_MS || pr >= 8192) break;
      pr = pr * 2;
    }
    var per = pd / pr;
    inner = per > 0 ? Math.ceil(CB_TARGET_MS / per) : 1;
    if (inner < 1) inner = 1;
    if (inner > 8192) inner = 8192;
    warns.push(name + " auto inner=" + inner);
  }
  var best = Infinity, sum = full, q, r, t0, dt;
  for (r = 0; r < CB_REPS; r++) {
    t0 = now();
    for (q = 0; q < inner; q++) sum = fn(n);
    dt = now() - t0;
    if (dt < best) best = dt;
  }
  if (sum !== full) {
    failures.push(name + ": NOT REPEATABLE -- " + full + " -> " + sum);
    results.push({ name: name, ms: -1, n: n, sum: sum, inner: inner, bad: true });
    return;
  }
  results.push({ name: name, ms: best / inner, n: n, sum: sum, inner: inner,
                 span: best, bad: false });
}

/* Async twin of bench(): `fn` returns a promise for the checksum. Each timed
   iteration awaits, so the job-queue latency is part of the measured cost. */
async function benchAsync(name, fn, n, check) {
  if (CB_FILTER && name.indexOf(CB_FILTER) < 0) return;
  n = Math.max(1, (n * CB_SCALE) | 0);
  var small = await fn((n / 8) | 0 || 1), full = await fn(n);
  if (!check(full, small, n)) {
    failures.push(name + ": checksum full=" + full + " small=" + small);
    results.push({ name: name, ms: -1, n: n, sum: full, inner: 0, bad: true });
    return;
  }
  await fn(n);
  await fn(n);
  var inner = CB_INNER[name] | 0 || 8;
  var best = Infinity, sum = full, q, r, t0, dt;
  for (r = 0; r < CB_REPS; r++) {
    t0 = now();
    for (q = 0; q < inner; q++) sum = await fn(n);
    dt = now() - t0;
    if (dt < best) best = dt;
  }
  if (sum !== full) {
    failures.push(name + ": NOT REPEATABLE -- " + full + " -> " + sum);
    results.push({ name: name, ms: -1, n: n, sum: sum, inner: inner, bad: true });
    return;
  }
  results.push({ name: name, ms: best / inner, n: n, sum: sum, inner: inner,
                 span: best, bad: false });
}

/* ------------------------------------------------------------------ *
 * Call shapes.
 * ------------------------------------------------------------------ */
function n_sqrt_floor_min(n) {          /* native call in loop */
  var s = 0.0;
  for (var i = 0; i < n; i++) s += Math.sqrt(i) + Math.floor(i * 0.5) + Math.min(i, 7);
  return s;
}
function leaf_add(x) { return (x + 1) | 0; }   /* JS leaf: inline candidate */
function n_js_leaf(n) {
  var s = 0;
  for (var i = 0; i < n; i++) s = leaf_add(s);
  return s;
}
function B_ab(x) { return (x * 3 + 1) | 0; }   /* A -> B */
function A_ab(x) { return B_ab(x) + B_ab(x + 1); }
function n_call_ab(n) {
  var s = 0;
  for (var i = 0; i < n; i++) s = (s + A_ab(i)) | 0;
  return s;
}
function n_host_call(n) {              /* host function in loop */
  var s = 0;
  for (var i = 0; i < n; i++) s = host_add(s, i);
  return s;
}

/* ------------------------------------------------------------------ *
 * Element loads.
 * ------------------------------------------------------------------ */
var F64 = new Float64Array(4096);
for (var _f = 0; _f < 4096; _f++) F64[_f] = _f * 0.25;
function n_elem_f64(n) {
  var s = 0.0;
  for (var i = 0; i < n; i++) s += F64[i & 4095];
  return s;
}
var DENSE = [];
for (var _d = 0; _d < 4096; _d++) DENSE[_d] = _d * 0.5;
function n_elem_dense_hole(n) {         /* hole injected after the array is hot */
  var a = DENSE.slice();
  a[100] = void 0;
  var s = 0.0;
  for (var i = 0; i < n; i++) {
    var v = a[i & 4095];
    s += (v === void 0) ? 0 : v;
  }
  return s;
}

/* ------------------------------------------------------------------ *
 * Generators and async (the resume switch).
 * ------------------------------------------------------------------ */
function* g_loop(n) {
  for (var i = 0; i < n; i++) yield i;
}
function n_gen_loop(n) {
  var it = g_loop(n), s = 0, r;
  while (!(r = it.next()).done) s = (s + r.value) | 0;
  return s;
}
function* g_finally(n) {
  for (var i = 0; i < n; i++) {
    try { yield i; } finally { }
  }
}
function n_gen_finally(n) {
  var it = g_finally(n), s = 0, r;
  while (!(r = it.next()).done) s = (s + r.value) | 0;
  return s;
}
async function n_async_await(n) {
  var s = 0;
  for (var i = 0; i < n; i++) {
    await 0;
    s = (s + i) | 0;
  }
  return s;
}
async function* g_async(n) {            /* for-await over an async generator */
  for (var i = 0; i < n; i++) yield i;
}
async function n_async_forawait(n) {
  var s = 0;
  for await (var v of g_async(n)) s = (s + v) | 0;
  return s;
}

/* ------------------------------------------------------------------ *
 * Exceptions: try/catch in a loop and a throw from a leaf callee.
 * ------------------------------------------------------------------ */
function n_try_catch(n) {
  var s = 0;
  for (var i = 0; i < n; i++) {
    try {
      if ((i & 1) === 0) throw i;
      s = (s + i) | 0;
    } catch (e) {
      s = (s - e) | 0;
    }
  }
  return s;
}
function throw_leaf(x) {                /* throw from a leaf: stack check */
  if ((x & 3) === 0) throw x;
  return (x + 1) | 0;
}
function n_throw_leaf(n) {
  var s = 0;
  for (var i = 0; i < n; i++) {
    try { s = (s + throw_leaf(i)) | 0; }
    catch (e) { s = (s - e) | 0; }
  }
  return s;
}
/* Unwind across several AOT frames: the throw starts two calls deep and the
   catch is in the outermost kernel frame, so the unwinder must walk out of the
   inner frames before it finds the handler. */
function u_throw(x) { if ((x & 7) === 0) throw x; return (x + 1) | 0; }
function u_mid(x) { return (u_throw(x) + 1) | 0; }
function u_outer(x) { return (u_mid(x) + 2) | 0; }
function n_unwind_nested(n) {
  var s = 0;
  for (var i = 0; i < n; i++) {
    try { s = (s + u_outer(i)) | 0; }
    catch (e) { s = (s - e) | 0; }
  }
  return s;
}
/* Handler resume: the finally body runs on the normal path, the throwing path
   and while an exception is in flight, all inside the same AOT frame. */
function n_unwind_finally(n) {
  var s = 0;
  for (var i = 0; i < n; i++) {
    try {
      try {
        if ((i & 1) === 0) throw i;
        s = (s + i) | 0;
      } finally {
        s = (s + 1) | 0;
      }
    } catch (e) {
      s = (s - e) | 0;
    }
  }
  return s;
}
/* `return` inside a finally overrides both the normal and the throwing path;
   the NIP_CATCH/return resume must agree with the interpreter. */
function fr_leaf(x) {
  try {
    if ((x & 7) === 0) throw x;
    return (x + 1) | 0;
  } finally {
    if ((x & 7) === 3) return (x + 3000) | 0;
  }
}
function n_finally_return(n) {
  var s = 0;
  for (var i = 0; i < n; i++) {
    try { s = (s + fr_leaf(i)) | 0; }
    catch (e) { s = (s - e) | 0; }
  }
  return s;
}
/* Unwind across the AOT boundary: a compiled kernel calls an INTERPRETED
   function that throws, and the exception crosses back into the compiled
   frame's handler. The callee is built by `new Function` under the real bench
   driver only, so qjsaot never sees it and it cannot enter the unit. */
var interp_throw = (typeof __aot_bench_driver__ !== "undefined")
  ? new Function("x", "if ((x & 3) === 0) throw new Error('c' + x); return (x + 1) | 0;")
  : void 0;
function n_unwind_cross(n) {
  if (interp_throw === void 0) return 0;
  var s = 0;
  for (var i = 0; i < n; i++) {
    try { s = (s + interp_throw(i)) | 0; }
    catch (e) { s = (s - e.message.length) | 0; }
  }
  return s;
}

/* ------------------------------------------------------------------ *
 * Guards: callee and shape change mid-run.
 * ------------------------------------------------------------------ */
function cc1(x) { return (x + 1) | 0; }
function cc2(x) { return (x + 2) | 0; }
var CALLEES = [cc1, cc2];
function n_callee_change(n) {
  var s = 0;
  for (var i = 0; i < n; i++) s = CALLEES[i & 1](s);
  return s;
}
var SHAPE_A = { x: 1, y: 2 };
var SHAPE_B = { y: 3, x: 4 };
function n_shape_change(n) {
  var s = 0;
  for (var i = 0; i < n; i++) {
    var o = (i & 1) ? SHAPE_B : SHAPE_A;
    s = (s + o.x) | 0;
  }
  return s;
}
/* Shape-lifetime: build a fresh shape each iteration (four key orders) so the
   short-lived shapes are freed and their storage reused, then read a common
   property through one site. A stale shape guard that false-hits a reused
   address reads the wrong slot, so this gates the IC shape ownership policy. */
var REUSE_KEYS = ["a", "b", "c", "d"];
function n_ic_reuse(n) {
  var s = 0;
  for (var i = 0; i < n; i++) {
    var o = {};
    o[REUSE_KEYS[i & 3]] = i;
    o.x = i;
    s = (s + o.x) | 0;
  }
  return s;
}

/* ------------------------------------------------------------------ *
 * Numeric edges: FMA sensitivity, -0, NaN, int32 overflow.
 * ------------------------------------------------------------------ */
function n_fma(n) {
  var s = 0.5;
  for (var i = 0; i < n; i++) s = s * 1.0000001 + 1e-9;
  return s;
}
var NEGZERO = [-0, 0];
function n_negzero(n) {
  var s = 0;
  for (var i = 0; i < n; i++) {
    var z = NEGZERO[i & 1];
    s += (1 / z === -Infinity) ? 1 : 0;
  }
  return s;
}
var NANV = 0 / 0;
function n_nan(n) {
  var s = 0;
  for (var i = 0; i < n; i++) {
    var v = NANV;
    if (v !== v) s++;
  }
  return s;
}
function n_overflow(n) {
  var s = 0;
  for (var i = 0; i < n; i++) s = (s + 0x20000000) | 0;
  return s;
}

/* ------------------------------------------------------------------ *
 * Call-site guards: callee identity, prototype-value folding,
 * recursion, arity padding, and builtin edge values.
 * ------------------------------------------------------------------ */
var _orig_sqrt = Math.sqrt;
function n_math_change(n) {             /* Math.sqrt replaced mid-loop */
  var s = 0.0;
  for (var i = 0; i < n; i++) {
    if ((i & 511) === 1) Math.sqrt = function (x) { return x + 1; };
    else if ((i & 511) === 256) Math.sqrt = _orig_sqrt;
    s += Math.sqrt(i & 255);
  }
  Math.sqrt = _orig_sqrt;
  return s;
}
var _orig_abs = Math.abs;
var _cf = Math.sqrt;
function n_cfunc_change(n) {            /* one call site, two C callees */
  var s = 0;
  for (var i = 0; i < n; i++) {
    _cf = (i & 1) ? _orig_abs : _orig_sqrt;
    s = (s + (_cf(i - 3) | 0)) | 0;
  }
  return s;
}
function P_m(x) { return (x + 1) | 0; }
function ProtoM() {}
ProtoM.prototype.m = P_m;
var proto_obj = new ProtoM();
function n_proto_change(n) {            /* prototype method replaced mid-loop */
  var s = 0;
  var orig = ProtoM.prototype.m;
  for (var i = 0; i < n; i++) {
    if ((i & 511) === 1) ProtoM.prototype.m = function (x) { return (x + 2) | 0; };
    else if ((i & 511) === 256) ProtoM.prototype.m = orig;
    s = (s + proto_obj.m(i & 255)) | 0;
  }
  ProtoM.prototype.m = orig;
  return s;
}
function rec_sum(x) { return x <= 0 ? 0 : (x + rec_sum(x - 1)) | 0; }
function n_recursion(n) {               /* recursion: no infinite inlining */
  var s = 0;
  for (var i = 0; i < n; i++) s = (s + rec_sum(i & 15)) | 0;
  return s;
}
function arity3(a, b, c) {
  return (a + (b === void 0 ? 0 : b) + (c === void 0 ? 0 : c)) | 0;
}
function n_arity(n) {                   /* fewer args than the callee length */
  var s = 0;
  for (var i = 0; i < n; i++) s = (s + arity3(i)) | 0;
  return s;
}
function st_leaf() { return new Error().stack.length; }  /* stack through direct calls */
function st_mid() { return st_leaf() + 1; }
function n_stack(n) {
  var s = 0;
  for (var i = 0; i < n; i++) s = (s + st_mid()) | 0;
  return s;
}
function n_math_edge(n) {               /* Math.sqrt/min edge values */
  var s = 0;
  for (var i = 0; i < n; i++) {
    var x = (i & 3) === 0 ? -1 : (i & 3) === 1 ? -0 : (i & 3) === 2 ? 0 : 4;
    var r = Math.sqrt(x);
    if (r !== r) s = (s + 1) | 0;
    else if (r === 0 && 1 / r === -Infinity) s = (s + 100) | 0;
    else s = (s + (r | 0)) | 0;
    if (Math.min(NaN, 1) !== Math.min(NaN, 1)) s = (s + 1000) | 0;
  }
  return s;
}
function n_host_arity(n) {              /* host C fn called with an unsupplied formal */
  var s = 0;
  for (var i = 0; i < n; i++) {
    s = (s + host_opt(i, 1000)) | 0;    /* 2-arg call seeds the slot after argv */
    s = (s + host_opt(i)) | 0;          /* 1-arg: padded to undefined -> 0 */
  }
  return s;
}

/* ------------------------------------------------------------------ *
 * Registration + report.  Driven by an async main so the async kernel's
 * continuation (and therefore the report) runs after the job queue drains.
 * ------------------------------------------------------------------ */
(async function main() {
  bench("call.native",      n_sqrt_floor_min,  300000, linear_f);
  bench("call.js_leaf",     n_js_leaf,         2000000, exact8);
  bench("call.ab",          n_call_ab,          500000, linear);
  bench("call.host",        n_host_call,       2000000, wrapsum);
  bench("elem.f64",         n_elem_f64,        3000000, linear_f);
  bench("elem.dense_hole",  n_elem_dense_hole, 1000000, linear_f);
  bench("gen.loop",         n_gen_loop,        1000000, wrapsum);
  bench("gen.finally",      n_gen_finally,     1000000, wrapsum);
  bench("try.catch",        n_try_catch,       2000000, linear);
  bench("throw.leaf",       n_throw_leaf,      1000000, linear);
  bench("unwind.nested",    n_unwind_nested,   1000000, linear);
  bench("unwind.finally",   n_unwind_finally,  1000000, linear);
  bench("finally.return",   n_finally_return,  1000000, linear);
  bench("unwind.cross",     n_unwind_cross,    1000000, linear);
  bench("callee.change",    n_callee_change,   1000000, linear);
  bench("shape.change",     n_shape_change,    1000000, linear);
  bench("ic.shape_reuse",   n_ic_reuse,        1000000, linear);
  bench("call.math_change", n_math_change,      500000, linear);
  bench("proto.method_change", n_proto_change, 1000000, linear);
  bench("call.cfunc_change", n_cfunc_change,   1000000, linear);
  bench("call.recursion",   n_recursion,       1000000, linear);
  bench("call.arity",       n_arity,           2000000, linear);
  bench("call.stack",       n_stack,            200000, linear);
  bench("math.edge",        n_math_edge,       1000000, linear);
  bench("call.host_arity",  n_host_arity,      1000000, linear);
  bench("num.fma",          n_fma,             2000000, anyv);
  bench("num.negzero",      n_negzero,         1000000, linear);
  bench("num.nan",          n_nan,             1000000, exact8);
  bench("num.overflow",     n_overflow,        1000000, anyv);
  await benchAsync("async.await", n_async_await, 20000, wrapsum);
  await benchAsync("async.forawait", n_async_forawait, 20000, wrapsum);

  var i, r;
  for (i = 0; i < results.length; i++) {
    r = results[i];
    var sc = r.ms > 0 ? (r.n / r.ms) : 0;
    print("##CB2## " + r.name + " " + (r.ms > 0 ? r.ms.toFixed(5) : r.ms) +
          " " + r.n + " " + r.sum + " " + sc.toFixed(1) + " " + (r.inner | 0) +
          " " + (r.span || 0) + (r.bad ? " WRONG" : ""));
  }
  var logsum = 0, cnt = 0;
  for (i = 0; i < results.length; i++) {
    r = results[i];
    if (r.bad || !(r.ms > 0)) continue;
    logsum += Math.log(r.ms);
    cnt++;
  }
  var geo = cnt ? Math.exp(logsum / cnt) : 0;
  print("##CBSUM## kernels " + results.length + " scored " + cnt +
        " failed " + failures.length + " warns " + warns.length);
  for (i = 0; i < failures.length; i++) print("##CBFAIL## " + failures[i]);
  for (i = 0; i < warns.length; i++) print("##CBWARN## " + warns[i]);
  if (failures.length) print("Alert: " + failures.length + " kernel(s) computed the wrong answer");
  print("Geomean-ms: " + geo.toFixed(4));
  print("Score: " + (cnt ? (10000 / geo).toFixed(2) : 0));
})();
