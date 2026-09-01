/*
 * `super` and `#private` members read across a generator suspend/resume.
 *
 * WHAT THIS PINS. `OP_special_object` / `HOME_OBJECT` is the only reader of
 * `JS_CallInternal`'s function-scope `JSObject *p` (vendor/quickjs-ng/quickjs.c
 * ~21958). That variable is written in exactly two places, both in the
 * prologue and mutually exclusive:
 *
 *   - `p = JS_VALUE_GET_OBJ(func_obj)` on a normal call, and
 *   - `p = JS_VALUE_GET_OBJ(sf->cur_func)` on the generator/async RESUME entry,
 *     the one that reaches the loop through `goto restart` with an already
 *     allocated stack frame.
 *
 * The second path is the interesting one, because it is the only way the
 * interpreter loop is entered for a frame whose `func_obj` argument is not a
 * function object at all (it is a `JSAsyncFunctionState *`). A resume that
 * failed to rebind `p` -- or any future change that reuses a C frame for a
 * second JS frame -- would make a suspended method read some OTHER function's
 * home object, and `super.m()` would dispatch to the wrong prototype or throw.
 *
 * That failure mode is not hypothetical: the dropped inline-JS-frames arm
 * (docs/js-call-path.md section 6, bug 3) reintroduced exactly it and produced
 * 83 test262 failures while every differential corpus file and all 15 Octane
 * rows passed. This file is the net that would have caught it from the resume
 * side, and it is the net for the next person who touches frame handling. See
 * docs/interpreter-frame-scope-variables.md for the reachability verdict on
 * the shipping configuration.
 *
 * WHY IT IS SHAPED LIKE THIS. Every construct is written INSIDE a function,
 * never at the top level, because a top-level `<eval>` frame is not a callee
 * frame and the top-level spelling of each case passes even on a broken build.
 * The load-bearing section is the INTERLEAVE one: two generators belonging to
 * two different classes, resumed alternately with unrelated calls in between,
 * so a `p` carried over from any other frame gives a visibly wrong answer
 * rather than an accidentally correct one.
 */

function log(name, v) { print(name + ': ' + v); }

/* ---- 1. super.m() before, between and after yields ---------------------- */
function case1() {
  class A { m() { return 'A'; } get g() { return 'gA'; } }
  class B extends A {
    m() { return 'B'; }
    get g() { return 'gB'; }
    *gen() {
      var out = [super.m(), super.g];
      yield 1;
      out.push(super.m(), super.g);
      yield 2;
      out.push(super.m(), super.g);
      return out.join('|');
    }
  }
  var it = new B().gen();
  it.next(); it.next();
  return it.next().value;
}
log('super-across-yields', case1());

/* ---- 2. object-literal method generator with super --------------------- */
function case2() {
  var proto = { m() { return 'proto-m'; } };
  var o = {
    __proto__: proto,
    m() { return 'own-m'; },
    *gen() { yield super.m(); yield super.m(); return super.m(); }
  };
  var it = o.gen();
  return [it.next().value, it.next().value, it.next().value].join(',');
}
log('objlit-super-gen', case2());

