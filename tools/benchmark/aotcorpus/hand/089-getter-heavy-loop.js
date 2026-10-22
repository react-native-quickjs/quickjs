var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 150000;
function P(x, y) { this._x = x; this._y = y; }
Object.defineProperty(P.prototype, "x", { get: function () { return this._x; }, set: function (v) { this._x = v; } });
Object.defineProperty(P.prototype, "y", { get: function () { return this._y; } });
Object.defineProperty(P.prototype, "norm", { get: function () { return this.x * this.x + this.y * this.y; } });
var ps = [new P(1, 2), new P(3, 4), new P(-1, 5)];
var t = 0;
for (var i = 0; i < N; i++) {
  var p = ps[i % 3];
  p.x = (p.x + 1) % 50;
  t = (t + p.norm + p.y) | 0;
}
mix(t);
print('getter-heavy-loop ' + h);
