/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * Benchmark driver: runs one workload/octane row/kernel file through the
 * patched engine and provides `print`.
 *
 * `print` and nothing else, deliberately. The same source is run by this
 * binary, by Hermes's driver and by node, so anything exposed here that the
 * others do not have is a way for the two to disagree for a reason unrelated
 * to the engine.
 *
 * Accepts either JavaScript source or an NSBCNGS bytecode container from qjsc,
 * so a workload can be measured from source and from AOT bytecode -- and they
 * are not the same path.
 *
 *   qjs-bench [--mem] [--stats] [--bytecode-mode=<mode>] <file.js|file.bc>
 *
 * Bytecode modes: eager, lazy-copy, lazy-borrow. Borrowed mode is safe here
 * because the input buffer remains allocated until the runtime is destroyed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "quickjs.h"

#define NSBC_MAGIC "NSBCNGS"
#define NSBC_HEADER_SIZE 12

static JSValue js_print(
    JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  for (int i = 0; i < argc; i++) {
    const char *s = JS_ToCString(ctx, argv[i]);
    printf("%s%s", i ? " " : "", s ? s : "");
    if (s) JS_FreeCString(ctx, s);
  }
  printf("\n");
  return JS_UNDEFINED;
}

/* Runs the cycle collector: the cases that need to know an object graph is freed or kept. */
#ifdef QJS_KIT_HTMLDDA
/* Kit only: an object that compares equal to null, the host's way of making one. */
static JSValue js_make_htmldda(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  JSValue f = JS_NewCFunction(ctx, js_print, "htmldda", 0);
  JS_SetIsHTMLDDA(ctx, f);
  return f;
}
#endif
static JSValue js_force_gc(
    JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  JS_RunGC(JS_GetRuntime(ctx));
  return JS_UNDEFINED;
}

/* A trivial host (C) function for the aot_bench.js "host function in loop"
   kernel; the AOT compiler must handle a native callee like any other. */
static JSValue js_host_add(
    JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  int32_t a = 0, b = 0;
  if (argc > 0) JS_ToInt32(ctx, &a, argv[0]);
  if (argc > 1) JS_ToInt32(ctx, &b, argv[1]);
  return JS_NewInt32(ctx, a + b);
}

/* Deliberately reads its second formal parameter even when the caller did not
   pass one, so an unpadded call observes stale register data. Used only by the
   AOT arity kernel to gate AOT_MUT_NO_ARG_PAD. */
static JSValue js_host_opt(
    JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  int32_t a = 0, b = 0;
  if (argc > 0) JS_ToInt32(ctx, &a, argv[0]);
  JS_ToInt32(ctx, &b, argv[1]);
  return JS_NewInt32(ctx, a + b);
}

static void dump_exception(JSContext *ctx) {
  JSValue e = JS_GetException(ctx);
  const char *s = JS_ToCString(ctx, e);
  fprintf(stderr, "uncaught: %s\n", s ? s : "(unprintable)");
  if (s) JS_FreeCString(ctx, s);
  JSValue stack = JS_GetPropertyStr(ctx, e, "stack");
  if (!JS_IsUndefined(stack)) {
    const char *st = JS_ToCString(ctx, stack);
    if (st) {
      fprintf(stderr, "%s\n", st);
      JS_FreeCString(ctx, st);
    }
  }
  JS_FreeValue(ctx, stack);
  JS_FreeValue(ctx, e);
}

static uint8_t *read_file(const char *path, size_t *out_size) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size < 0) {
    fclose(f);
    return NULL;
  }
  uint8_t *buf = malloc((size_t)size + 1);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  if (size > 0 && fread(buf, 1, (size_t)size, f) != (size_t)size) {
    fclose(f);
    free(buf);
    return NULL;
  }
  buf[size] = 0;
  fclose(f);
  *out_size = (size_t)size;
  return buf;
}

static double now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* QJS_BENCH_INTERRUPT=N: throw on the Nth interrupt, counting down, so a
   script's poll path is exercised and can be compared between the interpreter
   and a unit. The exception is the ordinary "interrupted" error, which the
   script sees as a throw. */
