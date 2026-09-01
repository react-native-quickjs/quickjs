/*
 * OP_get_var cell cache (docs/global-var-cell-cache.md).
 *
 * The cache memoizes "atom -> index into globalThis's property array" and is
 * guarded by one generation counter. Everything below is a way for a cached
 * {atom, index} pair to go stale. Each block is warmed with 300 reads first,
 * because a cache that is never filled cannot be wrong.
 *
 * Deliberately NOT covered here, because a single script cannot reach it: a
 * global LEXICAL binding introduced by a LATER script evaluation shadowing a
 * name whose global-object property is already cached. `let` is hoisted within
 * a script, so that case needs two JS_Eval calls; it lives in
 * bench/spikes/gvcell/ and is what the `mut-noadd` mutant is caught by.
 */
function warm(f) { for (var i = 0; i < 300; i++) f(); return f(); }

/* 1. the plain case, and a write through the same slot */
var a1 = 11;
function r1() { return a1; }
print('plain ' + warm(r1));
a1 = 22;
print('rewritten ' + r1());

/* 2. a second global that indexes into the same cache set as the first */
var a2 = 33;
function r2() { return a2; }
print('second ' + warm(r2) + ' first-still ' + r1());

/* 3. delete of a cached global: the read must become a ReferenceError */
globalThis.delme = 5;
function rdel() { try { return delme; } catch (e) { return e.name; } }
print('pre-delete ' + warm(rdel));
print('deleted ' + (delete globalThis.delme) + ' now ' + rdel());

/* 4. a cached data property converted to an accessor in place */
globalThis.acc = 1;
function racc() { return acc; }
print('pre-getter ' + warm(racc));
Object.defineProperty(globalThis, 'acc', {
  get: function () { return 99; }, configurable: true,
});
print('post-getter ' + racc());

/* 5. a name that does not exist yet, then does */
function radd() { try { return added; } catch (e) { return e.name; } }
print('pre-add ' + warm(radd));
globalThis.added = 7;
print('post-add ' + radd());

/* 6. a name resolved on globalThis's PROTOTYPE, which is never cacheable */
Object.setPrototypeOf(globalThis, { onProto: 'P' });
function rproto() { return onProto; }
print('proto ' + warm(rproto));

/* 7. `with` must beat the cache: dynamic scope is decided before OP_get_var */
var wv = 'outer';
function rwith(o) { with (o) { return wv; } }
print('with-empty ' + warm(function () { return rwith({}); }));
print('with-shadow ' + rwith({ wv: 'inner' }));

/* 8. a frozen (non-writable, non-configurable) global */
globalThis.frz = 1;
Object.defineProperty(globalThis, 'frz', { writable: false, configurable: false });
function rfrz() { return frz; }
print('frozen ' + warm(rfrz));

/* 9. a global lexical shadowing a same-named property of the global object.
   `dual` is hoisted, so every read below is lexical from the start. */
globalThis.dual = 'obj';
let dual = 'lex';
function rdual() { return dual; }
print('lexical-wins ' + warm(rdual) + ' property ' + globalThis.dual);
dual = 'lex2';
globalThis.dual = 'obj2';
print('lexical-after-writes ' + rdual() + ' property ' + globalThis.dual);

/* 10. more distinct global names than the cache has sets, read round-robin */
var names = [];
for (var i = 0; i < 400; i++) { names.push('v' + i); globalThis['v' + i] = i; }
var sum = 0;
for (var k = 0; k < 3; k++) for (var i = 0; i < 400; i++) sum += globalThis[names[i]];
print('many ' + sum);

/* 11. deleting one of those, then re-adding it with a different value */
delete globalThis.v7;
print('v7-gone ' + ('v7' in globalThis));
globalThis.v7 = 777;
var s2 = 0;
for (var i = 0; i < 400; i++) s2 += globalThis[names[i]];
print('after-readd ' + s2);
