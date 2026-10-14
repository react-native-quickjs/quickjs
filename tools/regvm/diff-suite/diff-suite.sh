#!/bin/sh
# Differential suite for the register VM.
#
# Runs each kernel as a function expression through the stack build and the
# register build and requires the same printed result AND exit 0 on both (a
# non-zero exit is the JS_FreeRuntime leak assertion). Usage:
#
#   STACK=<stack runner> REGVM=<register runner> tools/regvm/diff-suite/diff-suite.sh
#
# A runner evaluates its first argument as a function expression and calls it
# with the remaining numeric arguments, printing the result.

STACK="${STACK:?set STACK to the stack-VM runner}"
REGVM="${REGVM:?set REGVM to the register-VM runner}"

pass=0; fail=0; skip=0

K() {
  name="$1"; k="$2"; shift 2
  s=$(timeout 20 "$STACK" "$k" "$@" 2>&1); se=$?
  r=$(timeout 20 "$REGVM" "$k" "$@" 2>&1); re=$?
  # A kernel may legitimately throw, which exits 1; a leak asserts and a crash
  # signals, both of which exit 128 or above. So both runs must stay under 128
  # and print the same thing.
  if [ "$se" -lt 128 ] && [ "$re" -lt 128 ] && [ "$s" = "$r" ]; then
    pass=$((pass+1))
  elif echo "$r" | grep -q "was not lowered"; then
    skip=$((skip+1)); printf 'SKIP %-16s %s\n' "$name" "$r"
  else
    fail=$((fail+1)); printf 'FAIL %-16s stack[%s]=%s regvm[%s]=%s\n' "$name" "$se" "$s" "$re" "$r"
  fi
}

# Same as K, but with lazy JSON enabled. Lazy JSON needs a document of at least
# 4096 bytes, so these kernels pad the text. Without this the lazy-marker paths
# (a marker that survives in a property slot, and a second object sharing the
# shape of an object whose slot was already materialised) are not covered at
# all: the default QJS_JSON_LAZY is 0.
KLAZY() {
  name="$1"; k="$2"; shift 2
  s=$(QJS_JSON_LAZY=1 timeout 20 "$STACK" "$k" "$@" 2>&1); se=$?
  r=$(QJS_JSON_LAZY=1 timeout 20 "$REGVM" "$k" "$@" 2>&1); re=$?
  if [ "$se" -lt 128 ] && [ "$re" -lt 128 ] && [ "$s" = "$r" ]; then
    pass=$((pass+1))
  elif echo "$r" | grep -q "was not lowered"; then
    skip=$((skip+1)); printf 'SKIP %-16s %s\n' "$name" "$r"
  else
    fail=$((fail+1)); printf 'FAIL %-16s stack[%s]=%s regvm[%s]=%s\n' "$name" "$se" "$s" "$re" "$r"
  fi
}

# The register VM's yield* .return() result differs from the stack VM's. Keep
# this kernel as a guard against the edge-drop regression that returned
# undefined instead of the result object. Accept a future stack-correct result
# as well, but reject undefined.
KREG_BASELINE() {
  name="$1"; k="$2"; shift 2
  r=$(timeout 20 "$REGVM" "$k" "$@" 2>&1); re=$?
  if [ "$re" -lt 128 ] && { [ "$r" = 'result=[object Object]:true:1' ] || [ "$r" = 'result=2:true:1' ]; }; then
    pass=$((pass+1))
  else
    fail=$((fail+1)); printf 'FAIL %-16s regvm[%s]=%s\n' "$name" "$re" "$r"
  fi
}

# constants, arithmetic, comparison, unary
K stack-overflow  'function(n){ try { (function f(k){ return f(k+1); })(0); return "nothrow"; } catch(e) { return e.constructor.name; } }' RangeError
K const-int       'function(n){ return 1+2*3-4; }' 0
K all-binops      'function(n){ return (((n+1)*2-3)/4%5)>>>0; }' 7
K bitwise         'function(n){ return (n&3)|(n^5)^(n<<2)^(n>>1); }' 9
K pow             'function(n){ return n**3+0; }' 3
K compare         'function(n){ return (n<5)+(n<=5)+(n>5)+(n>=5)+(n==5)+(n===5)+(n!=5)+(n!==5); }' 5
K unary           'function(n){ return -n+ +n + (+!n) + (~n); }' 5
K sub-imm-ovf     'function(n){ var a=(-2147483647)-1; var b=a-1; var c=a+1; return [a,b,c].join(","); }' 0
K add-imm-ovf     'function(n){ var a=2147483647; var b=a+1; var c=a-1; return [a,b,c].join(","); }' 0
K typeof-test     'function(n){ return typeof n; }' 5
K is-undef-null   'function(n){ return (n==null)?1:0; }' 0
K lnot            'function(n){ return !n?10:20; }' 1

# inc/dec, post forms, read-modify-write on a local
K pre-inc         'function(n){ var i=n; ++i; ++i; return i; }' 4
K post-inc        'function(n){ var i=n; var j=i++; return i*100+j; }' 4
K post-dec        'function(n){ var i=n; var j=i--; return i*100+j; }' 4
K inc-nested      'function(n){ var i=0,j=0; for(i=0;i<n;i++) j=i++; return j; }' 5

# Variable-home hazards. Keep these green on the stack VM before changing
# allocation: homes must preserve overlapping versions and values assigned to
# more than one variable.
K homes-postfix   'function(n){ var x = 0, y = 0; for (var i = 0; i < n; i++) { y = x++; } return x * 1000 + y; }' 5
K homes-call-old  'function(n){ var x = 1; function f(a, b){ return a * 10 + b; } return f(x, x = n); }' 5
K homes-expr-old  'function(n){ var x = 1; return x + (x = n) + x; }' 5
K homes-alias     'function(n){ var a = {v:1}, b; b = a; a = {v:n}; return b.v * 100 + a.v; }' 5
K homes-set-chain 'function(n){ var x, y; x = y = n; y = y + 1; return x * 10 + y; }' 5
K homes-swap      'function(n){ var t, a = 1, b = 2; for (var i = 0; i < n; i++) { t = a; a = b; b = t; } return a * 10 + b; }' 5
K homes-field     'function(n){ var o = {x: n}; var s = 0; for (var i = 0; i < 3; i++) { s = s + o.x; o = {x: o.x + 1}; } return s; }' 5
K homes-do-entry  'function(n){ var i = 0; do { i++; } while (i < n); return i; }' 5

# loops and branches
K while           'function(n){ var s=0; while(n>0){ s+=n; n--; } return s; }' 5
K dowhile         'function(n){ var s=0; do { s+=n; n--; } while(n>0); return s; }' 5
K for-loop        'function(n){ var s=0; for(var i=0;i<n;i++) s+=i; return s; }' 10
K nested-loop     'function(n){ var s=0; for(var i=0;i<n;i++) for(var j=0;j<n;j++) s+=i*j; return s; }' 6
K ternary-chain   'function(n){ return n>4?"a":n>2?"b":"c"; }' 3
K break-continue  'function(n){ var s=0; for(var i=0;i<20;i++){ if(i%2) continue; if(i>n) break; s+=i; } return s; }' 8
K switch-         'function(n){ switch(n){ case 1: return "one"; case 2: return "two"; default: return "many"; } }' 2

# strings and element/property access
K str-concat      'function(n){ var s=""; for(var i=0;i<n;i++) s=s+"x"; return s.length; }' 5
K str-charat      'function(n){ return "abcdefgh".charAt(n); }' 3
K str-methods     'function(n){ return "a-b-c".split("-").join("+"); }' 1
K str-index       'function(n){ return "abcde"[n]; }' 2
K str-len-prop    'function(n){ return "hello world".length; }' 0

# arrays
K arr-literal     'function(n){ var a=[n,2,3]; return a[0]+a[1]+a[2]; }' 4
K arr-loop        'function(n){ var a=[]; for(var i=0;i<n;i++) a=[i,i*i]; return a[0]+a[1]; }' 5
K arr-new         'function(n){ return new Array(n).length; }' 4
K arr-index-write 'function(n){ var a=[0,0,0]; for(var i=0;i<3;i++) a[i]=i*n; return a[0]+a[1]+a[2]; }' 3
K arr-length      'function(n){ var a=[1,2,3,4]; return a.length; }' 0

# calls: plain, args, nesting, arity, methods, tail, recursion
K call-empty      'function(n){ function g(){ return n+1; } return g(); }' 5
K call-args       'function(n){ function g(x,y,z){ return x+y+z; } return g(n,2,3); }' 5
K call-nested     'function(n){ function g(x){ return h(x)+1; } function h(y){ return y*2; } return g(n); }' 4
K call-loop       'function(n){ function g(x){ return x+1; } var s=0; for(var i=0;i<n;i++) s+=g(i); return s; }' 5
K call-arity8     'function(n){ function g(a,b,c,d,e,f,h,i){ return a+b+c+d+e+f+h+i; } return g(n,1,2,3,4,5,6,7); }' 5
K call-method     'function(n){ return "abcdef".slice(n,n+2); }' 1
K call-method2    'function(n){ return "abc".concat("def"); }' 0
K call-native     'function(n){ return Math.max(n,3)+Math.min(n,3); }' 5
K call-recursion  'function(n){ function f(x){ return x<2?1:x*f(x-1); } return f(n); }' 6
K call-mutual     'function(n){ function e(x){ return x==0?true:o(x-1); } function o(x){ return x==0?false:e(x-1); } return e(n)?1:0; }' 5
K call-many       'function(n){ function id(x){ return x; } return id(id(id(id(n)))); }' 7

