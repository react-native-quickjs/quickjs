/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "QuickJSRuntime.h"

#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>
#include <vector>

namespace qjs {

static_assert(
    std::atomic<std::thread::id>::is_always_lock_free,
    "std::thread::id must be lock-free to be read on the release path");

static_assert(
    !std::is_abstract<QuickJSRuntime>::value,
    "QuickJSRuntime does not implement every jsi::Runtime method");

namespace {

const char *kEnumeratePropertyNamesSource =
    "(function (o) { const r = []; for (const k in o) r.push(k); return r; })";

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

  enumeratePropertyNames_ = evalInternal(kEnumeratePropertyNamesSource);
}

/// Undefined rather than throwing: a runtime that cannot build a helper is
/// still usable for everything that does not need it.
JSValue QuickJSRuntime::evalInternal(const char *source) noexcept {
  JSValue fn = JS_Eval(
      context_, source, std::strlen(source), "<jsi-internal>",
      JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(fn)) {
    JS_FreeValue(context_, JS_GetException(context_));
    return JS_UNDEFINED;
  }
  return fn;
}

QuickJSRuntime::~QuickJSRuntime() {
  drainPendingReleases();
  JS_FreeValue(context_, enumeratePropertyNames_);

  if (context_ != nullptr) {
    JS_FreeContext(context_);
    context_ = nullptr;
  }

  if (runtime_ != nullptr) {
    // JS_FreeRuntime runs one GC pass then asserts the heap is empty, but a
    // finalizer releasing a jsi value dirties it again, so settle first.
    for (int pass = 0; pass < kMaxTeardownGCPasses; ++pass) {
      JS_RunGC(runtime_);
      drainPendingReleases();
    }
    JS_FreeRuntime(runtime_);
    runtime_ = nullptr;
  }

  // After the engine: teardown still recycles into the free list.
  freePointerValuePool();
}

void QuickJSRuntime::lock() noexcept {
  engineMutex_.lock();
  adoptCurrentThread();
  drainPendingReleases();
}

void QuickJSRuntime::unlock() noexcept {
  engineMutex_.unlock();
}

/**
 * quickjs captures rt->stack_top inside JS_NewRuntime, on whichever thread
 * called it. On React Native's JS thread that mark was 1,115,672 bytes above
 * the real stack pointer, so every call threw "Maximum call stack size
 * exceeded" and the app aborted while loading its bundle.
 *
 * The budget is recomputed rather than only re-based, because a 1 MiB default
 * on a smaller thread puts the limit past the end of the mapping and trades a
 * catchable RangeError for a stack smash. The quarter held back covers the
 * native frames reached after the overflow check fires.
 */
void QuickJSRuntime::adoptCurrentThread() noexcept {
  if (isOnJSThread() || runtime_ == nullptr) {
    return;
  }
  jsThread_.store(std::this_thread::get_id(), std::memory_order_relaxed);
  JS_UpdateStackTop(runtime_);

  if (config_.stackSize > 0) {
    return;
  }
  const size_t remaining = remainingNativeStackBytes();
  if (remaining > 0) {
    JS_SetMaxStackSize(runtime_, remaining / 4 * 3);
  }
}

/// 0 when the platform cannot say, so the caller leaves the engine default
/// alone rather than guessing.
size_t QuickJSRuntime::remainingNativeStackBytes() noexcept {
  char here = 0;
  const auto sp = reinterpret_cast<uintptr_t>(&here);
#if defined(__ANDROID__) || defined(__linux__)
  pthread_attr_t attr;
  if (pthread_getattr_np(pthread_self(), &attr) != 0) {
    return 0;
  }
  void *base = nullptr;
  size_t size = 0;
  const int rc = pthread_attr_getstack(&attr, &base, &size);
  pthread_attr_destroy(&attr);
  if (rc != 0 || base == nullptr || size == 0) {
    return 0;
  }
  // pthread_attr_getstack reports the lowest address; the stack grows down.
  const auto lowest = reinterpret_cast<uintptr_t>(base);
  return sp > lowest ? static_cast<size_t>(sp - lowest) : 0;
#elif defined(__APPLE__)
  void *high = pthread_get_stackaddr_np(pthread_self());
  const size_t size = pthread_get_stacksize_np(pthread_self());
  if (high == nullptr || size == 0) {
    return 0;
  }
  const auto lowest = reinterpret_cast<uintptr_t>(high) - size;
  return sp > lowest ? static_cast<size_t>(sp - lowest) : 0;
#else
  (void)sp;
  return 0;
#endif
}

void QuickJSRuntime::releaseValue(JSValue value) noexcept {
  if (isOnJSThread()) {
    JS_FreeValueRT(runtime_, value);
    return;
  }
  std::lock_guard<std::mutex> lock(pendingMutex_);
  pendingValues_.push_back(value);
  // Published after the push, so a drainer that sees the flag sees the item.
  hasPendingReleases_.store(true, std::memory_order_release);
}

void QuickJSRuntime::releaseAtom(JSAtom atom) noexcept {
  if (isOnJSThread()) {
    JS_FreeAtomRT(runtime_, atom);
    return;
  }
  std::lock_guard<std::mutex> lock(pendingMutex_);
  pendingAtoms_.push_back(atom);
  hasPendingReleases_.store(true, std::memory_order_release);
}

void QuickJSRuntime::drainPendingReleases() noexcept {
  // Racing a producer is harmless: the flag is set after the push, so a release
  // landing just after this load is drained by the next call.
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

/**
 * Runs where describing an exception normally has already failed: JS_ToCString
 * calls toString, and reading .stack is what jsi::JSError does and what was
 * throwing. So own properties only, and anything they raise is swallowed --
 * a pending exception left behind would surface later against unrelated code.
 */
std::string QuickJSRuntime::describeExceptionWithoutRunningJS(
    JSValue exception) {
  if (JS_IsString(exception)) {
    const char *s = JS_ToCString(context_, exception);
    std::string out = s != nullptr
                          ? std::string("<string exception: ") + s + ">"
                          : std::string("<indescribable exception>");
    if (s != nullptr) {
      JS_FreeCString(context_, s);
    }
    return out;
  }
  if (!JS_IsObject(exception)) {
    return "<non-object exception>";
  }

  auto readString = [&](const char *key) -> std::string {
    JSValue v = JS_GetPropertyStr(context_, exception, key);
    if (JS_IsException(v)) {
      JS_FreeValue(context_, JS_GetException(context_));
      return {};
    }
    std::string out;
    if (JS_IsString(v)) {
      const char *s = JS_ToCString(context_, v);
      if (s != nullptr) {
        out = s;
        JS_FreeCString(context_, s);
      }
    }
    JS_FreeValue(context_, v);
    return out;
  };

  std::string name = readString("name");
  std::string message = readString("message");
  if (name.empty() && message.empty()) {
    return "<indescribable exception>";
  }
  if (message.empty()) {
    return name;
  }
  if (name.empty()) {
    return message;
  }
  return name + ": " + message;
}

void QuickJSRuntime::throwPendingError() {
  JSValue exception = JS_GetException(context_);
  if (JS_IsNull(exception) || JS_IsUndefined(exception)) {
    JS_FreeValue(context_, exception);
    throw jsi::JSINativeException(
        "QuickJSRuntime: operation failed with no pending exception");
  }

  if (errorDepth_ >= kMaxErrorDepth) {
    JS_FreeValue(context_, exception);
    std::string what =
        "QuickJSRuntime: exception thrown while handling an exception";
    if (!firstErrorDescription_.empty()) {
      what += "; original exception: " + firstErrorDescription_;
    }
    // A recursive bail-out is usually a stack-limit problem, and the limit is
    // the one thing invisible from JavaScript.
    what += "; jsThreadOwnsEngine=";
    what += isOnJSThread() ? "true" : "false";
    what +=
        ", remainingNativeStack=" + std::to_string(remainingNativeStackBytes());
    what += ", configuredStackSize=" + std::to_string(config_.stackSize);
    throw jsi::JSINativeException(what);
  }

  if (errorDepth_ == 0) {
    firstErrorDescription_ = describeExceptionWithoutRunningJS(exception);
  }

  struct DepthGuard {
    int &depth;
    explicit DepthGuard(int &d) : depth(d) {
      ++depth;
    }
    ~DepthGuard() {
      --depth;
    }
  } guard(errorDepth_);

  throw jsi::JSError(*this, createValue(exception));
}

JSValue QuickJSRuntime::checkException(JSValue value) {
  if (JS_IsException(value)) {
    throwPendingError();
  }
  return value;
}

void QuickJSRuntime::checkException(int result) {
  if (result < 0) {
    throwPendingError();
  }
}

JSValue QuickJSRuntime::throwAsJSException(
    const std::exception *e, const char *origin) noexcept {
  // A JSError already carries a thrown JS value, so rethrow it unchanged.
  if (const auto *jsError = dynamic_cast<const jsi::JSError *>(e)) {
    return JS_Throw(
        context_, JS_DupValue(context_, toJSValue(jsError->value())));
  }
  return JS_ThrowInternalError(
      context_, "Exception in %s: %s", origin,
      e != nullptr ? e->what() : "unknown C++ exception");
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
  return createPropNameIDFrom(JS_NewAtomLen(context_, str, length));
}

jsi::PropNameID QuickJSRuntime::createPropNameIDFromUtf8(
    const uint8_t *utf8, size_t length) {
  return createPropNameIDFrom(
      JS_NewAtomLen(context_, reinterpret_cast<const char *>(utf8), length));
}

jsi::PropNameID QuickJSRuntime::createPropNameIDFromUtf16(
    const char16_t *utf16, size_t length) {
  JSValue string = JS_NewStringUTF16(
      context_, reinterpret_cast<const uint16_t *>(utf16), length);
  checkException(string);
  JSAtom atom = JS_ValueToAtom(context_, string);
  JS_FreeValue(context_, string);
  return createPropNameIDFrom(atom);
}

jsi::PropNameID QuickJSRuntime::createPropNameIDFromString(
    const jsi::String &str) {
  return createPropNameIDFrom(JS_ValueToAtom(context_, toJSValue(str)));
}

jsi::PropNameID QuickJSRuntime::createPropNameIDFromSymbol(
    const jsi::Symbol &sym) {
  return createPropNameIDFrom(JS_ValueToAtom(context_, toJSValue(sym)));
}

std::string QuickJSRuntime::utf8(const jsi::PropNameID &name) {
  size_t length = 0;
  const char *chars = JS_AtomToCStringLen(context_, &length, toJSAtom(name));
  if (chars == nullptr) {
    throwPendingError();
  }
  std::string result(chars, length);
  JS_FreeCString(context_, chars);
  return result;
}

bool QuickJSRuntime::compare(
    const jsi::PropNameID &a, const jsi::PropNameID &b) {
  return toJSAtom(a) == toJSAtom(b);
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
  return createStringFromUtf8(reinterpret_cast<const uint8_t *>(str), length);
}

jsi::String QuickJSRuntime::createStringFromUtf8(
    const uint8_t *utf8, size_t length) {
  JSValue string =
      JS_NewStringLen(context_, reinterpret_cast<const char *>(utf8), length);
  checkException(string);
  return createStringFrom(string);
}

jsi::String QuickJSRuntime::createStringFromUtf16(
    const char16_t *utf16, size_t length) {
  JSValue string = JS_NewStringUTF16(
      context_, reinterpret_cast<const uint16_t *>(utf16), length);
  checkException(string);
  return createStringFrom(string);
}

std::string QuickJSRuntime::utf8(const jsi::String &string) {
  size_t length = 0;
  const char *chars = JS_ToCStringLen(context_, &length, toJSValue(string));
  if (chars == nullptr) {
    throwPendingError();
  }
  std::string result(chars, length);
  JS_FreeCString(context_, chars);
  return result;
}

jsi::Object QuickJSRuntime::createObject() {
  return createObjectFrom(checkException(JS_NewObject(context_)));
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
  if (!prototype.isObject() && !prototype.isNull()) {
    throw jsi::JSINativeException(
        "QuickJSRuntime: prototype must be an object or null");
  }
  return createObjectFrom(
      checkException(JS_NewObjectProto(context_, toJSValue(prototype))));
}

void QuickJSRuntime::setPrototypeOf(
    const jsi::Object &object, const jsi::Value &prototype) {
  if (!prototype.isObject() && !prototype.isNull()) {
    throw jsi::JSINativeException(
        "QuickJSRuntime: prototype must be an object or null");
  }
  checkException(
      JS_SetPrototype(context_, toJSValue(object), toJSValue(prototype)));
}

jsi::Value QuickJSRuntime::getPrototypeOf(const jsi::Object &object) {
  return createValue(
      checkException(JS_GetPrototype(context_, toJSValue(object))));
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
    const jsi::Object &object, const jsi::PropNameID &name) {
  return createValue(
      JS_GetProperty(context_, toJSValue(object), toJSAtom(name)));
}

jsi::Value QuickJSRuntime::getProperty(
    const jsi::Object &object, const jsi::String &name) {
  JSAtom atom = JS_ValueToAtom(context_, toJSValue(name));
  JSValue value = JS_GetProperty(context_, toJSValue(object), atom);
  JS_FreeAtom(context_, atom);
  return createValue(value);
}

bool QuickJSRuntime::hasProperty(
    const jsi::Object &object, const jsi::PropNameID &name) {
  int result = JS_HasProperty(context_, toJSValue(object), toJSAtom(name));
  checkException(result);
  return result != 0;
}

bool QuickJSRuntime::hasProperty(
    const jsi::Object &object, const jsi::String &name) {
  JSAtom atom = JS_ValueToAtom(context_, toJSValue(name));
  int result = JS_HasProperty(context_, toJSValue(object), atom);
  JS_FreeAtom(context_, atom);
  checkException(result);
  return result != 0;
}

void QuickJSRuntime::setPropertyValue(
    const jsi::Object &object, const jsi::PropNameID &name,
    const jsi::Value &value) {
  checkException(JS_SetProperty(
      context_, toJSValue(object), toJSAtom(name),
      JS_DupValue(context_, toJSValue(value))));
}

void QuickJSRuntime::setPropertyValue(
    const jsi::Object &object, const jsi::String &name,
    const jsi::Value &value) {
  JSAtom atom = JS_ValueToAtom(context_, toJSValue(name));
  int result = JS_SetProperty(
      context_, toJSValue(object), atom,
      JS_DupValue(context_, toJSValue(value)));
  JS_FreeAtom(context_, atom);
  checkException(result);
}

bool QuickJSRuntime::isArray(const jsi::Object &object) const {
  return JS_IsArray(toJSValue(object));
}

bool QuickJSRuntime::isArrayBuffer(const jsi::Object &object) const {
  return JS_IsArrayBuffer(toJSValue(object));
}

bool QuickJSRuntime::isFunction(const jsi::Object &object) const {
  return JS_IsFunction(context_, toJSValue(object));
}

bool QuickJSRuntime::isHostObject(const jsi::Object &) const {
  notImplemented("isHostObject");
}

bool QuickJSRuntime::isHostFunction(const jsi::Function &) const {
  notImplemented("isHostFunction");
}

jsi::Array QuickJSRuntime::getPropertyNames(const jsi::Object &object) {
  // Own and inherited enumerable string keys is exactly for-in, which has no
  // C API.
  JSValue argument = toJSValue(object);
  JSValue names =
      JS_Call(context_, enumeratePropertyNames_, JS_UNDEFINED, 1, &argument);
  checkException(names);
  return make<jsi::Object>(allocPointerValue(names)).getArray(*this);
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
  return JS_IsStrictEqual(context_, toJSValue(a), toJSValue(b));
}

bool QuickJSRuntime::strictEquals(
    const jsi::BigInt &a, const jsi::BigInt &b) const {
  return JS_IsStrictEqual(context_, toJSValue(a), toJSValue(b));
}

bool QuickJSRuntime::strictEquals(
    const jsi::String &a, const jsi::String &b) const {
  return JS_IsStrictEqual(context_, toJSValue(a), toJSValue(b));
}

bool QuickJSRuntime::strictEquals(
    const jsi::Object &a, const jsi::Object &b) const {
  return JS_IsStrictEqual(context_, toJSValue(a), toJSValue(b));
}

bool QuickJSRuntime::instanceOf(
    const jsi::Object &object, const jsi::Function &function) {
  int result =
      JS_IsInstanceOf(context_, toJSValue(object), toJSValue(function));
  checkException(result);
  return result != 0;
}

void QuickJSRuntime::setExternalMemoryPressure(
    const jsi::Object & /*object*/, size_t /*amount*/) {
  // quickjs has no external memory pressure hook.
}

}  // namespace qjs
