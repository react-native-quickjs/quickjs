/*
 * react-native-quickjs-text-encoding
 */

#pragma once

#include <jsi/jsi.h>

namespace rnqjs::textencoding {

/// Installs TextEncoder and TextDecoder as globals.
///
/// Requires a QuickJS-backed runtime: on anything else this is a no-op, and
/// whatever polyfill the bundle carries stays in place. Installing a slower
/// portable version would be worse than not installing at all.
void install(facebook::jsi::Runtime &runtime);

}  // namespace rnqjs::textencoding

extern "C" void textEncoding_install(facebook::jsi::Runtime &runtime);
