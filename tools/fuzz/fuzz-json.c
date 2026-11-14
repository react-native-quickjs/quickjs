// JSON.parse / JSON.stringify fuzzer (libFuzzer). The input after the first byte is the JSON text; the first byte picks
// the variant. Beyond crashes, it checks invariants and aborts on a violation:
//   - a second parse of the same text (the shape caches warm) stringifies the same as the first;
//   - stringify(parse(stringify(v))) == stringify(v), also with an indent and with an identity reviver;
//   - a parse failure is a SyntaxError (or a stack/memory error).
// Bit 0 wraps the text as `[<text>,"\u0100"]` so the parser reads a 16-bit string.
//   clang -g -O1 -fsanitize=address,undefined,fuzzer -I../../engine/quickjs-ng [-DQJS_REGVM=1] -o fuzz-json fuzz-json.c
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

static const char fz_check[] =
    "(function (inp, m) {\n"
    "  function ok(e) { return e instanceof SyntaxError || e instanceof RangeError || e instanceof InternalError; }\n"
    "  function fail(w) { throw 'FUZZFAIL ' + w; }\n"
    "  var s = (m & 1) ? '[' + inp + ',\"\\u0100\"]' : inp;\n"
    "  var a;\n"
    "  try { a = JSON.parse(s); } catch (e) { if (!ok(e)) fail('parse threw ' + e); return; }\n"
    "  var t1;\n"
    "  try { t1 = JSON.stringify(a); } catch (e) { if (!ok(e)) fail('stringify threw ' + e); return; }\n"
    "  var b = JSON.parse(s);\n"
    "  if (JSON.stringify(b) !== t1) fail('second parse');\n"
    "  if (t1 !== undefined && JSON.stringify(JSON.parse(t1)) !== t1) fail('round trip');\n"
    "  var ind = (m >> 1) & 7;\n"
    "  if (ind) {\n"
    "    var t3 = JSON.stringify(a, null, ind);\n"
    "    if (t3 !== undefined && JSON.stringify(JSON.parse(t3)) !== t1) fail('indent round trip');\n"
    "  }\n"
    "  if (m & 16) {\n"
    "    if (JSON.stringify(JSON.parse(s, function (k, v) { return v; })) !== t1) fail('reviver');\n"
    "  }\n"
    "})";

int LLVMFuzzerTestOneInput(const uint8_t *buf, size_t len) {
  if (len < 1) return 0;
  JSRuntime *rt = JS_NewRuntime();
  if (!rt) exit(1);
  JS_SetMemoryLimit(rt, 64u << 20);
  JS_SetMaxStackSize(rt, 512u << 10);
  JSContext *ctx = JS_NewContext(rt);
  if (!ctx) exit(1);
  JSValue fn = JS_Eval(ctx, fz_check, sizeof(fz_check) - 1, "<fuzz-json>", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(fn)) abort();
  JSValue args[2];
  args[0] = JS_NewStringLen(ctx, (const char *)buf + 1, len - 1);
  args[1] = JS_NewInt32(ctx, buf[0]);
  JSValue v = JS_IsException(args[0]) ? JS_EXCEPTION : JS_Call(ctx, fn, JS_UNDEFINED, 2, args);
  if (JS_IsException(v)) {
    JSValue e = JS_GetException(ctx);
    const char *str = JS_IsString(e) ? JS_ToCString(ctx, e) : NULL;
    if (str && !strncmp(str, "FUZZFAIL", 8)) {
      fprintf(stderr, "%s\n", str);
      abort();
    }
    if (str) JS_FreeCString(ctx, str);
    JS_FreeValue(ctx, e);
  }
  JS_FreeValue(ctx, v);
  JS_FreeValue(ctx, args[0]);
  JS_FreeValue(ctx, fn);
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
  return 0;
}
