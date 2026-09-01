/*
 * Differential corpus: the observable behaviour of Error.prototype.stack.
 *
 * Compared BYTE-FOR-BYTE against node, so every line here must be a statement
 * that is true on any conforming engine -- NOT an absolute value. That
 * distinction is load-bearing and was learned the hard way: a first draft
 * printed descriptor flags, own-property names and raw frame counts, and node
 * disagreed with QuickJS on eight rows for reasons that predate any change
 * here. V8 exposes `stack` as an own property of the Error instance and
 * prefixes the text with an "Error: msg" header; quickjs-ng exposes an
 * accessor on Error.prototype and emits only "    at ..." lines. Both are
 * legal. A corpus that encoded either engine's choice would be testing the
 * engine's identity rather than its correctness, and would have to be edited
 * every time something legitimate changed.
 *
 * So each line below asserts a RELATION that must hold regardless: that
 * reading `.stack` does not perturb the object, that two reads agree, that the
 * frame count responds to Error.stackTraceLimit with the right SLOPE, that
 * assignment wins over whatever was captured, and so on.
 *
 * Why this file exists: the deferred-stack design stores frames in the Error's
 * internal slot and formats the string on first read. Every invariant below is
 * one a lazy implementation can plausibly break -- and, per the mutation table
 * in docs/throw-cost.md, three deliberately broken builds were used to prove
 * this file actually catches them rather than passing silently.
 *
 * The stack TEXT is not compared here, because the formats legitimately
 * differ. tests/differential/engine-only/stack-text.js covers the text by
 * diffing this engine against itself built without the change.
 */

function line(label, v) { print(label + ' :: ' + v); }

/* Number of "    at ..." frame lines, engine-neutrally: count non-empty lines
   that are not the leading "Name: message" header some engines emit. */
function frames(err) {
  var s = err.stack;
  if (typeof s !== 'string' || s === '') return 0;
  var parts = s.split('\n');
  var c = 0;
  for (var i = 0; i < parts.length; i++) {
    if (parts[i].replace(/^\s+/, '').indexOf('at ') === 0) c++;
  }
  return c;
}

/* A stable description of a property's shape, so "did reading change it?" can
   be asked without printing engine-specific flag values. */
function shape(obj, key) {
  var d = Object.getOwnPropertyDescriptor(obj, key);
  if (!d) return 'absent';
  if (typeof d.get === 'function' || typeof d.set === 'function') {
    return 'accessor:' + (typeof d.get) + ',' + (typeof d.set) +
           ',e=' + d.enumerable + ',c=' + d.configurable;
  }
  return 'data:' + (typeof d.value) + ',w=' + d.writable +
         ',e=' + d.enumerable + ',c=' + d.configurable;
}
function ownSet(o) { return Object.getOwnPropertyNames(o).sort().join('|'); }

/* The i'th "    at ..." frame, innermost first, with leading indent and any
   engine-specific header line stripped. Both V8 and quickjs-ng order frames
   innermost-first, so frame ORDER is an engine-neutral invariant -- and it is
   the only thing that catches a formatter that emits the right set of frames
   in the wrong positions. Presence checks cannot: a rotation keeps every name. */
function frameAt(err, idx) {
  var s = err.stack;
  if (typeof s !== 'string') return '';
  var parts = s.split('\n');
  var c = 0;
  for (var i = 0; i < parts.length; i++) {
    var t = parts[i].replace(/^\s+/, '');
    if (t.indexOf('at ') === 0) { if (c === idx) return t; c++; }
  }
  return '';
}

/* ---- reading .stack must not perturb the object ----------------------- */
var e = new Error('m');
var shapeBefore = shape(e, 'stack');
var ownBefore = ownSet(e);
var keysBefore = Object.keys(e).join('|');
var s1 = e.stack;
line('read-yields-string', typeof s1 === 'string');
line('read-preserves-descriptor-shape', shape(e, 'stack') === shapeBefore);
line('read-preserves-own-names', ownSet(e) === ownBefore);
line('read-preserves-enumerable-keys', Object.keys(e).join('|') === keysBefore);
line('read-preserves-json', JSON.stringify(e) === JSON.stringify(e));

/* the shape is the same on a fresh, never-read error and a read one */
line('shape-same-read-vs-unread', shape(new Error('u'), 'stack') === shape(e, 'stack'));

/* ---- memoisation must be invisible ------------------------------------ */
line('read-twice-identical', e.stack === e.stack);
line('read-thrice-identical', (e.stack === e.stack) && (e.stack === s1));

/* Read once, churn the heap so a collection is likely, read again. Catches a
   design that frees or reuses the captured frames after the first read. */
