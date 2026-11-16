// GetBindingValue looks the property up again after the unscopables are read (ES2024 9.1.1.2.6):
// a binding deleted meanwhile is undefined in sloppy code and a ReferenceError in strict code, and
// a proxy environment sees has, get unscopables, has, get in that order. (V8 skips the second has.)
var log = [];
var env = new Proxy({ x: 1 }, {
  has: function (t, k) { log.push("has:" + String(k)); return k in t; },
  get: function (t, k) { log.push("get:" + String(k)); return t[k]; }
});
function read() { with (env) { return x; } }
function call() { with (env) { return typeof x; } }
print(read(), log.join(","));
log = [];
function callit() { var o = { f: function () { return "f"; } }; with (o) { return f(); } }
print(callit());
var env2 = { y: 2, get [Symbol.unscopables]() { delete env2.y; return {}; } };
function sloppy() { with (env2) { return (function () { return y; })(); } }
print(String(sloppy()));
var env3 = { z: 3, get [Symbol.unscopables]() { delete env3.z; return {}; } };
function strictRead() { with (env3) { return (function () { "use strict"; return z; })(); } }
try { print(strictRead()); } catch (e) { print(e.name); }
