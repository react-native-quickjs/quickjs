/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.ammarahmed.quickjs

import com.facebook.react.ReactPackage
import com.facebook.react.bridge.NativeModule
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.uimanager.ViewManager

/**
 * Deliberately empty.
 *
 * This package exports no modules and no view managers -- QuickJS is wired in
 * through [QuickJSInstance], not through the module system. It exists only
 * because React Native's autolinking skips any Android library that does not
 * expose a ReactPackage (see cli-config-android dependencyConfig), and without
 * autolinking the gradle project is never included in the app build.
 */
public class QuickJSPackage : ReactPackage {
  override fun createNativeModules(
      reactContext: ReactApplicationContext
  ): List<NativeModule> = emptyList()

  override fun createViewManagers(
      reactContext: ReactApplicationContext
  ): List<ViewManager<*, *>> = emptyList()
}
