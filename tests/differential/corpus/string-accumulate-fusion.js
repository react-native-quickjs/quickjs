/*
 * Differential corpus for the fused string-accumulation path
 * (patch 0051, `js_accum_append` / `js_add_accum_fused`).
 *
 * WHAT THE MECHANISM DOES: when `x += s` is executed and the store that follows
 * the `add` targets storage that holds the very JSString being concatenated, and
 * that string's refcount proves the storage plus the operand stack are its only
 * holders, the engine APPENDS INTO THE LIVE STRING instead of allocating a copy.
 *
 * So every case below is a way for that proof to be wrong. The failure mode of a
 * missing guard is not an exception, it is a *silently mutated string that
 * someone else can see* -- which is why this diffs against node rather than
 * against an expectation written here.
 *
 * TWO THINGS THAT MAKE A CASE READ ZERO, both of which have cost this project
 * real time and both of which are deliberately avoided here:
 *
 *   * EVERY accumulation must be inside a function. At top level `var s` is a
 *     property of the global object, so `s += x` compiles to get_var/put_var and
 *     never reaches the path; and a top-level expression statement saves its
 *     completion value, so `o.s += x` compiles to `add; insert2; put_field` and
 *     the `add` is not adjacent to the store.
 *   * The accumulator must actually grow past its allocation slack, or the
 *     regrowth branch is never taken. Loop counts below are chosen to cross
 *     JS_STRING_ACCUM_MIN_GROW (64) and several reallocations.
 *
 * `bench/spikes/octane-rescore/instrument-accum.py` is the counting build that
 * proves which arm each case lands in.
 */
var out = [];
function say(label, v) { out.push(label + '=' + v); }

/* ---- 1. the basic shapes, one per store form ------------------------------ */

function accLocal(n) {                    /* put_loc0..3 / put_loc8 */
  var s = '';
  for (var i = 0; i < n; i++) s += 'a';
  return s.length + ':' + s.charAt(0) + s.charAt(n - 1);
}
say('local', accLocal(5) + ',' + accLocal(300));

function accLocalManyVars(n) {             /* forces a high local index */
  var a0=0,a1=0,a2=0,a3=0,a4=0,a5=0,a6=0,a7=0,a8=0,a9=0;
  var s = '';
  for (var i = 0; i < n; i++) s += 'b';
  return s.length + ':' + (a0+a1+a2+a3+a4+a5+a6+a7+a8+a9);
}
say('localHighIdx', accLocalManyVars(300));

function accField(n) {                     /* put_field_ic */
  var o = { s: '' };
  for (var i = 0; i < n; i++) o.s += 'c';
  return o.s.length + ':' + o.s.charAt(n - 1);
}
say('field', accField(5) + ',' + accField(300));

function accNested(n) {                    /* o.a.b += x */
  var o = { a: { b: '' } };
  for (var i = 0; i < n; i++) o.a.b += 'd';
  return o.a.b.length;
}
say('nested', accNested(300));

function accAddLoc(n) {                    /* the OP_add_loc peephole shape:
                                              adjacent single-instruction RHS */
  var s = '';
  for (var i = 0; i < n; i++) s += 'e';
  var t = '';
  var ch = 'f';
  for (var i = 0; i < n; i++) t += ch;
  return s.length + ':' + t.length + ':' + (s === t);
}
say('addLoc', accAddLoc(300));

/* ---- 2. THE ALIASING CASES: another holder exists ------------------------- */

/* Every earlier stage is kept alive, so the accumulator is never uniquely
   referenced and must never be extended in place. If it were, every element of
   `keep` would observe the final string. */
function aliasedLocal(n) {
  var s = '', keep = [];
  for (var i = 0; i < n; i++) { keep.push(s); s += 'g'; }
  var lens = [];
  for (var i = 0; i < keep.length; i++) lens.push(keep[i].length);
  return lens.join('') === lens.join('') ? keep[n - 1] + '|' + s.length + '|' + lens[n - 1] : 'x';
}
say('aliasedLocal', aliasedLocal(80));

function aliasedField(n) {
  var o = { s: '' }, keep = [];
  for (var i = 0; i < n; i++) { keep.push(o.s); o.s += 'h'; }
  return keep[n - 1].length + '|' + o.s.length + '|' + keep[10];
}
say('aliasedField', aliasedField(80));

