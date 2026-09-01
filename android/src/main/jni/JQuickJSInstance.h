/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <fbjni/fbjni.h>
#include <react/runtime/JSRuntimeFactory.h>

#include <memory>

#include "JJSRuntimeFactory.h"
#include "QuickJSInstance.h"

namespace facebook::react {

class MessageQueueThread;

/**
 * The fbjni hybrid behind the Kotlin QuickJSInstance, mirroring React Native's
 * own JHermesInstance: the Java-side object is a JSRuntimeFactory and the C++
 * side answers createJSRuntime().
 */
class JQuickJSInstance
    : public jni::HybridClass<JQuickJSInstance, JJSRuntimeFactory> {
 public:
  static constexpr auto kJavaDescriptor =
      "Lcom/ammarahmed/quickjs/QuickJSInstance;";

  static jni::local_ref<jhybriddata> initHybrid(jni::alias_ref<jclass>);

  static void registerNatives();

  std::unique_ptr<JSRuntime> createJSRuntime(
      std::shared_ptr<MessageQueueThread> msgQueueThread) noexcept;

 private:
  friend HybridBase;

  QuickJSInstance instance_;
};

}  // namespace facebook::react
