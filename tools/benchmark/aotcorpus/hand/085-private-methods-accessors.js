var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 173758;
class Acc {
  #total = 0; static #made = 0;
  constructor() { Acc.#made++; }
  #bump(x) { this.#total = (this.#total + x) | 0; }
  get #double() { return this.#total * 2; }
  add(x) { this.#bump(x); return this; }
  get value() { return this.#double; }
  static get made() { return Acc.#made; }
  static has(o) { return #total in o; }
  set value(v) { this.#total = v; }
}
var a = new Acc(), t = 0;
for (var i = 0; i < N; i++) { a.add(i).add(1); t = (t + a.value) | 0; if (i % 10000 == 0) a.value = i; }
mix(t); mix(Acc.made); mix(Acc.has(a) ? 1 : 0); mix(Acc.has({}) ? 1 : 0);
print('private-methods-accessors ' + h);
