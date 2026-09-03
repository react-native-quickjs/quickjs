/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TextEncoding.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "QuickJSModuleNative.h"

namespace qjs::textencoding {

namespace jsi = facebook::jsi;

namespace {

constexpr uint32_t kReplacementChar = 0xFFFD;

/*
 * Number of UTF-8 bytes a UTF-16 sequence will occupy, following WHATWG
 * Encoding: an unpaired surrogate is not an error, it encodes as U+FFFD.
 *
 * Measured twice rather than encoding into a growable buffer: the sizing pass
 * is a branchy scan with no stores, and it lets the encode pass write into an
 * exactly-sized allocation with no bounds checks and no reallocation. On ASCII
 * — overwhelmingly the common case for JSON and app strings — the first pass
 * costs about as much as a strlen.
 */
size_t utf8Length(const uint16_t *units, size_t count) {
  size_t bytes = 0;
  for (size_t i = 0; i < count; i++) {
    uint32_t c = units[i];
    if (c < 0x80) {
      bytes += 1;
    } else if (c < 0x800) {
      bytes += 2;
    } else if (
        c >= 0xD800 && c <= 0xDBFF && i + 1 < count && units[i + 1] >= 0xDC00 &&
        units[i + 1] <= 0xDFFF) {
      // A well-formed surrogate pair is one astral code point: 4 bytes.
      bytes += 4;
      i++;
    } else {
      // Either a BMP character or a lone surrogate. A lone surrogate becomes
      // U+FFFD, which is itself 3 bytes, so both cases agree.
      bytes += 3;
    }
  }
  return bytes;
}

/// Encodes UTF-16 to UTF-8 into `out`, which must have room for utf8Length().
/// Returns the number of bytes written.
size_t utf8Encode(const uint16_t *units, size_t count, uint8_t *out) {
  uint8_t *p = out;
  for (size_t i = 0; i < count; i++) {
    uint32_t c = units[i];

    if (c < 0x80) {
      *p++ = static_cast<uint8_t>(c);
      continue;
    }

    if (c >= 0xD800 && c <= 0xDBFF) {
      if (i + 1 < count && units[i + 1] >= 0xDC00 && units[i + 1] <= 0xDFFF) {
        c = 0x10000 + ((c - 0xD800) << 10) + (units[i + 1] - 0xDC00);
        i++;
      } else {
        c = kReplacementChar;  // unpaired high surrogate
      }
    } else if (c >= 0xDC00 && c <= 0xDFFF) {
      c = kReplacementChar;  // unpaired low surrogate
    }

    if (c < 0x800) {
      *p++ = static_cast<uint8_t>(0xC0 | (c >> 6));
      *p++ = static_cast<uint8_t>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      *p++ = static_cast<uint8_t>(0xE0 | (c >> 12));
      *p++ = static_cast<uint8_t>(0x80 | ((c >> 6) & 0x3F));
      *p++ = static_cast<uint8_t>(0x80 | (c & 0x3F));
    } else {
      *p++ = static_cast<uint8_t>(0xF0 | (c >> 18));
      *p++ = static_cast<uint8_t>(0x80 | ((c >> 12) & 0x3F));
      *p++ = static_cast<uint8_t>(0x80 | ((c >> 6) & 0x3F));
      *p++ = static_cast<uint8_t>(0x80 | (c & 0x3F));
    }
  }
  return static_cast<size_t>(p - out);
}

/*
 * Decodes UTF-8 to UTF-16 following the WHATWG Encoding error handling: every
 * maximal subpart of an ill-formed sequence produces one U+FFFD, overlong forms
 * and surrogate code points are errors, and the maximum is U+10FFFF.
 *
 * Getting this exactly right matters more than it looks. A decoder that emits
 * one U+FFFD per bad *byte* rather than per maximal subpart passes casual
 * testing and fails the Web Platform Tests, and worse, silently disagrees with
 * every other implementation on network data.
 *
 * Returns false if `fatal` and the input is ill-formed.
 */
bool utf8Decode(
    const uint8_t *in, size_t len, bool fatal, std::u16string &out) {
  size_t i = 0;
  out.reserve(len);

  while (i < len) {
    uint8_t b0 = in[i];

    if (b0 < 0x80) {
      out.push_back(static_cast<char16_t>(b0));
      i++;
      continue;
    }

    // Determine the sequence length and the code point's lower bound, which is
    // what makes overlong encodings detectable.
    int extra;
    uint32_t cp;
    uint32_t lowerBound;
    if ((b0 & 0xE0) == 0xC0) {
      extra = 1;
      cp = b0 & 0x1F;
      lowerBound = 0x80;
    } else if ((b0 & 0xF0) == 0xE0) {
      extra = 2;
      cp = b0 & 0x0F;
      lowerBound = 0x800;
    } else if ((b0 & 0xF8) == 0xF0) {
      extra = 3;
      cp = b0 & 0x07;
      lowerBound = 0x10000;
    } else {
      // Continuation byte or 0xF8..0xFF: never a valid leader.
      if (fatal) return false;
      out.push_back(static_cast<char16_t>(kReplacementChar));
      i++;
      continue;
    }

    // Consume continuation bytes, stopping at the first byte that is not one.
    // Stopping rather than skipping is what makes this "maximal subpart":
    // the offending byte gets re-examined as a potential leader.
    int consumed = 0;
    bool bad = false;
    for (int k = 1; k <= extra; k++) {
      if (i + k >= len || (in[i + k] & 0xC0) != 0x80) {
        bad = true;
        break;
      }
      cp = (cp << 6) | (in[i + k] & 0x3F);
      consumed++;
    }

    if (bad || cp < lowerBound || cp > 0x10FFFF ||
        (cp >= 0xD800 && cp <= 0xDFFF)) {
      if (fatal) return false;
      out.push_back(static_cast<char16_t>(kReplacementChar));
      // Advance past the leader plus whatever continuation bytes we did
      // consume, so the next iteration resumes at the first byte that broke
      // the sequence.
      i += 1 + static_cast<size_t>(consumed);
      continue;
    }

    if (cp < 0x10000) {
      out.push_back(static_cast<char16_t>(cp));
    } else {
      cp -= 0x10000;
      out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
      out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
    }
    i += 1 + static_cast<size_t>(extra);
  }
  return true;
}

/*
 * Reads the bytes of an ArrayBuffer or any ArrayBufferView (TypedArray or
 * DataView) without copying.
 *
 * JSI models ArrayBuffer but not views, so a Uint8Array arrives as a plain
 * Object. Going through the engine handles every view uniformly and respects
 * byteOffset/byteLength, which the naive "read .buffer" approach gets wrong for
 * a subarray — a bug that only shows up once someone passes a slice.
 */
bool readBytes(
    jsi::Runtime &rt, const jsi::Value &value, const uint8_t *&data,
    size_t &size) {
  JSContext *ctx = qjs::contextFromRuntime(rt);
  if (ctx == nullptr || !value.isObject()) {
    return false;
  }

  JSValue v = qjs::borrowJSValue(rt, value);

  size_t len = 0;
  if (uint8_t *raw = JS_GetArrayBuffer(ctx, &len, v)) {
    data = raw;
    size = len;
    return true;
  }

  // Not an ArrayBuffer: try it as a view, which also gives us the correct
  // offset and length rather than the whole backing store.
  size_t byteOffset = 0;
  size_t byteLength = 0;
  size_t bytesPerElement = 0;
  JSValue bufferValue = JS_GetTypedArrayBuffer(
      ctx, v, &byteOffset, &byteLength, &bytesPerElement);
  if (JS_IsException(bufferValue)) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return false;
  }

