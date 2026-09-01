/* Adversarial corpus for HOLEY FAST ARRAYS
   (patch 0049-holey-fast-arrays.patch, docs/array-hole-representation.md).

   The mechanism under test lets a JS_CLASS_ARRAY keep its dense JSValue vector
   when a store skips indices, marking the skipped slots with
   JS_TAG_UNINITIALIZED and setting p->holey. Before the patch, one skipped
   index converted the array to a property hash table forever
   (convert_fast_array_to_array). A hole must be observationally identical to
   an ABSENT index: `in` false, hasOwnProperty false, not enumerated, JSON
   null, iteration undefined, prototype lookup visible through it.

   Every case below tries to make a hole look like something other than an
   absent index -- or to make a NON-hole in a holey array look absent, which is
   the mirror-image failure and the one a corpus written around `a[2]=x` misses.

   ## What each guard is discriminated by

     guard (engine site)                                    detected by
     -----------------------------------------------------  --------------
     js_get_fast_array_element hole test                     1, 2, 9, 14
     JS_GetPropertyInternal hole test (also: infinite loop)   9, 10
     JS_GetOwnPropertyInternal2 hole test (`in`, hasOwn)      3, 4
     JS_GetOwnPropertyNamesInternal count pass                5, 6
     JS_GetOwnPropertyNamesInternal emit pass                 5, 6, 7
     OP_get_array_el (plain load, compound assign, ++)        1, 25
     OP_get_array_el2 (computed-key nested destructuring)     26
     js_get_fast_array holey firewall (builtins)              11, 12, 13, 15
     build_arg_list !p->holey (apply / spread call)           14
     js_copy_elements !p->holey (copyWithin, splice)          16
     convert_fast_array_to_array skips holes                  17
     delete punches a hole instead of demoting                4, 6, 17
     set_array_length keeps hole_count exact                  18
     js_array_fill_hole / hole_count exactness (repack)       19, 20
     density rule declines and falls back to demotion         21
     JS_DefineProperty treats a hole as a CREATE              22
     JS_SetPropertyInternal2 proto-walk hole test             10
     OP_put_array_el hole test (store into a hole)            27 (HANGS)
     Array.prototype itself holey / std_array_prototype       27

   ## Not reachable by output diff

   **js_is_fast_array()'s holey test.** MEASURED 2026-07-30 by mutation: a build
   with it deleted passes this entire corpus, and that is correct rather than a
   corpus weakness. `js_is_fast_array` has exactly ONE caller
   (`vendor/quickjs-ng/quickjs.c`, in `js_array_slice`, guarding the fast
   slice/splice loop) and it is applied to `arr`, the DESTINATION returned by
   `JS_ArraySpeciesCreate`. Reaching the mutant requires a `Symbol.species`
   constructor that returns an already-holey array; even then the loop writes
   through `JS_CreateDataPropertyUint32Const`, whose append fast path demands
   `idx == p->u.array.count` and so declines at n=0 for any array with
   count > 0, falling to the hole-aware generic define. And `count == 0` implies
   `!holey`, because holes only ever exist below count and `set_array_length`
   clears the flag when it truncates the last one. The test is therefore
   defence in depth against a FUTURE second caller, kept deliberately, and no
   reachable input distinguishes it.

   The density rule's *thresholds* (JS_ARRAY_HOLE_MAX_GAP,
   JS_ARRAY_HOLE_DENSE_FLOOR, JS_ARRAY_HOLE_MIN_DENSITY) are a
   memory/representation policy, not a semantic one: both sides of every
   threshold must produce identical observable behaviour, which is exactly what
   case 21 asserts. A build with the thresholds changed is therefore expected
   to PASS this corpus; only a build that gets the *representation* wrong
   fails. hole_count drift is likewise invisible unless it reaches zero
   incorrectly -- case 19 and 20 are the ones that force that. */

function show(label, v) { print(label + ': ' + v); }
function keys(a) { return JSON.stringify(Object.keys(a)); }
function forin(a) { var r = []; for (var k in a) r.push(k); return JSON.stringify(r); }

/* ---- CASE 1: the canonical cliff. Array(n), first store at index 2. ---- */
(function () {
  var a = new Array(8);
  a[2] = 'x';
  show('1 len', a.length);
  show('1 a[0]', a[0]);
  show('1 a[1]', a[1]);
  show('1 a[2]', a[2]);
  show('1 a[7]', a[7]);
  show('1 typeof a[0]', typeof a[0]);
})();

