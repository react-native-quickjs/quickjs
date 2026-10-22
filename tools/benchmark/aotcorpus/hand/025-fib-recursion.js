var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 267;
function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
function ack(m, n) { return m == 0 ? n + 1 : n == 0 ? ack(m - 1, 1) : ack(m - 1, ack(m, n - 1)); }
for (var r = 0; r < N; r++) mix(fib(18 + r % 3));
mix(ack(2, 300)); mix(ack(3, 5));
function isEven(n) { return n == 0 ? true : isOdd(n - 1); }
function isOdd(n) { return n == 0 ? false : isEven(n - 1); }
var t = 0;
for (var i = 0; i < 2000; i++) t += isEven(i % 200) ? 1 : 0;
mix(t);
print('fib-recursion ' + h);
