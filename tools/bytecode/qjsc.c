/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * Ahead-of-time bytecode compiler.
 *
 * quickjs ships `qjsc`, but it emits a C source array rather than a loadable
 * blob, so it cannot feed a runtime. This tool compiles a file with
 * JS_EVAL_FLAG_COMPILE_ONLY, serialises it with JS_WriteObject, and writes:
 *
 *     [8 bytes magic "NSBCNGS\0"][4 bytes format version LE][JS_WriteObject]
 *
 * That container is shared with NativeScript's compiler, whose output this
 * runtime also accepts.
 *
 * It is built from the engine in this tree rather than shipped prebuilt,
 * because bytecode is only loadable by the exact engine build that produced
 * it.
 *
 *   usage: qjsc [--no-checksum] [--strip-source] <input.js> <output.bc>
 *
 * `--strip-source` drops the embedded source text of every function: 3.30 MB
 * -> 1.16 MB on a 990 KB production Metro bundle, and the same saving in
 * resident memory. It costs Function.prototype.toString, which then reports
 * `[native code]`. Line and column numbers are written under a separate flag
 * and are unaffected, so error.stack is byte-identical either way.
 *
 * JS_WRITE_OBJ_STRIP_DEBUG is deliberately not exposed. It saves a further
 * 0.18 MB and destroys every line and column number in every stack trace,
 * which breaks React Native's red box.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs.h"

#define NSBC_MAGIC "NSBCNGS"
#define NSBC_FORMAT_VERSION 1u
#define NSBC_HEADER_SIZE 12

static uint8_t *read_file(const char *path, size_t *out_len) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    return NULL;
  }
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  long n = ftell(f);
  if (n < 0 || fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return NULL;
  }
  /* JS_Eval requires a NUL-terminated buffer. */
  uint8_t *buf = (uint8_t *)malloc((size_t)n + 1);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  if (got != (size_t)n) {
    free(buf);
    return NULL;
  }
  buf[n] = '\0';
  *out_len = (size_t)n;
  return buf;
}

/* quickjs prefixes its payload with [u8 version][u32 checksum] and verifies the
 * checksum over the whole blob at load time -- a full pass over every byte
 * before any parsing happens. Measured at ~15% of bundle load time.
 *
 * It also honours UINT32_MAX as "do not verify", so writing that sentinel skips
 * the pass with no engine change. That is the right trade here: the bytecode
 * ships inside a signed app bundle, this container already carries its own
 * magic and format version, and BC_VERSION catches engine mismatch. quickjs
 * itself documents JS_READ_OBJ_BYTECODE as trusted-input-only, so the checksum
 * was never the thing standing between us and malicious bytecode.
 *
 * Skipping it is opt-in (--no-checksum), not the default: without it a
 * corrupted blob is undefined behaviour rather than a clean error, and a
 * segfault on a partially-written asset is a bad failure mode. Prefer the
 * hardware-CRC32 engine patch, which keeps the check and most of the speed. */
static void nsbc_clear_checksum(uint8_t *payload, size_t len) {
  if (len >= 5) {
    payload[1] = 0xff;
    payload[2] = 0xff;
    payload[3] = 0xff;
    payload[4] = 0xff;
  }
}

int main(int argc, char **argv) {
  bool keep_checksum = true;
  bool strip_source = false;
  int argi = 1;
  while (argi < argc && argv[argi][0] == '-') {
    if (strcmp(argv[argi], "--no-checksum") == 0) {
      keep_checksum = false;
    } else if (strcmp(argv[argi], "--strip-source") == 0) {
      strip_source = true;
    } else {
      fprintf(stderr, "unknown option %s\n", argv[argi]);
      return 2;
    }
    argi++;
  }
  if (argc - argi != 2) {
    fprintf(
        stderr,
        "usage: %s [--no-checksum] [--strip-source] <input.js> <output.bc>\n",
        argv[0]);
    return 2;
  }
  const char *in_path = argv[argi];
  const char *out_path = argv[argi + 1];

  size_t src_len = 0;
  uint8_t *src = read_file(in_path, &src_len);
  if (!src) {
    fprintf(stderr, "cannot read %s\n", in_path);
    return 1;
  }

  JSRuntime *rt = JS_NewRuntime();
  JSContext *ctx = rt ? JS_NewContext(rt) : NULL;
  if (!ctx) {
    fprintf(stderr, "quickjs init failed\n");
    free(src);
    return 1;
  }

  /* Compile without running. The completion value of the compiled top level is
   * the same function object the runtime would produce from source. */
  JSValue compiled = JS_Eval(
      ctx, (const char *)src, src_len, in_path,
      JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
  free(src);

  if (JS_IsException(compiled)) {
    JSValue exception = JS_GetException(ctx);
    const char *message = JS_ToCString(ctx, exception);
    fprintf(
        stderr, "compile error in %s: %s\n", in_path,
        message ? message : "(unknown)");
    if (message) {
      JS_FreeCString(ctx, message);
    }
    JS_FreeValue(ctx, exception);
    return 1;
  }

  int write_flags = JS_WRITE_OBJ_BYTECODE;
  if (strip_source) {
    /* Source text only. JS_WRITE_OBJ_STRIP_DEBUG is a separate flag and is not
     * set here, so filename/line/column/pc2line all still get written. */
    write_flags |= JS_WRITE_OBJ_STRIP_SOURCE;
  }

  size_t payload_len = 0;
  uint8_t *payload = JS_WriteObject(ctx, &payload_len, compiled, write_flags);
  JS_FreeValue(ctx, compiled);
  if (!payload) {
    fprintf(stderr, "JS_WriteObject failed for %s\n", in_path);
    return 1;
  }

  if (!keep_checksum) {
    nsbc_clear_checksum(payload, payload_len);
  }

  FILE *out = fopen(out_path, "wb");
  if (!out) {
    fprintf(stderr, "cannot write %s\n", out_path);
    js_free(ctx, payload);
    return 1;
  }

  unsigned char header[NSBC_HEADER_SIZE];
  memcpy(header, NSBC_MAGIC, 7);
  header[7] = 0;
  uint32_t version = NSBC_FORMAT_VERSION;
  header[8] = (unsigned char)(version & 0xff);
  header[9] = (unsigned char)((version >> 8) & 0xff);
  header[10] = (unsigned char)((version >> 16) & 0xff);
  header[11] = (unsigned char)((version >> 24) & 0xff);

  int ok =
      (fwrite(header, 1, sizeof(header), out) == sizeof(header)) &&
      (payload_len == 0 || fwrite(payload, 1, payload_len, out) == payload_len);
  fclose(out);
  js_free(ctx, payload);

  if (!ok) {
    fprintf(stderr, "write failed for %s\n", out_path);
    return 1;
  }
  return 0;
}
