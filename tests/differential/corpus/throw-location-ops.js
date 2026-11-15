/*
 * Differential corpus: the frame that performs a read, a write or a coercion
 * keeps its source position when the getter, setter, trap or conversion it
 * triggers throws. Each case reads through a different instruction form: a
 * named field of an argument, a local and a global, a computed element with a
 * string and an integer key, a store through a setter, `in` and `delete`
 * through proxy traps, and the coercions of arithmetic, comparison and
 * string concatenation.
 *
 * Only the frame's name and line are compared, as in accessor-throw-stack.js.
 */
function frames(e) {
  var out = [];
  var ls = String(e.stack).split("\n");
  for (var i = 0; i < ls.length; i++) {
    var m = /^\s*at\s+(.*?)\s+\((.*):(\d+):(\d+)\)\s*$/.exec(ls[i]);
    if (!m) continue;
    var name = m[1].replace(/\s*\[as\s+[^\]]+\]\s*$/, "").replace(/^(Object|Array)\./, "");
    out.push(name + "@" + m[3]);
    if (out.length === 3) break;
  }
  return out.join(" | ");
}
function boom() { throw new Error("boom"); }
var getter = { get x() { return boom(); } };
var setter = { set x(v) { boom(); } };
var arr = []; Object.defineProperty(arr, 0, { get: function () { return boom(); } });
var conv = { valueOf: function () { return boom(); }, toString: function () { return boom(); } };
var trap = new Proxy({}, { has: function () { return boom(); }, deleteProperty: function () { return boom(); },
                           ownKeys: function () { return boom(); } });
var g = getter;

function argField(o) { return o.x; }
function localField() { var o = getter; return o.x; }
function globalField() { return g.x; }
function elemString(o) { var k = "x"; return o[k]; }
function elemInt(a) { return a[0]; }
function storeField(o) { o.x = 1; return 0; }
function storeElem(o) { var k = "x"; o[k] = 1; return 0; }
function inOp(p) { return "a" in p; }
function deleteOp(p) { return delete p.a; }
function addOp(v) { return v + 1; }
function mulOp(v) { return v * 2; }
function lessOp(v) { return v < 3; }
function concatOp(v) { return "s" + v; }
function forIn(p) { var n = 0; for (var k in p) n++; return n; }

var cases = [
  [argField, getter], [localField], [globalField], [elemString, getter], [elemInt, arr],
  [storeField, setter], [storeElem, setter], [inOp, trap], [deleteOp, trap],
  [addOp, conv], [mulOp, conv], [lessOp, conv], [concatOp, conv], [forIn, trap]
];
for (var i = 0; i < cases.length; i++) {
  try {
    cases[i][0](cases[i][1]);
    print(cases[i][0].name + ": no throw");
  } catch (e) {
    print(cases[i][0].name + ": " + frames(e));
  }
}
