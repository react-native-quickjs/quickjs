/*
 * Android platform layer for react-native-quickjs-text-encoding.
 *
 * JNI glue between the shared C++ in cpp/ and the Kotlin in
 * src/main/java/. Implement the TextEncodingPlatform interface here in terms of
 * Android APIs.
 */

#include "TextEncodingPlatform.h"

#include <jni.h>

#include <string>

namespace {

JavaVM *g_vm = nullptr;
jclass g_platformClass = nullptr;

JNIEnv *env() {
  JNIEnv *e = nullptr;
  if (g_vm == nullptr) return nullptr;
  if (g_vm->GetEnv(reinterpret_cast<void **>(&e), JNI_VERSION_1_6) != JNI_OK) {
    // A call arriving on a thread the VM has not seen must attach first,
    // otherwise every JNI call returns garbage.
    if (g_vm->AttachCurrentThread(&e, nullptr) != JNI_OK) return nullptr;
  }
  return e;
}

class AndroidPlatform : public text_encoding::Platform {
 public:
  std::string deviceLocale() override {
    JNIEnv *e = env();
    if (e == nullptr || g_platformClass == nullptr) return "en-US";

    jmethodID mid = e->GetStaticMethodID(
        g_platformClass, "deviceLocale", "()Ljava/lang/String;");
    if (mid == nullptr) return "en-US";

    auto jstr =
        static_cast<jstring>(e->CallStaticObjectMethod(g_platformClass, mid));
    if (jstr == nullptr) return "en-US";

    const char *chars = e->GetStringUTFChars(jstr, nullptr);
    std::string result(chars ? chars : "en-US");
    e->ReleaseStringUTFChars(jstr, chars);
    e->DeleteLocalRef(jstr);
    return result;
  }
};

AndroidPlatform g_platform;

}  // namespace

// JNI mangles the underscore in com.text_encoding, so the export is
// Java_com_text_1encoding_TextEncodingPlatform_nativeAttach.
extern "C" JNIEXPORT void JNICALL
Java_com_text_1encoding_TextEncodingPlatform_nativeAttach(JNIEnv *e, jobject) {
  JavaVM *vm = nullptr;
  if (e->GetJavaVM(&vm) == JNI_OK) {
    g_vm = vm;
  }
  // Global ref: the local class reference dies with this frame, and later
  // calls arrive from other threads.
  jclass local = e->FindClass("com/text_encoding/TextEncodingPlatform");
  if (local != nullptr) {
    g_platformClass = static_cast<jclass>(e->NewGlobalRef(local));
    e->DeleteLocalRef(local);
  }
  text_encoding::setPlatform(&g_platform);
}
