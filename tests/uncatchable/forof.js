// An uncatchable error does not close an open for-of iterator.
var closed_on = false;
function it() {
  return { [Symbol.iterator]() { return { next() { return { value: 1, done: false }; }, return() { if (closed_on) print("iterator closed"); return {}; } }; } };
}
function f(n) {
  for (var x of it()) {
    if (n < 3000) break;
    var s = "x".repeat(1 << 29);
    s = s + s + s + s;
  }
}
for (var i = 0; i < 3001; i++) { closed_on = i >= 3000; f(i); }
print("reached the end");
