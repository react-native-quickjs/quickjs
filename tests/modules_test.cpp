/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * The QuickJS module ABI: registration, ordering, deduplication, the escape
 * hatch to the engine, and the React Native compatibility shim.
 *
 * The modules themselves live in modules/ as standalone packages and bring
 * their own conformance suites; this file is only about the surface they are
 * written against.
 */

#include <gtest/gtest.h>
#include <jsi/jsi.h>

#include <memory>
#include <string>

#include "QuickJSModule.h"
#include "QuickJSModuleNative.h"
#include "QuickJSRuntimeFactory.h"

using namespace facebook;

namespace {

class ModuleTest : public ::testing::Test {
 protected:
  void SetUp() override {
    runtime_ = qjs::makeQuickJSRuntime();
  }

  jsi::Value eval(const char *source) {
    return runtime_->evaluateJavaScript(
        std::make_shared<jsi::StringBuffer>(source), "test.js");
  }

  std::string evalString(const char *source) {
    return eval(source).asString(*runtime_).utf8(*runtime_);
  }

  bool evalBool(const char *source) {
    return eval(source).getBool();
  }

  std::unique_ptr<jsi::Runtime> runtime_;
};

// --- the ABI ---------------------------------------------------------------

int installCount = 0;
int installOrderNext = 0;
int firstInstalledOrder = -1;
int secondInstalledOrder = -1;

void countingInstall(jsi::Runtime &) {
  installCount++;
}

void lowPriorityInstall(jsi::Runtime &) {
  secondInstalledOrder = installOrderNext++;
}

void highPriorityInstall(jsi::Runtime &) {
  firstInstalledOrder = installOrderNext++;
}

TEST(ModuleRegistry, RegistersAndInstalls) {
  installCount = 0;
  qjs::registerModule("test-counting", countingInstall);

  auto runtime = qjs::makeQuickJSRuntime();
  EXPECT_GE(installCount, 1) << "module was registered but never installed";
}

TEST(ModuleRegistry, IgnoresDuplicateNames) {
  const size_t before = qjs::registeredModules().size();
  qjs::registerModule("test-duplicate", countingInstall);
  const size_t afterFirst = qjs::registeredModules().size();
  qjs::registerModule("test-duplicate", countingInstall);
  const size_t afterSecond = qjs::registeredModules().size();

  EXPECT_EQ(afterFirst, before + 1);
  EXPECT_EQ(afterSecond, afterFirst);
}

TEST(ModuleRegistry, IgnoresIncompleteRegistrations) {
  const size_t before = qjs::registeredModules().size();
  qjs::registerModule("test-no-installer", nullptr);
  qjs::registerModule(nullptr, countingInstall);
  EXPECT_EQ(qjs::registeredModules().size(), before);
}

TEST(ModuleRegistry, InstallsInPriorityOrder) {
  firstInstalledOrder = secondInstalledOrder = -1;
  installOrderNext = 0;

  // Registered low-priority-first, so if ordering were merely registration
  // order this test would fail.
  qjs::registerModule("test-order-low", lowPriorityInstall, 10);
  qjs::registerModule("test-order-high", highPriorityInstall, -10);

  auto runtime = qjs::makeQuickJSRuntime();

  ASSERT_NE(firstInstalledOrder, -1);
  ASSERT_NE(secondInstalledOrder, -1);
  EXPECT_LT(firstInstalledOrder, secondInstalledOrder);
}

std::string tiedOrder;

void tiedA(jsi::Runtime &) {
  tiedOrder += "a";
}
void tiedB(jsi::Runtime &) {
  tiedOrder += "b";
}
void tiedC(jsi::Runtime &) {
  tiedOrder += "c";
}

TEST(ModuleRegistry, EqualPrioritiesKeepRegistrationOrder) {
  // The sort is stable, so a module author who leaves priority alone gets the
  // order they wrote. An unstable sort would pass InstallsInPriorityOrder and
  // still shuffle these three.
  qjs::registerModule("test-tied-a", tiedA, 5);
  qjs::registerModule("test-tied-b", tiedB, 5);
  qjs::registerModule("test-tied-c", tiedC, 5);

  tiedOrder.clear();
  auto runtime = qjs::makeQuickJSRuntime();
  EXPECT_EQ(tiedOrder, "abc");
}

TEST(ModuleRegistry, ContextEscapeHatchResolves) {
  auto runtime = qjs::makeQuickJSRuntime();
  // A module that drops to the engine depends on this being non-null; if it
  // ever returned null for our own runtime, every such module would silently
  // fall back to its slow path.
  EXPECT_NE(qjs::contextFromRuntime(*runtime), nullptr);
  EXPECT_NE(qjs::quickJSRuntime(*runtime), nullptr);
}

// --- the engine escape hatch -----------------------------------------------

namespace {

JSValue thingConstructor(
    JSContext *ctx, JSValueConst newTarget, int argc, JSValueConst *argv) {
  JSValue proto = JS_GetPropertyStr(ctx, newTarget, "prototype");
  JSValue self = JS_NewObjectProto(ctx, proto);
  JS_FreeValue(ctx, proto);
  JS_SetPropertyStr(ctx, self, "made", JS_NewInt32(ctx, 1));
  return self;
}

}  // namespace

TEST_F(ModuleTest, EngineConstructorsAreRealConstructors) {
  /*
   * The reason QuickJSModuleNative.h exists. A jsi::Function from
   * createFromHostFunction is not a constructor: `new` against it does not
   * fail loudly, `instanceof` is false, and `class S extends Thing {}` gives
   * undefined from `new S()`. JSI cannot express the difference, so a module
   * that needs a real constructor has to reach the engine.
   */
  JSContext *ctx = qjs::contextFromRuntime(*runtime_);
  ASSERT_NE(ctx, nullptr);

  JSValue ctor = JS_NewCFunction2(
      ctx, reinterpret_cast<JSCFunction *>(thingConstructor), "Thing", 0,
      JS_CFUNC_constructor, 0);
  JSValue proto = JS_NewObject(ctx);
  JS_SetConstructor(ctx, ctor, proto);
  JS_FreeValue(ctx, proto);
  runtime_->global().setProperty(
      *runtime_, "Thing", qjs::adoptJSValue(*runtime_, ctor));

  EXPECT_TRUE(
      evalBool("class S extends Thing {};"
               "const s = new S();"
               "s.made === 1 && s instanceof Thing && s instanceof S"));
}

TEST_F(ModuleTest, AdoptAndBorrowRoundTrip) {
  JSContext *ctx = qjs::contextFromRuntime(*runtime_);
  ASSERT_NE(ctx, nullptr);

  jsi::Value text =
      qjs::adoptJSValue(*runtime_, JS_NewStringLen(ctx, "caf\xc3\xa9", 5));
  EXPECT_EQ(text.getString(*runtime_).utf8(*runtime_), "caf\xc3\xa9");

  // borrowJSValue returns a reference the jsi::Value still owns, so reading
  // through it must not disturb the original.
  JSValue borrowed = qjs::borrowJSValue(*runtime_, text);
  EXPECT_TRUE(JS_IsString(borrowed));
  EXPECT_EQ(text.getString(*runtime_).utf8(*runtime_), "caf\xc3\xa9");
}

TEST_F(ModuleTest, PendingEngineErrorBecomesAJSError) {
  JSContext *ctx = qjs::contextFromRuntime(*runtime_);
  ASSERT_NE(ctx, nullptr);

  JS_ThrowTypeError(ctx, "module said no");
  try {
    qjs::throwPendingQuickJSError(*runtime_);
    FAIL() << "expected the pending exception to be rethrown";
  } catch (const jsi::JSError &e) {
    EXPECT_NE(
        std::string(e.getMessage()).find("module said no"), std::string::npos);
  }
}

// --- React Native compatibility --------------------------------------------

/*
 * These assert against the exact expressions React Native 0.85 evaluates. If
 * React Native changes its detection these should fail -- that is the point.
 * Getting it wrong is invisible at runtime: the app simply runs a slower
 * Promise and nothing reports it.
 */

TEST_F(ModuleTest, ReactNativeTakesTheNativePromisePath) {
  // Libraries/Core/polyfillPromise.js:25
  EXPECT_TRUE(evalBool(
      "!!(globalThis.HermesInternal && globalThis.HermesInternal.hasPromise &&"
      " globalThis.HermesInternal.hasPromise())"));
  // And the Promise it keeps is genuinely the engine's.
  EXPECT_TRUE(evalBool("Promise.toString().indexOf('[native code]') > -1"));
}

TEST_F(ModuleTest, ReactNativeDevRejectionTrackerDoesNotThrow) {
  // Libraries/Core/polyfillPromise.js:32, __DEV__ only. Throwing here would
  // break every development build.
  EXPECT_TRUE(evalBool(
      "var ok = true;"
      "try { HermesInternal.enablePromiseRejectionTracker({allRejections: "
      "true}); }"
      "catch (e) { ok = false; }"
      "ok"));
}

TEST_F(ModuleTest, QuickJSStacksParseAsHermesStacks) {
  /*
   * Libraries/Core/Devtools/parseErrorStack.js:51 routes stack parsing to
   * parseHermesStack once HermesInternal exists, so declaring the object is
   * only safe if quickjs frames match the shape that parser expects. The
   * regular expression below is parseHermesStack's, verbatim.
   */
  EXPECT_TRUE(evalBool(
      "const re = /^ {4}at (.+?)(?: \\((native)\\)?| \\((address at "
      ")?(.*?):(\\d+):(\\d+)\\))$/;"
      "function inner() { throw new Error('x'); }"
      "let stack; try { inner(); } catch (e) { stack = e.stack; }"
      "const lines = stack.split('\\n').filter(l => l.startsWith('    at '));"
      "lines.length > 0 && lines.every(l => re.test(l))"));
}

TEST_F(ModuleTest, RuntimePropertiesAreHonest) {
  // The object is named for another engine because React Native's detection
  // requires it, but anything that asks precisely must not be misled.
  EXPECT_EQ(
      evalString("HermesInternal.getRuntimeProperties().Engine"), "QuickJS");
  EXPECT_FALSE(evalBool("HermesInternal.getRuntimeProperties().isHermes"));
}

TEST(ReactNativeCompat, CanBeDisabled) {
  qjs::QuickJSRuntimeConfig config;
  config.reactNativeCompat = false;
  auto runtime = qjs::makeQuickJSRuntime(config);
  auto v = runtime->evaluateJavaScript(
      std::make_shared<jsi::StringBuffer>("typeof globalThis.HermesInternal"),
      "t.js");
  EXPECT_EQ(v.asString(*runtime).utf8(*runtime), "undefined");
}

}  // namespace
