/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * The lowering: the IR's control flow drives the emitted C.
 *
 *   - Control flow is the CFG's own. A branch becomes a real `goto` between
 *     blocks and a loop becomes a real loop; there is no per-instruction
 *     dispatch and no `switch (pc)` after an instruction. Fallbacks share one
 *     step exit, and the entry dispatch covers every pc for a deoptimised or
 *     resumed frame and the control transfers the graph does not model.
 *   - The operations the profile is made of are lowered natively, through the
 *     same ABI helpers the interpreter's own fast paths use, on the frame's
 *     registers. Those lowerings are also where guards live: in a sweep build
 *     a guard miss leaves to the interpreter with a deopt taken *before* the
 *     instruction, so the interpreter re-executes it.
 *   - Everything else runs through the engine's step routine, one generic
 *     helper on tagged values.
 *   - `fr` stays the home of every value. Under DRC the register range is the
 *     engine's root range, so a value that must survive a collection has to be
 *     reachable from it; holding values only in C locals arrives with the
 *     allocator, and `fr` going stale is what a frame state is for. The IR's
 *     liveness is exact and verifier-checked for that pass. A guard thus
 *     re-executes rather than reconstructing: it misses before its
 *     instruction, where the frame still holds the interpreter's state.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir.h"
#include "quickjs-aot-gen.h"
#include "quickjs-regvm.h"

/* The driver's id spaces and sweep flag (qjsaot.c): the deopt sites and the
   proto-fold sites are allocated from the unit's global counters, so the
   emitter has to take its ids from the same place the driver does. */
int qjs_aot_sweep_on(void);
int qjs_aot_sweep_site(void);
int qjs_aot_site_new(void);

/* A fresh deopt site, or -1 when not sweeping. */
static int q2_site(void) {
  return qjs_aot_sweep_on() ? qjs_aot_sweep_site() : -1;
}

/* The guard exit: a miss leaves compiled code *before* the instruction and the
   interpreter re-executes it there. The frame already holds the state it
   expects, so nothing is written back. */
static void q2_guard(FILE *f, int off, int sid) {
  if (sid >= 0)
    fprintf(
        f,
        "      if (__builtin_expect(qjs_aot_should_deopt(ctx, %d), 0)) "
        "return qjs_aot_deopt(ctx, f, fr, %d, %d);\n",
        sid, off, sid);
}

static int q2_target(const Q2Insn *in, int arg, int *tgt) {
  if (arg >= in->narg || in->arg[arg].kind != 't') return 0;
  *tgt = in->next + (int)in->arg[arg].val;
  return 1;
}

/* A back edge polls the embedder's interrupt handler, the way every
   interpreted back edge does. The caller opened the arm this lands in. */
static void q2_emit_edge(FILE *f, int off, int tgt) {
  if (tgt <= off)
    fprintf(
        f,
        " if (__builtin_expect(qjs_aot_poll(ctx) < 0, 0)) { pc = %d; "
        "goto exception; }",
        off);
  fprintf(f, " goto pc_%d;", tgt);
}

/* One instruction through the shared step routine: the common case is a return
   of the next pc, everything else leaves through `step_exit`. */
static void q2_emit_fallback(FILE *f, const Q2Insn *in, int exit_pc) {
  fprintf(f, "    ip = qjs_aot_step(code + %d, fr, f, ctx);\n", in->pc);
  fprintf(
      f,
      "    if (__builtin_expect(ip == NULL, 0)) { pc = %d; goto "
      "exception; }\n",
      in->pc);
  if (in->next >= exit_pc)
    fprintf(
        f, "    if (__builtin_expect(ip == code + %d, 1)) goto pc_end;\n",
        in->next);
  else
    fprintf(
        f, "    if (__builtin_expect(ip == code + %d, 1)) goto pc_%d;\n",
        in->next, in->next);
  fprintf(f, "    pc = (uint32_t)(ip - code);\n    goto step_exit;\n");
}