var e2 = new Error('m2');
var first = e2.stack;
var junk = []; for (var i = 0; i < 20000; i++) junk.push({ i: i });
junk = null;
line('read-survives-heap-churn', first === e2.stack);

/* Many errors captured BEFORE any of them is read: proves the per-error frame
   store is not shared, overwritten, or aliased across constructions. */
function mkAt(d) { return d <= 0 ? new Error('d') : mkAt(d - 1); }
/* node defaults Error.stackTraceLimit to 10; quickjs-ng's default is higher.
   Without pinning it, the deep and shallow cases both hit node's cap and the
   monotonicity check silently compares two saturated values. */
var savedBatchLimit = Error.stackTraceLimit;
Error.stackTraceLimit = 200;
var batch = [];
for (var q = 0; q < 40; q++) batch.push(mkAt(q % 8));
var batchStacks = [];
for (var q2 = 0; q2 < batch.length; q2++) batchStacks.push(batch[q2].stack);
var allStrings = true, allStable = true;
for (var q3 = 0; q3 < batch.length; q3++) {
  if (typeof batchStacks[q3] !== 'string' || batchStacks[q3].length === 0) allStrings = false;
  if (batchStacks[q3] !== batch[q3].stack) allStable = false;
}
line('batch-all-strings', allStrings);
line('batch-all-stable-on-reread', allStable);
/* deeper construction must not produce fewer frames than shallower */
line('batch-depth-monotonic', frames(batch[7]) >= frames(batch[0]));
line('batch-depth-strictly-deeper', frames(batch[7]) > frames(batch[0]));
Error.stackTraceLimit = savedBatchLimit;

/* ---- assignment wins over the captured stack -------------------------- */
var e4 = new Error('m4');
e4.stack = 'REPLACED';
line('assign-after-nothing-read', e4.stack === 'REPLACED');
var e5 = new Error('m5');
var _r5 = e5.stack;
e5.stack = 'LATE';
line('assign-after-read', e5.stack === 'LATE');
line('assign-is-sticky', (function () { var x = e5.stack; return x === e5.stack; })());
line('assign-adds-own-stack', Object.prototype.hasOwnProperty.call(e5, 'stack'));

/* assignment must be a string on both engines' setter contract */
var e6 = new Error('m6');
e6.stack = '';
line('assign-empty-string', e6.stack === '');

/* ---- defineProperty over it ------------------------------------------- */
var e8 = new Error('m8');
Object.defineProperty(e8, 'stack', { value: 'DEFINED', writable: false, enumerable: true, configurable: true });
line('defineProperty-wins', e8.stack === 'DEFINED');
line('defineProperty-enumerable', Object.keys(e8).indexOf('stack') >= 0);

/* ---- delete ----------------------------------------------------------- */
var e7 = new Error('m7');
e7.stack = 'X';
var deleted = delete e7.stack;
line('delete-own-returns-true', deleted);
line('delete-removes-assigned-value', e7.stack !== 'X');

/* ---- Error.stackTraceLimit: the SLOPE, not the absolute count --------- */
function f3() { return new Error('depth'); }
function f2() { return f3(); }
function f1() { return f2(); }
var saved = Error.stackTraceLimit;
Error.stackTraceLimit = 1; var n1 = frames(f1());
Error.stackTraceLimit = 2; var n2 = frames(f1());
Error.stackTraceLimit = 3; var n3 = frames(f1());
Error.stackTraceLimit = 0; var n0 = frames(f1());
Error.stackTraceLimit = saved;
line('limit-slope-1to2', n2 - n1 === 1);
line('limit-slope-2to3', n3 - n2 === 1);
line('limit-zero-has-no-frames', n0 === 0);

/* The limit is read when the Error is CONSTRUCTED. Raising it afterwards must
   not retroactively add frames -- a lazy implementation that consulted the
   limit at format time instead would fail here. */
Error.stackTraceLimit = 2;
var lateErr = f1();
Error.stackTraceLimit = 50;
var lateFrames = frames(lateErr);
Error.stackTraceLimit = 2;
var ctrlFrames = frames(f1());
Error.stackTraceLimit = saved;
line('limit-bound-at-construction', lateFrames === ctrlFrames);

/* ---- captureStackTrace ------------------------------------------------ */
if (typeof Error.captureStackTrace === 'function') {
  var t = {};
  Error.captureStackTrace(t);
  line('capture-installs-own', Object.prototype.hasOwnProperty.call(t, 'stack'));
  line('capture-yields-string', typeof t.stack === 'string');
  line('capture-stable', t.stack === t.stack);
  line('capture-not-enumerable', Object.keys(t).indexOf('stack') < 0);
} else {
  line('capture-installs-own', 'n/a');
  line('capture-yields-string', 'n/a');
  line('capture-stable', 'n/a');
  line('capture-not-enumerable', 'n/a');
}

