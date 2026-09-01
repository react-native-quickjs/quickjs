/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "JQuickJSInstance.h"

namespace facebook::react {

jni::local_ref<JQuickJSInstance::jhybriddata> JQuickJSInstance::initHybrid(
    jni::alias_ref<jclass>) {
  return makeCxxInstance();
}

std::unique_ptr<JSRuntime> JQuickJSInstance::createJSRuntime(
    std::shared_ptr<MessageQueueThread> msgQueueThread) noexcept {
  return instance_.createJSRuntime(std::move(msgQueueThread));
}

void JQuickJSInstance::registerNatives() {
  registerHybrid(
      {makeNativeMethod("initHybrid", JQuickJSInstance::initHybrid)});
}

}  // namespace facebook::react
