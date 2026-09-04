/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef RNQJS_JNI_H
#define RNQJS_JNI_H

/*
 * A minimal hand-written JNI helper.
 *
 * WHY NOT fbjni
 *   fbjni is a large dependency with its own exception model, its own
 *   `HybridClass` lifecycle and its own build requirements, and this module
 *   needs five things from JNI: get an env, hold a global class reference, call
 *   a static method, convert a string, and not leak local references. Those
 *   five things are about 200 lines. Taking fbjni for them would make the
 *   module's build depend on React Native's C++ artefacts, which is precisely
 *   what the `tools/intl-cli` and test262 host builds exist to avoid.
 *
 * WHY IT LIVES HERE FOR NOW
 *   It is deliberately named `rnqjs::jni` rather than `intl::jni`, and has no
 *   Intl-specific content, so it can move to `common/` unchanged the first time
 *   a second module needs it. It ships inside this module until that happens,
 *   because a shared helper with one user is a shared helper nobody has
 *   pressure-tested.
 *
 * THE FIVE THINGS IT GETS RIGHT, AND WHY EACH ONE IS HERE
 *
 *   1. **Thread attach/detach.** React Native's JS thread is not the Android
 *      main thread and is generally not a thread the VM created, so
 *      `GetEnv` returns JNI_EDETACHED and every subsequent JNI call on a
 *      naively-obtained env is undefined behaviour. `Env` attaches on demand
 *      and, crucially, detaches when the *thread* dies rather than when the
 *      call returns: attaching and detaching per call costs a VM-internal
 *      thread registration each time. A thread this helper attached is
 *      detached by a `thread_local` destructor; a thread the VM already owned
 *      is never detached, because detaching a VM-owned thread is a crash.
 *
 *   2. **RAII references, and PushLocalFrame/PopLocalFrame around loops.** The
 *      default local reference table is 512 entries on most Android versions,
 *      and overflowing it is an abort with `ReferenceTable overflow`, not an
 *      exception. Enumerating ~600 timezone names — which
 *      `Intl.supportedValuesOf('timeZone')` does — overflows it comfortably.
 *      `LocalFrame` makes the fix one line at the top of the loop.
 *
 *   3. **Exception checking after every call that can throw.** A pending Java
 *      exception makes every subsequent JNI call undefined behaviour, and the
 *      failure surfaces far from its cause. `Env::check()` clears and reports;
 *      the platform layer turns that into a "no opinion" answer rather than
 *      propagating, because a locale the OS does not know is not a program
 *      error. Nothing here ever leaves an exception pending.
 *
 *   4. **Cached jclass globals and jmethodIDs.** `FindClass` walks the
 *      classloader and is the standard way JNI code becomes slow; a `jmethodID`
 *      is only valid while its class is alive, which is why the class reference
 *      must be a *global* and not a local. Both are resolved once, at attach.
 *
 *   5. **UTF-16 string conversion with no UTF-8 round trip.** `GetStringChars`
 *      hands back the java.lang.String's own UTF-16 data, which is what QuickJS
 *      wants (`JS_NewStringUTF16`). `GetStringUTFChars` would transcode to
 *      modified UTF-8 and then the engine would transcode back — two passes per
 *      formatted date for no benefit. (Note that Android's "UTF" JNI functions
 *      use *modified* UTF-8, in which U+0000 is two bytes and astral characters
 *      are surrogate pairs each encoded separately; round-tripping through it
 *      is not merely slow, it is lossy for some inputs.)
 *
 * WHAT THIS DELIBERATELY DOES NOT DO
 *   - No object wrappers, no Java class binding, no reflection. The Java side
 *     of this module is a handful of static methods returning strings.
 *   - No exception *translation* into C++ exceptions. The platform interface
 *     does not throw, by contract, so an exception becomes an empty result.
 *   - No thread-safety. Every call arrives on the JS thread.
 */

#include <jni.h>

#include <string>
#include <vector>

namespace rnqjs::jni {

/**
 * Holds the JavaVM. Call once from JNI_OnLoad.
 */
void setVM(JavaVM *vm);
JavaVM *vm();

/**
 * A JNIEnv for the calling thread, attaching it if necessary.
 *
 * `valid()` is false when there is no VM (a host build) or the attach failed;
 * every caller must check, because a null env used as if it were valid is an
 * immediate segfault rather than a recoverable error.
 */
class Env {
 public:
  Env();
  bool valid() const {
    return env_ != nullptr;
  }
  JNIEnv *operator->() const {
    return env_;
  }
  JNIEnv *get() const {
    return env_;
  }

