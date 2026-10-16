/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * qjsaot2 -- the SSA generator. This driver compiles the inputs, enumerates
 * every function's register program and puts it through the IR build and the
 * verifier. The unit emitter lands on top of the same graph (the lowering);
 * until then this mode is how the IR is checked over whole corpora:
 *
 *   qjsaot2 --verify-ir bundle.js ...      build and verify every function
 *   qjsaot2 --dump-ir bundle.js ...        print the SSA of every function
 *
 * Exit status is non-zero when a function fails to build or verify, so the
 * harness can gate on it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir.h"
#include "quickjs-aot-gen.h"
#include "quickjs-regvm.h"
#include "quickjs.h"

/* The unit builder (selection, profile, table emission and the layout header)
   lives in qjsaot.c. qjsaot2 drives it with its own body emitter so the ABI
   and the selection are shared rather than copied. Emission mode therefore
   hands over to it; --verify-ir and --dump-ir stay here, where the IR lives. */
int qjsaot_driver_main(int argc, char **argv);
int qjsaot2_emit_body(FILE *f, const JSAotGenFunc *g, char hex[33]);
extern int (*qjs_aot_emit_body)(FILE *f, const JSAotGenFunc *g, char hex[33]);

static char *read_file(const char *path, long *len_out) {
  FILE *f = fopen(path, "rb");
  long len;
  char *buf;
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  len = ftell(f);
  fseek(f, 0, SEEK_SET);
  buf = malloc((size_t)len + 1);
  if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) {
    fclose(f);
    free(buf);
    return NULL;
  }
  buf[len] = 0;
  fclose(f);
  *len_out = len;
  return buf;
}

static int digest_cmp(const void *a, const void *b) {
  return memcmp(a, b, 16);
}

