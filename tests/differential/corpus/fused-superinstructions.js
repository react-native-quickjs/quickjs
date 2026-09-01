function P(x){ print(String(x)); }
var log=[]; function V(n,v){this.n=n;this.v=v;}
V.prototype.valueOf=function(){log.push(this.n);return this.v;};
function BOOM(){} BOOM.prototype.valueOf=function(){throw new Error("boom");};
var O=[7,-3,0,2.5,-0.5,"4","x",true,false,null,undefined,NaN,Infinity,
       2147483647,-2147483648,1e10,{}];
function sh(v){ if(typeof v==="number"&&v===0&&1/v<0) return "-0";
                if(typeof v==="string") return JSON.stringify(v); return String(v); }
/* i8_sar / i8_add / imm8_bin(shl,or,sub) / imm16_bin / imm32_bin */
function f_sar(a){var l=a;return l>>3;}      function f_add(a){var l=a;return l+100;}
function f_shl(a){var l=a;return l<<3;}      function f_or (a){var l=a;return l|100;}
function f_sub(a){var l=a;return l-100;}     function f_and16(a){var l=a;return l&30000;}
function f_and32(a){var l=a;return l&1000000;}
/* loc_mul / frame_bin(loc_add, arg_add) */
function f_lmul(a,b){var l=b;return a*l;}    function f_ladd(a,b){var l=b;return a+l;}
function f_aadd(a,b){var l=a;return l+b;}
/* frame_un(inc, is_null) -- several source forms, the counters say which fire */
function f_inc(a){var l=a;return l++;}       function f_inc2(a){var l=a;var r=[];r[l++]=1;return l;}
function f_isn(a){var l=a;return l===null;}
/* this_put_loc0 / this_set_loc0 (non-strict AND strict) */
function C(){ var self=this; self.k=1; return self.k; }
function Cs(){ "use strict"; var self=this; return typeof self; }
/* field_chain (#9) */
function f_chain(o){var l=o;return l.a.b;}
var U=[f_sar,f_add,f_shl,f_or,f_sub,f_and16,f_and32,f_inc,f_isn];
var UN=["sar","add","shl","or","sub","and16","and32","inc","isn"];
var B=[f_lmul,f_ladd,f_aadd]; var BN=["lmul","ladd","aadd"];
var i,j,k;
for(k=0;k<U.length;k++) for(i=0;i<O.length;i++){var r;
  try{r=sh(U[k](O[i]));}catch(e){r="!"+e.name;} P(UN[k]+"("+sh(O[i])+")="+r);}
for(k=0;k<B.length;k++) for(i=0;i<O.length;i++) for(j=0;j<O.length;j++){var r2;
  try{r2=sh(B[k](O[i],O[j]));}catch(e){r2="!"+e.name;} P(BN[k]+"("+sh(O[i])+","+sh(O[j])+")="+r2);}
for(k=0;k<B.length;k++){log=[];try{B[k](new V("first",3),new V("second",5));}catch(e){}
  P(BN[k]+" order="+log.join(","));}
for(k=0;k<B.length;k++){try{B[k](new BOOM(),3);P(BN[k]+" lhs=NO");}catch(e){P(BN[k]+" lhs="+e.message);}
  try{B[k](3,new BOOM());P(BN[k]+" rhs=NO");}catch(e){P(BN[k]+" rhs="+e.message);}}
for(k=0;k<B.length;k++){try{P(BN[k]+" big="+B[k](6n,3n));}catch(e){P(BN[k]+" big=!"+e.name);}
  try{P(BN[k]+" mix="+B[k](6n,3));}catch(e){P(BN[k]+" mix=!"+e.name);}
  try{P(BN[k]+" sym="+B[k](Symbol("s"),3));}catch(e){P(BN[k]+" sym=!"+e.name);}}
P(f_ladd("a","b")); P(f_aadd("a","b")); P(f_add("a"));
P(sh(f_lmul(2000000000,2000000000))); P(sh(f_lmul(-1,0))); P(sh(f_lmul(0,-1)));
P(sh(f_ladd(2147483647,1))); P(sh(f_aadd(2147483647,1))); P(sh(f_add(2147483647)));
P(sh(f_sub(-2147483648)));
/* `this` in every binding: object, primitive, null/undefined, strict */
P(C.call({})); P(C.call(null)); P(C.call(undefined));
try{P(C.call(5));}catch(e){P("!"+e.name);}
try{P(C.call("s"));}catch(e){P("!"+e.name);}
P(Cs.call({})); P(Cs.call(null)); P(Cs.call(5)); P(Cs.call(undefined));
P(f_inc(5)); P(f_inc(2.5)); P(f_inc("7")); P(f_inc2(3));
P(f_isn(null)); P(f_isn(undefined)); P(f_isn(0));
P(f_chain({a:{b:42}})); P(f_chain({a:{b:"x"}}));
try{P(f_chain({}));}catch(e){P("!"+e.name);}
function hot(){var s=0,t=0,i;for(i=0;i<2000;i++){var o={v:i};
  s=f_ladd(s,o.v); s=f_and16(s); t=f_aadd(t,1); t=f_sar(t); t=f_or(t);}return s+":"+t;}