  uint8_t *raw = JS_GetArrayBuffer(ctx, &len, bufferValue);
  JS_FreeValue(ctx, bufferValue);
  if (raw == nullptr || byteOffset + byteLength > len) {
    return false;
  }

  data = raw + byteOffset;
  size = byteLength;
  return true;
}

/// Reads a JS string as UTF-16 code units. Returns false if not a string.
bool readUtf16(
    jsi::Runtime &rt, const jsi::Value &value, const uint16_t *&units,
    size_t &count) {
  JSContext *ctx = qjs::contextFromRuntime(rt);
  if (ctx == nullptr) return false;

  JSValue v = qjs::borrowJSValue(rt, value);
  size_t len = 0;
  const uint16_t *p = JS_ToCStringLenUTF16(ctx, &len, v);
  if (p == nullptr) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return false;
  }
  units = p;
  count = len;
  return true;
}

void freeUtf16(jsi::Runtime &rt, const uint16_t *units) {
  if (JSContext *ctx = qjs::contextFromRuntime(rt)) {
    JS_FreeCStringUTF16(ctx, units);
  }
}

jsi::Value makeUint8Array(jsi::Runtime &rt, const uint8_t *bytes, size_t len) {
  JSContext *ctx = qjs::contextFromRuntime(rt);
  // JS_NewUint8ArrayCopy allocates the backing store and the view in one call.
  // The portable JSI equivalent would fetch the global Uint8Array constructor
  // and invoke it on every encode.
  return qjs::adoptJSValue(rt, JS_NewUint8ArrayCopy(ctx, bytes, len));
}

