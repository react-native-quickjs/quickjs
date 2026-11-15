/*
 * Mixed program: calls, methods, constructors and classes across the host's
 * compiled code and code compiled at run time, in both directions.
 */
function hostAdd(a, b) { return a + b; }
function hostApply(f, x, y) { return f(x, y) * 10; }
function HostPoint(x, y) { this.x = x; this.y = y; }
HostPoint.prototype.sum = function () { return this.x + this.y; };
class HostBase {
  constructor(n) { this.n = n; }
  twice() { return this.n * 2; }
  static make(n) { return new this(n); }
}
/* Payloads compiled at run time see the host through the global object. */
globalThis.hostAdd = hostAdd;
globalThis.hostApply = hostApply;
globalThis.HostPoint = HostPoint;
globalThis.HostBase = HostBase;
var payload = (0, eval)([
  "function evAdd(a, b) { return hostAdd(a, b) + 1; }",
  "function EvPoint(x, y) { HostPoint.call(this, x, y); this.z = x * y; }",
  "EvPoint.prototype = Object.create(HostPoint.prototype);",
  "EvPoint.prototype.all = function () { return this.sum() + this.z; };",
  "class EvDerived extends HostBase {",
  "  constructor(n) { super(n + 1); this.tag = 'ev'; }",
  "  twice() { return super.twice() + 100; }",
  "}",
  "({ evAdd: evAdd, EvPoint: EvPoint, EvDerived: EvDerived,",
  "   viaHost: hostApply(evAdd, 2, 3), method: new HostPoint(4, 5).sum() })"
].join("\n"));
print(payload.viaHost, payload.method);
print(hostApply(payload.evAdd, 1, 1));
var p = new payload.EvPoint(3, 4);
print(p.all(), p instanceof HostPoint, p.sum());
var d = payload.EvDerived.make(6);
print(d.twice(), d.tag, d instanceof HostBase, d.constructor.name);
class HostDerived extends payload.EvDerived {
  twice() { return super.twice() + 1000; }
}
print(new HostDerived(1).twice());
var mul = new Function("a", "b", "return hostAdd(a, 0) * b;");
print(hostApply(mul, 6, 7), [1, 2, 3].map(payload.evAdd.bind(null, 10)).join(","));
function hostDirect(k) { var local = k * 3; return eval("local + hostAdd(k, 1)"); }
print(hostDirect(5));
