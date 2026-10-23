/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * The pure recursive form of a function: `aot_p_HEX`, a plain C function over int32 arguments
 * for a function that does nothing but integer arithmetic, compares, branches and calls to
 * itself (fib, ackermann, tak, gcd). It has no frame, no roots and no value boxing.
 *
 * It is only an attempt. Anything the function would do beyond int32 arithmetic (an overflow, a
 * -0, a stack that gets too deep, an argument that is not an int) returns Q2_PFAIL, and the
 * entry then runs the ordinary body from the start: the function has no effect, so doing the
 * work twice is not observable, and every exception, stack overflow and Error.stack is the
 * ordinary body's. The stack is checked on every call against a limit that stays well inside
 * what the ordinary body would allow, and the interrupt counter is decremented on every call.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "analysis.h"
#include "emit.h"
#include "ir.h"

int qjs_aot_pool_num(const struct JSAotGenFunc *g, uint32_t idx, double *out);
int qjs_aot_pool_tag(const struct JSAotGenFunc *g, uint32_t idx);
#define E_POOL_INT 1

/* The function being prepared: its text, the global sites its calls read and its arguments. */
static char *pure_text;
static size_t pure_len;
static int pure_nargs, pure_nsites, pure_sites[8];
static char pure_hex[33];

#define PV_NONE 0
#define PV_INT 1
#define PV_CALLEE 2

typedef struct PCtx {
  FILE *o;
  const Q2Func *fn;
  uint8_t *kind;
  int ok;
  const char *hex;
  int nslots;
} PCtx;

static void p_fail_at(PCtx *c, int line) {
  if (c->ok && getenv("QJS_AOT_DEBUG_PUREREC") != NULL) fprintf(stderr, "purerec %s: refused at pure.c:%d\n", c->hex, line);
  c->ok = 0;
}
#define p_fail(c) p_fail_at((c), __LINE__)

static int p_int(PCtx *c, Q2ValueId v) {
  if (v == Q2_NO_VALUE || v >= (Q2ValueId)c->fn->nvalues || c->kind[v] != PV_INT) {
    p_fail(c);
    return 0;
  }
  return 1;
}

/* The int32 operation `sub` on `a` and `b` into `d`; the C text, or NULL when there is none. */
static void p_arith(PCtx *c, int sub, const char *a, const char *b, Q2ValueId d) {
  FILE *o = c->o;
  switch (sub) {
    case REG_BIN_ADD:
    case REG_BIN_SUB:
    case REG_BIN_MUL: {
      const char *bi = sub == REG_BIN_ADD ? "add" : (sub == REG_BIN_SUB ? "sub" : "mul");
      fprintf(o, "    if (__builtin_%s_overflow(%s, %s, &x%u)) Q2P_RET(Q2_PFAIL);\n", bi, a, b, (unsigned)d);
      if (sub == REG_BIN_MUL)
        fprintf(o, "    if (x%u == 0 && (%s < 0 || %s < 0)) Q2P_RET(Q2_PFAIL);\n", (unsigned)d, a, b);
      break;
    }
    case REG_BIN_AND:
      fprintf(o, "    x%u = %s & %s;\n", (unsigned)d, a, b);
      break;
    case REG_BIN_OR:
      fprintf(o, "    x%u = %s | %s;\n", (unsigned)d, a, b);
      break;
    case REG_BIN_XOR:
      fprintf(o, "    x%u = %s ^ %s;\n", (unsigned)d, a, b);
      break;
    case REG_BIN_SHL:
      fprintf(o, "    x%u = (int32_t)((uint32_t)%s << (%s & 31));\n", (unsigned)d, a, b);
      break;
    case REG_BIN_SAR:
      fprintf(o, "    x%u = %s >> (%s & 31);\n", (unsigned)d, a, b);
      break;
    case REG_BIN_SHR:
      fprintf(o, "    { uint32_t u_ = (uint32_t)%s >> (%s & 31);\n", a, b);
      fprintf(o, "      if (u_ > (uint32_t)INT32_MAX) Q2P_RET(Q2_PFAIL);\n");
      fprintf(o, "      x%u = (int32_t)u_; }\n", (unsigned)d);
      break;
    case REG_BIN_MOD:
      fprintf(o, "    if (%s == 0 || (%s == INT32_MIN && %s == -1)) Q2P_RET(Q2_PFAIL);\n", b, a, b);
      fprintf(o, "    x%u = %s %% %s;\n", (unsigned)d, a, b);
      fprintf(o, "    if (x%u == 0 && %s < 0) Q2P_RET(Q2_PFAIL);\n", (unsigned)d, a);
      break;
    case REG_BIN_DIV:
      fprintf(o, "    if (%s == 0 || (%s == INT32_MIN && %s == -1) || %s %% %s != 0 ||\n", b, a, b, a, b);
      fprintf(o, "        (%s == 0 && %s < 0)) Q2P_RET(Q2_PFAIL);\n", a, b);
      fprintf(o, "    x%u = %s / %s;\n", (unsigned)d, a, b);
      break;
    default:
      p_fail(c);
      break;
  }
}

