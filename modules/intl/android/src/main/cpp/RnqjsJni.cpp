/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RnqjsJni.h"

namespace rnqjs::jni {

namespace {

JavaVM *g_vm = nullptr;

/*
 * Detaching on thread exit rather than after every call.
 *
 * AttachCurrentThread registers the thread with the VM; doing it per call costs
 * that registration every time and, worse, invalidates any local reference the
 * caller was holding across the detach. So: attach once, and let a
 * thread_local destructor detach when the thread actually ends.
 *
 * The flag matters. A thread the VM already owned — the Android main thread, a
 * thread the VM created — must **not** be detached by us; doing so is a crash
 * at the next JNI call from Java. Only a thread this helper attached is
 * detached here.
 */
struct ThreadDetacher {
  bool attachedByUs = false;
  ~ThreadDetacher() {
    if (attachedByUs && g_vm != nullptr) {
      g_vm->DetachCurrentThread();
    }
  }
};

thread_local ThreadDetacher t_detacher;

}  // namespace

void setVM(JavaVM *vm) {
  g_vm = vm;
}
JavaVM *vm() {
  return g_vm;
}

Env::Env() {
  if (g_vm == nullptr) return;  // host build; every caller checks valid()
  void *raw = nullptr;
  const jint rc = g_vm->GetEnv(&raw, JNI_VERSION_1_6);
  if (rc == JNI_OK) {
    env_ = static_cast<JNIEnv *>(raw);
    return;
  }
  if (rc != JNI_EDETACHED) return;  // JNI_EVERSION: nothing sensible to do
  JNIEnv *attached = nullptr;
  if (g_vm->AttachCurrentThread(&attached, nullptr) != JNI_OK) return;
  env_ = attached;
  t_detacher.attachedByUs = true;
}

bool Env::check() const {
  if (env_ == nullptr) return false;
  if (env_->ExceptionCheck() == JNI_FALSE) return false;
  /*
   * ExceptionDescribe writes to logcat, which is the only way anyone will ever
   * see this: the platform interface does not propagate errors, so the caller
   * turns a failure into a "no opinion" answer and the app carries on with
   * root-locale output. Silently clearing would make a systematic backend
   * failure indistinguishable from a locale the OS genuinely does not know.
   */
  env_->ExceptionDescribe();
  env_->ExceptionClear();
  return true;
}

LocalFrame::LocalFrame(const Env &env, jint capacity) : env_(env.get()) {
  if (env_ == nullptr) return;
  pushed_ = env_->PushLocalFrame(capacity) == 0;
  if (!pushed_) {
    // PushLocalFrame throws OutOfMemoryError on failure; leaving it pending
    // would make every following JNI call undefined behaviour.
    env_->ExceptionClear();
  }
}

LocalFrame::~LocalFrame() {
  if (pushed_ && env_ != nullptr) env_->PopLocalFrame(nullptr);
}

void ClassRef::adopt(const Env &env, jclass local) {
  if (!env.valid() || local == nullptr) return;
  if (clazz_ != nullptr) env->DeleteGlobalRef(clazz_);
  clazz_ = static_cast<jclass>(env->NewGlobalRef(local));
}

void ClassRef::release(const Env &env) {
  if (clazz_ != nullptr && env.valid()) {
    env->DeleteGlobalRef(clazz_);
    clazz_ = nullptr;
  }
}

jmethodID ClassRef::staticMethod(
    const Env &env, const char *name, const char *sig) {
  if (!env.valid() || clazz_ == nullptr) return nullptr;
  jmethodID m = env->GetStaticMethodID(clazz_, name, sig);
  if (env.check()) return nullptr;  // NoSuchMethodError, cleared
  return m;
}

std::u16string toU16(const Env &env, jstring s) {
  if (!env.valid() || s == nullptr) return {};
  const jsize n = env->GetStringLength(s);
  const jchar *chars = env->GetStringChars(s, nullptr);
  if (chars == nullptr) {
    env.check();
    return {};
  }
  // GetStringChars, not GetStringUTFChars: this is the java.lang.String's own
  // UTF-16 data, which is exactly what JS_NewStringUTF16 takes. The UTF variant
  // returns *modified* UTF-8 — U+0000 as two bytes, astral characters as two
  // separately-encoded surrogates — which is both slower and lossy.
  std::u16string out(
      reinterpret_cast<const char16_t *>(chars), static_cast<size_t>(n));
  env->ReleaseStringChars(s, chars);
  return out;
}

std::string toUtf8(const Env &env, jstring s) {
  if (!env.valid() || s == nullptr) return {};
  const char *chars = env->GetStringUTFChars(s, nullptr);
  if (chars == nullptr) {
    env.check();
    return {};
  }
  std::string out(chars);
  env->ReleaseStringUTFChars(s, chars);
  return out;
}

Local<jstring> fromUtf8(const Env &env, const std::string &s) {
  if (!env.valid()) return {};
  jstring js = env->NewStringUTF(s.c_str());
  if (env.check()) return {};
  return Local<jstring>(env.get(), js);
}

std::string callStaticStringString(
    const Env &env, ClassRef &cls, jmethodID method, const std::string &arg) {
  if (!env.valid() || !cls || method == nullptr) return {};
  LocalFrame frame(env, 4);
  Local<jstring> jarg = fromUtf8(env, arg);
  if (!jarg) return {};
  auto result = static_cast<jstring>(
      env->CallStaticObjectMethod(cls.get(), method, jarg.get()));
  if (env.check() || result == nullptr) return {};
  Local<jstring> owned(env.get(), result);
  return toUtf8(env, owned.get());
}

std::vector<std::string> callStaticStringArray(
    const Env &env, ClassRef &cls, jmethodID method) {
  std::vector<std::string> out;
  if (!env.valid() || !cls || method == nullptr) return out;

  auto array =
      static_cast<jobjectArray>(env->CallStaticObjectMethod(cls.get(), method));
  if (env.check() || array == nullptr) return out;
  Local<jobjectArray> ownedArray(env.get(), array);

  const jsize n = env->GetArrayLength(ownedArray.get());
  out.reserve(static_cast<size_t>(n));
  /*
   * Two mechanisms, and they do different jobs. The `Local` below deletes each
   * element reference as soon as it has been copied, which is what actually
   * keeps the table bounded — these arrays are large (Android's available
   * locale list is ~900 entries, its timezone list ~600) and the default local
   * reference table holds 512, and overflowing it is an abort rather than an
   * exception. The frame is a backstop for anything else in the loop that
   * creates a reference and does not own it; it does not by itself bound a loop
   * that leaks, since it is popped only once at the end.
   */
  LocalFrame frame(env, 16);
  for (jsize i = 0; i < n; i++) {
    auto element =
        static_cast<jstring>(env->GetObjectArrayElement(ownedArray.get(), i));
    if (env.check()) break;
    if (element == nullptr) continue;
    Local<jstring> owned(env.get(), element);
    out.push_back(toUtf8(env, owned.get()));
  }
  return out;
}

}  // namespace rnqjs::jni
