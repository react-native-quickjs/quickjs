var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 30000;
var tot = 0;
function f({ a, b: { c = 5, d } = {}, ...r }, [x, , y = 9] = []) { return a + c + (d || 0) + x + y + Object.keys(r).length; }
for (var i = 0; i < N; i++) {
  tot += f({ a: i, b: { d: 2 }, e: 1, g: 2 }, [1, 2]);
  tot += f({ a: 1, b: { c: i % 3 } }, [4, 5, 6]);
  var [p, q] = [q0(i), p0(i)];
  tot += p + q;
  var t1 = 1, t2 = 2;
  [t1, t2] = [t2, t1 + i % 5];
  tot += t1 * 10 + t2;
}
function q0(i) { return i & 3; } function p0(i) { return i & 5; }
mix(tot);
print('destructuring ' + h);
