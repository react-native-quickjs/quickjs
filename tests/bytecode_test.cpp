/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * Round-trips real ahead-of-time bytecode: compiles JS with the qjsc tool
 * built from the same engine, then loads the container through the runtime.
 * The conformance suite covers the JSI surface; this covers the container
 * format and the bytecode paths through evaluateJavaScript /
 * prepareJavaScript.
 */

#include <QuickJSBytecode.h>
#include <QuickJSRuntimeFactory.h>
#include <gtest/gtest.h>
#include <jsi/jsi.h>
#include <quickjs.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace jsi = facebook::jsi;

namespace {

class VectorBuffer : public jsi::Buffer {
 public:
  explicit VectorBuffer(std::vector<uint8_t> data) : data_(std::move(data)) {}

  size_t size() const override {
    return data_.size();
  }
  const uint8_t *data() const override {
    return data_.data();
  }

 private:
  std::vector<uint8_t> data_;
};

std::shared_ptr<jsi::Buffer> bufferOf(const std::string &text) {
  return std::make_shared<VectorBuffer>(
      std::vector<uint8_t>(text.begin(), text.end()));
}

uint32_t crc32cReference(const uint8_t *data, size_t size) {
  uint32_t crc = 0xffffffffu;
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0x82f63b78u & -(crc & 1));
    }
  }
  return crc ^ 0xffffffffu;
}

// JS_ReadObject validates the checksum before it parses the body. Supplying a
// deliberately malformed body therefore lets this test check the private
// engine checksum without adding a public checksum API. The body starts at an
// offset of five bytes, so the non-empty cases also exercise unaligned input.
bool engineAcceptsChecksum(JSContext *ctx, const std::vector<uint8_t> &body) {
  std::vector<uint8_t> blob = {
      static_cast<uint8_t>(qjs::kBytecodeFormatVersion), 0, 0, 0, 0};
  const uint32_t checksum = crc32cReference(body.data(), body.size());
  blob[1] = static_cast<uint8_t>(checksum);
  blob[2] = static_cast<uint8_t>(checksum >> 8);
  blob[3] = static_cast<uint8_t>(checksum >> 16);
  blob[4] = static_cast<uint8_t>(checksum >> 24);
  blob.insert(blob.end(), body.begin(), body.end());

  JSValue value =
      JS_ReadObject(ctx, blob.data(), blob.size(), JS_READ_OBJ_BYTECODE);
  if (!JS_IsException(value)) {
    JS_FreeValue(ctx, value);
    return true;
  }

  JSValue exception = JS_GetException(ctx);
  const char *message = JS_ToCString(ctx, exception);
  const bool checksumAccepted =
      message == nullptr || std::strstr(message, "checksum error") == nullptr;
  if (message != nullptr) {
    JS_FreeCString(ctx, message);
  }
  JS_FreeValue(ctx, exception);
  return checksumAccepted;
}

// A name no other process or call shares: two build trees under ctest, or tests run in parallel, must not overwrite or remove each
// other's files. `stem` is made once per compile so the source and its container get matching names.
std::string tempStem() {
  static std::atomic<unsigned> counter{0};
  const char *dir = std::getenv("TMPDIR");
  std::string base = dir != nullptr ? dir : "/tmp";
  if (base.back() != '/') {
    base += '/';
  }
  return base + "rnqjs-bytecode-test-" +
         std::to_string(static_cast<long>(getpid())) + "-" +
         std::to_string(counter.fetch_add(1));
}

/// Compiles `source` with the qjsc tool and returns the container bytes. A
/// caller that compares two compiles of one source passes the same `fixedStem`,
/// so the script name the stack traces carry is the same too.
std::vector<uint8_t> compileToBytecode(
    const std::string &source, const char *extraFlags = "",
    const std::string &fixedStem = std::string()) {
  const std::string stem = fixedStem.empty() ? tempStem() : fixedStem;
  const std::string jsPath = stem + ".js";
  const std::string bcPath = stem + ".bc";

  {
    std::ofstream out(jsPath, std::ios::binary);
    out << source;
  }

  const std::string command = std::string(QJSC_PATH) + " " + extraFlags + " '" +
                              jsPath + "' '" + bcPath + "'";
  const int status = std::system(command.c_str());
  EXPECT_EQ(status, 0) << "qjsc failed: " << command;

  std::ifstream in(bcPath, std::ios::binary);
  std::vector<uint8_t> bytes{
      std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};

  std::remove(jsPath.c_str());
  std::remove(bcPath.c_str());
  return bytes;
}

bool readLeb128(
    const std::vector<uint8_t> &bytes, size_t *pos, uint32_t *value) {
  uint32_t result = 0;
  int shift = 0;
  while (*pos < bytes.size() && shift < 32) {
    const uint8_t byte = bytes[(*pos)++];
    result |= static_cast<uint32_t>(byte & 0x7f) << shift;
    if (!(byte & 0x80)) {
      *value = result;
      return true;
    }
    shift += 7;
  }
  return false;
}

size_t firstStringLengthOffset(const std::vector<uint8_t> &bytes) {
  size_t pos = qjs::kBytecodeHeaderSize + 5;
  uint32_t atomCount = 0;
  if (!readLeb128(bytes, &pos, &atomCount)) return 0;
  for (uint32_t i = 0; i < atomCount && pos < bytes.size(); ++i) {
    const uint8_t type = bytes[pos++];
    if (type == 0) {
      pos += 4;
      continue;
    }
    const size_t lengthOffset = pos;
    uint32_t encodedLength = 0;
    if (!readLeb128(bytes, &pos, &encodedLength)) return 0;
    const size_t logicalLength = encodedLength >> 1;
    const size_t byteLength = logicalLength << (encodedLength & 1);
    if (byteLength > bytes.size() - pos) return 0;
    return lengthOffset;
  }
  return 0;
}

// The offset of the length field of the top-level function's deferrable region. That region runs to the end of the payload, so the
// field is the 32-bit word that equals the number of bytes after it; the function header in front of it differs between the stack
// and the register build, and a byte equal to the function tag also occurs inside atoms, so the header is not parsed.
size_t firstFunctionBodyLengthOffset(const std::vector<uint8_t> &bytes) {
  for (size_t pos = qjs::kBytecodeHeaderSize + 5;
       pos + sizeof(uint32_t) <= bytes.size(); ++pos) {
    const uint32_t declared = static_cast<uint32_t>(bytes[pos]) |
                              (static_cast<uint32_t>(bytes[pos + 1]) << 8) |
                              (static_cast<uint32_t>(bytes[pos + 2]) << 16) |
                              (static_cast<uint32_t>(bytes[pos + 3]) << 24);
    if (declared > 8 && declared == bytes.size() - pos - sizeof(uint32_t))
      return pos;
  }
  return 0;
}

void disableChecksum(std::vector<uint8_t> *bytes) {
  ASSERT_GT(bytes->size(), qjs::kBytecodeHeaderSize);
  std::fill(
      bytes->begin() + qjs::kBytecodeHeaderSize + 1,
      bytes->begin() + qjs::kBytecodeHeaderSize + 5, 0xff);
}

}  // namespace

