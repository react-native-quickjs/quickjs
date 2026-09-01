/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <jsi/jsi.h>

#include <memory>

#include "QuickJSRuntimeConfig.h"

namespace qjs {

/// Creates a QuickJS-backed jsi::Runtime with the compatibility shims and every
/// registered module installed. The only entry point a non-React consumer
/// needs.
std::unique_ptr<facebook::jsi::Runtime> makeQuickJSRuntime(
    QuickJSRuntimeConfig config = {});

}  // namespace qjs
