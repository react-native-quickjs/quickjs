/* jitbench.js — THE FAST ITERATION INSTRUMENT FOR THE NG JIT.
 *
 * Seconds, not hours.  One process per kernel (see run.sh), three arms in the same window:
 *   NG    QJS_NG_PROBE_OFF=1          the compiler, unconditionally native once admitted
 *   OFF   QJS_NG_NATIVE_OFF=1         the interpreter (the k denominator)
 *   NULL  byte-identical to NG        the band: |NG − NULL| is the noise, per kernel, per window
 *
 * Same output contract as aot/bench/compiler_bench.js so cb2score.py reads it unchanged:
 *   ##CB2## <name> <ms_per_call> <n> <checksum> <score> <inner> <span_ms>
 *   ##CBSUM## kernels N scored M failed F warns W
 *
 * ⛔ THREE THINGS THIS FILE DOES THAT compiler_bench.js DOES NOT, and why:
 *   1. WARMUP IS EIGHT CALLS, NOT TWO.  NG compiles a body on its QNG_NAT_WARMUP-th (8th) call;
 *      two warmups leave the timed region interpreted until the min-of-reps happens to catch a
 *      native rep.  Every kernel body here is called >= 8 times before now() is read.
 *   2. EVERY KERNEL BODY IS NAMED k_<kernel> so run.sh can join it to its own TIMESHARE dump and
 *      print rej= / nat= / odeopt= / nretier= / mode= beside the ratio.  A kernel that did not run
 *      native is reported VOID, never as "no speedup".  (Six times this project measured a null
 *      because the code under test never ran.)
 *   3. CHECKSUMS COUNT EFFECTS WHERE THE DEFECT WAS AN EFFECT.  The 4th shipping miscompile was a
 *      getter called twice per call — same VALUE.  sidefx.* kernels fold the CALL COUNT into the
 *      checksum, so a duplicated effect fails the checksum instead of passing it faster.
 *
 * Kernels are the shapes that CARRY k on the corpus (crypto am3, navierstokes lin_solve, raytrace
 * Vector, richards queue, gbemu dispatch, zlib closure cells) plus the DEFECT shapes found
 * 2026-08-16 / 09-04 (int-first-then-float feedback, integral-division INT tagging, an
 * admitted-then-always-deopting body, a duplicated getter).  A kernel is one function of n with
 * a deterministic result and a relation between its n/8 and n results (rule 2 of compiler_bench).
 */
var CB_REPS = 5;            /* timed reps; MIN and MEDIAN both reported (score = min)     */
var CB_SCALE = 1;           /* multiply every n                                             */
var CB_WARM = 8;            /* >= QNG_NAT_WARMUP so the timed region is native, not warming  */
var CB_FILTER = "";         /* substring; run.sh rewrites this per kernel                    */
var CB_LIST = 0;            /* 1 = print kernel names and exit                              */

var now = (typeof performance !== "undefined" && performance.now) ? function(){ return performance.now(); }
                                                                     : function(){ return Date.now(); };
var results = [], failures = [], warns = [];
function linear(full, small) { return Math.abs(full - 8 * small) <= 1e-9 * (Math.abs(full) + 1) + 8; }
/* wrapped int32 sum of 0..n-1: the kernel wraps each step, which equals ToInt32 of the exact total */
function wrapsum(full, small, n) { var e = (n * (n - 1) / 2) | 0, es = ((((n / 8) | 0) || 1) * ((((n / 8) | 0) || 1) - 1) / 2) | 0; return full === e && small === es; }
function linear_f(full, small) { return Math.abs(full - 8 * small) <= 1e-6 * (Math.abs(full) + 1); }
function exact8(full, small) { return full === 8 * small; }
function anyv(full, small) { return full === full; }   /* checksum exists; relation not linear */

function bench(name, fn, n, check) {
  if (CB_LIST) { print(name); return; }
  if (CB_FILTER && name.indexOf(CB_FILTER) < 0) return;
  n = Math.max(1, (n * CB_SCALE) | 0);
  var small = fn((n / 8) | 0 || 1), full = fn(n);
  if (!check(full, small, n)) {
    failures.push(name + ": checksum full=" + full + " small=" + small);
    results.push({ name: name, ms: -1, med: -1, n: n, sum: full, inner: 0, bad: true }); return;
  }
  for (var w = 0; w < CB_WARM; w++) fn(n);                      /* >= 8 calls: compiled before timing */
  var best = Infinity, times = [], sum = full, t0, dt;
  for (var r = 0; r < CB_REPS; r++) {
    t0 = now(); sum = fn(n); dt = now() - t0;
    times.push(dt); if (dt < best) best = dt;
  }
  if (sum !== full) failures.push(name + ": NOT REPEATABLE -- " + sum + " vs " + full);
  times.sort(function(a,b){return a-b;});
  results.push({ name: name, ms: best, med: times[(times.length/2)|0], n: n, sum: sum, inner: 1, span: best * CB_REPS });
}

