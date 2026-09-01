/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "QuickJSCompat.h"

namespace qjs {

namespace jsi = facebook::jsi;

/*
 * React Native detects engine capabilities by looking for
 * `global.HermesInternal`. That is not a capability protocol -- it is a check
 * for one specific engine -- so every other engine silently gets the fallback
 * path. There are three uses in React Native 0.85:
 *
 *   Libraries/Core/polyfillPromise.js:25
 *     if (global?.HermesInternal?.hasPromise?.()) { native Promise }
 *     else { polyfillGlobal('Promise', ...) }
 *   Libraries/Core/polyfillPromise.js:32
 *     enablePromiseRejectionTracker(...), in __DEV__ only
 *   Libraries/Core/Devtools/parseErrorStack.js:51
 *     branches on it to choose a stack-trace parser
 *
 * Without this shim every app replaces quickjs's native Promise with
 * `promise@7.3.1`, a JavaScript implementation on top of setImmediate.
 * Measured here: constructing and chaining costs 1205 ns through the polyfill
 * against 775 ns native, and every `await` in the app pays it.
 *
 * The third use is the one that could bite, since declaring the object reroutes
 * stack parsing to parseHermesStack. Its frame pattern is
 *
 *     /^ {4}at (.+?)(?: \((native)\)?| \((address at )?(.*?):(\d+):(\d+)\))$/
 *
 * and quickjs emits exactly that shape, `    at inner (/path/file.js:1:29)`,
 * including the `(native)` form. Checked against real quickjs stacks: every
 * frame parses with the captures the Hermes parser expects.
 *
 * The honest cost is outside React Native core, where third-party libraries use
 * the same object as an "am I on Hermes" test, sometimes to work around a
 * Hermes bug we do not have. That is why this is a config flag rather than
 * unconditional, and why getRuntimeProperties() below tells the truth about
 * which engine this is.
 */
void installReactNativeCompat(jsi::Runtime &runtime) {
  auto hermesInternal = jsi::Object(runtime);

  hermesInternal.setProperty(
      runtime, "hasPromise",
      jsi::Function::createFromHostFunction(
          runtime, jsi::PropNameID::forAscii(runtime, "hasPromise"), 0,
          [](jsi::Runtime &, const jsi::Value &, const jsi::Value *, size_t) {
            return jsi::Value(true);
          }));

  // Accepting and ignoring the options is deliberate: unhandled-rejection
  // reporting belongs to the host, which installs its own tracker through
  // JS_SetHostPromiseRejectionTracker. Throwing "not implemented" here would
  // break every development build for a feature the host already owns.
  hermesInternal.setProperty(
      runtime, "enablePromiseRejectionTracker",
      jsi::Function::createFromHostFunction(
          runtime,
          jsi::PropNameID::forAscii(runtime, "enablePromiseRejectionTracker"),
          1,
          [](jsi::Runtime &, const jsi::Value &, const jsi::Value *, size_t) {
            return jsi::Value::undefined();
          }));

  hermesInternal.setProperty(
      runtime, "getRuntimeProperties",
      jsi::Function::createFromHostFunction(
          runtime, jsi::PropNameID::forAscii(runtime, "getRuntimeProperties"),
          0,
          [](jsi::Runtime &rt, const jsi::Value &, const jsi::Value *, size_t) {
            auto props = jsi::Object(rt);
            props.setProperty(
                rt, "OSS Release Version",
                jsi::String::createFromAscii(rt, "quickjs-ng"));
            props.setProperty(
                rt, "Engine", jsi::String::createFromAscii(rt, "QuickJS"));
            props.setProperty(rt, "isHermes", jsi::Value(false));
            return jsi::Value(std::move(props));
          }));

  runtime.global().setProperty(runtime, "HermesInternal", hermesInternal);
}

}  // namespace qjs
