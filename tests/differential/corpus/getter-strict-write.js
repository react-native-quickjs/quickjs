/* The 0194 getter repro: a strict light function reads through a getter and
   writes to a frozen object, so every iteration throws at the write. Prints
   the same count three times; guards the strict-mode light frame's `this`/write
   path against the getter's re-entry.

   Derived from `.local/light-frame/repro-getter-strict.js`; the stack-trace
   part of that probe is engine-specific and is not included here. */

var frozen = Object.freeze({ p: 0 });
function helper() { return 1; }
var o = { get x() { return helper(); } };
var plain = { x: 1 };

function run(f, a) {
  var threw = 0;
  for (var i = 0; i < 2000; i++) {
    try { f(a, frozen); } catch (e) { threw++; }
  }
  return threw;
}

function fLight(a, fz) { "use strict"; a.x; fz.p = 1; return 0; }
function fLocal(a, fz) { "use strict"; var z = a.x; fz.p = 1; return z; }
function fNoGetter(a, fz) { "use strict"; a.x; fz.p = 1; return 0; }

print(run(fLight, o));
print(run(fLocal, o));
print(run(fNoGetter, plain));
