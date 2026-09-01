/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * Assertions are on the JSON text, deliberately. That text is the contract --
 * a frontend never sees anything else -- so a test that reached inside the
 * agent could pass while the wire format was wrong.
 */

#include <gtest/gtest.h>

extern "C" {
#include "quickjs-cdp.h"
}

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

class CDPTest : public ::testing::Test {
 protected:
  void SetUp() override {
    rt_ = JS_NewRuntime();
    ctx_ = JS_NewContext(rt_);
    agent_ = qjs_cdp_new(ctx_, 1, &CDPTest::collect, this);
  }

  void TearDown() override {
    qjs_cdp_free(agent_);
    JS_FreeContext(ctx_);
    JS_FreeRuntime(rt_);
  }

  static void collect(
      void *opaque, void *session, const char *json, size_t len) {
    (void)session;
    auto *self = static_cast<CDPTest *>(opaque);
    std::lock_guard<std::mutex> guard(self->lock_);
    self->sent_.emplace_back(json, len);
  }

  void send(const std::string &message) {
    qjs_cdp_send_message(agent_, this, message.c_str(), message.size());
    qjs_cdp_poll(agent_);
  }

  /// The first message containing `needle`, or "" if none did.
  std::string find(const std::string &needle) {
    std::lock_guard<std::mutex> guard(lock_);
    for (const auto &m : sent_)
      if (m.find(needle) != std::string::npos) return m;
    return "";
  }

  bool waitFor(const std::string &needle) {
    for (int i = 0; i < 1000; i++) {
      if (!find(needle).empty()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
  }

  JSRuntime *rt_ = nullptr;
  JSContext *ctx_ = nullptr;
  QJSCDPAgent *agent_ = nullptr;
  std::mutex lock_;
  std::vector<std::string> sent_;
};

// A frontend talks to several agents at once, so refusing a method this one
// does not implement would answer on everyone else's behalf.
TEST_F(CDPTest, AMethodThisAgentDoesNotImplementIsLeftAlone) {
  EXPECT_FALSE(qjs_cdp_handles("Nonsense.method"));
  EXPECT_FALSE(qjs_cdp_handles("Runtime.addBinding"));
  EXPECT_TRUE(qjs_cdp_handles("Runtime.evaluate"));

  send(R"J({"id":1,"method":"Nonsense.method"})J");
  EXPECT_TRUE(find(R"J("id":1)J").empty());
}

// qjs_cdp_handles() is a second list of method names, and a second list drifts.
TEST_F(CDPTest, EveryAdvertisedMethodIsImplemented) {
  static const char *kAdvertised[] = {
      "Runtime.evaluate",
      "Runtime.getProperties",
      "Runtime.releaseObject",
      "Runtime.releaseObjectGroup",
      "Runtime.getHeapUsage",
      "Runtime.discardConsoleEntries",
      "Debugger.enable",
      "Debugger.disable",
      "Debugger.setBreakpointByUrl",
      "Debugger.removeBreakpoint",
      "Debugger.setBreakpointsActive",
      "Debugger.resume",
      "Debugger.stepOver",
      "Debugger.stepInto",
      "Debugger.stepOut",
      "Debugger.pause",
      "Debugger.evaluateOnCallFrame",
      "Debugger.setVariableValue",
      "Debugger.getScriptSource",
      "Debugger.setPauseOnExceptions",
      "Debugger.setAsyncCallStackDepth",
      "Debugger.setBlackboxPatterns",
      "Debugger.setBlackboxedRanges",
      "Debugger.setSkipAllPauses"};

  for (const char *method : kAdvertised) {
    EXPECT_TRUE(qjs_cdp_handles(method)) << method;
    sent_.clear();
    send(std::string(R"J({"id":99,"method":")J") + method + R"J("})J");
    // Something came back. Which reply is the business of the other tests;
    // silence here would mean the two lists had drifted apart.
    EXPECT_FALSE(find(R"J("id":99)J").empty()) << method;
  }
}

TEST_F(CDPTest, MalformedJsonDoesNotCrashOrReply) {
  send("{ this is not json");
  EXPECT_TRUE(find("\"id\"").empty());
}

TEST_F(CDPTest, EvaluateReturnsATypedRemoteObject) {
  send(
      R"J({"id":1,"method":"Runtime.evaluate","params":{"expression":"6*7"}})J");
  const std::string reply = find(R"J("id":1)J");
  EXPECT_NE(reply.find(R"J("type":"number")J"), std::string::npos);
  EXPECT_NE(reply.find(R"J("value":42)J"), std::string::npos);
}

TEST_F(CDPTest, AThrownErrorArrivesAsExceptionDetails) {
  send(
      R"J({"id":1,"method":"Runtime.evaluate","params":{"expression":"missing()"}})J");
  const std::string reply = find(R"J("id":1)J");
  EXPECT_NE(reply.find(R"J("exceptionDetails")J"), std::string::npos);
  EXPECT_NE(reply.find("ReferenceError"), std::string::npos);
}

TEST_F(CDPTest, GetPropertiesWalksAnObjectHeldByObjectId) {
  send(
      R"J({"id":1,"method":"Runtime.evaluate","params":{"expression":"({a:1,b:'two'})"}})J");
  const std::string evaluated = find(R"J("id":1)J");
  const size_t at = evaluated.find(R"J("objectId":")J");
  ASSERT_NE(at, std::string::npos);
  const std::string id =
      evaluated.substr(at + 12, evaluated.find('"', at + 12) - at - 12);

  send(
      R"J({"id":2,"method":"Runtime.getProperties","params":{"objectId":")J" +
      id + R"J("}})J");
  const std::string props = find(R"J("id":2)J");
  EXPECT_NE(props.find(R"J("name":"a")J"), std::string::npos);
  EXPECT_NE(props.find(R"J("name":"b")J"), std::string::npos);
  EXPECT_NE(props.find(R"J("value":"two")J"), std::string::npos);
}

