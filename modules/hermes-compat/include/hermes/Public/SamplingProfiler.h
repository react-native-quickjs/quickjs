/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <hermes/Public/HermesExport.h>

namespace facebook::hermes::sampling_profiler {

// Named so that IHermes::dumpSampledTraceToProfile has a complete return type.
// QuickJS has no sampling profiler, so a caller that reads a Profile does not
// compile -- which is the right answer, and happens at build time.
class HERMES_EXPORT Profile {
 public:
  Profile() = default;
};

}  // namespace facebook::hermes::sampling_profiler
