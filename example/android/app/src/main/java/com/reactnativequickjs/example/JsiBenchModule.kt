package com.reactnativequickjs.example

import com.facebook.react.bridge.JavaScriptContextHolder
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.bridge.ReactContextBaseJavaModule
import com.facebook.react.bridge.ReactMethod
import com.facebook.react.module.annotations.ReactModule
import com.facebook.proguard.annotations.DoNotStrip
import com.facebook.soloader.SoLoader

@DoNotStrip
@ReactModule(name = JsiBenchModule.NAME)
class JsiBenchModule(context: ReactApplicationContext) : ReactContextBaseJavaModule(context) {
  override fun getName(): String = NAME

  @ReactMethod(isBlockingSynchronousMethod = true)
  fun install(): Boolean {
    val context: JavaScriptContextHolder =
        reactApplicationContext.javaScriptContextHolder ?: return false
    val pointer = context.get()
    if (pointer == 0L) return false
    nativeInstall(pointer)
    return true
  }

  private external fun nativeInstall(jsi: Long)

  companion object {
    const val NAME = "RNQJSJsiBench"

    init {
      SoLoader.loadLibrary("rnqjsbench")
    }
  }
}