static int bench_interrupt_left;

/* QJS_BENCH_IRQ_HEAP=N: every Nth poll the handler writes the heap through the C API, the way a host callback can: the object registered
   with host_irq_target(obj) gets copies of its `child` and `arr` (the same values in new objects, so the program's result does not depend on
   how often the engine polls) and the collector runs. What the interrupted code had loaded from the old ones must stay alive. */
static JSContext *bench_irq_ctx;
static JSValue bench_irq_obj = JS_UNDEFINED;
static int bench_irq_period, bench_irq_count;

static int bench_interrupt_cb(JSRuntime *rt, void *opaque) {
  (void)opaque;
  if (bench_irq_period > 0 && ++bench_irq_count % bench_irq_period == 0 && !JS_IsUndefined(bench_irq_obj)) {
    JSContext *c = bench_irq_ctx;
    JSValue oc = JS_GetPropertyStr(c, bench_irq_obj, "child"), oa = JS_GetPropertyStr(c, bench_irq_obj, "arr");
    JSValue nc = JS_NewObject(c), na = JS_NewArray(c);
    JSValue len = JS_GetPropertyStr(c, oa, "length"), v = JS_GetPropertyStr(c, oc, "v");
    int32_t n = 0, i;
    JS_ToInt32(c, &n, len);
    JS_SetPropertyStr(c, nc, "v", v);
    for (i = 0; i < n; i++) JS_SetPropertyUint32(c, na, (uint32_t)i, JS_GetPropertyUint32(c, oa, (uint32_t)i));
    JS_SetPropertyStr(c, bench_irq_obj, "child", nc);
    JS_SetPropertyStr(c, bench_irq_obj, "arr", na);
    JS_FreeValue(c, oc);
    JS_FreeValue(c, oa);
    JS_FreeValue(c, len);
    JS_RunGC(rt);
  }
  if (bench_interrupt_left > 0 && --bench_interrupt_left == 0) return 1;
  return 0;
}

static JSValue js_host_irq_target(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  (void)this_val;
  if (argc > 0) {
    JS_FreeValue(ctx, bench_irq_obj);
    bench_irq_obj = JS_DupValue(ctx, argv[0]);
    bench_irq_ctx = ctx;
  }
  return JS_UNDEFINED;
}

/* newRealm(): a new context on the same runtime; returns its global object, so a script can take
   arrays, functions and prototypes from a second realm (`newRealm().eval("[]")`). The contexts live
   until the end of the run. */
static JSContext *bench_realms[16];
static int bench_nrealms;

static JSValue js_new_realm(
    JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  JSContext *nc;
  JSValue g;
  (void)this_val;
  (void)argc;
  (void)argv;
  if (bench_nrealms >= 16) return JS_ThrowRangeError(ctx, "too many realms");
  nc = JS_NewContext(JS_GetRuntime(ctx));
  if (!nc) return JS_ThrowInternalError(ctx, "cannot create a realm");
  bench_realms[bench_nrealms++] = nc;
  g = JS_GetGlobalObject(nc);
  JS_SetPropertyStr(nc, g, "print", JS_NewCFunction(nc, js_print, "print", 1));
  return g;
}

/* ---- the verification kit's host hooks (.local/kit/vkit.sh) ---------------------------------
   Writes through the C API (a native module changing the heap), fact groups for the watchpoint cases, and a second runtime. */
static JSValue js_host_set(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  const char *k;
  int r;
  (void)this_val;
  if (argc < 3) return JS_ThrowTypeError(ctx, "host_set(obj, name, value)");
  k = JS_ToCString(ctx, argv[1]);
  if (!k) return JS_EXCEPTION;
  r = JS_SetPropertyStr(ctx, argv[0], k, JS_DupValue(ctx, argv[2]));
  JS_FreeCString(ctx, k);
  return JS_NewInt32(ctx, r);
}

