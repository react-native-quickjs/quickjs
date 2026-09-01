/*
 * Object-literal semantics, aimed at the shape-template fast path (patch 0002)
 * and at its bytecode round-trip (patch 0015).
 *
 * The template path pre-builds a shape and allocates from it, so anything that
 * makes property order, descriptor flags, or key identity differ from the
 * generic path shows up here. Run it twice -- once from source and once via
 * `--via-bytecode` -- because the template table only survives serialization
 * since 0015, and a table that came back wrong would silently produce objects
 * with the right keys in the wrong order.
 *
 * Deterministic, ES5-printable, no host objects.
 */

function show(o) {
  var keys = Object.keys(o);
  var parts = [];
  for (var i = 0; i < keys.length; i++) {
    var k = keys[i];
    var d = Object.getOwnPropertyDescriptor(o, k);
    var v;
    if ('value' in d) {
      v = typeof d.value === 'object' && d.value !== null ? '[obj]' : String(d.value);
    } else {
      v = '<' + (d.get ? 'get' : '') + (d.set ? 'set' : '') + '>';
    }
    parts.push(k + '=' + v + (d.writable ? 'w' : '') + (d.enumerable ? 'e' : '') + (d.configurable ? 'c' : ''));
  }
  print('{' + parts.join(' ') + '} proto=' + (Object.getPrototypeOf(o) === Object.prototype ? 'Object' : String(Object.getPrototypeOf(o))));
}

/* --- the shapes real code allocates ------------------------------------- */

for (var i = 0; i < 3; i++) {
  show({ type: 'div', key: null, ref: null, props: { a: i }, _owner: null });
  show({ x: i, y: i + 1 });
  show({});
  show({ a: 1, b: 2, c: 3, d: 4, e: 5, f: 6, g: 7, h: 8 });
}

/* --- exactly at, and past, the eligibility cap --------------------------- */

show({ k0: 0, k1: 1, k2: 2, k3: 3, k4: 4, k5: 5, k6: 6, k7: 7,
       k8: 8, k9: 9, k10: 10, k11: 11, k12: 12, k13: 13, k14: 14, k15: 15 });
show({ k0: 0, k1: 1, k2: 2, k3: 3, k4: 4, k5: 5, k6: 6, k7: 7,
       k8: 8, k9: 9, k10: 10, k11: 11, k12: 12, k13: 13, k14: 14, k15: 15,
       k16: 16 });
show({ k0: 0, k1: 1, k2: 2, k3: 3, k4: 4, k5: 5, k6: 6, k7: 7,
       k8: 8, k9: 9, k10: 10, k11: 11, k12: 12, k13: 13, k14: 14, k15: 15,
       k16: 16, k17: 17, k18: 18, k19: 19, k20: 20, k21: 21, k22: 22,
       k23: 23, k24: 24, k25: 25, k26: 26, k27: 27, k28: 28, k29: 29,
       k30: 30, k31: 31, k32: 32 });

/* --- the cases that must NOT take the fast path -------------------------- */

var computed = 'dyn';
show({ a: 1, get b() { return 2; }, c: 3 });
show({ a: 1, set b(v) { this._b = v; }, c: 3 });
show({ a: 1, b: 2, a: 3 });                       /* duplicate key */
show({ __proto__: null, a: 1 });                  /* proto-mutating literal */
show({ a: 1, ['x' + 'y']: 2, b: 3 });             /* computed key */
show({ a: 1, [computed]: 2 });
show({ 0: 'z', 1: 'o', a: 'a' });                 /* array-index keys first */
show({ a: 'a', 0: 'z', 10: 't', 2: 'w' });        /* mixed, order matters */
show({ 'has space': 1, '': 2, 'a-b': 3 });
var sh = 7;
show({ sh: sh, m: function () { return 1; } });

/* --- mutation after allocation ------------------------------------------ */

var o1 = { a: 1, b: 2, c: 3 };
delete o1.b;
o1.d = 4;
o1.b = 5;
show(o1);

var o2 = { a: 1, b: 2 };
Object.defineProperty(o2, 'a', { value: 9, writable: false, enumerable: false, configurable: false });
show(o2);

var o3 = { a: 1, b: 2 };
Object.freeze(o3);
o3.a = 99;
show(o3);
print('frozen a=' + o3.a + ' isFrozen=' + Object.isFrozen(o3));

/* --- shape sharing across two literals with the same keys ---------------- */

function mk(v) { return { p: v, q: v * 2 }; }
var acc = [];
for (var j = 0; j < 5; j++) acc.push(mk(j));
for (var j = 0; j < 5; j++) show(acc[j]);
print('same-keys ' + Object.keys(acc[0]).join(',') + '|' + Object.keys(acc[4]).join(','));

/* --- spread and rest interaction ----------------------------------------- */

var base = { a: 1, b: 2 };
show(Object.assign({}, base, { c: 3 }));
show({ z: 0, ...base });
var { a: ra, ...rest } = { a: 1, b: 2, c: 3 };
print('rest ' + ra + ' ' + JSON.stringify(rest));

/* --- JSON round-trip preserves the order the template produced ----------- */

print(JSON.stringify({ type: 'view', key: 'k', props: { style: { flex: 1 }, children: [] } }));
print(JSON.stringify([{ a: 1, b: 2 }, { a: 3, b: 4 }]));

/* --- for-in and Object.entries over templated objects -------------------- */

var seen = [];
for (var k in { one: 1, two: 2, three: 3 }) seen.push(k);
print('for-in ' + seen.join(','));
print('entries ' + JSON.stringify(Object.entries({ one: 1, two: 2, three: 3 })));
