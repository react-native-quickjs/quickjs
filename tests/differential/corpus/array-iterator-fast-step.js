/*
 * Differential corpus for N3-ARRITER -- the per-step fast path in
 * js_array_iterator_next() shared by `for...of` over an array, array
 * DESTRUCTURING, and .entries()/.keys()/.values().
 *
 * Run on a build with the fast path and one without, and diff byte for byte.
 * Also diffed against node.
 *
 * EVERY GUARD HAS AT LEAST ONE POSITIVE CONTROL -- a case whose printed output
 * CHANGES if that guard is deleted, not merely a case that passes. The four
 * guards are:
 *
 *   1. class_id == JS_CLASS_ARRAY   (Proxy, typed array, arguments, string)
 *   2. fast_array                   (index accessor, deleted element)
 *   3. !holey                       (holes must resolve on the prototype)
 *   4. idx < u.array.count          (widened length, mid-iteration shrink)
 *
 * Plus: iterator `return()` on early exit, which destructuring must honour.
 */
function p(l, v) { print(l + " :: " + v); }

/* ---- 1. plain for...of, the fast path itself ---------------------- */
p("forof_empty", (function () { var t = 0; for (var v of []) t += v; return t; })());
p("forof_1", (function () { var t = 0; for (var v of [7]) t += v; return t; })());
p("forof_8", (function () { var t = 0; for (var v of [1,2,3,4,5,6,7,8]) t += v; return t; })());
p("forof_mixed", (function () { var r = []; for (var v of [1,"a",null,undefined,true,{},[]]) r.push(typeof v); return r.join(","); })());
p("forof_order", (function () { var r = []; for (var v of [1,2,3]) r.push(v); return r.join(""); })());
p("forof_break", (function () { var r = []; for (var v of [1,2,3,4,5]) { if (v === 3) break; r.push(v); } return r.join(""); })());
p("forof_continue", (function () { var r = []; for (var v of [1,2,3,4,5]) { if (v % 2) continue; r.push(v); } return r.join(""); })());
p("forof_return", (function () { function f() { for (var v of [1,2,3]) return v; } return f(); })());
p("forof_throw", (function () { try { for (var v of [1,2,3]) { if (v === 2) throw new Error("x"); } } catch (e) { return "caught"; } })());
p("forof_nested", (function () { var t = 0; for (var a of [1,2]) for (var b of [10,20]) t += a * b; return t; })());

/* ---- 2. entries / keys / values ----------------------------------- */
p("entries", (function () { var r = []; for (var e of [7,8,9].entries()) r.push(e[0] + ":" + e[1]); return r.join(","); })());
p("entries_destr", (function () { var r = []; for (var [k, v] of [7,8,9].entries()) r.push(k + ":" + v); return r.join(","); })());
p("keys", (function () { var r = []; for (var k of [7,8,9].keys()) r.push(k); return r.join(","); })());
p("values", (function () { var r = []; for (var v of [7,8,9].values()) r.push(v); return r.join(","); })());
p("entries_isarray", (function () { for (var e of [1].entries()) return Array.isArray(e) + "," + e.length; })());
p("entries_fresh", (function () {
  var seen = []; for (var e of [1,2].entries()) seen.push(e);
  return (seen[0] !== seen[1]) + "," + seen[0].join("") + "," + seen[1].join("");
})());

/* ---- 3. array DESTRUCTURING (same opcode path) -------------------- */
p("destr2", (function () { var [x, y] = [1,2]; return x + "," + y; })());
p("destr8", (function () { var [a,b,c,d,e,f,g,h] = [1,2,3,4,5,6,7,8]; return a + "," + h; })());
p("destr_short", (function () { var [x, y, z] = [1,2]; return x + "," + y + "," + z; })());
p("destr_long", (function () { var [x] = [1,2,3]; return x; })());
p("destr_rest", (function () { var [x, ...r] = [1,2,3,4]; return x + "|" + r.join(","); })());
p("destr_rest_empty", (function () { var [x, ...r] = [1]; return x + "|" + r.length; })());
p("destr_default", (function () { var [x = 9, y = 8] = [1]; return x + "," + y; })());
p("destr_hole_default", (function () { var [x = 9, y = 8] = [1, undefined]; return x + "," + y; })());
p("destr_swap", (function () { var x = 1, y = 2; [x, y] = [y, x]; return x + "," + y; })());
p("destr_nested", (function () { var [[a], [b]] = [[1],[2]]; return a + "," + b; })());
p("destr_param", (function () { function f([a, b]) { return a + b; } return f([3,4]); })());
p("destr_forof", (function () { var r = []; for (var [a,b] of [[1,2],[3,4]]) r.push(a + b); return r.join(","); })());
p("destr_assign_expr", (function () { var o = {}; [o.a, o.b] = [1,2]; return o.a + "," + o.b; })());

