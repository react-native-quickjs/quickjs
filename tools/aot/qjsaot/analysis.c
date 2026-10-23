/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * Representation inference: which C type each SSA value of the speculative body
 * is held in, why a value that stays boxed stays boxed, and which guards an
 * exact integer result needs.
 *
 * The lattice is
 *
 *     BOT < {I32, BOOL};  I32 < F64;  everything < TAG
 *
 * BOT means "not computed yet" and nothing else. It is the identity of the join,
 * so a value computed late still comes down to I32 or F64 when its operands
 * turn out to be numbers. types.c's inference instead treats BOT as TAG during
 * the fixpoint (its `q2_is_set` is false for BOT), so a value's representation
 * can never improve after it is first visited: that pessimism is what left the
 * SVC's arithmetic boxed, and this pass does not reuse it.
 *
 * A value still at BOT when the fixpoint ends has no definition on any path the
 * pass reached and becomes TAG with UNREACHABLE as its reason.
 *
 * Seeds are the constants and the speculated definitions (a load, a call or
 * op-call result, `this`): their profile record decides, and a record that is
 * not exactly numeric stays boxed. Everything else follows the transfer table
 * from its operands and, where a result must stay exact, the record at the
 * definition. The table's "exits" column becomes exits[]: the guards an exact
 * integer result needs (overflow, division by zero, -0, INT32_MIN), so the
 * emitter does not re-derive them from the operation and cannot disagree with
 * the analysis about them.
 *
 * Three profile-lookup rules, each a bug an earlier lookup had that this pass avoids:
 *
 *   1. a call's result is its own call-result record (the `K` line), keyed by
 *      the pc after the call; the `P` record at that pc is the next
 *      instruction's, so it is never read for a call;
 *   2. `inc`/`dec`/`post_inc`/`post_dec` have no record of their own, so their
 *      representation follows their operand;
 *   3. a `get_field_chain`'s first link has no record, only its second has one.
 *
 * Every boxed value records why. The reasons form a graph (OPERAND_TAGGED and
 * PHI_TAGGED name another value), and the dump follows it back to roots, so one
 * bad seed shows up as one line instead of a hundred.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emit.h"
#include "analysis.h"
#include "quickjs-regvm.h"

/* Whether constant-pool entry `idx` is a number (qjsaot.c). */
int qjs_aot_pool_num(const struct JSAotGenFunc *g, uint32_t idx, double *out);

/* ---- the lattice -------------------------------------------------------- */

static int lb_join(int a, int b) {
  if (a == E_LB_BOT) return b;
  if (b == E_LB_BOT) return a;
  if (a == b) return a;
  if (a == E_LB_BOOL || b == E_LB_BOOL) return E_LB_TAG;
  if (a == E_LB_I32 && b == E_LB_F64) return E_LB_F64;
  if (a == E_LB_F64 && b == E_LB_I32) return E_LB_F64;
  return E_LB_TAG;
}

/* Whether a representation is a number held in a C number type. BOT is *not*
   one: an uncomputed value is unknown, not boxed. */
static int lb_num(int lb) {
  return lb == E_LB_I32 || lb == E_LB_F64;
}

/* ---- the profile's record for a definition ------------------------------ */

/* A record implies a representation only when it is exactly numeric (or exactly
   bool). A record holding a string, an object or undefined means the
   speculative body's guard would fail too often to pay for itself. */
static int lb_from_q2_types(uint32_t t, int has) {
  if (!has) return E_LB_TAG;
  if (t == Q2_T_INT32) return E_LB_I32;
  if (t == Q2_T_NUM) return E_LB_F64;
  if (t == (Q2_T_INT32 | Q2_T_NUM)) return E_LB_F64;
  if (t == Q2_T_BOOL) return E_LB_BOOL;
  return E_LB_TAG;
}

/* The profile's `P` record at `pc`, or 0 when the function has none there. */
static int prof_at(const Q2Func *fn, int pc, unsigned *types) {
  int lo = 0, hi = fn->nprof - 1;
  if (fn->prof == NULL) return 0;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    if (fn->prof[mid].pc == pc) {
      *types = fn->prof[mid].types;
      return 1;
    }
    if (fn->prof[mid].pc < pc)
      lo = mid + 1;
    else
      hi = mid - 1;
  }
  return 0;
}

/* The call-result (`K`) record of the call that ends at `pc`: what the site's
   calls returned, believed only when every call that entered it returned a
   recorded value. A site with no record stays untyped. */
static int cres_at(const Q2Func *fn, int pc, unsigned *types) {
  int lo = 0, hi = fn->ncres - 1;
  if (fn->cres == NULL) return 0;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    if (fn->cres[mid].pc == pc) {
      *types = fn->cres[mid].types;
      return 1;
    }
    if (fn->cres[mid].pc < pc)
      lo = mid + 1;
    else
      hi = mid - 1;
  }
  return 0;
}

/* The profile's type set mapped to the IR's kinds, as build.c maps it. */
static uint32_t prof_to_q2(unsigned types) {
  uint32_t t = 0;
  if (types & PT_INT) t |= Q2_T_INT32;
  if (types & PT_FLOAT) t |= Q2_T_NUM;
  if (types & PT_OBJECT) t |= Q2_T_OBJ;
  if (types & PT_STRING) t |= Q2_T_STR;
  if (types & PT_BOOL) t |= Q2_T_BOOL;
  if (types & PT_UNDEF) t |= Q2_T_UNDEF;
  if (types & PT_BIGINT) t |= Q2_T_BIGINT;
  if (types & PT_OTHER) t |= Q2_T_ANY;
  if (t == 0) t = Q2_T_ANY;
  return t;
}

/* A definition whose value comes from a JSValue the instruction read (a load, a
   call result, an op-call result, `this`) speculates from its record: it gets an
   unboxed C local and a guard at its definition. Any other instruction's
   representation follows its operands. */
static int def_from_record(const Q2Insn *in) {
  switch (in->op) {
    case Q2_OP_GET_FIELD:
    case Q2_OP_GET_ELEMENT:
    case Q2_OP_GET_VAR_REF:
    case Q2_OP_GET_ARG:
    case Q2_OP_GET_ARG2:
    case Q2_OP_GET_LOCAL:
    case Q2_OP_GET_GLOBAL:
    case Q2_OP_CALL:
    case Q2_OP_LOAD_THIS:
      return 1;
    case Q2_OP_FALLBACK:
      /* A fallback with declared destinations produces its own results (an op
         call); one with none rewrites a whole live set, which is not a
         speculation about one value. */
      return in->ndef > 0;
    case Q2_OP_UNARY:
      /* `neg` carries its result's type in the record as well. */
      return in->rop == ROP_neg;
    default:
      return 0;
  }
}

/* Rule 2. An increment or decrement has no record of its own (the profiler files
   none), so its representation follows its operand. */
static int inc_by_operand(const Q2Insn *in) {
  switch (in->rop) {
    case ROP_inc:
    case ROP_dec:
    case ROP_post_inc:
    case ROP_post_dec:
      return 1;
    default:
      return 0;
  }
}

/* Rule 3. A property chain's first link has no record; the record at the
   instruction describes only the second link. */
static int chain_first_link(const Q2Insn *in, int k) {
  return k == 0 && in->ndef == 2 &&
         (in->rop == ROP_get_field_chain || in->rop == ROP_get_field_chain_own);
}

