/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <hermes-compat/Diagnostics.h>
#include <hermes/hermes.h>
#include <hermes/inspector/RuntimeAdapter.h>
#include <jsi/threadsafe.h>

#include <chrono>
#include <cstring>
#include <string>
#include <vector>

using facebook::jsi::Value;
namespace hc = facebook::hermes;
namespace diag = qjs::hermescompat;

namespace {

/// Collects what the shim reports, so a test can assert that an API said it
/// was unsupported rather than failing quietly.
class Reports {
 public:
  Reports() {
    diag::resetForTesting();
    diag::setHandler(
        [this](diag::Severity severity, const char *api, const char *detail) {
          entries_.push_back({severity, api, detail});
        });
  }
  ~Reports() {
    diag::resetForTesting();
  }

  size_t count() const {
    return entries_.size();
  }

  bool saw(diag::Severity severity, const std::string &api) const {
    for (const auto &e : entries_) {
      if (e.severity == severity && e.api.find(api) != std::string::npos) {
        return true;
      }
    }
    return false;
  }

 private:
  struct Entry {
    diag::Severity severity;
    std::string api;
    std::string detail;
  };
  std::vector<Entry> entries_;
};

hc::IHermes &hermesOf(hc::HermesRuntime &runtime) {
  auto *iface = runtime.castInterface(hc::IHermes::uuid);
  EXPECT_NE(iface, nullptr);
  return *static_cast<hc::IHermes *>(iface);
}

}  // namespace

// The point of the whole shim: a library that asks Hermes for a runtime gets
// one that runs JavaScript.
TEST(HermesCompat, MakeHermesRuntimeRunsJavaScript) {
  auto runtime = hc::makeHermesRuntime();
  ASSERT_NE(runtime, nullptr);
  EXPECT_EQ(
      runtime
          ->evaluateJavaScript(
              std::make_shared<facebook::jsi::StringBuffer>("1 + 2"), "t")
          .getNumber(),
      3);
}

TEST(HermesCompat, RootAPIAndRuntimeAnswerTheirInterfaces) {
  auto *root = hc::makeHermesRootAPI();
  ASSERT_NE(root, nullptr);
  EXPECT_NE(root->castInterface(hc::IHermesRootAPI::uuid), nullptr);
  EXPECT_NE(root->castInterface(hc::ISetFatalHandler::uuid), nullptr);

  auto runtime = hc::makeHermesRuntime();
  EXPECT_NE(runtime->castInterface(hc::IHermes::uuid), nullptr);
}

// An unknown UUID must fall through to the QuickJS runtime rather than being
// answered with the Hermes interface.
TEST(HermesCompat, UnknownInterfaceIsNotAnsweredWithIHermes) {
  auto runtime = hc::makeHermesRuntime();
  constexpr facebook::jsi::UUID kUnknown{
      0x00000000, 0x0000, 0x0000, 0x0000, 0x000000000000};
  auto *iface = runtime->castInterface(kUnknown);
  EXPECT_NE(iface, static_cast<void *>(&hermesOf(*runtime)));
}

TEST(HermesCompat, UniqueIDIsStablePerObjectAndZeroForNonPointers) {
  Reports reports;
  auto runtime = hc::makeHermesRuntime();
  auto &hermes = hermesOf(*runtime);

  auto a = facebook::jsi::Object(*runtime);
  auto b = facebook::jsi::Object(*runtime);
  EXPECT_NE(hermes.getUniqueID(a), 0u);
  EXPECT_EQ(hermes.getUniqueID(a), hermes.getUniqueID(a));
  EXPECT_NE(hermes.getUniqueID(a), hermes.getUniqueID(b));

  EXPECT_EQ(hermes.getUniqueID(Value(42)), 0u);
  EXPECT_EQ(hermes.getUniqueID(Value::undefined()), 0u);
}

// A real Hermes bytecode bundle must be named as the cause, not reported as
// "not bytecode" like ordinary source.
TEST(HermesCompat, HermesBytecodeIsRecognisedAndRefused) {
  Reports reports;
  auto *root = static_cast<hc::IHermesRootAPI *>(
      hc::makeHermesRootAPI()->castInterface(hc::IHermesRootAPI::uuid));

  const uint8_t hbc[] = {0xC6, 0x1F, 0xBC, 0x03, 0xC1, 0x03, 0x19, 0x1F, 0x00};
  EXPECT_FALSE(root->isHermesBytecode(hbc, sizeof(hbc)));
  EXPECT_TRUE(reports.saw(diag::Severity::Unsupported, "isHermesBytecode"));

  const uint8_t js[] = "var x = 1;";
  std::string why;
  EXPECT_FALSE(root->hermesBytecodeSanityCheck(js, sizeof(js), &why));
  EXPECT_NE(why.find("not Hermes bytecode"), std::string::npos);
}

