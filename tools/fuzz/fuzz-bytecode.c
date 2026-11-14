// Bytecode loader fuzzer (libFuzzer): the input is a serialized function/module container. The custom mutator
// keeps the container checksum valid so that mutations reach the loader instead of being rejected up front;
// a container the loader accepts is also executed (bounded), because the loader must never hand the
// interpreter something it can crash on.
//   clang -g -O1 -fsanitize=address,undefined,fuzzer -I../../engine/quickjs-ng [-DQJS_REGVM=1] -o fuzz-bytecode fuzz-bytecode.c
// Seeds: tools/fuzz/seed-bytecode.sh compiles JS files with qjsc into a corpus directory.
#ifdef FUZZ_LOAD_ONLY
#define FUZZ_EXECUTE 0
#else
#define FUZZ_EXECUTE 1
#endif
#define _GNU_SOURCE  // before any system header: the engine uses pthread_getattr_np
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
  return ++fz_ticks > 20000;
}

static void fix_checksum(uint8_t *b, size_t len) {
  if (len < 6) return;
  put_u32(&b[1], bc_csum(&b[5], len - 5));
}

size_t LLVMFuzzerMutate(uint8_t *data, size_t size, size_t max_size);  // libFuzzer's default mutator

size_t LLVMFuzzerCustomMutator(
    uint8_t *data, size_t size, size_t max_size, unsigned int seed) {
  if (size < 6) return LLVMFuzzerMutate(data, size, max_size);
  size_t n = LLVMFuzzerMutate(data + 5, size - 5, max_size - 5);
  n += 5;
  fix_checksum(data, n);
  (void)seed;
  return n;
}

int LLVMFuzzerTestOneInput(const uint8_t *buf, size_t len) {
  if (len < 6) return 0;
  JSRuntime *rt = JS_NewRuntime();
  if (!rt) exit(1);
  JS_SetMemoryLimit(rt, 64u << 20);
  JS_SetMaxStackSize(rt, 512u << 10);
  fz_ticks = 0;
  JS_SetInterruptHandler(rt, fz_interrupt, NULL);
  JSContext *ctx = JS_NewContext(rt);
  if (!ctx) exit(1);
  uint8_t *copy = malloc(len);
  if (!copy) exit(1);
  memcpy(copy, buf, len);
  fix_checksum(copy, len);
  JSValue val = JS_ReadObject(ctx, copy, len, JS_READ_OBJ_BYTECODE);
  free(copy);
  if (JS_IsException(val)) {
    JSValue e = JS_GetException(ctx);
    JS_FreeValue(ctx, e);
  } else if (!FUZZ_EXECUTE) {
    // load-only: the loader is checked, an accepted container is not run
  } else if (JS_VALUE_GET_TAG(val) == JS_TAG_FUNCTION_BYTECODE) {
    JSValue r = JS_EvalFunction(ctx, val);  // consumes val
    if (JS_IsException(r)) {
      JSValue e = JS_GetException(ctx);
      JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, r);
    val = JS_UNDEFINED;
  } else if (JS_VALUE_GET_TAG(val) == JS_TAG_MODULE) {
    JSValue r = JS_EvalFunction(ctx, val);
    if (JS_IsException(r)) {
      JSValue e = JS_GetException(ctx);
      JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, r);
    val = JS_UNDEFINED;
  }
  JS_FreeValue(ctx, val);
  for (int i = 0; i < 50; i++) {
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
