/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <jsi/jsi.h>
#include <pthread.h>
#include <quickjs.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "QuickJSRuntimeConfig.h"

namespace qjs {

namespace jsi = facebook::jsi;

/// A jsi::Runtime backed by quickjs-ng.
class QuickJSRuntime : public jsi::Runtime {
 public:
  explicit QuickJSRuntime(QuickJSRuntimeConfig config);
  ~QuickJSRuntime() override;

  QuickJSRuntime(const QuickJSRuntime &) = delete;
  QuickJSRuntime &operator=(const QuickJSRuntime &) = delete;

  JSRuntime *runtime() const noexcept {
    return runtime_;
  }
  JSContext *context() const noexcept {
    return context_;
  }

  /**
   * Serialises access to the engine so more than one embedder can run
   * JavaScript on different threads. Lock where native enters JavaScript,
   * unlock when it leaves; the JSI methods in between do no locking.
   *
   * All-or-nothing: quickjs locks nothing itself, so once a second thread runs
   * JavaScript every entry point must hold this. A single-threaded embedder
   * never calls it and pays nothing. Dropping a jsi::Value is the exception --
   * an off-thread release is queued rather than blocked.
   */
  void lock() noexcept;
  void unlock() noexcept;

  class Lock {
   public:
    explicit Lock(QuickJSRuntime &runtime) noexcept : runtime_(runtime) {
      runtime_.lock();
    }
    ~Lock() {
      runtime_.unlock();
    }
    Lock(const Lock &) = delete;
    Lock &operator=(const Lock &) = delete;

   private:
    QuickJSRuntime &runtime_;
  };

  bool isOnJSThread() const noexcept {
    return jsThread_.load(std::memory_order_relaxed) ==
           std::this_thread::get_id();
  }

  /// Makes the calling thread the owner and re-bases the recursion bound onto
  /// its stack. A no-op when it already owns the engine. lock() calls it on
  /// every handover; an embedder that never locks calls it at its first
  /// evaluate, since React Native builds the runtime on one thread and runs
  /// JavaScript on another.
  void adoptCurrentThread() noexcept;

  static size_t remainingNativeStackBytes() noexcept;

  /// Holds one reference on a JSValue. invalidate() may run on any thread, so
  /// the release is routed through releaseValue(). Nested because
  /// jsi::Runtime::PointerValue is protected.
  class QuickJSPointerValue final : public PointerValue {
   public:
    QuickJSPointerValue(
        QuickJSRuntime &runtime, JSValue value, bool owned = true)
        : runtime_(runtime), owned_(owned), value_(value) {}

    void invalidate() noexcept override {
      if (owned_) {
        runtime_.releaseValue(value_);
      }
      runtime_.recyclePointerValue(this);
    }

    JSValue value() const noexcept {
      return value_;
    }

   private:
    friend class QuickJSRuntime;
    ~QuickJSPointerValue() override = default;

    QuickJSRuntime &runtime_;
    bool owned_;
    union {
      JSValue value_;
      QuickJSPointerValue *nextFree_;
    };
  };

  /// Backing store for jsi::PropNameID. Atoms are interned and comparable by
  /// identity, which is what PropNameID needs.
  class QuickJSAtomPointerValue final : public PointerValue {
   public:
    QuickJSAtomPointerValue(QuickJSRuntime &runtime, JSAtom atom)
        : runtime_(runtime), atom_(atom) {}

    void invalidate() noexcept override {
      runtime_.releaseAtom(atom_);
      runtime_.recycleAtomPointerValue(this);
    }

    JSAtom atom() const noexcept {
      return atom_;
    }

   private:
    friend class QuickJSRuntime;
    ~QuickJSAtomPointerValue() override = default;

    QuickJSRuntime &runtime_;
    union {
      JSAtom atom_;
      QuickJSAtomPointerValue *nextFree_;
    };
  };

  /// Safe to call from any thread: an off-thread release is queued and freed on
  /// the JS thread.
  void releaseValue(JSValue value) noexcept;
  void releaseAtom(JSAtom atom) noexcept;

