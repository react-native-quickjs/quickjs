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

// Weak default so anything that links quickjs_jsi without the generated
// registry TU still links and runs. An app that compiles the generated TU
// (which defines this strongly) gets that definition, exactly like the weak
// Swift symbols in modules/intl/ios/IntlPlatform.mm. clang and gcc both
// support this; MSVC does not, but no app or test target links quickjs_jsi on
// Windows (only the standalone qjsc bytecode compiler is built there).
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
  // The generated registry first, so a module's object file is force-referenced
  // (and registered) before any runtime installs it. See registerGeneratedModules.
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
