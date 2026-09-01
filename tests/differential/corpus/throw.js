// Differential corpus for the throw path (patch 0015, throw-backtrace-opt-in).
// Runs on node and on qjs; output must be byte-identical.
//
// Stack *text* is deliberately not compared here: its format legitimately
// differs between engines (node prefixes the message line, quickjs does not),
// so a byte comparison of it would fail for reasons that are not bugs. What is
// compared is everything the patch could plausibly break and that the spec
// pins down: which values arrive at the catch, whether a stack exists at all
// and of what type, the number of frames, and unwind ordering. The before/after
// byte comparison of the stack text itself is done separately, against the
// unpatched engine rather than against node -- see docs/throw-cost.md.

var out = [];
function log(name, v) { out.push(name + ': ' + v); }
function T(name, fn) {
  var r;
  try { r = fn(); } catch (e) { r = 'UNCAUGHT ' + (e && e.name); }
  log(name, r);
}

// ---------- 1. what arrives at the catch, for every throwable shape --------
function caught(fn) {
  try {
    fn();
    return 'NO THROW';
  } catch (e) {
    var t = typeof e;
    var hasStack;
    try {
      hasStack = e !== null && e !== undefined && e.stack !== undefined
        ? 'stack:' + typeof e.stack : 'nostack';
    } catch (x) {
      hasStack = 'stack-threw';
    }
    return t + ' ' + String(e) + ' ' + hasStack;
  }
}

function MyErr(m) { Error.call(this, m); this.message = m; this.name = 'MyErr'; }
MyErr.prototype = Object.create(Error.prototype);
MyErr.prototype.constructor = MyErr;

var thenable = { then: function () {} };

T('int', function () { return caught(function () { throw 1; }); });
T('zero', function () { return caught(function () { throw 0; }); });
T('str', function () { return caught(function () { throw 'oops'; }); });
T('null', function () { return caught(function () { throw null; }); });
T('undefined', function () { return caught(function () { throw undefined; }); });
T('bool', function () { return caught(function () { throw true; }); });
T('bigint', function () { return caught(function () { throw BigInt(10); }); });
T('object', function () { return caught(function () { throw { a: 1 }; }); });
T('thenable', function () { return caught(function () { throw thenable; }); });
T('array', function () { return caught(function () { throw [1, 2]; }); });
T('function', function () { return caught(function () { throw caught; }); });
T('error', function () { return caught(function () { throw new Error('E'); }); });
T('typeerror', function () { return caught(function () { throw new TypeError('T'); }); });
T('subclass', function () { return caught(function () { throw new MyErr('M'); }); });
T('proto-only', function () {
  return caught(function () { throw Object.create(Error.prototype); });
});
/* Engine-raised errors: the *message* text is engine-specific and legitimately
   differs, so only the class and the presence of a stack are compared. */
function caughtName(fn) {
  try {
    fn();
    return 'NO THROW';
  } catch (e) {
    return e.name + ' stack:' + typeof e.stack + ' msg:' + (e.message.length > 0);
  }
}
T('engine-type', function () { return caughtName(function () { return null.x; }); });
T('engine-ref', function () {
  return caughtName(function () { return nosuchglobalanywhere; });
});
T('engine-syntax', function () { return caughtName(function () { return JSON.parse('{'); }); });
T('engine-range', function () { return caughtName(function () { return new Array(-1); }); });

// ---------- 2. a stack still exists, and still has frames ------------------
// Frame *text* differs between engines; the frame count does not, as long as
// stackTraceLimit bounds it below the real depth.
function deep3() { throw new Error('deep'); }
function deep2() { return deep3(); }
function deep1() { return deep2(); }

function frameCount(limit) {
  var saved = Error.stackTraceLimit;
  Error.stackTraceLimit = limit;
  var n;
  try {
    deep1();
    n = 'NO THROW';
  } catch (e) {
    var lines = String(e.stack).split('\n');
    n = 0;
    for (var i = 0; i < lines.length; i++) {
      if (/^\s+at /.test(lines[i])) n++;
    }
  }
  Error.stackTraceLimit = saved;
  return n;
}
T('frames-limit-1', function () { return frameCount(1); });
T('frames-limit-2', function () { return frameCount(2); });
T('frames-limit-3', function () { return frameCount(3); });
T('frames-limit-0', function () { return frameCount(0); });

