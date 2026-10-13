/* `f.apply(this, arguments)` argument forwarding (patch 0231, second hunk).
 *
 * The mechanism: a record call that cannot borrow the caller's outgoing window
 * (because the callee is not `argv_safe` -- it writes a parameter, captures one,
 * builds mapped arguments, or runs eval) copies the arguments into its own
 * frame's `arg_buf`, and patch 0231's second hunk points the frame's `argv` at
 * that copy. `regvm_t_apply_arguments`'s fast path tests `arg_buf == argv` to
 * decide whether `arguments` can be forwarded as the caller's own window; with
 * `argv` left on the caller's window the test failed for every forwarder (none
 * is `argv_safe`) and the handler materialized a mapped `arguments` object on
 * every call.
 *
 * Every function below that contains `f.apply(this, arguments)` is therefore
 * NOT `argv_safe`, takes the owned-copy branch, and reads `arguments` from the
 * copy. The observable must be identical to reading the caller's window, which
 * is what node and the stack build do.
 *
 * Each case prints one line; the stack build, the register build and node must
 * agree byte for byte.
 */

function show(x) {
  if (typeof x === 'string') return JSON.stringify(x);
  if (x === undefined) return 'undefined';
  if (x === null) return 'null';
  if (typeof x === 'object') {
    if (Object.prototype.toString.call(x) === '[object Array]') {
      var out = [], i;
      for (i = 0; i < x.length; i++) out.push(show(x[i]));
      return '[' + out.join(',') + ']';
    }
    var ks = Object.keys(x);
    for (i = 0; i < ks.length; i++) out.push(ks[i] + ':' + show(x[ks[i]]));
    return '{' + out.join(',') + '}';
  }
  return String(x);
}
function t(name, v) { print(name + ' = ' + show(v)); }

/* ---------- a forwarder with a zero-length declared list ---------- */

/* The raytrace shape: `arguments` is the only view a zero-parameter forwarder
   has of what it was called with. 0, 1, 3 and 300 arguments. */
function fwd0() { return target.apply(null, arguments); }
function target() {
  return arguments.length + ':' + Array.prototype.slice.call(arguments).join('|');
}
t('zero0', fwd0());
t('zero1', fwd0('a'));
t('zero3', fwd0('a', 'b', 'c'));
var big = [];
for (var i = 0; i < 300; i++) big.push(i);
t('zero300len', fwd0.apply(null, big).length);
t('zero300head', fwd0.apply(null, big).slice(0, 12));
t('zero300tail', fwd0.apply(null, big).slice(-12));

/* ---------- declared count vs passed count, both directions ---------- */

function fwd2(a, b) { return target.apply(null, arguments); }
t('fwd2-0', fwd2());
t('fwd2-1', fwd2(1));
t('fwd2-3', fwd2(1, 2, 3));
t('fwd2-5', fwd2(1, 2, 3, 4, 5));

function fwd5(a, b, c, d, e) { return target.apply(null, arguments); }
t('fwd5-0', fwd5());
t('fwd5-2', fwd5(1, 2));
t('fwd5-5', fwd5(1, 2, 3, 4, 5));

/* ---------- the 0231 case: a callee that writes its parameters ---------- */

/* The callee writes its parameters; the forwarder must then report its OWN
   parameters (unmodified) and its own `arguments`. This is the case 0231
   fixed; it must stay fixed. */
function writer(x, y) { x = 'X'; y = 'Y'; return x + y; }
function c49(a, b) { var r = writer.apply(null, arguments); return [r, a, b, arguments[0], arguments[1]]; }
t('writer', c49('a0', 'b0'));

/* Same but the writer reads `arguments.length` after writing. */
function writer2(x, y) { x = 'X'; y = 'Y'; return x + y + arguments.length; }
function c49b(a, b) { var r = writer2.apply(null, arguments); return [r, a, b]; }
t('writer-len', c49b('a0', 'b0'));

/* A writer that writes MORE parameters than the forwarder declares. */
function writer3(x, y, z) { x = '1'; y = '2'; z = '3'; return x + y + z; }
function fwd2b(a, b) { var r = writer3.apply(null, arguments); return [r, a, b]; }
t('writer3', fwd2b('a0', 'b0'));

/* ---------- strict and sloppy forwarders, each writing a parameter ---------- */

