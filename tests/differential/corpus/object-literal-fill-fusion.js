// DEFINE-3 / route D: OP_object_fill, the object-literal store fusion.
//
// resolve_labels deletes an eligible literal's N OP_define_field ops and emits
// ONE OP_object_fill that pops N values into the template's slots 0..N-1.  The
// cases below are chosen so that a broken guard CHANGES OBSERVABLE OUTPUT, not
// so that they merely exercise the path:
//
//   * property ORDER and VALUES     -- a wrong slot index reorders or swaps
//   * duplicate keys                -- N defines, N-1 slots; must not fuse
//   * a killer AFTER some properties (spread / getter / computed / __proto__)
//   * an exception thrown BETWEEN the values -- the deferred stores must not
//     happen and nothing on the operand stack may leak or double-free
//   * a value expression that itself builds a literal (nested + interleaved)
//   * generators: the operand stack is saved and restored across a yield while
//     it is carrying the pending values
//   * 16 properties, the JS_OBJLIT_MAX_PROPS cap, and 17, one past it

function show(o) { print(JSON.stringify(o), "|", Object.keys(o).join(",")); }

show({ a: 1, b: 2, c: 3 });
show({ a: "a", b: "b", c: "c", d: "d", e: "e" });

// value order must be preserved: each value records when it was evaluated
var seq = [];
function t(k) { seq.push(k); return k; }
show({ p: t(1), q: t(2), r: t(3) });
print(seq.join(""));

// duplicate key: two defines, one slot
show({ a: 1, a: 2 });
show({ a: 1, b: 2, a: 3 });

// shorthand
var sh1 = 10, sh2 = 20;
show({ sh1, sh2 });

// killers, before and after plain properties
show({ m: 1, ...{ n: 2 }, o: 3 });
show({ ...{ x: 9 }, y: 1 });
var g = { h: 1, get gg() { return 42; }, i: 2 };
print(JSON.stringify(g), g.gg, Object.keys(g).join(","));
show({ j: 1, ["k"]: 2, l: 3 });
var pr = { u: 1, __proto__: null, v: 2 };
print(Object.getPrototypeOf(pr), JSON.stringify(pr));

// exception between the values: the object is abandoned half-built
try {
  var bad = { a: t("A"), b: (function () { throw new Error("mid"); })(), c: t("C") };
  print("unreachable");
} catch (e) {
  print("caught", e.message, seq.join(""));
}

// nested and interleaved
show({ outer1: { in1: 1, in2: 2 }, outer2: 3 });
show({ w: { z: { deep: 1 } }, v2: { q: 2 } });

// a call in the middle -- the callee runs its own literals while ours pend
function mid() { return { called: 1 }; }
show({ f1: 1, f2: mid(), f3: 3 });

// control flow inside the value expressions
var c1 = true, c2 = 0;
show({ a: c1 ? "y" : "n", b: c2 || "z", c: c1 && "w" });

// the cap and one past it
var at16 = { a1: 1, a2: 2, a3: 3, a4: 4, a5: 5, a6: 6, a7: 7, a8: 8,
             a9: 9, a10: 10, a11: 11, a12: 12, a13: 13, a14: 14, a15: 15, a16: 16 };
print(Object.keys(at16).length, at16.a1, at16.a16, Object.keys(at16).join(","));
var at17 = { b1: 1, b2: 2, b3: 3, b4: 4, b5: 5, b6: 6, b7: 7, b8: 8, b9: 9,
             b10: 10, b11: 11, b12: 12, b13: 13, b14: 14, b15: 15, b16: 16, b17: 17 };
print(Object.keys(at17).length, at17.b1, at17.b17);

// generator: the pending values survive a yield
function* gen() { var o = { a: yield 1, b: yield 2, c: 3 }; return o; }
var it = gen();
it.next(); it.next("A");
print(JSON.stringify(it.next("B").value));

// the same literal site, run many times, with different values
function site(i) { return { idx: i, sq: i * i, nm: "n" + i }; }
var acc = [];
for (var i = 0; i < 5; i++) acc.push(JSON.stringify(site(i)));
print(acc.join(" "));

// mutation after construction still behaves
var mo = { a: 1, b: 2, c: 3 };
delete mo.b;
mo.d = 4;
show(mo);
mo.b = 9;
show(mo);

// property descriptors must be configurable/writable/enumerable
var d = Object.getOwnPropertyDescriptor({ a: 1, b: 2 }, "a");
print(d.value, d.writable, d.enumerable, d.configurable);

// a literal whose values are functions (set_object_name interacts with the
// define the rewriter deletes)
var fo = { fa: function () {}, fb: function () {}, fc: 3 };
print(fo.fa.name, fo.fb.name, Object.keys(fo).join(","));

// frozen/sealed after the fill
var fr = Object.freeze({ a: 1, b: 2 });
try { fr.a = 5; } catch (e) { print("frozen-throw"); }
print(fr.a, Object.isFrozen(fr));

// literals inside an object literal's own value position, deeply
show({ l1: { l2: { l3: { l4: 4 } } } });
print("done");