static void p_edge(PCtx *c, int b, int k) {
  const Q2Block *blk = &c->fn->blocks[b];
  int sb = blk->succ_block[k], j, np;
  const Q2Block *s;
  if (sb < 0 || sb >= c->fn->nblocks) {
    p_fail(c);
    return;
  }
  s = &c->fn->blocks[sb];
  np = s->nparams;
  if (blk->nedge_args[k] >= 0 && blk->nedge_args[k] < np) np = blk->nedge_args[k];
  if (np > 0) {
    fprintf(c->o, "      {\n");
    for (j = 0; j < np; j++) {
      if (blk->edge_arg[k][j] == Q2_NO_VALUE) continue;
      if (!p_int(c, blk->edge_arg[k][j]) || !p_int(c, s->param_val[j])) return;
      fprintf(c->o, "        int32_t m%d_ = x%u;\n", j, (unsigned)blk->edge_arg[k][j]);
    }
    for (j = 0; j < np; j++) {
      if (blk->edge_arg[k][j] == Q2_NO_VALUE) continue;
      fprintf(c->o, "        x%u = m%d_;\n", (unsigned)s->param_val[j], j);
    }
    fprintf(c->o, "      }\n");
  }
  fprintf(c->o, "      goto pb%d;\n", sb);
}

static const char *p_cmp(int base) {
  switch (base) {
    case REG_BIN_LT: return "<";
    case REG_BIN_LTE: return "<=";
    case REG_BIN_GT: return ">";
    case REG_BIN_GTE: return ">=";
    case REG_BIN_EQ:
    case REG_BIN_STRICT_EQ: return "==";
    case REG_BIN_NEQ:
    case REG_BIN_STRICT_NEQ: return "!=";
    default: return NULL;
  }
}

