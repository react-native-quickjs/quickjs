/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * Where React Native's autolinking should look for this package's native code.
 *
 * It defaults to `android/` at the package root, and ours is under `platform/`.
 * Without this file autolinking finds nothing, the gradle project is never
 * added to the app, and the app silently keeps running on whichever engine it
 * had -- a build that succeeds and an app that works, just not on QuickJS.
 *
 * iOS needs no entry: CocoaPods finds the podspec at the package root.
 */

module.exports = {
  dependency: {
    platforms: {
      android: {
        sourceDir: 'platform/android',
      },
    },
  },
};
