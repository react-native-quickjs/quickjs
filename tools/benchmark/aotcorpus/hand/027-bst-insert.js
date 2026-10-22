var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 72470;
var seed = 17;
function rnd() { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed >> 3; }
function Node(k) { this.k = k; this.l = null; this.r = null; }
function insert(root, k) {
  if (!root) return new Node(k);
  var c = root;
  for (;;) {
    if (k < c.k) { if (c.l) c = c.l; else { c.l = new Node(k); break; } }
    else if (k > c.k) { if (c.r) c = c.r; else { c.r = new Node(k); break; } }
    else break;
  }
  return root;
}
var root = null;
for (var i = 0; i < N; i++) root = insert(root, rnd() % 100000);
function height(n) { return n ? 1 + Math.max(height(n.l), height(n.r)) : 0; }
function count(n) { return n ? 1 + count(n.l) + count(n.r) : 0; }
mix(height(root)); mix(count(root));
var s = 0;
for (var i = 0; i < N; i++) { var c = root, k = rnd() % 100000; while (c && c.k != k) c = k < c.k ? c.l : c.r; s += c ? 1 : 0; }
mix(s);
print('bst-insert ' + h);