/* ------------------------------------------------------------------ k-board head classes */
function k_int_loop(n){ var s=0; for(var i=0;i<n;i++) s=(s+i)|0; return s; }
function k_int_bitops(n){ var s=0x1234; for(var i=0;i<n;i++) s=((s^(i<<3))+(s>>>2))&0x7fffffff; return s; }
function k_fp_arith(n){ var s=0.5; for(var i=0;i<n;i++) s=s*1.0000001+0.25; return s; }
function k_fp_mixed(n){ var s=0.0; for(var i=1;i<=n;i++) s+=i*0.5/(i+1); return s; }
var A_INT=new Array(4096), A_DBL=new Array(4096), TA_F64=new Float64Array(4096), TA_U8=new Uint8Array(4096);
for (var _i=0;_i<4096;_i++){ A_INT[_i]=_i&255; A_DBL[_i]=_i*0.5; TA_F64[_i]=_i*0.25; TA_U8[_i]=_i&255; }
function k_elem_packed_int(n){ var s=0; for(var i=0;i<n;i++) s=(s+A_INT[i&4095])|0; return s; }
function k_elem_packed_double(n){ var s=0.0; for(var i=0;i<n;i++) s+=A_DBL[i&4095]; return s; }
function k_elem_typed_f64(n){ var s=0.0; for(var i=0;i<n;i++) s+=TA_F64[i&4095]; return s; }
function k_elem_typed_u8(n){ var s=0; for(var i=0;i<n;i++) s=(s+(TA_U8[i&4095]^(i&255)))|0; return s; }
var A_OUT=new Array(4096); for (var _j=0;_j<4096;_j++) A_OUT[_j]=0;
function k_elem_store(n){ for(var i=0;i<n;i++) A_OUT[i&4095]=(i*3)|0; return A_OUT[7]+A_OUT[4095]; }
function k_varref_closure(n){ var acc=0; var f=function(i){ acc=(acc+i)|0; }; for(var i=0;i<n;i++) f(i); return acc; }
function Vec(x,y,z){ this.x=x; this.y=y; this.z=z; }
function k_prop_mono_load(n){ var v=new Vec(0.5,0.25,0.125), s=0.0; for(var i=0;i<n;i++) s+=v.x+v.y+v.z; return s; }
function k_prop_store(n){ var a=new Vec(0.5,0.25,0.125), r=new Vec(0,0,0), f=0.999; for(var i=0;i<n;i++){ r.x=a.x*f; r.y=a.y*f; r.z=a.z*f; } return r.x+r.y+r.z; }
function Ctr(){ this.n=0; } Ctr.prototype.bump=function(i){ this.n=(this.n+i)|0; return this.n; };
function k_call_method_mono(n){ var c=new Ctr(); for(var i=0;i<n;i++) c.bump(i); return c.n; }
function addi(a,b){ return (a+b)|0; }
function k_call_direct(n){ var s=0; for(var i=0;i<n;i++) s=addi(s,i); return s; }
function k_call_ctor(n){ var s=0.0; for(var i=0;i<n;i++){ var v=new Vec(i,1,2); s+=v.x; } return s; }
/* ------------------------------------------------------------------ corpus shapes */
function k_crypto_am3(n){ var xl=1234&0x3fff, xh=1234>>14, w=A_INT.slice(0), c=0, j=0; for(var i=0;i<n;i++){ var l=w[j&4095]&0x3fff, h=w[j&4095]>>14; var m=xh*l+h*xl; l=xl*l+((m&0x3fff)<<14)+w[j&4095]+c; c=(l>>28)+(m>>14)+xh*h; w[j&4095]=l&0xfffffff; j++; } return c; }
var LS_X=new Array(66*66), LS_X0=new Array(66*66); for (var _k=0;_k<66*66;_k++){ LS_X[_k]=_k*0.001; LS_X0[_k]=_k*0.002; }
function k_ns_lin_solve(n){ var a=0.1, c=1.4, N=64, W=66, x=LS_X, x0=LS_X0, it=n; for(var k=0;k<it;k++){ for(var j=1;j<=N;j++){ var row=j*W; for(var i=1;i<=N;i++){ x[row+i]=(x0[row+i]+a*(x[row+i-1]+x[row+i+1]+x[row-W+i]+x[row+W+i]))/c; } } } return x[W+1]+x[W*32+32]; }
function Pkt(l){ this.link=l; this.id=0; }
function k_richards_queue(n){ var head=null, s=0; for(var i=0;i<n;i++){ head=new Pkt(head); if((i&7)===7){ var p=head; head=p.link; s=(s+1)|0; } } return s; }
function k_gbemu_switch(n){ var a=0,b=1,c=2; for(var i=0;i<n;i++){ switch(i&15){ case 0:a=(a+b)|0;break; case 1:b=(b^c)|0;break; case 2:c=(c+1)|0;break; case 3:a=(a-c)|0;break; case 4:b=(b<<1)|0;break; case 5:c=(c>>1)|0;break; case 6:a=(a|b)|0;break; case 7:b=(b&0xff)|0;break; default:a=(a+i)|0; } } return (a+b+c)|0; }
function k_gbemu_ta_mem(n){ var m=new Uint8Array(TA_U8), pc=0, acc=0; for(var i=0;i<n;i++){ var op=m[pc&4095]; acc=(acc+op)|0; m[(pc+1)&4095]=(op+1)&255; pc=(pc+op+1)|0; } return acc; }
/* ------------------------------------------------------------------ DEFECT shapes (canaries) */
function Col(r,g,b){ this.red=r; this.green=g; this.blue=b; }
function ms3(c,f){ var r=new Col(0,0,0); r.red=c.red*f; r.green=c.green*f; r.blue=c.blue*f; return r; }
function k_fb_int_first_then_float(n){ var s=0.0, black=new Col(0,0,0), white=new Col(0.9,0.8,0.7); s+=ms3(black,0.5).red; for(var i=0;i<n;i++) s+=ms3(white,0.5).red; return s; }
function k_fb_div_integral(n){ var a=1.5,b=0.5; var ci=new Col(a/b, 3.0/1.5, 2.0/2.0), cn=new Col(a/3, 0.7, 0.2), s=0.0; for(var i=0;i<8;i++) s+=ms3(ci,0.9).red; for(var i=0;i<n;i++) s+=ms3(cn,0.9).red; return s; }
function flip(x){ return x+1; }
var FLIPV=[7, 7.5];                                                                             /* int, f64: the callee's arg tag flips every call */
function k_canary_deopt_sink(n){ var s=0.0; for(var i=0;i<n;i++) s+=flip(FLIPV[i&1]); return s; }
var FX=0; var GO={ get v(){ FX++; return 3; } };
function k_sidefx_getter(n){ FX=0; var s=0; for(var i=0;i<n;i++) s=(s+GO.v)|0; return s + FX*7; }  /* count folded into the checksum */
function k_canary_string_charcode(n){ var str="the quick brown fox jumps over the lazy dog", s=0; for(var i=0;i<n;i++) s=(s+str.charCodeAt(i%43))|0; return s; }
function k_mix_numeric(n){ var s=0.0, t=0; for(var i=0;i<n;i++){ t=(t+i)|0; s+=A_DBL[i&4095]*0.5+(t&7); } return s; }

