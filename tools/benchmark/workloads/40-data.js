/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * Data and collection work — the second half of what an RN app actually
 * spends its JS time on, after property access.
 *
 * The payload is a social feed, because that is the shape almost every RN app
 * moves across the bridge: a list of items, each with a nested author object,
 * a short text body, an ISO timestamp, some counters, a media descriptor and
 * a small tag array. 423 bytes per item as serialized, so the 20-item variant
 * is 8.5 KB (one screen) and the 100-item variant 42 KB (a page fetch). Those are the two
 * sizes that show up in a real network response, and JSON.parse of them sits
 * directly between the user and the first frame.
 *
 * Everything is generated from a seeded PRNG so both engines serialize
 * byte-identical bytes and `expect` is meaningful. All numeric fields are
 * integers on purpose: a fractional double would make the comparison partly a
 * test of each engine's dtoa, which is measured separately in 50-strings.
 *
 * RESEARCH FLAG carried into this file: Array.prototype.sort with a JS
 * comparator was Hermes's single worst measured operation against V8 (18x).
 * Both sort rows below exist to answer whether QuickJS shares that weakness.
 */

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

var WORDS = [
  'shipping', 'the', 'new', 'renderer', 'today', 'after', 'months', 'of', 'profiling',
  'startup', 'paths', 'across', 'devices', 'and', 'it', 'finally', 'holds', 'sixty',
  'frames', 'under', 'load',
];

/* ~90 characters of text, deterministic from the item index. */
function makeText(rnd) {
  var s = '';
  while (s.length < 90) {
    s += (s.length ? ' ' : '') + WORDS[(rnd() * WORDS.length) | 0];
  }
  return s.slice(0, 90);
}

function makeFeed(n, seed) {
  var rnd = mulberry32(seed);
  var out = [];
  for (var i = 0; i < n; i++) {
    var uid = 1000 + ((rnd() * 9000) | 0);
    out.push({
      id: 'post_' + (100000 + i),
      author: {
        id: uid,
        name: 'Person ' + uid,
        handle: '@p' + uid,
        avatar: '/a/' + uid + '_128.jpg',
        verified: (uid & 7) === 0,
      },
      text: makeText(rnd),
      createdAt: '2024-03-' + (10 + (i % 20)) + 'T08:22:31.000Z',
      likes: (rnd() * 50000) | 0,
      comments: (rnd() * 900) | 0,
      shares: (rnd() * 300) | 0,
      media: {
        type: 'image',
        url: '/m/' + (200000 + i) + '_lg.webp',
        width: 1080,
        height: 1350,
      },
      tags: ['t' + (i % 17), 'p' + (i % 5), 'r' + (i % 3)],
      pinned: i === 0,
    });
  }
  return out;
}

/* ------------------------------------------------------------------- JSON */

var FEED20 = null;
var FEED20_JSON = '';
var FEED100 = null;
var FEED100_JSON = '';

/*
 * Parse. The returned object is handed back whole rather than reduced to a
 * number, so no engine may narrow the parse to only the fields we read.
 */
bench({
  name: 'data/json-parse-8.5kb-20items',
  unit: 'parse',
  setup: function () {
    FEED20 = makeFeed(20, 0xfeed01);
    FEED20_JSON = JSON.stringify(FEED20);
  },
  run: function () {
    return JSON.parse(FEED20_JSON);
  },
  expect: function (v) {
    return v.length === 20 && v[19].media.width === 1080 && v[0].tags.length === 3;
  },
});

bench({
  name: 'data/json-parse-42kb-100items',
  unit: 'parse',
  setup: function () {
    FEED100 = makeFeed(100, 0xfeed01);
    FEED100_JSON = JSON.stringify(FEED100);
  },
  run: function () {
    return JSON.parse(FEED100_JSON);
  },
  expect: function (v) {
    return v.length === 100 && v[99].media.width === 1080;
  },
});

/*
 * Stringify. Returning the length forces the string to be fully built while
 * keeping `expect` a cheap identity check — and it doubles as a differential
 * test that both engines serialize the same bytes.
 */
bench({
  name: 'data/json-stringify-8.5kb-20items',
  unit: 'stringify',
  setup: function () {
    FEED20 = makeFeed(20, 0xfeed01);
  },
  run: function () {
    return JSON.stringify(FEED20).length;
  },
  expect: 8450,
});

bench({
  name: 'data/json-stringify-42kb-100items',
  unit: 'stringify',
  setup: function () {
    FEED100 = makeFeed(100, 0xfeed01);
  },
  run: function () {
    return JSON.stringify(FEED100).length;
  },
  expect: 42262,
});

/*
 * Deep nesting, which exercises the recursion in the serializer rather than
 * its throughput. Redux state trees and navigation state reach this shape.
 */
var DEEP = null;

