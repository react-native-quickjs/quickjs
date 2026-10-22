var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 641;
var seed = 9;
function rnd() { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed >> 4; }
function Heap() { this.a = []; }
Heap.prototype.push = function (x) { var a = this.a; a.push(x); var i = a.length - 1; while (i > 0) { var p = (i - 1) >> 1; if (a[p][0] <= a[i][0]) break; var t = a[p]; a[p] = a[i]; a[i] = t; i = p; } };
Heap.prototype.pop = function () { var a = this.a, top = a[0], last = a.pop(); if (a.length) { a[0] = last; var i = 0, n = a.length; for (;;) { var l = 2 * i + 1, r = l + 1, m = i; if (l < n && a[l][0] < a[m][0]) m = l; if (r < n && a[r][0] < a[m][0]) m = r; if (m == i) break; var t = a[m]; a[m] = a[i]; a[i] = t; i = m; } } return top; };
var V = 400, adj = [];
for (var i = 0; i < V; i++) adj.push([]);
for (var i = 0; i < V * 5; i++) { var a = rnd() % V, b = rnd() % V, w = 1 + rnd() % 50; adj[a].push([b, w]); adj[b].push([a, w]); }
for (var r = 0; r < N; r++) {
  var dist = new Array(V).fill(Infinity), h2 = new Heap();
  dist[r % V] = 0; h2.push([0, r % V]);
  while (h2.a.length) {
    var e = h2.pop(), d = e[0], u = e[1];
    if (d > dist[u]) continue;
    for (var k = 0; k < adj[u].length; k++) { var v = adj[u][k][0], nd = d + adj[u][k][1]; if (nd < dist[v]) { dist[v] = nd; h2.push([nd, v]); } }
  }
  var s = 0; for (var i = 0; i < V; i++) if (dist[i] != Infinity) s += dist[i];
  mix(s);
}
print('dijkstra-heap ' + h);
