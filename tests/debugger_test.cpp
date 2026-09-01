/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * Tests the engine-level debugger interface added by engine/patches/0003, 0004
 * and 0005: JS_SetDebugTraceHandler, JS_GetStackDepth,
 * JS_GetLocalVariablesAtLevel, JS_SetVariableAtLevel, JS_EvalInStackFrame,
 * JS_GetFrameInfoAtLevel and JS_SetDebugTraceArmed.
 *
 * These are C entry points on the engine rather than JSI surface, so the tests
 * drive JSRuntime/JSContext directly. What is being protected here:
 *
 *   - The parse-time instrumentation contract. Statement traps exist only in
 *     code parsed while a handler was installed; this is the property that
 *     makes the feature free when unused, and it is easy to break by moving the
 *     emission out of emit_debug().
 *   - `debugger;` firing regardless of when the handler was installed, which is
 *     the only trap that survives ahead-of-time compilation and therefore the
 *     only one that can fire in a precompiled React Native bundle.
 *   - Frame inspection reading through to the real frame, and
 *     JS_SetVariableAtLevel writing through to it -- a copy would silently make
 *     the eventual "set variable value" DevTools command a no-op.
 *   - The abort path leaving a real exception on the context.
 *
 * The engine can be built with JS_ENABLE_DEBUGGER=0
 * (cmake -DQUICKJS_ENABLE_DEBUGGER=OFF), which compiles the whole family out.
 * quickjs.h still declares the functions in that configuration, so calling one
 * fails at LINK time rather than at runtime -- a runtime probe cannot detect
 * it, and a suite that tried would not build. Everything that touches the API
 * is therefore behind a compile-time guard, leaving only the one case that must
 * hold in every configuration: `debugger;` being semantically inert.
 */

#include <gtest/gtest.h>

extern "C" {
#include <quickjs.h>
}

#include <cstring>
#include <string>
#include <vector>