bench({
  name: 'data/json-stringify-deep-32',
  unit: 'stringify',
  setup: function () {
    var node = { depth: 32, name: 'leaf', value: 0, children: [] };
    for (var d = 31; d >= 0; d--) {
      node = { depth: d, name: 'node_' + d, value: d * 7, children: [node] };
    }
    DEEP = node;
  },
  run: function () {
    return JSON.stringify(DEEP).length;
  },
  expect: 1773,
});

/* A flat array of numbers, which is the pure number-to-string path with none
   of the key/quote/escape work of the feed rows. */
var NUMS1000 = null;

bench({
  name: 'data/json-stringify-1000-numbers',
  unit: 'stringify',
  setup: function () {
    var rnd = mulberry32(0xabc);
    NUMS1000 = [];
    for (var i = 0; i < 1000; i++) NUMS1000.push((rnd() * 1000000) | 0);
  },
  run: function () {
    return JSON.stringify(NUMS1000).length;
  },
  expect: 6904,
});

/* ------------------------------------------------------------------- sort */

/*
 * RESEARCH FLAG. Hermes measured 18x slower than V8 on sort-with-comparator —
 * its worst outlier by a wide margin, because every comparison is a full JS
 * call out of the sort's C++ loop. Whether QuickJS pays the same is a
 * first-order question for this project: RN sorts on every list reorder,
 * every "sort by name", every leaderboard.
 *
 * Both rows copy the array first because sort mutates in place. The copy is
 * charged to both engines identically, and `array-slice-1000` below measures
 * exactly that copy so it can be subtracted.
 */
var SORT_NUMS = null;
var SORT_OBJS = null;

bench({
  name: 'data/sort-1000-numbers',
  unit: 'sort',
  setup: function () {
    var rnd = mulberry32(0x50f7);
    SORT_NUMS = [];
    for (var i = 0; i < 1000; i++) SORT_NUMS.push((rnd() * 1000000) | 0);
  },
  run: function () {
    var a = SORT_NUMS.slice();
    a.sort(function (x, y) {
      return x - y;
    });
    return a[0] + a[999];
  },
  expect: 998036,
});

bench({
  name: 'data/sort-1000-objects-by-string',
  unit: 'sort',
  setup: function () {
    var rnd = mulberry32(0x50f8);
    SORT_OBJS = [];
    for (var i = 0; i < 1000; i++) {
      SORT_OBJS.push({ id: i, name: 'user_' + (100000 + ((rnd() * 900000) | 0)), score: i });
    }
  },
  run: function () {
    var a = SORT_OBJS.slice();
    a.sort(function (x, y) {
      return x.name < y.name ? -1 : x.name > y.name ? 1 : 0;
    });
    return a[0].name;
  },
  expect: 'user_100977',
});

/* The copy on its own — subtract this from the two rows above to get the cost
   of the sort proper. */
bench({
  name: 'data/array-slice-1000',
  unit: 'copy',
  run: function () {
    return SORT_NUMS.slice();
  },
  expect: function (a) {
    return a.length === 1000;
  },
});

/* ------------------------------------------------------ iteration methods */

/*
 * map / filter / reduce / forEach against a plain `for` loop over the same
 * 100 elements. Every one of these allocates a closure per call site and
 * performs 100 JS calls where the for loop performs none, which is precisely
 * the tradeoff RN code makes hundreds of times per render.
 */
var ITEMS100 = null;

function makeItems() {
  var rnd = mulberry32(0x17e5);
  ITEMS100 = [];
  for (var i = 0; i < 100; i++) {
    ITEMS100.push({ id: i, score: (rnd() * 1000) | 0, active: (i & 3) !== 0 });
  }
}

bench({
  name: 'data/for-loop-100',
  unit: 'pass',
  setup: makeItems,
  run: function () {
    var s = 0;
    for (var i = 0; i < 100; i++) {
      var it = ITEMS100[i];
      if (it.active) s += it.score * 2;
    }
    return s;
  },
  expect: 79940,
});

bench({
  name: 'data/map-100',
  unit: 'pass',
  setup: makeItems,
  run: function () {
    return ITEMS100.map(function (it) {
      return it.score * 2;
    });
  },
  expect: function (a) {
    return a.length === 100;
  },
});

bench({
  name: 'data/filter-100',
  unit: 'pass',
  setup: makeItems,
  run: function () {
    return ITEMS100.filter(function (it) {
      return it.active;
    });
  },
  expect: function (a) {
    return a.length === 75;
  },
});

bench({
  name: 'data/reduce-100',
  unit: 'pass',
  setup: makeItems,
  run: function () {
    return ITEMS100.reduce(function (acc, it) {
      return acc + it.score;
    }, 0);
  },
  expect: 53944,
});

bench({
  name: 'data/forEach-100',
  unit: 'pass',
  setup: makeItems,
  run: function () {
    var s = 0;
    ITEMS100.forEach(function (it) {
      s += it.score;
    });
    return s;
  },
  expect: 53944,
});

/* The chain as it is actually written in application code: three passes, three
   intermediate arrays, where the for loop above did one pass and none. */
