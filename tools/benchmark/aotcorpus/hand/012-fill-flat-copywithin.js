var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 4338;
for (var i = 0; i < N; i++) {
  var a = new Array(12).fill(i % 5);
  a.fill(9, 3, 6);
  a.copyWithin(0, 4, 8);
  var n = [[1, [2, 3]], [4, [5, [6]]], i];
  mixs(a); mixs(n.flat()); mixs(n.flat(Infinity)); mix(n.flat(2).length);
  mixs([3, 1, 2].reverse());
  mixs(Array.from({ length: 5 }, function (_, k) { return k * i; }));
  mixs(Array.of(i, i + 1).concat([[0]]).flatMap(function (x) { return [x, x]; }));
}
print('fill-flat-copywithin ' + h);