/* ---- 4. GUARD 1: not a plain Array -------------------------------- */
p("guard1_proxy", (function () {
  var log = [];
  var P = new Proxy([1,2,3], {
    get: function (t, k, r) { if (typeof k === "string") log.push(k); return Reflect.get(t, k, r); }
  });
  var t = 0; for (var v of P) t += v;
  return t + " | " + log.join(",");                 /* traps MUST fire */
})());
p("guard1_proxy_destr", (function () {
  var n = 0;
  var P = new Proxy([1,2], { get: function (t, k, r) { n++; return Reflect.get(t, k, r); } });
  var [a, b] = P; return a + "," + b + ",traps=" + (n > 0);
})());
p("guard1_typedarray", (function () { var t = 0; for (var v of new Uint8Array([1,2,3])) t += v; return t; })());
p("guard1_ta_float", (function () { var t = 0; for (var v of new Float64Array([1.5,2.5])) t += v; return t; })());
p("guard1_arguments", (function () {
  return (function () { var t = 0; for (var v of arguments) t += v; return t; })(1,2,3);
})());
p("guard1_string", (function () { var r = []; for (var c of "abc") r.push(c); return r.join("-"); })());
p("guard1_set", (function () { var t = 0; for (var v of new Set([1,2,3])) t += v; return t; })());
p("guard1_map", (function () { var r = []; for (var [k,v] of new Map([[1,"a"]])) r.push(k + v); return r.join(""); })());
p("guard1_borrowed_iter", (function () {
  /* an Array Iterator whose `this` is NOT an Array */
  var it = Array.prototype[Symbol.iterator].call({ 0: "x", 1: "y", length: 2 });
  var r = []; var n; while (!(n = it.next()).done) r.push(n.value);
  return r.join(",");
})());
p("guard1_subclass", (function () {
  class MyArr extends Array {}
  var a = MyArr.from([1,2,3]);
  var t = 0; for (var v of a) t += v;
  return t + "," + (a instanceof MyArr);
})());

/* ---- 5. GUARD 2: not a fast array (accessor / deletion) ----------- */
p("guard2_index_getter", (function () {
  var a = [1,2,3];
  var hits = 0;
  Object.defineProperty(a, 1, { get: function () { hits++; return 99; }, configurable: true });
  var r = []; for (var v of a) r.push(v);
  return r.join(",") + " getterhits=" + hits;         /* MUST be 1,99,3 hits=1 */
})());
p("guard2_index_getter_destr", (function () {
  var a = [1,2,3];
  Object.defineProperty(a, 0, { get: function () { return 42; }, configurable: true });
  var [x] = a; return x;                              /* MUST be 42 */
})());
p("guard2_deleted", (function () {
  var a = [1,2,3,4];
  delete a[2];
  var r = []; for (var v of a) r.push(String(v));
  return r.join(",");                                 /* 1,2,undefined,4 */
})());
p("guard2_deleted_proto", (function () {
  var a = [1,2,3,4];
  delete a[2];
  Array.prototype[2] = "FROM_PROTO";
  var r = []; try { for (var v of a) r.push(String(v)); } finally { delete Array.prototype[2]; }
  return r.join(",");                                 /* MUST show FROM_PROTO */
})());
p("guard2_frozen", (function () { var t = 0; for (var v of Object.freeze([1,2,3])) t += v; return t; })());
p("guard2_sealed", (function () { var t = 0; for (var v of Object.seal([1,2,3])) t += v; return t; })());

