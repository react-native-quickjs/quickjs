var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 3000;
var src = "let x = 10; if (x > 5) { y = x * 2; } else { y = 0; } while (y) y--;";
var kw = { let: 1, if: 2, else: 3, while: 4 };
var t = 0;
for (var r = 0; r < N; r++) {
  var re = /[A-Za-z_]\w*|\d+|[{}();=*><-]+/g, m, counts = [0, 0, 0, 0, 0];
  while ((m = re.exec(src)) !== null) {
    var w = m[0];
    if (kw.hasOwnProperty(w)) counts[kw[w]]++; else if (/^\d+$/.test(w)) counts[0] += +w; else counts[4]++;
  }
  t += counts.reduce(function (a, b) { return a + b; });
  mixs(counts);
}
mix(t);
print('string-switch-tokens-reverse ' + h);
