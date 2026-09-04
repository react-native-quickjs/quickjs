/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "QuickJSModule.h"

#include <algorithm>
#include <cstring>

#include "QuickJSRuntime.h"

namespace qjs {

namespace {

// Function-local rather than namespace-scope, so registration from another
// translation unit's static initializer cannot run before the vector is
// constructed. QJS_REGISTER_MODULE exists to be used from other translation
// units, and initialization order across them is unspecified.
std::vector<ModuleRegistration> &registry() {
  static std::vector<ModuleRegistration> modules;
  return modules;
}

}  // namespace

// Weak default: linking quickjs_jsi without the generated registry TU still
// links, and a target that compiles that TU overrides this (clang/gcc).
#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak)) void registerGeneratedModules() {}
#else
void registerGeneratedModules() {}
#endif

void registerModule(const ModuleRegistration &module) {
  if (module.install == nullptr || module.name == nullptr) {
    return;
  }

  auto &modules = registry();
  for (const auto &existing : modules) {
    if (std::strcmp(existing.name, module.name) == 0) {
      return;
    }
  }
  modules.push_back(module);

  // stable_sort so equal priorities keep registration order, which is what a
  // module author leaving priority at its default would expect.
  std::stable_sort(
      modules.begin(), modules.end(),
      [](const ModuleRegistration &a, const ModuleRegistration &b) {
        return a.priority < b.priority;
      });
}

const std::vector<ModuleRegistration> &registeredModules() {
  return registry();
}

void installModules(jsi::Runtime &runtime) {
  registerGeneratedModules();
  for (const auto &module : registry()) {
    module.install(runtime);
  }
}

JSContext *contextFromRuntime(jsi::Runtime &runtime) noexcept {
  // dynamic_cast rather than a type tag: the reference may be any
  // implementation, including a decorator around ours, and guessing wrong
  // would corrupt memory rather than fail cleanly.
  auto *quickjs = dynamic_cast<QuickJSRuntime *>(&runtime);
  return quickjs != nullptr ? quickjs->context() : nullptr;
}

}  // namespace qjs
