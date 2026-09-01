/*
 * Differential corpus for the depth-1 prototype-read inline-cache hit path.
 *
 * The IC entry for a `recv.p` read whose property lives on `recv`'s DIRECT
 * prototype is revalidated on every hit. Two formulations exist:
 *
 *   full   holder = recv->shape->proto; hsh = holder->shape;
 *          offset < hsh->prop_count && shape_prop[offset].atom == atom &&
 *          !(shape_prop[offset].flags & JS_PROP_TMASK)
 *   fast   holder->shape == <the holder shape recorded at fill time>
 *
 * `fast` is claimed equivalent because a JSShape determines its whole property
 * list, and the receiver-shape guard already pins the holder. This corpus
 * exists to discriminate the two, and to kill a build in which the revalidation
 * is removed altogether.
 *
 * TWO RULES, both learned by writing this file wrong first:
 *
 *  1. **Every case must read through ONE shared read site**, i.e. a function
 *     called both before and after the mutation. Each textual `o.p` in the
 *     source is a SEPARATE `ic_idx`, so a post-mutation read written as a
 *     second loop is a COLD site that simply fills fresh and proves nothing.
 *     The first version of this file did that and reported **zero IC misses on
 *     the whole corpus** while every mutant survived.
 *  2. **Warm past the IC fill threshold before mutating**, or the read after
 *     the mutation is a cold lookup rather than a cached hit.
 *
 * Referenced by docs/phone-remeasurement-round7.md.
 */

function P(o) { print(o); }
var i, s, acc;

/* One shared read site per property name. These are the sites under test. */
function rdV(o) { return o.v; }
function rdW(o) { return o.w; }
function rdG(o) { return o.g; }
function rdX(o) { return o.x; }
function rdK(o) { return o.k; }
function rdM(o) { return o.m; }
function rdAA(o) { return o.aa; }
function rdBB(o) { return o.bb; }
function rdDeep(o) { return o.deep; }
function rdQ(o) { return o.q; }
function rdR(o) { return o.r; }
function rdT(o) { return o.t; }
function warm(f, o, n) { var r; for (var j = 0; j < n; j++) r = f(o); return r; }

/* ---- 1. baseline: a stable depth-1 proto read ---- */
function A() { this.own = 1; }
A.prototype.v = "A.v";
var a = new A();
P("1 " + warm(rdV, a, 200000));

/* ---- 2. the proto property's VALUE is reassigned (holder shape unchanged) ---- */
A.prototype.v = "A.v2";
P("2 " + rdV(a));
P("2b " + warm(rdV, a, 1000));

/* ---- 3. the proto gains a NEW property AFTER the property under test, so the
       tested slot does NOT move. `full` keeps hitting; `fast` must miss (the
       holder shape changed) and refill. Both must return the same value. ---- */
A.prototype.zzz = "A.zzz";
P("3 " + rdV(a) + " " + a.zzz);
P("3b " + warm(rdV, a, 100000));

/* ---- 4. the proto property is DELETED: must fall through to Object.prototype
       (undefined here), not keep reading the stale slot ---- */
function B() { this.own = 1; }
B.prototype.w = "B.w";
var b = new B();
warm(rdW, b, 200000);
delete B.prototype.w;
P("4 " + rdW(b));
P("4b " + warm(rdW, b, 50000));
B.prototype.w = "B.w2";
P("4c " + warm(rdW, b, 50000));

/* ---- 5. the proto property is REPLACED BY A GETTER in place. This is the
       JS_PROP_TMASK case; reading the slot as a data property would hand back
       the getter/setter pair reinterpreted as a JSValue. ---- */
function C() { this.own = 1; }
C.prototype.g = "C.g";
var c = new C();
warm(rdG, c, 200000);
Object.defineProperty(C.prototype, "g", { get: function () { return "C.getter"; },
                                          configurable: true });
P("5 " + rdG(c));
P("5b " + warm(rdG, c, 50000));

/* ---- 6. and back to a plain data property ---- */
Object.defineProperty(C.prototype, "g", { value: "C.plain", writable: true,
                                          configurable: true });
P("6 " + warm(rdG, c, 100000));

/* ---- 7. SHADOWING: the receiver gains its own property of the same name.
       The receiver shape changes, so the whole entry must miss. ---- */
function D() { this.own = 1; }
D.prototype.x = "D.proto.x";
var d = new D();
warm(rdX, d, 200000);
d.x = "D.own.x";
P("7 " + rdX(d));
P("7b " + warm(rdX, d, 50000));
delete d.x;
P("7c " + warm(rdX, d, 50000));

