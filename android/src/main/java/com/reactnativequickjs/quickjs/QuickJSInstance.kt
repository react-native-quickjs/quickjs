/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.reactnativequickjs.quickjs

import com.facebook.jni.HybridData
import com.facebook.jni.annotations.DoNotStrip
import com.facebook.react.runtime.JSRuntimeFactory
import com.facebook.soloader.SoLoader

/**
 * The QuickJS engine, as a drop-in replacement for
 * `com.facebook.react.runtime.hermes.HermesInstance`.
 *
 * Pass it to `DefaultReactHost.getDefaultReactHost(...)` in your
 * `MainApplication`:
 * ```
 * override val reactHost: ReactHost
 *   get() = getDefaultReactHost(
 *     applicationContext,
 *     reactNativeHost,
 *     jsRuntimeFactory = QuickJSInstance(),
 *   )
 * ```
 */
public class QuickJSInstance : JSRuntimeFactory(initHybrid()) {

  public companion object {
    @JvmStatic @DoNotStrip protected external fun initHybrid(): HybridData

    init {
      SoLoader.loadLibrary("quickjsinstancejni")
    }
  }
}
