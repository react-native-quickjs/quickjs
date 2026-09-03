/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TextEncodingModule.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "QuickJSModuleNative.h"

namespace rnqjs::textencoding {

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
 * Encodes into a caller-provided buffer, reporting how much of the input was
 * consumed and how much was written. Stops on a character boundary: a
 * destination with no room for the next character consumes nothing rather than
 * writing a partial sequence, which is what encodeInto is specified to do.
 */
void utf8EncodeInto(
    const uint16_t *units, size_t count, uint8_t *out, size_t outLen,
    size_t &read, size_t &written) {
  read = 0;
  written = 0;

  for (size_t i = 0; i < count; i++) {
    uint32_t c = units[i];
    size_t consumedUnits = 1;

    if (c >= 0xD800 && c <= 0xDBFF) {
      if (i + 1 < count && units[i + 1] >= 0xDC00 && units[i + 1] <= 0xDFFF) {
        c = 0x10000 + ((c - 0xD800) << 10) + (units[i + 1] - 0xDC00);
        consumedUnits = 2;
      } else {
        c = kReplacementChar;
      }
    } else if (c >= 0xDC00 && c <= 0xDFFF) {
      c = kReplacementChar;
    }

    const size_t width = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
    if (written + width > outLen) {
      break;
    }

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

// --- reading arguments -----------------------------------------------------

/*
 * The bytes of an ArrayBuffer or of any ArrayBufferView.
 *
 * Asking the engine handles every view uniformly and respects byteOffset and
 * byteLength, which the naive "read .buffer" approach gets wrong for a
 * subarray -- a bug that only shows up once someone passes a slice.
 */
bool readBytes(
    JSContext *ctx, JSValueConst value, const uint8_t *&data, size_t &size) {
  size_t len = 0;
  if (uint8_t *raw = JS_GetArrayBuffer(ctx, &len, value)) {
    data = raw;
    size = len;
    return true;
  }

  size_t byteOffset = 0;
  size_t byteLength = 0;
  size_t bytesPerElement = 0;
  JSValue buffer = JS_GetTypedArrayBuffer(
      ctx, value, &byteOffset, &byteLength, &bytesPerElement);
  if (JS_IsException(buffer)) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return false;
  }

  uint8_t *raw = JS_GetArrayBuffer(ctx, &len, buffer);
  JS_FreeValue(ctx, buffer);
  if (raw == nullptr || byteOffset + byteLength > len) {
    return false;
  }

  data = raw + byteOffset;
  size = byteLength;
  return true;
}

/// The UTF-16 code units of a string, or nullptr with no exception pending.
/// Free with JS_FreeCStringUTF16.
const uint16_t *readUtf16(JSContext *ctx, JSValueConst value, size_t &count) {
  size_t len = 0;
  const uint16_t *units = JS_ToCStringLenUTF16(ctx, &len, value);
  if (units == nullptr) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return nullptr;
  }
  count = len;
  return units;
}

// --- classes ---------------------------------------------------------------

/*
 * Both classes are real engine classes rather than plain objects: that is what
 * makes `new TextEncoder()` construct, `instanceof` answer correctly, and
 * `class Mine extends TextDecoder {}` work. It is also the brand check -- a
 * method reached with the wrong receiver has to be a TypeError, not a read of
 * whatever that receiver happened to hold.
 *
 * The ids are file-static and allocated once; JS_NewClassID returns the
 * existing id when it already has one, while JS_NewClass registers the class
 * with each runtime that installs the module.
 */
JSClassID encoderClassId;
JSClassID decoderClassId;

/// TextDecoder is the one with state: the two flags fixed at construction.
struct DecoderState {
  bool fatal;
  bool ignoreBOM;
};

void decoderFinalizer(JSRuntime *rt, JSValue value) {
  js_free_rt(rt, JS_GetOpaque(value, decoderClassId));
}

JSClassDef encoderClass = {"TextEncoder", nullptr, nullptr, nullptr, nullptr};
JSClassDef decoderClass = {
    "TextDecoder", decoderFinalizer, nullptr, nullptr, nullptr};

// --- TextEncoder -----------------------------------------------------------

JSValue encoderConstruct(
    JSContext *ctx, JSValueConst newTarget, int argc, JSValueConst *argv) {
  // From new.target rather than the class prototype, so a subclass instance
  // gets the subclass prototype.
  JSValue proto = JS_GetPropertyStr(ctx, newTarget, "prototype");
  if (JS_IsException(proto)) {
    return proto;
  }
  JSValue self = JS_NewObjectProtoClass(ctx, proto, encoderClassId);
  JS_FreeValue(ctx, proto);
  return self;
}

JSValue encoderEncoding(JSContext *ctx, JSValueConst thisVal) {
  if (JS_GetClassID(thisVal) != encoderClassId) {
    return JS_ThrowTypeError(ctx, "not a TextEncoder");
  }
  return JS_NewString(ctx, "utf-8");
}

JSValue encoderEncode(
    JSContext *ctx, JSValueConst thisVal, int argc, JSValueConst *argv) {
  if (JS_GetClassID(thisVal) != encoderClassId) {
    return JS_ThrowTypeError(ctx, "not a TextEncoder");
  }
  if (argc == 0 || JS_IsUndefined(argv[0])) {
    return JS_NewUint8ArrayCopy(ctx, nullptr, 0);
  }

  size_t unitCount = 0;
  const uint16_t *units = readUtf16(ctx, argv[0], unitCount);
  if (units == nullptr) {
    return JS_ThrowTypeError(ctx, "TextEncoder.encode expects a string");
  }

  std::vector<uint8_t> buffer(utf8Length(units, unitCount));
  const size_t written = utf8Encode(units, unitCount, buffer.data());
  JS_FreeCStringUTF16(ctx, units);

  // Allocates the backing store and the view in one call; the portable
  // equivalent would fetch the global Uint8Array and invoke it every encode.
  return JS_NewUint8ArrayCopy(ctx, buffer.data(), written);
}

JSValue encoderEncodeInto(
    JSContext *ctx, JSValueConst thisVal, int argc, JSValueConst *argv) {
  if (JS_GetClassID(thisVal) != encoderClassId) {
    return JS_ThrowTypeError(ctx, "not a TextEncoder");
  }
  if (argc < 2) {
    return JS_ThrowTypeError(ctx, "TextEncoder.encodeInto expects 2 arguments");
  }

  size_t unitCount = 0;
  const uint16_t *units = readUtf16(ctx, argv[0], unitCount);
  if (units == nullptr) {
    return JS_ThrowTypeError(ctx, "TextEncoder.encodeInto expects a string");
  }

  const uint8_t *dest = nullptr;
  size_t destLen = 0;
  if (!readBytes(ctx, argv[1], dest, destLen)) {
    JS_FreeCStringUTF16(ctx, units);
    return JS_ThrowTypeError(
        ctx, "TextEncoder.encodeInto expects a Uint8Array destination");
  }

  size_t read = 0;
  size_t written = 0;
  utf8EncodeInto(
      units, unitCount, const_cast<uint8_t *>(dest), destLen, read, written);
  JS_FreeCStringUTF16(ctx, units);

  JSValue result = JS_NewObject(ctx);
  if (JS_IsException(result)) {
    return result;
  }
  JS_SetPropertyStr(
      ctx, result, "read", JS_NewInt64(ctx, static_cast<int64_t>(read)));
  JS_SetPropertyStr(
      ctx, result, "written", JS_NewInt64(ctx, static_cast<int64_t>(written)));
  return result;
}

// --- TextDecoder -----------------------------------------------------------

JSValue decoderConstruct(
    JSContext *ctx, JSValueConst newTarget, int argc, JSValueConst *argv) {
  // Only UTF-8 is supported, so an unknown label is a RangeError rather than a
  // silent mis-decode. The spec's own label table is far larger; every entry
  // this refuses is one this module could not have decoded anyway.
  if (argc > 0 && !JS_IsUndefined(argv[0])) {
    const char *label = JS_ToCString(ctx, argv[0]);
    if (label == nullptr) {
      return JS_EXCEPTION;
    }
    const bool utf8 = strcmp(label, "utf-8") == 0 ||
                      strcmp(label, "utf8") == 0 ||
                      strcmp(label, "unicode-1-1-utf-8") == 0;
    if (!utf8) {
      JSValue error = JS_ThrowRangeError(
          ctx, "TextDecoder only supports utf-8, got '%s'", label);
      JS_FreeCString(ctx, label);
      return error;
    }
    JS_FreeCString(ctx, label);
  }

  bool fatal = false;
  bool ignoreBOM = false;
  if (argc > 1 && JS_IsObject(argv[1])) {
    JSValue value = JS_GetPropertyStr(ctx, argv[1], "fatal");
    if (JS_IsException(value)) {
      return value;
    }
    fatal = JS_ToBool(ctx, value) > 0;
    JS_FreeValue(ctx, value);

    value = JS_GetPropertyStr(ctx, argv[1], "ignoreBOM");
    if (JS_IsException(value)) {
      return value;
    }
    ignoreBOM = JS_ToBool(ctx, value) > 0;
    JS_FreeValue(ctx, value);
  }

  JSValue proto = JS_GetPropertyStr(ctx, newTarget, "prototype");
  if (JS_IsException(proto)) {
    return proto;
  }
  JSValue self = JS_NewObjectProtoClass(ctx, proto, decoderClassId);
  JS_FreeValue(ctx, proto);
  if (JS_IsException(self)) {
    return self;
  }

  auto *state =
      static_cast<DecoderState *>(js_malloc(ctx, sizeof(DecoderState)));
  if (state == nullptr) {
    JS_FreeValue(ctx, self);
    return JS_EXCEPTION;
  }
  state->fatal = fatal;
  state->ignoreBOM = ignoreBOM;
  JS_SetOpaque(self, state);
  return self;
}

JSValue decoderEncoding(JSContext *ctx, JSValueConst thisVal) {
  if (JS_GetOpaque2(ctx, thisVal, decoderClassId) == nullptr) {
    return JS_EXCEPTION;
  }
  return JS_NewString(ctx, "utf-8");
}

JSValue decoderFatal(JSContext *ctx, JSValueConst thisVal) {
  auto *state =
      static_cast<DecoderState *>(JS_GetOpaque2(ctx, thisVal, decoderClassId));
  return state == nullptr ? JS_EXCEPTION : JS_NewBool(ctx, state->fatal);
}

JSValue decoderIgnoreBOM(JSContext *ctx, JSValueConst thisVal) {
  auto *state =
      static_cast<DecoderState *>(JS_GetOpaque2(ctx, thisVal, decoderClassId));
  return state == nullptr ? JS_EXCEPTION : JS_NewBool(ctx, state->ignoreBOM);
}

JSValue decoderDecode(
    JSContext *ctx, JSValueConst thisVal, int argc, JSValueConst *argv) {
  auto *state =
      static_cast<DecoderState *>(JS_GetOpaque2(ctx, thisVal, decoderClassId));
  if (state == nullptr) {
    return JS_EXCEPTION;
  }
  if (argc == 0 || JS_IsUndefined(argv[0])) {
    return JS_NewString(ctx, "");
  }

  const uint8_t *bytes = nullptr;
  size_t len = 0;
  if (!readBytes(ctx, argv[0], bytes, len)) {
    return JS_ThrowTypeError(
        ctx, "TextDecoder.decode expects an ArrayBuffer or a view");
  }

  // A leading byte order mark is stripped unless ignoreBOM was requested.
  if (!state->ignoreBOM && len >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB &&
      bytes[2] == 0xBF) {
    bytes += 3;
    len -= 3;
  }

  std::u16string out;
  if (!utf8Decode(bytes, len, state->fatal, out)) {
    return JS_ThrowTypeError(
        ctx, "The encoded data was not valid for encoding utf-8");
  }
  return JS_NewStringUTF16(
      ctx, reinterpret_cast<const uint16_t *>(out.data()), out.size());
}

// --- installation ----------------------------------------------------------

const JSCFunctionListEntry encoderProto[] = {
    JS_CFUNC_DEF("encode", 1, encoderEncode),
    JS_CFUNC_DEF("encodeInto", 2, encoderEncodeInto),
    JS_CGETSET_DEF("encoding", encoderEncoding, nullptr),
    JS_PROP_STRING_DEF(
        "[Symbol.toStringTag]", "TextEncoder", JS_PROP_CONFIGURABLE),
};

const JSCFunctionListEntry decoderProto[] = {
    JS_CFUNC_DEF("decode", 1, decoderDecode),
    JS_CGETSET_DEF("encoding", decoderEncoding, nullptr),
    JS_CGETSET_DEF("fatal", decoderFatal, nullptr),
    JS_CGETSET_DEF("ignoreBOM", decoderIgnoreBOM, nullptr),
    JS_PROP_STRING_DEF(
        "[Symbol.toStringTag]", "TextDecoder", JS_PROP_CONFIGURABLE),
};

/// Registers one class and puts its constructor on the global object.
void defineClass(
    JSContext *ctx, JSValueConst global, JSClassID classId,
    const JSClassDef &def, JSCFunction *construct, const char *name,
    int argLength, const JSCFunctionListEntry *proto, size_t protoCount) {
  JS_NewClass(JS_GetRuntime(ctx), classId, &def);

  JSValue prototype = JS_NewObject(ctx);
  JS_SetPropertyFunctionList(
      ctx, prototype, proto, static_cast<int>(protoCount));

  JSValue constructor = JS_NewCFunction2(
      ctx, construct, name, argLength, JS_CFUNC_constructor, 0);
  JS_SetConstructor(ctx, constructor, prototype);
  JS_SetClassProto(ctx, classId, prototype);

  JS_SetPropertyStr(ctx, global, name, constructor);
}

}  // namespace

