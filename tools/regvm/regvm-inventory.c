/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * regvm-inventory -- completeness and decode check for the register VM.
 *
 *   regvm-inventory <file.js> [file.js ...]
 *
 * Compiles each file (parse + resolve, no execution) through a QJS_REGVM engine
 * and reports:
 *
 *   - the lowering inventory's completeness: how many of the engine's emitted
 *     opcodes have no lowering (must be zero);
 *   - the decoder's totals over every function compiled: instructions, blocks,
 *     edges, and the per-kind histogram.
 *
 * An unclassified opcode, a reserved opcode, a mis-sized instruction or a bad
 * branch target makes the compile fail with an internal error, so a non-zero
 * exit means the verifier rejected the stream.
 *
 * It links the engine with QJS_REGVM=1; the stack build neither compiles nor
 * exposes these symbols.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs.h"

extern int js_regvm_inventory_unclassified(void);
extern int js_regvm_opcode_count(void);
extern int js_regvm_rop_count(void);
extern int js_regvm_isa_uncovered(void);
extern int js_regvm_encoder_uncovered(void);
extern int regvm_step_uncovered(void);
extern long long js_regvm_stat_encoded_funcs(void);
extern long long js_regvm_stat_unencodable(void);
extern long long js_regvm_stat_encoded_insns(void);
extern long long js_regvm_stat_encoded_bytes(void);
extern long long js_regvm_stat_decoded_insns(void);
extern long long js_regvm_stat_verify_failures(void);
extern long long js_regvm_stat_split_edges(void);
extern int js_regvm_kind_count(void);
extern void js_regvm_stats_reset(void);
extern long long js_regvm_stat_functions(void);
extern long long js_regvm_stat_instructions(void);
extern long long js_regvm_stat_blocks(void);
extern long long js_regvm_stat_edges(void);
extern long long js_regvm_stat_exc_edges(void);
extern long long js_regvm_stat_values(void);
extern long long js_regvm_stat_phis(void);
extern long long js_regvm_stat_deferred(void);
extern long long js_regvm_stat_peak_live(void);
extern long long js_regvm_stat_owned(void);
extern long long js_regvm_stat_borrowed(void);
extern long long js_regvm_stat_cleanup(void);
extern long long js_regvm_stat_max_cleanup(void);
extern long long js_regvm_stat_leaks(void);
extern long long js_regvm_stat_materialize(void);
extern long long js_regvm_stat_edge_releases(void);
extern long long js_regvm_stat_max_regs(void);
extern long long js_regvm_stat_coalesced(void);
extern long long js_regvm_stat_reused(void);
extern long long js_regvm_stat_max_frame_regs(void);
extern long long js_regvm_stat_captured_slots(void);
extern long long js_regvm_stat_reg_bucket(int i);
extern long long js_regvm_stat_max_depth(void);
extern long long js_regvm_stat_elided(void);
extern long long js_regvm_stat_kind(int kind);

static const char *kKindName[] = {
    "UNSET",      "RESERVED",    "ELIDED",  "CONST",     "MOVE",     "CLOSURE",
    "FRAME_READ", "FRAME_WRITE", "UNARY",   "BINARY",    "PROPERTY", "ELEMENT",
    "BRANCH",     "CALL",        "RETURN",  "EXCEPTION", "FINALLY",  "ITERATOR",
    "SUSPEND",    "SCOPE",       "SPECIAL",
};

static char *read_file(const char *path, long *len_out) {
  FILE *f = fopen(path, "rb");
  long len;
  char *buf;
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  len = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (len < 0) {
    fclose(f);
    return NULL;
  }
  buf = malloc((size_t)len + 1);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  if (len > 0 && fread(buf, 1, (size_t)len, f) != (size_t)len) {
    free(buf);
    fclose(f);
    return NULL;
  }
  buf[len] = '\0';
  fclose(f);
  *len_out = len;
  return buf;
}

