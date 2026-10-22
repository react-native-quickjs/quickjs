var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 153446;
var tot = 0;
for (var i = 0; i < N; i++) {
  try {
    if (i % 5 == 0) throw i;
    if (i % 5 == 1) throw "str" + i;
    if (i % 5 == 2) throw new Error("e" + i);
    if (i % 5 == 3) throw { code: i };
    tot += 1;
  } catch (e) {
    if (typeof e == "number") tot += e;
    else if (typeof e == "string") tot += e.length;
    else if (e instanceof Error) tot += e.message.length;
    else tot += e.code % 7;
  } finally {
    tot += 2;
  }
}
mix(tot);
function f(i) { try { if (i & 1) return "odd"; return "even"; } finally { tot++; } }
for (var i = 0; i < 1000; i++) mixs(f(i));
mix(tot);
print('try-catch-finally ' + h);
