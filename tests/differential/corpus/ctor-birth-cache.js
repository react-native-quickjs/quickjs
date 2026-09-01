/* Adversarial corpus for the CONSTRUCTOR BIRTH CACHE
   (patches/pending/0072-constructor-birth-cache.patch as repaired by lane
   CTOR-1; docs/ctor-1-the-constructor-birth-cache-can-be-made-correct.md).

   The mechanism under test replaces `js_create_from_ctor`'s generic
   `JS_GetProperty(ctor, "prototype")` plus `find_hashed_shape_proto()` with a
   32-way direct-mapped table on the constructor object pointer.  A hit returns
   `JS_NewObjectFromShape(ctx, js_dup_shape(e->obj_shape), ...)` directly.

   The cache holds counted references to `ctor`, `proto` and a HASHED
   `obj_shape` only.  It holds NO pointer to the constructor's own shape; the
   cached slot index is re-derived from the constructor's live shape on every
   hit.  The four tests on that path are:

     (1) idx < ctor->shape->prop_count        (the read is in bounds)
     (2) shape_prop[idx].atom == "prototype"  (it is the RIGHT property)
     (3) (shape_prop[idx].flags & TMASK) == 0 (u.value really is a value)
     (4) ctor->prop[idx].u.value == e->proto  (the value is still that proto)

   ## What each guard is discriminated by

     guard                                     detected by
     ----------------------------------------  ---------------------------
     (1) bounds                                cases 4, 5    (compaction)
     (2) atom == prototype                     cases 4, 5    (compaction)
     (3) TMASK == 0                            NOT detectable; see below
     (4) value == e->proto                     cases 2, 3, 9
     obj_shape pinned and hashed               NOT detectable by output
     the JS_MarkContext hunk                   NOT detectable here; see below

   ## The guards no output diff can reach, and why

   1. **(3), the TMASK test.** For it to fire on a HIT, a constructor's own
      `prototype` would have to become an accessor, a var-ref or an autoinit
      stub AFTER having been a plain data property.  `prototype` is
      non-configurable on every function object the language can produce
      (ordinary, generator, class), so no ECMAScript program can perform that
      transition.  The test is therefore not decoration but it is also not
      singly discriminable: it is discriminated only in the PAIR (2)+(3),
      because with the atom test also removed, compaction in cases 4 and 5 can
      leave an AUTOINIT `prototype` stub or a getter at the cached index and
      `u.value` is then read as a JSValue.  See the mutation matrix in the
      write-up.

   2. **The `JS_MarkContext` hunk** (four strong references reported to the
      collector) is a lifetime property, not an output property, and under
      reference counting a MISSING mark makes objects look MORE rooted rather
      than less.  It is discriminated only on a TRACING build with a forced
      collection while an entry is live; see the write-up.

   3. **The `obj_shape` pin.** Its absence would let the canonical empty shape
      be edited in place or unlinked from rt->shape_hash; both cost memory
      safety, not output.

   Plain ES5 where possible, `print()` for output, deterministic. */

function say(x) { print(x); }
function tag(o) { return o && o.__t ? o.__t : "?"; }

/* ---- 1. the baseline the cache is FOR ------------------------------- */
function A() { this.k = 1; }
A.prototype.__t = "A";
var s1 = "";
for (var i = 0; i < 50; i++) s1 += tag(new A());
say("1 " + (s1.length === 50 ? "all-A" : "MIXED") + " " + s1.charAt(0));

/* ---- 2. C.prototype reassigned AFTER warmup ------------------------- */
/* Guard (4).  Without it the cache keeps handing out the OLD prototype. */
function B() {}
B.prototype.__t = "B1";
for (var i = 0; i < 40; i++) new B();
var oldB = B.prototype;
B.prototype = { __t: "B2" };
say("2 " + tag(new B()) + " " + (Object.getPrototypeOf(new B()) === B.prototype) +
    " " + (Object.getPrototypeOf(new B()) === oldB));

/* ---- 3. reassigned, then reassigned BACK ---------------------------- */
function B3() {}
B3.prototype.__t = "P1";
for (var i = 0; i < 40; i++) new B3();
var p1 = B3.prototype;
B3.prototype = { __t: "P2" };
for (var i = 0; i < 40; i++) new B3();
B3.prototype = p1;
say("3 " + tag(new B3()) + " " + (Object.getPrototypeOf(new B3()) === p1));

/* ---- 4. delete the constructor's OTHER own properties --------------- */
/* `length` and `name` are configurable on a function; deleting them raises
   deleted_prop_count and makes compact_properties() re-index the shape, so the
   index at which `prototype` lives CHANGES and prop_count SHRINKS.  This is
   the case that discriminates guards (1) and (2). */
