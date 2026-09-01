// Differential corpus for patch 0038's INLINE PROPERTY BLOCK and anything that
// resizes it -- currently the QJS_OBJ_PAD probe (patch 0079) and the parked
// adaptive-capacity arm described in docs/adaptive-inline-block.md.
//
// Patch 0038 carves four JSProperty slots out of an object's own allocator
// block. `js_prop_grow` is the single place that knows about them, and it has
// three outcomes -- grow-and-stay, grow-and-move-out, and the shrink that
// `compact_properties` performs by calling it with a SMALLER byte count.
// Nothing else in this corpus drives all three densely, and any change to how
// that block is sized has to keep them correct:
//
//   A. An object must never be kept inline past the end of its block. If the
//      capacity the engine believes in exceeds the capacity it allocated, a
//      later property write scribbles on the next arena block and the symptom
//      is a CORRUPTED EARLIER PROPERTY, not a crash -- so every section below
//      reads the whole object back after every step rather than spot-checking.
//   B. The move-out memcpy must copy exactly the slots that exist. Copying
//      more reads past the end of the block; the extra bytes are then
//      overwritten by the caller, so this is invisible to output and is caught
//      only by AddressSanitizer (which forces JS_ARENA_LARGE_BLOCKS_ONLY so
//      every block is a real malloc). Run this file under ASan as well as
//      against node.
//   C. An object that has already moved out must not be dragged back inline by
//      a delete, and an object still inline must survive the shrink.
//
// MEASURED against the parked adaptive arm, 2026-08-02: this file is what
// kills a build whose stored capacity is too large (mutant M1) and a build
// whose stay-check ignores the per-object capacity (mutant M3); it does NOT
// kill the over-copying move-out (M2, ASan kills that one), and it correctly
// does not kill the two mutants that are legal-but-slower.
//
// Everything here is ES5, deterministic, and prints, so `node` is the oracle.
// It must be byte-identical from source AND through --via-bytecode.

function show(o) {
  var ks = Object.keys(o), i, out = [];
  for (i = 0; i < ks.length; i++) out.push(ks[i] + '=' + o[ks[i]]);
  return '{' + out.join(',') + '}';
}

// ---------------------------------------------------------------- A. growth
// Every literal arity 0..6, grown one property at a time to 8, reading back
// the whole object after every single step. A capacity that is one too large
// shows up as a corrupted earlier property, not as a crash, so the read-back
// has to be total rather than a spot check.
(function () {
  var lits = [
    function () { return {}; },
    function () { return { a: 1 }; },
    function () { return { a: 1, b: 2 }; },
    function () { return { a: 1, b: 2, c: 3 }; },
    function () { return { a: 1, b: 2, c: 3, d: 4 }; },
    function () { return { a: 1, b: 2, c: 3, d: 4, e: 5 }; },
    function () { return { a: 1, b: 2, c: 3, d: 4, e: 5, f: 6 }; }
  ];
  var names = ['p0', 'p1', 'p2', 'p3', 'p4', 'p5', 'p6', 'p7'];
  var i, j, k, o, line;
  for (i = 0; i < lits.length; i++) {
    line = [];
    // repeat so the shape's decaying birth/grow window is exercised at every
    // point of its cycle, including the wrap at 256
    for (k = 0; k < 300; k++) {
      o = lits[i]();
      for (j = 0; j < names.length; j++) {
        o[names[j]] = (j + 1) * 10 + i;
        if (k === 299) line.push(show(o));
      }
    }
    print('grow' + i + ' ' + line.join(' '));
  }
})();

// ------------------------------------------------- A'. constructor-built
// The population patch 0038 exists for. Two constructors that share nothing,
// one of which always grows past its birth shape and one of which never does,
// interleaved so the per-shape policy has to keep them apart.
(function () {
  function Small(x) { this.x = x; this.y = x + 1; }
  function Big(x) { this.x = x; this.y = x + 1; }
  var i, s, b, acc = 0, last = null;
  for (i = 0; i < 1000; i++) {
    s = new Small(i);
    b = new Big(i);
    b.z = i + 2;          // Big always outgrows its two-slot birth shape
    b.w = i + 3;
    b.v = i + 4;
    if (i === 17) { s.z = -1; }   // exactly one Small ever grows: must NOT
                                  // poison the other 999
    acc += s.x + s.y + b.x + b.y + b.z + b.w + b.v;
    last = [show(s), show(b)];
  }
  print('ctor ' + acc + ' ' + last[0] + ' ' + last[1]);
})();

