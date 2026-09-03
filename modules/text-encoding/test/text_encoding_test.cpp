/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * TextEncoder and TextDecoder are specified by WHATWG Encoding, and the parts
 * that are easy to get wrong are the edge cases rather than the happy path:
 * surrogates, the BOM, truncated sequences, and what `fatal` changes.
 */

#include <gtest/gtest.h>

#include "QuickJSRuntimeFactory.h"

// The symbol a generated module registry calls by name. Naming it here is also
// what keeps the object file: a static library link is entitled to drop one
// nothing references, which is the failure this entry point exists to prevent.
extern "C" void textEncoding_install(facebook::jsi::Runtime &runtime);

namespace {

namespace jsi = facebook::jsi;

class TextEncodingTest : public ::testing::Test {
 protected:
  std::unique_ptr<jsi::Runtime> rt = makeRuntime();

  static std::unique_ptr<jsi::Runtime> makeRuntime() {
    auto runtime = qjs::makeQuickJSRuntime();
    textEncoding_install(*runtime);
    return runtime;
  }

  jsi::Value eval(const char *source) {
    return rt->evaluateJavaScript(
        std::make_shared<jsi::StringBuffer>(source), "test.js");
  }

  std::string str(const char *source) {
    return eval(source).asString(*rt).utf8(*rt);
  }

  double num(const char *source) {
    return eval(source).asNumber();
  }
};

TEST_F(TextEncodingTest, GlobalsAreInstalled) {
  EXPECT_EQ(str("typeof TextEncoder"), "function");
  EXPECT_EQ(str("typeof TextDecoder"), "function");
}

TEST_F(TextEncodingTest, EncodesAscii) {
  EXPECT_EQ(str("new TextEncoder().encode('abc').join(',')"), "97,98,99");
  EXPECT_EQ(num("new TextEncoder().encode('').length"), 0);
}

TEST_F(TextEncodingTest, EncodesMultiByteSequences) {
  // U+00E9, U+20AC and U+1F600: the two, three and four byte cases.
  EXPECT_EQ(str("new TextEncoder().encode('\\u00e9').join(',')"), "195,169");
  EXPECT_EQ(
      str("new TextEncoder().encode('\\u20ac').join(',')"), "226,130,172");
  EXPECT_EQ(
      str("new TextEncoder().encode('\\u{1f600}').join(',')"),
      "240,159,152,128");
}

TEST_F(TextEncodingTest, EncodesLoneSurrogateAsReplacement) {
  // Not an error: WHATWG Encoding says an unpaired surrogate encodes as U+FFFD.
  EXPECT_EQ(
      str("new TextEncoder().encode('\\ud800').join(',')"), "239,191,189");
}

TEST_F(TextEncodingTest, EncodingPropertyIsUtf8) {
  EXPECT_EQ(str("new TextEncoder().encoding"), "utf-8");
  EXPECT_EQ(str("new TextDecoder().encoding"), "utf-8");
}

TEST_F(TextEncodingTest, EncodeIntoReportsReadAndWritten) {
  EXPECT_EQ(
      str("(() => { const r = new TextEncoder().encodeInto('abc', new "
          "Uint8Array(8)); return r.read + '/' + r.written; })()"),
      "3/3");
  // A destination too small stops on a character boundary rather than writing
  // half a sequence.
  EXPECT_EQ(
      str("(() => { const r = new TextEncoder().encodeInto('\\u20ac', new "
          "Uint8Array(2)); return r.read + '/' + r.written; })()"),
      "0/0");
}

TEST_F(TextEncodingTest, DecodesUtf8) {
  EXPECT_EQ(str("new TextDecoder().decode(new Uint8Array([97,98,99]))"), "abc");
  EXPECT_EQ(
      str("new TextDecoder().decode(new Uint8Array([226,130,172]))"),
      "\xe2\x82\xac");
}

TEST_F(TextEncodingTest, DecodeRoundTripsThroughEncode) {
  EXPECT_EQ(
      str("(() => { const s = 'héllo \\u{1f600} world'; return new "
          "TextDecoder().decode(new TextEncoder().encode(s)) === s ? 'yes' : "
          "'no'; })()"),
      "yes");
}

TEST_F(TextEncodingTest, DecodesWithNoArgumentToEmptyString) {
  EXPECT_EQ(str("new TextDecoder().decode()"), "");
}

TEST_F(TextEncodingTest, StripsByteOrderMarkByDefault) {
  EXPECT_EQ(
      num("new TextDecoder().decode(new Uint8Array([239,187,191,97])).length"),
      1);
  EXPECT_EQ(
      num("new TextDecoder('utf-8', {ignoreBOM: true})"
          ".decode(new Uint8Array([239,187,191,97])).length"),
      2);
}

TEST_F(TextEncodingTest, ReplacesTruncatedSequences) {
  // A lead byte with its continuation missing is one replacement character.
  EXPECT_EQ(
      str("new TextDecoder().decode(new Uint8Array([226,130]))"),
      "\xef\xbf\xbd");
}

TEST_F(TextEncodingTest, FatalThrowsOnInvalidInput) {
  EXPECT_EQ(
      str("(() => { try { new TextDecoder('utf-8', {fatal: true})"
          ".decode(new Uint8Array([226,130])); return 'no throw'; } "
          "catch (e) { return e.constructor.name; } })()"),
      "TypeError");
}

TEST_F(TextEncodingTest, DecodesFromArrayBufferAndOffsetView) {
  EXPECT_EQ(
      str("new TextDecoder().decode(new Uint8Array([97,98,99]).buffer)"),
      "abc");
  // A view with a non-zero offset must decode its own window, not the buffer.
  EXPECT_EQ(
      str("(() => { const b = new Uint8Array([97,98,99,100]); return new "
          "TextDecoder().decode(b.subarray(1, 3)); })()"),
      "bc");
}

}  // namespace
