/* Discriminator for patch 0078, the primitive-receiver / int-index arm on
   String.prototype.charCodeAt and String.prototype.charAt.

   The arm is taken only when the receiver is a primitive JS_TAG_STRING and the
   index argument is a JS_TAG_INT; everything else must reach the unchanged
   generic path.  This file exercises both sides of every one of those tests,
   and — importantly — it distinguishes charCodeAt, charAt and `at` from one
   another.

   THAT LAST POINT IS NOT DECORATION.  js_string_at, js_string_charCodeAt and
   js_string_charAt are adjacent in quickjs.c and their first six lines are
   identical, and the staged form of 0078 was applied by `git apply` — exit
   status 0 — with both hunks one function too early, so `"abc".charCodeAt(0)`
   returned "a" and `"abc".at(0)` returned 97.  A corpus that only checked
   charCodeAt against itself would not have noticed.  See
   patches/quickjs-ng/0078-charidx-primitive-receiver-arm.patch.

   Rope receivers are CONSTRUCTED rather than hoped for: `s = s + chunk` does
   not build a rope while chunk.length <= JS_STRING_ROPE_SHORT_LEN (512), and
   patch 0051 removes most of the rest, so the chunks below are deliberately
   longer than that. */

function show(label, v) {
  if (typeof v === 'string') print(label + ' ' + JSON.stringify(v));
  else print(label + ' ' + String(v));
}

/* ---- the three accessors must not be confused with one another ---------- */
var s = 'abc';
show('cca0', s.charCodeAt(0));
show('cca2', s.charCodeAt(2));
show('cha0', s.charAt(0));
show('cha2', s.charAt(2));
show('at0', s.at(0));
show('at2', s.at(2));
show('cpa0', s.codePointAt(0));

/* `at` takes a NEGATIVE index as an offset from the end; charAt and charCodeAt
   do not.  A fast arm written for one and grafted onto the other shows up
   here. */
show('at-1', s.at(-1));
show('at-3', s.at(-3));
show('at-4', s.at(-4));
show('at9', s.at(9));
show('cha-1', s.charAt(-1));
show('cca-1', s.charCodeAt(-1));

/* ---- out of range, boundaries ------------------------------------------ */
show('cca-len', s.charCodeAt(3));
show('cha-len', s.charAt(3));
show('cca-big', s.charCodeAt(2147483647));
show('cca-min', s.charCodeAt(-2147483648));
show('cha-big', s.charAt(2147483647));
show('empty-cca', ''.charCodeAt(0));
show('empty-cha', ''.charAt(0));

/* ---- index kinds that must NOT take the int arm ------------------------- */
show('frac-cca', s.charCodeAt(1.7));
show('frac-cha', s.charAt(1.7));
show('negzero-cca', s.charCodeAt(-0));
show('nan-cca', s.charCodeAt(NaN));
show('inf-cca', s.charCodeAt(Infinity));
show('-inf-cca', s.charCodeAt(-Infinity));
show('str-cca', s.charCodeAt('1'));
show('strjunk-cca', s.charCodeAt('nope'));
show('null-idx-cca', s.charCodeAt(null));
show('undef-idx-cca', s.charCodeAt(undefined));
show('true-idx-cca', s.charCodeAt(true));
show('missing-cca', s.charCodeAt());
show('missing-cha', s.charAt());
show('obj-idx-cca', s.charCodeAt({ valueOf: function () { return 2; } }));
show('obj-idx-cha', s.charAt({ valueOf: function () { return 2; } }));

/* valueOf must still run exactly once, and in the same order */
var calls = 0;
var counting = { valueOf: function () { calls++; return 1; } };
s.charCodeAt(counting);
s.charAt(counting);
show('valueOf-calls', calls);

/* ---- receiver kinds that must NOT take the primitive arm ---------------- */
show('obj-recv-cca', new String('xyz').charCodeAt(1));
show('obj-recv-cha', new String('xyz').charAt(1));
show('num-recv-cca', String.prototype.charCodeAt.call(1234, 2));
show('num-recv-cha', String.prototype.charAt.call(1234, 2));
show('float-recv-cca', String.prototype.charCodeAt.call(1.5, 1));
show('bool-recv-cca', String.prototype.charCodeAt.call(true, 1));
show('sym-desc-cha', String.prototype.charAt.call({ toString: function () { return 'ZZ'; } }, 1));
try { String.prototype.charCodeAt.call(null, 0); } catch (e) { show('null-recv', e.constructor.name); }
try { String.prototype.charCodeAt.call(undefined, 0); } catch (e) { show('undef-recv', e.constructor.name); }
try { String.prototype.charAt.call(null, 0); } catch (e) { show('null-recv-cha', e.constructor.name); }
try { String.prototype.charAt.call(undefined, 0); } catch (e) { show('undef-recv-cha', e.constructor.name); }

/* ---- wide strings, surrogate pairs ------------------------------------- */
var wide = 'aé中😀z';
for (var i = 0; i < wide.length; i++) show('wide' + i, wide.charCodeAt(i));
for (var j = 0; j < wide.length; j++) show('widec' + j, wide.charAt(j));
show('wide-cpa3', wide.codePointAt(3));

/* ---- SLICE / INDIRECT kinds of JS_TAG_STRING --------------------------- */
var base = '';
for (var k = 0; k < 64; k++) base += 'abcdefghijklmnop';
var sliced = base.slice(100, 300);
show('slice-cca', sliced.charCodeAt(0));
show('slice-cha', sliced.charAt(7));
show('slice-oob', sliced.charCodeAt(sliced.length));

/* ---- ROPE receivers: the arm must refuse them and still be correct ------ */
var chunkA = '';
for (var m = 0; m < 40; m++) chunkA += '0123456789abcdefghijklmnopqrstuvwxyzABCD';   /* 1600 chars */
var chunkB = chunkA.split('').reverse().join('');
var rope = chunkA + chunkB + chunkA;
show('rope-len', rope.length);
show('rope-cca-0', rope.charCodeAt(0));
show('rope-cca-mid', rope.charCodeAt(1600));
show('rope-cca-late', rope.charCodeAt(rope.length - 1));
show('rope-cha-mid', rope.charAt(1601));
show('rope-oob', rope.charCodeAt(rope.length));
show('rope-at', rope.at(-1));

/* a rope used repeatedly must give the same answer before and after it is
   linearized by some other operation */
var before = rope.charCodeAt(2000);
var forced = rope.indexOf('zzzzz');
var after = rope.charCodeAt(2000);
show('rope-stable', before === after ? 'same ' + before : 'DIFFER ' + before + '/' + after);
show('rope-indexof', forced);

/* ---- a hot loop, which is what the arm exists for ---------------------- */
var acc = 0;
var hot = 'The quick brown fox jumps over the lazy dog 0123456789';
for (var n = 0; n < 2000; n++) {
  for (var q = 0; q < hot.length; q++) acc = (acc + hot.charCodeAt(q)) | 0;
}
show('hot-sum', acc);

var built = '';
for (var r = 0; r < 40; r++) built += hot.charAt(r % hot.length);
show('hot-built', built);
