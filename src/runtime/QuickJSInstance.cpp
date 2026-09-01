/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "QuickJSInstance.h"

#include "QuickJSRuntimeFactory.h"

namespace facebook::react {

std::unique_ptr<JSRuntime> QuickJSInstance::createJSRuntime(
    std::shared_ptr<MessageQueueThread> /*msgQueueThread*/) noexcept {
  // The message queue thread is unused: the debugger work that must happen on
  // the JS thread is routed through React Native's RuntimeExecutor instead.
  //
  // JSIRuntimeHolder's base class vends a FallbackRuntimeTargetDelegate, which
  // is the right thing for a runtime that was never instrumented: DevTools
  // still gets the Runtime domain through generic JSI, and the Debugger domain
  // reports honestly that it cannot bind breakpoints.
  return std::make_unique<JSIRuntimeHolder>(qjs::makeQuickJSRuntime(config_));
}

}  // namespace facebook::react
