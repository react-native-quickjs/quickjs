/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * native-call-bench -- a fixed-work microbenchmark over every shape of
 * JavaScript -> C call QuickJS supports, plus JS -> JS controls at matching
 * arity and an empty-loop control.
 *
 *   native-call-bench <shape> <iterations>
 *
 * Runs exactly `iterations` calls of `shape` inside one hoisted-local loop and
 * exits. The caller (tools/benchmark) runs each shape at two iteration counts
 * N1 < N2 and takes the SLOPE
 *
 *     instructions_per_iteration = (I(N2) - I(N1)) / (N2 - N1)
 *
 * which cancels process startup, engine boot, parse, and every page fault
 * that does not scale with N. The `empty` shape is the loop-overhead control.
 *
 * Each process exit prints one `##CALL## <name> <ns_per_iter> <n>` line; the
 * runner dedupes by name and reports the median of N processes.
 *
 *   native-call-bench --list    print the known shape names
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs.h"

static JSValue c_gen(
    JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
  (void)ctx;
  (void)t;
  (void)argc;
  (void)argv;
  return JS_UNDEFINED;
}
static JSValue c_gen_magic(
    JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv, int magic) {
  (void)ctx;
  (void)t;
  (void)argc;
  (void)argv;
  (void)magic;
  return JS_UNDEFINED;
}
static JSValue c_data(
    JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv, int magic,
    JSValue *data) {
  (void)ctx;
  (void)t;
  (void)argc;
  (void)argv;
  (void)magic;
  (void)data;
  return JS_UNDEFINED;
}
static JSValue c_getter(JSContext *ctx, JSValueConst t) {
  (void)ctx;
  (void)t;
  return JS_UNDEFINED;
}
static JSValue c_setter(JSContext *ctx, JSValueConst t, JSValueConst v) {
  (void)ctx;
  (void)t;
  (void)v;
  return JS_UNDEFINED;
}
static JSValue c_getter_magic(JSContext *ctx, JSValueConst t, int magic) {
  (void)ctx;
  (void)t;
  (void)magic;
  return JS_UNDEFINED;
}
static JSValue c_setter_magic(
    JSContext *ctx, JSValueConst t, JSValueConst v, int magic) {
  (void)ctx;
  (void)t;
  (void)v;
  (void)magic;
  return JS_UNDEFINED;
}
static double c_ff(double a) {
  return a;
}
static double c_fff(double a, double b) {
  return a + b;
}

static JSValue c_ctor(
    JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
  (void)argc;
  (void)argv;
  return JS_NewObjectProto(ctx, JS_UNDEFINED);
  (void)nt;
}

static JSValue c_ret_int(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)t;
  (void)n;
  (void)a;
  return JS_NewInt32(c, 42);
}
static JSValue c_ret_dbl(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)t;
  (void)n;
  (void)a;
  return JS_NewFloat64(c, 3.5);
}
static JSValue c_ret_str_new(
    JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)t;
  (void)n;
  (void)a;
  return JS_NewString(c, "hello");
}
static JSValue cached_str;
static JSValue c_ret_str_dup(
    JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)t;
  (void)n;
  (void)a;
  return JS_DupValue(c, cached_str);
}
static JSValue c_ret_obj(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)t;
  (void)n;
  (void)a;
  return JS_NewObject(c);
}

static JSValue c_arg_int(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  int32_t v = 0;
  (void)t;
  if (n > 0) JS_ToInt32(c, &v, a[0]);
  return JS_NewInt32(c, v);
}
static JSValue c_arg_dbl(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  double v = 0;
  (void)t;
  if (n > 0) JS_ToFloat64(c, &v, a[0]);
  return JS_NewFloat64(c, v);
}
static JSValue c_arg_str(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)t;
  (void)n;
  (void)c;
  if (n > 0) {
    const char *s = JS_ToCString(c, a[0]);
    if (s) JS_FreeCString(c, s);
  }
  return JS_UNDEFINED;
}

