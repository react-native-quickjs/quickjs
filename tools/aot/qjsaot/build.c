/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * Builds qjsaot's IR from one function's register program.
 *
 * The program is the encoded bytecode the engine runs, decoded with the
 * engine's own decoder. The graph is a CFG of blocks with parameters:
 *   - a block starts at pc 0, at every branch target, and at the instruction
 *     after one that can leave the block;
 *   - each register write is one SSA definition; a block's parameters are the
 *     registers live on entry, one value each;
 *   - an instruction the classifier does not know is Q2_OP_FALLBACK: the
 *     lowering hands it to the step engine, and the IR models it as
 *     redefining every register live after it, which is conservative and
 *     correct;
 *   - the control stack depth (ctl) is propagated over the graph the way
 *     regvm_ctl_delta_rop derives it in the engine.
 *
 * Everything here is deliberately conservative: the IR is allowed to know
 * less than the engine, never more.
 */
#include <stdlib.h>
#include <string.h>

#include "ir.h"
#include "emit.h"
#include "quickjs-aot-gen.h"
#include "quickjs-regvm.h"

static void q2_build_rpo(Q2Func *f);

#define Q2_NONE8 ((int8_t)-1)

/* The driver's profile lookup (qjsaot.c): the sorted `P` records of a function
   digest, or NULL. */
const ProfPc *qjs_aot_prof_pcs(const uint8_t dg[16], int *n);
const ProfPc *qjs_aot_prof_cres(const uint8_t dg[16], int *n);
const Q2ProfReg *qjs_aot_prof_regs(const uint8_t dg[16], int *n);

/* The profile's type set mapped to the IR's. Anything that is not a number
   maps to TAGGED, which is what the inference needs to see. */
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

static int q2_ri(int off) {
  return off >> 4;
}

/* The REG_BIN_* sub-operation of a binary, compare or unary, or -1 when the
   opcode does not carry one. The immediate-heavy ops fold the sub-op into the
   opcode, so the table lists those explicitly. */
static int q2_rop_sub(uint16_t rop, const Q2Insn *in) {
  switch (rop) {
    case ROP_binary:
    case ROP_binary_imm:
    case ROP_binary_imm_s:
    case ROP_compare:
    case ROP_unary:
      /* operand 1 is the 'b' sub-op byte */
      if (in->narg > 1 && in->arg[1].kind == 'b') return (int)in->arg[1].val;
      return -1;
    case ROP_branch_cmp_reg_reg:
    case ROP_branch_cmp_reg_reg_s:
      if (in->narg > 0 && in->arg[0].kind == 'b') return (int)in->arg[0].val;
      return -1;
    case ROP_neg:
      return REG_UN_NEG;
    case ROP_inc:
      return REG_UN_INC;
    case ROP_add:
      return REG_BIN_ADD;
    case ROP_sub:
      return REG_BIN_SUB;
    case ROP_mul:
      return REG_BIN_MUL;
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
    case ROP_add_imm_s:
    case ROP_add_imm_self_s:
    case ROP_add_imm_self_branch:
    case ROP_add_imm_self_branch_s:
      return REG_BIN_ADD;
    case ROP_sub_imm_s:
    case ROP_sub_imm_self_s:
      return REG_BIN_SUB;
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
    case ROP_inc_branch_lt:
    case ROP_inc_branch_lt_s:
      return REG_BIN_LT;
    default:
      return -1;
  }
}

/* The second half of a fused operation, else -1. */
static int q2_rop_sub2(uint16_t rop) {
  switch (rop) {
    case ROP_mul_add:
    case ROP_add_mul:
      return REG_BIN_ADD;
    case ROP_mul_sub:
    case ROP_sub_mul:
      return REG_BIN_SUB;
    default:
      return -1;
  }
}

/* ---- names --------------------------------------------------------------- */
const char *q2_rep_name(int rep) {
  switch (rep) {
    case Q2_R_TAGGED:
      return "tagged";
    case Q2_R_I32:
      return "i32";
    case Q2_R_F64:
      return "f64";
    case Q2_R_BOOL:
      return "bool";
    case Q2_R_OBJ:
      return "obj";
    case Q2_R_STR:
      return "str";
    default:
      return "?";
  }
}

void q2_type_str(uint32_t t, char *out, size_t n) {
  static const struct {
    uint32_t bit;
    const char *name;
  } names[] = {
      {Q2_T_UNDEF, "undef"},   {Q2_T_NULL, "null"}, {Q2_T_BOOL, "bool"},
      {Q2_T_INT32, "int32"},   {Q2_T_NUM, "num"},   {Q2_T_STR, "str"},
      {Q2_T_OBJ, "obj"},       {Q2_T_FUNC, "func"}, {Q2_T_SYMBOL, "sym"},
      {Q2_T_BIGINT, "bigint"}, {Q2_T_HOLE, "hole"},
  };
  size_t used = 0;
  int i;
  out[0] = 0;
  if (t == Q2_T_ANY) {
    snprintf(out, n, "any");
    return;
  }
  for (i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++) {
    int len;
    if (!(t & names[i].bit)) continue;
    len = snprintf(
        out + used, used < n ? n - used : 0, "%s%s", used ? "|" : "",
        names[i].name);
    if (len < 0) break;
    used += (size_t)len;
    if (used >= n) break;
  }
  if (!used) snprintf(out, n, "none");
}

const char *q2_op_name(int op) {
  switch (op) {
    case Q2_OP_FALLBACK:
      return "fallback";
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
      return "return_undefined";
    case Q2_OP_GET_FIELD:
      return "get_field";
    case Q2_OP_PUT_FIELD:
      return "put_field";
    case Q2_OP_CALL:
      return "call";
    case Q2_OP_POST_INC:
      return "post_inc";
    case Q2_OP_PUT_VAR_REF:
      return "put_var_ref";
    case Q2_OP_TO_PROPKEY:
      return "to_propkey";
    case Q2_OP_SET_NAME:
      return "set_name";
    default:
      return "?";
  }
}

/* ---- classification ------------------------------------------------------ */
/* Which ROPs the IR understands. Everything else is Q2_OP_FALLBACK with a
   read and write set of Heap|Control (a call into the engine's step routine:
   it may do anything). Roles: operand indices, `def` before `use`. */
