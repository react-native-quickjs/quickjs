/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * The engine adapter that lets React Native's own inspector test suite run
 * against QuickJS.
 *
 * A failing-test list from someone else's suite is an objective work plan in a
 * way that our own assertions about compatibility are not, so this is the
 * measure that decides whether the layer is finished.
 *
 * JsiIntegrationTest.cpp is a TYPED_TEST_SUITE over engine adapters. Supplying
 * one is the entire integration; nothing under node_modules/ is edited.
 */

#pragma once

#include <folly/Executor.h>
#include <jsi/jsi.h>
#include <jsinspector-modern/RuntimeTarget.h>

#include <memory>

#include "QuickJSInspector.h"
#include "utils/InspectorFlagOverridesGuard.h"

namespace facebook::react::jsinspector_modern {

class JsiIntegrationTestQuickJSEngineAdapter {
 public:
  explicit JsiIntegrationTestQuickJSEngineAdapter(folly::Executor &jsExecutor);

  static InspectorFlagOverrides getInspectorFlagOverrides() noexcept;

  RuntimeTargetDelegate &getRuntimeTargetDelegate();

  jsi::Runtime &getRuntime() const noexcept;

  RuntimeExecutor getRuntimeExecutor() const noexcept;

 private:
  std::unique_ptr<jsi::Runtime> runtime_;
  folly::Executor &jsExecutor_;
  std::unique_ptr<qjs::QuickJSInspectorDelegate> delegate_;
};

}  // namespace facebook::react::jsinspector_modern