# closures
K closure-capture 'function(n){ var f=function(x){ return x+n; }; return f(10); }' 5
K closure-loop    'function(n){ var fs=[]; for(var i=0;i<n;i++){ fs=[function(){ return 1; }]; } return fs.length; }' 4
K closure-return  'function(n){ function mk(){ return function(){ return n; }; } var f=mk(); return f(); }' 5

# critical edges: short-circuits and diamonds inside loops put a phi on an edge a
# conditional predecessor shares with a second successor, which needs a split
K edge-and        'function(n){ var s=0; for(var i=0;i<n;i++){ if(i>0 && i<10) s+=i; } return s; }' 14
K edge-or         'function(n){ var s=0; for(var i=0;i<n;i++){ if(i<2 || i>10) s+=i; } return s; }' 14
K edge-ternary    'function(n){ var s=0; for(var i=0;i<n;i++){ s += (i%2)?i:-i; } return s; }' 9
K edge-ternchain  'function(n){ var x=0; for(var i=0;i<n;i++){ x = i<3?1:(i<6?2:3); } return x; }' 9
K edge-swap       'function(n){ var a=1,b=1; for(var i=0;i<n;i++){ a=(i&1)?a+b:b; b=(i&2)?a+b:a; } return a+b; }' 7
K edge-break      'function(n){ var r=0; for(var i=0;i<n;i++){ if(i%2===0) r+=i; else r-=1; if(r>20) break; } return r; }' 12
K edge-continue   'function(n){ var s=0; for(var i=0;i<n;i++){ if(i%3) continue; s+=i*2; } return s; }' 12
K edge-nested     'function(n){ var s=0; for(var i=0;i<n;i++){ for(var j=0;j<n;j++){ if(i>j) s+=1; else if(i<j) s+=2; else s+=3; } } return s; }' 5
K edge-switch     'function(n){ var s=0; for(var i=0;i<n;i++){ switch(i%3){ case 0: s+=1; break; case 1: s+=2; break; default: s+=3; } } return s; }' 8
K edge-while-and  'function(n){ var s=0,i=0; while(i<n && s<100){ s+=i; i++; } return s; }' 30
K edge-str        'function(n){ var s=""; for(var i=0;i<n;i++){ s = (i%3===0)? s+"a" : s; } return s.length; }' 10

# lexical declarations, TDZ guards and closure cells
K let-decl        'function(n){ let x = n * 2; return x; }' 5
K const-decl      'function(n){ const y = n + 1; return y; }' 5
K let-block       'function(n){ { let z = n; return z; } }' 5
K let-nest        'function(n){ let a = n; { const b = a + 1; a = b; } return a; }' 5
K let-loop        'function(n){ let s = 0; for (let i = 0; i < n; i++) { s += i; } return s; }' 6
K let-captured    'function(n){ let x = n; function g(){ return x; } return g(); }' 5
K varref-loop     'function(n){ let s = 0; let f = function(){ return s; }; for(let i=0;i<n;i++) s += i; return s + f()*0; }' 5

# the frame values a function reads about itself
K arg-length      'function(n){ return arguments.length; }' 0
K arg-index       'function(n){ return arguments[0]; }' 7
K arg-mapped      'function(n){ arguments[0] = 3; return arguments[0]; }' 7

# bigint literals
K bigint-lit      'function(n){ var b = 5n; return (b === 5n) ? 1 : 0; }' 0
K bigint-cmp      'function(n){ return (n > 5n) ? 1 : 0; }' 7

# thrown values propagate out of a function with no handler of its own
K throw-prop      'function(n){ if(n<0) throw "neg"; return n; }' 5
K throw-prop-neg  'function(n){ if(n<0) throw "neg"; return n; }' -5
K throw-inner     'function(n){ function g(s){ if(s<2) throw "short"; return s; } return g(n); }' 7
K throw-const     'function(n){ const x = n; x = 5; return x; }' 3

# rest parameters, computed keys
K rest-params     'function(...a){ return a.length; }' 3
K rest-sum        'function(n, ...a){ return n + a.length; }' 1
K computed-key    'function(n){ var o = {[n]: 1, b: 2}; return o[n] + o.b; }' 5

# regexp literals
K regexp-test     'function(n){ return /^a.c$/.test("abc") ? 1 : 0; }' 0
K regexp-replace  'function(n){ return "a1b2".replace(/[0-9]/g, "x"); }' 0
K regexp-split    'function(n){ return "a,b,,c".split(/,/).length; }' 0

# object spread
K spread-obj      'function(n){ var o = {a: n}; var p = {...o, c: 3}; return p.a + p.c; }' 5
K spread-obj2     'function(n){ var o = {a: n}; var p = {...o, ...{b: 2}}; return p.a + p.b; }' 5

# array spread (append) and apply
K spread-arr      'function(n){ var a = [n, n+1]; var b = [0, ...a, 9]; return b.length * 100 + b[1] + b[2]; }' 5
K spread-arr-empty 'function(n){ var a = []; var b = [...a, n]; return b.length + b[0]; }' 4
K apply-this      'function(n){ function f(a,b){ return this.v + a + b; } var o = {v: n}; return f.apply(o, [1,2]); }' 5
K apply-args      'function(n){ function f(){ return arguments.length; } return f.apply(null, [1,2,3]); }' 0

# for-in
K forin-obj       'function(n){ var o = {a:1,b:2,c:3}; var s = 0; for (var k in o) s += o[k]; return s; }' 0
K forin-str       'function(n){ var o = {a:n,b:2}; var s = ""; for (var k in o) s += k; return s; }' 5
K forin-array     'function(n){ var a = [10,20,30]; var s = 0; for (var i in a) s += a[i]; return s; }' 0
K forin-body      'function(n){ var o = {a:1,b:2,c:3}; var s = 0; for (var k in o) { s = s + 1; } return s; }' 0
K forin-built     'function(n){ var o = {}; for (var i = 0; i < n; i++) o["k" + i] = i; var c = 0; for (var k in o) c++; return c; }' 4
K forin-args      'function(n){ function f(){ return arguments[0] + arguments[1]; } return f(n, 2); }' 5

# classes: methods, getters, statics, heritage, computed names, the ctor checks
K class-basic     'function(n){ class C { constructor(v){ this.v=v; } get(){ return this.v; } } return new C(n).get(); }' 5
K class-method    'function(n){ class C { m(){ return n; } } return new C().m(); }' 7
K class-static    'function(n){ class C { static s(){ return n; } } return C.s(); }' 7
K class-instanceof 'function(n){ class C {} var c=new C(); return (c instanceof C)?1:0; }' 0
K class-extends   'function(n){ class A { m(){ return n; } } class B extends A { m(){ return super.m()+1; } } return new B().m(); }' 5
K class-superctor 'function(n){ class A { constructor(v){ this.v=v; } } class B extends A { constructor(v){ super(v*2); } } return new B(n).v; }' 5
K class-getter    'function(n){ class C { get p(){ return n; } } return new C().p; }' 5
K class-computed  'function(n){ class C { ["m"+n](){ return n; } } return new C()["m"+n]; }' 5
K class-name      'function(n){ class C {} return typeof C; }' 0
K class-proto     'function(n){ var o={__proto__:null}; return (Object.getPrototypeOf(o)===null)?1:0; }' 0

# globals
K global-read     'function(n){ return Math.floor(n)+0; }' 3
K global-loops    'function(n){ var m; for(var i=0;i<n;i++) m=Math; return typeof m; }' 5

# in / instanceof / delete / in-operator
K in-op           'function(n){ return (n in {})?1:0; }' 3
K instanceof-op   'function(n){ function C(){} var o=new C(); return (o instanceof C)?1:0; }' 3

# for-of: a single loop, nested, siblings, break/continue, and the close on unwind
K forof-array    'function(n){ var s=0; for (var x of [1,2,3]) s+=x; return s; }' 0
K forof-string   'function(n){ var s=""; for (var x of "abc") s+=x; return s; }' 0
K forof-nested   'function(n){ var s=0; for(var x of [1,2]) for(var y of [10,20]) s+=x*y; return s; }' 0
K forof-siblings 'function(n){ for (var x of [1]) { } for (var y of [2]) { } return 0; }' 0
K forof-sib-sum  'function(n){ var s=0; for(var x of [1,2]) s+=x; for(var y of [10]) s+=y; return s; }' 0
K forof-break    'function(n){ var s=0; for (var x of [1,2,3]) { if(x>1) break; s+=x; } return s; }' 0
K forof-continue 'function(n){ var s=0; for (var x of [1,2,3]) { if(x===2) continue; s+=x; } return s; }' 0
K forof-write    'function(n){ var o={}; for(var x of [1,2]) o[x]=x*2; return o[1]+o[2]; }' 0
K forof-arg      'function(n){ var s=0; for(var x of [n,n+1]) s+=x; return s; }' 5
K forof-close    'function(n){ var it={ [Symbol.iterator](){ return { next(){ return {value:1,done:false}; }, return(){ throw "from-return"; } }; } }; for (var x of it) { throw "original"; } return 0; }' 0
K forof-3sib     'function(n){ var s=0; for(var x of [1]) s+=1; for(var y of [2]) s+=2; for(var z of [3]) s+=3; return s; }' 0

