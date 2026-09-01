/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "QuickJSBytecode.h"

#include <cstring>

namespace qjs {

uint32_t bytecodeFormatVersion(const uint8_t *data) {
  const uint8_t *version = data + kBytecodeMagicSize;
  return static_cast<uint32_t>(version[0]) |
         (static_cast<uint32_t>(version[1]) << 8) |
         (static_cast<uint32_t>(version[2]) << 16) |
         (static_cast<uint32_t>(version[3]) << 24);
}

bool isBytecodeContainer(const uint8_t *data, size_t size) {
  if (data == nullptr || size < kBytecodeHeaderSize) {
    return false;
  }
  if (std::memcmp(data, kBytecodeMagic, kBytecodeMagicSize) != 0) {
    return false;
  }
  return bytecodeFormatVersion(data) == kBytecodeFormatVersion;
}

bool isHermesBytecode(const uint8_t *data, size_t size) {
  // hermes::hbc::MAGIC, from BCGen/HBC/BytecodeFileFormat.h. Read a byte at a
  // time: the buffer has no alignment guarantee.
  static constexpr uint64_t kHermesMagic = 0x1F1903C103BC1FC6ULL;

  if (data == nullptr || size < sizeof(uint64_t)) {
    return false;
  }

  uint64_t magic = 0;
  for (size_t i = 0; i < sizeof(uint64_t); i++) {
    magic |= static_cast<uint64_t>(data[i]) << (8 * i);
  }

  // ~MAGIC marks a delta bundle, equally unexecutable here.
  return magic == kHermesMagic || magic == ~kHermesMagic;
}

}  // namespace qjs
