/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#import <react/runtime/JSRuntimeFactoryCAPI.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Creates the QuickJS engine factory, as a drop-in replacement for
 * `jsrt_create_hermes_factory()`.
 *
 * Override `createJSRuntimeFactory` on your React Native factory delegate:
 *
 *   - (JSRuntimeFactoryRef)createJSRuntimeFactory
 *   {
 *     return jsrt_create_quickjs_factory();
 *   }
 *
 * Ownership of the returned factory transfers to React Native, which releases
 * it with `js_runtime_factory_destroy`.
 */
JSRuntimeFactoryRef jsrt_create_quickjs_factory(void);

#ifdef __cplusplus
}
#endif