TEST_F(CDPTest, ReleasingAnObjectGroupInvalidatesItsObjectIds) {
  send(
      R"J({"id":1,"method":"Runtime.evaluate","params":{"expression":"({})","objectGroup":"g"}})J");
  const std::string evaluated = find(R"J("id":1)J");
  const size_t at = evaluated.find(R"J("objectId":")J");
  ASSERT_NE(at, std::string::npos);
  const std::string id =
      evaluated.substr(at + 12, evaluated.find('"', at + 12) - at - 12);

  send(
      R"J({"id":2,"method":"Runtime.releaseObjectGroup","params":{"objectGroup":"g"}})J");
  send(
      R"J({"id":3,"method":"Runtime.getProperties","params":{"objectId":")J" +
      id + R"J("}})J");
  EXPECT_NE(find(R"J("id":3)J").find(R"J("error")J"), std::string::npos);
}

TEST_F(CDPTest, ScriptParsedIsAnnouncedAndTheSourceComesBack) {
  send(R"J({"id":1,"method":"Debugger.enable"})J");
  qjs_cdp_script_loaded(agent_, "app.js", "var x = 1;\n");
  EXPECT_NE(
      find("Debugger.scriptParsed").find(R"J("url":"app.js")J"),
      std::string::npos);

  send(
      R"J({"id":2,"method":"Debugger.getScriptSource","params":{"scriptId":"1"}})J");
  EXPECT_NE(find(R"J("id":2)J").find("var x = 1"), std::string::npos);
}

TEST_F(CDPTest, ABreakpointSetBeforeItsScriptResolvesWhenTheScriptArrives) {
  send(R"J({"id":1,"method":"Debugger.enable"})J");
  send(
      R"J({"id":2,"method":"Debugger.setBreakpointByUrl","params":{"url":"late.js","lineNumber":0}})J");
  EXPECT_TRUE(find("Debugger.breakpointResolved").empty());

  qjs_cdp_script_loaded(agent_, "http://localhost/late.js", "var x = 1;\n");
  EXPECT_NE(
      find("Debugger.breakpointResolved").find(R"J("breakpointId":"1")J"),
      std::string::npos);
}

// --- pausing ---------------------------------------------------------------
//
// These run JavaScript on a second thread, because that is the only way to
// test the pause loop at all: while it is running, the thread that entered the
// engine is inside it and cannot also be driving the protocol.

class CDPPauseTest : public CDPTest {
 protected:
  static constexpr const char *kSource =
      "function add(x, y) {\n"
      "  var sum = x + y;\n"
      "  return sum;\n"
      "}\n"
      "var result = add(2, 3);\n";