/* ---- 6. GUARD 3: holes -------------------------------------------- */
p("guard3_holes", (function () { var r = []; for (var v of [1,,3]) r.push(String(v)); return r.join(","); })());
p("guard3_holes_proto", (function () {
  var a = [1,,3];
  Array.prototype[1] = "P1";
  var r = []; try { for (var v of a) r.push(String(v)); } finally { delete Array.prototype[1]; }
  return r.join(",");                                 /* MUST be 1,P1,3 */
})());
p("guard3_holes_destr", (function () {
  var a = [1,,3];
  Array.prototype[1] = "P1";
  var x; try { var [q, w] = a; x = q + "," + w; } finally { delete Array.prototype[1]; }
  return x;                                           /* MUST be 1,P1 */
})());
p("guard3_trailing_hole", (function () { var r = []; for (var v of [1,2,,]) r.push(String(v)); return r.join(","); })());
p("guard3_sparse_big", (function () { var a = []; a[0] = 1; a[5] = 6; var r = []; for (var v of a) r.push(String(v)); return r.join(","); })());

/* ---- 7. GUARD 4: index at/past the dense count -------------------- */
p("guard4_widened", (function () {
  var a = [1,2]; a.length = 5;
  var r = []; for (var v of a) r.push(String(v));
  return r.join(",");                                 /* 1,2,undefined x3 */
})());
p("guard4_widened_proto", (function () {
  var a = [1,2]; a.length = 4;
  Array.prototype[3] = "P3";
  var r = []; try { for (var v of a) r.push(String(v)); } finally { delete Array.prototype[3]; }
  return r.join(",");                                 /* MUST show P3 */
})());
p("guard4_shrink_mid", (function () {
  var a = [1,2,3,4,5];
  var r = []; for (var v of a) { r.push(v); if (v === 2) a.length = 3; }
  return r.join(",");                                 /* 1,2,3 */
})());
p("guard4_grow_mid", (function () {
  var a = [1,2];
  var r = []; for (var v of a) { r.push(v); if (v === 1 && a.length < 4) a.push(9); }
  return r.join(",");                                 /* 1,2,9 */
})());
p("guard4_pop_mid", (function () {
  var a = [1,2,3,4];
  var r = []; for (var v of a) { r.push(v); a.pop(); }
  return r.join(",");                                 /* 1,2 */
})());
p("guard4_delete_mid", (function () {
  var a = [1,2,3,4];
  var r = []; for (var v of a) { r.push(String(v)); if (v === 1) delete a[2]; }
  return r.join(",");                                 /* 1,2,undefined,4 */
})());
p("guard4_shift_mid", (function () {
  var a = [1,2,3,4];
  var r = []; for (var v of a) { r.push(v); a.shift(); }
  return r.join(",");
})());

/* ---- 8. the protocol itself must still be replaceable ------------- */
p("proto_iter_patched", (function () {
  var orig = Array.prototype[Symbol.iterator];
  Array.prototype[Symbol.iterator] = function () {
    var i = 0, self = this;
    return { next: function () {
      return i < self.length ? { value: self[i++] * 10, done: false }
                             : { value: undefined, done: true }; } };
  };
  var t = 0;
  try { for (var v of [1,2,3]) t += v; } finally { Array.prototype[Symbol.iterator] = orig; }
  return t;                                           /* MUST be 60 */
})());
p("proto_iter_restored", (function () { var t = 0; for (var v of [1,2,3]) t += v; return t; })());
p("next_patched", (function () {
  var ap = Object.getPrototypeOf([][Symbol.iterator]());
  var orig = ap.next, calls = 0;
  ap.next = function () { calls++; return orig.call(this); };
  var t = 0;
  try { for (var v of [1,2,3]) t += v; } finally { ap.next = orig; }
  return t + " calls=" + calls;                       /* MUST be 6 calls=4 */
})());
p("next_patched_destr", (function () {
  var ap = Object.getPrototypeOf([][Symbol.iterator]());
  var orig = ap.next;
  ap.next = function () { var r = orig.call(this); if (!r.done) r.value = r.value * 100; return r; };
  var out;
  try { var [x, y] = [1,2]; out = x + "," + y; } finally { ap.next = orig; }
  return out;                                         /* MUST be 100,200 */
})());
p("own_iter", (function () {
  var a = [1,2,3];
  a[Symbol.iterator] = function () { var i = 0; return { next: function () {
    return i < 2 ? { value: "o" + i++, done: false } : { value: undefined, done: true }; } }; };
  var r = []; for (var v of a) r.push(v); return r.join(",");
})());
p("manual_next", (function () {
  var it = [1,2][Symbol.iterator]();
  return JSON.stringify([it.next(), it.next(), it.next()]);
})());
p("iter_self", (function () {
  var it = [1,2][Symbol.iterator]();
  return (it[Symbol.iterator]() === it) + "," + (typeof it.next);
})());
p("iter_exhausted_reuse", (function () {
  var it = [1][Symbol.iterator]();
  it.next(); it.next();
  return JSON.stringify(it.next());
})());