  // jsi::Runtime -- public API
  jsi::Value evaluateJavaScript(
      const std::shared_ptr<const jsi::Buffer> &buffer,
      const std::string &sourceURL) override;
  std::shared_ptr<const jsi::PreparedJavaScript> prepareJavaScript(
      const std::shared_ptr<const jsi::Buffer> &buffer,
      std::string sourceURL) override;
  jsi::Value evaluatePreparedJavaScript(
      const std::shared_ptr<const jsi::PreparedJavaScript> &js) override;

  void queueMicrotask(const jsi::Function &callback) override;
  bool drainMicrotasks(int maxMicrotasksHint = -1) override;

  jsi::Object global() override;
  std::string description() override;
  bool isInspectable() override;

 protected:
  // jsi::Runtime -- pointer lifetime
  PointerValue *cloneSymbol(const Runtime::PointerValue *pv) override;
  PointerValue *cloneBigInt(const Runtime::PointerValue *pv) override;
  PointerValue *cloneString(const Runtime::PointerValue *pv) override;
  PointerValue *cloneObject(const Runtime::PointerValue *pv) override;
  PointerValue *clonePropNameID(const Runtime::PointerValue *pv) override;

  // PropNameID
  jsi::PropNameID createPropNameIDFromAscii(
      const char *str, size_t length) override;
  jsi::PropNameID createPropNameIDFromUtf8(
      const uint8_t *utf8, size_t length) override;
  jsi::PropNameID createPropNameIDFromUtf16(
      const char16_t *utf16, size_t length) override;
  jsi::PropNameID createPropNameIDFromString(const jsi::String &str) override;
  jsi::PropNameID createPropNameIDFromSymbol(const jsi::Symbol &sym) override;
  std::string utf8(const jsi::PropNameID &) override;
  bool compare(const jsi::PropNameID &, const jsi::PropNameID &) override;

  // Symbol
  std::string symbolToString(const jsi::Symbol &) override;

  // BigInt
  jsi::BigInt createBigIntFromInt64(int64_t) override;
  jsi::BigInt createBigIntFromUint64(uint64_t) override;
  bool bigintIsInt64(const jsi::BigInt &) override;
  bool bigintIsUint64(const jsi::BigInt &) override;
  uint64_t truncate(const jsi::BigInt &) override;
  jsi::String bigintToString(const jsi::BigInt &, int) override;

  // String
  jsi::String createStringFromAscii(const char *str, size_t length) override;
  jsi::String createStringFromUtf8(const uint8_t *utf8, size_t length) override;
  jsi::String createStringFromUtf16(
      const char16_t *utf16, size_t length) override;
  std::string utf8(const jsi::String &) override;

  // Object
  jsi::Object createObject() override;
  jsi::Object createObject(std::shared_ptr<jsi::HostObject> ho) override;
  std::shared_ptr<jsi::HostObject> getHostObject(const jsi::Object &) override;
  jsi::HostFunctionType &getHostFunction(const jsi::Function &) override;

  jsi::Object createObjectWithPrototype(const jsi::Value &prototype) override;
  void setPrototypeOf(
      const jsi::Object &object, const jsi::Value &prototype) override;
  jsi::Value getPrototypeOf(const jsi::Object &object) override;

  bool hasNativeState(const jsi::Object &) override;
  std::shared_ptr<jsi::NativeState> getNativeState(
      const jsi::Object &) override;
  void setNativeState(
      const jsi::Object &, std::shared_ptr<jsi::NativeState>) override;

  jsi::Value getProperty(
      const jsi::Object &, const jsi::PropNameID &name) override;
  jsi::Value getProperty(const jsi::Object &, const jsi::String &name) override;
  bool hasProperty(const jsi::Object &, const jsi::PropNameID &name) override;
  bool hasProperty(const jsi::Object &, const jsi::String &name) override;
  void setPropertyValue(
      const jsi::Object &, const jsi::PropNameID &name,
      const jsi::Value &value) override;
  void setPropertyValue(
      const jsi::Object &, const jsi::String &name,
      const jsi::Value &value) override;

