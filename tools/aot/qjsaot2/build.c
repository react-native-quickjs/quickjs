/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * Builds qjsaot2's IR from one function's register program.
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
#include "quickjs-aot-gen.h"
#include "quickjs-regvm.h"

#define Q2_NONE8 ((int8_t)-1)

static int q2_ri(int off) {
  return off >> 4;
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
      info.op = Q2_OP_FALLBACK;
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
    case ROP_binary:
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
      info.use[0] = 0;
      info.use[1] = 1;
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
    case ROP_bitand: /* "rrr": d, a, b */
    case ROP_bitor:
    case ROP_bitxor:
    case ROP_shl:
    case ROP_sar:
    case ROP_shr:
    case ROP_mod:
    case ROP_pow:
    case ROP_eq:
    case ROP_neq:
    case ROP_strict_eq:
    case ROP_strict_neq:
    case ROP_lt:
    case ROP_lte:
    case ROP_gt:
    case ROP_gte:
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
    case ROP_inc_branch_lt: /* "rrt" */
    case ROP_inc_branch_lt_s:
      info.op = Q2_OP_FALLBACK;
      info.ndef = 0;
      info.nuse = 0; /* conservative: unknown roles */
      info.reads = Q2_E_HEAP;
      info.writes = Q2_E_HEAP | Q2_E_CONTROL;
      return &info;
    case ROP_add_imm_self_branch: /* "rit": d, imm, target; d defined */
    case ROP_add_imm_self_branch_s:
      info.op = Q2_OP_FALLBACK;
      info.ndef = 1;
      info.def[0] = 0;
      info.nuse = 1;
      info.use[0] = 0;
      info.reads = 0;
      info.writes = Q2_E_CONTROL;
      return &info;
    case ROP_local_rmw_imm: /* "buI": bool, slot, imm: no register operand */
    case ROP_local_rmw_branch:
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
      info.ndef = 1;
      info.def[0] = 0;
      info.nuse = 1;
      info.use[0] = 1;
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
      info.window = 3;
      info.reads = 0;
      info.writes = Q2_E_HEAP | Q2_E_SLOT | Q2_E_ELEMENTS | Q2_E_SHAPE |
                    Q2_E_GLOBAL | Q2_E_VARREF | Q2_E_CONTROL;
      return &info;
    case ROP_apply_arguments: /* "rru": d, callee, window */
      info.ndef = 1;
      info.def[0] = 0;
      info.nuse = 1;
      info.use[0] = 1;
      info.reads = 0;
      info.writes = Q2_E_HEAP | Q2_E_CONTROL;
      return &info;
    case ROP_div: /* "rrr": d, a, b */
    case ROP_neg: /* "rr": d, a */
    case ROP_inc: /* "rr" */
      info.ndef = 1;
      info.def[0] = 0;
      info.nuse = (rop == ROP_div) ? 2 : 1;
      info.use[0] = 1;
      info.use[1] = 2;
      info.reads = 0;
      info.writes = Q2_E_CONTROL;
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
      info.nuse = 1;
      info.use[0] = 1;
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
    case ROP_call: /* "rrrb": d, callee, window, argc */
      info.op = Q2_OP_CALL;
      info.ndef = 1;
      info.def[0] = 0;
      info.nuse = 1;
      info.use[0] = 1;
      info.window = 2;
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
      info.window = 3;
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
          int n, si = -1;
          Q2ValueId av;
          for (n = 0; n < pr->nsucc; n++)
            if (pr->succ_block[n] == b) {
              si = n;
              break;
            }
          if (si < 0 || p >= pr->nedge_args[si]) continue;
          av = q2_find(repl, pr->edge_arg[si][p]);
          if (av == pv) continue; /* this edge passes the parameter itself */
          if (common == Q2_NO_VALUE)
            common = av;
          else if (q2_find(repl, common) != av) {
            consistent = 0;
            break;
          }
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
    for (n = 0; n < blk->nsucc; n++)
      for (k = 0; k < blk->nedge_args[n]; k++)
        if (blk->edge_arg[n][k] != Q2_NO_VALUE)
          blk->edge_arg[n][k] = q2_find(repl, blk->edge_arg[n][k]);
  }
  free(repl);
  return 0;
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
    ri = q2_rop_info(in->rop);
    in->op = (int8_t)ri->op;
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
      b->succ_pc[b->nsucc] = last->next;
      b->succ_block[b->nsucc] = pc_to_block[last->next];
      b->nsucc++;
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
            /* unknown roles: the step may write any register live after it */
            memset(cur, 0, (size_t)words * 8);
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
        if (in2->op == Q2_OP_FALLBACK && in2->ndef == 0) {
          /* Unknown roles: the step may write any register live after this
             instruction (not the block), so redefine those and remember which,
             for the lowering to reload from the frame. */
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

  /* 5b. drop the block parameters that are the same value on every edge */
  if (!getenv("Q2_NO_PRUNE") && q2_prune_params(f) < 0) {
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
  free(starts);
  free(pc_to_block);
  return 0;

fail:
  free(starts);
  free(pc_to_block);
  return -1;
}

void q2_free(Q2Func *f) {
  int i;
  if (!f) return;
  for (i = 0; i < f->nblocks; i++) {
    free(f->blocks[i].preds);
    free(f->blocks[i].param_reg);
    free(f->blocks[i].param_val);
    free(f->blocks[i].edge_arg[0]);
    free(f->blocks[i].edge_arg[1]);
    free(f->blocks[i].edge_arg);
  }
  free(f->blocks);
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
