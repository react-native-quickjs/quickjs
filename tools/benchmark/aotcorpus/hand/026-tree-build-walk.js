var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 101;
function build(d, v) { return d == 0 ? null : { l: build(d - 1, v * 2), r: build(d - 1, v * 2 + 1), v: v }; }
function sum(t) { return t == null ? 0 : t.v + sum(t.l) + sum(t.r); }
function depth(t) { return t == null ? 0 : 1 + Math.max(depth(t.l), depth(t.r)); }
function inorder(t, out) { if (t) { inorder(t.l, out); out.push(t.v); inorder(t.r, out); } return out; }
for (var i = 0; i < N; i++) {
  var t = build(10 + i % 3, 1);
  mix(sum(t)); mix(depth(t));
  mix(inorder(t, []).length);
}
print('tree-build-walk ' + h);
