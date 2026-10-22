var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 48612;
var log = 0;
var target = { a: 1, b: 2 };
var p = new Proxy(target, {
  get: function (t, k, r) { log++; return k in t ? Reflect.get(t, k, r) : 42; },
  set: function (t, k, v) { log += 2; t[k] = v * 2; return true; },
  has: function (t, k) { return k != "hidden"; },
  deleteProperty: function (t, k) { log += 3; return delete t[k]; },
  ownKeys: function (t) { return Reflect.ownKeys(t).concat(["virt"]); },
  getOwnPropertyDescriptor: function (t, k) { return { value: t[k], enumerable: true, configurable: true }; }
});
var t = 0;
for (var i = 0; i < N; i++) {
  p.a = i; t += p.a + p.zzz + (("hidden" in p) ? 1 : 0);
  p["k" + (i % 3)] = 1; delete p["k" + ((i + 1) % 3)];
  if (i % 100 == 0) t += Object.keys(p).length;
  t = t % 1000003;
}
mix(t); mix(log);
mix(Reflect.has(target, "a") ? 1 : 0); mix(Reflect.ownKeys(target).length);
print('proxy-reflect ' + h);
