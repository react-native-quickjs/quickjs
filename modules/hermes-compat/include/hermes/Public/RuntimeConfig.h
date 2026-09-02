/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * The full field set, with Hermes's own defaults, so that code configuring the
 * VM compiles unchanged.
 *
 * Almost none of it has a QuickJS equivalent. Rather than drop settings
 * silently, makeHermesRuntime() compares the config it is handed against these
 * defaults and reports every field it cannot honour -- loudly for the ones
 * that change what JavaScript is allowed to do, such as EnableEval, and once
 * for tuning fields nothing observable depends on.
 */

#pragma once

#include <hermes/Public/CrashManager.h>
#include <hermes/Public/CtorConfig.h>
#include <hermes/Public/GCConfig.h>

#include <cstdint>
#include <string>

namespace hermes::vm {

enum CompilationMode {
  SmartCompilation,
  ForceEagerCompilation,
  ForceLazyCompilation
};

class PinnedHermesValue;

#define RUNTIME_FIELDS(F)                                    \
  F(HERMES_NON_CONSTEXPR, vm::GCConfig, GCConfig)            \
  F(constexpr, PinnedHermesValue *, RegisterStack, nullptr)  \
  F(constexpr, unsigned, MaxNumRegisters, 1024 * 1024)       \
  F(constexpr, bool, EnableJIT, false)                       \
  F(constexpr, bool, EnableEval, true)                       \
  F(constexpr, bool, VerifyEvalIR, false)                    \
  F(constexpr, bool, OptimizedEval, false)                   \
  F(constexpr, bool, AsyncBreakCheckInEval, false)           \
  F(constexpr, bool, ES6Promise, true)                       \
  F(constexpr, bool, ES6Proxy, true)                         \
  F(constexpr, bool, Intl, true)                             \
  F(constexpr, bool, TraceEnabled, false)                    \
  F(HERMES_NON_CONSTEXPR, std::string, TraceScratchPath, "") \
  F(HERMES_NON_CONSTEXPR, std::string, TraceResultPath, "")  \
  F(constexpr, bool, EnableSampledStats, false)              \
  F(constexpr, bool, EnableSampleProfiling, true)            \
  F(constexpr, bool, RandomizeMemoryLayout, false)           \
  F(constexpr, unsigned, BytecodeWarmupPercent, 0)           \
  F(constexpr, bool, TrackIO, false)                         \
  F(constexpr, bool, EnableHermesInternal, true)             \
  F(constexpr, bool, EnableHermesInternalTestMethods, false) \
  F(constexpr, bool, EnableGenerator, true)                  \
  F(constexpr, bool, MicrotaskQueue, false)                  \
  F(constexpr, uint32_t, VMExperimentFlags, 0)

_HERMES_CTORCONFIG_STRUCT(RuntimeConfig, RUNTIME_FIELDS, {});

}  // namespace hermes::vm
