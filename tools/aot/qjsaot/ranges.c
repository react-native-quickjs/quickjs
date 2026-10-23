/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * Integer value ranges. A small forward pass over the SSA values the analysis made
 * int32: constants, masks, shifts and the arithmetic of ranges give each value an
 * interval, and an add, subtract or multiply whose operand intervals prove the result
 * fits an int32 drops its overflow exit (a multiply also its -0 exit when neither
 * operand can be negative), and arithmetic whose results only reach bitwise operations
 * needs no exits at all (see the truncating chains). A value the pass knows nothing about -- an argument, a loaded
 * element, a loop-carried parameter -- is the whole int32 range, so it only ever removes
 * a check, never adds an assumption.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "analysis.h"
#include "emit.h"
#include "quickjs-regvm.h"

typedef struct R {
  int64_t lo, hi;
} R;

typedef struct RCtx {
  const Q2Func *fn;
  const ETypes *t;
  R *r;
  uint8_t *state; /* 0 unseen, 1 in progress, 2 done */
} RCtx;

static R rfull(void) {
  R x = {INT32_MIN, INT32_MAX};
  return x;
}

static R rclamp(R x) {
  if (x.lo < INT32_MIN) x.lo = INT32_MIN;
  if (x.hi > INT32_MAX) x.hi = INT32_MAX;
  if (x.lo > x.hi) return rfull();
  return x;
}

static int64_t mn(int64_t a, int64_t b) { return a < b ? a : b; }
static int64_t mx(int64_t a, int64_t b) { return a > b ? a : b; }

static R rng(RCtx *c, Q2ValueId v);

/* The interval of `a op b`; *exact is set when the interval is the true mathematical
   result (no int32 wrap), so the caller can tell a proven fit. */
static R apply_op(int sub, R a, R b, int *fits) {
  R x = rfull();
  int64_t p0, p1, p2, p3;
  *fits = 0;
  switch (sub) {
    case REG_BIN_ADD:
      x.lo = a.lo + b.lo;
      x.hi = a.hi + b.hi;
      break;
    case REG_BIN_SUB:
      x.lo = a.lo - b.hi;
      x.hi = a.hi - b.lo;
      break;
    case REG_BIN_MUL:
      p0 = a.lo * b.lo;
      p1 = a.lo * b.hi;
      p2 = a.hi * b.lo;
      p3 = a.hi * b.hi;
      x.lo = mn(mn(p0, p1), mn(p2, p3));
      x.hi = mx(mx(p0, p1), mx(p2, p3));
      break;
    case REG_BIN_AND:
      if (b.lo == b.hi && b.lo >= 0) {
        x.lo = 0;
        x.hi = b.lo;
      } else if (a.lo == a.hi && a.lo >= 0) {
        x.lo = 0;
        x.hi = a.lo;
      } else if (a.lo >= 0 && b.lo >= 0) {
        x.lo = 0;
        x.hi = mn(a.hi, b.hi);
      } else if (a.lo >= 0) {
        x.lo = 0;
        x.hi = a.hi;
      } else if (b.lo >= 0) {
        x.lo = 0;
        x.hi = b.hi;
      }
      return x;
    case REG_BIN_OR:
    case REG_BIN_XOR:
      if (a.lo >= 0 && b.lo >= 0) {
        int64_t m = mx(a.hi, b.hi), p = 1;
        while (p <= m) p <<= 1;
        x.lo = 0;
        x.hi = p - 1;
      }
      return x;
    case REG_BIN_SHL:
      if (b.lo == b.hi && b.lo >= 0 && b.lo < 32) {
        int k = (int)b.lo;
        int64_t lo = a.lo * ((int64_t)1 << k), hi = a.hi * ((int64_t)1 << k);
        if (lo >= INT32_MIN && hi <= INT32_MAX) {
          x.lo = lo;
          x.hi = hi;
        }
      }
      return x;
    case REG_BIN_SAR:
      if (b.lo == b.hi && b.lo >= 0 && b.lo < 32) {
        x.lo = a.lo >> (int)b.lo;
        x.hi = a.hi >> (int)b.lo;
      }
      return x;
    case REG_BIN_SHR:
      if (b.lo == b.hi && b.lo >= 1 && b.lo < 32) {
        if (a.lo >= 0) {
          x.lo = a.lo >> (int)b.lo;
          x.hi = a.hi >> (int)b.lo;
        } else {
          x.lo = 0;
          x.hi = (int64_t)(0xFFFFFFFFu >> (int)b.lo);
        }
      } else if (b.lo == b.hi && b.lo == 0 && a.lo >= 0) {
        x = a;
      }
      return x;
    default:
      return x;
  }
  *fits = x.lo >= INT32_MIN && x.hi <= INT32_MAX;
  return x;
}

