/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "QuickJSRuntime.h"

#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

namespace qjs {

// The release path reads jsThread_ from threads that do not own the engine, so
// a lock inside the atomic would be a lock on every jsi::Value destructor.
static_assert(
    std::atomic<std::thread::id>::is_always_lock_free,
    "std::thread::id must be lock-free to be read on the release path");

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

void QuickJSRuntime::lock() noexcept {
  engineMutex_.lock();
  if (++lockDepth_ > 1) {
    return;
  }
  // Ownership may have moved since the last acquisition, and a different thread
  // means a different stack and a different set of queued releases.
  if (!isOnJSThread()) {
    rebaseOntoCurrentThread();
  }
  drainPendingReleases();
}

void QuickJSRuntime::unlock() noexcept {
  --lockDepth_;
  engineMutex_.unlock();
}

/**
 * Binds the runtime to the thread that runs JavaScript.
 *
 * quickjs captures rt->stack_top inside JS_NewRuntime, on whichever thread
 * called it, and bounds recursion against it. React Native constructs the
 * runtime on one thread and runs JavaScript on another, where the stack pointer
 * measured 1,115,672 bytes below that mark -- already past the 1 MiB default --
 * so every call threw "Maximum call stack size exceeded" and the app aborted
 * while loading its bundle.
 *
 * The budget is recomputed, not just re-based: a 1 MiB budget on a thread with
 * less than 1 MiB below the new mark puts the limit past the end of the real
 * mapping, trading a spurious RangeError for a stack smash. Three quarters of
 * the measured remainder is assumed rather than tuned -- the quarter held back
 * covers the native frames between the overflow check and the deepest point
 * reached after it.
 */
void QuickJSRuntime::rebaseOntoCurrentThread() noexcept {
  jsThread_.store(std::this_thread::get_id(), std::memory_order_relaxed);
  if (runtime_ == nullptr) {
    return;
  }
  JS_UpdateStackTop(runtime_);

  if (config_.stackSize > 0) {
    return;  // the embedder chose; JS_UpdateStackTop already re-based it.
  }
  const size_t remaining = remainingNativeStackBytes();
  if (remaining == 0) {
    return;  // the platform could not say, so leave the engine default alone.
  }
  JS_SetMaxStackSize(runtime_, remaining / 4 * 3);
}

void QuickJSRuntime::adoptCurrentThreadAsJSThread() noexcept {
  if (jsThreadAdopted_) {
    return;
  }
  jsThreadAdopted_ = true;
  rebaseOntoCurrentThread();
}

/**
 * quickjs's bound is only meaningful if stack_size is smaller than the stack
 * that actually exists below stack_top. Its default is 1 MiB; React Native's
 * Android JS thread is created with the platform default, which is not 1 MiB
 * and differs between ART versions and ABIs. So the budget is derived from the
 * mapping rather than chosen, and 0 means the caller must leave the engine
 * default alone rather than guess.
 */
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

/**
 * Runs where describing an exception the normal way has already failed, or is
 * about to: JS_ToCString on an Error calls its toString, reading .message can
 * hit an accessor, and reading .stack is exactly what jsi::JSError does and
 * exactly what was throwing. So this reads own properties only, and swallows
 * anything they raise, because the caller is about to throw a C++ exception and
 * a stray pending JS exception would surface later against unrelated code.
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
    // Never empty: at the call site that is indistinguishable from "no original
    // exception was recorded".
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
    // Report the outermost exception: by the time the bound is reached this one
    // is a failure that happened while describing the original, and naming it
    // explains nothing.
    std::string what =
        "QuickJSRuntime: exception thrown while handling an exception";
    if (!firstErrorDescription_.empty()) {
      what += "; original exception: " + firstErrorDescription_;
    }
    // A recursive bail-out is usually a stack-limit problem, and the limit is
    // the one thing about it invisible from JavaScript.
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

  // JS_GetException already cleared the pending exception, so the reads inside
  // JSError start from a clean slate.
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
  // A JSError already carries a thrown JS value, so rethrow it unchanged rather
  // than reshaping it.
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
