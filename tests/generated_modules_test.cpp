/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * The generated module registry, exercised end to end.
 *
 * This test links the registry TU emitted by scripts/generate-module-registry.js
 * together with the modules it names, and must NOT call any module's install
 * function directly. That is the point: the registry is what force-references
 * each module's object file out of its static archive (the failure mode
 * QJS_REGISTER_MODULE alone is subject to), so if the wiring regresses and the
 * modules stop being linked, `Intl` and `TextEncoder` are simply undefined and
 * this fails. It is the host analogue of an app's iOS build.
 */

#include <gtest/gtest.h>

#include <memory>

#include "QuickJSRuntimeFactory.h"

namespace {

namespace jsi = facebook::jsi;

TEST(GeneratedModuleRegistry, InstallsAutolinkedModules) {
  auto rt = qjs::makeQuickJSRuntime();

  auto str = [&](const char *source) {
    return rt
        ->evaluateJavaScript(
            std::make_shared<jsi::StringBuffer>(source), "registry-test.js")
        .asString(*rt)
        .utf8(*rt);
  };

  // text-encoding.
  EXPECT_EQ(str("typeof TextEncoder"), "function");
  EXPECT_EQ(str("typeof TextDecoder"), "function");

  // intl -- and materialize it, which is what the lazy accessor is for.
  EXPECT_EQ(str("typeof Intl"), "object");
  EXPECT_EQ(str("typeof Intl.NumberFormat"), "function");
  EXPECT_EQ(str("new Intl.NumberFormat('en-US').format(1234.5)"), "1,234.5");
}

}  // namespace