TEST(Bytecode, ContainerIsRecognised) {
  auto bytes = compileToBytecode("globalThis.answer = 42;");
  ASSERT_GT(bytes.size(), qjs::kBytecodeHeaderSize);
  EXPECT_TRUE(qjs::isBytecodeContainer(bytes.data(), bytes.size()));
  EXPECT_EQ(
      qjs::bytecodeFormatVersion(bytes.data()), qjs::kBytecodeFormatVersion);
}

TEST(Bytecode, PlainSourceIsNotMistakenForBytecode) {
  const std::string source = "globalThis.answer = 42;";
  EXPECT_FALSE(qjs::isBytecodeContainer(
      reinterpret_cast<const uint8_t *>(source.data()), source.size()));

  // Nor is a truncated container, or one with a version we do not know.
  std::vector<uint8_t> truncated(qjs::kBytecodeMagic, qjs::kBytecodeMagic + 8);
  EXPECT_FALSE(qjs::isBytecodeContainer(truncated.data(), truncated.size()));

  std::vector<uint8_t> futureVersion(truncated);
  futureVersion.insert(futureVersion.end(), {99, 0, 0, 0});
  EXPECT_FALSE(
      qjs::isBytecodeContainer(futureVersion.data(), futureVersion.size()));

  // A quickjs (Bellard) container must be rejected: its bytecode is not
  // interchangeable with quickjs-ng's.
  std::vector<uint8_t> otherEngine = {'N', 'S', 'B', 'C', 'Q', 'J',
                                      'S', 0,   1,   0,   0,   0};
  EXPECT_FALSE(
      qjs::isBytecodeContainer(otherEngine.data(), otherEngine.size()));
}

TEST(Bytecode, RegExpObjectRoundTripUsesContextAllocator) {
  JSRuntime *rt = JS_NewRuntime();
  ASSERT_NE(rt, nullptr);
  JSContext *ctx = JS_NewContext(rt);
  ASSERT_NE(ctx, nullptr);

  static constexpr char source[] = "/a(?:b|c)+/g";
  JSValue regexp = JS_Eval(
      ctx, source, sizeof(source) - 1, "regexp.js", JS_EVAL_TYPE_GLOBAL);
  ASSERT_FALSE(JS_IsException(regexp));

  size_t serialized_size = 0;
  uint8_t *serialized =
      JS_WriteObject(ctx, &serialized_size, regexp, JS_WRITE_OBJ_BYTECODE);
  ASSERT_NE(serialized, nullptr);
  ASSERT_GT(serialized_size, 0u);

  JSValue round_trip =
      JS_ReadObject(ctx, serialized, serialized_size, JS_READ_OBJ_BYTECODE);
  EXPECT_FALSE(JS_IsException(round_trip));

  JS_FreeValue(ctx, round_trip);
  JS_FreeValue(ctx, regexp);
  js_free(ctx, serialized);
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
}

namespace {

/// A Hermes bytecode header: hermes::hbc::MAGIC little-endian, then padding.
/// Taken from include/hermes/BCGen/HBC/BytecodeFileFormat.h rather than
/// remembered, since the whole value of the check is that this constant is
/// right.
std::vector<uint8_t> hermesBytecodeHeader(bool delta = false) {
  std::vector<uint8_t> bytes = {0xC6, 0x1F, 0xBC, 0x03, 0xC1, 0x03, 0x19, 0x1F};
  if (delta) {
    for (auto &b : bytes) b = static_cast<uint8_t>(~b);
  }
  bytes.resize(64, 0);
  return bytes;
}

}  // namespace

TEST(Bytecode, HermesBytecodeIsRecognised) {
  auto hbc = hermesBytecodeHeader();
  EXPECT_TRUE(qjs::isHermesBytecode(hbc.data(), hbc.size()));

  // A delta bundle carries the complement of the magic and is equally
  // unexecutable here, so it must be named rather than ignored.
  auto deltaHbc = hermesBytecodeHeader(/*delta=*/true);
  EXPECT_TRUE(qjs::isHermesBytecode(deltaHbc.data(), deltaHbc.size()));

  // The two detectors must not claim each other's input.
  EXPECT_FALSE(qjs::isBytecodeContainer(hbc.data(), hbc.size()));
  auto ours = compileToBytecode("globalThis.answer = 42;");
  EXPECT_FALSE(qjs::isHermesBytecode(ours.data(), ours.size()));

  const std::string source = "globalThis.answer = 42;";
  EXPECT_FALSE(qjs::isHermesBytecode(
      reinterpret_cast<const uint8_t *>(source.data()), source.size()));

  // Must not read past a short buffer.
  EXPECT_FALSE(qjs::isHermesBytecode(hbc.data(), 7));
  EXPECT_FALSE(qjs::isHermesBytecode(nullptr, 64));
}

TEST(Bytecode, HermesBundleFailsWithAnActionableMessage) {
  auto runtime = qjs::makeQuickJSRuntime();
  auto hbc = hermesBytecodeHeader();
  auto buffer = std::make_shared<jsi::StringBuffer>(
      std::string(reinterpret_cast<const char *>(hbc.data()), hbc.size()));

  // The failure that matters is the one a developer reads, so assert on the
  // message: without this check the bundle reaches the parser as binary and
  // reports a syntax error at byte zero, which names nothing useful.
  try {
    runtime->evaluateJavaScript(buffer, "index.android.bundle");
    FAIL() << "expected Hermes bytecode to be rejected";
  } catch (const jsi::JSINativeException &e) {
    const std::string what = e.what();
    EXPECT_NE(what.find("Hermes bytecode"), std::string::npos) << what;
    EXPECT_NE(what.find("hermesEnabled"), std::string::npos) << what;
    EXPECT_NE(what.find("index.android.bundle"), std::string::npos) << what;
  }

  // prepareJavaScript is the other entry point and must fail the same way.
  EXPECT_THROW(
      runtime->prepareJavaScript(buffer, "index.android.bundle"),
      jsi::JSINativeException);
}

TEST(Bytecode, EvaluateJavaScriptRunsBytecode) {
  auto runtime = qjs::makeQuickJSRuntime();
  auto bytes = compileToBytecode("globalThis.answer = 6 * 7;");

  runtime->evaluateJavaScript(
      std::make_shared<VectorBuffer>(std::move(bytes)), "answer.bc");

  EXPECT_EQ(
      runtime->global().getProperty(*runtime, "answer").getNumber(), 42.0);
}

TEST(Bytecode, EvaluateJavaScriptStillRunsSource) {
  auto runtime = qjs::makeQuickJSRuntime();
  runtime->evaluateJavaScript(bufferOf("globalThis.answer = 6 * 7;"), "a.js");
  EXPECT_EQ(
      runtime->global().getProperty(*runtime, "answer").getNumber(), 42.0);
}

