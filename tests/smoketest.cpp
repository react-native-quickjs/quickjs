/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * A dependency-free sanity check: builds and runs without gtest, so it can
 * confirm a toolchain (a new NDK, a cross-compile) produces a working runtime
 * before reaching for the full suites.
 *
 * The real coverage lives in quickjs_jsi_conformance and
 * quickjs_bytecode_tests.
 */

#include <QuickJSRuntimeFactory.h>
#include <jsi/jsi.h>

#include <cstdio>
#include <memory>
#include <string>

namespace jsi = facebook::jsi;

namespace {

int failures = 0;

void check(bool ok, const char *what) {
  std::printf("%s %s\n", ok ? "  ok  " : "FAILED", what);
  if (!ok) {
    ++failures;
  }
}

jsi::Value eval(jsi::Runtime &runtime, const char *source) {
  return runtime.evaluateJavaScript(
      std::make_shared<jsi::StringBuffer>(source), "smoketest.js");
}

/*
 * A write that reaches a HostObject through a receiver's prototype chain must
 * land on the receiver, not call HostObject::set.
 *
 * This is React Native's TurboModule shape exactly.
 * TurboModuleBinding::getModule builds a plain object whose prototype is the
 * module's HostObject, and TurboModule::get memoises each method by writing it
 * back onto that plain object. Routing that write into HostObject::set reaches
 * jsi::HostObject's default, which throws "Cannot assign to property 'X' on
 * HostObject with default setter" -- an app dies on its first
 * performance.mark().
 */
void checkReceiverSemantics(jsi::Runtime &rt) {
  class ReadOnlyHost : public jsi::HostObject {
   public:
    jsi::Value get(jsi::Runtime &rt, const jsi::PropNameID &name) override {
      return name.utf8(rt) == "method" ? jsi::Value(42)
                                       : jsi::Value::undefined();
    }
    // set() is left as the base class's throwing default, which is what every
    // TurboModule inherits.
  };

  jsi::Object host =
      jsi::Object::createFromHostObject(rt, std::make_shared<ReadOnlyHost>());
  rt.global().setProperty(rt, "__hostProto", host);
  rt.global().setProperty(rt, "__receiver", jsi::Object(rt));
  eval(rt, "Object.setPrototypeOf(__receiver, __hostProto)");

  check(
      eval(rt, "__receiver.method").getNumber() == 42,
      "reads fall through the prototype to the host object");

  bool threw = false;
  try {
    eval(rt, "__receiver.method = 7");
  } catch (const jsi::JSError &) {
    threw = true;
  }
  check(!threw, "writing through a host prototype does not throw");
  check(
      eval(rt, "__receiver.method").getNumber() == 7,
      "the write created an own property on the receiver");
  check(
      eval(rt, "Object.getOwnPropertyNames(__receiver).indexOf('method')")
              .getNumber() >= 0,
      "the own property is really on the receiver");
  check(
      eval(rt, "__hostProto.method").getNumber() == 42,
      "the host object itself is unchanged");

  // OrdinarySetWithOwnDescriptor step 3.a: a non-object receiver returns
  // false. It must fail, not throw.
  bool primitiveThrew = false;
  bool primitiveResult = true;
  try {
    primitiveResult =
        eval(rt, "Reflect.set(__hostProto, 'zzz', 1, 5)").getBool();
  } catch (const jsi::JSError &) {
    primitiveThrew = true;
  }
  check(!primitiveThrew, "a primitive receiver does not throw");
  check(!primitiveResult, "a primitive receiver reports failure");

  // A frozen receiver in strict mode must throw, which needs JS_PROP_THROW
  // forwarded from the flags the engine passed in.
  eval(rt, "globalThis.__frozen = Object.freeze(Object.create(__hostProto))");
  bool frozenThrew = false;
  try {
    eval(rt, "'use strict'; __frozen.method = 1");
  } catch (const jsi::JSError &) {
    frozenThrew = true;
  }
  check(frozenThrew, "a frozen receiver throws in strict mode");
}

}  // namespace

int main() {
  auto runtime = qjs::makeQuickJSRuntime();
  check(runtime != nullptr, "runtime constructed");
  check(runtime->description() == "QuickJSRuntime", "description()");

  jsi::Runtime &rt = *runtime;

  check(eval(rt, "6 * 7").getNumber() == 42, "arithmetic");
  check(
      eval(rt, "'quick' + 'js'").getString(rt).utf8(rt) == "quickjs",
      "string concatenation");
  check(eval(rt, "[1,2,3].map(x => x * 2)[2]").getNumber() == 6, "closures");
  check(
      eval(rt, "JSON.stringify({a:1})").getString(rt).utf8(rt) == "{\"a\":1}",
      "JSON");
  check(
      eval(rt, "typeof Symbol()").getString(rt).utf8(rt) == "symbol", "Symbol");
  check(eval(rt, "10n ** 3n === 1000n").getBool(), "BigInt");

  rt.global().setProperty(
      rt, "twice",
      jsi::Function::createFromHostFunction(
          rt, jsi::PropNameID::forAscii(rt, "twice"), 1,
          [](jsi::Runtime &, const jsi::Value &, const jsi::Value *args,
             size_t count) -> jsi::Value {
            return jsi::Value(count == 1 ? args[0].getNumber() * 2 : 0);
          }));
  check(eval(rt, "twice(21)").getNumber() == 42, "host function");

  eval(
      rt,
      "globalThis.done = false; Promise.resolve().then(() => { done = true; "
      "});");
  check(
      !rt.global().getProperty(rt, "done").getBool(), "microtask is deferred");
  rt.drainMicrotasks();
  check(rt.global().getProperty(rt, "done").getBool(), "microtask drained");

  bool threw = false;
  std::string message;
  try {
    eval(rt, "throw new Error('boom')");
  } catch (const jsi::JSError &e) {
    threw = true;
    message = e.getMessage();
  }
  check(threw, "JS exception throws jsi::JSError");
  check(message == "boom", "exception message preserved");

  checkReceiverSemantics(rt);

  runtime.reset();
  check(true, "runtime destroyed cleanly");

  std::printf("\n%s\n", failures == 0 ? "PASS" : "FAIL");
  return failures == 0 ? 0 : 1;
}
