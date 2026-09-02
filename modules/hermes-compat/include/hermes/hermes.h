/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * A source-compatible <hermes/hermes.h> whose makeHermesRuntime() returns a
 * QuickJS-backed runtime, so libraries that create their own Hermes runtime --
 * react-native-worklets, and so react-native-reanimated -- build and run
 * unmodified. Those libraries select an engine with __has_include(<hermes/
 * hermes.h>) and have no third-engine branch, so without this they cannot be
 * used at all.
 *
 * This is source compatibility, never ABI compatibility. These headers must
 * not be on the include path while a real libhermes is on the link line.
 *
 * Rule when editing: no member declaration below may be conditional on a
 * preprocessor macro. Free functions may be; virtual functions and data
 * members may not. A conditional virtual shifts every vtable slot after it,
 * and consumers are compiled separately from this shim with flags it does not
 * control -- the failure is a silent call to the wrong function.
 */

#pragma once

#include <hermes/DebuggerAPI.h>
#include <hermes/Public/HermesExport.h>
#include <hermes/Public/RuntimeConfig.h>
#include <hermes/Public/SamplingProfiler.h>
#include <jsi/jsi.h>

#include <memory>
#include <ostream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct SHUnit;
struct SHRuntime;

namespace hermes::vm {
class GCExecTrace {
 public:
  GCExecTrace() = default;
};
class Runtime;
}  // namespace hermes::vm

namespace facebook {
namespace jsi {
class ThreadSafeRuntime;
}

namespace hermes {

class HermesRuntime;

/// Engine-global operations that need no runtime instance. The UUIDs match
/// real Hermes so that castInterface in unmodified third-party code succeeds.
class HERMES_EXPORT IHermesRootAPI : public jsi::ICast {
 public:
  static constexpr jsi::UUID uuid{
      0xb654d898, 0xdfad, 0x11ef, 0x859a, 0x325096b39f47};

  virtual std::unique_ptr<HermesRuntime> makeHermesRuntime(
      const ::hermes::vm::RuntimeConfig &runtimeConfig) = 0;

  /// Always false: nothing here produces Hermes bytecode. A caller handed a
  /// real .hbc bundle takes its "this is source" path and fails in the parser,
  /// so the shim sniffs the HBC magic and reports, naming the real cause.
  virtual bool isHermesBytecode(const uint8_t *data, size_t len) = 0;
  virtual uint32_t getBytecodeVersion() = 0;
  virtual void prefetchHermesBytecode(const uint8_t *data, size_t len) = 0;
  virtual bool hermesBytecodeSanityCheck(
      const uint8_t *data, size_t len, std::string *errorMessage) = 0;
  virtual void setFatalHandler(void (*handler)(const std::string &)) = 0;
  virtual std::pair<const uint8_t *, size_t> getBytecodeEpilogue(
      const uint8_t *data, size_t len) = 0;
  virtual void enableSamplingProfiler(double meanHzFreq) = 0;
  virtual void disableSamplingProfiler() = 0;
  virtual void dumpSampledTraceToFile(const std::string &fileName) = 0;
  virtual void dumpSampledTraceToStream(std::ostream &stream) = 0;
  virtual std::unordered_map<std::string, std::vector<std::string>>
  getExecutedFunctions() = 0;
  virtual bool isCodeCoverageProfilerEnabled() = 0;
  virtual void enableCodeCoverageProfiler() = 0;
  virtual void disableCodeCoverageProfiler() = 0;

 protected:
  ~IHermesRootAPI() {}
};

class HERMES_EXPORT ISetFatalHandler : public jsi::ICast {
 public:
  static constexpr jsi::UUID uuid{
      0xda98a610, 0x09cb, 0x11f0, 0x87bf, 0x325096b39f47};
  virtual void setFatalHandler(void (*handler)(const std::string &)) = 0;

 protected:
  ~ISetFatalHandler() = default;
};

/// Hermes-specific per-runtime methods.
class HERMES_EXPORT IHermes : public jsi::ICast {
 public:
  static constexpr jsi::UUID uuid{
      0xe85cfa22, 0xdfae, 0x11ef, 0xa6f7, 0x325096b39f47};

