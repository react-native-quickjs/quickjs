/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <hermes/hermes.h>

#include <memory>

#ifndef INSPECTOR_EXPORT
#if defined(_MSC_VER)
#define INSPECTOR_EXPORT
#else
#define INSPECTOR_EXPORT __attribute__((visibility("default")))
#endif
#endif

namespace facebook::hermes::inspector_modern {

/// Owns a runtime for the debugger to attach to. Kept because worklets names
/// the type; see Registration.h for what attaching actually does here.
class INSPECTOR_EXPORT RuntimeAdapter {
 public:
  virtual ~RuntimeAdapter() = 0;
  virtual HermesRuntime &getRuntime() = 0;
  virtual void tickleJs();
};

class INSPECTOR_EXPORT SharedRuntimeAdapter : public RuntimeAdapter {
 public:
  explicit SharedRuntimeAdapter(std::shared_ptr<HermesRuntime> runtime);
  ~SharedRuntimeAdapter() override;
  HermesRuntime &getRuntime() override;

 private:
  std::shared_ptr<HermesRuntime> runtime_;
};

}  // namespace facebook::hermes::inspector_modern