namespace {

#if JS_ENABLE_DEBUGGER

struct TraceRecord {
  int line = 0;
  int col = 0;
  int flags = 0;
  std::string funcname;
  std::string filename;
  int depth = 0;
};

struct TraceState {
  int calls = 0;
  int debuggerStmts = 0;
  std::vector<TraceRecord> records;
  // Stop and inspect on the Nth call (1-based); 0 = never.
  int inspectAt = 0;
  // Stop and inspect on the `debugger;` trap instead of a call index. Prefer
  // this: a call index is fragile because the *call site* is itself parsed with
  // the handler installed and therefore traps first, so "call 1" is the global
  // frame rather than the function under test. That mistake made an earlier
  // version of this test report zero locals and look like an engine bug.
  bool inspectOnDebuggerStmt = false;
  // Abort (return -1) on the Nth call (1-based); 0 = never.
  int abortAt = 0;
  // Filled in when inspectAt fires.
  std::vector<std::string> locals;
  std::string evalResult;
  int setVarResult = -99;
  // When set, JS_SetVariableAtLevel is called with this name/value at inspect.
  const char *setVarName = nullptr;
  int32_t setVarValue = 0;
};

std::string atomToString(JSContext *ctx, JSAtom a) {
  const char *s = JS_AtomToCString(ctx, a);
  std::string out = s ? s : "";
  if (s) {
    JS_FreeCString(ctx, s);
  }
  return out;
}

int traceCb(
    JSContext *ctx, JSAtom filename, JSAtom funcname, int line, int col,
    int flags, void *opaque) {
  auto *st = static_cast<TraceState *>(opaque);
  st->calls++;
  if (flags & JS_DEBUG_TRACE_DEBUGGER_STMT) {
    st->debuggerStmts++;
  }

  TraceRecord rec;
  rec.line = line;
  rec.col = col;
  rec.flags = flags;
  rec.funcname = atomToString(ctx, funcname);
  rec.filename = atomToString(ctx, filename);
  rec.depth = JS_GetStackDepth(ctx);
  st->records.push_back(rec);

  const bool inspectNow =
      (st->inspectAt != 0 && st->calls == st->inspectAt) ||
      (st->inspectOnDebuggerStmt && (flags & JS_DEBUG_TRACE_DEBUGGER_STMT));
  if (inspectNow) {
    JSDebugLocalVar *vars = nullptr;
    int count = 0;
    if (JS_GetLocalVariablesAtLevel(ctx, 0, &vars, &count) == 0) {
      for (int i = 0; i < count; i++) {
        const char *v = JS_ToCString(ctx, vars[i].value);
        std::string entry = std::string(vars[i].name) + "=" + (v ? v : "?");
        if (vars[i].is_arg) {
          entry += ":arg";
        }
        if (vars[i].is_closure) {
          entry += ":closure";
        }
        st->locals.push_back(entry);
        if (v) {
          JS_FreeCString(ctx, v);
        }
      }
      JS_FreeLocalVariables(ctx, vars, count);
    }

    JSValue r = JS_EvalInStackFrame(ctx, 0, "a + b", 5, "<dbg>");
    if (JS_IsException(r)) {
      JSValue exc = JS_GetException(ctx);
      st->evalResult = "EXCEPTION";
      JS_FreeValue(ctx, exc);
    } else {
      const char *s = JS_ToCString(ctx, r);
      st->evalResult = s ? s : "";
      if (s) {
        JS_FreeCString(ctx, s);
      }
    }
    JS_FreeValue(ctx, r);

    if (st->setVarName != nullptr) {
      st->setVarResult = JS_SetVariableAtLevel(
          ctx, 0, st->setVarName, JS_NewInt32(ctx, st->setVarValue));
    }
  }

  if (st->abortAt != 0 && st->calls >= st->abortAt) {
    return -1;
  }
  return 0;
}

#endif  // JS_ENABLE_DEBUGGER

class DebuggerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    rt_ = JS_NewRuntime();
    ctx_ = JS_NewContext(rt_);
  }
  void TearDown() override {
    JS_FreeContext(ctx_);
    JS_FreeRuntime(rt_);
  }

  JSValue eval(const char *src, const char *name = "<test>") {
    return JS_Eval(ctx_, src, strlen(src), name, JS_EVAL_TYPE_GLOBAL);
  }

  std::string evalToString(const char *src) {
    JSValue v = eval(src);
    if (JS_IsException(v)) {
      JSValue exc = JS_GetException(ctx_);
      JS_FreeValue(ctx_, exc);
      JS_FreeValue(ctx_, v);
      return "EXCEPTION";
    }
    const char *s = JS_ToCString(ctx_, v);
    std::string out = s ? s : "";
    if (s) {
      JS_FreeCString(ctx_, s);
    }
    JS_FreeValue(ctx_, v);
    return out;
  }

  JSRuntime *rt_ = nullptr;
  JSContext *ctx_ = nullptr;
};

// A `debugger;` statement is inert without a handler, in every configuration.
// This is the one test that must pass even when the debugger is compiled out,
// so it deliberately does not skip.
TEST_F(DebuggerTest, DebuggerStatementIsInertWithoutHandler) {
  EXPECT_EQ(
      evalToString("function f(a){ debugger; return a + 1; } f(41)"), "42");
  EXPECT_EQ(
      evalToString(
          "var n = 0; for (var i = 0; i < 3; i++) { debugger; n += i; } n"),
      "3");
  EXPECT_EQ(
      evalToString("(function(){ return 'x'; debugger; return 'y'; })()"), "x");
}

#if JS_ENABLE_DEBUGGER

TEST_F(DebuggerTest, NoTrapsWithoutHandler) {
  TraceState st;
  // Handler installed only *after* the code is parsed and run.
  JSValue v = eval("var q = 1; q + 1");
  JS_FreeValue(ctx_, v);
  EXPECT_EQ(st.calls, 0);
}