  virtual ICast *getHermesRootAPI() = 0;
  virtual void sampledTraceToStreamInDevToolsFormat(std::ostream &stream) = 0;
  virtual sampling_profiler::Profile dumpSampledTraceToProfile() = 0;
  virtual void resetTimezoneCache() = 0;
  virtual void loadSegment(
      std::unique_ptr<const jsi::Buffer> buffer, const jsi::Value &context) = 0;

  /// QuickJS heap objects do not move, so an object's address is a stable id
  /// for its lifetime -- the contract Hermes documents, including that an id
  /// may be reused after the object is collected.
  virtual uint64_t getUniqueID(const jsi::Object &o) const = 0;
  virtual uint64_t getUniqueID(const jsi::BigInt &s) const = 0;
  virtual uint64_t getUniqueID(const jsi::String &s) const = 0;
  virtual uint64_t getUniqueID(const jsi::PropNameID &pni) const = 0;
  virtual uint64_t getUniqueID(const jsi::Symbol &sym) const = 0;
  virtual uint64_t getUniqueID(const jsi::Value &val) const = 0;
  virtual jsi::Value getObjectForID(uint64_t id) = 0;

  virtual const ::hermes::vm::GCExecTrace &getGCExecTrace() const = 0;
  virtual std::string getIOTrackingInfoJSON() = 0;
  virtual debugger::Debugger &getDebugger() = 0;

  struct DebugFlags {};

  virtual void debugJavaScript(
      const std::string &src, const std::string &sourceURL,
      const DebugFlags &debugFlags) = 0;
  virtual void registerForProfiling() = 0;
  virtual void unregisterForProfiling() = 0;

  /// Backed by the engine's interrupt handler, which the interpreter polls.
  virtual void asyncTriggerTimeout() = 0;
  virtual void watchTimeLimit(uint32_t timeoutInMs) = 0;
  virtual void unwatchTimeLimit() = 0;

  virtual jsi::Value evaluateJavaScriptWithSourceMap(
      const std::shared_ptr<const jsi::Buffer> &buffer,
      const std::shared_ptr<const jsi::Buffer> &sourceMapBuf,
      const std::string &sourceURL) = 0;
  virtual jsi::Value evaluateSHUnit(SHUnit *(*shUnitCreator)()) = 0;
  virtual SHRuntime *getSHRuntime() noexcept = 0;

  /// Returns nullptr. Handing back the JSContext* would let a caller
  /// reinterpret_cast it to hermes::vm::Runtime* and corrupt memory.
  virtual void *getVMRuntimeUnsafe() const = 0;

 protected:
  ~IHermes() = default;
};

class HERMES_EXPORT IHermesTestHelpers : public jsi::ICast {
 public:
  static constexpr jsi::UUID uuid{
      0x664e489a, 0xf941, 0x11ef, 0xa44c, 0x325096b39f47};
  virtual size_t rootsListLengthForTests() const = 0;

 protected:
  ~IHermesTestHelpers() = default;
};

class HermesRuntime : public jsi::Runtime, public IHermes {
 public:
  ~HermesRuntime() override = default;

  using jsi::Runtime::castInterface;
};

HERMES_EXPORT jsi::ICast *makeHermesRootAPI();

HERMES_EXPORT ::hermes::vm::RuntimeConfig hardenedHermesRuntimeConfig();

HERMES_EXPORT std::unique_ptr<HermesRuntime> makeHermesRuntime(
    const ::hermes::vm::RuntimeConfig &runtimeConfig =
        ::hermes::vm::RuntimeConfig());

/// Returns nullptr rather than an unlocked runtime: a caller expecting
/// Hermes's ThreadSafeRuntime semantics should not silently get less.
HERMES_EXPORT std::unique_ptr<jsi::ThreadSafeRuntime>
makeThreadSafeHermesRuntime(
    const ::hermes::vm::RuntimeConfig &runtimeConfig =
        ::hermes::vm::RuntimeConfig());

}  // namespace hermes
}  // namespace facebook
