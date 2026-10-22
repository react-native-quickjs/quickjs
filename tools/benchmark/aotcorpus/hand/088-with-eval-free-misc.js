var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 108656;
var t = 0;
var gl = 5;
function sloppy() { return this === undefined ? 1 : 2; }
function strict() { "use strict"; return this === undefined ? 1 : 2; }
for (var i = 0; i < N; i++) {
  t += sloppy() + strict() + void 0 === undefined;
  t += (function () { return typeof this; })() == "object" ? 1 : 0;
  t += (function () { "use strict"; return typeof this; })() == "undefined" ? 1 : 0;
  t += typeof undeclaredVar == "undefined" ? 1 : 0;
  gl += i & 1;
  t += delete globalThis.nothing ? 1 : 0;
  t += isFinite(i) + isNaN(i) + (i in [1, 2, 3] ? 1 : 0);
}
mix(t); mix(gl);
print('with-eval-free-misc ' + h);
