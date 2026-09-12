#include <fbjni/fbjni.h>
#include <jsi/jsi.h>

namespace facebook::react {
namespace {

class JsiBenchModule : public jni::JavaClass<JsiBenchModule> {
 public:
  static constexpr const char *kJavaDescriptor =
      "Lcom/reactnativequickjs/example/JsiBenchModule;";

  static void registerNatives() {
    javaClassLocal()->registerNatives({
        makeNativeMethod("nativeInstall", nativeInstall),
    });
  }

 private:
  static void nativeInstall(
      jni::alias_ref<JsiBenchModule> /*module*/, jlong runtime_ptr) {
    auto &runtime = *reinterpret_cast<jsi::Runtime *>(runtime_ptr);
    auto bench = jsi::Object(runtime);
    auto noop = [](jsi::Runtime &, const jsi::Value &, const jsi::Value *,
                   size_t) { return jsi::Value(1); };
    auto add = [&](const char *name, unsigned int arity) {
      auto function = jsi::Function::createFromHostFunction(
          runtime, jsi::PropNameID::forAscii(runtime, name), arity, noop);
      bench.setProperty(runtime, name, std::move(function));
    };
    add("native0", 0);
    add("native1", 1);
    add("native4", 4);
    add("native8", 8);
    runtime.global().setProperty(runtime, "__RNQJSJsiBench", std::move(bench));
  }
};

}  // namespace
}  // namespace facebook::react

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *) {
  return facebook::jni::initialize(
      vm, [] { facebook::react::JsiBenchModule::registerNatives(); });
}