static const char *KNOWN[] = {
    "empty",       "jsjs0",     "jsjs1",    "jsjs2",      "jsjs4",
    "jsjs8",       "gen0",      "gen1",     "gen2",       "gen4",
    "gen8",        "pad4x1",    "pad8x1",   "pad8x0",     "over0x4",
    "data0",       "data1",     "data4",    "datapad4x1", "magic1",
    "f_f",         "f_f_f",     "getter",   "setter",     "gettermagic",
    "settermagic", "jsgetter",  "jssetter", "ctor",       "jsctor",
    "ret_undef",   "ret_int",   "ret_dbl",  "ret_bool",   "ret_strdup",
    "ret_strnew",  "ret_obj",   "arg_int",  "arg_dbl",    "arg_str",
    "apply1",      "apply4",    "apply8",   "jsapply4",   "spread0",
    "spread1",     "spread4",   "spread8",  "applyargs4", "applygen4",
    "applyacc4",   "jsspread4", NULL};

static int is_known(const char *n) {
  for (int i = 0; KNOWN[i]; i++)
    if (!strcmp(KNOWN[i], n)) return 1;
  return 0;
}

static JSValue js_print(
    JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
  (void)t;
  for (int i = 0; i < argc; i++) {
    const char *s = JS_ToCString(ctx, argv[i]);
    printf("%s%s", i ? " " : "", s ? s : "");
    if (s) JS_FreeCString(ctx, s);
  }
  printf("\n");
  return JS_UNDEFINED;
}

#include <time.h>
static double now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