static void p_insn(PCtx *c, const Q2Insn *in) {
  const Q2Func *fn = c->fn;
  char a[24], b[24];
  int sub, imm;
  Q2ValueId d;
  if (in->op == Q2_OP_FALLBACK && (in->rop == ROP_mod || in->rop == ROP_div) && in->ndef == 1 &&
      in->nuse == 2 && in->nredef == 0) {
    if (!p_int(c, in->use[0]) || !p_int(c, in->use[1])) return;
    snprintf(a, sizeof(a), "x%u", (unsigned)in->use[0]);
    snprintf(b, sizeof(b), "x%u", (unsigned)in->use[1]);
    c->kind[in->def[0]] = PV_INT;
    p_arith(c, in->rop == ROP_mod ? REG_BIN_MOD : REG_BIN_DIV, a, b, in->def[0]);
    return;
  }
  switch (in->op) {
    case Q2_OP_CONST: {
      int32_t cv;
      d = in->def[0];
      if (in->ndef != 1) {
        p_fail(c);
        return;
      }
      if (in->rop == ROP_load_i8 || in->rop == ROP_load_i8_s || in->rop == ROP_load_i32) {
        cv = (int32_t)in->arg[1].val;
      } else if (in->rop == ROP_load_const && in->narg >= 3 && in->arg[2].kind == 'c') {
        double pv;
        uint32_t idx = (uint32_t)in->arg[2].val;
        if (!qjs_aot_pool_num(fn->g, idx, &pv) || qjs_aot_pool_tag(fn->g, idx) != E_POOL_INT) {
          p_fail(c);
          return;
        }
        cv = (int32_t)pv;
      } else {
        p_fail(c);
        return;
      }
      c->kind[d] = PV_INT;
      fprintf(c->o, "    x%u = %d;\n", (unsigned)d, cv);
      return;
    }
    case Q2_OP_MOVE:
      if (in->nuse != 1 || in->ndef != 1 || !p_int(c, in->use[0])) {
        p_fail(c);
        return;
      }
      c->kind[in->def[0]] = PV_INT;
      fprintf(c->o, "    x%u = x%u;\n", (unsigned)in->def[0], (unsigned)in->use[0]);
      return;
    case Q2_OP_GET_ARG: {
      int slot = (int)in->arg[1].val;
      if (in->rop != ROP_get_arg || in->ndef != 1 || slot < 0 || slot > 3) {
        p_fail(c);
        return;
      }
      if (slot + 1 > c->nslots) c->nslots = slot + 1;
      c->kind[in->def[0]] = PV_INT;
      fprintf(c->o, "    x%u = a%d;\n", (unsigned)in->def[0], slot);
      return;
    }
    case Q2_OP_GET_ARG2: {
      int s0 = (int)in->arg[1].val, s1 = (int)in->arg[3].val;
      if (in->rop != ROP_get_arg2 || in->ndef != 2 || s0 < 0 || s0 > 3 || s1 < 0 || s1 > 3) {
        p_fail(c);
        return;
      }
      if (s0 + 1 > c->nslots) c->nslots = s0 + 1;
      if (s1 + 1 > c->nslots) c->nslots = s1 + 1;
      c->kind[in->def[0]] = c->kind[in->def[1]] = PV_INT;
      fprintf(c->o, "    x%u = a%d;\n    x%u = a%d;\n", (unsigned)in->def[0], s0, (unsigned)in->def[1], s1);
      return;
    }
    case Q2_OP_GET_GLOBAL:
      if (in->rop != ROP_get_global || in->ndef != 1 || in->narg < 4 || in->arg[3].kind != 's' ||
          pure_nsites >= 8) {
        p_fail(c);
        return;
      }
      c->kind[in->def[0]] = PV_CALLEE;
      return;
    case Q2_OP_BINARY:
      if (in->ndef != 1) {
        p_fail(c);
        return;
      }
      d = in->def[0];
      switch (in->rop) {
        case ROP_add: sub = REG_BIN_ADD; break;
        case ROP_sub: sub = REG_BIN_SUB; break;
        case ROP_mul: sub = REG_BIN_MUL; break;
        case ROP_bitand: sub = REG_BIN_AND; break;
        case ROP_bitor: sub = REG_BIN_OR; break;
        case ROP_bitxor: sub = REG_BIN_XOR; break;
        case ROP_shl: sub = REG_BIN_SHL; break;
        case ROP_sar: sub = REG_BIN_SAR; break;
        case ROP_shr: sub = REG_BIN_SHR; break;
        case ROP_binary:
        case ROP_binary_imm:
        case ROP_binary_imm_s:
          if (in->narg < 3 || in->arg[1].kind != 'b') {
            p_fail(c);
            return;
          }
          sub = (int)in->arg[1].val;
          break;
        case ROP_add_imm_s: sub = REG_BIN_ADD; break;
        case ROP_sub_imm_s: sub = REG_BIN_SUB; break;
        default:
          p_fail(c);
          return;
      }
      if (in->nuse < 1 || !p_int(c, in->use[0])) {
        p_fail(c);
        return;
      }
      snprintf(a, sizeof(a), "x%u", (unsigned)in->use[0]);
      if (in->rop == ROP_binary_imm || in->rop == ROP_binary_imm_s || in->rop == ROP_add_imm_s ||
          in->rop == ROP_sub_imm_s) {
        imm = in->rop == ROP_add_imm_s || in->rop == ROP_sub_imm_s ? 2 : 3;
        if (in->narg <= imm || (in->arg[imm].kind != 'I' && in->arg[imm].kind != 'i')) {
          p_fail(c);
          return;
        }
        snprintf(b, sizeof(b), "(%d)", (int)(int32_t)in->arg[imm].val);
      } else {
        if (in->nuse < 2 || !p_int(c, in->use[1])) {
          p_fail(c);
          return;
        }
        snprintf(b, sizeof(b), "x%u", (unsigned)in->use[1]);
      }
      c->kind[d] = PV_INT;
      p_arith(c, sub, a, b, d);
      return;
    case Q2_OP_CALL: {
      Q2ValueId cv;
      const Q2Insn *gi;
      int k, site;
      if (in->rop != ROP_call || in->ndef != 1 || in->nuse < 1 || in->win_n < c->nslots) {
        p_fail(c);
        return;
      }
      cv = in->use[0];
      if (cv == Q2_NO_VALUE || cv >= (Q2ValueId)fn->nvalues || c->kind[cv] != PV_CALLEE ||
          fn->values[cv].insn < 0) {
        p_fail(c);
        return;
      }
      gi = &fn->insns[fn->values[cv].insn];
      site = (int)gi->arg[3].val;
      /* the callee is whatever the global holds when the pure form is entered: the entry checks
         that it is this function, and nothing run in between can change it */
      for (k = 0; k < pure_nsites && pure_sites[k] != site; k++) {}
      if (k == pure_nsites) pure_sites[pure_nsites++] = site;
      /* the arguments the callee reads; the call is a plain C call */
      fprintf(c->o, "    Q2P_CHECK();\n");
      fprintf(c->o, "    { Q2PR r_ = aot_p_%s(e_, b_", c->hex);
      for (k = 0; k < c->nslots; k++) {
        if (!p_int(c, in->win_val[k])) return;
        fprintf(c->o, ", x%u", (unsigned)in->win_val[k]);
      }
      fprintf(c->o, ");\n      b_ = r_.b;\n      if (r_.r >> 32) return r_;\n      x%u = (int32_t)(uint32_t)r_.r; }\n",
              (unsigned)in->def[0]);
      c->kind[in->def[0]] = PV_INT;
      return;
    }
    case Q2_OP_RETURN:
      if (in->nuse != 1 || !p_int(c, in->use[0])) {
        p_fail(c);
        return;
      }
      fprintf(c->o, "    Q2P_RET((uint64_t)(uint32_t)x%u);\n", (unsigned)in->use[0]);
      return;
    default:
      p_fail(c);
      return;
  }
}

