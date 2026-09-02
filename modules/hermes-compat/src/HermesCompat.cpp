/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes-compat/Diagnostics.h>
#include <hermes/hermes.h>
#include <hermes/inspector/RuntimeAdapter.h>
#include <jsi/decorator.h>
#include <jsi/threadsafe.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_set>

#include "QuickJSRuntime.h"
#include "QuickJSRuntimeFactory.h"

#ifdef __ANDROID__
#include <android/log.h>
#endif

// ---------------------------------------------------------------- diagnostics

namespace qjs::hermescompat {
namespace {

std::mutex gMutex;
Handler gHandler;
std::unordered_set<const void *> gSeen;

const char *name(Severity severity) {
  switch (severity) {
    case Severity::Degraded:
      return "degraded";
    case Severity::Ignored:
      return "ignored";
    case Severity::Unsupported:
      return "unsupported";
  }
  return "unknown";
}

}  // namespace

void setHandler(Handler handler) {
  std::lock_guard<std::mutex> lock(gMutex);
  gHandler = std::move(handler);
}

void report(Severity severity, const char *api, const char *detail) {
  Handler handler;
  {
    std::lock_guard<std::mutex> lock(gMutex);
    if (!gSeen.insert(api).second) {
      return;
    }
    handler = gHandler;
  }

  if (handler) {
    handler(severity, api, detail);
    return;
  }
#ifdef __ANDROID__
  __android_log_print(
      ANDROID_LOG_WARN, "hermes-compat", "%s: %s -- %s", name(severity), api,
      detail);
#else
  std::fprintf(
      stderr, "[hermes-compat] %s: %s -- %s\n", name(severity), api, detail);
#endif
}

void resetForTesting() {
  std::lock_guard<std::mutex> lock(gMutex);
  gSeen.clear();
  gHandler = nullptr;
}

}  // namespace qjs::hermescompat

// -------------------------------------------------------------------- runtime

namespace facebook::hermes {
namespace {

namespace diag = qjs::hermescompat;
using diag::Severity;

/// Hermes bytecode files open with this, little-endian (BytecodeFileFormat.h).
/// Sniffed so that "this is plain JavaScript" and "this is an HBC bundle this
/// app can never run" are not reported as the same answer.
constexpr uint64_t kHermesMagic = 0x1F1903C103BC1FC6ULL;

void degraded(const char *api, const char *why) {
  diag::report(Severity::Degraded, api, why);
}
void ignored(const char *api, const char *why) {
  diag::report(Severity::Ignored, api, why);
}
void unsupported(const char *api, const char *why) {
  diag::report(Severity::Unsupported, api, why);
}

bool looksLikeHermesBytecode(const uint8_t *data, size_t len) {
  if (data == nullptr || len < sizeof(kHermesMagic)) {
    return false;
  }
  uint64_t magic = 0;
  std::memcpy(&magic, data, sizeof(magic));
  return magic == kHermesMagic;
}

class RootAPI final : public IHermesRootAPI, public ISetFatalHandler {
 public:
  jsi::ICast *castInterface(const jsi::UUID &uuid) override {
    if (uuid == IHermesRootAPI::uuid)
      return static_cast<IHermesRootAPI *>(this);
    if (uuid == ISetFatalHandler::uuid)
      return static_cast<ISetFatalHandler *>(this);
    return nullptr;
  }

  std::unique_ptr<HermesRuntime> makeHermesRuntime(
      const ::hermes::vm::RuntimeConfig &config) override;

  bool isHermesBytecode(const uint8_t *data, size_t len) override {
    if (looksLikeHermesBytecode(data, len)) {
      unsupported(
          "IHermesRootAPI::isHermesBytecode",
          "cannot execute HBC; rebuild as JavaScript or with "
          "tools/bytecode/qjsc");
    }
    return false;
  }

  uint32_t getBytecodeVersion() override {
    unsupported(
        "IHermesRootAPI::getBytecodeVersion",
        "no HBC version here; returning 0");
    return 0;
  }

  void prefetchHermesBytecode(const uint8_t *, size_t) override {
    ignored(
        "IHermesRootAPI::prefetchHermesBytecode",
        "an madvise hint for HBC files; nothing to do");
  }

  bool hermesBytecodeSanityCheck(
      const uint8_t *data, size_t len, std::string *errorMessage) override {
    if (errorMessage != nullptr) {
      *errorMessage = looksLikeHermesBytecode(data, len)
                          ? "Hermes bytecode, which this app cannot execute"
                          : "not Hermes bytecode (this app runs QuickJS)";
    }
    return false;
  }

  void setFatalHandler(void (*)(const std::string &)) override {
    unsupported(
        "IHermesRootAPI::setFatalHandler",
        "no fatal handler exists; it would never be called");
  }

