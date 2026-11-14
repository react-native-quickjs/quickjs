/* WeakMap and WeakSet keys that are made and dropped in a loop.
 *
 * A fresh object literal is a nursery cell, and a young collection drops a dead
 * cell without visiting it. A key with weak references used to stay young: when
 * it died, its weak records were never cleared, so the WeakMap kept its value
 * (an object leaked for the life of the runtime) and the runtime's weak table
 * kept the dead address. A new key made at that address then found the old
 * record: `has` was true for a key never added (register VM, release builds:
 * 191555 of 200000 lookups). A key with weak references is now old.
 *
 * Shape of the original: the Intl module keeps each formatter's state in a
 * WeakMap keyed by the instance (`new Intl.DateTimeFormat()` twice left 1658
 * objects at teardown).
 */
var wm = new WeakMap(), ws = new WeakSet(), hits = 0, shits = 0, gets = 0;
for (var i = 0; i < 200000; i++) {
  var k = { i: i };
  if (wm.has(k)) hits++;
  if (ws.has(k)) shits++;
  if (wm.get(k) !== undefined) gets++;
  wm.set(k, { v: i, tag: 'value' });
  ws.add(k);
  if (wm.get(k).v !== i || !ws.has(k)) gets++;
}
print('fresh keys: has ' + hits + ', set has ' + shits + ', wrong get ' + gets);

var state = new WeakMap();
function Formatter(locale) { state.set(this, { handle: locale.length, locale: locale }); }
Formatter.prototype.locale = function () { return state.get(this).locale; };
var names = [];
for (var j = 0; j < 3; j++) names.push(new Formatter('en-US').locale());
var kept = new Formatter('de-DE');
for (var z = 0; z < 50000; z++) new Formatter('x' + (z & 7));
print('instances: ' + names.join(',') + ' ' + kept.locale());
