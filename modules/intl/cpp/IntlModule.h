/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <jsi/jsi.h>

#include "IntlModuleC.h"

namespace rnqjs::intl {

namespace jsi = facebook::jsi;

/**
 * Installs the lazy `globalThis.Intl` accessor.
 *
 * Called once per runtime, before any application JavaScript. Installs one
 * accessor property and nothing else; the ECMA-402 implementation is not
 * loaded until `Intl` is first read.
 *
 * On a jsi::Runtime that is not QuickJS-backed this does nothing. There is no
 * portable JSI fallback and there should not be one: JSI cannot create a real
 * constructor, so any fallback would be a worse `Intl` than the polyfill the
 * bundle already carries.
 */
void install(jsi::Runtime &rt);

}  // namespace rnqjs::intl

extern "C" void intl_install(facebook::jsi::Runtime &runtime);
