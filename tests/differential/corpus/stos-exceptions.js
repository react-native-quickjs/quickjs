// Differential corpus for the static top-of-stack cache and exceptions.
//
// WHY THIS FILE.  A cached value is not on the operand stack, so anything that
// unwinds the frame has to put it there first or it is never freed.  These
// shapes throw out of the slow paths of cached arithmetic, out of user
// coercion, and out of a callee, with a live cached value on both sides.

function show(k, v) { print(k + '=' + v); }

function afterArith(a, b) {
  var t = a + b;
  try {
    t = t + ({ valueOf: function () { throw new Error('boom'); } });
  } catch (e) {
    return 'caught ' + e.message + ' t=' + t;
  }
  return 'no throw';
}
show('afterArith', afterArith(1, 2));

function stringThenThrow(a) {
  var s = 'x' + a;
  try {
    s = s + ({ toString: function () { throw new Error('str'); } });
  } catch (e) {
    return s;
  }
  return 'none';
}
show('stringThenThrow', stringThenThrow(7));

function throwInCallee(a, b) {
  function boom() { throw new Error('callee'); }
  var t = a * b;
  try {
    t = t + boom();
  } catch (e) {
    return t;
  }
  return -1;
}
show('throwInCallee', throwInCallee(6, 7));

function nestedTry(a) {
  var t = a;
  try {
    try {
      t = t + 1;
      throw new Error('inner');
    } finally {
      t = t * 10;
    }
  } catch (e) {
    return t;
  }
}
show('nestedTry', nestedTry(4));

function loopThrow(n) {
  var t = 0;
  for (var i = 0; i < n; i++) {
    try {
      t = t + 2;
      if (i === 2) throw new Error('at ' + i);
    } catch (e) {
      t = t - 1;
    }
  }
  return t;
}
show('loopThrow', loopThrow(5));

function throwPrimitive(a) {
  var t = a + 1;
  try {
    throw t;
  } catch (e) {
    return typeof e + ':' + e;
  }
}
show('throwPrimitive', throwPrimitive(41));

function rethrow(a) {
  var t = a * 3;
  try {
    try {
      throw new TypeError('t');
    } catch (e) {
      throw e;
    }
  } catch (e) {
    return t + '|' + e.name;
  }
}
show('rethrow', rethrow(2));

function nanFromCoercion(a) {
  var t = a - 1;
  try {
    t = t + undefined;
  } catch (e) {
    return 'never';
  }
  return t;
}
show('nanFromCoercion', nanFromCoercion(5));
