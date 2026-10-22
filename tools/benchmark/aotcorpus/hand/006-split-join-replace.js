var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 13158;
var text = "the quick brown fox jumps over the lazy dog";
var acc = 0;
for (var i = 0; i < N; i++) {
  var w = text.split(" ");
  w.reverse();
  var j = w.join("_" + (i % 10));
  var r = j.replace("fox", "cat").replace(/o/g, "0").replace(/(\w+)_/, "$1:");
  acc += r.length;
  mixs(r.substr(0, 12));
}
mix(acc);
print('split-join-replace ' + h);
