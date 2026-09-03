/*
 * The platform seam for react-native-quickjs-text-encoding.
 *
 * The shared C++ in this directory is platform-independent. Anything that needs
 * NSFoundation on Apple or Android APIs on Android goes behind this interface,
 * with one implementation per platform:
 *
 *   ios/TextEncodingPlatform.mm              Objective-C++
 *   android/src/main/cpp/TextEncodingPlatform.cpp  JNI -> Kotlin
 *
 * This is how Hermes implements Intl, and it is worth copying: NSLocale and
 * android.icu already ship on every device, so a module can have real
 * locale, crypto or filesystem behaviour without linking ICU or any other
 * multi-megabyte dependency.
 *
 * Platform calls are expensive relative to the C++ around them — a JNI crossing
 * especially — so keep this interface coarse. One call returning a batch beats
 * many calls returning scalars.
 */

#pragma once

#include <string>

namespace text_encoding {

class Platform {
 public:
  virtual ~Platform() = default;

  /// Example. Replace with what your module actually needs.
  virtual std::string deviceLocale() = 0;
};

/// Installed by the platform layer during startup, before any JavaScript runs.
void setPlatform(Platform *platform);

/// May return nullptr if the platform layer has not been linked in — for
/// example in a host-side unit test. Always check.
Platform *platform();

}  // namespace text_encoding
