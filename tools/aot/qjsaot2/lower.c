/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * The lowering: the IR's control flow drives the emitted C.
 *
 * A compiled function runs from pc 0 only, and leaving compiled code is
 * one-way: a deopt hands the activation to the interpreter for the rest of its
 * life. That gives the code its shape:
 *
 *   - a label per block start, never a `switch (pc)` and never a label per
 *     instruction;
 *   - a branch is a real `goto`, and the not-taken path falls through;
 *   - every fast path that can miss is a guard, and a miss returns
 *     `qjs_aot_deopt(ctx, f, fr, pc, site)` before the instruction, so the
 *     interpreter re-executes it there;
 *   - an operation the classifier does not know runs through the engine's step
 *     routine.
 *
 * Every SSA value is a C local, and every definition is **also** written to its
 * register's frame slot, with the same setter the interpreter's handler uses
 * (`qjs_aot_set_reg_p` when the handler adopts, `AOT_SET_B` when it borrows).
 * So:
 *
 *   - the invariant: a local and its register's `fr` slot are equal at every
 *     fallback, deopt, call and exception exit;
 *   - `fr` is the GC's root set under DRC: one store per definition, never
 *     reloaded, keeps a live heap pointer reachable;
 *   - a deopt needs no write-back, because the frame is already current: a
 *     guard misses before its instruction, so the interpreter sees the state it
 *     expects;
 *   - what would break it: an SSA value used after its register is redefined.
 *     The IR is built from the register program, which never does that, but a
 *     later pass that reorders or extends live ranges must preserve it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir.h"
#include "quickjs-aot-gen.h"
#include "quickjs-regvm.h"

/* The driver's id spaces and sweep flag (qjsaot.c). */
int qjs_aot_sweep_on(void);
int qjs_aot_site_alloc(void);
int qjs_aot_site_new(void);

/* The ROPs with a native lowering. The verifier checks the classifier against
   this, so a lowered op the IR calls a fallback is a build failure. */
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
      return 1;
    default:
      return 0;
  }
}

/* A fresh deopt site. */
static int q2_site(void) {
  return qjs_aot_site_alloc();
}

/* The guard test, emitted only in a sweep build: a normal unit has no sweep and
   would pay a call per guard. */
static void q2_guard(FILE *f, int off, int sid) {
  if (qjs_aot_sweep_on())
    fprintf(
        f,
        "      if (__builtin_expect(qjs_aot_should_deopt(ctx, %d), 0)) "
        "return qjs_aot_deopt(ctx, f, fr, %d, %d);\n",
        sid, off, sid);
}

/* The miss of a guard: leave to the interpreter at this pc. */
static void q2_miss(FILE *f, int off, int sid) {
  fprintf(f, "    return qjs_aot_deopt(ctx, f, fr, %d, %d);\n", off, sid);
}

/* The value operand `k` holds at this instruction, as a C expression. */
static const char *q2_op(const Q2Insn *in, int k, char *buf, size_t n) {
  if (k >= 0 && k < in->narg && in->arg[k].kind == 'r' &&
      in->argval[k] != Q2_NO_VALUE)
    snprintf(buf, n, "v%u", in->argval[k]);
  else if (k >= 0 && k < in->narg && in->arg[k].kind == 'r')
    snprintf(buf, n, "AOT_REG(fr, %d)", (int)in->arg[k].val);
  else
    snprintf(buf, n, "JS_UNDEFINED");
  return buf;
}

/* A definition: the local, then the frame, with the handler's ownership. */
static void q2_def(
    FILE *f, const Q2Func *fn, const Q2Insn *in, int defi, const char *expr,
    int adopt) {
  Q2ValueId v = in->def[defi];
  int off = fn->values[v].reg * 16;
  fprintf(f, "    v%u = %s;\n", v, expr);
  if (adopt)
    fprintf(f, "    qjs_aot_set_reg_p(ctx, fr, %d, v%u);\n", off, v);
  else
    fprintf(f, "    AOT_SET_B(ctx, fr, %d, v%u);\n", off, v);
}