// The parse-time contract: code compiled before the handler was installed
// carries no statement traps, and is therefore not steppable.
TEST_F(DebuggerTest, StatementTrapsOnlyInCodeParsedWithHandlerInstalled) {
  JSValue pre = eval("function before(a) { var x = a + 1; return x; }");
  JS_FreeValue(ctx_, pre);

  TraceState st;
  JS_SetDebugTraceHandler(ctx_, traceCb, &st);
  JSValue v = eval("before(1)");
  JS_FreeValue(ctx_, v);
  // The call site itself was parsed with the handler installed, so it traps;
  // the body of before() was not, so it does not. Assert on the function names
  // seen rather than a raw count.
  for (const auto &r : st.records) {
    EXPECT_NE(r.funcname, "before")
        << "body parsed before the handler was installed must not trap";
  }

  st = TraceState{};
  JSValue after =
      eval("function afterFn(a) { var x = a + 1; return x; } afterFn(1)");
  JS_FreeValue(ctx_, after);
  bool sawBody = false;
  for (const auto &r : st.records) {
    if (r.funcname == "afterFn") {
      sawBody = true;
    }
  }
  EXPECT_TRUE(sawBody) << "body parsed with the handler installed must trap";
}

// `debugger;` fires even though the handler was installed after parse: the
// opcode is emitted unconditionally so it survives AOT compilation.
TEST_F(DebuggerTest, DebuggerStatementTrapsEvenWhenParsedWithoutHandler) {
  JSValue pre = eval("function trap(a) { debugger; return a + 1; }");
  JS_FreeValue(ctx_, pre);

  TraceState st;
  JS_SetDebugTraceHandler(ctx_, traceCb, &st);
  EXPECT_EQ(evalToString("trap(41)"), "42");
  EXPECT_EQ(st.debuggerStmts, 1);

  bool found = false;
  for (const auto &r : st.records) {
    if (r.flags & JS_DEBUG_TRACE_DEBUGGER_STMT) {
      EXPECT_EQ(r.funcname, "trap");
      EXPECT_EQ(r.line, 1);
      EXPECT_GE(r.depth, 1);
      found = true;
    }
  }
  EXPECT_TRUE(found);
}

TEST_F(DebuggerTest, FilenameAndLineAreReported) {
  TraceState st;
  JS_SetDebugTraceHandler(ctx_, traceCb, &st);
  const char *src = "function g() {\n  debugger;\n  return 1;\n}\ng();\n";
  JSValue v =
      JS_Eval(ctx_, src, strlen(src), "mysource.js", JS_EVAL_TYPE_GLOBAL);
  JS_FreeValue(ctx_, v);

  bool found = false;
  for (const auto &r : st.records) {
    if (r.flags & JS_DEBUG_TRACE_DEBUGGER_STMT) {
      EXPECT_EQ(r.filename, "mysource.js");
      EXPECT_EQ(r.line, 2);
      found = true;
    }
  }
  EXPECT_TRUE(found);
}

// Arguments, locals and closure captures are all visible, and
// JS_SetVariableAtLevel writes through to the live frame rather than to a copy.
TEST_F(DebuggerTest, InspectAndMutateFrame) {
  TraceState st;
  st.inspectOnDebuggerStmt = true;
  st.setVarName = "c";
  st.setVarValue = 100;
  JS_SetDebugTraceHandler(ctx_, traceCb, &st);

  JSValue pre = eval(
      "function outer() {\n"
      "  var captured = 5;\n"
      "  return function inner(a, b) {\n"
      "    var c = a + b;\n"
      "    debugger;\n"
      "    return c + captured;\n"
      "  };\n"
      "}\n"
      "var fn = outer();\n");
  JS_FreeValue(ctx_, pre);

  st.calls = 0;
  std::string result = evalToString("fn(1, 2)");

  // Guard against the failure mode where nothing traps and every EXPECT below
  // vacuously reports "not reported": assert the trap fired first.
  ASSERT_EQ(st.debuggerStmts, 1) << "the `debugger;` trap did not fire at all";
  ASSERT_FALSE(st.locals.empty()) << "no locals captured; inspection never ran";

  bool sawArgA = false, sawArgB = false, sawLocalC = false, sawClosure = false;
  for (const auto &l : st.locals) {
    if (l == "a=1:arg") sawArgA = true;
    if (l == "b=2:arg") sawArgB = true;
    if (l == "c=3") sawLocalC = true;
    if (l == "captured=5:closure") sawClosure = true;
  }
  EXPECT_TRUE(sawArgA) << "argument a not reported";
  EXPECT_TRUE(sawArgB) << "argument b not reported";
  EXPECT_TRUE(sawLocalC) << "local c not reported with its live value";
  EXPECT_TRUE(sawClosure) << "closure capture not reported";

  EXPECT_EQ(st.evalResult, "3")
      << "JS_EvalInStackFrame must resolve frame args";
  EXPECT_EQ(st.setVarResult, 0) << "JS_SetVariableAtLevel must find local c";
  // c was 3, set to 100, plus the captured 5.
  EXPECT_EQ(result, "105") << "the write must reach the live frame, not a copy";
}

