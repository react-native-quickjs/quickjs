var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 14087;
var t = 0;
for (var i = 0; i < N; i++) {
  var s = "  Hello, World " + i + "!  ";
  var u = s.trim().toUpperCase(), l = s.toLowerCase();
  t += u.length + l.indexOf("world") + s.trimStart().length + s.trimEnd().length;
  t += s.padStart(30, "*").length + s.padEnd(30, "ab").charCodeAt(29);
  t += s.repeat(2).length + s.at(-3).charCodeAt(0) + s.codePointAt(3);
  t += s.startsWith("  H") + s.endsWith("!  ") + s.includes("World");
  t += s.replaceAll("l", "LL").length + s.substring(5, 2).length + s.substr(-5, 2).length;
  t += s.split(/[ ,]+/).length + s.split("").length + s.concat("x", 1).length;
  t += "b".localeCompare === undefined ? 0 : 1;
  t += s.normalize().length + ("a" + i).charAt(1).length;
}
mix(t);
print('string-methods ' + h);
