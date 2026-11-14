#!/usr/bin/env node
/*
 * Grammar-based JS program generator for the differential fuzzer.
 *
 *   node tools/fuzz/gen.mjs SEED OUTDIR
 *
 * Writes OUTDIR/prog.js (the program, with a main(mode) entry), OUTDIR/train.js (calls main with the
 * modes the profile may see) and OUTDIR/run.js (the same program, then a run input that leaves the
 * profile: other types, new classes, changed prototypes, thrown errors, wrong arity). The three files
 * print only deterministic values, so any difference between engines or modes is a finding.
 */
import { writeFileSync, mkdirSync } from 'node:fs';
import { join } from 'node:path';

const seed = Number(process.argv[2] ?? 1) >>> 0;
const out = process.argv[3] ?? '.';
let s = seed || 1;
function rnd() { s |= 0; s = (s + 0x6d2b79f5) | 0; let t = Math.imul(s ^ (s >>> 15), 1 | s); t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t; return ((t ^ (t >>> 14)) >>> 0) / 4294967296; }
const ri = (n) => Math.floor(rnd() * n);
const pick = (a) => a[ri(a.length)];
const chance = (p) => rnd() < p;

const nClasses = 2 + ri(3);
const fieldNames = ['a', 'b', 'c', 'd', 'e', 'f'];
const classes = [];
for (let i = 0; i < nClasses; i++) {
  const nf = 2 + ri(4);
  const fs = [];
  while (fs.length < nf) { const f = pick(fieldNames); if (!fs.includes(f)) fs.push(f); }
  const late = chance(0.5) ? pick(['late', 'extra', 'tag']) : null;
  const proto = chance(0.4) ? pick(['pd', 'pe']) : null;
  classes.push({ name: 'K' + i, fs, late, proto, kids: chance(0.3) });
}

function numExpr(env, depth) {
  if (depth <= 0 || chance(0.25)) return pick([String(ri(10)), String(ri(1000) / 8), '0.5', '3', 'i', 'n', '-1', '2147483647', '1e10']);
  const k = ri(8);
  if (k === 0) return `(${numExpr(env, depth - 1)} + ${numExpr(env, depth - 1)})`;
  if (k === 1) return `(${numExpr(env, depth - 1)} - ${numExpr(env, depth - 1)})`;
  if (k === 2) return `(${numExpr(env, depth - 1)} * ${numExpr(env, depth - 1)})`;
  if (k === 3) return `(${numExpr(env, depth - 1)} / (${numExpr(env, depth - 1)} || 1))`;
  if (k === 4) return `((${numExpr(env, depth - 1)} | 0) & 1023)`;
  if (k === 5) return `Math.${pick(['sqrt', 'abs', 'floor', 'min', 'max'])}(${numExpr(env, depth - 1)}${chance(0.4) ? ', ' + numExpr(env, depth - 1) : ''})`;
  if (k === 6 && env.objs.length) { const o = pick(env.objs); return `(${o.v}.${pick(o.cls.fs)})`; }
  return `(${numExpr(env, depth - 1)} % 7)`;
}