static JSValue js_host_define(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  const char *k;
  int r;
  (void)this_val;
  if (argc < 3) return JS_ThrowTypeError(ctx, "host_define(obj, name, value)");
  k = JS_ToCString(ctx, argv[1]);
  if (!k) return JS_EXCEPTION;
  r = JS_DefinePropertyValueStr(ctx, argv[0], k, JS_DupValue(ctx, argv[2]), JS_PROP_C_W_E);
  JS_FreeCString(ctx, k);
  return JS_NewInt32(ctx, r);
}

static JSValue js_host_delete(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  JSAtom a;
  int r;
  const char *k;
  (void)this_val;
  if (argc < 2) return JS_ThrowTypeError(ctx, "host_delete(obj, name)");
  k = JS_ToCString(ctx, argv[1]);
  if (!k) return JS_EXCEPTION;
  a = JS_NewAtom(ctx, k);
  JS_FreeCString(ctx, k);
  r = JS_DeleteProperty(ctx, argv[0], a, 0);
  JS_FreeAtom(ctx, a);
  return JS_NewInt32(ctx, r);
}

static JSValue js_host_setproto(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  (void)this_val;
  if (argc < 2) return JS_ThrowTypeError(ctx, "host_setproto(obj, proto)");
  return JS_NewInt32(ctx, JS_SetPrototype(ctx, argv[0], argv[1]));
}

/* The same writes with the target named by a dotted path from the global object ("w.cs.3", "Sc.prototype"), resolved in C: the script never hands the object
   to native code, so the closed-world analysis of the unit keeps its proofs and the break arrives from outside the program. A value that is a string starting
   with '@' is a path too. */
static JSValue bench_path(JSContext *ctx, const char *path) {
  JSValue cur = JS_GetGlobalObject(ctx);
  char buf[256], *tok, *save = NULL;
  snprintf(buf, sizeof(buf), "%s", path);
  for (tok = strtok_r(buf, ".", &save); tok != NULL; tok = strtok_r(NULL, ".", &save)) {
    JSValue nx = JS_GetPropertyStr(ctx, cur, tok);
    JS_FreeValue(ctx, cur);
    cur = nx;
    if (JS_IsException(cur) || JS_IsUndefined(cur)) break;
  }
  return cur;
}
static JSValue bench_pval(JSContext *ctx, JSValueConst v) {
  if (JS_IsString(v)) {
    const char *s = JS_ToCString(ctx, v);
    JSValue r;
    if (s != NULL && s[0] == '@') { r = bench_path(ctx, s + 1); JS_FreeCString(ctx, s); return r; }
    if (s != NULL) JS_FreeCString(ctx, s);
  }
  return JS_DupValue(ctx, v);
}
static JSValue js_host_p_op(JSContext *ctx, int argc, JSValueConst *argv, int op) {
  const char *pth, *k = NULL;
  JSValue o, v = JS_UNDEFINED;
  int r = -1;
  if (argc < 2) return JS_ThrowTypeError(ctx, "host_p_*(path, name[, value])");
  /* the profiling run of a vkit case must not see the break (the profile would hold the shapes the break makes, and the unit would be generated for a program that has them) */
  if (getenv("QJS_AOT_PROFILE_OUT") != NULL) return JS_NewInt32(ctx, -2);
  pth = JS_ToCString(ctx, argv[0]);
  if (!pth) return JS_EXCEPTION;
  o = bench_path(ctx, pth);
  JS_FreeCString(ctx, pth);
  if (op != 3) k = JS_ToCString(ctx, argv[1]);
  if (JS_IsObject(o)) {
    if (op == 0 && argc >= 3) { v = bench_pval(ctx, argv[2]); r = JS_SetPropertyStr(ctx, o, k, v); }
    else if (op == 1 && argc >= 3) { v = bench_pval(ctx, argv[2]); r = JS_DefinePropertyValueStr(ctx, o, k, v, JS_PROP_C_W_E); }
    else if (op == 2) { JSAtom a = JS_NewAtom(ctx, k); r = JS_DeleteProperty(ctx, o, a, 0); JS_FreeAtom(ctx, a); }
    else if (op == 3) { JSValue pr = bench_pval(ctx, argv[1]); r = JS_SetPrototype(ctx, o, pr); JS_FreeValue(ctx, pr); }
  }
  if (k) JS_FreeCString(ctx, k);
  JS_FreeValue(ctx, o);
  return JS_NewInt32(ctx, r);
}
static JSValue js_host_p_set(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) { (void)t; return js_host_p_op(ctx, argc, argv, 0); }
static JSValue js_host_p_define(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) { (void)t; return js_host_p_op(ctx, argc, argv, 1); }
static JSValue js_host_p_delete(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) { (void)t; return js_host_p_op(ctx, argc, argv, 2); }
static JSValue js_host_p_setproto(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) { (void)t; return js_host_p_op(ctx, argc, argv, 3); }

