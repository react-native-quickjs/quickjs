var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 28826;
class Vec {
  x; y;
  static count = 0;
  static #hidden = 5;
  #len = 0;
  constructor(x, y) { this.x = x; this.y = y; Vec.count++; this.#len = x * x + y * y; }
  get len2() { return this.#len; }
  add(o) { return new Vec(this.x + o.x, this.y + o.y); }
  static hid() { return Vec.#hidden; }
  toString() { return "(" + this.x + "," + this.y + ")"; }
}
class Vec3 extends Vec {
  z = 7;
  constructor(x, y, z) { super(x, y); this.z = z; }
  add(o) { var r = super.add(o); return new Vec3(r.x, r.y, this.z + (o.z || 0)); }
  get len2() { return super.len2 + this.z * this.z; }
}
var v = new Vec(1, 2), w = new Vec3(1, 1, 1);
var t = 0;
for (var i = 0; i < N; i++) {
  v = v.add(new Vec(1, 0));
  w = w.add(new Vec3(0, 1, 1));
  t = (t + v.len2 + w.len2) | 0;
}
mix(t); mix(Vec.count); mix(Vec.hid()); mixs(v); mixs(w);
print('classes-basic ' + h);
