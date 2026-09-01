/*
 * OP_get_loc8_array_el -- the fused `get_loc(n) get_array_el` superinstruction.
 *
 * `obj[k]` compiles to  get_x(obj) get_loc(k) get_array_el, so the peephole
 * folds the KEY load, not the receiver load. The fused handler reads the key
 * straight out of the local frame and BORROWS it -- it never takes a reference.
 * Everything below is a way for that borrow, or the missing ToPropertyKey /
 * exception / receiver-free bookkeeping, to be observable.
 *
 * Every case must be exercised with the key in a plain `var` local, because a
 * `let` in TDZ range emits get_loc_check, a captured variable emits
 * get_var_ref, and a parameter emits get_arg -- none of which the peephole
 * matches, so none of which would test anything.
 */

// EVERYTHING RUNS INSIDE A FUNCTION, AND THAT IS LOAD-BEARING. A top-level
// `var` in a script is a GLOBAL, so `obj[k]` compiles to OP_get_var and the
// peephole never fires. The first version of this file was written at top
// level and, verified with an opcode counter, executed OP_get_loc8_array_el
// ZERO times -- it passed identically against builds with two different
// deliberate bugs in the handler. Inside a function the same `var`s are locals
// and the fusion fires.
(function () {

// --- ordinary reads, both key kinds ---------------------------------------
var obj = { a: 1, b: 2, 3: 'three', '': 'empty' };
var ka = 'a';
print(obj[ka]);
ka = 'b';
print(obj[ka]);
var ki = 3;
print(obj[ki]);
var ke = '';
print(obj[ke]);
var kmissing = 'nope';
print(obj[kmissing]);

var arr = [10, 20, 30];
var i0 = 0, i2 = 2, ioob = 7, ineg = -1;
print(arr[i0], arr[i2], arr[ioob], arr[ineg]);
var slen = 'length';
print(arr[slen]);
var sfloat = 1.0;
print(arr[sfloat]);

// --- an index EXACTLY at the dense fast path's bound -------------------------
// The inline fast path admits `(uint32_t)idx < p->u.array.count`. At
// idx == count it must fall through to the generic path and produce undefined.
// If that comparison is ever written `<=`, the handler reads one JSValue past
// the end of u.array.u.values and js_dup()s it -- a refcount increment on
// whatever pointer happens to sit there. That is memory corruption, and NOTHING
// ELSE IN THIS FILE CAN SEE IT: the only out-of-bounds index tested above is
// ioob = 7 on a 3-element array, which both the correct bound and the off-by-one
// reject. The boundary index is the single index that discriminates them.
// Verified: against a build with `<` changed to `<=` the first line below prints
// an integer and the second prints `[unsupported type]`, where the shipped
// engine prints `undefined undefined`.
var atlen = [1, 2, 3];
var ilen = 3;
print(atlen[ilen], atlen[ilen]);
var atlen1 = [{ tag: 'obj-at-0' }];
var ilen1 = 1;
print(atlen1[ilen1], typeof atlen1[ilen1]);
var atempty = [];
var izero = 0;
print(atempty[izero]);
// Same boundary after the array grows and shrinks, so `count` is not equal to
// the allocated capacity and the byte past the end is live memory rather than
// fresh pages.
var grown = [1, 2, 3, 4, 5, 6, 7, 8];
grown.length = 2;
var igrown = 2;
print(grown[igrown], grown.length);
// Typed arrays take the call path, not the inline one, but the boundary is the
// same and a mistake there is equally invisible without this case.
var tabound = new Int32Array([7, 8, 9]);
var tlen = 3;
print(tabound[tlen]);

// --- the borrow: a key that reassigns its own local during ToPropertyKey ---
// The fused op holds a borrowed pointer to the local across the toString call,
// which reassigns the local and drops the last reference to the key object. If
// the borrow outlived the call this is a use-after-free, and if the key were
// re-read after the call the wrong property would be returned.
var kself = {
  toString: function () {
    kself = 'b';
    return 'a';
  },
};
print(obj[kself], typeof kself);

// Same shape, but the reassignment makes the key object garbage immediately.
var kdrop = {
  toString: function () {
    kdrop = null;
    return 'a';
  },
};
print(obj[kdrop], kdrop);

// valueOf rather than toString, and a Symbol.toPrimitive.
var kv = { valueOf: function () { return 3; }, toString: function () { return 'a'; } };
print(obj[kv]);
var kp = {};
kp[Symbol.toPrimitive] = function () { return 'b'; };
print(obj[kp]);

// --- keys that throw --------------------------------------------------------
var kthrow = { toString: function () { throw new Error('key threw'); } };
try {
  print(obj[kthrow]);
} catch (e) {
  print('caught: ' + e.message);
}
var ksym = Symbol('s');
var kbad = { toString: function () { return ksym; } };
print(typeof obj[kbad]);

// --- symbol and numeric-string keys ----------------------------------------
var kiter = Symbol.iterator;
print(typeof arr[kiter]);
var knumstr = '3';
print(obj[knumstr]);

// --- receiver kinds ---------------------------------------------------------
var s = 'hello';
var si = 1, sl = 'length';
print(s[si], s[sl]);
var nn = 42, nk = 'toFixed';
print(typeof nn[nk]);
var nul = null, und;
var pk = 'p';
// The wording of the TypeError differs between engines, so only its type and
// the fact that it names the property at all are compared.
try { print(nul[pk]); } catch (e) {
  print('caught null: ' + (e instanceof TypeError) + ' ' + (e.message.indexOf('p') >= 0));
}
try { print(und[pk]); } catch (e) {
  print('caught undefined: ' + (e instanceof TypeError) + ' ' + (e.message.indexOf('p') >= 0));
}

// --- a METHOD CALL through a computed local key, which must NOT fuse ---------
// `recv[mkey]()` does not emit OP_get_array_el; it emits OP_get_array_el2,
// which leaves the RECEIVER on the stack underneath the loaded function so the
// call can bind `this`. Patch 0020's header spends a paragraph arguing that the
// peephole must not match get_array_el2 -- the fused opcode has get_array_el's
// stack effect (2 in, 1 out) and firing it here pops the receiver the call still
// needs. Until these lines existed, NO TEST COVERED THAT ARGUMENT. Verified:
// against a build whose code_match() also accepts OP_get_array_el2, the first
// line below fails with `InternalError: stack underflow (op=248, pc=31)`.
var recv = { f: function () { return this === recv; } };
var mkey = 'f';
print(recv[mkey]());
var arrm = [3, 1, 2];
var jm = 'join', sm = 'sort';
print(arrm[jm]('-'), arrm[sm]()[0]);
var nested = { inner: { g: function () { return 'nested-this-ok'; } } };
var gk2 = 'g';
print(nested.inner[gk2]());
var strv = 'abc';
var upk = 'toUpperCase';
print(strv[upk]());
// With arguments, so that a wrong stack effect misaligns them rather than
// merely underflowing.
var adder = { add: function (a, b) { return a + b + (this === adder ? 0 : 100); } };
var addk = 'add';
print(adder[addk](1, 2));
// A method call whose key local is reassigned by the callee: the receiver and
// the function are already on the stack, so this must be unaffected.
var mutk = 'go';
var mutrecv = {
  go: function () {
    mutk = 'stop';
    return 'went';
  },
  stop: function () { return 'should-not-run'; },
};
print(mutrecv[mutk](), mutk);
// new through a computed key, and an optional call, both of which route
// differently again.
var Ctor = { C: function () { this.made = true; } };
var ck = 'C';
print(new Ctor[ck]().made);
print(recv[mkey]?.());

// A receiver that is the last reference to itself: the fused op frees the
// receiver after the read, and the read must complete first.
var rk = 'v';
print({ v: 'temp-receiver' }[rk]);

// --- getters, prototypes, proxies ------------------------------------------
var gk = 'x';
var withGetter = { get x() { return 'getter-ran'; } };
print(withGetter[gk]);

function Base() {}
Base.prototype.inherited = 'from-proto';
var child = new Base();
var ik = 'inherited';
print(child[ik]);

var proxied = new Proxy({ real: 1 }, {
  get: function (t, k) { return 'trap:' + String(k); },
});
var xk = 'real';
print(proxied[xk]);

// A getter that mutates the key local while it runs.
var mk = 'm';
var mutator = {
  get m() {
    mk = 'other';
    return 'getter-m';
  },
  other: 'should-not-be-read',
};
print(mutator[mk], mk);

// --- typed arrays and arguments --------------------------------------------
var ta = new Int32Array([7, 8, 9]);
var ti = 1, toob = 9;
print(ta[ti], ta[toob]);
var f64 = new Float64Array([1.5, 2.5]);
var fi = 0;
print(f64[fi]);

function argsTest() {
  var j = 1;
  return arguments[j];
}
print(argsTest('zero', 'one', 'two'));

// --- inside a for-in, which is the shape the RN prop path actually runs -----
var src = { alpha: 1, beta: 2, gamma: 3 };
var config = { alpha: 'A', gamma: 'C' };
var out = '';
for (var key in src) {
  out += key + '=' + src[key] + '/' + config[key] + ';';
}
print(out);

// --- the retired OP_put_arg1: writes to parameters 1..3 ----------------------
// put_arg1/2/3 no longer exist, so every one of these now emits the 3-byte
// general form. Anything that mis-encodes shows up as a wrong result here.
function writesArgs(a, b, c, d) {
  a = 'A'; b = 'B'; c = 'C'; d = 'D';
  return a + b + c + d;
}
print(writesArgs(1, 2, 3, 4));

function incArgs(a, b, c, d) {
  a++; b++; c++; d++;
  return [a, b, c, d].join(',');
}
print(incArgs(1, 2, 3, 4));

function argsAliasing(a, b) {
  b = 'changed';
  return arguments[1] + '|' + b;
}
print(argsAliasing('x', 'y'));

function defaulted(a, b, c) {
  if (b === undefined) b = 'db';
  if (c === undefined) c = 'dc';
  return a + b + c;
}
print(defaulted('a'));

function manyArgs(a0, a1, a2, a3, a4, a5) {
  a5 = a0; a4 = a1; a3 = a2; a2 = 'z'; a1 = 'y'; a0 = 'x';
  return [a0, a1, a2, a3, a4, a5].join('');
}
print(manyArgs('0', '1', '2', '3', '4', '5'));

// --- a key the local UNIQUELY owns -----------------------------------------
// This is the case that discriminates a fused handler which wrongly CONSUMES
// the borrowed key. A string literal is interned and referenced elsewhere, so
// one spurious free is invisible; a freshly concatenated string is owned only
// by the local, so freeing it leaves the local pointing at dead memory and the
// second read reports garbage or crashes. Verified to fail (a crash partway
// through the file) against a build whose handler calls JS_GetPropertyValue
// instead of JS_GetPropertyValueConst.
var blackholeSink = 0;
var uniq = { abc: 'unique-key-hit' };
var built = 'a';
built += 'b';
built += 'c';
print(uniq[built]);
print(uniq[built]);
print(built.length, built);
for (var rep = 0; rep < 50; rep++) {
  var churn = 'a' + 'b' + String(rep % 2 === 0 ? 'c' : 'd');
  blackholeSink += uniq[churn] === undefined ? 0 : 1;
}
print('churn ' + blackholeSink);

// --- keys through a with-scope and an eval, which must NOT fuse -------------
// Both force the key reference off the local frame; included so that a future
// change to the peephole's admission check fails here rather than silently.
function evalKey() {
  var k = 'a';
  var o = { a: 'eval-visible' };
  return eval('o[k]');
}
print(evalKey());
})();
