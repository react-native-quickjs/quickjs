/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * The primitives React Native's renderer is actually made of.
 *
 * Nothing here is a "program". Each entry is one interpreter operation isolated
 * far enough that its cost is readable: an own-property enumeration, a dynamic
 * keyed store, a keyed load out of a large object, a property read at a site
 * with 1, 2 or 5 receiver shapes. These are the operations RN executes millions
 * of times per second during a scroll, and measured against V8 they are exactly
 * where interpreters lose the most ground — so they are where an interpreter
 * has the most to gain.
 *
 * Why these specific shapes:
 *
 *   - 7 keys is the median prop count of an RN host component. The prop diff
 *     that runs on every element on every update does TWO for-in passes over an
 *     object of roughly that size, so `for-in` is multiplied by 2x the element
 *     count before anything is drawn.
 *
 *   - 187 and 111 keys are the sizes of real config/style objects that sit on
 *     the hot path. They are past every engine's "small object" representation,
 *     which is what makes the keyed load interesting rather than trivial.
 *
 *   - The mono/poly/mega ladder is the same read site fed 1, 2 and 5 shapes.
 *     The spread between the three rungs, not any single rung, is the number
 *     that describes the property-lookup design.
 *
 * Several bodies use an inner repeat loop. That is deliberate: a single
 * property read costs a few nanoseconds and would otherwise be measuring the
 * harness's own call and blackhole overhead rather than the operation. The
 * repeat count is in the `unit` field so per-op cost is recoverable.
 */

/* Deterministic PRNG. Math.random would make `expect` unusable, and `expect`
   is the only thing proving both engines computed the same work. */
