var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 268250;
function A() { this.a = 1; }
A.prototype.f = function () { return this.a + 1; };
A.prototype.g = function () { return this.f() * 2; };
function B() { A.call(this); this.b = 2; }
B.prototype = Object.create(A.prototype);
B.prototype.constructor = B;
B.prototype.f = function () { return A.prototype.f.call(this) + this.b; };
function C() { B.call(this); this.c = 3; }
C.prototype = Object.create(B.prototype);
var objs = [new A(), new B(), new C()];
var t = 0;
for (var i = 0; i < N; i++) {
  var o = objs[i % 3];
  t = (t + o.g() + o.f()) | 0;
}
mix(t);
mix(objs[2] instanceof A ? 1 : 0); mix(Object.getPrototypeOf(objs[2]) === C.prototype ? 1 : 0);
mix(A.prototype.isPrototypeOf(objs[1]) ? 1 : 0);
print('proto-chain ' + h);
