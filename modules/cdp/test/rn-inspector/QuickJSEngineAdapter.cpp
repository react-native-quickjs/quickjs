/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "QuickJSEngineAdapter.h"

#include "QuickJSRuntime.h"
#include "QuickJSRuntimeFactory.h"

namespace facebook::react::jsinspector_modern {

JsiIntegrationTestQuickJSEngineAdapter::JsiIntegrationTestQuickJSEngineAdapter(
    folly::Executor &jsExecutor)
    : jsExecutor_{jsExecutor} {
  qjs::QuickJSRuntimeConfig config;
  config.enableDebugger = true;
  runtime_ = qjs::makeQuickJSRuntime(std::move(config));

  auto &quickjs = dynamic_cast<qjs::QuickJSRuntime &>(*runtime_);
  delegate_ =
      std::make_unique<qjs::QuickJSInspectorDelegate>(quickjs.context());
  quickjs.setScriptEvaluatedHook(
      [this](const std::string &url, const std::string &source) {
        delegate_->scriptLoaded(url, source);
      });
}

InspectorFlagOverrides
JsiIntegrationTestQuickJSEngineAdapter::getInspectorFlagOverrides() noexcept {
  return {};
}

RuntimeTargetDelegate &
JsiIntegrationTestQuickJSEngineAdapter::getRuntimeTargetDelegate() {
  return *delegate_;
}

jsi::Runtime &JsiIntegrationTestQuickJSEngineAdapter::getRuntime()
    const noexcept {
  return *runtime_;
}

RuntimeExecutor JsiIntegrationTestQuickJSEngineAdapter::getRuntimeExecutor()
    const noexcept {
  return [&jsExecutor = jsExecutor_, &runtime = getRuntime()](auto fn) {
    jsExecutor.add([fn, &runtime]() { fn(runtime); });
  };
}

}  // namespace facebook::react::jsinspector_modern
