// GC-stress corpus for the for-in shape-pinning patch.
//
// A pinned shape is a GC object that itself holds a reference to a prototype.
// If the iterator's mark function does not report it, the collector's reference
// accounting is wrong; if the finalizer does not release it, the shape and its
// prototype leak. Both are only observable when an in-flight for-in iterator
// is (a) reachable only through a cycle, and (b) collected by the cycle
// collector rather than by refcounting.
//
// A generator suspended inside a for-in is the way to make an iterator escape
// the stack: the iterator lives in the generator's saved frame, so the
// generator object is the only thing keeping it alive.
//
// Run this under a FORCE_GC_AT_MALLOC build, and preferably also under ASan.
var out = [];
function log(name, v) { out.push(name + ': ' + v); }

function* walk(o) { for (var k in o) yield k; }

// 1. iterator suspended mid-enumeration, whole graph dropped
(function () {
  var n = 0;
  for (var i = 0; i < 300; i++) {
    var proto = {};
    var o = Object.create(proto);
    o.a = 1; o.b = 2; o.c = 3;
    var g = walk(o);
    n += g.next().value === 'a' ? 1 : 0;
    proto.back = g;      // cycle: proto -> generator -> frame -> o -> shape -> proto
    o = null; g = null; proto = null;
  }
  log('suspended-dropped', n);
})();

// 2. same, but the object is mutated after the iterator is suspended so the
//    pinned shape is NOT the object's shape any more and is reachable only
//    from the iterator
(function () {
  var n = 0;
  for (var i = 0; i < 300; i++) {
    var proto = {};
    var o = Object.create(proto);
    o.a = 1; o.b = 2; o.c = 3;
    var g = walk(o);
    g.next();
    delete o.b;          // o moves to a fresh shape; the pin is now the sole owner
    o.d = 4;
    proto.back = g;
    n += 1;
    o = null; g = null; proto = null;
  }
  log('pin-sole-owner', n);
})();

// 3. resume the generator after the shape has changed: the pinned list must be
//    revalidated per key
(function () {
  var r = [];
  var proto = {};
  var o = Object.create(proto);
  o.a = 1; o.b = 2; o.c = 3;
  var g = walk(o);
  r.push(g.next().value);
  delete o.b;
  r.push(g.next().value);
  o.d = 4;
  r.push(g.next().value);
  log('resume-after-mutation', r.join(','));
})();

// 4. long prototype chains with pinned shapes at several levels, all dropped
(function () {
  var n = 0;
  for (var i = 0; i < 200; i++) {
    var a = { x: 1 }, b = Object.create(a), c = Object.create(b);
    b.y = 2; c.z = 3;
    var g1 = walk(c), g2 = walk(b);
    g1.next(); g2.next();
    a.g1 = g1; b.g2 = g2;
    n++;
    a = b = c = g1 = g2 = null;
  }
  log('chain-dropped', n);
})();

// 5. many live iterators at once, then all released
(function () {
  var live = [];
  for (var i = 0; i < 400; i++) {
    var o = {}; o['k' + i] = 1; o.shared = 2;
    var g = walk(o); g.next();
    live.push(g);
  }
  var n = live.length;
  live.length = 0;
  log('many-live', n);
})();

print(out.join('\n'));
