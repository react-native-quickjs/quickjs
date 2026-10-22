var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 7537;
var queue = [], tot = 0;
function Prom() { this.state = 0; this.val = undefined; this.cbs = []; }
Prom.prototype.settle = function (st, v) {
  if (this.state) return;
  this.state = st; this.val = v;
  var self = this;
  this.cbs.forEach(function (cb) { queue.push(function () { cb(self); }); });
  this.cbs = [];
};
Prom.prototype.then = function (ok, bad) {
  var np = new Prom();
  var cb = function (src) {
    var f = src.state == 1 ? ok : bad;
    if (!f) { np.settle(src.state, src.val); return; }
    try { np.settle(1, f(src.val)); } catch (e) { np.settle(2, e); }
  };
  if (this.state) { var self = this; queue.push(function () { cb(self); }); } else this.cbs.push(cb);
  return np;
};
function resolved(v) { var p = new Prom(); p.settle(1, v); return p; }
for (var i = 0; i < N; i++) {
  resolved(i).then(function (x) { return x * 2; }).then(function (x) { if (x % 7 == 0) throw new Error("e" + x); return x + 1; })
    .then(null, function (e) { return e.message.length; }).then(function (x) { tot += x; });
}
var steps = 0;
while (queue.length) { var job = queue.shift(); job(); steps++; }
mix(tot); mix(steps);
print('microtask-queue ' + h);
