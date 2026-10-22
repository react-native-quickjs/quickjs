var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 161929;
var wm = new WeakMap();
var keys = [];
for (var i = 0; i < 100; i++) { var o = { id: i }; keys.push(o); wm.set(o, i * 2); }
var t = 0;
for (var i = 0; i < N; i++) {
  var k = keys[i % 100];
  wm.set(k, wm.get(k) + 1);
  t += wm.has(k) ? wm.get(k) : -1;
  var tmp = { n: i };
  wm.set(tmp, i);
  t += wm.get(tmp);
}
mix(t);
var ws = new WeakSet(); ws.add(keys[0]);
mix(ws.has(keys[0]) + ws.has(keys[1]));
print('weakmap ' + h);