const Q2RopInfo *q2_rop_info(uint16_t rop) {
  static Q2RopInfo info;

  info.ndef = info.nuse = 0;
  info.window = -1;
  info.reads = Q2_E_HEAP;
  info.writes = Q2_E_HEAP | Q2_E_CONTROL;
  info.op = Q2_OP_FALLBACK;

  switch (rop) {
    /* constants and moves */
    case ROP_load_i8:
    case ROP_load_i8_s:
    case ROP_load_i32:
    case ROP_load_undefined:
    case ROP_load_null:
    case ROP_load_true:
    case ROP_load_false:
      info.op = Q2_OP_CONST;
      info.ndef = 1;
      info.def[0] = 0;
      info.reads = 0;
      info.writes = 0;
      return &info;
    case ROP_move:
    case ROP_move_s:
    case ROP_move_xfer:
      info.op = Q2_OP_MOVE;
      info.ndef = 1;
      info.def[0] = 0;
      info.nuse = 1;
      info.use[0] = 1;
      info.reads = 0;
      info.writes = 0;
      return &info;

    /* bindings */
    case ROP_get_local:
      info.op = Q2_OP_GET_LOCAL;
      info.ndef = 1;
      info.def[0] = 0;
      info.reads = 0;
      info.writes = 0;
      return &info;
    case ROP_put_local:
      info.op = Q2_OP_PUT_LOCAL;
      info.nuse = 1;
      info.use[0] = 1; /* the value; operand 0 is the slot */
      info.reads = 0;
      info.writes = Q2_E_HEAP; /* the slot is reachable from inner functions */
      info.writes |= Q2_E_CONTROL; /* the TDZ check can throw */
      return &info;
    case ROP_put_local_xfer:
      info.op = Q2_OP_PUT_LOCAL;
      info.nuse = 1;
      info.use[0] = 1;
      info.reads = 0;
      info.writes = Q2_E_HEAP | Q2_E_CONTROL;
      return &info;
    case ROP_get_arg2: /* "ruru": d1, slot1, d2, slot2 */
      info.op = Q2_OP_GET_ARG2;
      info.ndef = 2;
      info.def[0] = 0;
      info.def[1] = 2;
      info.reads = 0;
      info.writes = 0;
      return &info;
    case ROP_get_var_ref: /* "ru": d, slot */
      info.op = Q2_OP_GET_VAR_REF;
      info.ndef = 1;
      info.def[0] = 0;
      info.reads = Q2_E_VARREF;
      info.writes = 0;
      return &info;
    case ROP_load_this: /* "r" */
      info.op = Q2_OP_LOAD_THIS;
      info.ndef = 1;
      info.def[0] = 0;
      info.reads = 0;
      info.writes = 0;
      return &info;
    case ROP_get_arg:
      info.op = Q2_OP_GET_ARG;
      info.ndef = 1;
      info.def[0] = 0;
      info.reads = 0;
      info.writes = 0;
      return &info;
    case ROP_put_arg:
      info.op = Q2_OP_PUT_ARG;
      info.nuse = 1;
      info.use[0] = 1;
      info.reads = 0;
      info.writes = Q2_E_HEAP;
      return &info;
    case ROP_put_arg_xfer:
      info.op = Q2_OP_PUT_ARG;
      info.nuse = 1;
      info.use[0] = 1;
      info.reads = 0;
      info.writes = Q2_E_HEAP;
      return &info;
    case ROP_get_global:
      info.op = Q2_OP_GET_GLOBAL;
      info.ndef = 1;
      info.def[0] = 0;
      info.reads = Q2_E_GLOBAL;
      info.writes = Q2_E_CONTROL;
      return &info;

    /* arithmetic, logic and comparison */
    /* "rbrr": d, sub, a, b; "rbr": d, sub, s; "rbrI": d, sub, a, imm */
    case ROP_binary_cpool: /* "rbrbc": d, sub, a, kind, idx (a const) */
      info.op = Q2_OP_BINARY;
      info.ndef = 1;
      info.def[0] = 0;
      info.nuse = 1;
      info.use[0] = 2;
      info.reads = 0;
      info.writes = Q2_E_CONTROL;
      return &info;
    case ROP_add: /* "rrr": d, a, b */
    case ROP_sub:
    case ROP_mul:
    /* The bitwise and shift operations have the same "rrr" layout and the same
       lowering: q2_num_fast handles the int/int case (and the double case for
       the comparisons), and qjs_aot_num_fast the rest. Lowering them keeps a
       `(a ^ b) | 0` chain compiled instead of an op call per operator. */
    case ROP_bitand:
    case ROP_bitor:
    case ROP_bitxor:
    case ROP_shl:
    case ROP_sar:
    case ROP_shr:
      info.op = Q2_OP_BINARY;
      info.ndef = 1;
      info.def[0] = 0;
      info.nuse = 2;
      info.use[0] = 1;
      info.use[1] = 2;
      info.reads = 0;
      info.writes = Q2_E_CONTROL;
      return &info;
    case ROP_binary: /* "rbrr": d, sub, a, b */
      info.op = Q2_OP_BINARY;
      info.ndef = 1;
      info.def[0] = 0;
      info.nuse = 2;
      info.use[0] = 2;
      info.use[1] = 3;
      info.reads = 0;
      info.writes = Q2_E_CONTROL;
      return &info;
    /* "rbrI": d, sub, a, imm; the short immediates are "rbri"/"rri", with the
       source at operand 1 for the plain forms and operand 2 for "rbrI" */
    case ROP_binary_imm:
    case ROP_binary_imm_s:
      info.op = Q2_OP_BINARY;
      info.ndef = 1;
      info.def[0] = 0;
      info.nuse = 1;
      info.use[0] = (rop == ROP_binary_imm) ? 2 : 2;
      info.reads = 0;
      info.writes = Q2_E_CONTROL;
      return &info;
    case ROP_add_imm_s:
    case ROP_sub_imm_s:
      info.op = Q2_OP_BINARY;
      info.ndef = 1;
      info.def[0] = 0;
      info.nuse = 1;
      info.use[0] = 1; /* "rri": d, src, imm */
      info.reads = 0;
      info.writes = Q2_E_CONTROL;
      return &info;
    /* "ri": one register, both source and destination -- `regs[d] = regs[d] +
       imm`. A read-modify-write, so the same operand is one def and one use.
       The projection counted 48 of these boxed in navierstokes. */
    case ROP_add_imm_self_s:
    case ROP_sub_imm_self_s:
      info.op = Q2_OP_UNARY;
      info.ndef = 1;
      info.def[0] = 0;
      info.nuse = 1;
      info.use[0] = 0;
      info.reads = 0;
      info.writes = Q2_E_CONTROL;
      return &info;
    case ROP_compare:
      info.op = Q2_OP_COMPARE;
      info.ndef = 1;
      info.def[0] = 0;
      info.nuse = 2;
      info.use[0] = 2;
      info.use[1] = 3;
      info.reads = 0;
      info.writes = Q2_E_CONTROL;
      return &info;
    case ROP_unary:
      info.op = Q2_OP_UNARY;
      info.ndef = 1;
      info.def[0] = 0;
      info.nuse = 1;
      info.use[0] = 2;
      info.reads = 0;
      info.writes = Q2_E_CONTROL;
      return &info;

    case ROP_mul_add: /* "rrrrr": dm, x, y, da, c */
    case ROP_mul_sub:
    case ROP_add_mul:
    case ROP_sub_mul:
      info.op = Q2_OP_BINARY;
      info.ndef = 2;
      info.def[0] = 0;
      info.def[1] = 3;
      info.nuse = 3;
      info.use[0] = 1;
      info.use[1] = 2;
      info.use[2] = 4;
      info.reads = 0;
      info.writes = Q2_E_CONTROL;
      return &info;
    case ROP_branch_null:
    case ROP_branch_not_null: /* "rt": reg, target */
      info.op = Q2_OP_BRANCH;
      info.nuse = 1;
      info.use[0] = 0;
      info.reads = 0;
      info.writes = Q2_E_CONTROL;
      return &info;
    case ROP_branch_nullish:
    case ROP_branch_not_nullish:
      info.op = Q2_OP_BRANCH;
      info.nuse = 1;
      info.use[0] = 0;
      info.reads = 0;
      info.writes = Q2_E_CONTROL;
      return &info;

    /* control flow */
    case ROP_branch:
    case ROP_branch_s:
      info.op = Q2_OP_BRANCH;
      info.reads = 0;
      info.writes = 0;
      return &info;
    case ROP_branch_if:
    case ROP_branch_if_not:
      info.op = Q2_OP_BRANCH;
      info.nuse = 1;
      info.use[0] = 0;
      info.reads = 0;
      info.writes = Q2_E_CONTROL;
      return &info;
    case ROP_branch_cmp_reg_reg:
    case ROP_branch_cmp_reg_reg_s:
      info.op = Q2_OP_BRANCH_CMP;
      info.nuse = 2;
      info.use[0] = 1;
      info.use[1] = 2;
      info.reads = 0;
      info.writes = Q2_E_CONTROL;
      return &info;
    case ROP_return:
      info.op = Q2_OP_RETURN;
      info.nuse = 1;
      info.use[0] = 0;
      info.reads = 0;
      info.writes = Q2_E_CONTROL;
      return &info;
    case ROP_return_undefined:
      info.op = Q2_OP_RETURN_UNDEF;
      info.reads = 0;
      info.writes = Q2_E_CONTROL;
      return &info;

    /* branch_cmp_loc_reg / _reg_loc family are already covered; the short
       compare branches are "rrt": two registers, the compare in the opcode. */
    /* roles for the rest of the profile's fallbacks */
    case ROP_local_uninit: /* "u": slot (no register operand) */
    case ROP_local_check:  /* "u" */
    case ROP_local_check_init:
    case ROP_var_ref_check:
    case ROP_var_ref_check_init:
      info.op = Q2_OP_FALLBACK;
      info.ndef = 0;
      info.nuse = 0;
      info.reads = Q2_E_HEAP;
      info.writes = Q2_E_CONTROL;
      return &info;
    case ROP_set_home_object: /* "rr": this, home (both read) */
      info.op = Q2_OP_FALLBACK;
      info.nuse = 2;
      info.use[0] = 0;
      info.use[1] = 1;
      info.reads = 0;
      info.writes = Q2_E_HEAP;
      return &info;
    case ROP_branch_nlt_s: /* "rrt": a, b, target */
    case ROP_branch_ngt_s:
    case ROP_branch_nlte_s:
    case ROP_branch_ngte_s:
    case ROP_branch_eq_s:
    case ROP_branch_neq_s:
    case ROP_branch_lt_s:
    case ROP_branch_lte_s:
    case ROP_branch_gt_s:
    case ROP_branch_gte_s:
      info.op = Q2_OP_BRANCH;
      info.nuse = 2;
      info.use[0] = 0;
      info.use[1] = 1;
      info.reads = 0;
      info.writes = Q2_E_CONTROL;
      return &info;
    case ROP_mod:
    case ROP_pow:
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
      info.op = Q2_OP_FALLBACK;
      info.ndef = 1;
      info.def[0] = 0;
      info.nuse = 2;
      info.use[0] = 1;
      info.use[1] = 2;
      info.reads = 0;
      info.writes = Q2_E_CONTROL;
      return &info;
    case ROP_put_element_xfer: /* "rrr": object, key, value */
      info.op = Q2_OP_FALLBACK;
      info.nuse = 3;
      info.use[0] = 0;
      info.use[1] = 1;
      info.use[2] = 2;
      info.reads = Q2_E_ELEMENTS;
      info.writes = Q2_E_ELEMENTS | Q2_E_SLOT | Q2_E_HEAP | Q2_E_CONTROL;
      return &info;
    case ROP_catch_enter: /* "rt": catches; the target is a handler pc */
    case ROP_finally_enter:
      info.op = Q2_OP_FALLBACK;
      info.ndef = 0;
      info.nuse = 0;
      info.reads = 0;
      info.writes = Q2_E_CONTROL;
      return &info;
    case ROP_inc_branch_lt: /* "rrt": d, bound, target; d is read and updated */
    case ROP_inc_branch_lt_s:
      info.op = Q2_OP_BRANCH_CMP;
      info.ndef = 1;
      info.def[0] = 0;
      info.nuse = 2; /* the register incremented in place, and the bound */
      info.use[0] = 0;
      info.use[1] = 1;
      info.reads = 0;
      info.writes = Q2_E_CONTROL;
      return &info;
    case ROP_add_imm_self_branch: /* "rit": d, imm, target; d is updated */
    case ROP_add_imm_self_branch_s:
      info.op = Q2_OP_BRANCH;
      info.ndef = 1;
      info.def[0] = 0;
      info.nuse = 1;
      info.use[0] = 0;
      info.reads = 0;
      info.writes = Q2_E_CONTROL;
      return &info;
    /* The compare-and-branch forms that read a local slot: a branch that defines
       nothing and writes no register (the slot is memory), so the step wrapper and
       the typed forms can take it with the registers left as they are. */
    case ROP_branch_cmp_loc_loc: /* "buut": sub, slot, slot, target */
      info.op = Q2_OP_BRANCH;
      info.reads = Q2_E_SLOT;
      info.writes = Q2_E_CONTROL;
      return &info;
    case ROP_branch_cmp_loc_reg: /* "burt": sub, slot, register, target */
      info.op = Q2_OP_BRANCH;
      info.nuse = 1;
      info.use[0] = 2;
      info.reads = Q2_E_SLOT;
      info.writes = Q2_E_CONTROL;
      return &info;
    case ROP_branch_cmp_reg_loc: /* "brut": sub, register, slot, target */
      info.op = Q2_OP_BRANCH;
      info.nuse = 1;
      info.use[0] = 1;
      info.reads = Q2_E_SLOT;
      info.writes = Q2_E_CONTROL;
      return &info;
    case ROP_local_rmw_branch: /* "buIt": sub, slot, imm, target: updates the slot */
      info.op = Q2_OP_BRANCH;
      info.reads = Q2_E_SLOT;
      info.writes = Q2_E_SLOT | Q2_E_CONTROL;
      return &info;
    case ROP_local_rmw_imm: /* "buI": bool, slot, imm: no register operand */
      info.op = Q2_OP_FALLBACK;
      info.ndef = 0;
      info.nuse = 0;
      info.reads = Q2_E_HEAP;
      info.writes = Q2_E_HEAP | Q2_E_CONTROL;
      return &info;

    /* Roles for the fallbacks that dominate the profile: known operands, so
       the step redefines only its own destinations instead of the whole live
       set. The lowering still runs them through the step. */
    case ROP_make_closure: /* "rc": d, cpool */
      info.op = Q2_OP_FALLBACK;
      info.ndef = 1;
      info.def[0] = 0;
      info.reads = Q2_E_HEAP;
      info.writes = Q2_E_HEAP | Q2_E_CONTROL;
      return &info;
    case ROP_load_const: /* "rbc": d, kind, cpool */
      info.op = Q2_OP_FALLBACK;
      info.ndef = 1;
      info.def[0] = 0;
      info.reads = 0;
      info.writes = Q2_E_CONTROL;
      return &info;
    case ROP_get_local_field: /* "ruas": d, slot, atom, site */
      info.ndef = 1;
      info.def[0] = 0;
      info.reads = Q2_E_SLOT | Q2_E_SHAPE;
      info.writes = Q2_E_SHAPE | Q2_E_CONTROL;
      return &info;
    case ROP_get_local_field_0: /* "ras": d, atom, site */
      info.ndef = 1;
      info.def[0] = 0;
      info.reads = Q2_E_SLOT | Q2_E_SHAPE;
      info.writes = Q2_E_SHAPE | Q2_E_CONTROL;
      return &info;
    case ROP_get_this_field: /* "rras": d, recv, atom, site */
    case ROP_get_this_field2:
      /* The handler stores `this` into `recv` and then reads the field from
         it, so both operands are destinations and neither is read. */
      info.op = Q2_OP_GET_FIELD;
      info.ndef = 2;
      info.def[0] = 0;
      info.def[1] = 1;
      info.nuse = 0;
      info.reads = Q2_E_SLOT | Q2_E_SHAPE;
      info.writes = Q2_E_SHAPE | Q2_E_CONTROL;
      return &info;
    case ROP_get_local_s0: /* "r": d */
    case ROP_get_local_s1:
    case ROP_get_local_s2:
    case ROP_get_local_s3:
      info.ndef = 1;
      info.def[0] = 0;
      info.reads = 0;
      info.writes = 0;
      return &info;
    case ROP_put_local_s0: /* "r": value */
    case ROP_put_local_s1:
    case ROP_put_local_s2:
    case ROP_put_local_s3:
      info.nuse = 1;
      info.use[0] = 0;
      info.reads = 0;
      info.writes = Q2_E_HEAP;
      return &info;
    case ROP_construct: /* "rrrrb": d, callee, newtarget, window, argc */
      info.ndef = 1;
      info.def[0] = 0;
      info.nuse = 2;
      info.use[0] = 1;
      info.use[1] = 2;
      info.window = 4;
      info.reads = 0;
      info.writes = Q2_E_HEAP | Q2_E_SLOT | Q2_E_ELEMENTS | Q2_E_SHAPE |
                    Q2_E_GLOBAL | Q2_E_VARREF | Q2_E_CONTROL;
      return &info;
    case ROP_apply_arguments: /* "rru": window, d, arguments slot */
      /* The window is three registers from operand 0 (the callee, `apply` and
         the receiver); the decoder sets it, since it has no count operand. */
      info.ndef = 1;
      info.def[0] = 1;
      info.reads = 0;
      info.writes = Q2_E_HEAP | Q2_E_CONTROL;
      return &info;
    case ROP_div: /* "rrr": d, a, b */
    case ROP_neg: /* "rr": d, a */
    case ROP_inc: /* "rr" */
    /* `dec`, `not` and `to_numeric` belong to this same family. They were
       absent, so they fell back to a roles-free instruction that redefined
       every live register -- which boxed the arithmetic after them. `dec` alone
       accounted for 58 boxed values in crypto and `post_inc` for 53. */
    case ROP_dec:
    case ROP_not:
    case ROP_to_numeric:
      if (rop != ROP_div) info.op = Q2_OP_UNARY;
      info.ndef = 1;
      info.def[0] = 0;
      info.nuse = (rop == ROP_div) ? 2 : 1;
      info.use[0] = 1;
      info.use[1] = 2;
      info.reads = 0;
      info.writes = Q2_E_CONTROL;
      return &info;

    /* "rrr": t, s, h -- h = ++s (or --s), t = the value s had before. Two
       registers are written, which is why this cannot be Q2_OP_UNARY. */
    case ROP_post_inc:
    case ROP_post_dec:
      info.op = Q2_OP_POST_INC;
      info.ndef = 2;
      info.def[0] = 0;
      info.def[1] = 2;
      info.nuse = 1;
      info.use[0] = 1;
      info.reads = 0;
      info.writes = Q2_E_CONTROL;
      return &info;

    /* "ur": slot, s -- the closure cell takes a copy of register s. The cell is
       a heap location, so this writes a variable reference rather than a
       register; naming it stops it redefining every live register. */
    case ROP_put_var_ref:
      info.op = Q2_OP_PUT_VAR_REF;
      info.ndef = 0;
      info.nuse = 1;
      info.use[0] = 1;
      info.reads = Q2_E_VARREF;
      info.writes = Q2_E_VARREF | Q2_E_CONTROL;
      return &info;

    /* "rr": d, src (and "rrr": d, src, chk -- the propkey2 form, whose third
       register is only tested for null/undefined). Keys are strings, symbols or
       ints, all of which are already keys, so the result is a register the IR
       can name. */
    case ROP_to_propkey:
    case ROP_to_propkey2:
      info.op = Q2_OP_TO_PROPKEY;
      info.ndef = 1;
      info.def[0] = 0;
      info.nuse = (rop == ROP_to_propkey2) ? 2 : 1;
      info.use[0] = 1;
      info.use[1] = 2;
      info.reads = 0;
      info.writes = Q2_E_CONTROL;
      return &info;

    /* "ra": r, atom -- JS_DefineObjectName on the register. It can run a
       setter, so it is a control-effect instruction that writes no register. */
    case ROP_set_name:
      info.op = Q2_OP_SET_NAME;
      info.ndef = 0;
      info.nuse = 1;
      info.use[0] = 0;
      info.reads = Q2_E_HEAP;
      info.writes = Q2_E_HEAP | Q2_E_CONTROL;
      return &info;

    /* properties */
    case ROP_get_field:
    case ROP_get_field_own:
    case ROP_get_field2:
    case ROP_get_field2_own:
      info.op = Q2_OP_GET_FIELD;
      info.ndef = 1;
      info.def[0] = 0;
      info.nuse = 1;
      info.use[0] = 1;
      info.reads = Q2_E_SLOT | Q2_E_SHAPE;
      info.writes = Q2_E_SHAPE | Q2_E_CONTROL; /* a getter may run */
      return &info;
    case ROP_put_field: /* "rasr": receiver, atom, site, value */
      info.op = Q2_OP_PUT_FIELD;
      info.nuse = 2;
      info.use[0] = 0;
      info.use[1] = 3;
      info.reads = Q2_E_SLOT | Q2_E_SHAPE;
      info.writes = Q2_E_SLOT | Q2_E_SHAPE | Q2_E_HEAP | Q2_E_CONTROL;
      return &info;
    case ROP_get_field_chain: /* "rrasrras": two own reads */
    case ROP_get_field_chain_own:
      info.op = Q2_OP_GET_FIELD;
      info.ndef = 2;
      info.def[0] = 0;
      info.def[1] = 4;
      info.nuse = 1;
      info.use[0] = 1;
      info.reads = Q2_E_SLOT | Q2_E_SHAPE;
      info.writes = Q2_E_SHAPE | Q2_E_CONTROL;
      return &info;
    case ROP_get_element: /* "rrr": d, object, key */
    case ROP_get_length:  /* "rrs": d, object, site */
      info.op = Q2_OP_GET_ELEMENT;
      info.ndef = 1;
      info.def[0] = 0;
      info.nuse = rop == ROP_get_element ? 2 : 1;
      info.use[0] = 1;
      info.use[1] = 2;
      info.reads = Q2_E_ELEMENTS | Q2_E_SLOT;
      info.writes = Q2_E_ELEMENTS | Q2_E_SHAPE | Q2_E_CONTROL;
      return &info;
    case ROP_put_element: /* "rrr": object, key, value */
      info.op = Q2_OP_FALLBACK;
      info.nuse = 3;
      info.use[0] = 0;
      info.use[1] = 1;
      info.use[2] = 2;
      info.reads = Q2_E_ELEMENTS;
      info.writes = Q2_E_ELEMENTS | Q2_E_SLOT | Q2_E_HEAP | Q2_E_CONTROL;
      return &info;

    /* calls: the destination, the callee, the receiver and the argument
       window, every register of which the callee may read */
    /* The steps that read a register window: the window is a use of every
       register in it, or a typed value there would never reach the frame. */
    case ROP_array_from: /* "rrb": d, window, count */
      info.ndef = 1;
      info.def[0] = 0;
      info.window = 2;
      info.reads = 0;
      info.writes = Q2_E_HEAP | Q2_E_CONTROL;
      return &info;
    case ROP_object_fill: /* "rrbu": object (filled in place), window, count, template */
      info.nuse = 1;
      info.use[0] = 0;
      info.window = 2;
      info.reads = 0;
      info.writes = Q2_E_HEAP | Q2_E_SHAPE | Q2_E_SLOT | Q2_E_CONTROL;
      return &info;
    case ROP_tail_call: /* "rrb": callee, window, argc */
      info.nuse = 1;
      info.use[0] = 0;
      info.window = 2;
      info.reads = 0;
      info.writes = Q2_E_HEAP | Q2_E_SLOT | Q2_E_ELEMENTS | Q2_E_SHAPE |
                    Q2_E_GLOBAL | Q2_E_VARREF | Q2_E_CONTROL;
      return &info;
    case ROP_tail_call_method: /* "rrrb": callee, receiver, window, argc */
      info.nuse = 2;
      info.use[0] = 0;
      info.use[1] = 1;
      info.window = 3;
      info.reads = 0;
      info.writes = Q2_E_HEAP | Q2_E_SLOT | Q2_E_ELEMENTS | Q2_E_SHAPE |
                    Q2_E_GLOBAL | Q2_E_VARREF | Q2_E_CONTROL;
      return &info;
    case ROP_call: /* "rrrb": d, callee, window, argc */
      info.op = Q2_OP_CALL;
      info.ndef = 1;
      info.def[0] = 0;
      info.nuse = 1;
      info.use[0] = 1;
      info.window = 3;
      info.reads = 0;
      info.writes = Q2_E_HEAP | Q2_E_SLOT | Q2_E_ELEMENTS | Q2_E_SHAPE |
                    Q2_E_GLOBAL | Q2_E_VARREF | Q2_E_CONTROL;
      return &info;
    case ROP_call_method: /* "rrrrb": d, callee, receiver, window, argc */
      info.op = Q2_OP_CALL;
      info.ndef = 1;
      info.def[0] = 0;
      info.nuse = 2;
      info.use[0] = 1;
      info.use[1] = 2;
      info.window = 4;
      info.reads = 0;
      info.writes = Q2_E_HEAP | Q2_E_SLOT | Q2_E_ELEMENTS | Q2_E_SHAPE |
                    Q2_E_GLOBAL | Q2_E_VARREF | Q2_E_CONTROL;
      return &info;
    default:
      return &info;
  }
}

