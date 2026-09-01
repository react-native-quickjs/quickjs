/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <jsi/jsi.h>

namespace qjs {

/// Installs the `HermesInternal` object React Native probes for. See the
/// implementation for what it costs to omit and why claiming the name is safe.
/// Controlled by QuickJSRuntimeConfig::reactNativeCompat.
void installReactNativeCompat(facebook::jsi::Runtime &runtime);

}  // namespace qjs