#define BENCH_MAX_FACTS 64
static JSAotFact *bench_facts[BENCH_MAX_FACTS];
static uint32_t bench_fact_word[BENCH_MAX_FACTS];
static int bench_nfacts;

/* fact_new(obj, flags, proto, [names]) -> handle */
static JSValue js_fact_new(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  JSAtom atoms[16];
  int32_t flags = 0, n = 0, i;
  JSAotFact *g;
  (void)this_val;
  if (argc < 3 || bench_nfacts >= BENCH_MAX_FACTS) return JS_ThrowTypeError(ctx, "fact_new(obj, flags, proto, [names])");
  JS_ToInt32(ctx, &flags, argv[1]);
  if (argc > 3 && JS_IsArray(argv[3])) {
    JSValue len = JS_GetPropertyStr(ctx, argv[3], "length");
    JS_ToInt32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    if (n > 16) n = 16;
    for (i = 0; i < n; i++) {
      JSValue e = JS_GetPropertyUint32(ctx, argv[3], (uint32_t)i);
      const char *s = JS_ToCString(ctx, e);
      atoms[i] = s ? JS_NewAtom(ctx, s) : JS_ATOM_NULL;
      if (s) JS_FreeCString(ctx, s);
      JS_FreeValue(ctx, e);
    }
  }
  g = qjs_aot_fact_new(ctx, argv[0], argv[2], (uint32_t)flags, atoms, n);
  for (i = 0; i < n; i++) JS_FreeAtom(ctx, atoms[i]);
  if (!g) return JS_NewInt32(ctx, -1);
  bench_fact_word[bench_nfacts] = 0;
  qjs_aot_fact_depend(g, &bench_fact_word[bench_nfacts], 1);
  bench_facts[bench_nfacts] = g;
  return JS_NewInt32(ctx, bench_nfacts++);
}

static JSAotFact *bench_fact_of(JSContext *ctx, JSValueConst v) {
  int32_t h = -1;
  (void)ctx;
  JS_ToInt32(ctx, &h, v);
  return h >= 0 && h < bench_nfacts ? bench_facts[h] : NULL;
}

static JSValue js_fact_validate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  JSAotFact *g = argc > 0 ? bench_fact_of(ctx, argv[0]) : NULL;
  (void)this_val;
  return JS_NewInt32(ctx, g ? qjs_aot_fact_validate(ctx, g) : -1);
}

static JSValue js_fact_state(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  JSAotFact *g = argc > 0 ? bench_fact_of(ctx, argv[0]) : NULL;
  (void)this_val;
  return JS_NewInt32(ctx, g ? qjs_aot_fact_state(g) : -1);
}

/* the dependent bit: what a compiled body's facts_valid word would hold */
static JSValue js_fact_bit(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  int32_t h = -1;
  (void)this_val;
  if (argc > 0) JS_ToInt32(ctx, &h, argv[0]);
  return JS_NewInt32(ctx, h >= 0 && h < bench_nfacts ? (int)(bench_fact_word[h] & 1u) : -1);
}

