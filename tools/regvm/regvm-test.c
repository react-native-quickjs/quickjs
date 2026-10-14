/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * regvm-test -- a harness for the register VM.
 *
 *   regvm-test [--runs N] <file.js>
 *   regvm-test --expr [--runs N] <file.js> [arg ...]
 *
 * Minimal context: JS_NewRuntime + JS_NewContext, no module registry and no
 * serialized built-ins. The engine's core built-ins are native C and always
 * present; the builtin-*.h blobs are *module* payloads, and a register-VM
 * runtime must never be handed stack bytecode, so this target links none of
 * them. The harness is native and self-contained: the only file it reads is the
 * script named on the command line.
 *
 * --expr wraps the file in parentheses, evaluates it, and calls the resulting
 * function with the numeric arguments that follow. That is how a kernel is run
 * without the script needing globals or a call site of its own.
 *
 * Output, in the shape the project's fixed-work harnesses already use:
 *
 *   regvm-test vm=<stack|regvm> builtins=none runs=<n> ms=<elapsed>
 *   result=<value>
 *   regvm funcs=<n> code=<bytes> stack=<bytes> edge_moves=<n> refused=<n>
 *
 * Built only when QJS_REGVM=ON -- see CMakeLists.txt for the target and the
 * recorded built-in strategy.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "quickjs.h"

/* Undefined means the stack VM: the flag defines nothing in that build, on
   purpose, so the off case's compile line is unchanged. */
#ifndef QJS_REGVM
#define QJS_REGVM 0
#endif

#if QJS_REGVM
extern long long js_regvm_stat_encoded_funcs(void);
extern long long js_regvm_stat_unencodable(void);
extern long long js_regvm_stat_encoded_bytes(void);
extern long long js_regvm_stat_encoded_insns(void);
extern long long js_regvm_stat_split_edges(void);
#endif

/* The application packaging wrapper around a JS_WriteObject payload
   (src/bytecode/QuickJSBytecode.h). This harness exists to run *source*; a
   container is refused rather than mis-parsed; under QJS_REGVM a stack blob is
   not loadable anyway. */
static const char kContainerMagic[8] = {'N', 'S', 'B', 'C',
                                        'N', 'G', 'S', '\0'};

