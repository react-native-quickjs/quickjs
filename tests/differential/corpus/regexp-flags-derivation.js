// RegExp.prototype.flags: the spec derives it from eight generic Gets on the
// receiver. Patch 0071 answers it from re->bytecode behind a guard, so every
// way an observer can make those eight Gets return something other than the
// compiled flags has to be here.
//
// Run this file against a SABOTAGE_RF_* mutant with QJS_REGEXP_FLAGS_FAST=1 in
// the environment; see tests/differential/mutants/regexp-flags-sabotage.py.

function show(x) {
  if (typeof x === 'string') return JSON.stringify(x);
  return String(x);
}

function attempt(label, fn) {
  var r;
  try {
    r = fn();
  } catch (e) {
    print(label + ' -> throw ' + (e && e.constructor ? e.constructor.name : String(e)));
    return;
  }
  print(label + ' -> ' + show(r));
}

// ---------------------------------------------------------------- 1. every
// legal flag combination, so the mask table and the character order are both
// covered exhaustively rather than by example.
var SINGLES = ['d', 'g', 'i', 'm', 's', 'y'];
var UNI = ['', 'u', 'v'];
(function () {
  var lines = [];
  for (var u = 0; u < UNI.length; u++) {
    for (var bits = 0; bits < 64; bits++) {
      var f = '';
      for (var k = 0; k < 6; k++) if (bits & (1 << k)) f += SINGLES[k];
      // canonical order is dgimsuvy; insert the unicode flag in its place
      var canonical = f.replace(/([^y]*)(y?)$/, function (_, head, tail) {
        return head + UNI[u] + tail;
      });
      var re = new RegExp('a', canonical);
      lines.push(canonical + '|' + re.flags);
    }
  }
  print('combos ' + lines.length);
  print(lines.join(' '));
})();

// flags given out of order still come back canonical
attempt('unsorted ysimgd', function () { return new RegExp('a', 'ysimgd').flags; });
attempt('unsorted vy', function () { return new RegExp('a', 'vy').flags; });
attempt('u and v together', function () { return new RegExp('a', 'uv').flags; });
attempt('unknown flag', function () { return new RegExp('a', 'q').flags; });
attempt('duplicate flag', function () { return new RegExp('a', 'gg').flags; });

// ------------------------------------------------------ 2. odd receivers
attempt('RegExp.prototype.flags', function () { return RegExp.prototype.flags; });
attempt('Object.create(RegExp.prototype)', function () {
  return Object.create(RegExp.prototype).flags;
});
attempt('plain object via Reflect.get', function () {
  return Reflect.get(RegExp.prototype, 'flags', {});
});
// A non-RegExp object whose [[Prototype]] IS RegExp.prototype and whose second
// internal-union word is a live pointer. JSObject.u.array.u.values overlays
// JSObject.u.regexp.bytecode, so a fast array is the receiver that tells the
// class check apart from the has-bytecode check; without it the two guards mask
// each other and both mutants survive.
attempt('array reparented onto RegExp.prototype', function () {
  var a = [1, 2, 3];
  Object.setPrototypeOf(a, RegExp.prototype);
  return a.flags;
});
attempt('typed array reparented onto RegExp.prototype', function () {
  var t = new Uint8Array(64);
  Object.setPrototypeOf(t, RegExp.prototype);
  return t.flags;
});
attempt('function reparented onto RegExp.prototype', function () {
  var f = function () {};
  Object.setPrototypeOf(f, RegExp.prototype);
  return f.flags;
});
attempt('Map reparented onto RegExp.prototype', function () {
  var m = new Map([[1, 2]]);
  Object.setPrototypeOf(m, RegExp.prototype);
  return m.flags;
});
attempt('regexp receiver via Reflect.get', function () {
  return Reflect.get(RegExp.prototype, 'flags', /a/gimy);
});
attempt('number receiver via call', function () {
  return Object.getOwnPropertyDescriptor(RegExp.prototype, 'flags').get.call(7);
});
attempt('null receiver via call', function () {
  return Object.getOwnPropertyDescriptor(RegExp.prototype, 'flags').get.call(null);
});
attempt('proxy of a regexp', function () {
  return Object.getOwnPropertyDescriptor(RegExp.prototype, 'flags')
    .get.call(new Proxy(/a/gi, {}));
});
attempt('proxy that lies about global', function () {
  var target = /a/i;
  var pr = new Proxy(target, {
    get: function (t, k, r) { return k === 'global' ? true : Reflect.get(t, k, t); }
  });
  return Object.getOwnPropertyDescriptor(RegExp.prototype, 'flags').get.call(pr);
});