/* ---- the transfer ------------------------------------------------------- */

/* I32 when both operands are i32 and the record is int-only (so no result is a
   float), else F64 when both are numbers, else BOT when an operand is not
   computed yet, else TAG. This is the shape of add, sub, mul, div, mod and the
   unsigned shift. */
static int rep_exact(int a, int b, int rec_int_only) {
  if (a == E_LB_BOT || b == E_LB_BOT) return E_LB_BOT;
  if (a == E_LB_I32 && b == E_LB_I32)
    return rec_int_only ? E_LB_I32 : E_LB_F64;
  if (lb_num(a) && lb_num(b)) return E_LB_F64;
  return E_LB_TAG;
}

/* The bitwise operations and the two arithmetic shifts: ToInt32 of a number is
   total, so two numbers give an exact i32 and no record is needed. */
static int rep_bit(int a, int b) {
  if (a == E_LB_BOT || b == E_LB_BOT) return E_LB_BOT;
  return (lb_num(a) && lb_num(b)) ? E_LB_I32 : E_LB_TAG;
}

/* A compare: BOOL when both sides are numbers, and for strict equality also when
   both are bools (the tags are the whole test). */
static int rep_cmp(int sub, int a, int b) {
  if (a == E_LB_BOT || b == E_LB_BOT) return E_LB_BOT;
  if (lb_num(a) && lb_num(b)) return E_LB_BOOL;
  if ((sub == REG_BIN_STRICT_EQ || sub == REG_BIN_STRICT_NEQ) &&
      a == E_LB_BOOL && b == E_LB_BOOL)
    return E_LB_BOOL;
  return E_LB_TAG;
}

/* An increment keeps the operand's representation: an int stays an int (the
   lowering guards the overflow) and a double stays a double. A bool becomes an
   exact int, since `true++` is 1 and 2 does not fit a bool. */
static int rep_inc(int a) {
  if (a == E_LB_BOOL) return E_LB_I32;
  return a;
}

/* An increment-like result follows its operand, except where the profile saw the result
   leave the int range at this instruction: that case failed in the profiled run, so the
   result is a double rather than an int whose overflow exit would be taken again. */
static int rep_inc_rec(int a, const Q2Value *dv) {
  if (a == E_LB_I32 && dv != NULL && dv->has_prof && dv->ptypes != Q2_T_INT32 &&
      (dv->ptypes & ~(Q2_T_INT32 | Q2_T_NUM)) == 0)
    return E_LB_F64;
  return rep_inc(a);
}

/* The exits an exact integer result needs. A result that came out of an op call
   (a fallback, a fused second half) is already exact and needs none. */
static uint8_t exits_for(int rop, int sub, int from_opcall) {
  if (from_opcall) return 0;
  switch (rop) {
    case ROP_add:
    case ROP_sub:
    case ROP_add_imm_s:
    case ROP_sub_imm_s:
    case ROP_add_imm_self_s:
    case ROP_sub_imm_self_s:
    case ROP_add_imm_self_branch:
    case ROP_add_imm_self_branch_s:
    case ROP_inc:
    case ROP_dec:
      return E_EX_OVF;
    case ROP_mul:
      /* overflow, and -0: a zero product with a negative operand is -0 in JS,
         which an int32 cannot hold. */
      return E_EX_OVF | E_EX_NEGZERO;
    case ROP_div:
      return E_EX_ZERODIV | E_EX_NOTEXACT | E_EX_INTMIN | E_EX_NEGZERO;
    case ROP_mod:
      return E_EX_ZERODIV | E_EX_INTMIN | E_EX_SIGNEDMIN;
    case ROP_shr:
      return E_EX_SHRBIG;
    case ROP_neg:
    case ROP_post_inc:
    case ROP_post_dec:
      return E_EX_OVF;
    default:
      break;
  }
  if (rop == ROP_binary || rop == ROP_binary_imm || rop == ROP_binary_imm_s ||
      rop == ROP_binary_cpool) {
    switch (sub & ~REG_CMP_INVERT) {
      case REG_BIN_ADD:
      case REG_BIN_SUB:
        return E_EX_OVF;
      case REG_BIN_MUL:
        return E_EX_OVF | E_EX_NEGZERO;
      case REG_BIN_DIV:
        return E_EX_ZERODIV | E_EX_NOTEXACT | E_EX_INTMIN | E_EX_NEGZERO;
      case REG_BIN_MOD:
        return E_EX_ZERODIV | E_EX_INTMIN | E_EX_SIGNEDMIN;
      case REG_BIN_SHR:
        return E_EX_SHRBIG;
      default:
        return 0;
    }
  }
  if (rop == ROP_unary && (sub & ~REG_CMP_INVERT) == REG_UN_NEG)
    return E_EX_OVF | E_EX_NEGZERO | E_EX_SIGNEDMIN;
  return 0;
}

/* A binary's operand representations, with an immediate operand counting as an
   i32. Returns 0 when the instruction has no operand pair. */
static int binary_operands(const Q2Insn *in, const uint8_t *w, int *a, int *b) {
  switch (in->rop) {
    case ROP_add_imm_s:
    case ROP_sub_imm_s:
    case ROP_binary_imm:
    case ROP_binary_imm_s:
      *a = in->nuse >= 1 ? w[in->use[0]] : E_LB_BOT;
      *b = E_LB_I32;
      return 1;
    case ROP_binary_cpool:
      *a = in->nuse >= 1 ? w[in->use[0]] : E_LB_BOT;
      *b = E_LB_BOT; /* the pool entry decides, in the transfer */
      return 1;
    default:
      break;
  }
  if (in->nuse >= 2) {
    *a = w[in->use[0]];
    *b = w[in->use[1]];
    return 1;
  }
  if (in->nuse == 1) {
    *a = w[in->use[0]];
    *b = E_LB_I32;
    return 1;
  }
  return 0;
}

/* The record is int-only when the profiler saw ints and nothing else there. */
static int record_int_only(const Q2Value *v) {
  return v->has_prof && v->ptypes == Q2_T_INT32;
}

/* The representation of destination `d` of `in`, from its operands. Returns BOT
   when the operands are not computed yet (the result cannot be decided before
   they are) and TAG when no rule applies. `whyp`/`whyvp` name the operand or
   operation that forced TAG. */