/* SET_REG_P: the destination takes over the value. */
static void q2_set_p(FILE *f, int d, const char *val) {
  fprintf(f, "    qjs_aot_set_reg_p(ctx, fr, %d, %s);\n", d, val);
}

/* 1 lowered it, 0 to fall back, 2 when the fast path is lowered and a miss
   still needs the step. */
static int q2_emit_native(FILE *f, const Q2Insn *in) {
  switch (in->rop) {
    case ROP_load_undefined:
    case ROP_load_null:
    case ROP_load_true:
    case ROP_load_false: {
      int d = (int)in->arg[0].val;
      q2_set_p(
          f, d,
          in->rop == ROP_load_undefined ? "JS_UNDEFINED"
          : in->rop == ROP_load_null    ? "JS_NULL"
          : in->rop == ROP_load_true    ? "JS_TRUE"
                                        : "JS_FALSE");
      return 1;
    }
    case ROP_load_i8:
    case ROP_load_i8_s: {
      int d = (int)in->arg[0].val;
      fprintf(
          f,
          "    qjs_aot_set_reg_p(ctx, fr, %d, JS_MKVAL(JS_TAG_INT, "
          "%d));\n",
          d, (int)(int8_t)in->arg[1].val);
      return 1;
    }
    case ROP_load_i32: {
      int d = (int)in->arg[0].val;
      fprintf(
          f,
          "    qjs_aot_set_reg_p(ctx, fr, %d, JS_MKVAL(JS_TAG_INT, "
          "(int32_t)%d));\n",
          d, (int32_t)in->arg[1].val);
      return 1;
    }
    case ROP_move:
    case ROP_move_s: {
      int d = (int)in->arg[0].val, a = (int)in->arg[1].val;
      if (d != a)
        fprintf(f, "    AOT_SET_B(ctx, fr, %d, AOT_REG(fr, %d));\n", d, a);
      return 1;
    }
    case ROP_move_xfer: {
      int d = (int)in->arg[0].val, a = (int)in->arg[1].val;
      q2_guard(f, in->pc, q2_site());
      fprintf(f, "    qjs_aot_move_xfer(ctx, fr, %d, %d);\n", d, a);
      return 1;
    }

    case ROP_get_local: {
      int d = (int)in->arg[0].val, slot = (int)in->arg[1].val;
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, vb[%d]);\n", d, slot);
      return 1;
    }
    case ROP_put_local:
    case ROP_put_local_xfer: {
      int slot = (int)in->arg[0].val, s = (int)in->arg[1].val;
      fprintf(
          f, "    qjs_aot_put_local(ctx, vb, %d, AOT_REG(fr, %d));\n", slot, s);
      return 1;
    }
    case ROP_get_arg: {
      int d = (int)in->arg[0].val, slot = (int)in->arg[1].val;
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, ab[%d]);\n", d, slot);
      return 1;
    }
    case ROP_get_arg2: {
      int d = (int)in->arg[0].val, s1 = (int)in->arg[1].val;
      int d2 = (int)in->arg[2].val, s2 = (int)in->arg[3].val;
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, ab[%d]);\n", d, s1);
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, ab[%d]);\n", d2, s2);
      return 1;
    }

    case ROP_add:
    case ROP_sub:
    case ROP_mul: {
      int d = (int)in->arg[0].val, a = (int)in->arg[1].val,
          b = (int)in->arg[2].val;
      int sub = in->rop == ROP_add   ? REG_BIN_ADD
                : in->rop == ROP_sub ? REG_BIN_SUB
                                     : REG_BIN_MUL;
      fprintf(f, "    { JSValue _o;\n");
      q2_guard(f, in->pc, q2_site());
      fprintf(
          f,
          "      if (qjs_aot_num_fast(%d, Q2_REG(%d), Q2_REG(%d), &_o)) "
          "{ AOT_SET_B(ctx, fr, %d, _o); goto pc_%d; } }\n",
          sub, a, b, d, in->next);
      return 2;
    }
    case ROP_add_imm_s:
    case ROP_sub_imm_s:
    case ROP_binary_imm:
    case ROP_binary_imm_s: {
      int d = (int)in->arg[0].val;
      int sub = (in->rop == ROP_binary_imm || in->rop == ROP_binary_imm_s)
                    ? (int)in->arg[1].val
                    : (in->rop == ROP_add_imm_s ? REG_BIN_ADD : REG_BIN_SUB);
      int a = (in->rop == ROP_binary_imm || in->rop == ROP_binary_imm_s)
                  ? (int)in->arg[2].val
                  : (int)in->arg[1].val;
      int32_t imm = (in->rop == ROP_binary_imm || in->rop == ROP_binary_imm_s)
                        ? (int32_t)in->arg[3].val
                        : (int32_t)in->arg[2].val;
      fprintf(f, "    { JSValue _o;\n");
      q2_guard(f, in->pc, q2_site());
      fprintf(
          f,
          "      if (qjs_aot_num_fast(%d, Q2_REG(%d), "
          "JS_MKVAL(JS_TAG_INT, %d), &_o)) { AOT_SET_B(ctx, fr, %d, _o); "
          "goto pc_%d; } }\n",
          sub, a, imm, d, in->next);
      return 2;
    }

    /* the fused multiply-add family: mul then add/sub, as the handler does;
       both operands are read before either register is written */
    case ROP_mul_add:
    case ROP_mul_sub:
    case ROP_add_mul:
    case ROP_sub_mul: {
      int dm = (int)in->arg[0].val, x = (int)in->arg[1].val,
          y = (int)in->arg[2].val;
      int da = (int)in->arg[3].val, c = (int)in->arg[4].val;
      int mul_first = (in->rop == ROP_mul_add || in->rop == ROP_mul_sub);
      int sub2 = (in->rop == ROP_mul_add || in->rop == ROP_add_mul)
                     ? REG_BIN_ADD
                     : REG_BIN_SUB;
      int lhs2 = mul_first ? dm : c;
      int rhs2 = mul_first ? c : dm;
      fprintf(f, "    { JSValue _t, _r;\n");
      q2_guard(f, in->pc, q2_site());
      fprintf(
          f,
          "      if (qjs_aot_num_fast(%d, Q2_REG(%d), Q2_REG(%d), &_t)) "
          "{\n",
          REG_BIN_MUL, x, y);
      fprintf(f, "        AOT_SET_B(ctx, fr, %d, _t);\n", dm);
      fprintf(
          f,
          "        if (qjs_aot_num_fast(%d, Q2_REG(%d), Q2_REG(%d), &_r)) "
          "{ AOT_SET_B(ctx, fr, %d, _r); goto pc_%d; } }\n",
          sub2, lhs2, rhs2, da, in->next);
      fprintf(f, "    }\n");
      return 2;
    }

    /* property reads and the chain form: the shape-guarded own read */
    case ROP_get_field:
    case ROP_get_field_own: {
      int d = (int)in->arg[0].val, a = (int)in->arg[1].val;
      int site = (int)in->arg[3].val;
      if ((unsigned)site == REG_IC_NONE) return 0;
      fprintf(f, "    { JSValue _o;\n");
      q2_guard(f, in->pc, q2_site());
      fprintf(
          f,
          "      if (qjs_aot_own_read_hit(f, pic, Q2_REG(%d), %d, &_o)) "
          "{ AOT_SET_B(ctx, fr, %d, _o); goto pc_%d; } }\n",
          a, site, d, in->next);
      return 2;
    }
    case ROP_get_field_chain:
    case ROP_get_field_chain_own: {
      int d1 = (int)in->arg[0].val, r1 = (int)in->arg[1].val;
      int s1 = (int)in->arg[3].val, d2 = (int)in->arg[4].val;
      int s2 = (int)in->arg[7].val;
      if (s1 == REG_IC_NONE || s2 == REG_IC_NONE) return 0;
      fprintf(f, "    { JSValue _v1, _v2;\n");
      q2_guard(f, in->pc, q2_site());
      fprintf(
          f,
          "      if (qjs_aot_own_read_hit(f, pic, Q2_REG(%d), %d, &_v1) && "
          "qjs_aot_own_read_hit(f, pic, _v1, %d, &_v2)) { AOT_SET_B(ctx, "
          "fr, %d, _v1); AOT_SET_B(ctx, fr, %d, _v2); goto pc_%d; } }\n",
          r1, s1, s2, d1, d2, in->next);
      return 2;
    }
    /* get_field2 is the "load a method for a call" form: the folded prototype
       read first (its own site id space), then the own read */
    case ROP_get_field2:
    case ROP_get_field2_own: {
      int d = (int)in->arg[0].val, a = (int)in->arg[1].val;
      int site = (int)in->arg[3].val;
      int sid;
      if ((unsigned)site == REG_IC_NONE) return 0;
      sid = qjs_aot_site_new();
      fprintf(f, "    { JSValue _o;\n");
      q2_guard(f, in->pc, q2_site());
      fprintf(
          f,
          "      if (qjs_aot_proto_fold(ctx, f, pic, %d, Q2_REG(%d), %d, "
          "&_o)) { qjs_aot_set_reg_p(ctx, fr, %d, _o); goto pc_%d; }\n",
          sid, a, site, d, in->next);
      fprintf(
          f,
          "      if (qjs_aot_own_read_hit(f, pic, Q2_REG(%d), %d, &_o)) "
          "{ AOT_SET_B(ctx, fr, %d, _o); goto pc_%d; } }\n",
          a, site, d, in->next);
      return 2;
    }

    /* get_element: the plain-array fast path, which returns an owned value */
    case ROP_get_element: {
      int d = (int)in->arg[0].val, a = (int)in->arg[1].val,
          b = (int)in->arg[2].val;
      fprintf(f, "    { JSValue _o;\n");
      q2_guard(f, in->pc, q2_site());
      fprintf(
          f,
          "      if (qjs_aot_get_element(Q2_REG(%d), Q2_REG(%d), &_o)) "
          "{ qjs_aot_set_reg_p(ctx, fr, %d, _o); goto pc_%d; } }\n",
          a, b, d, in->next);
      return 2;
    }

    /* the tests with no coercion: no guard, no step */
    case ROP_branch_null:
    case ROP_branch_not_null: {
      int a = (int)in->arg[0].val, tgt;
      int want = (in->rop == ROP_branch_null);
      if (!q2_target(in, 1, &tgt)) return 0;
      fprintf(
          f,
          "    if ((JS_VALUE_GET_TAG(Q2_REG(%d)) == JS_TAG_NULL) == %d) "
          "{",
          a, want);
      q2_emit_edge(f, in->pc, tgt);
      fprintf(f, " } else goto pc_%d;\n", in->next);
      return 1;
    }
    case ROP_branch_nullish:
    case ROP_branch_not_nullish: {
      int a = (int)in->arg[0].val, tgt;
      int want = (in->rop == ROP_branch_nullish);
      if (!q2_target(in, 1, &tgt)) return 0;
      fprintf(f, "    if (qjs_aot_nullish(Q2_REG(%d)) == %d) {", a, want);
      q2_emit_edge(f, in->pc, tgt);
      fprintf(f, " } else goto pc_%d;\n", in->next);
      return 1;
    }

    case ROP_branch:
    case ROP_branch_s: {
      int tgt;
      if (!q2_target(in, 0, &tgt)) return 0;
      q2_emit_edge(f, in->pc, tgt);
      fputc('\n', f);
      return 1;
    }
    case ROP_branch_if:
    case ROP_branch_if_not: {
      int tgt, want_true = (in->rop == ROP_branch_if);
      int a = (int)in->arg[0].val;
      if (!q2_target(in, 1, &tgt)) return 0;
      fprintf(f, "    { int _b = qjs_aot_to_bool(ctx, Q2_REG(%d));\n", a);
      fprintf(f, "      if (_b < 0) { pc = %d; goto exception; }\n", in->pc);
      fprintf(f, "      if (_b == %d) {", want_true);
      q2_emit_edge(f, in->pc, tgt);
      fprintf(f, " } else goto pc_%d; }\n", in->next);
      return 1;
    }
    case ROP_branch_cmp_reg_reg:
    case ROP_branch_cmp_reg_reg_s: {
      int s = (int)in->arg[0].val;
      int la = (int)in->arg[1].val, rb = (int)in->arg[2].val;
      int tgt;
      if (!q2_target(in, 3, &tgt)) return 0;
      fprintf(f, "    { int _t;\n");
      fprintf(
          f,
          "      if (qjs_aot_branch_cmp(%d, Q2_REG(%d), Q2_REG(%d), "
          "&_t)) {\n",
          s, la, rb);
      fprintf(f, "        if (_t) {");
      q2_emit_edge(f, in->pc, tgt);
      fprintf(f, " } else goto pc_%d; } }\n", in->next);
      return 2;
    }
    default:
      return 0;
  }
}

