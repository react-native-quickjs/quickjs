/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * Representation inference.
 *
 * The profile says which kinds a definition's register ever carried. Where that
 * is exactly one numeric kind, or `int|float`, the value can live in a C
 * `int32_t`/`double` instead of a boxed `JSValue`, and the arithmetic, compares
 * and branches that consume it become plain C.
 *
 * The lattice is
 *
 *     BOTTOM < I32 < F64 < TAGGED
 *     BOTTOM < BOOL < TAGGED      (BOOL never meets a number: TAGGED)
 *
 * and every step only moves a value up, so the fixpoint terminates. A value
 * still at BOTTOM is unreachable and becomes TAGGED.
 *
 * Two flags travel with a typed value:
 *   spec   a speculated load: its source produced a JSValue, so its C local is
 *          kept alongside the unboxed one and a tag guard sits at the
 *          definition; only a load may carry it.
 *   synced the value's register slot already holds it boxed, so a leave to the
 *          interpreter needs no store for it.
 */
#include <stdlib.h>
#include <string.h>

#include "ir.h"
#include "quickjs-regvm.h"

/* Whether constant-pool entry `idx` is a number (qjsaot.c). */
int qjs_aot_pool_num(const struct JSAotGenFunc *g, uint32_t idx, double *out);

/* The working lattice. */
enum { LB_BOT = 0, LB_BOOL, LB_I32, LB_F64, LB_TAG };

static int lb_join(int a, int b) {
  if (a == LB_BOT) return b;
  if (b == LB_BOT) return a;
  if (a == b) return a;
  if (a == LB_BOOL || b == LB_BOOL) return LB_TAG;
  if (a == LB_I32 && b == LB_F64) return LB_F64;
  if (a == LB_F64 && b == LB_I32) return LB_F64;
  return LB_TAG;
}

static uint8_t lb_rep(int lb) {
  switch (lb) {
    case LB_I32:
      return Q2_R_I32;
    case LB_F64:
      return Q2_R_F64;
    case LB_BOOL:
      return Q2_R_BOOL;
    default:
      return Q2_R_TAGGED;
  }
}

/* A definition whose source produces a JSValue: its record decides whether the
   unboxed form is worth keeping. Any other instruction does not. */
static int q2_is_load(int op) {
  switch (op) {
    case Q2_OP_GET_FIELD:
    case Q2_OP_GET_ELEMENT:
    case Q2_OP_GET_VAR_REF:
    case Q2_OP_GET_ARG:
    case Q2_OP_GET_ARG2:
    case Q2_OP_GET_LOCAL:
    case Q2_OP_GET_GLOBAL:
    case Q2_OP_CALL:
      return 1;
    default:
      return 0;
  }
}

int q2_def_from_record(const Q2Insn *in) {
  return q2_is_load(in->op) || (in->op == Q2_OP_FALLBACK && in->ndef > 0) ||
         in->rop == ROP_neg || in->rop == ROP_inc;
}

/* The profiler writes no record for `inc`, so its representation follows its
   operand instead: an int stays an int (the lowering exits on overflow) and a
   double stays a double. Only when there is no record; a record still
   speculates. */
int q2_inc_by_operand(const Q2Func *fn, const Q2Insn *in) {
  return in->rop == ROP_inc && in->ndef == 1 && in->nuse >= 1 &&
         !fn->values[in->def[0]].has_prof;
}

/* A load's representation from its record. Only an exactly-numeric set
   speculates; anything else stays tagged. */
static int q2_load_lb(const Q2Value *v) {
  if (!v->has_prof) return LB_TAG;
  if (v->ptypes == Q2_T_INT32) return LB_I32;
  if (v->ptypes == Q2_T_NUM) return LB_F64;
  if (v->ptypes == (Q2_T_INT32 | Q2_T_NUM)) return LB_F64;
  return LB_TAG;
}

/* Whether an operand of `in` at index `idx` is the immediate of an
   immediate-form binary, which is always an I32. */
static int q2_imm_operand(const Q2Insn *in, int idx) {
  return idx < in->narg &&
         (in->arg[idx].kind == 'i' || in->arg[idx].kind == 'I');
}

/* The joined representation of a binary's two register operands, with the
   immediate forms giving the constant an I32. */
