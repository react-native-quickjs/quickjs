/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * Runs Hermes' JSI conformance suite (API/jsi/jsi/test/testlib.cpp) against
 * QuickJSRuntime. testlib.cpp is parameterised over a RuntimeFactory, so
 * supplying ours below is the entire integration.
 *
 * This is the primary development driver: it is faster and far more precise
 * than iterating through an app build, and it is the definition of "the JSI
 * surface works".
 */

#include <QuickJSRuntimeFactory.h>
#include <jsi/test/testlib.h>

namespace facebook::jsi {

std::vector<RuntimeFactory> runtimeGenerators() {
  return {[]() -> std::shared_ptr<Runtime> {
    return std::shared_ptr<Runtime>(qjs::makeQuickJSRuntime().release());
  }};
}

}  // namespace facebook::jsi