TEST(Bytecode, PrepareJavaScriptAcceptsBytecode) {
  auto runtime = qjs::makeQuickJSRuntime();
  auto bytes = compileToBytecode("globalThis.answer = 6 * 7;");

  auto prepared = runtime->prepareJavaScript(
      std::make_shared<VectorBuffer>(std::move(bytes)), "answer.bc");
  runtime->evaluatePreparedJavaScript(prepared);

  EXPECT_EQ(
      runtime->global().getProperty(*runtime, "answer").getNumber(), 42.0);
}

TEST(Bytecode, RuntimeUsesCopiedLazyBodiesForRetainedFunctions) {
  auto runtime = qjs::makeQuickJSRuntime();
  auto bytes = compileToBytecode(
      "globalThis.answerFunction = () => ({value: 40}).value + 2;");
  runtime->evaluateJavaScript(
      std::make_shared<VectorBuffer>(std::move(bytes)), "lazy-runtime.bc");
  auto function = runtime->global().getProperty(*runtime, "answerFunction");
  EXPECT_EQ(
      function.asObject(*runtime)
          .asFunction(*runtime)
          .call(*runtime)
          .getNumber(),
      42.0);
}

TEST(Bytecode, LazyFunctionSerializesBeforeCall) {
  auto bytes = compileToBytecode("globalThis.answer = {value: 40}.value + 2;");
  JSRuntime *rt = JS_NewRuntime();
  ASSERT_NE(rt, nullptr);
  JSContext *ctx = JS_NewContext(rt);
  ASSERT_NE(ctx, nullptr);

  JSValue top = JS_ReadObject(
      ctx, bytes.data() + qjs::kBytecodeHeaderSize,
      bytes.size() - qjs::kBytecodeHeaderSize,
      JS_READ_OBJ_BYTECODE | JS_READ_OBJ_LAZY);
  ASSERT_FALSE(JS_IsException(top));
  size_t serialized_size = 0;
  uint8_t *serialized =
      JS_WriteObject(ctx, &serialized_size, top, JS_WRITE_OBJ_BYTECODE);
  ASSERT_NE(serialized, nullptr);
  bytes.clear();
  JS_FreeValue(ctx, top);

  JSValue round_trip =
      JS_ReadObject(ctx, serialized, serialized_size, JS_READ_OBJ_BYTECODE);
  ASSERT_FALSE(JS_IsException(round_trip));
  JSValue result = JS_EvalFunction(ctx, round_trip);
  ASSERT_FALSE(JS_IsException(result));
  int32_t value = 0;
  ASSERT_EQ(JS_ToInt32(ctx, &value, result), 0);
  EXPECT_EQ(value, 42);
  JS_FreeValue(ctx, result);
  js_free(ctx, serialized);
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
}

TEST(Bytecode, LazyCopiedInputSurvivesCallerBufferDestruction) {
  auto bytes = compileToBytecode(
      "globalThis.answerFunction = () => ({value: 40}).value + 2;");
  JSRuntime *rt = JS_NewRuntime();
  ASSERT_NE(rt, nullptr);
  JSContext *ctx = JS_NewContext(rt);
  ASSERT_NE(ctx, nullptr);

  JSValue top = JS_ReadObject(
      ctx, bytes.data() + qjs::kBytecodeHeaderSize,
      bytes.size() - qjs::kBytecodeHeaderSize,
      JS_READ_OBJ_BYTECODE | JS_READ_OBJ_LAZY);
  ASSERT_FALSE(JS_IsException(top));
  JSValue result = JS_EvalFunction(ctx, top);
  ASSERT_FALSE(JS_IsException(result));
  JS_FreeValue(ctx, result);
  bytes.clear();

  JSValue global = JS_GetGlobalObject(ctx);
  JSValue function = JS_GetPropertyStr(ctx, global, "answerFunction");
  JSValue answer = JS_Call(ctx, function, JS_UNDEFINED, 0, nullptr);
  ASSERT_FALSE(JS_IsException(answer));
  double value = 0;
  ASSERT_EQ(JS_ToFloat64(ctx, &value, answer), 0);
  EXPECT_EQ(value, 42.0);
  JS_FreeValue(ctx, answer);
  JS_FreeValue(ctx, function);
  JS_FreeValue(ctx, global);
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
}

TEST(Bytecode, LazyBorrowedBytecodeMaterializesOnCall) {
  auto bytes = compileToBytecode(
      "function answerFunction() { return {value: 40}.value + 2; }"
      "globalThis.answer = answerFunction();");
  ASSERT_GT(bytes.size(), qjs::kBytecodeHeaderSize);
  JSRuntime *rt = JS_NewRuntime();
  ASSERT_NE(rt, nullptr);
  JSContext *ctx = JS_NewContext(rt);
  ASSERT_NE(ctx, nullptr);

  JSValue function = JS_ReadObject(
      ctx, bytes.data() + qjs::kBytecodeHeaderSize,
      bytes.size() - qjs::kBytecodeHeaderSize,
      JS_READ_OBJ_BYTECODE | JS_READ_OBJ_LAZY | JS_READ_OBJ_BORROW);
  ASSERT_FALSE(JS_IsException(function));
  JSValue result = JS_EvalFunction(ctx, function);
  ASSERT_FALSE(JS_IsException(result));
  JS_FreeValue(ctx, result);
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
}

TEST(Bytecode, LazyFlagCombinationsAreRejected) {
  JSRuntime *rt = JS_NewRuntime();
  ASSERT_NE(rt, nullptr);
  JSContext *ctx = JS_NewContext(rt);
  ASSERT_NE(ctx, nullptr);
  const uint8_t payload[] = {0};
  EXPECT_TRUE(JS_IsException(
      JS_ReadObject(ctx, payload, sizeof(payload), JS_READ_OBJ_BORROW)));
  JS_FreeValue(ctx, JS_GetException(ctx));
  EXPECT_TRUE(JS_IsException(
      JS_ReadObject(ctx, payload, sizeof(payload), JS_READ_OBJ_LAZY)));
  JS_FreeValue(ctx, JS_GetException(ctx));
  EXPECT_TRUE(JS_IsException(JS_ReadObject(
      ctx, payload, sizeof(payload),
      JS_READ_OBJ_BYTECODE | JS_READ_OBJ_LAZY | JS_READ_OBJ_REFERENCE)));
  JS_FreeValue(ctx, JS_GetException(ctx));
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
}

