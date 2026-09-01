/* Adversarial corpus for the GENERALIZED (shape, key) -> property-index memo
   (patches/quickjs-ng/0026-property-index-memo-table.patch,
   docs/property-index-memo-table.md).

   This is the successor to tests/differential/corpus/for-in-element-load.js,
   which covers the ONE-ENTRY register of patch 0021. Keep both: the older file
   still discriminates the guards that survived into the table, and its cases
   run through a different fill path.

   ## What is different about the mechanism, and therefore about this file

   Patch 0021's register was written by `js_for_in_next` and could only ever
   describe the shape a `for-in` was walking. The table is written by the READ
   SITE, on a miss, for ANY string-keyed element load on a plain object. Three
   consequences, and all three are what the cases below attack:

     1. `for-in` is no longer involved. `o[k]` with `k` an ordinary variable
        fills and then hits. Cases 1-6 never mention `for-in` at all.
     2. The key no longer has to be an interned atom. `o['a' + i]` builds a
        FRESH JSString every time, and the table holds a counted reference on
        the exact JSString it was filled with. Cases 7-10 exercise that.
     3. Entries outlive the statement that created them and are keyed on the
        shape, so an entry filled from object A is read by object B whenever
        the two share a shape. Cases 11-16 make A and B disagree.

   ## What each guard is discriminated by

     guard                                        detected by
     -------------------------------------------  ---------------------------
     e->shape == p->shape                         cases 11, 12, 13, 17
     e->key == the key pointer                    cases 5, 6, 14
     idx < sh->prop_count  (BOUNDS)               not reachable from JS; see below
     prs->atom == e->atom                         cases 12, 13 (in combination)
     (prs->flags & JS_PROP_TMASK) == 0            cases 15, 16, 19
     receiver is a plain non-exotic object        not detectable by output diff
     the entry holds a shape REFERENCE            NOT testable at all; see below
     the entry holds a key REFERENCE              NOT testable at all; see below

   ## The guards no output diff can reach, and why

   Both counted references defend against ADDRESS RECYCLING, which needs the
   allocator to hand back an exact freed block at an exact moment. A build with
   either reference deleted produces byte-identical output on this entire
   corpus and on every workload in bench/workloads/. That is an accident of the
   allocator, not a semantic guarantee, and the argument for keeping them is in
   the patch header and in the js_propmemo_lookup() comment in quickjs.c. Do not
   delete them on the grounds that this file stays green.

   The bounds check is likewise unreachable from JavaScript: with the shape
   reference held, no entry can name an index outside its own shape. It exists
   to convert the address-recycling failure from an out-of-bounds READ into a
   miss, and it is one branch on an already-loaded word.

   Run with:
     node tests/differential/run.mjs
     node tests/differential/run.mjs --via-bytecode --qjsc <build>/qjsc */

var out = [];
function log(x) { out.push(String(x)); }

/* ------------------------------------------------------------------ */
/* 1-6: the plain read-site fill, with no for-in anywhere.             */
/* ------------------------------------------------------------------ */

/* 1. same shape, same key, different values: the entry must describe the
      SHAPE, so the second object's own value must come back. */
(function () {
  var a = {p: 'a-p', q: 'a-q'};
  var b = {p: 'b-p', q: 'b-q'};
  var k = 'p';
  log('1:' + a[k] + ',' + b[k] + ',' + a[k]);
})();

/* 2. every key of a wide object, twice, so that each key is filled on the
      first pass and served from the table on the second. */
(function () {
  var o = {}, keys = [], i, s = '';
  for (i = 0; i < 12; i++) { o['f' + i] = i * i; keys.push('f' + i); }
  for (i = 0; i < keys.length; i++) s += o[keys[i]] + ',';
  for (i = 0; i < keys.length; i++) s += o[keys[i]] + ',';
  log('2:' + s);
})();

/* 3. the value is overwritten between two loads of the same key. An
      overwrite takes no shape transition, so the entry stays valid and MUST
      report the new value. */
(function () {
  var o = {a: 1, b: 2};
  var k = 'b';
  var first = o[k];
  o.b = 99;
  log('3:' + first + ',' + o[k]);
})();

/* 4. a property is ADDED between two loads. The add transitions the object to
      a new shape, so the entry must stop applying to it. */
(function () {
  var o = {a: 1, b: 2};
  var k = 'a';
  var first = o[k];
  o.c = 3;
  log('4:' + first + ',' + o[k] + ',' + o.c);
})();

/* 5. two DIFFERENT keys on one shape, alternating, so the two entries must not
      be confused with each other. */
(function () {
  var o = {u: 'U', v: 'V', w: 'W'};
  var ks = ['u', 'v', 'w', 'v', 'u', 'w'], s = '', i;
  for (i = 0; i < ks.length; i++) s += o[ks[i]];
  log('5:' + s);
})();

