var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 130198;
function LRU(cap) { this.cap = cap; this.m = new Map(); }
LRU.prototype.get = function (k) { if (!this.m.has(k)) return -1; var v = this.m.get(k); this.m.delete(k); this.m.set(k, v); return v; };
LRU.prototype.put = function (k, v) { if (this.m.has(k)) this.m.delete(k); else if (this.m.size >= this.cap) this.m.delete(this.m.keys().next().value); this.m.set(k, v); };
var c = new LRU(64), hit = 0;
for (var i = 0; i < N; i++) {
  var k = (i * i + (i >> 3)) % 97;
  if (c.get(k) >= 0) hit++; else c.put(k, i);
}
mix(hit); mix(c.m.size);
print('lru-cache-map ' + h);