static int transfer_def(
    const Q2Func *fn, const EFacts *facts, const Q2Insn *in, int insn, int d,
    const uint8_t *w, int *whyp, Q2ValueId *whyvp, int *from_opcall) {
  const Q2Value *dv = &fn->values[in->def[d]];
  int sub = in->sub & ~REG_CMP_INVERT;
  int a, b;

  *from_opcall = 0;
  switch (in->op) {
    case Q2_OP_MOVE:
      return in->nuse >= 1 ? w[in->use[0]] : E_LB_TAG;
    case Q2_OP_CONST:
      switch (dv->con_kind) {
        case Q2_CON_INT:
          return E_LB_I32;
        case Q2_CON_DOUBLE:
          return E_LB_F64;
        case Q2_CON_TRUE:
        case Q2_CON_FALSE:
          return E_LB_BOOL;
        default:
          return E_LB_TAG;
      }
    case Q2_OP_BINARY:
      if (in->rop == ROP_mul_add || in->rop == ROP_mul_sub ||
          in->rop == ROP_add_mul || in->rop == ROP_sub_mul) {
        /* The two halves share one record and must get one representation: a
           typed first half with a boxed second would need a box this analysis
           does not plan for. The first half is a multiply of use[0] and use[1];
           the second is an add or subtract of the first half and use[2]. */
        int p, q, rec_int;
        if (in->nuse < 3 || in->ndef < 2) return E_LB_TAG;
        p = rep_exact(
            w[in->use[0]], w[in->use[1]],
            record_int_only(&fn->values[in->def[0]]));
        rec_int = record_int_only(&fn->values[in->def[0]]);
        q = d == 0 ? p : rep_exact(w[in->def[0]], w[in->use[2]], rec_int);
        if (d == 0) return p;
        return q;
      }
      if (!binary_operands(in, w, &a, &b)) return E_LB_TAG;
      if (in->rop == ROP_binary_cpool) {
        /* "rbrbc": d, sub, a, kind, index. A constant-pool right operand folds
           to a C literal only when the pool entry is a number, and a number
           with anything is a double, so the result is F64. The kind and the
           index are the last two operands, not the two before the source: an
           earlier version read them one slot to the left and rejected every
           one of these. */
        double c;
        int ki = in->narg > 3 ? 3 : -1, ui = in->narg > 4 ? 4 : -1;
        if (ki < 0 || in->arg[ki].val != REG_CONST_CPOOL || ui < 0 ||
            !qjs_aot_pool_num(fn->g, (uint32_t)in->arg[ui].val, &c)) {
          *whyp = E_WHY_UNKNOWN_RESULT;
          return E_LB_TAG;
        }
        if (a == E_LB_BOT) return E_LB_BOT;
        if (!lb_num(a)) {
          *whyp = E_WHY_OPERAND_TAGGED;
          *whyvp = in->use[0];
          return E_LB_TAG;
        }
        return E_LB_F64;
      }
      switch (sub) {
        case REG_BIN_ADD:
        case REG_BIN_SUB:
        case REG_BIN_MUL:
          return rep_exact(a, b, record_int_only(dv));
        case REG_BIN_DIV:
        case REG_BIN_MOD:
          return rep_exact(a, b, record_int_only(dv));
        case REG_BIN_AND:
        case REG_BIN_OR:
        case REG_BIN_XOR:
        case REG_BIN_SHL:
        case REG_BIN_SAR:
          return rep_bit(a, b);
        case REG_BIN_SHR:
          return rep_exact(a, b, record_int_only(dv));
        default:
          *whyp = E_WHY_UNKNOWN_RESULT;
          return E_LB_TAG;
      }
    case Q2_OP_TO_PROPKEY:
      /* An integer is already a property key; the object operand of the two-operand
         form is only tested for null and undefined. */
      if (in->nuse < 1) return E_LB_TAG;
      a = w[in->use[0]];
      if (a == E_LB_BOT) return E_LB_BOT;
      if (a == E_LB_I32) return E_LB_I32;
      *whyp = E_WHY_OPERAND_TAGGED;
      *whyvp = in->use[0];
      return E_LB_TAG;
    case Q2_OP_COMPARE:
      if (!binary_operands(in, w, &a, &b)) return E_LB_TAG;
      return rep_cmp(sub, a, b);
    case Q2_OP_UNARY:
      if (in->nuse < 1) return E_LB_TAG;
      a = w[in->use[0]];
      /* `add_imm_self_s` / `sub_imm_self_s` ("ri") arrive here with their OWN rop, so
         the sub-based checks below never see them. They are an in-place add/sub of a
         small constant: the inc rule applies (an int stays an int, the lowering's
         overflow exit covers the rest). */
      if (in->rop == ROP_add_imm_self_s || in->rop == ROP_sub_imm_self_s) {
        if (a == E_LB_BOT) return E_LB_BOT;
        if (a == E_LB_I32) return rep_inc_rec(a, dv);
        if (a == E_LB_F64) return E_LB_F64;
        *whyp = E_WHY_OPERAND_TAGGED;
        *whyvp = in->use[0];
        return E_LB_TAG;
      }
      if (sub == REG_UN_NEG) {
        if (a == E_LB_BOT) return E_LB_BOT;
        if (a == E_LB_I32) return record_int_only(dv) ? E_LB_I32 : E_LB_F64;
        if (a == E_LB_F64) return E_LB_F64;
        *whyp = E_WHY_OPERAND_TAGGED;
        *whyvp = in->use[0];
        return E_LB_TAG;
      }
      if (sub == REG_UN_INC || sub == REG_UN_DEC) return rep_inc_rec(a, dv);
      if (sub == REG_UN_NOT || sub == REG_UN_LNOT) {
        if (a == E_LB_BOT) return E_LB_BOT;
        if (a == E_LB_BOOL || lb_num(a)) return E_LB_BOOL;
        *whyp = E_WHY_OPERAND_TAGGED;
        *whyvp = in->use[0];
        return E_LB_TAG;
      }
      *whyp = E_WHY_UNKNOWN_RESULT;
      return E_LB_TAG;
    case Q2_OP_POST_INC:
      /* "rrr": t, s, h -- h is s after the increment, t is the value s had
         before it. Both are the operand's representation, so both follow
         `rep_inc`; the integer case keeps i32 because the lowering guards the
         overflow. */
      if (in->nuse < 1) return E_LB_TAG;
      return rep_inc_rec(w[in->use[0]], dv);
    case Q2_OP_BRANCH_CMP:
      /* `inc_branch_lt`: `d = d + 1` in place, then compare. An int stays an
         int (the lowering guards the overflow). */
      if (in->rop == ROP_inc_branch_lt || in->rop == ROP_inc_branch_lt_s)
        return in->nuse >= 1 ? rep_inc(w[in->use[0]]) : E_LB_TAG;
      return E_LB_TAG;
    case Q2_OP_BRANCH:
      /* `add_imm_self_branch`: the same in-place add with an immediate. */
      if (in->rop == ROP_add_imm_self_branch ||
          in->rop == ROP_add_imm_self_branch_s) {
        if (in->nuse < 1) return E_LB_TAG;
        a = w[in->use[0]];
        if (a == E_LB_BOT) return E_LB_BOT;
        /* An in-place add of a small constant IS an increment, so it takes the
           inc rule: an int stays an int and the lowering guards the overflow.
           Widening to f64 because there is no profile record was wrong -- a loop
           counter's record is for its own def, and the fused add's def has none,
           so `i += 3` was typed f64 while `i++` was typed i32. */
        if (a == E_LB_I32) return rep_inc(a);
        if (a == E_LB_F64) return E_LB_F64;
        *whyp = E_WHY_OPERAND_TAGGED;
        *whyvp = in->use[0];
        return E_LB_TAG;
      }
      return E_LB_TAG;
    case Q2_OP_FALLBACK: {
      /* An op call. Its result is exact (the interpreter computed it), so an
         integer result needs no exit of its own; the tag guard at the
         definition is the whole check. */
      *from_opcall = 1;
      if (d < in->ndef) return E_LB_TAG; /* replaced by the seed below */
      return E_LB_TAG;
    }
    default:
      break;
  }
  (void)insn;
  (void)facts;
  *whyp = E_WHY_UNKNOWN_RESULT;
  return E_LB_TAG;
}

