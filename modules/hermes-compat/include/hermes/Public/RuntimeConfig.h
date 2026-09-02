/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

namespace hermes::vm {

// Empty on purpose. Hermes's version carries about forty fields, several of
// them security settings such as withEnableEval(false), and QuickJS has no
// equivalent for most. Accepting and dropping them is the silent divergence
// this shim exists to prevent, so only the default constructor is offered:
// makeHermesRuntime() compiles, while anything that configures the VM fails to
// compile with the field named. Add a field only when a real consumer needs
// it, and wire that one to QuickJS.
class RuntimeConfig {
 public:
  RuntimeConfig() = default;
};

}  // namespace hermes::vm