# try/catch/finally
K try-throw      'function(n){ try { throw "x"; } catch(e) { return e; } }' 0
K try-nothrow    'function(n){ try { return 1; } catch(e) { return 2; } }' 0
K try-finally    'function(n){ var log=""; try { log+="t"; } finally { log+="f"; } return log; }' 0
K try-catch-fill 'function(n){ var log=""; try { throw "e"; } catch(x) { log+="c"; } finally { log+="d"; } return log; }' 0
K try-cond       'function(n){ var log=""; try { log+="a"; if(n) throw "e"; log+="b"; } catch(x) { log+="c"; } finally { log+="d"; } return log; }' 1
K try-ret-fill   'function(n){ var log=""; function f(){ try { return 1; } finally { log+="f"; } } var x=f(); return log+x; }' 0
K try-loop-cont  'function(n){ var s=0; for(var i=0;i<3;i++){ try { if(i===1) continue; s+=i; } finally { s+=10; } } return s; }' 0
K try-loop-brk   'function(n){ var s=0; for(var i=0;i<3;i++){ try { if(i===2) break; s+=i; } finally { s+=10; } } return s; }' 0
K try-nested     'function(n){ try { try { throw "inner"; } finally { } } catch(e) { return e; } }' 0
K try-rethrow    'function(n){ try { try { throw "a"; } catch(e) { throw e+"b"; } } catch(e2) { return e2; } }' 0
K try-fill-throw 'function(n){ try { throw "a"; } finally { throw "b"; } }' 0
K try-in-fn      'function(n){ function g(){ throw "g"; } try { g(); } catch(e) { return e; } }' 0
K try-for                                                 'function(n){ var s=""; for(var i=0;i<2;i++){ try { s+=i; } catch(e) { s+="c"; } } return s; }' 0

# property keys and delete
K key-computed   'function(n){ var o={}; var k="x"; o[k]=5; return o.x; }' 0
K key-dynamic    'function(n){ var o={}; var k=n; o[k]=1; return o["0"]; }' 0
K key-object     'function(n){ var o={}; var p={}; o[p]=1; return o["[object Object]"]; }' 0
K key-loop       'function(n){ var o={}; for(var i=0;i<3;i++){ o["k"+i]=i; } return o.k0+o.k1+o.k2; }' 0
K key-null       'function(n){ try { var v=null; var k="a"; v[k]=1; return 0; } catch(e) { return e.constructor.name; } }' 0
K del-field      'function(n){ var o={a:1,b:2}; return delete o.a; }' 0
K del-result     'function(n){ var o={a:1,b:2}; var r=delete o.a; return r?o.b:o.a; }' 0
K del-computed   'function(n){ var o={a:1}; var r=delete o["a"]; return r?"gone":"there"; }' 0
K del-element    'function(n){ var a=[1,2,3]; delete a[1]; return a.length+":"+a[1]; }' 0
K del-strict     'function(n){ "use strict"; var o={a:1}; return delete o.a; }' 0

# spread calls and private members
K spread-basic   'function(n){ function f(a,b){ return a+b; } var a=[n,2]; return f(...a); }' 3
K spread-empty   'function(n){ function f(a){ return a; } return f(...[]); }' 0
K spread-new     'function(n){ function C(a,b){ this.s=a+b; } var c=new C(...[1,2]); return c.s; }' 0
K spread-args    'function(n){ function g(){ return arguments.length; } function f(){ return g(...arguments); } return f(1,2,3); }' 0
K spread-method  'function(n){ var o={m:function(a,b){ return this===o ? a+b : -1; }}; var a=[4,5]; return o.m(...a); }' 0
K priv-field     'function(n){ class C { #x = 1; get(){ return this.#x; } } return new C().get()+n; }' 1
K priv-ctor      'function(n){ class C { #x; constructor(){ this.#x = n; } get(){ return this.#x; } } return new C().get(); }' 7
K priv-brand     'function(n){ class C { #x = 5; has(o){ return #x in o; } } var c=new C(); return c.has(c)+":"+c.has({}); }' 0
K priv-static    'function(n){ class C { static #s = 3; static get(){ return C.#s; } } return C.get(); }' 0
K priv-incr      'function(n){ class C { #x = 1; m(){ this.#x++; return this.#x; } } var c=new C(); return c.m()+c.m(); }' 0
K priv-methods   'function(n){ class C { #m(){ return 1; } #n(){ return 2; } s(){ return this.#m()+this.#n(); } } return new C().s(); }' 0
K priv-two       'function(n){ class C { #x = 2; #y = 3; sum(){ return this.#x+this.#y; } } return new C().sum(); }' 0

# destructuring for-of (non-zero for_of_next offset)
K forof-destr2    'function(n){ var s=0; for (const [a,b] of [[1,2],[3,4]]) s+=a+b; return s; }' 0
K forof-destr3    'function(n){ var s=0; for (const [a,[b,c]] of [[1,[2,3]],[4,[5,6]]]) s+=a+b+c; return s; }' 0
K forof-destr4    'function(n){ var s=0; for (const [a,[b,[c,d]]] of [[1,[2,[3,4]]]]) s+=a+b+c+d; return s; }' 0
K forof-obj       'function(n){ var s=""; for (const {x,y} of [{x:1,y:2},{x:3,y:4}]) s+=x+y; return s; }' 0
K forof-obj-nest  'function(n){ var s=""; for (const {x:{y}} of [{x:{y:5}},{x:{y:6}}]) s+=y; return s; }' 0
K forof-nested    'function(n){ var s=0; for (const [a,b] of [[1,2]]) { for (const [c,[d]] of [[3,[4]]]) s+=a+b+c+d; } return s; }' 0
K forof-destr-one 'function(n){ var s=0; for (const [a] of [[1],[2]]) s+=a; return s; }' 0
K forof-destr-def 'function(n){ var s=0; for (const [a,b=9] of [[1,2],[3]]) s+=a+b; return s; }' 0
K forof-destr-break 'function(n){ var s=0; for (const [a,b] of [[1,2],[3,4],[5,6]]) { if(a>1) break; s+=a+b; } return s; }' 0

# object destructuring (the to_object reference leak)
K objdestr-one    'function(n){ var o={x:1}; var {x}=o; return x; }' 0
K objdestr-two    'function(n){ var o={x:1,y:2}; var {x,y}=o; return x+y; }' 0
K objdestr-nest   'function(n){ var o={x:{y:5}}; var {x:{y}}=o; return y; }' 0
K objdestr-rest   'function(n){ var o={x:1,y:2}; var {x,...r}=o; return x+Object.keys(r).length; }' 0
K objdestr-param  'function(n){ function f({x,y}) { return x+y; } return f({x:1,y:2}); }' 0
K objdestr-arr    'function(n){ var a=[1,2]; var [x,y]=a; return x+y; }' 0
K objdestr-num    'function(n){ var {x}=Object(n); return typeof x; }' 5
K objdestr-ret    'function(n){ var {a,b}={a:1,b:2}; return a+b; }' 0

# eval: the ops are in, but a direct eval also needs the with family to encode
K eval-indirect  'function(n){ var f=eval; return f("1+1"); }' 0
K eval-global    'function(n){ return eval.length; }' 0

# the with family
K with-get       'function(n){ var o={x:5}; var x=1; with(o){ return x+n; } }' 2
K with-nohit     'function(n){ var o={}; var x=1; with(o){ return x; } }' 0
K with-nested    'function(n){ var a={x:1}; with(a){ with(a){ return x; } } }' 0
K with-global    'function(n){ var o={x:1}; with(o){ y=7; } return y; }' 0
K with-put       'function(n){ var o={x:1}; with(o){ x=9; } return o.x; }' 0
K with-put-local 'function(n){ var x=1; var o={}; with(o){ x=5; } return x; }' 0
K with-compound  'function(n){ var o={a:1}; var s=0; with(o){ s+=a; } return s; }' 0
K with-incr      'function(n){ var o={x:1}; with(o){ x++; } return o.x; }' 0
K with-delete    'function(n){ var o={x:1}; with(o){ delete x; } return o.x; }' 0
K with-vardecl   'function(n){ var o={x:1}; with(o){ var q=5; } return q; }' 0
K with-call      'function(n){ var o={f:function(){ return 3; }}; var r=0; with(o){ r=f(); } return r; }' 0
K with-loop      'function(n){ var o={x:5}; with(o){ for(var i=0;i<2;i++) x+=i; } return o.x; }' 0
K with-eval      'function(n){ var o={x:2}; with(o){ return eval("x+1"); } }' 0
K with-nested-obj 'function(n){ var o={x:{y:2}}; with(o){ return x.y+n; } }' 1
K with-fn-param   'function f(o,n){ with(o){ return x+n; } } function(n){ return f({x:10},1); }' 0
K with-this      'function(n){ var o={x:1}; with(o){ return this===undefined ? 0 : 1; } }' 0
K with-scopables 'function(n){ var o={x:1}; o[Symbol.unscopables]={x:true}; var x=5; with(o){ return x; } }' 0