/* ---- CASE 2: literal elision produces the same shape via a different path. */
(function () {
  var a = [1, , 3];
  show('2 len', a.length);
  show('2 a[1]', a[1]);
  show('2 1 in a', 1 in a);
  show('2 keys', keys(a));
})();

/* ---- CASE 3: `in` and hasOwnProperty over every index of a holey array. --- */
(function () {
  var a = [];
  a[3] = 'v';
  var r = [];
  for (var i = 0; i < 5; i++) r.push(i + ':' + (i in a) + ':' + a.hasOwnProperty(i));
  show('3', r.join(' '));
  show('3 desc0', JSON.stringify(Object.getOwnPropertyDescriptor(a, 0)));
  show('3 desc3', JSON.stringify(Object.getOwnPropertyDescriptor(a, 3)));
})();

/* ---- CASE 4: delete makes a hole; the deleted index must go absent, the
   others must stay present, and length must not move. -------------------- */
(function () {
  var a = [10, 11, 12, 13];
  delete a[1];
  show('4 len', a.length);
  show('4 1 in a', 1 in a);
  show('4 0 in a', 0 in a);
  show('4 a[1]', a[1]);
  show('4 keys', keys(a));
  delete a[1];                       /* deleting an existing hole */
  show('4 redelete keys', keys(a));
  show('4 delete rv', delete a[1]);
  show('4 delete oob rv', delete a[99]);
})();

/* ---- CASE 5: Object.keys / getOwnPropertyNames / for-in over holes,
   including a holey array that ALSO carries a string key. ---------------- */
(function () {
  var a = [];
  a[4] = 'e';
  a[1] = 'b';
  a.tail = 'z';
  show('5 keys', keys(a));
  show('5 gopn', JSON.stringify(Object.getOwnPropertyNames(a)));
  show('5 forin', forin(a));
  show('5 values', JSON.stringify(Object.values(a)));
  show('5 entries', JSON.stringify(Object.entries(a)));
})();

/* ---- CASE 6: for-in ORDER across holes, deletes and later refills. ------ */
(function () {
  var a = [0, 1, 2, 3, 4, 5];
  delete a[0];
  delete a[3];
  delete a[5];
  show('6 forin', forin(a));
  a[3] = 'back';
  show('6 refilled forin', forin(a));
  show('6 keys', keys(a));
})();

/* ---- CASE 7: a holey array used as a prototype -- for-in must walk both,
   and must not report an index the child shadows. ------------------------ */
(function () {
  var proto = [];
  proto[1] = 'p1';
  proto[3] = 'p3';
  var child = Object.create(proto);
  child[0] = 'c0';
  var r = [];
  for (var k in child) r.push(k);
  r.sort();
  show('7 forin', JSON.stringify(r));
  show('7 child[1]', child[1]);
  show('7 own1', child.hasOwnProperty(1));
})();

/* ---- CASE 8: JSON.stringify over holes -- must emit null, not undefined
   and not a crash, at every position including trailing. ----------------- */
(function () {
  var a = [];
  a[2] = 1;
  show('8 json', JSON.stringify(a));
  var b = [1, , 3, , ];
  show('8 json2', JSON.stringify(b));
  show('8 json nested', JSON.stringify({ k: a }));
  show('8 json indent', JSON.stringify(a, null, 1));
})();

/* ---- CASE 9: read through a hole must reach Array.prototype, not stop at
   the hole. This is the case that also detects the infinite recursion in
   JS_GetPropertyInternal if its hole test is missing. -------------------- */
(function () {
  Array.prototype[1] = 'FROM_PROTO';
  var a = [];
  a[3] = 'own3';
  show('9 a[1]', a[1]);
  show('9 1 in a', 1 in a);
  show('9 own1', a.hasOwnProperty(1));
  show('9 a[3]', a[3]);
  show('9 join', a.join('|'));
  delete Array.prototype[1];
})();

/* ---- CASE 10: WRITE through a hole on a prototype. The setter search must
   not stop at the hole. -------------------------------------------------- */
