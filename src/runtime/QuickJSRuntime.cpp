/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "QuickJSRuntime.h"

#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "QuickJSBytecode.h"

namespace qjs {

static_assert(
    std::atomic<std::thread::id>::is_always_lock_free,
    "std::thread::id must be lock-free to be read on the release path");

static_assert(
    !std::is_abstract<QuickJSRuntime>::value,
    "QuickJSRuntime does not implement every jsi::Runtime method");

namespace {

struct NativePayloadLink {
  NativePayloadLink *prev{nullptr};
  NativePayloadLink *next{nullptr};
};

/// Opaque payload of a host object. Owned by the JS object and released by the
/// class finalizer.
struct HostObjectProxy {
  NativePayloadLink link;  // must stay first
  QuickJSRuntime *runtime;
  std::shared_ptr<jsi::HostObject> hostObject;
};

/// Opaque payload of a host function. `function` is returned by reference from
/// getHostFunction, so it must keep a stable address for the lifetime of the JS
/// function.
struct HostFunctionProxy {
  NativePayloadLink link;  // must stay first: the registry casts through it
  QuickJSRuntime *runtime;
  jsi::HostFunctionType function;
};

QuickJSRuntime *runtimeOf(JSContext *ctx) {
  return static_cast<QuickJSRuntime *>(JS_GetContextOpaque(ctx));
}

void payloadLink(void *&head, void *proxy) {
  auto *link = static_cast<NativePayloadLink *>(proxy);
  link->prev = nullptr;
  link->next = static_cast<NativePayloadLink *>(head);
  if (link->next != nullptr) {
    link->next->prev = link;
  }
  head = link;
}

void payloadUnlink(void *&head, void *proxy) {
  auto *link = static_cast<NativePayloadLink *>(proxy);
  if (link->prev != nullptr) {
    link->prev->next = link->next;
  } else if (head == link) {
    head = link->next;
  } else {
    return;
  }
  if (link->next != nullptr) {
    link->next->prev = link->prev;
  }
  link->prev = link->next = nullptr;
}

struct HostObjectHandlers {
  static HostObjectProxy *proxyOf(JSContext *ctx, JSValueConst obj) {
    return static_cast<HostObjectProxy *>(
        JS_GetOpaque(obj, runtimeOf(ctx)->hostObjectClassID()));
  }

  static int getOwnProperty(
      JSContext *ctx, JSPropertyDescriptor *desc, JSValueConst obj,
      JSAtom prop) {
    auto *proxy = proxyOf(ctx, obj);
    if (proxy == nullptr) {
      return false;
    }
    // A host object answers for every key, so an existence check needs no call
    // into C++.
    if (desc == nullptr) {
      return true;
    }

    auto *runtime = proxy->runtime;
    try {
      jsi::PropNameID name =
          runtime->createPropNameIDFrom(JS_DupAtom(ctx, prop));
      jsi::Value value = proxy->hostObject->get(*runtime, name);
      desc->flags = JS_PROP_C_W_E;
      desc->value = JS_DupValue(ctx, runtime->toJSValue(value));
      desc->getter = JS_UNDEFINED;
      desc->setter = JS_UNDEFINED;
      return true;
    } catch (const std::exception &e) {
      runtime->throwAsJSException(&e, "HostObject");
      return -1;
    } catch (...) {
      runtime->throwAsJSException(nullptr, "HostObject");
      return -1;
    }
  }

  static int getOwnPropertyNames(
      JSContext *ctx, JSPropertyEnum **ptab, uint32_t *plen, JSValueConst obj) {
    auto *proxy = proxyOf(ctx, obj);
    if (proxy == nullptr) {
      *ptab = nullptr;
      *plen = 0;
      return 0;
    }

    auto *runtime = proxy->runtime;
    std::vector<jsi::PropNameID> names;
    try {
      names = proxy->hostObject->getPropertyNames(*runtime);
    } catch (const std::exception &e) {
      runtime->throwAsJSException(&e, "HostObject");
      return -1;
    } catch (...) {
      runtime->throwAsJSException(nullptr, "HostObject");
      return -1;
    }

    // JSI allows duplicates here; the property enumeration protocol does not.
    std::vector<JSAtom> unique;
    std::unordered_set<JSAtom> seen;
    unique.reserve(names.size());
    for (const jsi::PropNameID &name : names) {
      JSAtom atom = QuickJSRuntime::toJSAtom(name);
      if (seen.insert(atom).second) {
        unique.push_back(atom);
      }
    }

    auto *table = static_cast<JSPropertyEnum *>(js_malloc(
        ctx, sizeof(JSPropertyEnum) * std::max<size_t>(unique.size(), 1)));
    if (table == nullptr) {
      return -1;
    }
    for (size_t i = 0; i < unique.size(); ++i) {
      table[i].is_enumerable = true;
      table[i].atom = JS_DupAtom(ctx, unique[i]);
    }

    *ptab = table;
    *plen = static_cast<uint32_t>(unique.size());
    return 0;
  }