/* The operation that defines def `d` of an instruction: its sub-operation, the operand
   values (`vb` is Q2_NO_VALUE for an immediate, which `*imm` holds) and, for a fused
   multiply-add pair, which half it is. Returns 0 for an instruction the pass does not
   model. */
static int model_def(const Q2Insn *in, int d, int *sub, Q2ValueId *va, Q2ValueId *vb, int32_t *imm) {
  *vb = Q2_NO_VALUE;
  switch (in->rop) {
    case ROP_binary_imm:
    case ROP_binary_imm_s:
      if (d != 0 || in->ndef != 1) return 0;
      if (in->narg < 4 || in->arg[1].kind != 'b') return 0;
      if (in->arg[3].kind != 'i' && in->arg[3].kind != 'I') return 0;
      if (in->nuse < 1) return 0;
      *imm = (int32_t)in->arg[3].val;
      *sub = (int)in->arg[1].val & ~REG_CMP_INVERT;
      *va = in->use[0];
      return 1;
    case ROP_add_imm_s:
    case ROP_add_imm_self_s:
    case ROP_sub_imm_s:
    case ROP_sub_imm_self_s:
      if (d != 0 || in->ndef != 1) return 0;
      if (in->narg < 3 || (in->arg[2].kind != 'i' && in->arg[2].kind != 'I')) return 0;
      if (in->nuse < 1) return 0;
      *imm = (int32_t)in->arg[2].val;
      *sub = (in->rop == ROP_add_imm_s || in->rop == ROP_add_imm_self_s) ? REG_BIN_ADD : REG_BIN_SUB;
      *va = in->use[0];
      return 1;
    case ROP_inc:
    case ROP_dec:
      if (d != 0 || in->ndef != 1) return 0;
      if (in->nuse < 1) return 0;
      *sub = in->rop == ROP_inc ? REG_BIN_ADD : REG_BIN_SUB;
      *imm = 1;
      *va = in->use[0];
      return 1;
    case ROP_binary:
      if (d != 0 || in->ndef != 1) return 0;
      if (in->nuse < 2 || in->sub < 0) return 0;
      *sub = in->sub & ~REG_CMP_INVERT;
      *va = in->use[0];
      *vb = in->use[1];
      return 1;
    case ROP_add:
    case ROP_sub:
    case ROP_mul:
      if (d != 0 || in->ndef != 1) return 0;
      if (in->nuse < 2) return 0;
      *sub = in->rop == ROP_add ? REG_BIN_ADD : in->rop == ROP_sub ? REG_BIN_SUB : REG_BIN_MUL;
      *va = in->use[0];
      *vb = in->use[1];
      return 1;
    case ROP_mul_add:
    case ROP_mul_sub:
    case ROP_add_mul:
    case ROP_sub_mul:
      if (in->ndef != 2 || in->nuse < 3 || d < 0 || d > 1) return 0;
      if (d == 0) {
        *sub = REG_BIN_MUL;
        *va = in->use[0];
        *vb = in->use[1];
      } else {
        *sub = (in->rop == ROP_mul_add || in->rop == ROP_add_mul) ? REG_BIN_ADD : REG_BIN_SUB;
        if (in->rop == ROP_mul_add || in->rop == ROP_mul_sub) {
          *va = in->def[0];
          *vb = in->use[2];
        } else {
          *va = in->use[2];
          *vb = in->def[0];
        }
      }
      return 1;
    default:
      return 0;
  }
}

static R operand_range(RCtx *c, Q2ValueId v, int32_t imm) {
  R x;
  if (v == Q2_NO_VALUE) {
    x.lo = x.hi = imm;
    return x;
  }
  return rng(c, v);
}

static int def_index(const Q2Insn *in, Q2ValueId v) {
  int d;
  for (d = 0; d < in->ndef; d++)
    if (in->def[d] == v) return d;
  return -1;
}

