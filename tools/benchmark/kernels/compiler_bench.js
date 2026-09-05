/*
 * compiler_bench.js -- a benchmark whose UNIT IS A COMPILER DECISION.
 *
 * WHY THIS EXISTS.  Octane grades the engine.  It cannot tell you WHICH RUNG
 * of the AOT compiler paid, because every row mixes twenty shapes and the
 * winner is whichever one happens to dominate that row.  MEASURED on the
 * 13-row table: coverage and score are almost uncorrelated (mandreel 91.6%
 * coverage -> 1.026x; raytrace 75.8% -> 0.889x), so "coverage went up" is not
 * evidence and "the row got faster" does not say what got faster.
 *
 * Each kernel here is ONE SHAPE, sized so that the shape is ~all of its own
 * runtime.  A kernel's ratio between two binaries is therefore attributable:
 * it names the rung.
 *
 * FOUR RULES, each because of a defect this project has already paid for:
 *
 *  1. EVERY KERNEL RETURNS A CHECKSUM AND THE CHECKSUM IS ASSERTED.
 *     A region that computes the wrong thing runs to completion and reports a
 *     good time.  box2d does not verify its output and that is exactly how
 *     two silent wrong answers survived every conformance run that reached
 *     them.  A kernel with no checksum is a kernel that cannot fail.
 *
 *  2. THE EXPECTED CHECKSUM IS COMPUTED BY THE KERNEL ITSELF ON A SHORT RUN,
 *     never hard-coded.  A hard-coded constant is a second implementation
 *     that drifts; and it would have to be regenerated on the very binary
 *     under test, which is the trap it is meant to catch.  Instead every
 *     kernel is run at TWO sizes and the relation between them is asserted,
 *     which no wrong-answer arm satisfies by accident.
 *
 *  3. FIXED ITERATION COUNTS, NOT A TIME BOX.  A time box makes the sample
 *     size a function of the thing being measured, so a faster arm executes
 *     MORE work and the comparison silently changes population.  These
 *     kernels report milliseconds for a FIXED amount of work; lower is
 *     better and the ratio is the answer.
 *
 *  4. NOTHING IS PRINTED FROM INSIDE A TIMED REGION, and no kernel allocates
 *     in a way that makes the collector the subject unless that IS the
 *     subject (see the alloc-* kernels, which are labelled).
 *
 * OUTPUT: one `##CB2## <name> <ms> <iters> <checksum> <score> <inner> <span>`
 * line per kernel on stdout, where ms is a FLOAT -- the timed span divided by
 * the inner repeat count -- and score = iters/ms (higher is better, and
 * comparable across BINARIES for one kernel, never across kernels -- see the
 * report block); then `##CBSUM##`, and a `Score:` line so the project's
 * existing row classifier (`grep -cE '^Score'`) accepts the file as a
 * completed row.
 *
 * ⛔ THE MARKER IS ##CB2##, NOT ##CB##, AND THE SUMMARY IS ##CBSUM##.  Both
 * changed in the 09-03 instrument fix: ms became a float (old parsers matched
 * it with `(-?\d+)` and would silently drop every row), and the old summary
 * line `##CB## kernels 93 scored 93` parsed as a 94th kernel named `kernels`
 * -- it was inside the 09-02 scoreboard's geomean population.
 *
 * THE MEASURED NULL FLOOR OF THIS FILE (same binary, same token, both sides,
 * ABBA-interleaved, minimum of 3 reps, one Mac host):
 *
 *     GEOMEAN over 60 kernels     1.0010x
 *     per-kernel scatter          19/60 move >2%, 10/60 >3%, 1/60 >5%,
 *                                 worst 7.1% (caps.locals16)
 *
 * ⛔ READ BOTH NUMBERS.  The GEOMEAN is trustworthy to a few tenths of a
 *   percent, so a geomean difference of even 2% is a result.  A SINGLE
 *   KERNEL moving 3% is NOT: one kernel in six crosses that on a null.
 *   Four "regressions" of 3-9% were once attributed to a rung and every one
 *   of them was inside this floor.
 *
 * ⛔ AND THE FLOOR ABOVE IS FOR AN ARM-vs-ARM COMPARISON (both engines doing
 *   the same amount of work).  AN AOT-ON-vs-OFF RATIO IS LOAD-SENSITIVE in a
 *   way that one is not: the two arms have very different runtimes, so a
 *   loaded machine slows the slower arm proportionally more and COMPRESSES
 *   the ratio toward 1.  MEASURED: two ON/OFF runs of near-identical binaries
 *   read 0.6213x and 0.6962x, while the ON/ON null between them read 1.0010x.
 *   ⇒ Compare RUNGS binary-against-binary, both with AOT on and interleaved.
 *     Use ON/OFF only within a single session, and never across sessions.
 *
 * USAGE:  qjs-bench compiler_bench.js
 *         CB_REPS=5 CB_FILTER=int qjs-bench compiler_bench.js   (via globals,
 *         see the CB_* block below -- there is no getenv in this driver)
 */

/* ------------------------------------------------------------------ *
 * Harness
 * ------------------------------------------------------------------ */

var CB_REPS = 3;          /* repetitions of the timed region; MINIMUM reported */
var CB_SCALE = 3;         /* multiply every kernel's iteration count       */
var CB_TARGET_MS = 100;   /* every TIMED REGION must span at least this    */
var CB_CALIBRATE = 0;     /* 1 = print a CB_INNER table and exit           */
var CB_FILTER = "";       /* substring; "" runs everything                 */

/* ⛔ WHY AN INNER REPEAT COUNT AND NOT A BIGGER n.
 *
 * Date.now() has 1 ms resolution.  At CB_SCALE=3 twelve kernels still finish
 * in 5-18 ms on the fast arm, so 5-20 % of their reading is quantisation and
 * a real 5 % effect on them is BELOW THE INSTRUMENT.  (MEASURED 2026-09-02:
 * flow.do_while 5 ms, slow.incloc_double 7 ms, flow.throw_catch 8 ms.)
 *
 * The fix is NOT to raise n.  n is an input to the SHAPE -- a loop over 10^7
 * elements has a different cache footprint from one over 10^5, and n also
 * feeds the checksum relation asserted at n/8 vs n.  Raising it changes what
 * is being measured.  Instead the SAME fn(n) is called `inner` times inside
 * one timed region and the time is divided by `inner`: the shape is
 * untouched, only the clock becomes adequate.
 *
 * ⛔ AND `inner` IS A BAKED PER-KERNEL CONSTANT, NOT CALIBRATED PER RUN.
 * If each arm calibrated its own, the FAST arm would get the LARGER inner,
 * run more back-to-back iterations, and enjoy hotter caches and ICs than the
 * slow arm -- a systematic bias in favour of whichever arm is already
 * winning.  Both arms must use the same value.  cb2score.py ASSERTS that
 * they did; a mismatch voids the cell rather than being averaged in.
 *
 * Regenerate with CB_CALIBRATE=1 on the FAST arm after adding kernels.  A
 * kernel missing from the table auto-calibrates and prints ##CBWARN##. */
