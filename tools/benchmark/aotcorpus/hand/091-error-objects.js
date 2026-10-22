var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 40388;
var t = 0;
for (var i = 0; i < N; i++) {
  try {
    switch (i % 5) {
      case 0: null.x; break;
      case 1: undefinedFn(); break;
      case 2: (void 0)(); break;
      case 3: new Array(-1); break;
      case 4: JSON.parse("{bad"); break;
    }
  } catch (e) {
    t += e.name.length + (e instanceof TypeError ? 1 : 0) + (e instanceof RangeError ? 2 : 0) + (e instanceof SyntaxError ? 4 : 0) + (e instanceof ReferenceError ? 8 : 0);
    if (i < 5) mixs(e.name);
  }
}
class MyErr extends Error { constructor(m, c) { super(m); this.code = c; this.name = "MyErr"; } }
for (var i = 0; i < N; i++) {
  try { throw new MyErr("m" + i, i); } catch (e) { t += e.code + e.message.length + (e instanceof Error ? 1 : 0) + String(e).length; }
}
mix(t);
print('error-objects ' + h);
