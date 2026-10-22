var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 139569;
function add(a, b, c) { return (this.base || 0) + a + b + (c || 0); }
var o1 = { base: 10 }, o2 = { base: 20 };
var b1 = add.bind(o1, 1), b2 = add.bind(o2, 1, 2);
var tot = 0;
for (var i = 0; i < N; i++) {
  tot = (tot + add.call(o1, i, 1) + add.apply(o2, [i, 2, 3]) + b1(i) + b2() + b1.call(o2, 5, 6)) | 0;
}
mix(tot);
mix(b1.name.length); mix(b2.length);
var Ctor = function (x) { this.x = x; };
var BC = Ctor.bind(null, 5);
mix(new BC().x);
print('call-apply-bind ' + h);