// ------------------------------------------- 3. own properties on the instance
attempt('own hasIndices data property', function () {
  var r = /a/g;
  Object.defineProperty(r, 'hasIndices', { value: true, configurable: true });
  return r.flags;
});
attempt('own global = false', function () {
  var r = /a/g;
  Object.defineProperty(r, 'global', { value: false, configurable: true });
  return r.flags;
});
attempt('own sticky accessor', function () {
  var r = /a/i;
  Object.defineProperty(r, 'sticky', { get: function () { return true; }, configurable: true });
  return r.flags;
});
attempt('own unrelated property', function () {
  var r = /a/gi;
  r.tag = 1;
  return r.flags;
});
attempt('lastIndex deleted', function () {
  var r = /a/gm;
  delete r.lastIndex;
  return r.flags;
});
attempt('lastIndex deleted then global shadowed', function () {
  var r = /a/gm;
  delete r.lastIndex;
  Object.defineProperty(r, 'global', { value: false, configurable: true });
  return r.flags;
});
attempt('own dotAll on a frozen instance', function () {
  var r = /a/s;
  Object.defineProperty(r, 'dotAll', { value: false });
  Object.freeze(r);
  return r.flags;
});

// ------------------------------------------------------------- 4. subclassing
attempt('class extends RegExp', function () {
  var src = 'class RFSub extends RegExp {}; new RFSub("a", "gi").flags';
  return eval(src);
});
attempt('subclass overriding global', function () {
  var src = 'class RFSub2 extends RegExp { get global() { return true; } };' +
            ' new RFSub2("a", "i").flags';
  return eval(src);
});
attempt('setPrototypeOf to a plain object', function () {
  var r = /a/gi;
  var alt = Object.create(RegExp.prototype);
  Object.defineProperty(alt, 'multiline', { get: function () { return true; } });
  Object.setPrototypeOf(r, alt);
  return r.flags;
});
// The receiver's prototype is NOT RegExp.prototype but carries the eight
// intrinsic accessors verbatim. Both legs must agree: the generic derivation
// calls those very getters with this receiver, so it reads the same bytecode
// the fast path reads. This is why SABOTAGE_RF_PROTO is an equivalent mutant
// rather than an untested guard.
var FLAG_NAMES = ['hasIndices', 'global', 'ignoreCase', 'multiline',
                  'dotAll', 'unicode', 'unicodeSets', 'sticky'];
attempt('proto carrying the intrinsic accessors', function () {
  var alt = {};
  for (var i = 0; i < FLAG_NAMES.length; i++)
    Object.defineProperty(alt, FLAG_NAMES[i],
      Object.getOwnPropertyDescriptor(RegExp.prototype, FLAG_NAMES[i]));
  var r = /a/gim;
  Object.setPrototypeOf(r, alt);
  return Object.getOwnPropertyDescriptor(RegExp.prototype, 'flags').get.call(r);
});
attempt('proto carrying seven intrinsic accessors and one fake', function () {
  var alt = {};
  for (var i = 0; i < FLAG_NAMES.length; i++)
    Object.defineProperty(alt, FLAG_NAMES[i],
      Object.getOwnPropertyDescriptor(RegExp.prototype, FLAG_NAMES[i]));
  Object.defineProperty(alt, 'sticky', { get: function () { return true; } });
  var r = /a/gim;
  Object.setPrototypeOf(r, alt);
  return Object.getOwnPropertyDescriptor(RegExp.prototype, 'flags').get.call(r);
});
attempt('setPrototypeOf to null', function () {
  var r = /a/gi;
  Object.setPrototypeOf(r, null);
  return Object.getOwnPropertyDescriptor(RegExp.prototype, 'flags').get.call(r);
});

// ------------------------------------------- 5. mutating RegExp.prototype
function withProtoPatch(label, name, desc, fn) {
  var saved = Object.getOwnPropertyDescriptor(RegExp.prototype, name);
  if (desc === null) delete RegExp.prototype[name];
  else Object.defineProperty(RegExp.prototype, name, desc);
  attempt(label, fn);
  if (saved) Object.defineProperty(RegExp.prototype, name, saved);
  print('  restored ' + name + ' -> ' + show(/a/gi.flags));
}

withProtoPatch('proto global always true', 'global',
  { get: function () { return true; }, configurable: true },
  function () { return /a/i.flags; });

withProtoPatch('proto sticky is a data property', 'sticky',
  { value: true, configurable: true, writable: true },
  function () { return /a/i.flags; });

withProtoPatch('proto dotAll deleted', 'dotAll', null,
  function () { return /a/s.flags; });

withProtoPatch('proto hasIndices always true', 'hasIndices',
  { get: function () { return true; }, configurable: true },
  function () { return /a/.flags; });

withProtoPatch('proto ignoreCase always false', 'ignoreCase',
  { get: function () { return false; }, configurable: true },
  function () { return /a/gi.flags; });