  static int setProperty(
      JSContext *ctx, JSValueConst obj, JSAtom atom, JSValueConst value,
      JSValueConst receiver, int flags) {
    auto *proxy = proxyOf(ctx, obj);
    if (proxy == nullptr) {
      return false;
    }

    // OrdinarySetWithOwnDescriptor: an assignment that reached this host object
    // by walking a receiver's prototype chain creates an own property on the
    // receiver instead of calling HostObject::set. React Native's TurboModules
    // depend on it -- TurboModule::get memoises each method by writing it back
    // onto a plain object whose prototype is the module's HostObject.
    const bool receiverIsThisObject =
        JS_VALUE_GET_TAG(receiver) == JS_VALUE_GET_TAG(obj) &&
        JS_VALUE_GET_PTR(receiver) == JS_VALUE_GET_PTR(obj);

    if (!JS_IsUndefined(receiver) && !receiverIsThisObject) {
      // Step 3.a returns false for a non-object receiver, reachable through
      // Reflect.set(hostObj, k, v, 5). Defining would throw instead.
      if (JS_VALUE_GET_TAG(receiver) != JS_TAG_OBJECT) {
        return false;
      }
      // Defining is safe: quickjs only consults a prototype's set_property
      // after failing to find an own property. Both throw flags are forwarded
      // so the caller's own strictness policy is reproduced -- without them a
      // strict assignment onto a frozen receiver returns false where the spec
      // throws.
      int defined = JS_DefinePropertyValue(
          ctx, receiver, JS_DupAtom(ctx, atom), JS_DupValue(ctx, value),
          JS_PROP_C_W_E | (flags & (JS_PROP_THROW | JS_PROP_THROW_STRICT)));
      return defined < 0 ? -1 : defined;
    }

    auto *runtime = proxy->runtime;
    try {
      jsi::PropNameID name =
          runtime->createPropNameIDFrom(JS_DupAtom(ctx, atom));
      proxy->hostObject->set(
          *runtime, name, runtime->createValue(JS_DupValue(ctx, value)));
      return true;
    } catch (const std::exception &e) {
      runtime->throwAsJSException(&e, "HostObject");
      return -1;
    } catch (...) {
      runtime->throwAsJSException(nullptr, "HostObject");
      return -1;
    }
  }

  static void finalizer(JSRuntime *rt, JSValueConst value) {
    auto *runtime = static_cast<QuickJSRuntime *>(JS_GetRuntimeOpaque(rt));
    auto *proxy = static_cast<HostObjectProxy *>(
        JS_GetOpaque(value, runtime->hostObjectClassID()));
    runtime->unregisterHostObject(proxy);
    delete proxy;
  }
};

JSClassExoticMethods gHostObjectExotic = {
    /* get_own_property */ HostObjectHandlers::getOwnProperty,
    /* get_own_property_names */ HostObjectHandlers::getOwnPropertyNames,
    /* delete_property */ nullptr,
    /* define_own_property */ nullptr,
    /* has_property */ nullptr,
    /* get_property */ nullptr,
    /* set_property */ HostObjectHandlers::setProperty,
};

struct HostFunctionHandlers {
  static JSValue call(
      JSContext *ctx, JSValueConst funcObj, JSValueConst thisVal, int argc,
      JSValueConst *argv, int /*flags*/) {
    auto *runtime = runtimeOf(ctx);
    auto *proxy = static_cast<HostFunctionProxy *>(
        JS_GetOpaque(funcObj, runtime->hostFunctionClassID()));
    if (proxy == nullptr) {
      return JS_ThrowTypeError(ctx, "not a host function");
    }

    try {
      // Borrowed, not dup'd: quickjs keeps argv and thisVal alive for the whole
      // call, so a reference taken here and dropped on return would cancel out.
      constexpr int kInlineArgs = 8;
      jsi::Value inlineArgs[kInlineArgs];
      std::vector<jsi::Value> heapArgs;
      jsi::Value *args = inlineArgs;
      if (argc > kInlineArgs) {
        heapArgs.resize(static_cast<size_t>(argc));
        args = heapArgs.data();
      }
      for (int i = 0; i < argc; ++i) {
        args[i] = runtime->borrowValue(argv[i]);
      }
      jsi::Value thisValue = runtime->borrowValue(thisVal);
      jsi::Value result =
          proxy->function(*runtime, thisValue, args, static_cast<size_t>(argc));
      return runtime->takeJSValue(std::move(result));
    } catch (const std::exception &e) {
      return runtime->throwAsJSException(&e);
    } catch (...) {
      return runtime->throwAsJSException(nullptr);
    }
  }

  static void finalizer(JSRuntime *rt, JSValueConst value) {
    auto *runtime = static_cast<QuickJSRuntime *>(JS_GetRuntimeOpaque(rt));
    auto *proxy = static_cast<HostFunctionProxy *>(
        JS_GetOpaque(value, runtime->hostFunctionClassID()));
    runtime->unregisterHostFunction(proxy);
    delete proxy;
  }
};

struct NativeStateProxy {
  NativePayloadLink link;  // must stay first
  std::shared_ptr<jsi::NativeState> state;
};

void nativeStateFinalizer(JSRuntime *rt, JSValueConst value) {
  auto *runtime = static_cast<QuickJSRuntime *>(JS_GetRuntimeOpaque(rt));
  auto *proxy = static_cast<NativeStateProxy *>(
      JS_GetOpaque(value, runtime->nativeStateClassID()));
  runtime->unregisterNativeState(proxy);
  delete proxy;
}

/// Keeps a jsi::MutableBuffer alive for as long as the ArrayBuffer pointing
/// into it.
struct ArrayBufferProxy {
  std::shared_ptr<jsi::MutableBuffer> buffer;
};

void freeArrayBufferProxy(JSRuntime * /*rt*/, void *opaque, void * /*ptr*/) {
  delete static_cast<ArrayBufferProxy *>(opaque);
}

const char *kEnumeratePropertyNamesSource =
    "(function (o) { const r = []; for (const k in o) r.push(k); return r; })";
JSValue microtaskJob(JSContext *ctx, int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_UNDEFINED;
  }
  return JS_Call(ctx, argv[0], JS_UNDEFINED, 0, nullptr);
}

const char *kSymbolToStringSource = "(function (s) { return s.toString(); })";
const char *kBigIntToStringSource =
    "(function (b, radix) { return b.toString(radix); })";

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