// after a run of caught non-Error throws, a real Error still gets a stack
T('stack-after-throw-loop', function () {
  for (var i = 0; i < 200; i++) {
    try { throw i; } catch (e) { /* discard */ }
  }
  return frameCount(3);
});

// a non-Error never acquires a stack, however deep it was thrown from
function plain3() { throw { tag: 'plain' }; }
function plain2() { return plain3(); }
function plain1() { return plain2(); }
T('plain-has-no-stack', function () {
  try { plain1(); return 'NO THROW'; } catch (e) { return String(e.stack); }
});

// ---------- 3. captureStackTrace ------------------------------------------
T('capture-frames', function () {
  if (typeof Error.captureStackTrace !== 'function') return 'unsupported';
  var target = {};
  Error.captureStackTrace(target);
  var lines = String(target.stack).split('\n');
  var n = 0;
  for (var i = 0; i < lines.length; i++) if (/^\s+at /.test(lines[i])) n++;
  return n > 0 ? 'frames>0 ' + typeof target.stack : 'EMPTY';
});
T('capture-own-property', function () {
  if (typeof Error.captureStackTrace !== 'function') return 'unsupported';
  var target = {};
  Error.captureStackTrace(target);
  return Object.prototype.hasOwnProperty.call(target, 'stack');
});

// ---------- 4. prepareStackTrace ------------------------------------------
T('prepare-called', function () {
  var saved = Error.prepareStackTrace;
  var seen = 0;
  Error.prepareStackTrace = function (err, frames) {
    seen = frames.length > 0 ? 1 : 0;
    return 'PREPARED:' + (typeof frames[0].getFunctionName());
  };
  var r;
  try { deep1(); r = 'NO THROW'; } catch (e) { r = e.stack + ' seen=' + seen; }
  Error.prepareStackTrace = saved;
  return r;
});

// ---------- 5. unwind ordering and control flow ---------------------------
T('finally-order', function () {
  var log2 = [];
  try {
    try { throw 1; } finally { log2.push('f1'); }
  } catch (e) { log2.push('c:' + e); }
  return log2.join(',');
});

T('nested-finally', function () {
  var log2 = [];
  function inner() {
    try { throw 'x'; } finally { log2.push('inner-finally'); }
  }
  try {
    try { inner(); } finally { log2.push('outer-finally'); }
  } catch (e) { log2.push('caught:' + e); }
  return log2.join(',');
});

T('iterator-close-on-throw', function () {
  var log2 = [];
  var iterable = {};
  iterable[Symbol.iterator] = function () {
    var i = 0;
    return {
      next: function () { return { value: i++, done: i > 5 }; },
      'return': function () { log2.push('closed'); return { done: true }; },
    };
  };
  try {
    for (var v of iterable) { throw 'break-out:' + v; }
  } catch (e) { log2.push('caught:' + e); }
  return log2.join(',');
});

T('rethrow-chain', function () {
  function r3() { throw 'orig'; }
  function r2() { try { r3(); } catch (e) { throw e + '/2'; } }
  function r1() { try { r2(); } catch (e) { throw e + '/1'; } }
  try { r1(); return 'NO THROW'; } catch (e) { return e; }
});

T('rethrow-same-error-keeps-stack', function () {
  try {
    try { throw new Error('inner'); } catch (e) { throw e; }
  } catch (e2) {
    return e2.message + ' ' + typeof e2.stack;
  }
});

T('throw-in-catch', function () {
  var log2 = [];
  try {
    try { throw 1; } catch (e) { log2.push('c1:' + e); throw 2; }
  } catch (e) { log2.push('c2:' + e); }
  return log2.join(',');
});

T('throw-in-finally-replaces', function () {
  try {
    try { throw 'first'; } finally { throw 'second'; }
  } catch (e) { return e; }
});

T('generator-return-on-throw', function () {
  var log2 = [];
  function* g() { try { yield 1; yield 2; } finally { log2.push('gen-finally'); } }
  try {
    for (var v of g()) { throw 'stop'; }
  } catch (e) { log2.push('caught:' + e); }
  return log2.join(',');
});

// ---------- 6. deep throws keep working after a stack overflow ------------
T('after-overflow', function () {
  function so() { return so(); }
  var first;
  try { so(); first = 'NO THROW'; } catch (e) { first = e.name; }
  return first + ' then frames=' + (frameCount(3) > 0);
});

print(out.join('\n'));
