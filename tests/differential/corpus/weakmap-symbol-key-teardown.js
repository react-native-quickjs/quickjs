// A WeakMap entry keyed by a symbol that is still alive when the program ends, whose value nothing else refers to. The value must not
// keep the collector's view of the heap alive (the runtime's teardown checks that nothing is left). The program prints nothing: a
// print call before the end changes what the final collection sees.
var s = Symbol("k");
var wm = new WeakMap();
wm.set(s, {});
