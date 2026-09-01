/* Adversarial corpus for the for-in pinned-shape element-load mechanism
   (docs/for-in-pinned-shape-spike.md, docs/mutation-testing-the-safety-nets.md).

   The mechanism under test replaces the generic string-keyed `o[k]` load with a
   direct `p->prop[i]` read when a one-entry runtime register — written at the
   pinned `for-in` yield site — says that this receiver's shape is the pinned
   shape and this key is the key just yielded. Every case below tries to make
   that direct read return something other than what the specification requires.

   CASES 1-15 were written alongside the measurement. CASES 16-24 were added
   after a mutation test showed the first fifteen did not discriminate the key
   check: every one of them loads `o[k]` where `k` IS the loop variable, so the
   key matches by construction and a build with the key comparison deleted
   passed silently. Cases 16-24 exist to load a key that is NOT the yielded one
   from a receiver that DOES share the pinned shape.

   ## What each guard is discriminated by

     guard                                     detected by
     ----------------------------------------  ---------------------------
     (pr->flags & JS_PROP_TMASK) == 0          case 15
     correct slot index                        cases 1, 13, 16
     key == the yielded atom                   cases 16, 17, 18, 23, 24
     receiver is a plain non-exotic object     NOT detectable by output; see below
     the register holds a shape REFERENCE      NOT testable at all; see below

   ## The two guards no output diff can reach, and why

   1. **Holding a counted reference on the register's shape.** If the register
      keeps a raw `JSShape *`, the shape can be freed when the loop ends and a
      NEW shape can be allocated at the same address, after which
      `p->shape == reg.shape` compares equal against a different layout. This
      cannot be turned into a deterministic test: it needs the allocator to hand
      back the exact freed block, and in practice deleting the reference makes
      the comparison FAIL rather than wrongly succeed, because
      `js_shape_prepare_update` (vendor/quickjs-ng/quickjs.c:12103-12129) and
      `add_property` (:11108-11123) *clone* a shape whose refcount is not 1 —
      so removing our reference changes which pointer the object ends up on and
      the fast path simply misses. A sabotaged build with the reference removed
      produced byte-identical output on this entire corpus.
      This guard must therefore be argued from the source, not tested: it is
      required because `compact_properties` (:6672, called from :11230) frees
      and reallocates shape storage with no regard for reference counts, and
      because the allocator is free to reuse the address. Do not delete it on
      the grounds that the corpus stays green.

   2. **The `class_id == JS_CLASS_OBJECT && !is_exotic && !fast_array` guard.**
      Case 22 constructs a String wrapper whose shape is pointer-identical to a
      pinned shape and loads a yielded key from it, which is the only way to
      reach the guard at all. The *value* it returns is the same either way,
      because the exotic behaviour of every reachable exotic class concerns
      index keys (which a pinned `for-in` never yields) or VARREF slots (which
      the TMASK check already rejects). So this guard is defence-in-depth: case
      22 proves the fast path can be entered on an exotic receiver, and a
      hit-rate counter — not an output diff — is what shows whether the guard
      is doing anything. Keep it: it is measured to cost zero hits. */
var out = [];
function log(x) { out.push(String(x)); }

/* 1. plain two-object React pattern, same shape */
(function () {
  var a = {x: 1, y: 2, z: 3}, b = {x: 1, y: 9, z: 3}, n = 0;
  for (var k in a) if (a[k] !== b[k]) n++;
  log('1:' + n);
})();

/* 2. two objects, different shape (different key order) */
(function () {
  var a = {x: 1, y: 2}, b = {y: 2, x: 1}, n = 0;
  for (var k in a) if (a[k] !== b[k]) n++;
  log('2:' + n);
})();

/* 3. getter on the receiver */
(function () {
  var a = {p: 1, q: 2};
  var b = {p: 1};
  Object.defineProperty(b, 'q', {get: function () { return 42; }, enumerable: true});
  var s = '';
  for (var k in a) s += k + '=' + b[k] + ';';
  log('3:' + s);
})();

/* 4. receiver mutated mid-loop (shape changes underneath) */
(function () {
  var a = {m: 1, n: 2, o: 3};
  var s = '';
  for (var k in a) { s += a[k]; a.extra = 1; delete a.extra; }
  log('4:' + s);
})();

/* 5. receiver is an array with a string key */
(function () {
  var src = {0: 'a', len: 1};
  var arr = [7, 8, 9];
  var s = '';
  for (var k in src) s += k + ':' + arr[k] + ';';
  log('5:' + s);
})();