/* ---- small helpers ------------------------------------------------------- */
static uint64_t *bits_new(int nwords) {
  return calloc((size_t)(nwords > 0 ? nwords : 1), sizeof(uint64_t));
}
static int bits_test(const uint64_t *b, int i) {
  return (int)((b[i >> 6] >> (i & 63)) & 1);
}
static void bits_set(uint64_t *b, int i) {
  b[i >> 6] |= 1ull << (i & 63);
}
static void bits_or(uint64_t *d, const uint64_t *a, int nwords) {
  int i;
  for (i = 0; i < nwords; i++) d[i] |= a[i];
}
static int bits_equal(const uint64_t *a, const uint64_t *b, int nwords) {
  int i;
  for (i = 0; i < nwords; i++)
    if (a[i] != b[i]) return 0;
  return 1;
}

/* ---- value creation ------------------------------------------------------ */
static Q2ValueId q2_new_value(Q2Func *f) {
  if (f->nvalues == f->vcap) {
    int ncap = f->vcap ? f->vcap * 2 : 64;
    Q2Value *nv = realloc(f->values, sizeof(*nv) * (size_t)ncap);
    if (!nv) return Q2_NO_VALUE;
    f->values = nv;
    f->vcap = ncap;
  }
  memset(&f->values[f->nvalues], 0, sizeof(f->values[f->nvalues]));
  f->values[f->nvalues].reg = -1;
  f->values[f->nvalues].insn = -1;
  f->values[f->nvalues].param_of = -1;
  return (Q2ValueId)f->nvalues++;
}