static R rng(RCtx *c, Q2ValueId v) {
  const Q2Value *val;
  const Q2Insn *in;
  R out = rfull();
  int sub, fits, d;
  Q2ValueId va, vb;
  int32_t imm = 0;
  if (v >= (Q2ValueId)c->t->nvalues) return out;
  if (c->state[v] == 2) return c->r[v];
  if (c->state[v] == 1) return out;
  val = &c->fn->values[v];
  c->state[v] = 1;
  if (val->is_const && val->con_kind == Q2_CON_INT) {
    out.lo = out.hi = val->con;
  } else if (c->t->lb[v] == E_LB_I32 && val->insn >= 0 && val->insn < c->fn->ninsns) {
    in = &c->fn->insns[val->insn];
    d = def_index(in, v);
    if (d >= 0 && model_def(in, d, &sub, &va, &vb, &imm)) {
      R a = rng(c, va), b = operand_range(c, vb, imm);
      out = apply_op(sub, a, b, &fits);
      out = rclamp(out);
    }
  }
  c->r[v] = out;
  c->state[v] = 2;
  return out;
}

/* ---- truncating chains ---------------------------------------------------
   An add, subtract or multiply whose result only ever reaches bitwise operations
   (which take ToInt32 of their operands), directly or through other such arithmetic,
   needs no overflow or -0 exit: the exact result is a double (the operand intervals
   keep it under 2^52, so the double is exact), and ToInt32 of an exact integer is its
   low 32 bits, which is what the wrapping int32 arithmetic computes. Every consumer
   must be one of these, so a value that also feeds a store, a call, a loop-carried
   parameter or a comparison keeps its exits. */
typedef struct TC {
  int *head, *enext, *eto, nedge, ecap;
  uint8_t *bad;  /* a consumer that needs the exact int32 */
  uint8_t *cand; /* the result of a chain operation */
  uint8_t *ok;
  R *x;
  uint8_t *xs;
} TC;

static int is_bitwise(int sub) {
  return sub == REG_BIN_AND || sub == REG_BIN_OR || sub == REG_BIN_XOR || sub == REG_BIN_SHL ||
         sub == REG_BIN_SAR || sub == REG_BIN_SHR;
}

#define XCAP ((int64_t)1 << 52)

/* The interval of the exact (unwrapped) value of v, assuming every chain operation
   is unchecked. Sets *wide when a bound leaves 2^52. */
static R xrange(RCtx *c, TC *tc, Q2ValueId v, int *wide) {
  const Q2Value *val = &c->fn->values[v];
  R out = rfull();
  if (!tc->cand[v]) return rng(c, v);
  if (tc->xs[v] == 2) return tc->x[v];
  if (tc->xs[v] == 1) {
    *wide = 1;
    return out;
  }
  tc->xs[v] = 1;
  {
    const Q2Insn *in = &c->fn->insns[val->insn];
    int sub, d = def_index(in, v), k;
    Q2ValueId va, vb;
    int32_t imm = 0;
    R a, b;
    __int128 lo, hi, p[4];
    if (d < 0 || !model_def(in, d, &sub, &va, &vb, &imm)) {
      *wide = 1;
    } else {
      a = xrange(c, tc, va, wide);
      if (vb == Q2_NO_VALUE) b.lo = b.hi = imm;
      else b = xrange(c, tc, vb, wide);
      switch (sub) {
        case REG_BIN_ADD:
          lo = (__int128)a.lo + b.lo;
          hi = (__int128)a.hi + b.hi;
          break;
        case REG_BIN_SUB:
          lo = (__int128)a.lo - b.hi;
          hi = (__int128)a.hi - b.lo;
          break;
        default:
          p[0] = (__int128)a.lo * b.lo;
          p[1] = (__int128)a.lo * b.hi;
          p[2] = (__int128)a.hi * b.lo;
          p[3] = (__int128)a.hi * b.hi;
          lo = hi = p[0];
          for (k = 1; k < 4; k++) {
            if (p[k] < lo) lo = p[k];
            if (p[k] > hi) hi = p[k];
          }
          break;
      }
      if (lo < -XCAP || hi > XCAP) {
        *wide = 1;
      } else {
        out.lo = (int64_t)lo;
        out.hi = (int64_t)hi;
      }
    }
  }
  tc->x[v] = out;
  tc->xs[v] = 2;
  return out;
}