/* A sloppy forwarder writes a parameter BEFORE forwarding. The write lands in
   its own arg_buf; the forwarded list must show the written value (mapped
   arguments and the parameter are one storage slot). */
function sloppyWrite(a, b) { a = 'SA'; target.apply(null, arguments); return [a, b, arguments[0], arguments[1]]; }
t('sloppy-write', sloppyWrite('a0', 'b0'));

/* Strict: `arguments` is unmapped, so a parameter write does NOT change it --
   but the write must not corrupt the forwarded list either. */
function strictWrite(a, b) { 'use strict'; a = 'SA'; target.apply(null, arguments); return [a, b, arguments[0], arguments[1]]; }
t('strict-write', strictWrite('a0', 'b0'));

/* Non-simple parameter list (default) -> unmapped even in sloppy mode. */
function defaultWrite(a, b) { a = 'DA'; target.apply(null, arguments); return [a, b, arguments[0], arguments[1]]; }
function defaultWrite2(a, b = 'def') { a = 'DA'; target.apply(null, arguments); return [a, b, arguments[0], arguments[1]]; }
t('default-write-plain', defaultWrite('a0', 'b0'));
t('default-write-param', defaultWrite2('a0', 'b0'));

/* ---------- arguments.length and arguments[i] read by the forwarder ---------- */

function readsArgs(a) {
  var n = arguments.length;
  var first = arguments[0];
  var last = arguments[arguments.length - 1];
  var r = target.apply(null, arguments);
  return [n, first, last, r];
}
t('reads-args', readsArgs('p', 'q', 'r'));

/* Zero-argument call: `arguments[0]` is undefined, length is 0. */
function readsArgs0() {
  var n = arguments.length;
  var first = arguments[0];
  var r = target.apply(null, arguments);
  return [n, first, r];
}
t('reads-args-0', readsArgs0());

/* ---------- three levels deep ---------- */

function level1() { return level2.apply(null, arguments); }
function level2() { return level3.apply(null, arguments); }
function level3() { return target.apply(null, arguments); }
t('deep-0', level1());
t('deep-3', level1('d0', 'd1', 'd2'));

/* A backtrace across the three forwarders: all four frames present and in
   order. Only the booleans are compared -- stack text differs across engines. */
function throwsHere() { throw new Error('boom'); }
function fwdThrow() { return throwsHere.apply(this, arguments); }
function outerThrow() { return fwdThrow.apply(this, arguments); }
function checkStack() {
  try { outerThrow(1, 2); } catch (e) {
    var s = String(e.stack);
    return [s.indexOf('throwsHere') >= 0, s.indexOf('fwdThrow') >= 0,
            s.indexOf('outerThrow') >= 0,
            s.indexOf('throwsHere') < s.indexOf('fwdThrow'),
            s.indexOf('fwdThrow') < s.indexOf('outerThrow')];
  }
}
t('deep-stack', checkStack());

/* ---------- called from C: forEach, call, apply, bound ---------- */

/* forEach passes (element, index, array) -- three arguments to a callback with
   a declared count of one, so the callback's frame has an owned copy. */
var seen = [];
[10, 20, 30].forEach(function (a) {
  seen.push([arguments.length, arguments[0], arguments[1], arguments[2] === undefined ? 'undef' : String(arguments[2]).length, target.apply(null, arguments).split(':')[0]]);
});
t('foreach', seen);

function fwdVar() { return target.apply(null, arguments); }
t('call', fwdVar.call(null, 'c0', 'c1'));
t('call-this', fwdVar.call({ tag: 'T' }, 'c2'));

function fwdThis() { return this && this.tag ? this.tag : 'none'; }
function forwardThis() { return fwdThis.apply(this, arguments); }
t('apply-this', forwardThis.apply({ tag: 'AT' }, ['x']));

var bound = target.bind(null, 'bound0');
function fwdBound() { return bound.apply(null, arguments); }
t('bound', fwdBound('bound1', 'bound2'));

/* A forwarder in a bound function's own arguments. */
function bindFwd() { return target.apply(null, arguments); }
t('bound-forwarder', bindFwd.bind(null, 'bf0')('bf1', 'bf2'));

/* ---------- a forwarder that is a constructor ---------- */