/* ---- arithmetic classification, for the counters and the gate ------------ */

static int is_cmp_sub(int sub) {
  switch (sub) {
    case REG_BIN_LT:
    case REG_BIN_LTE:
    case REG_BIN_GT:
    case REG_BIN_GTE:
    case REG_BIN_EQ:
    case REG_BIN_NEQ:
    case REG_BIN_STRICT_EQ:
    case REG_BIN_STRICT_NEQ:
    case REG_BIN_INSTANCEOF:
    case REG_BIN_IN:
    case REG_BIN_PRIVATE_IN:
      return 1;
    default:
      return 0;
  }
}

/* Whether an instruction produces an arithmetic result. The gate counts those,
   and "typed" means every one of its destinations is held unboxed. */
static int arith_kind(const Q2Insn *in) {
  int sub = in->sub & ~REG_CMP_INVERT;
  switch (in->rop) {
    case ROP_mul_add:
    case ROP_mul_sub:
    case ROP_add_mul:
    case ROP_sub_mul:
      return E_AK_FUSED;
    case ROP_add:
    case ROP_sub:
    case ROP_mul:
    case ROP_div:
    case ROP_mod:
    case ROP_pow:
    case ROP_bitand:
    case ROP_bitor:
    case ROP_bitxor:
    case ROP_shl:
    case ROP_sar:
    case ROP_shr:
    case ROP_neg:
    case ROP_inc:
    case ROP_dec:
    case ROP_post_inc:
    case ROP_post_dec:
    case ROP_add_imm_s:
    case ROP_sub_imm_s:
    case ROP_add_imm_self_s:
    case ROP_sub_imm_self_s:
    case ROP_add_imm_self_branch:
    case ROP_add_imm_self_branch_s:
    case ROP_inc_branch_lt:
    case ROP_inc_branch_lt_s:
    case ROP_binary_imm:
    case ROP_binary_imm_s:
    case ROP_binary_cpool:
    case ROP_to_number:
    case ROP_to_numeric:
      return E_AK_ARITH;
    case ROP_binary:
      return is_cmp_sub(sub) ? E_AK_CMP : E_AK_ARITH;
    case ROP_unary:
      /* Only negate and the two in-place increments produce a number. The
         typeof/null/undefined tests produce a bool or a string and must not be
         counted as arithmetic results: the gate's "no UNKNOWN_RESULT root on an
         arithmetic op" would otherwise fail on `x === undefined`'s own test. */
      if (sub == REG_UN_NEG || sub == REG_UN_INC || sub == REG_UN_DEC)
        return E_AK_ARITH;
      return E_AK_CMP;
    case ROP_compare:
    case ROP_lt:
    case ROP_lte:
    case ROP_gt:
    case ROP_gte:
    case ROP_eq:
    case ROP_neq:
    case ROP_strict_eq:
    case ROP_strict_neq:
    case ROP_instanceof:
    case ROP_in:
    case ROP_not:
      return E_AK_CMP;
    default:
      return E_AK_NONE;
  }
}

/* ---- the fixpoint ------------------------------------------------------- */

/* Whether a block parameter is a loop counter, and so gets the optimistic I32
   the fixpoint cannot derive.

   A counter's header has two kinds of incoming edge: the initial value from
   outside the loop, and the back edge carrying the increment. The join is
   `pv = join(init, inc(pv))`, and `inc` is monotone but has no least fixpoint
   above BOT: once `pv` is TAG the increment is TAG too, and it never comes back
   down. Starting from BOT, a header whose initial value is boxed therefore ends
   TAG even though the register holds an int on every trip after the first.

   The rule is therefore: at least one incoming edge is an increment of the
   parameter itself (the loop, not just a block parameter that happens to be
   assigned), and no incoming argument is already known to be boxed. "Known" is
   the seed pass's answer, which is why this runs right after it. The lowering
   guards the overflow and exits before the instruction if the initial value is
   not a number, so the optimism is checked at run time. */
static int loop_counter_param(
    const Q2Func *fn, const uint8_t *w, int b, int k) {
  const Q2Block *blk = &fn->blocks[b];
  Q2ValueId pv = blk->param_val[k];
  int p, saw_self_inc = 0;

  for (p = 0; p < blk->npreds; p++) {
    const Q2Block *pr;
    int n, m;
    /* A dead predecessor's edge argument never reaches this block, so it must
       not widen a type or disqualify the cycle. */
    if (blk->preds[p] < 0 || fn->rpo_pos[blk->preds[p]] < 0) continue;
    pr = &fn->blocks[blk->preds[p]];
    for (n = 0; n < pr->nsucc; n++) {
      if (pr->succ_block[n] != b) continue;
      for (m = 0; m < pr->nedge_args[n]; m++) {
        Q2ValueId av;
        const Q2Insn *ai;
        if (m != k) continue;
        av = pr->edge_arg[n][m];
        if (av >= (Q2ValueId)fn->nvalues) continue;
        if (av == pv) {
          /* The block passes its own parameter through: not an increment, but
             it does not disqualify the cycle. */
          continue;
        }
        if (fn->values[av].insn < 0) {
          if (w[av] == E_LB_TAG) return 0;
          continue;
        }
        /* An incoming argument that is already known to be boxed means the
           register really does hold a JSValue on that path. */
        if (w[av] == E_LB_TAG) return 0;
        ai = &fn->insns[fn->values[av].insn];
        if (ai->rop == ROP_inc_branch_lt || ai->rop == ROP_inc_branch_lt_s ||
            ai->rop == ROP_add_imm_self_branch ||
            ai->rop == ROP_add_imm_self_branch_s || ai->rop == ROP_inc ||
            ai->rop == ROP_add_imm_self_s || ai->rop == ROP_sub_imm_self_s) {
          if (ai->nuse >= 1 && ai->use[0] == pv) saw_self_inc = 1;
        }
      }
    }
  }
  return saw_self_inc;
}

/* Why value `v` ends up boxed, from the final state. The reasons form a graph:
   OPERAND_TAGGED and PHI_TAGGED name the value that forced the result, so the
   dump can follow a chain back to the one seed that started it instead of
   printing a hundred lines that all say "an operand was boxed".

   This is a separate walk rather than something the fixpoint records because the
   fixpoint sees intermediate lattice values; a reason must describe the final
   answer. */