/* Whether a block's last instruction ends with its own transfer. */
static int q2_block_ends_with_goto(const Q2Func *fn, int b) {
  const Q2Block *blk = &fn->blocks[b];
  const Q2Insn *last;
  if (blk->ninsns == 0) return 0;
  last = &fn->insns[blk->first_insn + blk->ninsns - 1];
  return last->op == Q2_OP_BRANCH || last->op == Q2_OP_BRANCH_CMP ||
         last->op == Q2_OP_RETURN || last->op == Q2_OP_RETURN_UNDEF ||
         last->op == Q2_OP_FALLBACK || !q2_lowered(last->rop);
}

/* One edge: assign the successor's parameters through the temps (a parallel
   copy), then jump. `si` is the successor's index in `b`. */
static void q2_emit_edge(FILE *f, const Q2Func *fn, int b, int si) {
  const Q2Block *blk = &fn->blocks[b];
  const Q2Block *s = blk->succ_block[si] < fn->nblocks
                         ? &fn->blocks[blk->succ_block[si]]
                         : NULL;
  int k, np;
  if (!s) {
    fprintf(f, " return qjs_aot_ret(f);");
    return;
  }
  if (s->start_pc <= blk->start_pc)
    fprintf(
        f,
        " if (__builtin_expect(qjs_aot_poll(ctx) < 0, 0)) { pc = %d; "
        "goto exception; }",
        blk->start_pc);
  /* The parameters of a successor that was not an edge when the arguments were
     built (the exit, or a dead target) have no arguments: read them from the
     frame, which is the same value. */
  if (si >= 2 || !blk->edge_arg[si]) {
    for (k = 0; k < s->nparams; k++)
      fprintf(
          f, " v%u = AOT_REG(fr, %d);", s->param_val[k], s->param_reg[k] * 16);
    fprintf(f, " goto b_%d;", s->start_pc);
    return;
  }
  np = s->nparams;
  if (blk->nedge_args[si] < np) np = blk->nedge_args[si];
  for (k = 0; k < np; k++) fprintf(f, " e%d = v%u;", k, blk->edge_arg[si][k]);
  for (k = 0; k < np; k++) fprintf(f, " v%u = e%d;", s->param_val[k], k);
  fprintf(f, " goto b_%d;", s->start_pc);
}

static int q2_target(const Q2Insn *in, int arg, int *tgt) {
  if (arg >= in->narg || in->arg[arg].kind != 't') return 0;
  *tgt = in->next + (int)in->arg[arg].val;
  return 1;
}

/* One instruction through the shared step routine. The frame is current, so
   the step sees the state it expects; afterwards the locals of whatever it may
   have written are reloaded from the frame. */
