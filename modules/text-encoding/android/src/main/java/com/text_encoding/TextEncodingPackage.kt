package com.text_encoding

import com.facebook.react.ReactPackage
import com.facebook.react.bridge.NativeModule
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.uimanager.ViewManager

/**
 * Android entry point.
 *
 * The JS-facing API is installed directly into the runtime by the C++ (no
 * bridge, no TurboModule on the call path). This package exists for the things
 * only the Java side can provide: the Application Context, and access to
 * Android APIs the shared C++ wants to call through [TextEncodingPlatform].
 */
class TextEncodingPackage : ReactPackage {
  override fun createNativeModules(context: ReactApplicationContext): List<NativeModule> {
    // Hand the context to the native layer before any JavaScript runs.
    TextEncodingPlatform.attach(context.applicationContext)
    return emptyList()
  }

  override fun createViewManagers(context: ReactApplicationContext): List<ViewManager<*, *>> =
    emptyList()
}
