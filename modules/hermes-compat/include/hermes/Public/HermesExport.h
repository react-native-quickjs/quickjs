/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

// The shim is always compiled into the app, so there is no import case.
#ifndef HERMES_EXPORT
#if defined(_MSC_VER)
#define HERMES_EXPORT
#else
#define HERMES_EXPORT __attribute__((visibility("default")))
#endif
#endif