/* The first pass over the argument count: a call before the last get_arg of the block order
   must know every slot, so the slots are counted first. */
static int p_count_slots(const Q2Func *fn) {
  int i, n = 0;
  for (i = 0; i < fn->ninsns; i++) {
    const Q2Insn *in = &fn->insns[i];
    if (in->op == Q2_OP_GET_ARG && in->rop == ROP_get_arg && (int)in->arg[1].val + 1 > n)
      n = (int)in->arg[1].val + 1;
    if (in->op == Q2_OP_GET_ARG2 && in->rop == ROP_get_arg2) {
      if ((int)in->arg[1].val + 1 > n) n = (int)in->arg[1].val + 1;
      if ((int)in->arg[3].val + 1 > n) n = (int)in->arg[3].val + 1;
    }
  }
  return n;
}

void e_pure_clear(void) {
  free(pure_text);
  pure_text = NULL;
  pure_len = 0;
  pure_nargs = pure_nsites = 0;
  pure_hex[0] = 0;
}

/* Decide whether `fn` has a pure recursive form and prepare it. */
void e_pure_plan(const Q2Func *fn, const char hex[33]) {
  PCtx c;
  FILE *o;
  int bi, ii, i, selfcall = 0;

  e_pure_clear();
  if (getenv("QJS_AOT_NOPUREREC") != NULL) return;
  if (fn == NULL || !fn->ok || fn->g->ctl_count != 0 || fn->nblocks <= 0) return;
  c.nslots = p_count_slots(fn);
  if (c.nslots > 4) return;
  for (i = 0; i < fn->ninsns; i++)
    if (fn->insns[i].op == Q2_OP_CALL) selfcall = 1;
  if (!selfcall) return;
  if (getenv("QJS_AOT_DEBUG_PUREREC") != NULL) fprintf(stderr, "purerec %s: trying\n", hex);
  c.fn = fn;
  c.hex = hex;
  c.ok = 1;
  c.kind = calloc((size_t)(fn->nvalues > 0 ? fn->nvalues : 1), 1);
  if (c.kind == NULL) return;
  o = open_memstream(&pure_text, &pure_len);
  if (o == NULL) {
    free(c.kind);
    return;
  }
  c.o = o;
  fprintf(
      o,
      "#ifndef Q2_PFAIL\n#define Q2_PFAIL (1ull << 32)\n#define Q2_PEXC (2ull << 32)\n"
      "typedef struct { uint64_t r; int32_t b; } Q2PR;\n"
      "typedef struct { uintptr_t lim; JSContext *ctx; } Q2PE;\n"
      "#define Q2P_RET(v_) return (Q2PR){ (v_), b_ }\n"
      "#define Q2P_CHECK() do { \\\n"
      "    if (__builtin_expect((uintptr_t)__builtin_frame_address(0) < e_->lim, 0)) Q2P_RET(Q2_PFAIL); \\\n"
      "    if (__builtin_expect(--b_ <= 0, 0)) { \\\n"
      "        b_ = q2_pure_poll(e_->ctx, b_); \\\n"
      "        if (b_ <= 0) Q2P_RET(Q2_PEXC); \\\n"
      "    } } while (0)\n"
      "__attribute__((noinline, cold)) static int32_t q2_pure_poll(JSContext *ctx_in, int32_t b)\n"
      "{\n"
      "    int *cnt = (int *)((uint8_t *)ctx_in + QJS_AOT_OFF_CTX_INTERRUPT_COUNTER);\n"
      "    *cnt = b + 1;\n"
      "    if (qjs_aot_poll(ctx_in) < 0) return 0;\n"
      "    return *cnt > 0 ? *cnt : 1;\n"
      "}\n#endif\n");
  fprintf(o, "static uint8_t e_pdis_%s, e_pfail_%s;\n", hex, hex);
  fprintf(o, "E_TEXT static Q2PR aot_p_%s(const Q2PE *e_, int32_t b_", hex);
  for (i = 0; i < c.nslots; i++) fprintf(o, ", int32_t a%d", i);
  fprintf(o, ")\n{\n");
  for (i = 0; i < fn->nvalues; i++) fprintf(o, "    int32_t x%d = 0;\n", i);
  fprintf(
      o,
      "    goto pb%d;\n",
      fn->entry_block);
  for (bi = 0; bi < fn->nrpo && c.ok; bi++) {
    int b = fn->rpo[bi];
    const Q2Block *blk = &fn->blocks[b];
    int terminated = 0;
    if (fn->blocks[b].resume) {
      c.ok = 0;
      break;
    }
    fprintf(o, "  pb%d: (void)0;\n", b);
    for (ii = 0; ii < blk->ninsns && c.ok; ii++) {
      const Q2Insn *in = &fn->insns[blk->first_insn + ii];
      const ERole *role = e_roles[in->rop];
      if (in->op == Q2_OP_BRANCH_CMP) {
        int sub, inv, base;
        const char *op;
        if ((in->rop != ROP_branch_cmp_reg_reg && in->rop != ROP_branch_cmp_reg_reg_s) || in->narg < 4 ||
            in->arg[0].kind != 'b' || in->nuse < 2 || blk->nsucc != 2 || ii + 1 != blk->ninsns) {
          c.ok = 0;
          break;
        }
        sub = (int)in->arg[0].val;
        inv = (sub & REG_CMP_INVERT) != 0;
        base = sub & ~REG_CMP_INVERT;
        if (!p_int(&c, in->use[0]) || !p_int(&c, in->use[1])) break;
        op = p_cmp(base);
        if (op != NULL) {
          fprintf(o, "    if (%s(x%u %s x%u)) {\n", inv ? "!" : "", (unsigned)in->use[0], op, (unsigned)in->use[1]);
        } else if (base == REG_BIN_AND || base == REG_BIN_OR || base == REG_BIN_XOR) {
          fprintf(
              o, "    if (%s((x%u %c x%u) != 0)) {\n", inv ? "!" : "", (unsigned)in->use[0],
              base == REG_BIN_AND ? '&' : (base == REG_BIN_OR ? '|' : '^'), (unsigned)in->use[1]);
        } else {
          c.ok = 0;
          break;
        }
        p_edge(&c, b, 0);
        fprintf(o, "    } else {\n");
        p_edge(&c, b, 1);
        fprintf(o, "    }\n");
        terminated = 1;
        break;
      }
      if (role != NULL && role->control == E_CF_BRANCH && in->nuse == 0 && in->ndef == 0 && blk->nsucc == 1 &&
          ii + 1 == blk->ninsns) {
        continue; /* an unconditional jump: the edge below is it */
      }
      p_insn(&c, in);
      if (in->op == Q2_OP_RETURN) terminated = 1;
    }
    if (!c.ok) break;
    if (!terminated) {
      if (blk->nsucc != 1) {
        c.ok = 0;
        break;
      }
      p_edge(&c, b, 0);
    }
  }
  fprintf(o, "}\n");
  fclose(o);
  free(c.kind);
  if (!c.ok || pure_nsites == 0) {
    if (c.ok && getenv("QJS_AOT_DEBUG_PUREREC") != NULL) fprintf(stderr, "purerec %s: no call sites\n", hex);
    e_pure_clear();
    return;
  }
  if (getenv("QJS_AOT_DEBUG_PUREREC") != NULL) fprintf(stderr, "purerec %s: planned (%d args, %d sites)\n", hex, c.nslots, pure_nsites);
  pure_nargs = c.nslots;
  snprintf(pure_hex, sizeof(pure_hex), "%s", hex);
}

