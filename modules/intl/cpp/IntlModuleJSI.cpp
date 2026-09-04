/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * The JSI-facing edge of the Intl module — and the *only* file in it that
 * includes a JSI header.
 *
 * Everything else (IntlModule.cpp, the backends, the JS layer) talks to the
 * QuickJS C API and to the platform, and nothing else. That separation is what
 * lets the host tools link the whole implementation without React Native in the
 * build: `tools/intl-cli` and the test262 runner build IntlModule.cpp and a
 * backend and get a complete Intl, while an app additionally builds this file
 * and gets the module registration.
 *
 * It also means the engine-facing code cannot accidentally acquire a
 * dependency on jsi::Runtime's lifetime rules, which is the thing that would
 * make the module's threading contract subtle. There is nothing to get wrong
 * here: install() runs once, on the JS thread, during runtime construction.
 */

#include <jsi/jsi.h>

#include "IntlModule.h"
#include "QuickJSModule.h"
#include "QuickJSModuleNative.h"

namespace rnqjs::intl {

void install(jsi::Runtime &rt) {
  /*
   * This module requires the engine escape hatch. On a foreign jsi::Runtime it
   * installs nothing rather than installing something that cannot work — JSI
   * has no way to make a real constructor and no way to load bytecode, so a
   * portable fallback would be a worse Intl than the polyfill the bundle
   * already carries, and it would suppress that polyfill by existing.
   */
  installInto(qjs::contextFromRuntime(rt));
}

}  // namespace rnqjs::intl

/*
 * The symbol the generated module registry calls by name.
 *
 * Both registration routes exist on purpose. The static initializer below works
 * for a shared library; this explicit entry point is what survives a
 * static-library link, where the linker is entitled to drop an object file
 * nothing references — leaving the module compiled, linked, and silently
 * installing nothing. Registration is deduplicated by name, so a module reached
 * by both routes installs exactly once.
 */
extern "C" void intl_install(facebook::jsi::Runtime &runtime) {
  rnqjs::intl::install(runtime);
}

QJS_REGISTER_MODULE("react-native-quickjs-intl", rnqjs::intl::install)
