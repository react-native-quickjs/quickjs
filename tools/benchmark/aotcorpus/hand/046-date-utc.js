var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 60669;
var tot = 0;
for (var i = 0; i < N; i++) {
  var d = new Date(86400000 * (i * 7) + i * 3600123);
  tot += d.getUTCDay() + d.getUTCFullYear() + d.getUTCMonth() + d.getUTCDate() + d.getUTCHours() + d.getUTCMinutes() + d.getUTCSeconds() + d.getUTCMilliseconds();
  if (i % 100 == 0) mixs(d.toISOString());
  var d2 = new Date(Date.UTC(2000 + i % 30, i % 12, 1 + i % 28, i % 24));
  tot += d2.getTime() / 1000 % 1000003 | 0;
  d2.setUTCDate(d2.getUTCDate() + 40);
  tot += d2.getUTCMonth();
}
mix(tot);
mix(Date.parse("2001-02-03T04:05:06Z") % 1000003);
print('date-utc ' + h);
