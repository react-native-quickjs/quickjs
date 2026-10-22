var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 3000;
function List() { this.head = null; this.n = 0; }
List.prototype.push = function (v) { this.head = { v: v, next: this.head }; this.n++; };
List.prototype.reverse = function () { var p = null, c = this.head; while (c) { var nx = c.next; c.next = p; p = c; c = nx; } this.head = p; };
List.prototype.sum = function () { var s = 0; for (var c = this.head; c; c = c.next) s += c.v; return s; };
List.prototype.toArray = function () { var a = []; for (var c = this.head; c; c = c.next) a.push(c.v); return a; };
var t = 0;
for (var r = 0; r < N; r++) {
  var l = new List();
  for (var i = 0; i < 100; i++) l.push(i * r % 17);
  l.reverse();
  t += l.sum() + l.toArray()[3] + l.n;
}
mix(t);
print('linked-list ' + h);