  std::pair<const uint8_t *, size_t> getBytecodeEpilogue(
      const uint8_t *, size_t) override {
    unsupported(
        "IHermesRootAPI::getBytecodeEpilogue",
        "an HBC concept; returning empty");
    return {nullptr, 0};
  }

  void enableSamplingProfiler(double) override {
    noProfiler("enableSamplingProfiler");
  }
  void disableSamplingProfiler() override {
    noProfiler("disableSamplingProfiler");
  }
  void dumpSampledTraceToFile(const std::string &) override {
    noProfiler("dumpSampledTraceToFile");
  }
  void dumpSampledTraceToStream(std::ostream &) override {
    noProfiler("dumpSampledTraceToStream");
  }

  std::unordered_map<std::string, std::vector<std::string>>
  getExecutedFunctions() override {
    unsupported(
        "IHermesRootAPI::getExecutedFunctions", "not tracked; returning empty");
    return {};
  }

  bool isCodeCoverageProfilerEnabled() override {
    return false;
  }
  void enableCodeCoverageProfiler() override {
    noCoverage("enableCodeCoverageProfiler");
  }
  void disableCodeCoverageProfiler() override {
    noCoverage("disableCodeCoverageProfiler");
  }

 private:
  static void noProfiler(const char *api) {
    unsupported(api, "QuickJS has no sampling profiler");
  }
  static void noCoverage(const char *api) {
    unsupported(api, "QuickJS has no code coverage profiler");
  }
};

RootAPI &rootAPI() {
  static RootAPI api;
  return api;
}

/// RuntimeDecorator forwards every jsi::Runtime virtual to the QuickJS runtime,
/// so a change to JSI is a compile error here rather than a silent divergence.
/// Only the Hermes-specific half is written out below.
class CompatRuntime final
    : public jsi::RuntimeDecorator<jsi::Runtime, HermesRuntime> {
 public:
  explicit CompatRuntime(std::unique_ptr<jsi::Runtime> plain)
      : jsi::RuntimeDecorator<jsi::Runtime, HermesRuntime>(*plain),
        owned_(std::move(plain)),
        quickjs_(dynamic_cast<qjs::QuickJSRuntime *>(owned_.get())) {}

  ~CompatRuntime() override {
    unwatchTimeLimit();
  }

  jsi::ICast *castInterface(const jsi::UUID &uuid) override {
    if (uuid == IHermes::uuid) return static_cast<IHermes *>(this);
    return plain().castInterface(uuid);
  }

  ICast *getHermesRootAPI() override {
    return static_cast<IHermesRootAPI *>(&rootAPI());
  }

  void sampledTraceToStreamInDevToolsFormat(std::ostream &) override {
    noProfiler("sampledTraceToStreamInDevToolsFormat");
  }

  sampling_profiler::Profile dumpSampledTraceToProfile() override {
    noProfiler("dumpSampledTraceToProfile");
    return {};
  }

  /// QuickJS reads the zone on every conversion and keeps no cache of its
  /// own, so the one that can go stale is libc's.
  void resetTimezoneCache() override {
    ::tzset();
  }

  void loadSegment(
      std::unique_ptr<const jsi::Buffer>, const jsi::Value &) override {
    unsupported(
        "IHermes::loadSegment", "segmented and RAM bundles are HBC features");
  }

  uint64_t getUniqueID(const jsi::Object &o) const override {
    return id(o);
  }
  uint64_t getUniqueID(const jsi::BigInt &b) const override {
    return id(b);
  }
  uint64_t getUniqueID(const jsi::String &s) const override {
    return id(s);
  }
  uint64_t getUniqueID(const jsi::PropNameID &p) const override {
    return id(p);
  }
  uint64_t getUniqueID(const jsi::Symbol &s) const override {
    return id(s);
  }

  uint64_t getUniqueID(const jsi::Value &v) const override {
    if (quickjs_ == nullptr ||
        !(v.isObject() || v.isString() || v.isSymbol() || v.isBigInt())) {
      return 0;  // Hermes documents 0 as "no id for this value".
    }
    return reinterpret_cast<uint64_t>(JS_VALUE_GET_PTR(quickjs_->toJSValue(v)));
  }

  jsi::Value getObjectForID(uint64_t) override {
    unsupported(
        "IHermes::getObjectForID",
        "no id-to-object registry is kept; returning null");
    return jsi::Value::null();
  }

  const ::hermes::vm::GCExecTrace &getGCExecTrace() const override {
    static const ::hermes::vm::GCExecTrace kEmpty;
    unsupported("IHermes::getGCExecTrace", "not recorded");
    return kEmpty;
  }

  std::string getIOTrackingInfoJSON() override {
    unsupported("IHermes::getIOTrackingInfoJSON", "not tracked; returning {}");
    return "{}";
  }

  debugger::Debugger &getDebugger() override {
    static debugger::Debugger kEmpty;
    unsupported(
        "IHermes::getDebugger",
        "debugging goes through jsinspector-modern, not this API");
    return kEmpty;
  }

  /// Hermes disables optimisation so debug info stays exact; QuickJS's
  /// interpreter has no separate optimised mode, so this is a plain evaluate.
  void debugJavaScript(
      const std::string &src, const std::string &sourceURL,
      const DebugFlags &) override {
    plain().evaluateJavaScript(
        std::make_shared<jsi::StringBuffer>(src), sourceURL);
  }

  void registerForProfiling() override {
    noProfiler("registerForProfiling");
  }
  void unregisterForProfiling() override {
    noProfiler("unregisterForProfiling");
  }

  void asyncTriggerTimeout() override {
    interrupt_.store(true, std::memory_order_relaxed);
  }

  void watchTimeLimit(uint32_t timeoutInMs) override {
    if (quickjs_ == nullptr) {
      unsupported(
          "IHermes::watchTimeLimit",
          "the wrapped runtime is not a QuickJSRuntime; no limit is enforced");
      return;
    }
    deadline_.store(nowMs() + timeoutInMs, std::memory_order_relaxed);
    JS_SetInterruptHandler(
        quickjs_->runtime(), &CompatRuntime::onInterrupt, this);
  }

  void unwatchTimeLimit() override {
    deadline_.store(0, std::memory_order_relaxed);
    interrupt_.store(false, std::memory_order_relaxed);
    if (quickjs_ != nullptr) {
      JS_SetInterruptHandler(quickjs_->runtime(), nullptr, nullptr);
    }
  }

  /// The source map is accepted and ignored: QuickJS does not apply source
  /// maps to stack traces.
  jsi::Value evaluateJavaScriptWithSourceMap(
      const std::shared_ptr<const jsi::Buffer> &buffer,
      const std::shared_ptr<const jsi::Buffer> &,
      const std::string &sourceURL) override {
    degraded(
        "IHermes::evaluateJavaScriptWithSourceMap",
        "source map ignored; stack traces point at generated code");
    return plain().evaluateJavaScript(buffer, sourceURL);
  }

  jsi::Value evaluateSHUnit(SHUnit *(*)()) override {
    unsupported("IHermes::evaluateSHUnit", "Static Hermes only");
    return jsi::Value::undefined();
  }

  SHRuntime *getSHRuntime() noexcept override {
    return nullptr;
  }

  void *getVMRuntimeUnsafe() const override {
    unsupported(
        "IHermes::getVMRuntimeUnsafe",
        "returning null; a JSContext cast to hermes::vm::Runtime corrupts");
    return nullptr;
  }

 private:
  template <typename T>
  uint64_t id(const T &pointer) const {
    return reinterpret_cast<uint64_t>(
        JS_VALUE_GET_PTR(qjs::QuickJSRuntime::toJSValue(pointer)));
  }

  static void noProfiler(const char *api) {
    unsupported(api, "QuickJS has no sampling profiler");
  }

  static uint64_t nowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
  }