int main(int argc, char **argv) {
  JSRuntime *rt;
  JSContext *ctx;
  int i, fails = 0, k;

  if (argc < 2) {
    fprintf(stderr, "usage: regvm-inventory <file.js> [file.js ...]\n");
    return 2;
  }

  rt = JS_NewRuntime();
  ctx = JS_NewContext(rt);
  js_regvm_stats_reset();

  for (i = 1; i < argc; i++) {
    long len = 0;
    char *src = read_file(argv[i], &len);
    JSValue v;
    if (!src) {
      fprintf(stderr, "cannot read %s\n", argv[i]);
      fails++;
      continue;
    }
    v = JS_Eval(
        ctx, src, (size_t)len, argv[i],
        JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(v)) {
      /* Test262 has module fixtures; a global compile of one is a syntax
         error, not a verifier failure. Retry as a module before giving up. */
      JS_FreeValue(ctx, JS_GetException(ctx));
      v = JS_Eval(
          ctx, src, (size_t)len, argv[i],
          JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    }
    if (JS_IsException(v)) {
      JSValue e = JS_GetException(ctx);
      const char *s = JS_ToCString(ctx, e);
      fprintf(stderr, "%s: %s\n", argv[i], s ? s : "(unprintable)");
      JS_FreeCString(ctx, s);
      JS_FreeValue(ctx, e);
      fails++;
    }
    JS_FreeValue(ctx, v);
    free(src);
  }

  printf(
      "inventory opcodes=%d kinds=%d unclassified=%d rops=%d isa_uncovered=%d "
      "enc_uncovered=%d step_uncovered=%d\n",
      js_regvm_opcode_count(), js_regvm_kind_count(),
      js_regvm_inventory_unclassified(), js_regvm_rop_count(),
      js_regvm_isa_uncovered(), js_regvm_encoder_uncovered(),
      regvm_step_uncovered());
  printf(
      "regvm funcs=%lld insns=%lld blocks=%lld edges=%lld exc=%lld "
      "values=%lld phis=%lld deferred=%lld peaklive=%lld maxdepth=%lld "
      "elided=%lld\n",
      js_regvm_stat_functions(), js_regvm_stat_instructions(),
      js_regvm_stat_blocks(), js_regvm_stat_edges(), js_regvm_stat_exc_edges(),
      js_regvm_stat_values(), js_regvm_stat_phis(), js_regvm_stat_deferred(),
      js_regvm_stat_peak_live(), js_regvm_stat_max_depth(),
      js_regvm_stat_elided());
  printf(
      "own owned=%lld borrowed=%lld cleanup=%lld maxcleanup=%lld leaks=%lld "
      "materialize=%lld edgerel=%lld maxregs=%lld coalesced=%lld reused=%lld\n",
      js_regvm_stat_owned(), js_regvm_stat_borrowed(), js_regvm_stat_cleanup(),
      js_regvm_stat_max_cleanup(), js_regvm_stat_leaks(),
      js_regvm_stat_materialize(), js_regvm_stat_edge_releases(),
      js_regvm_stat_max_regs(), js_regvm_stat_coalesced(),
      js_regvm_stat_reused());
  for (k = 0; k < js_regvm_kind_count(); k++)
    printf("kind %-12s %lld\n", kKindName[k], js_regvm_stat_kind(k));
  printf(
      "frame max_frame_regs=%lld captured_slots=%lld\n",
      js_regvm_stat_max_frame_regs(), js_regvm_stat_captured_slots());
  printf(
      "enc funcs=%lld unencodable=%lld insns=%lld bytes=%lld decoded=%lld "
      "verifyfail=%lld split_edges=%lld\n",
      js_regvm_stat_encoded_funcs(), js_regvm_stat_unencodable(),
      js_regvm_stat_encoded_insns(), js_regvm_stat_encoded_bytes(),
      js_regvm_stat_decoded_insns(), js_regvm_stat_verify_failures(),
      js_regvm_stat_split_edges());
  printf(
      "regbuckets <=8:%lld <=16:%lld <=32:%lld <=64:%lld <=128:%lld <=256:%lld "
      ">256(wide):%lld\n",
      js_regvm_stat_reg_bucket(0), js_regvm_stat_reg_bucket(1),
      js_regvm_stat_reg_bucket(2), js_regvm_stat_reg_bucket(3),
      js_regvm_stat_reg_bucket(4), js_regvm_stat_reg_bucket(5),
      js_regvm_stat_reg_bucket(6));
  printf("fails=%d\n", fails);

  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
  return fails == 0 && js_regvm_inventory_unclassified() == 0 ? 0 : 1;
}