// ------------------------------------------------- B. move-out fidelity
// Objects whose properties are heap values (strings, objects), so a memcpy
// that copies the wrong number of slots corrupts a REFERENCE, which the GC
// then follows. Held live in an array to defeat immediate reuse of the block.
(function () {
  var keep = [], i, o;
  for (i = 0; i < 400; i++) {
    o = { s: 'v' + i, t: { n: i } };
    o.u = 'u' + i;
    o.w = { m: i * 2 };
    o.x = 'x' + i;
    keep.push(o);
  }
  var sum = 0, sig = '';
  for (i = 0; i < keep.length; i++) {
    sum += keep[i].t.n + keep[i].w.m;
    if (i === 399) sig = keep[i].s + '|' + keep[i].u + '|' + keep[i].x;
  }
  print('moveout ' + sum + ' ' + sig);
})();

// ------------------------------------------------- C. delete and compaction
// compact_properties() runs through js_prop_grow with a SMALLER byte count.
// An object that is inline with cap 2 must stay inline; one that has moved out
// must not be dragged back in. Deleting then re-adding walks both.
(function () {
  var i, o, out = [];
  for (i = 0; i < 300; i++) {
    o = { a: 1, b: 2 };
    o.c = 3; o.d = 4; o.e = 5; o.f = 6;
    delete o.b; delete o.d; delete o.f;
    o.g = 7; o.h = 8;
    delete o.a;
    o.i = 9;
    if (i === 299) out.push(show(o));
  }
  print('compact ' + out.join(' '));
})();

// ------------------------------------------- shape sharing across capacity
// Two objects reach the same shape by different routes, one of which was born
// with an exact-fit block and one with headroom. They must remain
// indistinguishable: same key order, same descriptors, same JSON.
(function () {
  var i, a, b, sig = '';
  for (i = 0; i < 300; i++) {
    a = { p: 1, q: 2 };
    a.r = 3;
    b = { p: 1, q: 2, r: 3 };
    if (i === 299) {
      sig = JSON.stringify(a) + '|' + JSON.stringify(b) + '|' +
            Object.keys(a).join(',') + '|' + Object.keys(b).join(',');
    }
  }
  print('shared ' + sig);
})();

// ------------------------------------------- descriptors survive the move
(function () {
  var i, o, d, out = '';
  for (i = 0; i < 300; i++) {
    o = { a: 1 };
    Object.defineProperty(o, 'b', { value: 2, enumerable: false, writable: false, configurable: true });
    Object.defineProperty(o, 'c', { get: function () { return 3; }, enumerable: true, configurable: true });
    o.d = 4; o.e = 5; o.f = 6;
    if (i === 299) {
      d = Object.getOwnPropertyDescriptor(o, 'b');
      out = show(o) + '|' + d.value + ',' + d.enumerable + ',' + d.writable + ',' + d.configurable +
            '|' + o.c + '|' + Object.getOwnPropertyNames(o).join(',');
    }
  }
  print('descr ' + out);
})();

// ------------------------------------------- seal / freeze / preventExtensions
(function () {
  var i, o, out = '';
  for (i = 0; i < 300; i++) {
    o = { a: 1, b: 2 };
    o.c = 3;
    Object.seal(o);
    try { o.d = 4; } catch (e) { /* sloppy mode: silent */ }
    if (i === 299) out = show(o) + '|' + Object.isSealed(o) + '|' + Object.isFrozen(o);
  }
  print('seal ' + out);
})();

// ------------------------------------------- spread / assign / for-in
(function () {
  var i, src, dst, ks, out = '';
  for (i = 0; i < 300; i++) {
    src = { a: 1, b: 2 };
    src.c = 3; src.d = 4; src.e = 5;
    dst = {};
    for (var k in src) dst[k] = src[k];
    var asn = Object.assign({ z: 0 }, src);
    if (i === 299) {
      ks = [];
      for (var k2 in asn) ks.push(k2);
      out = show(dst) + '|' + show(asn) + '|' + ks.join(',');
    }
  }
  print('copy ' + out);
})();

// ------------------------------------------- prototype objects
// is_prototype objects take a different arm in add_property; they must not be
// sized differently in a way that breaks the prototype chain.
(function () {
  function Base() { this.a = 1; }
  Base.prototype.m1 = function () { return 1; };
  Base.prototype.m2 = function () { return 2; };
  Base.prototype.m3 = function () { return 3; };
  Base.prototype.m4 = function () { return 4; };
  Base.prototype.m5 = function () { return 5; };
  var i, o, s = 0;
  for (i = 0; i < 500; i++) { o = new Base(); s += o.m1() + o.m2() + o.m3() + o.m4() + o.m5() + o.a; }
  print('proto ' + s);
})();