static void q2_emit_fallback(
    FILE *f, const Q2Func *fn, int blk_index, const Q2Insn *in, int exit_pc) {
  int i;
  fprintf(f, "    ip = qjs_aot_step(code + %d, fr, f, ctx);\n", in->pc);
  fprintf(
      f,
      "    if (__builtin_expect(ip == NULL, 0)) { pc = %d; goto "
      "exception; }\n",
      in->pc);
  fprintf(
      f,
      "    if (__builtin_expect(ip == QJS_AOT_COLD_RET, 0)) return "
      "qjs_aot_ret(f);\n");
  fprintf(
      f,
      "    if (__builtin_expect(ip == QJS_AOT_SUSPEND, 0)) return "
      "qjs_aot_ret(f);\n");
  /* An unknown-role fallback may write any register live after it; a known-role
     one writes only its own destinations. */
  if (in->nredef > 0) {
    for (i = 0; i < in->nredef; i++)
      fprintf(
          f, "    v%u = AOT_REG(fr, %d);\n", in->redef_val[i],
          in->redef_reg[i] * 16);
  } else {
    for (i = 0; i < in->ndef; i++) {
      Q2ValueId v = in->def[i];
      fprintf(f, "    v%u = AOT_REG(fr, %d);\n", v, fn->values[v].reg * 16);
    }
  }
  if (fn->blocks[blk_index].nsucc == 2) {
    int32_t tgt = fn->blocks[blk_index].succ_pc[0];
    fprintf(f, "    if (__builtin_expect(ip == code + %d, 1)) {", tgt);
    q2_emit_edge(f, fn, blk_index, 0);
    fprintf(f, " }\n");
    fprintf(f, "    if (__builtin_expect(ip == code + %d, 1)) {", in->next);
    q2_emit_edge(f, fn, blk_index, 1);
    fprintf(f, " }\n");
  } else if (in->next >= exit_pc) {
    fprintf(
        f,
        "    if (__builtin_expect(ip == code + %d, 1)) return "
        "qjs_aot_ret(f);\n",
        in->next);
  } else {
    fprintf(f, "    if (__builtin_expect(ip == code + %d, 1)) {", in->next);
    q2_emit_edge(f, fn, blk_index, 0);
    fprintf(f, " }\n");
  }
  fprintf(
      f,
      "    return qjs_aot_deopt(ctx, f, fr, (uint32_t)(ip - code), "
      "QJS_AOT_NO_SITE);\n");
}

/* 1 lowered it fully, 2 lowered its fast path and deopts on a miss, 0 to fall
   back to the step routine. */
