/* Releasing a plain object: the park shortcut and the plain-object free.
 *
 * A plain object whose refcount reaches zero is parked on the deferred list and
 * released at the next reconcile. The shortcut that parks it without the tag
 * switch and the large-value walk, and the free that skips the finalizer
 * lookup for an object with only data properties, must not change what any of
 * the release paths do: fields are still released, a weak reference still
 * clears, a FinalizationRegistry entry still runs, and a child that owns a
 * large buffer is still released.
 *
 * Everything printed here is deterministic: no output depends on when a
 * collection happens, only on what the collectable object held.
 */

var log = [];
function say(s) { log.push(s); }

/* A deep chain of plain objects dropped at once. */
(function () {
  var head = null, i;
  for (i = 0; i < 20000; i++) head = { next: head, i: i };
  say('chain=' + head.i);
  head = null;
})();

/* A plain object holding a large string: the field loop releases it. */
(function () {
  var s = '';
  for (var i = 0; i < 4000; i++) s += 'x';
  var o = { big: s, n: s.length };
  say('big=' + o.n + '/' + o.big.length);
  o = null;
})();

/* A WeakRef to a plain object that stays reachable: the reference derefs. */
(function () {
  var target = { a: 1, b: 2 };
  var wr = new WeakRef(target);
  say('live=' + (wr.deref() === target));
  say('fields=' + Object.keys(wr.deref()).join(','));
})();

/* A WeakMap keyed by a plain object that stays reachable. */
(function () {
  var wm = new WeakMap();
  var k = { id: 7 };
  wm.set(k, 'seven');
  say('weakmap=' + wm.get(k) + '/' + wm.has(k));
})();

/* A FinalizationRegistry entry for a plain object: registering it must not
 * disturb the object, and the callback must not run while it is reachable. */
(function () {
  var ran = 0;
  var reg = new FinalizationRegistry(function () { ran++; });
  var o = { tag: 'payload' };
  reg.register(o, 'payload');
  say('registered=' + (ran === 0));
  say('payload=' + o.tag);
})();

/* Many plain objects created and dropped in one loop. */
(function () {
  var last = 0;
  for (var i = 0; i < 200000; i++) {
    var o = { a: i, b: i + 1, c: 'v' };
    last = o.a + o.c.length;
  }
  say('loop=' + last);
})();

/* A plain object with an accessor: the ordinary path releases it. */
(function () {
  var o = {};
  Object.defineProperty(o, 'x', { get: function () { return 42; }, enumerable: true });
  o.y = 1;
  say('accessor=' + o.x + ',' + o.y);
  o = null;
})();

/* A plain object with an inline property area and one with a separate area. */
(function () {
  function C(a, b, c) { this.a = a; this.b = b; this.c = c; }
  var t = 0, i, o;
  for (i = 0; i < 5000; i++) { o = new C(i, i + 1, i + 2); t += o.c; }
  say('inline=' + t);
  var w = {};
  for (i = 0; i < 12; i++) w['k' + i] = i;
  say('wide=' + w.k11);
})();

for (var i = 0; i < log.length; i++) print(log[i]);