TEST(Bytecode, DeferredAtomsRejectOversizedAndTruncatedStrings) {
  auto oversized = compileToBytecode("globalThis.value = {longAtom: 1};");
  const size_t lengthOffset = firstStringLengthOffset(oversized);
  ASSERT_NE(lengthOffset, 0u);
  ASSERT_LE(lengthOffset + 5, oversized.size());
  const uint8_t tooLong[] = {0x80, 0x80, 0x80, 0x80, 0x08};
  std::copy(tooLong, tooLong + 5, oversized.begin() + lengthOffset);
  disableChecksum(&oversized);

  auto truncated = compileToBytecode("globalThis.value = {shortAtom: 1};");
  ASSERT_GT(truncated.size(), qjs::kBytecodeHeaderSize + 1);
  disableChecksum(&truncated);
  truncated.resize(truncated.size() - 1);

  for (const auto &bytes : {oversized, truncated}) {
    for (const int flags :
         {JS_READ_OBJ_BYTECODE, JS_READ_OBJ_BYTECODE | JS_READ_OBJ_LAZY}) {
      JSRuntime *rt = JS_NewRuntime();
      ASSERT_NE(rt, nullptr);
      JSContext *ctx = JS_NewContext(rt);
      ASSERT_NE(ctx, nullptr);
      for (int attempt = 0; attempt < 2; ++attempt) {
        JSValue value = JS_ReadObject(
            ctx, bytes.data() + qjs::kBytecodeHeaderSize,
            bytes.size() - qjs::kBytecodeHeaderSize, flags);
        EXPECT_TRUE(JS_IsException(value));
        JS_FreeValue(ctx, JS_GetException(ctx));
      }
      JS_FreeContext(ctx);
      JS_FreeRuntime(rt);
    }
  }
}

TEST(Bytecode, LazyFramedBodyTruncationIsRejectedRepeatedly) {
  auto bytes = compileToBytecode(
      "function deferred() { return {value: 42}.value; }\n"
      "globalThis.deferred = deferred;");
  ASSERT_GT(bytes.size(), qjs::kBytecodeHeaderSize + 1);
  disableChecksum(&bytes);
  bytes.resize(bytes.size() - 1);

  for (const int flags :
       {JS_READ_OBJ_BYTECODE, JS_READ_OBJ_BYTECODE | JS_READ_OBJ_LAZY}) {
    JSRuntime *rt = JS_NewRuntime();
    ASSERT_NE(rt, nullptr);
    JSContext *ctx = JS_NewContext(rt);
    ASSERT_NE(ctx, nullptr);
    for (int attempt = 0; attempt < 2; ++attempt) {
      JSValue value = JS_ReadObject(
          ctx, bytes.data() + qjs::kBytecodeHeaderSize,
          bytes.size() - qjs::kBytecodeHeaderSize, flags);
      EXPECT_TRUE(JS_IsException(value));
      JS_FreeValue(ctx, JS_GetException(ctx));
    }
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
  }
}

TEST(Bytecode, LazyFramedBodyOverDeclarationIsRejected) {
  auto bytes = compileToBytecode(
      "function deferred() { return {value: 42}.value; }\n"
      "globalThis.deferred = deferred;");
  const size_t lengthOffset = firstFunctionBodyLengthOffset(bytes);
  ASSERT_NE(lengthOffset, 0u);
  ASSERT_LE(lengthOffset + sizeof(uint32_t), bytes.size());
  const uint32_t length =
      static_cast<uint32_t>(bytes[lengthOffset]) |
      (static_cast<uint32_t>(bytes[lengthOffset + 1]) << 8) |
      (static_cast<uint32_t>(bytes[lengthOffset + 2]) << 16) |
      (static_cast<uint32_t>(bytes[lengthOffset + 3]) << 24);
  ASSERT_LT(length, UINT32_MAX);
  const uint32_t overdeclared = length + 1;
  for (int i = 0; i < 4; ++i)
    bytes[lengthOffset + i] = static_cast<uint8_t>(overdeclared >> (i * 8));
  disableChecksum(&bytes);

  for (const int flags :
       {JS_READ_OBJ_BYTECODE, JS_READ_OBJ_BYTECODE | JS_READ_OBJ_LAZY}) {
    JSRuntime *rt = JS_NewRuntime();
    ASSERT_NE(rt, nullptr);
    JSContext *ctx = JS_NewContext(rt);
    ASSERT_NE(ctx, nullptr);
    JSValue value = JS_ReadObject(
        ctx, bytes.data() + qjs::kBytecodeHeaderSize,
        bytes.size() - qjs::kBytecodeHeaderSize, flags);
    EXPECT_TRUE(JS_IsException(value));
    JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
  }
}

TEST(Bytecode, LazyNestedAsyncGeneratorAndDebugFunctionsMaterialize) {
  const std::string source =
      "function outer() { function nested() { return 7; } return nested(); }\n"
      "async function asyncFn() { return 3; }\n"
      "function* generatorFn() { yield 5; }\n"
      "globalThis.values = [outer(), asyncFn, generatorFn, String(asyncFn)];";
  auto runtime = qjs::makeQuickJSRuntime();
  runtime->evaluateJavaScript(
      std::make_shared<VectorBuffer>(compileToBytecode(source)), "lazy.bc");
  auto values = runtime->global()
                    .getProperty(*runtime, "values")
                    .asObject(*runtime)
                    .asArray(*runtime);
  EXPECT_EQ(values.getValueAtIndex(*runtime, 0).getNumber(), 7.0);
  auto asyncFn = values.getValueAtIndex(*runtime, 1)
                     .asObject(*runtime)
                     .asFunction(*runtime);
  EXPECT_TRUE(asyncFn.call(*runtime).isObject());
  auto generatorFn = values.getValueAtIndex(*runtime, 2)
                         .asObject(*runtime)
                         .asFunction(*runtime);
  EXPECT_TRUE(generatorFn.call(*runtime).isObject());
  EXPECT_NE(
      values.getValueAtIndex(*runtime, 3)
          .getString(*runtime)
          .utf8(*runtime)
          .find("asyncFn"),
      std::string::npos);
}

TEST(Bytecode, PreparedScriptIsReusableAcrossRuntimes) {
  // JSI allows a PreparedJavaScript to be shared between runtimes of the same
  // concrete type, so evaluating one must not consume it.
  auto first = qjs::makeQuickJSRuntime();
  auto prepared =
      first->prepareJavaScript(bufferOf("globalThis.answer = 6 * 7;"), "a.js");

  first->evaluatePreparedJavaScript(prepared);
  EXPECT_EQ(first->global().getProperty(*first, "answer").getNumber(), 42.0);

  auto second = qjs::makeQuickJSRuntime();
  second->evaluatePreparedJavaScript(prepared);
  EXPECT_EQ(second->global().getProperty(*second, "answer").getNumber(), 42.0);

  // And the first runtime can still run it again.
  first->evaluatePreparedJavaScript(prepared);
  EXPECT_EQ(first->global().getProperty(*first, "answer").getNumber(), 42.0);
}

TEST(Bytecode, CompiledBytecodeMatchesSourceSemantics) {
  const std::string source =
      "globalThis.result = (function () {"
      "  const xs = [1, 2, 3].map((x) => x * 2);"
      "  return xs.reduce((a, b) => a + b, 0);"
      "})();";

  auto fromSource = qjs::makeQuickJSRuntime();
  fromSource->evaluateJavaScript(bufferOf(source), "a.js");

  auto fromBytecode = qjs::makeQuickJSRuntime();
  fromBytecode->evaluateJavaScript(
      std::make_shared<VectorBuffer>(compileToBytecode(source)), "a.bc");

  EXPECT_EQ(
      fromSource->global().getProperty(*fromSource, "result").getNumber(),
      fromBytecode->global().getProperty(*fromBytecode, "result").getNumber());
}