/* The type a constant instruction produces. */
static uint32_t q2_const_type(uint16_t rop) {
  switch (rop) {
    case ROP_load_undefined:
      return Q2_T_UNDEF;
    case ROP_load_null:
      return Q2_T_NULL;
    case ROP_load_true:
    case ROP_load_false:
      return Q2_T_BOOL;
    case ROP_load_i8:
    case ROP_load_i32:
      return Q2_T_INT32 | Q2_T_NUM;
    default:
      return Q2_T_ANY;
  }
}

/* ---- the build ----------------------------------------------------------- */
/* Instructions that do not continue at the next pc. A static branch or return
   is one; so is a fallback whose engine handler leaves the frame (throw,
   yield, tail call). Everything else is assumed to continue, which is what
   the engine's handler does on the normal path. */
static int q2_ends_control(uint16_t rop) {
  switch (rop) {
    case ROP_throw:
    case ROP_throw_error:
    case ROP_initial_yield:
    case ROP_return:
    case ROP_return_async:
    case ROP_return_undefined:
    case ROP_tail_call:
    case ROP_tail_call_method:
    case ROP_yield:
    case ROP_yield_star:
      return 1;
    default:
      return 0;
  }
}

/* Whether a branch ROP can fall through. An unconditional `branch` cannot;
   the compare-branches and the `branch_if`/`branch_null*` family can. */
static int q2_branch_is_conditional(uint16_t rop) {
  switch (rop) {
    case ROP_branch:
    case ROP_branch_s:
    /* `add_imm_self_branch(_s)` ALWAYS jumps: every path of its engine handler
       (quickjs.c L_ROP_add_imm_self_branch) ends in `rpc += off`. Treating it as
       conditional added a phantom fall-through edge to the next instruction,
       which made the IR think the pc after it was reachable from here -- that
       edge loosened liveness and blocked the lowering of the loop it belongs to. */
    case ROP_add_imm_self_branch:
    case ROP_add_imm_self_branch_s:
      return 0;
    default:
      return 1;
  }
}

static int q2_has_target(const Q2Insn *in) {
  int n;
  for (n = 0; n < in->narg; n++)
    if (in->arg[n].kind == 't') return 1;
  return 0;
}
static int64_t q2_first_target(const Q2Insn *in) {
  int n;
  for (n = 0; n < in->narg; n++)
    if (in->arg[n].kind == 't') return in->arg[n].val;
  return 0;
}

int64_t q2_first_target_pub(const Q2Insn *in) {
  return q2_first_target(in);
}

static int cmp_i32(const void *a, const void *b) {
  int32_t x = *(const int32_t *)a, y = *(const int32_t *)b;
  return x < y ? -1 : (x > y ? 1 : 0);
}

/* Trivial block-parameter removal (Braun et al. 2013): a parameter whose
   incoming arguments are all the same value becomes that value, so the
   parameter goes away and every use names it directly. The entry block's
   parameters stay: they are the frame loads. */
static Q2ValueId q2_find(const Q2ValueId *repl, Q2ValueId v) {
  while (repl[v] != v) v = repl[v];
  return v;
}

static int q2_prune_params(Q2Func *f) {
  Q2ValueId *repl =
      malloc(sizeof(Q2ValueId) * (size_t)(f->nvalues > 0 ? f->nvalues : 1));
  int changed = 1, b, i, guard = 0;
  if (!repl) return -1;
  for (i = 0; i < f->nvalues; i++) repl[i] = (Q2ValueId)i;
  for (b = 0; b < f->nblocks; b++) {
    Q2Block *blk = &f->blocks[b];
    int n = blk->nparams > 0 ? blk->nparams : 1;
    blk->in_reg = malloc(sizeof(int32_t) * (size_t)n);
    blk->in_val = malloc(sizeof(Q2ValueId) * (size_t)n);
    if (!blk->in_reg || !blk->in_val) {
      free(repl);
      return -1;
    }
    memcpy(blk->in_reg, blk->param_reg, sizeof(int32_t) * (size_t)blk->nparams);
    memcpy(
        blk->in_val, blk->param_val, sizeof(Q2ValueId) * (size_t)blk->nparams);
    blk->nlive_in = blk->nparams;
  }
  while (changed && guard++ < f->nblocks + 8) {
    changed = 0;
    for (b = 1; b < f->nblocks; b++) { /* the entry block's params stay */
      Q2Block *blk = &f->blocks[b];
      int p = 0;
      while (p < blk->nparams) {
        Q2ValueId pv = blk->param_val[p];
        Q2ValueId common = Q2_NO_VALUE;
        int consistent = 1, e;
        for (e = 0; e < blk->npreds; e++) {
          Q2Block *pr = &f->blocks[blk->preds[e]];
          int n;
          Q2ValueId av;
          for (n = 0; n < pr->nsucc; n++) {
            int si = n;
            if (pr->succ_block[si] != b) continue;
            if (p >= pr->nedge_args[si]) {
              consistent = 0;
              break;
            }
            av = q2_find(repl, pr->edge_arg[si][p]);
            if (av == pv) continue; /* this edge passes the parameter itself */
            if (common == Q2_NO_VALUE)
              common = av;
            else if (q2_find(repl, common) != av) {
              consistent = 0;
              break;
            }
          }
          if (!consistent) break;
        }
        if (consistent && common != Q2_NO_VALUE) {
          repl[pv] = common;
          for (i = p; i + 1 < blk->nparams; i++) {
            blk->param_val[i] = blk->param_val[i + 1];
            blk->param_reg[i] = blk->param_reg[i + 1];
          }
          blk->nparams--;
          for (e = 0; e < blk->npreds; e++) {
            Q2Block *pr = &f->blocks[blk->preds[e]];
            int n, k;
            for (n = 0; n < pr->nsucc; n++) {
              int si = n;
              if (pr->succ_block[si] != b) continue;
              if (p >= pr->nedge_args[si]) continue;
              for (k = p; k + 1 < pr->nedge_args[si]; k++)
                pr->edge_arg[si][k] = pr->edge_arg[si][k + 1];
              pr->nedge_args[si]--;
            }
          }
          changed = 1;
        } else {
          p++;
        }
      }
    }
  }
  /* rewrite every reference through the replacement map */
  for (i = 0; i < f->ninsns; i++) {
    Q2Insn *in = &f->insns[i];
    int k;
    for (k = 0; k < in->nuse; k++) in->use[k] = q2_find(repl, in->use[k]);
    for (k = 0; k < in->narg; k++)
      if (in->argval[k] != Q2_NO_VALUE)
        in->argval[k] = q2_find(repl, in->argval[k]);
    for (k = 0; k < in->win_n; k++)
      in->win_val[k] = q2_find(repl, in->win_val[k]);
    for (k = 0; k < in->nredef; k++)
      in->redef_val[k] = q2_find(repl, in->redef_val[k]);
  }
  for (b = 0; b < f->nblocks; b++) {
    Q2Block *blk = &f->blocks[b];
    int n, k;
    for (k = 0; k < blk->nparams; k++)
      blk->param_val[k] = q2_find(repl, blk->param_val[k]);
    for (k = 0; k < blk->nlive_in; k++)
      blk->in_val[k] = q2_find(repl, blk->in_val[k]);
    for (n = 0; n < blk->nsucc; n++)
      for (k = 0; k < blk->nedge_args[n]; k++)
        if (blk->edge_arg[n][k] != Q2_NO_VALUE)
          blk->edge_arg[n][k] = q2_find(repl, blk->edge_arg[n][k]);
  }
  free(repl);
  return 0;
}

/* The compare-and-branch opcodes with the comparison in the opcode
   (`branch_lt_s` .. `branch_neq_s`, "rrt") are rewritten into the operand form
   of `branch_cmp_reg_reg_s` ("brrt"): the comparison becomes a synthetic `b`
   operand ahead of the registers, so they share its roles and lowering. Only
   the IR changes; a step still runs the real instruction at its pc. */
