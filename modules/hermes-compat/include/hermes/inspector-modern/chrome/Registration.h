/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#ifdef HERMES_ENABLE_DEBUGGER

#include <hermes/hermes.h>
#include <hermes/inspector/RuntimeAdapter.h>

#include <memory>
#include <string>

namespace facebook::hermes::inspector_modern::chrome {

using DebugSessionToken = int;

/// Defined so debug builds of worklets link. It does not register a target:
/// the app's main runtime is debuggable through jsinspector-modern and
/// modules/cdp, a runtime created through this shim is not.
extern DebugSessionToken enableDebugging(
    std::unique_ptr<RuntimeAdapter> adapter, const std::string &title);
extern void disableDebugging(DebugSessionToken session);

}  // namespace facebook::hermes::inspector_modern::chrome

#endif  // HERMES_ENABLE_DEBUGGER
