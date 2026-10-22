var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 234264;
var o = {
  _v: 1,
  get v() { return this._v; },
  set v(x) { this._v = (x * 3 + 1) % 10007; },
  get dbl() { return this.v * 2; }
};
var t = 0;
for (var i = 0; i < N; i++) {
  o.v = o.v + i;
  t = (t + o.dbl) | 0;
}
mix(t); mix(o.v);
print('getters-setters ' + h);