static uint64_t now_ns(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static JSValue js_print(
    JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  int i;
  (void)this_val;
  for (i = 0; i < argc; i++) {
    const char *s = JS_ToCString(ctx, argv[i]);
    if (i) fputc(' ', stdout);
    fputs(s ? s : "", stdout);
    JS_FreeCString(ctx, s);
  }
  fputc('\n', stdout);
  return JS_UNDEFINED;
}

static void report_exception(JSContext *ctx) {
  JSValue e = JS_GetException(ctx);
  const char *s = JS_ToCString(ctx, e);
  fprintf(stderr, "uncaught: %s\n", s ? s : "(unprintable)");
  JS_FreeCString(ctx, s);
  JS_FreeValue(ctx, e);
}

#ifdef REGVM_TEST_FIXTURES
/* Scratch fixture: a C method that always throws, so the method-call failure
   path (callee, receiver and argument cleanup) can be exercised from JS. */
static JSValue regvm_fixture_boom(
    JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  (void)this_val;
  (void)argc;
  (void)argv;
  return JS_ThrowTypeError(ctx, "boom");
}
#endif

#ifdef REGVM_TEST_FIXTURES
/* Element-read fixtures: a real array, an array with a hole whose prototype
   supplies the missing index, and an object with one value getter and one that
   throws. Built in C because the JS subset has no object/array literals. */
static JSValue regvm_fixture_get42(
    JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  (void)this_val;
  (void)argc;
  (void)argv;
  return JS_NewInt32(ctx, 42);
}
static JSValue regvm_fixture_getthrow(
    JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  (void)this_val;
  (void)argc;
  (void)argv;
  return JS_ThrowTypeError(ctx, "elem-throw");
}
static void regvm_fixture_double(JSContext *ctx, const char *name, double v) {
  JSValue glob = JS_GetGlobalObject(ctx);
  JS_SetPropertyStr(ctx, glob, name, JS_NewFloat64(ctx, v));
  JS_FreeValue(ctx, glob);
}
static void regvm_fixture_elements(JSContext *ctx) {
  JSValue glob, arr, hole, proto, getobj;
  JSAtom atom;
  glob = JS_GetGlobalObject(ctx);

  arr = JS_NewArray(ctx);
  JS_SetPropertyUint32(ctx, arr, 0, JS_NewInt32(ctx, 10));
  JS_SetPropertyUint32(ctx, arr, 1, JS_NewInt32(ctx, 20));
  JS_SetPropertyUint32(ctx, arr, 2, JS_NewInt32(ctx, 30));
  JS_SetPropertyStr(ctx, glob, "ARR", arr);

  /* index 1 left unset: a hole, whose lookup must reach the prototype */
  hole = JS_NewArray(ctx);
  JS_SetPropertyUint32(ctx, hole, 0, JS_NewInt32(ctx, 7));
  JS_SetPropertyUint32(ctx, hole, 2, JS_NewInt32(ctx, 9));
  proto = JS_NewObject(ctx);
  JS_SetPropertyUint32(ctx, proto, 1, JS_NewInt32(ctx, 99));
  JS_SetPrototype(ctx, hole, proto);
  JS_SetPropertyStr(ctx, glob, "HOLEARR", hole);

  getobj = JS_NewObject(ctx);
  atom = JS_NewAtomUInt32(ctx, 0);
  JS_DefineProperty(
      ctx, getobj, atom, JS_UNDEFINED,
      JS_NewCFunction(ctx, regvm_fixture_get42, "get0", 0), JS_UNDEFINED,
      JS_PROP_HAS_GET | JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
  JS_FreeAtom(ctx, atom);
  atom = JS_NewAtomUInt32(ctx, 1);
  JS_DefineProperty(
      ctx, getobj, atom, JS_UNDEFINED,
      JS_NewCFunction(ctx, regvm_fixture_getthrow, "get1", 0), JS_UNDEFINED,
      JS_PROP_HAS_GET | JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
  JS_FreeAtom(ctx, atom);
  JS_SetPropertyStr(ctx, glob, "GETOBJ", getobj);

  JS_FreeValue(ctx, glob);
}
#endif

int main(int argc, char **argv) {
  const char *path = NULL;
  int runs = 1;
  int expr = 0;
  int nobj = 0;
  int i;
  double call_args[8];
  int ncall = 0;

  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--runs") && i + 1 < argc) {
      runs = atoi(argv[++i]);
      if (runs < 1) runs = 1;
    } else if (!strcmp(argv[i], "--expr")) {
      expr = 1;
    } else if (!strcmp(argv[i], "--obj") && i + 1 < argc) {
      nobj = atoi(argv[++i]);
    } else if (!path) {
      path = argv[i];
    } else if (ncall < (int)(sizeof(call_args) / sizeof(call_args[0]))) {
      call_args[ncall++] = atof(argv[i]);
    } else {
      fprintf(
          stderr,
          "usage: regvm-test [--expr] [--runs N] <file.js> [arg ...]\n");
      return 2;
    }
  }
  if (!path) {
    fprintf(
        stderr, "usage: regvm-test [--expr] [--runs N] <file.js> [arg ...]\n");
    return 2;
  }

  FILE *f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "cannot read %s\n", path);
    return 2;
  }
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (len <= 0) {
    fprintf(stderr, "%s is empty\n", path);
    fclose(f);
    return 2;
  }
  char *src = malloc((size_t)len + 4);
  if (fread(src, 1, (size_t)len, f) != (size_t)len) {
    fprintf(stderr, "short read on %s\n", path);
    fclose(f);
    return 2;
  }
  src[len] = '\0';
  fclose(f);

  if (len >= 8 && !memcmp(src, kContainerMagic, 8)) {
    fprintf(
        stderr,
        "regvm-test: %s is a precompiled bytecode container, not source.\n"
        "regvm-test runs source only.\n",
        path);
    free(src);
    return 2;
  }

  /* --expr: evaluate the file as an expression, then call what it produced. */
  long eval_len = len;
  if (expr) {
    memmove(src + 1, src, (size_t)len);
    src[0] = '(';
    src[len + 1] = ')';
    src[len + 2] = '\0';
    eval_len = len + 2;
  }

  JSRuntime *rt = JS_NewRuntime();
  JSContext *ctx = JS_NewContext(rt);
  JSValue global = JS_GetGlobalObject(ctx);
  JS_SetPropertyStr(
      ctx, global, "print", JS_NewCFunction(ctx, js_print, "print", 1));
  JS_FreeValue(ctx, global);

