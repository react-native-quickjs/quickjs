/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * Host acceptance test for the Intl module.
 *
 * The module's real checks live in JavaScript: test/invariants.js is a
 * self-checking, backend-agnostic suite (92 checks covering the lazy accessor,
 * subclassing, formatter lifetimes, and every fast path) written to run under
 * any binary that has the module installed. This test installs the module into
 * a fresh runtime exactly as an app would and runs that file, so CI exercises
 * the shipped entry point and the shipped algorithm layer rather than a
 * parallel C++ assertion set that could drift.
 *
 * The runtime here links the default, no-platform backend (en-US, UTC), which
 * is a working backend rather than a null pointer — the invariants file is its
 * acceptance test too.
 */

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

#include "QuickJSRuntimeFactory.h"

// The symbol a generated module registry calls by name. Naming it here is also
// what keeps the object file: a static library link is entitled to drop one
// nothing references, which is the failure this entry point exists to prevent.
extern "C" void intl_install(facebook::jsi::Runtime &runtime);

#ifndef RNQJS_INTL_TEST_DATA_DIR
#define RNQJS_INTL_TEST_DATA_DIR "test"
#endif

namespace {

namespace jsi = facebook::jsi;

class IntlTest : public ::testing::Test {
 protected:
  std::unique_ptr<jsi::Runtime> rt = makeRuntime();

  static std::unique_ptr<jsi::Runtime> makeRuntime() {
    auto runtime = qjs::makeQuickJSRuntime();
    intl_install(*runtime);
    return runtime;
  }

  jsi::Value eval(const char *source) {
    return rt->evaluateJavaScript(
        std::make_shared<jsi::StringBuffer>(source), "intl-test.js");
  }

  std::string str(const char *source) {
    return eval(source).asString(*rt).utf8(*rt);
  }
};

std::string readFile(const char *name) {
  std::ifstream in(std::string(RNQJS_INTL_TEST_DATA_DIR) + "/" + name);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

TEST_F(IntlTest, InstallsALazyIntlAccessor) {
  EXPECT_EQ(str("typeof Intl"), "object");
  EXPECT_EQ(str("typeof Intl.NumberFormat"), "function");
  EXPECT_EQ(str("typeof Intl.DateTimeFormat"), "function");
  EXPECT_EQ(str("typeof Intl.Collator"), "function");
  EXPECT_EQ(str("typeof Intl.PluralRules"), "function");
}

TEST_F(IntlTest, NumberFormatProducesLocaleDigits) {
  EXPECT_EQ(str("new Intl.NumberFormat('en-US').format(1234.5)"), "1,234.5");
}

TEST_F(IntlTest, SelfCheckingInvariants) {
  // `print` is the shell builtin the file was written for; a no-op keeps the
  // output quiet and the pass/fail signal is the throw at the end.
  eval("print = function () {};");
  const std::string source = readFile("invariants.js");
  EXPECT_NO_THROW(rt->evaluateJavaScript(
      std::make_shared<jsi::StringBuffer>(source), "invariants.js"));
}

}  // namespace
