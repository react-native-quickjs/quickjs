// An InternalError is uncatchable: the try/catch around it does not run.
function f(n) {
  try {
    if (n < 3000) throw new Error("plain");
    var s = "x".repeat(1 << 29);
    s = s + s + s + s;          // string too long: InternalError
    return "no error";
  } catch (e) {
    return "caught " + (e && e.name);
  }
}
var r = "";
for (var i = 0; i < 3001; i++) r = f(i);
print("reached the end: " + r);