/* ---- 8. the RECEIVER's prototype is swapped for another object carrying the
       same property name at a DIFFERENT slot ---- */
function E() { this.own = 1; }
E.prototype.k = "E.proto.k";
var e = new E();
warm(rdK, e, 200000);
var alt = { pad0: 0, pad1: 1, pad2: 2, k: "alt.k" };
Object.setPrototypeOf(e, alt);
P("8 " + rdK(e));
P("8b " + warm(rdK, e, 100000));

/* ---- 9. IN-PLACE slot move: delete a property that sits BEFORE the one under
       test, then re-add it, so the tested property's offset changes while the
       holder object stays the same. A guard that compares only prop_count is
       killed here; a guard that compares only the atom at the OLD offset is
       killed here; shape identity survives. ---- */
var h9 = { first: 1, w: "h9.w" };
var o9 = Object.create(h9); o9.own = 1;
warm(rdW, o9, 200000);
P("9 " + rdW(o9));
delete h9.first;
P("9a " + rdW(o9) + " " + warm(rdW, o9, 50000));
h9.first = 2;              /* re-added at the END: prop_count is back to 2 */
P("9b " + rdW(o9) + " " + warm(rdW, o9, 50000));
h9.zz = 3; delete h9.w; h9.w = "h9.w2";
P("9c " + rdW(o9) + " " + warm(rdW, o9, 50000));

/* ---- 10. two protos with the SAME property at DIFFERENT slots read through
       one polymorphic site, in both orders ---- */
var p1 = { m: "p1.m" };
var p2 = { pad: 0, m: "p2.m" };
var o1 = Object.create(p1), o2 = Object.create(p2);
o1.own = 1; o2.own = 1;
for (i = 0; i < 200000; i++) acc = rdM(o1) + "/" + rdM(o2);
P("10 " + acc);
for (i = 0; i < 200000; i++) acc = rdM(o2) + "/" + rdM(o1);
P("10b " + acc);

/* ---- 11. two DIFFERENT property names read from the same holder shape ---- */
var hh = { aa: "h.aa", bb: "h.bb" };
var g1 = Object.create(hh); g1.own = 1;
for (i = 0; i < 200000; i++) acc = rdAA(g1) + "/" + rdBB(g1);
P("11 " + acc);

/* ---- 12. depth-2: the property is on the grandparent, which is NOT a
       depth-1 proto hit, and then it is shadowed at depth 1 ---- */
var gp = { deep: "gp.deep" };
var mid = Object.create(gp);
var leaf = Object.create(mid); leaf.own = 1;
P("12 " + warm(rdDeep, leaf, 200000));
mid.deep = "mid.deep";
P("12b " + rdDeep(leaf));
P("12c " + warm(rdDeep, leaf, 100000));

/* ---- 13. frozen prototype, then a fresh one ---- */
function F() { this.own = 1; }
F.prototype.q = "F.q";
Object.freeze(F.prototype);
var f = new F();
P("13 " + warm(rdQ, f, 200000) + " " + Object.isFrozen(F.prototype));
F.prototype = { q: "F.q2" };
var f2 = new F();
P("13b " + warm(rdQ, f2, 100000) + " " + rdQ(f));

/* ---- 14. property made non-writable in place, then written through the
       receiver (sloppy mode: silently ignored) ---- */
function G() { this.own = 1; }
G.prototype.r = "G.r";
var gg = new G();
warm(rdR, gg, 200000);
Object.defineProperty(G.prototype, "r", { writable: false });
gg.r = "ignored";
P("14 " + rdR(gg) + " " + Object.prototype.hasOwnProperty.call(gg, "r"));

/* ---- 15. many receiver shapes sharing one holder: pushes the site
       megamorphic and back through the slow path ---- */
var proto15 = { t: "p15.t" };
for (i = 0; i < 20000; i++) {
  var o = Object.create(proto15);
  o["k" + (i % 40)] = i;
  acc = rdT(o);
}
P("15 " + acc);

/* ---- 16. holder shape churn plus GC, to catch a dangling holder-shape ref ---- */
function mk(name) { var pr = { u: name }; var ob = Object.create(pr); ob.own = 1; return ob; }
var keep = "";
for (i = 0; i < 50000; i++) keep = mk("u" + (i % 3)).u;
P("16 " + keep);
