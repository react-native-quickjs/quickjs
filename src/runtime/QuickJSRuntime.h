/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <jsi/jsi.h>
#include <quickjs.h>

#include <string>
#include <thread>

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

 private:
  QuickJSRuntimeConfig config_;
  JSRuntime *runtime_{nullptr};
  JSContext *context_{nullptr};
  std::thread::id jsThread_;
};

}  // namespace qjs