int main(int argc, char **argv) {
  JSRuntime *rt;
  JSContext *ctx;
  JSAotGenFunc *funcs = NULL;
  int nfuncs = 0, i;
  int verify = 0, dump = 0, stats = 0;
  const char *profile = NULL;
  int ninputs = 0, fails = 0, nkeep = 0;
  JSValue *keep = NULL;
  long nblocks = 0, ninsns = 0, nvalues = 0, nfuncs_ok = 0, nfuncs_uniq = 0;

  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--verify-ir") == 0)
      verify = 1;
    else if (strcmp(argv[i], "--dump-ir") == 0)
      dump = 1;
    else if (strcmp(argv[i], "--stats") == 0)
      stats = 1;
  }
  if (!verify && !dump && !stats) {
    /* The unit-builder mode: the same CLI as qjsaot, with the SSA body
       emitter. Options are the driver's, so nothing is validated here. */
    qjs_aot_emit_body = qjsaot2_emit_body;
    return qjsaot_driver_main(argc, argv);
  }
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--verify-ir") == 0 ||
        strcmp(argv[i], "--dump-ir") == 0 || strcmp(argv[i], "--stats") == 0)
      continue;
    if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc) {
      profile = argv[++i];
      continue;
    }
    if (argv[i][0] == '-') {
      fprintf(stderr, "qjsaot2: unknown option %s\n", argv[i]);
      return 2;
    }
    ninputs++;
  }
  if (ninputs == 0) {
    fprintf(
        stderr, "usage: qjsaot2 --verify-ir|--dump-ir|--stats bundle.js ...\n");
    return 2;
  }

  rt = JS_NewRuntime();
  ctx = JS_NewContext(rt);
  if (profile) {
    extern int qjs_aot_prof_load(const char *path);
    if (qjs_aot_prof_load(profile) < 0) {
      fprintf(stderr, "qjsaot2: cannot use profile %s\n", profile);
      return 1;
    }
  }
  /* Keep every compiled script alive until after the collection: the
     enumerator walks the runtime's live function list, so a collection in the
     middle of a multi-file build would drop the earlier inputs. */
  keep = malloc(sizeof(*keep) * (size_t)(ninputs > 0 ? ninputs : 1));
  if (!keep) return 2;
  for (i = 1; i < argc; i++) {
    long len = 0;
    char *src;
    JSValue v;
    if (argv[i][0] == '-') {
      /* skip the value of a two-token option */
      if (strcmp(argv[i], "--profile") == 0) i++;
      continue;
    }
    src = read_file(argv[i], &len);
    if (!src) {
      fprintf(stderr, "qjsaot2: cannot read %s\n", argv[i]);
      fails++;
      continue;
    }
    v = JS_Eval(
        ctx, src, (size_t)len, argv[i],
        JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(v)) {
      JS_FreeValue(ctx, JS_GetException(ctx));
      v = JS_Eval(
          ctx, src, (size_t)len, argv[i],
          JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    }
    if (JS_IsException(v)) {
      JSValue e = JS_GetException(ctx);
      const char *s = JS_ToCString(ctx, e);
      fprintf(stderr, "qjsaot2: %s: %s\n", argv[i], s ? s : "(unprintable)");
      JS_FreeCString(ctx, s);
      JS_FreeValue(ctx, e);
      fails++;
    }
    keep[nkeep++] = v;
    free(src);
  }

  nfuncs = js_aot_gen_collect(rt, &funcs);
  for (i = 0; i < nkeep; i++) JS_FreeValue(ctx, keep[i]);
  free(keep);
  qsort(funcs, (size_t)nfuncs, sizeof(*funcs), digest_cmp);

  for (i = 0; i < nfuncs; i++) {
    Q2Func f;
    char msg[256];
    if (funcs[i].eligible != AOT_GEN_ELIGIBLE) continue;
    if (i + 1 < nfuncs && memcmp(funcs[i].digest, funcs[i + 1].digest, 16) == 0)
      continue; /* one body serves both */
    nfuncs_uniq++;
    if (q2_build(&funcs[i], &f) < 0) {
      fprintf(
          stderr, "qjsaot2: %s: build failed: %s\n",
          funcs[i].name[0] ? funcs[i].name : "<anon>", f.why ? f.why : "?");
      q2_free(&f);
      fails++;
      continue;
    }
    if (verify && q2_verify(&f, msg, sizeof(msg)) < 0) {
      fprintf(
          stderr, "qjsaot2: %s: verify failed: %s\n",
          funcs[i].name[0] ? funcs[i].name : "<anon>", msg);
      q2_free(&f);
      fails++;
      continue;
    }
    nfuncs_ok++;
    nblocks += f.nblocks;
    ninsns += f.ninsns;
    nvalues += f.nvalues;
    if (dump) q2_dump(stdout, &f);
    q2_free(&f);
  }

  if (stats) {
    long nfb = 0, nunk = 0, nparams = 0, nparams_before = 0;
    long ndefs = 0, nrec = 0, nrec_int = 0, nrec_float = 0, nrec_num = 0;
    long nrec_other = 0, nrec_none = 0, nprof_hit = 0;
    long byrop[ROP_COUNT];
    int k2;
    memset(byrop, 0, sizeof(byrop));
    for (i = 0; i < nfuncs; i++) {
      Q2Func sf;
      int b, k;
      if (funcs[i].eligible != AOT_GEN_ELIGIBLE) continue;
      if (i + 1 < nfuncs &&
          memcmp(funcs[i].digest, funcs[i + 1].digest, 16) == 0)
        continue;
      if (q2_build(&funcs[i], &sf) < 0) continue;
      if (sf.prof) nprof_hit++;
      nparams_before += sf.nparams_before;
      for (k = 0; k < sf.nvalues; k++) {
        const Q2Value *v = &sf.values[k];
        if (v->insn < 0) continue;
        ndefs++;
        if (!v->has_prof) {
          nrec_none++;
          continue;
        }
        nrec++;
        if (v->ptypes == Q2_T_INT32)
          nrec_int++;
        else if (v->ptypes == Q2_T_NUM)
          nrec_float++;
        else if (v->ptypes == (Q2_T_INT32 | Q2_T_NUM))
          nrec_num++;
        else
          nrec_other++;
      }
      for (b = 0; b < sf.nblocks; b++) {
        nparams += sf.blocks[b].nparams;
        (void)0;
        for (k = 0; k < sf.blocks[b].ninsns; k++) {
          const Q2Insn *in = &sf.insns[sf.blocks[b].first_insn + k];
          if (in->op == Q2_OP_FALLBACK) {
            const Q2RopInfo *ri = q2_rop_info(in->rop);
            nfb++;
            if (ri->ndef == 0 && ri->nuse == 0) {
              nunk++;
              byrop[in->rop]++;
            }
          }
        }
      }
      q2_free(&sf);
    }
    fprintf(stderr, "qjsaot2: prof-hit %ld\n", nprof_hit);
    fprintf(
        stderr,
        "qjsaot2: fallbacks %ld, unknown-role %ld, params %ld (was %ld)\n", nfb,
        nunk, nparams, nparams_before);
    fprintf(
        stderr,
        "qjsaot2: defs %ld, records %ld (int %ld, float %ld, int|float "
        "%ld, other %ld), no record %ld\n",
        ndefs, nrec, nrec_int, nrec_float, nrec_num, nrec_other, nrec_none);
    for (k2 = 0; k2 < ROP_COUNT; k2++)
      if (byrop[k2] > 0)
        fprintf(stderr, "qjsaot2:   unknown rop %d x%ld\n", k2, byrop[k2]);
  }
  fprintf(
      stderr,
      "qjsaot2: %ld/%ld functions%s, %ld blocks, %ld insns, %ld values\n",
      nfuncs_ok, nfuncs_uniq, verify ? " verified" : " built", nblocks, ninsns,
      nvalues);
  js_aot_gen_free(rt, funcs);
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
  return fails ? 1 : 0;
}