  if (config_.deferGC) {
    JS_SetGCDeferred(runtime_, true, config_.deferredGCLimit);
  }

  context_ = JS_NewContext(runtime_);
  if (context_ == nullptr) {
    JS_FreeRuntime(runtime_);
    runtime_ = nullptr;
    throw jsi::JSINativeException("QuickJSRuntime: JS_NewContext failed");
  }

  JS_SetContextOpaque(context_, this);
  JS_SetRuntimeOpaque(runtime_, this);

  // Baseline for the pressure valve. Left at zero, the first `grown` reading is
  // the whole live heap and the valve fires whatever the app is doing.
  heapBaseline_ = JS_GetMallocSize(runtime_);

  registerClasses();
  enumeratePropertyNames_ = evalInternal(kEnumeratePropertyNamesSource);
  symbolToString_ = evalInternal(kSymbolToStringSource);
  bigIntToString_ = evalInternal(kBigIntToStringSource);

  JSValue nativeStateKey = JS_NewPrivateSymbol(context_, "nativeState");
  nativeStateKey_ = JS_ValueToAtom(context_, nativeStateKey);
  JS_FreeValue(context_, nativeStateKey);

  // WeakRef has no C API, so reach for the JS-visible constructor once.
  JSValue global = JS_GetGlobalObject(context_);
  weakRefConstructor_ = JS_GetPropertyStr(context_, global, "WeakRef");
  if (JS_IsObject(weakRefConstructor_)) {
    JSValue proto =
        JS_GetPropertyStr(context_, weakRefConstructor_, "prototype");
    weakRefDeref_ = JS_GetPropertyStr(context_, proto, "deref");
    JS_FreeValue(context_, proto);
  }
  JS_FreeValue(context_, global);
}

void QuickJSRuntime::registerClasses() {
  JS_NewClassID(runtime_, &hostObjectClassID_);
  JSClassDef hostObjectDef = {};
  hostObjectDef.class_name = "HostObject";
  hostObjectDef.finalizer = HostObjectHandlers::finalizer;
  hostObjectDef.exotic = &gHostObjectExotic;
  JS_NewClass(runtime_, hostObjectClassID_, &hostObjectDef);

  JS_NewClassID(runtime_, &hostFunctionClassID_);
  JSClassDef hostFunctionDef = {};
  hostFunctionDef.class_name = "HostFunction";
  hostFunctionDef.finalizer = HostFunctionHandlers::finalizer;
  hostFunctionDef.call = HostFunctionHandlers::call;
  JS_NewClass(runtime_, hostFunctionClassID_, &hostFunctionDef);

  JS_NewClassID(runtime_, &nativeStateClassID_);
  JSClassDef nativeStateDef = {};
  nativeStateDef.class_name = "NativeState";
  nativeStateDef.finalizer = nativeStateFinalizer;
  JS_NewClass(runtime_, nativeStateClassID_, &nativeStateDef);

  // Host functions must be indistinguishable from JS functions: without
  // Function.prototype, `instanceof Function` is false and bind/call/apply are
  // missing.
  JSValue global = JS_GetGlobalObject(context_);
  JSValue functionConstructor = JS_GetPropertyStr(context_, global, "Function");
  JS_SetClassProto(
      context_, hostFunctionClassID_,
      JS_GetPropertyStr(context_, functionConstructor, "prototype"));
  JS_FreeValue(context_, functionConstructor);
  JS_FreeValue(context_, global);
}

void QuickJSRuntime::registerHostObject(void *proxy) {
  payloadLink(hostObjectProxies_, proxy);
}

void QuickJSRuntime::unregisterHostObject(void *proxy) {
  payloadUnlink(hostObjectProxies_, proxy);
}

void QuickJSRuntime::registerNativeState(void *proxy) {
  payloadLink(nativeStateProxies_, proxy);
}

void QuickJSRuntime::unregisterNativeState(void *proxy) {
  payloadUnlink(nativeStateProxies_, proxy);
}

void QuickJSRuntime::registerHostFunction(void *proxy) {
  payloadLink(hostFunctionProxies_, proxy);
}

void QuickJSRuntime::unregisterHostFunction(void *proxy) {
  payloadUnlink(hostFunctionProxies_, proxy);
}

