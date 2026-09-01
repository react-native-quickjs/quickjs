/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * Runtime behaviour that is not part of the JSI surface. It is invisible from
 * JavaScript and fails silently: a collection that never runs looks like a
 * memory leak, not like a scheduling bug.
 */

#include <gtest/gtest.h>
#include <jsi/jsi.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "QuickJSRuntime.h"

using namespace facebook;
using clock_type = std::chrono::steady_clock;

namespace {

jsi::Value eval(jsi::Runtime &runtime, const std::string &source) {
  return runtime.evaluateJavaScript(
      std::make_shared<jsi::StringBuffer>(source), "runtime_test.js");
}

/// Builds and drops cycles until the engine has a collection waiting. Cycles,
/// because acyclic garbage dies at its last decref and never reaches the
/// collector at all.
bool churnUntilPending(qjs::QuickJSRuntime &runtime) {
  for (int i = 0; i < 400; i++) {
    if (JS_HasPendingGC(runtime.runtime())) {
      return true;
    }
    eval(
        runtime,
        "(function () { for (var i = 0; i < 400; i++) {"
        "  var a = {}, b = {}; a.b = b; b.a = a; } })()");
  }
  return JS_HasPendingGC(runtime.runtime());
}

/// Deferral on, idleness unreachable, pressure unreachable: the only thing that
/// can release a collection is the max-deferral valve, which is what these
/// tests are about.
qjs::QuickJSRuntimeConfig valveOnlyConfig() {
  qjs::QuickJSRuntimeConfig config;
  config.deferGC = true;
  config.gcIdleGapMs = 1000000;
  config.gcMaxDeferralMs = 50;
  config.gcPressureBytes = SIZE_MAX;
  return config;
}

}  // namespace

TEST(GCScheduling, MaxDeferralValveFires) {
  qjs::QuickJSRuntime runtime{valveOnlyConfig()};

  // Spend the first safepoint. Until one has been taken there is no previous
  // task to measure a gap from, so every safepoint reads as idle and would
  // collect before the valve was ever consulted.
  runtime.drainMicrotasks();
  ASSERT_TRUE(churnUntilPending(runtime));

  runtime.runPendingGCIfIdle(clock_type::now());
  ASSERT_TRUE(JS_HasPendingGC(runtime.runtime()))
      << "the app is not idle, so this safepoint must decline";

  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  runtime.runPendingGCIfIdle(clock_type::now());
  EXPECT_FALSE(JS_HasPendingGC(runtime.runtime()))
      << "past gcMaxDeferralMs the collection must run anyway";
}

TEST(GCScheduling, EvaluatingDoesNotRestartTheDeferralClock) {
  /*
   * The valve measures from when the collection started waiting. Evaluating a
   * script rebaselines the heap, because growth after it is what the pressure
   * rule cares about -- but it is not a collection, and resetting the wait
   * there lets an app that keeps evaluating defer a collection forever while
   * every individual safepoint looks reasonable.
   */
  qjs::QuickJSRuntime runtime{valveOnlyConfig()};
  runtime.drainMicrotasks();
  ASSERT_TRUE(churnUntilPending(runtime));

  runtime.runPendingGCIfIdle(clock_type::now());
  ASSERT_TRUE(JS_HasPendingGC(runtime.runtime()));

  eval(runtime, "1 + 1");

  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  runtime.runPendingGCIfIdle(clock_type::now());
  EXPECT_FALSE(JS_HasPendingGC(runtime.runtime()))
      << "a script ran between the two safepoints and reset the wait";
}

TEST(GCScheduling, RunPendingGCCollectsUnconditionally) {
  qjs::QuickJSRuntime runtime{valveOnlyConfig()};
  runtime.drainMicrotasks();
  ASSERT_TRUE(churnUntilPending(runtime));

  runtime.runPendingGC();
  EXPECT_FALSE(JS_HasPendingGC(runtime.runtime()));
}
