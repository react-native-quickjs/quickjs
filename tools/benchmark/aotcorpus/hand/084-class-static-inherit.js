var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 40000;
class Shape {
  static registry = [];
  static make(kind, ...a) { var s = new kind(...a); Shape.registry.push(s.name()); return s; }
  area() { return 0; }
  name() { return this.constructor.name; }
  describe() { return this.name() + ":" + (this.area() | 0); }
}
class Rect extends Shape { constructor(w, h) { super(); this.w = w; this.h = h; } area() { return this.w * this.h; } }
class Square extends Rect { constructor(s) { super(s, s); } name() { return "Sq" + super.name(); } }
class Circle extends Shape { #r; constructor(r) { super(); this.#r = r; } area() { return Math.PI * this.#r * this.#r; } static unit() { return new this(1); } }
var t = 0;
for (var i = 0; i < N; i++) {
  var s = [Shape.make(Rect, i % 5, 3), Shape.make(Square, i % 7), Shape.make(Circle, i % 4)];
  t += s[0].area() + s[1].area() + s[2].area();
  if (i % 1000 == 0) mixs(s.map(function (x) { return x.describe(); }).join());
  if (Shape.registry.length > 100) Shape.registry.length = 0;
}
mixd(t); mixs(Circle.unit().describe());
print('class-static-inherit ' + h);
