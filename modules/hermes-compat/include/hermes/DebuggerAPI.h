/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

namespace facebook::hermes::debugger {

// Named so that IHermes::getDebugger has a complete return type. Hermes's
// debugger is expressed in bytecode offsets and has no QuickJS meaning;
// debugging here goes through jsinspector-modern and modules/cdp instead.
class Debugger {
 public:
  Debugger() = default;
};

}  // namespace facebook::hermes::debugger