  bool isArray(const jsi::Object &) const override;
  bool isArrayBuffer(const jsi::Object &) const override;
  bool isFunction(const jsi::Object &) const override;
  bool isHostObject(const jsi::Object &) const override;
  bool isHostFunction(const jsi::Function &) const override;
  jsi::Array getPropertyNames(const jsi::Object &) override;

  // WeakObject
  jsi::WeakObject createWeakObject(const jsi::Object &) override;
  jsi::Value lockWeakObject(const jsi::WeakObject &) override;

  // Array / ArrayBuffer
  jsi::Array createArray(size_t length) override;
  jsi::ArrayBuffer createArrayBuffer(
      std::shared_ptr<jsi::MutableBuffer> buffer) override;
  size_t size(const jsi::Array &) override;
  size_t size(const jsi::ArrayBuffer &) override;
  uint8_t *data(const jsi::ArrayBuffer &) override;
  jsi::Value getValueAtIndex(const jsi::Array &, size_t i) override;
  void setValueAtIndexImpl(
      const jsi::Array &, size_t i, const jsi::Value &value) override;

  // Function
  jsi::Function createFunctionFromHostFunction(
      const jsi::PropNameID &name, unsigned int paramCount,
      jsi::HostFunctionType func) override;
  jsi::Value call(
      const jsi::Function &, const jsi::Value &jsThis, const jsi::Value *args,
      size_t count) override;
  jsi::Value callAsConstructor(
      const jsi::Function &, const jsi::Value *args, size_t count) override;

  // Comparison
  bool strictEquals(const jsi::Symbol &a, const jsi::Symbol &b) const override;
  bool strictEquals(const jsi::BigInt &a, const jsi::BigInt &b) const override;
  bool strictEquals(const jsi::String &a, const jsi::String &b) const override;
  bool strictEquals(const jsi::Object &a, const jsi::Object &b) const override;
  bool instanceOf(const jsi::Object &o, const jsi::Function &f) override;

  void setExternalMemoryPressure(
      const jsi::Object &obj, size_t amount) override;

 public:
  // Conversions. Public because the class callbacks are free functions; they
  // are not part of the JSI surface.

  /// Takes ownership of the reference.
  jsi::Value createValue(JSValue value);

  /**
   * Wraps a JSValue without taking a reference of its own, so the caller's
   * reference must outlive the wrapper.
   *
   * Used for host function arguments: quickjs keeps argv alive for the whole
   * call, so dup'ing each argument on entry and freeing it on return is a pair
   * of refcount operations that provably cancel. Every path that keeps an
   * argument copies it, and every copy path takes a real reference, so a
   * borrowed wrapper cannot outlive the call that made it.
   */
  jsi::Value borrowValue(JSValue value);

  /// Takes the reference out of a jsi::Value that is about to be destroyed,
  /// rather than dup'ing a value whose destructor is about to free the
  /// original. Symmetric with borrowValue.
  JSValue takeJSValue(jsi::Value &&value);

  jsi::Object createObjectFrom(JSValue value);
  jsi::Symbol createSymbolFrom(JSValue value);
  jsi::String createStringFrom(JSValue value);
  jsi::PropNameID createPropNameIDFrom(JSAtom atom);

  /// Returns a borrowed reference; the caller must not free it.
  static JSValue toJSValue(const jsi::Pointer &pointer);
  JSValue toJSValue(const jsi::Value &value) const;
  static JSAtom toJSAtom(const jsi::PropNameID &name);

  /// Throws the pending quickjs exception as a jsi::JSError. Never returns.
  [[noreturn]] void throwPendingError();

  /// Describes `exception` using nothing that can re-enter JavaScript. Always
  /// returns a non-empty string for a non-empty exception and never leaves a
  /// pending exception behind.
  std::string describeExceptionWithoutRunningJS(JSValue exception);