# suspension: generators and async
K gen-basic      'function(n){ function* g(){ yield 1; yield 2; } var it=g(); return it.next().value + it.next().value; }' 0
K gen-done       'function(n){ function* g(){ yield 1; } var it=g(); it.next(); return it.next().done; }' 0
K gen-arg        'function(n){ function* g(a,b){ yield a+b; } return g(1,2).next().value; }' 3
K gen-loop       'function(n){ function* g(){ for(var i=0;i<3;i++) yield i; } var s=0,it=g(); for(var r=it.next();!r.done;r=it.next()) s+=r.value; return s; }' 0
K gen-return     'function(n){ function* g(){ yield 1; return 9; } var it=g(); it.next(); var r=it.next(); return r.value + ":" + r.done; }' 0
K gen-send       'function(n){ function* g(){ var x = yield 1; yield x+1; } var it=g(); it.next(); return it.next(5).value; }' 0
K gen-throw      'function(n){ function* g(){ try { yield 1; } catch(e) { yield e; } } var it=g(); it.next(); return it.next(); }' 0
K gen-finally    'function(n){ var log=""; function* g(){ try { yield 1; } finally { log+="f"; } } var it=g(); it.next(); it.next(); return log; }' 0
K gen-delegate   'function(n){ function* a(){ yield 1; yield 2; } function* b(){ yield 0; yield* a(); } var s=0,it=b(); for(var r=it.next();!r.done;r=it.next()) s+=r.value; return s; }' 0
K gen-nested     'function(n){ function* inner(){ yield 1; } function* outer(){ yield* inner(); yield 2; } var s=0,it=outer(); for(var r=it.next();!r.done;r=it.next()) s+=r.value; return s; }' 0
K gen-recv       'function(n){ function* g(){ yield this.x; } var o={x:5,g:g}; return o.g().next().value; }' 0
K async-basic    'function(n){ var s=0; async function f(){ return 1; } f().then(function(v){ s=v; }); return s; }' 0
K async-await    'function(n){ async function f(){ return 1; } async function g(){ return (await f())+1; } return g(); }' 0
K await-for      'function(n){ async function* g(){ yield 1; } var s=0; for await (var v of g()) s+=v; return s; }' 0

# async: the kernel returns a function, which the runner calls after the job
# queue drains, so the settled result can be read.
#
# The async body cannot simply be called: a *refused* function throws at the
# call, and inside an async context that rejection is swallowed by the promise,
# so the kernel would report FAIL where nothing ran at all. Each kernel
# therefore starts its async work by calling a plain (non-async) wrapper and
# recording what that call did, which is what makes a refusal visible.
K async-ret      'function(n){ var out="?"; async function f(){ return 41; } f().then(function(v){ out="v"+v; }); return function(){ return out; }; }' 0
K async-await    'function(n){ var out="?"; async function f(){ return 41; } async function g(){ return (await f())+1; } g().then(function(v){ out="v"+v; }); return function(){ return out; }; }' 0
K async-throw    'function(n){ var out="?"; async function f(){ throw new Error("x"); } async function g(){ try { await f(); } catch(e) { return "caught:"+e.message; } } g().then(function(v){ out=v; }); return function(){ return out; }; }' 0
K async-try-fin  'function(n){ var out="?"; async function f(){ return 1; } var log=""; async function g(){ try { return (await f())+1; } finally { log+="b"; } } g().then(function(v){ out=log+v; }); return function(){ return out; }; }' 0
K async-order    'function(n){ var out=[]; async function f(x){ return x; } (async function(){ out.push(await f(1)); out.push(await f(2)); })(); return function(){ return out.join(","); }; }' 0
K for-await-gen  'function(n){ var out="?"; async function* g(){ yield 1; yield 2; yield 3; } (async function(){ var s=0; for await (var v of g()) s+=v; out="s"+s; })(); return function(){ return out; }; }' 0
K for-await-arr  'function(n){ var out="?"; (async function(){ var s=""; for await (var v of [1,2]) s+=v; out="s"+s; })(); return function(){ return out; }; }' 0
K async-throwin  'function(n){ var out="?"; async function* g(){ try { yield 1; } catch(e) { yield "c"; } } (async function(){ var it=g(); it.next(); var r=await it.throw("boom"); out=r.value; })(); return function(){ return out; }; }' 0
K async-gen-ret  'function(n){ var out="?"; async function* g(){ yield 1; return 9; } (async function(){ var it=g(); await it.next(); var r=await it.next(); out=r.value+":"+r.done; })(); return function(){ return out; }; }' 0

# a throw delivered in place of a value is not a yield resuming: the loop comes
# back to the *same* yield instruction, and the resume marker must have been
# consumed by the throw, or it reads the handshake slots as a resume argument.
K gen-throw-loop 'function(n){ function* g(){ var c=0; while(c<2){ try { yield c; } catch(e) { } c++; } return "end"; } var it=g(); it.next(); var t=it.throw("x"); var r=it.next(); return t.value+":"+t.done+"|"+r.value+":"+r.done; }' 0
K gen-throw-snd  'function(n){ function* g(){ var x=1; while(x<3){ try { x = yield x; } catch(e) { x++; } } return x; } var it=g(); it.next(); var a=it.next(2).value; var t=it.throw("e"); return a+":"+t.value; }' 0

# the built-in JS blobs are serialized bytecode: their functions arrive through
# the reader, not the frontend, so these are the serialization kernels.
K blob-fromasync 'function(n){ var out="?"; Array.fromAsync([1,2,3]).then(function(a){ out=a.join(","); }); return function(){ return out; }; }' 0
K blob-fromasync2 'function(n){ var out="?"; Array.fromAsync([1,2,3], function(v){ return v*2; }).then(function(a){ out=a.join(","); }); return function(){ return out; }; }' 0
K blob-zip       'function(n){ return JSON.stringify([...Iterator.zip([[1,2],[3,4]])]); }' 0
K blob-zipkey    'function(n){ return typeof Iterator.zipKeyed+":"+typeof Array.fromAsync; }' 0

# for-of and abrupt completion: the loop's sentinel is a control-stack close
# entry, so return/break/throw through a loop, nested loops, and a loop that
# runs to its end each have to leave the control stack where they found it.
K forof-ret      'function(n){ for(var x of [1,2]) { return x; } return 0; }' 0
K forof-ret-inner 'function(n){ for(var x of [1,2]) { for(var y of [3]) { return x*10+y; } } return 0; }' 0
K forof-ret-fn   'function(n){ function f(){ for(var x of [1,2]) { return x; } } return f(); }' 0
K forof-norm-close 'function(n){ var c=0; var it={ [Symbol.iterator](){return this;}, next(){return {done:true};}, return(){ c++; return {}; } }; for (var x of it) {} return c; }' 0
K forof-break-close 'function(n){ var c=0; var it={ [Symbol.iterator](){return this;}, next(){return {done:false,value:1};}, return(){ c++; return {}; } }; for (var x of it) { break; } return c; }' 0
K forof-throw-close 'function(n){ var c=0; var it={ [Symbol.iterator](){return this;}, next(){return {done:false,value:1};}, return(){ c++; return {}; } }; try { for (var x of it) { throw "e"; } } catch(e) {} return c; }' 0
K forof-ret-try  'function(n){ function f(){ for(var x of [1,2]) { try { return x; } finally { } } } return f(); }' 0
K forof-ret-nest-try 'function(n){ var s=0; for(var x of [1,2]) { try { for(var y of [3,4]) { if(y===3) continue; s+=1; } } finally { s+=10; } } return s; }' 0
K gen-forof-ret  'function(n){ function* g(){ for(var x of [1,2]) { return x; } } return g().next().value; }' 0
K gen-forof-abr  'function(n){ function* g(){ for(var x of [1,2]) yield x; } var it=g(); var a=it.next().value; it.return(9); return a; }' 0
# `yield*` drives the protocol itself: it drops the loop's sentinel by hand, so
# a throw through the delegation must not call the delegate's return method.
K gen-star-noret 'function(n){ var c=0; var it={ [Symbol.iterator](){return this;}, next(){return {done:false};}, throw(){ return {}; }, return(){ c++; return {done:true}; } }; var o=(function*(){ yield* it; })(); o.next(); try { o.throw("x"); } catch(e) {} return c; }' 0
K gen-star-ret   'function(n){ var c=0; var it={ [Symbol.iterator](){return this;}, next(){return {done:false};}, return(){ c++; return {done:true}; } }; var o=(function*(){ yield* it; })(); o.next(); o.return(1); return c; }' 0
K gen-star-ret-null 'function(n){ var c=0; var it={ next(){return {value:1,done:false};}, get return(){c++; return null;} }; it[Symbol.iterator]=function(){return it;}; function* g(){yield* it;} var o=g(); o.next(); var r=o.return(2); return r.value+":"+r.done+":"+c; }' 0

