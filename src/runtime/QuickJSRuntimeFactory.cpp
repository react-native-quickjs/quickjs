/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "QuickJSRuntimeFactory.h"

#include "QuickJSCompat.h"
#include "QuickJSModule.h"
#include "QuickJSRuntime.h"

namespace qjs {

std::unique_ptr<facebook::jsi::Runtime> makeQuickJSRuntime(
    QuickJSRuntimeConfig config) {
  const bool reactNativeCompat = config.reactNativeCompat;

  auto runtime = std::make_unique<QuickJSRuntime>(std::move(config));

  // Both install before any application JavaScript runs, because React
  // Native's polyfill checks run very early: anything installed afterwards is
  // shadowed by a polyfill that has already decided the engine lacks it.
  if (reactNativeCompat) {
    installReactNativeCompat(*runtime);
  }

  installModules(*runtime);

  return runtime;
}

}  // namespace qjs
