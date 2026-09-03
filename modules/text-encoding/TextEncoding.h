/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <jsi/jsi.h>

namespace qjs::textencoding {

/// Installs TextEncoder and TextDecoder as globals.
///
/// Requires a QuickJS-backed runtime: on anything else this is a no-op, and
/// whatever polyfill the bundle carries stays in place. Installing a slower
/// portable version would be worse than not installing at all.
void install(facebook::jsi::Runtime &runtime);

}  // namespace qjs::textencoding