var CB_INNER = {"int.add_imm8":3,"int.bin_imm8":5,"int.bin_imm16":4,"int.bin_imm32":5,"int.sar_imm8":5,"int.loc_mul":5,"int.frame_bin":4,"int.frame_bin_arg":4,"int.bitops":5,"int.incdec_loc":5,"int.postinc":5,"int.unary":4,"flow.cmp_branch":7,"flow.nested":5,"flow.while_break":5,"fp.arith":4,"fp.div_mod":2,"fp.mixed":5,"prop.mono":16,"prop.mono_nocall":16,"prop.store":10,"prop.poly2":3,"prop.mega":3,"prop.proto":3,"prop.chain":5,"elem.load":2,"elem.store":3,"elem.length":4,"elem.sum_len":3,"elem.literal":8,"elem.literal_loop":3,"elem.typedarray":3,"call.call0":4,"call.call1":4,"call.call2":5,"call.call3":5,"call.method":3,"call.ctor":3,"call.loop_arith":4,"call.recursion":3,"var.global_read":9,"var.global_write":3,"var.closure_cell":4,"tag.predicates":3,"tag.instanceof":7,"refuse.try_catch":7,"refuse.str_concat":3,"refuse.str_charcode":3,"refuse.forin":3,"alloc.object":4,"alloc.array":3,"caps.locals16":4,"caps.locals48":5,"caps.straightline":8,"hot.tiny":4,"hot.cold_many":3,"slow.incloc_double":19,"slow.incloc_object":4,"slow.incloc_throw":15,"slow.addloc_object":4,"slow.neg":10,"slow.bit2":5,"slow.shr":9,"slow.loadel":7,"slow.addloc_str":7,"int.shr_unsigned":7,"obj.literal_small":4,"obj.literal_wide":3,"obj.transition":4,"obj.delete_in":3,"obj.accessor":4,"flow.switch_dense":8,"flow.switch_sparse":8,"flow.do_while":25,"flow.label_cont":13,"flow.for_of":3,"flow.ternary":12,"flow.throw_catch":15,"bi.math_float":2,"bi.math_minmax":3,"bi.string_methods":3,"bi.string_build":3,"bi.array_push":2,"bi.array_indexof":3,"bi.json":2,"bi.float64array":3,"bi.array_holes":4,"call.polymorphic":5,"call.method_poly":4,"call.closure_alloc":3,"call.deep_recurse":3,"mix.numeric":3,"mix.objects":3,"mix.calls":3};        /* name -> inner repeat count (baked, see above)  */

var results = [];
var failures = [];
var warns = [];

function now() { return Date.now(); }

/* Run one kernel.  `n` is the kernel's iteration count; `check` receives the
 * kernel's return value at size n and at size n/8 and must return true. */
function bench(name, fn, n, check) {
  if (CB_FILTER && name.indexOf(CB_FILTER) < 0) return;
  n = Math.max(1, (n * CB_SCALE) | 0);

  /* CORRECTNESS FIRST, and on TWO sizes -- see rule 2.  Done before any
     timing so a broken arm reports the defect rather than a fast number. */
  var small = fn((n / 8) | 0 || 1);
  var full = fn(n);
  if (!check(full, small)) {
    failures.push(name + ": checksum full=" + full + " small=" + small);
    results.push({ name: name, ms: -1, n: n, sum: full, inner: 0, bad: true });
    return;
  }

  /* WARMUP.  The AOT regions disarm after 8 refusals and arm on entry, and
     the ICs need to be filled; a first-iteration measurement grades the
     warmup, not the steady state.  MEASURED on this project: every
     always-refused AOT region had been entered EXACTLY 8 times. */
  fn(n);
  fn(n);

  var inner = CB_INNER[name] | 0;
  if (inner < 1) {
    /* Not in the table: calibrate here, and SAY SO.  The pilot doubles until
       the region clears a coarse floor, so a kernel 1000x faster than another
       costs the same pilot time. */
    var pr = 1, pd = 0;
    for (;;) {
      var pt = now();
      for (var pq = 0; pq < pr; pq++) fn(n);
      pd = now() - pt;
      if (pd >= 25 || pr >= 8192) break;
      pr = pr * 2;
    }
    var per = pd / pr;
    inner = per > 0 ? Math.ceil(CB_TARGET_MS / per) : 1;
    if (inner < 1) inner = 1;
    if (inner > 8192) inner = 8192;
    if (!CB_CALIBRATE) warns.push(name + " not in CB_INNER, auto inner=" + inner);
  }

  var best = Infinity, sum = full, q, r, t0, dt;
  for (r = 0; r < CB_REPS; r++) {
    t0 = now();
    for (q = 0; q < inner; q++) sum = fn(n);
    dt = now() - t0;
    if (dt < best) best = dt;
  }

  /* ⛔ STATE-ACCUMULATION GUARD.  Repeating fn(n) is only sound if the kernel
     is a pure function of n.  A kernel that grows an array across calls, or
     memoises, would report a per-call cost that is not the shape's cost.
     The checksum from inside the timed region must equal the one the
     correctness pass computed. */
  if (sum !== full) {
    failures.push(name + ": NOT REPEATABLE -- checksum drifted across the " +
                  "timed region (" + full + " -> " + sum + ")");
    results.push({ name: name, ms: -1, n: n, sum: sum, inner: inner, bad: true });
    return;
  }

  results.push({ name: name, ms: best / inner, n: n, sum: sum,
                 inner: inner, span: best, bad: false });
}

/* The two relations used by almost every kernel. */
function linear(full, small) {   /* an accumulating sum: monotone, both finite */
  return isFinite(full) && isFinite(small) && full !== small;
}
function exact(v) { return function (full, small) { return full === v; }; }

/* ------------------------------------------------------------------ *
 * 1. INTEGER ARITHMETIC -- the fused-superinstruction shapes (1118 / F18)
 *    Each of these becomes ONE opcode on the shipping engine:
 *      x + <i8>        -> i8_add        x >> <i8>   -> i8_sar
 *      x <op> <i8>     -> imm8_bin      x & <i32>   -> imm32_bin
 *      x <op> <i16>    -> imm16_bin     x * loc     -> loc_mul
 *      x <op> loc/arg  -> frame_bin
 *    Before F18 the AOT translator had no case for any of them and refused
 *    the whole body: on crypto that was 94.09% of the executed refused work.
 * ------------------------------------------------------------------ */

function k_int_add_imm8(n) {          /* i8_add */
  var x = 0;
  for (var i = 0; i < n; i++) { x = x + 3; x = x + 7; x = x + 11; x = x + 1; }
  return x;
}
function k_int_bin_imm8(n) {          /* imm8_bin: sub / mul / div / and / or */
  var x = 1;
  for (var i = 0; i < n; i++) {
    x = x * 3; x = x - 2; x = x & 127; x = x | 1; x = x - 1;
  }
  return x + n;
}
function k_int_bin_imm16(n) {         /* imm16_bin: add / mod / and */
  var x = 0;
  for (var i = 0; i < n; i++) { x = x + 1234; x = x % 30011; x = x & 32767; }
  return x + n;
}
function k_int_bin_imm32(n) {         /* imm32_bin: and with a wide mask */
  var x = 0;
  for (var i = 0; i < n; i++) { x = (x + i) & 0x3fffffff; x = x & 0x0fffff0f; }
  return x + n;
}
function k_int_sar_imm8(n) {          /* i8_sar */
  var x = 0;
  for (var i = 0; i < n; i++) { x = (x + i) >> 1; x = (x + 1024) >> 3; }
  return x + n;
}
function k_int_loc_mul(n) {           /* loc_mul: x * <local> */
  var x = 1, m = 3;
  for (var i = 0; i < n; i++) { x = (x * m) & 65535; x = (x * m) & 65535; }
  return x + n;
}
function k_int_frame_bin(n) {         /* frame_bin over LOCALS: add/sub/or/div */
  var x = 0, a = 7, b = 3;
  for (var i = 0; i < n; i++) { x = x + a; x = x - b; x = x | b; }
  return x + n;
}
function k_int_frame_bin_arg(n, a, b) {  /* frame_bin over ARGUMENTS: add/mul */
  a = a | 0 || 5; b = b | 0 || 3;
  var x = 0;
  for (var i = 0; i < n; i++) { x = x + a; x = (x * b) & 1048575; }
  return x + n;
}

/* ------------------------------------------------------------------ *
 * 2. THE R1int RUNG -- the bit operators in their UNFUSED form, plus the
 *    unary and increment families.  These are the ops behind qjs_aot_intops.
 * ------------------------------------------------------------------ */

