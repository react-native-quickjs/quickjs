// Compile + evaluate fuzzer (libFuzzer). The input is JS source; the first byte picks script or module mode and
// the strictness. The engine translation units are included so that the engine itself is instrumented.
//   clang -g -O1 -fsanitize=address,undefined,fuzzer -I../../engine/quickjs-ng [-DQJS_REGVM=1] -o fuzz-eval fuzz-eval.c
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cutils.h"
#include "dtoa.c"
#include "libregexp.c"
#include "libunicode.c"
#include "quickjs.c"
#include "quickjs.h"

static int fz_ticks;
static int fz_interrupt(JSRuntime *rt, void *opaque) {
  (void)rt;
  (void)opaque;
  return ++fz_ticks > 20000;  // a runaway loop ends with an exception
}

int LLVMFuzzerTestOneInput(const uint8_t *buf, size_t len) {
  if (len < 2) return 0;
  JSRuntime *rt = JS_NewRuntime();
  if (!rt) exit(1);
  JS_SetMemoryLimit(rt, 64u << 20);
  JS_SetMaxStackSize(rt, 512u << 10);
  fz_ticks = 0;
  JS_SetInterruptHandler(rt, fz_interrupt, NULL);
  JSContext *ctx = JS_NewContext(rt);
  if (!ctx) exit(1);
  int flags = (buf[0] & 1) ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL;
  if (buf[0] & 2) flags |= JS_EVAL_FLAG_STRICT;
  if (buf[0] & 4) flags |= JS_EVAL_FLAG_COMPILE_ONLY;
  char *src = malloc(len);
  if (!src) exit(1);
  memcpy(src, buf + 1, len - 1);
  src[len - 1] = 0;
  JSValue v = JS_Eval(ctx, src, len - 1, "<fuzz>", flags);
  free(src);
  if (JS_IsException(v)) {
    JSValue e = JS_GetException(ctx);
    JS_FreeValue(ctx, e);
  }
  JS_FreeValue(ctx, v);
  for (int i = 0; i < 100; i++) {
    JSContext *c1;
    int r = JS_ExecutePendingJob(rt, &c1);
    if (r <= 0) {
      if (r < 0) {
        JSValue e = JS_GetException(c1);
        JS_FreeValue(c1, e);
      }
      break;
    }
  }
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
  return 0;
}