static int tc_edge(TC *tc, Q2ValueId from, Q2ValueId to) {
  if (tc->nedge == tc->ecap) {
    int nc = tc->ecap ? tc->ecap * 2 : 64;
    int *a = realloc(tc->eto, sizeof(int) * (size_t)nc);
    int *b;
    if (a == NULL) return -1;
    tc->eto = a;
    b = realloc(tc->enext, sizeof(int) * (size_t)nc);
    if (b == NULL) return -1;
    tc->enext = b;
    tc->ecap = nc;
  }
  tc->eto[tc->nedge] = (int)to;
  tc->enext[tc->nedge] = tc->head[from];
  tc->head[from] = tc->nedge++;
  return 0;
}

static void truncating_chains(RCtx *c, ETypes *t) {
  const Q2Func *fn = c->fn;
  size_t nv = (size_t)(t->nvalues > 0 ? t->nvalues : 1);
  TC tc;
  int i, k, d, changed;
  memset(&tc, 0, sizeof tc);
  tc.head = malloc(sizeof(int) * nv);
  tc.bad = calloc(nv, 1);
  tc.cand = calloc(nv, 1);
  tc.ok = calloc(nv, 1);
  tc.x = calloc(nv, sizeof(R));
  tc.xs = calloc(nv, 1);
  if (tc.head == NULL || tc.bad == NULL || tc.cand == NULL || tc.ok == NULL || tc.x == NULL || tc.xs == NULL)
    goto done;
  for (i = 0; i < (int)nv; i++) tc.head[i] = -1;
  /* the chain operations: an int32 add, subtract or multiply that still has an exit */
  for (i = 0; i < fn->ninsns; i++) {
    const Q2Insn *in = &fn->insns[i];
    for (d = 0; d < in->ndef; d++) {
      int sub;
      Q2ValueId va, vb, v = in->def[d];
      int32_t imm;
      if (v >= (Q2ValueId)t->nvalues || t->lb[v] != E_LB_I32) continue;
      if (!(t->exits[v] & (E_EX_OVF | E_EX_NEGZERO))) continue;
      if (!model_def(in, d, &sub, &va, &vb, &imm)) continue;
      if (sub != REG_BIN_ADD && sub != REG_BIN_SUB && sub != REG_BIN_MUL) continue;
      if (in->rop == ROP_inc || in->rop == ROP_dec) continue;
      tc.cand[v] = 1;
    }
  }
  /* the consumers of every value */
  for (i = 0; i < fn->ninsns; i++) {
    const Q2Insn *in = &fn->insns[i];
    int used[8];
    for (k = 0; k < in->nuse && k < 8; k++) used[k] = 0;
    for (d = 0; d < in->ndef; d++) {
      int sub;
      Q2ValueId va, vb, v = in->def[d];
      int32_t imm;
      if (!model_def(in, d, &sub, &va, &vb, &imm)) continue;
      /* the operands this definition reads: a bitwise one takes ToInt32 of them, a chain
         operation hands them its own fate, anything else needs them exact */
      for (k = 0; k < in->nuse && k < 8; k++) {
        if (in->use[k] != va && in->use[k] != vb) continue;
        if (is_bitwise(sub)) {
          used[k] = 1;
        } else if (v < (Q2ValueId)t->nvalues && tc.cand[v]) {
          if (tc_edge(&tc, in->use[k], v) < 0) goto done;
          used[k] = 1;
        }
      }
      /* the product of a fused pair feeds its second half */
      if (va == in->def[0] && d == 1 && tc.cand[v] && tc_edge(&tc, in->def[0], v) < 0) goto done;
      if (vb == in->def[0] && d == 1 && tc.cand[v] && tc_edge(&tc, in->def[0], v) < 0) goto done;
    }
    for (k = 0; k < in->nuse && k < 8; k++)
      if (!used[k] && in->use[k] < (Q2ValueId)t->nvalues) tc.bad[in->use[k]] = 1;
    /* an operand not accounted above (a use slot past the table) is exact-needing too */
    for (k = 8; k < in->nuse; k++)
      if (in->use[k] < (Q2ValueId)t->nvalues) tc.bad[in->use[k]] = 1;
    for (k = 0; k < in->win_n; k++)
      if (in->win_val[k] < (Q2ValueId)t->nvalues) tc.bad[in->win_val[k]] = 1;
    /* a fused pair's product is also a register the rest of the function may read: its
       other consumers are found through their own use slots */
  }
  for (i = 0; i < fn->nblocks; i++) {
    const Q2Block *b = &fn->blocks[i];
    int n;
    for (n = 0; n < 2; n++) {
      if (b->edge_arg == NULL || b->edge_arg[n] == NULL) continue;
      for (k = 0; k < b->nedge_args[n]; k++)
        if (b->edge_arg[n][k] < (Q2ValueId)t->nvalues) tc.bad[b->edge_arg[n][k]] = 1;
    }
  }
  /* a value is a candidate only if no consumer needs the exact int32 ... */
  for (i = 0; i < (int)nv; i++) tc.ok[i] = tc.cand[i] && !tc.bad[i];
  /* ... and its exact interval stays under 2^52 */
  for (i = 0; i < (int)nv; i++) {
    int wide = 0;
    if (!tc.ok[i]) continue;
    (void)xrange(c, &tc, (Q2ValueId)i, &wide);
    if (wide) tc.ok[i] = 0;
  }
  /* ... and so does every chain operation it feeds */
  do {
    changed = 0;
    for (i = 0; i < (int)nv; i++) {
      int e;
      if (!tc.ok[i]) continue;
      for (e = tc.head[i]; e >= 0; e = tc.enext[e])
        if (!tc.ok[tc.eto[e]]) {
          tc.ok[i] = 0;
          changed = 1;
          break;
        }
    }
  } while (changed);
  if (getenv("QJS_AOT_DEBUG_TRUNC") != NULL)
    for (i = 0; i < (int)nv; i++)
      if (tc.cand[i])
        fprintf(stderr, "trunc v%d pc=%d bad=%d ok=%d\n", i, fn->insns[fn->values[i].insn].pc, tc.bad[i], tc.ok[i]);
  for (i = 0; i < (int)nv; i++)
    if (tc.ok[i]) t->exits[i] &= (uint8_t) ~(E_EX_OVF | E_EX_NEGZERO);
done:
  free(tc.head);
  free(tc.enext);
  free(tc.eto);
  free(tc.bad);
  free(tc.cand);
  free(tc.ok);
  free(tc.x);
  free(tc.xs);
}

