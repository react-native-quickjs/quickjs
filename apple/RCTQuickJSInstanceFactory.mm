/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#import "RCTQuickJSInstanceFactory.h"

#import "QuickJSInstance.h"

using namespace facebook::react;

JSRuntimeFactoryRef jsrt_create_quickjs_factory(void) {
  return reinterpret_cast<JSRuntimeFactoryRef>(new QuickJSInstance());
}