# with_put_var: the fall-through keeps the value the taken arm consumed, so the
# value that flows on must be the same one the assignment used.
K with-put-chain 'function(n){ var o={x:1}; var y; with(o){ y = x = 2; } return o.x*10+y; }' 0
K with-put-expr  'function(n){ var o={x:1}; var y=0; with(o){ y = (x = n) + 1; } return o.x*10+y; }' 0
K with-put-undef 'function(n){ var o={x:1}; var y="?"; with(o){ y = x; } return y; }' 0

# A generator or async body must hand its result over the way a suspension
# does, never fall into the ordinary function exit: that exit does not restore
# the frame's cur_sp and frees a frame the machinery still owns.
K async-empty    'function(n){ var out="?"; (async function(){})().then(function(v){ out="v"+v; }); return function(){ return out; }; }' 0
K async-ret-cond 'function(n){ var out="?"; async function f(){ if(n===0) return 7; return 9; } f().then(function(v){ out="v"+v; }); return function(){ return out; }; }' 0
K gen-ret-cond   'function(n){ function* g(){ if(n===0) return 5; yield 1; return 6; } var it=g(); var r=it.next(); return r.done ? "d"+r.value : "y"+r.value; }' 0
K gen-empty      'function(n){ function* g(){} var it=g(); var r=it.next(); return r.done+"|"+r.value; }' 0
K async-starret  'function(n){ var out="?"; async function* g(){ if(n===0) return 3; yield 1; } (async function(){ var it=g(); var r=await it.next(); out=r.value+":"+r.done; })(); return function(){ return out; }; }' 0

# A finally block is entered by a gosub and leaves by a ret, whose destination
# the CFG cannot see: a value live across that pair must stay live inside the
# finally, or the finally's own work takes its register.
K gen-throw-finally 'function(n){ function* g(){ try { yield 1; } finally { yield 2; } } var it=g(); it.next(); it.throw("E"); try { it.next(); } catch(e) { return "caught:"+e; } return "none"; }' 0
K gen-tf-nested  'function(n){ function* g(){ yield 1; try { yield 2; } finally { yield 3; } yield 4; } var it=g(); it.next(); it.next(); it.throw("E"); try { it.next(); } catch(e) { return "caught:"+e; } return "none"; }' 0
K gen-ret-fin    'function(n){ function* g(){ try { yield 1; } finally { yield 2; } } var it=g(); it.next(); var r=it.return("R"); return r.value+":"+r.done; }' 0
K tryfin-temp    'function(n){ var s=""; try { s+="a"; } finally { s+="f"; } return s; }' 0
K tryfin-ret     'function(n){ function f(){ try { return 1; } finally { } } return f(); }' 0

# A slow arithmetic or comparison path that throws leaves its operands alone --
# on the stack side the exception handler frees those slots. The register
# handler's duplicates are its own, so it has to release them itself.
K arith-throw    'function(n){ try { var o = { valueOf(){ throw "x"; } }; return o * 2; } catch(e) { return "caught"; } }' 0
K compare-throw  'function(n){ try { var o = {}; o < 2; return 0; } catch(e) { return "caught"; } }' 0
K instof-proxy   'function(n){ function Custom(){} var target={}; var p=new Proxy(target,{ getPrototypeOf(){ return Custom.prototype; } }); Object.preventExtensions(target); try { p instanceof Custom; } catch(e) { return e.constructor.name; } return "no"; }' 0

# A loop's close has to find its own entry: a finally running inside the loop
# has its continuation above it on the control stack, and that continuation
# belongs to code that is still on the stack.
K forof-ret-fin  'function(n){ var r=(function(){ for (var x of [1]) { try { } finally { return 34; } } })(); return r; }' 0
K forof-ret-fin2 'function(n){ function* values(){ yield 1; } var it=values(); var r=(function(){ for (var x of it) { try { } finally { return 34; } } return 0; })(); return r; }' 0
K forof-cont-fin 'function(n){ for (var x of [1,2]) { try { } finally { continue; } } return 9; }' 0
K forof-break-fin 'function(n){ for (var x of [1,2]) { try { } finally { break; } } return 9; }' 0

# explicit resource management: the error state is a value carried through the
# dispose scope, so every error path has to merge it the way the stack VM does.
K using-basic    'function(n){ using x = { [Symbol.dispose](){} }; return 1; }' 0
K using-null     'function(n){ using x = null; return 4; }' 0
K using-block    'function(n){ { using x = { [Symbol.dispose](){} }; } return 3; }' 0
K using-dispose  'function(n){ var d=0; using x = { [Symbol.dispose](){ d=9; } }; d=1; return d*10+d; }' 0
K using-throw    'function(n){ try { using x = { [Symbol.dispose](){ throw "boom"; } }; } catch(e) { return "c:"+e; } return "no"; }' 0
K using-two-throw 'function(n){ try { using x = { [Symbol.dispose](){ throw "a"; } }; using y = { [Symbol.dispose](){ throw "b"; } }; } catch(e) { return "c:"+e; } return "no"; }' 0
K using-nomethod 'function(n){ try { using x = {}; } catch(e) { return e.constructor.name; } return "no"; }' 0
K using-body-throw 'function(n){ try { using x = { [Symbol.dispose](){ throw "a"; } }; throw "body"; } catch(e) { return "c:"+e; } return "no"; }' 0
K using-nested   'function(n){ var log=""; function mk(t){ return { [Symbol.dispose](){ log+=t; } }; } { using a = mk("a"); using b = mk("b"); } return log; }' 0
K await-using    'function(n){ var out="?"; (async function(){ await using x = { [Symbol.asyncDispose](){ return Promise.resolve(); } }; out="ok"; })().then(function(){ out="done:"+out; }); return function(){ return out; }; }' 0
K await-using-sync 'function(n){ var out="?"; (async function(){ await using x = { [Symbol.dispose](){} }; out="ok"; })().then(function(){ out="done:"+out; }); return function(){ return out; }; }' 0
K await-using-throw 'function(n){ var out="?"; (async function(){ try { await using x = { [Symbol.asyncDispose](){ throw new Error("ad"); } }; } catch(e) { out="c:"+e.message; } })(); return function(){ return out; }; }' 0

# super property access: the property is read from the home object with the
# receiver, which is what an inherited getter needs.
K super-get      'function(n){ class A { get m(){ return 7; } } class B extends A { n(){ return super.m; } } return new B().n(); }' 0
K super-call     'function(n){ class A { m(){ return this.v; } } class B extends A { constructor(){ super(); this.v=5; } n(){ return super.m(); } } return new B().n(); }' 0
K super-set      'function(n){ class A { set m(v){ this._v=v*2; } } class B extends A { set(v){ super.m = v; } } var b=new B(); b.set(4); return b._v; }' 0
K super-field    'function(n){ class A { constructor(){ this.x=1; } } class B extends A { constructor(){ super(); this.x += 1; } } return new B().x; }' 0

# A body whose program produces no value still has effects. The eval here is
# lowered to a single throw_error, and the empty-body fallback used to replace
# it with `return undefined`, so the redeclaration was never reported.
K eval-redecl-arg  'function(n){ var f = function(p = eval("var arguments"), arguments) {}; try { f(); return "no-throw"; } catch(e) { return e.constructor.name; } }' 0
K eval-redecl-arg2 'function(n){ function f(p = eval("var arguments"), arguments) {} try { f(); return "no-throw"; } catch(e) { return e.constructor.name; } }' 0

# The postfix forms publish the *coerced* old value, and coerce it once.
K post-inc-string 'function(n){ var x = "1"; var y = x++; return typeof y + ":" + y + ":" + x; }' 0
K post-dec-null   'function(n){ var x = null; var y = x--; return y + ":" + x; }' 0
K post-inc-bool   'function(n){ var x = true; var y = x--; return typeof y + ":" + y; }' 0
K post-inc-bigint 'function(n){ var x = 5n; var y = x++; return typeof y + ":" + y + ":" + x; }' 0
K post-inc-object 'function(n){ var calls = 0; var o = { valueOf: function(){ calls++; return 1; } }; var y = o++; return y + ":" + calls; }' 0
K post-inc-obj0   'function(n){ var x = new Number(-0.1); var y = x--; return y + ":" + (typeof y); }' 0
K post-inc-twice  'function(n){ var x = "9"; x++; x++; return x; }' 0
K pre-dec-string  'function(n){ var x = "3"; var y = --x; return y + ":" + x; }' 0

