/* Copyright (c) Ammar Ahmed. */

#include <quickjs.h>

#include <cstdio>
#include <string>

namespace {

bool runWithLimit(size_t extra, bool *parsed, bool *failed_out) {
  JSRuntime *rt = JS_NewRuntime();
  if (!rt) return false;
  JSContext *ctx = JS_NewContext(rt);
  if (!ctx) {
    JS_FreeRuntime(rt);
    return false;
  }

  const size_t baseline = JS_GetMallocSize(rt);
  JS_SetMemoryLimit(rt, baseline + extra);
  const std::string source = "JSON.parse('{\"value\":1,\"padding\":\"" +
                             std::string(8192, 'x') + "\"}')";
  JSValue value = JS_Eval(
      ctx, source.c_str(), source.size(), "oom-json.js", JS_EVAL_TYPE_GLOBAL);
  const bool failed = JS_IsException(value);
  *failed_out = failed;
  if (failed) {
    JSValue error = JS_GetException(ctx);
    JS_FreeValue(ctx, error);
  } else {
    *parsed = true;
    JS_FreeValue(ctx, value);
  }

  JSValue sanity = JS_Eval(ctx, "1 + 1", 5, "sanity.js", JS_EVAL_TYPE_GLOBAL);
  const bool healthy = !JS_IsException(sanity);
  if (JS_IsException(sanity)) {
    JSValue error = JS_GetException(ctx);
    JS_FreeValue(ctx, error);
  } else {
    JS_FreeValue(ctx, sanity);
  }
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
  return healthy || failed;
}

}  // namespace

int main() {
  bool parsed = false;
  bool failed = false;
  for (size_t extra :
       {size_t(0), size_t(4096), size_t(32768), size_t(131072)}) {
    bool failed_this_run = false;
    if (!runWithLimit(extra, &parsed, &failed_this_run)) return 1;
    failed = failed || failed_this_run;
  }
  if (!parsed || !failed) {
    std::fprintf(
        stderr, "lazy JSON allocation-failure coverage was incomplete\n");
    return 1;
  }
  return 0;
}