  static int onInterrupt(JSRuntime *, void *opaque) {
    auto *self = static_cast<CompatRuntime *>(opaque);
    if (self->interrupt_.load(std::memory_order_relaxed)) return 1;
    const uint64_t deadline = self->deadline_.load(std::memory_order_relaxed);
    return deadline != 0 && nowMs() > deadline ? 1 : 0;
  }

  std::unique_ptr<jsi::Runtime> owned_;
  qjs::QuickJSRuntime *quickjs_;
  std::atomic<uint64_t> deadline_{0};
  std::atomic<bool> interrupt_{false};
};

/// A config field set away from its Hermes default is a request this engine
/// cannot carry out. Silently running with the opposite of what was asked is
/// the failure this shim exists to prevent, so each one is named: unsupported
/// where it changes what JavaScript may do, ignored where nothing observable
/// depends on it.
void reportUnhonoured(const ::hermes::vm::RuntimeConfig &config) {
  using Config = ::hermes::vm::RuntimeConfig;
#define QJS_IF_SET(FIELD) \
  if (config.get##FIELD() != Config::getDefault##FIELD())

  QJS_IF_SET(EnableEval)
  unsupported("RuntimeConfig::EnableEval", "eval and Function stay enabled");
  QJS_IF_SET(ES6Promise)
  unsupported(
      "RuntimeConfig::ES6Promise", "Promise stays as the engine has it");
  QJS_IF_SET(ES6Proxy)
  unsupported("RuntimeConfig::ES6Proxy", "Proxy stays as the engine has it");
  QJS_IF_SET(Intl)
  unsupported("RuntimeConfig::Intl", "Intl stays as the engine has it");
  QJS_IF_SET(EnableGenerator)
  unsupported("RuntimeConfig::EnableGenerator", "generators stay enabled");
  QJS_IF_SET(EnableHermesInternal)
  unsupported(
      "RuntimeConfig::EnableHermesInternal",
      "HermesInternal is installed by the runtime, not by this");
  QJS_IF_SET(MaxNumRegisters)
  unsupported(
      "RuntimeConfig::MaxNumRegisters", "the stack limit is the engine's own");
  QJS_IF_SET(VMExperimentFlags)
  unsupported("RuntimeConfig::VMExperimentFlags", "no Hermes VM to configure");

  QJS_IF_SET(EnableJIT)
  ignored("RuntimeConfig::EnableJIT", "QuickJS interprets; there is no JIT");
  QJS_IF_SET(TraceEnabled)
  ignored("RuntimeConfig::TraceEnabled", "no synth trace is recorded");
  QJS_IF_SET(TrackIO)
  ignored("RuntimeConfig::TrackIO", "no IO tracking");
  QJS_IF_SET(EnableSampleProfiling)
  ignored("RuntimeConfig::EnableSampleProfiling", "no sampling profiler");
  QJS_IF_SET(EnableSampledStats)
  ignored("RuntimeConfig::EnableSampledStats", "no sampled stats");
  QJS_IF_SET(BytecodeWarmupPercent)
  ignored("RuntimeConfig::BytecodeWarmupPercent", "an HBC warmup hint");
  QJS_IF_SET(RandomizeMemoryLayout)
  ignored("RuntimeConfig::RandomizeMemoryLayout", "layout is the allocator's");
#undef QJS_IF_SET

  const auto &gc = config.getGCConfig();
  using GC = ::hermes::vm::GCConfig;
  if (gc.getMinHeapSize() != GC::getDefaultMinHeapSize() ||
      gc.getInitHeapSize() != GC::getDefaultInitHeapSize() ||
      gc.getMaxHeapSize() != GC::getDefaultMaxHeapSize()) {
    ignored("RuntimeConfig::GCConfig", "QuickJS sizes its own heap");
  }
}

std::unique_ptr<HermesRuntime> RootAPI::makeHermesRuntime(
    const ::hermes::vm::RuntimeConfig &config) {
  reportUnhonoured(config);
  return std::make_unique<CompatRuntime>(qjs::makeQuickJSRuntime());
}

}  // namespace

jsi::ICast *makeHermesRootAPI() {
  // RootAPI reaches ICast through both of its bases, so the path is named.
  return static_cast<IHermesRootAPI *>(&rootAPI());
}

::hermes::vm::RuntimeConfig hardenedHermesRuntimeConfig() {
  unsupported(
      "hardenedHermesRuntimeConfig",
      "no hardening switches exist; returning a default config");
  return {};
}

std::unique_ptr<HermesRuntime> makeHermesRuntime(
    const ::hermes::vm::RuntimeConfig &config) {
  return rootAPI().makeHermesRuntime(config);
}

std::unique_ptr<jsi::ThreadSafeRuntime> makeThreadSafeHermesRuntime(
    const ::hermes::vm::RuntimeConfig &) {
  unsupported(
      "makeThreadSafeHermesRuntime",
      "returning null rather than an unlocked runtime");
  return nullptr;
}

// ------------------------------------------------------------------- adapters

namespace inspector_modern {

RuntimeAdapter::~RuntimeAdapter() = default;
void RuntimeAdapter::tickleJs() {}

SharedRuntimeAdapter::SharedRuntimeAdapter(
    std::shared_ptr<HermesRuntime> runtime)
    : runtime_(std::move(runtime)) {}
SharedRuntimeAdapter::~SharedRuntimeAdapter() = default;
HermesRuntime &SharedRuntimeAdapter::getRuntime() {
  return *runtime_;
}

}  // namespace inspector_modern
}  // namespace facebook::hermes

#ifdef HERMES_ENABLE_DEBUGGER
#include <hermes/inspector-modern/chrome/Registration.h>

namespace facebook::hermes::inspector_modern::chrome {

DebugSessionToken enableDebugging(
    std::unique_ptr<RuntimeAdapter>, const std::string &) {
  qjs::hermescompat::report(
      qjs::hermescompat::Severity::Unsupported, "chrome::enableDebugging",
      "not registered as a DevTools target; the app's main runtime is, a "
      "runtime made through this shim is not");
  static std::atomic<DebugSessionToken> next{1};
  return next.fetch_add(1, std::memory_order_relaxed);
}

void disableDebugging(DebugSessionToken) {}

}  // namespace facebook::hermes::inspector_modern::chrome
#endif  // HERMES_ENABLE_DEBUGGER