  /// Throws if `value` is the exception sentinel, otherwise returns it.
  JSValue checkException(JSValue value);
  /// Throws if `result` is negative, the quickjs error convention.
  void checkException(int result);

  /// Converts an in-flight C++ exception into a pending quickjs exception.
  /// Called from class callbacks, which must not let C++ exceptions escape into
  /// the engine's C frames. `origin` names the kind of callback that threw.
  JSValue throwAsJSException(
      const std::exception *e, const char *origin = "HostFunction") noexcept;

 private:
  void drainPendingReleases() noexcept;
  JSValue evalInternal(const char *source) noexcept;

  // Pooled: one PointerValue per jsi value crossing the boundary, and
  // new/delete measured ~20 ns against a ~4.5 ns property lookup. Inline
  // because the call itself was a measurable part of that cost.
  QuickJSPointerValue *allocPointerValue(JSValue value, bool owned = true) {
    if (freeValues_ == nullptr) {
      refillValueSlab();
    }
    QuickJSPointerValue *slot = freeValues_;
    freeValues_ = slot->nextFree_;
    return new (static_cast<void *>(slot))
        QuickJSPointerValue(*this, value, owned);
  }

  QuickJSAtomPointerValue *allocAtomPointerValue(JSAtom atom) {
    if (freeAtoms_ == nullptr) {
      refillAtomSlab();
    }
    QuickJSAtomPointerValue *slot = freeAtoms_;
    freeAtoms_ = slot->nextFree_;
    return new (static_cast<void *>(slot)) QuickJSAtomPointerValue(*this, atom);
  }

  void recyclePointerValue(QuickJSPointerValue *pv) noexcept {
    if (isOnJSThread()) {
      pv->nextFree_ = freeValues_;
      freeValues_ = pv;
      return;
    }
    queuePointerValue(pv);
  }

  void recycleAtomPointerValue(QuickJSAtomPointerValue *pv) noexcept {
    if (isOnJSThread()) {
      pv->nextFree_ = freeAtoms_;
      freeAtoms_ = pv;
      return;
    }
    queueAtomPointerValue(pv);
  }

  void refillValueSlab();
  void refillAtomSlab();
  void queuePointerValue(QuickJSPointerValue *pv) noexcept;
  void queueAtomPointerValue(QuickJSAtomPointerValue *pv) noexcept;
  void freePointerValuePool() noexcept;

  static constexpr size_t kPointerValueSlabSize = 256;
  static constexpr int kMaxTeardownGCPasses = 2;

  QuickJSRuntimeConfig config_;
  JSRuntime *runtime_{nullptr};
  JSContext *context_{nullptr};

  // Atomic because ownership moves while other threads read it to route their
  // releases. Recursive so a host function called from JavaScript can lock
  // without deadlocking against the boundary lock it already runs under.
  std::atomic<std::thread::id> jsThread_;
  std::recursive_mutex engineMutex_;

  // Building a jsi::JSError reads .message and .stack, which can throw again.
  int errorDepth_{0};
  static constexpr int kMaxErrorDepth = 8;

  // Described on the way in, because by the time the nesting bound is hit the
  // original exception is gone and the bail-out would name only itself.
  std::string firstErrorDescription_;

  // Engine facilities with no C API, evaluated once at startup.
  JSValue enumeratePropertyNames_{JS_UNDEFINED};

  QuickJSPointerValue *freeValues_{nullptr};
  QuickJSAtomPointerValue *freeAtoms_{nullptr};
  std::vector<void *> valueSlabs_;
  std::vector<void *> atomSlabs_;

  std::atomic<bool> hasPendingReleases_{false};
  std::mutex pendingMutex_;
  std::vector<JSValue> pendingValues_;
  std::vector<JSAtom> pendingAtoms_;
  std::vector<QuickJSPointerValue *> pendingPointerValues_;
  std::vector<QuickJSAtomPointerValue *> pendingAtomPointerValues_;
};

}  // namespace qjs