#ifdef REGVM_TEST_TRACE_SRC
  fprintf(stderr, "SRC len=%ld text=[", eval_len);
  fwrite(src, 1, (size_t)eval_len, stderr);
  fprintf(stderr, "]\n");
#endif
#ifdef REGVM_TEST_MEMPROBE
  {
    JSMemoryUsage mu_;
    JS_ComputeMemoryUsage(rt, &mu_);
    fprintf(
        stderr, "MEM before str=%lld obj=%lld malloc=%lld msize=%lld\n", "%s",
        (long long)mu_.str_count, (long long)mu_.obj_count,
        (long long)mu_.malloc_count, (long long)mu_.malloc_size);
  }
#endif
#ifdef REGVM_TEST_FIXTURES
  {
    JSValue glob = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(
        ctx, glob, "boom", JS_NewCFunction(ctx, regvm_fixture_boom, "boom", 3));
    JS_FreeValue(ctx, glob);
  }
#endif
#ifdef REGVM_TEST_FIXTURES
  regvm_fixture_elements(ctx);
#endif
  uint64_t t0 = now_ns();
  JSValue v = JS_Eval(ctx, src, (size_t)eval_len, path, JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(v)) {
    report_exception(ctx);
    JS_FreeValue(ctx, v);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    free(src);
    return 1;
  }

  if (expr) {
    /* --obj K: K plain objects with x/y/w data fields, passed first. That is
       how a field-heavy kernel gets receivers without needing object-literal
       support in the register path. */
    JSValueConst args[16];
    int k;
    if (nobj > 8) nobj = 8;
    for (k = 0; k < nobj; k++) {
      JSValue o = JS_NewObject(ctx);
      JS_SetPropertyStr(ctx, o, "x", JS_NewFloat64(ctx, 1.0 + k));
      JS_SetPropertyStr(ctx, o, "y", JS_NewFloat64(ctx, 2.0 + k));
      JS_SetPropertyStr(ctx, o, "w", JS_NewFloat64(ctx, 1.5 + k));
      args[k] = o;
    }
    for (k = 0; k < ncall; k++)
      args[nobj + k] = JS_NewFloat64(ctx, call_args[k]);
    for (i = 0; i < runs; i++) {
      JSValue ret = JS_Call(ctx, v, JS_UNDEFINED, nobj + ncall, args);
      if (JS_IsException(ret)) {
        report_exception(ctx);
        JS_FreeValue(ctx, ret);
        break;
      }
      if (i == runs - 1) {
        const char *s = JS_ToCString(ctx, ret);
        printf("result=%s\n", s ? s : "(unprintable)");
        JS_FreeCString(ctx, s);
      }
      JS_FreeValue(ctx, ret);
    }
    for (k = 0; k < nobj + ncall; k++) JS_FreeValue(ctx, (JSValue)args[k]);
  } else {
    for (i = 0; i < runs; i++) {
      JSValue r =
          JS_Eval(ctx, src, (size_t)eval_len, path, JS_EVAL_TYPE_GLOBAL);
      if (JS_IsException(r)) {
        report_exception(ctx);
        JS_FreeValue(ctx, r);
        break;
      }
      JS_FreeValue(ctx, r);
    }
  }
  uint64_t elapsed = now_ns() - t0;

  JS_FreeValue(ctx, v);
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
  free(src);

  fprintf(
      stdout, "regvm-test vm=%s builtins=none runs=%d ms=%.0f\n",
#if QJS_REGVM
      "regvm",
#else
      "stack",
#endif
      runs, (double)elapsed / 1e6);

#if QJS_REGVM
  fprintf(
      stdout,
      "regvm encoded=%lld unencodable=%lld bytes=%lld insns=%lld"
      " split_edges=%lld\n",
      js_regvm_stat_encoded_funcs(), js_regvm_stat_unencodable(),
      js_regvm_stat_encoded_bytes(), js_regvm_stat_encoded_insns(),
      js_regvm_stat_split_edges());
#endif

#ifdef REGVM_TEST_MEMPROBE
  {
    JSMemoryUsage mu_;
    JS_ComputeMemoryUsage(rt, &mu_);
    fprintf(
        stderr, "MEM after str=%lld obj=%lld malloc=%lld msize=%lld\n", "%s",
        (long long)mu_.str_count, (long long)mu_.obj_count,
        (long long)mu_.malloc_count, (long long)mu_.malloc_size);
  }
#endif
  return i == runs ? 0 : 1;
}