/* 6. receiver is a String wrapper (exotic; index keys synthesized) */
(function () {
  var src = {0: 1, 1: 2};
  var w = new String('hi');
  var s = '';
  for (var k in src) s += w[k] + ';';
  log('6:' + s);
})();

/* 7. receiver is `arguments` */
(function () { return (function () {
  var src = {0: 1, 1: 2, length: 0};
  var s = '';
  for (var k in src) s += arguments[k] + ';';
  log('7:' + s);
})('A', 'B'); })();

/* 8. receiver is a Proxy whose target has the same shape as the enumerated obj */
(function () {
  var t = {u: 1, v: 2};
  var p = new Proxy(t, {get: function (o, k) { return 'P' + k; }});
  var s = '';
  for (var k in t) s += p[k] + ';';
  log('8:' + s);
})();

/* 9. nested for-in over two objects with different shapes */
(function () {
  var outer = {a: 1, b: 2};
  var inner = {c: 3, d: 4, e: 5};
  var s = '';
  for (var i in outer) { for (var j in inner) s += outer[i] + '' + inner[j]; s += '|'; }
  log('9:' + s);
})();

/* 10. property deleted from the OTHER object mid-loop */
(function () {
  var a = {g: 1, h: 2, i: 3};
  var b = {g: 1, h: 2, i: 3};
  var s = '';
  for (var k in a) { s += b[k]; delete b.i; }
  log('10:' + s);
})();

/* 11. prototype shadowing: key exists only on the prototype of the receiver */
(function () {
  var proto = {w: 'proto-w'};
  var recv = Object.create(proto);
  recv.q = 'own-q';
  var src = {q: 0, w: 0};
  var s = '';
  for (var k in src) s += recv[k] + ';';
  log('11:' + s);
})();

/* 12. frozen / non-writable receiver with the same shape */
(function () {
  var a = {r: 1, s: 2};
  var b = Object.freeze({r: 10, s: 20});
  var s = '';
  for (var k in a) s += b[k] + ';';
  log('12:' + s);
})();

/* 13. same-shape receiver holding every value tag */
(function () {
  var a = {t1: 0, t2: 0, t3: 0, t4: 0, t5: 0, t6: 0};
  var b = {t1: 1.5, t2: 'str', t3: true, t4: null, t5: undefined, t6: {n: 1}};
  var s = '';
  for (var k in a) s += typeof b[k] + ';';
  log('13:' + s);
})();

/* 14. the enumerated object itself is replaced by a same-shape twin */
(function () {
  var a = {p1: 1, p2: 2};
  var twin = {p1: 100, p2: 200};
  var s = '';
  for (var k in a) s += twin[k] + ',' + a[k] + ';';
  log('14:' + s);
})();


/* 15. BOTH objects carry the accessor at the same slot, so their shapes are
   EQUAL and the pinned index names a JS_PROP_GETSET slot. This is the case that
   makes the flags check load-bearing; the verifier must report it as skipped,
   not as a hit. */
(function () {
  function mk(v) {
    var o = {a: v};
    Object.defineProperty(o, 'g', {get: function () { return v * 10; }, enumerable: true, configurable: true});
    o.b = v + 1;
    return o;
  }
  var src = mk(1), other = mk(2);
  var s = '';
  for (var k in src) s += other[k] + ';';
  log('15:' + s);
})();

/* ------------------------------------------------------------------------
   16-24: the key check. Each loads a key OTHER than the one `for-in` just
   yielded, from a receiver that DOES have the pinned shape, so a mechanism
   that checks only the shape returns the wrong slot.
   ---------------------------------------------------------------------- */

/* 16. rotated keys: same-shaped receiver, deliberately mismatched key */
(function () {
  var a = {k1: 1, k2: 2, k3: 3};
  var b = {k1: 'b1', k2: 'b2', k3: 'b3'};
  var rot = ['k3', 'k1', 'k2'];
  var i = 0, s = '';
  for (var k in a) s += k + '->' + b[rot[i++]] + ';';
  log('16:' + s);
})();

/* 17. a CONSTANT key that is first in shape order, loaded on every iteration.
   The shape matches every time and the pinned index walks 0,1,2 while the key
   being asked for stays at index 0. */
(function () {
  var a = {first: 'A', second: 'B', third: 'C'};
  var b = {first: 'a', second: 'b', third: 'c'};
  var fixed = 'first';
  var s = '';
  for (var k in a) s += b[fixed];
  log('17:' + s);
})();

/* 18. a getter runs a NESTED for-in over a same-shaped object, clobbering the
   register, and the outer load then happens with the inner loop's index still
   published. */