function D() { this.d = 1; }
D.prototype.__t = "D";
for (var i = 0; i < 40; i++) new D();
delete D.length;
delete D.name;
say("4a " + D.hasOwnProperty("length") + " " + D.hasOwnProperty("name") + " " +
    D.hasOwnProperty("prototype"));
var s4 = "";
for (var i = 0; i < 40; i++) s4 += tag(new D());
say("4b " + s4.length + " " + s4.charAt(0) + " " +
    (Object.getPrototypeOf(new D()) === D.prototype));

/* ---- 5. grow the constructor, then delete most of it ---------------- */
/* Growth reallocates the constructor's shape (this is the exact motion that
   made the LAND-26 version hold a freed pointer); the deletions then compact
   it back down, so the cached index is both stale AND out of range. */
function E() { this.e = 1; }
E.prototype.__t = "E";
for (var i = 0; i < 20; i++) new E();
for (var k = 0; k < 60; k++) E["s" + k] = k;
for (var i = 0; i < 20; i++) new E();
for (var k = 0; k < 60; k++) delete E["s" + k];
delete E.length;
delete E.name;
var s5 = "";
for (var i = 0; i < 40; i++) s5 += tag(new E());
say("5 " + s5.length + " " + s5.charAt(0) + " " + E.hasOwnProperty("prototype") +
    " " + (Object.getPrototypeOf(new E()) === E.prototype));

/* ---- 6. the ordinary React shape: a static added after first render -- */
function SC() { this.mark = "SC"; }
SC.prototype.__t = "SC";
for (var i = 0; i < 30; i++) new SC();
SC.defaultProps = { a: 1 };
SC.displayName = "SC";
var s6 = "";
for (var i = 0; i < 30; i++) s6 += tag(new SC());
say("6 " + s6.length + " " + s6.charAt(0) + " " + SC.defaultProps.a);

/* ---- 7. class hierarchies and Reflect.construct ---------------------- */
class P { constructor() { this.p = 1; } }
P.prototype.__t = "P";
class Q extends P { constructor() { super(); this.q = 1; } }
Q.prototype.__t = "Q";
var s7 = "";
for (var i = 0; i < 30; i++) s7 += tag(new Q());
function NT() {}
NT.prototype = { __t: "NT" };
var r = Reflect.construct(P, [], NT);
say("7 " + s7.charAt(0) + s7.length + " " + tag(r) + " " +
    (Object.getPrototypeOf(r) === NT.prototype));

/* ---- 8. a Proxy constructor and a bound function --------------------- */
function Base() { this.b = 1; }
Base.prototype.__t = "Base";
var PX = new Proxy(Base, {
  get: function (t, k, rc) { return k === "prototype" ? { __t: "TRAP" } : Reflect.get(t, k, rc); }
});
var s8 = "";
for (var i = 0; i < 20; i++) s8 += tag(new PX());
var BF = Base.bind(null);
say("8 " + s8.charAt(0) + s8.length + " " + tag(new BF()) + " " +
    (Object.getPrototypeOf(new BF()) === Base.prototype));

/* ---- 9. prototype set to a NON-object, and back ---------------------- */
function N() {}
N.prototype.__t = "N";
for (var i = 0; i < 30; i++) new N();
var pn = N.prototype;
N.prototype = 42;
var n1 = new N();
say("9 " + (Object.getPrototypeOf(n1) === Object.prototype) + " " + tag(n1));
N.prototype = pn;
say("9b " + tag(new N()));

/* ---- 10. Object.setPrototypeOf on the CONSTRUCTOR object ------------- */
function S1() {}
S1.prototype.__t = "S1";
for (var i = 0; i < 30; i++) new S1();
Object.setPrototypeOf(S1, { extra: 7 });
say("10 " + tag(new S1()) + " " + S1.extra + " " +
    (Object.getPrototypeOf(new S1()) === S1.prototype));

/* ---- 11. 80 constructors: way thrash and eviction -------------------- */
var cs = [], mk = [];
for (var i = 0; i < 80; i++) {
  var C = new Function("this.i = " + i + ";");
  C.prototype.__t = "C" + i;
  cs.push(C); mk.push(0);
}
var bad = 0;
for (var round = 0; round < 6; round++)
  for (var i = 0; i < 80; i++)
    if (tag(new cs[i]()) !== "C" + i) bad++;
say("11 " + bad);

/* ---- 12. objects created from the cached empty shape stay extensible - */
function X() {}
X.prototype.__t = "X";
for (var i = 0; i < 30; i++) new X();
var x = new X();
x.added = 5;
say("12 " + x.added + " " + Object.isExtensible(x) + " " +
    Object.getOwnPropertyNames(x).join(",") + " " + tag(x));

