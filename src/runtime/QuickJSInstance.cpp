/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "QuickJSInstance.h"

#include "QuickJSRuntimeFactory.h"

#if defined(RNQJS_ENABLE_CDP) && RNQJS_ENABLE_CDP
#include "QuickJSInspector.h"
#endif

namespace facebook::react {

std::unique_ptr<JSRuntime> QuickJSInstance::createJSRuntime(
    std::shared_ptr<MessageQueueThread> /*msgQueueThread*/) noexcept {
  // The message queue thread is unused: the debugger work that has to happen on
  // the JS thread is routed through React Native's RuntimeExecutor instead.
  auto runtime = qjs::makeQuickJSRuntime(config_);

#if defined(RNQJS_ENABLE_CDP) && RNQJS_ENABLE_CDP
  if (config_.enableDebugger) {
    return std::make_unique<qjs::QuickJSInspectorRuntime>(std::move(runtime));
  }
#endif

  // JSIRuntimeHolder's base class vends a FallbackRuntimeTargetDelegate, which
  // is the honest answer for a runtime that was never instrumented: DevTools
  // still gets the Runtime domain through generic JSI, and the Debugger domain
  // reports that it cannot bind breakpoints.
  return std::make_unique<JSIRuntimeHolder>(std::move(runtime));
}

}  // namespace facebook::react
