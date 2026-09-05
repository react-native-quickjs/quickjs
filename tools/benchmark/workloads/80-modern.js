/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * The modern-JavaScript surface a React Native app ships and this suite did
 * not previously cover.
 *
 * WHY THIS FILE EXISTS. The suite before it covered React bookkeeping, native
 * prop payloads, data/JSON, strings and a general ECMAScript floor. Four things
 * that every shipped RN bundle contains had NO row anywhere:
 *
 *   1. Class hierarchies. Every RN component written before hooks, every
 *      Animated node, every Error subclass, and everything Babel lowers from
 *      `class` when the target supports it natively.
 *   2. Promise and microtask traffic. The bridge, `fetch`, `AsyncStorage`,
 *      Suspense and every `await` in application code. `70-suspense.js` covers
 *      throw-based control flow, not the job queue.
 *   3. Iteration protocol: `for...of`, spread, destructuring, generators.
 *      Metro's output is full of it and it is much more expensive than the
 *      indexed loops the rest of the suite measures.
 *   4. Module registry init. Metro emits `__d(factory, id, deps)` and `__r(id)`;
 *      startup is several hundred factory invocations, each defining functions
 *      and an exports object. `bench/startup-bundle.mjs` measures this on a real
 *      bundle from outside the process, but there was no in-suite row for it, so
 *      no A/B could see it.
 *
 * Every row returns a SCALAR checksum rather than an object, so that a
 * cross-build differential can compare the printed `result=` value directly and
 * not just `[object Object]`. That is what makes this file usable by
 * bench/nonoctane/instr.mjs as a correctness check as well as a workload.
 *
 * TWO WARNINGS ABOUT THE ASYNC ROWS, both MEASURED 2026-08-01 and neither
 * obvious from reading them.
 *
 *   (a) THEY ARE NOT USABLE UNDER THE TIMING PRELUDE. Jobs queue up and drain
 *       only after the script ends, so `bench/run.mjs` reports run-to-run
 *       spreads of 477%, 474% and 2370% on `promise/then-chain-50`,
 *       `promise/all-20` and `promise/async-await-depth-10`. Those numbers are
 *       not measurements of anything. Score these rows with
 *       `bench/nonoctane/instr.mjs`, which counts instructions under fixed work.
 *   (b) THEIR PER-ITERATION COST IS NOT CONSTANT IN N, for the same reason: the
 *       queue and its heap grow with the iteration count.
 *       `promise/then-chain-50` measures 1.076 M instructions per iteration
 *       differenced at n=100..200 and 1.270 M at n=400..800, +18%. Comparing two
 *       ENGINE BUILDS at the same pinned N is still valid and that is what the
 *       harness does; comparing two different Ns is not. Three non-async rows
 *       checked the same way (`iter/destructure-props-100`,
 *       `module/registry-init-120`, `rnprops/commit-20-rows`) are linear in N to
 *       within 0.5%.
 *
 * ASYNC ROWS AND WHAT THEIR `expect` GATE ACTUALLY COVERS. QuickJS and Hermes
 * both drain the job queue after the script finishes, so a promise created
 * inside `run()` cannot have settled by the time `report()` prints. The async
 * rows therefore gate on the SYNCHRONOUS half (chains constructed, callbacks
 * registered) and the settlement work is counted by the process-level
 * instrument but not verified by the row's own `expect`. This is stated rather
 * than hidden: an engine that silently dropped every job would still pass these
 * rows' expect gates, though it would show a large instruction drop.
 *
 * Deliberately avoids: private class fields, optional chaining, `??`,
 * `Object.fromEntries`, and anything else where the two engines' support has
 * historically differed. A row that fails to parse on one engine makes the
 * comparison a comparison of parsers.
 */

/* ------------------------------------------------------------- classes */

class Node2 {
  constructor(id) {
    this.id = id;
    this.children = null;
    this.dirty = false;
  }
  get key() {
    return this.id * 2;
  }
  measure() {
    return this.id & 15;
  }
  toString() {
    return 'Node2#' + this.id;
  }
}

