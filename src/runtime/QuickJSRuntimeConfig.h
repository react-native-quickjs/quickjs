/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstddef>
#include <string>

namespace qjs {

struct QuickJSRuntimeConfig {
  /// Where compiled bytecode is cached between launches. Empty disables it.
  std::string codeCacheDir;

  /// Hard cap on the JS heap in bytes. 0 means unlimited.
  size_t memoryLimit{0};

  /// JS stack size in bytes. 0 uses the quickjs default.
  size_t stackSize{0};

  /// Installs the `HermesInternal` object React Native probes for. Without it
  /// React Native's `hasPromise()` check fails and every app replaces the
  /// native Promise with a setImmediate polyfill, at 1205 ns per construct and
  /// chain against 775 ns native.
  bool reactNativeCompat{true};

  /// Must be decided before the bundle is parsed: statement traps are emitted
  /// at parse time, so a runtime created with this off cannot be attached to
  /// later. A frontend would connect, accept breakpoints and never stop.
  bool enableDebugger{false};

  /// Skips a collection triggered while JS is on the stack and runs it at the
  /// next safepoint instead. Only cycles wait -- acyclic garbage still dies at
  /// its last decref. Measured on a list-rebuilding workload: worst frame
  /// 45.07 ms -> 3.33 ms, frames over budget 68/300 -> 0.
  bool deferGC{true};

  /// Heap size past which a collection runs wherever execution happens to be.
  /// 0 derives it from the live heap, re-derived after every collection.
  ///
  /// The engine's own default is `malloc_gc_threshold * 4` fixed at
  /// construction time, which is 1.00 MiB forever. Deferral is gated on
  /// `live + size < ceiling`, and React Native's entry cascade alone peaks at
  /// 1.557 MB, so that default stops engaging before an app finishes starting.
  size_t deferredGCLimit{0};

  /// The derived ceiling is `live + clamp(live, minSlack, maxSlack)`. Additive
  /// rather than a multiple, which at a 69 MB heap would give 1.1 GB.
  ///
  /// What it bounds is garbage accumulated while deferred, so the rule is
  /// `minSlack > 2 x heap growth between quiet windows` -- measured at 48.4 MB
  /// on the example app with a 50,000-row list. Peak heap can reach
  /// `live + slack`, so lowering these trades memory for frame drops.
  size_t deferredGCMinSlack{48 * 1024 * 1024};
  size_t deferredGCMaxSlack{256 * 1024 * 1024};

  /// How far the GC threshold may drift above the live heap before it is pulled
  /// back down, as a multiple of it. 0 disables the retune.
  ///
  /// The engine only recomputes the threshold at a collection, so refcounting
  /// can shrink the heap far below it and nothing lowers it again. Measured on
  /// device: frozen at 144 MB against a 63 MB heap, two collections in 20
  /// cycles. The engine targets 1.5x, so 3.0 leaves a wide dead band.
  double gcThresholdRetuneRatio{3.0};

  /// Heap growth allowed to the first top-level script before the collector may
  /// run during it. 0 disables the bracket.
  ///
  /// Module instantiation builds a graph and keeps it, so the collector finds
  /// 14 collectable objects across React Native's whole entry cascade and fires
  /// three times before any of them exist -- 772 us freeing nothing. Bounded,
  /// so a larger script resumes normal collection by itself.
  size_t startupGCBracketBytes{8 * 1024 * 1024};

  /// How long a gap before the current task counts as the app going quiet.
  ///
  /// An empty JS stack is not idleness: during a gesture native drives JS every
  /// frame, so collecting at the first safepoint just moves the pause between
  /// two frames of the same gesture. Measured, a scroll took 74 collections at
  /// 0 and 1 at anything from 16 ms up, so this only has to clear the frame
  /// cadence by enough that one slow frame does not read as quiet.
  size_t gcIdleGapMs{120};

  /// The longest a collection may stay pending before the next safepoint takes
  /// it regardless. 0 disables.
  ///
  /// Idleness alone never fires for an app with a ticker. Measured with a 16 ms
  /// setInterval: one collection pending across thousands of safepoints, zero
  /// idle drains, and every collection that ran was forced by the pressure
  /// valve -- mid-gesture, which is what deferral exists to avoid.
  size_t gcMaxDeferralMs{2000};

  /// Heap size past which the idle requirement is dropped, though the
  /// collection still waits for a safepoint. Sits below deferredGCLimit. 0
  /// derives it from the slack.
  size_t gcPressureBytes{0};
};

}  // namespace qjs