static int why_tagged(
    const Q2Func *fn, const EFacts *facts, const ETypes *t, Q2ValueId v,
    Q2ValueId *src) {
  const Q2Value *val = &fn->values[v];

  *src = Q2_NO_VALUE;
  if (val->insn < 0) {
    /* A block parameter. The entry block's parameters are the caller's
       arguments, which arrive boxed; any other parameter joins its incoming
       arguments. */
    int b, k;
    for (b = 0; b < fn->nblocks; b++) {
      const Q2Block *blk = &fn->blocks[b];
      for (k = 0; k < blk->nparams; k++) {
        Q2ValueId pv = blk->param_val[k];
        int p, n, m;
        if (pv != v) continue;
        if (b == 0) return E_WHY_INCOMING;
        for (p = 0; p < blk->npreds; p++) {
          const Q2Block *pr = &fn->blocks[blk->preds[p]];
          for (n = 0; n < pr->nsucc; n++) {
            if (pr->succ_block[n] != b) continue;
            for (m = 0; m < pr->nedge_args[n]; m++) {
              Q2ValueId av;
              if (m != k) continue;
              av = pr->edge_arg[n][m];
              if (av >= (Q2ValueId)fn->nvalues) continue;
              if (t->lb[av] == E_LB_TAG) {
                *src = av;
                return E_WHY_PHI_TAGGED;
              }
            }
          }
        }
        return E_WHY_UNKNOWN_RESULT;
      }
    }
    return E_WHY_UNREACHABLE;
  }
  {
    const Q2Insn *in = &fn->insns[val->insn];
    int d, from_opcall = 0;
    /* A constant that is not a number has no unboxed form to be held in. */
    if (in->op == Q2_OP_CONST) return E_WHY_CONST_NONNUM;
    /* An op call whose destinations the role table does not declare rewrote the
       whole live set: the value is boxed because the interpreter stored it, not
       because of any operand. */
    for (d = 0; d < in->nredef; d++) {
      if (in->redef_val[d] == v) return E_WHY_OPCALL_REDEF;
    }
    /* A forwarded instruction is a boxed copy of a boxed value. */
    if (facts != NULL && facts->fwd[val->insn] != Q2_NO_VALUE) {
      *src = facts->fwd[val->insn];
      return E_WHY_OPERAND_TAGGED;
    }
    /* A speculated definition whose record was not numeric: no profile, or a
       mixed record. */
    if (def_from_record(in) && !inc_by_operand(in)) {
      int k;
      for (k = 0; k < in->ndef; k++) {
        if (in->def[k] != v) continue;
        if (chain_first_link(in, k)) return E_WHY_NO_PROFILE;
        if (t->spec[v]) {
          unsigned types = 0;
          if (in->op == Q2_OP_CALL)
            return prof_at(fn, in->next, &types) ? E_WHY_MIXED
                                                 : E_WHY_NO_PROFILE;
          return val->has_prof ? E_WHY_MIXED : E_WHY_NO_PROFILE;
        }
        break;
      }
    }
    /* Otherwise the result follows its operands: name the first boxed one. */
    for (d = 0; d < in->ndef; d++) {
      int whyp = E_WHY_NONE;
      int lb;
      Q2ValueId whyv = Q2_NO_VALUE;
      if (in->def[d] != v) continue;
      lb = transfer_def(
          fn, facts, in, val->insn, d, t->lb, &whyp, &whyv, &from_opcall);
      if (whyp != E_WHY_NONE) {
        *src = whyv;
        return whyp;
      }
      /* No reason from the transfer: the result is boxed because one of its
         operands is. */
      if (lb == E_LB_TAG) {
        int k;
        for (k = 0; k < in->nuse; k++) {
          Q2ValueId u = in->use[k];
          if (u < (Q2ValueId)fn->nvalues && t->lb[u] == E_LB_TAG) {
            *src = u;
            return E_WHY_OPERAND_TAGGED;
          }
        }
        return E_WHY_UNKNOWN_RESULT;
      }
    }
  }
  return E_WHY_UNKNOWN_RESULT;
}