void install(jsi::Runtime &rt) {
  // The one place this module touches JSI: the runtime is how the installer
  // reaches the engine. Everything past this line is the engine's own API.
  //
  // On a runtime that is not ours there is no context to bind to, so nothing
  // is installed and whatever polyfill the bundle carries stays in place --
  // better than replacing it with something slower.
  JSContext *ctx = qjs::contextFromRuntime(rt);
  if (ctx == nullptr) {
    return;
  }

  JS_NewClassID(JS_GetRuntime(ctx), &encoderClassId);
  JS_NewClassID(JS_GetRuntime(ctx), &decoderClassId);

  JSValue global = JS_GetGlobalObject(ctx);
  defineClass(
      ctx, global, encoderClassId, encoderClass, encoderConstruct,
      "TextEncoder", 0, encoderProto,
      sizeof(encoderProto) / sizeof(encoderProto[0]));
  defineClass(
      ctx, global, decoderClassId, decoderClass, decoderConstruct,
      "TextDecoder", 0, decoderProto,
      sizeof(decoderProto) / sizeof(decoderProto[0]));
  JS_FreeValue(ctx, global);
}

}  // namespace rnqjs::textencoding

/*
 * The symbol the generated module registry calls by name.
 *
 * Both registration routes exist on purpose. The static initializer below is
 * convenient and works for a shared library; this explicit entry point is what
 * survives a static-library link, where the linker is entitled to drop an
 * object file nothing references -- leaving the module compiled, linked, and
 * silently installing nothing. Registration is deduplicated by name, so a
 * module reached by both routes installs exactly once.
 */
extern "C" void textEncoding_install(facebook::jsi::Runtime &runtime) {
  rnqjs::textencoding::install(runtime);
}

QJS_REGISTER_MODULE(
    "react-native-quickjs-text-encoding", rnqjs::textencoding::install)