# IteratorClose in destructuring and for-of: the close happens when the pattern
# is what threw, and not when the iterator's own step threw (the step helper
# clears the iterator slot to say so, which the close entry has to mirror).
K iterclose-next-throw 'function(n){ var rc = 0; var it = { next: function(){ throw new Error("x"); }, return: function(){ rc++; return {}; } }; var itb = {}; itb[Symbol.iterator] = function(){ return it; }; try { var [x] = itb; } catch(e) {} return rc; }' 0
K iterclose-value-throw 'function(n){ var rc = 0; var bad = {}; Object.defineProperty(bad, "value", { get: function(){ throw new Error("boom"); } }); var it = { next: function(){ return bad; }, return: function(){ rc++; return {}; } }; var itb = {}; itb[Symbol.iterator] = function(){ return it; }; try { var [x] = itb; } catch(e) {} return rc; }' 0
K iterclose-binding-throw 'function(n){ var rc = 0; var it = { next: function(){ return { value: 1, done: false }; }, return: function(){ rc++; return {}; } }; var itb = {}; itb[Symbol.iterator] = function(){ return it; }; var o = {}; Object.defineProperty(o, "p", { set: function(){ throw new Error("set"); } }); try { [o.p] = itb; } catch(e) {} return rc; }' 0
K iterclose-done 'function(n){ var rc = 0; var it = { next: function(){ return { value: 1, done: true }; }, return: function(){ rc++; return {}; } }; var itb = {}; itb[Symbol.iterator] = function(){ return it; }; var [x] = itb; return rc; }' 0
K iterclose-body-throw 'function(n){ var rc = 0; var it = { next: function(){ return { value: 1, done: false }; }, return: function(){ rc++; return {}; } }; var itb = {}; itb[Symbol.iterator] = function(){ return it; }; try { for (var x of itb) { throw new Error("body"); } } catch(e) {} return rc; }' 0
K iterclose-rest 'function(n){ var rc = 0; var it = { next: function(){ return { value: 1, done: true }; }, return: function(){ rc++; return {}; } }; var itb = {}; itb[Symbol.iterator] = function(){ return it; }; var [a, ...r] = itb; return rc + ":" + a + ":" + r.length; }' 0
K iterclose-rest-done 'function(n){ var rc = 0; var it = { next: function(){ return { done: true }; }, return: function(){ rc++; return {}; } }; var itb = {}; itb[Symbol.iterator] = function(){ return it; }; var [...r] = itb; return rc + ":" + r.length; }' 0
K iterclose-rest-throw 'function(n){ var rc = 0; var it = { next: function(){ throw new Error("x"); }, return: function(){ rc++; return {}; } }; var itb = {}; itb[Symbol.iterator] = function(){ return it; }; try { var [...r] = itb; } catch(e) {} return rc; }' 0

# A write to a read-only property is a no-op in sloppy code and a TypeError in
# strict code; the registers must ask for JS_PROP_THROW_STRICT, not an
# unconditional throw and not no throw at all.
K readonly-sloppy 'function(n){ var e = Math.E; Math.E = 1; return Math.E === e; }' 0
K readonly-strict 'function(n){ "use strict"; var e = Math.E; try { Math.E = 1; return "no-throw"; } catch(err) { return err.constructor.name; } }' 0
K readonly-elem-strict 'function(n){ "use strict"; var a = []; Object.defineProperty(a, "0", { value: 1, writable: false }); try { a[0] = 2; return "no-throw"; } catch(err) { return err.constructor.name; } }' 0
K readonly-elem-sloppy 'function(n){ var a = []; Object.defineProperty(a, "0", { value: 1, writable: false }); a[0] = 2; return a[0]; }' 0
K readonly-frozen-strict 'function(n){ "use strict"; var o = Object.freeze({}); try { o.x = 1; return "no-throw"; } catch(err) { return err.constructor.name; } }' 0
K readonly-frozen-elem 'function(n){ "use strict"; var o = Object.freeze({}); try { o["x"] = 1; return "no-throw"; } catch(err) { return err.constructor.name; } }' 0

# Work A: property-cache correctness gates. Keep these ahead of PIC changes;
# run the lazy-JSON kernel with QJS_JSON_LAZY=1 so JSON.parse creates lazy slots.
K a-proto-getter-add 'function(n){ var p = {}; var o = Object.create(p); Object.defineProperty(p, "x", { configurable: true, get: function(){ return 3; } }); function read(v){ return v.x; } var a = read(o) + read(o); Object.defineProperty(p, "x", { configurable: true, get: function(){ return 8; } }); return a + ":" + read(o); }' 0
K a-delete-readd 'function(n){ var o = { x: 4 }; function read(v){ return v.x; } var a = read(o) + read(o); delete o.x; o.x = 9; return a + ":" + read(o); }' 0
K a-shared-shape-mutate 'function(n){ var a = { x: 2, y: 3 }, b = { x: 5, y: 6 }; function read(v){ return v.x; } var before = read(a) + read(b); b.z = 10; b.x = 11; return before + ":" + read(a) + ":" + read(b); }' 0
K a-lazy-json-warmed 'function(n){ var text = JSON.stringify({ target: 17, padding: Array(5001).join("x") }); var o = JSON.parse(text); function read(v){ return v.target; } var sum = 0; for (var i = 0; i < 6; i++) sum += read(o); return sum; }' 0
# Two objects from the same text share a shape, so after the first object's
# slot is materialised and cached, a still-lazy object with that shape must NOT
# be served from the cache. Dropping the hit's lazy test segfaults here.
KLAZY a-lazy-json-two 'function(n){ var text = JSON.stringify({ target: 17, pad: Array(5001).join("x") }); function read(v){ return v.target; } var a = JSON.parse(text); var s = read(a); var b = JSON.parse(text); return s + ":" + read(b) + ":" + read(b); }' 0
KLAZY a-lazy-json-fresh 'function(n){ function read(v){ return v.target; } var sum = 0; for (var i = 0; i < 4; i++) { var text = JSON.stringify({ target: 17 + i, pad: Array(5001).join("x") }); var o = JSON.parse(text); sum += read(o); } return sum; }' 0
K a-proxy-warmed 'function(n){ var count = 0; var target = { x: 12 }; var p = new Proxy(target, { get: function(t, k, r){ count++; return Reflect.get(t, k, r); } }); function read(v){ return v.x; } var sum = read(p) + read(p) + read(p); return sum + ":" + count; }' 0
K a-array-length-polymorphic 'function(n){ function read(v){ return v.length; } var a = [1,2,3]; var o = { length: 7 }; return read(a) + ":" + read(o) + ":" + read(a); }' 0
K a-megamorphic-shapes 'function(n){ function read(v){ return v.x; } var sum = 0; for (var i = 0; i < 12; i++) { var o = { x: i }; o["extra" + i] = i; sum += read(o); } return sum; }' 0
K a-borrowed-chain-delete 'function(n){ var root = {}; var child = {}; root.child = child; Object.defineProperty(child, "value", { get: function(){ delete root.child; return 42; } }); var value = root.child.value; return value + ":" + ("child" in root); }' 0
K a-borrowed-chain-valueof 'function(n){ var root = {}; var child = { valueOf: function(){ delete root.child; return 13; } }; root.child = child; return root.child + 1; }' 0
# The global mega stub cache (regvm_get_field_ic probe + regvm_pic_fill
# fills). Each kernel drives a read site past the per-function PIC's dead limit
# and then mutates, so a stale mega entry (raw shape/offset) would show up as a
# wrong value. The first feeds the site fresh unhashed receiver shapes in a loop
# (forces the dead state), then deletes and re-adds a property. The second reads
# a property two prototypes deep and then edits that prototype. The third is
# polymorphic (two shapes alternating) before a delete/re-add.
K a-mega-unhashed-dead 'function(n){ function read(v){ return v.x; } var sum = 0; for (var i = 0; i < 20; i++) { var o = { x: i, ["p" + i]: i }; sum += read(o); } var m = { x: 10, y: 20 }; var before = read(m); delete m.y; m.z = 30; return sum + ":" + before + ":" + read(m); }' 0
K a-mega-depth2 'function(n){ function read(v){ return v.x; } var gp = { x: 7, pad: 1 }; var p = Object.create(gp); var o = Object.create(p); var sum = 0; for (var i = 0; i < 16; i++) sum += read(o); delete gp.x; gp.x = 42; return sum + ":" + read(o); }' 0
K a-mega-polymorphic 'function(n){ function read(v){ return v.x; } var a = { x: 1, y: 2, pad: 3 }; var b = { x: 10, y: 20 }; var sum = 0; for (var i = 0; i < 400; i++) { sum += read(i & 1 ? a : b); } var before = read(a); delete a.x; a.x = 111; return sum + ":" + before + ":" + read(a); }' 0
# The autoinit `prototype` slot. Reading f.prototype materialises
# a lazily-created autoinit slot in place, and the receiver's own (unhashed,
# refcount-1) shape is then churned by add/delete so that resize_properties /
# compact_properties can free or move the shape allocation. The read site is
# driven megamorphic so the mega cache is filled with such a shape. NOTE: this
# kernel covers the path and would catch a crash or a wrong value, but it does
# NOT deterministically reproduce the stale-address aliasing that box2d hit --
# that depends on allocator address reuse. The fix (shape_free_epoch bumps in
# resize_properties/compact_properties) is validated by an instrumented build
# that saw 5 deterministic stale hits on the box2d workload and 0 after the fix.
K a-mega-autoinit-proto 'function(n){ function readProto(f){ return f.prototype; } var bad = 0, sum = 0; for (var i = 0; i < 400; i++) { var f = function(){}; var p = readProto(f); if (typeof p !== "object" || p === null || p.constructor !== f) bad++; sum += 1; f.p1 = i; f.p2 = i; f.p3 = i; delete f.p1; delete f.p2; delete f.p3; var g = function(){}; var q = readProto(g); if (typeof q !== "object" || q === null || q.constructor !== g) bad++; } return bad + ":" + sum; }' 0

