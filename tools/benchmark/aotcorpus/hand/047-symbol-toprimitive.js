var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 50000;
function Money(c) { this.c = c; }
Money.prototype[Symbol.toPrimitive] = function (hint) { return hint == "string" ? "$" + this.c : hint == "number" ? this.c : this.c + 0.5; };
var o1 = { valueOf: function () { return 7; }, toString: function () { return "seven"; } };
var o2 = { toString: function () { return "10"; } };
var tot = 0;
for (var i = 0; i < N; i++) {
  var m = new Money(i % 100);
  tot += +m + (m + 1) + `${m}`.length + (m * 2) + (o1 + 1) + (o2 * 2) + (o1 > 6 ? 1 : 0) + String(o1).length + (o2 + "").length;
  tot += [1, 2] + "" == "1,2" ? 1 : 0;
}
mixd(tot);
print('symbol-toprimitive ' + h);