// --- TextEncoder -----------------------------------------------------------

/*
 * WHATWG Encoding says a fatal decode failure is a TypeError, and WebIDL says
 * the same for an argument of the wrong type. jsi::JSError builds a plain
 * Error, so the class is constructed here -- code that branches on
 * `e instanceof TypeError` is the reason this matters.
 */
[[noreturn]] void throwTypeError(jsi::Runtime &rt, const char *message) {
  auto ctor = rt.global().getPropertyAsFunction(rt, "TypeError");
  throw jsi::JSError(
      rt, ctor.callAsConstructor(rt, jsi::String::createFromUtf8(rt, message)));
}

jsi::Value encode(jsi::Runtime &rt, const jsi::Value *args, size_t count) {
  if (count == 0 || args[0].isUndefined()) {
    return makeUint8Array(rt, nullptr, 0);
  }

  const uint16_t *units = nullptr;
  size_t unitCount = 0;
  if (!readUtf16(rt, args[0], units, unitCount)) {
    throwTypeError(rt, "TextEncoder.encode expects a string");
  }

  size_t needed = utf8Length(units, unitCount);
  std::vector<uint8_t> buffer(needed);
  size_t written = utf8Encode(units, unitCount, buffer.data());
  freeUtf16(rt, units);

  return makeUint8Array(rt, buffer.data(), written);
}

/*
 * encodeInto(source, destination) -> { read, written }
 *
 * The zero-allocation path, and the reason it exists: callers streaming into a
 * pooled buffer avoid an allocation per chunk. It must never write a partial
 * code point, so encoding stops at the last character that fits whole.
 */
