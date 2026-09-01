/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * for-in, and the PROTOTYPE CHAIN specifically.
 *
 * WHY THIS FILE EXISTS. `build_for_in_iterator()` decides between two
 * completely different enumeration strategies by asking, for every object on
 * the prototype chain, "does it have any enumerable string-keyed own
 * property?". If the answer is no everywhere, it takes a fast path that only
 * ever looks at the receiver's own keys; if yes anywhere, it takes a slow path
 * that walks the whole chain and records non-enumerable names so they can
 * SHADOW enumerable ones further up.
 *
 * docs/for-in-loop-setup-cost.md MEASURED that question costing 29.9 ns of
 * `OP_for_in_start`'s 62 ns on `rnprops/commit-20-rows` -- 48% -- because it is
 * answered by calling `JS_GetOwnPropertyNamesInternal()`, which allocates an
 * atom array, dups every matching atom and immediately frees them all, purely
 * to look at the count. Any cheaper way of answering it has to agree with the
 * expensive one on every case below, and the existing corpus file `for-in.js`
 * does not exercise a single one of them: it has no prototype with enumerable
 * properties, no shadowing, no symbol keys on a prototype and no exotic object
 * in a chain.
 *
 * Every case is chosen because it can distinguish a shape-only scan from
 * `JS_GetOwnPropertyNamesInternal(JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)`:
 *
 *   - a symbol-keyed enumerable prototype property must NOT count (the mask
 *     excludes symbols), so it must not push enumeration onto the slow path
 *   - an array-index key IS a string key and MUST count
 *   - a deleted property leaves a JS_ATOM_NULL hole in the shape which a naive
 *     scan would mistake for a live entry
 *   - a Proxy, a String wrapper and a fast array synthesize keys that are not
 *     in the shape at all, so a shape-only scan is WRONG for them and must
 *     fall back
 *   - accessor properties are enumerable-or-not independently of being
 *     accessors
 *   - shadowing is the whole reason the slow path exists
 *
 * Output is compared byte-for-byte against node by tests/differential/run.mjs.
 * Keys are printed in enumeration ORDER, not sorted, because order is part of
 * the contract this file is defending.
 */

function keys(o) {
  var out = [];
  for (var k in o) out.push(k);
  return out.join(',');
}

function show(label, o) {
  print(label + ': [' + keys(o) + ']');
}

/* 1. plain object, plain prototype: the fast path everything else is measured
      against. */
var plain = { a: 1, b: 2 };
show('1 plain', plain);

/* 2. an ENUMERABLE property on the prototype. It must be enumerated, after the
      receiver's own keys. */
function Proto2() {}
Proto2.prototype.inherited = 3;
var o2 = new Proto2();
o2.own = 1;
show('2 enumerable-on-proto', o2);

/* 3. a NON-enumerable property on the prototype, shadowed by an enumerable own
      one. The own key is enumerated; the prototype's is not. */
function Proto3() {}
Object.defineProperty(Proto3.prototype, 'hidden', {
  value: 1,
  enumerable: false,
  configurable: true,
});
var o3 = new Proto3();
o3.hidden = 2;
o3.visible = 3;
show('3 nonenum-proto-shadowed', o3);

/* 4. THE SHADOWING CASE the slow path exists for: a NON-enumerable own
      property hides an ENUMERABLE one on the prototype. Neither is
      enumerated. */
function Proto4() {}
Proto4.prototype.k = 'from-proto';
var o4 = new Proto4();
Object.defineProperty(o4, 'k', { value: 'own', enumerable: false });
o4.other = 1;
show('4 own-nonenum-hides-proto-enum', o4);
print('4 value still reachable: ' + o4.k);

/* 5. a SYMBOL-keyed enumerable property on the prototype. for-in never yields
      symbols, so this must behave exactly like case 1. */
function Proto5() {}
Proto5.prototype[Symbol('s')] = 1;
var sym = Symbol('t');
Proto5.prototype[sym] = 2;
var o5 = new Proto5();
o5.own = 1;
show('5 symbol-on-proto', o5);

/* 6. an ARRAY-INDEX key on the prototype: a string key, and enumerable, so it
      must be yielded. Integer keys come first, in ascending numeric order. */
function Proto6() {}
Proto6.prototype[7] = 'seven';
Proto6.prototype[2] = 'two';
var o6 = new Proto6();
o6.z = 1;
o6[3] = 'three';
show('6 index-keys-on-proto', o6);

/* 7. a DELETED property on the prototype leaves a hole in the shape. */
function Proto7() {}
Proto7.prototype.gone = 1;
Proto7.prototype.stays = 2;
delete Proto7.prototype.gone;
var o7 = new Proto7();
o7.own = 1;
show('7 deleted-on-proto', o7);

/* 8. every enumerable property on the prototype deleted: the chain is empty
      again and the fast path must be taken, correctly. */
function Proto8() {}
Proto8.prototype.gone1 = 1;
Proto8.prototype.gone2 = 2;
delete Proto8.prototype.gone1;
delete Proto8.prototype.gone2;
var o8 = new Proto8();
o8.own = 1;
show('8 all-deleted-on-proto', o8);

/* 9. an ACCESSOR on the prototype, enumerable. */
function Proto9() {}
Object.defineProperty(Proto9.prototype, 'acc', {
  get: function () {
    return 1;
  },
  enumerable: true,
  configurable: true,
});
var o9 = new Proto9();
o9.own = 1;
show('9 enumerable-accessor-on-proto', o9);

/* 10. null prototype: the chain terminates immediately. */
var o10 = Object.create(null);
o10.a = 1;
o10.b = 2;
show('10 null-proto', o10);

