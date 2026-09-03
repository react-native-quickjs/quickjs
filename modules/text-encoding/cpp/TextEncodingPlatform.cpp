/*
 * Shared half of the platform seam for react-native-quickjs-text-encoding.
 *
 * Holds the pointer the platform layer installs. Deliberately not a
 * std::unique_ptr: the implementations are static objects owned by their own
 * translation units, registered before main in the Apple case, so this must not
 * take ownership of them.
 */

#include "TextEncodingPlatform.h"

namespace text_encoding {

namespace {
Platform *g_platform = nullptr;
}

void setPlatform(Platform *p) {
  g_platform = p;
}

Platform *platform() {
  // Null on a host-side unit test, where neither platform layer is linked.
  // Callers must handle it rather than assume a platform exists.
  return g_platform;
}

}  // namespace text_encoding