  /**
   * Clears any pending exception and returns true if there was one.
   *
   * Call after **every** JNI operation that can throw. Leaving one pending
   * makes the next JNI call undefined behaviour and produces a crash whose
   * stack has nothing to do with the cause.
   */
  bool check() const;

 private:
  JNIEnv *env_ = nullptr;
};

/**
 * PushLocalFrame/PopLocalFrame, as a scope guard.
 *
 * Wrap any loop that creates local references. The default local reference
 * table holds 512 entries and overflowing it aborts the process.
 */
class LocalFrame {
 public:
  explicit LocalFrame(const Env &env, jint capacity = 16);
  ~LocalFrame();
  LocalFrame(const LocalFrame &) = delete;
  LocalFrame &operator=(const LocalFrame &) = delete;
  bool ok() const {
    return pushed_;
  }

 private:
  JNIEnv *env_;
  bool pushed_ = false;
};

/** An owning local reference. Deletes on scope exit. */
template <typename T>
class Local {
 public:
  Local() = default;
  Local(JNIEnv *env, T ref) : env_(env), ref_(ref) {}
  ~Local() {
    if (env_ != nullptr && ref_ != nullptr) env_->DeleteLocalRef(ref_);
  }
  Local(const Local &) = delete;
  Local &operator=(const Local &) = delete;
  Local(Local &&o) noexcept : env_(o.env_), ref_(o.ref_) {
    o.ref_ = nullptr;
  }
  Local &operator=(Local &&o) noexcept {
    if (this != &o) {
      if (env_ != nullptr && ref_ != nullptr) env_->DeleteLocalRef(ref_);
      env_ = o.env_;
      ref_ = o.ref_;
      o.ref_ = nullptr;
    }
    return *this;
  }
  T get() const {
    return ref_;
  }
  explicit operator bool() const {
    return ref_ != nullptr;
  }

 private:
  JNIEnv *env_ = nullptr;
  T ref_ = nullptr;
};

/**
 * A class reference plus its resolved method ids, held globally.
 *
 * Resolve once, at attach time, on a thread that can see the app's
 * classloader. A jclass obtained on the JS thread with FindClass will often
 * *not* resolve an application class, because that thread's classloader is the
 * system one — which is the classic "works in a unit test, NoClassDefFoundError
 * on device" JNI bug. Resolving from a Java-initiated call sidesteps it
 * entirely, which is why IntlPlatform.kt calls `nativeAttach()` rather than the
 * C++ calling `FindClass` when it first needs the class.
 */
class ClassRef {
 public:
  /// Takes a *local* class ref and promotes it to a global. Safe to call twice.
  void adopt(const Env &env, jclass local);
  void release(const Env &env);
  jclass get() const {
    return clazz_;
  }
  explicit operator bool() const {
    return clazz_ != nullptr;
  }

  /// Resolves and caches a static method id. Returns nullptr and clears any
  /// exception if the method does not exist.
  jmethodID staticMethod(const Env &env, const char *name, const char *sig);

 private:
  jclass clazz_ = nullptr;
};

/**
 * java.lang.String -> std::u16string, with no UTF-8 round trip.
 *
 * Returns the empty string for a null jstring, which the platform interface
 * reads as "no answer".
 */
std::u16string toU16(const Env &env, jstring s);

/** java.lang.String -> std::string (UTF-8), for ASCII identifiers. */
std::string toUtf8(const Env &env, jstring s);

/** std::string -> a new local java.lang.String reference. */
Local<jstring> fromUtf8(const Env &env, const std::string &s);

/**
 * Calls a `static String name(String)` method and returns its result.
 *
 * The single most common shape in this module's Java surface. Returns "" on any
 * failure, having cleared the exception.
 */
std::string callStaticStringString(
    const Env &env, ClassRef &cls, jmethodID method, const std::string &arg);

/**
 * Calls a `static String[] name()` method.
 *
 * Uses a LocalFrame sized to the array, because the enumeration cases here
 * (600+ timezone ids, 900+ locale ids) are exactly where the default local
 * reference table overflows.
 */
std::vector<std::string> callStaticStringArray(
    const Env &env, ClassRef &cls, jmethodID method);

}  // namespace rnqjs::jni

#endif  // RNQJS_JNI_H
