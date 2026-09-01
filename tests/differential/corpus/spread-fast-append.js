/*
 * Differential corpus for N1-SPREADFAST -- the fast-array bulk append in
 * js_append_enumerate (quickjs.c, the `is_array_iterator` block hoisted above
 * JS_GetIterator).
 *
 * Run the SAME file on a build with the fast path and a build without it and
 * diff the output byte for byte. Every case below either exercises the fast
 * path or is a shape that MUST fall back to the iterator protocol; a guard
 * that stops working shows up as a differing line, not as a crash.
 *
 * Each of the five guards has at least two cases aimed at it, because
 * single-case coverage has repeatedly failed to discriminate in this project:
 * a corpus written alongside the optimization tends to exercise only what the
 * author already had in mind. The guards are:
 *
 *   1. Array.prototype[Symbol.iterator] is the builtin
 *   2. %ArrayIteratorPrototype%.next is the builtin
 *   3. the source is a dense non-holey fast Array
 *   4. length agrees with the dense element count
 *   5. the destination accumulator is a dense fast Array at the expected count
 */
function p(label, v) { print(label + " :: " + v); }
function sink() { return Array.prototype.slice.call(arguments).join(","); }
function count() { return arguments.length; }

/* ---- 1. the fast path itself -------------------------------------- */
p("dense0", sink(...[]));
p("dense1", sink(...[1]));
p("dense4", sink(...[1, 2, 3, 4]));
p("dense8", sink(...[1, 2, 3, 4, 5, 6, 7, 8]));
p("dense-len0", count(...[]));
p("dense-len8", count(...[1, 2, 3, 4, 5, 6, 7, 8]));
p("mixed-before", sink(0, ...[1, 2]));
p("mixed-after", sink(...[1, 2], 3));
p("mixed-both", sink(0, ...[1, 2], 3, ...[4, 5], 6));
p("two-spreads-adjacent", sink(...[1, 2], ...[3, 4]));
p("nested", sink(...[1, 2].concat([3, 4])));
p("objects", sink(...[{ toString: function () { return "o1"; } },
                      { toString: function () { return "o2"; } }]));
p("strings", sink(...["a", "b", "c"]));
p("doubles", sink(...[1.5, 2.5, -0.5]));
p("undef-null", sink(...[undefined, null, 0, false, ""]));
p("new-spread", (function () {
  function C() { this.v = Array.prototype.join.call(arguments, "|"); }
  return new C(...[7, 8, 9]).v;
})());

/* ---- 2. GUARD 1: Array.prototype[Symbol.iterator] replaced --------- */
p("guard1-proto-iter", (function () {
  var orig = Array.prototype[Symbol.iterator];
  Array.prototype[Symbol.iterator] = function () {
    var i = 0, self = this;
    return { next: function () {
      return i < self.length ? { value: self[i++] * 10, done: false }
                             : { value: undefined, done: true }; } };
  };
  var r;
  try { r = sink(...[1, 2, 3]); } finally {
    Array.prototype[Symbol.iterator] = orig;
  }
  return r;                                    /* MUST be "10,20,30" */
})());
p("guard1-restored", sink(...[1, 2, 3]));      /* and back to "1,2,3" */

p("guard1-own-iter", (function () {
  var a = [1, 2, 3];
  a[Symbol.iterator] = function () {
    var n = 0;
    return { next: function () {
      return n < 2 ? { value: "own" + n++, done: false }
                   : { value: undefined, done: true }; } };
  };
  return sink(...a);                           /* MUST be "own0,own1" */
})());

/* ---- 3. GUARD 2: %ArrayIteratorPrototype%.next replaced ------------ */
p("guard2-next", (function () {
  var proto = Object.getPrototypeOf([][Symbol.iterator]()); /* %ArrayIteratorPrototype% */
  var orig = proto.next;
  var calls = 0;
  proto.next = function () { calls++; return orig.call(this); };
  var r;
  try { r = sink(...[1, 2, 3]); } finally { proto.next = orig; }
  return r + " calls=" + calls;                /* MUST show calls=4 */
})());
p("guard2-restored", sink(...[1, 2, 3]));
p("guard2-proto-identity", (function () {
  var ap = Object.getPrototypeOf([][Symbol.iterator]());
  return ap.hasOwnProperty("next") + "," +
         (typeof ap.next === "function");     /* true,true or the test is aimed wrong */
})());

p("guard2-next-transform", (function () {
  var proto = Object.getPrototypeOf([][Symbol.iterator]()); /* %ArrayIteratorPrototype% */
  var orig = proto.next;
  proto.next = function () {
    var r = orig.call(this);
    if (!r.done) r.value = "<" + r.value + ">";
    return r;
  };
  var r;
  try { r = sink(...[1, 2]); } finally { proto.next = orig; }
  return r;                                    /* MUST be "<1>,<2>" */
})());

