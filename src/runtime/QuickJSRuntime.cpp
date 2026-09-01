/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "QuickJSRuntime.h"

#include <type_traits>
#include <utility>
#include <vector>

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
  drainPendingReleases();

  if (context_ != nullptr) {
    JS_FreeContext(context_);
    context_ = nullptr;
  }

  if (runtime_ != nullptr) {
    // The global object sits in reference cycles, so dropping the context only
    // makes it collectable. Objects hanging off it can own jsi values, and
    // releasing those from a finalizer dirties the heap again. JS_FreeRuntime
    // runs one GC pass and then asserts the heap is empty, so collect until it
    // settles first.
    for (int pass = 0; pass < kMaxTeardownGCPasses; ++pass) {
      JS_RunGC(runtime_);
      drainPendingReleases();
    }
    JS_FreeRuntime(runtime_);
    runtime_ = nullptr;
  }

  // After the engine, because recycling during teardown still touches the free
  // list.
  freePointerValuePool();
}

void QuickJSRuntime::releaseValue(JSValue value) noexcept {
  if (std::this_thread::get_id() == jsThread_) {
    JS_FreeValueRT(runtime_, value);
    return;
  }
  std::lock_guard<std::mutex> lock(pendingMutex_);
  pendingValues_.push_back(value);
  // Published after the push, so a drainer that sees the flag sees the item.
  hasPendingReleases_.store(true, std::memory_order_release);
}

void QuickJSRuntime::releaseAtom(JSAtom atom) noexcept {
  if (std::this_thread::get_id() == jsThread_) {
    JS_FreeAtomRT(runtime_, atom);
    return;
  }
  std::lock_guard<std::mutex> lock(pendingMutex_);
  pendingAtoms_.push_back(atom);
  hasPendingReleases_.store(true, std::memory_order_release);
}

void QuickJSRuntime::drainPendingReleases() noexcept {
  // Runs on every call into JS, where the queue is almost always empty, so the
  // common case is one relaxed load rather than a mutex. Racing a producer is
  // harmless: the flag is set after the push, so a release landing just after
  // this load is drained by the next call. These frees have no deadline.
  if (!hasPendingReleases_.load(std::memory_order_acquire)) {
    return;
  }

  std::vector<JSValue> values;
  std::vector<JSAtom> atoms;
  std::vector<QuickJSPointerValue *> pointerValues;
  std::vector<QuickJSAtomPointerValue *> atomPointerValues;
  {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    hasPendingReleases_.store(false, std::memory_order_relaxed);
    values.swap(pendingValues_);
    atoms.swap(pendingAtoms_);
    pointerValues.swap(pendingPointerValues_);
    atomPointerValues.swap(pendingAtomPointerValues_);
  }

  for (JSValue value : values) {
    JS_FreeValueRT(runtime_, value);
  }
  for (JSAtom atom : atoms) {
    JS_FreeAtomRT(runtime_, atom);
  }
  for (QuickJSPointerValue *pv : pointerValues) {
    pv->nextFree_ = freeValues_;
    freeValues_ = pv;
  }
  for (QuickJSAtomPointerValue *pv : atomPointerValues) {
    pv->nextFree_ = freeAtoms_;
    freeAtoms_ = pv;
  }
}

void QuickJSRuntime::refillValueSlab() {
  auto *slab = static_cast<QuickJSPointerValue *>(
      ::operator new(sizeof(QuickJSPointerValue) * kPointerValueSlabSize));
  valueSlabs_.push_back(slab);
  // Raw storage: slots are constructed in place on allocation, not here.
  for (size_t i = 0; i < kPointerValueSlabSize; ++i) {
    auto *slot = slab + i;
    slot->nextFree_ = freeValues_;
    freeValues_ = slot;
  }
}

void QuickJSRuntime::refillAtomSlab() {
  auto *slab = static_cast<QuickJSAtomPointerValue *>(
      ::operator new(sizeof(QuickJSAtomPointerValue) * kPointerValueSlabSize));
  atomSlabs_.push_back(slab);
  for (size_t i = 0; i < kPointerValueSlabSize; ++i) {
    auto *slot = slab + i;
    slot->nextFree_ = freeAtoms_;
    freeAtoms_ = slot;
  }
}

void QuickJSRuntime::queuePointerValue(QuickJSPointerValue *pv) noexcept {
  std::lock_guard<std::mutex> lock(pendingMutex_);
  pendingPointerValues_.push_back(pv);
  hasPendingReleases_.store(true, std::memory_order_release);
}

void QuickJSRuntime::queueAtomPointerValue(
    QuickJSAtomPointerValue *pv) noexcept {
  std::lock_guard<std::mutex> lock(pendingMutex_);
  pendingAtomPointerValues_.push_back(pv);
  hasPendingReleases_.store(true, std::memory_order_release);
}

void QuickJSRuntime::freePointerValuePool() noexcept {
  // Anything still live here is a JSI pointer the embedder outlived the runtime
  // with, which is already undefined behaviour and not something the pool can
  // repair.
  for (void *slab : valueSlabs_) {
    ::operator delete(slab);
  }
  for (void *slab : atomSlabs_) {
    ::operator delete(slab);
  }
  valueSlabs_.clear();
  atomSlabs_.clear();
  freeValues_ = nullptr;
  freeAtoms_ = nullptr;
}

JSValue QuickJSRuntime::toJSValue(const jsi::Pointer &pointer) {
  return static_cast<const QuickJSPointerValue *>(getPointerValue(pointer))
      ->value();
}

JSAtom QuickJSRuntime::toJSAtom(const jsi::PropNameID &name) {
  return static_cast<const QuickJSAtomPointerValue *>(getPointerValue(name))
      ->atom();
}

