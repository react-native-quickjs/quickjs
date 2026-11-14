// A running generator's stack (stack VM) holds an object literal under construction while the next allocation starts a young
// collection. The frame lives in the generator's heap state, not on the native stack, so the stack scan did not find the
// nursery object and the collection freed it; a later read of the reused cell failed. The second part parks the generator
// in the middle of an object literal (`{ a: i, b: yield 0, ... }`): the unfilled literal is a nursery object that only the
// suspended frame names, and a young collection while it waited freed it. Found while reproducing JetStream 3
// babel-minify-wtb, whose config loading runs through generators.
"use strict";
var M = (function () {
  function Desc(name, opts) { this.name = name; this.options = opts; }
  function* load(i) {
    var desc = new Desc("p" + i, { code: i, nested: { v: i * 2 } });
    var cfg = { a: i, b: [i, i + 1] };
    yield 1;
    var extra = { code: i + 1 };
    yield 2;
    return desc.options.code + desc.options.nested.v + cfg.b[1] + extra.code;
  }
  return { load: load };
})();
var gens = [], sum = 0, junk = 0;
for (var round = 0; round < 4; round++) {
  gens.length = 0;
  for (var i = 0; i < 20000; i++) { var g = M.load(i); g.next(); gens.push(g); }
  for (var i = 0; i < 20000; i++) { var t = { x: i, y: [i] }; junk += t.y[0] & 1; }
  for (var i = 0; i < gens.length; i++) gens[i].next();
  for (var i = 0; i < 20000; i++) { var t2 = { x: i, z: i }; junk += t2.z & 1; }
  for (var i = 0; i < gens.length; i++) sum += gens[i].next().value;
}
print("generator-frame-young-object " + sum + " " + junk);

function* lit(i) { var o = { a: i, b: yield 0, c: [i, i + 1] }; return o.a + o.b + o.c[1]; }
var lsum = 0, ljunk = 0;
for (var round = 0; round < 4; round++) {
  var parked = [];
  for (var i = 0; i < 30000; i++) { var lg = lit(i); lg.next(); parked.push(lg); }
  for (var i = 0; i < 200000; i++) { var t3 = { p: i, q: [i] }; ljunk += t3.q[0] & 1; }
  for (var i = 0; i < parked.length; i++) lsum += parked[i].next(1).value;
}
print("generator-frame-young-object parked " + lsum + " " + ljunk);