/* The function itself, before the frameless body that calls it. */
void e_pure_emit_fn(FILE *f) {
  if (pure_text != NULL) fputs(pure_text, f);
}

int e_pure_active(void) { return pure_text != NULL; }

/* The attempt at the top of the frameless body: with ints for arguments and the callee
   global still this function, run the pure form; any failure continues into the ordinary body. */
void e_pure_emit_entry(FILE *f) {
  int i;
  if (pure_text == NULL) return;
  fprintf(f, "    if (__builtin_expect(!e_pdis_%s, 1) && argc >= %d) {\n", pure_hex, pure_nargs);
  fprintf(f, "        int32_t pa_[%d];\n        JSValue pg_;\n", pure_nargs > 0 ? pure_nargs : 1);
  fprintf(f, "        if (");
  for (i = 0; i < pure_nargs; i++) fprintf(f, "Q2_I32_OK(argv[%d], pa_[%d]) && ", i, i);
  for (i = 0; i < pure_nsites; i++)
    fprintf(
        f, "q2_global_hit(ctx, pic, %d, &pg_) && q2_call_entry(pg_, (const void *)aot_f_%s)%s", pure_sites[i],
        pure_hex, i + 1 < pure_nsites ? " && " : "");
  fprintf(f, ") {\n");
  fprintf(
      f,
      "            uintptr_t sp_ = (uintptr_t)__builtin_frame_address(0);\n"
      "            uintptr_t rl_ = *(uintptr_t *)(*(uint8_t **)((uint8_t *)ctx_in + QJS_AOT_OFF_CTX_RT) + "
      "QJS_AOT_OFF_RT_STACK_LIMIT) + E_STACK_MARGIN;\n"
      "            if (sp_ > rl_) {\n"
      "                int *cnt_ = (int *)((uint8_t *)ctx_in + QJS_AOT_OFF_CTX_INTERRUPT_COUNTER);\n"
      "                Q2PE pe_ = { sp_ - (sp_ - rl_) / 16, ctx_in };\n"
      "                Q2PR pq_ = aot_p_%s(&pe_, *cnt_",
      pure_hex);
  for (i = 0; i < pure_nargs; i++) fprintf(f, ", pa_[%d]", i);
  fprintf(
      f,
      ");\n"
      "                uint64_t pr_ = pq_.r;\n"
      "                *cnt_ = pq_.b;\n"
      "                if (!(pr_ >> 32)) return JS_MKVAL(JS_TAG_INT, (int32_t)(uint32_t)pr_);\n"
      "                if (pr_ == Q2_PEXC) return JS_EXCEPTION;\n"
      "                if (++e_pfail_%s >= 3) e_pdis_%s = 1;\n"
      "            }\n"
      "        }\n"
      "    }\n",
      pure_hex, pure_hex);
}