int e_types_run(const Q2Func *fn, const EFacts *facts, ETypes *out) {
  uint8_t *w = NULL;
  uint8_t *fixed = NULL;
  int nv, i, b, k, pass, changed;

  memset(out, 0, sizeof(*out));
  if (fn == NULL || fn->nvalues <= 0) return 0;
  nv = fn->nvalues;
  out->nvalues = nv;
  out->lb = calloc((size_t)nv, 1);
  out->spec = calloc((size_t)nv, 1);
  out->exits = calloc((size_t)nv, 1);
  out->why = calloc((size_t)nv, 1);
  out->whyv = malloc(sizeof(*out->whyv) * (size_t)nv);
  out->arith = calloc((size_t)nv, 1);
  out->loopcnt = calloc((size_t)nv, 1);
  out->live = calloc((size_t)nv, 1);
  w = calloc((size_t)nv, 1);
  fixed = calloc((size_t)nv, 1);
  if (out->lb == NULL || out->spec == NULL || out->exits == NULL ||
      out->why == NULL || out->whyv == NULL || out->arith == NULL ||
      w == NULL || fixed == NULL) {
    free(w);
    free(fixed);
    e_types_free(out);
    return -1;
  }
  for (i = 0; i < nv; i++) out->whyv[i] = Q2_NO_VALUE;

  /* Which values the emitter will actually see: those an instruction defines,
     and those a block keeps as a parameter. Trivial-parameter removal leaves
     value objects behind that nothing refers to; counting them would overstate
     how boxed the function is (and print roots nothing reads). */
  for (i = 0; i < fn->ninsns; i++) {
    const Q2Insn *in = &fn->insns[i];
    int d;
    for (d = 0; d < in->ndef; d++) out->live[in->def[d]] = 1;
  }
  for (b = 0; b < fn->nblocks; b++) {
    for (k = 0; k < fn->blocks[b].nparams; k++)
      out->live[fn->blocks[b].param_val[k]] = 1;
  }

  /* The function's incoming registers are boxed: the frame holds JSValues. */
  for (k = 0; k < fn->blocks[0].nparams; k++) {
    Q2ValueId v = fn->blocks[0].param_val[k];
    w[v] = E_LB_TAG;
    fixed[v] = 1;
  }

  /* ---- seeds: constants, and the speculated definitions ---- */
  for (i = 0; i < fn->ninsns; i++) {
    const Q2Insn *in = &fn->insns[i];
    int d;
    /* A forwarded instruction is a move of the value it copies, so its
       record says nothing about it: the transfer decides. */
    if (facts != NULL && facts->fwd[i] != Q2_NO_VALUE) continue;
    if (in->op == Q2_OP_CONST) {
      for (d = 0; d < in->ndef; d++) {
        Q2ValueId v = in->def[d];
        switch (fn->values[v].con_kind) {
          case Q2_CON_INT:
            w[v] = E_LB_I32;
            break;
          case Q2_CON_DOUBLE:
            w[v] = E_LB_F64;
            break;
          case Q2_CON_TRUE:
          case Q2_CON_FALSE:
            w[v] = E_LB_BOOL;
            break;
          default:
            w[v] = E_LB_TAG;
            break;
        }
        fixed[v] = 1;
        out->spec[v] = 0;
      }
      continue;
    }
    if (def_from_record(in) && !inc_by_operand(in)) {
      for (d = 0; d < in->ndef; d++) {
        Q2ValueId v = in->def[d];
        uint32_t t = 0;
        int has = 0;
        if (in->op == Q2_OP_CALL) {
          /* Rule 1: a call's result comes from its own call-result record,
             keyed by the pc just past the call. The P record at that pc is the
             NEXT instruction's own and says nothing about the call (box2d: a
             `const 0` after a call that returns undefined typed the call i32,
             and its guard failed on every entry). No record -- an old profile,
             a call that threw, a path the profiler does not see -- leaves the
             result boxed. */
          unsigned types = 0;
          has = cres_at(fn, in->next, &types);
          t = has ? prof_to_q2(types) : 0;
        } else if (chain_first_link(in, d)) {
          /* Rule 3: a chain's first link has no record. */
          has = 0;
          t = 0;
        } else {
          has = fn->values[v].has_prof;
          t = fn->values[v].ptypes;
        }
        w[v] = (uint8_t)lb_from_q2_types(t, has);
        fixed[v] = 1;
        out->spec[v] = 1;
      }
      continue;
    }
    /* An op-call fallback that rewrites a live set: the interpreter wrote those
       registers, so they are boxed until a reload says otherwise. */
    for (d = 0; d < in->nredef; d++) {
      w[in->redef_val[d]] = E_LB_TAG;
      fixed[in->redef_val[d]] = 1;
      out->why[in->redef_val[d]] = E_WHY_OPCALL_REDEF;
    }
  }

  /* A loop counter with no seed at all: the cycle has no least fixpoint above
     BOT, so it is broken here the way the lowering breaks it (an int that
     overflows leaves compiled code). */
  for (b = 1; b < fn->nblocks; b++) {
    for (k = 0; k < fn->blocks[b].nparams; k++) {
      Q2ValueId pv = fn->blocks[b].param_val[k];
      if (!fixed[pv] && loop_counter_param(fn, w, b, k)) {
        w[pv] = E_LB_I32;
        fixed[pv] = 1;
        out->loopcnt[pv] = 1;
      }
    }
  }

  /* ---- the join fixpoint over the blocks in reverse postorder ---- */
  for (pass = 0; pass < fn->nblocks + 8; pass++) {
    changed = 0;
    for (b = 0; b < fn->nblocks; b++) {
      const Q2Block *blk = &fn->blocks[b];
      /* Parameters: the join of their incoming edge arguments. */
      for (k = 0; k < blk->nparams; k++) {
        Q2ValueId pv = blk->param_val[k];
        int lb = w[pv], p;
        if (fixed[pv]) continue;
        for (p = 0; p < blk->npreds; p++) {
          const Q2Block *pr = &fn->blocks[blk->preds[p]];
          int n, m;
          for (n = 0; n < pr->nsucc; n++) {
            if (pr->succ_block[n] != b) continue;
            for (m = 0; m < pr->nedge_args[n]; m++) {
              Q2ValueId av;
              if (m != k) continue;
              av = pr->edge_arg[n][m];
              if (av >= (Q2ValueId)nv) continue;
              lb = lb_join(lb, w[av]);
            }
          }
        }
        if (lb != w[pv]) {
          w[pv] = (uint8_t)lb;
          changed = 1;
        }
      }
      /* Instructions: each destination from its operands. */
      for (k = 0; k < blk->ninsns; k++) {
        int insn = blk->first_insn + k;
        const Q2Insn *in = &fn->insns[insn];
        int d;
        if (in->ndef <= 0) continue;
        if (fixed[in->def[0]]) continue;
        for (d = 0; d < in->ndef; d++) {
          Q2ValueId v = in->def[d];
          int whyp = E_WHY_NONE, from_opcall = 0;
          Q2ValueId whyv = Q2_NO_VALUE;
          int lb;
          if (fixed[v]) continue;
          if (facts != NULL && facts->fwd[insn] != Q2_NO_VALUE) {
            Q2ValueId src = facts->fwd[insn];
            lb = src < (Q2ValueId)nv ? w[src] : E_LB_TAG;
          } else {
            lb = transfer_def(
                fn, facts, in, insn, d, w, &whyp, &whyv, &from_opcall);
          }
          if (lb != w[v]) {
            w[v] = (uint8_t)lb;
            changed = 1;
          }
        }
      }
    }
    if (!changed) break;
  }

  /* ---- the final walk: representations, reasons, exits, counters ---- */
  for (i = 0; i < nv; i++) {
    int lb = w[i];
    if (lb == E_LB_BOT) lb = E_LB_TAG;
    out->lb[i] = (uint8_t)lb;
  }
  for (i = 0; i < fn->ninsns; i++) {
    const Q2Insn *in = &fn->insns[i];
    int d;
    for (d = 0; d < in->ndef; d++) {
      Q2ValueId v = in->def[d];
      if (out->arith[v] == E_AK_NONE) out->arith[v] = (uint8_t)arith_kind(in);
    }
  }
  for (i = 0; i < fn->ninsns; i++) {
    const Q2Insn *in = &fn->insns[i];
    int d;
    /* The exits of an exact integer result are the operation's. A result that
       came out of an op call is already exact and needs none. */
    for (d = 0; d < in->ndef; d++) {
      Q2ValueId v = in->def[d];
      if (out->lb[v] != E_LB_I32) continue;
      out->exits[v] = exits_for(in->rop, in->sub, in->op == Q2_OP_FALLBACK);
      /* a fused multiply-add pair: the product has mul's exits, the sum add's */
      if ((in->rop == ROP_mul_add || in->rop == ROP_mul_sub || in->rop == ROP_add_mul ||
           in->rop == ROP_sub_mul) && in->op != Q2_OP_FALLBACK)
        out->exits[v] = d == 0 ? (E_EX_OVF | E_EX_NEGZERO) : E_EX_OVF;
    }
  }

  /* Why each value that stays boxed stays boxed. The chain is re-derived from
     the final state, so a reason cannot describe an intermediate step. */
  for (i = 0; i < nv; i++) {
    if (out->lb[i] != E_LB_TAG) continue;
    out->why[i] =
        (uint8_t)why_tagged(fn, facts, out, (Q2ValueId)i, &out->whyv[i]);
    if (out->why[i] == E_WHY_NONE) out->why[i] = E_WHY_UNREACHABLE;
  }

  /* Counters, over the live values only. */
  for (i = 0; i < nv; i++) {
    if (!out->live[i]) continue;
    out->nlive++;
    switch (out->lb[i]) {
      case E_LB_BOOL:
        out->ntyped[1]++;
        break;
      case E_LB_I32:
        out->ntyped[2]++;
        break;
      case E_LB_F64:
        out->ntyped[3]++;
        break;
      default:
        out->ntagged++;
        break;
    }
  }
  for (i = 0; i < fn->ninsns; i++) {
    const Q2Insn *in = &fn->insns[i];
    int d, all_typed = 1;
    if (in->ndef <= 0) continue;
    if (arith_kind(in) == E_AK_NONE) continue;
    if (in->op == Q2_OP_COMPARE || arith_kind(in) == E_AK_CMP) continue;
    out->arith_total++;
    for (d = 0; d < in->ndef; d++) {
      if (out->lb[in->def[d]] == E_LB_TAG) all_typed = 0;
    }
    if (all_typed) out->arith_typed++;
  }
  out->arith_tagged = out->arith_total - out->arith_typed;
  /* The same count over values: a fused op defines two results. */
  for (i = 0; i < fn->ninsns; i++) {
    const Q2Insn *in = &fn->insns[i];
    int d;
    for (d = 0; d < in->ndef; d++) {
      Q2ValueId v = in->def[d];
      if (out->arith[v] != E_AK_ARITH && out->arith[v] != E_AK_FUSED)
        continue;
      out->arithv_total++;
      if (out->lb[v] != E_LB_TAG) out->arithv_typed++;
    }
  }
  free(w);
  free(fixed);
  e_ranges_run(fn, out);
  return 0;
}

void e_types_free(ETypes *t) {
  if (t == NULL) return;
  free(t->lb);
  free(t->spec);
  free(t->exits);
  free(t->why);
  free(t->whyv);
  free(t->arith);
  free(t->loopcnt);
  free(t->live);
  memset(t, 0, sizeof(*t));
}