/* ---- 9. iterator return() on early exit --------------------------- */
p("return_called_break", (function () {
  var log = [];
  var o = {}; o[Symbol.iterator] = function () {
    var i = 0;
    return { next: function () { return { value: i++, done: i > 5 }; },
             "return": function () { log.push("ret"); return {}; } };
  };
  for (var v of o) { if (v === 2) break; }
  return log.join(",");                               /* MUST be "ret" */
})());
p("return_called_destr", (function () {
  var log = [];
  var o = {}; o[Symbol.iterator] = function () {
    var i = 0;
    return { next: function () { return { value: i++, done: false }; },
             "return": function () { log.push("ret"); return {}; } };
  };
  var [a, b] = o;
  return a + "," + b + "," + log.join(",");           /* destructuring MUST call return() */
})());
p("return_not_called_exhaust", (function () {
  var log = [];
  var o = {}; o[Symbol.iterator] = function () {
    var i = 0;
    return { next: function () { return { value: i++, done: i > 2 }; },
             "return": function () { log.push("ret"); return {}; } };
  };
  for (var v of o) { }
  return "[" + log.join(",") + "]";                   /* MUST be empty */
})());
p("array_destr_partial", (function () {
  /* a real Array, destructured shorter than its length: the array iterator has
     no observable return(), so this must simply work */
  var [x] = [1,2,3,4]; return x;
})());

/* ---- 10. exceptions ----------------------------------------------- */
p("throw_not_iterable", (function () { try { for (var v of {}) ; return "NO"; } catch (e) { return "TypeError=" + (e instanceof TypeError); } })());
p("throw_null", (function () { try { for (var v of null) ; return "NO"; } catch (e) { return "TypeError=" + (e instanceof TypeError); } })());
p("throw_destr_null", (function () { try { var [x] = null; return "NO"; } catch (e) { return "TypeError=" + (e instanceof TypeError); } })());
p("throw_destr_number", (function () { try { var [x] = 5; return "NO"; } catch (e) { return "TypeError=" + (e instanceof TypeError); } })());

/* ---- 11. large, to exercise growth and many steps ----------------- */
p("large_1000", (function () {
  var a = []; for (var i = 0; i < 1000; i++) a[i] = i;
  var t = 0; for (var v of a) t += v; return t;       /* 499500 */
})());
p("large_entries", (function () {
  var a = []; for (var i = 0; i < 200; i++) a[i] = i;
  var t = 0; for (var e of a.entries()) t += e[0] * e[1]; return t;
})());
p("large_spread", (function () {
  var a = []; for (var i = 0; i < 200; i++) a[i] = i;
  return [...a].length + "," + Math.max(...[1,5,3]);
})());

print("DONE");

/* ==================================================================
 * N6 additions -- ITERATOR LIFETIME. These exist because N6 moves
 * JSArrayIteratorData from a js_malloc'd block INTO the JSObject, which
 * changes finalisation and the GC mark path. Each case is designed to fail
 * if the finalizer or the mark function is wrong for the inlined form.
 * ================================================================== */
function p2(l, v) { print(l + " :: " + v); }