function k_bitops(n) {
  var x = 12345, y = 6789;
  for (var i = 0; i < n; i++) {
    x = (x ^ y) & 0xffff; y = (y << 1) | 1; y = y & 0xffff;
    x = (x >>> 1) + (y >> 2); x = x & 0xffff;
  }
  return x + y + n;
}
function k_incdec_loc(n) {            /* inc_loc / dec_loc / add_loc */
  var a = 0, b = 0;
  for (var i = 0; i < n; i++) { a++; a++; b--; a += 3; b += 1; }
  return a + b + n;
}
function k_postinc(n) {
  var a = 0, s = 0;
  for (var i = 0; i < n; i++) { s += a++; s += a++; s = s & 0xffffff; }
  return s + n;
}
function k_unary(n) {
  var x = 0, f = false;
  for (var i = 0; i < n; i++) { f = !f; x = x + (f ? 1 : 0); x = ~x; x = ~x; }
  return x + n;
}

/* ------------------------------------------------------------------ *
 * 3. COMPARISON AND CONTROL FLOW -- RIR_CMP / RIR_CMPBR / RIR_BRF.
 *    The `cmp_br` fusion and the flow rung (R4).
 * ------------------------------------------------------------------ */

function k_cmp_branch(n) {
  var s = 0;
  for (var i = 0; i < n; i++) {
    if (i < 100) s += 1;
    else if (i > 1000) s += 2;
    else if (i === 500) s += 3;
    else s += 4;
  }
  return s + n;
}
function k_loop_nested(n) {
  var s = 0, m = 8;
  for (var i = 0; i < n; i++)
    for (var j = 0; j < m; j++) { s += j; if (s > 1000000) s -= 1000000; }
  return s + n;
}
function k_loop_while_break(n) {
  var s = 0;
  for (var i = 0; i < n; i++) {
    var j = 0;
    while (true) { j++; s += 1; if (j >= 6) break; }
  }
  return s + n;
}

/* ------------------------------------------------------------------ *
 * 4. FLOATING POINT -- the arm where the region's "both operands are
 *    numbers" guard holds but the INT fast path does not.  Separated from
 *    the int kernels on purpose: the tag parity of the result differs and
 *    the totalized post-call arms are keyed on it.
 * ------------------------------------------------------------------ */

function k_float_arith(n) {
  var x = 1.5, y = 2.25;
  for (var i = 0; i < n; i++) {
    x = x * 1.0000001 + 0.5; y = y + x * 0.25; x = x - 0.125;
    if (x > 1e12) { x = 1.5; y = 2.25; }
  }
  return (x + y) | 0;
}
function k_float_div_mod(n) {
  var x = 1.0, s = 0;
  for (var i = 0; i < n; i++) { x = (i + 1) / 3.0; s += x % 7.0; }
  return s | 0;
}
function k_mixed_int_float(n) {       /* tag transitions every iteration */
  var s = 0;
  for (var i = 0; i < n; i++) { s += (i & 1) ? 1 : 0.5; }
  return s | 0;
}

/* ------------------------------------------------------------------ *
 * 5. PROPERTY ACCESS -- the IC rungs.  ic_own / ic_proto2 / ic_mega2 are the
 *    three arms the emitted regions carry; these kernels hit them one at a
 *    time.  MEASURED on the 13 rows: 8327 ic_own, 5531 ic_proto2, 6540
 *    ic_mega2 sites, so all three are load-bearing and all three are here.
 * ------------------------------------------------------------------ */

function Pt(x, y) { this.x = x; this.y = y; }
/* ⭐ L2 PROBE, added 2026-09-03.  IDENTICAL LOOP to k_field_mono, but the
   receiver arrives as an ARGUMENT instead of being constructed in the
   preamble -- so the region contains NO CALL.  k_field_mono is refused the
   numeric-locals rung by C3 (`rg->has_call`, emit.h:1560), which is
   REGION-WIDE, even though its loop is call-free; this kernel isolates that
   one difference and nothing else.  Their ratio prices C3. */
function k_field_mono_nocall(n, p) {
  var s = 0;
  for (var i = 0; i < n; i++) { s += p.x; s += p.y; s += p.x; s += p.y; }
  return s + n;
}

function k_field_mono(n) {            /* ic_own: one shape, own property */
  var p = new Pt(1, 2), s = 0;
  for (var i = 0; i < n; i++) { s += p.x; s += p.y; s += p.x; s += p.y; }
  return s + n;
}
function k_field_store(n) {
  var p = new Pt(0, 0);
  for (var i = 0; i < n; i++) { p.x = i; p.y = p.x + 1; }
  return p.x + p.y + n;
}
function A2() { this.a = 1; this.b = 2; }
function B2() { this.b = 3; this.a = 4; }   /* DIFFERENT shape, same names */
function k_field_poly2(n) {
  var o = [new A2(), new B2()], s = 0;
  for (var i = 0; i < n; i++) { var q = o[i & 1]; s += q.a + q.b; }
  return s + n;
}
function k_field_mega(n) {            /* ic_mega2: many shapes at one site */
  var arr = [];
  for (var k = 0; k < 8; k++) {
    var o = {};
    for (var j = 0; j <= k; j++) o["f" + j] = j;
    o.v = k;
    arr.push(o);
  }
  var s = 0;
  for (var i = 0; i < n; i++) s += arr[i & 7].v;
  return s + n;
}
function Base() { this.own = 1; }
Base.prototype.pv = 7;
function Mid() { Base.call(this); }
Mid.prototype = new Base();
function Leaf() { Mid.call(this); }
Leaf.prototype = new Mid();
function k_field_proto(n) {           /* ic_proto2: a 3-deep prototype chain */
  var o = new Leaf(), s = 0;
  for (var i = 0; i < n; i++) { s += o.pv; s += o.own; }
  return s + n;
}
function k_field_chain(n) {           /* a.b.c.d -- the field_chain fusion */
  var o = { b: { c: { d: 5 } } }, s = 0;
  for (var i = 0; i < n; i++) s += o.b.c.d;
  return s + n;
}

/* ------------------------------------------------------------------ *
 * 6. ELEMENTS -- the R3elem / R3len rung.  Both the plain form and the two
 *    fused forms (loc8_array_el, arg8_loc8_array_el) the emitter produces.
 * ------------------------------------------------------------------ */

function k_array_load(n) {
  var a = [], s = 0;
  for (var k = 0; k < 64; k++) a[k] = k;
  for (var i = 0; i < n; i++) { var j = i & 63; s += a[j]; s += a[j ^ 1]; }
  return s + n;
}
function k_array_store(n) {
  var a = [];
  for (var k = 0; k < 64; k++) a[k] = 0;
  for (var i = 0; i < n; i++) { var j = i & 63; a[j] = j; a[j ^ 1] = j + 1; }
  return a[0] + a[63] + n;
}
function k_array_len(n) {
  var a = [];
  for (var k = 0; k < 32; k++) a[k] = k;
  var s = 0;
  for (var i = 0; i < n; i++) s += a.length;
  return s + n;
}
function k_array_sum_len(n) {         /* the canonical `for (i<a.length)` */
  var a = [];
  for (var k = 0; k < 32; k++) a[k] = k;
  var s = 0;
  for (var i = 0; i < n; i++)
    for (var j = 0; j < a.length; j++) s += a[j];
  return s + n;
}
/* ⛔⛔ THE TWO ARRAY IDIOMS ARE DIFFERENT COMPILER PROBLEMS AND MUST BOTH BE
   HERE.  Every array kernel above builds its array by APPENDING
   (`var a = []; for (k) a[k] = k`), and an append writes p->u.array.count --
   rule R1 -- so the region's element-store guard refuses it.  MEASURED: after
   the array-literal rung (F21) admitted those five bodies, every one of them
   reported `enter=5 done=0 why=elemst.not_in_range_fast_array` and the rung
   read as a clean null at 1.0093x.  It was not the literal that blocked them.
   ⇒ This kernel is the OTHER idiom -- a literal built in one op and then only
   READ -- so the rung has a population that can actually express its value,
   and the append wall is named rather than hidden inside a null. */
