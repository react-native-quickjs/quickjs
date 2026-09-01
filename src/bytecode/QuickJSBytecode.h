/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace qjs {

/**
 * Precompiled bytecode container, written by tools/bytecode/qjsc-ng.c.
 *
 *     [8 bytes magic "NSBCNGS\0"][4 bytes format version, little-endian]
 *     [JS_WriteObject payload]
 *
 * A JS_WriteObject payload has no strong leading magic of its own, so it is
 * wrapped to make detection unambiguous. The magic is specific to quickjs-ng;
 * bytecode from Bellard's quickjs is not interchangeable.
 */
inline constexpr char kBytecodeMagic[8] = {'N', 'S', 'B', 'C',
                                           'N', 'G', 'S', '\0'};
inline constexpr size_t kBytecodeMagicSize = sizeof(kBytecodeMagic);
inline constexpr size_t kBytecodeHeaderSize = kBytecodeMagicSize + 4;
inline constexpr uint32_t kBytecodeFormatVersion = 1;

/// True if `data` opens a container this runtime can execute. An unrecognised
/// format version reads as false, so a stale artifact is treated as source
/// rather than mis-parsed as bytecode.
bool isBytecodeContainer(const uint8_t *data, size_t size);

/// Only meaningful when `size >= kBytecodeHeaderSize`.
uint32_t bytecodeFormatVersion(const uint8_t *data);

/**
 * True if `data` is a Hermes bytecode bundle.
 *
 * Detected only to name it. An app that sets `hermesEnabled=true` in Gradle to
 * satisfy a library that expects Hermes also makes `BundleHermesCTask` run
 * `hermesc`, so the shipped bundle becomes Hermes bytecode. Unrecognised, it
 * falls through to the parser as binary and surfaces as an unintelligible
 * syntax error at byte zero.
 */
bool isHermesBytecode(const uint8_t *data, size_t size);

}  // namespace qjs