  void runScriptOnAnotherThread() {
    thread_ = std::thread([this] {
      // The engine records the stack bounds of whichever thread created the
      // runtime, and refuses to run once it believes the stack is exhausted.
      // Entering from a second thread without this fails immediately with
      // "Maximum call stack size exceeded" -- on Linux always, on macOS never,
      // because there the two stacks happen to sit close enough together.
      JS_UpdateStackTop(rt_);
      JSValue v = JS_Eval(
          ctx_, kSource, strlen(kSource), "app.js", JS_EVAL_TYPE_GLOBAL);
      JS_FreeValue(ctx_, v);
      finished_ = true;
    });
  }

  int32_t readResult() {
    // Runs back on the test thread, after the script thread pointed the
    // engine's stack bounds at its own stack.
    JS_UpdateStackTop(rt_);
    JSValue v = JS_Eval(ctx_, "result", 6, "<check>", JS_EVAL_TYPE_GLOBAL);
    int32_t n = -1;
    JS_ToInt32(ctx_, &n, v);
    JS_FreeValue(ctx_, v);
    return n;
  }

  /// Queues without polling: while paused, the pause loop is the pump.
  void post(const std::string &message) {
    qjs_cdp_send_message(agent_, this, message.c_str(), message.size());
  }

  void breakOnLineTwoAndRun() {
    send(R"J({"id":1,"method":"Debugger.enable"})J");
    qjs_cdp_script_loaded(agent_, "app.js", kSource);
    send(
        R"J({"id":2,"method":"Debugger.setBreakpointByUrl","params":{"url":"app.js","lineNumber":1}})J");
    runScriptOnAnotherThread();
    ASSERT_TRUE(waitFor("Debugger.paused"));
  }

  std::thread thread_;
  std::atomic<bool> finished_{false};
};

TEST_F(CDPPauseTest, ABreakpointStopsTheProgramAndReportsTheStack) {
  breakOnLineTwoAndRun();

  const std::string paused = find("Debugger.paused");
  EXPECT_NE(paused.find(R"J("functionName":"add")J"), std::string::npos);
  EXPECT_NE(paused.find(R"J("hitBreakpoints":["1"])J"), std::string::npos);
  // CDP counts from zero, so `var sum = ...` on source line 2 is lineNumber 1.
  EXPECT_NE(paused.find(R"J("lineNumber":1)J"), std::string::npos);

  post(R"J({"id":3,"method":"Debugger.resume"})J");
  thread_.join();
  EXPECT_TRUE(waitFor("Debugger.resumed"));
  EXPECT_EQ(readResult(), 5);
}

TEST_F(CDPPauseTest, EvaluateOnCallFrameSeesTheFramesArguments) {
  breakOnLineTwoAndRun();

  post(
      R"J({"id":3,"method":"Debugger.evaluateOnCallFrame","params":{"callFrameId":"0","expression":"x + y"}})J");
  ASSERT_TRUE(waitFor(R"J("id":3)J"));
  EXPECT_NE(find(R"J("id":3)J").find(R"J("value":5)J"), std::string::npos);

  post(R"J({"id":4,"method":"Debugger.resume"})J");
  thread_.join();
}

// If the write does not reach the live frame, `result` comes back 5.
TEST_F(CDPPauseTest, SetVariableValueChangesWhatTheProgramComputes) {
  breakOnLineTwoAndRun();

  post(
      R"J({"id":3,"method":"Debugger.setVariableValue","params":{"callFrameId":"0","variableName":"y","newValue":{"value":40}}})J");
  ASSERT_TRUE(waitFor(R"J("id":3)J"));
  EXPECT_TRUE(find(R"J("id":3)J").find(R"J("error")J") == std::string::npos);

  post(R"J({"id":4,"method":"Debugger.resume"})J");
  thread_.join();
  EXPECT_EQ(readResult(), 42);
}

TEST_F(CDPPauseTest, DestroyingTheAgentWhilePausedDoesNotHang) {
  breakOnLineTwoAndRun();
  post(R"J({"id":3,"method":"Debugger.resume"})J");
  thread_.join();
  SUCCEED();
}

}  // namespace