let out_ = [];
const emit = (x) => out_.push(x);
for (const c of classes) {
  emit(`function ${c.name}(${c.fs.map((f) => 'p_' + f).join(', ')}) {`);
  for (const f of c.fs) emit(`  this.${f} = p_${f};`);
  if (c.kids) emit(`  this.kid = p_${c.fs[0]} > 100 ? null : new Vec(p_${c.fs[0]}, 1, 2);`);
  emit('}');
  if (c.proto) emit(`${c.name}.prototype.${c.proto} = ${ri(5)};`);
  emit(`${c.name}.prototype.sum = function () { var s = 0; ${c.fs.map((f) => `s += this.${f};`).join(' ')} return s;${c.proto ? ` };` : ' };'}`);
  emit(`${c.name}.prototype.scale = function (k) { ${c.fs.map((f) => `this.${f} = this.${f} * k;`).join(' ')} return this; };`);
}
emit('function Vec(x, y, z) { this.x = x; this.y = y; this.z = z; }');
emit('Vec.prototype.dot = function (o) { return this.x * o.x + this.y * o.y + this.z * o.z; };');
emit('function mix(a, b) { return a * 0.5 + b; }');
const nFns = 3 + ri(3);
const fnCls = [];
for (let i = 0; i < nFns; i++) {
  const c = pick(classes);
  fnCls.push(c);
  const env = { objs: [{ v: 'o', cls: c }] };
  emit(`function f${i}(o, n) {`);
  emit('  var acc = 0, i = 0;');
  emit(`  for (i = 0; i < n; i++) {`);
  const body = 1 + ri(4);
  for (let j = 0; j < body; j++) {
    const k = ri(7);
    if (k === 0) emit(`    acc += ${numExpr(env, 3)};`);
    else if (k === 1) emit(`    if (${numExpr(env, 2)} > ${numExpr(env, 2)}) acc -= ${numExpr(env, 2)}; else acc += 1;`);
    else if (k === 2) emit(`    var t${j} = new ${pick(classes).name}(${pick(classes)[0] ? '' : ''}${Array.from({ length: 6 }, () => numExpr(env, 1)).join(', ')});`), emit(`    acc += t${j}.sum();`);
    else if (k === 3) emit(`    o.${pick(c.fs)} = ${numExpr(env, 2)};`);
    else if (k === 4) emit(`    acc += o.sum() * ${numExpr(env, 1)};`);
    else if (k === 5 && c.late) emit(`    if (i === 2) o.${c.late} = ${numExpr(env, 1)}; acc += (o.${c.late} || 0);`);
    else emit(`    acc = mix(acc, ${numExpr(env, 2)});`);
  }
  emit('  }');
  if (chance(0.4)) emit(`  if (n === 13) throw new Error('boom' + acc);`);
  emit('  return acc;');
  emit('}');
}
emit('function rec(k, o) { return k <= 0 ? o.sum() : rec(k - 1, o) + k; }');
emit('function main(mode) {');
emit('  var res = [], arr = [];');
const mainCalls = [];
emit('  for (var r = 0; r < 6; r++) {');
for (let i = 0; i < nFns; i++) {
  const c = fnCls[i];
  emit(`    var obj${i} = new ${c.name}(${c.fs.map(() => ri(20) + (chance(0.3) ? 0.5 : 0)).join(', ')});`);
  emit(`    res.push(f${i}(obj${i}, ${3 + ri(8)}));`);
  emit(`    arr.push(obj${i});`);
  mainCalls.push(i);
}
emit('    res.push(rec(5, arr[arr.length - 1]));');
emit('  }');
emit('  var tot = 0; for (var q = 0; q < res.length; q++) tot += res[q];');
emit('  return tot + arr.length;');
emit('}');
// Coroutine shapes (drawn after everything above, so a seed's earlier program is unchanged): objects made between
// suspensions and read after more allocation, in generators and async functions; stores far past an array's end.
const nCo = 1 + ri(3);
for (let i = 0; i < nCo; i++) {
  const c = pick(classes);
  const f = pick(c.fs);
  emit(`function* g${i}(o, n) {`);
  emit('  for (var i = 0; i < n; i++) {');
  emit(`    var held = { v: i, w: { x: ${numExpr({ objs: [{ v: 'o', cls: c }] }, 1)} }, k: new ${c.name}(${c.fs.map(() => ri(9)).join(', ')}) };`);
  if (chance(0.5)) emit(`    var got = yield held.v + held.w.x;`);
  else emit(`    var got = yield held;`);
  emit(`    for (var j = 0; j < ${2 + ri(30)}; j++) o.${f} = { j: j }.j + (got === undefined ? 0 : 1);`);
  emit(`    if (held.k.sum() !== held.k.sum()) throw new Error('held');`);
  emit(`    yield held.w.x + held.k.${f};`);
  emit('  }');
  emit('  return n;');
  emit('}');
  emit(`async function a${i}(o, n) {`);
  emit('  var acc = 0;');
  emit('  for (var i = 0; i < n; i++) {');
  emit(`    var before = { i: i, p: [i, i + 1] };`);
  emit(`    await ${chance(0.5) ? 'null' : `idle${i}()`};`);
  emit(`    var mid = { b: before, q: new ${c.name}(${c.fs.map(() => ri(9)).join(', ')}) };`);
  emit(`    acc += await take${i}(mid.q.sum() + before.p[1]);`);
  emit(`    for (var j = 0; j < ${1 + ri(20)}; j++) acc += { j: j }.j & 1;`);
  emit('    acc += mid.b.i + mid.b.p[0];');
  emit('  }');
  emit('  return acc;');
  emit('}');
  emit(`async function idle${i}() { }`);
  emit(`async function take${i}(x) { return x; }`);
}
emit('function far(n) { var a = []; for (var k = 0; k < n; k++) a[k * ' + (50 + ri(400)) + '] = { k: k, s: [k] }; var t = 0; for (var q in a) t += a[q].k + a[q].s[0]; return t + a.length; }');
emit('function genSum(it) { var t = 0, r = it.next(); var c = 0; while (!r.done) { t += typeof r.value === "object" ? r.value.v + r.value.w.x : r.value; r = it.next(c++); } return t + r.value; }');
// A forward jump over a run of field accesses whose cache operands make the code grow past an 8-bit offset (the input gap is
// just under 128 bytes for some run lengths).
{
  const c = classes[0];
  const nput = 2 + ri(13);
  const seq = Array.from({ length: nput }, (_, q) => `this.p.${c.fs[q % c.fs.length]} = this.c.${c.fs[(q + 1) % c.fs.length]}`).join(', ');
  emit(`function FJ(o) { this.c = o; this.p = new ${c.name}(${c.fs.map(() => 0).join(', ')}); }`);
  emit(`FJ.prototype.run = function (k) { var i = k && this.c.${c.fs[0]} !== this.p.${c.fs[0]}; return i && (${seq}), i ? this.p.sum() : -1; };`);
  emit(`function fj(o, k) { return new FJ(o).run(k); }`);
}
const coCalls = [];
for (let i = 0; i < nCo; i++) coCalls.push(`genSum(g${i}(new ${classes[0].name}(1, 2, 3, 4, 5, 6), ${2 + ri(6)}))`);
emit(`function coMain() { return ${coCalls.join(' + ')} + far(${3 + ri(10)}) + fj(new ${classes[0].name}(1, 2, 3, 4, 5, 6), 1) + fj(new ${classes[0].name}(1, 2, 3, 4, 5, 6), 0); }`);
const prog = out_.join('\n') + '\n';
const classNames = classes.map((c) => c.name);

