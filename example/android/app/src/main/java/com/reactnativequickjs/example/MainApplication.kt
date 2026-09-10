package com.reactnativequickjs.example

import android.app.Application
import android.os.SystemClock
import java.util.concurrent.atomic.AtomicLong
import com.facebook.react.PackageList
import com.facebook.react.ReactApplication
import com.facebook.react.ReactHost
import com.facebook.react.ReactNativeApplicationEntryPoint.loadReactNative
import com.facebook.react.defaults.DefaultReactHost.getDefaultReactHost
import com.reactnativequickjs.quickjs.QuickJSInstance

class MainApplication : Application(), ReactApplication {

  companion object {
    @JvmField
    val launchTimeNanos = AtomicLong()
  }

  override val reactHost: ReactHost by lazy {
    getDefaultReactHost(
      context = applicationContext,
      packageList =
        PackageList(this).packages.filterNot { pkg ->
          BuildConfig.RNQJS_ENGINE == "hermes" &&
              (pkg.javaClass.name.startsWith("com.intl.") ||
                  pkg.javaClass.name.startsWith("com.text_encoding.") ||
                  pkg.javaClass.name.startsWith("com.reactnativequickjs.quickjs."))
        }.toMutableList().apply {
          add(TtiPackage())
        },
      jsRuntimeFactory = if (BuildConfig.RNQJS_ENGINE == "quickjs") {
        QuickJSInstance()
      } else {
        null
      },
    )
  }

  override fun onCreate() {
    super.onCreate()
    launchTimeNanos.set(SystemClock.elapsedRealtimeNanos())
    loadReactNative(this)
  }
}