/* ---- 3. private fields, methods and brand checks across a yield -------- */
function case3() {
  class C {
    #x = 1;
    static #s = 'stat';
    #priv() { return 'priv' + this.#x; }
    static has(o) { return #x in o; }
    static getS() { return C.#s; }
    *gen() {
      var out = [this.#x, this.#priv(), C.has(this), C.getS()];
      yield 'a';
      this.#x = 2;
      out.push(this.#x, this.#priv(), C.has(this), C.getS());
      yield 'b';
      out.push(this.#priv(), C.has({}), C.getS());
      return out.join('|');
    }
  }
  var it = new C().gen();
  it.next(); it.next();
  return it.next().value;
}
log('private-across-yields', case3());

/*
 * ---- 4. INTERLEAVE: the case that discriminates a stale `p` -------------
 *
 * Two classes with the same method names and different prototypes, resumed
 * alternately, with an unrelated call between every resume. A frame variable
 * carried over from the previous JS frame answers with the other class's home
 * object here and only here.
 */
function case4() {
  class PA { m() { return 'PA'; } }
  class PB { m() { return 'PB'; } }
  class DA extends PA { m() { return 'DA'; } *gen() { yield super.m(); yield super.m(); yield super.m(); } }
  class DB extends PB { m() { return 'DB'; } *gen() { yield super.m(); yield super.m(); yield super.m(); } }
  function noise(n) { var s = 0; for (var i = 0; i < n; i++) s += i; return s; }
  var a = new DA().gen(), b = new DB().gen();
  var out = [];
  for (var i = 0; i < 3; i++) {
    noise(3);
    out.push(a.next().value);
    noise(3);
    out.push(b.next().value);
    noise(3);
    out.push(new DA().m(), new DB().m());
  }
  return out.join(',');
}
log('interleaved-super', case4());

/* ---- 5. a generator resumed from inside another generator's body ------- */
function case5() {
  class P { m() { return 'P'; } }
  class Q { m() { return 'Q'; } }
  class DP extends P { *gen(other) { yield super.m(); yield other.next().value; yield super.m(); } }
  class DQ extends Q { *gen() { yield super.m(); yield super.m(); yield super.m(); } }
  var q = new DQ().gen();
  var it = new DP().gen(q);
  return [it.next().value, it.next().value, it.next().value, q.next().value].join(',');
}
log('nested-resume-super', case5());

/* ---- 6. super in a generator that throws and is resumed through finally - */
function case6() {
  class P { m() { return 'P'; } }
  class D extends P {
    m() { return 'D'; }
    *gen() {
      try {
        yield super.m();
        yield super.m();
      } finally {
        seen.push('finally:' + super.m());
      }
    }
  }
  var seen = [];
  var it = new D().gen();
  seen.push(it.next().value);
  seen.push(it.return('r').value);
  return seen.join(',');
}
log('super-in-finally', case6());

/* ---- 7. super through a generator delegated with yield* ---------------- */
function case7() {
  class P { m() { return 'P'; } }
  class R { m() { return 'R'; } }
  class D extends P {
    *inner() { yield super.m(); yield super.m(); }
    *outer(o) { yield 'start'; yield* this.inner(); yield* o.inner2(); yield super.m(); }
  }
  class E extends R { *inner2() { yield super.m(); } }
  var it = new D().outer(new E());
  var out = [];
  for (var v of it) out.push(v);
  return out.join(',');
}
log('yield-star-super', case7());

/* ---- 8. static blocks and a static generator method with super --------- */
function case8() {
  class P { static m() { return 'sP'; } }
  class D extends P {
    static m() { return 'sD'; }
    static *gen() { yield super.m(); yield super.m(); }
  }
  var it = D.gen();
  return [it.next().value, it.next().value].join(',');
}
log('static-super-gen', case8());

/* ---- 9. home object survives the generator outliving its creator ------- */
function case9() {
  function make() {
    class P { m() { return 'made-P'; } }
    class D extends P { m() { return 'made-D'; } *gen() { yield super.m(); yield super.m(); } }
    return new D().gen();
  }
  var its = [make(), make(), make()];
  var out = [];
  for (var round = 0; round < 2; round++)
    for (var i = 0; i < its.length; i++) out.push(its[i].next().value);
  return out.join(',');
}
log('outliving-generator', case9());

/* ---- 10. same again but the home object's proto is mutated mid-flight -- */
function case10() {
  class P { m() { return 'P1'; } }
  class D extends P { *gen() { yield super.m(); yield super.m(); yield super.m(); } }
  var it = new D().gen();
  var out = [it.next().value];
  Object.setPrototypeOf(D.prototype, { m() { return 'P2'; } });
  out.push(it.next().value);
  Object.setPrototypeOf(D.prototype, { m() { return 'P3'; } });
  out.push(it.next().value);
  return out.join(',');
}
log('proto-mutated-mid-flight', case10());
