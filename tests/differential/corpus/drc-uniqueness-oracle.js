// Adversarial corpus for the `ref_count == 1` UNIQUENESS ORACLE under deferred
// reference counting's invariant A (the operand stack stops being counted).
//
// docs/refcount-as-uniqueness-oracle.md predicts that invariant A breaks two
// sites, both about in-place string append:
//
//   quickjs.c JS_ConcatString   `if (JS_REF_COUNT(p1) == 1 && ...)`
//   quickjs.c js_accum_append   `if (JS_REF_COUNT(p1) != 1 + stack_ref)`
//
// The mechanism, in one sentence: a string held by a local AND sitting on the
// operand stack reads 2 today and 1 under invariant A, so the "I am the only
// holder, mutate in place" test fires on data the local still points at, and
// the local's string silently grows.  That is a WRONG ANSWER, not a slowdown,
// and it surfaces far from its cause -- so it is tested for here rather than
// hoped about.
//
// Every case below is arranged so that:
//   (a) the receiver of the append has SPARE CAPACITY, which is what makes the
//       in-place path reachable at all (js_malloc_usable_size >= the joined
//       length); building it by concatenation is what produces the slack;
//   (b) a SECOND live reference exists that is NOT the operand-stack one, so
//       corruption is observable by printing it afterwards;
//   (c) the consumer is varied -- store to a local, store to a property, pass
//       as an argument, push into an array, return -- because the existing
//       identity guard only catches the store-to-the-same-slot shape.
//
// Run it in BOTH arms (QJS_DRC_A unset and QJS_DRC_A=1); the two outputs and
// node's must be byte-identical.

function slack(n) {
  // Build a string with allocator slack by repeated concatenation, then force
  // it flat (a rope has no in-place path to abuse).
  var s = '';
  for (var i = 0; i < n; i++) s += 'ab';
  return s.charAt(0) === 'a' ? s : s;
}

function tag(name, v) { print(name + ' ' + v.length + ' ' + v.charAt(0) + v.charAt(v.length - 1)); }

/* --- 1. the doc's exact shape: local + operand stack, consumer never stores */
(function () {
  var t = slack(40);
  var seen = [];
  function sink(x) { seen.push(x.length); }
  sink(t + 'Z');
  tag('C1-t', t);
  print('C1-sink ' + seen.join(','));
})();

/* --- 2. store to a DIFFERENT local (the identity guard's blind spot) ------ */
(function () {
  var t = slack(40);
  var u = t + 'Z';
  tag('C2-t', t);
  tag('C2-u', u);
})();

/* --- 3. store into an object property (the put_field fused arm) ----------- */
(function () {
  var t = slack(40);
  var o = { k: '' };
  o.k = t + 'Z';
  tag('C3-t', t);
  tag('C3-k', o.k);
})();

/* --- 4. push into an array (a consuming callee) --------------------------- */
(function () {
  var t = slack(40);
  var a = [];
  a.push(t + 'Z');
  tag('C4-t', t);
  tag('C4-a0', a[0]);
})();

/* --- 5. the accumulate shape, with an ALIAS taken mid-loop ---------------- */
(function () {
  var s = '';
  var snapshot = null;
  for (var i = 0; i < 60; i++) {
    s += 'xy';
    if (i === 30) snapshot = s;      // second live reference from here on
  }
  tag('C5-s', s);
  tag('C5-snap', snapshot);
})();

/* --- 6. alias held in a property rather than a local ---------------------- */
(function () {
  var box = { v: slack(40) };
  var t = box.v;                      // two counted holders + a stack borrow
  var u = t + 'Z';
  tag('C6-box', box.v);
  tag('C6-t', t);
  tag('C6-u', u);
})();

/* --- 7. alias captured by a closure --------------------------------------- */
(function () {
  var t = slack(40);
  var get = function () { return t; };
  var u = t + 'Z';
  tag('C7-closure', get());
  tag('C7-u', u);
})();

/* --- 8. the receiver is the RESULT of a call (no local holds it) ---------- */
(function () {
  function mk() { return slack(40); }
  var u = mk() + 'Z';
  tag('C8-u', u);
})();

/* --- 9. append in a loop while an array holds every intermediate ---------- */
(function () {
  var s = slack(10);
  var keep = [];
  for (var i = 0; i < 20; i++) {
    keep.push(s);
    s = s + 'q';
  }
  var lens = [];
  for (var j = 0; j < keep.length; j++) lens.push(keep[j].length);
  print('C9-lens ' + lens.join(','));
  tag('C9-s', s);
})();

/* --- 10. compound assignment to a property with a live alias -------------- */
(function () {
  var o = { s: slack(30) };
  var alias = o.s;
  o.s += 'TAIL';
  tag('C10-alias', alias);
  tag('C10-s', o.s);
})();

/* --- 11. wide (16-bit) strings: the oracle also tests is_wide_char -------- */
(function () {
  var w = '';
  for (var i = 0; i < 40; i++) w += 'é中';
  var alias = w;
  var u = w + 'ÿ';
  tag('C11-alias', alias);
  tag('C11-u', u);
  print('C11-eq ' + (alias === w));
})();

/* --- 12. narrow receiver, wide appendage (the fast path must refuse) ------ */
(function () {
  var n = slack(30);
  var alias = n;
  var u = n + '中';
  tag('C12-alias', alias);
  tag('C12-u', u);
})();

/* --- 13. the value is on the stack twice ---------------------------------- */
(function () {
  var t = slack(40);
  var u = (t + 'A') + (t + 'B');
  tag('C13-t', t);
  tag('C13-u', u);
})();

/* --- 14. exception thrown between the borrow and the append --------------- */
(function () {
  var t = slack(40);
  var out = '';
  try {
    out = t + ({ toString: function () { throw new Error('x'); } });
  } catch (e) {
    out = 'threw';
  }
  tag('C14-t', t);
  print('C14-out ' + out);
})();

/* --- 15. a getter that mutates the alias while the concat is in flight ---- */
(function () {
  var t = slack(40);
  var side = null;
  var o = { get v() { side = t; return 'Z'; } };
  var u = t + o.v;
  tag('C15-t', t);
  tag('C15-side', side);
  tag('C15-u', u);
})();