/* alias created only every other iteration: the path must switch on and off */
function aliasedAlternate(n) {
  var s = '', keep = [];
  for (var i = 0; i < n; i++) { if (i % 2 === 0) keep.push(s); s += 'i'; }
  return keep[keep.length - 1].length + '|' + s.length;
}
say('aliasedAlternate', aliasedAlternate(120));

/* the alias lives in ANOTHER local, not in an array */
function aliasedSecondLocal(n) {
  var s = '', t = '';
  for (var i = 0; i < n; i++) { t = s; s += 'j'; }
  return t.length + '|' + s.length + '|' + (t === s);
}
say('aliasedSecondLocal', aliasedSecondLocal(120));

/* the alias is captured by a closure */
function aliasedClosure(n) {
  var s = '', snap = null;
  for (var i = 0; i < n; i++) {
    if (i === 60) { var c = s; snap = function () { return c; }; }
    s += 'k';
  }
  return snap().length + '|' + s.length;
}
say('aliasedClosure', aliasedClosure(120));

/* the alias is a Map key and an object key -- both hold a reference, and an
   object key additionally interns the string as an atom */
function aliasedAsKey(n) {
  var s = '', m = {}, seen = [];
  for (var i = 0; i < n; i++) { if (i === 70) { m[s] = 1; seen.push(s); } s += 'l'; }
  var ks = Object.keys(m);
  return ks[0].length + '|' + seen[0].length + '|' + s.length;
}
say('aliasedAsKey', aliasedAsKey(120));

/* the accumulator IS an interned atom to begin with */
function fromAtomKey(n) {
  var o = { someKeyName: 1 };
  var s = Object.keys(o)[0];
  for (var i = 0; i < n; i++) s += 'm';
  return s.length + '|' + Object.keys(o)[0];
}
say('fromAtomKey', fromAtomKey(120));

/* ---- 3. SLICED / SUBSTRING accumulators ---------------------------------- */

/* A long string's slice shares the parent's characters. Appending into a slice
   would write into the parent's buffer; appending into a parent that a slice
   points into would be observed through the slice. Both are exercised, at
   lengths on both sides of the slice threshold. */
function sliceAccum(n) {
  var big = '';
  for (var i = 0; i < 4000; i++) big += 'n';
  var sl = big.substring(10, 3000);         /* long enough to be a slice */
  var acc = sl;
  for (var i = 0; i < n; i++) acc += 'o';
  return acc.length + '|' + sl.length + '|' + big.length + '|' +
         acc.charAt(2989) + acc.charAt(2990) + '|' + big.charAt(3999);
}
say('sliceAccum', sliceAccum(200));

function parentWithLiveSlice(n) {
  var p = '';
  for (var i = 0; i < 4000; i++) p += 'p';
  var sl = p.substring(0, 3500);
  for (var i = 0; i < n; i++) p += 'q';
  return p.length + '|' + sl.length + '|' + sl.charAt(3499) + '|' + p.charAt(3999);
}
say('parentWithLiveSlice', parentWithLiveSlice(200));

function shortSliceOfLongParent(n) {
  var p = '';
  for (var i = 0; i < 20000; i++) p += 'r';
  var sl = p.substring(5, 8);               /* short slice, long parent */
  var acc = sl;
  for (var i = 0; i < n; i++) acc += 's';
  return acc.length + '|' + acc.charAt(0) + acc.charAt(3) + '|' + p.length + '|' + p.charAt(19999);
}
say('shortSliceOfLongParent', shortSliceOfLongParent(200));

/* ---- 4. WIDTH transitions and surrogate pairs ---------------------------- */

function narrowThenWide(n) {
  var s = '';
  for (var i = 0; i < n; i++) s += 't';
  for (var i = 0; i < n; i++) s += '中';
  return s.length + '|' + s.charCodeAt(n - 1) + '|' + s.charCodeAt(n);
}
say('narrowThenWide', narrowThenWide(300));

function wideThenNarrow(n) {
  var s = '';
  for (var i = 0; i < n; i++) s += 'é';
  for (var i = 0; i < n; i++) s += 'u';
  return s.length + '|' + s.charCodeAt(0) + '|' + s.charCodeAt(n);
}
say('wideThenNarrow', wideThenNarrow(300));