jsi::Value encodeInto(jsi::Runtime &rt, const jsi::Value *args, size_t count) {
  if (count < 2) {
    throwTypeError(rt, "TextEncoder.encodeInto expects 2 arguments");
  }

  const uint16_t *units = nullptr;
  size_t unitCount = 0;
  if (!readUtf16(rt, args[0], units, unitCount)) {
    throwTypeError(rt, "TextEncoder.encodeInto expects a string");
  }

  const uint8_t *dest = nullptr;
  size_t destLen = 0;
  if (!readBytes(rt, args[1], dest, destLen)) {
    freeUtf16(rt, units);
    throwTypeError(
        rt, "TextEncoder.encodeInto expects a Uint8Array destination");
  }

  auto *out = const_cast<uint8_t *>(dest);
  size_t read = 0;
  size_t written = 0;

  for (size_t i = 0; i < unitCount; i++) {
    uint32_t c = units[i];
    size_t consumedUnits = 1;

    if (c >= 0xD800 && c <= 0xDBFF) {
      if (i + 1 < unitCount && units[i + 1] >= 0xDC00 &&
          units[i + 1] <= 0xDFFF) {
        c = 0x10000 + ((c - 0xD800) << 10) + (units[i + 1] - 0xDC00);
        consumedUnits = 2;
      } else {
        c = kReplacementChar;
      }
    } else if (c >= 0xDC00 && c <= 0xDFFF) {
      c = kReplacementChar;
    }

    size_t width = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
    if (written + width > destLen) {
      break;  // never split a code point across the boundary
    }

    // Encode this one code point. `c` is already the decoded scalar value —
    // including U+FFFD for an unpaired surrogate — so it is written directly
    // rather than re-deriving it from the source units.
    if (c < 0x80) {
      out[written] = static_cast<uint8_t>(c);
    } else if (c < 0x800) {
      out[written] = static_cast<uint8_t>(0xC0 | (c >> 6));
      out[written + 1] = static_cast<uint8_t>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      out[written] = static_cast<uint8_t>(0xE0 | (c >> 12));
      out[written + 1] = static_cast<uint8_t>(0x80 | ((c >> 6) & 0x3F));
      out[written + 2] = static_cast<uint8_t>(0x80 | (c & 0x3F));
    } else {
      out[written] = static_cast<uint8_t>(0xF0 | (c >> 18));
      out[written + 1] = static_cast<uint8_t>(0x80 | ((c >> 12) & 0x3F));
      out[written + 2] = static_cast<uint8_t>(0x80 | ((c >> 6) & 0x3F));
      out[written + 3] = static_cast<uint8_t>(0x80 | (c & 0x3F));
    }

    written += width;
    read += consumedUnits;
    i += consumedUnits - 1;
  }

  freeUtf16(rt, units);

  jsi::Object result(rt);
  result.setProperty(rt, "read", jsi::Value(static_cast<double>(read)));
  result.setProperty(rt, "written", jsi::Value(static_cast<double>(written)));
  return result;
}

// --- TextDecoder -----------------------------------------------------------

jsi::Value decode(
    jsi::Runtime &rt, const jsi::Value *args, size_t count, bool fatal,
    bool ignoreBOM) {
  if (count == 0 || args[0].isUndefined()) {
    return jsi::String::createFromUtf8(rt, "");
  }

  const uint8_t *bytes = nullptr;
  size_t len = 0;
  if (!readBytes(rt, args[0], bytes, len)) {
    throwTypeError(rt, "TextDecoder.decode expects an ArrayBuffer or a view");
  }

  // A leading UTF-8 BOM is stripped unless ignoreBOM was requested.
  if (!ignoreBOM && len >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB &&
      bytes[2] == 0xBF) {
    bytes += 3;
    len -= 3;
  }

  std::u16string out;
  if (!utf8Decode(bytes, len, fatal, out)) {
    throwTypeError(rt, "The encoded data was not valid for encoding utf-8");
  }

  JSContext *ctx = qjs::contextFromRuntime(rt);
  return qjs::adoptJSValue(
      rt, JS_NewStringUTF16(
              ctx, reinterpret_cast<const uint16_t *>(out.data()), out.size()));
}

}  // namespace

/*
 * The class shapes are defined in JavaScript, not C++.
 *
 * A jsi::Function made by createFromHostFunction is not a real constructor, and
 * the failure is quieter than "the instance is empty" — there is no instance.
 * `jsi::HostFunctionType` is
 *
 *     Value(Runtime &, const Value &thisVal, const Value *args, size_t)
 *
 * with no new.target and no way for the engine to build `this` from the
 * function's `.prototype`. MEASURED against build-rel, 2026-07-26: `typeof
 * TextEncoder` reports "function"; under `new`, thisVal is *the constructor
 * function itself* rather than a fresh instance, so writing to `this` mutates
 * the constructor object; and the `new` expression evaluates to whatever the
 * host function returned — `undefined` when it returns nothing, which makes
 * `new TextEncoder().encode(...)` a TypeError on undefined. Returning an object
 * explicitly gives you that object, but with `Object.prototype` as its
 * prototype: the assigned `.prototype` is never consulted, the methods are
 * missing, and `instanceof` is false. `class S extends TextEncoder {}` then
 * `new S()` yields `undefined` rather than throwing.
 *
 * Defining the classes in JS avoids all of that and costs nothing that matters:
 * this runs once per runtime, and every byte of actual work still happens in
 * the native functions below. It also makes the spec surface — property
 * attributes, `Symbol.toStringTag`, argument coercion — the engine's problem
 * rather than ours.
 *
 * A module written as direct QuickJS bindings has the other option and does not
 * need the shim: JS_NewCFunction2 with JS_CFUNC_constructor, plus
 * JS_SetConstructor, is a real constructor with a working `instanceof`,
 * `extends`, and "TypeError: must be called with new". That is the default the
 * module scaffold now generates; see docs-site/docs/quickjs-modules/writing.md.
 */
