// Globals: reads and writes, top-level let/const, global functions.
var gv = 11;
let gl = 3;
const gc = 4;
function bump() { gv = (gv + 1) | 0; }
function k(n) {
  var s = 0, i;
  for (i = 0; i < n; i++) { s = (s + gv) | 0; bump(); }
  return [s, gv, gl, gc, typeof missing === "undefined"];
}
print(k(3000));
