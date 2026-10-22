var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 788298;
function counter(step) { var c = 0; return { inc: function () { c += step; return c; }, get: function () { return c; } }; }
var cs = [counter(1), counter(2), counter(3)];
var t = 0;
for (var i = 0; i < N; i++) { t = (t + cs[i % 3].inc()) | 0; }
mix(t); mix(cs[0].get()); mix(cs[2].get());
function compose(f, g) { return function (x) { return f(g(x)); }; }
var inc = function (x) { return x + 1; }, dbl = function (x) { return x * 2; };
var fn = compose(inc, compose(dbl, inc));
for (var i = 0; i < N; i++) t = (t + fn(i)) | 0;
mix(t);
print('closure-counters ' + h);
