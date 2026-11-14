/* A store far past the end of an array.
 *
 * A store just past the end of a fast array leaves holes in the gap. When the
 * gap is too large, the array too sparse, or its length not writable, the
 * store declines and the array becomes an ordinary (dictionary) array instead.
 * The declined store must not keep a reference to the value: the value is
 * stored once, through the ordinary path. Run under QJS_DEBUG_LEAK=1, a kept
 * reference shows as objects left at teardown (each stored object holds the
 * realm through its prototype).
 *
 * Output is the observable array state, which must match on every path.
 */

function show(tag, a) {
  var ks = Object.keys(a);
  print(tag + ' len=' + a.length + ' keys=' + ks.length + ' last=' + ks[ks.length - 1] +
        ' v=' + JSON.stringify(a[ks[ks.length - 1]]));
}

/* Gap above the hole limit, from an empty array: the brackets-map shape. */
function brackets() {
  var e = [];
  return e[1024] = ['{', '}'], e[2048] = ['(', ')'], e[4096] = ['<', '>'], e;
}
var b;
for (var i = 0; i < 20; i++) b = brackets();
show('brackets', b);
print('pair ' + b[1024].join('') + b[4096].join(''));

/* Gap limit boundary: a small gap stays holey, a large one converts. */
[1, 100, 255, 256, 257, 300, 1000].forEach(function (n) {
  var a = [];
  a[n] = { n: n };
  show('gap' + n, a);
});

/* A gap from a non-empty array, after a length extension. */
(function () {
  var a = [1, 2];
  a.length = 1000;
  a[999] = { tail: true };
  show('extended', a);
})();

/* Too sparse: many small gaps until the density bound declines. */
(function () {
  var a = [{ i: 0 }], i;
  for (i = 1; i < 60; i++) a[i * 200] = { i: i };
  show('sparse', a);
})();

/* Non-writable length: the store declines and then fails on the ordinary path. */
(function () {
  var a = [{ x: 1 }];
  Object.defineProperty(a, 'length', { writable: false });
  a[5] = { y: 2 };
  show('frozenlen', a);
  try {
    'use strict';
    (function () { 'use strict'; a[700] = { z: 3 }; })();
    print('frozenlen strict: no error');
  } catch (e) {
    print('frozenlen strict: ' + e.name);
  }
})();

/* defineProperty far past the end takes the same decline. */
(function () {
  var a = [];
  Object.defineProperty(a, '1024', { value: { d: 1 }, writable: true, enumerable: true, configurable: true });
  show('define', a);
})();
