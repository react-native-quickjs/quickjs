/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * A QuickJS shell with the Intl module installed and nothing else.
 *
 * PURPOSE
 *   Three consumers, all of which need an engine with Intl and none of which
 *   can have React Native in the build:
 *
 *     - `tools/test262/run.py`, which needs a binary that runs a file and exits
 *       non-zero on an uncaught exception;
 *     - the differential corpus, which runs the same script here and under
 *       node and diffs stdout byte for byte;
 *     - iterating on js/intl.js without a 90-second CMake cycle.
 *
 * CONTRACT
 *   intl-cli [--eager] [--print-backend] <file.js>
 *   intl-cli [--eager] -e <source>
 *
 *   Exit 0 on success, 1 on an uncaught exception (message on stderr), 2 on a
 *   usage error. `print()` writes a line to stdout; test262 harness files call
 *   it, and so does the differential corpus, which is why it must match node's
 *   `console.log` for the value shapes the corpus uses.
 *
 *   `--eager` reads `Intl` during startup, before the script runs. That exists
 *   so a test can distinguish "the lazy accessor is broken" from "the Intl
 *   layer is broken": if a script passes under --eager and fails without it,
 *   the bug is in the accessor, not in ECMA-402.
 *
 * WHAT IT DELIBERATELY DOES NOT DO
 *   No module loader, no job queue beyond the engine's, no REPL. Anything more
 *   makes it a second shell to maintain, and `qjs-bench` already exists for
 *   benchmarking.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "IntlModuleC.h"
#include "quickjs.h"

static JSValue js_print(
    JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  (void)this_val;
  for (int i = 0; i < argc; i++) {
    if (i != 0) fputc(' ', stdout);
    const char *s = JS_ToCString(ctx, argv[i]);
    if (!s) return JS_EXCEPTION;
    fputs(s, stdout);
    JS_FreeCString(ctx, s);
  }
  fputc('\n', stdout);
  return JS_UNDEFINED;
}

static void dump_error(JSContext *ctx) {
  JSValue exc = JS_GetException(ctx);
  const char *s = JS_ToCString(ctx, exc);
  fprintf(stderr, "%s\n", s ? s : "(uncaught, unprintable)");
  JS_FreeCString(ctx, s);
  if (JS_IsError(exc)) {
    JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
    if (!JS_IsUndefined(stack)) {
      const char *st = JS_ToCString(ctx, stack);
      if (st) fprintf(stderr, "%s", st);
      JS_FreeCString(ctx, st);
    }
    JS_FreeValue(ctx, stack);
  }
  JS_FreeValue(ctx, exc);
}

static char *read_file(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = malloc((size_t)n + 1);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
    fclose(f);
    free(buf);
    return NULL;
  }
  fclose(f);
  buf[n] = 0;
  *len = (size_t)n;
  return buf;
}

int main(int argc, char **argv) {
  int eager = 0, print_backend = 0, expr = 0;
  int i = 1;
  for (; i < argc && argv[i][0] == '-' && argv[i][1] != 0; i++) {
    if (!strcmp(argv[i], "--eager"))
      eager = 1;
    else if (!strcmp(argv[i], "--print-backend"))
      print_backend = 1;
    else if (!strcmp(argv[i], "-e")) {
      expr = 1;
      i++;
      break;
    } else {
      fprintf(stderr, "unknown option %s\n", argv[i]);
      return 2;
    }
  }
  if (i >= argc) {
    fprintf(
        stderr,
        "usage: intl-cli [--eager] [--print-backend] <file.js|-e src>\n");
    return 2;
  }

  JSRuntime *rt = JS_NewRuntime();
  JSContext *ctx = JS_NewContext(rt);
  JSValue global = JS_GetGlobalObject(ctx);
  JS_SetPropertyStr(
      ctx, global, "print", JS_NewCFunction(ctx, js_print, "print", 1));
  /* test262's harness and much RN-shaped code use console.log. */
  JSValue console = JS_NewObject(ctx);
  JS_SetPropertyStr(
      ctx, console, "log", JS_NewCFunction(ctx, js_print, "log", 1));
  JS_SetPropertyStr(ctx, global, "console", console);
  JS_FreeValue(ctx, global);

  rnqjs_intl_install(ctx);

  if (eager || print_backend) {
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue intl = JS_GetPropertyStr(ctx, g, "Intl");
    if (print_backend) {
      JSValue fn = JS_GetPropertyStr(ctx, intl, "__rnqjsBackend");
      JSValue name = JS_Call(ctx, fn, intl, 0, NULL);
      const char *s = JS_ToCString(ctx, name);
      printf("backend=%s\n", s ? s : "?");
      JS_FreeCString(ctx, s);
      JS_FreeValue(ctx, name);
      JS_FreeValue(ctx, fn);
    }
    JS_FreeValue(ctx, intl);
    JS_FreeValue(ctx, g);
  }

  int status = 0;
  JSValue result;
  if (expr) {
    result =
        JS_Eval(ctx, argv[i], strlen(argv[i]), "<expr>", JS_EVAL_TYPE_GLOBAL);
  } else {
    size_t len = 0;
    char *src = read_file(argv[i], &len);
    if (!src) {
      fprintf(stderr, "cannot read %s\n", argv[i]);
      return 2;
    }
    result = JS_Eval(ctx, src, len, argv[i], JS_EVAL_TYPE_GLOBAL);
    free(src);
  }
  if (JS_IsException(result)) {
    dump_error(ctx);
    status = 1;
  }
  JS_FreeValue(ctx, result);

  /* Drain pending jobs so a rejected promise is not silently swallowed. */
  for (;;) {
    JSContext *c = NULL;
    int rc = JS_ExecutePendingJob(rt, &c);
    if (rc <= 0) {
      if (rc < 0) {
        dump_error(c ? c : ctx);
        status = 1;
      }
      break;
    }
  }

  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
  return status;
}
