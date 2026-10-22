var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 5000;
for (var i = 0; i < N; i++) {
  var n = i * 12345 + 17;
  mixs(n.toString(2)); mixs(n.toString(16)); mixs(n.toString(36));
  mixs((n / 7).toFixed(3));
  mixs((n / 13).toPrecision(5));
  mixs((n * 1.5e-7).toExponential(2));
  mixs((-n).toString(8));
  mix(parseInt(n.toString(7), 7));
  mixd(parseFloat((n / 3).toFixed(4)));
}
mixs(String(0.1 + 0.2)); mixs(String(1e21)); mixs(String(1 / 3)); mixs(String(-1e-7)); mixs((255).toString(16));
print('number-format ' + h);
