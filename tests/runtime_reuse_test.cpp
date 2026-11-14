// Runtimes made one after another in one process. The first JS_FreeRuntime used to leave a process-wide flag set that
// turned off the native-stack scan for every later runtime, so a young object held only by a stack-VM frame or a C
// local was not a root of the young collection (engine/patches/0618). QJS_Y_CHECK makes the young collection report
// such an object ("unrecorded root"); this test runs the same script in three runtimes with it on and fails on a
// report or a wrong result. Then two threads each make and free three runtimes, so teardowns overlap: the flag that
// keeps a runtime's final collection off its (stale) stack was one process-wide pointer, and a second thread's
// teardown overwrote the first's (engine/patches/0622). Its own executable: QJS_Y_CHECK is read once per process.
// No gtest, like json_oom_test.cpp.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

#include "quickjs.h"

static const char kSrc[] =
    "var bad = 0, sum = 0;\n"
    "function churn(n) { var a = []; for (var j = 0; j < n; j++) a.push({ j: j, k: [j] }); return a.length; }\n"
    "for (let i = 0; i < 3000; i++) {\n"
    "  const held = { v: i, w: { x: i * 2 }, f: function () { return i; } };\n"
    "  sum += churn(200);\n"
    "  if (held.v !== i || held.w.x !== i * 2 || held.f() !== i) bad++;\n"
    "}\n"
    "bad + ':' + sum;\n";

static bool RunOnce(int n) {
  JSRuntime *rt = JS_NewRuntime();
  JSContext *ctx = JS_NewContext(rt);
  JSValue v = JS_Eval(ctx, kSrc, strlen(kSrc), "<reuse>", JS_EVAL_TYPE_GLOBAL);
  JSValue shown = JS_IsException(v) ? JS_GetException(ctx) : JS_DupValue(ctx, v);
  const char *s = JS_ToCString(ctx, shown);
  bool ok = s && std::strcmp(s, "0:600000") == 0;
  if (!ok) std::fprintf(stdout, "runtime %d: %s\n", n, s ? s : "(null)");
  JS_FreeCString(ctx, s);
  JS_FreeValue(ctx, shown);
  JS_FreeValue(ctx, v);
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
  return ok;
}

int main() {
  setenv("QJS_Y_CHECK", "1", 1);
  // The young collection reports on stderr: send it to a file in the working directory and read it back.
  const char *log = "runtime_reuse_test.stderr";
  fflush(stderr);
  int saved = dup(2);
  int fd = open(log, O_CREAT | O_TRUNC | O_RDWR, 0644);
  if (fd < 0 || saved < 0) return 2;
  dup2(fd, 2);
  bool ok = true;
  for (int i = 1; i <= 3; i++) ok &= RunOnce(i);
  bool tok[2] = {true, true};
  std::thread t0([&] { for (int i = 0; i < 3; i++) tok[0] &= RunOnce(10 + i); });
  std::thread t1([&] { for (int i = 0; i < 3; i++) tok[1] &= RunOnce(20 + i); });
  t0.join();
  t1.join();
  ok &= tok[0] && tok[1];
  fflush(stderr);
  dup2(saved, 2);
  close(saved);
  std::string text;
  lseek(fd, 0, SEEK_SET);
  char buf[4096];
  ssize_t k;
  while ((k = read(fd, buf, sizeof buf)) > 0) text.append(buf, (size_t)k);
  close(fd);
  unlink(log);
  if (text.find("unrecorded root") != std::string::npos) {
    std::fprintf(stdout, "young objects held by a frame were not roots:\n%s", text.c_str());
    ok = false;
  }
  std::fprintf(stdout, "%s\n", ok ? "runtime_reuse: ok" : "runtime_reuse: FAILED");
  return ok ? 0 : 1;
}
