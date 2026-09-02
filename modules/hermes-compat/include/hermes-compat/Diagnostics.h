/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <functional>

namespace qjs::hermescompat {

enum class Severity : uint8_t {
  Degraded,     ///< Ran, but not the way Hermes would have.
  Ignored,      ///< Did nothing, and nothing observable depended on it.
  Unsupported,  ///< Could not be honoured; the result is a safe stand-in.
};

using Handler =
    std::function<void(Severity, const char *api, const char *detail)>;

/// Replaces the default handler, which writes to stderr, or to logcat on
/// Android. Nothing aborts: how strict to be is the embedder's call.
void setHandler(Handler handler);

/// Reports the first time each `api` is seen, so a call in a loop says it
/// once. Called by the shim; `api` is always a literal.
void report(Severity severity, const char *api, const char *detail);

/// Forgets which APIs have reported, so the next call to each reports again.
void resetForTesting();

}  // namespace qjs::hermescompat