/* a surrogate pair split across two appends must survive as one code point */
function splitSurrogate(n) {
  var s = '';
  for (var i = 0; i < n; i++) { s += '\ud83d'; s += '\ude00'; }
  return s.length + '|' + encodeURIComponent(s.slice(0, 2)) + '|' +
         JSON.stringify(s.slice(0, 2)).length + '|' + s.codePointAt(0) + '|' +
         s.charCodeAt(2 * n - 1);
}
say('splitSurrogate', splitSurrogate(200));

/* a lone surrogate must stay lone */
function loneSurrogate(n) {
  var s = '';
  for (var i = 0; i < n; i++) s += '\udc00';
  return s.length + '|' + s.charCodeAt(0) + '|' + s.charCodeAt(n - 1);
}
say('loneSurrogate', loneSurrogate(200));

/* ---- 5. the store target is not a plain writable data slot --------------- */

function accessorTarget() {
  var log = [];
  var o = { get s() { return log.join(''); }, set s(v) { log.push('S'); } };
  for (var i = 0; i < 5; i++) o.s += 'v';
  return log.join('') + '|' + o.s;
}
say('accessorTarget', accessorTarget());

function protoAccessorTarget() {
  var proto = { get s() { return 'P'; }, set s(v) { this._v = v; } };
  var o = Object.create(proto);
  for (var i = 0; i < 5; i++) o.s += 'w';
  return o._v + '|' + o.s;
}
say('protoAccessorTarget', protoAccessorTarget());

function nonWritableTarget() {
  var o = {};
  Object.defineProperty(o, 's', { value: 'V', writable: false, configurable: true });
  o.s += 'x';
  var f = Object.freeze({ s: 'F' });
  f.s += 'y';
  return o.s + '|' + f.s;
}
say('nonWritableTarget', nonWritableTarget());

function proxyTarget() {
  var log = [];
  var p = new Proxy({ s: '' }, {
    get: function (t, k) { log.push('g'); return t[k]; },
    set: function (t, k, v) { log.push('s'); t[k] = v + '!'; return true; },
  });
  for (var i = 0; i < 5; i++) p.s += 'z';
  return p.s + '|' + log.length;
}
say('proxyTarget', proxyTarget());

/* an accumulator that is a String OBJECT's boxed value, and a length property */
function exoticTargets() {
  var so = new String('abc');
  so.extra = '';
  for (var i = 0; i < 100; i++) so.extra += '1';
  var a = ['q'];
  var r = '' + a.length;
  return so.extra.length + '|' + so.valueOf() + '|' + r;
}
say('exoticTargets', exoticTargets());

/* ---- 6. right-hand sides that are not plain strings ---------------------- */

function rhsNumber(n) {
  var s = '';
  for (var i = 0; i < n; i++) s += i;
  return s.length + '|' + s.substring(0, 12);
}
say('rhsNumber', rhsNumber(200));

/* the RHS's valueOf mutates the accumulator's storage. The spec order is
   GetValue(target), evaluate RHS, add, PutValue -- so the write performed by
   valueOf is overwritten by the assignment, and the concatenation uses the OLD
   value. Any fused path that resolved the target slot before the RHS ran, or
   that read the slot again afterwards, gets a different answer here.
   NB the LOCAL variant of this case is parked in
   tests/differential/pending/add-loc-operand-order.js: quickjs-ng's OP_add_loc
   reads the accumulator AFTER running the RHS's valueOf and so diverges from
   node, independently of this patch. */
function rhsMutatesTarget() {
  var o = { s: 'A' };
  var evil = { valueOf: function () { o.s = 'HIJACKED'; return 'B'; } };
  o.s += evil;
  return o.s;
}
say('rhsMutatesTarget', rhsMutatesTarget());

/* the RHS's valueOf makes the accumulator non-uniquely referenced */
function rhsAliasesTarget() {
  var keep = null;
  var o = { s: '' };
  for (var i = 0; i < 100; i++) o.s += 'C';
  var evil = { valueOf: function () { keep = o.s; return 'D'; } };
  o.s += evil;
  return keep.length + '|' + o.s.length + '|' + o.s.charAt(100);
}
say('rhsAliasesTarget', rhsAliasesTarget());

