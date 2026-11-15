/*
 * Differential corpus: `x + y` where the left operand is a string read from a
 * property of a freshly made object. The in-place string accumulator may only
 * extend a string no one else can see; a string that a young object's
 * property holds is shared with that object even though no count says so, and
 * extending it in place changes the property.
 */
function split(s) { var i = s.slice(0, 1); var f = s.slice(2); return { int: i, frac: f }; }
function sumThenRead(d) { var p = split(d); var x = p.int + p.frac + "z"; return p.int + "/" + x; }
function loopRead(d) { var p = split(d); var n = 0; for (var k = 0; k < 2; k++) n += (p.int + p.frac).length; return p.int + n; }
function readTwice(d) { var p = split(d); var digits = p.int + p.frac; return p.int + "|" + digits; }
function arrayPair(arr) { var t = arr[0] + arr[1]; return arr[0] + "/" + t; }
function objPair(o) { var t = o.a + o.b; return o.a + "/" + t; }
var mk = function () { return { a: "q7.5".slice(1, 2), b: "z9".slice(1) }; };
print(sumThenRead("7.5"), loopRead("7.5"), readTwice("7.5"), readTwice("12.5"));
print(arrayPair(["q7".slice(1), "9"]), objPair(mk()), [mk()].map(function (o) { return [o.a + o.b, o.a].join(","); })[0]);