function k_array_literal(n) {
  var a = [3, 1, 4, 1, 5, 9, 2, 6];
  var s = 0;
  for (var i = 0; i < n; i++) { s += a[i & 7]; s += a[(i + 3) & 7]; }
  return s + n;
}
function k_array_literal_sum(n) {
  var s = 0;
  for (var i = 0; i < n; i++) {
    var a = [i & 7, 2, 3];        /* a literal INSIDE the loop */
    s = (s + a[0] + a[2]) & 0xffffff;
  }
  return s + n;
}

function k_typedarray(n) {            /* the TA rung */
  var a = new Int32Array(64), s = 0;
  for (var k = 0; k < 64; k++) a[k] = k;
  for (var i = 0; i < n; i++) { var j = i & 63; a[j] = j + 1; s += a[j]; }
  return s + n;
}

/* ------------------------------------------------------------------ *
 * 7. CALLS -- the CALLK / CALLF rungs, and the boundary that gates every
 *    lift: `!rg->has_call`.  MEASURED on this project: every AOT lift is
 *    gated on the region being call-free and the weight is in call-bearing
 *    loops.  These kernels are the population that gate excludes.
 * ------------------------------------------------------------------ */

function leaf0() { return 1; }
function leaf1(a) { return a + 1; }
function leaf2(a, b) { return a + b; }
function leaf3(a, b, c) { return a + b + c; }
function k_call0(n) { var s = 0; for (var i = 0; i < n; i++) s += leaf0(); return s + n; }
function k_call1(n) { var s = 0; for (var i = 0; i < n; i++) s += leaf1(i); return s + n; }
function k_call2(n) { var s = 0; for (var i = 0; i < n; i++) s += leaf2(i, 1); return s + n; }
function k_call3(n) { var s = 0; for (var i = 0; i < n; i++) s += leaf3(i, 1, 2); return s + n; }

function Obj7() { this.v = 1; }
Obj7.prototype.m = function (a) { return this.v + a; };
function k_method_call(n) {
  var o = new Obj7(), s = 0;
  for (var i = 0; i < n; i++) s += o.m(1);
  return s + n;
}
function k_ctor_call(n) {
  var s = 0;
  for (var i = 0; i < n; i++) { var p = new Pt(i, 1); s += p.x + p.y; }
  return s + n;
}
function k_call_in_loop_arith(n) {    /* THE call-bearing arithmetic loop */
  var s = 0;
  for (var i = 0; i < n; i++) { s = s + leaf1(i); s = s * 1 + 3; s = s & 0xffffff; }
  return s + n;
}
function k_recursion(n) {
  function fib(k) { return k < 2 ? k : fib(k - 1) + fib(k - 2); }
  var s = 0;
  for (var i = 0; i < n; i++) s += fib(12);
  return s + n;
}

/* ------------------------------------------------------------------ *
 * 8. CLOSURES AND GLOBALS -- F10 (var_ref reads) and the GVAR rung.
 *    A body with closure cells was refused OUTRIGHT before the var-ref
 *    relaxation; these say what that rung is worth.
 * ------------------------------------------------------------------ */

var g_counter = 0;
function k_global_read(n) {
  var s = 0;
  for (var i = 0; i < n; i++) { s += g_counter; s += g_counter; }
  return s + n;
}
function k_global_write(n) {
  for (var i = 0; i < n; i++) { g_counter = i; g_counter = g_counter + 1; }
  return g_counter + n;
}
function makeClosure() {
  var cell = 0;
  return function (k) { cell = cell + k; return cell; };
}
function k_closure_cell(n) {
  var f = makeClosure(), s = 0;
  for (var i = 0; i < n; i++) s = f(1);
  return s + n;
}

/* ------------------------------------------------------------------ *
 * 9. TAG PREDICATES AND TYPE TESTS -- F05 / F06.
 * ------------------------------------------------------------------ */

function k_tag_predicates(n) {
  var vals = [null, undefined, 0, "", {}, 1.5];
  var s = 0;
  for (var i = 0; i < n; i++) {
    var v = vals[i % 6];
    if (v === null) s += 1;
    if (v === undefined) s += 2;
    if (v == null) s += 4;
    if (typeof v === "undefined") s += 8;
    if (typeof v === "function") s += 16;
  }
  return s + n;
}
function k_instanceof(n) {
  var p = new Pt(1, 2), s = 0;
  for (var i = 0; i < n; i++) { if (p instanceof Pt) s += 1; }
  return s + n;
}

/* ------------------------------------------------------------------ *
 * 10. SHAPES THE COMPILER SHOULD REFUSE.  These are here so a change that
 *     makes them FASTER is visible -- and, more importantly, so a change
 *     that makes them SLOWER (the region entered, guarded, and bailed) is
 *     visible too.  A compiler that only reports its wins is not an
 *     instrument.
 * ------------------------------------------------------------------ */

function k_try_catch(n) {             /* exception handler: refused rung */
  var s = 0;
  for (var i = 0; i < n; i++) {
    try { s += 1; if (i === -1) throw new Error("x"); } catch (e) { s += 2; }
  }
  return s + n;
}
function k_string_concat(n) {         /* strings: no numeric region applies */
  var s = "";
  for (var i = 0; i < n; i++) { s = "ab"; s = s + "cd"; s = s + i; }
  return s.length + n;
}
function k_string_charcode(n) {
  var str = "the quick brown fox jumps over the lazy dog";
  var s = 0;
  for (var i = 0; i < n; i++) s += str.charCodeAt(i % 43);
  return s + n;
}
function k_forin(n) {                 /* for-in: the shape-pin path */
  var o = { a: 1, b: 2, c: 3, d: 4 }, s = 0;
  for (var i = 0; i < n; i++) for (var k in o) s += o[k];
  return s + n;
}
function k_alloc_object(n) {          /* THE COLLECTOR IS THE SUBJECT HERE */
  var s = 0;
  for (var i = 0; i < n; i++) { var o = { a: i, b: i + 1 }; s += o.a + o.b; }
  return s + n;
}
function k_alloc_array(n) {           /* ditto -- labelled, not incidental */
  var s = 0;
  for (var i = 0; i < n; i++) { var a = [i, i + 1, i + 2]; s += a[0] + a[2]; }
  return s + n;
}

/* ------------------------------------------------------------------ *
 * 11. THE COMPILER'S OWN CAPS.  QJS_AOT_LOC_MAX is 64, QJS_AOT_ST_MAX is 48,
 *     and the body pre-checks refuse anything over them.  These kernels sit
 *     just INSIDE and just OUTSIDE each cap, so a change to a cap has a
 *     visible price and a visible payer.  ⛔ A cap must be laddered, never
 *     derived -- these are the rungs of that ladder.
 * ------------------------------------------------------------------ */