class ViewNode extends Node2 {
  constructor(id, style) {
    super(id);
    this.style = style;
    this.layout = 0;
  }
  measure() {
    return super.measure() + this.style;
  }
}

class TextNode extends ViewNode {
  constructor(id, style, text) {
    super(id, style);
    this.text = text;
  }
  measure() {
    return super.measure() + this.text.length;
  }
}

class ImageNode extends ViewNode {
  measure() {
    return super.measure() + 7;
  }
}

bench({
  name: 'class/construct-3level-100',
  unit: 'construction',
  run: function () {
    var sum = 0;
    for (var i = 0; i < 100; i++) {
      var n = new TextNode(i, i & 3, 'abcdefgh');
      sum += n.id + n.style + n.layout;
    }
    return sum;
  },
  expect: 5100,
});

var NODES = null;

/* Every row builds its own fixture: bench/nonoctane/instr.mjs runs ONE row per
   process, so a row that depended on a previous row's setup would measure an
   empty loop there and a full one under bench/run.mjs. */
function makeNodes() {
  var out = [];
  for (var i = 0; i < 300; i++) {
    out.push(
      i % 3 === 0
        ? new ViewNode(i, 1)
        : i % 3 === 1
          ? new TextNode(i, 1, 'abcd')
          : new ImageNode(i, 1)
    );
  }
  return out;
}

bench({
  name: 'class/virtual-call-poly3-300',
  unit: 'call',
  setup: function () {
    NODES = makeNodes();
  },
  run: function () {
    var sum = 0;
    for (var i = 0; i < NODES.length; i++) sum += NODES[i].measure();
    return sum;
  },
  expect: 3626,
});

bench({
  name: 'class/getter-read-300',
  unit: 'read',
  setup: function () {
    NODES = makeNodes();
  },
  run: function () {
    var sum = 0;
    for (var i = 0; i < NODES.length; i++) sum += NODES[i].key;
    return sum;
  },
  expect: 89700,
});

bench({
  name: 'class/instanceof-chain-300',
  unit: 'test',
  setup: function () {
    NODES = makeNodes();
  },
  run: function () {
    var n = 0;
    for (var i = 0; i < NODES.length; i++) {
      var x = NODES[i];
      if (x instanceof TextNode) n += 1;
      else if (x instanceof ImageNode) n += 2;
      else if (x instanceof Node2) n += 4;
    }
    return n;
  },
  expect: 700,
});

/* --------------------------------------------------- promises/microtasks */

bench({
  name: 'promise/then-chain-50',
  unit: 'chain',
  run: function () {
    var made = 0;
    for (var i = 0; i < 50; i++) {
      Promise.resolve(i)
        .then(function (v) {
          return v + 1;
        })
        .then(function (v) {
          return v * 2;
        });
      made++;
    }
    return made;
  },
  expect: 50,
});

bench({
  name: 'promise/all-20',
  unit: 'all',
  run: function () {
    var ps = [];
    for (var i = 0; i < 20; i++) ps.push(Promise.resolve(i));
    Promise.all(ps).then(function (vs) {
      return vs.length;
    });
    return ps.length;
  },
  expect: 20,
});

bench({
  name: 'promise/async-await-depth-10',
  unit: 'chain',
  setup: function () {
    ASYNC_STEP = async function (n) {
      if (n === 0) return 0;
      var v = await ASYNC_STEP(n - 1);
      return v + n;
    };
  },
  run: function () {
    ASYNC_STEP(10);
    return 10;
  },
  expect: 10,
});

var ASYNC_STEP = null;

/* ----------------------------------------------------- iteration protocol */

bench({
  name: 'iter/for-of-array-500',
  unit: 'iteration',
  setup: function () {
    ARR500 = [];
    for (var i = 0; i < 500; i++) ARR500.push(i);
  },
  run: function () {
    var sum = 0;
    for (const v of ARR500) sum += v;
    return sum;
  },
  expect: 124750,
});

var ARR500 = null;

