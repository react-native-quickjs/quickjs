var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 38461;
var tot = 0;
for (var i = 0; i < N; i++) {
  var o = {};
  Object.defineProperty(o, "ro", { value: i, writable: false, enumerable: false, configurable: true });
  o.ro = 5;
  tot += o.ro;
  Object.defineProperty(o, "g", { get: function () { return this.ro * 2; }, enumerable: true, configurable: true });
  tot += o.g;
  Object.defineProperty(o, "ro", { value: 1, writable: true });
  o.ro = 8;
  tot += o.ro + Object.keys(o).length;
  var f = Object.freeze({ q: 1 });
  f.q = 2; f.r = 3;
  tot += f.q + (f.r === undefined ? 1 : 0) + (Object.isFrozen(f) ? 1 : 0);
  var dsc = Object.getOwnPropertyDescriptor(o, "ro");
  tot += (dsc.writable ? 1 : 0) + (dsc.enumerable ? 1 : 0) + (dsc.configurable ? 1 : 0);
}
mix(tot);
print('define-property ' + h);