P(hot());
/* `this` captured with this_var_idx == 0 (no named local precedes it), which is
   the only shape the prologue fusion fires on.  An arrow forces the capture. */
function T0(){ return (() => this)(); }
function T1(){ return (() => typeof this)(); }
function T2(){ "use strict"; return (() => this)(); }
function T3(){ return (() => this.constructor === Object)(); }
var G = (function(){ return this; })();
P(T0.call({a:1}).a); P(String(T0.call(null) === G || T0.call(null) === globalThis));
P(String(T0.call(undefined) === G || T0.call(undefined) === globalThis));
P(typeof T0.call(5)); P(typeof T0.call("s")); P(typeof T0.call(true));
P(T1.call(5)); P(T1.call(null)); P(T1.call({}));
P(String(T2.call(null))); P(String(T2.call(5))); P(String(T2.call(undefined)));
P(T3.call({})); P(T3.call(5));
function T4(){ var r=[]; for (var i=0;i<3;i++) r.push((() => this)()); return r.length; }
P(T4.call({})); P(T4.call(7));
/* batch 2: frame-receiver property fusions */
function pg2(o){ var l=o; return l.m(); }          /* loc_get_field2_ic */
function ag2(o){ return o.m(); }                    /* arg_get_field2_ic */
function ppf(o,v){ var l=o; l.x=v; return l.x; }    /* loc_put_field_ic  */
function apf(o,v){ o.x=v; return o.x; }             /* arg_put_field_ic  */
var proto={m:function(){return "proto:"+this.tag;}};
function mk(t){ var o=Object.create(proto); o.tag=t; return o; }
P(pg2({m:function(){return "own";},tag:1})); P(pg2(mk("A"))); P(ag2(mk("B")));
var deep=Object.create(Object.create(proto)); deep.tag="D"; P(pg2(deep)); P(ag2(deep));
P(ppf({},5)); P(apf({},6)); P(ppf(Object.create(null),7));
var sealed=Object.seal({x:1}); P(ppf(sealed,9)); P(apf(sealed,10));
var acc={set x(v){this._v=v*2;},get x(){return this._v;}}; P(ppf(acc,4)); P(apf(acc,5));
try{ P(pg2(null)); }catch(e){ P("!"+e.name); }
try{ P(ag2(undefined)); }catch(e){ P("!"+e.name); }
try{ P(ppf(null,1)); }catch(e){ P("!"+e.name); }
try{ P(apf(undefined,1)); }catch(e){ P("!"+e.name); }
try{ P(pg2(5)); }catch(e){ P("!"+e.name); }
P(ppf("str",1)); P(apf(42,2));
var thrower={ get m(){ throw new Error("g"); }, set x(v){ throw new Error("s"); } };
try{ pg2(thrower); }catch(e){ P("caught "+e.message); }
try{ ppf(thrower,1); }catch(e){ P("caught "+e.message); }
function hotp(){ var s=0,i; for(i=0;i<300;i++){ var o=mk(i); s+=ag2(o).length; apf(o,i); s+=o.x; } return s; }
P(hotp());
/* constructor field init: push_this, get_arg(n), put_field -- the shape the
   histogram actually shows for `get_arg0 -> put_field_ic` (earleyboyer 3.2%) */
function Ctor(a,b){ this.p = a; this.q = b; }
function ctorLoc(a){ var l = a; this.p = l; }
var c1=new Ctor(1,"two"); P(c1.p+","+c1.q);
var c2=new Ctor(null,undefined); P(String(c2.p)+","+String(c2.q));
P(new (function(a){ this.p = a; })({z:1}).p.z);
var C3=function(a){ this.p=a; }; C3.prototype={set p(v){ this._s=v*3; },get p(){return this._s;}};
P(new C3(4).p);
var C4=function(a){ this.p=a; }; C4.prototype={set p(v){ throw new Error("cs"); }};
try{ new C4(1); }catch(e){ P("caught "+e.message); }
function hotc(){ var s=0,i; for(i=0;i<300;i++){ var o=new Ctor(i,i+1); s+=o.p+o.q; } return s; }
P(hotc());
/* RECEIVER vs HOLDER: spec-visible through a Proxy on the prototype chain.
   OP_get_field2_ic's own comment records this -- the walk reassigns `obj` to
   the exotic object it stopped on, so the receiver must be kept separately or
   the trap sees the PROTOTYPE where V8 passes the child. */