function mulberry32(seed) {
  var a = seed >>> 0;
  return function () {
    a = (a + 0x6d2b79f5) >>> 0;
    var t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

/* ------------------------------------------------------------------ for-in */

/*
 * RN's prop diff (ReactNativeAttributePayload.diffProperties) walks the
 * previous props with for-in to find removals, then the next props with for-in
 * to find changes. Two enumerations per element per update. Hermes measured
 * 12.7x slower than V8 here — the single worst ratio among the ordinary
 * operations, and the one with the highest multiplier in real RN work.
 */
var FORIN_OBJ = {
  width: 100,
  height: 200,
  opacity: 1,
  borderRadius: 8,
  flexGrow: 1,
  marginTop: 12,
  zIndex: 3,
};

bench({
  name: 'prim/for-in-7key',
  unit: 'enumeration',
  run: function () {
    var s = 0;
    for (var k in FORIN_OBJ) s += FORIN_OBJ[k];
    return s;
  },
  expect: 325,
});

/* The diff as RN actually runs it: two passes, keyed load on each visit. */
bench({
  name: 'prim/for-in-diff-2pass',
  unit: 'diff',
  run: function () {
    var prev = FORIN_OBJ;
    var next = {
      width: 100,
      height: 200,
      opacity: 1,
      borderRadius: 8,
      flexGrow: 1,
      marginTop: 12,
      zIndex: 4,
    };
    var changed = 0;
    /* Pass 1: keys removed in next. */
    for (var k in prev) {
      if (next[k] === undefined) changed++;
    }
    /* Pass 2: keys added or changed. */
    for (var j in next) {
      if (next[j] !== prev[j]) changed++;
    }
    return changed;
  },
  expect: 1,
});

/* --------------------------------------------------------- dynamic stores */

/*
 * Building an object one computed key at a time. This is what every
 * `payload[key] = value` in the RN bridge serializer does, and it is 4.6x
 * slower than V8 on Hermes even in the easy case where the keys always arrive
 * in the same order.
 */
var STORE_KEYS = ['width', 'height', 'opacity', 'borderRadius', 'flexGrow', 'marginTop', 'zIndex'];
var STORE_VALS = [100, 200, 1, 8, 1, 12, 3];

/*
 * All three object-construction rows below return the object itself rather
 * than a number derived from it. That is not incidental: Hermes at -O will
 * scalar-replace an object whose fields are only read back into a sum, delete
 * the allocation entirely, and report ~10 ns for what should be a heap
 * allocation. Returning the object forces it to be materialized on both
 * engines, which is the only way the three rows are comparable.
 */
bench({
  name: 'prim/dyn-store-7key',
  unit: 'object',
  run: function () {
    var o = {};
    for (var i = 0; i < 7; i++) o[STORE_KEYS[i]] = STORE_VALS[i];
    return o;
  },
  expect: function (o) {
    return o.width === 100 && o.zIndex === 3;
  },
});

/*
 * The same stores in a different order every time.
 *
 * Engines that build a hidden-class/shape tree get one transition chain per
 * insertion order, so randomizing the order explodes the tree and turns every
 * store into a miss. Measured at 6.8x on Hermes versus 4.6x for the ordered
 * case — a ~50% penalty purely for the order the keys arrived in. RN hits this
 * for real whenever props come from an object built by user code or parsed
 * from JSON.
 *
 * The permutations are precomputed from a seeded PRNG so the work is identical
 * on both engines, and the returned value is order-independent (a sum), so
 * `expect` holds no matter which permutation a given iteration lands on.
 */
var PERMS = [];
var PERM_I = 0;

bench({
  name: 'prim/dyn-store-7key-shuffled',
  unit: 'object',
  setup: function () {
    var rnd = mulberry32(0x5eed);
    PERMS = [];
    for (var p = 0; p < 64; p++) {
      var idx = [0, 1, 2, 3, 4, 5, 6];
      /* Fisher-Yates with a seeded source. */
      for (var i = idx.length - 1; i > 0; i--) {
        var j = (rnd() * (i + 1)) | 0;
        var t = idx[i];
        idx[i] = idx[j];
        idx[j] = t;
      }
      PERMS.push(idx);
    }
    PERM_I = 0;
  },
  run: function () {
    var perm = PERMS[PERM_I++ & 63];
    var o = {};
    for (var i = 0; i < 7; i++) {
      var k = perm[i];
      o[STORE_KEYS[k]] = STORE_VALS[k];
    }
    return o;
  },
  expect: function (o) {
    return (
      o.width + o.height + o.opacity + o.borderRadius + o.flexGrow + o.marginTop + o.zIndex === 325
    );
  },
});

/*
 * The fast baseline. Same seven keys, same seven values, written as a literal
 * so the engine knows the shape at compile time. The gap between this and
 * `dyn-store-7key` is the entire cost of not knowing the keys statically, and
 * it is the number that says whether a "build the props object literally"
 * optimization in RN would pay for itself.
 */
bench({
  name: 'prim/object-literal-7key',
  unit: 'object',
  run: function () {
    var o = {
      width: 100,
      height: 200,
      opacity: 1,
      borderRadius: 8,
      flexGrow: 1,
      marginTop: 12,
      zIndex: 3,
    };
    return o;
  },
  expect: function (o) {
    return o.width === 100 && o.zIndex === 3;
  },
});

/* ------------------------------------------------------------- keyed loads */

/*
 * Reading out of a large object. 187 keys is the size of a real RN
 * platform-constants / view-config object; 111 is a large stylesheet. Both are
 * well past the point where an engine switches representation, and the ratio
 * between them shows whether lookup cost tracks object size (a scan or a
 * badly-sized hash) or not (a proper hash).
 *
 * Twenty reads spread across the whole key range per unit, so the measurement
 * is a lookup rather than one cache hit repeated.
 */
function makeConfig(n, seed) {
  var rnd = mulberry32(seed);
  var o = {};
  for (var i = 0; i < n; i++) o['config_key_' + i] = (rnd() * 1000) | 0;
  return o;
}

var CFG187 = null;
var CFG187_KEYS = [];
var CFG111 = null;
var CFG111_KEYS = [];

bench({
  name: 'prim/keyed-load-187key',
  unit: '20 loads',
  setup: function () {
    CFG187 = makeConfig(187, 0xc0ffee);
    CFG187_KEYS = [];
    for (var i = 0; i < 20; i++) CFG187_KEYS.push('config_key_' + ((i * 9 + 3) % 187));
  },
  run: function () {
    var s = 0;
    for (var i = 0; i < 20; i++) s += CFG187[CFG187_KEYS[i]];
    return s;
  },
  expect: 11204,
});

bench({
  name: 'prim/keyed-load-111key',
  unit: '20 loads',
  setup: function () {
    CFG111 = makeConfig(111, 0xc0ffee);
    CFG111_KEYS = [];
    for (var i = 0; i < 20; i++) CFG111_KEYS.push('config_key_' + ((i * 5 + 3) % 111));
  },
  run: function () {
    var s = 0;
    for (var i = 0; i < 20; i++) s += CFG111[CFG111_KEYS[i]];
    return s;
  },
  expect: 11402,
});

/* Same 20 loads out of a 7-key object, as the small-object control. If this is
   much cheaper than the 111/187 rows, lookup cost tracks object size. */
bench({
  name: 'prim/keyed-load-7key',
  unit: '20 loads',
  run: function () {
    var s = 0;
    for (var i = 0; i < 20; i++) s += FORIN_OBJ[STORE_KEYS[i % 7]];
    return s;
  },
  expect: 972,
});

/* ---------------------------------------------------- mono / poly / mega */

/*
 * The shape-lookup ladder.
 *
 * Three benchmarks, each with exactly ONE property read site (`readX`), fed
 * receivers of 1, 2 and 5 distinct shapes. Everything else — the loop, the
 * array indexing, the call — is identical between the three, so the difference
 * between the rows is purely the cost of the site having to cope with more
 * shapes.
 *
 * This matters for RN because a single generic helper (`getStyle(node)`,
 * `props.style`) sees every component type in the app, which is the megamorphic
 * case by construction. If the ladder is flat, generic helpers are free; if it
 * is steep, they are the thing to specialize.
 */
function readX(o) {
  return o.x;
}

/* Five shapes that all carry `x`, but at different offsets and with different
   sibling keys, so no two can share a shape. */
function ShapeA(x) {
  this.x = x;
}
function ShapeB(x) {
  this.b0 = 1;
  this.x = x;
}
function ShapeC(x) {
  this.c0 = 1;
  this.c1 = 2;
  this.x = x;
}
function ShapeD(x) {
  this.d0 = 1;
  this.d1 = 2;
  this.d2 = 3;
  this.x = x;
}
function ShapeE(x) {
  this.e0 = 1;
  this.e1 = 2;
  this.e2 = 3;
  this.e3 = 4;
  this.x = x;
}

var MONO = [];
var POLY = [];
var MEGA = [];

function fillReceivers() {
  MONO = [];
  POLY = [];
  MEGA = [];
  var ctors = [ShapeA, ShapeB, ShapeC, ShapeD, ShapeE];
  for (var i = 0; i < 100; i++) {
    MONO.push(new ShapeA(i));
    POLY.push(i & 1 ? new ShapeB(i) : new ShapeA(i));
    MEGA.push(new ctors[i % 5](i));
  }
}

bench({
  name: 'prim/read-mono',
  unit: '100 reads',
  setup: fillReceivers,
  run: function () {
    var s = 0;
    for (var i = 0; i < 100; i++) s += readX(MONO[i]);
    return s;
  },
  expect: 4950,
});

bench({
  name: 'prim/read-poly2',
  unit: '100 reads',
  setup: fillReceivers,
  run: function () {
    var s = 0;
    for (var i = 0; i < 100; i++) s += readX(POLY[i]);
    return s;
  },
  expect: 4950,
});

bench({
  name: 'prim/read-mega5',
  unit: '100 reads',
  setup: fillReceivers,
  run: function () {
    var s = 0;
    for (var i = 0; i < 100; i++) s += readX(MEGA[i]);
    return s;
  },
  expect: 4950,
});

/* ---------------------------------------------------------------- closures */

/*
 * Closure allocation capturing two variables. Every `useCallback`, every event
 * handler in a render, every `.map(item => ...)` allocates one of these. React
 * renders allocate them by the thousand, and they are pure overhead: the
 * closure usually outlives the render by microseconds.
 */
/*
 * Same hazard as the object rows: a closure that is allocated and immediately
 * invoked in the same function gets inlined and deleted, which measures
 * nothing. The closures are parked in a global array first, so the allocation
 * has to happen and the environment has to be real.
 */
var HANDLERS = [];

/* A factory, because `var` in a loop body is function-scoped: writing the
   closure inline would give all ten closures ONE shared environment and
   measure a single allocation instead of ten. */
function makeHandler(a, b) {
  return function () {
    return a + b;
  };
}

bench({
  name: 'prim/closure-alloc-noinvoke',
  unit: '10 closures',
  setup: function () {
    HANDLERS = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
  },
  run: function () {
    for (var i = 0; i < 10; i++) HANDLERS[i] = makeHandler(i, i + 1);
    return HANDLERS;
  },
  expect: function (h) {
    return h.length === 10 && typeof h[9] === 'function';
  },
});

/* The same allocation, then invoked indirectly through the array — the cost of
   a handler that is actually used. The difference between the two rows is the
   call, not the allocation. */
bench({
  name: 'prim/closure-alloc-2capture',
  unit: '10 closures',
  setup: function () {
    HANDLERS = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
  },
  run: function () {
    for (var i = 0; i < 10; i++) HANDLERS[i] = makeHandler(i, i + 1);
    var s = 0;
    for (var j = 0; j < 10; j++) s += HANDLERS[j]();
    return s;
  },
  expect: 100,
});

/* -------------------------------------------------------------- Object.keys */

/*
 * The other way RN walks props. `Object.keys(props)` allocates an array where
 * for-in does not, so the two rows together say which enumeration form to
 * prefer in the diff path.
 */
bench({
  name: 'prim/object-keys-7key',
  unit: 'call',
  run: function () {
    var ks = Object.keys(FORIN_OBJ);
    var s = 0;
    for (var i = 0; i < ks.length; i++) s += FORIN_OBJ[ks[i]];
    return s;
  },
  expect: 325,
});

/* Keys without the walk, isolating the array allocation itself. */
bench({
  name: 'prim/object-keys-7key-alloc',
  unit: 'call',
  run: function () {
    return Object.keys(FORIN_OBJ).length;
  },
  expect: 7,
});

/* ---------------------------------------------------------------- in / has */

/*
 * `hasOwnProperty` and `in` are the guards that surround every one of the
 * reads above in real RN code, and they are frequently the more expensive half
 * of the pair.
 */
bench({
  name: 'prim/hasOwnProperty-7key',
  unit: '7 checks',
  run: function () {
    var n = 0;
    for (var i = 0; i < 7; i++) {
      if (Object.prototype.hasOwnProperty.call(FORIN_OBJ, STORE_KEYS[i])) n++;
    }
    return n;
  },
  expect: 7,
});

bench({
  name: 'prim/in-operator-7key',
  unit: '7 checks',
  run: function () {
    var n = 0;
    for (var i = 0; i < 7; i++) {
      if (STORE_KEYS[i] in FORIN_OBJ) n++;
    }
    return n;
  },
  expect: 7,
});
