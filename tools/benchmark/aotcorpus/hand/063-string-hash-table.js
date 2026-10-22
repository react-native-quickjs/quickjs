var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 357505;
var table = {};
var seed = 21;
function rnd() { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed >> 7; }
var words = [];
for (var i = 0; i < 500; i++) words.push("w" + (rnd() % 100000).toString(36));
for (var i = 0; i < N; i++) {
  var w = words[rnd() % 500];
  table[w] = (table[w] || 0) + 1;
  if (i % 500 == 499) delete table[words[rnd() % 500]];
}
var ks = Object.keys(table);
mix(ks.length);
var tot = 0;
for (var i = 0; i < ks.length; i++) tot += table[ks[i]] * (i + 1);
mix(tot);
function hash(s) { var x = 5381; for (var i = 0; i < s.length; i++) x = ((x << 5) + x + s.charCodeAt(i)) | 0; return x; }
var bucket = new Array(64).fill(0);
for (var i = 0; i < words.length; i++) bucket[hash(words[i]) & 63]++;
mixs(bucket);
print('string-hash-table ' + h);