int e_types_typed(const ETypes *t, Q2ValueId v) {
  if (t == NULL || t->lb == NULL || v >= (Q2ValueId)t->nvalues) return 0;
  return t->lb[v] != E_LB_TAG;
}

const char *e_types_rep_name(const ETypes *t, Q2ValueId v) {
  if (t == NULL || t->lb == NULL || v >= (Q2ValueId)t->nvalues) return "tag";
  switch (t->lb[v]) {
    case E_LB_I32:
      return "i32";
    case E_LB_F64:
      return "f64";
    case E_LB_BOOL:
      return "bool";
    default:
      return "tag";
  }
}

/* ---- the dump (`--types`) ----------------------------------------- */

static const char *why_name(int why) {
  switch (why) {
    case E_WHY_NO_PROFILE:
      return "NO_PROFILE";
    case E_WHY_MIXED:
      return "MIXED";
    case E_WHY_OPERAND_TAGGED:
      return "OPERAND_TAGGED";
    case E_WHY_UNKNOWN_RESULT:
      return "UNKNOWN_RESULT";
    case E_WHY_PHI_TAGGED:
      return "PHI_TAGGED";
    case E_WHY_UNREACHABLE:
      return "UNREACHABLE";
    case E_WHY_INCOMING:
      return "INCOMING";
    case E_WHY_OPCALL_REDEF:
      return "OPCALL_REDEF";
    case E_WHY_CONST_NONNUM:
      return "CONST_NONNUM";
    default:
      return "?";
  }
}

/* The root of a value's reason chain: follow OPERAND_TAGGED/PHI_TAGGED links
   until one has a reason that does not name another value. */
static Q2ValueId why_root(
    const Q2Func *fn, const ETypes *t, Q2ValueId v, int *hops) {
  int guard = 0;
  *hops = 0;
  while (guard++ < 64) {
    int why = t->why[v];
    Q2ValueId next = t->whyv[v];
    if (next == Q2_NO_VALUE || next >= (Q2ValueId)fn->nvalues) break;
    if (why != E_WHY_OPERAND_TAGGED && why != E_WHY_PHI_TAGGED) break;
    v = next;
    (*hops)++;
  }
  return v;
}

/* The operation name of an instruction, for the dump's `def=pc:op`. */
static const char *insn_opname(const Q2Insn *in) {
  static char buf[48];
  switch (in->op) {
    case Q2_OP_FALLBACK:
      return "opcall";
    case Q2_OP_CONST:
      return "const";
    case Q2_OP_MOVE:
      return "move";
    case Q2_OP_GET_LOCAL:
      return "get_local";
    case Q2_OP_PUT_LOCAL:
      return "put_local";
    case Q2_OP_GET_ARG:
      return "get_arg";
    case Q2_OP_PUT_ARG:
      return "put_arg";
    case Q2_OP_GET_GLOBAL:
      return "get_global";
    case Q2_OP_BINARY:
      return "binary";
    case Q2_OP_COMPARE:
      return "compare";
    case Q2_OP_UNARY:
      return "unary";
    case Q2_OP_BRANCH:
      return "branch";
    case Q2_OP_BRANCH_CMP:
      return "branch_cmp";
    case Q2_OP_RETURN:
      return "return";
    case Q2_OP_RETURN_UNDEF:
      return "return_undef";
    case Q2_OP_GET_FIELD:
      return "get_field";
    case Q2_OP_GET_ELEMENT:
      return "get_element";
    case Q2_OP_GET_ARG2:
      return "get_arg2";
    case Q2_OP_GET_VAR_REF:
      return "get_var_ref";
    case Q2_OP_LOAD_THIS:
      return "load_this";
    case Q2_OP_PUT_FIELD:
      return "put_field";
    case Q2_OP_CALL:
      return "call";
    default:
      break;
  }
  e_rop_name(in->rop, buf, sizeof(buf));
  return buf;
}

/* The location a fact names, for the dump. */
static void key_str(const EKey *k, char *out, size_t n) {
  switch (k->kind) {
    case E_K_ARG:
      snprintf(out, n, "A%" PRId64, k->a);
      break;
    case E_K_LOCAL:
      snprintf(out, n, "L%" PRId64, k->a);
      break;
    case E_K_VARREF:
      snprintf(out, n, "V%" PRId64, k->a);
      break;
    case E_K_FIELD:
      if (k->a == E_RECV_THIS) {
        snprintf(out, n, "this.a%" PRId64, k->b);
      } else if (k->a <= E_RECV_LOCAL(0)) {
        snprintf(out, n, "L%" PRId64 ".a%" PRId64, -(k->a + 3), k->b);
      } else {
        snprintf(out, n, "v%" PRId64 ".a%" PRId64, k->a, k->b);
      }
      break;
    case E_K_ELEM:
      if (k->b == E_ELEM_LEN) {
        snprintf(out, n, "len(v%" PRId64 ")", k->a);
      } else {
        snprintf(out, n, "v%" PRId64 "[v%" PRId64 "]", k->a, k->b);
      }
      break;
    case E_K_GLOBAL:
      snprintf(out, n, "G%" PRId64, k->a);
      break;
    case E_K_THIS:
      snprintf(out, n, "this");
      break;
    default:
      snprintf(out, n, "?");
      break;
  }
}

/* The roots of the tagged set, by how many values each root is responsible for.
   Sorted descending so the report's "top roots" line is the first few. */
typedef struct RootCount {
  Q2ValueId v;
  int count;
  int why;
} RootCount;

static int rootcmp(const void *a, const void *b) {
  const RootCount *x = a, *y = b;
  if (x->count != y->count) return y->count - x->count;
  return (int)x->v - (int)y->v;
}

