/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * What does react-native-quickjs-intl cost an app that never uses it?
 *
 * THE QUESTION THIS ANSWERS, AND WHY IT IS THE ONLY ONE THAT MATTERS FIRST
 *   The module is installed into *every* runtime, before any application
 *   JavaScript. Most React Native apps never format a date. So the number that
 *   decides whether this design is acceptable is not how fast `format()` is —
 *   it is what an app pays for `Intl` existing and never being touched.
 *
 *   The mechanism under test is the self-replacing accessor: install a
 *   getter/setter pair on `globalThis.Intl`, and on first *read* deserialize a
 *   precompiled bytecode blob and redefine the property as plain data. Nothing
 *   is deserialized, parsed or evaluated until then.
 *
 * MODES
 *   install-only  create a context, install the module, tear down. `Intl` is
 *                 never read.
 *   control       the same without installing. The difference is the cost.
 *   touched       ... plus one read of `Intl`, i.e. the full materialization:
 *                 JS_ReadObject of the blob, JS_EvalFunction, and building the
 *                 native function object.
 *   format        ... plus constructing one formatter and calling format().
 *
 * HOW IT AVOIDS THE USUAL WAYS OF LYING
 *   - Every mode consumes its result and the sink value is printed and
 *     checked, so nothing can be eliminated and a mode that silently did no
 *     work is visible.
 *   - Distribution, not a single number: min / median / p95 / max over
 *     iterations, and the driver is expected to be run several times because
 *     inter-run spread is the real error bar.
 *   - `control` exists because JS_NewRuntime + JS_NewContext costs ~54 us on
 *     an M4 Pro, which is three orders of magnitude more than the accessor
 *     install. Reporting the absolute per-iteration time of `install-only`
 *     would report the cost of creating a context and call it the cost of Intl.
 *     That mistake is why this file has a control arm at all.
 *
 * USAGE
 *   intl-bench [iterations]        default 300
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "IntlModuleC.h"
#include "quickjs.h"

static double now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static int cmp(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y ? 1 : 0;
}

enum Mode { CONTROL, INSTALL_ONLY, TOUCHED, FORMAT };

static const char *kModeName[] = {
    "control", "install-only", "touched", "format"};

/* Kept identical across modes so no mode can win by doing less. */
static const char kSink[] =
    "var s = 0; for (var i = 0; i < 100; i++) s += i; s";

static double run_iteration(enum Mode mode, long long *sink) {
  const double t0 = now_ns();
  JSRuntime *rt = JS_NewRuntime();
  JSContext *ctx = JS_NewContext(rt);

  if (mode != CONTROL) rnqjs_intl_install(ctx);

  if (mode >= TOUCHED) {
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue intl = JS_GetPropertyStr(ctx, g, "Intl");
    if (JS_IsObject(intl)) (*sink)++;
    JS_FreeValue(ctx, intl);
    JS_FreeValue(ctx, g);
  }

  if (mode >= FORMAT) {
    static const char kFmt[] =
        "new Intl.DateTimeFormat('en-US', {timeZone:'UTC'}).format(0).length";
    JSValue v = JS_Eval(ctx, kFmt, strlen(kFmt), "fmt", 0);
    int32_t n = 0;
    JS_ToInt32(ctx, &n, v);
    JS_FreeValue(ctx, v);
    *sink += n;
  }

  JSValue v = JS_Eval(ctx, kSink, strlen(kSink), "sink", 0);
  int32_t out = -1;
  JS_ToInt32(ctx, &out, v);
  JS_FreeValue(ctx, v);
  if (out == 4950) (*sink)++;

  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
  return now_ns() - t0;
}

int main(int argc, char **argv) {
  const int iters = argc > 1 ? atoi(argv[1]) : 300;
  if (iters < 10) {
    fprintf(stderr, "usage: intl-bench [iterations >= 10]\n");
    return 2;
  }

  double *s = malloc(sizeof(double) * (size_t)iters);
  double median[4];
  long long sinks[4] = {0, 0, 0, 0};

  /*
   * Interleaved rather than one mode after another: a machine that gets busier
   * during the run would otherwise put the drift entirely into whichever mode
   * ran last, and the difference between modes is the whole measurement.
   */
  for (int m = 0; m < 4; m++) {
    for (int i = 0; i < iters; i++) {
      for (int w = 0; w < 4; w++) {
        double dt = run_iteration((enum Mode)w, &sinks[w]);
        if (w == m) s[i] = dt;
      }
    }
    qsort(s, (size_t)iters, sizeof(double), cmp);
    median[m] = s[iters / 2];
    printf(
        "%-13s n=%d  min=%8.2f  med=%8.2f  p95=%8.2f  max=%9.2f  (us)\n",
        kModeName[m], iters, s[0] / 1000.0, s[iters / 2] / 1000.0,
        s[(int)(iters * 0.95)] / 1000.0, s[iters - 1] / 1000.0);
  }

  printf("\ndeltas against control, by median:\n");
  printf(
      "  install, never touched : %+8.3f us   <- what an app that never uses "
      "Intl pays\n",
      (median[INSTALL_ONLY] - median[CONTROL]) / 1000.0);
  printf(
      "  first read (materialize): %+8.3f us\n",
      (median[TOUCHED] - median[INSTALL_ONLY]) / 1000.0);
  printf(
      "  + one formatter + format: %+8.3f us\n",
      (median[FORMAT] - median[TOUCHED]) / 1000.0);

  /*
   * Assert the workload ran. A benchmark that cannot prove this is not
   * evidence; this project has published three confident wrong numbers exactly
   * that way.
   */
  const long long expect_control = iters * 4;
  int ok = sinks[CONTROL] == expect_control &&
           sinks[INSTALL_ONLY] == expect_control &&
           sinks[TOUCHED] == expect_control * 2 && sinks[FORMAT] > 0;
  printf(
      "\nsink control=%lld install=%lld touched=%lld format=%lld  %s\n",
      sinks[CONTROL], sinks[INSTALL_ONLY], sinks[TOUCHED], sinks[FORMAT],
      ok ? "ok" : "BROKEN");
  /* Count, do not infer: which load path the module actually took is printed,
     not assumed. Empty unless built with -DRNQJS_INTL_COUNTERS=1. */
  printf("\n");
  rnqjs_intl_dump_counters();
  free(s);
  return ok ? 0 : 1;
}
