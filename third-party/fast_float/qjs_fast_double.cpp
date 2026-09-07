// The vendored fast_float header is dual-licensed under the MIT, Apache 2.0,
// and Boost 1.0 licenses. See fast_float/fast_float.h for the full notices.

#include <cstddef>
#include <system_error>

#include "fast_float/fast_float.h"

extern "C" int qjs_fast_strtod(
    const char *first, const char *last, double *out) {
  fast_float::parse_options_t<char> options;
  options.format = fast_float::chars_format::general;
  auto result = fast_float::from_chars_advanced(first, last, *out, options);
  if (result.ec == std::errc::invalid_argument || result.ptr != last) return 0;
  return 1;
}