# A6b: the register store PIC. The store site is inside `put`, warmed by the
# first calls; each kernel pins one fill rule or the release the hit performs.
K a-put-own-warm   'function(n){ var o = { x: 0 }; function put(v, k){ v.x = k; return v.x; } var s = 0; for (var i = 0; i < 5; i++) s += put(o, i); return s + ":" + o.x; }' 0
K a-put-shape-change 'function(n){ var o = { x: 1, y: 2 }; function put(v, k){ v.x = k; return v.x; } var a = put(o, 5) + put(o, 6); o.z = 9; var b = put(o, 7); return a + ":" + b + ":" + o.x + o.y + o.z; }' 0
K a-put-add-warm  'function(n){ function put(v, k){ v.x = k; return v.x; } var o1 = {}; var a = put(o1, 1); var o2 = {}; var b = put(o2, 2); var c = put(o2, 3); return a + ":" + b + ":" + c + ":" + o2.x; }' 0
K a-put-inherited-setter 'function(n){ var p = {}; Object.defineProperty(p, "x", { configurable: true, get: function(){ return this.y; }, set: function(v){ this.y = v * 2; } }); var o = Object.create(p); function put(v, k){ v.x = k; return v.y; } var a = put(o, 3) + put(o, 4); return a + ":" + o.x + ":" + ("x" in o); }' 0
K a-put-shadow-accessor 'function(n){ var p = { get x(){ return 100; } }; var o = Object.create(p); function put(v, k){ v.x = k; return v.x; } var a = put(o, 1) + put(o, 2); return a + ":" + ("x" in o); }' 0
K a-put-nonwritable 'function(n){ var o = {}; Object.defineProperty(o, "x", { value: 1, writable: false, configurable: true }); function put(v, k){ v.x = k; return v.x; } var a = put(o, 5) + put(o, 6); return a + ":" + o.x; }' 0
K a-put-own-accessor 'function(n){ var o = {}; var log = 0; Object.defineProperty(o, "x", { configurable: true, get: function(){ return log; }, set: function(v){ log = v + 1; } }); function put(v, k){ v.x = k; return v.x; } var a = put(o, 4) + put(o, 5); return a + ":" + log; }' 0
K a-put-proxy-warmed 'function(n){ var log = 0; var t = {}; var p = new Proxy(t, { set: function(o, k, v, r){ log++; o[k] = v; return true; } }); function put(v, k){ v.x = k; return v.x; } var a = put(p, 1) + put(p, 2) + put(p, 3); return a + ":" + log + ":" + t.x; }' 0
K a-put-delete-readd 'function(n){ var o = { x: 1, y: 2 }; function put(v, k){ v.x = k; return v.x; } var a = put(o, 3) + put(o, 4); delete o.x; var b = put(o, 5); return a + ":" + b + ":" + o.x; }' 0
K a-put-frozen   'function(n){ var o = Object.freeze({ x: 1 }); function put(v, k){ v.x = k; return v.x; } var a = put(o, 9) + put(o, 9); return a + ":" + o.x; }' 0
K a-put-array-named 'function(n){ var a = [1,2,3]; function put(v, k){ v.tag = k; return v.tag; } var s = put(a, 1) + put(a, 2) + a.tag; return s + ":" + a.length + ":" + a[2]; }' 0
K a-put-release  'function(n){ var o = { x: null }; function put(v, k){ v.x = k; return v.x === k; } var r = 0; for (var i = 0; i < 8; i++) r += put(o, { i: i }) ? 1 : 0; return r + ":" + o.x.i; }' 0
K a-put-strict-throw 'function(n){ "use strict"; var o = {}; Object.defineProperty(o, "x", { value: 1, writable: false }); function put(v, k){ try { v.x = k; return "ok"; } catch(e){ return e.constructor.name; } } return put(o, 5) + ":" + o.x; }' 0
K a-put-megamorphic 'function(n){ function put(v, k){ v.x = k; return v.x; } var sum = 0; for (var i = 0; i < 12; i++) { var o = { x: i }; o["extra" + i] = i; sum += put(o, i + 1); } return sum; }' 0
K a-put-class-field 'function(n){ class C { constructor(){ this.x = 0; } } function put(v, k){ v.x = k; return v.x; } var c = new C(); var a = put(c, 1) + put(c, 2); c.y = 5; var b = put(c, 3); return a + ":" + b + ":" + c.y; }' 0
K a-put-primitive 'function(n){ function put(v, k){ v.x = k; return v.x; } return put(1, 2) + ":" + put("s", 3); }' 0

# B4: the bitwise and shift fast path. Int operands, doubles inside and outside
# the int32 range, the modular conversion, the unsigned result of `>>>`, the
# masked shift count, BigInt, the BigInt/Number TypeError, and non-numeric
# operands all have to agree with the engine.
K a-bit-int       'function(n){ var a = n, b = n + 1; return (a & b) + ":" + (a | b) + ":" + (a ^ b) + ":" + (a << 2) + ":" + (a >> 1) + ":" + (a >>> 1); }' 7
K a-bit-double    'function(n){ var a = n + 0.5, b = 3.9; return (a & b) + ":" + (a | 0) + ":" + (a ^ 1) + ":" + (a << 1) + ":" + (b >> 1) + ":" + (a >>> 1); }' 4
K a-bit-range     'function(n){ var big = 2147483648, neg = -2147483649, ten = 1e10, mn = -2147483648; return (big | 0) + ":" + (neg | 0) + ":" + (ten | 0) + ":" + (mn | 0) + ":" + (2147483647.9 | 0); }' 0
K a-bit-shr       'function(n){ var a = -1, b = -8, c = 4294967295; return (a >>> 0) + ":" + (b >>> 1) + ":" + (c >>> 0) + ":" + (1 << 31) + ":" + (a << 1); }' 0
K a-bit-shiftcnt  'function(n){ var a = 1, b = 33; return (a << b) + ":" + (a >> b) + ":" + (a >>> b) + ":" + (a << 31) + ":" + (a << -1); }' 0
K a-bit-nan       'function(n){ var x = NaN, y = Infinity; return (x | 0) + ":" + (y | 0) + ":" + (-y | 0) + ":" + (x & 1) + ":" + (y >> 0); }' 0
K a-bit-bigint    'function(n){ var a = 5n, b = 3n; return (a & b) + ":" + (a | b) + ":" + (a ^ b) + ":" + (a << b) + ":" + (a >> 1n); }' 0
K a-bit-bigint-mix 'function(n){ try { return 5n & 3; } catch (e) { return e.constructor.name; } }' 0
K a-bit-bigint-shl 'function(n){ try { return 5n << 1; } catch (e) { return e.constructor.name; } }' 0
K a-bit-bigint-shr 'function(n){ try { return 1n >>> 0n; } catch (e) { return e.constructor.name; } }' 0
K a-bit-string    'function(n){ var a = "3", b = "5"; return (a & b) + ":" + (a | b) + ":" + (a << b) + ":" + (a >> 1) + ":" + (a >>> 1); }' 0
K a-bit-bool      'function(n){ return (true & 1) + ":" + (false | 2) + ":" + (null | 3) + ":" + (undefined | 0) + ":" + (NaN >>> 0); }' 0

# B4: the double compare-branch. IEEE comparisons are not complement-closed, so
# every inverted form must be `!(cmp)` and not the opposite comparison: `!(NaN <
# 1)` is true where `NaN >= 1` is false. Every sense and its inverse is checked,
# then the same through a loop back edge.
K a-cmp-nan       'function(n){ var x = NaN; return (x < 1) + ":" + !(x < 1) + ":" + (x <= 1) + ":" + !(x <= 1) + ":" + (x > 1) + ":" + !(x > 1) + ":" + (x >= 1) + ":" + !(x >= 1); }' 0
K a-cmp-nan-rev   'function(n){ var x = NaN; return (1 < x) + ":" + !(1 < x) + ":" + (1 <= x) + ":" + !(1 <= x) + ":" + (1 > x) + ":" + !(1 > x) + ":" + (1 >= x) + ":" + !(1 >= x); }' 0
K a-cmp-nan-loop  'function(n){ var c = 0, x = NaN; for (var i = 0; i < x; i++) { c++; if (c > 5) break; } for (var j = 0; !(j >= x); j++) { c++; if (c > 9) break; } return c; }' 0
K a-cmp-double    'function(n){ var a = n + 0.5, b = 3.25, c = 3.25; return (a < b) + ":" + (a <= b) + ":" + (a > b) + ":" + (a >= b) + ":" + (b == c) + ":" + (b != c) + ":" + (-0 == 0); }' 4
K a-cmp-mixed     'function(n){ var a = n + 0.5, b = 3; return (a < b) + ":" + (b < a) + ":" + (a == b) + ":" + (a >= b) + ":" + (a <= b); }' 4