function Init(x, y) { this.x = x; this.y = y; }
function F() { Init.apply(this, arguments); }
var f = new F(3, 4);
t('ctor', [f.x, f.y, f instanceof F, f instanceof Init]);

/* Constructor with a forwarder that reads arguments. */
function Init2(x) { this.x = x; this.n = arguments.length; }
function F2() { this.m = arguments.length; Init2.apply(this, arguments); this.k = arguments.length; }
var f2 = new F2('v', 'extra');
t('ctor-args', [f2.m, f2.x, f2.n, f2.k]);

/* ---------- generator and async forwarders ---------- */

function* genFwd(a, b) {
  var before = arguments.length;
  yield target.apply(null, arguments);
  yield [before, arguments[0], arguments[1]];
}
var g = genFwd('g0', 'g1', 'g2');
t('gen', [g.next().value, g.next().value]);

function* genWrite(a) { a = 'GW'; yield target.apply(null, arguments); yield [a, arguments[0]]; }
var g2 = genWrite('g0');
t('gen-write', [g2.next().value, g2.next().value]);

/* Async: the runner drains the promise job queue before it exits, so the
   `.then` callback prints. The forwarder reads `arguments` after an await,
   which is a resume of a coroutine frame, not a record frame -- the forward
   itself must still be correct. */
async function asyncFwd(a, b) {
  var before = arguments.length;
  await null;
  return before + ':' + target.apply(null, arguments);
}
asyncFwd('a0', 'a1', 'a2').then(function (r) { t('async', r); },
                                 function (e) { t('async', 'ERR:' + e.message); });

/* ---------- apply replaced on the callee ---------- */

var replaced = function () { return 'never'; };
replaced.apply = function (thisArg, args) { return 'REPLACED(' + args.length + ',' + args[0] + ')'; };
function fwdReplaced() { return replaced.apply(null, arguments); }
t('replaced', fwdReplaced('r0', 'r1'));

/* Replace the builtin itself; the guard must miss and the frame's own
   `arguments` must still be handed over intact. */
var realApply = Function.prototype.apply;
Function.prototype.apply = function (thisArg, args) {
  return 'HOOK(' + args.length + ',' + args[0] + ',' + Object.prototype.toString.call(args) + ')';
};
function fwdHook() { return target.apply(null, arguments); }
t('hook-on', fwdHook('h0', 'h1'));
Function.prototype.apply = realApply;
t('hook-off', fwdHook('h0', 'h1'));

/* ---------- identity: one arguments object per frame ---------- */

var seenA = null, seenB = null;
var recorder = function () { return 'rec'; };
recorder.apply = function (thisArg, args) { if (seenA === null) seenA = args; else seenB = args; return 'R'; };
function twoSites() {
  recorder.apply(null, arguments);
  recorder.apply(null, arguments);
  return seenA === seenB;
}
t('identity', twoSites('i0'));

/* ---------- the tail-call forwarder (`return f.apply(...)` is not a tail
   call, but `return f(...)` in the callee's own tail position is) ---------- */

function tailFwd() { return target.apply(null, arguments); }
function tailOuter() { return tailFwd.apply(null, arguments); }
t('tail', tailOuter('t0', 't1', 't2'));

/* Recursive forwarder: deep, and each level owns its copy. */
function recur(n) {
  if (n === 0) return target.apply(null, arguments);
  return recur.apply(null, [n - 1, 'x' + n]);
}
t('recur', recur(4).slice(0, 40));

/* ---------- a forwarder with rest, destructuring and captured parameters ---- */

function restFwd(...rest) { return target.apply(null, rest); }
t('rest', restFwd('s0', 's1', 's2'));

function restAndArguments(a, ...rest) {
  return [rest.length, target.apply(null, arguments)];
}
t('rest-args', restAndArguments('x', 'y', 'z'));

function captured(a) {
  var closure = function () { return a; };
  return [target.apply(null, arguments), closure()];
}
t('captured', captured('cap0', 'cap1'));

/* ---------- `arguments` used in a constructor's own body and forwarded ---- */

function Base(x) { this.v = x; }
function Derived(x, y) { Base.apply(this, arguments); this.w = y; }
var d = new Derived('dv', 'dw');
t('derived', [d.v, d.w, d instanceof Derived, d instanceof Base]);
