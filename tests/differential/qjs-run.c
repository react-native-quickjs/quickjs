/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * Minimal CLI for the differential corpus: runs one file and provides `print`.
 *
 * `print` and nothing else, deliberately. The same corpus file is run by this
 * binary and by node, and the outputs are compared byte for byte, so anything
 * exposed here that node does not have is a way for the two to disagree for a
 * reason that has nothing to do with the engine.
 *
 * Accepts either JavaScript source or an NSBCNGS container from qjsc-ng, so
 * the corpus can be run down both paths -- and they are not the same path:
 * anything the compiler derives from source that the serializer drops is
 * present in one and absent in the other.
 *
 *   qjs-run <file.js|file.bc>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
  /* One byte over, NUL-terminated: JS_Eval requires it and a bytecode
     container never looks at it. */
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

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: qjs-run <file.js|file.bc>\n");
    return 2;
  }

  size_t size = 0;
  uint8_t *buf = read_file(argv[1], &size);
  if (!buf) {
    fprintf(stderr, "cannot read %s\n", argv[1]);
    return 2;
  }

  JSRuntime *rt = JS_NewRuntime();
  JSContext *ctx = JS_NewContext(rt);
  JSValue global = JS_GetGlobalObject(ctx);
  JS_SetPropertyStr(
      ctx, global, "print", JS_NewCFunction(ctx, js_print, "print", 1));
  JS_FreeValue(ctx, global);

  JSValue result;
  if (size >= NSBC_HEADER_SIZE && memcmp(buf, NSBC_MAGIC, 7) == 0) {
    JSValue fn = JS_ReadObject(
        ctx, buf + NSBC_HEADER_SIZE, size - NSBC_HEADER_SIZE,
        JS_READ_OBJ_BYTECODE);
    result = JS_IsException(fn) ? fn : JS_EvalFunction(ctx, fn);
  } else {
    result =
        JS_Eval(ctx, (const char *)buf, size, argv[1], JS_EVAL_TYPE_GLOBAL);
  }

  int status = 0;
  if (JS_IsException(result)) {
    dump_exception(ctx);
    status = 1;
  }
  JS_FreeValue(ctx, result);

  /* Promise callbacks are output too, so a corpus file that resolves one must
     see it before the process exits. */
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

  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
  free(buf);
  fflush(stdout);
  return status;
}