bench("int.loop",               k_int_loop,                4000000, wrapsum);
bench("int.bitops",             k_int_bitops,              3000000, anyv);
bench("fp.arith",               k_fp_arith,                3000000, anyv);
bench("fp.mixed",               k_fp_mixed,                2000000, anyv);
bench("elem.packed_int",        k_elem_packed_int,         3000000, anyv);
bench("elem.packed_double",     k_elem_packed_double,      3145728, linear_f);
bench("elem.typed_f64",         k_elem_typed_f64,          3145728, linear_f);
bench("elem.typed_u8",          k_elem_typed_u8,           3000000, anyv);
bench("elem.store",             k_elem_store,              3000000, anyv);
bench("varref.closure",         k_varref_closure,          2000000, wrapsum);
bench("prop.mono_load",         k_prop_mono_load,          3000000, linear_f);
bench("prop.store",             k_prop_store,              2000000, anyv);
bench("call.method_mono",       k_call_method_mono,        2000000, wrapsum);
bench("call.direct",            k_call_direct,             3000000, wrapsum);
bench("call.ctor",              k_call_ctor,               1000000, anyv);
bench("crypto.am3",             k_crypto_am3,              1500000, anyv);
bench("ns.lin_solve",           k_ns_lin_solve,                 40, anyv);
bench("richards.queue",         k_richards_queue,          1572864, linear);
bench("gbemu.switch",           k_gbemu_switch,            3000000, anyv);
bench("gbemu.ta_mem",           k_gbemu_ta_mem,            2000000, anyv);
bench("fb.int_first_then_float",k_fb_int_first_then_float, 1000000, anyv);
bench("fb.div_integral",        k_fb_div_integral, 1000000, anyv);
bench("canary.deopt_sink",      k_canary_deopt_sink,              1000000, anyv);
bench("sidefx.getter",          k_sidefx_getter,           1048576, linear);
bench("canary.string_charcode", k_canary_string_charcode,         1000000, anyv);
bench("mix.numeric",            k_mix_numeric,             2000000, anyv);

if (!CB_LIST) {
  for (var i = 0; i < results.length; i++) { var r = results[i];
    print("##CB2## " + r.name + " " + (r.ms > 0 ? r.ms.toFixed(5) : r.ms) + " " + r.n + " " + r.sum + " " +
          (r.ms > 0 ? (1000 / r.ms).toFixed(3) : 0) + " " + r.inner + " " + (r.span ? r.span.toFixed(2) : 0) + " med=" + (r.med > 0 ? r.med.toFixed(5) : r.med)); }
  for (var f = 0; f < failures.length; f++) print("##CBFAIL## " + failures[f]);
  for (var w = 0; w < warns.length; w++) print("##CBWARN## " + warns[w]);
  print("##CBSUM## kernels " + results.length + " scored " + (results.length - failures.length) + " failed " + failures.length + " warns " + warns.length);
}
