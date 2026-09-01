// 0315 -- the sticky-mark remembered-set barrier corpus.
//
// Under sticky marking a MINOR does not re-traverse anything the previous cycle
// marked.  So if an OLD (already marked) container acquires a reference to a
// YOUNG object and the barrier does not remember the container, the minor never
// visits the container, the young object is never marked, and the sweep frees
// it while it is still reachable.
//
// ⚠ THE HARD PART IS MAKING THE YOUNG OBJECT REACHABLE ONLY THROUGH THE OLD
// CONTAINER.  A first version of this file stored fresh objects into old
// containers from the top level and PASSED WITH THE ENTIRE BARRIER REMOVED --
// `swept_obj` was byte-identical (12,527,431) between the correct build and the
// no-barrier mutant, because the fresh values were still live on the JS value
// stack and the conservative root scan found them directly.  A corpus that a
// whole-barrier mutant survives is measuring nothing.
//
// So: every store happens inside a helper that returns undefined and keeps no
// local, the containers live in one long-lived array, and a deep churn between
// the store and the read-back overwrites the value stack.  The arms are one per
// BARRIER SITE CLASS, so a mutant that removes only JS_WBTV fails the element
// arm and passes the property arm.
var CHURN = 40000, ROUNDS = 60, NC = 24;
function churn() {                       // also buries any stale stack slot
  var i, s = 0, t;
  for (i = 0; i < CHURN; i++) { t = { a: i, b: i + 1, c: i + 2 }; if (i < 0) s += t.a; }
  return s;
}
var KEEP = [];                            // the only root for every container
var ok = true;
function fail(arm, k, i, got) { print("FAIL " + arm + " k=" + k + " i=" + i + " got=" + got); ok = false; }

// --- helpers that keep NO local reference to what they allocate -------------
function putProp(o, k)  { o.slot = { tag: "A" + k, pad: [k, k, k] }; }
function putElem(a, i, k) { a[i] = { tag: "B" + k + "_" + i, pad: [k, k, k] }; }
function putStr(o, k)   { o.s = ("young" + k + "|").repeat(11); }
function putMap(m, k)   { m.set("k" + k, { tag: "D" + k, pad: [k, k, k] }); }
function putBox(b, k)   { b.set({ tag: "E" + k, pad: [k, k, k] }); }

// build the containers, then age them with two full cycles' worth of churn
(function build() {
  var i;
  for (i = 0; i < NC; i++) KEEP.push({});                       // A: objects
  for (i = 0; i < NC; i++) { var a = new Array(32), j;
    for (j = 0; j < 32; j++) a[j] = j; KEEP.push(a); }          // B: arrays
  for (i = 0; i < NC; i++) KEEP.push({ s: "seed" });            // C: string slot
  for (i = 0; i < NC; i++) KEEP.push(new Map());                // D: Map
  for (i = 0; i < NC; i++) {
    var cell = null;
    KEEP.push({ set: function (v) { cell = v; }, get: function () { return cell; } });
  }                                                             // E: closure
})();
churn(); churn(); churn();

var A0 = 0, B0 = NC, C0 = 2 * NC, D0 = 3 * NC, E0 = 4 * NC;
for (var k = 1; k <= ROUNDS && ok; k++) {
  var i;
  for (i = 0; i < NC; i++) putProp(KEEP[A0 + i], k);
  for (i = 0; i < NC; i++) putElem(KEEP[B0 + i], (k * 7) % 32, k);
  for (i = 0; i < NC; i++) putStr(KEEP[C0 + i], k);
  for (i = 0; i < NC; i++) putMap(KEEP[D0 + i], k);
  for (i = 0; i < NC; i++) putBox(KEEP[E0 + i], k);
  churn(); churn();
  for (i = 0; i < NC && ok; i++) {
    var o = KEEP[A0 + i].slot;
    if (!o || o.tag !== "A" + k || o.pad[2] !== k) fail("A/prop", k, i, o && o.tag);
  }
  for (i = 0; i < NC && ok; i++) {
    var e = KEEP[B0 + i][(k * 7) % 32];
    if (!e || e.tag !== "B" + k + "_" + ((k * 7) % 32) || e.pad[2] !== k) fail("B/elem", k, i, e && e.tag);
  }
  for (i = 0; i < NC && ok; i++) {
    var s = KEEP[C0 + i].s;
    if (typeof s !== "string" || s.indexOf("young" + k + "|") !== 0) fail("C/string", k, i, typeof s);
  }
  for (i = 0; i < NC && ok; i++) {
    var v = KEEP[D0 + i].get("k" + k);
    if (!v || v.tag !== "D" + k || v.pad[2] !== k) fail("D/map", k, i, v && v.tag);
  }
  for (i = 0; i < NC && ok; i++) {
    var g = KEEP[E0 + i].get();
    if (!g || g.tag !== "E" + k || g.pad[2] !== k) fail("E/closure", k, i, g && g.tag);
  }
}
print(ok ? "ALL OK" : "CORPUS FAILED");