// Unsupported APIs must return something a caller cannot mistake for success.
TEST(HermesCompat, UnsupportedAPIsFailSafelyAndSaySo) {
  Reports reports;
  auto runtime = hc::makeHermesRuntime();
  auto &hermes = hermesOf(*runtime);

  EXPECT_EQ(hermes.getVMRuntimeUnsafe(), nullptr);
  EXPECT_EQ(hermes.getSHRuntime(), nullptr);
  EXPECT_TRUE(hermes.getObjectForID(1).isNull());
  EXPECT_EQ(hc::makeThreadSafeHermesRuntime(), nullptr);

  auto *root = static_cast<hc::IHermesRootAPI *>(
      hc::makeHermesRootAPI()->castInterface(hc::IHermesRootAPI::uuid));
  EXPECT_EQ(root->getBytecodeVersion(), 0u);
  EXPECT_EQ(root->getBytecodeEpilogue(nullptr, 0).first, nullptr);
  EXPECT_FALSE(root->isCodeCoverageProfilerEnabled());

  EXPECT_TRUE(reports.saw(diag::Severity::Unsupported, "getVMRuntimeUnsafe"));
  EXPECT_TRUE(reports.saw(diag::Severity::Unsupported, "getObjectForID"));
  EXPECT_TRUE(reports.saw(diag::Severity::Unsupported, "getBytecodeVersion"));
}

// Each API reports under its own name, and only once however often it is hit.
TEST(HermesCompat, EachAPIReportsOnceUnderItsOwnName) {
  Reports reports;
  auto *root = static_cast<hc::IHermesRootAPI *>(
      hc::makeHermesRootAPI()->castInterface(hc::IHermesRootAPI::uuid));

  root->enableSamplingProfiler(100);
  root->enableSamplingProfiler(100);
  root->disableSamplingProfiler();

  EXPECT_EQ(reports.count(), 2u);
  EXPECT_TRUE(
      reports.saw(diag::Severity::Unsupported, "enableSamplingProfiler"));
  EXPECT_TRUE(
      reports.saw(diag::Severity::Unsupported, "disableSamplingProfiler"));
}

// Degraded, not unsupported: the code runs, the source map does not apply.
TEST(HermesCompat, SourceMapEvaluationRunsAndReportsDegraded) {
  Reports reports;
  auto runtime = hc::makeHermesRuntime();
  auto &hermes = hermesOf(*runtime);

  auto value = hermes.evaluateJavaScriptWithSourceMap(
      std::make_shared<facebook::jsi::StringBuffer>("6 * 7"),
      std::make_shared<facebook::jsi::StringBuffer>("{}"), "t.js");
  EXPECT_EQ(value.getNumber(), 42);
  EXPECT_TRUE(reports.saw(diag::Severity::Degraded, "SourceMap"));
}

TEST(HermesCompat, DebugJavaScriptEvaluates) {
  auto runtime = hc::makeHermesRuntime();
  hc::IHermes::DebugFlags flags;
  hermesOf(*runtime).debugJavaScript("globalThis.ran = 7;", "t.js", flags);
  EXPECT_EQ(runtime->global().getProperty(*runtime, "ran").getNumber(), 7);
}

// watchTimeLimit is a real capability here, not a stub: it must stop a loop
// that would otherwise never return.
TEST(HermesCompat, WatchTimeLimitInterruptsARunawayLoop) {
  auto runtime = hc::makeHermesRuntime();
  auto &hermes = hermesOf(*runtime);
  hermes.watchTimeLimit(50);

  const auto start = std::chrono::steady_clock::now();
  // Bounded so that a broken interrupt fails the test instead of hanging.
  EXPECT_ANY_THROW(runtime->evaluateJavaScript(
      std::make_shared<facebook::jsi::StringBuffer>(
          "var i = 0; while (i < 2e9) { i++; } i"),
      "loop.js"));
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_LT(
      std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(), 10);

  hermes.unwatchTimeLimit();
}

TEST(HermesCompat, AsyncTriggerTimeoutInterrupts) {
  auto runtime = hc::makeHermesRuntime();
  auto &hermes = hermesOf(*runtime);
  hermes.watchTimeLimit(1000 * 60);
  hermes.asyncTriggerTimeout();

  EXPECT_ANY_THROW(runtime->evaluateJavaScript(
      std::make_shared<facebook::jsi::StringBuffer>(
          "var i = 0; while (i < 2e9) { i++; } i"),
      "loop.js"));
  hermes.unwatchTimeLimit();
}