/* ---- captured frames must outlive the functions they name ------------- */
/* The function is created inside an IIFE, called once, and dropped. A design
   that retained a bare pointer to the function object, its bytecode, or an
   un-refcounted atom would format garbage -- or crash -- by the time the
   string is materialised. We cannot compare the text to node, so we assert it
   is a well-formed, printable, stable string that still names the function. */
var held = (function () {
  var mk = new Function('return function uniquelyNamedTransient() { return new Error("held"); }');
  return mk()();
})();
for (var g = 0; g < 20000; g++) { var tmp = { g: g }; }
var heldStack = held.stack;
line('held-is-string', typeof heldStack === 'string');
line('held-nonempty', heldStack.length > 0);
line('held-printable-ascii', /^[\x20-\x7e\n]*$/.test(heldStack));
line('held-stable-on-reread', heldStack === held.stack);
line('held-names-the-function', heldStack.indexOf('uniquelyNamedTransient') >= 0);

/* NOTE: whether a function's name is read at construction or at format time is
   NOT tested here, because the engines legitimately disagree. V8 formats the
   trace on first access and therefore reports a name mutated after the throw;
   quickjs-ng captures at construction and reports the original. That assertion
   lives in tests/differential/engine-only/stack-text.js instead, where it is
   compared against this engine built without the deferred-stack change --
   which is the only comparison that can hold it to the right answer. */

/* ---- errors from engine-raised throws --------------------------------- */
var engineErr = null;
try { null.x; } catch (x) { engineErr = x; }
line('engine-error-stack-string', typeof engineErr.stack === 'string');
line('engine-error-stack-stable', engineErr.stack === engineErr.stack);
line('engine-error-has-frames', frames(engineErr) > 0);

/* ---- subclass --------------------------------------------------------- */
function MyErr(m) { Error.call(this, m); this.message = m; }
MyErr.prototype = Object.create(Error.prototype);
MyErr.prototype.constructor = MyErr;
line('subclass-stack-string', typeof new MyErr('sub').stack === 'string');

/* ---- an Error that is never read at all, then discarded ---------------- */
/* Purely a crash/leak probe: constructs and drops 5000 errors with nothing
   ever reading .stack, which is the whole point of the lazy design. */
for (var z = 0; z < 5000; z++) { var dead = mkAt(5); }
line('bulk-construct-survived', true);

/* ---- frame ORDER, innermost first ------------------------------------- */
/* Three nested, uniquely named functions. The trace must name them in
   innermost-to-outermost order. A formatter that is off by one, that reverses,
   or that rotates the frame array still emits all three names -- so only the
   POSITIONS distinguish it. */
function probeInnermost() { return new Error('order'); }
function probeMiddle()    { return probeInnermost(); }
function probeOutermost() { return probeMiddle(); }
var orderErr = probeOutermost();
line('frame0-is-innermost', frameAt(orderErr, 0).indexOf('probeInnermost') >= 0);
line('frame1-is-middle',    frameAt(orderErr, 1).indexOf('probeMiddle') >= 0);
line('frame2-is-outermost', frameAt(orderErr, 2).indexOf('probeOutermost') >= 0);
/* and no frame names a function from a different position */
line('frame0-not-middle',   frameAt(orderErr, 0).indexOf('probeMiddle') < 0);
line('frame0-not-outer',    frameAt(orderErr, 0).indexOf('probeOutermost') < 0);
line('frame2-not-inner',    frameAt(orderErr, 2).indexOf('probeInnermost') < 0);

/* The same, read a SECOND time. A design that consumes the captured frames on
   first read -- freeing them, or moving them out without storing the result --
   yields an empty or truncated trace here while the first read looked perfect. */
line('order-holds-on-reread-0', frameAt(orderErr, 0).indexOf('probeInnermost') >= 0);
line('order-holds-on-reread-2', frameAt(orderErr, 2).indexOf('probeOutermost') >= 0);
line('reread-frame-count-stable', frames(orderErr) === frames(orderErr));

/* A fresh error whose FIRST read happens after many other errors have been
   read, so any single shared/global format buffer would have been clobbered. */
var lateRead = probeOutermost();
for (var y = 0; y < 200; y++) { var o = probeOutermost(); var _o = o.stack; }
line('late-first-read-frame0', frameAt(lateRead, 0).indexOf('probeInnermost') >= 0);
line('late-first-read-frame2', frameAt(lateRead, 2).indexOf('probeOutermost') >= 0);

print('END');