static void q2_canon_cmp_branch(Q2Insn *in) {
  int sub, k;
  switch (in->rop) {
    case ROP_branch_lt_s:
      sub = REG_BIN_LT;
      break;
    case ROP_branch_nlt_s:
      sub = REG_BIN_LT | REG_CMP_INVERT;
      break;
    case ROP_branch_lte_s:
      sub = REG_BIN_LTE;
      break;
    case ROP_branch_nlte_s:
      sub = REG_BIN_LTE | REG_CMP_INVERT;
      break;
    case ROP_branch_gt_s:
      sub = REG_BIN_GT;
      break;
    case ROP_branch_ngt_s:
      sub = REG_BIN_GT | REG_CMP_INVERT;
      break;
    case ROP_branch_gte_s:
      sub = REG_BIN_GTE;
      break;
    case ROP_branch_ngte_s:
      sub = REG_BIN_GTE | REG_CMP_INVERT;
      break;
    case ROP_branch_eq_s:
      sub = REG_BIN_EQ;
      break;
    case ROP_branch_neq_s:
      sub = REG_BIN_NEQ;
      break;
    default:
      return;
  }
  if (in->narg != 3 || in->arg[0].kind != 'r' || in->arg[1].kind != 'r' ||
      in->arg[2].kind != 't')
    return;
  for (k = 3; k > 0; k--) in->arg[k] = in->arg[k - 1];
  in->arg[0].kind = 'b';
  in->arg[0].val = sub;
  in->arg[0].off = -1;
  in->narg = 4;
  in->rop = ROP_branch_cmp_reg_reg_s;
}

/* Whether an instruction's destination takes its type from a profile record
   rather than from its operands. Kept in step with `def_from_record` in
   e_types.c, which decides the same set for the inference; a destination not
   in this set must not be typed from an `R` record here. */
static int def_from_record_insn(const Q2Insn *in) {
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
      return in->ndef > 0;
    case Q2_OP_UNARY:
      return in->rop == ROP_neg;
    default:
      return 0;
  }
}