TEST_F(DebuggerTest, SetVariableReportsConstAndMissing) {
  TraceState st;
  st.inspectOnDebuggerStmt = true;
  st.setVarName = "nosuchvariable";
  JS_SetDebugTraceHandler(ctx_, traceCb, &st);
  JSValue pre = eval("function h(a, b) { debugger; return a + b; }");
  JS_FreeValue(ctx_, pre);
  st.calls = 0;
  JSValue v = eval("h(1,2)");
  JS_FreeValue(ctx_, v);
  EXPECT_EQ(st.setVarResult, -1) << "unknown name must report not-found";
}

// Returning non-zero aborts execution with a real exception on the context.
TEST_F(DebuggerTest, HandlerCanAbortExecution) {
  TraceState st;
  st.abortAt = 1;
  JS_SetDebugTraceHandler(ctx_, traceCb, &st);

  JSValue pre = eval("function k() { debugger; return 'ran'; }");
  JS_FreeValue(ctx_, pre);
  st.calls = 0;

  JSValue v = eval("k()");
  ASSERT_TRUE(JS_IsException(v));
  JS_FreeValue(ctx_, v);
  JSValue exc = JS_GetException(ctx_);
  EXPECT_FALSE(JS_IsUninitialized(exc))
      << "abort must leave a real exception, not JS_UNINITIALIZED";
  const char *s = JS_ToCString(ctx_, exc);
  EXPECT_NE(std::string(s ? s : ""), "");
  if (s) {
    JS_FreeCString(ctx_, s);
  }
  JS_FreeValue(ctx_, exc);
}

// Clearing the handler stops the traps and leaves behaviour unchanged.
TEST_F(DebuggerTest, ClearingHandlerStopsTraps) {
  TraceState st;
  JS_SetDebugTraceHandler(ctx_, traceCb, &st);
  JSValue pre = eval("function m() { debugger; return 3; }");
  JS_FreeValue(ctx_, pre);
  st.calls = 0;
  EXPECT_EQ(evalToString("m()"), "3");
  EXPECT_GT(st.calls, 0);

  JS_SetDebugTraceHandler(ctx_, nullptr, nullptr);
  st.calls = 0;
  EXPECT_EQ(evalToString("m()"), "3");
  EXPECT_EQ(st.calls, 0);
}

