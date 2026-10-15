// Objects: literals, named reads and writes, hashed shapes, in, delete.
function k(n) {
  var o = { a: 1, b: 2, c: 3 }, s = 0, i, p;
  for (i = 0; i < n; i++) {
    o.a = (o.a + i) | 0;
    s = (s + o.b) | 0;
    s = (s + o.c) | 0;
    p = { x: i, y: i + 1 };
    s = (s + p.x - p.y) | 0;
  }
  var q = { m: 1 };
  var has = "m" in q ? 1 : 0;
  delete q.m;
  var after = "m" in q ? 1 : 0;
  var plain = {};
  plain.z = 9;
  return [o.a, s, has, after, plain.z, Object.keys({ k1: 1, k2: 2 }).join("+")];
}
print(k(5000));
