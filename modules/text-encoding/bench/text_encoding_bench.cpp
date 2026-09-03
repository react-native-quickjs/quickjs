/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * The native TextEncoder and TextDecoder against the JavaScript polyfill they
 * replace, both measured in the same runtime on the same inputs.
 *
 * The loop is in JavaScript rather than C++ on purpose: what an app pays is a
 * call from its own code, so timing from outside the engine would add a
 * crossing that neither implementation actually has.
 */

#include <cstdio>
#include <memory>
#include <string>

#include "QuickJSRuntimeFactory.h"

extern "C" void textEncoding_install(facebook::jsi::Runtime &runtime);

namespace jsi = facebook::jsi;

namespace {

// A representative polyfill: charCodeAt over the string, pushing bytes into an
// array, which is what the shims in the wild do.
const char *kBenchmark = R"JS(
(function () {
  'use strict';

  function encodePolyfill(str) {
    const out = [];
    for (let i = 0; i < str.length; i++) {
      let c = str.charCodeAt(i);
      if (c < 0x80) {
        out.push(c);
      } else if (c < 0x800) {
        out.push(0xc0 | (c >> 6), 0x80 | (c & 63));
      } else if (c >= 0xd800 && c <= 0xdbff && i + 1 < str.length) {
        const c2 = str.charCodeAt(i + 1);
        const cp = 0x10000 + ((c - 0xd800) << 10) + (c2 - 0xdc00);
        out.push(
          0xf0 | (cp >> 18), 0x80 | ((cp >> 12) & 63),
          0x80 | ((cp >> 6) & 63), 0x80 | (cp & 63));
        i++;
      } else {
        out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
      }
    }
    return new Uint8Array(out);
  }

  function decodePolyfill(bytes) {
    let out = '';
    for (let i = 0; i < bytes.length; ) {
      const c = bytes[i];
      if (c < 0x80) {
        out += String.fromCharCode(c);
        i += 1;
      } else if (c < 0xe0) {
        out += String.fromCharCode(((c & 31) << 6) | (bytes[i + 1] & 63));
        i += 2;
      } else if (c < 0xf0) {
        out += String.fromCharCode(
          ((c & 15) << 12) | ((bytes[i + 1] & 63) << 6) | (bytes[i + 2] & 63));
        i += 3;
      } else {
        const cp = ((c & 7) << 18) | ((bytes[i + 1] & 63) << 12) |
          ((bytes[i + 2] & 63) << 6) | (bytes[i + 3] & 63);
        out += String.fromCodePoint(cp);
        i += 4;
      }
    }
    return out;
  }

  const encoder = new TextEncoder();
  const decoder = new TextDecoder();

  const short = 'user_session_id';
  const ascii = 'x'.repeat(4096);
  const mixed = 'héllo wörld \u{1f600} '.repeat(64);

  const asciiBytes = encoder.encode(ascii);
  const mixedBytes = encoder.encode(mixed);
  const shortBytes = encoder.encode(short);

  /*
   * Date.now() is millisecond resolution, so the iteration count is calibrated
   * until a pass lasts long enough for that to be noise rather than the
   * measurement: at 2 ms per pass the answer quantizes to a few values and
   * comes out suspiciously round.
   */
  function calibrate(body) {
    let iterations = 64;
    for (;;) {
      const start = Date.now();
      for (let i = 0; i < iterations; i++) body();
      const elapsed = Date.now() - start;
      if (elapsed >= 150 || iterations >= (1 << 26)) return iterations;
      iterations *= elapsed < 10 ? 8 : 2;
    }
  }

  // Median of several passes: one descheduled pass should not set the number.
  function nsPerOp(body) {
    const iterations = calibrate(body);
    const passes = [];
    for (let pass = 0; pass < 5; pass++) {
      const start = Date.now();
      for (let i = 0; i < iterations; i++) body();
      passes.push(((Date.now() - start) * 1e6) / iterations);
    }
    passes.sort((a, b) => a - b);
    return passes[2];
  }

  const cases = [
    ['encode 15 B ascii',
      () => encoder.encode(short), () => encodePolyfill(short)],
    ['encode 4 KB ascii',
      () => encoder.encode(ascii), () => encodePolyfill(ascii)],
    ['encode 1.2 KB mixed',
      () => encoder.encode(mixed), () => encodePolyfill(mixed)],
    ['decode 15 B ascii',
      () => decoder.decode(shortBytes), () => decodePolyfill(shortBytes)],
    ['decode 4 KB ascii',
      () => decoder.decode(asciiBytes), () => decodePolyfill(asciiBytes)],
    ['decode 1.2 KB mixed',
      () => decoder.decode(mixedBytes), () => decodePolyfill(mixedBytes)],
  ];

  // Warm up both paths so neither pays for a cold shape or a first allocation.
  for (const [, native, polyfill] of cases) {
    for (let i = 0; i < 200; i++) { native(); polyfill(); }
  }

  let out = '';
  for (const [name, native, polyfill] of cases) {
    const n = nsPerOp(native);
    const p = nsPerOp(polyfill);
    out += name + '|' + n.toFixed(0) + '|' + p.toFixed(0) + '|' +
      (p / n).toFixed(1) + '\n';
  }
  return out;
})()
)JS";

}  // namespace

int main() {
#if !QJS_BENCH_OPTIMIZED
  printf(
      "text-encoding bench: skipped, the engine was not built optimized.\n"
      "Configure with -DCMAKE_BUILD_TYPE=RelWithDebInfo to measure.\n");
  return 0;
#else
  auto runtime = qjs::makeQuickJSRuntime();
  textEncoding_install(*runtime);

  const std::string rows =
      runtime
          ->evaluateJavaScript(
              std::make_shared<jsi::StringBuffer>(kBenchmark), "bench.js")
          .asString(*runtime)
          .utf8(*runtime);

  printf("\n%-22s %12s %12s %8s\n", "case", "native", "polyfill", "speedup");
  printf("%-22s %12s %12s %8s\n", "----", "------", "--------", "-------");
  size_t start = 0;
  while (start < rows.size()) {
    const size_t end = rows.find('\n', start);
    const std::string line = rows.substr(start, end - start);
    start = end == std::string::npos ? rows.size() : end + 1;
    if (line.empty()) continue;

    const size_t a = line.find('|');
    const size_t b = line.find('|', a + 1);
    const size_t c = line.find('|', b + 1);
    printf(
        "%-22s %9s ns %9s ns %7sx\n", line.substr(0, a).c_str(),
        line.substr(a + 1, b - a - 1).c_str(),
        line.substr(b + 1, c - b - 1).c_str(), line.substr(c + 1).c_str());
  }
  printf("\n");
  return 0;
#endif
}
