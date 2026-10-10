/* Counted-loop semantics the encoder's loop work must not change: continue,
   break, a labelled continue to an outer loop, a bound that changes in the
   body, a double and a NaN bound, an int32 counter overflow, `for (;;)`,
   `do`/`while`, `continue` through `try`/`finally`, and a bound whose `valueOf`
   counts its evaluations. Output is deterministic and compares byte for byte
   with node. */

var out = [];
for (var i = 0; i < 10; i++) {
  if (i % 2) continue;
  if (i > 6) break;
  out.push(i);
}
print("continue/break:", out.join(","));

out = [];
outer: for (var i = 0; i < 4; i++) {
  for (var j = 0; j < 4; j++) {
    if (j === 2) continue outer;
    out.push(i * 10 + j);
  }
}
print("labelled continue:", out.join(","));

var n = 5, s = 0;
for (var i = 0; i < n; i++) {
  s += i;
  if (i === 2) n = 8;
}
print("changing bound:", s);

s = 0;
for (var i = 0; i < 3.5; i++) s += i;
print("double bound:", s);

s = 0;
for (var i = 0; i < NaN; i++) s += i;
print("NaN bound:", s);

s = 0;
for (var i = 2147483645; i < 2147483650; i++) s += i > 2147483646 ? 1 : 0;
print("int32 overflow:", s, i);

s = 0;
var k = 0;
for (;;) {
  if (++k > 5) break;
  s += k;
}
print("no test:", s);

s = 0;
var d = 0;
do {
  s += d;
  d++;
} while (d < 4);
print("do/while:", s);

s = 0;
for (var i = 0; i < 5; i++) {
  try {
    if (i === 2) continue;
    s += i;
  } finally {
    s += 100;
  }
}
print("try/finally continue:", s);

var calls = 0;
var bound = {
  valueOf: function () {
    calls++;
    return 3;
  }
};
s = 0;
for (var i = 0; i < bound; i++) s += i;
print("valueOf:", s, calls);