JSValue QuickJSRuntime::toJSValue(const jsi::Value &value) const {
  if (value.isUndefined()) {
    return JS_UNDEFINED;
  }
  if (value.isNull()) {
    return JS_NULL;
  }
  if (value.isBool()) {
    return JS_NewBool(context_, value.getBool());
  }
  if (value.isNumber()) {
    return JS_NewFloat64(context_, value.getNumber());
  }
  return static_cast<const QuickJSPointerValue *>(getPointerValue(value))
      ->value();
}

jsi::Value QuickJSRuntime::createValue(JSValue value) {
  if (JS_IsException(value)) {
    throwPendingError();
  }
  if (JS_IsUndefined(value)) {
    return jsi::Value::undefined();
  }
  if (JS_IsNull(value)) {
    return jsi::Value::null();
  }
  if (JS_IsBool(value)) {
    return jsi::Value(static_cast<bool>(JS_ToBool(context_, value)));
  }
  if (JS_IsNumber(value)) {
    double number = 0;
    JS_ToFloat64(context_, &number, value);
    return jsi::Value(number);
  }
  if (JS_IsString(value)) {
    return jsi::Value(createStringFrom(value));
  }
  if (JS_IsSymbol(value)) {
    return jsi::Value(createSymbolFrom(value));
  }
  if (JS_IsBigInt(value)) {
    return jsi::Value(make<jsi::BigInt>(allocPointerValue(value)));
  }
  return jsi::Value(createObjectFrom(value));
}

jsi::Value QuickJSRuntime::borrowValue(JSValue value) {
  // Primitives carry no reference, so they are already borrow-shaped.
  if (JS_IsUndefined(value)) {
    return jsi::Value::undefined();
  }
  if (JS_IsNull(value)) {
    return jsi::Value::null();
  }
  if (JS_IsBool(value)) {
    return jsi::Value(static_cast<bool>(JS_ToBool(context_, value)));
  }
  if (JS_IsNumber(value)) {
    double number = 0;
    JS_ToFloat64(context_, &number, value);
    return jsi::Value(number);
  }
  if (JS_IsString(value)) {
    return jsi::Value(make<jsi::String>(allocPointerValue(value, false)));
  }
  if (JS_IsSymbol(value)) {
    return jsi::Value(make<jsi::Symbol>(allocPointerValue(value, false)));
  }
  if (JS_IsBigInt(value)) {
    return jsi::Value(make<jsi::BigInt>(allocPointerValue(value, false)));
  }
  return jsi::Value(make<jsi::Object>(allocPointerValue(value, false)));
}

JSValue QuickJSRuntime::takeJSValue(jsi::Value &&value) {
  if (value.isUndefined()) {
    return JS_UNDEFINED;
  }
  if (value.isNull()) {
    return JS_NULL;
  }
  if (value.isBool()) {
    return JS_NewBool(context_, value.getBool());
  }
  if (value.isNumber()) {
    return JS_NewFloat64(context_, value.getNumber());
  }
  auto *pv = const_cast<QuickJSPointerValue *>(
      static_cast<const QuickJSPointerValue *>(getPointerValue(value)));
  if (!pv->owned_) {
    return JS_DupValue(context_, pv->value());
  }
  // Hand the reference over and disarm the destructor, which is about to run
  // as `value` goes out of scope.
  pv->owned_ = false;
  return pv->value();
}

jsi::Object QuickJSRuntime::createObjectFrom(JSValue value) {
  return make<jsi::Object>(allocPointerValue(value));
}

jsi::Symbol QuickJSRuntime::createSymbolFrom(JSValue value) {
  return make<jsi::Symbol>(allocPointerValue(value));
}

jsi::String QuickJSRuntime::createStringFrom(JSValue value) {
  return make<jsi::String>(allocPointerValue(value));
}

jsi::PropNameID QuickJSRuntime::createPropNameIDFrom(JSAtom atom) {
  return make<jsi::PropNameID>(allocAtomPointerValue(atom));
}

void QuickJSRuntime::throwPendingError() {
  notImplemented("throwPendingError");
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
  return createObjectFrom(JS_GetGlobalObject(context_));
}

jsi::Runtime::PointerValue *QuickJSRuntime::cloneSymbol(
    const Runtime::PointerValue *pv) {
  const auto *value = static_cast<const QuickJSPointerValue *>(pv);
  return allocPointerValue(JS_DupValue(context_, value->value()));
}

jsi::Runtime::PointerValue *QuickJSRuntime::cloneBigInt(
    const Runtime::PointerValue *pv) {
  const auto *value = static_cast<const QuickJSPointerValue *>(pv);
  return allocPointerValue(JS_DupValue(context_, value->value()));
}

jsi::Runtime::PointerValue *QuickJSRuntime::cloneString(
    const Runtime::PointerValue *pv) {
  const auto *value = static_cast<const QuickJSPointerValue *>(pv);
  return allocPointerValue(JS_DupValue(context_, value->value()));
}

jsi::Runtime::PointerValue *QuickJSRuntime::cloneObject(
    const Runtime::PointerValue *pv) {
  const auto *value = static_cast<const QuickJSPointerValue *>(pv);
  return allocPointerValue(JS_DupValue(context_, value->value()));
}

jsi::Runtime::PointerValue *QuickJSRuntime::clonePropNameID(
    const Runtime::PointerValue *pv) {
  const auto *value = static_cast<const QuickJSAtomPointerValue *>(pv);
  return allocAtomPointerValue(JS_DupAtom(context_, value->atom()));
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