/* ---- 13. subclassing a builtin -------------------------------------- */
class MyErr extends Error { constructor(m) { super(m); this.tagged = 1; } }
var e13 = "";
for (var i = 0; i < 20; i++) e13 = new MyErr("m").message;
say("13 " + e13 + " " + (new MyErr("m") instanceof Error));

/* ---- 14. a frozen constructor ---------------------------------------- */
function F() {}
F.prototype.__t = "F";
for (var i = 0; i < 30; i++) new F();
Object.freeze(F);
say("14 " + tag(new F()) + " " + Object.isFrozen(F));

/* ---- 15. GC with live entries ---------------------------------------- */
function G() { this.g = 1; }
G.prototype.__t = "G";
for (var i = 0; i < 40; i++) new G();
var keep = [];
for (var i = 0; i < 4000; i++) keep.push({ z: i });
keep = null;
var s15 = "";
for (var i = 0; i < 40; i++) s15 += tag(new G());
say("15 " + s15.length + " " + s15.charAt(0));

/* ====================================================================
   CASES 16-18 were added AFTER a mutation test showed cases 1-15 did not
   discriminate guards (1) and (2): every one of them either never triggers
   compact_properties() (which needs deleted_prop_count >= 8 AND >= half the
   properties) or leaves `prototype` at the index it started at, so a build
   with the bounds test or the atom test deleted passed silently.  Cases 16-18
   move `prototype` OUT of the cached index and put something else there.
   ==================================================================== */

/* ---- 16. compaction slides a DIFFERENT property into the cached index,
           and that property's value is the OLD prototype ---------------- */
/* This is the case that discriminates guard (2), the atom test, ALONE.  Guard
   (4) cannot catch it: the value at the cached index really IS the pinned
   prototype object -- it is just no longer the value of `prototype`. */
function AL() {}
AL.prototype.__t = "AL1";
for (var i = 0; i < 40; i++) new AL();       /* fill: idx = 2 (length,name,prototype) */
var P0 = AL.prototype;
delete AL.length;                            /* deleted = 1 */
AL.alias = P0;                               /* appended after `prototype`   */
for (var k = 0; k < 8; k++) AL["t" + k] = k;
for (var k = 0; k < 7; k++) delete AL["t" + k];  /* deleted = 8 -> compaction */
AL.prototype = { __t: "AL2" };
say("16 " + tag(new AL()) + " " + (AL.alias === P0) + " " +
    (Object.getPrototypeOf(new AL()) === AL.prototype));

/* ---- 17. compaction shrinks the constructor BELOW the cached index ---- */
/* This is the case that discriminates guard (1), the bounds test.  It costs
   memory safety rather than output: with the test deleted the engine reads a
   JSShapeProperty past the end of the shape allocation.  Run under ASan. */
function BD() {}
BD.prototype.__t = "BD";
for (var i = 0; i < 40; i++) new BD();       /* fill: idx = 2 */
delete BD.length;
delete BD.name;                              /* deleted = 2 */
for (var k = 0; k < 8; k++) BD["t" + k] = k;
for (var k = 0; k < 6; k++) delete BD["t" + k];  /* deleted = 8 -> compaction */
delete BD.t6;
delete BD.t7;
for (var k = 0; k < 6; k++) BD["u" + k] = k;
for (var k = 0; k < 6; k++) delete BD["u" + k];  /* deleted = 8 -> compaction */
/* NOT Object.getOwnPropertyNames(BD): node gives a non-strict function own
   `arguments` and `caller` properties and QuickJS does not, which is an engine
   difference with nothing to do with this mechanism. */
say("17a " + BD.hasOwnProperty("prototype") + " " + BD.hasOwnProperty("t7") + " " +
    BD.hasOwnProperty("length") + " " + BD.hasOwnProperty("u5"));
var s17 = "";
for (var i = 0; i < 20; i++) s17 += tag(new BD());
say("17b " + s17.length + " " + s17.charAt(0));

/* ---- 18. the same, but the constructor then regrows ------------------ */
function RG() {}
RG.prototype.__t = "RG1";
for (var i = 0; i < 40; i++) new RG();
delete RG.length;
delete RG.name;
for (var k = 0; k < 8; k++) RG["t" + k] = k;
for (var k = 0; k < 6; k++) delete RG["t" + k];
delete RG.t6;
delete RG.t7;
for (var k = 0; k < 6; k++) RG["u" + k] = k;
for (var k = 0; k < 6; k++) delete RG["u" + k];
RG.a = RG.prototype;                          /* a former-proto alias again */
RG.b = 1;
RG.prototype = { __t: "RG2" };
say("18 " + tag(new RG()) + " " + RG.hasOwnProperty("a") + RG.hasOwnProperty("b") +
    (RG.a === undefined) + " " + (Object.getPrototypeOf(new RG()) === RG.prototype));