(function () {
  var proto = [];
  proto[2] = 'p2';
  var log = [];
  Object.defineProperty(proto, 1, {
    set: function (v) { log.push('setter:' + v); },
    get: function () { return 'getter1'; },
    configurable: true,
  });
  var child = Object.create(proto);
  child[1] = 'assigned';
  show('10 log', JSON.stringify(log));
  show('10 child[1]', child[1]);
  show('10 own1', child.hasOwnProperty(1));
})();

/* ---- CASE 11: the builtin firewall. Every method below has a dense fast
   path behind js_get_fast_array(); a hole must not leak through any. ----- */
(function () {
  var a = [1, , 3, , 5];
  show('11 join', a.join(','));
  show('11 toString', a.toString());
  show('11 indexOf u', a.indexOf(undefined));
  show('11 lastIndexOf u', a.lastIndexOf(undefined));
  show('11 includes u', a.includes(undefined));
  show('11 indexOf 3', a.indexOf(3));
  show('11 slice', JSON.stringify(a.slice(1, 4)));
  show('11 concat', JSON.stringify(a.concat([7])));
  show('11 reverse', JSON.stringify(a.reverse()));
  show('11 keys after reverse', keys(a));
})();

/* ---- CASE 12: the callback methods must SKIP holes, not call back with
   undefined. The visit log is the discriminator. ------------------------- */
(function () {
  var a = [];
  a[0] = 'a'; a[2] = 'c'; a[4] = 'e';
  var seen = [];
  a.forEach(function (v, i) { seen.push(i + '=' + v); });
  show('12 forEach', JSON.stringify(seen));
  show('12 map', JSON.stringify(a.map(function (v) { return v + '!'; })));
  show('12 map keys', keys(a.map(function (v) { return v; })));
  show('12 filter', JSON.stringify(a.filter(function () { return true; })));
  show('12 some', a.some(function (v) { return v === undefined; }));
  show('12 every', a.every(function (v) { return typeof v === 'string'; }));
  show('12 reduce', a.reduce(function (p, v) { return p + v; }, ''));
  var seen2 = [];
  a.reduceRight(function (p, v, i) { seen2.push(i); return p; }, 0);
  show('12 reduceRight idx', JSON.stringify(seen2));
})();

/* ---- CASE 13: sort must move holes to the end and keep them holes. ----- */
(function () {
  var a = [3, , 1, undefined, 2];
  a.sort();
  show('13 sorted', JSON.stringify(a));
  show('13 len', a.length);
  show('13 keys', keys(a));
  show('13 3 in a', 3 in a);
  show('13 4 in a', 4 in a);
  var b = [5, , 4];
  b.sort(function (x, y) { return x - y; });
  show('13 cmp sorted keys', keys(b));
})();

/* ---- CASE 14: apply / spread must pass undefined for a hole, and arity
   must count the hole. --------------------------------------------------- */
(function () {
  function f() {
    var r = [];
    for (var i = 0; i < arguments.length; i++) r.push(typeof arguments[i] + ':' + arguments[i]);
    return arguments.length + '|' + r.join(',');
  }
  var a = [];
  a[0] = 'p'; a[2] = 'q';
  show('14 apply', f.apply(null, a));
  show('14 spread', f.apply(null, [].concat(a)));
  show('14 args holey', f.apply(null, [1, , 3]));
})();

/* ---- CASE 15: iteration protocol -- for-of, Array.from, spread-into-array
   must all yield undefined for a hole (NOT skip it). --------------------- */
(function () {
  var a = [];
  a[1] = 'y';
  a.length = 3;
  var r = [];
  for (var i = 0; i < a.length; i++) r.push(String(a[i]));
  show('15 indexed', JSON.stringify(r));
  show('15 from', JSON.stringify(Array.from(a)));
  show('15 from keys', keys(Array.from(a)));
  show('15 arraykeys', JSON.stringify(Array.prototype.slice.call(a)));
})();

/* ---- CASE 16: copyWithin and splice move holes; the destination index must
   become ABSENT, not undefined. ------------------------------------------ */