// the run input: changes the profile does not contain
const variants = [];
variants.push(`print('base', main(0));`);
variants.push(`try { f0('str', 4); print('strarg'); } catch (e) { print('strarg threw', e instanceof TypeError); }`);
variants.push(`try { print('null', f0(null, 3)); } catch (e) { print('null threw', e instanceof TypeError); }`);
variants.push(`try { print('thr', f0(new ${classNames[0]}(1, 2, 3, 4, 5, 6), 13)); } catch (e) { print('thr', String(e.message).length > 0); }`);
variants.push(`${classNames[0]}.prototype.sum = function () { return 42; }; print('proto', main(1));`);
variants.push(`${classNames[0]}.prototype.extraProto = 7; Object.prototype.polluted = 1; print('pollute', main(2)); delete Object.prototype.polluted;`);
variants.push(`print('wrongarity', f0(new ${classNames[0]}(1), 5), f0(new ${classNames[0]}(), 5));`);
variants.push(`var big = 0; for (var z = 0; z < 50; z++) big += main(3); print('loop', big);`);
variants.push(`print('str+num', mix('a', 1), mix(1, 'b'), mix(2147483647, 1), mix(-0, 0));`);
variants.push(`var oo = new ${classNames[0]}(1, 2, 3, 4, 5, 6); oo.zz = 1; delete oo.${classes[0].fs[0]}; print('shape', f0(oo, 4));`);
variants.push(`print('gc', (function () { for (var g = 0; g < 200; g++) main(4); if (typeof gc === 'function') gc(); return main(5); })());`);
variants.push(`print('rec', rec(40, new ${classNames[0]}(1, 2, 3, 4, 5, 6)));`);
variants.push(`print('co', coMain(), coMain());`);
variants.push(`(function () { var ps = []; for (var z = 0; z < ${20 + ri(200)}; z++) ps.push(a${ri(nCo)}(new ${classNames[0]}(1, 2, 3, 4, 5, 6), ${1 + ri(5)})); Promise.all(ps).then(function (v) { var t = 0; for (var q = 0; q < v.length; q++) t += v[q]; print('async', v.length, t); }, function (e) { print('async threw', String(e)); }); })();`);
const trainSeq = `for (var t = 0; t < 400; t++) main(0);\nfor (var t = 0; t < 40; t++) coMain();\nprint('train', main(0), coMain());`;
mkdirSync(out, { recursive: true });
writeFileSync(join(out, 'prog.js'), prog);
writeFileSync(join(out, 'train.js'), `${prog}${trainSeq}\n`);
writeFileSync(join(out, 'run.js'), `${prog}${trainSeq}\n${variants.join('\n')}\n`);
