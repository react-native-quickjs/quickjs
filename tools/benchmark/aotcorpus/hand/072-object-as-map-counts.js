var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 407124;
var counts = {};
var text = "it was the best of times it was the worst of times it was the age of wisdom it was the age of foolishness";
var words = text.split(" ");
for (var i = 0; i < N; i++) {
  var w = words[i % words.length] + (i % 3 ? "" : "s");
  counts[w] = (counts[w] || 0) + 1;
}
var keys = Object.keys(counts).sort();
for (var i = 0; i < keys.length; i++) { mixs(keys[i]); mix(counts[keys[i]]); }
var inv = {};
for (var k in counts) (inv[counts[k]] = inv[counts[k]] || []).push(k);
mixs(JSON.stringify(inv).length);
mix("hasOwnProperty" in counts ? 1 : 0);
print('object-as-map-counts ' + h);