/* the RHS deletes / redefines the target property */
function rhsDeletesTarget() {
  var o = { s: 'E' };
  var evil = { valueOf: function () { delete o.s; return 'F'; } };
  o.s += evil;
  var o2 = { s: 'G' };
  var evil2 = { valueOf: function () {
    Object.defineProperty(o2, 's', { get: function () { return 'GET'; }, configurable: true });
    return 'H';
  } };
  o2.s += evil2;
  return o.s + '|' + o2.s;
}
say('rhsDeletesTarget', rhsDeletesTarget());

/* empty right-hand side, and empty accumulator */
function emptyOperands() {
  var s = 'I';
  for (var i = 0; i < 10; i++) s += '';
  var t = '';
  for (var i = 0; i < 10; i++) t += '';
  var u = '';
  u += 'J';
  return s + '|' + s.length + '|' + t.length + '|' + u;
}
say('emptyOperands', emptyOperands());

/* ---- 7. identity, interning and hashing after mutation ------------------- */

/* `===` on strings, and use as a property key, must both see the new content.
   A cached hash left over from before the append would break both. */
function identityAfterAppend(n) {
  var a = '', b = '';
  for (var i = 0; i < n; i++) { a += 'K'; b += 'K'; }
  var m = {};
  m[a] = 1;
  var res = [a === b, a.length === b.length, m[b] === 1, Object.keys(m)[0] === a];
  a += 'L';
  res.push(a === b, m[a] === undefined, a.length);
  return res.join(',');
}
say('identityAfterAppend', identityAfterAppend(200));

function hashAfterAppend(n) {
  var s = '', keys = {};
  for (var i = 0; i < n; i++) { s += 'M'; keys[s] = i; }
  var count = 0;
  for (var k in keys) count++;
  return count + '|' + keys[s] + '|' + Object.keys(keys).length;
}
say('hashAfterAppend', hashAfterAppend(150));

/* reading the accumulator mid-flight, by index, by charCodeAt and by slice */
function readMidFlight(n) {
  var s = '', acc = 0, tail = '';
  for (var i = 0; i < n; i++) {
    s += 'N';
    if (i > 100) { acc += s.charCodeAt(i - 1); tail = s.slice(-1) + s[0]; }
  }
  return s.length + '|' + acc + '|' + tail;
}
say('readMidFlight', readMidFlight(200));

/* JSON round-trip of a heavily accumulated string */
function jsonRoundTrip(n) {
  var s = '';
  for (var i = 0; i < n; i++) s += 'O"\\ÿ';
  var j = JSON.stringify(s);
  return s.length + '|' + j.length + '|' + (JSON.parse(j) === s);
}
say('jsonRoundTrip', jsonRoundTrip(120));

/* comparison, search and regexp over an accumulated string */
function consumers(n) {
  var s = '';
  for (var i = 0; i < n; i++) s += 'PQ';
  return [s.length, s.indexOf('QP'), s.lastIndexOf('PQ'), s.charCodeAt(2 * n - 1),
          /(?:PQ){10}/.test(s), s.split('Q').length, s.toUpperCase().length,
          s.localeCompare(s), s < s + 'R', s.substring(n, n + 4)].join(',');
}
say('consumers', consumers(150));

/* ---- 8. exception paths and loop/try interaction ------------------------- */

function throwsMidLoop(n) {
  var o = { s: '' }, thrown = 0;
  for (var i = 0; i < n; i++) {
    try {
      if (i === 50) o.s += { valueOf: function () { throw new Error('boom'); } };
      else o.s += 'S';
    } catch (e) { thrown++; }
  }
  return o.s.length + '|' + thrown + '|' + o.s.charAt(0) + o.s.charAt(o.s.length - 1);
}
say('throwsMidLoop', throwsMidLoop(120));

/* the accumulator survives a nested call that re-enters the same function */
function reentrant(depth) {
  var s = '';
  for (var i = 0; i < 100; i++) {
    s += 'T';
    if (i === 50 && depth > 0) s += reentrant(depth - 1);
  }
  return s.length + ':' + s.charAt(0);
}
say('reentrant', reentrant(2));

