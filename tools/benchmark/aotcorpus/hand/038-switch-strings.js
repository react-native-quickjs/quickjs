var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 288797;
var words = ["alpha", "beta", "gamma", "delta", "eps", "zeta", "eta"];
function cls(w) {
  switch (w) {
    case "alpha": return 1;
    case "beta": case "gamma": return 2;
    case "delta": return 3;
    default: return 0;
    case "zeta": return 5;
  }
}
function num(n) {
  switch (n % 8) {
    case 0: return 1000;
    case 1: case 2: n++;
    case 3: n += 2; break;
    case 7: return 7;
    default: n = -n;
  }
  return n;
}
var tot = 0;
for (var i = 0; i < N; i++) { tot += cls(words[i % 7]) + num(i); }
mix(tot);
print('switch-strings ' + h);
