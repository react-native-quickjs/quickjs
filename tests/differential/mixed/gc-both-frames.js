/*
 * Mixed program: garbage made and released while frames of both kinds are
 * live on the stack, objects linked across the boundary, and cycles through
 * closures of both kinds. Run with --leak-check and the GC stress modes.
 */
function HostNode(v, next) { this.v = v; this.next = next; }
var evBuild = (0, eval)(
  "(function evBuild(n, mk) {" +
  "  var head = null;" +
  "  for (var i = 0; i < n; i++) head = mk(i, head);" +
  "  var junk = [];" +
  "  for (var j = 0; j < n; j++) junk.push({ j: j, s: 'x' + j, a: [j, j] });" +
  "  return head;" +
  "})");
function hostSum(list) { var s = 0; while (list) { s += list.v; list = list.next; } return s; }
var total = 0;
for (var round = 0; round < 30; round++) {
  var list = evBuild(200, function (v, next) { return new HostNode(v, next); });
  total += hostSum(list);
}
print(total);
var evCycle = new Function("H", "var a = { name: 'a' }; var b = { name: 'b', a: a }; a.b = b;" +
                                 "a.f = function () { return b.name; }; b.g = function () { return a.name; };" +
                                 "a.h = new H(1, b); return a;");
var keep = [];
for (var k = 0; k < 500; k++) { var cyc = evCycle(HostNode); if (k % 100 === 0) keep.push(cyc.f() + cyc.b.g() + cyc.h.v); }
print(keep.join(","));
function hostDeep(n) { return n === 0 ? evLeaf() : hostDeep(n - 1) + 1; }
var evLeaf = new Function("var o = []; for (var i = 0; i < 1000; i++) o.push({ i: i }); return o.length;");
print(hostDeep(50));
