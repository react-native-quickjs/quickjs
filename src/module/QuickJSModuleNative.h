/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <jsi/jsi.h>
#include <quickjs.h>

#include "QuickJSModule.h"
#include "QuickJSRuntime.h"

/**
 * The engine escape hatch for QuickJS modules.
 *
 * Include this to work directly against the engine, which is the recommended
 * way to write a module here. A module written against `QuickJSModule.h` alone
 * is portable JSI and runs on any jsi::Runtime; that is the right choice only
 * when the module must also run on Hermes.
 *
 * The decisive reason is constructors. A jsi::Function from
 * createFromHostFunction is not a constructor, and `new` against it does not
 * fail loudly: `thisVal` is the constructor function itself, so writes to
 * `this` mutate the constructor, `instanceof` is false, and
 * `class S extends Thing {}` gives undefined from `new S()`. JSI's
 * HostFunctionType has no new.target and no way to build `this` from
 * `.prototype`, so it cannot be fixed from above. `JS_NewCFunction2` with
 * `JS_CFUNC_constructor` gives a real constructor.
 *
 * The rest is cost. Creating a Uint8Array in plain JSI means fetching the
 * global constructor and calling it, per call, against one `JS_NewUint8Array`;
 * `JS_NewStringLen` takes UTF-8 bytes directly; `JS_GetUint8Array` reads a
 * typed array without copying. Atoms, classes and opaque pointers have no JSI
 * spelling at all.
 *
 * A module that needs both writes the fast path behind a check:
 *
 *     JSContext *ctx = qjs::contextFromRuntime(rt);
 *     if (ctx) { ... } else { ... }
 *
 * Ownership follows the rest of the runtime. `adopt*` takes the reference you
 * pass; `borrow*` returns one you must not free, valid only as long as the
 * original. A `JS_*` function returning a JSValue gives you a reference you
 * own.
 */

namespace qjs {

/// The concrete runtime, or nullptr if `runtime` is not ours.
inline QuickJSRuntime *quickJSRuntime(jsi::Runtime &runtime) noexcept {
  return dynamic_cast<QuickJSRuntime *>(&runtime);
}

/// Wraps a JSValue you own as a jsi::Value, transferring ownership. The usual
/// return path for a host function that used the engine directly.
inline jsi::Value adoptJSValue(jsi::Runtime &runtime, JSValue value) {
  auto *rt = quickJSRuntime(runtime);
  if (rt == nullptr) {
    // Nothing owns this value now and it cannot be freed without a context, so
    // this is a programming error rather than a runtime condition.
    throw jsi::JSINativeException(
        "adoptJSValue called on a runtime that is not QuickJS-backed");
  }
  return rt->createValue(value);
}

/// Borrows the JSValue behind a jsi::Value. Owned by `value`, valid only while
/// it is, and must not be freed.
inline JSValue borrowJSValue(jsi::Runtime &runtime, const jsi::Value &value) {
  auto *rt = quickJSRuntime(runtime);
  if (rt == nullptr) {
    throw jsi::JSINativeException(
        "borrowJSValue called on a runtime that is not QuickJS-backed");
  }
  return rt->toJSValue(value);
}

/// Converts a pending quickjs exception into a thrown jsi::JSError. Call it
/// whenever an engine API returns the exception sentinel: a pending exception
/// left alive past the host function's return corrupts the engine's error
/// state and fails far from its cause.
[[noreturn]] inline void throwPendingQuickJSError(jsi::Runtime &runtime) {
  auto *rt = quickJSRuntime(runtime);
  if (rt == nullptr) {
    throw jsi::JSINativeException("QuickJS error on a non-QuickJS runtime");
  }
  rt->throwPendingError();
}

}  // namespace qjs