TEST(Bytecode, CorruptBytecodeIsRejectedCleanly) {
  // The blob checksum exists to turn a damaged asset into an exception rather
  // than undefined behaviour. Corrupt a byte well past the header and require
  // a throw -- if this ever segfaults instead, the checksum has been defeated.
  auto bytes = compileToBytecode("globalThis.answer = 6 * 7;");
  ASSERT_GT(bytes.size(), qjs::kBytecodeHeaderSize + 64);
  bytes[bytes.size() - 8] ^= 0xff;

  // JSIException, not JSError: a blob that fails the gate never becomes a JS
  // value, so the runtime reports it as a native load failure.
  auto runtime = qjs::makeQuickJSRuntime();
  EXPECT_THROW(
      runtime->evaluateJavaScript(
          std::make_shared<VectorBuffer>(std::move(bytes)), "corrupt.bc"),
      jsi::JSIException);
}

TEST(Bytecode, ModuleLoaderRejectsMalformedEntryCounts) {
  // Module sections whose declared entry count is above INT32_MAX or negative. The loader used to record the count before it
  // allocated the table, so freeing the half-built module walked entries nothing backed. Each input must fail with an exception
  // and leave a runtime that frees cleanly (an assertion build or a sanitizer shows the old behaviour).
  const std::vector<std::vector<uint8_t>> inputs = {
      {0x1c, 0x27, 0x9e, 0x68, 0x09, 0x00, 0x0d, 0x7f, 0xff, 0xff, 0xff, 0x7f,
       0x2c, 0x00, 0x00, 0x00, 0x00, 0x0d, 0x7f},
      {0x1c, 0x74, 0x61, 0x89, 0xba, 0x00, 0x0d, 0xbb, 0xff, 0xff, 0xff, 0x32,
       0xb7, 0xfe, 0xff, 0xff, 0x7f, 0x00, 0x00, 0x00, 0x00, 0x32, 0xb7},
  };
  for (const auto &input : inputs) {
    JSRuntime *rt = JS_NewRuntime();
    ASSERT_NE(rt, nullptr);
    // The count is rejected against the input that is left before a table is allocated (F49: the loader used to touch gigabytes first):
    // under this limit an allocation before the check is "out of memory", not "invalid count".
    JS_SetMemoryLimit(rt, 64u << 20);
    JSContext *ctx = JS_NewContext(rt);
    ASSERT_NE(ctx, nullptr);
    JSValue value =
        JS_ReadObject(ctx, input.data(), input.size(), JS_READ_OBJ_BYTECODE);
    EXPECT_TRUE(JS_IsException(value));
    JSValue e = JS_GetException(ctx);
    const char *m = JS_ToCString(ctx, e);
    EXPECT_TRUE(m != nullptr && std::strstr(m, "invalid count") != nullptr)
        << (m ? m : "(no message)");
    if (m) JS_FreeCString(ctx, m);
    JS_FreeValue(ctx, e);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
  }
}

TEST(Bytecode, ModuleLoaderRejectsNonFunctionBody) {
  // A module container whose body (the value after the has_tla byte) is a plain object instead of function bytecode. The module
  // definition owns that object but the collector does not trace the definition, so the object counted as an outside reference
  // and kept the realm alive: the context was never released and JS_FreeRuntime found objects left on the collector's list (an
  // assertion build aborts, a sanitizer build reports the leak). The loader must reject the container.
  const std::vector<uint8_t> input = {
      0x1c, 0x96, 0x64, 0x86, 0xc8, 0x00, 0x0d, 0x01, 0x00, 0x00, 0x00, 0x02,
      0x15, 0x0d, 0x0d, 0x00, 0x08, 0x7f, 0x00, 0x08, 0x7f, 0x00, 0x08, 0x00,
      0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
      0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
      0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
      0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
      0x01, 0x01, 0xfc, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
      0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
      0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
      0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
      0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
      0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
      0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
      0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
      0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
      0x01, 0xf2, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
      0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
      0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
      0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
      0xf2, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
      0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x0a, 0x01, 0x01, 0x01, 0x01,
      0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
      0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
      0x01, 0x01, 0x01,
  };
  JSRuntime *rt = JS_NewRuntime();
  ASSERT_NE(rt, nullptr);
  JSContext *ctx = JS_NewContext(rt);
  ASSERT_NE(ctx, nullptr);
  JSValue value =
      JS_ReadObject(ctx, input.data(), input.size(), JS_READ_OBJ_BYTECODE);
  EXPECT_TRUE(JS_IsException(value));
  JS_FreeValue(ctx, JS_GetException(ctx));
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
}

