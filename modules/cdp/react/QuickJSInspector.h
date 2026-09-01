/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * Plugs the CDP layer into React Native's inspector.
 *
 * React Native owns the WebSocket, the page target and the session; all this
 * has to do is hand CDP messages to the agent and hand its replies back. The
 * protocol work is all in modules/cdp, which knows nothing about any of this.
 */

#pragma once

#include <jsinspector-modern/ReactCdp.h>
#include <react/runtime/JSRuntimeFactory.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include "quickjs-cdp.h"
}

namespace qjs {

/**
 * Owns the agent for one runtime.
 *
 * Constructed with the runtime, not when a frontend connects: the engine emits
 * statement traps at parse time, so a runtime that was not instrumented before
 * its first script ran cannot be debugged afterwards.
 */
class QuickJSInspectorDelegate : public facebook::react::jsinspector_modern::
                                     FallbackRuntimeTargetDelegate {
 public:
  explicit QuickJSInspectorDelegate(JSContext *ctx);
  ~QuickJSInspectorDelegate() override;

  std::unique_ptr<facebook::react::jsinspector_modern::RuntimeAgentDelegate>
  createAgentDelegate(
      facebook::react::jsinspector_modern::FrontendChannel channel,
      facebook::react::jsinspector_modern::SessionState &sessionState,
      std::unique_ptr<facebook::react::jsinspector_modern::
                          RuntimeAgentDelegate::ExportedState>
          previouslyExportedState,
      const facebook::react::jsinspector_modern::ExecutionContextDescription
          &executionContextDescription,
      facebook::react::RuntimeExecutor runtimeExecutor) override;

  /// Tells the frontend a script exists, and resolves breakpoints set against
  /// its URL. JS thread only.
  void scriptLoaded(const std::string &url, const std::string &source);

  QJSCDPAgent *agent() const {
    return agent_;
  }

 private:
  static void sendToFrontends(void *opaque, const char *json, size_t len);

  QJSCDPAgent *agent_;
  std::mutex lock_;
  std::vector<facebook::react::jsinspector_modern::FrontendChannel> channels_;
};

/**
 * The JSRuntime React Native asks for, with the debugger attached.
 *
 * The delegate is built here, immediately after the runtime and before any
 * script has been evaluated, rather than when a frontend connects. React
 * Native calls getRuntimeTargetDelegate() lazily, long after the bundle has
 * run, and by then it is far too late to instrument anything.
 */
class QuickJSInspectorRuntime : public facebook::react::JSRuntime {
 public:
  explicit QuickJSInspectorRuntime(
      std::unique_ptr<facebook::jsi::Runtime> runtime);

  facebook::jsi::Runtime &getRuntime() noexcept override;

  facebook::react::jsinspector_modern::RuntimeTargetDelegate &
  getRuntimeTargetDelegate() override;

 private:
  std::unique_ptr<facebook::jsi::Runtime> runtime_;
  std::unique_ptr<QuickJSInspectorDelegate> delegate_;
};

}  // namespace qjs