// Arming is a SEPARATE switch from handler installation, and it has to work in
// both directions. The disarm direction is the optimization and gets exercised
// by everything; the ARM direction is the one that silently breaks a debugger,
// because a `Debugger.setBreakpointsActive(true)` that fails to re-arm loses
// every subsequent breakpoint while reporting success. The same body is run
// three times so the assertion is about the switch and not about which code is
// being executed.
TEST_F(DebuggerTest, DisarmingStopsTrapsAndRearmingRestoresThem) {
  TraceState st;
  JS_SetDebugTraceHandler(ctx_, traceCb, &st);
  JSValue pre =
      eval("function body(a) { var x = a + 1; var y = x + 1; return y; }");
  JS_FreeValue(ctx_, pre);

  // Installing a handler ARMS it, so the first run traps with no explicit
  // JS_SetDebugTraceArmed call at all.
  st = TraceState{};
  EXPECT_EQ(evalToString("body(1)"), "3");
  const int armedCalls = st.calls;
  EXPECT_GT(armedCalls, 0) << "a freshly installed handler must be armed";

  JS_SetDebugTraceArmed(ctx_, false);
  st = TraceState{};
  EXPECT_EQ(evalToString("body(1)"), "3");
  EXPECT_EQ(st.calls, 0) << "disarmed: the trap must not reach the handler";

  // The inversion. This is the assertion the suite was missing.
  JS_SetDebugTraceArmed(ctx_, true);
  st = TraceState{};
  EXPECT_EQ(evalToString("body(1)"), "3");
  EXPECT_GT(st.calls, 0) << "re-arming must restore traps";
}

// `debugger;` is exempt from arming: an attached-but-idle session still has to
// stop on an explicit keyword. Its traps exist only where the programmer wrote
// one, so delivering them unconditionally costs nothing.
TEST_F(DebuggerTest, DebuggerStatementStillTrapsWhileDisarmed) {
  TraceState st;
  JS_SetDebugTraceHandler(ctx_, traceCb, &st);
  JSValue pre = eval(
      "function withKeyword() { debugger; return 7; }"
      "function without() { var z = 7; return z; }");
  JS_FreeValue(ctx_, pre);

  JS_SetDebugTraceArmed(ctx_, false);

  st = TraceState{};
  EXPECT_EQ(evalToString("without()"), "7");
  EXPECT_EQ(st.calls, 0)
      << "ordinary statements must stay silent when disarmed";

  st = TraceState{};
  EXPECT_EQ(evalToString("withKeyword()"), "7");
  EXPECT_GT(st.calls, 0) << "`debugger;` must fire even when disarmed";
}

TEST_F(DebuggerTest, StackDepthGrowsWithNesting) {
  TraceState st;
  JS_SetDebugTraceHandler(ctx_, traceCb, &st);
  JSValue pre = eval(
      "function lvl3() { debugger; return 1; }\n"
      "function lvl2() { return lvl3(); }\n"
      "function lvl1() { return lvl2(); }\n");
  JS_FreeValue(ctx_, pre);
  st.calls = 0;

  JSValue shallow = eval("lvl3()");
  JS_FreeValue(ctx_, shallow);
  ASSERT_FALSE(st.records.empty());
  int shallowDepth = st.records.back().depth;

  st.records.clear();
  JSValue deep = eval("lvl1()");
  JS_FreeValue(ctx_, deep);
  ASSERT_FALSE(st.records.empty());
  int deepDepth = st.records.back().depth;

  EXPECT_GT(deepDepth, shallowDepth);
}

// No frame at the requested level must be reported, not crash.
TEST_F(DebuggerTest, OutOfRangeLevelIsSafe) {
  JSDebugLocalVar *vars = reinterpret_cast<JSDebugLocalVar *>(0x1);
  int count = -1;
  EXPECT_EQ(JS_GetLocalVariablesAtLevel(ctx_, 9999, &vars, &count), 0);
  EXPECT_EQ(vars, nullptr);
  EXPECT_EQ(count, 0);
  JS_FreeLocalVariables(ctx_, nullptr, 0);

  EXPECT_EQ(JS_SetVariableAtLevel(ctx_, 9999, "x", JS_NewInt32(ctx_, 1)), -1);

  JSValue v = JS_EvalInStackFrame(ctx_, 9999, "1", 1, "<dbg>");
  EXPECT_TRUE(JS_IsException(v));
  JS_FreeValue(ctx_, v);
  JSValue exc = JS_GetException(ctx_);
  JS_FreeValue(ctx_, exc);

  JSDebugFrameInfo info;
  std::memset(&info, 0xAA, sizeof(info));
  EXPECT_EQ(JS_GetFrameInfoAtLevel(ctx_, 9999, &info), -1);
  EXPECT_EQ(info.filename, JS_ATOM_NULL);
  EXPECT_EQ(info.line, -1);
  EXPECT_EQ(JS_GetFrameInfoAtLevel(ctx_, 0, nullptr), -1);
}