withProtoPatch('proto multiline throws', 'multiline',
  { get: function () { throw new RangeError('nope'); }, configurable: true },
  function () { return /a/g.flags; });

withProtoPatch('proto unicode always true', 'unicode',
  { get: function () { return true; }, configurable: true },
  function () { return /a/g.flags; });

withProtoPatch('proto unicodeSets always true', 'unicodeSets',
  { get: function () { return true; }, configurable: true },
  function () { return /a/g.flags; });

withProtoPatch('proto global returns a truthy string', 'global',
  { get: function () { return 'no'; }, configurable: true },
  function () { return /a/.flags; });

withProtoPatch('proto global returns 0', 'global',
  { get: function () { return 0; }, configurable: true },
  function () { return /a/g.flags; });

// an accessor swapped for the *same* function object is still pristine
(function () {
  var d = Object.getOwnPropertyDescriptor(RegExp.prototype, 'global');
  Object.defineProperty(RegExp.prototype, 'global', d);
  attempt('proto global redefined to itself', function () { return /a/gi.flags; });
})();

// a getter reinstated after being clobbered must go back to being fast
(function () {
  var d = Object.getOwnPropertyDescriptor(RegExp.prototype, 'multiline');
  Object.defineProperty(RegExp.prototype, 'multiline',
    { get: function () { return true; }, configurable: true });
  attempt('multiline clobbered', function () { return /a/g.flags; });
  Object.defineProperty(RegExp.prototype, 'multiline', d);
  attempt('multiline restored', function () { return /a/gm.flags; });
})();

// ------------------------------------------------- 6. the internal consumers
// String.prototype.replace/match/split/matchAll read .flags themselves.
attempt('replace with /g', function () { return 'a-a-a'.replace(/a/g, 'b'); });
attempt('replace without /g', function () { return 'a-a-a'.replace(/a/, 'b'); });
attempt('replace with /y', function () { return 'aaa'.replace(/a/y, 'b'); });
attempt('match /g', function () { return String('abcabc'.match(/b/g)); });
attempt('matchAll /g', function () {
  var out = [];
  var it = 'abcabc'.matchAll(/b/g);
  var n = it.next();
  while (!n.done) { out.push(n.value[0] + '@' + n.value.index); n = it.next(); }
  return out.join(',');
});
attempt('split', function () { return String('a1b2c'.split(/\d/)); });
attempt('split with /u', function () { return String('a1b2c'.split(/\d/u)); });
attempt('toString', function () { return String(/a\/b/gim); });
// NOT TESTED HERE, deliberately: `'a-a-a'.replace(/a/, 'b')` with
// RegExp.prototype.global monkey-patched to true HANGS this engine, on the
// UNPATCHED baseline as well (verified 2026-08-01). js_regexp_Symbol_replace
// takes the generic `global` bit from .flags but js_regexp_exec advances
// lastIndex from its own bytecode flags, so the driver loop never moves. That
// is a pre-existing spec deviation in the exec path, unrelated to how .flags is
// derived; see docs/regexp-flags-derivation.md. A corpus case for it would
// wedge the runner rather than diff.
attempt('match sees a patched proto', function () {
  var saved = Object.getOwnPropertyDescriptor(RegExp.prototype, 'global');
  Object.defineProperty(RegExp.prototype, 'global',
    { get: function () { return false; }, configurable: true });
  var out = String('abcabc'.match(/b/g));
  Object.defineProperty(RegExp.prototype, 'global', saved);
  return out;
});

// ------------------------------------------------------ 7. repeated reads
// the fast path is a per-call guard, so the same object read many times, with a
// mutation in the middle, must not go stale.
(function () {
  var r = /a/gi;
  var acc = [];
  for (var i = 0; i < 5; i++) acc.push(r.flags);
  var saved = Object.getOwnPropertyDescriptor(RegExp.prototype, 'ignoreCase');
  Object.defineProperty(RegExp.prototype, 'ignoreCase',
    { get: function () { return false; }, configurable: true });
  for (i = 0; i < 5; i++) acc.push(r.flags);
  Object.defineProperty(RegExp.prototype, 'ignoreCase', saved);
  for (i = 0; i < 5; i++) acc.push(r.flags);
  print('repeated -> ' + acc.join(','));
})();

// and the same for an own property appearing between reads
(function () {
  var r = /a/m;
  var acc = [r.flags];
  Object.defineProperty(r, 'multiline', { value: false, configurable: true });
  acc.push(r.flags);
  delete r.multiline;
  acc.push(r.flags);
  print('own-prop churn -> ' + acc.join(','));
})();

// compile() rewrites the bytecode under an existing object
attempt('compile changes the flags', function () {
  var r = /a/g;
  var before = r.flags;
  r.compile('b', 'im');
  return before + '/' + r.flags;
});

print('done');
