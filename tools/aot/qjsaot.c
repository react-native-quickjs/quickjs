/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * qjsaot -- the AOT unit generator.
 *
 *   qjsaot --out aot_unit.c [--all] [--budget N] [--profile app.qjsprof] bundle.js ...
 *
 * The generator links a QJS_REGVM engine built with -DQJS_AOT_GEN. It compiles
 * each bundle exactly as the app will (same flags, so the same register
 * bytecode and therefore the same digest), enumerates every eligible function,
 * and emits a translation unit of step-only bodies plus the digest-sorted
 * binding table. Hot opcodes get inline templates (Layer A) and typed loop
 * regions (Layer B); every other instruction calls qjs_aot_step.
 *
 * `--all` emits every eligible function; `--profile`/`--budget` select a
 * subset (see the selection section below).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs-aot-gen.h"
#include "quickjs-aot-rt.h"
#include "quickjs-regvm.h"
#include "quickjs.h"

/* Mutation-testing hooks. Each selects a deliberately wrong
 * lowering so the guard it bypasses can be shown to be load-bearing. Set with
 * `--mut <name>`; NULL means a correct build. */
static const char *g_mut = NULL;
static int mut_is(const char *s) {
  return g_mut != NULL && strcmp(g_mut, s) == 0;
}

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

static int digest_cmp(const void *a, const void *b) {
  const JSAotGenFunc *x = a, *y = b;
  return memcmp(x->digest, y->digest, 16);
}

static void hex16(const uint8_t *d, char out[33]) {
  static const char hx[] = "0123456789abcdef";
  int i;
  for (i = 0; i < 16; i++) {
    out[i * 2] = hx[d[i] >> 4];
    out[i * 2 + 1] = hx[d[i] & 15];
  }
  out[32] = '\0';
}

/* --- refusal census ------------------------------------------------------ */

static const char *reason_name(int r) {
  switch (r) {
    case AOT_GEN_ELIGIBLE:
      return "eligible";
    case AOT_GEN_REF_LAZY:
      return "lazy";
    case AOT_GEN_REF_REGVM:
      return "regvm_refused";
    case AOT_GEN_REF_EMPTY:
      return "empty";
    case AOT_GEN_REF_TOO_BIG:
      return "too_big";
    default:
      return "?";
  }
}

