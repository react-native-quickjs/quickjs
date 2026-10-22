var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 177806;
function Even() {}
Object.defineProperty(Even, Symbol.hasInstance, { value: function (n) { return n % 2 == 0; } });
class A {} class B extends A {} class C extends B {}
var objs = [new A(), new B(), new C(), {}, [], null, 3, 4];
var t = 0;
for (var i = 0; i < N; i++) {
  var o = objs[i & 7];
  t += (o instanceof A ? 1 : 0) + (o instanceof B ? 2 : 0) + (o instanceof C ? 4 : 0) + (o instanceof Even ? 8 : 0);
  t += (o instanceof Object) ? 16 : 0;
}
mix(t);
print('instanceof-hasinstance ' + h);