int q2_build(const JSAotGenFunc *g, Q2Func *f) {
  int i, n, words;
  int32_t *starts = NULL;
  int nstarts = 0, cap = 0;
  int32_t *pc_to_block = NULL;

  memset(f, 0, sizeof(*f));
  f->g = g;
  f->nregs = g->reg_count;
  words = (f->nregs + 63) / 64;
  f->lwords = words;
  f->exit_pc = g->len;
  if (g->len <= 0 || g->reg_count < 0) {
    f->why = "empty program";
    return -1;
  }
  if (g->reg_count > 4096 || g->len > (1 << 18)) {
    f->why = "program too large for the IR";
    return -1;
  }

  /* 1. decode every instruction */
  f->insns = calloc((size_t)g->len + 1, sizeof(*f->insns));
  if (!f->insns) {
    f->why = "out of memory";
    return -1;
  }
  for (i = 0; i < g->len;) {
    JSAotGenInsn cur;
    Q2Insn *in;
    const Q2RopInfo *ri;

    if (js_aot_gen_decode_insn(g->code, g->len, i, &cur) < 0) {
      f->why = "malformed program";
      return -1;
    }
    in = &f->insns[f->ninsns];
    in->rop = (uint16_t)cur.op;
    in->pc = i;
    in->next = i + cur.size;
    in->narg = cur.nargs;
    for (n = 0; n < cur.nargs && n < 8; n++) {
      in->arg[n].kind = cur.arg[n].kind;
      in->arg[n].val = cur.arg[n].val;
      in->arg[n].off = cur.arg[n].off;
    }
    q2_canon_cmp_branch(in);
    ri = q2_rop_info(in->rop);
    in->op = (int8_t)ri->op;
    in->sub = q2_rop_sub(in->rop, in);
    in->sub2 = q2_rop_sub2(in->rop);
    for (n = 0; n < ri->ndef && n < 2; n++) {
      int idx = ri->def[n];
      if (idx < in->narg && in->arg[idx].kind == 'r')
        in->def[in->ndef++] = (Q2ValueId)q2_ri((int)in->arg[idx].val);
      else
        in->ndef = 0; /* a role that does not match the decoding: fall back */
    }
    for (n = 0; n < ri->nuse && n < 3; n++) {
      int idx = ri->use[n];
      if (idx < in->narg && in->arg[idx].kind == 'r')
        in->use[in->nuse++] = (Q2ValueId)q2_ri((int)in->arg[idx].val);
    }
    if (in->ndef == 0 && ri->ndef != 0) in->op = Q2_OP_FALLBACK;
    if (in->op == Q2_OP_FALLBACK && ri->ndef == 0 && ri->nuse == 0) {
      /* Unknown roles: the step engine reads every register operand of the
         instruction, so every one of them is a use, and which is the
         destination is unknown, so the instruction redefines the whole live
         set instead. A fallback that declares roles keeps them. */
      in->ndef = 0;
      in->nuse = 0;
      for (n = 0; n < in->narg && in->nuse < 8; n++)
        if (in->arg[n].kind == 'r')
          in->use[in->nuse++] = (Q2ValueId)q2_ri((int)in->arg[n].val);
    } else if (ri->window >= 0 && ri->window < in->narg) {
      /* A call: the callee reads the whole argument window, so every register
         in it is live at the call and must be current in the frame. */
      int na = (int)in->arg[ri->window].val;
      int ab = (int)in->arg[ri->window - 1].val;
      if (na < 0 || na > 255) na = 0;
      in->win_reg = q2_ri(ab);
      in->win_n = na;
      if (in->win_n > 0 && in->win_reg + in->win_n > g->reg_count)
        in->win_n = g->reg_count - in->win_reg;
      if (in->win_n < 0) in->win_n = 0;
    }
    {
      /* Ops that read a staged run of registers the format does not count: the
         run's first register is operand `wop` and its length is fixed. `apply` and
         `apply_arguments` read the function, `apply` and the receiver; `append`
         reads the array, the next index and the iterable and writes the index
         back; `iterator_call` reads four; the class definitions read their staged
         key, parent and constructor. */
      int wop = -1, wn = 0;
      switch (in->rop) {
        case ROP_apply_arguments:
        case ROP_append: wop = 0; wn = 3; break;
        case ROP_apply: wop = 1; wn = 3; break;
        case ROP_iterator_call: wop = 0; wn = 4; break;
        case ROP_define_class: wop = 0; wn = 2; break;
        case ROP_define_class_computed: wop = 0; wn = 3; break;
        default: break;
      }
      if (wop >= 0 && wop < in->narg && in->arg[wop].kind == 'r') {
        in->win_reg = q2_ri((int)in->arg[wop].val);
        in->win_n = wn;
        if (in->win_reg + in->win_n > g->reg_count)
          in->win_n = g->reg_count - in->win_reg;
        if (in->win_n < 0) in->win_n = 0;
      }
    }
    f->ninsns++;
    i += cur.size;
  }
  if (f->ninsns == 0) {
    f->why = "no instructions";
    return -1;
  }

  /* 2. block starts: pc 0, every branch target, and after a terminator */
  {
    /* A block starts at pc 0, at every branch target, at the fall-through of
       an instruction with a target, and after a terminator. A fallback is not a
       terminator: since a fallback that does not fall through leaves compiled
       code, it cannot re-enter at an arbitrary pc, so there is no reason to
       split at one. */
    int32_t *tmp = malloc(sizeof(*tmp) * (size_t)(3 * f->ninsns + 2));
    if (!tmp) {
      f->why = "out of memory";
      return -1;
    }
    tmp[nstarts++] = 0;
    for (i = 0; i < f->ninsns; i++) {
      Q2Insn *in = &f->insns[i];
      /* A fallback ends its block: it may leave compiled code, and its
         fall-through (the reloads after the step) is the next block. */
      int term =
          (in->op == Q2_OP_BRANCH || in->op == Q2_OP_BRANCH_CMP ||
           in->op == Q2_OP_RETURN || in->op == Q2_OP_RETURN_UNDEF ||
           in->op == Q2_OP_FALLBACK || !q2_lowered(in->rop) ||
           q2_ends_control(in->rop));
      int ntargets = 0;
      for (n = 0; n < in->narg; n++) {
        if (in->arg[n].kind != 't') continue;
        ntargets++;
        tmp[nstarts++] = in->next + (int32_t)in->arg[n].val;
        /* the fall-through of a branch is a block too */
        tmp[nstarts++] = in->next;
      }
      if (ntargets > 1) {
        f->why = "branch with two targets";
        free(tmp);
        return -1;
      }
      if (term) tmp[nstarts++] = in->next;
      /* An instruction the lowering will not handle runs through the step and
         its fall-through is a block start: that is where the reloads after the
         step land, and where the step's next pc is labelled. */
      if (in->op == Q2_OP_FALLBACK || !q2_lowered(in->rop))
        tmp[nstarts++] = in->next;
    }
    tmp[nstarts++] = f->exit_pc;
    qsort(tmp, (size_t)nstarts, sizeof(*tmp), cmp_i32);
    starts = malloc(sizeof(*starts) * (size_t)nstarts);
    if (!starts) {
      free(tmp);
      f->why = "out of memory";
      return -1;
    }
    cap = 0;
    for (i = 0; i < nstarts; i++) {
      if (tmp[i] < 0 || tmp[i] > f->exit_pc) continue; /* dead target */
      if (cap > 0 && starts[cap - 1] == tmp[i]) continue;
      starts[cap++] = tmp[i];
    }
    nstarts = cap;
    free(tmp);
  }
  f->nblocks = nstarts - 1; /* the last start is the exit */
  if (f->nblocks <= 0) {
    f->why = "no block";
    return -1;
  }
  f->blocks = calloc((size_t)f->nblocks, sizeof(*f->blocks));
  pc_to_block = malloc(sizeof(*pc_to_block) * (size_t)(f->exit_pc + 1));
  f->live_in = bits_new(words * f->nblocks);
  f->live_out = bits_new(words * f->nblocks);
  f->reg_def =
      malloc(sizeof(*f->reg_def) * (size_t)(f->nregs > 0 ? f->nregs : 1));
  if (!f->blocks || !pc_to_block || !f->live_in || !f->live_out ||
      !f->reg_def) {
    f->why = "out of memory";
    goto fail;
  }
  for (i = 0; i <= f->exit_pc; i++) pc_to_block[i] = -1;
  for (i = 0; i < f->nblocks; i++) {
    f->blocks[i].start_pc = starts[i];
    f->blocks[i].end_pc = starts[i + 1];
    f->blocks[i].first_insn = -1;
    f->blocks[i].ctl_in = -1;
    pc_to_block[starts[i]] = i;
  }
  pc_to_block[f->exit_pc] = f->nblocks; /* the exit block */

  /* instruction -> block, and block extents */
  for (i = 0, n = 0; i < f->ninsns; i++) {
    while (n + 1 < f->nblocks && f->blocks[n + 1].start_pc <= f->insns[i].pc)
      n++;
    if (f->blocks[n].first_insn < 0) f->blocks[n].first_insn = i;
    f->blocks[n].ninsns++;
    if (f->insns[i].next > f->blocks[n].end_pc) {
      f->why = "instruction crosses a block boundary";
      goto fail;
    }
  }

  /* 3. successors: the last instruction of a block decides */
  for (i = 0; i < f->nblocks; i++) {
    Q2Block *b = &f->blocks[i];
    Q2Insn *last;
    if (b->ninsns == 0) {
      b->nsucc = 0;
      continue;
    }
    last = &f->insns[b->first_insn + b->ninsns - 1];
    b->nsucc = 0;
    if (last->op == Q2_OP_RETURN || last->op == Q2_OP_RETURN_UNDEF) {
      b->nsucc = 0;
    } else if (last->op == Q2_OP_BRANCH || last->op == Q2_OP_BRANCH_CMP) {
      int is_cmp = (last->op == Q2_OP_BRANCH_CMP);
      for (n = 0; n < last->narg; n++) {
        int32_t tgt;
        if (last->arg[n].kind != 't') continue;
        tgt = last->next + (int32_t)last->arg[n].val;
        if (tgt < 0 || tgt > f->exit_pc) continue;
        b->succ_pc[b->nsucc] = tgt;
        b->succ_block[b->nsucc] = pc_to_block[tgt];
        b->nsucc++;
        break;
      }
      /* Only a *conditional* branch has a fall-through. An unconditional
         `branch` transfers, so its fall-through is dead and its edge (which
         could carry a parameter) must not be required. */
      if (is_cmp || q2_branch_is_conditional(last->rop)) {
        b->succ_pc[b->nsucc] = last->next;
        b->succ_block[b->nsucc] = pc_to_block[last->next];
        b->nsucc++;
      }
    } else if (q2_ends_control(last->rop)) {
      b->nsucc = 0;
    } else if (
        (last->op == Q2_OP_FALLBACK || !q2_lowered(last->rop)) &&
        q2_has_target(last)) {
      /* A fallback that is a branch (a fused increment-branch and the like)
         keeps both edges: the taken target first, then the fall-through, so
         the lowering can route the step's returned ip. */
      int32_t tgt = last->next + (int32_t)q2_first_target(last);
      b->succ_pc[0] = tgt;
      b->succ_block[0] = pc_to_block[tgt];
      b->nsucc = 1;
      b->succ_pc[b->nsucc] = last->next;
      b->succ_block[b->nsucc] = pc_to_block[last->next];
      b->nsucc++;
    } else {
      b->succ_pc[0] = last->next;
      b->succ_block[0] = pc_to_block[last->next];
      b->nsucc = 1;
    }
    for (n = 0; n < b->nsucc; n++) {
      Q2Block *s =
          b->succ_block[n] < f->nblocks ? &f->blocks[b->succ_block[n]] : NULL;
      if (s) s->npreds++; /* counted here, filled below */
    }
  }
  f->entry_block = 0;

  /* 3b. the predecessor lists, sized by the count above */
  for (i = 0; i < f->nblocks; i++) {
    if (f->blocks[i].npreds > 0) {
      f->blocks[i].preds =
          malloc(sizeof(int32_t) * (size_t)f->blocks[i].npreds);
      if (!f->blocks[i].preds) {
        f->why = "out of memory";
        goto fail;
      }
    }
    f->blocks[i].npreds = 0;
  }
  for (i = 0; i < f->nblocks; i++) {
    Q2Block *b = &f->blocks[i];
    for (n = 0; n < b->nsucc; n++) {
      Q2Block *s =
          b->succ_block[n] < f->nblocks ? &f->blocks[b->succ_block[n]] : NULL;
      if (s) s->preds[s->npreds++] = i;
    }
  }

  /* A predecessor with two successors that are the same block (a conditional
     branch whose target is its fall-through) appears once: pruning walks a
     predecessor's successor slots directly, so a duplicate entry would remove
     a parameter's argument twice. */
  for (i = 0; i < f->nblocks; i++) {
    Q2Block *b = &f->blocks[i];
    int x, y, w = 0;
    for (x = 0; x < b->npreds; x++) {
      int dup = 0;
      for (y = 0; y < w; y++)
        if (b->preds[y] == b->preds[x]) {
          dup = 1;
          break;
        }
      if (!dup) b->preds[w++] = b->preds[x];
    }
    b->npreds = w;
  }

  /* 4. liveness over the whole register file, one backward walk per block, so
     the live set at each instruction is exact. A fallback with known roles
     defines only its own destinations; one with unknown roles may write any
     register live after it. */
  {
    int changed = 1, pass = 0;
    uint64_t *use = bits_new(words), *def = bits_new(words);
    uint64_t *in = bits_new(words), *out = bits_new(words),
             *cur = bits_new(words);
    if (!use || !def || !in || !out || !cur) {
      f->why = "out of memory";
      goto fail;
    }
    f->insn_live = calloc((size_t)f->ninsns * (size_t)words, sizeof(uint64_t));
    if (!f->insn_live) {
      f->why = "out of memory";
      goto fail;
    }
    while (changed && pass++ < f->nblocks + 8) {
      changed = 0;
      for (i = f->nblocks - 1; i >= 0; i--) {
        Q2Block *b = &f->blocks[i];
        int k;
        memset(out, 0, (size_t)words * 8);
        for (n = 0; n < b->nsucc; n++)
          if (b->succ_block[n] < f->nblocks)
            bits_or(out, f->live_in + (size_t)b->succ_block[n] * words, words);
        memcpy(cur, out, (size_t)words * 8);
        for (k = b->ninsns - 1; k >= 0; k--) {
          Q2Insn *in2 = &f->insns[b->first_insn + k];
          int m;
          memcpy(
              f->insn_live + (size_t)(b->first_insn + k) * words, cur,
              (size_t)words * 8);
          if (in2->op == Q2_OP_FALLBACK && in2->ndef == 0) {
            /* Unknown roles: the step may write any register live after it,
               and the lowering reloads all of them. It kills none, though: a
               register it leaves alone keeps its value, so that value must be
               in the frame before the step, which needs it live there. */
          } else {
            for (m = 0; m < in2->ndef; m++) {
              int r = (int)in2->def[m];
              cur[r >> 6] &= ~(1ull << (r & 63));
            }
          }
          for (m = 0; m < in2->nuse; m++) {
            int r = (int)in2->use[m];
            cur[r >> 6] |= 1ull << (r & 63);
          }
          for (m = 0; m < in2->win_n; m++) {
            int r = in2->win_reg + m;
            cur[r >> 6] |= 1ull << (r & 63);
          }
        }
        memcpy(in, cur, (size_t)words * 8);
        if (!bits_equal(in, f->live_in + (size_t)i * words, words)) {
          memcpy(f->live_in + (size_t)i * words, in, (size_t)words * 8);
          changed = 1;
        }
        memcpy(f->live_out + (size_t)i * words, out, (size_t)words * 8);
      }
    }
    free(use);
    free(def);
    free(in);
    free(out);
    free(cur);
  }

  /* 5. values: block parameters, then one definition per write */
  {
    Q2ValueId *def_at = f->reg_def; /* per register: the value defined so far */
    int b;
    /* Parameters first: every block's live-in registers get one value, so a
       predecessor can name them while the graph is still being walked. */
    for (b = 0; b < f->nblocks; b++) {
      Q2Block *blk = &f->blocks[b];
      int k, nreg = 0;
      for (k = 0; k < f->nregs; k++)
        if (bits_test(f->live_in + (size_t)b * words, k)) nreg++;
      blk->param_reg = malloc(sizeof(int32_t) * (size_t)(nreg > 0 ? nreg : 1));
      blk->param_val =
          malloc(sizeof(Q2ValueId) * (size_t)(nreg > 0 ? nreg : 1));
      blk->edge_arg = calloc(2, sizeof(*blk->edge_arg));
      if (!blk->param_reg || !blk->param_val || !blk->edge_arg) {
        f->why = "out of memory";
        goto fail;
      }
      for (k = 0; k < f->nregs; k++) {
        Q2ValueId v;
        if (!bits_test(f->live_in + (size_t)b * words, k)) continue;
        v = q2_new_value(f);
        if (v == Q2_NO_VALUE) {
          f->why = "out of memory";
          goto fail;
        }
        f->values[v].reg = k;
        f->values[v].block = b;
        f->values[v].type = Q2_T_ANY;
        f->values[v].rep = Q2_R_TAGGED;
        f->values[v].param_of = blk->nparams;
        blk->param_reg[blk->nparams] = k;
        blk->param_val[blk->nparams] = v;
        blk->nparams++;
      }
    }
    for (b = 0; b < f->nblocks; b++) {
      Q2Block *blk = &f->blocks[b];
      int k;
      for (k = 0; k < f->nregs; k++) def_at[k] = Q2_NO_VALUE;
      for (k = 0; k < blk->nparams; k++)
        def_at[blk->param_reg[k]] = blk->param_val[k];
      for (k = 0; k < blk->ninsns; k++) {
        Q2Insn *in2 = &f->insns[blk->first_insn + k];
        int m;
        /* The value each register operand holds, before this instruction's own
           definitions: what the lowering reads. */
        for (m = 0; m < in2->narg; m++) {
          in2->argval[m] = Q2_NO_VALUE;
          if (in2->arg[m].kind == 'r') {
            int r = q2_ri((int)in2->arg[m].val);
            if (r >= 0 && r < f->nregs) in2->argval[m] = def_at[r];
          }
        }
        for (m = 0; m < in2->nuse; m++) {
          int r = (int)in2->use[m];
          if (def_at[r] == Q2_NO_VALUE) {
            f->why = "use of a register with no definition";
            goto fail;
          }
          in2->use[m] = def_at[r]; /* now a value id */
        }
        if (in2->win_n > 0) {
          int w;
          in2->win_val = malloc(sizeof(Q2ValueId) * (size_t)in2->win_n);
          if (!in2->win_val) {
            f->why = "out of memory";
            goto fail;
          }
          for (w = 0; w < in2->win_n; w++) {
            int r = in2->win_reg + w;
            if (def_at[r] == Q2_NO_VALUE) {
              f->why = "call window register with no definition";
              goto fail;
            }
            in2->win_val[w] = def_at[r];
          }
        }
        if (in2->op == Q2_OP_FALLBACK && in2->ndef == 0 &&
            q2_rop_info(in2->rop)->nuse == 0) {
          /* Unknown roles: the step may write any register live after this
             instruction (not the block), so redefine those and remember which,
             for the lowering to reload from the frame. A fallback whose roles
             are declared (a store: uses, no destination) writes no register,
             so the values live across it keep their representation. */
          uint64_t *lo = f->insn_live + (size_t)(blk->first_insn + k) * words;
          int nlive = 0;
          for (m = 0; m < f->nregs; m++)
            if (bits_test(lo, m)) nlive++;
          if (nlive > 0) {
            in2->redef_reg = malloc(sizeof(int32_t) * (size_t)nlive);
            in2->redef_val = malloc(sizeof(Q2ValueId) * (size_t)nlive);
            if (!in2->redef_reg || !in2->redef_val) {
              f->why = "out of memory";
              goto fail;
            }
          }
          for (m = 0; m < f->nregs; m++) {
            Q2ValueId v;
            if (!bits_test(lo, m)) continue;
            v = q2_new_value(f);
            if (v == Q2_NO_VALUE) {
              f->why = "out of memory";
              goto fail;
            }
            f->values[v].reg = m;
            f->values[v].block = b;
            f->values[v].insn = blk->first_insn + k;
            f->values[v].type = Q2_T_ANY;
            f->values[v].rep = Q2_R_TAGGED;
            in2->redef_reg[in2->nredef] = m;
            in2->redef_val[in2->nredef] = v;
            in2->nredef++;
            def_at[m] = v;
          }
          in2->ndef = 0;
        } else {
          for (m = 0; m < in2->ndef; m++) {
            int r = (int)in2->def[m];
            Q2ValueId v = q2_new_value(f);
            if (v == Q2_NO_VALUE) {
              f->why = "out of memory";
              goto fail;
            }
            f->values[v].reg = r;
            f->values[v].block = b;
            f->values[v].insn = blk->first_insn + k;
            if (in2->op == Q2_OP_CONST) {
              f->values[v].type = q2_const_type(in2->rop);
              switch (in2->rop) {
                case ROP_load_i8:
                case ROP_load_i8_s:
                case ROP_load_i32:
                  f->values[v].con_kind = Q2_CON_INT;
                  break;
                case ROP_load_true:
                case ROP_load_false:
                  f->values[v].con_kind = Q2_CON_TRUE;
                  break;
                case ROP_load_null:
                  f->values[v].con_kind = Q2_CON_NULL;
                  break;
                case ROP_load_undefined:
                  f->values[v].con_kind = Q2_CON_UNDEF;
                  break;
                default:
                  f->values[v].con_kind = Q2_CON_UNDEF;
                  break;
              }
            } else if (
                in2->op == Q2_OP_BINARY || in2->op == Q2_OP_UNARY ||
                in2->op == Q2_OP_COMPARE) {
              f->values[v].type = Q2_T_NUM | Q2_T_STR | Q2_T_OBJ | Q2_T_BOOL |
                                  Q2_T_BIGINT | Q2_T_UNDEF | Q2_T_NULL |
                                  Q2_T_SYMBOL;
            } else {
              f->values[v].type = Q2_T_ANY;
            }
            f->values[v].rep = Q2_R_TAGGED;
            in2->def[m] = v;
            def_at[r] = v;
          }
        }
      }
      /* edge arguments */
      for (n = 0; n < blk->nsucc; n++) {
        Q2Block *s = blk->succ_block[n] < f->nblocks
                         ? &f->blocks[blk->succ_block[n]]
                         : NULL;
        int m;
        if (!s) continue;
        blk->edge_arg[n] = malloc(
            sizeof(Q2ValueId) * (size_t)(s->nparams > 0 ? s->nparams : 1));
        blk->nedge_args[n] = s->nparams;
        if (!blk->edge_arg[n]) {
          f->why = "out of memory";
          goto fail;
        }
        for (m = 0; m < s->nparams; m++) {
          int r = s->param_reg[m];
          if (def_at[r] != Q2_NO_VALUE)
            blk->edge_arg[n][m] = def_at[r];
          else
            blk->edge_arg[n][m] = s->param_val[m];
        }
      }
    }
  }

  {
    int pb, pk;
    for (pb = 0; pb < f->nblocks; pb++)
      f->nparams_before += f->blocks[pb].nparams;
    (void)pk;
  }

  /* Drop the block parameters that are the same value on every edge */
  if (q2_prune_params(f) < 0) {
    f->why = "out of memory";
    goto fail;
  }

  /* 6. control-stack depth per block */
  {
    int changed = 1, guard = 0;
    f->blocks[0].ctl_in = 0;
    while (changed && guard++ < f->nblocks + 8) {
      changed = 0;
      for (i = 0; i < f->nblocks; i++) {
        Q2Block *b = &f->blocks[i];
        int k, d = 0;
        if (b->ctl_in < 0) continue;
        for (k = 0; k < b->ninsns; k++) {
          switch (f->insns[b->first_insn + k].rop) {
            case ROP_catch_enter:
            case ROP_finally_enter:
            case ROP_iter_start:
            case ROP_for_await_of_start:
              d += 1;
              break;
            case ROP_finally_leave:
            case ROP_nip_catch:
            case ROP_iter_close:
            case ROP_sentinel_drop:
              d -= 1;
              break;
            default:
              break;
          }
        }
        if (d > f->max_ctl - b->ctl_in) f->max_ctl = b->ctl_in + d;
        if (d < 0 && b->ctl_in + d < 0) f->why = "control stack underflow";
        for (n = 0; n < b->nsucc; n++) {
          Q2Block *s = b->succ_block[n] < f->nblocks
                           ? &f->blocks[b->succ_block[n]]
                           : NULL;
          if (!s) continue;
          if (s->ctl_in < 0) {
            s->ctl_in = b->ctl_in + d;
            changed = 1;
          }
        }
      }
    }
  }

  f->ok = 1;

  /* 8. the profile's per-pc type sets, mapped onto each definition */
  {
    int n = 0;
    f->prof = qjs_aot_prof_pcs(g->digest, &n);
    f->nprof = n;
    f->cres = qjs_aot_prof_cres(g->digest, &n);
    f->ncres = n;
    /* The per-register `R` records, kept for the per-destination lookup. A
       register with no record is simply absent: nprof_regs counts only
       registers the profiler actually saw. */
    {
      int nr = 0;
      f->prof_regs = qjs_aot_prof_regs(g->digest, &nr);
      f->nprof_regs = nr;
    }
    if (f->prof && f->nprof > 0) {
      for (i = 0; i < f->nvalues; i++) {
        Q2Value *v = &f->values[i];
        const ProfPc *p;
        int lo, hi, found = -1;
        int32_t dpc;
        if (v->insn < 0) continue;
        dpc = f->insns[v->insn].pc;
        lo = 0;
        hi = f->nprof - 1;
        while (lo <= hi) {
          int mid = (lo + hi) / 2;
          if (f->prof[mid].pc == dpc) {
            found = mid;
            break;
          }
          if (f->prof[mid].pc < dpc)
            lo = mid + 1;
          else
            hi = mid - 1;
        }
        if (found < 0) continue;
        p = &f->prof[found];
        v->has_prof = 1;
        v->ptypes = prof_to_q2(p->types);
        v->frac = p->frac;
        v->negzero = p->negzero;
        v->nmin = p->num_min;
        v->nmax = p->num_max;
      }
    }
  }

  /* 9. the profile's per-register `R` records, for a destination the `P` record
     could not type. An `R` record covers the whole function, so it describes a
     destination exactly only when that register is written once; a register
     written more than once may hold a different value's history, and then the
     record is at best imprecise. This is a *precision* rule (a reused register's
     record is wide anyway, and the threaded build records both `get_arg2`
     results every time it runs), kept because it is cheap. It is only sound at
     all because the profile's header said `thr=1`, which
     `qjs_aot_prof_regs` enforces by returning nothing otherwise. */
  if (f->prof_regs != NULL && f->nprof_regs > 0 && f->nregs > 0 &&
      strcmp(getenv("E_R_RULE") ?: "", "off")) {
    int32_t *nwrites = calloc((size_t)f->nregs, sizeof(*nwrites));
    if (nwrites == NULL) goto fail;
    for (i = 0; i < f->ninsns; i++) {
      const Q2Insn *in = &f->insns[i];
      int k;
      for (k = 0; k < in->ndef; k++) {
        const Q2Value *dv = &f->values[in->def[k]];
        if (dv->reg >= 0 && dv->reg < f->nregs) nwrites[dv->reg]++;
      }
      for (k = 0; k < in->nredef; k++) {
        int32_t r = in->redef_reg[k];
        if (r >= 0 && r < f->nregs) nwrites[r]++;
      }
      if (in->win_reg >= 0 && in->win_n > 0 &&
          in->win_reg + in->win_n <= f->nregs) {
        for (k = 0; k < in->win_n; k++) nwrites[in->win_reg + k]++;
      }
    }
    for (i = 0; i < f->nvalues; i++) {
      Q2Value *v = &f->values[i];
      const Q2Insn *in;
      int lo, hi, found = -1;
      /* Only where the `P` record could not type it: a destination with no
         record at all, or one whose record is mixed. An `R` record is a
         per-register union over the whole function, so it can only help when
         the per-pc view is unusable -- and then only if the register is written
         once, which the write count below checks. */
      if (v->insn < 0) continue;
      in = &f->insns[v->insn];
      if (!def_from_record_insn(in)) continue;
      /* Which values the `R` record may type.
         - A `get_arg2` destination whose `P` record holds **more than one**
           type. That record is merged across both destinations by
           construction (`get_arg2` is "ruru": two results at one pc), so it is
           never more precise than the destination's own register record. This
           is the case E7d was written for.
         - A value with no `P` record at all.
         A single-type `P` record is already exact, so an `R` record could only
         widen it; leave those alone. */
      {
        int multi = v->ptypes != 0 && (v->ptypes & (v->ptypes - 1)) != 0;
        int want = v->has_prof ? (in->op == Q2_OP_GET_ARG2 && multi) : 1;
        if (!want) continue;
      }

      if (v->reg < 0 || v->reg >= f->nregs) continue;
      if (getenv("E_DEBUG_CAND")) {
        int w, rtypes = -1;
        for (w = 0; w < f->nprof_regs; w++)
          if (f->prof_regs[w].reg == v->reg) rtypes = (int)f->prof_regs[w].types;
        fprintf(stderr, "CAND %s v%u reg=r%d ptypes=0x%x R=0x%x writes=%d pcs:",
                g->name && g->name[0] ? g->name : "?", i, v->reg, v->ptypes,
                rtypes < 0 ? -1 : rtypes, nwrites[v->reg]);
        for (w = 0; w < f->ninsns; w++) {
          const Q2Insn *wi = &f->insns[w];
          int k, hit = 0;
          for (k = 0; k < wi->ndef; k++) {
            const Q2Value *dv = &f->values[wi->def[k]];
            if (dv->reg == v->reg) hit = 1;
          }
          for (k = 0; k < wi->nredef; k++)
            if (wi->redef_reg[k] == v->reg) hit = 1;
          if (wi->win_reg >= 0 && wi->win_n > 0 &&
              v->reg >= wi->win_reg && v->reg < wi->win_reg + wi->win_n)
            hit = 1;
          if (hit) {
            char nm[32];
            e_rop_name((uint16_t)wi->rop, nm, sizeof(nm));
            fprintf(stderr, " %d:%s", wi->pc, nm);
          }
        }
        fprintf(stderr, "\n");
      }
      /* The three preconditions for using an `R` record. Together they say:
         the record is complete, this pc is part of it, and the register has
         block the rule for the values it targets, so it was removed. The
         write count is still computed for the `E_DEBUG_CAND` report.
         `E_R_RULE=off` drops the whole pass, so a test can show it does
         something; a count that does not move when it is off is measuring
         nothing.

         1. `thr=1` in the profile header. Enforced by `qjs_aot_prof_regs`,
            which returns nothing for any other build. Only the threaded build
            records `R` completely: its `regvm_t_get_arg2` writes both
            destinations every time it runs, so a register's record contains
            every type its `get_arg2` result ever had. A monolith profile writes
            no `R` for a value, so a record it emits would describe only the
            *other* values a reused register held and would type the result from
            the wrong history.
         2. the `get_arg2` pc has a `P` record, so the instruction ran during
            profiling and its writes are in the `R` record. Without that the
            record could predate the value entirely.
         3. the destination register has an `R` record at all.

         Given those, the result takes its type from its own register's record.
         A reused register whose record is mixed types nothing -- that is the
         correct answer, not a failure: the record says more than one kind of
         value was in that register, and the compiled code must box. */
      {
        int pc_found = 0;
        int32_t dpc = in->pc;
        lo = 0;
        hi = f->nprof - 1;
        while (lo <= hi) {
          int mid = (lo + hi) / 2;
          if (f->prof[mid].pc == dpc) {
            pc_found = 1;
            break;
          }
          if (f->prof[mid].pc < dpc)
            lo = mid + 1;
          else
            hi = mid - 1;
        }
        if (!pc_found) continue;
      }
      lo = 0;
      hi = f->nprof_regs - 1;
      while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (f->prof_regs[mid].reg == v->reg) {
          found = mid;
          break;
        }
        if (f->prof_regs[mid].reg < v->reg)
          lo = mid + 1;
        else
          hi = mid - 1;
      }
      if (found < 0) continue;
      v->has_prof = 1;
      v->ptypes = prof_to_q2(f->prof_regs[found].types);
      v->frac = 0;
      v->negzero = 0;
      v->nmin = 0;
      v->nmax = 0;
      v->prof_src = 1; /* R */
    }
    free(nwrites);
  }

  free(starts);
  free(pc_to_block);
  q2_build_rpo(f);
  return 0;