/* Drop the overflow (and -0) exits of every add, subtract and multiply whose operand
   intervals prove the result fits and cannot be -0. */
void e_ranges_run(const Q2Func *fn, ETypes *t) {
  RCtx c;
  int i, d;
  if (getenv("QJS_AOT_NORANGES") != NULL) return;
  c.fn = fn;
  c.t = t;
  c.r = calloc((size_t)(t->nvalues > 0 ? t->nvalues : 1), sizeof(R));
  c.state = calloc((size_t)(t->nvalues > 0 ? t->nvalues : 1), 1);
  if (c.r == NULL || c.state == NULL) {
    free(c.r);
    free(c.state);
    return;
  }
  for (i = 0; i < fn->ninsns; i++) {
    const Q2Insn *in = &fn->insns[i];
    for (d = 0; d < in->ndef; d++) {
      Q2ValueId v = in->def[d], va, vb;
      int sub, fits;
      int32_t imm = 0;
      R a, b;
      if (v >= (Q2ValueId)t->nvalues || t->lb[v] != E_LB_I32) continue;
      if (!(t->exits[v] & (E_EX_OVF | E_EX_NEGZERO))) continue;
      if (!model_def(in, d, &sub, &va, &vb, &imm)) continue;
      if (sub != REG_BIN_ADD && sub != REG_BIN_SUB && sub != REG_BIN_MUL) continue;
      a = rng(&c, va);
      b = operand_range(&c, vb, imm);
      (void)apply_op(sub, a, b, &fits);
      if (!fits) continue;
      t->exits[v] &= (uint8_t)~E_EX_OVF;
      /* a product is -0 only for a zero product with a negative operand */
      if (sub == REG_BIN_MUL && a.lo >= 0 && b.lo >= 0) t->exits[v] &= (uint8_t)~E_EX_NEGZERO;
    }
  }
  truncating_chains(&c, t);
  free(c.r);
  free(c.state);
}