(function () {
  var a = [1, 2, 3, 4, 5];
  delete a[1];
  a.copyWithin(3, 0, 3);
  show('16 copyWithin', JSON.stringify(a));
  show('16 keys', keys(a));
  show('16 4 in a', 4 in a);
  var b = [1, , 3, , 5];
  var rm = b.splice(1, 2);
  show('16 splice rv', JSON.stringify(rm) + ' keys ' + keys(rm));
  show('16 spliced', JSON.stringify(b) + ' keys ' + keys(b));
  var c = [1, , 3];
  c.unshift('u');
  show('16 unshift', JSON.stringify(c) + ' keys ' + keys(c));
  var d = [1, , 3];
  show('16 shift', d.shift() + ' then ' + JSON.stringify(d) + ' keys ' + keys(d));
  var e = [1, , 3];
  show('16 pop', e.pop() + ' then ' + JSON.stringify(e) + ' keys ' + keys(e));
})();

/* ---- CASE 17: force the demotion path on an array that ALREADY has holes.
   A getter/setter or a non-C_W_E descriptor demotes; the holes must not
   materialise as own properties on the way through. ---------------------- */
(function () {
  var a = [];
  a[1] = 'one';
  a[4] = 'four';
  Object.defineProperty(a, 2, { get: function () { return 'g2'; }, configurable: true, enumerable: true });
  show('17 keys', keys(a));
  show('17 0 in a', 0 in a);
  show('17 3 in a', 3 in a);
  show('17 a[2]', a[2]);
  show('17 a[3]', a[3]);
  show('17 json', JSON.stringify(a));
  show('17 forin', forin(a));

  var b = [];
  b[2] = 'x';
  Object.defineProperty(b, 0, { value: 'ro', writable: false, enumerable: true, configurable: false });
  show('17b keys', keys(b));
  show('17b 1 in b', 1 in b);
  b[0] = 'ignored';
  show('17b b[0]', b[0]);
})();

/* ---- CASE 18: length truncation across holes, and re-growth. ----------- */
(function () {
  var a = [];
  a[5] = 'f';
  a[2] = 'c';
  show('18 len', a.length);
  a.length = 3;
  show('18 keys', keys(a));
  show('18 a[2]', a[2]);
  show('18 5 in a', 5 in a);
  a.length = 6;
  show('18 regrown keys', keys(a));
  show('18 regrown', JSON.stringify(a));
  a.length = 0;
  show('18 zeroed keys', keys(a) + ' len ' + a.length);
  a[1] = 'again';
  show('18 reused', JSON.stringify(a) + ' keys ' + keys(a));
})();

/* ---- CASE 19: fill every hole back in; the array must behave exactly like a
   dense one afterwards, including through the builtin fast paths that only
   run on a re-packed array. ---------------------------------------------- */
(function () {
  var a = new Array(4);
  a[3] = 'd';
  a[0] = 'a';
  a[1] = 'b';
  a[2] = 'c';
  show('19 keys', keys(a));
  show('19 join', a.join('-'));
  show('19 json', JSON.stringify(a));
  show('19 indexOf', a.indexOf('c'));
  show('19 slice', JSON.stringify(a.slice(1)));
  show('19 reverse', JSON.stringify(a.reverse()));
  show('19 sort', JSON.stringify(a.sort()));
  show('19 forin', forin(a));
  show('19 every present', [0, 1, 2, 3].map(function (i) { return i in a; }).join(','));
})();

/* ---- CASE 20: interleave hole creation and hole filling many times, so a
   drifting hole counter reaches zero at the wrong moment. ---------------- */
(function () {
  var a = [];
  var log = [];
  for (var round = 0; round < 4; round++) {
    a[round * 3 + 2] = 'v' + round;         /* creates 2 holes */
    a[round * 3] = 'w' + round;             /* fills one back */
    log.push(Object.keys(a).length + '/' + a.length);
  }
  show('20 progression', log.join(' '));
  show('20 keys', keys(a));
  show('20 json', JSON.stringify(a));
  for (var i = 0; i < a.length; i++) if (!(i in a)) a[i] = 'fill' + i;
  show('20 dense keys len', Object.keys(a).length + '/' + a.length);
  show('20 dense join', a.join(','));
  delete a[5];
  show('20 after delete', keys(a) + ' ' + JSON.stringify(a));
})();

/* ---- CASE 21: BOTH SIDES OF THE DENSITY RULE. The engine represents these
   two differently (one holey-fast, one demoted to a property table) and they
   must be indistinguishable. --------------------------------------------- */