static int hexval(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int hex_to_bytes16(const char *s, uint8_t out[16]) {
  int i;
  for (i = 0; i < 16; i++) {
    int hi = hexval((unsigned char)s[i * 2]);
    int lo = hexval((unsigned char)s[i * 2 + 1]);
    if (hi < 0 || lo < 0) return -1;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return 0;
}

/* Type classes the profiler records per register (mirrors quickjs.c's
   AOT_TYPE_*). A register is a numeric-region candidate only when every bit it
   ever carried is inside the arithmetic set; the emitted code still guards. */
#define PT_FLOAT 0x01u
#define PT_INT 0x02u
#define PT_OBJECT 0x04u
#define PT_STRING 0x08u
#define PT_BOOL 0x10u
#define PT_UNDEF 0x20u
#define PT_BIGINT 0x40u
#define PT_OTHER 0x80u
#define PT_ARITH (PT_FLOAT | PT_INT)

typedef struct {
  int pc;
  long long iters;
} ProfLoop;
typedef struct {
  int reg;
  unsigned types;
} ProfReg;
typedef struct {
  int pc;
  unsigned long long hits; /* the dominant callee's hit count */
  int kind;                /* 1 bytecode, 2 C, 3 other */
  int cproto, magic;       /* kind 2 */
  int argc_min, argc_max;
  char digest[33]; /* kind 1: the callee digest hex */
} ProfCall;

typedef struct {
  uint8_t digest[16];
  unsigned long long ops;
  ProfLoop *loops;
  int nloops, caploops;
  ProfReg *regs;
  int nregs, capregs;
  ProfCall *calls;
  int ncalls, capcalls;
} ProfEntry;

/* The parsed profile (for call-site specialisation) and the current function's
   entry, set by emit_func; `g_site_next` hands out one AOT site id per emitted
   call site (the engine's per-runtime binding array is indexed by it). */
static const ProfEntry *g_prof = NULL;
static int g_nprof = 0;
static const ProfEntry *g_cur_prof = NULL;
static int g_site_next = 0;

static unsigned prof_type_bit(const char *name) {
  if (!strcmp(name, "float")) return PT_FLOAT;
  if (!strcmp(name, "int")) return PT_INT;
  if (!strcmp(name, "object")) return PT_OBJECT;
  if (!strcmp(name, "string")) return PT_STRING;
  if (!strcmp(name, "bool")) return PT_BOOL;
  if (!strcmp(name, "undef")) return PT_UNDEF;
  if (!strcmp(name, "bigint")) return PT_BIGINT;
  if (!strcmp(name, "other")) return PT_OTHER;
  return PT_OTHER;
}

static unsigned prof_parse_types(const char *s) {
  unsigned t = 0;
  char buf[64];
  int j = 0;
  for (;; s++) {
    if (*s == '|' || *s == '\0' || *s == '\n' || *s == ' ') {
      if (j) {
        buf[j] = '\0';
        t |= prof_type_bit(buf);
        j = 0;
      }
      if (*s == '\0' || *s == '\n') break;
    } else if (j < (int)sizeof(buf) - 1) {
      buf[j++] = *s;
    }
  }
  return t;
}

static ProfEntry *prof_get(
    ProfEntry **parr, int *pn, int *pcap, const uint8_t dg[16]) {
  ProfEntry *arr = *parr;
  int i;
  for (i = 0; i < *pn; i++)
    if (memcmp(arr[i].digest, dg, 16) == 0) return &arr[i];
  if (*pn == *pcap) {
    ProfEntry *na;
    *pcap = *pcap ? *pcap * 2 : 64;
    na = realloc(arr, sizeof(*arr) * (size_t)*pcap);
    if (!na) {
      free(arr);
      *parr = NULL;
      return NULL;
    }
    arr = na;
    *parr = arr;
  }
  memset(&arr[*pn], 0, sizeof(arr[*pn]));
  memcpy(arr[*pn].digest, dg, 16);
  (*pn)++;
  return &arr[*pn - 1];
}

/* Parse a .qjsprof file: `F` (function + ops), `L` (loop pc + trip count) and
   `R` (register type set) lines, summed/merged per digest. The header carries
   the engine identity; a profile from another engine or build configuration is
   refused rather than silently matching nothing. Returns a malloc'd array (may
   be empty) or NULL. */
static ProfEntry *parse_profile(const char *path, int *pn) {
  FILE *f = fopen(path, "r");
  ProfEntry *arr = NULL;
  int n = 0, cap = 0;
  char line[8192];
  *pn = 0;
  if (!f) return NULL;
  if (fgets(line, sizeof(line), f)) {
    if (strncmp(line, "qjsprof ", 8) == 0) {
      const char *p = strstr(line, "abi=");
      const char *fp = strstr(line, "fmt=");
      if (p && strtoul(p + 4, NULL, 10) != (unsigned long)QJS_AOT_ABI) {
        fprintf(stderr, "qjsaot: %s: ABI mismatch (profile %lu, engine %lu); "
                        "collect the profile with this engine build\n",
                path, strtoul(p + 4, NULL, 10), (unsigned long)QJS_AOT_ABI);
        fclose(f);
        return NULL;
      }
      if (fp && strtoul(fp + 4, NULL, 10) != (unsigned long)REGVM_FORMAT_VERSION) {
        fprintf(stderr, "qjsaot: %s: register-format mismatch (profile %lu, "
                        "engine %u); collect the profile with this engine build\n",
                path, strtoul(fp + 4, NULL, 10), (unsigned)REGVM_FORMAT_VERSION);
        fclose(f);
        return NULL;
      }
    }
  }
  while (fgets(line, sizeof(line), f)) {
    uint8_t dg[16];
    ProfEntry *e;
    if ((line[0] != 'F' && line[0] != 'L' && line[0] != 'R' &&
         line[0] != 'C') ||
        line[1] != ' ')
      continue;
    if (hex_to_bytes16(line + 2, dg) < 0) continue;
    e = prof_get(&arr, &n, &cap, dg);
    if (!e) return NULL;
    if (line[0] == 'F') {
      char *p = strstr(line + 2, "ops=");
      if (p) e->ops += strtoull(p + 4, NULL, 10);
    } else if (line[0] == 'L') {
      char *p = strstr(line + 2, "pc=");
      if (p) {
        long long it = 0;
        char *q = strstr(p, "iters=");
        if (q) it = strtoll(q + 6, NULL, 10);
        if (e->nloops == e->caploops) {
          ProfLoop *nl;
          e->caploops = e->caploops ? e->caploops * 2 : 8;
          nl = realloc(e->loops, sizeof(*nl) * (size_t)e->caploops);
          if (!nl) return NULL;
          e->loops = nl;
        }
        e->loops[e->nloops].pc = (int)strtol(p + 3, NULL, 10);
        e->loops[e->nloops].iters = it;
        e->nloops++;
      }
    } else if (line[0] == 'C') {
      /* C <hex> pc= count= argc=min..max callee=... hits= ; keep the dominant
         callee per pc (a site is usually monomorphic). */
      char *p = strstr(line + 2, "pc=");
      char *h = strstr(line + 2, "hits=");
      char *a = strstr(line + 2, "argc=");
      char *cl = strstr(line + 2, "callee=");
      if (p && h && cl) {
        int pc = (int)strtol(p + 3, NULL, 10);
        unsigned long long hits = strtoull(h + 5, NULL, 10);
        int k, amin = 0, amax = 0, kind = 3, cproto = -1, magic = 0;
        char cd[33] = "";
        if (a) sscanf(a + 5, "%d..%d", &amin, &amax);
        if (!strncmp(cl + 7, "bc:", 3)) {
          kind = 1;
          if (hex_to_bytes16(cl + 10, dg) == 0) {
            int z;
            for (z = 0; z < 16; z++) sprintf(cd + z * 2, "%02x", dg[z]);
            cd[32] = 0;
          }
        } else if (!strncmp(cl + 7, "c:", 2)) {
          char *mg = strstr(cl, ":magic=");
          char *cp = strstr(cl, ":cproto=");
          kind = 2;
          if (mg) magic = (int)strtol(mg + 7, NULL, 10);
          if (cp) cproto = (int)strtol(cp + 8, NULL, 10);
        }
        for (k = 0; k < e->ncalls; k++)
          if (e->calls[k].pc == pc) break;
        if (k == e->ncalls) {
          if (e->ncalls == e->capcalls) {
            ProfCall *nc;
            e->capcalls = e->capcalls ? e->capcalls * 2 : 16;
            nc = realloc(e->calls, sizeof(*nc) * (size_t)e->capcalls);
            if (!nc) return NULL;
            e->calls = nc;
          }
          e->ncalls++;
          memset(&e->calls[k], 0, sizeof(e->calls[k]));
          e->calls[k].pc = pc;
        }
        if (hits >= e->calls[k].hits) {
          e->calls[k].hits = hits;
          e->calls[k].kind = kind;
          e->calls[k].cproto = cproto;
          e->calls[k].magic = magic;
          e->calls[k].argc_min = amin;
          e->calls[k].argc_max = amax;
          snprintf(e->calls[k].digest, sizeof(e->calls[k].digest), "%s", cd);
        }
      }
    } else { /* R */
      char *p = strstr(line + 2, "reg=");
      char *q = strstr(line + 2, "types=");
      if (p && q) {
        int reg = (int)strtol(p + 4, NULL, 10);
        unsigned t = prof_parse_types(q + 6);
        int k;
        for (k = 0; k < e->nregs; k++)
          if (e->regs[k].reg == reg) {
            e->regs[k].types |= t;
            break;
          }
        if (k == e->nregs) {
          if (e->nregs == e->capregs) {
            ProfReg *nr;
            e->capregs = e->capregs ? e->capregs * 2 : 16;
            nr = realloc(e->regs, sizeof(*nr) * (size_t)e->capregs);
            if (!nr) return NULL;
            e->regs = nr;
          }
          e->regs[e->nregs].reg = reg;
          e->regs[e->nregs].types = t;
          e->nregs++;
        }
      }
    }
  }
  fclose(f);
  *pn = n;
  return arr;
}

static const ProfEntry *prof_entry(
    const ProfEntry *arr, int n, const uint8_t dg[16]) {
  int i;
  for (i = 0; i < n; i++)
    if (memcmp(arr[i].digest, dg, 16) == 0) return &arr[i];
  return NULL;
}

/* The dominant profiled callee at `pc`, or NULL. */
static const ProfCall *prof_call_at(const ProfEntry *e, int pc) {
  int i;
  if (!e) return NULL;
  for (i = 0; i < e->ncalls; i++)
    if (e->calls[i].pc == pc) return &e->calls[i];
  return NULL;
}

static unsigned long long prof_lookup(
    const ProfEntry *arr, int n, const uint8_t dg[16]) {
  int i;
  for (i = 0; i < n; i++)
    if (memcmp(arr[i].digest, dg, 16) == 0) return arr[i].ops;
  return 0;
}

/* Count register instructions in an eligible program. */
static long long count_insns(const uint8_t *code, int len) {
  long long n = 0;
  int off = 0;
  while (off < len) {
    int op, sz = js_aot_gen_insn_size(code, len, off, &op);
    if (sz < 0) break;
    n++;
    off += sz;
  }
  return n;
}

static void print_census(
    const JSAotGenFunc *funcs, int nfuncs, const ProfEntry *prof, int nprof) {
  long long cnt[AOT_GEN_REF_REASON_COUNT];
  long long insns[AOT_GEN_REF_REASON_COUNT];
  long long stack[AOT_GEN_REF_REASON_COUNT];
  long long pops[AOT_GEN_REF_REASON_COUNT];
  long long tot_insns = 0, tot_stack = 0, tot_pops = 0;
  long long ref_pops = 0, ref_funcs = 0;
  int i, r;

  memset(cnt, 0, sizeof(cnt));
  memset(insns, 0, sizeof(insns));
  memset(stack, 0, sizeof(stack));
  memset(pops, 0, sizeof(pops));
  for (i = 0; i < nfuncs; i++) {
    r = funcs[i].eligible;
    if (r < 0 || r >= AOT_GEN_REF_REASON_COUNT) r = AOT_GEN_REF_REGVM;
    cnt[r]++;
    if (r == AOT_GEN_ELIGIBLE)
      insns[r] += count_insns(funcs[i].code, funcs[i].len);
    else
      stack[r] += funcs[i].stack_len;
    if (nprof > 0) {
      unsigned long long o = prof_lookup(prof, nprof, funcs[i].digest);
      pops[r] += (long long)o;
      tot_pops += (long long)o;
    }
  }
  for (r = 0; r < AOT_GEN_REF_REASON_COUNT; r++) {
    tot_insns += insns[r];
    tot_stack += stack[r];
    if (r != AOT_GEN_ELIGIBLE) {
      ref_pops += pops[r];
      ref_funcs += cnt[r];
    }
  }

  /* A profile entry whose digest is not in the unit is the sharpest measure of
     refused work: it ran on the register VM but the generator did not emit a
     body for it. (Functions the register backend itself refused never reach the
     profile, so they cannot be weighted here.) */
  {
    int j;
    for (j = 0; j < nprof; j++) {
      int k, hit = 0;
      for (k = 0; k < nfuncs; k++)
        if (funcs[k].eligible == AOT_GEN_ELIGIBLE &&
            memcmp(funcs[k].digest, prof[j].digest, 16) == 0) {
          hit = 1;
          break;
        }
      if (!hit) {
        ref_pops += (long long)prof[j].ops;
        tot_pops += (long long)prof[j].ops;
        fprintf(
            stderr,
            "qjsaot: profiled digest %02x%02x%02x%02x... has no unit body "
            "(%llu ops)\n",
            prof[j].digest[0], prof[j].digest[1], prof[j].digest[2],
            prof[j].digest[3], prof[j].ops);
      }
    }
  }

  fprintf(stderr, "qjsaot: refusal census\n");
  fprintf(
      stderr, "  %-14s %8s %12s %12s %12s\n", "reason", "funcs", "reginsns",
      "stackB", "profops");
  for (r = 0; r < AOT_GEN_REF_REASON_COUNT; r++) {
    if (cnt[r] == 0) continue;
    fprintf(
        stderr, "  %-14s %8lld %12lld %12lld %12lld\n", reason_name(r), cnt[r],
        insns[r], stack[r], pops[r]);
  }
  fprintf(
      stderr, "  %-14s %8d %12lld %12lld %12lld\n", "total", nfuncs, tot_insns,
      tot_stack, tot_pops);
  fprintf(
      stderr, "qjsaot: refused functions %lld/%d (%.2f%%)", ref_funcs, nfuncs,
      nfuncs ? 100.0 * (double)ref_funcs / nfuncs : 0.0);
  if (nprof > 0)
    fprintf(
        stderr, ", refused profiled ops %lld/%lld (%.3f%%)", ref_pops, tot_pops,
        tot_pops ? 100.0 * (double)ref_pops / (double)tot_pops : 0.0);
  fprintf(stderr, "\n");
}

/* The shared step slow path for one instruction: run the op through the step
   engine and route the result (the slow path is always the step). */
static void emit_fallback(FILE *f, int off, int next, int last) {
  fprintf(f, "    ip = qjs_aot_step(code + %d, fr, f, ctx);\n", off);
  fprintf(
      f,
      "    if (__builtin_expect(ip == NULL, 0)) { pc = %d; goto exception; }\n",
      off);
  fprintf(f, "    if (ip == QJS_AOT_COLD_RET) return qjs_aot_ret(f);\n");
  fprintf(
      f,
      "    if (__builtin_expect(ip == QJS_AOT_SUSPEND, 0)) return "
      "qjs_aot_ret(f);\n");
  if (!last)
    fprintf(
        f, "    if (__builtin_expect(ip == code + %d, 1)) goto pc_%d;\n", next,
        next);
  fprintf(f, "    pc = (uint32_t)(ip - code);\n    goto dispatch_ip;\n");
}

/* The REG_BIN_* sub-op of a per-sense short branch, or -1 for any other op. */
static int branch_subop(int op) {
  switch (op) {
    case ROP_branch_lt_s:
      return REG_BIN_LT;
    case ROP_branch_nlt_s:
      return REG_BIN_LT | REG_CMP_INVERT;
    case ROP_branch_lte_s:
      return REG_BIN_LTE;
    case ROP_branch_nlte_s:
      return REG_BIN_LTE | REG_CMP_INVERT;
    case ROP_branch_gt_s:
      return REG_BIN_GT;
    case ROP_branch_ngt_s:
      return REG_BIN_GT | REG_CMP_INVERT;
    case ROP_branch_gte_s:
      return REG_BIN_GTE;
    case ROP_branch_ngte_s:
      return REG_BIN_GTE | REG_CMP_INVERT;
    case ROP_branch_eq_s:
      return REG_BIN_EQ;
    case ROP_branch_neq_s:
      return REG_BIN_NEQ;
    default:
      return -1;
  }
}

/* The REG_BIN_* sub-op of a "rrr" binary op (excluding add/sub/mul, which have
   their own immediate-friendly template), or -1 for any other op. */
static int binop_subop(int op) {
  switch (op) {
    case ROP_div:
      return REG_BIN_DIV;
    case ROP_mod:
      return REG_BIN_MOD;
    case ROP_pow:
      return REG_BIN_POW;
    case ROP_bitand:
      return REG_BIN_AND;
    case ROP_bitor:
      return REG_BIN_OR;
    case ROP_bitxor:
      return REG_BIN_XOR;
    case ROP_shl:
      return REG_BIN_SHL;
    case ROP_sar:
      return REG_BIN_SAR;
    case ROP_shr:
      return REG_BIN_SHR;
    case ROP_lt:
      return REG_BIN_LT;
    case ROP_lte:
      return REG_BIN_LTE;
    case ROP_gt:
      return REG_BIN_GT;
    case ROP_gte:
      return REG_BIN_GTE;
    case ROP_eq:
      return REG_BIN_EQ;
    case ROP_neq:
      return REG_BIN_NEQ;
    case ROP_strict_eq:
      return REG_BIN_STRICT_EQ;
    case ROP_strict_neq:
      return REG_BIN_STRICT_NEQ;
    case ROP_instanceof:
      return REG_BIN_INSTANCEOF;
    case ROP_in:
      return REG_BIN_IN;
    default:
      return -1;
  }
}

/* The REG_UN_* sub-op of a "rr" unary op, or -1 for any other op. */
static int unop_subop(int op) {
  switch (op) {
    case ROP_neg:
      return REG_UN_NEG;
    case ROP_inc:
      return REG_UN_INC;
    case ROP_dec:
      return REG_UN_DEC;
    case ROP_not:
      return REG_UN_NOT;
    case ROP_to_numeric:
      return REG_UN_TO_NUMERIC;
    default:
      return -1;
  }
}

/*
 * Emit the Layer A fast path for one instruction. Returns 1 when it emitted
 * the whole `pc_N:` block (including the step fallback where the fast path can
 * miss), 0 to let the caller emit the plain step body. Every fast path mirrors
 * the named interpreter handler exactly; a miss falls through to the step.
 */
static int emit_template(
    FILE *f, const JSAotGenInsn *in, int off, int next, int last) {
  int d, a, b, site, sub;

  switch (in->op) {
    /* get_local / get_local_s0..s3: SET_REG_B(d, var_buf[slot]). */
    case ROP_get_local:
      d = (int)in->arg[0].val;
      fprintf(
          f, "    AOT_SET_B(ctx, fr, %d, vb[%d]);\n    goto pc_%d;\n", d,
          (int)in->arg[1].val, next);
      return 1;
    case ROP_get_local_s0:
    case ROP_get_local_s1:
    case ROP_get_local_s2:
    case ROP_get_local_s3:
      d = (int)in->arg[0].val;
      fprintf(
          f, "    AOT_SET_B(ctx, fr, %d, vb[%d]);\n    goto pc_%d;\n", d,
          in->op - ROP_get_local_s0, next);
      return 1;

    /* add/sub/mul: REGVM_BIN_OP over regvm_num_fast, step on a miss. */
    case ROP_add:
    case ROP_sub:
    case ROP_mul:
      sub = in->op == ROP_add   ? REG_BIN_ADD
            : in->op == ROP_sub ? REG_BIN_SUB
                                : REG_BIN_MUL;
      d = (int)in->arg[0].val;
      a = (int)in->arg[1].val;
      b = (int)in->arg[2].val;
      fprintf(f, "    { JSValue _o;\n");
      fprintf(
          f,
          "      if (qjs_aot_num_fast(%d, AOT_REG(fr, %d), AOT_REG(fr, %d), "
          "&_o)) {\n",
          sub, a, b);
      fprintf(
          f, "          AOT_SET_B(ctx, fr, %d, _o); goto pc_%d; } }\n", d,
          next);
      emit_fallback(f, off, next, last);
      return 1;

    /* get_field / get_field_own / get_field2 / get_field2_own: own_read_hit.
     get_field2 is the "load a method for call_method" form, so it also tries
     the folded-prototype read first. */
    case ROP_get_field:
    case ROP_get_field_own:
    case ROP_get_field2:
    case ROP_get_field2_own:
      d = (int)in->arg[0].val;
      a = (int)in->arg[1].val;
      site = (int)in->arg[3].val;
      /* No site (a baseline-tier function has no PIC table): the step does the
         generic read, as the interpreter handler does. */
      if ((unsigned)site == REG_IC_NONE) return 0;
      if (in->op == ROP_get_field2 || in->op == ROP_get_field2_own) {
        int sid = g_site_next++;
        fprintf(f, "    { JSValue _o;\n");
        fprintf(
            f,
            "      if (qjs_aot_proto_fold(ctx, f, pic, %d, AOT_REG(fr, %d), "
            "%d, &_o)) {\n",
            sid, a, site);
        fprintf(
            f,
            "          qjs_aot_set_reg_p(ctx, fr, %d, _o); goto pc_%d; } }\n",
            d, next);
      }
      fprintf(f, "    { JSValue _o;\n");
      fprintf(
          f,
          "      if (qjs_aot_own_read_hit(f, pic, AOT_REG(fr, %d), %d, &_o)) "
          "{\n",
          a, site);
      fprintf(
          f, "          AOT_SET_B(ctx, fr, %d, _o); goto pc_%d; } }\n", d,
          next);
      emit_fallback(f, off, next, last);
      return 1;

    /* call / call_method: the specialised C-call path when the profile shows
     a C callee at this pc; the direct compiled path when it shows a bytecode
     callee; otherwise (and on a miss) the step engine. */
    case ROP_call:
    case ROP_call_method: {
      const ProfCall *pcl = prof_call_at(g_cur_prof, next);
      if (pcl && pcl->kind == 2) {
        int is_m = in->op == ROP_call_method;
        int sid = g_site_next++;
        int dc = (int)in->arg[0].val;
        int cc = (int)in->arg[1].val;
        int rc = is_m ? (int)in->arg[2].val : -1;
        int abc = (int)in->arg[is_m ? 3 : 2].val;
        int nac = (int)in->arg[is_m ? 4 : 3].val;
        fprintf(f, "    { JSValue _o;\n");
        fprintf(
            f,
            "      int _h = qjs_aot_call_c(ctx, %d, %d, %d, AOT_REG(fr, %d), ",
            sid, pcl->cproto, nac, cc);
        if (is_m)
          fprintf(f, "AOT_REG(fr, %d), ", rc);
        else
          fprintf(f, "JS_UNDEFINED, ");
        fprintf(f, "&AOT_REG(fr, %d), &_o);\n", abc);
        fprintf(
            f,
            "      if (_h > 0) { qjs_aot_set_reg_p(ctx, fr, %d, _o); goto "
            "pc_%d; }\n",
            dc, next);
        fprintf(f, "      if (_h < 0) { pc = %d; goto exception; } }\n", off);
      } else if (pcl && pcl->kind == 1) {
        int is_m = in->op == ROP_call_method;
        int dc = (int)in->arg[0].val;
        int cc = (int)in->arg[1].val;
        int rc = is_m ? (int)in->arg[2].val : -1;
        int abc = (int)in->arg[is_m ? 3 : 2].val;
        int nac = (int)in->arg[is_m ? 4 : 3].val;
        fprintf(f, "    { JSValue _o;\n");
        fprintf(
            f, "      int _h = qjs_aot_call_b(ctx, %d, AOT_REG(fr, %d), ", nac,
            cc);
        if (is_m)
          fprintf(f, "AOT_REG(fr, %d), ", rc);
        else
          fprintf(f, "JS_UNDEFINED, ");
        fprintf(f, "&AOT_REG(fr, %d), &_o);\n", abc);
        fprintf(
            f,
            "      if (_h > 0) { qjs_aot_set_reg_p(ctx, fr, %d, _o); goto "
            "pc_%d; }\n",
            dc, next);
        fprintf(f, "      if (_h < 0) { pc = %d; goto exception; } }\n", off);
      }
      emit_fallback(f, off, next, last);
      return 1;
    }

    /* move/move_s: SET_REG_B(d, regs[s]) unless d == s (self-move is a no-op). */
    case ROP_move:
    case ROP_move_s:
      d = (int)in->arg[0].val;
      a = (int)in->arg[1].val;
      if (d == a)
        fprintf(f, "    goto pc_%d;\n", next);
      else
        fprintf(
            f,
            "    AOT_SET_B(ctx, fr, %d, AOT_REG(fr, %d));\n    goto pc_%d;\n",
            d, a, next);
      return 1;

    /* load_i8 / load_i8_s / load_i32: SET_REG_P(d, constant int). */
    case ROP_load_i8:
    case ROP_load_i8_s:
      d = (int)in->arg[0].val;
      fprintf(
          f,
          "    qjs_aot_set_reg_p(ctx, fr, %d, JS_MKVAL(JS_TAG_INT, %d));\n"
          "    goto pc_%d;\n",
          d, (int)(int8_t)in->arg[1].val, next);
      return 1;
    case ROP_load_i32:
      d = (int)in->arg[0].val;
      fprintf(
          f,
          "    qjs_aot_set_reg_p(ctx, fr, %d, JS_MKVAL(JS_TAG_INT, "
          "(int32_t)%d));\n"
          "    goto pc_%d;\n",
          d, (int32_t)in->arg[1].val, next);
      return 1;

    /* get_arg: SET_REG_B(d, arg_buf[slot]). */
    case ROP_get_arg:
      d = (int)in->arg[0].val;
      fprintf(
          f, "    AOT_SET_B(ctx, fr, %d, ab[%d]);\n    goto pc_%d;\n", d,
          (int)in->arg[1].val, next);
      return 1;

    /* put_local / put_local_s0..s3: set_value(ctx, &var_buf[slot], dup(regs[s])). */
    case ROP_put_local:
      fprintf(
          f,
          "    qjs_aot_put_local(ctx, vb, %d, AOT_REG(fr, %d));\n    goto "
          "pc_%d;\n",
          (int)in->arg[0].val, (int)in->arg[1].val, next);
      return 1;
    case ROP_put_local_s0:
    case ROP_put_local_s1:
    case ROP_put_local_s2:
    case ROP_put_local_s3:
      fprintf(
          f,
          "    qjs_aot_put_local(ctx, vb, %d, AOT_REG(fr, %d));\n    goto "
          "pc_%d;\n",
          in->op - ROP_put_local_s0, (int)in->arg[0].val, next);
      return 1;

    /* get_global: cell-cache probe then JS_GetGlobalVar (full semantics). The
     atom is runtime-remapped, so re-read it from the live code rather than
     baking the generator's atom id. */
    case ROP_get_global:
      d = (int)in->arg[0].val;
      fprintf(f, "    { JSValue _o;\n");
      fprintf(
          f,
          "      if (qjs_aot_get_global(ctx, (int)qjs_aot_ld_u32(code + %d), "
          "%d, &_o) < 0) goto exception;\n",
          in->arg[1].off, (int)in->arg[2].val);
      fprintf(
          f, "      qjs_aot_set_reg_p(ctx, fr, %d, _o); goto pc_%d; }\n", d,
          next);
      return 1;

    /* put_field: the REG_PIC_OWN_W direct-slot store, else the step. */
    case ROP_put_field:
      a = (int)in->arg[0].val;
      site = (int)in->arg[2].val;
      b = (int)in->arg[3].val;
      if ((unsigned)site == REG_IC_NONE) return 0;
      fprintf(
          f,
          "    if (qjs_aot_put_field_own(ctx, f, AOT_REG(fr, %d), %d, "
          "AOT_REG(fr, %d)))\n"
          "        goto pc_%d;\n",
          a, site, b, next);
      emit_fallback(f, off, next, last);
      return 1;

    /* get_element: the plain-array fast path, else the step. */
    case ROP_get_element:
      d = (int)in->arg[0].val;
      a = (int)in->arg[1].val;
      b = (int)in->arg[2].val;
      fprintf(f, "    { JSValue _o;\n");
      fprintf(
          f,
          "      if (qjs_aot_get_element(AOT_REG(fr, %d), AOT_REG(fr, %d), "
          "&_o)) {\n",
          a, b);
      fprintf(
          f, "          qjs_aot_set_reg_p(ctx, fr, %d, _o); goto pc_%d; } }\n",
          d, next);
      emit_fallback(f, off, next, last);
      return 1;

    /* Fused multiply-add family: mul then add/sub, exactly as the handler. */
    case ROP_mul_add:
    case ROP_mul_sub:
    case ROP_add_mul:
    case ROP_sub_mul: {
      int dm = (int)in->arg[0].val, x = (int)in->arg[1].val,
          y = (int)in->arg[2].val;
      int da = (int)in->arg[3].val, c = (int)in->arg[4].val;
      int mul_first = (in->op == ROP_mul_add || in->op == ROP_mul_sub);
      int sub2 = (in->op == ROP_mul_add || in->op == ROP_add_mul) ? REG_BIN_ADD
                                                                  : REG_BIN_SUB;
      int lhs2 = mul_first ? dm : c;
      int rhs2 = mul_first ? c : dm;
      fprintf(f, "    { JSValue _t, _r;\n");
      fprintf(
          f,
          "      if (!qjs_aot_num_fast(%d, AOT_REG(fr, %d), AOT_REG(fr, %d), "
          "&_t) &&\n",
          REG_BIN_MUL, x, y);
      fprintf(
          f,
          "          qjs_aot_binary(ctx, %d, AOT_REG(fr, %d), AOT_REG(fr, %d), "
          "&_t))\n",
          REG_BIN_MUL, x, y);
      fprintf(f, "          goto exception;\n");
      fprintf(f, "      qjs_aot_set_reg_p(ctx, fr, %d, _t);\n", dm);
      fprintf(
          f,
          "      if (!qjs_aot_num_fast(%d, AOT_REG(fr, %d), AOT_REG(fr, %d), "
          "&_r) &&\n",
          sub2, lhs2, rhs2);
      fprintf(
          f,
          "          qjs_aot_binary(ctx, %d, AOT_REG(fr, %d), AOT_REG(fr, %d), "
          "&_r))\n",
          sub2, lhs2, rhs2);
      fprintf(f, "          goto exception;\n");
      fprintf(
          f, "      qjs_aot_set_reg_p(ctx, fr, %d, _r); goto pc_%d; }\n", da,
          next);
      return 1;
    }

    /* get_field_chain / get_field_chain_own: two own_read_hit fast paths. Both
     hits are tested before either register is written, so a miss leaves the
     registers untouched and the step fallback starts from a clean state. */
    case ROP_get_field_chain:
    case ROP_get_field_chain_own: {
      int d1 = (int)in->arg[0].val, r1 = (int)in->arg[1].val;
      int s1 = (int)in->arg[3].val, d2 = (int)in->arg[4].val;
      int s2 = (int)in->arg[7].val;
      if (s1 == REG_IC_NONE || s2 == REG_IC_NONE) return 0;
      fprintf(f, "    { JSValue _v1, _v2;\n");
      fprintf(
          f,
          "      if (qjs_aot_own_read_hit(f, pic, AOT_REG(fr, %d), %d, &_v1) "
          "&&\n",
          r1, s1);
      fprintf(
          f, "          qjs_aot_own_read_hit(f, pic, _v1, %d, &_v2)) {\n", s2);
      fprintf(f, "          AOT_SET_B(ctx, fr, %d, _v1);\n", d1);
      fprintf(
          f, "          AOT_SET_B(ctx, fr, %d, _v2); goto pc_%d; } }\n", d2,
          next);
      emit_fallback(f, off, next, last);
      return 1;
    }

    /* branch_cmp_reg_reg / _s: the int/float compare, else the step. */
    case ROP_branch_cmp_reg_reg:
    case ROP_branch_cmp_reg_reg_s: {
      int s = (int)in->arg[0].val;
      int la = (int)in->arg[1].val, rb = (int)in->arg[2].val;
      int tgt = next + (int)in->arg[3].val;
      fprintf(f, "    { int _t;\n");
      fprintf(
          f,
          "      if (qjs_aot_branch_cmp(%d, AOT_REG(fr, %d), AOT_REG(fr, %d), "
          "&_t)) {\n",
          s, la, rb);
      fprintf(
          f, "          if (_t) goto pc_%d; else goto pc_%d; } }\n", tgt, next);
      emit_fallback(f, off, next, last);
      return 1;
    }

    /* get_var_ref: SET_REG_B(d, *var_refs[slot]->pvalue). */
    case ROP_get_var_ref:
      d = (int)in->arg[0].val;
      fprintf(
          f,
          "    AOT_SET_B(ctx, fr, %d, qjs_aot_get_var_ref(f, %d));\n    goto "
          "pc_%d;\n",
          d, (int)in->arg[1].val, next);
      return 1;

    /* load_this: SET_REG_B for a borrowed this, SET_REG_P when boxed. */
    case ROP_load_this:
      d = (int)in->arg[0].val;
      fprintf(
          f, "    { JSValue _o; int _r = qjs_aot_load_this(ctx, f, &_o);\n");
      fprintf(f, "      if (_r < 0) goto exception;\n");
      fprintf(f, "      if (_r) qjs_aot_set_reg_p(ctx, fr, %d, _o);\n", d);
      fprintf(f, "      else AOT_SET_B(ctx, fr, %d, _o);\n", d);
      fprintf(f, "      goto pc_%d; }\n", next);
      return 1;

    /* move_xfer: move d<-s then clear s (self-move is a no-op). */
    case ROP_move_xfer:
      d = (int)in->arg[0].val;
      a = (int)in->arg[1].val;
      if (d == a)
        fprintf(f, "    goto pc_%d;\n", next);
      else
        fprintf(
            f, "    qjs_aot_move_xfer(ctx, fr, %d, %d);\n    goto pc_%d;\n", d,
            a, next);
      return 1;

    /* load_undefined / load_null / load_true / load_false: SET_REG_P(d, const). */
    case ROP_load_undefined:
    case ROP_load_null:
    case ROP_load_true:
    case ROP_load_false:
      d = (int)in->arg[0].val;
      fprintf(
          f, "    qjs_aot_set_reg_p(ctx, fr, %d, %s);\n    goto pc_%d;\n", d,
          in->op == ROP_load_undefined ? "JS_UNDEFINED"
          : in->op == ROP_load_null    ? "JS_NULL"
          : in->op == ROP_load_true    ? "JS_TRUE"
                                       : "JS_FALSE",
          next);
      return 1;

    /* binary_imm / binary_imm_s: regvm_num_fast with an int immediate. */
    case ROP_binary_imm:
    case ROP_binary_imm_s: {
      int32_t imm = (int32_t)in->arg[3].val;
      d = (int)in->arg[0].val;
      sub = (int)in->arg[1].val;
      a = (int)in->arg[2].val;
      fprintf(f, "    { JSValue _o;\n");
      fprintf(
          f,
          "      if (!qjs_aot_num_fast(%d, AOT_REG(fr, %d), "
          "JS_MKVAL(JS_TAG_INT, %d), &_o) &&\n",
          sub, a, (int)imm);
      fprintf(
          f,
          "          qjs_aot_binary(ctx, %d, AOT_REG(fr, %d), "
          "JS_MKVAL(JS_TAG_INT, %d), &_o))\n",
          sub, a, (int)imm);
      fprintf(f, "          goto exception;\n");
      fprintf(
          f, "      qjs_aot_set_reg_p(ctx, fr, %d, _o); goto pc_%d; }\n", d,
          next);
      return 1;
    }

    /* branch / branch_s: always taken. */
    case ROP_branch:
    case ROP_branch_s:
      fprintf(f, "    goto pc_%d;\n", next + (int)in->arg[0].val);
      return 1;

    /* branch_if / branch_if_not: regvm_to_bool, else the step. */
    case ROP_branch_if:
    case ROP_branch_if_not: {
      int want_true = (in->op == ROP_branch_if);
      int tgt = next + (int)in->arg[1].val;
      a = (int)in->arg[0].val;
      fprintf(
          f,
          "    if ((qjs_aot_to_bool(ctx, AOT_REG(fr, %d)) != 0) == %d) goto "
          "pc_%d; else goto pc_%d;\n",
          a, want_true, tgt, next);
      return 1;
    }

    default:
      sub = branch_subop(in->op);
      if (sub >= 0) {
        /* The per-sense short branches: the int/float compare, else step. */
        a = (int)in->arg[0].val;
        b = (int)in->arg[1].val;
        int tgt = next + (int)in->arg[2].val;
        fprintf(f, "    { int _t;\n");
        fprintf(
            f,
            "      if (qjs_aot_branch_cmp(%d, AOT_REG(fr, %d), AOT_REG(fr, "
            "%d), &_t)) {\n",
            sub, a, b);
        fprintf(
            f, "          if (_t) goto pc_%d; else goto pc_%d; } }\n", tgt,
            next);
        emit_fallback(f, off, next, last);
        return 1;
      }
      sub = binop_subop(in->op);
      if (sub >= 0) {
        /* The remaining "rrr" binary ops: REGVM_BIN_OP. */
        d = (int)in->arg[0].val;
        a = (int)in->arg[1].val;
        b = (int)in->arg[2].val;
        fprintf(f, "    { JSValue _o;\n");
        fprintf(
            f,
            "      if (!qjs_aot_num_fast(%d, AOT_REG(fr, %d), AOT_REG(fr, %d), "
            "&_o) &&\n",
            sub, a, b);
        fprintf(
            f,
            "          qjs_aot_binary(ctx, %d, AOT_REG(fr, %d), AOT_REG(fr, "
            "%d), &_o))\n",
            sub, a, b);
        fprintf(f, "          goto exception;\n");
        fprintf(
            f, "      qjs_aot_set_reg_p(ctx, fr, %d, _o); goto pc_%d; }\n", d,
            next);
        return 1;
      }
      sub = unop_subop(in->op);
      if (sub >= 0) {
        /* The unary ops: REGVM_UN_OP. */
        d = (int)in->arg[0].val;
        a = (int)in->arg[1].val;
        fprintf(f, "    { JSValue _o;\n");
        fprintf(
            f,
            "      if (!qjs_aot_unary_num_fast(%d, AOT_REG(fr, %d), &_o) &&\n",
            sub, a);
        fprintf(
            f, "          qjs_aot_unary(ctx, %d, AOT_REG(fr, %d), &_o))\n", sub,
            a);
        fprintf(f, "          goto exception;\n");
        fprintf(
            f, "      qjs_aot_set_reg_p(ctx, fr, %d, _o); goto pc_%d; }\n", d,
            next);
        return 1;
      }
      return 0;
  }
}

/* ========================= Layer B typed regions =========================
 *
 * A "region" is a single-entry back-edge loop whose body only reads numbers
 * and dense array elements. The loop-carried var slots and the numeric
 * registers are kept unboxed in C locals (a double plus an is-int flag); the
 * loop bound and the array bases are guarded once at entry. Any value that is
 * not provably numeric, and any op outside the whitelist, deopts: the
 * unboxed values are boxed back into the register file / var buffer and
 * control jumps to the Layer A label for the same pc, which re-executes the
 * instruction with full interpreter semantics.
 *
 * The region does no allocation and makes no calls, so it can never trigger a
 * GC while a value is held only as a double; that is what makes the unboxed
 * representation sound without a safepoint.
 */

#include <stdarg.h>

#define RG_NONE 0
#define RG_NUM 1
#define RG_OBJ 2
#define RG_BOX 3
#define RG_MIX 4

typedef struct {
  const JSAotGenFunc *g;
  const JSAotGenInsn *ins;
  const int *offs, *nxts;
  int h, back, n; /* instruction indices: head .. back-edge */
  int head_pc, exit_pc;
  int rn;                           /* number of register slots (off>>4 + 1) */
  int sn;                           /* number of var slots */
  unsigned char *role_r, *role_s;   /* how each value is read */
  unsigned char *prod_r, *prod_s;   /* how each value is written */
  unsigned char *live_r, *live_s;   /* read before first write in the region */
  unsigned char *read_r, *read_s;   /* read anywhere in the region */
  unsigned char *unb_r, *unb_s;     /* currently unboxed during emission */
  unsigned char *entry_r, *entry_s; /* unboxed set at the loop head */
  unsigned char *elem_r;            /* register used as an element base */
  unsigned char *elem_iv_r;         /* element base read with the IV as key */
  int iv_reg;                 /* induction-variable register (-1 = none) */
  int bound_isreg, bound_off; /* loop-test bound operand */
  int pic_count;
  int *fguard_recv; /* PIC site -> receiver register (-1 = none) */
  int *vref_slot;   /* register -> captured-var slot (-1 = none) */
  FILE *f;
  int dry;
} RgCtx;

static int rg_ri(int off) {
  return off >> 4;
}

static void rg_out(RgCtx *c, const char *fmt, ...) {
  va_list ap;
  if (c->dry) return;
  va_start(ap, fmt);
  vfprintf(c->f, fmt, ap);
  va_end(ap);
}

/* Record how a value is read / written; a disagreement is RG_MIX (reject). */
static void rg_role(unsigned char *role, int idx, int r) {
  if (idx < 0) return;
  if (role[idx] == RG_NONE)
    role[idx] = (unsigned char)r;
  else if (role[idx] != r)
    role[idx] = RG_MIX;
}

/* A numeric value must be unboxed at region entry when it is read before its
   first write (a loop-carried input) OR when it is written in-region but never
   read there (a "sink" — a live-out variable such as `for (...) s = expr`). A
   sink has no in-region reader to keep its local initialised, so it must be
   seeded from the entry value and boxed back at the exit; otherwise a
   zero-iteration exit would drop the last write (and a >=1-iteration exit would
   leave the variable stale). */
static int rg_entry_unbox(RgCtx *c, int i, int isreg) {
  unsigned char *role = isreg ? c->role_r : c->role_s;
  unsigned char *prod = isreg ? c->prod_r : c->prod_s;
  unsigned char *live = isreg ? c->live_r : c->live_s;
  unsigned char *rd = isreg ? c->read_r : c->read_s;
  if (role[i] != RG_NUM) return 0;
  return live[i] || (prod[i] != RG_NONE && !rd[i]);
}

/* Allocate the analysis arrays for a candidate loop. */
static RgCtx *rg_new(
    const JSAotGenFunc *g, const JSAotGenInsn *ins, const int *offs,
    const int *nxts, int n, int h, int back, int exit_pc) {
  RgCtx *c = (RgCtx *)calloc(1, sizeof(*c));
  int i, maxoff = 0;
  if (!c) return NULL;
  c->g = g;
  c->ins = ins;
  c->offs = offs;
  c->nxts = nxts;
  c->h = h;
  c->back = back;
  c->n = n;
  c->head_pc = offs[h];
  c->exit_pc = exit_pc;
  for (i = h; i <= back; i++) {
    int k;
    for (k = 0; k < c->ins[i].nargs; k++)
      if (c->ins[i].arg[k].kind == 'r' && (int)c->ins[i].arg[k].val > maxoff)
        maxoff = (int)c->ins[i].arg[k].val;
  }
  c->rn = maxoff / 16 + 1;
  c->sn = g->var_count > 0 ? g->var_count : 1;
  c->role_r = (unsigned char *)calloc((size_t)c->rn, 1);
  c->role_s = (unsigned char *)calloc((size_t)c->sn, 1);
  c->prod_r = (unsigned char *)calloc((size_t)c->rn, 1);
  c->prod_s = (unsigned char *)calloc((size_t)c->sn, 1);
  c->live_r = (unsigned char *)calloc((size_t)c->rn, 1);
  c->live_s = (unsigned char *)calloc((size_t)c->sn, 1);
  c->read_r = (unsigned char *)calloc((size_t)c->rn, 1);
  c->read_s = (unsigned char *)calloc((size_t)c->sn, 1);
  c->unb_r = (unsigned char *)calloc((size_t)c->rn, 1);
  c->unb_s = (unsigned char *)calloc((size_t)c->sn, 1);
  c->entry_r = (unsigned char *)calloc((size_t)c->rn, 1);
  c->entry_s = (unsigned char *)calloc((size_t)c->sn, 1);
  c->elem_r = (unsigned char *)calloc((size_t)c->rn, 1);
  c->elem_iv_r = (unsigned char *)calloc((size_t)c->rn, 1);
  c->iv_reg = -1;
  c->bound_isreg = -1;
  c->bound_off = 0;
  c->pic_count = g->pic_count;
  c->fguard_recv = (int *)malloc(
      sizeof(int) * (size_t)(c->pic_count > 0 ? c->pic_count : 1));
  if (c->fguard_recv) {
    int j;
    for (j = 0; j < c->pic_count; j++) c->fguard_recv[j] = -1;
  }
  c->vref_slot = (int *)malloc(sizeof(int) * (size_t)c->rn);
  if (c->vref_slot) {
    int j;
    for (j = 0; j < c->rn; j++) c->vref_slot[j] = -1;
  }
  if (!c->role_r || !c->role_s || !c->prod_r || !c->prod_s || !c->live_r ||
      !c->live_s || !c->read_r || !c->read_s || !c->unb_r || !c->unb_s ||
      !c->entry_r || !c->entry_s || !c->elem_r || !c->elem_iv_r ||
      !c->fguard_recv || !c->vref_slot) {
    free(c->role_r);
    free(c->role_s);
    free(c->prod_r);
    free(c->prod_s);
    free(c->live_r);
    free(c->live_s);
    free(c->read_r);
    free(c->read_s);
    free(c->unb_r);
    free(c->unb_s);
    free(c->entry_r);
    free(c->entry_s);
    free(c->elem_r);
    free(c->elem_iv_r);
    free(c->fguard_recv);
    free(c->vref_slot);
    free(c);
    return NULL;
  }
  return c;
}

static void rg_free(RgCtx *c) {
  if (!c) return;
  free(c->role_r);
  free(c->role_s);
  free(c->prod_r);
  free(c->prod_s);
  free(c->live_r);
  free(c->live_s);
  free(c->read_r);
  free(c->read_s);
  free(c->unb_r);
  free(c->unb_s);
  free(c->entry_r);
  free(c->entry_s);
  free(c->elem_r);
  free(c->elem_iv_r);
  free(c->fguard_recv);
  free(c->vref_slot);
  free(c);
}

/* Classify a value read by an op. */
static void rg_read(RgCtx *c, int isreg, int off, int role) {
  int idx = isreg ? rg_ri(off) : off;
  unsigned char *role_a = isreg ? c->role_r : c->role_s;
  unsigned char *live_a = isreg ? c->live_r : c->live_s;
  unsigned char *read_a = isreg ? c->read_r : c->read_s;
  unsigned char *prod_a = isreg ? c->prod_r : c->prod_s;
  if (idx < 0 || (isreg ? idx >= c->rn : idx >= c->sn)) return;
  read_a[idx] = 1;
  rg_role(role_a, idx, role);
  if (prod_a[idx] == RG_NONE)
    live_a[idx] = 1; /* read before any write in the region */
}

/* Mark a value produced by an op. */
static void rg_write(RgCtx *c, int isreg, int off, int prod) {
  int idx = isreg ? rg_ri(off) : off;
  unsigned char *prod_a = isreg ? c->prod_r : c->prod_s;
  if (idx < 0 || (isreg ? idx >= c->rn : idx >= c->sn)) return;
  /* A register written inside the region must carry a role even when it is
     never read in-region (a write-only / live-out temporary): otherwise it is
     neither declared nor boxed, yet rg_numop still emits `u_/k_` for it. */
  rg_role(isreg ? c->role_r : c->role_s, idx, prod);
  if (prod_a[idx] == RG_NONE)
    prod_a[idx] = (unsigned char)prod;
  else if (prod_a[idx] != prod)
    prod_a[idx] = RG_MIX;
}

/* Compute roles / producers / liveness for a candidate region. Returns 0 if
   the region contains an op outside the whitelist. */
static int rg_scan(RgCtx *c) {
  int i;
  for (i = c->h; i <= c->back; i++) {
    const JSAotGenInsn *in = &c->ins[i];
    switch (in->op) {
      case ROP_local_check:
      case ROP_local_check_init:
        break;

      case ROP_get_local:
        rg_read(
            c, 0, (int)in->arg[1].val, RG_NUM); /* provisional: fixed below */
        rg_write(c, 1, (int)in->arg[0].val, RG_NUM);
        break;
      case ROP_get_local_s0:
      case ROP_get_local_s1:
      case ROP_get_local_s2:
      case ROP_get_local_s3:
        rg_read(c, 0, in->op - ROP_get_local_s0, RG_NUM);
        rg_write(c, 1, (int)in->arg[0].val, RG_NUM);
        break;

      case ROP_put_local:
        rg_read(c, 1, (int)in->arg[1].val, RG_NUM);
        rg_write(c, 0, (int)in->arg[0].val, RG_NUM);
        break;
      case ROP_put_local_s0:
      case ROP_put_local_s1:
      case ROP_put_local_s2:
      case ROP_put_local_s3:
        rg_read(c, 1, (int)in->arg[0].val, RG_NUM);
        rg_write(c, 0, in->op - ROP_put_local_s0, RG_NUM);
        break;
      case ROP_put_local_xfer:
        rg_read(c, 1, (int)in->arg[1].val, RG_NUM);
        rg_write(c, 0, (int)in->arg[0].val, RG_NUM);
        break;
      case ROP_put_local_xfer_s0:
      case ROP_put_local_xfer_s1:
      case ROP_put_local_xfer_s2:
      case ROP_put_local_xfer_s3:
        rg_read(c, 1, (int)in->arg[0].val, RG_NUM);
        rg_write(c, 0, in->op - ROP_put_local_xfer_s0, RG_NUM);
        break;

      case ROP_get_element:
        rg_read(c, 1, (int)in->arg[1].val, RG_OBJ);
        rg_read(c, 1, (int)in->arg[2].val, RG_NUM);
        rg_write(c, 1, (int)in->arg[0].val, RG_NUM);
        {
          int bi = rg_ri((int)in->arg[1].val);
          if (bi >= 0 && bi < c->rn) c->elem_r[bi] = 1;
        }
        break;

      case ROP_get_field:
      case ROP_get_field2: {
        int d = (int)in->arg[0].val, r = (int)in->arg[1].val;
        int site = (int)in->arg[3].val;
        rg_read(c, 1, r, RG_OBJ);
        rg_write(c, 1, d, RG_NUM);
        if (site >= 0 && site < c->pic_count) {
          if (c->fguard_recv[site] < 0)
            c->fguard_recv[site] = r;
          else if (c->fguard_recv[site] != r)
            return 0; /* same site, different receiver: cannot hoist */
        } else {
          return 0; /* no PIC site: cannot hoist the guard */
        }
        break;
      }

      case ROP_get_length:
        rg_read(c, 1, (int)in->arg[1].val, RG_OBJ);
        rg_write(c, 1, (int)in->arg[0].val, RG_NUM);
        break;

      case ROP_get_var_ref:
        rg_write(c, 1, (int)in->arg[0].val, RG_NUM);
        break;

      case ROP_add_mul:
      case ROP_mul_add:
      case ROP_mul_sub:
      case ROP_sub_mul:
        rg_read(c, 1, (int)in->arg[1].val, RG_NUM);
        rg_read(c, 1, (int)in->arg[2].val, RG_NUM);
        rg_read(c, 1, (int)in->arg[4].val, RG_NUM);
        rg_write(c, 1, (int)in->arg[0].val, RG_NUM); /* dm */
        rg_write(c, 1, (int)in->arg[3].val, RG_NUM); /* da */
        break;

      case ROP_eq:
      case ROP_lt:
      case ROP_lte:
      case ROP_gt:
      case ROP_gte:
      case ROP_neq:
      case ROP_strict_eq:
      case ROP_strict_neq:
        rg_read(c, 1, (int)in->arg[1].val, RG_NUM);
        rg_read(c, 1, (int)in->arg[2].val, RG_NUM);
        rg_write(c, 1, (int)in->arg[0].val, RG_BOX);
        break;

      case ROP_mul:
      case ROP_add:
      case ROP_sub:
        rg_read(c, 1, (int)in->arg[1].val, RG_NUM);
        rg_read(c, 1, (int)in->arg[2].val, RG_NUM);
        rg_write(c, 1, (int)in->arg[0].val, RG_NUM);
        break;
      case ROP_binary_loc_reg:
        rg_read(c, 0, (int)in->arg[2].val, RG_NUM);
        rg_read(c, 1, (int)in->arg[3].val, RG_NUM);
        rg_write(c, 1, (int)in->arg[0].val, RG_NUM);
        break;
      case ROP_binary_loc_loc:
        rg_read(c, 0, (int)in->arg[2].val, RG_NUM);
        rg_read(c, 0, (int)in->arg[3].val, RG_NUM);
        rg_write(c, 1, (int)in->arg[0].val, RG_NUM);
        break;
      case ROP_binary_imm:
      case ROP_binary_imm_s:
        rg_read(c, 1, (int)in->arg[2].val, RG_NUM);
        rg_write(c, 1, (int)in->arg[0].val, RG_NUM);
        break;
      case ROP_add_imm_s:
      case ROP_sub_imm_s:
        rg_read(c, 1, (int)in->arg[1].val, RG_NUM);
        rg_write(c, 1, (int)in->arg[0].val, RG_NUM);
        break;
      case ROP_add_imm_self_s:
      case ROP_sub_imm_self_s:
        rg_read(c, 1, (int)in->arg[0].val, RG_NUM);
        rg_write(c, 1, (int)in->arg[0].val, RG_NUM);
        break;
      case ROP_add_imm_self_branch:
      case ROP_add_imm_self_branch_s:
        rg_read(c, 1, (int)in->arg[0].val, RG_NUM);
        rg_write(c, 1, (int)in->arg[0].val, RG_NUM);
        break;

      case ROP_neg:
      case ROP_inc:
      case ROP_dec:
      case ROP_not:
      case ROP_to_numeric:
        rg_read(c, 1, (int)in->arg[1].val, RG_NUM);
        rg_write(c, 1, (int)in->arg[0].val, RG_NUM);
        break;

      case ROP_load_i8:
      case ROP_load_i8_s:
      case ROP_load_i32:
        rg_write(c, 1, (int)in->arg[0].val, RG_NUM);
        break;

      case ROP_move:
      case ROP_move_s:
        rg_read(c, 1, (int)in->arg[1].val, RG_NUM);
        rg_write(c, 1, (int)in->arg[0].val, RG_NUM);
        break;
      case ROP_move_xfer:
        rg_read(c, 1, (int)in->arg[1].val, RG_NUM);
        rg_write(c, 1, (int)in->arg[0].val, RG_NUM);
        break;

      case ROP_branch_cmp_loc_loc:
        rg_read(c, 0, (int)in->arg[1].val, RG_NUM);
        rg_read(c, 0, (int)in->arg[2].val, RG_NUM);
        break;
      case ROP_branch_cmp_loc_reg:
        rg_read(c, 0, (int)in->arg[1].val, RG_NUM);
        rg_read(c, 1, (int)in->arg[2].val, RG_NUM);
        break;
      case ROP_branch_cmp_reg_loc:
        rg_read(c, 1, (int)in->arg[1].val, RG_NUM);
        rg_read(c, 0, (int)in->arg[2].val, RG_NUM);
        break;
      case ROP_branch_cmp_reg_reg:
      case ROP_branch_cmp_reg_reg_s:
        rg_read(c, 1, (int)in->arg[1].val, RG_NUM);
        rg_read(c, 1, (int)in->arg[2].val, RG_NUM);
        break;

      case ROP_branch_lt_s:
      case ROP_branch_nlt_s:
      case ROP_branch_lte_s:
      case ROP_branch_nlte_s:
      case ROP_branch_gt_s:
      case ROP_branch_ngt_s:
      case ROP_branch_gte_s:
      case ROP_branch_ngte_s:
      case ROP_branch_eq_s:
      case ROP_branch_neq_s:
        rg_read(c, 1, (int)in->arg[0].val, RG_NUM);
        rg_read(c, 1, (int)in->arg[1].val, RG_NUM);
        break;

      case ROP_div:
        rg_read(c, 1, (int)in->arg[1].val, RG_NUM);
        rg_read(c, 1, (int)in->arg[2].val, RG_NUM);
        rg_write(c, 1, (int)in->arg[0].val, RG_NUM);
        break;

      case ROP_load_false:
      case ROP_load_true:
      case ROP_load_null:
        rg_write(c, 1, (int)in->arg[0].val, RG_BOX);
        break;

      case ROP_branch_if:
      case ROP_branch_if_not:
        rg_read(c, 1, (int)in->arg[0].val, RG_NUM);
        break;

      case ROP_branch:
      case ROP_branch_s:
        break;

      default:
        if (getenv("QJSAOT_REGION_DEBUG"))
          fprintf(stderr, "    op %d not whitelisted\n", in->op);
        return 0;
    }
  }
  return 1;
}

/* Recognise the canonical `for (i = 0; i < n; i++) a[i]` induction
   variable and its loop bound. Sets c->iv_reg and c->bound_off, and marks the
   element bases read with the IV as key (so their index checks can be elided).
   Only the register-IV shape is handled; anything else leaves iv_reg = -1. */
static void rg_find_iv(RgCtx *c) {
  int t;
  c->iv_reg = -1;
  c->bound_isreg = -1;
  for (t = c->h; t <= c->h + 1 && t <= c->back; t++) {
    const JSAotGenInsn *in = &c->ins[t];
    int idx_off = 0, bd_off = 0, tgt = -1, ok = 0;
    switch (in->op) {
      case ROP_branch_nlt_s:
        idx_off = (int)in->arg[0].val;
        bd_off = (int)in->arg[1].val;
        tgt = c->nxts[t] + (int)in->arg[2].val;
        ok = 1;
        break;
      case ROP_branch_cmp_reg_reg:
      case ROP_branch_cmp_reg_reg_s: {
        int sub = (int)in->arg[0].val;
        if (sub == REG_BIN_GTE || sub == (REG_BIN_LT | REG_CMP_INVERT)) {
          idx_off = (int)in->arg[1].val;
          bd_off = (int)in->arg[2].val;
          tgt = c->nxts[t] + (int)in->arg[3].val;
          ok = 1;
        }
        break;
      }
      default:
        break;
    }
    if (!ok || tgt != c->exit_pc) continue;
    {
      int iv = rg_ri(idx_off), bi = rg_ri(bd_off), k, writes = 0,
          last_write = -1;
      if (iv < 0 || iv >= c->rn || bi < 0 || bi >= c->rn) continue;
      if (c->role_r[iv] != RG_NUM || !c->live_r[iv]) continue;
      if (c->role_r[bi] != RG_NUM || c->prod_r[bi] != RG_NONE)
        continue; /* the bound must be loop-invariant */
      /* The IV is written only by self +/- 1, and every element read keyed by
         it happens before that write (so the read never sees the +1 value). */
      for (k = c->h; k <= c->back; k++) {
        const JSAotGenInsn *q = &c->ins[k];
        int d = (int)q->arg[0].val;
        if ((q->op == ROP_inc || q->op == ROP_dec) && d == idx_off &&
            (int)q->arg[1].val == idx_off) {
          writes++;
          last_write = k;
        } else if (
            (q->op == ROP_add_imm_self_s || q->op == ROP_sub_imm_self_s ||
             q->op == ROP_add_imm_self_branch ||
             q->op == ROP_add_imm_self_branch_s) &&
            d == idx_off && (int)(int8_t)q->arg[1].val == 1) {
          writes++;
          last_write = k;
        }
      }
      if (writes == 0) continue;
      for (k = c->h; k <= c->back; k++) {
        const JSAotGenInsn *q = &c->ins[k];
        if (q->op == ROP_get_element && (int)q->arg[2].val == idx_off &&
            k > last_write)
          return; /* an element read sees the incremented value: unsafe */
      }
      /* mark element bases read with the IV as key */
      for (k = c->h; k <= c->back; k++) {
        const JSAotGenInsn *q = &c->ins[k];
        if (q->op == ROP_get_element && (int)q->arg[2].val == idx_off) {
          int b = rg_ri((int)q->arg[1].val);
          if (b >= 0 && b < c->rn) c->elem_iv_r[b] = 1;
        }
      }
      c->iv_reg = iv;
      c->bound_isreg = 1;
      c->bound_off = bd_off;
      return;
    }
  }
}

/* Emit the box-back of every currently-unboxed value. */
static void rg_box_all(RgCtx *c) {
  int i;
  if (mut_is("no_box_on_exit")) return;
  for (i = 0; i < c->rn; i++)
    if (c->role_r[i] == RG_NUM && c->unb_r[i]) {
      if (i == c->iv_reg && !mut_is("iv_no_box"))
        rg_out(
            c,
            "        qjs_aot_set_reg_p(ctx, fr, %d, "
            "qjs_aot_box_num(u_%d, qjs_aot_fitsi(u_%d)));\n",
            i * 16, i * 16, i * 16);
      else
        rg_out(
            c,
            "        qjs_aot_set_reg_p(ctx, fr, %d, qjs_aot_box_num(u_%d, "
            "k_%d));\n",
            i * 16, i * 16, i * 16);
    }
  for (i = 0; i < c->sn; i++)
    if (c->role_s[i] == RG_NUM && c->unb_s[i])
      rg_out(
          c,
          "        qjs_aot_put_local(ctx, vb, %d, qjs_aot_box_num(us_%d, "
          "ks_%d));\n",
          i, i, i);
}

/* A deopt: box everything, then jump to the Layer A label for pc. The
   per-region disable counter is bumped so a loop that keeps failing its entry
   guard stops paying for it. */
static void rg_deopt(RgCtx *c, int pc, int is_head) {
  rg_box_all(c);
  rg_out(c, "        rg_dis_%d++;\n", c->head_pc);
  if (is_head)
    rg_out(c, "        goto pc_%d_la;\n", pc);
  else
    rg_out(c, "        goto pc_%d;\n", pc);
}

typedef struct {
  int isreg;
  int off;
} RgVal;

static RgVal rg_reg(int off) {
  RgVal v;
  v.isreg = 1;
  v.off = off;
  return v;
}
static RgVal rg_slot(int s) {
  RgVal v;
  v.isreg = 0;
  v.off = s;
  return v;
}

/* Emit a deopt when a value is not currently unboxed as a number. */
static void rg_ensure_num(RgCtx *c, int isreg, int off, int pc) {
  int idx = isreg ? rg_ri(off) : off;
  unsigned char *unb = isreg ? c->unb_r : c->unb_s;
  unsigned char *role = isreg ? c->role_r : c->role_s;
  int bad;
  if (idx < 0 || (isreg ? idx >= c->rn : idx >= c->sn))
    bad = 1;
  else
    bad = (role[idx] != RG_NUM || !unb[idx]);
  if (bad) rg_deopt(c, pc, pc == c->head_pc);
}

static void rg_num_expr(RgVal v, char *buf, size_t n) {
  if (v.isreg)
    snprintf(buf, n, "u_%d", v.off);
  else
    snprintf(buf, n, "us_%d", v.off);
}
/* The induction variable keeps k=1 with no per-iteration FITSI, so its
   tag is stale past INT32_MAX; every consumer re-derives the exact isint. */
static int rg_is_iv(RgCtx *c, RgVal v) {
  return v.isreg && c->iv_reg >= 0 && rg_ri(v.off) == c->iv_reg;
}
static void rg_kint_expr(RgCtx *c, RgVal v, char *buf, size_t n) {
  if (rg_is_iv(c, v) && !mut_is("iv_no_box")) {
    snprintf(buf, n, "qjs_aot_fitsi(u_%d)", v.off);
  } else if (v.isreg) {
    snprintf(buf, n, "k_%d", v.off);
  } else {
    snprintf(buf, n, "ks_%d", v.off);
  }
}
static void rg_box_expr(RgCtx *c, RgVal v, char *buf, size_t n) {
  char k[32];
  if (v.isreg) {
    rg_kint_expr(c, v, k, sizeof k);
    snprintf(buf, n, "qjs_aot_box_num(u_%d,%s)", v.off, k);
  } else {
    rg_kint_expr(c, v, k, sizeof k);
    snprintf(buf, n, "qjs_aot_box_num(us_%d,%s)", v.off, k);
  }
}

/* A JSValue expression for a value of any role: an unboxed numeric is boxed,
   a boxed/object value already lives in fr[] / vb[]. */
static void rg_val_box_expr(RgCtx *c, int isreg, int off, char *buf, size_t n) {
  int idx = isreg ? rg_ri(off) : off;
  unsigned char *role = isreg ? c->role_r : c->role_s;
  int lim = isreg ? c->rn : c->sn;
  if (idx >= 0 && idx < lim && role[idx] == RG_NUM)
    rg_box_expr(c, isreg ? rg_reg(off) : rg_slot(off), buf, n);
  else if (isreg)
    snprintf(buf, n, "AOT_REG(fr,%d)", off);
  else
    snprintf(buf, n, "vb[%d]", off);
}

/* Emit a numeric binary op into an unboxed destination expression. */
static void rg_numop(
    RgCtx *c, int sub, const char *dst, const char *dka, const char *ea,
    const char *ka, const char *eb, const char *kb, int pc) {
  if (sub == REG_BIN_ADD || sub == REG_BIN_SUB || sub == REG_BIN_MUL) {
    const char *cs =
        sub == REG_BIN_ADD ? "+" : (sub == REG_BIN_SUB ? "-" : "*");
    if (mut_is("fitsi_loose"))
      rg_out(
          c, "        { double _d = %s %s %s; %s = _d; %s = %s && %s; }\n", ea,
          cs, eb, dst, dka, ka, kb);
    else
      rg_out(
          c,
          "        { double _d = %s %s %s; %s = _d; %s = %s && %s && "
          "qjs_aot_fitsi(_d); }\n",
          ea, cs, eb, dst, dka, ka, kb);
  } else if (sub == REG_BIN_DIV) {
    /* E1 div: the result is an int only when it is exactly an int32. */
    if (mut_is("div_int"))
      rg_out(
          c, "        { double _d = %s / %s; %s = _d; %s = 1; }\n", ea, eb, dst,
          dka);
    else
      rg_out(
          c,
          "        { double _d = %s / %s; %s = _d; %s = qjs_aot_fitsi(_d); }\n",
          ea, eb, dst, dka);
  } else {
    rg_out(c, "        { JSValue _o;\n");
    rg_out(
        c,
        "          if (!qjs_aot_num_fast(%d, qjs_aot_box_num(%s,%s), "
        "qjs_aot_box_num(%s,%s), &_o)) {\n",
        sub, ea, ka, eb, kb);
    rg_deopt(c, pc, pc == c->head_pc);
    rg_out(c, "          }\n");
    rg_out(c, "          qjs_aot_to_num(_o, &%s, &%s);\n", dst, dka);
    rg_out(c, "        }\n");
  }
}

/* A boxed-result binary op (comparisons): the destination register holds the
   boxed boolean/result the interpreter would produce. */
static void rg_boxop(RgCtx *c, int sub, int d, RgVal a, RgVal b, int pc) {
  char ea[64], eb[64];
  rg_ensure_num(c, a.isreg, a.off, pc);
  rg_ensure_num(c, b.isreg, b.off, pc);
  rg_box_expr(c, a, ea, sizeof ea);
  rg_box_expr(c, b, eb, sizeof eb);
  rg_out(c, "        { JSValue _r;\n");
  rg_out(
      c, "          if (qjs_aot_binary(ctx, %d, %s, %s, &_r)) {\n", sub, ea,
      eb);
  rg_deopt(c, pc, pc == c->head_pc);
  rg_out(c, "          }\n");
  rg_out(c, "          qjs_aot_set_reg_p(ctx, fr, %d, _r); }\n", d);
  c->unb_r[rg_ri(d)] = 0;
}

/* E1 binary op d = a OP b (d is always a register). */
static void rg_bin2(RgCtx *c, int sub, int d, RgVal a, RgVal b, int pc) {
  char ea[40], eb[40], ka[40], kb[40], dst[40], dka[40];
  rg_ensure_num(c, a.isreg, a.off, pc);
  rg_ensure_num(c, b.isreg, b.off, pc);
  rg_num_expr(a, ea, sizeof ea);
  rg_num_expr(b, eb, sizeof eb);
  rg_kint_expr(c, a, ka, sizeof ka);
  rg_kint_expr(c, b, kb, sizeof kb);
  snprintf(dst, sizeof dst, "u_%d", d);
  snprintf(dka, sizeof dka, "k_%d", d);
  rg_numop(c, sub, dst, dka, ea, ka, eb, kb, pc);
  c->unb_r[rg_ri(d)] = 1;
}

/* E1 binary op with an int immediate: d = a OP imm. */
static void rg_bin_imm(RgCtx *c, int sub, int d, RgVal a, int64_t imm, int pc) {
  char ea[40], ka[40], dst[40], dka[40], ei[32];
  rg_ensure_num(c, a.isreg, a.off, pc);
  rg_num_expr(a, ea, sizeof ea);
  rg_kint_expr(c, a, ka, sizeof ka);
  snprintf(dst, sizeof dst, "u_%d", d);
  snprintf(dka, sizeof dka, "k_%d", d);
  snprintf(ei, sizeof ei, "%.17g", (double)imm);
  rg_numop(c, sub, dst, dka, ea, ka, ei, "1", pc);
  c->unb_r[rg_ri(d)] = 1;
}

/* E1 unary op d = OP a. */
static void rg_un(RgCtx *c, int op, int d, RgVal a, int pc) {
  char ea[40], ka[40];
  rg_ensure_num(c, a.isreg, a.off, pc);
  rg_num_expr(a, ea, sizeof ea);
  rg_kint_expr(c, a, ka, sizeof ka);
  if (op == ROP_to_numeric) {
    rg_out(c, "        u_%d = %s; k_%d = %s;\n", d, ea, d, ka);
  } else if (op == ROP_neg || op == ROP_inc || op == ROP_dec) {
    const char *cs = op == ROP_neg ? "-" : (op == ROP_inc ? "+" : "-");
    int is_iv = (op == ROP_inc || op == ROP_dec) && c->iv_reg >= 0 &&
                rg_ri(d) == c->iv_reg && a.isreg && rg_ri(a.off) == c->iv_reg;
    if (op == ROP_neg)
      rg_out(
          c,
          "        { double _d = -%s; u_%d = _d; k_%d = %s && "
          "qjs_aot_fitsi(_d); }\n",
          ea, d, d, ka);
    else if (is_iv)
      /* The IV is kept as an exact double (an int64-range integer) with no
         per-iteration FITSI; its tag stays 1 and may be stale past 2^31. Every
         box site re-applies the exact interpreter rule (rg_box_expr/rg_kint_expr
         and rg_box_all), so the observable tag is still INT iff (int32_t)d == d. */
      rg_out(
          c, "        { double _d = %s %s 1.0; u_%d = _d; k_%d = 1; }\n", ea,
          cs, d, d);
    else
      rg_out(
          c,
          "        { double _d = %s %s 1.0; u_%d = _d; k_%d = %s && "
          "qjs_aot_fitsi(_d); }\n",
          ea, cs, d, d, ka);
  } else {
    rg_out(c, "        { JSValue _o;\n");
    rg_out(
        c,
        "          if (!qjs_aot_unary_num_fast(%d, qjs_aot_box_num(%s,%s), "
        "&_o)) {\n",
        REG_UN_NOT, ea, ka);
    rg_out(
        c,
        "            if (qjs_aot_unary(ctx, %d, qjs_aot_box_num(%s,%s), &_o)) "
        "{\n",
        REG_UN_NOT, ea, ka);
    rg_deopt(c, pc, pc == c->head_pc);
    rg_out(c, "            }\n          }\n");
    rg_out(c, "          qjs_aot_to_num(_o, &u_%d, &k_%d);\n", d, d);
    rg_out(c, "        }\n");
  }
  c->unb_r[rg_ri(d)] = 1;
}

/* Dense-array element read, all unboxed. */
static void rg_elem(RgCtx *c, int d, int base, RgVal key, int pc) {
  int bk = rg_ri(base);
  char ek[40], kk[40];
  rg_ensure_num(c, key.isreg, key.off, pc);
  if (bk < 0 || bk >= c->rn || c->role_r[bk] != RG_OBJ)
    rg_deopt(c, pc, pc == c->head_pc);
  rg_num_expr(key, ek, sizeof ek);
  rg_kint_expr(c, key, kk, sizeof kk);
  {
    /* An IV-keyed read is covered by the hoisted `bound <= len` guard. */
    int iv_key = c->iv_reg >= 0 && key.isreg && rg_ri(key.off) == c->iv_reg;
    if (!mut_is("skip_bounds") && !iv_key) {
      rg_out(
          c, "        if (!(%s && %s >= 0.0 && %s < (double)len_%d)) {\n", kk,
          ek, ek, base);
      rg_deopt(c, pc, pc == c->head_pc);
      rg_out(c, "        }\n");
    }
  }
  rg_out(
      c,
      "        if (!qjs_aot_elem_num(data_%d, (uint32_t)%s, &u_%d, &k_%d)) {\n",
      base, ek, d, d);
  rg_deopt(c, pc, pc == c->head_pc);
  rg_out(c, "        }\n");
  c->unb_r[rg_ri(d)] = 1;
}

/* Emit `goto` for a branch target: loop head, region exit, or leave the
   region (boxing first). */
static void rg_goto_target(RgCtx *c, int tgt) {
  if (tgt == c->head_pc) {
    rg_out(c, "        goto rg_%d_loop;\n", c->head_pc);
  } else if (tgt == c->exit_pc) {
    rg_out(c, "        goto rg_%d_exit;\n", c->head_pc);
  } else {
    rg_box_all(c);
    rg_out(c, "        goto pc_%d;\n", tgt);
  }
}

/* Conditional numeric compare branch. */
static void rg_branch_cmp(
    RgCtx *c, int sub, RgVal a, RgVal b, int tgt, int pc) {
  char ea[40], eb[40];
  int invert = (sub & REG_CMP_INVERT) != 0;
  int base = sub & ~REG_CMP_INVERT;
  const char *cop = NULL;
  rg_ensure_num(c, a.isreg, a.off, pc);
  rg_ensure_num(c, b.isreg, b.off, pc);
  rg_num_expr(a, ea, sizeof ea);
  rg_num_expr(b, eb, sizeof eb);
  switch (base) {
    case REG_BIN_LT:
      cop = "<";
      break;
    case REG_BIN_LTE:
      cop = "<=";
      break;
    case REG_BIN_GT:
      cop = ">";
      break;
    case REG_BIN_GTE:
      cop = ">=";
      break;
    case REG_BIN_EQ:
    case REG_BIN_STRICT_EQ:
      cop = "==";
      break;
    case REG_BIN_NEQ:
    case REG_BIN_STRICT_NEQ:
      cop = "!=";
      break;
    default:
      break;
  }
  if (cop == NULL) { /* not a plain numeric compare: fall back to the helper */
    char ea2[40], eb2[40];
    rg_box_expr(c, a, ea2, sizeof ea2);
    rg_box_expr(c, b, eb2, sizeof eb2);
    rg_out(c, "        { int _t;\n");
    rg_out(
        c, "          if (!qjs_aot_branch_cmp(%d, %s, %s, &_t)) {\n", sub, ea2,
        eb2);
    rg_deopt(c, pc, pc == c->head_pc);
    rg_out(c, "          }\n");
  } else {
    /* Both operands are unboxed numbers (the entry to_num guards prove it),
       so a direct double compare has exactly JS semantics, including NaN and
       -0. */
    rg_out(c, "        { int _t = (%s %s %s);\n", ea, cop, eb);
    if (invert) rg_out(c, "          _t = !_t;\n");
  }
  if (tgt == c->head_pc) {
    rg_out(c, "          if (_t) goto rg_%d_loop;\n", c->head_pc);
  } else if (tgt == c->exit_pc) {
    rg_out(c, "          if (_t) goto rg_%d_exit;\n", c->head_pc);
  } else {
    rg_out(c, "          if (_t) {\n");
    rg_box_all(c);
    rg_out(c, "            goto pc_%d;\n          }\n", tgt);
  }
  rg_out(c, "        }\n");
}

/* Conditional boolean branch. */
static void rg_branch_bool(RgCtx *c, int op, int a, int tgt, int pc) {
  int want = (op == ROP_branch_if);
  char ev[64];
  rg_val_box_expr(c, 1, a, ev, sizeof ev);
  if (tgt == c->head_pc) {
    rg_out(
        c,
        "        if ((qjs_aot_to_bool(ctx, %s) != 0) == %d) goto rg_%d_loop;\n",
        ev, want, c->head_pc);
  } else if (tgt == c->exit_pc) {
    rg_out(
        c,
        "        if ((qjs_aot_to_bool(ctx, %s) != 0) == %d) goto rg_%d_exit;\n",
        ev, want, c->head_pc);
  } else {
    rg_out(
        c, "        if ((qjs_aot_to_bool(ctx, %s) != 0) == %d) {\n", ev, want);
    rg_box_all(c);
    rg_out(c, "          goto pc_%d;\n        }\n", tgt);
  }
}

/* Emit the unboxed loop body. Assumes rg_scan accepted the region. */
static void rg_emit_body(RgCtx *c) {
  int i;
  for (i = c->h; i <= c->back; i++) {
    const JSAotGenInsn *in = &c->ins[i];
    int pc = c->offs[i], next = c->nxts[i];
    switch (in->op) {
      case ROP_local_check:
      case ROP_local_check_init:
        break;

      case ROP_get_local: {
        int d = (int)in->arg[0].val, s = (int)in->arg[1].val;
        if (s >= 0 && s < c->sn && c->role_s[s] == RG_NUM) {
          rg_out(c, "        u_%d = us_%d; k_%d = ks_%d;\n", d, s, d, s);
          c->unb_r[rg_ri(d)] = 1;
        } else {
          rg_out(c, "        AOT_SET_B(ctx, fr, %d, vb[%d]);\n", d, s);
          c->unb_r[rg_ri(d)] = 0;
        }
        break;
      }
      case ROP_get_local_s0:
      case ROP_get_local_s1:
      case ROP_get_local_s2:
      case ROP_get_local_s3: {
        int d = (int)in->arg[0].val, s = in->op - ROP_get_local_s0;
        if (s < c->sn && c->role_s[s] == RG_NUM) {
          rg_out(c, "        u_%d = us_%d; k_%d = ks_%d;\n", d, s, d, s);
          c->unb_r[rg_ri(d)] = 1;
        } else {
          rg_out(c, "        AOT_SET_B(ctx, fr, %d, vb[%d]);\n", d, s);
          c->unb_r[rg_ri(d)] = 0;
        }
        break;
      }

      case ROP_put_local: {
        int s = (int)in->arg[0].val, r = (int)in->arg[1].val;
        if (s >= 0 && s < c->sn && c->role_s[s] == RG_NUM) {
          rg_ensure_num(c, 1, r, pc);
          rg_out(c, "        us_%d = u_%d; ks_%d = k_%d;\n", s, r, s, r);
          c->unb_s[s] = 1;
        } else {
          rg_deopt(c, pc, pc == c->head_pc);
        }
        break;
      }
      case ROP_put_local_s0:
      case ROP_put_local_s1:
      case ROP_put_local_s2:
      case ROP_put_local_s3: {
        int s = in->op - ROP_put_local_s0, r = (int)in->arg[0].val;
        if (c->role_s[s] == RG_NUM) {
          rg_ensure_num(c, 1, r, pc);
          rg_out(c, "        us_%d = u_%d; ks_%d = k_%d;\n", s, r, s, r);
          c->unb_s[s] = 1;
        } else {
          rg_deopt(c, pc, pc == c->head_pc);
        }
        break;
      }
      case ROP_put_local_xfer:
      case ROP_put_local_xfer_s0:
      case ROP_put_local_xfer_s1:
      case ROP_put_local_xfer_s2:
      case ROP_put_local_xfer_s3: {
        int s = (in->op == ROP_put_local_xfer) ? (int)in->arg[0].val
                                               : in->op - ROP_put_local_xfer_s0;
        int r = (int)in->arg[in->op == ROP_put_local_xfer ? 1 : 0].val;
        if (s >= 0 && s < c->sn && c->role_s[s] == RG_NUM) {
          rg_ensure_num(c, 1, r, pc);
          rg_out(c, "        us_%d = u_%d; ks_%d = k_%d;\n", s, r, s, r);
          c->unb_s[s] = 1;
          c->unb_r[rg_ri(r)] = 0; /* the xfer empties the register */
        } else {
          rg_deopt(c, pc, pc == c->head_pc);
        }
        break;
      }

      case ROP_get_element:
        rg_elem(
            c, (int)in->arg[0].val, (int)in->arg[1].val,
            rg_reg((int)in->arg[2].val), pc);
        break;

      case ROP_get_field:
      case ROP_get_field2: {
        int d = (int)in->arg[0].val, r = (int)in->arg[1].val;
        int site = (int)in->arg[3].val;
        rg_out(
            c,
            "        { JSValue _v = qjs_aot_field_read(AOT_REG(fr,%d), "
            "_fso_%d);\n",
            r, site);
        rg_out(c, "          if (!qjs_aot_to_num(_v, &u_%d, &k_%d)) {\n", d, d);
        rg_deopt(c, pc, pc == c->head_pc);
        rg_out(c, "          } }\n");
        c->unb_r[rg_ri(d)] = 1;
        break;
      }

      case ROP_get_length: {
        int d = (int)in->arg[0].val, r = (int)in->arg[1].val;
        rg_out(
            c,
            "        if (!qjs_aot_get_length_num(AOT_REG(fr,%d), &u_%d, "
            "&k_%d)) {\n",
            r, d, d);
        rg_deopt(c, pc, pc == c->head_pc);
        rg_out(c, "        }\n");
        c->unb_r[rg_ri(d)] = 1;
        break;
      }
      case ROP_get_var_ref: {
        int d = (int)in->arg[0].val, slot = (int)in->arg[1].val;
        rg_out(
            c, "        { JSValue _v = *qjs_aot_var_ref_ptr(f, %d);\n", slot);
        rg_out(c, "          if (!qjs_aot_to_num(_v, &u_%d, &k_%d)) {\n", d, d);
        rg_deopt(c, pc, pc == c->head_pc);
        rg_out(c, "          } }\n");
        c->unb_r[rg_ri(d)] = 1;
        break;
      }

      case ROP_add_mul:
      case ROP_mul_add:
      case ROP_mul_sub:
      case ROP_sub_mul: {
        int dm = (int)in->arg[0].val, x = (int)in->arg[1].val,
            y = (int)in->arg[2].val;
        int da = (int)in->arg[3].val, cc = (int)in->arg[4].val;
        int isadd = (in->op == ROP_mul_add || in->op == ROP_add_mul);
        int mul_first = (in->op == ROP_mul_add || in->op == ROP_mul_sub);
        int bsub = isadd ? REG_BIN_ADD : REG_BIN_SUB;
        rg_bin2(c, REG_BIN_MUL, dm, rg_reg(x), rg_reg(y), pc);
        if (mul_first)
          rg_bin2(c, bsub, da, rg_reg(dm), rg_reg(cc), pc);
        else
          rg_bin2(c, bsub, da, rg_reg(cc), rg_reg(dm), pc);
        break;
      }

      case ROP_eq:
      case ROP_lt:
      case ROP_lte:
      case ROP_gt:
      case ROP_gte:
      case ROP_neq:
      case ROP_strict_eq:
      case ROP_strict_neq:
        rg_boxop(
            c, binop_subop(in->op), (int)in->arg[0].val,
            rg_reg((int)in->arg[1].val), rg_reg((int)in->arg[2].val), pc);
        break;

      case ROP_mul:
      case ROP_add:
      case ROP_sub: {
        int sub = in->op == ROP_mul   ? REG_BIN_MUL
                  : in->op == ROP_add ? REG_BIN_ADD
                                      : REG_BIN_SUB;
        rg_bin2(
            c, sub, (int)in->arg[0].val, rg_reg((int)in->arg[1].val),
            rg_reg((int)in->arg[2].val), pc);
        break;
      }
      case ROP_binary_loc_reg:
        rg_bin2(
            c, (int)in->arg[1].val, (int)in->arg[0].val,
            rg_slot((int)in->arg[2].val), rg_reg((int)in->arg[3].val), pc);
        break;
      case ROP_binary_loc_loc:
        rg_bin2(
            c, (int)in->arg[1].val, (int)in->arg[0].val,
            rg_slot((int)in->arg[2].val), rg_slot((int)in->arg[3].val), pc);
        break;
      case ROP_binary_imm:
      case ROP_binary_imm_s:
        rg_bin_imm(
            c, (int)in->arg[1].val, (int)in->arg[0].val,
            rg_reg((int)in->arg[2].val), (int32_t)in->arg[3].val, pc);
        break;
      case ROP_add_imm_s:
      case ROP_sub_imm_s:
        rg_bin_imm(
            c, in->op == ROP_add_imm_s ? REG_BIN_ADD : REG_BIN_SUB,
            (int)in->arg[0].val, rg_reg((int)in->arg[1].val),
            (int32_t)in->arg[2].val, pc);
        break;
      case ROP_add_imm_self_s:
      case ROP_sub_imm_self_s: {
        int d = (int)in->arg[0].val;
        rg_bin_imm(
            c, in->op == ROP_add_imm_self_s ? REG_BIN_ADD : REG_BIN_SUB, d,
            rg_reg(d), (int32_t)in->arg[1].val, pc);
        break;
      }
      case ROP_add_imm_self_branch:
      case ROP_add_imm_self_branch_s: {
        int d = (int)in->arg[0].val;
        int tgt = next + (int)in->arg[2].val;
        rg_bin_imm(c, REG_BIN_ADD, d, rg_reg(d), (int32_t)in->arg[1].val, pc);
        rg_goto_target(c, tgt);
        break;
      }
      case ROP_neg:
      case ROP_inc:
      case ROP_dec:
      case ROP_not:
      case ROP_to_numeric:
        rg_un(c, in->op, (int)in->arg[0].val, rg_reg((int)in->arg[1].val), pc);
        break;

      case ROP_load_i8:
      case ROP_load_i8_s: {
        int d = (int)in->arg[0].val;
        rg_out(
            c, "        u_%d = (double)(int)%d; k_%d = 1;\n", d,
            (int)(int8_t)in->arg[1].val, d);
        c->unb_r[rg_ri(d)] = 1;
        break;
      }
      case ROP_load_i32: {
        int d = (int)in->arg[0].val;
        rg_out(
            c, "        u_%d = (double)(int32_t)%d; k_%d = 1;\n", d,
            (int32_t)in->arg[1].val, d);
        c->unb_r[rg_ri(d)] = 1;
        break;
      }

      case ROP_move:
      case ROP_move_s:
      case ROP_move_xfer: {
        int d = (int)in->arg[0].val, a = (int)in->arg[1].val;
        rg_ensure_num(c, 1, a, pc);
        rg_out(c, "        u_%d = u_%d; k_%d = k_%d;\n", d, a, d, a);
        c->unb_r[rg_ri(d)] = 1;
        if (in->op == ROP_move_xfer) c->unb_r[rg_ri(a)] = 0;
        break;
      }

      case ROP_branch_cmp_loc_loc:
        rg_branch_cmp(
            c, (int)in->arg[0].val, rg_slot((int)in->arg[1].val),
            rg_slot((int)in->arg[2].val), next + (int)in->arg[3].val, pc);
        break;
      case ROP_branch_cmp_loc_reg:
        rg_branch_cmp(
            c, (int)in->arg[0].val, rg_slot((int)in->arg[1].val),
            rg_reg((int)in->arg[2].val), next + (int)in->arg[3].val, pc);
        break;
      case ROP_branch_cmp_reg_loc:
        rg_branch_cmp(
            c, (int)in->arg[0].val, rg_reg((int)in->arg[1].val),
            rg_slot((int)in->arg[2].val), next + (int)in->arg[3].val, pc);
        break;
      case ROP_branch_cmp_reg_reg:
      case ROP_branch_cmp_reg_reg_s:
        rg_branch_cmp(
            c, (int)in->arg[0].val, rg_reg((int)in->arg[1].val),
            rg_reg((int)in->arg[2].val), next + (int)in->arg[3].val, pc);
        break;

      case ROP_branch_lt_s:
      case ROP_branch_nlt_s:
      case ROP_branch_lte_s:
      case ROP_branch_nlte_s:
      case ROP_branch_gt_s:
      case ROP_branch_ngt_s:
      case ROP_branch_gte_s:
      case ROP_branch_ngte_s:
      case ROP_branch_eq_s:
      case ROP_branch_neq_s:
        rg_branch_cmp(
            c, branch_subop(in->op), rg_reg((int)in->arg[0].val),
            rg_reg((int)in->arg[1].val), next + (int)in->arg[2].val, pc);
        break;

      case ROP_div:
        rg_bin2(
            c, REG_BIN_DIV, (int)in->arg[0].val, rg_reg((int)in->arg[1].val),
            rg_reg((int)in->arg[2].val), pc);
        break;

      case ROP_load_false:
        rg_out(
            c, "        AOT_SET_B(ctx, fr, %d, JS_FALSE);\n",
            (int)in->arg[0].val);
        c->unb_r[rg_ri((int)in->arg[0].val)] = 0;
        break;
      case ROP_load_true:
        rg_out(
            c, "        AOT_SET_B(ctx, fr, %d, JS_TRUE);\n",
            (int)in->arg[0].val);
        c->unb_r[rg_ri((int)in->arg[0].val)] = 0;
        break;
      case ROP_load_null:
        rg_out(
            c, "        AOT_SET_B(ctx, fr, %d, JS_NULL);\n",
            (int)in->arg[0].val);
        c->unb_r[rg_ri((int)in->arg[0].val)] = 0;
        break;

      case ROP_branch_if:
      case ROP_branch_if_not:
        rg_branch_bool(
            c, in->op, (int)in->arg[0].val, next + (int)in->arg[1].val, pc);
        break;

      case ROP_branch:
      case ROP_branch_s:
        rg_goto_target(c, next + (int)in->arg[0].val);
        break;

      default:
        break;
    }
  }
}

/* If `in` is a branch, return 1 and set *tgt to its target pc. */
static int insn_branch_target(const JSAotGenInsn *in, int next, int *tgt) {
  int op = in->op;
  if (op == ROP_branch || op == ROP_branch_s) {
    *tgt = next + (int)in->arg[0].val;
    return 1;
  }
  if (op == ROP_branch_if || op == ROP_branch_if_not) {
    *tgt = next + (int)in->arg[1].val;
    return 1;
  }
  if (op == ROP_branch_cmp_loc_loc || op == ROP_branch_cmp_loc_reg ||
      op == ROP_branch_cmp_reg_loc || op == ROP_branch_cmp_reg_reg ||
      op == ROP_branch_cmp_reg_reg_s) {
    *tgt = next + (int)in->arg[3].val;
    return 1;
  }
  if (op == ROP_branch_lt_s || op == ROP_branch_nlt_s ||
      op == ROP_branch_lte_s || op == ROP_branch_nlte_s ||
      op == ROP_branch_gt_s || op == ROP_branch_ngt_s ||
      op == ROP_branch_gte_s || op == ROP_branch_ngte_s ||
      op == ROP_branch_eq_s || op == ROP_branch_neq_s) {
    *tgt = next + (int)in->arg[2].val;
    return 1;
  }
  if (op == ROP_add_imm_self_branch || op == ROP_add_imm_self_branch_s) {
    *tgt = next + (int)in->arg[2].val;
    return 1;
  }
  if (op == ROP_local_rmw_branch) {
    *tgt = next + (int)in->arg[3].val;
    return 1;
  }
  return 0;
}

/* Try to build a typed region for the loop [h, back]. Returns 1 and emits it
   when eligible. */
static int emit_region(
    FILE *f, const JSAotGenFunc *g, const JSAotGenInsn *ins, const int *offs,
    const int *nxts, int n, int h, int back, int exit_pc) {
  RgCtx *c = rg_new(g, ins, offs, nxts, n, h, back, exit_pc);
  int i, ok = 1, region_end = nxts[back];
  const char *rej = NULL;
  if (!c) return 0;
  if (exit_pc != region_end) ok = 0, rej = "exit != back-fallthrough";
  /* v1 handles only a top-tested loop with an unconditional back-edge. */
  if (ok && ins[back].op != ROP_branch && ins[back].op != ROP_branch_s)
    ok = 0, rej = "back-edge not unconditional";
  if (ok) {
    int btgt = nxts[back] + (int)ins[back].arg[0].val;
    if (btgt != c->head_pc) ok = 0, rej = "back-edge target != head";
  }
  if (ok && !rg_scan(c)) ok = 0, rej = "op outside whitelist";
  if (ok) {
    for (i = 0; i < c->rn; i++)
      if (c->role_r[i] == RG_MIX || c->prod_r[i] == RG_MIX)
        ok = 0, rej = "register role/producer mix";
    for (i = 0; i < c->sn; i++)
      if (c->role_s[i] == RG_MIX || c->prod_s[i] == RG_MIX)
        ok = 0, rej = "slot role/producer mix";
  }
  /* A numeric value must not be produced by a boxed op. */
  if (ok)
    for (i = 0; i < c->rn; i++)
      if (c->role_r[i] == RG_NUM && c->prod_r[i] == RG_BOX)
        ok = 0, rej = "num register produced by boxed op";
  if (ok)
    for (i = 0; i < c->sn; i++)
      if (c->role_s[i] == RG_NUM && c->prod_s[i] == RG_BOX)
        ok = 0, rej = "num slot produced by boxed op";
  /* Recognise the induction variable / loop bound (best effort). */
  if (ok) rg_find_iv(c);
  /* Field-guard receivers must be loop-invariant so the guard can be hoisted
     out of the loop. */
  if (ok) {
    for (i = 0; i < c->pic_count; i++) {
      int r = c->fguard_recv[i], ri;
      if (r < 0) continue;
      ri = rg_ri(r);
      if (ri < 0 || ri >= c->rn || c->role_r[ri] != RG_OBJ || !c->live_r[ri] ||
          c->prod_r[ri] != RG_NONE)
        ok = 0, rej = "field receiver not loop-invariant";
    }
  }
  /* Element bases are captured once at region entry (data_/len_), so they
     must be loop-invariant too. */
  if (ok) {
    for (i = 0; i < c->rn; i++) {
      if (!c->elem_r[i]) continue;
      if (c->role_r[i] != RG_OBJ || !c->live_r[i] || c->prod_r[i] != RG_NONE)
        ok = 0, rej = "element base not loop-invariant";
    }
  }
  /* Every branch must stay within the region, target the loop head (the
     back-edge), or leave to the single exit. Anything else is rejected. */
  if (ok) {
    for (i = h; i <= back && ok; i++) {
      int tgt;
      if (!insn_branch_target(&ins[i], nxts[i], &tgt)) continue;
      if (tgt == c->head_pc || tgt == c->exit_pc) continue;
      ok = 0, rej = "branch to a non-exit target";
    }
  }
  if (!ok) {
    if (getenv("QJSAOT_REGION_DEBUG"))
      fprintf(
          stderr, "region [%d,%d] head=%d reject: %s\n", h, back, c->head_pc,
          rej ? rej : "?");
    rg_free(c);
    return 0;
  }

  c->f = f;
  c->dry = 0;
  rg_out(c, "        {\n");
  rg_out(c, "        static unsigned int rg_dis_%d = 0;\n", c->head_pc);
  rg_out(
      c, "        if (rg_dis_%d > 64u) goto pc_%d_la;\n", c->head_pc,
      c->head_pc);
  for (i = 0; i < c->rn; i++)
    if (c->role_r[i] == RG_NUM)
      rg_out(c, "        double u_%d; int k_%d;\n", i * 16, i * 16);
  for (i = 0; i < c->rn; i++)
    if (c->role_r[i] == RG_OBJ && c->elem_r[i])
      rg_out(c, "        JSValue *data_%d; uint32_t len_%d;\n", i * 16, i * 16);
  for (i = 0; i < c->sn; i++)
    if (c->role_s[i] == RG_NUM)
      rg_out(c, "        double us_%d; int ks_%d;\n", i, i);
  for (i = 0; i < c->rn; i++) {
    if (rg_entry_unbox(c, i, 1)) {
      rg_out(
          c,
          "        if (!qjs_aot_to_num(AOT_REG(fr,%d), &u_%d, &k_%d)) goto "
          "pc_%d_la;\n",
          i * 16, i * 16, i * 16, c->head_pc);
      c->unb_r[i] = 1;
    } else if (c->live_r[i] && c->role_r[i] == RG_OBJ && c->elem_r[i]) {
      rg_out(
          c,
          "        if (!qjs_aot_dense(AOT_REG(fr,%d), &data_%d, &len_%d)) goto "
          "pc_%d_la;\n",
          i * 16, i * 16, i * 16, c->head_pc);
    }
  }
  for (i = 0; i < c->sn; i++) {
    if (rg_entry_unbox(c, i, 0)) {
      rg_out(
          c,
          "        if (!qjs_aot_to_num(vb[%d], &us_%d, &ks_%d)) goto "
          "pc_%d_la;\n",
          i, i, i, c->head_pc);
      c->unb_s[i] = 1;
    }
  }
  /* The induction variable starts >= 0 and stays below the (invariant)
     loop bound; if the bound is within every element base's length, the
     per-iteration index checks for IV-keyed reads can be elided. */
  if (c->iv_reg >= 0) {
    int iv = c->iv_reg;
    rg_out(
        c, "        if (!(k_%d && u_%d >= 0.0)) goto pc_%d_la;\n", iv * 16,
        iv * 16, c->head_pc);
    if (!mut_is("bounds_elide_len_write")) {
      for (i = 0; i < c->rn; i++)
        if (c->elem_iv_r[i])
          rg_out(
              c,
              "        if (!(k_%d && u_%d <= (double)len_%d)) goto pc_%d_la;\n",
              c->bound_off, c->bound_off, i * 16, c->head_pc);
    }
  }
  for (i = 0; i < c->pic_count; i++) {
    int r = c->fguard_recv[i];
    if (r < 0) continue;
    rg_out(c, "        int _fso_%d;\n", i);
    if (mut_is("hoist_ignore_store")) {
      /* Mutant: trust the generation-time slot offset and skip the shape
         guard, so a mid-loop shape change reads a stale slot. */
      rg_out(c, "        _fso_%d = 0;\n", i);
    } else {
      rg_out(
          c,
          "        if (!qjs_aot_field_guard(pic, AOT_REG(fr,%d), %d, "
          "&_fso_%d)) goto pc_%d_la;\n",
          r, i, i, c->head_pc);
    }
  }
  memcpy(c->entry_r, c->unb_r, (size_t)c->rn);
  memcpy(c->entry_s, c->unb_s, (size_t)c->sn);
  rg_out(c, "rg_%d_loop:\n", c->head_pc);
  rg_emit_body(c);
  /* The exit is reached from the top of the loop, where only the loop-carried
     (entry) values are definitely unboxed. Restore that set before boxing so
     that temporaries defined later in the body are never read uninitialised. */
  memcpy(c->unb_r, c->entry_r, (size_t)c->rn);
  memcpy(c->unb_s, c->entry_s, (size_t)c->sn);
  rg_out(c, "rg_%d_exit:\n", c->head_pc);
  rg_box_all(c);
  if (exit_pc >= g->len)
    rg_out(c, "        goto pc_end;\n");
  else
    rg_out(c, "        goto pc_%d;\n", exit_pc);
  rg_out(c, "        }\n");
  rg_free(c);
  return 1;
}

/* Emit one function's body (templates, step for the rest). Returns the
   instruction count, or -1 on a malformed program. */
static int emit_func(FILE *f, const JSAotGenFunc *g, char hex[33]) {
  int off = 0, n = 0, i;
  int *offs = NULL;
  int *nxts = NULL;
  JSAotGenInsn *ins = NULL;
  int cap = 0;

  g_cur_prof = prof_entry(g_prof, g_nprof, g->digest);

  while (off < g->len) {
    JSAotGenInsn cur;
    if (js_aot_gen_decode_insn(g->code, g->len, off, &cur) < 0) {
      free(offs);
      free(nxts);
      free(ins);
      return -1;
    }
    if (n == cap) {
      int *no, *nn;
      JSAotGenInsn *ni;
      cap = cap ? cap * 2 : 64;
      no = realloc(offs, sizeof(*offs) * (size_t)cap);
      nn = realloc(nxts, sizeof(*nxts) * (size_t)cap);
      ni = realloc(ins, sizeof(*ins) * (size_t)cap);
      if (!no || !nn || !ni) {
        free(no ? no : offs);
        free(nn ? nn : nxts);
        free(ni ? ni : ins);
        return -1;
      }
      offs = no;
      nxts = nn;
      ins = ni;
    }
    offs[n] = off;
    nxts[n] = off + cur.size;
    ins[n] = cur;
    n++;
    off += cur.size;
  }

  fprintf(
      f,
      "static JSValue aot_f_%s(JSContext *ctx, JSRegRun *f,\n"
      "                                 JSValue *fr, uint32_t entry_pc)\n{\n",
      hex);
  fprintf(f, "    uint8_t *code = qjs_aot_code(f);\n");
  fprintf(f, "    uint8_t *ip;\n    uint32_t pc = entry_pc;\n");
  fprintf(f, "    JSValue *vb = qjs_aot_var_buf(f);\n");
  fprintf(f, "    JSValue *ab = qjs_aot_arg_buf(f);\n");
  fprintf(f, "    const void *pic = qjs_aot_pic(f);\n");
  fprintf(f, "    (void)code; (void)vb; (void)ab; (void)pic;\n");
  fprintf(f, "    goto dispatch_ip;\n");
  /* Detect back-edges: a branch whose target is an earlier instruction start.
     region_back[head_idx] holds the back-edge instruction index. */
  {
    int *rb = calloc((size_t)n, sizeof(int));
    int k;
    if (rb) {
      for (k = 0; k < n; k++) rb[k] = -1;
      for (k = 0; k < n; k++) {
        int tgt, j;
        if (!insn_branch_target(&ins[k], nxts[k], &tgt)) continue;
        if (tgt >= offs[k]) continue;
        for (j = 0; j < n; j++)
          if (offs[j] == tgt) break;
        if (j < n && rb[j] < 0) rb[j] = k;
      }
    }
    for (i = 0; i < n; i++) {
      int last = nxts[i] >= g->len;
      int is_region = 0;
      fprintf(f, "pc_%d:\n", offs[i]);
      if (rb && rb[i] >= 0) {
        is_region =
            emit_region(f, g, ins, offs, nxts, n, i, rb[i], nxts[rb[i]]);
        if (is_region) fprintf(f, "pc_%d_la:\n", offs[i]);
      }
      if (!emit_template(f, &ins[i], offs[i], nxts[i], last))
        emit_fallback(f, offs[i], nxts[i], last);
    }
    free(rb);
  }
  fprintf(f, "pc_end:\n    return qjs_aot_ret(f);\n");
  fprintf(f, "dispatch_ip:\n    switch (pc) {\n");
  for (i = 0; i < n; i++)
    fprintf(f, "    case %d: goto pc_%d;\n", offs[i], offs[i]);
  fprintf(f, "    case %d: goto pc_end;\n", g->len);
  fprintf(f, "    default: __builtin_unreachable();\n    }\n");
  fprintf(f, "exception: {\n");
  fprintf(f, "        uint32_t rp;\n");
  fprintf(f, "        JSValue ex = qjs_aot_unwind(ctx, f, fr, pc, &rp);\n");
  fprintf(
      f, "        if (!JS_IsException(ex)) { pc = rp; goto dispatch_ip; }\n");
  fprintf(f, "        return JS_EXCEPTION;\n    }\n}\n\n");

  free(offs);
  free(nxts);
  return n;
}

/* --- selection ------------------------------------------------------------
   The generator measures each function's emitted body once, then chooses a
   subset: rank by profiled ops, take the prefix covering HOT_COVER of the
   profiled ops, drop startup-only entries below MIN_ENTRY_OPS, close over
   compiled callees, and finally drop the lowest ops/byte until the
   estimated body bytes fit the budget. `--all` bypasses selection. */

typedef struct {
  char *buf; /* memstream body, or NULL when the function failed */
  size_t len;
} EmitBuf;

static unsigned long long prof_ops_of(
    const ProfEntry *prof, int nprof, const uint8_t dg[16]) {
  int i;
  for (i = 0; i < nprof; i++)
    if (memcmp(prof[i].digest, dg, 16) == 0) return prof[i].ops;
  return 0;
}

static const ProfEntry *prof_entry_of(
    const ProfEntry *prof, int nprof, const uint8_t dg[16]) {
  int i;
  for (i = 0; i < nprof; i++)
    if (memcmp(prof[i].digest, dg, 16) == 0) return &prof[i];
  return NULL;
}

/* The unique-body representative of a digest (the last index of its sorted
   run, matching the emit loop's dedupe), or -1. funcs is digest-sorted. */
static int find_unit(
    const JSAotGenFunc *funcs, int nfuncs, const uint8_t dg[16]) {
  int i, found = -1;
  for (i = 0; i < nfuncs; i++) {
    int c = memcmp(funcs[i].digest, dg, 16);
    if (c == 0) {
      if (funcs[i].eligible == AOT_GEN_ELIGIBLE) found = i;
    } else if (c > 0) {
      break;
    }
  }
  return found;
}

/* Compare for a descending-ops sort of unit indices. */
static const unsigned long long *g_sort_ops;
static int unit_ops_cmp(const void *a, const void *b) {
  int ia = *(const int *)a, ib = *(const int *)b;
  if (g_sort_ops[ia] != g_sort_ops[ib])
    return g_sort_ops[ia] < g_sort_ops[ib] ? 1 : -1;
  return ia < ib ? -1 : (ia > ib ? 1 : 0);
}

int main(int argc, char **argv) {
  JSRuntime *rt;
  JSContext *ctx;
  JSAotGenFunc *funcs = NULL;
  int nfuncs = 0;
  const char *out = "aot_unit.c";
  const char *profile = NULL;
  long budget = -1; /* -1 = unlimited */
  double hot_cover = 0.95;
  long min_ops = 1000;
  int all = 0;
  int i, fails = 0;
  FILE *f;
  long long total_insns = 0;
  int emitted = 0;
  char **inputs = NULL;
  int ninputs = 0;

  inputs = malloc(sizeof(*inputs) * (size_t)argc);
  if (!inputs) return 2;
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
      out = argv[++i];
    } else if (strcmp(argv[i], "--all") == 0) {
      all = 1;
    } else if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc) {
      profile = argv[++i];
    } else if (strcmp(argv[i], "--budget") == 0 && i + 1 < argc) {
      budget = strtol(argv[++i], NULL, 10);
    } else if (strcmp(argv[i], "--hot-cover") == 0 && i + 1 < argc) {
      hot_cover = strtod(argv[++i], NULL);
    } else if (strcmp(argv[i], "--min-ops") == 0 && i + 1 < argc) {
      min_ops = strtol(argv[++i], NULL, 10);
    } else if (strcmp(argv[i], "--mut") == 0 && i + 1 < argc) {
      g_mut = argv[++i];
    } else if (argv[i][0] == '-') {
      fprintf(stderr, "qjsaot: unknown option %s\n", argv[i]);
      free(inputs);
      return 2;
    } else {
      inputs[ninputs++] = argv[i];
    }
  }
  if (ninputs == 0) {
    fprintf(
        stderr,
        "usage: qjsaot --out aot_unit.c [--all] [--budget N] "
        "[--hot-cover F] [--min-ops N] [--profile app.qjsprof] "
        "bundle.js ...\n");
    free(inputs);
    return 2;
  }
  {
    const char *e;
    if ((e = getenv("QJS_AOT_HOT_COVER")) != NULL) hot_cover = strtod(e, NULL);
    if ((e = getenv("QJS_AOT_MIN_OPS")) != NULL) min_ops = strtol(e, NULL, 10);
    if ((e = getenv("QJS_AOT_BUDGET")) != NULL) budget = strtol(e, NULL, 10);
  }
  if (profile && !all && !(hot_cover > 0.0))
    fprintf(stderr, "qjsaot: --profile selection is disabled; using --all\n");

  rt = JS_NewRuntime();
  ctx = JS_NewContext(rt);

  for (i = 0; i < ninputs; i++) {
    long len = 0;
    char *src = read_file(inputs[i], &len);
    JSValue v;
    if (!src) {
      fprintf(stderr, "qjsaot: cannot read %s\n", inputs[i]);
      fails++;
      continue;
    }
    v = JS_Eval(
        ctx, src, (size_t)len, inputs[i],
        JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(v)) {
      JS_FreeValue(ctx, JS_GetException(ctx));
      v = JS_Eval(
          ctx, src, (size_t)len, inputs[i],
          JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    }
    if (JS_IsException(v)) {
      JSValue e = JS_GetException(ctx);
      const char *s = JS_ToCString(ctx, e);
      fprintf(stderr, "qjsaot: %s: %s\n", inputs[i], s ? s : "(unprintable)");
      JS_FreeCString(ctx, s);
      JS_FreeValue(ctx, e);
      fails++;
    }
    JS_FreeValue(ctx, v);
    free(src);
  }
  free(inputs);

  nfuncs = js_aot_gen_collect(rt, &funcs);
  qsort(funcs, (size_t)nfuncs, sizeof(*funcs), digest_cmp);

  {
    ProfEntry *prof = NULL;
    int nprof = 0;
    if (profile) {
      prof = parse_profile(profile, &nprof);
      if (!prof) {
        fprintf(stderr, "qjsaot: cannot use profile %s\n", profile);
        return 1;
      }
    }
    g_prof = prof;
    g_nprof = nprof;
    print_census(funcs, nfuncs, prof, nprof);
  }

  /* --- emit every unique eligible body once into a memory buffer --- */
  {
    EmitBuf *eb = calloc((size_t)nfuncs, sizeof(*eb));
    int *uniq = malloc(sizeof(int) * (size_t)(nfuncs > 0 ? nfuncs : 1));
    int nuniq = 0;
    char *sel = calloc((size_t)(nfuncs > 0 ? nfuncs : 1), 1);
    unsigned long long total_ops = 0, cov_ops = 0;
    long est_bytes = 0, actual_bytes = 0;

    if (!eb || !uniq || !sel) {
      free(eb);
      free(uniq);
      free(sel);
      js_aot_gen_free(rt, funcs);
      JS_FreeContext(ctx);
      JS_FreeRuntime(rt);
      return 2;
    }

    for (i = 0; i < nfuncs; i++) {
      char hex[33];
      int ninsn;
      FILE *mf;

      if (funcs[i].eligible != AOT_GEN_ELIGIBLE)
        continue; /* refused: no body to emit */
      if (i + 1 < nfuncs &&
          memcmp(funcs[i].digest, funcs[i + 1].digest, 16) == 0)
        continue; /* duplicate bytecode: one body serves both */
      hex16(funcs[i].digest, hex);
      mf = open_memstream(&eb[i].buf, &eb[i].len);
      if (!mf) {
        fails++;
        continue;
      }
      ninsn = emit_func(mf, &funcs[i], hex);
      fclose(mf);
      if (ninsn < 0) {
        fprintf(
            stderr, "qjsaot: malformed program in %s (%s)\n",
            funcs[i].name[0] ? funcs[i].name : "?", funcs[i].file);
        free(eb[i].buf);
        eb[i].buf = NULL;
        eb[i].len = 0;
        fails++;
        continue;
      }
      total_insns += ninsn;
      uniq[nuniq++] = i;
    }

    /* --- selection --- */
    if (all || !profile || g_nprof == 0) {
      int k;
      for (k = 0; k < nuniq; k++) {
        sel[uniq[k]] = 1;
        est_bytes += (long)eb[uniq[k]].len;
      }
    } else {
      unsigned long long *ops =
          calloc((size_t)(nfuncs > 0 ? nfuncs : 1), sizeof(*ops));
      int k;
      if (!ops) {
        free(eb);
        free(uniq);
        free(sel);
        js_aot_gen_free(rt, funcs);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 2;
      }
      for (k = 0; k < nuniq; k++) {
        ops[uniq[k]] = prof_ops_of(g_prof, g_nprof, funcs[uniq[k]].digest);
        total_ops += ops[uniq[k]];
      }
      if (total_ops == 0) {
        for (k = 0; k < nuniq; k++) {
          sel[uniq[k]] = 1;
          est_bytes += (long)eb[uniq[k]].len;
        }
      } else {
        int *order = malloc(sizeof(int) * (size_t)nuniq);
        unsigned long long cum = 0;
        unsigned long long target =
            (unsigned long long)(hot_cover * (double)total_ops);
        if (order) {
          for (k = 0; k < nuniq; k++) order[k] = uniq[k];
          g_sort_ops = ops;
          qsort(order, (size_t)nuniq, sizeof(int), unit_ops_cmp);
          for (k = 0; k < nuniq; k++) {
            if (cum >= target) break;
            sel[order[k]] = 1;
            cum += ops[order[k]];
          }
          free(order);
        }
        /* drop startup-only noise */
        for (k = 0; k < nuniq; k++)
          if (sel[uniq[k]] && ops[uniq[k]] < (unsigned long long)min_ops)
            sel[uniq[k]] = 0;
        /* close over compiled callees */
        {
          int changed = 1;
          while (changed) {
            changed = 0;
            for (k = 0; k < nuniq; k++) {
              const ProfEntry *pe;
              int c;
              if (!sel[uniq[k]]) continue;
              pe = prof_entry_of(g_prof, g_nprof, funcs[uniq[k]].digest);
              if (!pe) continue;
              for (c = 0; c < pe->ncalls; c++) {
                uint8_t dg[16];
                int ci;
                if (pe->calls[c].kind != 1 || !pe->calls[c].digest[0]) continue;
                if (hex_to_bytes16(pe->calls[c].digest, dg) < 0) continue;
                ci = find_unit(funcs, nfuncs, dg);
                if (ci >= 0 && !sel[ci]) {
                  sel[ci] = 1;
                  changed = 1;
                }
              }
            }
          }
        }
        /* budget: drop the lowest ops/byte until it fits */
        for (k = 0; k < nuniq; k++)
          if (sel[uniq[k]]) est_bytes += (long)eb[uniq[k]].len;
        if (budget > 0) {
          while (est_bytes > budget) {
            int best = -1, m;
            double best_ratio = 0;
            for (m = 0; m < nuniq; m++) {
              int idx = uniq[m];
              double ratio;
              if (!sel[idx]) continue;
              ratio =
                  eb[idx].len ? (double)ops[idx] / (double)eb[idx].len : 0.0;
              if (best < 0 || ratio < best_ratio ||
                  (ratio == best_ratio && eb[idx].len > eb[best].len)) {
                best = idx;
                best_ratio = ratio;
              }
            }
            if (best < 0) break;
            sel[best] = 0;
            est_bytes -= (long)eb[best].len;
          }
        }
      }
      free(ops);
    }
    for (i = 0; i < nfuncs; i++)
      if (sel[i]) cov_ops += prof_ops_of(g_prof, g_nprof, funcs[i].digest);

    f = fopen(out, "w");
    if (!f) {
      fprintf(stderr, "qjsaot: cannot write %s\n", out);
      for (i = 0; i < nfuncs; i++) free(eb[i].buf);
      free(eb);
      free(uniq);
      free(sel);
      js_aot_gen_free(rt, funcs);
      JS_FreeContext(ctx);
      JS_FreeRuntime(rt);
      return 2;
    }

    fprintf(
        f,
        "/* Generated by qjsaot from the register-VM bytecode. Do not edit. "
        "*/\n");
    fprintf(f, "#include \"quickjs-aot-rt.h\"\n\n");

    for (i = 0; i < nfuncs; i++) {
      if (!sel[i] || !eb[i].buf) continue;
      fwrite(eb[i].buf, 1, eb[i].len, f);
      actual_bytes += (long)eb[i].len;
      emitted++;
    }

    fprintf(f, "const JSAotEntry qjs_aot_table[] = {\n");
    for (i = 0; i < nfuncs; i++) {
      char hex[33];
      if (!sel[i]) continue;
      hex16(funcs[i].digest, hex);
      fprintf(f, "    { {");
      {
        int k;
        for (k = 0; k < 16; k++)
          fprintf(f, "%s0x%02x", k ? ", " : "", funcs[i].digest[k]);
      }
      fprintf(f, "}, aot_f_%s, 0u, 0x%08xu },\n", hex, funcs[i].code_crc);
    }
    fprintf(f, "};\n");
    fprintf(
        f,
        "const uint32_t qjs_aot_count = "
        "sizeof(qjs_aot_table) / sizeof(qjs_aot_table[0]);\n");
    fprintf(f, "const uint32_t qjs_aot_abi = QJS_AOT_ABI;\n");
    fclose(f);

    fprintf(
        stderr,
        "qjsaot: %d functions seen, %d emitted, %lld instructions, "
        "coverage %.1f%% (%llu/%llu profiled ops), est %ld B, "
        "actual %ld B -> %s\n",
        nfuncs, emitted, total_insns,
        total_ops ? 100.0 * (double)cov_ops / (double)total_ops : 100.0,
        cov_ops, total_ops, est_bytes, actual_bytes, out);

    /* The bound fraction: of the profile's function records, how many the
       built bundle still has by digest. A changed bundle or a foreign profile
       drops its functions out silently, and this is the number that shows it. */
    if (g_nprof > 0) {
      int present = 0, hi;
      unsigned long long present_ops = 0, all_ops = 0;
      for (hi = 0; hi < g_nprof; hi++) {
        int ui = find_unit(funcs, nfuncs, g_prof[hi].digest);
        all_ops += g_prof[hi].ops;
        if (ui >= 0) {
          present++;
          present_ops += g_prof[hi].ops;
        }
      }
      fprintf(
          stderr,
          "qjsaot: bound %d/%d profile records (%.1f%% of profiled ops)\n",
          present, g_nprof,
          all_ops ? 100.0 * (double)present_ops / (double)all_ops : 100.0);
    }

    for (i = 0; i < nfuncs; i++) free(eb[i].buf);
    free(eb);
    free(uniq);
    free(sel);
  }

  js_aot_gen_free(rt, funcs);
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
  return fails == 0 ? 0 : 1;
}