function k_locals_16(n) {
  var a0=0,a1=1,a2=2,a3=3,a4=4,a5=5,a6=6,a7=7;
  var a8=8,a9=9,b0=0,b1=1,b2=2,b3=3,b4=4,b5=5;
  var s = 0;
  for (var i = 0; i < n; i++) {
    a0+=a1; a2+=a3; a4+=a5; a6+=a7; a8+=a9; b0+=b1; b2+=b3; b4+=b5;
    s = (a0+a2+a4+a6+a8+b0+b2+b4) & 0xffffff;
  }
  return s + n;
}
function k_locals_48(n) {
  var v00=0,v01=1,v02=2,v03=3,v04=4,v05=5,v06=6,v07=7,v08=8,v09=9;
  var v10=0,v11=1,v12=2,v13=3,v14=4,v15=5,v16=6,v17=7,v18=8,v19=9;
  var v20=0,v21=1,v22=2,v23=3,v24=4,v25=5,v26=6,v27=7,v28=8,v29=9;
  var v30=0,v31=1,v32=2,v33=3,v34=4,v35=5,v36=6,v37=7,v38=8,v39=9;
  var v40=0,v41=1,v42=2,v43=3,v44=4,v45=5,v46=6,v47=7;
  var s = 0;
  for (var i = 0; i < n; i++) {
    v00+=v01; v02+=v03; v04+=v05; v06+=v07; v08+=v09;
    v10+=v11; v12+=v13; v14+=v15; v16+=v17; v18+=v19;
    v20+=v21; v22+=v23; v24+=v25; v26+=v27; v28+=v29;
    v30+=v31; v32+=v33; v34+=v35; v36+=v37; v38+=v39;
    v40+=v41; v42+=v43; v44+=v45; v46+=v47;
    s = (v00+v10+v20+v30+v40) & 0xffffff;
  }
  return s + n;
}
function k_long_straightline(n) {     /* a long CALL-FREE body: the best case */
  var s = 0;
  for (var i = 0; i < n; i++) {
    var a = i + 1, b = a * 3, c = b - 2, d = c & 255, e = d | 1;
    var f = e + a, g = f * 2, h = g - b, j = h & 511, k = j | 3;
    var l = k + c, m = l * 5, o = m - d, p = o & 1023, q = p | 7;
    s = (s + q + k + e) & 0xffffff;
  }
  return s + n;
}

/* ------------------------------------------------------------------ *
 * 12. THE HOT/COLD SPLIT.  This is the population question behind
 *     "compile what actually runs hot": a body executed millions of times
 *     and a body executed once are indistinguishable to a translator that
 *     decides on SHAPE alone.  Both are here, adjacent, so the cost of
 *     compiling the cold one is measurable rather than argued.
 * ------------------------------------------------------------------ */

function hot_tiny(a, b) { return (a * 3 + b) & 0xffff; }
function k_hot_tiny(n) {
  var s = 0;
  for (var i = 0; i < n; i++) s = (s + hot_tiny(i, 1)) & 0xffffff;
  return s + n;
}
/* Twenty distinct COLD bodies, each entered once per call of the kernel.
   They are shaped exactly like the hot one, so a shape-only admission policy
   compiles all twenty and a hotness-aware one compiles none. */
function cold00(a){return (a*3+1)&0xffff;} function cold01(a){return (a*3+2)&0xffff;}
function cold02(a){return (a*3+3)&0xffff;} function cold03(a){return (a*3+4)&0xffff;}
function cold04(a){return (a*3+5)&0xffff;} function cold05(a){return (a*3+6)&0xffff;}
function cold06(a){return (a*3+7)&0xffff;} function cold07(a){return (a*3+8)&0xffff;}
function cold08(a){return (a*3+9)&0xffff;} function cold09(a){return (a*3+10)&0xffff;}
function cold10(a){return (a*3+11)&0xffff;} function cold11(a){return (a*3+12)&0xffff;}
function cold12(a){return (a*3+13)&0xffff;} function cold13(a){return (a*3+14)&0xffff;}
function cold14(a){return (a*3+15)&0xffff;} function cold15(a){return (a*3+16)&0xffff;}
function cold16(a){return (a*3+17)&0xffff;} function cold17(a){return (a*3+18)&0xffff;}
function cold18(a){return (a*3+19)&0xffff;} function cold19(a){return (a*3+20)&0xffff;}
function k_cold_many(n) {
  var s = 0;
  for (var i = 0; i < n; i++) {
    s += cold00(i) + cold01(i) + cold02(i) + cold03(i) + cold04(i);
    s = s & 0xffffff;
  }
  s += cold05(1)+cold06(1)+cold07(1)+cold08(1)+cold09(1);
  s += cold10(1)+cold11(1)+cold12(1)+cold13(1)+cold14(1);
  s += cold15(1)+cold16(1)+cold17(1)+cold18(1)+cold19(1);
  return s + n;
}

/* ------------------------------------------------------------------ *
 * 12b. THE COMPLETING SLOW PATHS -- kernels that exist to EXECUTE an arm,
 *      not to be fast.
 *
 * ⛔⛔ WHY THESE ARE HERE.  A totalized rung is two arms: a fast one that
 *   makes it worth having, and a COMPLETING one that makes it sound (post a
 *   call nothing may refuse, so the slow case must finish or throw, never
 *   return 0).  MEASURED: after F20 landed and the whole benchmark passed on
 *   three engines, its completing arm counter read `incdecloc_totalized=0` --
 *   the arm had NEVER EXECUTED.  Every kernel above increments an int.
 *   An arm that has never run is not known to work; it is only known not to
 *   have broken anything, which is a different and much weaker statement.
 *
 * Each kernel below is post-call by construction (a `new` before the loop
 * puts the region in the owned regime) and drives the local through a
 * representation the fast arm cannot take: a double, an object with valueOf
 * (which is USER CODE running inside the region -- the thing the ownership
 * argument is about), and a valueOf that THROWS (the exception exit).
 * ------------------------------------------------------------------ */

function k_slow_incloc_double(n) {
  var p = new Pt(1, 2);          /* a call: the loop below is post-call */
  var s = 0, f = 0.5;
  for (var i = 0; i < n; i++) { f++; s = (s + (f | 0)) & 0xffffff; }
  return s + p.x + (f | 0);
}
function k_slow_incloc_object(n) {
  var p = new Pt(1, 2);
  var s = 0, c = 0;
  for (var i = 0; i < n; i++) {
    /* c holds a fresh OBJECT at the top of every iteration, so the increment
       takes the completing path EVERY time and runs a user valueOf inside
       the region.  If the arm's ownership were wrong this either leaks one
       object per iteration or frees one still in use. */
    c = { v: i & 255, valueOf: function () { return this.v; } };
    c++;
    s = (s + c) & 0xffffff;
  }
  return s + p.x;
}
function k_slow_incloc_throw(n) {
  var p = new Pt(1, 2);
  var s = 0;
  var bomb = { valueOf: function () { throw new Error("boom"); } };
  for (var i = 0; i < n; i++) {
    var c = (i & 1023) === 1023 ? bomb : 1;
    try { c++; s = (s + c) & 0xffffff; }
    catch (e) { s = (s + 7) & 0xffffff; }   /* the region's exception exit */
  }
  return s + p.x;
}
function k_slow_addloc_object(n) {
  /* the sibling rung (F15's totalized ADDLOC, counter AOT_HB(3)) on the same
     shape, so the two completing arms are exercised by the same file. */
  var p = new Pt(1, 2);
  var s = 0;
  for (var i = 0; i < n; i++) {
    var o = { valueOf: function () { return 2; } };
    s += o;
    s = s & 0xffffff;
  }
  return s + p.x;
}

/* ------------------------------------------------------------------ *
 * 12c. THE OTHER FOUR COMPLETING ARMS.
 *
 * ⛔⛔ MEASURED 2026-09-02, after the whole benchmark passed on three engines:
 *     AOTHB neg=0 bit2=0 shr=0 addloc=0 loadel=0
 *   ALL FIVE of half (b)'s completing arms read ZERO.  `incdecloc` was the
 *   fifth and section 12b now drives it; these four drive the rest.  `shr` is
 *   the one that matters most: the `>>>` arm is a NAMED OPEN HAZARD in this
 *   lane precisely because no corpus had ever executed it, so a differential
 *   over it proved nothing.
 *
 * Every kernel here is post-call by construction (a `new` before the loop) and
 * feeds the operator a value its fast arm cannot take, so the slow, COMPLETING
 * path is the one that runs.  They are not meant to be fast.
 * ------------------------------------------------------------------ */