/* fact_expect(h, name, value) / fact_expect_proto(h, proto) */
static JSValue js_fact_expect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  JSAotFact *g = argc > 2 ? bench_fact_of(ctx, argv[0]) : NULL;
  const char *s;
  JSAtom a;
  int r;
  (void)this_val;
  if (!g) return JS_NewInt32(ctx, -1);
  s = JS_ToCString(ctx, argv[1]);
  if (!s) return JS_EXCEPTION;
  a = JS_NewAtom(ctx, s);
  JS_FreeCString(ctx, s);
  r = qjs_aot_fact_expect(ctx, g, a, argv[2]);
  JS_FreeAtom(ctx, a);
  return JS_NewInt32(ctx, r);
}

static JSValue js_fact_expect_proto(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  JSAotFact *g = argc > 1 ? bench_fact_of(ctx, argv[0]) : NULL;
  (void)this_val;
  return JS_NewInt32(ctx, g ? qjs_aot_fact_expect_proto(ctx, g, argv[1]) : -1);
}

static JSValue js_fact_break(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  JSAotFact *g = argc > 0 ? bench_fact_of(ctx, argv[0]) : NULL;
  (void)this_val;
  if (g) qjs_aot_fact_break(ctx, g);
  return JS_UNDEFINED;
}

/* a second runtime evaluating `src`: how many compiled functions did it bind (a unit serves one runtime), and how many did this one */
static JSValue js_host_bound(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  (void)this_val; (void)argc; (void)argv;
  return JS_NewInt32(ctx, (int)qjs_aot_runtime_bound(JS_GetRuntime(ctx)));
}

static JSValue js_host_second_runtime(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  const char *src;
  JSRuntime *rt2;
  JSContext *c2;
  JSValue r, g;
  int bound = -1;
  (void)this_val;
  if (argc < 1) return JS_ThrowTypeError(ctx, "host_second_runtime(src)");
  src = JS_ToCString(ctx, argv[0]);
  if (!src) return JS_EXCEPTION;
  rt2 = JS_NewRuntime();
  c2 = JS_NewContext(rt2);
  g = JS_GetGlobalObject(c2);
  JS_SetPropertyStr(c2, g, "print", JS_NewCFunction(c2, js_print, "print", 1));
  JS_FreeValue(c2, g);
  r = JS_Eval(c2, src, strlen(src), "<second runtime>", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(r)) {
    JSValue e = JS_GetException(c2);
    JS_FreeValue(c2, e);
  }
  JS_FreeValue(c2, r);
  bound = (int)qjs_aot_runtime_bound(rt2);
  JS_FreeCString(ctx, src);
  JS_FreeContext(c2);
  JS_FreeRuntime(rt2);
  return JS_NewInt32(ctx, bound);
}

int main(int argc, char **argv) {
  int mem = 0;
  int stats = 0;
  const char *bytecode_mode = "eager";
  const char *path = NULL;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--mem"))
      mem = 1;
    else if (!strcmp(argv[i], "--stats"))
      stats = 1;
    else if (!strncmp(argv[i], "--bytecode-mode=", 16))
      bytecode_mode = argv[i] + 16;
    else if (!strcmp(argv[i], "--bytecode-mode") && i + 1 < argc)
      bytecode_mode = argv[++i];
    else
      path = argv[i];
  }
  if (!path) {
    fprintf(
        stderr,
        "usage: qjs-bench [--mem] [--stats] "
        "[--bytecode-mode=<eager|lazy-copy|lazy-borrow>] <file.js|file.bc>\n");
    return 2;
  }
  int read_flags = JS_READ_OBJ_BYTECODE;
  if (!strcmp(bytecode_mode, "lazy-copy"))
    read_flags |= JS_READ_OBJ_LAZY;
  else if (!strcmp(bytecode_mode, "lazy-borrow"))
    read_flags |= JS_READ_OBJ_LAZY | JS_READ_OBJ_BORROW;
  else if (strcmp(bytecode_mode, "eager")) {
    fprintf(stderr, "unknown bytecode mode: %s\n", bytecode_mode);
    return 2;
  }

  size_t size = 0;
  uint8_t *buf = read_file(path, &size);
  if (!buf) {
    fprintf(stderr, "cannot read %s\n", path);
    return 2;
  }

  double t_boot = now_ms();
  JSRuntime *rt = JS_NewRuntime();
  /* An interrupt every N polls, so a poll can be tested the way the
     interpreter's is: the same script must stop at the same point. Driven by
     the environment so the poll test needs no separate harness. */
  {
    const char *ip = getenv("QJS_BENCH_INTERRUPT"), *irq = getenv("QJS_BENCH_IRQ_HEAP");
    if (irq && *irq) bench_irq_period = atoi(irq);
    if ((ip && *ip) || bench_irq_period > 0) {
      if (ip && *ip) bench_interrupt_left = atoi(ip);
      JS_SetInterruptHandler(rt, bench_interrupt_cb, NULL);
    }
  }
  JSContext *ctx = JS_NewContext(rt);
  JSValue global = JS_GetGlobalObject(ctx);
  JS_SetPropertyStr(
      ctx, global, "print", JS_NewCFunction(ctx, js_print, "print", 1));
  JS_SetPropertyStr(
      ctx, global, "host_add",
      JS_NewCFunction(ctx, js_host_add, "host_add", 2));
  JS_SetPropertyStr(
      ctx, global, "gc", JS_NewCFunction(ctx, js_force_gc, "gc", 0));
