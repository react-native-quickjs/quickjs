var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 768218;
var sieve = new Uint8Array(N + 1);
var cnt = 0;
for (var i = 2; i <= N; i++) {
  if (!sieve[i]) { cnt++; for (var j = i * 2; j <= N; j += i) sieve[j] = 1; }
}
mix(cnt);
var arr = [];
for (var i = 0; i < 100000; i++) arr.push(true);
for (var i = 2; i * i < 100000; i++) if (arr[i]) for (var j = i * i; j < 100000; j += i) arr[j] = false;
var c2 = 0; for (var i = 2; i < 100000; i++) if (arr[i]) c2++;
mix(c2);
print('sieve ' + h);
