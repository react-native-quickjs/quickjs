/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * ---------------------------------------------------------------------------
 * Verbatim copy of
 *   ReactAndroid/src/main/jni/react/runtime/jni/JJSRuntimeFactory.h
 *
 * React Native exports only ReactCommon/jsitooling through its prefab headers,
 * so an out-of-tree engine has to carry its own copy of this one. It is a pure
 * declaration -- it binds to the com.facebook.react.runtime.JSRuntimeFactory
 * class that ships in the app -- so the copy duplicates no code, only a version
 * coupling. Re-check it when raising the supported React Native version.
 * ---------------------------------------------------------------------------
 */

#pragma once

#include <fbjni/fbjni.h>
#include <jni.h>
#include <react/runtime/JSRuntimeFactory.h>

namespace facebook::react {

class JJSRuntimeFactory : public jni::HybridClass<JJSRuntimeFactory>,
                          public JSRuntimeFactory {
 public:
  static auto constexpr kJavaDescriptor =
      "Lcom/facebook/react/runtime/JSRuntimeFactory;";

 private:
  friend HybridBase;
};

} // namespace facebook::react
