/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * Reports the cost of each operation that crosses the JSI boundary.
 */

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "QuickJSRuntime.h"

namespace jsi = facebook::jsi;
using qjs::QuickJSRuntime;
using Clock = std::chrono::steady_clock;

namespace {

/// Median of several passes. A mean would let one descheduled pass set the
/// number, and these are tens of nanoseconds.
double nanosPerOp(size_t iterations, const std::function<void()> &body) {
  constexpr int kPasses = 5;
  std::vector<double> passes;
  for (int pass = 0; pass < kPasses; ++pass) {
    const auto start = Clock::now();
    for (size_t i = 0; i < iterations; ++i) {
      body();
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             Clock::now() - start)
                             .count();
    passes.push_back(static_cast<double>(elapsed) / iterations);
  }
  std::sort(passes.begin(), passes.end());
  return passes[kPasses / 2];
}

void report(
    const char *name, size_t iterations, const std::function<void()> &body) {
  printf("  %-32s %8.1f ns\n", name, nanosPerOp(iterations, body));
}

struct BenchState : jsi::NativeState {
  int value{0};
};

struct BenchHostObject : jsi::HostObject {
  jsi::Value get(jsi::Runtime &, const jsi::PropNameID &) override {
    return jsi::Value(1);
  }
};

jsi::Value evaluate(QuickJSRuntime &runtime, const char *source) {
  return runtime.createValue(JS_Eval(
      runtime.context(), source, std::strlen(source), "<bench>",
      JS_EVAL_TYPE_GLOBAL));
}

}  // namespace

int main() {
#if !QJS_BENCH_OPTIMIZED
  // An unoptimized engine reports a property read at 45ns that really costs 5,
  // and a native state read at 213ns that really costs 4. Numbers that wrong
  // are worse than none.
  printf(
      "jsi bench: skipped, this is not a release build.\n"
      "           cmake -B build-release -DCMAKE_BUILD_TYPE=Release\n");
  return 0;
#else
  qjs::QuickJSRuntimeConfig config;
  QuickJSRuntime runtime(config);
  const size_t n = 200000;

  printf("jsi bench: nanoseconds per operation, median of 5 passes\n");

  printf("\nvalues\n");
  report("Value(double)", n, [&] {
    jsi::Value v(3.5);
    (void)v;
  });
  report("Object()", n, [&] {
    jsi::Object o(runtime);
    (void)o;
  });
  report("Array(8)", n, [&] {
    jsi::Array a(runtime, 8);
    (void)a;
  });

  printf("\nstrings\n");
  const std::string text = "a moderately sized property value";
  report("String::createFromAscii", n, [&] {
    auto s = jsi::String::createFromAscii(runtime, text);
    (void)s;
  });
  auto sample = jsi::String::createFromAscii(runtime, text);
  report("String::utf8", n, [&] {
    volatile size_t size = sample.utf8(runtime).size();
    (void)size;
  });
  report("PropNameID::forAscii", n, [&] {
    auto id = jsi::PropNameID::forAscii(runtime, "property");
    (void)id;
  });

  printf("\nproperties\n");
  auto object = jsi::Object(runtime);
  auto name = jsi::PropNameID::forAscii(runtime, "value");
  object.setProperty(runtime, name, 42);
  report("getProperty(PropNameID)", n, [&] {
    volatile double v = object.getProperty(runtime, name).getNumber();
    (void)v;
  });
  report("getProperty(const char *)", n, [&] {
    volatile double v = object.getProperty(runtime, "value").getNumber();
    (void)v;
  });
  report("setProperty(PropNameID)", n, [&] {
    object.setProperty(runtime, name, 42);
  });
  report("hasProperty", n, [&] {
    volatile bool has = object.hasProperty(runtime, name);
    (void)has;
  });
  auto array = jsi::Array(runtime, 16);
  report("getValueAtIndex", n, [&] {
    auto v = array.getValueAtIndex(runtime, 3);
    (void)v;
  });
  report("setValueAtIndex", n, [&] { array.setValueAtIndex(runtime, 3, 1); });

  printf("\nnative state\n");
  auto state = std::make_shared<BenchState>();
  auto stateful = jsi::Object(runtime);
  stateful.setNativeState(runtime, state);
  report("hasNativeState", n, [&] {
    volatile bool has = stateful.hasNativeState(runtime);
    (void)has;
  });
  report("getNativeState", n, [&] {
    auto s = stateful.getNativeState(runtime);
    (void)s;
  });
  report("setNativeState (replace)", n, [&] {
    stateful.setNativeState(runtime, state);
  });

  printf("\ncalls\n");
  auto identity = evaluate(runtime, "(function (a) { return a; })")
                      .getObject(runtime)
                      .getFunction(runtime);
  report("call, 0 args", n, [&] {
    auto v = identity.call(runtime);
    (void)v;
  });
  report("call, 1 arg", n, [&] {
    auto v = identity.call(runtime, 1);
    (void)v;
  });
  report("call, 4 args", n, [&] {
    auto v = identity.call(runtime, 1, 2, 3, 4);
    (void)v;
  });

  auto hostFunction = jsi::Function::createFromHostFunction(
      runtime, jsi::PropNameID::forAscii(runtime, "host"), 1,
      [](jsi::Runtime &, const jsi::Value &, const jsi::Value *arguments,
         size_t count) {
        return jsi::Value(count > 0 ? arguments[0].getNumber() : 0);
      });
  runtime.global().setProperty(runtime, "host", hostFunction);
  report("host function, from C++", n, [&] {
    auto v = hostFunction.call(runtime, 1);
    (void)v;
  });
  auto callHost = evaluate(runtime, "(function () { return host(1); })")
                      .getObject(runtime)
                      .getFunction(runtime);
  report("host function, from JS", n, [&] {
    auto v = callHost.call(runtime);
    (void)v;
  });

  printf("\nhost objects\n");
  auto hostObject = jsi::Object::createFromHostObject(
      runtime, std::make_shared<BenchHostObject>());
  report("host object get", n, [&] {
    auto v = hostObject.getProperty(runtime, name);
    (void)v;
  });

  printf("\nengine\n");
  report("evaluate, tiny expression", n / 20, [&] {
    auto v = evaluate(runtime, "1 + 1");
    (void)v;
  });
  report("drainMicrotasks, empty", n, [&] { runtime.drainMicrotasks(); });

  printf("\n");
  return 0;
#endif
}