(function () {
  var inner = {n1: 'i1', n2: 'i2', n3: 'i3'};
  var recv = {n1: 'r1', n2: 'r2', n3: 'r3'};
  var src = {n1: 0, n2: 0, n3: 0};
  var probe = {};
  Object.defineProperty(probe, 'trigger', {get: function () {
    var t = 0;
    for (var j in inner) t++;
    return t;
  }});
  var s = '';
  for (var k in src) s += probe.trigger + ':' + recv[k] + ';';
  log('18:' + s);
})();

/* 19. setPrototypeOf mid-loop. A prototype swap changes the SHAPE without
   changing the property layout, so a mechanism comparing layouts rather than
   shape pointers keeps hitting; and the key resolves differently afterwards. */
(function () {
  var pa = {p: 'PA'}, pb = {p: 'PB'};
  var src = {q: 1, p: 2, r: 3};
  var twin = {q: 'Q', p: 'P', r: 'R'};
  var thin = Object.create(pa); thin.q = 'q'; thin.r = 'r';
  var s = '', n = 0;
  for (var k in src) {
    s += twin[k] + thin[k] + ';';
    if (n++ === 0) { Object.setPrototypeOf(twin, pa); Object.setPrototypeOf(thin, pb); }
  }
  log('19:' + s);
})();

/* 20. an accessor installed mid-loop at an ALREADY-VISITED slot, on both the
   enumerated object and the receiver, so the two shapes stay in step and the
   pinned index comes to name a JS_PROP_GETSET slot during the loop rather than
   before it. */
(function () {
  var src = {w1: 1, w2: 2, w3: 3};
  var recv = {w1: 'A', w2: 'B', w3: 'C'};
  var s = '', n = 0;
  for (var k in src) {
    s += recv[k] + ';';
    if (n++ === 0) {
      var d = {get: function () { return 'GET'; }, enumerable: true, configurable: true};
      Object.defineProperty(src, 'w1', d);
      Object.defineProperty(recv, 'w1', d);
    }
  }
  log('20:' + s);
})();

/* 21. delete-and-re-add index churn, on the receiver and then on the
   enumerated object itself. compact_properties may move every slot. */
(function () {
  var src = {d1: 1, d2: 2, d3: 3};
  var recv = {d1: 'A', d2: 'B', d3: 'C'};
  var s = '', n = 0;
  for (var k in src) {
    s += recv[k] + ';';
    if (n++ === 0) { delete recv.d1; recv.d1 = 'Z'; }
  }
  var s2 = '', m = 0;
  for (var k2 in src) {
    s2 += src[k2] + ';';
    if (m++ === 0) { delete src.d1; src.d1 = 9; }
  }
  log('21:' + s + '|' + s2);
})();

/* 22. a String wrapper whose shape is hand-matched to the pinned shape: same
   prototype, same property transitions in the same order with the same flags,
   so the two objects land on the SAME JSShape. This is the only construction
   that reaches the class_id/is_exotic/fast_array guard at all. */
(function () {
  var w = new String('hi');
  w.tag = 'W';
  var src = Object.create(String.prototype);
  Object.defineProperty(src, 'length', {value: 2, writable: false, enumerable: false, configurable: false});
  src.tag = 'S';
  var s = '';
  for (var k in src) s += k + '=' + w[k] + ';';
  log('22:' + s);
})();

/* 23. the key is re-derived through string operations, so it is a fresh
   JSString with no pointer identity to the yielded atom, and it names a
   DIFFERENT property of a same-shaped receiver. */
(function () {
  var src = {e1: 1, e2: 2, e3: 3};
  var recv = {e1: 'A', e2: 'B', e3: 'C'};
  var s = '';
  for (var k in src) {
    var derived = 'e' + (4 - Number(k.charAt(1)));
    s += recv[derived];
  }
  log('23:' + s);
})();

/* 24. two for-in loops genuinely interleaved: a generator suspended inside its
   own for-in is advanced from the body of another for-in, so the register is
   overwritten between the outer yield and the outer load. */
(function () {
  var A = {t1: 'a1', t2: 'a2', t3: 'a3'};
  var B = {t1: 'b1', t2: 'b2', t3: 'b3'};
  var C = {t1: 'c1', t2: 'c2', t3: 'c3'};
  function walk() { return (function* () { for (var j in B) yield j; })(); }
  var g = walk();
  g.next();
  var s = '';
  for (var k in A) {
    var j = g.next().value;
    s += C[k] + '/' + (j === undefined ? '-' : C[j]) + ';';
  }
  log('24:' + s);
})();

print(out.join('\n'));