bench({
  name: 'iter/spread-args-100',
  unit: 'spread',
  setup: function () {
    SPREAD_SINK = function (a, b, c, d) {
      return (a | 0) + (b | 0) + (c | 0) + (d | 0);
    };
    SMALL = [1, 2, 3, 4];
  },
  run: function () {
    var sum = 0;
    for (var i = 0; i < 100; i++) sum += SPREAD_SINK(...SMALL);
    return sum;
  },
  expect: 1000,
});

var SPREAD_SINK = null;
var SMALL = null;

bench({
  name: 'iter/destructure-props-100',
  unit: 'destructure',
  setup: function () {
    PROPS = { width: 3, height: 4, color: 'red', opacity: 1, flex: 2 };
  },
  run: function () {
    var sum = 0;
    for (var i = 0; i < 100; i++) {
      const { width, height, flex } = PROPS;
      sum += width + height + flex;
    }
    return sum;
  },
  expect: 900,
});

var PROPS = null;

bench({
  name: 'iter/array-spread-clone-100',
  unit: 'clone',
  setup: function () {
    ARR500 = [];
    for (var i = 0; i < 500; i++) ARR500.push(i);
  },
  run: function () {
    var out = [...ARR500];
    return out.length;
  },
  expect: 500,
});

bench({
  name: 'iter/generator-drain-100',
  unit: 'yield',
  setup: function () {
    GEN = function* (n) {
      for (var i = 0; i < n; i++) yield i;
    };
  },
  run: function () {
    var sum = 0;
    for (const v of GEN(100)) sum += v;
    return sum;
  },
  expect: 4950,
});

var GEN = null;

/* ------------------------------------------------------------ Map and Set */

bench({
  name: 'coll/set-dedupe-500',
  unit: 'dedupe',
  run: function () {
    var s = new Set();
    for (var i = 0; i < 500; i++) s.add('k' + (i & 255));
    return s.size;
  },
  expect: 256,
});

bench({
  name: 'coll/map-churn-500',
  unit: 'op',
  run: function () {
    var m = new Map();
    for (var i = 0; i < 500; i++) m.set('k' + i, i);
    for (var j = 0; j < 500; j += 2) m.delete('k' + j);
    return m.size;
  },
  expect: 250,
});

/* --------------------------------------------------- module registry init */

/*
 * Metro's runtime, reduced. `__d` registers a factory; `__r` runs it once and
 * memoises the exports. The shape that matters is what a factory DOES: create
 * an exports object, define a handful of closures on it, and require its
 * dependencies. 120 modules with a 3-deep dependency chain models the entry
 * cascade of a small screen; a real bundle's entry cascade is ~73 factories
 * (bench/startup-bundle.mjs), so this is the same order.
 */
bench({
  name: 'module/registry-init-120',
  unit: 'cascade',
  setup: function () {
    MODULE_DEFS = [];
    for (var i = 0; i < 120; i++) {
      MODULE_DEFS.push({
        id: i,
        deps: i > 2 ? [i - 1, i - 2, i - 3] : [],
      });
    }
  },
  run: function () {
    var modules = {};
    function __d(factory, id, deps) {
      modules[id] = { factory: factory, deps: deps, exports: null, done: false };
    }
    function __r(id) {
      var m = modules[id];
      if (m.done) return m.exports;
      m.done = true;
      var exp = {};
      m.factory(__r, m.deps, exp);
      m.exports = exp;
      return exp;
    }
    for (var i = 0; i < MODULE_DEFS.length; i++) {
      (function (def) {
        __d(
          function (req, deps, exports) {
            var acc = def.id;
            for (var k = 0; k < deps.length; k++) acc += req(deps[k]).value;
            exports.value = acc & 1023;
            exports.render = function (x) {
              return x + acc;
            };
            exports.compare = function (a, b) {
              return a === b;
            };
          },
          def.id,
          def.deps
        );
      })(MODULE_DEFS[i]);
    }
    return __r(119).value;
  },
  expect: 285,
});

var MODULE_DEFS = null;
