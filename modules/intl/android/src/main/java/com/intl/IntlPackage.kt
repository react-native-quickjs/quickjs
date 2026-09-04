package com.intl

import com.facebook.react.ReactPackage
import com.facebook.react.bridge.NativeModule
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.uimanager.ViewManager

/**
 * Android entry point.
 *
 * The JS-facing API is installed directly into the runtime by the C++ (no
 * bridge, no TurboModule on the call path). This package exists for the one
 * thing only the Java side can provide: a chance to resolve the
 * [IntlPlatform] class and its method ids from a Java-initiated call, on a
 * thread whose classloader can see application classes.
 *
 * That ordering is load-bearing rather than incidental. `FindClass` uses the
 * calling thread's classloader, and the JS thread's is the system one, so a
 * `FindClass("com/intl/IntlPlatform")` issued from the JS thread simply does
 * not resolve — the classic "works in a unit test, NoClassDefFoundError on
 * device" JNI failure. Calling [IntlPlatform.attach] from here sidesteps it.
 *
 * It is also early enough. The module's own install() only defines an accessor
 * on `globalThis`; the platform has to be in place by the time that accessor is
 * first *read*, which is the first time application code touches `Intl`.
 */
class IntlPackage : ReactPackage {
  override fun createNativeModules(context: ReactApplicationContext): List<NativeModule> {
    IntlPlatform.attach(context.applicationContext)
    return emptyList()
  }

  override fun createViewManagers(context: ReactApplicationContext): List<ViewManager<*, *>> =
    emptyList()
}
