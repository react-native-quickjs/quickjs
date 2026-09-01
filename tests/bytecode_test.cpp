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

#include <cstdio>
#include <cstdlib>
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

std::string tempPath(const char *suffix) {
  const char *dir = std::getenv("TMPDIR");
  std::string base = dir != nullptr ? dir : "/tmp";
  if (base.back() != '/') {
    base += '/';
  }
  return base + "rnqjs-bytecode-test" + suffix;
}

/// Compiles `source` with the qjsc tool and returns the container bytes.
std::vector<uint8_t> compileToBytecode(
    const std::string &source, const char *extraFlags = "") {
  const std::string jsPath = tempPath(".js");
  const std::string bcPath = tempPath(".bc");

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

  auto runFor = [&](const char *flags) {
    auto runtime = qjs::makeQuickJSRuntime();
    auto bytes = compileToBytecode(source, flags);
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
  // discovered. The placeholder is spec-conformant NativeFunction syntax and
  // matches what node prints for a built-in; note that the *kind* is not
  // reflected -- an async function and a class both stringify as `function`,
  // which is also what node does for its built-ins.
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
      "function named() {\n    [native code]\n}~"
      "function asyncFn() {\n    [native code]\n}~"
      "function Klass() {\n    [native code]\n}");
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