/* 6. a key that is absent. Absent keys are not memoized, and the load must
      walk to the prototype and produce undefined -- both before and after the
      same object's present keys have filled entries. */
(function () {
  var o = {only: 1};
  log('6:' + o['only'] + ',' + o['missing'] + ',' + o['only'] +
      ',' + o['toString'].call({}));
})();

/* ------------------------------------------------------------------ */
/* 7-10: keys that are FRESH JSStrings, not interned atoms.            */
/* ------------------------------------------------------------------ */

/* 7. every key is built by concatenation, so no two loads share a key
      pointer, and no entry may ever be reused across them. */
(function () {
  var o = {k0: 'z', k1: 'y', k2: 'x'}, s = '', i;
  for (i = 0; i < 3; i++) s += o['k' + i];
  for (i = 2; i >= 0; i--) s += o['k' + i];
  log('7:' + s);
})();

/* 8. the SAME freshly built string object is reused, so it does keep one
      pointer, and the entry filled by the first load must serve the rest. */
(function () {
  var o = {alpha: 1, beta: 2};
  var k = 'al' + 'pha';
  log('8:' + o[k] + ',' + o[k] + ',' + o[k]);
})();

/* 9. two distinct string objects with equal contents. They intern to the same
      atom but are different pointers, so the second must MISS and refill --
      and must not be answered with a stale index if the shapes differ. */
(function () {
  var a = {m: 'A-m', n: 'A-n'};
  var b = {n: 'B-n', m: 'B-m'};   /* different key ORDER => different shape */
  var k1 = 'm';
  var k2 = String.fromCharCode(109); /* 'm', a different JSString */
  log('9:' + a[k1] + ',' + b[k2] + ',' + a[k2] + ',' + b[k1]);
})();

/* 10. array-index-like string keys on a plain (non-fast-array) object. These
       intern to TAGGED INT atoms rather than string atoms, which is a
       different path through JS_ValueToAtom. */
(function () {
  var o = {};
  o[0] = 'zero'; o[1] = 'one'; o.x = 'ex';
  var s = '';
  s += o['0']; s += o['1']; s += o['x']; s += o['0'];
  s += (o['2'] === undefined ? '-' : '?');
  log('10:' + s);
})();

/* ------------------------------------------------------------------ */
/* 11-16: entries read by an object other than the one that filled them */
/* ------------------------------------------------------------------ */

/* 11. shape divergence: this is exactly the RN diffProperties pattern and the
       reason the table exists. prev and next share three keys but next has a
       fourth, so their shapes differ and each needs its own entry. */
(function () {
  var prev = {color: 'red', width: 1, height: 2};
  var next = {color: 'blue', width: 1, height: 2, opacity: 0.5};
  var s = '';
  for (var k in next) s += k + ':' + prev[k] + '>' + next[k] + ';';
  log('11:' + s);
})();

/* 12. two shapes with the same property COUNT and the same key SET in a
       different order, so an index memoized for one names a different property
       in the other. */
(function () {
  var a = {one: 'a1', two: 'a2', three: 'a3'};
  var b = {three: 'b3', two: 'b2', one: 'b1'};
  var ks = ['one', 'two', 'three'], s = '', i;
  for (i = 0; i < 3; i++) s += a[ks[i]] + '/' + b[ks[i]] + ';';
  for (i = 0; i < 3; i++) s += b[ks[i]] + '/' + a[ks[i]] + ';';
  log('12:' + s);
})();

/* 13. same key at a DIFFERENT index in two shapes: 'target' is index 0 in one
       and index 2 in the other. A memo that ignored the shape would read the
       wrong slot. */
(function () {
  var a = {target: 'A', pad1: 1, pad2: 2};
  var b = {pad1: 1, pad2: 2, target: 'B'};
  log('13:' + a.target + b.target + a['target'] + b['target'] +
      a['target'] + b['target']);
})();

/* 14. one shape, and a load whose key is the NEXT key rather than the one just
       loaded, alternating -- attacks the key check specifically. */
(function () {
  var o = {aa: 'A', bb: 'B', cc: 'C'};
  var s = '', pairs = [['aa', 'bb'], ['bb', 'cc'], ['cc', 'aa']], i;
  for (i = 0; i < pairs.length; i++) s += o[pairs[i][0]] + o[pairs[i][1]];
  log('14:' + s);
})();

/* 15. an ACCESSOR at the same slot index in a same-shaped object. Two objects
       that each carry an accessor at slot 1 have EQUAL shapes, so the entry
       can name a JS_PROP_GETSET slot whose union is a getter/setter pair, not
       a value. Without the TMASK guard this prints a raw pointer. */
(function () {
  var mk = function (v) {
    var o = {lead: 'L'};
    Object.defineProperty(o, 'acc', {get: function () { return v; },
                                     enumerable: true, configurable: true});
    o.trail = 'T';
    return o;
  };
  var a = mk(10), b = mk(20);
  log('15:' + a['lead'] + a['acc'] + b['acc'] + a['acc'] + b['trail']);
})();

