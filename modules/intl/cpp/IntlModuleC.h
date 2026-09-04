/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef RNQJS_INTL_MODULE_C_H
#define RNQJS_INTL_MODULE_C_H

/*
 * The C entry point, for embedders that have a JSContext and no JSI.
 *
 * `tools/intl-cli`, the test262 runner and the differential harness all build
 * the module without React Native in the link. `IntlModule.h` is the C++/JSI
 * face of the same thing and is what an app uses.
 */

struct JSContext;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Installs the lazy `globalThis.Intl` accessor into `ctx`.
 *
 * Call once per context, before any application JavaScript. Costs one accessor
 * property; the ECMA-402 implementation is not loaded until `Intl` is first
 * read. Safe to call with NULL (does nothing).
 */
void rnqjs_intl_install(struct JSContext *ctx);

/**
 * Writes the instrumentation counters to stdout, or does nothing when the
 * module was built without -DRNQJS_INTL_COUNTERS=1.
 *
 * The counter that matters is `intl.sourceLoads`. It must be **zero** in any
 * build that quotes the precompiled-bytecode startup number; non-zero means the
 * build-time compile step did not run and the number is fiction. This project
 * has shipped a fast path with zero hits before, so "the blob is being used" is
 * a counted fact here rather than an assumption.
 */
void rnqjs_intl_dump_counters(void);

#ifdef __cplusplus
} /* extern "C" */

#include <cstdio>

namespace rnqjs::intl {

/** The same as rnqjs_intl_install, in the module's namespace. */
void installInto(JSContext *ctx);

#if defined(RNQJS_INTL_COUNTERS) && RNQJS_INTL_COUNTERS
/**
 * Writes the instrumentation counters.
 *
 * Compiled only under -DRNQJS_INTL_COUNTERS=1 and costs nothing when off. The
 * counter that matters is `intl.sourceLoads`: it must be zero in any build that
 * quotes the precompiled-bytecode startup number, and non-zero means the
 * build-time compile step did not run and the number is fiction.
 */
void dumpCounters(std::FILE *out);
#endif

}  // namespace rnqjs::intl
#endif

#endif /* RNQJS_INTL_MODULE_C_H */