/* A fresh entry, a deoptimised or resumed frame and an unwind all arrive with
   a pc, and every instruction has a label. */
static void q2_emit_pc_switch(FILE *f, const Q2Func *fn) {
  int i;
  fprintf(f, "    switch (pc) {\n");
  for (i = 0; i < fn->ninsns; i++)
    fprintf(f, "    case %d: goto pc_%d;\n", fn->insns[i].pc, fn->insns[i].pc);
  fprintf(f, "    case %d: goto pc_end;\n", fn->exit_pc);
  fprintf(f, "    default: __builtin_unreachable();\n    }\n");
}

static void q2_emit_body(FILE *f, const Q2Func *fn, char hex[33]) {
  int i;

  fprintf(
      f,
      "static JSValue aot_f_%s(JSContext *ctx, JSRegRun *f,\n"
      "                                 JSValue *fr, uint32_t entry_pc)\n"
      "{\n",
      hex);
  fprintf(f, "    uint8_t *code = qjs_aot_code(f);\n");
  fprintf(f, "    uint8_t *ip;\n    uint32_t pc = entry_pc;\n");
  fprintf(f, "    JSValue *vb = qjs_aot_var_buf(f);\n");
  fprintf(f, "    JSValue *ab = qjs_aot_arg_buf(f);\n");
  fprintf(f, "    const void *pic = qjs_aot_pic(f);\n");
  fprintf(f, "    (void)code; (void)ip; (void)vb; (void)ab; (void)pic;\n");
  fprintf(f, "#define Q2_REG(off) AOT_REG(fr, off)\n");
  fprintf(f, "    (void)qjs_aot_should_deopt;\n");
  fprintf(f, "    goto dispatch_ip;\n");

  for (i = 0; i < fn->ninsns; i++) {
    const Q2Insn *in = &fn->insns[i];
    int done;
    fprintf(f, "pc_%d:\n", in->pc);
    done = q2_emit_native(f, in);
    if (done != 1) q2_emit_fallback(f, in, fn->exit_pc);
  }
  fprintf(f, "pc_end:\n    return qjs_aot_ret(f);\n");
  fprintf(f, "step_exit:\n");
  fprintf(
      f,
      "    if (__builtin_expect(ip == QJS_AOT_COLD_RET, 0)) return "
      "qjs_aot_ret(f);\n");
  fprintf(
      f,
      "    if (__builtin_expect(ip == QJS_AOT_SUSPEND, 0)) return "
      "qjs_aot_ret(f);\n");
  q2_emit_pc_switch(f, fn);
  fprintf(f, "dispatch_ip:\n");
  q2_emit_pc_switch(f, fn);
  fprintf(f, "exception: {\n");
  fprintf(f, "        uint32_t rp;\n");
  fprintf(f, "        JSValue ex = qjs_aot_unwind(ctx, f, fr, pc, &rp);\n");
  fprintf(
      f, "        if (!JS_IsException(ex)) { pc = rp; goto dispatch_ip; }\n");
  fprintf(f, "        return JS_EXCEPTION;\n    }\n}\n\n");
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