/* accumulate in a generator across yields, so the frame is suspended mid-loop */
function genAccum(n) {
  function* g() {
    var s = '';
    for (var i = 0; i < n; i++) { s += 'U'; if (i % 40 === 0) yield s.length; }
    return s;
  }
  var it = g(), r = [], v;
  while (!(v = it.next()).done) r.push(v.value);
  return r.join(',') + '|' + v.value.length;
}
say('genAccum', genAccum(200));

/* ---- 8b. cases added because mutation testing found the corpus blind ----- *
 * Each of the three below kills a mutant that the rest of the file did NOT.
 * They are separated out so the reason they exist is not lost: a guard whose
 * removal the corpus does not notice is untested, whatever its line count. */

/* IDENTITY GUARD. `s = t + u` where the STORE target `s` and the CONCATENATED
   string `t` are different strings, and `t` happens to be at refcount 2 (its own
   local slot plus the operand stack). Without the pointer-identity check the
   engine appends into `t` and never stores to `s`: `t` grows by one and `s` keeps
   its old value. `t` must be a COMPUTED string, because a literal is an interned
   atom and would be rejected for a different reason. */
/* TWO THINGS ABOUT THE SPELLING OF THIS CASE, both established by building the
   mutant and looking at the bytecode rather than by reasoning:
 *
 *   1. The result must be reported by a LATER read of `s`, not by an expression
 *      that consumes the assignment's own value. `var s = ...; s = t + 'Z'; if
 *      (s.length !== ...)` compiles the store as `set_loc`, the store-and-keep
 *      form, which the fused path does not recognise at all -- so the mutant
 *      survives for want of coverage rather than for want of a bug. Returning a
 *      string that reads `s` afterwards makes the store a `put_loc`, which is
 *      the form the path handles.
 *   2. The length is swept, because whether the accumulator has spare capacity
 *      at the moment of the trial depends on where its last geometric regrowth
 *      landed, and only the IN-PLACE branch is wrong without the identity check
 *      -- the regrowth branch stores through the slot and is accidentally
 *      correct. A single trial passes about half the time. */
function identityTrial(n) {
  var t = '';
  for (var i = 0; i < n; i++) t += 'x';
  var s = 'unchanged';
  s = t + 'Z';
  return n + '->s' + s.length + '/t' + t.length;
}
function identityGuard() {
  var bad = [];
  for (var n = 60; n < 200; n++) {
    var got = identityTrial(n);
    if (got !== n + '->s' + (n + 1) + '/t' + n) bad.push(got);
  }
  /* MEASURED: with the identity check removed this reports 137 of 140 lengths
     wrong. The three that pass are the lengths at which the accumulator had no
     spare capacity, so the regrowth branch ran instead of the in-place one. */
  return bad.length + '|' + bad.slice(0, 3).join(',');
}
say('identityGuard', identityGuard());

/* NON-WRITABLE TARGET, with the value held ONLY by that property slot, so the
   refcount check cannot shield the flags check. In sloppy mode the assignment is
   silently ignored; without the flags check the engine appends into the
   non-writable value in place and the frozen string changes length. */
function nonWritableSoleRef() {
  var v = '';
  for (var i = 0; i < 200; i++) v += 'b2';
  var o = {};
  Object.defineProperty(o, 'p', { value: v, writable: false, configurable: true });
  v = null;                       /* the property slot is now the only holder */
  o.p += 'Z';
  var f = {};
  var w = '';
  for (var i = 0; i < 200; i++) w += 'c3';
  Object.defineProperty(f, 'q', { value: w, writable: true, configurable: true });
  Object.freeze(f);
  w = null;
  f.q += 'Z';
  return o.p.length + '|' + o.p.charAt(399) + '|' + f.q.length + '|' + f.q.charAt(399);
}
say('nonWritableSoleRef', nonWritableSoleRef());

/* STORE-INSTRUCTION LENGTH. The fused path skips the store it absorbed, so the
   number of bytes skipped must match that store's encoding. put_loc0..3 are one
   byte, put_loc8 is two and put_loc is three; only the one-byte forms are
   reachable unless the accumulator sits at a high local index. Enough locals are
   declared here to push it past 3, and past 255 for the u16 form.  Getting the
   skip wrong desynchronises the instruction stream, so the failure is arbitrary
   rather than subtle -- which is exactly why it must be covered.

   THE RIGHT-HAND SIDE MUST BE MULTI-INSTRUCTION (`rhs.u`, not a literal). With a
   single-instruction right-hand side the resolve_labels peephole folds
   `get_loc(n) <push> add dup put_loc(n) drop` into `<push> add_loc(n)`, which
   reaches the accumulate path by the OTHER route -- the one that absorbs no store
   and skips nothing -- so the skip length is never exercised at all. A first
   version of this case used `acc += 'd4'` and left the mutant alive. */