const char *kClassShim = R"JS(
(function (native) {
  'use strict';

  function TextEncoder() {}
  Object.defineProperty(TextEncoder.prototype, 'encoding', {
    get: function () { return 'utf-8'; },
  });
  TextEncoder.prototype.encode = function (input) {
    return native.encode(input === undefined ? '' : String(input));
  };
  TextEncoder.prototype.encodeInto = function (source, destination) {
    return native.encodeInto(String(source), destination);
  };

  function TextDecoder(label, options) {
    var name = label === undefined ? 'utf-8' : String(label).toLowerCase();
    if (name !== 'utf-8' && name !== 'utf8' && name !== 'unicode-1-1-utf-8') {
      // Failing loudly beats decoding some other encoding as UTF-8 and
      // producing mojibake the caller cannot explain.
      throw new RangeError("TextDecoder only supports utf-8, got '" + name + "'");
    }
    var opts = options || {};
    Object.defineProperty(this, 'fatal', { value: !!opts.fatal, enumerable: true });
    Object.defineProperty(this, 'ignoreBOM', { value: !!opts.ignoreBOM, enumerable: true });
  }
  Object.defineProperty(TextDecoder.prototype, 'encoding', {
    get: function () { return 'utf-8'; },
  });
  TextDecoder.prototype.decode = function (input) {
    if (input === undefined) return '';
    return native.decode(input, this.fatal, this.ignoreBOM);
  };

  globalThis.TextEncoder = TextEncoder;
  globalThis.TextDecoder = TextDecoder;
})
)JS";

void install(jsi::Runtime &rt) {
  // This module requires the escape hatch. On a foreign runtime it installs
  // nothing rather than installing something slower than the shim it replaces,
  // leaving whatever polyfill the bundle carries in place.
  if (qjs::contextFromRuntime(rt) == nullptr) {
    return;
  }

  // The native half: three functions doing all the byte work. The shim above
  // wraps them in the spec's class shapes.
  auto native = jsi::Object(rt);

  native.setProperty(
      rt, "encode",
      jsi::Function::createFromHostFunction(
          rt, jsi::PropNameID::forAscii(rt, "encode"), 1,
          [](jsi::Runtime &rt, const jsi::Value &, const jsi::Value *args,
             size_t count) { return encode(rt, args, count); }));
  native.setProperty(
      rt, "encodeInto",
      jsi::Function::createFromHostFunction(
          rt, jsi::PropNameID::forAscii(rt, "encodeInto"), 2,
          [](jsi::Runtime &rt, const jsi::Value &, const jsi::Value *args,
             size_t count) { return encodeInto(rt, args, count); }));
  native.setProperty(
      rt, "decode",
      jsi::Function::createFromHostFunction(
          rt, jsi::PropNameID::forAscii(rt, "decode"), 3,
          [](jsi::Runtime &rt, const jsi::Value &, const jsi::Value *args,
             size_t count) -> jsi::Value {
            bool fatal = count > 1 && args[1].isBool() && args[1].getBool();
            bool ignoreBOM = count > 2 && args[2].isBool() && args[2].getBool();
            return decode(rt, args, count, fatal, ignoreBOM);
          }));

  // Evaluating the shim returns the factory; calling it installs the globals.
  // The native object is passed in rather than stashed on globalThis, so the
  // module leaves no extra global behind.
  auto factory = rt.evaluateJavaScript(
      std::make_shared<jsi::StringBuffer>(kClassShim), "qjs:text-encoding");
  factory.asObject(rt).asFunction(rt).call(rt, std::move(native));
}

}  // namespace qjs::textencoding
