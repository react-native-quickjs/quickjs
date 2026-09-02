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

  virtual ~CrashManager() = default;
  virtual void registerMemory(void *, size_t) {}
  virtual void unregisterMemory(void *) {}
  virtual void setCustomData(const char *, const char *) {}
  virtual void removeCustomData(const char *) {}
  virtual CallbackKey registerCallback(CallbackFunc) {
    return 0;
  }
  virtual void unregisterCallback(CallbackKey) {}
};

/// react-native-worklets constructs one of these, so the class needs a vtable
/// in this library. The destructor is deliberately out of line: it is the key
/// function, and that is what decides where the vtable is emitted.
class NopCrashManager : public CrashManager {
 public:
  ~NopCrashManager() override;
};

}  // namespace hermes::vm