function accumAtHighLocalIndex(n) {
  var f0=0,f1=1,f2=2,f3=3,f4=4,f5=5,f6=6,f7=7,f8=8,f9=9;
  var g0=0,g1=1,g2=2,g3=3,g4=4,g5=5,g6=6,g7=7,g8=8,g9=9;
  var rhs = { u: 'd4' };
  var acc = '';
  for (var i = 0; i < n; i++) acc += rhs.u;
  var sum = f0+f1+f2+f3+f4+f5+f6+f7+f8+f9+g0+g1+g2+g3+g4+g5+g6+g7+g8+g9;
  return acc.length + '|' + sum + '|' + acc.charAt(2 * n - 1);
}
say('accumAtHighLocalIndex', accumAtHighLocalIndex(300));

function accumAtVeryHighLocalIndex(n) {
  var v000,v001,v002,v003,v004,v005,v006,v007,v008,v009,v010,v011,v012,v013,v014,v015;
  var v016,v017,v018,v019,v020,v021,v022,v023,v024,v025,v026,v027,v028,v029,v030,v031;
  var v032,v033,v034,v035,v036,v037,v038,v039,v040,v041,v042,v043,v044,v045,v046,v047;
  var v048,v049,v050,v051,v052,v053,v054,v055,v056,v057,v058,v059,v060,v061,v062,v063;
  var v064,v065,v066,v067,v068,v069,v070,v071,v072,v073,v074,v075,v076,v077,v078,v079;
  var v080,v081,v082,v083,v084,v085,v086,v087,v088,v089,v090,v091,v092,v093,v094,v095;
  var v096,v097,v098,v099,v100,v101,v102,v103,v104,v105,v106,v107,v108,v109,v110,v111;
  var v112,v113,v114,v115,v116,v117,v118,v119,v120,v121,v122,v123,v124,v125,v126,v127;
  var v128,v129,v130,v131,v132,v133,v134,v135,v136,v137,v138,v139,v140,v141,v142,v143;
  var v144,v145,v146,v147,v148,v149,v150,v151,v152,v153,v154,v155,v156,v157,v158,v159;
  var v160,v161,v162,v163,v164,v165,v166,v167,v168,v169,v170,v171,v172,v173,v174,v175;
  var v176,v177,v178,v179,v180,v181,v182,v183,v184,v185,v186,v187,v188,v189,v190,v191;
  var v192,v193,v194,v195,v196,v197,v198,v199,v200,v201,v202,v203,v204,v205,v206,v207;
  var v208,v209,v210,v211,v212,v213,v214,v215,v216,v217,v218,v219,v220,v221,v222,v223;
  var v224,v225,v226,v227,v228,v229,v230,v231,v232,v233,v234,v235,v236,v237,v238,v239;
  var v240,v241,v242,v243,v244,v245,v246,v247,v248,v249,v250,v251,v252,v253,v254,v255;
  var v256,v257,v258,v259,v260;
  var rhs = { u: 'e5' };
  var acc = '';
  for (var i = 0; i < n; i++) acc += rhs.u;
  v000 = v260 = 1;
  return acc.length + '|' + (v000 + v260) + '|' + acc.charAt(2 * n - 1);
}
say('accumAtVeryHighLocalIndex', accumAtVeryHighLocalIndex(300));

/* ---- 9. a long run, to exercise many reallocations ---------------------- */

function longRun(n) {
  var s = '';
  for (var i = 0; i < n; i++) s += 'V';
  var o = { s: '' };
  for (var i = 0; i < n; i++) o.s += 'W';
  return s.length + '|' + o.s.length + '|' + s.charAt(n - 1) + o.s.charAt(n - 1) +
         '|' + s.charCodeAt(0) + '|' + (s.length === o.s.length);
}
say('longRun', longRun(70000));

print(out.join('\n'));