(function () {
  var near = [];
  near[100] = 'n';                    /* small gap: stays a fast holey array */
  var far = [];
  far[100000] = 'f';                  /* huge gap: demotes */
  var sparse = [];
  for (var i = 0; i < 60; i++) sparse[i * 200] = i;   /* density rule declines */
  function probe(a, label) {
    show(label + ' len', a.length);
    show(label + ' keys', keys(a));
    show(label + ' 0 in', 0 in a);
    show(label + ' first', a[Object.keys(a)[0]]);
    show(label + ' forin len', JSON.parse(forin(a)).length);
    show(label + ' json len', JSON.stringify(a).length);
  }
  probe(near, '21 near');
  probe(far, '21 far');
  probe(sparse, '21 sparse');
  show('21 sparse join len', sparse.join(',').length);
})();

/* ---- CASE 22: defineProperty ON a hole is a CREATE, not a modify -- so it
   must respect extensibility and non-default descriptors. ---------------- */
(function () {
  var a = [];
  a[2] = 'two';
  Object.preventExtensions(a);
  var threw = false;
  try { Object.defineProperty(a, 0, { value: 'nope', writable: true, enumerable: true, configurable: true }); }
  catch (e) { threw = e instanceof TypeError; }
  show('22 preventExt define threw', threw);
  show('22 0 in a', 0 in a);
  a[1] = 'silent';                  /* sloppy-mode assignment: silently fails */
  show('22 1 in a', 1 in a);
  show('22 a[2]', a[2]);

  var b = [];
  b[2] = 'two';
  Object.defineProperty(b, 0, { value: 'ro', writable: false, enumerable: false, configurable: true });
  show('22b keys', keys(b));
  show('22b gopn', JSON.stringify(Object.getOwnPropertyNames(b)));
  show('22b desc', JSON.stringify(Object.getOwnPropertyDescriptor(b, 0)));
  show('22b 1 in b', 1 in b);

  var c = [];
  c[2] = 'two';
  Object.seal(c);
  show('22c sealed', Object.isSealed(c) + ' ' + keys(c));
  show('22c frozen empty', Object.isFrozen(Object.freeze([])));
})();

/* ---- CASE 23: holes must survive a round trip through the places that
   rebuild an array wholesale. ------------------------------------------- */
(function () {
  var a = [];
  a[1] = 'b';
  a[4] = 'e';
  show('23 flat', JSON.stringify([a].flat()) + ' keys ' + keys([a].flat()));
  show('23 fill', JSON.stringify(a.slice().fill('F', 0, 2)));
  show('23 find', a.find(function (v) { return v === undefined; }));
  show('23 findIndex', a.findIndex(function (v) { return v === undefined; }));
  show('23 keys iter', JSON.stringify(Array.prototype.slice.call(a).map(String)));
  show('23 sortcopy', JSON.stringify(a.slice().sort()));
})();

/* ---- CASE 24: Arguments objects must NOT acquire holes -- delete on a
   fast Arguments still demotes, and mapped arguments must stay mapped. --- */
(function () {
  function f(x, y) {
    delete arguments[0];
    return (0 in arguments) + '|' + arguments[1] + '|' + arguments.length + '|' + x;
  }
  show('24 args', f('a', 'b'));
  function g(x) {
    arguments[0] = 'changed';
    return x;
  }
  show('24 mapped', g('orig'));
})();

/* ---- CASE 25: COMPOUND ASSIGNMENT AND ++ over a hole. These compile to
   OP_get_array_el2 (see the OP_get_array_el -> OP_get_array_el2 rewrite in
   the parser), which keeps the receiver on the stack and is a SEPARATE
   interpreter fast path from OP_get_array_el. Added 2026-07-30 after a
   mutation test showed cases 1-24 did not reach it: every one of them reads
   `a[i]` in a plain load, so a build with OP_get_array_el2's hole test deleted
   passed the whole corpus. `hole + 1` is NaN and `hole++` is NaN; a build that
   reads the raw JS_TAG_UNINITIALIZED instead produces something else. --- */