var ptrap = new Proxy({}, { get: function(t,k,recv){ return "recv:"+(recv && recv.tag); } });
function viaLoc(o){ var l=o; return l.anything(); }
function viaArg(o){ return o.anything(); }
var kid = Object.create(ptrap); kid.tag = "KID";
try { P(String(kid.anything)); } catch(e) { P("!"+e.name); }
var ptrap2 = new Proxy({}, { get: function(t,k,recv){ return function(){ return "R:"+(recv&&recv.tag); }; } });
var kid2 = Object.create(ptrap2); kid2.tag = "KID2";
P(viaLoc(kid2)); P(viaArg(kid2));
var gp = Object.create(Object.create(ptrap2)); gp.tag = "GP";
P(viaLoc(gp)); P(viaArg(gp));
/* setter side: a proxy prototype whose trap writes to a DIFFERENT key, so the
   store does not re-enter the trap (recv.seen = ... on the same chain would
   recurse forever -- in node too). */
var sslot = {};
var strap = new Proxy({}, { set: function(t,k,v,recv){ sslot.seen = k+"="+v; return true; } });
function Cp(a){ this.zz = a; }
Cp.prototype = Object.create(strap);
var cp = new Cp("V"); P(String(sslot.seen)+"/"+String(cp.zz));
/* the nine zero-slot subop additions: loc_{sub,or,div}, arg_mul,
   i8_{and,div,mul}, i16_{mod,add} */
function q_lsub(a,b){ var l=b; return a-l; }
function q_lor (a,b){ var l=b; return a|l; }
function q_ldiv(a,b){ var l=b; return a/l; }
function q_amul(a,b){ var l=a; return l*b; }
function q_iand(a){ var l=a; return l&100; }
function q_idiv(a){ var l=a; return l/100; }
function q_imul(a){ var l=a; return l*100; }
function q_imod(a){ var l=a; return l%30000; }
function q_iadd(a){ var l=a; return l+30000; }
var QU=[q_iand,q_idiv,q_imul,q_imod,q_iadd];
var QUN=["iand","idiv","imul","imod","iadd"];
var QB=[q_lsub,q_lor,q_ldiv,q_amul]; var QBN=["lsub","lor","ldiv","amul"];
var i,j,k;
for(k=0;k<QU.length;k++) for(i=0;i<O.length;i++){var r;
  try{r=sh(QU[k](O[i]));}catch(e){r="!"+e.name;} P(QUN[k]+"("+sh(O[i])+")="+r);}
for(k=0;k<QB.length;k++) for(i=0;i<O.length;i++) for(j=0;j<O.length;j++){var r2;
  try{r2=sh(QB[k](O[i],O[j]));}catch(e){r2="!"+e.name;} P(QBN[k]+"("+sh(O[i])+","+sh(O[j])+")="+r2);}
for(k=0;k<QB.length;k++){log=[];try{QB[k](new V("first",6),new V("second",3));}catch(e){}
  P(QBN[k]+" order="+log.join(","));}
for(k=0;k<QB.length;k++){try{QB[k](new BOOM(),3);P(QBN[k]+" lhs=NO");}catch(e){P(QBN[k]+" lhs="+e.message);}
  try{QB[k](3,new BOOM());P(QBN[k]+" rhs=NO");}catch(e){P(QBN[k]+" rhs="+e.message);}
  try{P(QBN[k]+" big="+QB[k](6n,3n));}catch(e){P(QBN[k]+" big=!"+e.name);}}
/* mod corners: the int fast path is only valid for v1>=0, v2>0 */
P(sh(q_imod(-7))); P(sh(q_imod(7))); P(sh(-2147483648 % -1)); P(sh(q_ldiv(1,0)));
P(sh(q_ldiv(-1,0))); P(sh(q_ldiv(0,0))); P(sh(q_imul(2147483647)));
function qhot(){var s=0,i;for(i=0;i<400;i++){s=q_iand(s+i);s=q_imod(s+7);s=q_lsub(s,1);s=q_lor(s,0);}return s;}
P(qhot());