fail:
  free(starts);
  free(pc_to_block);
  return -1;
}

/* ---- the block order ---------------------------------------------------- */

/* Compute the blocks in DFS reverse postorder, from the entry block, with the
   IR's successor order so the result is deterministic (an unchanged function
   must produce byte-identical output, which is what makes an incremental build
   possible). Unreachable blocks are left out: they are not emitted.

   Why DFS RPO rather than pc order: pc order is NOT a reverse postorder in
   general. A merge block can be laid out before some of its predecessors -- a
   shared `return v0` with out-of-line tails placed after it that jump back --
   so a block has a successor at a lower pc without being in a loop. The
   speculative body needs a real topological order of the forward edges. */
static void q2_build_rpo(Q2Func *f) {
  int nb = f->nblocks, i, sp = 0;
  int *iter = calloc((size_t)(nb > 0 ? nb : 1), sizeof(int));
  int *stack = calloc((size_t)(nb > 0 ? nb : 1), sizeof(int));
  uint8_t *seen = calloc((size_t)(nb > 0 ? nb : 1), 1);

  f->rpo = calloc((size_t)(nb > 0 ? nb : 1), sizeof(int32_t));
  f->rpo_pos = malloc(sizeof(int32_t) * (size_t)(nb > 0 ? nb : 1));
  f->nrpo = 0;
  for (i = 0; i < nb; i++) f->rpo_pos[i] = -1;
  if (!iter || !stack || !seen || !f->rpo || !f->rpo_pos) goto done;

  /* Iterative DFS: a block is appended after all its successors, so the array
     built in visit order, reversed, is a reverse postorder. */
  stack[sp++] = 0;
  seen[0] = 1;
  while (sp > 0) {
    int b = stack[sp - 1];
    const Q2Block *blk = &f->blocks[b];
    if (iter[b] < blk->nsucc) {
      int s = blk->succ_block[iter[b]++];
      if (s >= 0 && s < nb && !seen[s]) {
        seen[s] = 1;
        stack[sp++] = s;
      }
    } else {
      sp--;
      f->rpo[f->nrpo++] = b; /* postorder */
    }
  }
  /* Reverse into postorder -> reverse postorder. */
  for (i = 0; i < f->nrpo / 2; i++) {
    int32_t t = f->rpo[i];
    f->rpo[i] = f->rpo[f->nrpo - 1 - i];
    f->rpo[f->nrpo - 1 - i] = t;
  }
  for (i = 0; i < f->nrpo; i++) f->rpo_pos[f->rpo[i]] = i;

done:
  free(iter);
  free(stack);
  free(seen);
}

