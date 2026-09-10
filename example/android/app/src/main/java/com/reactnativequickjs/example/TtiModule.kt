package com.reactnativequickjs.example

import android.util.Log
import android.os.SystemClock
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.bridge.ReactContextBaseJavaModule
import com.facebook.react.bridge.ReactMethod

class TtiModule(context: ReactApplicationContext) : ReactContextBaseJavaModule(context) {
  override fun getName(): String = "RNQJSTti"

  override fun getConstants(): MutableMap<String, Any> = mutableMapOf(
    "engine" to BuildConfig.RNQJS_ENGINE,
  )

  @ReactMethod
  fun report(event: String) {
    val elapsedMillis = (SystemClock.elapsedRealtimeNanos() -
        MainApplication.launchTimeNanos.get()) / 1_000_000.0
    Log.i("RNQJS_TTI", "TTI event=$event timeMs=$elapsedMillis")
  }
}
