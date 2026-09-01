// 0313 -- the element-target cache's barrier corpus.
//
// Each arm mutates a >=1024-element array through a DIFFERENT store path, with
// enough allocation on either side that a collection lands between the mutation
// and the read-back.  If the cache is trusted while stale, the replaced element
// is never marked, is swept, and the read-back sees corrupted or recycled
// memory.  Every arm prints OK/FAIL so a mutant build fails loudly rather than
// by signal.
//
// The arms exist because they route through different store sites, and the
// corpus is only as good as the sites it reaches: arm A alone does NOT
// discriminate a removal at JS_SetPropertyValue, because `a[i] = x` in a loop
// is served by the interpreter's own inline store in quickjs-tos-body.h.
var N = 2048, ROUNDS = 60, CHURN = 120000;
function churn() { var i, s = 0, t; for (i = 0; i < CHURN; i++) { t = { pad: i }; if (i < 0) s += t.pad; } return s; }
function mk(k, i) { return { v: k * 100000 + i, s: "s" + k + "_" + i }; }
function chk(a, k, i, arm) {
  var e = a[i];
  if (!e || e.v !== k * 100000 + i || e.s !== "s" + k + "_" + i) {
    print("FAIL " + arm + " k=" + k + " i=" + i + " got=" + (e && e.v)); return false;
  }
  return true;
}
function fresh() { var a = new Array(N), i; for (i = 0; i < N; i++) a[i] = mk(0, i); return a; }
var ok = true, k, i;

// A -- the interpreter's inline element store (quickjs-tos-body.h)
var a = fresh();
for (k = 1; k <= ROUNDS && ok; k++) {
  churn();
  for (i = 0; i < N; i += 5) a[i] = mk(k, i);
  churn();
  for (i = 0; i < N && ok; i += 5) ok = chk(a, k, i, "A/inline-store");
}
if (ok) print("OK A");

// B -- Reflect.set, which does NOT use the interpreter's inline store
var b = fresh();
for (k = 1; k <= ROUNDS && ok; k++) {
  churn();
  for (i = 1; i < N; i += 5) Reflect.set(b, i, mk(k, i));
  churn();
  for (i = 1; i < N && ok; i += 5) ok = chk(b, k, i, "B/Reflect.set");
}
if (ok) print("OK B");

// C -- growth through push (add_fast_array_element / expand_fast_array)
var c = fresh();
for (k = 1; k <= 20 && ok; k++) {
  churn();
  var base = c.length;
  for (i = 0; i < 64; i++) c.push(mk(k, base + i));
  churn();
  for (i = 0; i < 64 && ok; i++) ok = chk(c, k, base + i, "C/push");
}
if (ok) print("OK C");

// D -- bulk element movement (JS_CopySubArray via copyWithin), then read back
var d = fresh();
for (k = 1; k <= 20 && ok; k++) {
  churn();
  for (i = 0; i < 128; i++) d[i] = mk(k, i);
  d.copyWithin(N / 2, 0, 128);          // moves live refs to indices the
  churn();                              // cached list never saw at those slots
  for (i = 0; i < 128 && ok; i++) {
    var e = d[(N / 2) + i];
    if (!e || e.v !== k * 100000 + i) { print("FAIL D/copyWithin k=" + k + " i=" + i); ok = false; }
  }
}
if (ok) print("OK D");

// E -- splice and fill
var f = fresh();
for (k = 1; k <= 20 && ok; k++) {
  churn();
  f.splice(10, 0, mk(k, 10), mk(k, 11));
  churn();
  var e0 = f[10], e1 = f[11];
  if (!e0 || e0.v !== k * 100000 + 10 || !e1 || e1.v !== k * 100000 + 11) { print("FAIL E/splice k=" + k); ok = false; }
}
if (ok) print("OK E");

print(ok ? "ALL OK" : "CORPUS FAILED");
