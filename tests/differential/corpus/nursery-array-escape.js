/* Small array literals that leave the function that made them by every route. */
var sink = { held: null, list: [] };
var m = new Map(), st = new Set(), wm = new WeakMap();
function mk(i) { return [i, i + 1, 'x' + i]; }
function viaStore(i) { var a = mk(i); sink.held = a; return a.length; }
function viaPush(i) { var a = [i, {k: i}]; sink.list.push(a); return a[1].k; }
function viaClosure(i) { var a = [i, i * 2]; return function () { return a[0] + a[1]; }; }
function viaMapKey(i) { var a = [i]; m.set(a, i); st.add(a); wm.set(a, i); return m.get(a) + (st.has(a) ? 1 : 0) + wm.get(a); }
function viaSpread(i) { var a = [i, i + 1, i + 2]; var b = [...a, ...a]; return b.length + Math.max(...a); }
function viaApply(i) { var a = [i, 2]; return Math.min.apply(null, a) + Math.max.apply(null, a); }
function viaJSON(i) { return JSON.stringify([[i, i + 1], [i]]) + JSON.parse('[1,2,3]').length; }
function viaMethods(i) { var a = [3, 1, 2, i]; a.sort(); var b = a.map(function (x) { return x * 2; }); var c = b.filter(function (x) { return x > 2; }); return a.join() + '|' + c.slice(1).join() + '|' + a.concat([9]).length + '|' + a.indexOf(i); }
function viaGrow(i) { var a = [i, 1]; for (var k = 0; k < 20; k++) a.push(k); a[30] = 1; a.length = 25; return a.length + a[0]; }
function viaProp(i) { var a = [i, 2]; a.foo = 'bar'; return a.foo + a.length; }
function viaFreeze(i) { var a = Object.freeze([i, 2]); return Object.isFrozen(a) + ',' + a[0]; }
function viaDefine(i) { var a = [i, 2]; Object.defineProperty(a, 0, { get: function () { return 7; } }); return a[0]; }
function viaProto(i) { var a = [i, 2]; Object.setPrototypeOf(a, null); return a.length; }
function viaIter(i) { var a = [i, 2, 3]; var s = 0; for (var x of a) s += x; for (var k in a) s += +k; var it = a[Symbol.iterator](); it.next(); return s + it.next().value; }
function viaDestructure(i) { var [x, y] = [i, i + 1]; var [p, ...q] = [1, 2, 3]; return x + y + p + q.length; }
function viaArgs(i) { return (function () { return arguments.length + [].slice.call(arguments).length; })([i], [i, i]); }
function viaGenerator(i) { function* g() { yield [i, 1]; yield [i, 2]; } var s = 0; for (var a of g()) s += a[1]; return s; }
function viaPromise(i) { var r = []; Promise.resolve([i, 5]).then(function (a) { r.push(a[1]); }); return r.length; }
function viaThrow(i) { try { throw [i, 1]; } catch (e) { return e[0] + e[1]; } }
function viaTyped(i) { return new Uint8Array([i, 2, 3]).length + new Float64Array([1.5, i]).length; }
function viaFrom(i) { return Array.from([i, 2]).length + Array.of(i, 1).length + [i, [i, [i]]].flat(2).length; }
function viaToString(i) { return String([i, 2]) + ([i] + '') + [[i]].toString(); }
function viaStrict(i) { 'use strict'; var a = [i, 2]; try { a.length = -1; } catch (e) { return e.name; } }
var fns = [viaStore, viaPush, viaMapKey, viaSpread, viaApply, viaJSON, viaMethods, viaGrow, viaProp, viaFreeze, viaDefine, viaProto, viaIter, viaDestructure, viaArgs, viaGenerator, viaPromise, viaThrow, viaTyped, viaFrom, viaToString, viaStrict];
var out = [], closures = [];
for (var round = 0; round < 3; round++)
    for (var f = 0; f < fns.length; f++)
        for (var i = 0; i < 400; i++) { var r = fns[f](i); if (i % 133 == 0) out.push(fns[f].name + ':' + i + '=' + r); closures.push(viaClosure(i)); if (closures.length > 50) closures.shift(); }
var s = 0; for (var c = 0; c < closures.length; c++) s += closures[c]();
print(out.join('\n')); print('closures', s, 'held', String(sink.held), 'list', sink.list.length, 'map', m.size, st.size);
for (var churn = 0, keep; churn < 200000; churn++) keep = [churn, {a: churn}, [churn]];
print('after churn', String(sink.held), String(sink.list[5]), String(mk(3)));