int e_analysis_dump(
    FILE *f, const Q2Func *fn, const char *hex, double weight) {
  EFacts facts;
  ETypes types;
  int b, k, i, rc = -1;
  RootCount *roots = NULL;
  int nroots = 0, caproots = 0;
  int nfwd = 0, nskip = 0, naccess = 0;

  if (e_analyze(fn, &facts, &types) < 0) return -1;

  {
    /* The header names the function the way the report does: `name
       file:line:col`, so a dump and a report line can be matched by eye. */
    char pos[320];
    if (fn->g->line > 0)
      snprintf(
          pos, sizeof(pos), "%s:%d:%d", fn->g->file, fn->g->line, fn->g->col);
    else
      snprintf(pos, sizeof(pos), "%s", fn->g->file[0] ? fn->g->file : "?");
    fprintf(
        f, "T %s %s ops=%d weight=%.1f%% values=%d\n",
        fn->g->name[0] ? fn->g->name : "<anon>", pos, fn->g->len / 4, weight,
        fn->nvalues);
  }
  fprintf(f, "T   digest=%s\n", hex);
  /* Values by representation. */
  fprintf(
      f, "T   values i32 %d f64 %d bool %d tag %d\n", types.ntyped[2],
      types.ntyped[3], types.ntyped[1], types.ntagged);
  /* Arithmetic results: the number the gate reads. */
  fprintf(
      f, "T   arith results %d typed %d tagged %d (values %d/%d)\n",
      types.arith_total, types.arith_typed, types.arith_tagged,
      types.arithv_typed, types.arithv_total);
  /* Groups. */
  fprintf(f, "T   groups %d sites %d\n", facts.ngroups, facts.nsites);
  for (i = 0; i < facts.ngroups; i++) {
    const EGroup *g = &facts.groups[i];
    fprintf(
        f, "T     G%d shape=%lld first=%d sites=", i, (long long)g->shape,
        g->first_site);
    for (k = 0; k < g->nsites; k++)
      fprintf(f, "%s%d", k ? "," : "", g->sites[k]);
    fputc('\n', f);
  }

  /* Facts per block, then one line per value: the block parameters first, then
     one per instruction's destination. A parameter has no defining instruction,
     so it is named by its block and register; the gate reads the loop counters
     there. */
  for (b = 0; b < fn->nblocks; b++) {
    const Q2Block *blk = &fn->blocks[b];
    const EBlockFacts *bf = &facts.blocks[b];
    fprintf(
        f, "T B%d pc=%d..%d ctl=%d\n", b, blk->start_pc, blk->end_pc,
        blk->ctl_in);
    if (bf->nshape > 0 || bf->navail > 0) {
      fprintf(f, "T   facts");
      for (k = 0; k < bf->nshape; k++)
        fprintf(f, " SHAPE(v%d,G%d)", bf->shape_val[k], bf->shape_grp[k]);
      for (k = 0; k < bf->navail; k++) {
        char key[64];
        key_str(&bf->avail[k].key, key, sizeof(key));
        fprintf(f, " AVAIL(%s)=v%u", key, bf->avail[k].val);
      }
      fputc('\n', f);
    }
    for (k = 0; k < blk->nparams; k++) {
      Q2ValueId v = blk->param_val[k];
      if (!types.live[v]) continue;
      fprintf(
          f, "T   v%u rep=%s def=param:B%d:r%d%s", v,
          e_types_rep_name(&types, v), b, fn->values[v].reg,
          types.loopcnt[v] ? " loop=1" : "");
      /* A parameter is boxed for one of two reasons, both of which the dump
         has to name: it arrives in a JSValue (the entry block), or an incoming
         edge carries a boxed value. */
      if (types.lb[v] == E_LB_TAG)
        fprintf(f, " why=%s", why_name(types.why[v]));
      fputc('\n', f);
    }
    for (k = 0; k < blk->ninsns; k++) {
      int insn = blk->first_insn + k;
      const Q2Insn *in = &fn->insns[insn];
      const char *opname = insn_opname(in);
      int ndef = in->ndef;
      int d;
      if (facts.access[insn]) naccess++;
      if (facts.nskip > 0 && facts.skip[insn]) nskip++;
      if (facts.fwd[insn] != Q2_NO_VALUE) nfwd++;
      /* The instruction's own line, then one line per value it defines. */
      if (ndef <= 0) continue;
      for (d = 0; d < ndef; d++) {
        Q2ValueId v = in->def[d];
        const char *rep = e_types_rep_name(&types, v);
        fprintf(f, "T   v%u rep=%s def=%d:%s", v, rep, in->pc, opname);
        /* Which profile record this destination's type came from, so a test can
           assert `src=P` for a reused register (E7d's N1) rather than infer it
           from the representation. */
        if (fn->values[v].has_prof)
          fprintf(f, " src=%s", fn->values[v].prof_src ? "R" : "P");
        /* The analysis's own classification: the gate reads this rather than
           guessing from the operation's name. */
        if (types.arith[v] == E_AK_ARITH || types.arith[v] == E_AK_FUSED)
          fprintf(f, " arith=1");
        else if (types.arith[v] == E_AK_CMP)
          fprintf(f, " arith=cmp");
        if (facts.fwd[insn] != Q2_NO_VALUE) {
          fprintf(f, " fwd=v%u", facts.fwd[insn]);
        } else if (facts.access[insn]) {
          fprintf(f, facts.skip[insn] ? " shape=skipped" : " shape=checked");
        }
        if (rep[0] == 't' && rep[1] == 'a') {
          Q2ValueId whyv = types.whyv[v];
          fprintf(f, " why=%s", why_name(types.why[v]));
          if (whyv != Q2_NO_VALUE && (types.why[v] == E_WHY_OPERAND_TAGGED ||
                                      types.why[v] == E_WHY_PHI_TAGGED))
            fprintf(f, "(v%u)", whyv);
        } else if (types.exits[v] != 0) {
          fprintf(f, " exits=");
          if (types.exits[v] & E_EX_OVF) fputc('o', f);
          if (types.exits[v] & E_EX_ZERODIV) fputc('z', f);
          if (types.exits[v] & E_EX_NOTEXACT) fputc('e', f);
          if (types.exits[v] & E_EX_INTMIN) fputc('i', f);
          if (types.exits[v] & E_EX_NEGZERO) fputc('n', f);
          if (types.exits[v] & E_EX_SHRBIG) fputc('s', f);
          if (types.exits[v] & E_EX_SIGNEDMIN) fputc('m', f);
        }
        fputc('\n', f);
      }
    }
  }

  /* Aggregate the tagged roots. */
  for (i = 0; i < fn->nvalues; i++) {
    Q2ValueId root;
    int hops;
    int m, found = -1;
    if (types.lb[i] != E_LB_TAG || !types.live[i]) continue;
    root = why_root(fn, &types, (Q2ValueId)i, &hops);
    for (m = 0; m < nroots; m++) {
      if (roots[m].v == root) {
        found = m;
        break;
      }
    }
    if (found < 0) {
      if (nroots == caproots) {
        int nc = caproots > 0 ? caproots * 2 : 16;
        RootCount *q = realloc(roots, sizeof(*q) * (size_t)nc);
        if (q == NULL) goto done;
        roots = q;
        caproots = nc;
      }
      found = nroots++;
      roots[found].v = root;
      roots[found].count = 0;
      roots[found].why = types.why[root];
    }
    roots[found].count++;
  }
  if (nroots > 0) qsort(roots, (size_t)nroots, sizeof(*roots), rootcmp);
  fprintf(f, "T   roots %d\n", nroots);
  for (i = 0; i < nroots && i < 12; i++) {
    Q2ValueId v = roots[i].v;
    const Q2Value *val = &fn->values[v];
    fprintf(
        f, "T     root %s v%u -> %d values", why_name(roots[i].why), v,
        roots[i].count);
    if (val->insn >= 0)
      fprintf(
          f, " (def pc %d %s)", fn->insns[val->insn].pc,
          insn_opname(&fn->insns[val->insn]));
    else
      fprintf(f, " (block parameter r%d)", val->reg);
    fputc('\n', f);
  }
  /* The counters the gate reads, on one line each so a script can grep them. */
  fprintf(
      f, "T   facts-loads forwarded %d shape-checks skipped %d of %d\n", nfwd,
      nskip, naccess);
  /* `arith` counts instructions, `arithv` the SSA results they define (a fused
     op defines two), so a reader can match either the design's "arithmetic
     results" or its op counts. */
  fprintf(
      f,
      "T   summary i32=%d f64=%d bool=%d tag=%d live=%d arith=%d/%d "
      "arithv=%d/%d "
      "fwd=%d skip=%d\n",
      types.ntyped[2], types.ntyped[3], types.ntyped[1], types.ntagged,
      types.nlive, types.arith_typed, types.arith_total, types.arithv_typed,
      types.arithv_total, nfwd, nskip);
  rc = 0;
done:
  free(roots);
  e_types_free(&types);
  e_facts_free(&facts);
  return rc;
}
