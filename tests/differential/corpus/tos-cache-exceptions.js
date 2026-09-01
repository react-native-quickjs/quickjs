/*
 * Patch 0058 (R2-1, two-state dynamic top-of-stack cache): the leak surface.
 *
 * The cache keeps the logical top of the operand stack in a register that `sp`
 * does not count, and that register holds a COUNTED reference.  Every path out
 * of a participating handler that can reach `exception:` must therefore either
 * have written that reference back below `sp` (TOS_SPILL / TOS_THROW) or have
 * consumed it.  Get it wrong in one handler and the result is a refcount leak
 * that produces the RIGHT ANSWER on every functional test -- which is why this
 * file exists and why it must be run under the assert build, where
 * assert(list_empty(&rt->gc_obj_list)) at JS_FreeRuntime is the instrument that
 * actually sees it.
 *
 * Every case below throws out of a DIFFERENT participating handler class while
 * an object-typed value is live in the cache, so a missed spill shows up as a
 * surviving GC object rather than as a wrong answer.  Objects, not numbers:
 * a leaked int is invisible to the collector and proves nothing.
 *
 *   node tests/differential/run.mjs tos-cache
 *   node tests/differential/run.mjs tos-cache --qjs <assert build>
 */

function t(name, fn) {
  try {
    var r = fn();
    print(name + ' -> ' + r);
  } catch (e) {
    print(name + ' threw ' + e.name);
  }
}

/* --- property load: OP_get_field_ic / OP_get_loc_field_nr ---------------- */

t('get_field on undefined', function () {
  var o = { a: { b: 1 } };
  return o.a.missing.deeper;
});

t('get_field via throwing getter', function () {
  var o = { box: { get v() { throw new RangeError('g'); } } };
  return o.box.v;
});

/* --- property store: OP_put_field_ic ------------------------------------ */

t('put_field via throwing setter', function () {
  var o = { box: { set v(x) { throw new RangeError('s'); } } };
  o.box.v = { payload: 1 };
  return 'no';
});

t('put_field on frozen in strict mode', function () {
  'use strict';
  var o = Object.freeze({ p: 1 });
  o.p = { payload: 2 };
  return 'no';
});

/* --- element load and store: OP_get_array_el / OP_put_array_el ---------- */

t('get_array_el with throwing key', function () {
  var a = [1, 2, 3];
  var k = { toString: function () { throw new TypeError('k'); } };
  return a[k];
});

t('put_array_el with throwing key', function () {
  var a = [1, 2, 3];
  var k = { toString: function () { throw new TypeError('k2'); } };
  a[k] = { payload: 3 };
  return 'no';
});

t('get_array_el on null receiver', function () {
  var holder = { arr: null };
  var i = 0;
  return holder.arr[i];
});

/* --- arithmetic slow paths: OP_add, OP_sub, OP_mul, OP_lt ... ----------- */

t('add with throwing valueOf', function () {
  var bad = { valueOf: function () { throw new TypeError('add'); } };
  var good = { toString: function () { return 'g'; } };
  return good + bad;
});

t('sub with throwing valueOf', function () {
  var bad = { valueOf: function () { throw new TypeError('sub'); } };
  return { x: 1 } - bad;
});

t('lt with throwing valueOf', function () {
  var bad = { valueOf: function () { throw new TypeError('lt'); } };
  var obj = { y: 2 };
  return obj < bad;
});

t('cmp+branch with throwing valueOf', function () {
  var bad = { valueOf: function () { throw new TypeError('cmpbr'); } };
  var obj = { z: 3 };
  if (obj < bad) return 'yes';
  return 'no';
});

t('bitwise or with throwing valueOf', function () {
  var bad = { valueOf: function () { throw new TypeError('or'); } };
  return { w: 4 } | bad;
});

/* --- unary slow paths: OP_inc, OP_neg, OP_not, OP_to_propkey ------------ */

t('inc with throwing valueOf', function () {
  var o = { v: { valueOf: function () { throw new TypeError('inc'); } } };
  var x = o.v;
  x++;
  return 'no';
});

t('neg with throwing valueOf', function () {
  var o = { v: { valueOf: function () { throw new TypeError('neg'); } } };
  return -o.v;
});

t('to_propkey with throwing toString', function () {
  var o = { a: 1 };
  var k = { toString: function () { throw new TypeError('pk'); } };
  o[k] = 5;
  return 'no';
});

/* --- global variable access: OP_get_var --------------------------------- */

t('get_var undefined global', function () {
  var keep = { held: 1 };
  return keep.held + thisGlobalDoesNotExist;
});

/* --- calls: the non-participating stub path ----------------------------- */

t('call a non-function property', function () {
  var o = { notAFunction: 7 };
  return o.notAFunction({ arg: 1 });
});

t('throw from deep in an argument list', function () {
  function f(a, b, c) { return a + b + c; }
  var bad = { valueOf: function () { throw new TypeError('argv'); } };
  return f({ p: 1 }, bad, { q: 3 });
});

