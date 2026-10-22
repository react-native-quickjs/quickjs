var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 8470;
function Coll(n) { this.n = n; }
Coll.prototype[Symbol.iterator] = function () {
  var i = 0, n = this.n;
  return { next: function () { return i < n ? { value: i++ * 3, done: false } : { value: undefined, done: true }; }, return: function () { return {}; } };
};
var tot = 0;
for (var i = 0; i < N; i++) {
  var c = new Coll(i % 10 + 1);
  for (var v of c) tot += v;
  var [a, b, ...rest] = c;
  tot += a + (b || 0) + rest.length;
  tot += Math.max(...c);
  for (var x of c) { if (x > 6) break; tot++; }
  tot += Array.from(c).length + new Set(c).size;
}
mix(tot);
print('iterators-protocol ' + h);