/* 16. a plain data property is CONVERTED to an accessor after it has been
       memoized. The conversion goes through js_shape_prepare_update, which
       must move the object to a cloned shape because the memo holds a
       reference -- so the entry must stop applying. */
(function () {
  var o = {d: 'data', e: 'other'};
  var before = o['d'];
  Object.defineProperty(o, 'd', {get: function () { return 'GET'; },
                                 configurable: true});
  log('16:' + before + ',' + o['d'] + ',' + o['e']);
})();

/* ------------------------------------------------------------------ */
/* 17-22: shape mutation, deletion, prototypes, freezing               */
/* ------------------------------------------------------------------ */

/* 17. delete after memoizing. delete_property also goes through
       js_shape_prepare_update and, with the memo holding a reference, clones. */
(function () {
  var o = {g: 1, h: 2, i: 3};
  var first = o['h'];
  delete o.g;
  log('17:' + first + ',' + o['h'] + ',' + o['i'] + ',' + o['g']);
})();

/* 18. the property lives on the PROTOTYPE, so no own-property entry may be
       filled, and shadowing it later must be observed immediately. */
(function () {
  var proto = {shared: 'from-proto'};
  var o = Object.create(proto);
  o.own = 1;
  var s = o['shared'];
  o.shared = 'from-own';
  log('18:' + s + ',' + o['shared'] + ',' + proto['shared']);
})();

/* 19. __proto__ swap between loads: the same own key must keep working and the
       inherited one must follow the new prototype. */
(function () {
  var p1 = {inh: 'p1'}, p2 = {inh: 'p2'};
  var o = Object.create(p1);
  o.own = 'O';
  var a = o['own'] + o['inh'];
  Object.setPrototypeOf(o, p2);
  log('19:' + a + ',' + o['own'] + o['inh']);
})();

/* 20. frozen and sealed objects, whose property flags differ from a plain
       object's while the value union is still a value. */
(function () {
  var a = Object.freeze({fa: 1, fb: 2});
  var b = Object.seal({fa: 3, fb: 4});
  var c = {fa: 5, fb: 6};
  log('20:' + a['fa'] + b['fa'] + c['fa'] + a['fb'] + b['fb'] + c['fb']);
})();

/* 21. an exotic receiver whose shape can equal a plain object's: a String
       wrapper and an arguments object. */
(function () {
  var s = '';
  var w = new String('str');
  w.extra = 'E';
  s += w['extra'] + w['length'] + w[0];
  var args = (function () { return arguments; })(1, 2, 3);
  s += ',' + args['length'] + args[0];
  log('21:' + s);
})();

/* 22. a Proxy with the same key set. Proxies are exotic and must never be
       served from the table; the trap must run every time. */
(function () {
  var hits = 0;
  var target = {px: 'T-px', py: 'T-py'};
  var p = new Proxy(target, {
    get: function (t, k, r) { hits++; return typeof k === 'string' && t[k] !== undefined ? 'P-' + t[k] : t[k]; }
  });
  var s = p['px'] + p['py'] + p['px'] + target['px'];
  log('22:' + s + ',' + hits);
})();

/* ------------------------------------------------------------------ */
/* 23-26: interaction with the for-in path the table replaced          */
/* ------------------------------------------------------------------ */

/* 23. the exact patch-0021 pattern, which must still be correct now that the
       fill comes from the read site rather than from js_for_in_next. */
(function () {
  var a = {x: 1, y: 2, z: 3}, b = {x: 1, y: 9, z: 3}, n = 0;
  for (var k in a) if (a[k] !== b[k]) n++;
  log('23:' + n);
})();

/* 24. receiver mutated inside its own for-in body. */
(function () {
  var a = {m: 1, n: 2, o: 3};
  var s = '';
  for (var k in a) { s += a[k]; a.extra = 1; delete a.extra; }
  log('24:' + s);
})();

/* 25. nested for-in over two objects, with cross loads in both directions. */
(function () {
  var A = {r: 'Ar', s: 'As'};
  var B = {r: 'Br', s: 'Bs'};
  var out2 = '';
  for (var i in A) for (var j in B) out2 += A[i] + B[j] + A[j] + B[i] + ';';
  log('25:' + out2);
})();

/* 26. a getter that mutates the receiver from inside the loop, so a shape
       transition happens between the yield and the load. */
(function () {
  var o = {p1: 1, p2: 2};
  Object.defineProperty(o, 'p3', {
    enumerable: true,
    get: function () { this.p4 = 4; return 3; }
  });
  var s = '';
  for (var k in o) s += k + '=' + o[k] + ';';
  log('26:' + s);
})();

print(out.join('\n'));
