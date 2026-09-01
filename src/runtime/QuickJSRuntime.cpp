/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "QuickJSRuntime.h"

#include <type_traits>
#include <utility>

namespace qjs {

// jsi::Runtime is abstract, so a missing override would otherwise only show up
// at the first attempt to construct one, which is in another translation unit.
static_assert(
    !std::is_abstract<QuickJSRuntime>::value,
    "QuickJSRuntime does not implement every jsi::Runtime method");

namespace {

[[noreturn]] void notImplemented(const char *what) {
  throw jsi::JSINativeException(
      std::string("QuickJSRuntime::") + what + " is not implemented yet");
}

}  // namespace

QuickJSRuntime::QuickJSRuntime(QuickJSRuntimeConfig config)
    : config_(std::move(config)), jsThread_(std::this_thread::get_id()) {
  runtime_ = JS_NewRuntime();
  if (runtime_ == nullptr) {
    throw jsi::JSINativeException("QuickJSRuntime: JS_NewRuntime failed");
  }

  if (config_.memoryLimit > 0) {
    JS_SetMemoryLimit(runtime_, config_.memoryLimit);
  }
  if (config_.stackSize > 0) {
    JS_SetMaxStackSize(runtime_, config_.stackSize);
  }

  context_ = JS_NewContext(runtime_);
  if (context_ == nullptr) {
    JS_FreeRuntime(runtime_);
    runtime_ = nullptr;
    throw jsi::JSINativeException("QuickJSRuntime: JS_NewContext failed");
  }

  JS_SetContextOpaque(context_, this);
  JS_SetRuntimeOpaque(runtime_, this);
}

QuickJSRuntime::~QuickJSRuntime() {
  if (context_ != nullptr) {
    JS_FreeContext(context_);
  }
  if (runtime_ != nullptr) {
    JS_FreeRuntime(runtime_);
  }
}

std::string QuickJSRuntime::description() {
  return "QuickJSRuntime";
}

/// Tied to enableDebugger rather than an unconditional true: a runtime built
/// without instrumentation cannot bind a breakpoint, because the statement
/// traps are emitted at parse time.
bool QuickJSRuntime::isInspectable() {
  return config_.enableDebugger;
}

jsi::Value QuickJSRuntime::evaluateJavaScript(
    const std::shared_ptr<const jsi::Buffer> &buffer,
    const std::string &sourceURL) {
  notImplemented("evaluateJavaScript");
}

std::shared_ptr<const jsi::PreparedJavaScript>
QuickJSRuntime::prepareJavaScript(
    const std::shared_ptr<const jsi::Buffer> &buffer, std::string sourceURL) {
  notImplemented("prepareJavaScript");
}

jsi::Value QuickJSRuntime::evaluatePreparedJavaScript(
    const std::shared_ptr<const jsi::PreparedJavaScript> &js) {
  notImplemented("evaluatePreparedJavaScript");
}

void QuickJSRuntime::queueMicrotask(const jsi::Function &callback) {
  notImplemented("queueMicrotask");
}

bool QuickJSRuntime::drainMicrotasks(int maxMicrotasksHint) {
  notImplemented("drainMicrotasks");
}

jsi::Object QuickJSRuntime::global() {
  notImplemented("global");
}

jsi::Runtime::PointerValue *QuickJSRuntime::cloneSymbol(
    const Runtime::PointerValue *pv) {
  notImplemented("cloneSymbol");
}

jsi::Runtime::PointerValue *QuickJSRuntime::cloneBigInt(
    const Runtime::PointerValue *pv) {
  notImplemented("cloneBigInt");
}

jsi::Runtime::PointerValue *QuickJSRuntime::cloneString(
    const Runtime::PointerValue *pv) {
  notImplemented("cloneString");
}

jsi::Runtime::PointerValue *QuickJSRuntime::cloneObject(
    const Runtime::PointerValue *pv) {
  notImplemented("cloneObject");
}

jsi::Runtime::PointerValue *QuickJSRuntime::clonePropNameID(
    const Runtime::PointerValue *pv) {
  notImplemented("clonePropNameID");
}

jsi::PropNameID QuickJSRuntime::createPropNameIDFromAscii(
    const char *str, size_t length) {
  notImplemented("createPropNameIDFromAscii");
}

jsi::PropNameID QuickJSRuntime::createPropNameIDFromUtf8(
    const uint8_t *utf8, size_t length) {
  notImplemented("createPropNameIDFromUtf8");
}

jsi::PropNameID QuickJSRuntime::createPropNameIDFromUtf16(
    const char16_t *utf16, size_t length) {
  notImplemented("createPropNameIDFromUtf16");
}

jsi::PropNameID QuickJSRuntime::createPropNameIDFromString(
    const jsi::String &str) {
  notImplemented("createPropNameIDFromString");
}

jsi::PropNameID QuickJSRuntime::createPropNameIDFromSymbol(
    const jsi::Symbol &sym) {
  notImplemented("createPropNameIDFromSymbol");
}

std::string QuickJSRuntime::utf8(const jsi::PropNameID &) {
  notImplemented("utf8");
}

bool QuickJSRuntime::compare(const jsi::PropNameID &, const jsi::PropNameID &) {
  notImplemented("compare");
}

std::string QuickJSRuntime::symbolToString(const jsi::Symbol &) {
  notImplemented("symbolToString");
}

jsi::BigInt QuickJSRuntime::createBigIntFromInt64(int64_t) {
  notImplemented("createBigIntFromInt64");
}

jsi::BigInt QuickJSRuntime::createBigIntFromUint64(uint64_t) {
  notImplemented("createBigIntFromUint64");
}

bool QuickJSRuntime::bigintIsInt64(const jsi::BigInt &) {
  notImplemented("bigintIsInt64");
}

bool QuickJSRuntime::bigintIsUint64(const jsi::BigInt &) {
  notImplemented("bigintIsUint64");
}

uint64_t QuickJSRuntime::truncate(const jsi::BigInt &) {
  notImplemented("truncate");
}

jsi::String QuickJSRuntime::bigintToString(const jsi::BigInt &, int) {
  notImplemented("bigintToString");
}

jsi::String QuickJSRuntime::createStringFromAscii(
    const char *str, size_t length) {
  notImplemented("createStringFromAscii");
}

jsi::String QuickJSRuntime::createStringFromUtf8(
    const uint8_t *utf8, size_t length) {
  notImplemented("createStringFromUtf8");
}

jsi::String QuickJSRuntime::createStringFromUtf16(
    const char16_t *utf16, size_t length) {
  notImplemented("createStringFromUtf16");
}

std::string QuickJSRuntime::utf8(const jsi::String &) {
  notImplemented("utf8");
}

jsi::Object QuickJSRuntime::createObject() {
  notImplemented("createObject");
}

jsi::Object QuickJSRuntime::createObject(std::shared_ptr<jsi::HostObject> ho) {
  notImplemented("createObject");
}

std::shared_ptr<jsi::HostObject> QuickJSRuntime::getHostObject(
    const jsi::Object &) {
  notImplemented("getHostObject");
}

jsi::HostFunctionType &QuickJSRuntime::getHostFunction(const jsi::Function &) {
  notImplemented("getHostFunction");
}

jsi::Object QuickJSRuntime::createObjectWithPrototype(
    const jsi::Value &prototype) {
  notImplemented("createObjectWithPrototype");
}

void QuickJSRuntime::setPrototypeOf(
    const jsi::Object &object, const jsi::Value &prototype) {
  notImplemented("setPrototypeOf");
}

jsi::Value QuickJSRuntime::getPrototypeOf(const jsi::Object &object) {
  notImplemented("getPrototypeOf");
}

bool QuickJSRuntime::hasNativeState(const jsi::Object &) {
  notImplemented("hasNativeState");
}

std::shared_ptr<jsi::NativeState> QuickJSRuntime::getNativeState(
    const jsi::Object &) {
  notImplemented("getNativeState");
}

void QuickJSRuntime::setNativeState(
    const jsi::Object &, std::shared_ptr<jsi::NativeState>) {
  notImplemented("setNativeState");
}

jsi::Value QuickJSRuntime::getProperty(
    const jsi::Object &, const jsi::PropNameID &name) {
  notImplemented("getProperty");
}

jsi::Value QuickJSRuntime::getProperty(
    const jsi::Object &, const jsi::String &name) {
  notImplemented("getProperty");
}

bool QuickJSRuntime::hasProperty(
    const jsi::Object &, const jsi::PropNameID &name) {
  notImplemented("hasProperty");
}

bool QuickJSRuntime::hasProperty(const jsi::Object &, const jsi::String &name) {
  notImplemented("hasProperty");
}

void QuickJSRuntime::setPropertyValue(
    const jsi::Object &, const jsi::PropNameID &name, const jsi::Value &value) {
  notImplemented("setPropertyValue");
}

void QuickJSRuntime::setPropertyValue(
    const jsi::Object &, const jsi::String &name, const jsi::Value &value) {
  notImplemented("setPropertyValue");
}

bool QuickJSRuntime::isArray(const jsi::Object &) const {
  notImplemented("isArray");
}

bool QuickJSRuntime::isArrayBuffer(const jsi::Object &) const {
  notImplemented("isArrayBuffer");
}

bool QuickJSRuntime::isFunction(const jsi::Object &) const {
  notImplemented("isFunction");
}

bool QuickJSRuntime::isHostObject(const jsi::Object &) const {
  notImplemented("isHostObject");
}

bool QuickJSRuntime::isHostFunction(const jsi::Function &) const {
  notImplemented("isHostFunction");
}

jsi::Array QuickJSRuntime::getPropertyNames(const jsi::Object &) {
  notImplemented("getPropertyNames");
}

jsi::WeakObject QuickJSRuntime::createWeakObject(const jsi::Object &) {
  notImplemented("createWeakObject");
}

jsi::Value QuickJSRuntime::lockWeakObject(const jsi::WeakObject &) {
  notImplemented("lockWeakObject");
}

jsi::Array QuickJSRuntime::createArray(size_t length) {
  notImplemented("createArray");
}

jsi::ArrayBuffer QuickJSRuntime::createArrayBuffer(
    std::shared_ptr<jsi::MutableBuffer> buffer) {
  notImplemented("createArrayBuffer");
}

size_t QuickJSRuntime::size(const jsi::Array &) {
  notImplemented("size");
}

size_t QuickJSRuntime::size(const jsi::ArrayBuffer &) {
  notImplemented("size");
}

uint8_t *QuickJSRuntime::data(const jsi::ArrayBuffer &) {
  notImplemented("data");
}

jsi::Value QuickJSRuntime::getValueAtIndex(const jsi::Array &, size_t i) {
  notImplemented("getValueAtIndex");
}

void QuickJSRuntime::setValueAtIndexImpl(
    const jsi::Array &, size_t i, const jsi::Value &value) {
  notImplemented("setValueAtIndexImpl");
}

jsi::Function QuickJSRuntime::createFunctionFromHostFunction(
    const jsi::PropNameID &name, unsigned int paramCount,
    jsi::HostFunctionType func) {
  notImplemented("createFunctionFromHostFunction");
}

jsi::Value QuickJSRuntime::call(
    const jsi::Function &, const jsi::Value &jsThis, const jsi::Value *args,
    size_t count) {
  notImplemented("call");
}

jsi::Value QuickJSRuntime::callAsConstructor(
    const jsi::Function &, const jsi::Value *args, size_t count) {
  notImplemented("callAsConstructor");
}

bool QuickJSRuntime::strictEquals(
    const jsi::Symbol &a, const jsi::Symbol &b) const {
  notImplemented("strictEquals");
}

bool QuickJSRuntime::strictEquals(
    const jsi::BigInt &a, const jsi::BigInt &b) const {
  notImplemented("strictEquals");
}

bool QuickJSRuntime::strictEquals(
    const jsi::String &a, const jsi::String &b) const {
  notImplemented("strictEquals");
}

bool QuickJSRuntime::strictEquals(
    const jsi::Object &a, const jsi::Object &b) const {
  notImplemented("strictEquals");
}

bool QuickJSRuntime::instanceOf(const jsi::Object &o, const jsi::Function &f) {
  notImplemented("instanceOf");
}

void QuickJSRuntime::setExternalMemoryPressure(
    const jsi::Object &obj, size_t amount) {
  notImplemented("setExternalMemoryPressure");
}

}  // namespace qjs