/// Leaves the lists intact: the proxies stay allocated and their finalizers
/// still run during JS_FreeRuntime, which is what unlinks them. Releasing a
/// payload can free jsi values and so re-enter, hence remembering `next` first.
void QuickJSRuntime::releaseNativePayloads() noexcept {
  for (auto *l = static_cast<NativePayloadLink *>(hostObjectProxies_);
       l != nullptr;) {
    auto *next = l->next;
    reinterpret_cast<HostObjectProxy *>(l)->hostObject.reset();
    l = next;
  }
  for (auto *l = static_cast<NativePayloadLink *>(hostFunctionProxies_);
       l != nullptr;) {
    auto *next = l->next;
    reinterpret_cast<HostFunctionProxy *>(l)->function = nullptr;
    l = next;
  }
  for (auto *l = static_cast<NativePayloadLink *>(nativeStateProxies_);
       l != nullptr;) {
    auto *next = l->next;
    reinterpret_cast<NativeStateProxy *>(l)->state.reset();
    l = next;
  }
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
  releaseNativePayloads();
  JS_FreeValue(context_, enumeratePropertyNames_);
  JS_FreeValue(context_, symbolToString_);
  JS_FreeValue(context_, weakRefConstructor_);
  JS_FreeValue(context_, weakRefDeref_);
  JS_FreeValue(context_, bigIntToString_);
  JS_FreeAtom(context_, nativeStateKey_);

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

namespace {

void throwIfHermesBytecode(
    const uint8_t *data, size_t size, const std::string &sourceURL) {
  if (!isHermesBytecode(data, size)) {
    return;
  }
  throw jsi::JSINativeException(
      "QuickJSRuntime: " +
      (sourceURL.empty() ? std::string("this bundle") : sourceURL) +
      " is Hermes bytecode, which this engine cannot execute. In a React "
      "Native app that usually means hermesEnabled is still true in "
      "android/gradle.properties, or :hermes_enabled in the Podfile. Turn it "
      "off so Metro ships plain JavaScript, or precompile with qjsc-ng.");
}

/**
 * The name a script should be known by: its `//# sourceURL=` comment if it has
 * one, otherwise the URL the embedder passed.
 *
 * Every other engine renames a script by that comment -- it is what
 * `error.stack` shows and what `Debugger.setBreakpointByUrl` matches against.
 * quickjs ignores it and records whatever filename JS_Eval was handed, so a
 * script carrying the comment would be known by two names and a breakpoint set
 * on it would never fire.
 *
 * Only the last 8 KiB are scanned, which makes this O(1) in bundle size rather
 * than a full pass over a multi-megabyte bundle on the startup path. Tools
 * append the comment at the end; one placed earlier is not honoured.
 */
constexpr size_t kSourceURLScanBytes = 8192;

std::string effectiveSourceURL(
    const uint8_t *data, size_t size, const std::string &sourceURL) {
  const size_t start =
      size > kSourceURLScanBytes ? size - kSourceURLScanBytes : 0;
  const std::string_view tail(
      reinterpret_cast<const char *>(data) + start, size - start);

  size_t best = std::string_view::npos;
  for (std::string_view form : {"//# sourceURL=", "//@ sourceURL="}) {
    const size_t pos = tail.rfind(form);
    if (pos != std::string_view::npos &&
        (best == std::string_view::npos || pos > best)) {
      best = pos + form.size();
    }
  }
  if (best == std::string_view::npos) {
    return sourceURL;
  }

  const size_t end = tail.find_first_of("\r\n", best);
  const std::string_view value = tail.substr(
      best,
      end == std::string_view::npos ? std::string_view::npos : end - best);
  const size_t first = value.find_first_not_of(" \t");
  if (first == std::string_view::npos) {
    return sourceURL;
  }
  const size_t last = value.find_last_not_of(" \t");
  return std::string(value.substr(first, last - first + 1));
}

/// Raises the collection threshold for the first top-level script and hands
/// pacing back at the rung the engine would otherwise be on. Bounded rather
/// than a disable, so a larger bundle degrades to normal pacing by itself. See
/// QuickJSRuntimeConfig::startupGCBracketBytes.
class StartupGCBracket final {
 public:
  StartupGCBracket(JSRuntime *rt, size_t bytes, bool &used) {
    if (rt == nullptr || bytes == 0 || used) {
      return;
    }
    used = true;
    rt_ = rt;
    JS_SetGCThreshold(rt_, JS_GetMallocSize(rt_) + bytes);
  }

  ~StartupGCBracket() {
    if (rt_ == nullptr) {
      return;
    }
    const size_t live = JS_GetMallocSize(rt_);
    JS_SetGCThreshold(rt_, live + (live >> 1));
  }

  StartupGCBracket(const StartupGCBracket &) = delete;
  StartupGCBracket &operator=(const StartupGCBracket &) = delete;

 private:
  JSRuntime *rt_{nullptr};
};

/// Compiled form of a script: a JS_WriteObject payload, whether it arrived as
/// source or as a container.
class QuickJSPreparedJavaScript final : public jsi::PreparedJavaScript {
 public:
  explicit QuickJSPreparedJavaScript(std::vector<uint8_t> bytecode)
      : bytecode_(std::move(bytecode)) {}

  const std::vector<uint8_t> &bytecode() const noexcept {
    return bytecode_;
  }

 private:
  std::vector<uint8_t> bytecode_;
};

}  // namespace

jsi::Value QuickJSRuntime::evaluateSource(
    const uint8_t *source, size_t size, const std::string &sourceURL) {
  // JS_Eval wants a NUL-terminated buffer, which jsi::Buffer does not promise.
  const std::string owned(reinterpret_cast<const char *>(source), size);
  return createValue(JS_Eval(
      context_, owned.c_str(), owned.size(), sourceURL.c_str(),
      JS_EVAL_TYPE_GLOBAL));
}

jsi::Value QuickJSRuntime::evaluateBytecode(
    const uint8_t *payload, size_t size) {
  JSValue function =
      JS_ReadObject(context_, payload, size, JS_READ_OBJ_BYTECODE);
  if (JS_IsException(function)) {
    // Almost always an engine mismatch. quickjs bytecode loads only in the
    // build that wrote it, and "bytecode function expected" does not say so.
    JS_FreeValue(context_, JS_GetException(context_));
    throw jsi::JSINativeException(
        "QuickJSRuntime: could not load bytecode. It was most likely compiled "
        "by a different build of the engine -- recompile it with the qjsc-ng "
        "from this checkout.");
  }
  return createValue(JS_EvalFunction(context_, function));
}

jsi::Value QuickJSRuntime::evaluateJavaScript(
    const std::shared_ptr<const jsi::Buffer> &buffer,
    const std::string &sourceURL) {
  adoptCurrentThread();
  drainPendingReleases();

  const uint8_t *data = buffer->data();
  const size_t size = buffer->size();

  StartupGCBracket bracket(
      runtime_, config_.startupGCBracketBytes, startupGCBracketUsed_);

  jsi::Value result;
  if (isBytecodeContainer(data, size)) {
    result = evaluateBytecode(
        data + kBytecodeHeaderSize, size - kBytecodeHeaderSize);
  } else {
    throwIfHermesBytecode(data, size, sourceURL);
    const std::string url = effectiveSourceURL(data, size, sourceURL);
    result = evaluateSource(data, size, url);
    if (onScriptEvaluated_) {
      onScriptEvaluated_(
          url, std::string(reinterpret_cast<const char *>(data), size));
    }
  }

  rebaselineHeap();
  return result;
}

std::shared_ptr<const jsi::PreparedJavaScript>
QuickJSRuntime::prepareJavaScript(
    const std::shared_ptr<const jsi::Buffer> &buffer, std::string sourceURL) {
  // Compiling is an engine entry like any other, and it recurses: the parser
  // descends once per nesting level. Without this the recursion bound is still
  // keyed to whichever thread built the runtime, and a script prepared on the
  // JS thread before anything has evaluated runs off the end of its stack.
  adoptCurrentThread();
  drainPendingReleases();

  const uint8_t *data = buffer->data();
  const size_t size = buffer->size();

  if (isBytecodeContainer(data, size)) {
    return std::make_shared<const QuickJSPreparedJavaScript>(
        std::vector<uint8_t>(data + kBytecodeHeaderSize, data + size));
  }
  throwIfHermesBytecode(data, size, sourceURL);

  // The name compiled in here is the only one that survives into the function
  // bytecode, so it has to be the name the debugger will look the script up by.
  sourceURL = effectiveSourceURL(data, size, sourceURL);

  const std::string owned(reinterpret_cast<const char *>(data), size);
  JSValue compiled = checkException(JS_Eval(
      context_, owned.c_str(), owned.size(), sourceURL.c_str(),
      JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY));

  size_t length = 0;
  uint8_t *bytes =
      JS_WriteObject(context_, &length, compiled, JS_WRITE_OBJ_BYTECODE);
  JS_FreeValue(context_, compiled);
  if (bytes == nullptr) {
    throwPendingError();
  }

  std::vector<uint8_t> bytecode(bytes, bytes + length);
  js_free(context_, bytes);
  return std::make_shared<const QuickJSPreparedJavaScript>(std::move(bytecode));
}

jsi::Value QuickJSRuntime::evaluatePreparedJavaScript(
    const std::shared_ptr<const jsi::PreparedJavaScript> &js) {
  adoptCurrentThread();
  drainPendingReleases();

  auto prepared =
      std::dynamic_pointer_cast<const QuickJSPreparedJavaScript>(js);
  if (!prepared) {
    throw jsi::JSINativeException(
        "QuickJSRuntime: PreparedJavaScript was created by a different runtime "
        "implementation");
  }

  StartupGCBracket bracket(
      runtime_, config_.startupGCBracketBytes, startupGCBracketUsed_);

  jsi::Value result = evaluateBytecode(
      prepared->bytecode().data(), prepared->bytecode().size());
  rebaselineHeap();
  return result;
}

void QuickJSRuntime::queueMicrotask(const jsi::Function &callback) {
  JSValue function = toJSValue(callback);
  checkException(JS_EnqueueJob(context_, microtaskJob, 1, &function));
}

bool QuickJSRuntime::drainMicrotasks(int maxMicrotasksHint) {
  const auto taskStart = std::chrono::steady_clock::now();
  adoptCurrentThread();
  drainPendingReleases();

  int executed = 0;
  while (maxMicrotasksHint < 0 || executed < maxMicrotasksHint) {
    JSContext *jobContext = nullptr;
    int result = JS_ExecutePendingJob(runtime_, &jobContext);
    if (result == 0) {
      // Queue drained, so the JS stack is empty: a safepoint. Every safepoint
      // ends a task, whether or not it collected.
      runPendingGCIfIdle(taskStart);
      lastTaskEnd_ = std::chrono::steady_clock::now();
      return true;
    }
    if (result < 0) {
      // JSI discards the exceptional job and keeps going; the queue is only
      // reported as not drained if work remains.
      JSContext *ctx = jobContext != nullptr ? jobContext : context_;
      JS_FreeValue(ctx, JS_GetException(ctx));
    }
    ++executed;
  }
  return !JS_IsJobPending(runtime_);
}

void QuickJSRuntime::runPendingGC() noexcept {
  // Unconditional: the embedder is asserting the app is idle. Safe only with an
  // empty JS stack, never from inside a host function.
  if (runtime_ == nullptr) {
    return;
  }
  JS_RunPendingGC(runtime_);
  noteCollection();
  lastTaskEnd_ = std::chrono::steady_clock::now();
}

/// The live set just changed, so the growth baseline and the ceiling derived
/// from it are stale.
void QuickJSRuntime::rebaselineHeap() noexcept {
  heapBaseline_ = JS_GetMallocSize(runtime_);
  refreshDeferredGCLimit();
}

/// A collection ran, so the clock the max-deferral valve measures from restarts
/// too. Only a collection may reset it: a script that merely rebaselines the
/// heap would restart the wait without ending it, and an app that keeps
/// evaluating would defer a pending collection forever.
void QuickJSRuntime::noteCollection() noexcept {
  rebaselineHeap();
  pendingSince_ = {};
}

size_t QuickJSRuntime::deferredGCSlack() const noexcept {
  size_t slack = JS_GetMallocSize(runtime_);
  if (slack < config_.deferredGCMinSlack) {
    slack = config_.deferredGCMinSlack;
  }
  if (slack > config_.deferredGCMaxSlack) {
    slack = config_.deferredGCMaxSlack;
  }
  return slack;
}

size_t QuickJSRuntime::deferredGCCeiling() const noexcept {
  if (config_.deferredGCLimit != 0) {
    return config_.deferredGCLimit;
  }
  return JS_GetMallocSize(runtime_) + deferredGCSlack();
}

void QuickJSRuntime::refreshDeferredGCLimit() noexcept {
  if (runtime_ == nullptr || !config_.deferGC) {
    return;
  }
  if (config_.deferredGCLimit != 0) {
    return;  // an explicit ceiling belongs to the embedder
  }
  JS_SetGCDeferred(runtime_, true, deferredGCCeiling());
}

void QuickJSRuntime::runPendingGCIfIdle(
    std::chrono::steady_clock::time_point taskStart) noexcept {
  if (runtime_ == nullptr) {
    return;
  }

  // Before the pending check on purpose: the case this fixes is where nothing
  // is pending because the trigger has drifted out of reach of the live heap.
  if (config_.gcThresholdRetuneRatio > 0.0) {
    const size_t liveNow = JS_GetMallocSize(runtime_);
    const size_t threshold = JS_GetGCThreshold(runtime_);
    const double drift = liveNow > 0 ? static_cast<double>(threshold) /
                                           static_cast<double>(liveNow)
                                     : 0.0;
    if (liveNow > 0 && drift > config_.gcThresholdRetuneRatio) {
      JS_SetGCThreshold(runtime_, liveNow + (liveNow >> 1));
    }
  }

  if (!JS_HasPendingGC(runtime_)) {
    return;
  }

  // Measured against the end of the previous task, not the last collection:
  // timing from the collection makes any long stretch of work look idle.
  const auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(
                       taskStart - lastTaskEnd_)
                       .count();
  const bool idle = lastTaskEnd_.time_since_epoch().count() == 0 ||
                    gap >= static_cast<long long>(config_.gcIdleGapMs);

  if (pendingSince_.time_since_epoch().count() == 0) {
    pendingSince_ = taskStart;
  }
  bool stale = false;
  if (!idle && config_.gcMaxDeferralMs > 0) {
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                            taskStart - pendingSince_)
                            .count();
    stale = waited >= static_cast<long long>(config_.gcMaxDeferralMs);
  }

  // Pressure is growth since the last collection, not size. Comparing live size
  // against half the ceiling reduces to `live > slack`, which is true on every
  // safepoint for any real app, turning the valve into "collect constantly".
  const size_t live = JS_GetMallocSize(runtime_);
  const size_t grown = live > heapBaseline_ ? live - heapBaseline_ : 0;
  size_t pressureLimit = config_.gcPressureBytes;
  if (pressureLimit == 0) {
    // Also capped by the live heap, or the valve is unreachable on an app whose
    // whole heap is smaller than the slack -- measured as a collection pending
    // forever on an app with a ticker, which never reaches an idle gap either.
    const size_t bySlack = deferredGCSlack() / 2;
    const size_t byHeap = live / 2;
    pressureLimit = bySlack < byHeap ? bySlack : byHeap;
  }
  if (!idle && !stale && grown < pressureLimit) {
    return;
  }

  JS_RunPendingGC(runtime_);
  noteCollection();
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

std::string QuickJSRuntime::symbolToString(const jsi::Symbol &symbol) {
  JSValue argument = toJSValue(symbol);
  JSValue result =
      JS_Call(context_, symbolToString_, JS_UNDEFINED, 1, &argument);
  checkException(result);

  size_t length = 0;
  const char *chars = JS_ToCStringLen(context_, &length, result);
  JS_FreeValue(context_, result);
  if (chars == nullptr) {
    throwPendingError();
  }
  std::string string(chars, length);
  JS_FreeCString(context_, chars);
  return string;
}

jsi::BigInt QuickJSRuntime::createBigIntFromInt64(int64_t value) {
  JSValue bigint = JS_NewBigInt64(context_, value);
  checkException(bigint);
  return make<jsi::BigInt>(allocPointerValue(bigint));
}

jsi::BigInt QuickJSRuntime::createBigIntFromUint64(uint64_t value) {
  JSValue bigint = JS_NewBigUint64(context_, value);
  checkException(bigint);
  return make<jsi::BigInt>(allocPointerValue(bigint));
}

bool QuickJSRuntime::bigintIsInt64(const jsi::BigInt &bigint) {
  int64_t truncated = 0;
  if (JS_ToBigInt64(context_, &truncated, toJSValue(bigint)) != 0) {
    JS_FreeValue(context_, JS_GetException(context_));
    return false;
  }
  // JS_ToBigInt64 wraps rather than failing, so round-trip to detect loss.
  JSValue roundTrip = JS_NewBigInt64(context_, truncated);
  bool fits = JS_IsStrictEqual(context_, roundTrip, toJSValue(bigint));
  JS_FreeValue(context_, roundTrip);
  return fits;
}

bool QuickJSRuntime::bigintIsUint64(const jsi::BigInt &bigint) {
  uint64_t truncated = 0;
  if (JS_ToBigUint64(context_, &truncated, toJSValue(bigint)) != 0) {
    JS_FreeValue(context_, JS_GetException(context_));
    return false;
  }
  JSValue roundTrip = JS_NewBigUint64(context_, truncated);
  bool fits = JS_IsStrictEqual(context_, roundTrip, toJSValue(bigint));
  JS_FreeValue(context_, roundTrip);
  return fits;
}

uint64_t QuickJSRuntime::truncate(const jsi::BigInt &bigint) {
  uint64_t truncated = 0;
  if (JS_ToBigUint64(context_, &truncated, toJSValue(bigint)) != 0) {
    throwPendingError();
  }
  return truncated;
}

jsi::String QuickJSRuntime::bigintToString(
    const jsi::BigInt &bigint, int radix) {
  if (radix < 2 || radix > 36) {
    throw jsi::JSINativeException(
        "QuickJSRuntime: radix must be between 2 and 36");
  }
  JSValue arguments[2] = {toJSValue(bigint), JS_NewInt32(context_, radix)};
  JSValue result =
      JS_Call(context_, bigIntToString_, JS_UNDEFINED, 2, arguments);
  checkException(result);
  return createStringFrom(result);
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
  JSValue object = JS_NewObjectClass(context_, hostObjectClassID_);
  checkException(object);
  auto *proxy = new HostObjectProxy{{}, this, std::move(ho)};
  registerHostObject(proxy);
  JS_SetOpaque(object, proxy);
  return createObjectFrom(object);
}

std::shared_ptr<jsi::HostObject> QuickJSRuntime::getHostObject(
    const jsi::Object &object) {
  auto *proxy = static_cast<HostObjectProxy *>(
      JS_GetOpaque(toJSValue(object), hostObjectClassID_));
  if (proxy == nullptr) {
    throw jsi::JSINativeException("QuickJSRuntime: not a host object");
  }
  return proxy->hostObject;
}

jsi::HostFunctionType &QuickJSRuntime::getHostFunction(
    const jsi::Function &function) {
  auto *proxy = static_cast<HostFunctionProxy *>(
      JS_GetOpaque(toJSValue(function), hostFunctionClassID_));
  if (proxy == nullptr) {
    throw jsi::JSINativeException("QuickJSRuntime: not a host function");
  }
  return proxy->function;
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

/// JSI reserves native state for ordinary objects: a proxy would route the
/// property through its traps, and a host object answers for every key.
void QuickJSRuntime::checkCanHoldNativeState(const jsi::Object &object) {
  JSValue value = toJSValue(object);
  if (JS_IsProxy(value)) {
    throw jsi::JSINativeException(
        "QuickJSRuntime: cannot set native state on a proxy");
  }
  if (JS_GetOpaque(value, hostObjectClassID_) != nullptr) {
    throw jsi::JSINativeException(
        "QuickJSRuntime: cannot set native state on a host object");
  }
}

bool QuickJSRuntime::hasNativeState(const jsi::Object &object) {
  JSValue holder = JS_GetProperty(context_, toJSValue(object), nativeStateKey_);
  if (JS_IsException(holder)) {
    JS_FreeValue(context_, JS_GetException(context_));
    return false;
  }
  const bool present = JS_GetOpaque(holder, nativeStateClassID_) != nullptr;
  JS_FreeValue(context_, holder);
  return present;
}

std::shared_ptr<jsi::NativeState> QuickJSRuntime::getNativeState(
    const jsi::Object &object) {
  JSValue holder = checkException(
      JS_GetProperty(context_, toJSValue(object), nativeStateKey_));
  auto *proxy = static_cast<NativeStateProxy *>(
      JS_GetOpaque(holder, nativeStateClassID_));
  JS_FreeValue(context_, holder);
  if (proxy == nullptr) {
    throw jsi::JSINativeException("QuickJSRuntime: object has no native state");
  }
  // May be null: JSI allows clearing the state, after which hasNativeState
  // still reports true.
  return proxy->state;
}

void QuickJSRuntime::setNativeState(
    const jsi::Object &object, std::shared_ptr<jsi::NativeState> state) {
  checkCanHoldNativeState(object);

  // Overwriting is a pointer swap: the holder is reachable only from this
  // property, so nothing else can be looking at it.
  JSValue existing = checkException(
      JS_GetProperty(context_, toJSValue(object), nativeStateKey_));
  auto *held = static_cast<NativeStateProxy *>(
      JS_GetOpaque(existing, nativeStateClassID_));
  JS_FreeValue(context_, existing);
  if (held != nullptr) {
    held->state = std::move(state);
    return;
  }

  JSValue holder = JS_NewObjectClass(context_, nativeStateClassID_);
  checkException(holder);
  auto *proxy = new NativeStateProxy{{}, std::move(state)};
  registerNativeState(proxy);
  JS_SetOpaque(holder, proxy);

  // Writable and configurable so a later call can replace it, which JSI
  // requires. Neither weakens anything: script has no way to name the key.
  checkException(JS_DefinePropertyValue(
      context_, toJSValue(object), JS_DupAtom(context_, nativeStateKey_),
      holder, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE));
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

bool QuickJSRuntime::isHostObject(const jsi::Object &object) const {
  return JS_GetOpaque(toJSValue(object), hostObjectClassID_) != nullptr;
}

bool QuickJSRuntime::isHostFunction(const jsi::Function &function) const {
  return JS_GetOpaque(toJSValue(function), hostFunctionClassID_) != nullptr;
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

jsi::WeakObject QuickJSRuntime::createWeakObject(const jsi::Object &object) {
  if (!JS_IsObject(weakRefConstructor_)) {
    throw jsi::JSINativeException(
        "QuickJSRuntime: WeakRef is unavailable in this context");
  }
  JSValue argument = toJSValue(object);
  JSValue weakRef =
      JS_CallConstructor(context_, weakRefConstructor_, 1, &argument);
  checkException(weakRef);
  return make<jsi::WeakObject>(allocPointerValue(weakRef));
}

jsi::Value QuickJSRuntime::lockWeakObject(const jsi::WeakObject &weakObject) {
  JSValue result =
      JS_Call(context_, weakRefDeref_, toJSValue(weakObject), 0, nullptr);
  checkException(result);
  return createValue(result);
}

jsi::Array QuickJSRuntime::createArray(size_t length) {
  JSValue array = JS_NewArray(context_);
  checkException(array);
  if (JS_SetLength(context_, array, static_cast<int64_t>(length)) < 0) {
    JS_FreeValue(context_, array);
    throwPendingError();
  }
  return make<jsi::Object>(allocPointerValue(array)).getArray(*this);
}

jsi::ArrayBuffer QuickJSRuntime::createArrayBuffer(
    std::shared_ptr<jsi::MutableBuffer> buffer) {
  uint8_t *data = buffer->data();
  size_t size = buffer->size();
  auto *proxy = new ArrayBufferProxy{std::move(buffer)};

  JSValue arrayBuffer = JS_NewArrayBuffer(
      context_, data, size, freeArrayBufferProxy, proxy, false);
  if (JS_IsException(arrayBuffer)) {
    delete proxy;
    throwPendingError();
  }
  return make<jsi::Object>(allocPointerValue(arrayBuffer))
      .getArrayBuffer(*this);
}

size_t QuickJSRuntime::size(const jsi::Array &array) {
  int64_t length = 0;
  checkException(JS_GetLength(context_, toJSValue(array), &length));
  return static_cast<size_t>(length);
}

size_t QuickJSRuntime::size(const jsi::ArrayBuffer &arrayBuffer) {
  size_t size = 0;
  if (JS_GetArrayBuffer(context_, &size, toJSValue(arrayBuffer)) == nullptr) {
    throwPendingError();
  }
  return size;
}

uint8_t *QuickJSRuntime::data(const jsi::ArrayBuffer &arrayBuffer) {
  size_t size = 0;
  uint8_t *data = JS_GetArrayBuffer(context_, &size, toJSValue(arrayBuffer));
  if (data == nullptr) {
    throwPendingError();
  }
  return data;
}

jsi::Value QuickJSRuntime::getValueAtIndex(
    const jsi::Array &array, size_t index) {
  return createValue(JS_GetPropertyUint32(
      context_, toJSValue(array), static_cast<uint32_t>(index)));
}

void QuickJSRuntime::setValueAtIndexImpl(
    const jsi::Array &array, size_t index, const jsi::Value &value) {
  checkException(JS_SetPropertyUint32(
      context_, toJSValue(array), static_cast<uint32_t>(index),
      JS_DupValue(context_, toJSValue(value))));
}

jsi::Function QuickJSRuntime::createFunctionFromHostFunction(
    const jsi::PropNameID &name, unsigned int paramCount,
    jsi::HostFunctionType func) {
  JSValue function = JS_NewObjectClass(context_, hostFunctionClassID_);
  checkException(function);
  auto *proxy = new HostFunctionProxy{{}, this, std::move(func)};
  registerHostFunction(proxy);
  JS_SetOpaque(function, proxy);
  JS_SetConstructorBit(context_, function, true);

  // `name` and `length` are ordinary own properties on JS functions, and code
  // in the wild reads them.
  JS_DefinePropertyValueStr(
      context_, function, "length",
      JS_NewInt32(context_, static_cast<int32_t>(paramCount)),
      JS_PROP_CONFIGURABLE);
  JS_DefinePropertyValueStr(
      context_, function, "name", JS_AtomToString(context_, toJSAtom(name)),
      JS_PROP_CONFIGURABLE);

  return make<jsi::Object>(allocPointerValue(function)).getFunction(*this);
}

jsi::Value QuickJSRuntime::call(
    const jsi::Function &function, const jsi::Value &jsThis,
    const jsi::Value *args, size_t count) {
  adoptCurrentThread();
  drainPendingReleases();

  ArgumentList arguments(*this, args, count);
  return createValue(JS_Call(
      context_, toJSValue(function), toJSValue(jsThis), static_cast<int>(count),
      arguments.data()));
}

jsi::Value QuickJSRuntime::callAsConstructor(
    const jsi::Function &function, const jsi::Value *args, size_t count) {
  adoptCurrentThread();
  drainPendingReleases();

  ArgumentList arguments(*this, args, count);
  return createValue(JS_CallConstructor(
      context_, toJSValue(function), static_cast<int>(count),
      arguments.data()));
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
