// RegExp fuzzer (libFuzzer). Input: one flags byte, then the pattern up to the first NUL byte, then the subject. Beyond
// crashes, it aborts when a search disagrees with a scan of sticky matches: exec without `y` must report the first
// position (a code point boundary under `u`/`v`) where the sticky match succeeds, with the same match. The subject is
// tried as given and, with bit 6, widened to 16 bits by a trailing U+0100. Bit 7 also runs replace/split/matchAll.
//   clang -g -O1 -fsanitize=address,undefined,fuzzer -I../../engine/quickjs-ng [-DQJS_REGVM=1] -o fuzz-regexp fuzz-regexp.c
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
  return ++fz_ticks > 200;  // catastrophic backtracking ends with an exception (a tick is 10,000 matcher steps)
}

static const char fz_check[] =
    "(function (pat, sub, m) {\n"
    "  function fail(w) { throw 'FUZZFAIL ' + w; }\n"
    "  var fl = '';\n"
    "  if (m & 1) fl += 'i';\n"
    "  if (m & 2) fl += 'm';\n"
    "  if (m & 4) fl += 's';\n"
    "  if (m & 8) fl += 'd';\n"
    "  if (m & 16) fl += 'u'; else if (m & 32) fl += 'v';\n"
    "  var re, ry;\n"
    "  try { re = new RegExp(pat, fl + 'g'); ry = new RegExp(pat, fl + 'y'); } catch (e) {\n"
    "    if (!(e instanceof SyntaxError)) fail('compile threw ' + e); return;\n"
    "  }\n"
    "  var uni = (m & 48) !== 0;\n"
    "  function same(x, y) {\n"
    "    if (x === null || y === null) return x === y;\n"
    "    if (x.index !== y.index || x.length !== y.length) return false;\n"
    "    for (var k = 0; k < x.length; k++) if (x[k] !== y[k]) return false;\n"
    "    return true;\n"
    "  }\n"
    "  function check(s) {\n"
    "    re.lastIndex = 0;\n"
    "    var r = re.exec(s);\n"
    "    var want = null;\n"
    "    for (var i = 0; i <= s.length; i++) {\n"
    "      if (uni && i > 0 && i < s.length) {\n"
    "        var hi = s.charCodeAt(i - 1), lo = s.charCodeAt(i);\n"
    "        if (hi >= 0xd800 && hi < 0xdc00 && lo >= 0xdc00 && lo < 0xe000) continue;\n"
    "      }\n"
    "      ry.lastIndex = i;\n"
    "      want = ry.exec(s);\n"
    "      if (want) break;\n"
    "    }\n"
    "    if (!same(r, want)) fail('exec vs sticky scan /' + pat + '/' + fl + ' at ' + (r ? r.index : -1) + ' want ' + (want ? want.index : -1));\n"
    "    re.lastIndex = 0;\n"
    "    if (re.test(s) !== (want !== null)) fail('test');\n"
    "    if (m & 128) {\n"
    "      s.replace(re, '<$&>'); s.split(re); s.search(re); [...s.matchAll(re)];\n"
    "      s.replace(re, function () { return '-'; });\n"
    "    }\n"
    "  }\n"
    "  check(sub);\n"
    "  if (m & 64) check(sub + '\\u0100');\n"
    "})";

int LLVMFuzzerTestOneInput(const uint8_t *buf, size_t len) {
  if (len < 1) return 0;
  const uint8_t *p = buf + 1, *end = buf + len;
  const uint8_t *nul = memchr(p, 0, end - p);
  size_t plen = nul ? (size_t)(nul - p) : (size_t)(end - p);
  const uint8_t *s = nul ? nul + 1 : end;
  if (plen > 256 || end - s > 512) return 0;
  JSRuntime *rt = JS_NewRuntime();
  if (!rt) exit(1);
  JS_SetMemoryLimit(rt, 64u << 20);
  JS_SetMaxStackSize(rt, 512u << 10);
  fz_ticks = 0;
  JSContext *ctx = JS_NewContext(rt);
  if (!ctx) exit(1);
  JSValue fn = JS_Eval(ctx, fz_check, sizeof(fz_check) - 1, "<fuzz-regexp>", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(fn)) abort();
  JS_SetInterruptHandler(rt, fz_interrupt, NULL);
  JSValue args[3];
  args[0] = JS_NewStringLen(ctx, (const char *)p, plen);
  args[1] = JS_NewStringLen(ctx, (const char *)s, end - s);
  args[2] = JS_NewInt32(ctx, buf[0]);
  JSValue v = JS_Call(ctx, fn, JS_UNDEFINED, 3, args);
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
  JS_FreeValue(ctx, args[1]);
  JS_FreeValue(ctx, fn);
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
  return 0;
}