(function () {
  var a = [];
  a[1] = 10;
  a[3] = 30;
  a[5] = 50;
  a.length = 6;
  var log = [];
  for (var i = 0; i < 6; i++) {
    var b = [];
    b[1] = 10; b[3] = 30; b[5] = 50; b.length = 6;
    b[i] += 1;
    log.push(i + ':' + b[i] + ':' + (i in b));
  }
  show('25 compound +=', log.join(' '));

  var log2 = [];
  for (var j = 0; j < 6; j++) {
    var c = [];
    c[1] = 10; c[3] = 30; c[5] = 50; c.length = 6;
    c[j]++;
    log2.push(j + ':' + c[j]);
  }
  show('25 postincrement', log2.join(' '));

  var d = [];
  d[2] = 4;
  d.length = 4;
  d[0] |= 8;
  d[1] = (d[1] || 'dflt');
  d[3] = (typeof d[3]) + '/' + (d[3] === undefined);
  show('25 bitwise+logical', JSON.stringify(d));
  show('25 keys', keys(d));

  /* string concatenation reads the hole through the same opcode */
  var e = [];
  e[1] = 'x';
  e.length = 3;
  e[0] += '!';
  e[2] += '?';
  show('25 concat', JSON.stringify(e));

  /* and the receiver-on-stack path with a computed, non-literal index */
  var f = [];
  f[2] = 7;
  f.length = 4;
  var idx = 0;
  f[idx] += 100;
  idx = 2;
  f[idx] += 100;
  show('25 computed idx', JSON.stringify(f) + ' keys ' + keys(f));
})();

/* ---- CASE 26: OP_get_array_el2, which is NOT what compound assignment
   compiles to. MEASURED 2026-07-30 with a dynamic opcode census: `b[i] += 1`,
   `b[i]++` and `b[0] += 'x'` execute OP_get_array_el (3 times) and
   OP_get_array_el2 ZERO times, so case 25 -- written to reach el2 -- does not,
   and a build with el2's hole test deleted passed cases 1-25 in silence. The
   census says el2 is emitted for DESTRUCTURING WITH A COMPUTED KEY AND A
   NESTED PATTERN (see the OP_get_array_el2 emit in js_parse_destructuring_element:
   it requires the next token to be `[` or `{`). Confirmed: the three
   destructurings below execute get_array_el2=3.

   The DEFAULT (`= {...}`) is what makes this discriminate rather than merely
   execute. A hole read must produce exactly `undefined`, because the default is
   applied by a `strict_eq undefined` test; a build that yields the raw
   JS_TAG_UNINITIALIZED fails that test and does not default. Without the
   default, both the correct and the broken engine throw a TypeError and the
   diff sees only the message. --------------------------------------------- */
(function () {
  var src = [];
  src[2] = { q: 'present' };
  src.length = 5;
  var out = [];
  var k = 2;
  var t1 = function () { var { [k]: { q } } = src; return q; };
  out.push('k2=' + t1());
  k = 0;                                  /* a HOLE */
  var t2 = function () { var { [k]: { q } = { q: 'DFLT0' } } = src; return q; };
  out.push('k0=' + t2());
  k = 4;                                  /* a HOLE at the end */
  var t3 = function () { var { [k]: { q } = { q: 'DFLT4' } } = src; return q; };
  out.push('k4=' + t3());
  show('26 computed destructure', out.join(' '));

  /* nested ARRAY pattern reaches the same opcode */
  var src2 = [];
  src2[1] = [7, 8];
  src2.length = 3;
  var j = 0;
  var u1 = function () { var { [j]: [a, b] = ['DA', 'DB'] } = src2; return a + '/' + b; };
  show('26 nested array k0', u1());
  j = 1;
  var u2 = function () { var { [j]: [a, b] = ['DA', 'DB'] } = src2; return a + '/' + b; };
  show('26 nested array k1', u2());

  /* the hole must be `undefined`, not absent, to the default machinery */
  var src3 = [];
  src3[1] = { q: 'q1' };
  src3.length = 3;
  var m, r = [];
  for (m = 0; m < 3; m++) {
    var kk = m;
    var g = function () { var { [kk]: { q } = { q: 'D' + kk } } = src3; return q; };
    r.push(m + ':' + g());
  }
  show('26 sweep', r.join(' '));
})();