/* an iterator that OUTLIVES its loop and is resumed afterwards */
p2("n6_outlive", (function () {
  var a = [1,2,3,4,5];
  var it = a[Symbol.iterator]();
  for (var v of a) { if (v === 2) break; }        /* a different iterator */
  var r = [];
  var n; while (!(n = it.next()).done) r.push(n.value);
  return r.join(",");
})());

/* the iterator survives a GC while still live -- the MARK function must keep
   it->obj reachable, or the array is collected and the next step reads freed
   memory */
p2("n6_survive_gc", (function () {
  var it = [1,2,3,4,5,6,7,8].slice()[Symbol.iterator]();
  it.next();
  for (var i = 0; i < 200000; i++) { var junk = { a: i, b: [i] }; }
  var r = [];
  var n; while (!(n = it.next()).done) r.push(n.value);
  return r.join(",");
})());

/* many iterators created and abandoned -- exercises finalisation in bulk */
p2("n6_bulk_finalize", (function () {
  var a = [1,2,3];
  for (var i = 0; i < 100000; i++) { var it = a[Symbol.iterator](); it.next(); }
  var t = 0; for (var v of a) t += v;
  return t;
})());

/* manual next() after the backing array is mutated */
p2("n6_mutate_then_next", (function () {
  var a = [1,2,3,4];
  var it = a[Symbol.iterator]();
  it.next(); it.next();
  a.length = 3;
  var r = []; var n; while (!(n = it.next()).done) r.push(n.value);
  return r.join(",");
})());
p2("n6_mutate_grow", (function () {
  var a = [1,2];
  var it = a[Symbol.iterator]();
  it.next(); it.next();
  a.push(9, 10);
  var r = []; var n; while (!(n = it.next()).done) r.push(n.value);
  return r.join(",");
})());
p2("n6_mutate_delete", (function () {
  var a = [1,2,3,4];
  var it = a[Symbol.iterator]();
  it.next();
  delete a[2];
  var r = []; var n; while (!(n = it.next()).done) r.push(String(n.value));
  return r.join(",");
})());

/* the iterator's backing array is only reachable THROUGH the iterator */
p2("n6_only_ref", (function () {
  var it = (function () { return [7,8,9][Symbol.iterator](); })();
  for (var i = 0; i < 200000; i++) { var junk = { a: i }; }
  var r = []; var n; while (!(n = it.next()).done) r.push(n.value);
  return r.join(",");
})());

/* string iterator shares the same record and the same finalizer */
p2("n6_string_iter", (function () {
  var it = "abcd"[Symbol.iterator]();
  var r = []; var n; while (!(n = it.next()).done) r.push(n.value);
  return r.join("");
})());
p2("n6_string_iter_gc", (function () {
  var it = ("ab" + "cd")[Symbol.iterator]();
  it.next();
  for (var i = 0; i < 100000; i++) { var junk = { a: i }; }
  var r = []; var n; while (!(n = it.next()).done) r.push(n.value);
  return r.join("");
})());
p2("n6_string_surrogate", (function () {
  var r = []; for (var c of "😀ab") r.push(c.length);
  return r.join(",");
})());

/* entries/keys iterators outliving their loop */
p2("n6_entries_outlive", (function () {
  var it = [5,6][Symbol.iterator] && [5,6].entries();
  it.next();
  for (var i = 0; i < 100000; i++) { var junk = [i]; }
  var n = it.next();
  return n.done + "," + (n.value ? n.value.join(":") : "");
})());

/* the TypeError path for a borrowed next() on the wrong receiver */
p2("n6_wrong_receiver", (function () {
  var next = [1][Symbol.iterator]().next;
  try { next.call({}); return "NO THROW"; }
  catch (e) { return "TypeError=" + (e instanceof TypeError); }
})());
p2("n6_wrong_receiver_str", (function () {
  var next = "a"[Symbol.iterator]().next;
  try { next.call([1,2]); return "NO THROW"; }
  catch (e) { return "TypeError=" + (e instanceof TypeError); }
})());
p2("n6_exhausted_then_gc", (function () {
  var it = [1][Symbol.iterator]();
  it.next(); it.next();
  for (var i = 0; i < 100000; i++) { var junk = { a: i }; }
  return JSON.stringify(it.next());
})());

print("DONE2");