// The reader stores constants and indices into typed places that the interpreter and the linker later trust. A valid container with
// one byte moved (checksum fixed) must be rejected with a SyntaxError where that byte makes an index leave its table or a closure
// constant stop being function bytecode. The check is reachability: over a handful of byte values at every offset, each message
// must appear at least once. Without the checks the same mutations load and the expected messages never appear.
static JSModuleDef *depLoader(
    JSContext *ctx, const char *name, void *, JSValueConst) {
  static const char src[] = "export var m = 41; export function k() {}";
  JSValue v = JS_Eval(
      ctx, src, sizeof(src) - 1, name,
      JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
  if (JS_IsException(v)) return nullptr;
  JSModuleDef *m = static_cast<JSModuleDef *>(JS_VALUE_GET_PTR(v));
  JS_FreeValue(ctx, v);
  return m;
}

static std::vector<std::string> mutationMessages(
    JSContext *ctx, const char *source, const char *name, int evalFlags) {
  std::vector<std::string> messages;
  JSValue fn = JS_Eval(
      ctx, source, std::strlen(source), name,
      evalFlags | JS_EVAL_FLAG_COMPILE_ONLY);
  EXPECT_FALSE(JS_IsException(fn));
  size_t size = 0;
  uint8_t *buf = JS_WriteObject(ctx, &size, fn, JS_WRITE_OBJ_BYTECODE);
  JS_FreeValue(ctx, fn);
  EXPECT_NE(buf, nullptr);
  if (buf == nullptr) return messages;
  const std::vector<uint8_t> valid(buf, buf + size);
  js_free(ctx, buf);
  static const uint8_t values[] = {0x00, 0x01, 0x02, 0x0d, 0x1f, 0x7f, 0xff};
  for (size_t off = 5; off < valid.size(); ++off) {
    for (uint8_t v : values) {
      if (valid[off] == v) continue;
      std::vector<uint8_t> blob = valid;
      blob[off] = v;
      const uint32_t crc = crc32cReference(blob.data() + 5, blob.size() - 5);
      for (int i = 0; i < 4; ++i)
        blob[1 + i] = static_cast<uint8_t>(crc >> (8 * i));
      JSValue r =
          JS_ReadObject(ctx, blob.data(), blob.size(), JS_READ_OBJ_BYTECODE);
      if (JS_IsException(r)) {
        JSValue e = JS_GetException(ctx);
        if (const char *m = JS_ToCString(ctx, e)) {
          messages.emplace_back(m);
          JS_FreeCString(ctx, m);
        }
        JS_FreeValue(ctx, e);
      } else {
        JS_FreeValue(ctx, r);
      }
    }
  }
  return messages;
}

static bool anyMessageHas(
    const std::vector<std::string> &messages, const char *needle) {
  for (const auto &m : messages)
    if (m.find(needle) != std::string::npos) return true;
  return false;
}

TEST(Bytecode, ReaderRejectsOutOfRangeConstantsAndModuleIndices) {
  JSRuntime *rt = JS_NewRuntime();
  ASSERT_NE(rt, nullptr);
  JS_SetModuleLoaderFunc2(rt, nullptr, depLoader, nullptr, nullptr);
  JSContext *ctx = JS_NewContext(rt);
  ASSERT_NE(ctx, nullptr);
  const auto script = mutationMessages(
      ctx,
      "function a(x){ function b(y){ return y+x; } return "
      "b(2)+[1,2,3].map(function(z){return z*x;}).length; } a(3);",
      "a.js", 0);
  const auto module = mutationMessages(
      ctx,
      "import {m} from 'dep'; import * as ns from 'dep'; export var b = m + 1; "
      "export function g(){ return b + ns.m; }"
      " export {m as mm} from 'dep'; export * from 'dep';",
      "m.mjs", JS_EVAL_TYPE_MODULE);
#if QJS_TEST_REGVM
  EXPECT_TRUE(anyMessageHas(script, "closure constant is not a function"));
#else
  EXPECT_TRUE(anyMessageHas(script, "invalid constant operand"));
#endif
  EXPECT_TRUE(anyMessageHas(module, "invalid module entry index"));
  // A required-module name whose first byte is mutated to 0 decodes as atom 0 (JS_ATOM_NULL, a predefined-atom index): the reader rejects
  // it where it reads it. Without the check the load resolved a module named by the atom-0 entry, whose C string had no terminator
  // (F46: an AddressSanitizer overflow in js_default_module_normalize_name); here the missing message is the failure.
  EXPECT_TRUE(anyMessageHas(module, "invalid module name atom"));
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
}

#if QJS_TEST_REGVM
TEST(Bytecode, RegisterFunctionWithMoreThan65535PropertySitesRoundTrips) {
  // F51: a function with more property reads than the 16-bit site index can name is encoded by the baseline tier, which writes
  // REG_IC_NONE (0xffff) for every site and keeps no site table. The reader rejected that operand ("register PIC site out of
  // range"), so qjsc's own output for such a function (tools/benchmark/octane mandreel) could not be loaded.
  std::string src = "function big(o){";
  for (int i = 0; i < 70000; ++i) src += "o.p;";
  src += "return 1;} big({p:1});";
  JSRuntime *rt = JS_NewRuntime();
  ASSERT_NE(rt, nullptr);
  JSContext *ctx = JS_NewContext(rt);
  ASSERT_NE(ctx, nullptr);
  JSValue fn =
      JS_Eval(ctx, src.data(), src.size(), "big.js", JS_EVAL_FLAG_COMPILE_ONLY);
  ASSERT_FALSE(JS_IsException(fn));
  size_t size = 0;
  uint8_t *buf = JS_WriteObject(ctx, &size, fn, JS_WRITE_OBJ_BYTECODE);
  JS_FreeValue(ctx, fn);
  ASSERT_NE(buf, nullptr);
  JSValue back = JS_ReadObject(ctx, buf, size, JS_READ_OBJ_BYTECODE);
  js_free(ctx, buf);
  ASSERT_FALSE(JS_IsException(back));
  JSValue r = JS_EvalFunction(ctx, back);
  EXPECT_FALSE(JS_IsException(r));
  JS_FreeValue(ctx, r);
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
}
#endif

#if QJS_TEST_REGVM
extern "C" int JS_TestRegvmVarCountOfChild(
    JSContext *ctx, JSValueConst fn, int child);

TEST(Bytecode, RegisterFunctionFromBytecodeKeepsTheCompilersVariableArea) {
  // F52: the compiler promotes locals to registers, so a call needs no variable slots (`regvm_var_count`); the promotion map is not
  // serialized, and the reader gave every blob function all `var_count` slots. Same register program, a bigger frame filled and released on
  // every call: richards/deltablue 6-10% slower loaded from bytecode than from source. The reader derives the area from what the program
  // names and what inner closures capture.
  static const char *src =
      "function promoted(n) { var a = n, b = a + 1, c = b + 1; return a + b + "
      "c; }"
      "function captured(n) { var a = n, b = 2; return function () { return b; "
      "}; }"
      "function named(n) { var a = n; with ({}) { return a; } }"
      "promoted(1);";
  JSRuntime *rt = JS_NewRuntime();
  ASSERT_NE(rt, nullptr);
  JSContext *ctx = JS_NewContext(rt);
  ASSERT_NE(ctx, nullptr);
  JSValue fn =
      JS_Eval(ctx, src, std::strlen(src), "v.js", JS_EVAL_FLAG_COMPILE_ONLY);
  ASSERT_FALSE(JS_IsException(fn));
  size_t size = 0;
  uint8_t *buf = JS_WriteObject(ctx, &size, fn, JS_WRITE_OBJ_BYTECODE);
  ASSERT_NE(buf, nullptr);
  JSValue back = JS_ReadObject(ctx, buf, size, JS_READ_OBJ_BYTECODE);
  js_free(ctx, buf);
  ASSERT_FALSE(JS_IsException(back));
  for (int child = 0; child < 3; ++child) {
    const int fromSource = JS_TestRegvmVarCountOfChild(ctx, fn, child);
    const int fromBytecode = JS_TestRegvmVarCountOfChild(ctx, back, child);
    EXPECT_GE(fromSource, 0) << "child " << child;
    EXPECT_EQ(fromSource, fromBytecode) << "child " << child;
  }
  JS_FreeValue(ctx, fn);
  JS_FreeValue(ctx, back);
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
}
#endif

#if QJS_TEST_REGVM
TEST(Bytecode, RegisterFunctionVariableAreaBelowWhatTheProgramNamesIsRejected) {
  // The writer records the compiler's variable area in the register program's flag byte; the reader accepts it only if it covers every slot the
  // program names and every slot an inner closure captures. A function with a try/catch keeps its three locals in the frame (no promotion),
  // so its area is 4 slots (a, b, c, e): the byte is 1 | (4 + 1) << 1 = 11, followed by REGVM_FORMAT_VERSION. Lowering it must fail the load, not leave slots
  // the program reads outside the area the frame fills.
  static const char *src =
      "function f(n) { var a = n, b = 2, c = 3; try { a = b + c; } catch (e) { "
      "a = 0; } return a + b + c; } f(1);";
  JSRuntime *rt = JS_NewRuntime();
  ASSERT_NE(rt, nullptr);
  JSContext *ctx = JS_NewContext(rt);
  ASSERT_NE(ctx, nullptr);
  JSValue fn =
      JS_Eval(ctx, src, std::strlen(src), "v.js", JS_EVAL_FLAG_COMPILE_ONLY);
  ASSERT_FALSE(JS_IsException(fn));
  size_t size = 0;
  uint8_t *buf = JS_WriteObject(ctx, &size, fn, JS_WRITE_OBJ_BYTECODE);
  JS_FreeValue(ctx, fn);
  ASSERT_NE(buf, nullptr);
  const std::vector<uint8_t> valid(buf, buf + size);
  js_free(ctx, buf);
  int tried = 0, rejected = 0;
  for (size_t off = 5; off + 1 < valid.size(); ++off) {
    // a flag byte with a recorded area of 1 or more slots, followed by the format version byte
    if (valid[off + 1] != 19 || (valid[off] & 1) == 0 || (valid[off] >> 1) < 2)
      continue;
    std::vector<uint8_t> blob = valid;
    blob[off] = 1 | (1 << 1);  // area 0: below what the program names
    const uint32_t crc = crc32cReference(blob.data() + 5, blob.size() - 5);
    for (int i = 0; i < 4; ++i)
      blob[1 + i] = static_cast<uint8_t>(crc >> (8 * i));
    ++tried;
    JSValue r =
        JS_ReadObject(ctx, blob.data(), blob.size(), JS_READ_OBJ_BYTECODE);
    if (JS_IsException(r)) {
      ++rejected;
      JS_FreeValue(ctx, JS_GetException(ctx));
    } else {
      JS_FreeValue(ctx, r);
    }
  }
  EXPECT_GT(tried, 0);
  EXPECT_EQ(tried, rejected);
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
}
#endif

TEST(Engine, ComputeMemoryUsageWalksPlainObjects) {
  // A plain object has no class data, so the walk must not read the union that other classes use for their records (an object
  // with inline property room has no such union). The overread shows under AddressSanitizer; the CDP test that reported it is
  // modules/cdp EveryAdvertisedMethodIsImplemented.
  JSRuntime *rt = JS_NewRuntime();
  ASSERT_NE(rt, nullptr);
  JSContext *ctx = JS_NewContext(rt);
  ASSERT_NE(ctx, nullptr);
  JSValue global = JS_GetGlobalObject(ctx);
  for (int i = 0; i < 64; ++i) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "a", JS_NewInt32(ctx, i));
    JS_SetPropertyStr(ctx, obj, "b", JS_NewInt32(ctx, i * 2));
    JS_SetPropertyStr(ctx, global, i % 2 ? "odd" : "even", obj);
  }
  JS_FreeValue(ctx, global);
  JSMemoryUsage usage;
  JS_ComputeMemoryUsage(rt, &usage);
  EXPECT_GT(usage.obj_count, 0);
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
}

