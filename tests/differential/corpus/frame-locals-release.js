/*
 * Differential corpus: a function whose locals still hold objects when it
 * returns or throws. Every tier that keeps its variables in the frame must
 * release them on the way out; run with --leak-check, a slot that is not
 * released keeps the object (and through its prototype the whole realm) alive
 * at teardown.
 *
 * The shapes cover `var`, `let` and `const`, a block-scoped binding, a catch
 * binding, an object kept past a nested call, a local array, a captured local,
 * and an exit by `throw`.
 */
function varObj() { var o = {}; return 1; }
function letObj() { let o = { a: 1 }; return o.a; }
function constArr() { const a = [1, 2, 3]; return a.length; }
function blockObj(n) { var s = 0; for (var i = 0; i < n; i++) { let o = { i: i }; s += o.i; } return s; }
function catchObj() { try { throw { m: 2 }; } catch (e) { var keep = e; } return keep.m; }
function nested() { var o = { v: 3 }; var r = varObj(); return o.v + r; }
function captured() { var o = { v: 4 }; var f = function () { return o.v; }; return f(); }
function many() {
  var a = {}, b = {}, c = [], d = { x: {} }, e = function () {}, f = new Date(0), g = /x/;
  return typeof a + typeof b + c.length + typeof d.x + typeof e + f.getTime() + g.source;
}
function throwsWithObj() { var o = { t: 5 }; throw new Error("exit " + o.t); }

print(varObj(), letObj(), constArr(), blockObj(4), catchObj(), nested(), captured());
print(many());
try { throwsWithObj(); } catch (err) { print(err.message); }
