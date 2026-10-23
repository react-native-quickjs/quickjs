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
 * operand can be negative). A value the pass knows nothing about -- an argument, a loaded
 * element, a loop-carried parameter -- is the whole int32 range, so it only ever removes
 * a check, never adds an assumption.
 */
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

/* The operation of an instruction defining one int value: its sub-operation and the
   operand intervals. Returns 0 for an instruction the pass does not model. */
static int model(RCtx *c, const Q2Insn *in, int *sub, R *a, R *b) {
  int32_t imm;
  switch (in->rop) {
    case ROP_binary_imm:
    case ROP_binary_imm_s:
      if (in->narg < 4 || in->arg[1].kind != 'b') return 0;
      if (in->arg[3].kind != 'i' && in->arg[3].kind != 'I') return 0;
      if (in->nuse < 1) return 0;
      imm = (int32_t)in->arg[3].val;
      *sub = (int)in->arg[1].val & ~REG_CMP_INVERT;
      *a = rng(c, in->use[0]);
      b->lo = b->hi = imm;
      return 1;
    case ROP_add_imm_s:
    case ROP_add_imm_self_s:
    case ROP_sub_imm_s:
    case ROP_sub_imm_self_s:
      if (in->narg < 3 || (in->arg[2].kind != 'i' && in->arg[2].kind != 'I')) return 0;
      if (in->nuse < 1) return 0;
      imm = (int32_t)in->arg[2].val;
      *sub = (in->rop == ROP_add_imm_s || in->rop == ROP_add_imm_self_s) ? REG_BIN_ADD : REG_BIN_SUB;
      *a = rng(c, in->use[0]);
      b->lo = b->hi = imm;
      return 1;
    case ROP_inc:
    case ROP_dec:
      if (in->nuse < 1) return 0;
      *sub = in->rop == ROP_inc ? REG_BIN_ADD : REG_BIN_SUB;
      *a = rng(c, in->use[0]);
      b->lo = b->hi = 1;
      return 1;
    case ROP_binary:
      if (in->nuse < 2 || in->sub < 0) return 0;
      *sub = in->sub & ~REG_CMP_INVERT;
      *a = rng(c, in->use[0]);
      *b = rng(c, in->use[1]);
      return 1;
    case ROP_add:
    case ROP_sub:
    case ROP_mul:
      if (in->nuse < 2) return 0;
      *sub = in->rop == ROP_add ? REG_BIN_ADD : in->rop == ROP_sub ? REG_BIN_SUB : REG_BIN_MUL;
      *a = rng(c, in->use[0]);
      *b = rng(c, in->use[1]);
      return 1;
    default:
      return 0;
  }
}

static R rng(RCtx *c, Q2ValueId v) {
  const Q2Value *val;
  const Q2Insn *in;
  R out = rfull();
  int sub, fits;
  R a, b;
  if (v >= (Q2ValueId)c->t->nvalues) return out;
  if (c->state[v] == 2) return c->r[v];
  if (c->state[v] == 1) return out;
  val = &c->fn->values[v];
  c->state[v] = 1;
  if (val->is_const && val->con_kind == Q2_CON_INT) {
    out.lo = out.hi = val->con;
  } else if (c->t->lb[v] == E_LB_I32 && val->insn >= 0 && val->insn < c->fn->ninsns) {
    in = &c->fn->insns[val->insn];
    if (in->ndef == 1 && model(c, in, &sub, &a, &b)) {
      out = apply_op(sub, a, b, &fits);
      out = rclamp(out);
    }
  }
  c->r[v] = out;
  c->state[v] = 2;
  return out;
}

/* Drop the overflow (and -0) exits of every add, subtract and multiply whose operand
   intervals prove the result fits and cannot be -0. */
void e_ranges_run(const Q2Func *fn, ETypes *t) {
  RCtx c;
  int i;
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
    Q2ValueId v;
    int sub, fits;
    R a, b, x;
    if (in->ndef != 1) continue;
    v = in->def[0];
    if (v >= (Q2ValueId)t->nvalues || t->lb[v] != E_LB_I32) continue;
    if (!(t->exits[v] & (E_EX_OVF | E_EX_NEGZERO))) continue;
    if (!model(&c, in, &sub, &a, &b)) continue;
    if (sub != REG_BIN_ADD && sub != REG_BIN_SUB && sub != REG_BIN_MUL) continue;
    x = apply_op(sub, a, b, &fits);
    if (!fits) continue;
    t->exits[v] &= (uint8_t)~E_EX_OVF;
    /* a product is -0 only for a zero product with a negative operand */
    if (sub == REG_BIN_MUL && a.lo >= 0 && b.lo >= 0) t->exits[v] &= (uint8_t)~E_EX_NEGZERO;
    (void)x;
  }
  free(c.r);
  free(c.state);
}