TEST(Bytecode, CRC32CKnownVectorAndInputLengths) {
  const std::vector<uint8_t> knownVector = {'1', '2', '3', '4', '5',
                                            '6', '7', '8', '9'};
  EXPECT_EQ(
      crc32cReference(knownVector.data(), knownVector.size()), 0xe3069283u);

  JSRuntime *rt = JS_NewRuntime();
  ASSERT_NE(rt, nullptr);
  JSContext *ctx = JS_NewContext(rt);
  ASSERT_NE(ctx, nullptr);

  // The first byte is zero where possible, making the malformed body fail
  // quickly after the checksum gate instead of asking the parser to consume
  // an arbitrary atom table. The expected checksum is computed independently
  // so this verifies the engine's selected path, including the full body.
  const std::vector<size_t> lengths = {0, 1, 7, 8, 9, 15, 16, 17, 257, 4097};
  for (size_t length : lengths) {
    std::vector<uint8_t> body(length, 0xa5);
    if (!body.empty() && length != knownVector.size()) {
      body[0] = 0;
    }
    EXPECT_TRUE(engineAcceptsChecksum(ctx, body)) << "length=" << length;
  }
  EXPECT_TRUE(engineAcceptsChecksum(ctx, knownVector));

  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
}

TEST(Bytecode, EngineBuiltinBlobsLoad) {
  // quickjs implements a few builtins as bytecode blobs compiled into the
  // engine itself, and loads them lazily on first use -- so they go through
  // the same version and checksum gate as our own bundles, and a stale
  // regeneration is invisible to every other test in this file.
  auto runtime = qjs::makeQuickJSRuntime();
  runtime->evaluateJavaScript(
      bufferOf("globalThis.result = ["
               "  typeof Array.fromAsync,"
               "  typeof Iterator.zip,"
               "  typeof Iterator.zipKeyed,"
               "  JSON.stringify([...Iterator.zip([[1, 2], [3, 4]])]),"
               "].join('|');"),
      "builtins.js");

  EXPECT_EQ(
      runtime->global()
          .getProperty(*runtime, "result")
          .getString(*runtime)
          .utf8(*runtime),
      "function|function|function|[[1,3],[2,4]]");
}

// --- qjsc --strip-source -------------------------------------------------
//
// Stripping omits the embedded source text of every function, which is 65% of
// the .bc on a real React Native bundle. It needs no format change and no
// BC_VERSION bump, because the per-function source length is already written
// explicitly and stripping writes zero -- so both kinds of blob load on the
// same engine build, and everything except Function.prototype.toString behaves
// identically.

TEST(Bytecode, StrippedBytecodeIsSmallerAndStillRuns) {
  // Something with enough function text for the difference to be unambiguous.
  std::string source = "globalThis.total = 0;\n";
  for (int i = 0; i < 50; i++) {
    source += "function padding" + std::to_string(i) +
              "() { /* ................................................ */ "
              "return " +
              std::to_string(i) + "; }\n";
    source += "globalThis.total += padding" + std::to_string(i) + "();\n";
  }

  auto full = compileToBytecode(source);
  auto stripped = compileToBytecode(source, "--strip-source");

  EXPECT_LT(stripped.size(), full.size())
      << "stripping did not remove anything: full " << full.size()
      << " stripped " << stripped.size();

  auto runtime = qjs::makeQuickJSRuntime();
  runtime->evaluateJavaScript(
      std::make_shared<VectorBuffer>(std::move(stripped)), "stripped.bc");
  EXPECT_EQ(
      runtime->global().getProperty(*runtime, "total").getNumber(), 1225.0);
}

