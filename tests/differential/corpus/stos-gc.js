// GC-stress corpus for the static top-of-stack cache.
//
// WHY THIS FILE.  The cache holds a counted reference that is not on the
// operand stack, so a collection triggered between two cached instructions must
// see it.  If it does not -- because the value is parked in a C local the
// collector cannot walk -- the collection frees an object that is still in use
// and the next read is a use-after-free.
//
// The slow paths of the cached arithmetic are the interesting points: they run
// user code (valueOf, toString) and allocate, which is exactly where a
// collection lands.  Run this under a FORCE_GC_AT_MALLOC build and under ASan.

var out = [];
function log(name, v) { out.push(name + ': ' + v); }

function withValueOf(a) {
  var calls = 0;
  var o = {
    valueOf: function () {
      // allocate during the coercion
      var junk = [];
      for (var i = 0; i < 20; i++) junk.push({ i: i });
      calls = calls + junk.length;
      return 7;
    }
  };
  var t = (a + o) * 3;
  return t + ':' + calls;
}
log('valueOf-alloc', withValueOf(2));

function loopAlloc(n) {
  var t = 0;
  for (var i = 0; i < n; i++) {
    var o = { n: i };
    t = t + (i * 2);
    o = null;
  }
  return t;
}
log('loop-alloc', loopAlloc(1000));

function closureCycle(n) {
  var sum = 0;
  for (var i = 0; i < n; i++) {
    (function () {
      var f = function () { return f ? 1 : 0; };
      sum = sum + (f() & 1);
    })();
  }
  return sum;
}
log('closure-cycle', closureCycle(500));

function stringBuild(n) {
  var s = '';
  var t = 0;
  for (var i = 0; i < n; i++) {
    s = s + (i + 1) + ',';
    t = t + i;
  }
  return s.length + ':' + t;
}
log('string-build', stringBuild(200));

function throwAcrossAlloc(n) {
  var t = 0;
  for (var i = 0; i < n; i++) {
    try {
      var junk = [];
      for (var j = 0; j < 10; j++) junk.push({ j: j });
      t = t + 1;
      if ((i & 7) === 0) throw new Error('e' + i);
    } catch (e) {
      t = t - 1;
    }
  }
  return t;
}
log('throw-across-alloc', throwAcrossAlloc(200));

print(out.join('\n'));