bench({
  name: 'data/chain-filter-map-reduce-100',
  unit: 'pass',
  setup: makeItems,
  run: function () {
    return ITEMS100.filter(function (it) {
      return it.active;
    })
      .map(function (it) {
        return it.score * 2;
      })
      .reduce(function (acc, v) {
        return acc + v;
      }, 0);
  },
  expect: 79940,
});

/* --------------------------------------------------- immutable primitives */

/*
 * `Object.assign({}, props, patch)` is how every reducer and every
 * `setState`-style update produces a new props object. Seven keys, matching
 * the median RN component.
 */
var PROPS7 = {
  width: 100,
  height: 200,
  opacity: 1,
  borderRadius: 8,
  flexGrow: 1,
  marginTop: 12,
  zIndex: 3,
};

bench({
  name: 'data/object-assign-7key',
  unit: 'update',
  run: function () {
    return Object.assign({}, PROPS7, { opacity: 0.5 });
  },
  expect: function (o) {
    return o.opacity === 0.5 && o.width === 100;
  },
});

/* The spread form of the same idea over an array. */
bench({
  name: 'data/array-spread-100',
  unit: 'copy',
  setup: makeItems,
  run: function () {
    return [...ITEMS100];
  },
  expect: function (a) {
    return a.length === 100;
  },
});

/*
 * The immutable list update: replace one element of a 100-item list by mapping
 * the whole list. This is what a "toggle one row" does in a React list, and it
 * touches all 100 items to change one.
 */
bench({
  name: 'data/immutable-list-update-100',
  unit: 'update',
  setup: makeItems,
  run: function () {
    return ITEMS100.map(function (it) {
      return it.id === 42 ? { id: it.id, score: it.score + 1, active: it.active } : it;
    });
  },
  expect: function (a) {
    return a.length === 100 && a[42] !== ITEMS100[42] && a[41] === ITEMS100[41];
  },
});

/* -------------------------------------------------------------------- Map */

/*
 * Map lookups. Int keys with 500 entries is the shape of a node registry
 * (reactTag -> view); string keys with 100 entries is a component or handler
 * registry. Twenty lookups per unit so the row measures hashing rather than
 * call overhead.
 */
var INT_MAP = null;
var STR_MAP = null;
var STR_KEYS = null;

bench({
  name: 'data/map-get-int-500',
  unit: '20 lookups',
  setup: function () {
    INT_MAP = new Map();
    for (var i = 0; i < 500; i++) INT_MAP.set(i * 3, i);
  },
  run: function () {
    var s = 0;
    for (var i = 0; i < 20; i++) s += INT_MAP.get(((i * 71) % 500) * 3);
    return s;
  },
  expect: 4990,
});

bench({
  name: 'data/map-get-string-100',
  unit: '20 lookups',
  setup: function () {
    STR_MAP = new Map();
    STR_KEYS = [];
    for (var i = 0; i < 100; i++) STR_MAP.set('component_key_' + i, i);
    for (var j = 0; j < 20; j++) STR_KEYS.push('component_key_' + ((j * 7) % 100));
  },
  run: function () {
    var s = 0;
    for (var i = 0; i < 20; i++) s += STR_MAP.get(STR_KEYS[i]);
    return s;
  },
  expect: 830,
});

/* Object-identity keys, which is how React keys its own Maps and Sets (fibers,
   roots, lanes) and how WeakMap-backed caches are keyed. Different hash path
   from both rows above — pointer rather than value — so it needs its own row. */
var OBJ_MAP = null;
var OBJ_KEYS = null;

bench({
  name: 'data/map-get-object-500',
  unit: '20 lookups',
  setup: function () {
    OBJ_MAP = new Map();
    OBJ_KEYS = [];
    var all = [];
    for (var i = 0; i < 500; i++) {
      var k = { id: i };
      all.push(k);
      OBJ_MAP.set(k, i);
    }
    for (var j = 0; j < 20; j++) OBJ_KEYS.push(all[(j * 71) % 500]);
  },
  run: function () {
    var s = 0;
    for (var i = 0; i < 20; i++) s += OBJ_MAP.get(OBJ_KEYS[i]);
    return s;
  },
  expect: 4990,
});

/* Plain-object lookup with the same 20 string keys, as the control: RN often
   has a choice between a Map and an object, and this row is the answer. */
var STR_OBJ = null;

bench({
  name: 'data/object-get-string-100',
  unit: '20 lookups',
  setup: function () {
    STR_OBJ = {};
    STR_KEYS = [];
    for (var i = 0; i < 100; i++) STR_OBJ['component_key_' + i] = i;
    for (var j = 0; j < 20; j++) STR_KEYS.push('component_key_' + ((j * 7) % 100));
  },
  run: function () {
    var s = 0;
    for (var i = 0; i < 20; i++) s += STR_OBJ[STR_KEYS[i]];
    return s;
  },
  expect: 830,
});