/* --- generators suspended mid-expression -------------------------------- */

function* gen() {
  var held = { inside: 1 };
  var a = (yield held) + 1;
  var b = { pair: [a, yield { second: 2 }] };
  yield b;
  return { done: true };
}

t('generator suspended mid-expression', function () {
  var g = gen();
  var out = [];
  out.push(JSON.stringify(g.next().value));
  out.push(JSON.stringify(g.next(10).value));
  out.push(JSON.stringify(g.next(20).value));
  out.push(JSON.stringify(g.next().value));
  return out.join(' ');
});

t('generator abandoned mid-expression', function () {
  var g = gen();
  g.next();
  g.next(1);
  /* dropped without draining: the suspended frame's operand stack, including
     anything the cache had to spill at the yield, must still be freed */
  return 'abandoned';
});

t('generator throw() into a suspended expression', function () {
  var g = gen();
  g.next();
  try {
    g.throw(new RangeError('into-gen'));
  } catch (e) {
    return 'rethrew ' + e.name;
  }
  return 'no';
});

/* --- finally re-entered via OP_gosub / OP_ret --------------------------- */

t('finally over a return', function () {
  var log = [];
  function f() {
    try {
      return { r: 1 };
    } finally {
      log.push('fin');
    }
  }
  var v = f();
  return log.join(',') + ' ' + JSON.stringify(v);
});

t('finally that throws over a pending value', function () {
  function f() {
    try {
      return { r: 2 };
    } finally {
      throw new RangeError('fin');
    }
  }
  return f();
});

t('nested finally with break', function () {
  var log = [];
  outer: for (var i = 0; i < 3; i++) {
    try {
      try {
        if (i === 1) break outer;
        log.push({ i: i });
      } finally {
        log.push('f1-' + i);
      }
    } finally {
      log.push('f2-' + i);
    }
  }
  return JSON.stringify(log);
});

/* --- for-in over a mutated object --------------------------------------- */

t('for-in with deletion during iteration', function () {
  var o = { a: 1, b: 2, c: 3, d: 4 };
  var seen = [];
  for (var k in o) {
    seen.push(k);
    delete o.c;
    o['x' + k] = { added: k };
  }
  return seen.join(',');
});

t('for-in that throws mid-iteration', function () {
  var o = { a: { v: 1 }, b: { v: 2 }, c: { v: 3 } };
  var n = 0;
  for (var k in o) {
    n++;
    if (n === 2) throw new RangeError('in-loop');
  }
  return 'no';
});

t('for-in over a prototype chain with a throwing getter', function () {
  var proto = { get boom() { throw new TypeError('proto'); } };
  var o = Object.create(proto);
  o.own = { v: 1 };
  var out = [];
  for (var k in o) out.push(k + '=' + JSON.stringify(o[k]));
  return out.join(',');
});

/* --- deep recursion to stack overflow ----------------------------------- */

t('stack overflow unwinds cleanly', function () {
  var depth = 0;
  function rec(carry) {
    depth++;
    return rec({ prev: carry });
  }
  try {
    rec({ base: 1 });
  } catch (e) {
    return e instanceof RangeError ? 'RangeError, depth > 100: ' + (depth > 100) : 'wrong: ' + e;
  }
  return 'no';
});

t('stack overflow inside an expression', function () {
  function rec(o) { return { n: rec(o) }; }
  try {
    rec({ a: 1 });
  } catch (e) {
    return e instanceof RangeError ? 'RangeError' : 'wrong';
  }
  return 'no';
});

/* --- a plain functional sweep of every participating family ------------- */

(function functionalSweep() {
  var o = { a: 1, b: 'two', c: [3, 4, 5], d: { e: 6 } };
  var out = [];
  out.push(o.a + o.d.e, o.b + o.a, o.c[1] * 2, o.c.length);
  o.c[1] = 40;
  o.d.e = 60;
  out.push(o.c.join('-'), o.d.e);
  var s = 0;
  for (var i = 0; i < 10; i++) s += i * i - i / 2;
  out.push(s);
  var bits = 0;
  for (var j = 0; j < 8; j++) bits = (bits << 1) | (j & 1);
  out.push(bits, bits >>> 2, bits >> 1, ~bits, -bits, !bits);
  var t2 = 0;
  for (var k2 = 0; k2 < 5; k2++) { t2 += k2; if (t2 > 5) break; }
  out.push(t2);
  out.push(1 < 2, 2 <= 2, 3 > 4, 4 >= 4, 1 == '1', 1 === 1, 1 != 2, 1 !== '1');
  out.push(typeof o, typeof o.zz, o.zz === undefined, o.zz == null);
  var post = 5;
  out.push(post++, post--, post, ++post, --post);
  out.push([o.a, o.b].join('|'));
  print('functionalSweep -> ' + out.join(','));
})();