/* 11. a THREE-deep chain with an enumerable key at the top only. */
var top = { fromTop: 1 };
var mid = Object.create(top);
var bottom = Object.create(mid);
bottom.own = 1;
show('11 deep-chain', bottom);

/* 12. a three-deep chain with a NON-enumerable at the bottom hiding an
       enumerable at the top. */
var top12 = { k: 1 };
var mid12 = Object.create(top12);
Object.defineProperty(mid12, 'k', { value: 2, enumerable: false });
var bottom12 = Object.create(mid12);
bottom12.own = 1;
show('12 deep-shadow', bottom12);

/* 13. an ARRAY in the prototype chain. Arrays are exotic: their indices are
       not in the shape at all, so anything that only reads the shape is wrong
       for them. */
var arrProto = [10, 20, 30];
var o13 = Object.create(arrProto);
o13.own = 1;
show('13 array-in-chain', o13);

/* 14. a STRING WRAPPER in the prototype chain, whose indices and `length` are
       likewise synthesized rather than stored in the shape. */
var o14 = Object.create(Object('ab'));
o14.own = 1;
show('14 string-object-in-chain', o14);

/* 15. a PROXY in the prototype chain. Its keys come from the ownKeys trap and
       not from any shape, so a shape-only scan must fall back for it.
       The trap returns exactly the target's own keys on purpose: a trap that
       invents a key that is not on the target diverges between quickjs-ng and
       node for reasons that have nothing to do with for-in setup, and that
       divergence is recorded separately in
       tests/differential/pending/for-in-proxy-ownkeys.js. */
var proxyTarget15 = { real: 1 };
var trapCalls15 = 0;
var o15 = Object.create(
  new Proxy(proxyTarget15, {
    ownKeys: function (t) {
      trapCalls15++;
      return Reflect.ownKeys(t);
    },
  })
);
o15.own = 1;
show('15 proxy-in-chain', o15);
print('15 ownKeys trap called at least once: ' + (trapCalls15 > 0));

/* 16. the receiver ITSELF is exotic (an array), with a named own property. */
var a16 = [1, 2];
a16.named = 'x';
show('16 array-receiver', a16);

/* 17. FROZEN prototype: frozen makes properties non-writable and
       non-configurable but leaves enumerability alone, so they still show. */
function Proto17() {}
Proto17.prototype.a = 1;
Object.freeze(Proto17.prototype);
var o17 = new Proto17();
o17.own = 2;
show('17 frozen-proto', o17);

/* 18. MUTATING the prototype between two loops over the same receiver. The
       second loop must see the newly added prototype key, so nothing may cache
       the chain's verdict past a mutation. */
function Proto18() {}
var o18 = new Proto18();
o18.own = 1;
show('18a before-proto-mutation', o18);
Proto18.prototype.added = 2;
show('18b after-proto-mutation', o18);
delete Proto18.prototype.added;
show('18c after-proto-delete', o18);

/* 19. __proto__ SWAPPED between two loops over the same receiver. */
var o19 = { own: 1 };
show('19a proto=Object.prototype', o19);
Object.setPrototypeOf(o19, { fromNewProto: 2 });
show('19b proto=swapped', o19);

/* 20. Object.prototype itself gains an enumerable property. Every plain object
       in the program is affected; this is the case a cached "the chain is
       clean" verdict gets wrong most spectacularly. */
var o20 = { own: 1 };
show('20a before-polluting-Object.prototype', o20);
Object.prototype.polluted = 9;
show('20b after-polluting-Object.prototype', o20);
delete Object.prototype.polluted;
show('20c after-cleanup', o20);

/* 21. the same receiver enumerated twice with a DELETE in between, to confirm
       nothing is memoized across a shape change on the receiver. */
var o21 = { a: 1, b: 2, c: 3 };
show('21a', o21);
delete o21.b;
show('21b after-delete-b', o21);
o21.d = 4;
show('21c after-add-d', o21);

/* 22. a getter on the prototype that MUTATES the prototype while the loop is
       running. The spec snapshots nothing, but implementations differ in how
       much they have already materialized; both engines must agree. */
var proto22 = {};
Object.defineProperty(proto22, 'trigger', {
  get: function () {
    proto22.lateAddition = 1;
    return 1;
  },
  enumerable: true,
  configurable: true,
});
var o22 = Object.create(proto22);
o22.own = 1;
var seen22 = [];
for (var k22 in o22) {
  seen22.push(k22);
  void o22[k22];
}
print('22 mutate-during-loop: [' + seen22.join(',') + ']');

/* 23. enumerating a chain where an intermediate link is non-extensible. */
var top23 = { fromTop: 1 };
Object.preventExtensions(top23);
var o23 = Object.create(top23);
o23.own = 1;
show('23 non-extensible-proto', o23);

/* 24. a receiver with MANY own keys over a prototype with many non-enumerable
       ones -- the shape scan's loop bounds. */
function Proto24() {}
for (var i = 0; i < 40; i++) {
  Object.defineProperty(Proto24.prototype, 'n' + i, {
    value: i,
    enumerable: false,
    configurable: true,
  });
}
Object.defineProperty(Proto24.prototype, 'yes', {
  value: 1,
  enumerable: true,
  configurable: true,
});
var o24 = new Proto24();
for (var j = 0; j < 12; j++) o24['own' + j] = j;
show('24 wide-shapes', o24);

/* 25. break out of a for-in early, then enumerate again. */
var o25 = { a: 1, b: 2, c: 3, d: 4 };
var first25 = [];
for (var k25 in o25) {
  first25.push(k25);
  if (first25.length === 2) break;
}
print('25a partial: [' + first25.join(',') + ']');
show('25b full-after-break', o25);