#ifdef QJS_KIT_HTMLDDA
  JS_SetPropertyStr(
      ctx, global, "makeHTMLDDA", JS_NewCFunction(ctx, js_make_htmldda, "makeHTMLDDA", 0));
#endif
  JS_SetPropertyStr(
      ctx, global, "newRealm", JS_NewCFunction(ctx, js_new_realm, "newRealm", 0));
  JS_SetPropertyStr(
      ctx, global, "host_opt",
      JS_NewCFunction(ctx, js_host_opt, "host_opt", 2));
  /* Marks the real bench driver, so a kernel can build a function that exists
     only at run time (never seen by qjsaot) to exercise the compiled ->
     interpreted -> compiled unwind path. */
  JS_SetPropertyStr(ctx, global, "__aot_bench_driver__", JS_NewBool(ctx, 1));
  JS_SetPropertyStr(ctx, global, "host_set", JS_NewCFunction(ctx, js_host_set, "host_set", 3));
  JS_SetPropertyStr(ctx, global, "host_define", JS_NewCFunction(ctx, js_host_define, "host_define", 3));
  JS_SetPropertyStr(ctx, global, "host_delete", JS_NewCFunction(ctx, js_host_delete, "host_delete", 2));
  JS_SetPropertyStr(ctx, global, "host_setproto", JS_NewCFunction(ctx, js_host_setproto, "host_setproto", 2));
  JS_SetPropertyStr(ctx, global, "host_p_set", JS_NewCFunction(ctx, js_host_p_set, "host_p_set", 3));
  JS_SetPropertyStr(ctx, global, "host_p_define", JS_NewCFunction(ctx, js_host_p_define, "host_p_define", 3));
  JS_SetPropertyStr(ctx, global, "host_p_delete", JS_NewCFunction(ctx, js_host_p_delete, "host_p_delete", 2));
  JS_SetPropertyStr(ctx, global, "host_p_setproto", JS_NewCFunction(ctx, js_host_p_setproto, "host_p_setproto", 2));
  JS_SetPropertyStr(ctx, global, "host_bound", JS_NewCFunction(ctx, js_host_bound, "host_bound", 0));
  JS_SetPropertyStr(ctx, global, "host_second_runtime", JS_NewCFunction(ctx, js_host_second_runtime, "host_second_runtime", 1));
  JS_SetPropertyStr(ctx, global, "host_irq_target", JS_NewCFunction(ctx, js_host_irq_target, "host_irq_target", 1));
  JS_SetPropertyStr(ctx, global, "fact_new", JS_NewCFunction(ctx, js_fact_new, "fact_new", 4));
  JS_SetPropertyStr(ctx, global, "fact_validate", JS_NewCFunction(ctx, js_fact_validate, "fact_validate", 1));
  JS_SetPropertyStr(ctx, global, "fact_state", JS_NewCFunction(ctx, js_fact_state, "fact_state", 1));
  JS_SetPropertyStr(ctx, global, "fact_bit", JS_NewCFunction(ctx, js_fact_bit, "fact_bit", 1));
  JS_SetPropertyStr(ctx, global, "fact_break", JS_NewCFunction(ctx, js_fact_break, "fact_break", 1));
  JS_SetPropertyStr(ctx, global, "fact_expect", JS_NewCFunction(ctx, js_fact_expect, "fact_expect", 3));
  JS_SetPropertyStr(ctx, global, "fact_expect_proto", JS_NewCFunction(ctx, js_fact_expect_proto, "fact_expect_proto", 2));
  JS_FreeValue(ctx, global);
  double t_start = now_ms();
  JSValue result;
  if (size >= NSBC_HEADER_SIZE && memcmp(buf, NSBC_MAGIC, 7) == 0) {
    JSValue fn = JS_ReadObject(
        ctx, buf + NSBC_HEADER_SIZE, size - NSBC_HEADER_SIZE, read_flags);
    result = JS_IsException(fn) ? fn : JS_EvalFunction(ctx, fn);
  } else {
    result = JS_Eval(ctx, (const char *)buf, size, path, JS_EVAL_TYPE_GLOBAL);
  }
  double t_eval = now_ms();
  int status = 0;
  if (JS_IsException(result)) {
    dump_exception(ctx);
    status = 1;
  }
  JS_FreeValue(ctx, result);

  for (;;) {
    JSContext *job_ctx = NULL;
    int rc = JS_ExecutePendingJob(rt, &job_ctx);
    if (rc == 0) break;
    if (rc < 0) {
      dump_exception(job_ctx);
      status = 1;
      break;
    }
  }
  double t_done = now_ms();

  if (stats) {
    JSMemoryUsage usage;
    JS_ComputeMemoryUsage(rt, &usage);
    fprintf(
        stderr,
        "##STATS## {\"startupMs\":%.3f,\"evalMs\":%.3f,\"mallocSize\":%lld}\n",
        t_start - t_boot, t_eval - t_start, (long long)usage.malloc_size);
  }

  if (mem) {
    JSMemoryUsage u;
    JS_RunGC(rt);
    JS_ComputeMemoryUsage(rt, &u);
    fprintf(
        stderr,
        "##MEM## {\"mallocSize\":%lld,\"mallocCount\":%lld,"
        "\"objCount\":%lld,\"objSize\":%lld,"
        "\"propCount\":%lld,\"propSize\":%lld,"
        "\"shapeCount\":%lld,\"shapeSize\":%lld,"
        "\"jsFuncCount\":%lld,\"jsFuncSize\":%lld,\"jsFuncCodeSize\":%lld,"
        "\"strCount\":%lld,\"strSize\":%lld,"
        "\"atomCount\":%lld,\"atomSize\":%lld,"
        "\"fastArrayCount\":%lld,\"fastArrayElements\":%lld}\n",
        (long long)u.malloc_size, (long long)u.malloc_count,
        (long long)u.obj_count, (long long)u.obj_size, (long long)u.prop_count,
        (long long)u.prop_size, (long long)u.shape_count,
        (long long)u.shape_size, (long long)u.js_func_count,
        (long long)u.js_func_size, (long long)u.js_func_code_size,
        (long long)u.str_count, (long long)u.str_size, (long long)u.atom_count,
        (long long)u.atom_size, (long long)u.fast_array_count,
        (long long)u.fast_array_elements);
  }

  JS_FreeValue(ctx, bench_irq_obj);
  bench_irq_obj = JS_UNDEFINED;
  for (int fi = 0; fi < bench_nfacts; fi++) bench_facts[fi] = NULL;
  while (bench_nrealms > 0) JS_FreeContext(bench_realms[--bench_nrealms]);
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
  free(buf);
  fflush(stdout);
  return status;
}
