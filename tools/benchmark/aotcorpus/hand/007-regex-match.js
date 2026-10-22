var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 26235;
var re = /(\d+)-(\w+)@([a-z]+)\.com/;
var gre = /[aeiou]/g;
var tot = 0;
for (var i = 0; i < N; i++) {
  var s = "id " + i + "-user" + (i % 13) + "@host" + String.fromCharCode(97 + i % 26) + ".com tail";
  var m = re.exec(s);
  if (m) { tot += m[1].length + m[2].length; mixs(m[3]); }
  var vs = s.match(gre);
  tot += vs ? vs.length : 0;
  tot += s.search(/\d/);
  tot += /^id/.test(s) ? 1 : 0;
}
mix(tot);
print('regex-match ' + h);