function k_slow_neg(n) {                 /* AOT_HB(0): totalized RIR_NEG */
  var p = new Pt(1, 2), s = 0;
  var o = { valueOf: function () { return 3; } };
  for (var i = 0; i < n; i++) { var v = -o; s = (s + v) & 0xffffff; }
  return s + p.x;
}
function k_slow_bit2(n) {                /* AOT_HB(1): totalized RIR_BIT2 */
  var p = new Pt(1, 2), s = 0;
  var o = { valueOf: function () { return 0x0f0f; } };
  for (var i = 0; i < n; i++) { s = (s + ((o & 0xff) | (o ^ 1))) & 0xffffff; }
  return s + p.x;
}
function k_slow_shr(n) {                 /* AOT_HB(2): the `>>>` arm */
  var p = new Pt(1, 2), s = 0;
  var o = { valueOf: function () { return -8; } };
  for (var i = 0; i < n; i++) { s = (s + ((o >>> 4) & 0xffff)) & 0xffffff; }
  return s + p.x;
}
function k_shr_unsigned(n) {             /* `>>>` on the FAST arm, and the
                                            sign boundary the arm exists for */
  var s = 0, x = -1;
  for (var i = 0; i < n; i++) {
    s = (s + ((x >>> 28) & 15) + ((i - 1000000) >>> 30)) & 0xffffff;
  }
  return s + n;
}
function k_slow_loadel(n) {              /* AOT_HB(4): totalized RIR_LOADEL */
  var p = new Pt(1, 2), s = 0;
  var a = [5, 6, 7, 8];
  /* ⛔ THE FIRST VERSION OF THIS USED `a[{valueOf: ...}]` AND THE CHECKSUM
     GUARD CAUGHT IT: an element KEY goes through ToPropertyKey, which calls
     toString, NOT valueOf -- so the read was a["[object Object]"], undefined,
     and `s` stayed 0 for every n.  It would have been a kernel that ran, timed
     cleanly, and exercised nothing.  A string key and an out-of-range index
     are the two real ways off the fast int-index arm. */
  var key = "2";
  for (var i = 0; i < n; i++) {
    s = (s + a[key]) & 0xffffff;         /* string key: not the fast arm */
    if (a[8] === undefined) s = (s + 1) & 0xffffff;   /* out of range */
  }
  return s + p.x;
}
function k_slow_addloc_str(n) {          /* AOT_HB(3): ADDLOC through
                                            js_add_slow's STRING path */
  var p = new Pt(1, 2), s = 0;
  for (var i = 0; i < n; i++) {
    var t = "";
    t += (i & 7);                        /* add_loc on a string local */
    s = (s + t.length) & 0xffffff;
  }
  return s + p.x;
}

/* ------------------------------------------------------------------ *
 * 14. OBJECT LITERALS AND SHAPE TRANSITIONS.
 *   `{a:1,b:2}` is OP_object + OP_define_field and is the next named blocker
 *   after the array literal (~5 % of refused work).  It has no population in
 *   this file beyond alloc.object, so it could not be graded if it were built.
 * ------------------------------------------------------------------ */

function k_objlit_small(n) {
  var s = 0;
  for (var i = 0; i < n; i++) { var o = { a: i, b: i + 1 }; s += o.a + o.b; }
  return s + n;
}
function k_objlit_wide(n) {
  var s = 0;
  for (var i = 0; i < n; i++) {
    var o = { a: i, b: 1, c: 2, d: 3, e: 4, f: 5, g: 6, h: 7 };
    s = (s + o.a + o.h) & 0xffffff;
  }
  return s + n;
}
function k_shape_transition(n) {         /* a property ADDED in a loop:
                                            every iteration changes shape */
  var s = 0;
  for (var i = 0; i < n; i++) {
    var o = { a: 1 };
    o.b = i;                             /* transition */
    s = (s + o.a + o.b) & 0xffffff;
  }
  return s + n;
}
function k_delete_in(n) {
  var s = 0;
  for (var i = 0; i < n; i++) {
    var o = { a: 1, b: 2 };
    if ("a" in o) s += 1;
    delete o.a;
    if ("a" in o) s += 100;              /* must not fire */
    s = (s + o.b) & 0xffffff;
  }
  return s + n;
}
function k_accessor(n) {                 /* USER CODE on a property READ --
                                            the IC and every hoist must bail */
  var o = { v: 1, get g() { return this.v + 1; }, set g2(x) { this.v = x; } };
  var s = 0;
  for (var i = 0; i < n; i++) { o.g2 = i & 255; s = (s + o.g) & 0xffffff; }
  return s + n;
}

/* ------------------------------------------------------------------ *
 * 15. CONTROL-FLOW SHAPES THE FILE DID NOT HAVE.
 *   switch, for-of, do/while and a labeled continue are all ordinary JS and
 *   all produce CFG shapes the flow rung (R4) has to model.  A join-depth
 *   mismatch already shows in the census at 0.86 %; these give it a
 *   population.
 * ------------------------------------------------------------------ */

function k_switch_dense(n) {
  var s = 0;
  for (var i = 0; i < n; i++) {
    switch (i & 7) {
      case 0: s += 1; break;   case 1: s += 2; break;
      case 2: s += 3; break;   case 3: s += 4; break;
      case 4: s += 5; break;   case 5: s += 6; break;
      case 6: s += 7; break;   default: s += 8; break;
    }
  }
  return s + n;
}
function k_switch_sparse(n) {
  var s = 0;
  for (var i = 0; i < n; i++) {
    switch (i & 1023) {
      case 1: s += 1; break;   case 100: s += 2; break;
      case 511: s += 3; break; case 1000: s += 4; break;
      default: s += 5; break;
    }
  }
  return s + n;
}
function k_do_while(n) {
  var s = 0, i = 0;
  do { s += i & 15; i++; } while (i < n);
  return s + n;
}
function k_labeled_continue(n) {
  var s = 0;
  outer:
  for (var i = 0; i < n; i++) {
    for (var j = 0; j < 4; j++) {
      if (j === 2) continue outer;
      s += j;
    }
    s += 100;                            /* unreachable: continue skips it */
  }
  return s + n;
}
function k_for_of(n) {
  var a = [1, 2, 3, 4], s = 0;
  for (var i = 0; i < n; i++) for (var v of a) s += v;
  return s + n;
}
function k_ternary_chain(n) {
  var s = 0;
  for (var i = 0; i < n; i++) {
    var v = (i & 3) === 0 ? 1 : (i & 3) === 1 ? 2 : (i & 3) === 2 ? 3 : 4;
    s += v;
  }
  return s + n;
}
function k_throw_catch(n) {              /* an exception ACTUALLY thrown --
                                            refuse.try_catch never throws */
  var s = 0;
  for (var i = 0; i < n; i++) {
    try { if ((i & 63) === 63) throw i; s += 1; }
    catch (e) { s += (e & 7); }
  }
  return s + n;
}

/* ------------------------------------------------------------------ *
 * 16. BUILTINS AND THE IDIOMS REAL BUNDLES ARE MADE OF.
 *   Every one of these is a C function call from the region's point of view,
 *   so they say what the CALL-BEARING half of a real body costs -- which is
 *   the half every AOT lift is gated away from (`!has_call`).
 * ------------------------------------------------------------------ */