/* ---- CASE 27: Array.prototype ITSELF becoming a holey fast array, and the
   ctx->std_array_prototype flag.

   Added 2026-07-30 after tracing why the OP_put_array_el mutant HUNG rather
   than merely differing. `Array.prototype[1] = x` now keeps Array.prototype a
   holey FAST array, so it goes through neither add_shape_property (which clears
   ctx->std_array_prototype for a small-integer key) nor
   convert_fast_array_to_array (which clears it unconditionally). The flag
   therefore stays SET while Array.prototype has an indexed element -- which is
   not what its name suggests, and it guards the three append fast paths in
   JS_SetPropertyValue, OP_put_array_el and js_array_push.

   Why that is nonetheless correct, and what these cases assert:
     * a fast-array element is ALWAYS C_W_E, hence writable, and a writable
       inherited data property cannot block a plain assignment -- so it does not
       need to disable the append path (parts 1 and 4);
     * making one non-writable requires a descriptor that is not C_W_E, which
       routes through JS_DefineProperty's convert_to_slow_array and DOES clear
       the flag, so a non-writable inherited element still refuses an append and
       still throws in strict mode (part 2);
     * an accessor takes the same route, so a prototype setter still intercepts
       (part 3), including when Array.prototype was already holey when the
       accessor arrived (part 4).
   MEASURED: byte-identical to node v22.20.0. ------------------------------- */
(function () {
  var log = [];

  /* 1. plain data element on Array.prototype, then append to a child at it */
  Array.prototype[1] = 'PROTO1';
  var a = ['own0'];
  a[1] = 'own1';
  show('27 a1', a[1] + ' own=' + a.hasOwnProperty(1) + ' len=' + a.length);
  var b = ['z'];
  b.push('pushed');
  show('27 push', b[1] + ' own=' + b.hasOwnProperty(1));
  var c = [];
  show('27 inherited', c[1] + ' own=' + c.hasOwnProperty(1));
  c[0] = 'c0';
  c[1] = 'c1';
  show('27 shadowed', JSON.stringify(c) + ' ' + keys(c));
  delete Array.prototype[1];
  show('27 cleanup', 1 in Array.prototype);

  /* 2. a NON-WRITABLE inherited element must refuse the append */
  Object.defineProperty(Array.prototype, 2,
                        { value: 'RO2', writable: false, configurable: true });
  var d = ['x', 'y'];
  d[2] = 'attempt';
  show('27 ro sloppy', d[2] + ' own=' + d.hasOwnProperty(2) + ' len=' + d.length);
  var e = ['x', 'y'];
  var threw = false;
  try { (function () { 'use strict'; e[2] = 'attempt'; })(); }
  catch (err) { threw = err instanceof TypeError; }
  show('27 ro strict threw', threw + ' own=' + e.hasOwnProperty(2));
  var e2 = ['x', 'y'];
  var pushThrew = false;
  /* Array.prototype.push is spec'd to throw on a failed [[Set]] regardless of
     mode. Only the error TYPE is compared: the two engines word the message
     differently and that is not a conformance difference. */
  try { e2.push('viapush'); } catch (err) { pushThrew = err instanceof TypeError; }
  show('27 ro push threw', pushThrew + ' own=' + e2.hasOwnProperty(2) +
       ' len=' + e2.length);
  delete Array.prototype[2];

  /* 3. a SETTER must intercept the append */
  Object.defineProperty(Array.prototype, 3, {
    set: function (v) { log.push('set:' + v); },
    get: function () { return 'GET3'; },
    configurable: true,
  });
  var f = ['p', 'q', 'r'];
  f[3] = 'intercepted';
  show('27 setter', JSON.stringify(log) + ' f3=' + f[3] +
       ' own=' + f.hasOwnProperty(3) + ' len=' + f.length);
  var g = ['p', 'q', 'r'];
  g.push('viapush');
  show('27 setter push', JSON.stringify(log) + ' g3=' + g[3] +
       ' own=' + g.hasOwnProperty(3));
  delete Array.prototype[3];

  /* 4. accessor arriving when Array.prototype is ALREADY holey */
  Array.prototype[5] = 'PROTO5';
  Object.defineProperty(Array.prototype, 6, {
    set: function (v) { log.push('set6:' + v); },
    configurable: true,
  });
  var h = [0, 1, 2, 3, 4, 5];
  h[6] = 'x6';
  show('27 holey proto', JSON.stringify(log) + ' own6=' + h.hasOwnProperty(6) +
       ' h5=' + h[5] + ' own5=' + h.hasOwnProperty(5));
  delete Array.prototype[5];
  delete Array.prototype[6];
  show('27 final', (5 in Array.prototype) + ' ' + (6 in Array.prototype));
})();
