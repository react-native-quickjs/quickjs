var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 123689;
var a = 1n, tot = 0n;
for (var i = 0; i < N; i++) {
  a = (a * 6364136223846793005n + 1442695040888963407n) & 0xffffffffffffffffn;
  tot += a >> 40n;
  tot ^= BigInt(i) << 3n;
  tot -= a % 1000n;
}
mixs(tot); mixs(a);
mixs((2n ** 100n).toString()); mixs(BigInt.asUintN(8, -1n)); mixs(BigInt.asIntN(8, 255n));
mix(Number(tot % 1000003n)); mixs(typeof tot); mixs(10n > 9 ? 1 : 0); mixs(-7n / 2n); mixs(-7n % 3n);
print('bigint ' + h);