void q2_free(Q2Func *f) {
  int i;
  if (!f) return;
  for (i = 0; i < f->nblocks; i++) {
    free(f->blocks[i].preds);
    free(f->blocks[i].param_reg);
    free(f->blocks[i].in_reg);
    free(f->blocks[i].in_val);
    free(f->blocks[i].param_val);
    free(f->blocks[i].edge_arg[0]);
    free(f->blocks[i].edge_arg[1]);
    free(f->blocks[i].edge_arg);
  }
  free(f->blocks);
  free(f->rpo);
  free(f->rpo_pos);
  if (f->insns) {
    int i;
    for (i = 0; i < f->ninsns; i++) {
      free(f->insns[i].win_val);
      free(f->insns[i].redef_reg);
      free(f->insns[i].redef_val);
    }
  }
  free(f->insns);
  free(f->values);
  free(f->live_in);
  free(f->live_out);
  free(f->insn_live);
  free(f->reg_def);
  memset(f, 0, sizeof(*f));
}

/* The ROPs the IR models as more than an opaque instruction. The verifier checks the
   classifier against this, so a modelled op the IR calls a fallback is a build
   failure. */
int q2_lowered(uint16_t rop) {
  switch (rop) {
    case ROP_load_undefined:
    case ROP_load_null:
    case ROP_load_true:
    case ROP_load_false:
    case ROP_load_i8:
    case ROP_load_i8_s:
    case ROP_load_i32:
    case ROP_move:
    case ROP_move_s:
    case ROP_move_xfer:
    case ROP_get_local:
    case ROP_put_local:
    case ROP_put_local_xfer:
    case ROP_get_arg:
    case ROP_get_arg2:
    case ROP_get_var_ref:
    case ROP_load_this:
    case ROP_add:
    case ROP_sub:
    case ROP_mul:
    case ROP_add_imm_s:
    case ROP_sub_imm_s:
    case ROP_binary_imm:
    case ROP_binary_imm_s:
    case ROP_mul_add:
    case ROP_mul_sub:
    case ROP_add_mul:
    case ROP_sub_mul:
    case ROP_get_field:
    case ROP_get_field_own:
    case ROP_get_field_chain:
    case ROP_get_field_chain_own:
    case ROP_get_field2:
    case ROP_get_field2_own:
    case ROP_get_element:
    case ROP_branch_null:
    case ROP_branch_not_null:
    case ROP_branch_nullish:
    case ROP_branch_not_nullish:
    case ROP_branch:
    case ROP_branch_s:
    case ROP_branch_if:
    case ROP_branch_if_not:
    case ROP_branch_cmp_reg_reg:
    case ROP_branch_cmp_reg_reg_s:
    case ROP_put_field:
    case ROP_get_global:
    case ROP_neg:
    case ROP_inc:
    case ROP_inc_branch_lt:
    case ROP_inc_branch_lt_s:
    case ROP_add_imm_self_branch:
    case ROP_add_imm_self_branch_s:
    case ROP_return:
    case ROP_return_undefined:
    case ROP_get_this_field:
    case ROP_get_this_field2:
    case ROP_binary_cpool:
      return 1;
    default:
      return 0;
  }
}

/* The live set immediately after an instruction: what build.c computed and
   stored. Equal to the successor's live-in for a fall-through edge. */
const uint64_t *q2_live_out(const Q2Func *fn, int insn) {
  return fn->insn_live + (size_t)insn * (size_t)fn->lwords;
}

void q2_live_before(const Q2Func *fn, int insn, uint64_t *out) {
  int words = fn->lwords, k;
  const Q2Insn *in = &fn->insns[insn];
  memcpy(out, fn->insn_live + (size_t)insn * words, (size_t)words * 8);
  for (k = 0; k < in->ndef; k++) {
    int r = fn->values[in->def[k]].reg;
    out[r >> 6] &= ~(1ull << (r & 63));
  }
  for (k = 0; k < in->nuse; k++) {
    int r = fn->values[in->use[k]].reg;
    out[r >> 6] |= 1ull << (r & 63);
  }
  for (k = 0; k < in->win_n; k++) {
    int r = in->win_reg + k;
    out[r >> 6] |= 1ull << (r & 63);
  }
}

