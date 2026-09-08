/*
 * Focused property-cache microbenchmarks.
 *
 * Each row keeps the hot property site in one function and moves fixture
 * construction out of the timed body.  The rows are intentionally small:
 * they make monomorphic, polymorphic, prototype and store behavior visible
 * without the application noise in the broader primitives and Octane suites.
 */

function loadX(o) {
  return o.x;
}

function loadLocal(o) {
  var receiver = o;
  return receiver.x;
}

function storeX(o, v) {
  o.x = v;
  return o.x;
}

function ShapeA(x) { this.x = x; }
function ShapeB(x) { this.y = 1; this.x = x; }
function ShapeC(x) { this.z = 1; this.x = x; }
function ShapeD(x) { this.a = 1; this.b = 2; this.x = x; }
function ShapeE(x) { this.c = 1; this.d = 2; this.e = 3; this.x = x; }

var mono = [], poly2 = [], mega5 = [], mostlyPolyStore = [];
var ownStore = [], proto1 = [], proto2 = [], proto3 = [], missing = [];

function makeFixtures() {
  mono = [];
  poly2 = [];
  mega5 = [];
  mostlyPolyStore = [];
  ownStore = [];
  proto1 = [];
  proto2 = [];
  proto3 = [];
  missing = [];
  var ctors = [ShapeA, ShapeB, ShapeC, ShapeD, ShapeE];
  var proto = {x: 7};
  var protoMid = Object.create(proto);
  var protoDeep = Object.create(protoMid);
  for (var i = 0; i < 128; i++) {
    mono.push(new ShapeA(i));
    poly2.push(i & 1 ? new ShapeB(i) : new ShapeA(i));
    mega5.push(new ctors[i % 5](i));
    mostlyPolyStore.push(i === 63 ? new ShapeC(i) : (i & 1 ? new ShapeB(i) : new ShapeA(i)));
    ownStore.push(new ShapeA(0));
    proto1.push(Object.create(proto));
    proto2.push(Object.create(protoMid));
    proto3.push(Object.create(protoDeep));
    missing.push({y: i});
  }
}

bench({
  name: 'ic/mono-own-load',
  unit: '128 reads',
  setup: makeFixtures,
  run: function () {
    var s = 0;
    for (var i = 0; i < 128; i++) s += loadX(mono[i]);
    return s;
  },
  expect: 8128,
});

bench({
  name: 'ic/local-own-load',
  unit: '128 reads',
  setup: makeFixtures,
  run: function () {
    var s = 0;
    for (var i = 0; i < 128; i++) s += loadLocal(mono[i]);
    return s;
  },
  expect: 8128,
});

bench({
  name: 'ic/poly2-own-load',
  unit: '128 reads',
  setup: makeFixtures,
  run: function () {
    var s = 0;
    for (var i = 0; i < 128; i++) s += loadX(poly2[i]);
    return s;
  },
  expect: 8128,
});

bench({
  name: 'ic/mega5-own-load',
  unit: '128 reads',
  setup: makeFixtures,
  run: function () {
    var s = 0;
    for (var i = 0; i < 128; i++) s += loadX(mega5[i]);
    return s;
  },
  expect: 8128,
});

bench({
  name: 'ic/prototype-depth1',
  unit: '128 reads',
  setup: makeFixtures,
  run: function () {
    var s = 0;
    for (var i = 0; i < 128; i++) s += loadX(proto1[i]);
    return s;
  },
  expect: 896,
});

bench({
  name: 'ic/prototype-depth2',
  unit: '128 reads',
  setup: makeFixtures,
  run: function () {
    var s = 0;
    for (var i = 0; i < 128; i++) s += loadX(proto2[i]);
    return s;
  },
  expect: 896,
});

bench({
  name: 'ic/prototype-depth3',
  unit: '128 reads',
  setup: makeFixtures,
  run: function () {
    var s = 0;
    for (var i = 0; i < 128; i++) s += loadX(proto3[i]);
    return s;
  },
  expect: 896,
});

bench({
  name: 'ic/missing-own-load',
  unit: '128 reads',
  setup: makeFixtures,
  run: function () {
    var s = 0;
    for (var i = 0; i < 128; i++) s += loadX(missing[i]) === undefined;
    return s;
  },
  expect: 128,
});

bench({
  name: 'ic/own-store',
  unit: '128 stores',
  setup: makeFixtures,
  run: function () {
    var s = 0;
    for (var i = 0; i < 128; i++) s += storeX(ownStore[i], i);
    return s;
  },
  expect: 8128,
});

bench({
  name: 'ic/mostly-bimorphic-store-rare-outlier',
  unit: '4096 stores',
  setup: makeFixtures,
  run: function () {
    var s = 0;
    for (var round = 0; round < 32; round++) {
      for (var i = 0; i < 128; i++) {
        var o = mostlyPolyStore[i];
        o.x = round + i;
        s += o.x;
      }
    }
    return s;
  },
  expect: 323584,
});

bench({
  name: 'ic/failed-prototype-mutations-then-constructor-store',
  unit: '128 mutation groups and constructions',
  setup: makeFixtures,
  run: function () {
    var s = 0;
    for (var i = 0; i < 128; i++) {
      var rejected = new ShapeA(i);
      Object.preventExtensions(rejected);
      for (var j = 0; j < 64; j++) {
        try { Object.setPrototypeOf(rejected, null); } catch (e) {}
      }
      var o = new makeObject(i);
      s += o.x + o.y;
    }
    return s;
  },
  expect: 16384,
});

function makeObject(x) {
  this.x = x;
  this.y = x + 1;
}

bench({
  name: 'ic/constructor-store',
  unit: '128 constructions',
  run: function () {
    var s = 0;
    for (var i = 0; i < 128; i++) {
      var o = new makeObject(i);
      s += o.x + o.y;
    }
    return s;
  },
  expect: 16384,
});