TEST(Bytecode, StrippedAndUnstrippedLoadInTheSameRuntime) {
  // A stripped blob and an unstripped one are the same BC_VERSION, so one
  // runtime must accept both, in either order.
  auto runtime = qjs::makeQuickJSRuntime();
  auto stripped = compileToBytecode("globalThis.a = 6 * 7;", "--strip-source");
  auto full = compileToBytecode("globalThis.b = globalThis.a + 1;");

  runtime->evaluateJavaScript(
      std::make_shared<VectorBuffer>(std::move(stripped)), "a.bc");
  runtime->evaluateJavaScript(
      std::make_shared<VectorBuffer>(std::move(full)), "b.bc");

  EXPECT_EQ(runtime->global().getProperty(*runtime, "a").getNumber(), 42.0);
  EXPECT_EQ(runtime->global().getProperty(*runtime, "b").getNumber(), 43.0);
}

TEST(Bytecode, StrippedBytecodeKeepsLineAndColumnInformation) {
  // The claim the whole feature rests on: line and column come from the
  // pc2line table, which is written under a different writer flag than the
  // source text. If this ever regresses, React Native's red box loses every
  // frame position: QuickJSCompat.cpp routes stacks through parseHermesStack,
  // which parses `file:line:column`.
  const std::string source =
      "function inner() { throw new Error('boom'); }\n"
      "function outer() { return inner(); }\n"
      "function probe() { return 1; }\n"
      "try { outer(); } catch (e) { globalThis.stack = e.stack; }\n"
      "globalThis.loc = probe.lineNumber + ':' + probe.columnNumber;\n";

  const std::string stem = tempStem();
  auto runFor = [&](const char *flags) {
    auto runtime = qjs::makeQuickJSRuntime();
    auto bytes = compileToBytecode(source, flags, stem);
    runtime->evaluateJavaScript(
        std::make_shared<VectorBuffer>(std::move(bytes)), "loc.bc");
    // The filename is the temp path qjsc was given, which is the same for
    // both compiles, so the whole stack string is comparable verbatim.
    return runtime->global()
               .getProperty(*runtime, "stack")
               .getString(*runtime)
               .utf8(*runtime) +
           "\n---\n" +
           runtime->global()
               .getProperty(*runtime, "loc")
               .getString(*runtime)
               .utf8(*runtime);
  };

  const std::string full = runFor("");
  const std::string stripped = runFor("--strip-source");

  EXPECT_EQ(full, stripped);
  // And prove the assertion above is not vacuous.
  EXPECT_NE(full.find("inner ("), std::string::npos) << full;
  EXPECT_NE(full.find(":1:30"), std::string::npos) << full;  // line AND column
  EXPECT_NE(full.find(":2:27"), std::string::npos) << full;
  EXPECT_NE(full.find("\n3:1"), std::string::npos) << full;  // probe's position
}

TEST(Bytecode, StrippedBytecodeDegradesFunctionToString) {
  // What stripping costs, pinned so the trade is explicit rather than
  // discovered. The placeholder is spec-conformant NativeFunction syntax;
  // note that the *kind* is not reflected -- an async function and a class
  // both stringify as `function`, which is also what node does for its
  // built-ins.
  const std::string source =
      "function named(a, b) { return a + b; }\n"
      "async function asyncFn() {}\n"
      "class Klass {}\n"
      "globalThis.out = [String(named), String(asyncFn), String(Klass)]"
      "  .join('~');\n";

  auto runFor = [&](const char *flags) {
    auto runtime = qjs::makeQuickJSRuntime();
    auto bytes = compileToBytecode(source, flags);
    runtime->evaluateJavaScript(
        std::make_shared<VectorBuffer>(std::move(bytes)), "ts.bc");
    return runtime->global()
        .getProperty(*runtime, "out")
        .getString(*runtime)
        .utf8(*runtime);
  };

  EXPECT_EQ(
      runFor(""),
      "function named(a, b) { return a + b; }~"
      "async function asyncFn() {}~"
      "class Klass {}");
  EXPECT_EQ(
      runFor("--strip-source"),
      "function named() {\n    [stripped source]\n}~"
      "function asyncFn() {\n    [stripped source]\n}~"
      "function Klass() {\n    [stripped source]\n}");
}

TEST(Bytecode, SyntaxErrorInSourceThrows) {
  auto runtime = qjs::makeQuickJSRuntime();
  EXPECT_THROW(
      runtime->evaluateJavaScript(bufferOf("this is not javascript"), "bad.js"),
      jsi::JSError);
}

// --- script naming ---------------------------------------------------------
//
// `//# sourceURL=` renames a script in every other engine; quickjs ignores it
// and keeps whatever filename JS_Eval was handed. evaluateJavaScript honours
// the comment so that the name in a stack, and the name a debugger sets a
// breakpoint by, are the same string.

TEST(Bytecode, SourceURLCommentRenamesTheScript) {
  auto runtime = qjs::makeQuickJSRuntime();
  runtime->evaluateJavaScript(
      bufferOf("try { null.x } catch (e) { globalThis.stack = e.stack; }\n"
               "//# sourceURL=renamed.js\n"),
      "original.js");

  const std::string stack = runtime->global()
                                .getProperty(*runtime, "stack")
                                .getString(*runtime)
                                .utf8(*runtime);
  EXPECT_NE(stack.find("renamed.js"), std::string::npos) << stack;
  EXPECT_EQ(stack.find("original.js"), std::string::npos) << stack;
}

TEST(Bytecode, WithoutTheCommentTheEmbedderURLIsKept) {
  auto runtime = qjs::makeQuickJSRuntime();
  runtime->evaluateJavaScript(
      bufferOf("try { null.x } catch (e) { globalThis.stack = e.stack; }"),
      "original.js");

  EXPECT_NE(
      runtime->global()
          .getProperty(*runtime, "stack")
          .getString(*runtime)
          .utf8(*runtime)
          .find("original.js"),
      std::string::npos);
}

TEST(Bytecode, PreparedScriptCarriesTheRenamedURL) {
  // The name is baked in at compile time, so it has to be resolved before
  // JS_Eval rather than at evaluation.
  auto runtime = qjs::makeQuickJSRuntime();
  auto prepared = runtime->prepareJavaScript(
      bufferOf("try { null.x } catch (e) { globalThis.stack = e.stack; }\n"
               "//# sourceURL=renamed.js\n"),
      "original.js");
  runtime->evaluatePreparedJavaScript(prepared);

  EXPECT_NE(
      runtime->global()
          .getProperty(*runtime, "stack")
          .getString(*runtime)
          .utf8(*runtime)
          .find("renamed.js"),
      std::string::npos);
}

TEST(Bytecode, ACommentBeyondTheScanWindowIsNotHonoured) {
  // Only the last 8 KiB are scanned, so the cost is O(1) in bundle size. That
  // is a stated limit rather than an oversight, and this pins it.
  auto runtime = qjs::makeQuickJSRuntime();
  std::string source =
      "//# sourceURL=too-early.js\n"
      "try { null.x } catch (e) { globalThis.stack = e.stack; }\n";
  source += std::string(16384, ' ') + "\n";

  runtime->evaluateJavaScript(bufferOf(source), "original.js");
  EXPECT_NE(
      runtime->global()
          .getProperty(*runtime, "stack")
          .getString(*runtime)
          .utf8(*runtime)
          .find("original.js"),
      std::string::npos);
}