static int q2_binary_operands(
    const Q2Func *fn, const Q2Insn *in, const uint8_t *w, int *a, int *b) {
  switch (in->rop) {
    case ROP_add_imm_s:
    case ROP_sub_imm_s:
    case ROP_add_imm_self_s:
    case ROP_sub_imm_self_s:
    case ROP_add_imm_self_branch:
    case ROP_add_imm_self_branch_s:
      *a = w[in->use[0]];
      *b = LB_I32;
      return 1;
    case ROP_binary_imm:
    case ROP_binary_imm_s:
      *a = w[in->use[0]];
      *b = LB_I32;
      return 1;
    default:
      if (in->nuse >= 2) {
        *a = w[in->use[0]];
        *b = w[in->use[1]];
        return 1;
      }
      if (in->nuse == 1) {
        *a = w[in->use[0]];
        *b = LB_I32;
        return 1;
      }
      *a = *b = LB_BOT;
      return 0;
  }
}

/* The record is int-only when the profiler saw ints and nothing else. */
static int q2_record_int_only(const Q2Value *v) {
  return v->has_prof && v->ptypes == Q2_T_INT32;
}

static int q2_is_set(int lb) {
  return lb == LB_I32 || lb == LB_F64;
}

int q2_infer(Q2Func *fn) {
  uint8_t *w;
  uint8_t *fixed;
  int i, b, pass;
  int changed;

  if (!fn || fn->nvalues <= 0) return 0;
  w = calloc((size_t)fn->nvalues, 1);
  fixed = calloc((size_t)fn->nvalues, 1);
  if (!w || !fixed) {
    free(w);
    free(fixed);
    return -1;
  }

  /* ---- seeds: constants, speculated loads, unknown-role redefinitions ---- */
  for (i = 0; i < fn->ninsns; i++) {
    const Q2Insn *in = &fn->insns[i];
    int k;
    if (in->op == Q2_OP_CONST) {
      for (k = 0; k < in->ndef; k++) {
        Q2ValueId v = in->def[k];
        switch (fn->values[v].con_kind) {
          case Q2_CON_INT:
            w[v] = LB_I32;
            break;
          case Q2_CON_DOUBLE:
            w[v] = LB_F64;
            break;
          case Q2_CON_TRUE:
          case Q2_CON_FALSE:
            w[v] = LB_BOOL;
            break;
          default:
            w[v] = LB_TAG;
            break;
        }
        fixed[v] = 1;
      }
      continue;
    }
    if (q2_def_from_record(in) && !q2_inc_by_operand(fn, in)) {
      /* A load, or a fallback with declared destinations (a call). Its
         destinations speculate from their record, else stay tagged. */
      for (k = 0; k < in->ndef; k++) {
        Q2ValueId v = in->def[k];
        /* A chain's first link is the receiver of its second read. The
           profiler records only the second link at this pc, so the record
           says nothing about the first. */
        if (k == 0 && in->ndef == 2 &&
            (in->rop == ROP_get_field_chain ||
             in->rop == ROP_get_field_chain_own))
          w[v] = LB_TAG;
        else
          w[v] = q2_load_lb(&fn->values[v]);
        fixed[v] = 1;
      }
      continue;
    }
    /* An unknown-role fallback redefines whole live sets: those stay tagged. */
    for (k = 0; k < in->nredef; k++) {
      w[in->redef_val[k]] = LB_TAG;
      fixed[in->redef_val[k]] = 1;
    }
  }

  /* ---- transfer, to a fixpoint over the blocks ---- */
  for (b = 0; b < fn->blocks[0].nparams; b++) {
    w[fn->blocks[0].param_val[b]] = LB_TAG;
    fixed[fn->blocks[0].param_val[b]] = 1;
  }
  /* A loop counter: a block parameter whose only incoming argument is the
     result of the in-place increment, which itself is int when the parameter
     is. The cycle has no least fixpoint, so it is broken here, by the same
     rule the lowering applies (an int that overflows leaves compiled code). */
  for (b = 1; b < fn->nblocks; b++) {
    const Q2Block *blk = &fn->blocks[b];
    int k;
    for (k = 0; k < blk->nparams; k++) {
      Q2ValueId pv = blk->param_val[k];
      int e, only_inc = 1, seen = 0;
      for (e = 0; e < blk->npreds && only_inc; e++) {
        const Q2Block *pr = &fn->blocks[blk->preds[e]];
        int n, m;
        for (n = 0; n < pr->nsucc && only_inc; n++) {
          if (pr->succ_block[n] != b || k >= pr->nedge_args[n]) continue;
          for (m = 0; m < pr->nedge_args[n]; m++) {
            Q2ValueId av;
            const Q2Insn *ai;
            if (m != k) continue;
            av = pr->edge_arg[n][m];
            if (av >= (Q2ValueId)fn->nvalues) continue;
            seen = 1;
            if (fn->values[av].insn < 0) {
              if (av != pv) only_inc = 0;
              continue;
            }
            ai = &fn->insns[fn->values[av].insn];
            if (!((ai->op == Q2_OP_BRANCH_CMP &&
                   (ai->rop == ROP_inc_branch_lt ||
                    ai->rop == ROP_inc_branch_lt_s)) ||
                  (ai->op == Q2_OP_BRANCH &&
                   (ai->rop == ROP_add_imm_self_branch ||
                    ai->rop == ROP_add_imm_self_branch_s))))
              only_inc = 0;
          }
        }
      }
      if (seen && only_inc && !fixed[pv]) w[pv] = LB_I32;
    }
  }
  for (pass = 0; pass < fn->nblocks + 8; pass++) {
    changed = 0;
    for (b = 0; b < fn->nblocks; b++) {
      const Q2Block *blk = &fn->blocks[b];
      int k;
      /* Parameters: the join of their incoming edge arguments. */
      for (k = 0; k < blk->nparams; k++) {
        Q2ValueId pv = blk->param_val[k];
        int lb = w[pv];
        int e;
        if (fixed[pv]) continue;
        for (e = 0; e < blk->npreds; e++) {
          int pb = blk->preds[e];
          const Q2Block *pr = &fn->blocks[pb];
          int n, m;
          for (n = 0; n < pr->nsucc; n++) {
            if (pr->succ_block[n] != b) continue;
            for (m = 0; m < pr->nedge_args[n]; m++)
              if (m == k) lb = lb_join(lb, w[pr->edge_arg[n][m]]);
          }
        }
        if (lb != w[pv]) {
          w[pv] = (uint8_t)lb;
          changed = 1;
        }
      }
      for (k = 0; k < blk->ninsns; k++) {
        int insn = blk->first_insn + k;
        const Q2Insn *in = &fn->insns[insn];
        int d;
        for (d = 0; d < in->ndef; d++)
          if (fixed[in->def[d]]) break;
        if (d < in->ndef) continue; /* a seeded destination is fixed */
        for (d = 0; d < in->ndef; d++) {
          Q2ValueId v = in->def[d];
          int lb = LB_TAG, old = w[v];
          if (in->op == Q2_OP_MOVE && in->nuse >= 1) {
            lb = w[in->use[0]];
          } else if (q2_inc_by_operand(fn, in)) {
            lb = w[in->use[0]] == LB_I32 || w[in->use[0]] == LB_F64
                     ? w[in->use[0]]
                     : LB_TAG;
          } else if (in->op == Q2_OP_BINARY) {
            int a, bb;
            if (in->rop == ROP_mul_add || in->rop == ROP_mul_sub ||
                in->rop == ROP_add_mul || in->rop == ROP_sub_mul) {
              /* The two halves share one record and must get the same
                 representation: a typed first half with a tagged second would
                 need a box this step does not emit. Both are int only when the
                 record is int-only and every operand is an int, else both are
                 f64; anything tagged makes both tagged. */
              int p = q2_is_set(w[in->use[0]]) && q2_is_set(w[in->use[1]])
                          ? lb_join(w[in->use[0]], w[in->use[1]])
                          : LB_TAG;
              int q = q2_is_set(w[in->use[2]]) ? w[in->use[2]] : LB_TAG;
              const Q2Value *rec = &fn->values[in->def[0]];
              if (p == LB_TAG || q == LB_TAG) {
                lb = LB_TAG;
              } else if (
                  q2_record_int_only(rec) && w[in->use[0]] == LB_I32 &&
                  w[in->use[1]] == LB_I32 && q == LB_I32) {
                lb = LB_I32;
              } else {
                lb = LB_F64;
              }
              goto def_done;
            }
            if (!q2_binary_operands(fn, in, w, &a, &bb)) {
              lb = LB_TAG;
            } else if (
                in->sub == REG_BIN_ADD || in->sub == REG_BIN_SUB ||
                in->sub == REG_BIN_MUL) {
              if (a == LB_I32 && bb == LB_I32) {
                /* Keep the int only when the record is int-only: a float
                   result would need a guard this step does not emit, so a
                   possibly-float add stays in a double. */
                lb = q2_record_int_only(&fn->values[v]) ? LB_I32 : LB_F64;
              } else if (q2_is_set(a) && q2_is_set(bb)) {
                lb = LB_F64;
              } else {
                lb = LB_TAG;
              }
            } else if (in->sub == REG_BIN_DIV) {
              lb = q2_is_set(a) && q2_is_set(bb) ? LB_F64 : LB_TAG;
            } else if (
                in->sub == REG_BIN_AND || in->sub == REG_BIN_OR ||
                in->sub == REG_BIN_XOR || in->sub == REG_BIN_SHL ||
                in->sub == REG_BIN_SAR) {
              lb = (a == LB_I32 && bb == LB_I32) ? LB_I32 : LB_TAG;
            } else {
              lb = LB_TAG; /* MOD, POW, SHR and the rest stay tagged */
            }
            /* A constant-pool right operand is typed only when the pool entry
               is a number, which the lowering then folds to a literal: the
               result is a double, boxed with the interpreter's tag where it
               escapes. Anything else keeps the helper's tagged form. */
            if (in->rop == ROP_binary_cpool) {
              double c;
              int ua = in->nuse >= 1 ? w[in->use[0]] : LB_TAG;
              int sb = (int)in->arg[1].val;
              lb = LB_TAG;
              if (in->arg[3].val == REG_CONST_CPOOL &&
                  qjs_aot_pool_num(fn->g, (uint32_t)in->arg[4].val, &c) &&
                  (ua == LB_I32 || ua == LB_F64) &&
                  (sb == REG_BIN_ADD || sb == REG_BIN_SUB ||
                   sb == REG_BIN_MUL || sb == REG_BIN_DIV))
                lb = LB_F64;
            }
          } else if (in->op == Q2_OP_BRANCH_CMP && in->ndef >= 1 &&
                     (in->rop == ROP_inc_branch_lt ||
                      in->rop == ROP_inc_branch_lt_s)) {
            /* `d = d + 1` in place: an int stays an int. An increment past
               INT32_MAX is a genuine misprediction, and the lowering guards
               it. `use[0]` is the value register `d` held before the
               instruction. */
            lb = (in->nuse >= 1 && w[in->use[0]] == LB_I32) ? LB_I32 : LB_TAG;
          } else if (in->op == Q2_OP_BRANCH_CMP && in->ndef >= 1 &&
                     (in->rop == ROP_add_imm_self_branch ||
                      in->rop == ROP_add_imm_self_branch_s)) {
            /* the same in-place add, with an immediate */
            lb = (in->nuse >= 1 && w[in->use[0]] == LB_I32) ? LB_I32 : LB_TAG;
          } else if (in->op == Q2_OP_COMPARE) {
            int a, bb;
            if (q2_binary_operands(fn, in, w, &a, &bb) && q2_is_set(a) &&
                q2_is_set(bb))
              lb = LB_BOOL;
            else
              lb = LB_TAG;
          } else {
            lb = LB_TAG; /* any other op */
          }
        def_done:
          if (w[v] != (uint8_t)lb) {
            w[v] = (uint8_t)lb;
            changed = 1;
          }
        }
      }
    }
    if (!changed) break;
  }

  /* ---- finalise ---- */
  for (i = 0; i < fn->nvalues; i++) {
    Q2Value *v = &fn->values[i];
    int lb = w[i] ? w[i] : LB_TAG;
    v->rep = lb_rep(lb);
    v->spec = 0;
    v->ovf = 0;
    /* R1: nothing is in its frame slot until a write-back puts it there. A
       tagged value is no exception: it lives in its C local, and the
       write-back before a safepoint, a leave or a return stores it. */
    v->synced = 0;
    /* A tagged value is written through: its slot is current as soon as it is
       defined. A typed one is not, and is boxed at a safepoint or a leave. */
    if (v->rep == Q2_R_TAGGED) { v->synced = 1; continue; }
    /* A load's record also decides whether the value keeps the JSValue it
       produced, so a tagged consumer never re-boxes it. */
    if (v->insn >= 0) {
      const Q2Insn *in = &fn->insns[v->insn];
      if (q2_def_from_record(in) && !q2_inc_by_operand(fn, in)) v->spec = 1;
      else if (in->op == Q2_OP_CONST || in->op == Q2_OP_MOVE) v->synced = 1;
      else if (
          in->op == Q2_OP_BINARY &&
          (in->sub == REG_BIN_ADD || in->sub == REG_BIN_SUB ||
           in->sub == REG_BIN_MUL) &&
          v->rep == Q2_R_I32)
        v->ovf = 1;
    }
  }

  free(w);
  free(fixed);
  return 0;
}