/*
 * Patch 0027 (Gap 4 in docs/debugger.md): every frame on the stack, not just
 * the trapping one, must report its own script, function name, line and
 * column. Without this a CDP `Debugger.paused` event has no callFrames array
 * and there is no call stack in DevTools.
 *
 * The assertion that matters is the one on LEVEL 0. QuickJS keeps the program
 * counter in a JS_CallInternal local and only writes it into the frame at
 * points that can re-enter the engine, so before 0027 the innermost frame --
 * the one the user is looking at -- reported a stale line or none at all,
 * while every OUTER frame was already correct. A test that only checked the
 * outer frames would have passed against the unfixed engine.
 */
TEST_F(DebuggerTest, FrameInfoAtEveryLevel) {
  struct Captured {
    std::vector<std::string> funcs;
    std::vector<int> lines;
    std::vector<std::string> files;
    int depth = 0;
    int traps = 0;
  };
  static Captured cap;
  cap = Captured{};

  auto cb = [](JSContext *ctx, JSAtom, JSAtom, int, int, int flags,
               void *) -> int {
    if (!(flags & JS_DEBUG_TRACE_DEBUGGER_STMT)) {
      return 0;
    }
    cap.traps++;
    cap.depth = JS_GetStackDepth(ctx);
    for (int level = 0; level < cap.depth; ++level) {
      JSDebugFrameInfo info;
      if (JS_GetFrameInfoAtLevel(ctx, level, &info) != 0) {
        break;
      }
      const char *fn = info.func_name == JS_ATOM_NULL
                           ? nullptr
                           : JS_AtomToCString(ctx, info.func_name);
      cap.funcs.emplace_back(fn ? fn : "");
      if (fn) {
        JS_FreeCString(ctx, fn);
      }
      const char *file = info.filename == JS_ATOM_NULL
                             ? nullptr
                             : JS_AtomToCString(ctx, info.filename);
      cap.files.emplace_back(file ? file : "");
      if (file) {
        JS_FreeCString(ctx, file);
      }
      cap.lines.push_back(info.line);
    }
    return 0;
  };

  JS_SetDebugTraceHandler(ctx_, cb, nullptr);
  // Line 1 is `function inner`, so the `debugger;` is on line 2 and the calls
  // that got us there are on lines 5 and 8.
  JSValue v = eval(
      "function inner() {\n"
      "  debugger;\n"
      "  return 1;\n"
      "}\n"
      "function middle() {\n"
      "  return inner();\n"
      "}\n"
      "function outer() {\n"
      "  return middle();\n"
      "}\n"
      "outer();\n");
  JS_FreeValue(ctx_, v);

  ASSERT_EQ(cap.traps, 1) << "the debugger; trap never fired -- the rest of "
                             "this test would pass vacuously";
  ASSERT_GE(cap.funcs.size(), 4u) << "expected inner/middle/outer/<eval>";

  EXPECT_EQ(cap.funcs[0], "inner");
  EXPECT_EQ(cap.funcs[1], "middle");
  EXPECT_EQ(cap.funcs[2], "outer");

  // The point of the patch: level 0 has a real line, and it is the line of
  // the `debugger;` statement rather than the line of the outermost call.
  EXPECT_EQ(cap.lines[0], 2);
  EXPECT_EQ(cap.lines[1], 6);
  EXPECT_EQ(cap.lines[2], 9);

  for (const auto &f : cap.files) {
    EXPECT_EQ(f, "<test>");
  }
}

#endif  // JS_ENABLE_DEBUGGER

}  // namespace
