/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <hermes/Public/CtorConfig.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>

namespace hermes::vm {

using gcheapsize_t = uint32_t;

enum ReleaseUnused {
  kReleaseUnusedNone = 0,
  kReleaseUnusedOld,
  kReleaseUnusedYoungOnFull,
  kReleaseUnusedYoungAlways
};

enum class GCEventKind {
  CollectionStart,
  CollectionEnd,
};

/// Hermes fills this in for its analytics callback. QuickJS never emits one,
/// so the fields exist to be named, not to be populated.
struct GCAnalyticsEvent {
  std::string runtimeDescription;
  std::string gcKind;
  std::string collectionType;
  std::string cause;
  std::chrono::milliseconds duration{0};
  std::chrono::milliseconds cpuDuration{0};
  uint64_t allocated{0};
  uint64_t size{0};
  uint64_t external{0};
  uint64_t survivalRatio{0};
};

#define GC_TRIPWIRE_FIELDS(F) \
  F(constexpr, gcheapsize_t, Limit, std::numeric_limits<gcheapsize_t>::max())

_HERMES_CTORCONFIG_STRUCT(GCTripwireConfig, GC_TRIPWIRE_FIELDS, {});

#define GC_HANDLESAN_FIELDS(F)            \
  F(constexpr, double, SanitizeRate, 0.0) \
  F(constexpr, int64_t, RandomSeed, -1)

_HERMES_CTORCONFIG_STRUCT(GCSanitizeConfig, GC_HANDLESAN_FIELDS, {});

#define GC_FIELDS(F)                                                      \
  F(constexpr, gcheapsize_t, MinHeapSize, 0)                              \
  F(constexpr, gcheapsize_t, InitHeapSize, 32 << 20)                      \
  F(constexpr, gcheapsize_t, MaxHeapSize, 3u << 30)                       \
  F(constexpr, double, OccupancyTarget, 0.5)                              \
  F(constexpr, unsigned, EffectiveOOMThreshold,                           \
    std::numeric_limits<unsigned>::max())                                 \
  F(constexpr, GCSanitizeConfig, SanitizeConfig)                          \
  F(constexpr, bool, ShouldRandomizeAllocSpace, false)                    \
  F(constexpr, bool, ShouldRecordStats, false)                            \
  F(constexpr, ReleaseUnused, ShouldReleaseUnused, kReleaseUnusedOld)     \
  F(HERMES_NON_CONSTEXPR, std::string, Name, "")                          \
  F(HERMES_NON_CONSTEXPR, GCTripwireConfig, TripwireConfig)               \
  F(constexpr, bool, AllocInYoung, true)                                  \
  F(constexpr, bool, RevertToYGAtTTI, false)                              \
  F(constexpr, bool, ProtectMetadata, false)                              \
  F(HERMES_NON_CONSTEXPR, std::function<void(const GCAnalyticsEvent &)>,  \
    AnalyticsCallback, nullptr)                                           \
  F(HERMES_NON_CONSTEXPR, std::function<void(GCEventKind, const char *)>, \
    Callback, nullptr)

_HERMES_CTORCONFIG_STRUCT(GCConfig, GC_FIELDS, {});

}  // namespace hermes::vm