function k_math_float(n) {
  var s = 0;
  for (var i = 0; i < n; i++) {
    s += Math.sqrt(i & 1023) + Math.abs(-(i & 15)) + Math.floor((i & 255) / 7);
    if (s > 1e12) s = 0;
  }
  return s | 0;
}
function k_math_minmax(n) {
  var s = 0;
  for (var i = 0; i < n; i++) s = (s + Math.max(i & 7, 3) + Math.min(i & 15, 9)) & 0xffffff;
  return s + n;
}
function k_string_methods(n) {
  var str = "the/quick/brown/fox/jumps", s = 0;
  for (var i = 0; i < n; i++) {
    s += str.indexOf("brown") + str.charCodeAt(i % 25) + str.length;
    s = s & 0xffffff;
  }
  return s + n;
}
function k_string_build(n) {
  var s = 0;
  for (var i = 0; i < n; i++) {
    var t = "id-" + (i & 255) + "-x";
    s = (s + t.length + t.charCodeAt(0)) & 0xffffff;
  }
  return s + n;
}
function k_array_push(n) {               /* the APPEND idiom through a method */
  var s = 0;
  for (var i = 0; i < n; i++) {
    var a = [];
    a.push(i & 7); a.push(2); a.push(3);
    s = (s + a.length + a[0]) & 0xffffff;
  }
  return s + n;
}
function k_array_iter_methods(n) {
  var a = [1, 2, 3, 4, 5, 6, 7, 8], s = 0;
  for (var i = 0; i < n; i++) {
    s += a.indexOf(5) + a.length;
    s = s & 0xffffff;
  }
  return s + n;
}
function k_json_roundtrip(n) {
  var o = { a: 1, b: "two", c: [3, 4], d: { e: 5 } }, s = 0;
  for (var i = 0; i < n; i++) {
    var t = JSON.stringify(o);
    var q = JSON.parse(t);
    s = (s + t.length + q.a + q.d.e) & 0xffffff;
  }
  return s + n;
}
function k_float64array(n) {
  var a = new Float64Array(64), s = 0;
  for (var k = 0; k < 64; k++) a[k] = k * 0.5;
  for (var i = 0; i < n; i++) { var j = i & 63; a[j] = a[j] + 0.25; s += a[j]; }
  return s | 0;
}
function k_array_holes(n) {              /* the HOLE test in the elem guard */
  var a = [0, 1, 2, 3, 4, 5, 6, 7];
  delete a[3];
  var s = 0;
  for (var i = 0; i < n; i++) { var v = a[i & 7]; s += (v === undefined) ? 1 : v; }
  return s + n;
}

/* ------------------------------------------------------------------ *
 * 17. CALL-SITE POLYMORPHISM AND CLOSURES.
 *   Every call kernel above is monomorphic; a real dispatch site is not.
 *   `fclosure8` also shows in the census (0.63 %) with no population.
 * ------------------------------------------------------------------ */

function d0(a) { return a + 1; } function d1(a) { return a + 2; }
function d2(a) { return a + 3; } function d3(a) { return a + 4; }
function k_call_polymorphic(n) {
  var fns = [d0, d1, d2, d3], s = 0;
  for (var i = 0; i < n; i++) s = (s + fns[i & 3](i & 255)) & 0xffffff;
  return s + n;
}
function k_method_polymorphic(n) {
  function A() { this.v = 1; } A.prototype.m = function () { return this.v; };
  function B() { this.v = 2; } B.prototype.m = function () { return this.v * 2; };
  var o = [new A(), new B()], s = 0;
  for (var i = 0; i < n; i++) s = (s + o[i & 1].m()) & 0xffffff;
  return s + n;
}
function k_closure_alloc(n) {            /* a closure CREATED per iteration */
  var s = 0;
  for (var i = 0; i < n; i++) {
    var base = i & 7;
    var f = function (x) { return x + base; };
    s = (s + f(1)) & 0xffffff;
  }
  return s + n;
}
function k_deep_recursion(n) {
  function down(k) { return k === 0 ? 0 : 1 + down(k - 1); }
  var s = 0;
  for (var i = 0; i < n; i++) s += down(40);
  return s + n;
}

/* ------------------------------------------------------------------ *
 * 13. WORKLOAD-SHAPED COMPOSITES.  A kernel that is one opcode in a loop is
 *     not what any real body looks like; these three mix the shapes the way
 *     the Octane rows do, and exist so a per-shape win can be checked
 *     against a composite before it is claimed as a program-level win.
 *     ⛔ PART-PRICES DO NOT SUM TO THE WHOLE-PRICE -- that is exactly why
 *     these are separate kernels and not a derived number.
 * ------------------------------------------------------------------ */

function k_mix_numeric(n) {           /* crypto/navierstokes shaped */
  var a = [], s = 0;
  for (var k = 0; k < 32; k++) a[k] = k * 7 + 1;
  for (var i = 0; i < n; i++) {
    var j = i & 31;
    var x = a[j];
    x = (x * 3 + 5) & 0xffff;
    x = x ^ (x >> 3);
    x = (x + 1234) % 30011;
    a[j] = x;
    s = (s + x) & 0xffffff;
  }
  return s + n;
}
function k_mix_objects(n) {           /* richards/deltablue shaped */
  var head = null;
  for (var k = 0; k < 16; k++) head = { next: head, v: k, w: 0 };
  var s = 0;
  for (var i = 0; i < n; i++) {
    var p = head;
    while (p !== null) { p.w = p.v + i; s += p.w; p = p.next; }
    s = s & 0xffffff;
  }
  return s + n;
}
function k_mix_calls(n) {             /* raytrace shaped: math through calls */
  function vadd(a, b) { return { x: a.x + b.x, y: a.y + b.y }; }
  function vdot(a, b) { return a.x * b.x + a.y * b.y; }
  var u = { x: 1.5, y: 2.5 }, v = { x: 0.5, y: 0.25 }, s = 0;
  for (var i = 0; i < n; i++) { var w = vadd(u, v); s += vdot(w, v); }
  return s | 0;
}

/* ------------------------------------------------------------------ *
 * Registration.  Iteration counts are chosen so every kernel lands in the
 * same order of magnitude of wall time on an unaccelerated build, which is
 * what makes the per-kernel ratios comparable to each other.
 * ------------------------------------------------------------------ */

bench("int.add_imm8",      k_int_add_imm8,      2000000, linear);
bench("int.bin_imm8",      k_int_bin_imm8,      2000000, linear);
bench("int.bin_imm16",     k_int_bin_imm16,     2000000, linear);
bench("int.bin_imm32",     k_int_bin_imm32,     2000000, linear);
bench("int.sar_imm8",      k_int_sar_imm8,      2000000, linear);
bench("int.loc_mul",       k_int_loc_mul,       2000000, linear);
bench("int.frame_bin",     k_int_frame_bin,     2000000, linear);
bench("int.frame_bin_arg", function (n) { return k_int_frame_bin_arg(n, 5, 3); },
                                                2000000, linear);

bench("int.bitops",        k_bitops,            2000000, linear);
bench("int.incdec_loc",    k_incdec_loc,        2000000, linear);
bench("int.postinc",       k_postinc,           2000000, linear);
bench("int.unary",         k_unary,             2000000, linear);

bench("flow.cmp_branch",   k_cmp_branch,        2000000, linear);
bench("flow.nested",       k_loop_nested,        300000, linear);
bench("flow.while_break",  k_loop_while_break,   500000, linear);

bench("fp.arith",          k_float_arith,       2000000, linear);
bench("fp.div_mod",        k_float_div_mod,     2000000, linear);
bench("fp.mixed",          k_mixed_int_float,   2000000, linear);

bench("prop.mono_nocall", function (n) { return k_field_mono_nocall(n, new Pt(1, 2)); },
                            2000000, linear);
bench("prop.mono",         k_field_mono,        2000000, linear);
bench("prop.store",        k_field_store,       2000000, linear);
bench("prop.poly2",        k_field_poly2,       2000000, linear);
bench("prop.mega",         k_field_mega,        2000000, linear);
bench("prop.proto",        k_field_proto,       2000000, linear);
bench("prop.chain",        k_field_chain,       2000000, linear);

bench("elem.load",         k_array_load,        2000000, linear);
bench("elem.store",        k_array_store,       2000000, linear);
bench("elem.length",       k_array_len,         2000000, linear);
bench("elem.sum_len",      k_array_sum_len,      100000, linear);
bench("elem.literal",      k_array_literal,     2000000, linear);
bench("elem.literal_loop", k_array_literal_sum,  600000, linear);
bench("elem.typedarray",   k_typedarray,        2000000, linear);

