var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 3098;
function tokenize(s) {
  var out = [], i = 0;
  while (i < s.length) {
    var c = s[i];
    if (c == " ") { i++; continue; }
    if (c >= "0" && c <= "9") { var j = i; while (j < s.length && s[j] >= "0" && s[j] <= "9") j++; out.push({ t: "n", v: +s.slice(i, j) }); i = j; }
    else if ("+-*/()".indexOf(c) >= 0) { out.push({ t: c }); i++; }
    else throw new SyntaxError("bad char " + c);
  }
  return out;
}
function parse(toks) {
  var p = 0;
  function expr() { var v = term(); while (p < toks.length && (toks[p].t == "+" || toks[p].t == "-")) { var o = toks[p++].t; var r = term(); v = o == "+" ? v + r : v - r; } return v; }
  function term() { var v = fac(); while (p < toks.length && (toks[p].t == "*" || toks[p].t == "/")) { var o = toks[p++].t; var r = fac(); v = o == "*" ? v * r : (r ? Math.floor(v / r) : 0); } return v; }
  function fac() { var k = toks[p++]; if (k.t == "n") return k.v; if (k.t == "(") { var v = expr(); p++; return v; } if (k.t == "-") return -fac(); throw new SyntaxError("unexpected " + k.t); }
  return expr();
}
var seed = 5;
function rnd() { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed >> 5; }
function gen(d) { if (d == 0) return String(rnd() % 100); var o = "+-*/"[rnd() % 4]; var e = gen(d - 1) + " " + o + " " + gen(d - 1); return rnd() % 2 ? "(" + e + ")" : e; }
for (var i = 0; i < N; i++) {
  var s = gen(4);
  mix(parse(tokenize(s)));
}
try { tokenize("1 $ 2"); } catch (e) { mixs(e.name + e.message); }
print('tokenizer-parser ' + h);
