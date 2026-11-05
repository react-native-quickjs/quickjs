// An object literal in code that follows a return is never reached. The bytecode pass drops it, and the literal template table
// has to count only the literals that are still there: an assertion build failed on the first function, and a build without
// assertions paired every later literal of the function with the wrong template.
function afterReturn() { return 1; ({ a: 1 }); }
function mixed(x) {
  var live1 = { p: x, q: 2 };
  if (x > 100) return live1;
  var live2 = { r: x + 1, s: 3, t: 4 };
  return live1.p + live2.r + live2.t;
  var dead1 = { u: 1 };
  var dead2 = { v: 2, w: 3 };
  return dead1.u + dead2.v;
}
function thrown(x) {
  if (x) throw new Error("e");
  return { k: 1 };
  throw { code: 1 };
}
function loopy(n) {
  var out = [];
  for (var i = 0; i < n; i++) { out.push({ i: i, sq: i * i }); continue; out.push({ never: 1 }); }
  return out;
}
function nested() {
  return { outer: { inner: 1, more: 2 }, tail: 3 };
  return { dead: { x: 1 } };
}
print(afterReturn());
print(mixed(5), mixed(500).p);
try { thrown(1); } catch (e) { print("caught", e.message); }
print(JSON.stringify(thrown(0)));
print(JSON.stringify(loopy(3)));
print(JSON.stringify(nested()));