bench("call.call0",        k_call0,             1000000, linear);
bench("call.call1",        k_call1,             1000000, linear);
bench("call.call2",        k_call2,             1000000, linear);
bench("call.call3",        k_call3,             1000000, linear);
bench("call.method",       k_method_call,       1000000, linear);
bench("call.ctor",         k_ctor_call,          800000, linear);
bench("call.loop_arith",   k_call_in_loop_arith,1000000, linear);
bench("call.recursion",    k_recursion,            5000, linear);

bench("var.global_read",   k_global_read,       2000000, linear);
bench("var.global_write",  k_global_write,      2000000, linear);
bench("var.closure_cell",  k_closure_cell,      1000000, linear);

bench("tag.predicates",    k_tag_predicates,    1000000, linear);
bench("tag.instanceof",    k_instanceof,        1000000, linear);

bench("refuse.try_catch",  k_try_catch,         1000000, linear);
bench("refuse.str_concat", k_string_concat,      500000, linear);
bench("refuse.str_charcode", k_string_charcode, 1000000, linear);
bench("refuse.forin",      k_forin,              300000, linear);
bench("alloc.object",      k_alloc_object,       500000, linear);
bench("alloc.array",       k_alloc_array,        500000, linear);

bench("caps.locals16",     k_locals_16,         1000000, linear);
bench("caps.locals48",     k_locals_48,          400000, linear);
bench("caps.straightline", k_long_straightline,  600000, linear);

bench("hot.tiny",          k_hot_tiny,          1000000, linear);
bench("hot.cold_many",     k_cold_many,          500000, linear);

bench("slow.incloc_double", k_slow_incloc_double, 400000, linear);
bench("slow.incloc_object", k_slow_incloc_object, 200000, linear);
bench("slow.incloc_throw",  k_slow_incloc_throw,  200000, linear);
bench("slow.addloc_object", k_slow_addloc_object, 200000, linear);

bench("slow.neg",          k_slow_neg,           200000, linear);
bench("slow.bit2",         k_slow_bit2,          200000, linear);
bench("slow.shr",          k_slow_shr,           200000, linear);
bench("slow.loadel",       k_slow_loadel,        200000, linear);
bench("slow.addloc_str",   k_slow_addloc_str,    200000, linear);
bench("int.shr_unsigned",  k_shr_unsigned,      1500000, linear);

bench("obj.literal_small", k_objlit_small,       500000, linear);
bench("obj.literal_wide",  k_objlit_wide,        300000, linear);
bench("obj.transition",    k_shape_transition,   400000, linear);
bench("obj.delete_in",     k_delete_in,          300000, linear);
bench("obj.accessor",      k_accessor,           500000, linear);

bench("flow.switch_dense", k_switch_dense,      1500000, linear);
bench("flow.switch_sparse",k_switch_sparse,     1500000, linear);
bench("flow.do_while",     k_do_while,          1500000, linear);
bench("flow.label_cont",   k_labeled_continue,   800000, linear);
bench("flow.for_of",       k_for_of,             200000, linear);
bench("flow.ternary",      k_ternary_chain,     1000000, linear);
bench("flow.throw_catch",  k_throw_catch,        300000, linear);

bench("bi.math_float",     k_math_float,         800000, linear);
bench("bi.math_minmax",    k_math_minmax,        800000, linear);
bench("bi.string_methods", k_string_methods,     500000, linear);
bench("bi.string_build",   k_string_build,       300000, linear);
bench("bi.array_push",     k_array_push,         300000, linear);
bench("bi.array_indexof",  k_array_iter_methods, 800000, linear);
bench("bi.json",           k_json_roundtrip,      60000, linear);
bench("bi.float64array",   k_float64array,      1000000, linear);
bench("bi.array_holes",    k_array_holes,       1000000, linear);

bench("call.polymorphic",  k_call_polymorphic,   600000, linear);
bench("call.method_poly",  k_method_polymorphic, 600000, linear);
bench("call.closure_alloc",k_closure_alloc,      400000, linear);
bench("call.deep_recurse",  k_deep_recursion,     40000, linear);

bench("mix.numeric",       k_mix_numeric,       1000000, linear);
bench("mix.objects",       k_mix_objects,        100000, linear);
bench("mix.calls",         k_mix_calls,          500000, linear);

/* ------------------------------------------------------------------ *
 * Report.
 * ------------------------------------------------------------------ */

var i, r;

/* ⛔ THE MARKER CHANGED FROM ##CB## TO ##CB2## ON PURPOSE.
 * `ms` is now a FLOAT (time divided by the inner repeat count).  Every parser
 * in this project matched ms with `(-?\d+)`, which against "12.3456" matches
 * "12" and then fails on the ".", so the line is SILENTLY DROPPED and the
 * geomean is taken over whatever survived.  This project has already been
 * bitten by a printed table that was not its geomean's population.  A new
 * marker makes an old parser read ZERO kernels -- loudly wrong instead of
 * quietly wrong.
 *
 * ##CB2## <name> <ms_per_call> <n> <checksum> <score> <inner> <span_ms> */
for (i = 0; i < results.length; i++) {
  r = results[i];
  /* PER-KERNEL SCORE.  ⛔ IT IS COMPARABLE ACROSS BINARIES FOR ONE KERNEL,
     NEVER ACROSS KERNELS: an "iteration" means something different in every
     kernel, so a kernel with a high score is not doing better than one with a
     low score -- it is doing something cheaper.  The only legitimate reading
     is armA.score / armB.score for the SAME name. */
  var sc = r.ms > 0 ? (r.n / r.ms) : 0;
  print("##CB2## " + r.name + " " + (r.ms > 0 ? r.ms.toFixed(5) : r.ms) + " " +
        r.n + " " + r.sum + " " + sc.toFixed(1) + " " + (r.inner | 0) + " " +
        (r.span || 0) + (r.bad ? " WRONG" : ""));
}

/* The aggregate is a GEOMETRIC MEAN OF TIMES, reported as its reciprocal so
   that -- like every other row in this project -- HIGHER IS BETTER and the
   number can be divided by another binary's to get a ratio.  Kernels that
   failed their checksum are EXCLUDED FROM THE MEAN AND COUNTED SEPARATELY:
   folding a wrong answer's time into a score is how a crash flatters a
   number. */
var logsum = 0, cnt = 0;
for (i = 0; i < results.length; i++) {
  r = results[i];
  if (r.bad || !(r.ms > 0)) continue;
  logsum += Math.log(r.ms);
  cnt++;
}
var geo = cnt ? Math.exp(logsum / cnt) : 0;

/* ⛔ THE SUMMARY LINE NO LONGER CARRIES THE KERNEL MARKER.  It used to read
   "##CB## kernels 93 scored 93 failed 0", which every name-keyed parser in
   this project happily accepted as a 94th kernel called `kernels` with
   ms=93 -- and it was in the geomean population of the 09-02 scoreboard. */
print("##CBSUM## kernels " + results.length + " scored " + cnt +
      " failed " + failures.length + " warns " + warns.length);
for (i = 0; i < failures.length; i++) print("##CBFAIL## " + failures[i]);
for (i = 0; i < warns.length; i++) print("##CBWARN## " + warns[i]);

/* CALIBRATION OUTPUT: paste this over the CB_INNER literal above.  Take it on
   the FAST arm -- see the CB_INNER comment for why both arms must share it. */
if (CB_CALIBRATE) {
  var parts = [];
  for (i = 0; i < results.length; i++)
    if (results[i].inner) parts.push('"' + results[i].name + '":' + results[i].inner);
  print("##CBINNER## var CB_INNER = {" + parts.join(",") + "};");
}

if (failures.length) print("Alert: " + failures.length + " kernel(s) computed the wrong answer");
print("Geomean-ms: " + geo.toFixed(4));
print("Score: " + (cnt ? (10000 / geo).toFixed(2) : 0));