/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <react/runtime/JSRuntimeFactory.h>

#include <memory>
#include <utility>

#include "QuickJSRuntimeConfig.h"

namespace facebook::react {

class MessageQueueThread;

/**
 * The React Native engine entry point: hands a QuickJS-backed jsi::Runtime to
 * ReactInstance. Shared by the Android and Apple layers, which do nothing but
 * construct this and pass it through.
 */
class QuickJSInstance : public JSRuntimeFactory {
 public:
  /**
   * Default construction turns the debugger on in debug builds.
   *
   * Decided here rather than in the two platform layers, which would otherwise
   * each need the same decision and fail silently on getting it wrong: the app
   * runs, DevTools attaches, and no breakpoint ever binds -- on that platform
   * only.
   *
   * It cannot be deferred until a frontend connects. Statement traps are
   * emitted at parse time, so a runtime that was not instrumented before the
   * bundle ran cannot be made debuggable afterwards. "Is this app debuggable?"
   * is therefore a build-time question, and the honest default is yes in debug
   * and no in release -- matching where React Native enables its own inspector.
   */
  QuickJSInstance() {
    config_.enableDebugger = debuggerEnabledByDefault();
  }

  explicit QuickJSInstance(qjs::QuickJSRuntimeConfig config)
      : config_(std::move(config)) {}

  ~QuickJSInstance() override = default;

  /// Exposed so an embedder building a config by hand can ask for the same
  /// answer rather than re-deriving it. DEBUG is what CocoaPods defines for
  /// Debug configurations and NDEBUG what CMake defines for Release; neither
  /// platform sets the other's macro, so both are checked.
  static constexpr bool debuggerEnabledByDefault() {
#if !defined(RNQJS_ENABLE_CDP) || !RNQJS_ENABLE_CDP
    // No debugger compiled into this binary, so setting the flag would only buy
    // parse-time instrumentation nothing can use.
    return false;
#elif (defined(DEBUG) && DEBUG) || !defined(NDEBUG)
    return true;
#else
    return false;
#endif
  }

  std::unique_ptr<JSRuntime> createJSRuntime(
      std::shared_ptr<MessageQueueThread> msgQueueThread) noexcept override;

 private:
  qjs::QuickJSRuntimeConfig config_;
};

}  // namespace facebook::react