static int q2_emit_native(
    FILE *f, const Q2Func *fn, int cur_block, const Q2Insn *in) {
  char a[32], b[32];
  switch (in->rop) {
    case ROP_load_undefined:
    case ROP_load_null:
    case ROP_load_true:
    case ROP_load_false:
      q2_def(
          f, fn, in, 0,
          in->rop == ROP_load_undefined ? "JS_UNDEFINED"
          : in->rop == ROP_load_null    ? "JS_NULL"
          : in->rop == ROP_load_true    ? "JS_TRUE"
                                        : "JS_FALSE",
          1);
      return 1;
    case ROP_load_i8:
    case ROP_load_i8_s: {
      char e[64];
      snprintf(
          e, sizeof(e), "JS_MKVAL(JS_TAG_INT, %d)",
          (int)(int8_t)in->arg[1].val);
      q2_def(f, fn, in, 0, e, 1);
      return 1;
    }
    case ROP_load_i32: {
      char e[64];
      snprintf(
          e, sizeof(e), "JS_MKVAL(JS_TAG_INT, (int32_t)%d)",
          (int32_t)in->arg[1].val);
      q2_def(f, fn, in, 0, e, 1);
      return 1;
    }
    case ROP_move:
    case ROP_move_s: {
      int d = (int)in->arg[0].val, s = (int)in->arg[1].val;
      if (d != s) q2_def(f, fn, in, 0, q2_op(in, 1, a, sizeof(a)), 0);
      return 1;
    }
    case ROP_move_xfer: {
      int off = fn->values[in->def[0]].reg * 16;
      q2_guard(f, in->pc, q2_site());
      fprintf(f, "    v%u = %s;\n", in->def[0], q2_op(in, 1, a, sizeof(a)));
      fprintf(
          f, "    qjs_aot_move_xfer(ctx, fr, %d, %d);\n", (int)in->arg[0].val,
          (int)in->arg[1].val);
      (void)off;
      return 1;
    }
    case ROP_get_local: {
      char e[64];
      snprintf(e, sizeof(e), "vb[%d]", (int)in->arg[1].val);
      q2_def(f, fn, in, 0, e, 0);
      return 1;
    }
    case ROP_put_local:
    case ROP_put_local_xfer:
      fprintf(
          f, "    qjs_aot_put_local(ctx, vb, %d, %s);\n", (int)in->arg[0].val,
          q2_op(in, 1, a, sizeof(a)));
      return 1;
    case ROP_get_arg: {
      char e[64];
      snprintf(e, sizeof(e), "ab[%d]", (int)in->arg[1].val);
      q2_def(f, fn, in, 0, e, 0);
      return 1;
    }
    case ROP_get_arg2: {
      char e[64];
      snprintf(e, sizeof(e), "ab[%d]", (int)in->arg[1].val);
      q2_def(f, fn, in, 0, e, 0);
      snprintf(e, sizeof(e), "ab[%d]", (int)in->arg[3].val);
      q2_def(f, fn, in, 1, e, 0);
      return 1;
    }
    case ROP_get_var_ref: {
      char e[64];
      snprintf(e, sizeof(e), "qjs_aot_get_var_ref(f, %d)", (int)in->arg[1].val);
      q2_def(f, fn, in, 0, e, 0);
      return 1;
    }
    case ROP_load_this: {
      int off = fn->values[in->def[0]].reg * 16;
      fprintf(
          f,
          "    { JSValue _o; int _r = qjs_aot_load_this(ctx, f, &_o);\n"
          "      if (_r < 0) { pc = %d; goto exception; }\n"
          "      if (_r) qjs_aot_set_reg_p(ctx, fr, %d, _o);\n"
          "      else AOT_SET_B(ctx, fr, %d, _o);\n"
          "      v%u = _o; }\n",
          in->pc, off, off, in->def[0]);
      return 1;
    }

    case ROP_add:
    case ROP_sub:
    case ROP_mul: {
      int sub = in->rop == ROP_add   ? REG_BIN_ADD
                : in->rop == ROP_sub ? REG_BIN_SUB
                                     : REG_BIN_MUL;
      int sid = q2_site();
      int off = fn->values[in->def[0]].reg * 16;
      q2_guard(f, in->pc, sid);
      fprintf(
          f,
          "    if (__builtin_expect(!qjs_aot_num_fast(%d, %s, %s, &v%u), "
          "0))\n",
          sub, q2_op(in, 1, a, sizeof(a)), q2_op(in, 2, b, sizeof(b)),
          in->def[0]);
      q2_miss(f, in->pc, sid);
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, v%u);\n", off, in->def[0]);
      return 1;
    }
    case ROP_add_imm_s:
    case ROP_sub_imm_s:
    case ROP_binary_imm:
    case ROP_binary_imm_s: {
      int is_imm = (in->rop == ROP_binary_imm || in->rop == ROP_binary_imm_s);
      int sub = is_imm ? (int)in->arg[1].val
                       : (in->rop == ROP_add_imm_s ? REG_BIN_ADD : REG_BIN_SUB);
      int ai = is_imm ? 2 : 1;
      int32_t imm = is_imm ? (int32_t)in->arg[3].val : (int32_t)in->arg[2].val;
      int sid = q2_site();
      int off = fn->values[in->def[0]].reg * 16;
      q2_guard(f, in->pc, sid);
      fprintf(
          f,
          "    if (__builtin_expect(!qjs_aot_num_fast(%d, %s, "
          "JS_MKVAL(JS_TAG_INT, %d), &v%u), 0))\n",
          sub, q2_op(in, ai, a, sizeof(a)), imm, in->def[0]);
      q2_miss(f, in->pc, sid);
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, v%u);\n", off, in->def[0]);
      return 1;
    }
    case ROP_mul_add:
    case ROP_mul_sub:
    case ROP_add_mul:
    case ROP_sub_mul: {
      int dm_off = fn->values[in->def[0]].reg * 16;
      int da_off = fn->values[in->def[1]].reg * 16;
      int mul_first = (in->rop == ROP_mul_add || in->rop == ROP_mul_sub);
      int sub2 = (in->rop == ROP_mul_add || in->rop == ROP_add_mul)
                     ? REG_BIN_ADD
                     : REG_BIN_SUB;
      int sid = q2_site();
      q2_guard(f, in->pc, sid);
      fprintf(
          f,
          "    if (__builtin_expect(!qjs_aot_num_fast(%d, %s, %s, &v%u), "
          "0))\n",
          REG_BIN_MUL, q2_op(in, 1, a, sizeof(a)), q2_op(in, 2, b, sizeof(b)),
          in->def[0]);
      q2_miss(f, in->pc, sid);
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, v%u);\n", dm_off, in->def[0]);
      q2_guard(f, in->pc, q2_site());
      {
        char l[32], r[32];
        if (mul_first) {
          snprintf(l, sizeof(l), "v%u", in->def[0]);
          q2_op(in, 4, r, sizeof(r));
        } else {
          q2_op(in, 4, l, sizeof(l));
          snprintf(r, sizeof(r), "v%u", in->def[0]);
        }
        fprintf(
            f,
            "      if (__builtin_expect(!qjs_aot_num_fast(%d, %s, %s, "
            "&v%u), 0))\n",
            sub2, l, r, in->def[1]);
      }
      q2_miss(f, in->pc, sid);
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, v%u);\n", da_off, in->def[1]);
      return 1;
    }

    case ROP_get_field:
    case ROP_get_field_own: {
      int site = (int)in->arg[3].val;
      int sid, off;
      if ((unsigned)site == REG_IC_NONE) return 0;
      sid = q2_site();
      off = fn->values[in->def[0]].reg * 16;
      q2_guard(f, in->pc, sid);
      fprintf(
          f,
          "    if (__builtin_expect(!qjs_aot_own_read_hit(f, pic, %s, %d, "
          "&v%u), 0))\n",
          q2_op(in, 1, a, sizeof(a)), site, in->def[0]);
      q2_miss(f, in->pc, sid);
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, v%u);\n", off, in->def[0]);
      return 1;
    }
    case ROP_get_field_chain:
    case ROP_get_field_chain_own: {
      int s1 = (int)in->arg[3].val, s2 = (int)in->arg[7].val;
      int sid, off0, off1;
      if (s1 == REG_IC_NONE || s2 == REG_IC_NONE) return 0;
      sid = q2_site();
      off0 = fn->values[in->def[0]].reg * 16;
      off1 = fn->values[in->def[1]].reg * 16;
      q2_guard(f, in->pc, sid);
      fprintf(
          f,
          "    if (__builtin_expect(!(qjs_aot_own_read_hit(f, pic, %s, %d, "
          "&v%u) && qjs_aot_own_read_hit(f, pic, v%u, %d, &v%u)), 0))\n",
          q2_op(in, 1, a, sizeof(a)), s1, in->def[0], in->def[0], s2,
          in->def[1]);
      q2_miss(f, in->pc, sid);
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, v%u);\n", off0, in->def[0]);
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, v%u);\n", off1, in->def[1]);
      return 1;
    }
    case ROP_get_field2:
    case ROP_get_field2_own: {
      int site = (int)in->arg[3].val;
      int fsid, sid, off;
      if ((unsigned)site == REG_IC_NONE) return 0;
      fsid = qjs_aot_site_new();
      sid = q2_site();
      off = fn->values[in->def[0]].reg * 16;
      q2_guard(f, in->pc, sid);
      fprintf(
          f,
          "    if (qjs_aot_proto_fold(ctx, f, pic, %d, %s, %d, &v%u)) {\n"
          "      qjs_aot_set_reg_p(ctx, fr, %d, v%u);\n"
          "    } else if (__builtin_expect(!qjs_aot_own_read_hit(f, pic, "
          "%s, %d, &v%u), 0)) {\n",
          fsid, q2_op(in, 1, a, sizeof(a)), site, in->def[0], off, in->def[0],
          q2_op(in, 1, b, sizeof(b)), site, in->def[0]);
      q2_miss(f, in->pc, sid);
      fprintf(
          f, "    } else {\n      AOT_SET_B(ctx, fr, %d, v%u);\n    }\n", off,
          in->def[0]);
      return 1;
    }
    case ROP_get_element: {
      int sid = q2_site();
      int off = fn->values[in->def[0]].reg * 16;
      q2_guard(f, in->pc, sid);
      fprintf(
          f,
          "    if (__builtin_expect(!qjs_aot_get_element(%s, %s, &v%u), "
          "0))\n",
          q2_op(in, 1, a, sizeof(a)), q2_op(in, 2, b, sizeof(b)), in->def[0]);
      q2_miss(f, in->pc, sid);
      fprintf(f, "    qjs_aot_set_reg_p(ctx, fr, %d, v%u);\n", off, in->def[0]);
      return 1;
    }

    case ROP_branch_null:
    case ROP_branch_not_null: {
      int tgt, want = (in->rop == ROP_branch_null);
      if (!q2_target(in, 1, &tgt)) return 0;
      fprintf(
          f, "    if ((JS_VALUE_GET_TAG(%s) == JS_TAG_NULL) == %d) {",
          q2_op(in, 0, a, sizeof(a)), want);
      q2_emit_edge(f, fn, cur_block, 0);
      fprintf(f, " }\n");
      return 1;
    }
    case ROP_branch_nullish:
    case ROP_branch_not_nullish: {
      int tgt, want = (in->rop == ROP_branch_nullish);
      if (!q2_target(in, 1, &tgt)) return 0;
      fprintf(
          f, "    if (qjs_aot_nullish(%s) == %d) {", q2_op(in, 0, a, sizeof(a)),
          want);
      q2_emit_edge(f, fn, cur_block, 0);
      fprintf(f, " }\n");
      return 1;
    }
    case ROP_branch:
    case ROP_branch_s: {
      int tgt;
      if (!q2_target(in, 0, &tgt)) return 0;
      q2_emit_edge(f, fn, cur_block, 0);
      fputc('\n', f);
      return 1;
    }
    case ROP_branch_if:
    case ROP_branch_if_not: {
      int tgt, want = (in->rop == ROP_branch_if);
      if (!q2_target(in, 1, &tgt)) return 0;
      fprintf(
          f, "    { int _b = qjs_aot_to_bool(ctx, %s);\n",
          q2_op(in, 0, a, sizeof(a)));
      fprintf(f, "      if (_b < 0) { pc = %d; goto exception; }\n", in->pc);
      fprintf(f, "      if (_b == %d) {", want);
      q2_emit_edge(f, fn, cur_block, 0);
      fprintf(f, " } }\n");
      return 1;
    }
    case ROP_branch_cmp_reg_reg:
    case ROP_branch_cmp_reg_reg_s: {
      int s = (int)in->arg[0].val;
      int tgt, sid;
      if (!q2_target(in, 3, &tgt)) return 0;
      sid = q2_site();
      q2_guard(f, in->pc, sid);
      fprintf(f, "    { int _t = 0;\n");
      fprintf(
          f,
          "      if (__builtin_expect(!qjs_aot_branch_cmp(%d, %s, %s, "
          "&_t), 0))\n",
          s, q2_op(in, 1, a, sizeof(a)), q2_op(in, 2, b, sizeof(b)));
      q2_miss(f, in->pc, sid);
      fprintf(f, "      if (_t) {");
      q2_emit_edge(f, fn, cur_block, 0);
      fprintf(f, " } }\n");
      return 1;
    }
    default:
      return 0;
  }
}