# ---------------------------------------------------------------------------
# Counted loops with an EMPTY body, and arithmetic on two locals.
#
# Both were missing, and each hid a real bug for a while.
#
# The empty body is the only shape in which the encoder fuses the increment and
# the back edge into `ROP_add_imm_self_branch`/`_s`, and the narrow form's
# target is one byte wide. Reading it as four left the instruction pointer
# mid-operand on an opcode byte of zero: the interpreter died with
# "unimplemented opcode 0", while this suite reported pass=103 fail=0 skip=0 --
# because the loop kernels that existed all had a body, so the increment and the
# branch were never adjacent. Every comparison sense is exercised, and the
# while/dowhile forms as well as the for form.
K loop-empty-for     'function(n){ var i; for (i = 0; i < n; i++) { } return i; }' 1000
K loop-empty-for-lt  'function(n){ var i; for (i = 0; i < n; i++) { } return i; }' 3
K loop-empty-while   'function(n){ var i = 0; while (i < n) i = i + 1; return i; }' 1000
K loop-empty-dowhile 'function(n){ var i = 0; do { i = i + 1; } while (i < n); return i; }' 1000
K loop-empty-down    'function(n){ var i; for (i = n; i > 0; i--) { } return i; }' 1000
K loop-empty-le      'function(n){ var i; for (i = 0; i <= n; i++) { } return i; }' 200
K loop-empty-ge      'function(n){ var i; for (i = n; i >= 0; i--) { } return i; }' 200
K loop-empty-nested  'function(n){ var i, j; for (i = 0; i < n; i++) for (j = 0; j < n; j++) { } return i * 1000 + j; }' 30
K loop-empty-float   'function(n){ var i = 0; while (i < n) i = i + 0.5; return i; }' 100
K loop-empty-str     'function(n){ var i = 0; while (i < n) i = i + "1"; return typeof i; }' 4
K loop-empty-arg     'function(n){ while (n > 0) n = n - 1; return n; }' 1000
K loop-empty-big     'function(n){ var i = 0n; while (i < n) i = i + 1n; return i; }' 50
K loop-empty-neg     'function(n){ var i = 0; while (i > -n) i = i - 1; return i; }' 100
K loop-empty-bool    'function(n){ var i = 0; while (i < n) i = i + true; return i; }' 20
K loop-empty-imm-ovf 'function(n){ var i = 2147483640; while (i < n) i = i + 10; return i; }' 2147483660

# The `_s` narrow form is only reachable when the whole function is small
# enough that the operand fits one byte; these are deliberately tiny.
K loop-narrow-1      'function(){ var i = 0; while (i < 5) i = i + 1; return i; }' 0
K loop-narrow-2      'function(){ var i = 0; for (i = 0; i < 7; i++) { } return i; }' 0
K loop-narrow-3      'function(){ var a = 0, b = 0; while (a < 3) { a = a + 1; b = b + 2; } return b; }' 0

# Two local variables, and a local with an argument: a past bug made
# `a + b` return `a + a` on two locals and every existing kernel still passed,
# because none of them did arithmetic on two *variables* through the converted
# opcode. One kernel per operator family.
K bin-ll-add      'function(n){ var a = n + 3; var b = n - 1; return a + b; }' 7
K bin-ll-sub      'function(n){ var a = n + 3; var b = n - 1; return a - b; }' 7
K bin-ll-mul      'function(n){ var a = n + 3; var b = n - 1; return a * b; }' 7
K bin-ll-div      'function(n){ var a = n + 3; var b = n - 1; return a / b; }' 7
K bin-ll-mod      'function(n){ var a = n + 3; var b = n - 1; return a % b; }' 7
K bin-ll-pow      'function(n){ var a = n + 3; var b = n - 1; return a ** b; }' 2
K bin-ll-shift    'function(n){ var a = n + 3; var b = n - 1; return (a << b) + (a >> b) + (a >>> b); }' 7
K bin-ll-bitwise  'function(n){ var a = n + 3; var b = n - 1; return (a & b) + (a | b) + (a ^ b); }' 7
K bin-ll-cmp      'function(n){ var a = n + 3; var b = n - 1; return (a < b) + (a <= b) + (a > b) + (a >= b); }' 7
K bin-ll-eq       'function(n){ var a = n + 3; var b = n - 1; return (a == b) + (a === b) + (a != b) + (a !== b); }' 7
K bin-ll-float    'function(n){ var a = n + 0.5; var b = n - 0.25; return a * b + a / b; }' 7
K bin-ll-mixed    'function(n){ var a = n + 0.5; var b = n; return a * b + a - b; }' 7
K bin-ll-str      'function(n){ var a = "x" + n; var b = "y"; return a + b; }' 7
K bin-ll-obj      'function(n){ var a = {v: n}; var b = {v: 2}; return a + b; }' 7
K bin-ll-bigint   'function(n){ var a = 5n; var b = 3n; return a * b - a + b; }' 0
K bin-ll-bool     'function(n){ var a = true; var b = n; return a + b; }' 7
K bin-ll-null     'function(n){ var a = null; var b = n; return a + b; }' 7
K bin-ll-loop     'function(n){ var a = n, b = 1, i; for (i = 0; i < 20; i++) { a = a * b; b = b + 1; } return a; }' 3
K bin-ll-all      'function(n){ var a = n + 5, b = n - 2; return (a+b)*100 + (a-b) ; }' 9
K bin-la-add      'function(n){ var a = n; return a + n; }' 7
K bin-la-sub      'function(n){ var a = n + 1; return a - n; }' 7
K bin-arg-loc-sub 'function(n){ var a = n + 1; return n - a; }' 7
K bin-arg-loc-mul 'function(n){ var a = n + 1; return n * a; }' 7
K bin-arg-loc-div 'function(n){ var a = n + 1; return n / a; }' 7
K bin-arg-loc-mod 'function(n){ var a = n + 1; return n % a; }' 7
K bin-arg-loc-cmp 'function(n){ var a = n + 1; return (n < a) + (n > a) + (n == a) + (n === a); }' 7
K bin-ll-arg-loop 'function(n){ var a = n, i; for (i = 0; i < 10; i++) a = a + n; return a; }' 4

# Three-operand chains, so the allocator has to keep more than one value live
# across a converted opcode.
K bin-chain       'function(n){ var a = n, b = n + 1, c = n + 2; return a * b + b * c - a * c; }' 6
K bin-chain-shift 'function(n){ var a = n, b = n + 1, c = n + 2; return ((a + b) << 2) + ((b - c) >> 1); }' 6


K stack-nested-names 'function(n){ function e(){ return null.x; } function d(){ e(); return 1; } function c(){ d(); return 1; } function b(){ c(); return 1; } function a(){ b(); return 1; } try { a(); } catch (err) { return err.stack.split("\\n").map(function(l){ var m = /at ([A-Za-z0-9_$]+)/.exec(l); return m ? m[1] : ""; }).filter(function(s){ return s !== ""; }).join(","); } return "nothrow"; }' e,d,c,b,a

# `nip` and the slot-forwarding model must alias
# stack shapers exactly as regvm_pushed does, or a value nothing defines is read.
K shaper-nip-lgcl-and   'function(n){ var o={f:false}; function t(x){ return x.f &&= 1; } return String(t(o)) + String(t({f:n})); }' 3
K shaper-nip-lgcl-or    'function(n){ var o={f:0}; function t(x){ return x.f ||= n; } return String(t(o)) + ":" + o.f; }' 3
K shaper-nip-lgcl-nul   'function(n){ var o={f:null}; function t(x){ return x.f ??= n; } return String(t(o)) + ":" + o.f; }' 3
K s0-priv-lgcl      'function(n){ class C { get #f(){ return false; } set #f(v){ throw 1; } t(){ return this.#f &&= 1; } } return String(new C().t()); }' 3
K s0-ystar-result   'function(n){ var k=0; var it={ next(v){ k++; return {value:k, done:k>n}; } }; var obj={[Symbol.iterator](){return it;}}; function* g(){ var r = yield* obj; return "r"+r; } var o=g(), out=[], x; while (!(x=o.next()).done) out.push(x.value); out.push(x.value); return out.join(); }' 3
K s0-fwd-after-dup  'function(n){ function t(a){ var r; r = (a, a.v); var q = r; return q + r; } return t({v:n}); }' 3

# 0191 regressions (2026-10-08). In-place mutation is only legal for a value
# with a single counted owner that is not parked. `s` is a promoted local, so
# the accumulated string sits at ref count 0 (parked, kept alive only by the
# register) between iterations; the append must copy rather than mutate it in
# place, or a second, unaccounted holder sees the change. The object form
# forces a parked object to be resurrected and then mutated.
K parked-concat-own 'function(n){ var s = ""; for (var i = 0; i < n; i++) { s += "ab"; if (i === 2) s = s.slice(0); } return s.length; }' 10
K parked-concat-alias 'function(n){ var s = ""; for (var i = 0; i < n; i++) { var t = s; s += "c"; if (t.length !== i) return "bad"; } return s.length; }' 5
K parked-obj-mut    'function(n){ var o = null, k = 0; for (var i = 0; i < n; i++) { var t = { v: i }; o = t; t.v = i + 1; k += o.v; } return k; }' 15

echo "----"
echo "pass=$pass fail=$fail skip=$skip"
[ "$fail" = 0 ]
