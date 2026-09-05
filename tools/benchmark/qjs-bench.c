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
 *   qjs-bench [--mem] [--stats] <file.js|file.bc>
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

int main(int argc, char **argv) {
  int mem = 0;
  int stats = 0;
  const char *path = NULL;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--mem"))
      mem = 1;
    else if (!strcmp(argv[i], "--stats"))
      stats = 1;
    else
      path = argv[i];
  }
  if (!path) {
    fprintf(stderr, "usage: qjs-bench [--mem] [--stats] <file.js|file.bc>\n");
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
  JSContext *ctx = JS_NewContext(rt);
  JSValue global = JS_GetGlobalObject(ctx);
  JS_SetPropertyStr(
      ctx, global, "print", JS_NewCFunction(ctx, js_print, "print", 1));
  JS_FreeValue(ctx, global);
  double t_start = now_ms();
  JSValue result;
  if (size >= NSBC_HEADER_SIZE && memcmp(buf, NSBC_MAGIC, 7) == 0) {
    JSValue fn = JS_ReadObject(
        ctx, buf + NSBC_HEADER_SIZE, size - NSBC_HEADER_SIZE,
        JS_READ_OBJ_BYTECODE);
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
        "\"strCount\":%lld,\"strSize\":%lld,"
        "\"atomCount\":%lld,\"atomSize\":%lld,"
        "\"fastArrayCount\":%lld,\"fastArrayElements\":%lld}\n",
        (long long)u.malloc_size, (long long)u.malloc_count,
        (long long)u.obj_count, (long long)u.obj_size, (long long)u.prop_count,
        (long long)u.prop_size, (long long)u.shape_count,
        (long long)u.shape_size, (long long)u.str_count, (long long)u.str_size,
        (long long)u.atom_count, (long long)u.atom_size,
        (long long)u.fast_array_count, (long long)u.fast_array_elements);
  }

  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
  free(buf);
  fflush(stdout);
  return status;
}