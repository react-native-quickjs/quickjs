// An uncatchable error runs no finally block either.
function f(n) {
  try {
    if (n < 3000) return n;
    var s = "x".repeat(1 << 29);
    s = s + s + s + s;
  } finally {
    if (n >= 3000) print("finally ran");
  }
}
for (var i = 0; i < 3001; i++) f(i);
print("reached the end");
