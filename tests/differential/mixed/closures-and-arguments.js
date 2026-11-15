/*
 * Mixed program: closures created on one side capture and write variables the
 * other side also writes; arguments objects, rest and spread cross the boundary.
 */
function hostCounter() {
  var count = 0;
  return {
    inc: function () { return ++count; },
    get: function () { return count; },
    evalInc: new Function("c", "return c.inc() + c.inc();")
  };
}
var c = hostCounter();
print(c.evalInc(c), c.get());
var mk = (0, eval)(
  "(function (start) {" +
  "  var shared = start;" +
  "  return { bump: function (n) { shared += n; return shared; }," +
  "           peek: function () { return shared; } };" +
  "})");
var box = mk(10);
function hostBump(b, n) { return b.bump(n) + b.peek(); }
print(hostBump(box, 5), box.peek());
function hostCapture() {
  var v = 1;
  var setV = eval("(function (x) { v = x; return v; })");
  setV(42);
  return v;
}
print(hostCapture());
function hostArgs() { return arguments.length + ":" + Array.prototype.join.call(arguments, "-"); }
/* Payloads compiled at run time see the host through the global object. */
globalThis.hostArgs = hostArgs;
var evRest = (0, eval)("(function (first, ...rest) { return hostArgs(first, ...rest, rest.length); })");
print(evRest(1, 2, 3));
var evArgs = new Function("return arguments.length + ':' + hostArgs.apply(null, arguments);");
print(evArgs("a", "b"));
function hostSpread(f) { var xs = [4, 5, 6]; return f(...xs); }
print(hostSpread(new Function("a", "b", "c", "return a * b * c;")));
var adders = [];
for (let i = 0; i < 3; i++) adders.push((0, eval)("(function (j) { return function () { return j + " + i + "; }; })")(i));
print(adders.map(function (f) { return f(); }).join(","));