// After unwatch, ordinary code must still run.
TEST(HermesCompat, UnwatchTimeLimitRestoresNormalExecution) {
  auto runtime = hc::makeHermesRuntime();
  auto &hermes = hermesOf(*runtime);
  hermes.watchTimeLimit(1);
  hermes.unwatchTimeLimit();
  EXPECT_EQ(
      runtime
          ->evaluateJavaScript(
              std::make_shared<facebook::jsi::StringBuffer>("1 + 1"), "t")
          .getNumber(),
      2);
}

TEST(HermesCompat, ResetTimezoneCacheDoesNotCrash) {
  auto runtime = hc::makeHermesRuntime();
  hermesOf(*runtime).resetTimezoneCache();
  SUCCEED();
}

TEST(HermesCompat, SharedRuntimeAdapterHandsBackItsRuntime) {
  std::shared_ptr<hc::HermesRuntime> runtime = hc::makeHermesRuntime();
  hc::inspector_modern::SharedRuntimeAdapter adapter(runtime);
  EXPECT_EQ(&adapter.getRuntime(), runtime.get());
  adapter.tickleJs();
}

// The whole point of carrying the real field set: code that configures the VM
// must compile, not fail on a header that left the fields out.
TEST(HermesCompatConfig, BuilderCompilesAndRoundTrips) {
  auto config = ::hermes::vm::RuntimeConfig::Builder()
                    .withEnableEval(false)
                    .withIntl(false)
                    .withMaxNumRegisters(2048)
                    .withGCConfig(::hermes::vm::GCConfig::Builder()
                                      .withInitHeapSize(1 << 20)
                                      .withName("test")
                                      .build())
                    .build();

  EXPECT_FALSE(config.getEnableEval());
  EXPECT_FALSE(config.getIntl());
  EXPECT_EQ(config.getMaxNumRegisters(), 2048u);
  EXPECT_EQ(config.getGCConfig().getInitHeapSize(), 1u << 20);
  EXPECT_EQ(config.getGCConfig().getName(), "test");

  // rebuild() keeps what was set and lets one field change.
  auto again = config.rebuild().withIntl(true).build();
  EXPECT_TRUE(again.getIntl());
  EXPECT_FALSE(again.getEnableEval());
}

TEST(HermesCompatConfig, DefaultsMatchHermesAndReportNothing) {
  Reports reports;
  ::hermes::vm::RuntimeConfig config;
  EXPECT_TRUE(config.getEnableEval());
  EXPECT_TRUE(config.getES6Promise());
  EXPECT_TRUE(config.getES6Proxy());
  EXPECT_TRUE(config.getIntl());
  EXPECT_FALSE(config.getEnableJIT());

  auto runtime = hc::makeHermesRuntime(config);
  ASSERT_NE(runtime, nullptr);
  EXPECT_EQ(reports.count(), 0u);
}

// A field this engine cannot honour must be named, not dropped.
TEST(HermesCompatConfig, SecuritySettingsAreReportedUnsupported) {
  Reports reports;
  auto config =
      ::hermes::vm::RuntimeConfig::Builder().withEnableEval(false).build();

  auto runtime = hc::makeHermesRuntime(config);
  ASSERT_NE(runtime, nullptr);
  EXPECT_TRUE(reports.saw(diag::Severity::Unsupported, "EnableEval"));

  // and it really is not honoured, which is why it had to be said out loud
  EXPECT_EQ(
      runtime
          ->evaluateJavaScript(
              std::make_shared<facebook::jsi::StringBuffer>("eval('1+1')"), "t")
          .getNumber(),
      2);
}

TEST(HermesCompatConfig, TuningSettingsAreReportedIgnored) {
  Reports reports;
  auto config = ::hermes::vm::RuntimeConfig::Builder()
                    .withEnableJIT(true)
                    .withGCConfig(::hermes::vm::GCConfig::Builder()
                                      .withMaxHeapSize(1 << 28)
                                      .build())
                    .build();

  auto runtime = hc::makeHermesRuntime(config);
  ASSERT_NE(runtime, nullptr);
  EXPECT_TRUE(reports.saw(diag::Severity::Ignored, "EnableJIT"));
  EXPECT_TRUE(reports.saw(diag::Severity::Ignored, "GCConfig"));
}