static void q2_emit_body(FILE *f, const Q2Func *fn, char hex[33]) {
  int b, i, j;

  fprintf(
      f,
      "static JSValue aot_f_%s(JSContext *ctx, JSRegRun *f,\n"
      "                                 JSValue *fr, uint32_t entry_pc)\n"
      "{\n",
      hex);
  fprintf(f, "    uint8_t *code = qjs_aot_code(f);\n");
  fprintf(f, "    uint8_t *ip;\n    uint32_t pc = 0;\n");
  fprintf(f, "    JSValue *vb = qjs_aot_var_buf(f);\n");
  fprintf(f, "    JSValue *ab = qjs_aot_arg_buf(f);\n");
  fprintf(f, "    const void *pic = qjs_aot_pic(f);\n");
  fprintf(f, "    (void)code; (void)ip; (void)vb; (void)ab; (void)pic;\n");
  fprintf(f, "#define Q2_REG(off) AOT_REG(fr, off)\n");
  /* Only pc 0 runs compiled; anything else is the interpreter's, from that pc
     on. */
  fprintf(
      f,
      "    if (__builtin_expect(entry_pc != 0, 0)) return "
      "qjs_aot_deopt(ctx, f, fr, entry_pc, QJS_AOT_NO_SITE);\n");

  /* Every SSA value is a C local, declared once so a goto never crosses a
     declaration. */
  for (i = 0; i < fn->nvalues; i++)
    fprintf(f, "    JSValue v%d = JS_UNDEFINED;\n", i);
  {
    int maxp = 0;
    for (b = 0; b < fn->nblocks; b++)
      if (fn->blocks[b].nparams > maxp) maxp = fn->blocks[b].nparams;
    for (i = 0; i < maxp; i++) fprintf(f, "    JSValue e%d;\n", i);
  }

  /* The entry block reads its parameters from the frame. */
  for (j = 0; j < fn->blocks[0].nparams; j++)
    fprintf(
        f, "    v%u = AOT_REG(fr, %d);\n", fn->blocks[0].param_val[j],
        fn->blocks[0].param_reg[j] * 16);

  for (b = 0; b < fn->nblocks; b++) {
    const Q2Block *blk = &fn->blocks[b];
    fprintf(f, "b_%d:\n", blk->start_pc);
    for (j = 0; j < blk->ninsns; j++) {
      const Q2Insn *in = &fn->insns[blk->first_insn + j];
      if (q2_emit_native(f, fn, b, in) == 0)
        q2_emit_fallback(f, fn, b, in, fn->exit_pc);
    }
    /* The fall-through edge. A block with one successor that did not transfer
       falls into it; a conditional branch already emitted its taken arm and
       still has to assign the not-taken successor's parameters. */
    if (blk->nsucc == 2) {
      fprintf(f, "    ");
      q2_emit_edge(f, fn, b, 1);
      fputc('\n', f);
    } else if (blk->nsucc == 1 && !q2_block_ends_with_goto(fn, b)) {
      fprintf(f, "    ");
      q2_emit_edge(f, fn, b, 0);
      fputc('\n', f);
    }
  }
  fprintf(f, "    return qjs_aot_ret(f);\n");
  fprintf(f, "exception: {\n");
  fprintf(f, "        uint32_t rp = 0;\n");
  fprintf(f, "        JSValue ex = qjs_aot_unwind(ctx, f, fr, pc, &rp);\n");
  fprintf(f, "        if (JS_IsException(ex)) return JS_EXCEPTION;\n");
  fprintf(
      f,
      "        return qjs_aot_deopt(ctx, f, fr, rp, QJS_AOT_NO_SITE);\n"
      "    }\n}\n\n");
}

int qjsaot2_emit_body(FILE *f, const JSAotGenFunc *g, char hex[33]) {
  Q2Func fn;
  char msg[256];
  int rc;

  if (q2_build(g, &fn) < 0) return -1;
  if (q2_verify(&fn, msg, sizeof(msg)) < 0) {
    fprintf(
        stderr, "qjsaot2: %s: the IR failed to verify: %s\n",
        g->name[0] ? g->name : "?", msg);
    q2_free(&fn);
    return -1;
  }
  q2_emit_body(f, &fn, hex);
  rc = fn.ninsns;
  q2_free(&fn);
  return rc;
}