int main(int argc, char **argv) {
  if (argc >= 2 && !strcmp(argv[1], "--list")) {
    for (int i = 0; KNOWN[i]; i++) printf("%s\n", KNOWN[i]);
    return 0;
  }
  if (argc < 3) {
    fprintf(stderr, "usage: %s <shape> <iterations>  (or --list)\n", argv[0]);
    return 2;
  }
  const char *shape = argv[1];
  long long n = atoll(argv[2]);
  if (!is_known(shape) && strcmp(shape, "empty")) {
    fprintf(stderr, "unknown shape %s\n", shape);
    return 2;
  }

  JSRuntime *rt = JS_NewRuntime();
  JSContext *ctx = JS_NewContext(rt);
  JSValue g = JS_GetGlobalObject(ctx);
  cached_str = JS_NewString(ctx, "cached");
  JS_SetPropertyStr(
      ctx, g, "print", JS_NewCFunction(ctx, js_print, "print", 1));
  JS_SetPropertyStr(
      ctx, g, "c0", JS_NewCFunction2(ctx, c_gen, "c0", 0, JS_CFUNC_generic, 0));
  JS_SetPropertyStr(
      ctx, g, "c1", JS_NewCFunction2(ctx, c_gen, "c1", 1, JS_CFUNC_generic, 0));
  JS_SetPropertyStr(
      ctx, g, "c2", JS_NewCFunction2(ctx, c_gen, "c2", 2, JS_CFUNC_generic, 0));
  JS_SetPropertyStr(
      ctx, g, "c4", JS_NewCFunction2(ctx, c_gen, "c4", 4, JS_CFUNC_generic, 0));
  JS_SetPropertyStr(
      ctx, g, "c8", JS_NewCFunction2(ctx, c_gen, "c8", 8, JS_CFUNC_generic, 0));
  JSValue dat = JS_NewInt32(ctx, 1);
  JS_SetPropertyStr(
      ctx, g, "d0", JS_NewCFunctionData(ctx, c_data, 0, 0, 1, &dat));
  JS_SetPropertyStr(
      ctx, g, "d1", JS_NewCFunctionData(ctx, c_data, 1, 0, 1, &dat));
  JS_SetPropertyStr(
      ctx, g, "d4", JS_NewCFunctionData(ctx, c_data, 4, 0, 1, &dat));
  JS_SetPropertyStr(
      ctx, g, "m1",
      JS_NewCFunctionMagic(
          ctx, c_gen_magic, "m1", 1, JS_CFUNC_generic_magic, 7));
  {
    JSCFunctionType ft;
    ft.f_f = c_ff;
    JS_SetPropertyStr(
        ctx, g, "ff",
        JS_NewCFunction2(ctx, ft.generic, "ff", 1, JS_CFUNC_f_f, 0));
    ft.f_f_f = c_fff;
    JS_SetPropertyStr(
        ctx, g, "fff",
        JS_NewCFunction2(ctx, ft.generic, "fff", 2, JS_CFUNC_f_f_f, 0));
  }
  {
    JSValue o = JS_NewObject(ctx);
    JSAtom p = JS_NewAtom(ctx, "p");
    JS_DefinePropertyGetSet(
        ctx, o, p,
        JS_NewCFunction2(
            ctx, (JSCFunction *)(void *)c_getter, "get p", 0, JS_CFUNC_getter,
            0),
        JS_NewCFunction2(
            ctx, (JSCFunction *)(void *)c_setter, "set p", 1, JS_CFUNC_setter,
            0),
        JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, p);
    JS_SetPropertyStr(ctx, g, "acc", o);
    JSValue om = JS_NewObject(ctx);
    JSAtom pm = JS_NewAtom(ctx, "p");
    JS_DefinePropertyGetSet(
        ctx, om, pm,
        JS_NewCFunction2(
            ctx, (JSCFunction *)(void *)c_getter_magic, "get p", 0,
            JS_CFUNC_getter_magic, 3),
        JS_NewCFunction2(
            ctx, (JSCFunction *)(void *)c_setter_magic, "set p", 1,
            JS_CFUNC_setter_magic, 3),
        JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, pm);
    JS_SetPropertyStr(ctx, g, "accm", om);
  }
  JS_SetPropertyStr(
      ctx, g, "ctor",
      JS_NewCFunction2(ctx, c_ctor, "ctor", 0, JS_CFUNC_constructor, 0));
  JS_SetPropertyStr(
      ctx, g, "rundef",
      JS_NewCFunction2(ctx, c_gen, "rundef", 0, JS_CFUNC_generic, 0));
  JS_SetPropertyStr(
      ctx, g, "rint",
      JS_NewCFunction2(ctx, c_ret_int, "rint", 0, JS_CFUNC_generic, 0));
  JS_SetPropertyStr(
      ctx, g, "rdbl",
      JS_NewCFunction2(ctx, c_ret_dbl, "rdbl", 0, JS_CFUNC_generic, 0));
  JS_SetPropertyStr(
      ctx, g, "rbool",
      JS_NewCFunction2(ctx, c_gen, "rbool", 0, JS_CFUNC_generic, 0));
  JS_SetPropertyStr(
      ctx, g, "rstrdup",
      JS_NewCFunction2(ctx, c_ret_str_dup, "rstrdup", 0, JS_CFUNC_generic, 0));
  JS_SetPropertyStr(
      ctx, g, "rstrnew",
      JS_NewCFunction2(ctx, c_ret_str_new, "rstrnew", 0, JS_CFUNC_generic, 0));
  JS_SetPropertyStr(
      ctx, g, "robj",
      JS_NewCFunction2(ctx, c_ret_obj, "robj", 0, JS_CFUNC_generic, 0));
  JS_SetPropertyStr(
      ctx, g, "aint",
      JS_NewCFunction2(ctx, c_arg_int, "aint", 1, JS_CFUNC_generic, 0));
  JS_SetPropertyStr(
      ctx, g, "adbl",
      JS_NewCFunction2(ctx, c_arg_dbl, "adbl", 1, JS_CFUNC_generic, 0));
  JS_SetPropertyStr(
      ctx, g, "astr",
      JS_NewCFunction2(ctx, c_arg_str, "astr", 1, JS_CFUNC_generic, 0));
  JS_FreeValue(ctx, g);

  const char *setup;
  const char *body;
  if (!strcmp(shape, "empty")) {
    setup = "";
    body = "";
  } else if (!strcmp(shape, "jsjs0")) {
    setup = "var f=g.js0;";
    body = "f();";
  } else if (!strcmp(shape, "jsjs1")) {
    setup = "var f=g.js1;";
    body = "f(1);";
  } else if (!strcmp(shape, "jsjs2")) {
    setup = "var f=g.js2;";
    body = "f(1,2);";
  } else if (!strcmp(shape, "jsjs4")) {
    setup = "var f=g.js4;";
    body = "f(1,2,3,4);";
  } else if (!strcmp(shape, "jsjs8")) {
    setup = "var f=g.js8;";
    body = "f(1,2,3,4,5,6,7,8);";
  } else if (!strcmp(shape, "gen0")) {
    setup = "var f=g.c0;";
    body = "f();";
  } else if (!strcmp(shape, "gen1")) {
    setup = "var f=g.c1;";
    body = "f(1);";
  } else if (!strcmp(shape, "gen2")) {
    setup = "var f=g.c2;";
    body = "f(1,2);";
  } else if (!strcmp(shape, "gen4")) {
    setup = "var f=g.c4;";
    body = "f(1,2,3,4);";
  } else if (!strcmp(shape, "gen8")) {
    setup = "var f=g.c8;";
    body = "f(1,2,3,4,5,6,7,8);";
  } else if (!strcmp(shape, "pad4x1")) {
    setup = "var f=g.c4;";
    body = "f(1);";
  } else if (!strcmp(shape, "pad8x1")) {
    setup = "var f=g.c8;";
    body = "f(1);";
  } else if (!strcmp(shape, "pad8x0")) {
    setup = "var f=g.c8;";
    body = "f();";
  } else if (!strcmp(shape, "over0x4")) {
    setup = "var f=g.c0;";
    body = "f(1,2,3,4);";
  } else if (!strcmp(shape, "data0")) {
    setup = "var f=g.d0;";
    body = "f();";
  } else if (!strcmp(shape, "data1")) {
    setup = "var f=g.d1;";
    body = "f(1);";
  } else if (!strcmp(shape, "data4")) {
    setup = "var f=g.d4;";
    body = "f(1,2,3,4);";
  } else if (!strcmp(shape, "datapad4x1")) {
    setup = "var f=g.d4;";
    body = "f(1);";
  } else if (!strcmp(shape, "magic1")) {
    setup = "var f=g.m1;";
    body = "f(1);";
  } else if (!strcmp(shape, "f_f")) {
    setup = "var f=g.ff;";
    body = "f(1.5);";
  } else if (!strcmp(shape, "f_f_f")) {
    setup = "var f=g.fff;";
    body = "f(1.5,2.5);";
  } else if (!strcmp(shape, "getter")) {
    setup = "var o=g.acc;var s=0;";
    body = "s+=o.p;";
  } else if (!strcmp(shape, "setter")) {
    setup = "var o=g.acc;";
    body = "o.p=1;";
  } else if (!strcmp(shape, "gettermagic")) {
    setup = "var o=g.accm;var s=0;";
    body = "s+=o.p;";
  } else if (!strcmp(shape, "settermagic")) {
    setup = "var o=g.accm;";
    body = "o.p=1;";
  } else if (!strcmp(shape, "jsgetter")) {
    setup = "var o=g.jsacc;var s=0;";
    body = "s+=o.p;";
  } else if (!strcmp(shape, "jssetter")) {
    setup = "var o=g.jsacc;";
    body = "o.p=1;";
  } else if (!strcmp(shape, "ctor")) {
    setup = "var f=g.ctor;";
    body = "new f();";
  } else if (!strcmp(shape, "jsctor")) {
    setup = "var f=g.jsctor;";
    body = "new f();";
  } else if (!strcmp(shape, "ret_undef")) {
    setup = "var f=g.rundef;";
    body = "f();";
  } else if (!strcmp(shape, "ret_int")) {
    setup = "var f=g.rint;";
    body = "f();";
  } else if (!strcmp(shape, "ret_dbl")) {
    setup = "var f=g.rdbl;";
    body = "f();";
  } else if (!strcmp(shape, "ret_bool")) {
    setup = "var f=g.rbool;";
    body = "f();";
  } else if (!strcmp(shape, "ret_strdup")) {
    setup = "var f=g.rstrdup;";
    body = "f();";
  } else if (!strcmp(shape, "ret_strnew")) {
    setup = "var f=g.rstrnew;";
    body = "f();";
  } else if (!strcmp(shape, "ret_obj")) {
    setup = "var f=g.robj;";
    body = "f();";
  } else if (!strcmp(shape, "arg_int")) {
    setup = "var f=g.aint;";
    body = "f(7);";
  } else if (!strcmp(shape, "arg_dbl")) {
    setup = "var f=g.adbl;";
    body = "f(7.5);";
  } else if (!strcmp(shape, "arg_str")) {
    setup = "var f=g.astr;var s='abcdefgh';";
    body = "f(s);";
  } else if (!strcmp(shape, "apply1")) {
    setup = "var f=g.c1;var A=[1];";
    body = "f.apply(null,A);";
  } else if (!strcmp(shape, "apply4")) {
    setup = "var f=g.c4;var A=[1,2,3,4];";
    body = "f.apply(null,A);";
  } else if (!strcmp(shape, "apply8")) {
    setup = "var f=g.c8;var A=[1,2,3,4,5,6,7,8];";
    body = "f.apply(null,A);";
  } else if (!strcmp(shape, "jsapply4")) {
    setup = "var f=g.js4;var A=[1,2,3,4];";
    body = "f.apply(null,A);";
  } else if (!strcmp(shape, "spread0")) {
    setup = "var f=g.c0;var A=[];";
    body = "f(...A);";
  } else if (!strcmp(shape, "spread1")) {
    setup = "var f=g.c1;var A=[1];";
    body = "f(...A);";
  } else if (!strcmp(shape, "spread4")) {
    setup = "var f=g.c4;var A=[1,2,3,4];";
    body = "f(...A);";
  } else if (!strcmp(shape, "spread8")) {
    setup = "var f=g.c8;var A=[1,2,3,4,5,6,7,8];";
    body = "f(...A);";
  } else if (!strcmp(shape, "applyargs4")) {
    setup =
        "var f=g.c4;var w=function(a,b,c,d){ return f.apply(null, arguments); "
        "};";
    body = "w(1,2,3,4);";
  } else if (!strcmp(shape, "applygen4")) {
    setup = "var f=g.c4;var O={0:1,1:2,2:3,3:4,length:4};";
    body = "f.apply(null,O);";
  } else if (!strcmp(shape, "applyacc4")) {
    setup =
        "var f=g.c4;var O={length:4};"
        "for (var q=0;q<4;q++) (function(k){"
        "  Object.defineProperty(O,k,{get:function(){return k;}});})(q);";
    body = "f.apply(null,O);";
  } else if (!strcmp(shape, "jsspread4")) {
    setup = "var f=g.js4;var A=[1,2,3,4];";
    body = "f(...A);";
  } else {
    fprintf(stderr, "shape %s not implemented in driver\n", shape);
    return 2;
  }

  const char *prelude =
      "var g = globalThis;\n"
      "g.js0 = function(){};\n"
      "g.js1 = function(a){};\n"
      "g.js2 = function(a,b){};\n"
      "g.js4 = function(a,b,c,d){};\n"
      "g.js8 = function(a,b,c,d,e,f,h,i){};\n"
      "g.jsctor = function(){};\n"
      "g.jsacc = (function(){ var o={};"
      "  Object.defineProperty(o,'p',{get:function(){return undefined;},"
      "                               set:function(v){},configurable:true}); "
      "  return o; })();\n";

  char src[8192];
  snprintf(
      src, sizeof(src),
      "%s(function(){ %s var i; for (i = 0; i < %lld; i++) { %s } })();\n",
      prelude, setup, n, body);

  double t0 = now_ms();
  JSValue r = JS_Eval(ctx, src, strlen(src), "<bench>", JS_EVAL_TYPE_GLOBAL);
  double dt = now_ms() - t0;
  int rc = 0;
  if (JS_IsException(r)) {
    rc = 1;
  }
  JS_FreeValue(ctx, r);

  double ns_per_iter = n > 0 ? (dt * 1e6) / (double)n : 0;
  printf("##CALL## %s %.3f %lld\n", shape, ns_per_iter, n);
  fflush(stdout);
  _Exit(rc);
}