/* ---- 4. GUARD 3: not a dense fast array ---------------------------- */
p("guard3-holes", sink(...[1, , 3]));
p("guard3-holes-len", count(...[1, , 3]));
p("guard3-holes-proto", (function () {
  var a = [1, , 3];
  Array.prototype[1] = "FROM_PROTO";
  var r;
  try { r = sink(...a); } finally { delete Array.prototype[1]; }
  return r;                                    /* MUST be "1,FROM_PROTO,3" */
})());
p("guard3-trailing-hole", sink(...[1, 2, , ]));
p("guard3-proxy", (function () {
  var a = [1, 2, 3];
  var pr = new Proxy(a, {
    get: function (t, k) {
      if (typeof k === "string" && /^\d+$/.test(k)) return t[k] * 100;
      return Reflect.get(t, k);
    }
  });
  return sink(...pr);                          /* MUST be "100,200,300" */
})());
p("guard3-arguments", (function () {
  return (function () { return sink(...arguments); })(1, 2, 3);
})());
p("guard3-string", sink(..."abc"));
p("guard3-set", sink(...new Set([1, 2, 2, 3])));
p("guard3-map", sink(...new Map([[1, "a"]])).length > 0);
p("guard3-typedarray", sink(...new Uint8Array([1, 2, 3])));
p("guard3-generator", (function () {
  function* g() { yield 1; yield 2; }
  return sink(...g());
})());
p("guard3-subclass", (function () {
  function Sub() {}
  Sub.prototype = Object.create(Array.prototype);
  Sub.prototype[Symbol.iterator] = function () {
    var n = 0;
    return { next: function () {
      return n < 2 ? { value: "sub" + n++, done: false }
                   : { value: undefined, done: true }; } };
  };
  var o = new Sub(); o.length = 2; o[0] = "x"; o[1] = "y";
  return sink(...o);
})());

/* ---- 5. GUARD 4: length disagrees with the dense count ------------- */
p("guard4-widened", (function () {
  var a = [1, 2];
  a.length = 5;
  return sink(...a) + " n=" + count(...a);     /* 5 args, 3 undefined */
})());
p("guard4-widened-proto", (function () {
  var a = [1, 2];
  a.length = 4;
  Array.prototype[3] = "P3";
  var r;
  try { r = sink(...a); } finally { delete Array.prototype[3]; }
  return r;                                    /* MUST be "1,2,,P3" */
})());
p("guard4-shrunk", (function () {
  var a = [1, 2, 3, 4];
  a.length = 2;
  return sink(...a);
})());

/* ---- 6. GUARD 5: destination accumulator shape --------------------- */
p("guard5-many", (function () {
  var big = [];
  for (var i = 0; i < 200; i++) big.push(i);
  return count(...big) + ":" + sink(...big).length;
})());
p("guard5-grow", (function () {
  /* forces repeated expand of the accumulator across several spreads */
  var a = [1, 2], b = [3, 4, 5], c = [6];
  return sink(...a, ...b, ...c, 7, ...a);
})());
p("guard5-large", (function () {
  var a = new Array(1000);
  for (var i = 0; i < 1000; i++) a[i] = i;
  var t = 0;
  (function () { for (var j = 0; j < arguments.length; j++) t += arguments[j]; })(...a);
  return t;                                    /* 499500 */
})());

/* ---- 7. side-effect ORDER, which a bulk copy could reorder --------- */
p("order-getter-src", (function () {
  var log = [];
  var a = [1, 2, 3];
  var b = { get x() { log.push("x"); return 9; } };
  log.push("call");
  sink(...a, b.x);
  return log.join(",");
})());
p("order-two-iterables", (function () {
  var log = [];
  function mk(tag, n) {
    var o = {};
    o[Symbol.iterator] = function () {
      var i = 0;
      return { next: function () {
        log.push(tag + i);
        return i < n ? { value: tag + (i++), done: false }
                     : { value: undefined, done: true }; } };
    };
    return o;
  }
  var r = sink(...mk("A", 2), ...[7, 8], ...mk("B", 1));
  return r + " | " + log.join(",");
})());

/* ---- 8. aliasing: spreading the array into itself-ish -------------- */
p("alias-same-twice", (function () {
  var a = [1, 2, 3];
  return sink(...a, ...a);
})());
p("alias-mutate-during", (function () {
  var a = [1, 2, 3];
  var o = {};
  o[Symbol.iterator] = function () {
    var i = 0;
    return { next: function () {
      a.push(99);
      return i++ < 1 ? { value: "m", done: false }
                     : { value: undefined, done: true }; } };
  };
  var r = sink(...a, ...o);
  return r + " | a=" + a.join(",");
})());

/* ---- 9. exceptions must still propagate --------------------------- */
p("throw-in-iter", (function () {
  var o = {};
  o[Symbol.iterator] = function () { throw new Error("boom"); };
  try { sink(...o); return "NO THROW"; } catch (e) { return "caught " + e.message; }
})());
p("throw-not-iterable", (function () {
  try { sink(...{}); return "NO THROW"; } catch (e) { return "caught TypeError=" + (e instanceof TypeError); }
})());
p("throw-null", (function () {
  try { sink(...null); return "NO THROW"; } catch (e) { return "caught TypeError=" + (e instanceof TypeError); }
})());

/* ---- 10. frozen / sealed / non-writable length -------------------- */
p("frozen-src", (function () {
  var a = Object.freeze([1, 2, 3]);
  return sink(...a);
})());
p("sealed-src", (function () {
  var a = Object.seal([1, 2, 3]);
  return sink(...a);
})());

print("DONE");
