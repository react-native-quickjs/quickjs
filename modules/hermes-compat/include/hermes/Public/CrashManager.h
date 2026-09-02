/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace hermes::vm {

/// Named so that code passing a crash manager compiles. QuickJS installs no
/// crash handlers, so a registered callback is never invoked.
class CrashManager {
 public:
  using CallbackKey = int;
  using CallbackFunc = void (*)(int fd);

  /// Out of line, and so this class's key function: it is what places the
  /// vtable in this library. react-native-worklets references the base class
  /// vtable directly, not only NopCrashManager's, and with every virtual
  /// defined inline no translation unit here emits it -- the shim then loads
  /// on its own but fails dlopen the moment worklets links against it.
  virtual ~CrashManager();

  virtual void registerMemory(void *, size_t) {}
  virtual void unregisterMemory(void *) {}
  virtual void setCustomData(const char *, const char *) {}
  virtual void removeCustomData(const char *) {}
  virtual CallbackKey registerCallback(CallbackFunc) {
    return 0;
  }
  virtual void unregisterCallback(CallbackKey) {}
};

/// react-native-worklets constructs one of these, so this class needs its own
/// vtable here too, by the same rule.
class NopCrashManager : public CrashManager {
 public:
  ~NopCrashManager() override;
};

}  // namespace hermes::vm
