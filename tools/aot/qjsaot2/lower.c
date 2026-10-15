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
int qjs_aot_site_alloc(void);
int qjs_aot_site_new(void);

/* A fresh deopt site, or -1 when not sweeping. */
static int q2_site(void) {
  return qjs_aot_site_alloc();
}

/* The guard exit: a miss leaves compiled code *before* the instruction and the
   interpreter re-executes it there. The frame already holds the state it
   expects, so nothing is written back. */
/* The miss of a guard: leave to the interpreter at this pc. */
static void q2_miss(FILE *f, int off, int sid) {
  fprintf(f, "        return qjs_aot_deopt(ctx, f, fr, %d, %d);\n", off, sid);
}

static void q2_guard(FILE *f, int off, int sid) {
  /* The sweep test exists only in a deopt-everywhere build: a normal unit has
     no sweep and would pay a call per guard. */
  if (qjs_aot_sweep_on())
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
  fprintf(f, " goto b_%d;", tgt);
}

/* One instruction through the shared step routine: the common case is a return
   of the next pc, everything else leaves through `step_exit`. */
static void q2_emit_fallback(FILE *f, const Q2Insn *in, int exit_pc) {
  fprintf(f, "    ip = qjs_aot_step(code + %d, fr, f, ctx);\n", in->pc);
  fprintf(f, "    if (__builtin_expect(ip == NULL, 0)) goto exception;\n");
  if (in->next >= exit_pc)
    fprintf(
        f,
        "    if (__builtin_expect(ip == code + %d, 1)) return "
        "qjs_aot_ret(f);\n",
        in->next);
  else
    fprintf(
        f, "    if (__builtin_expect(ip == code + %d, 1)) goto b_%d;\n",
        in->next, in->next);
  fprintf(
      f,
      "    if (__builtin_expect(ip == QJS_AOT_COLD_RET, 0)) return "
      "qjs_aot_ret(f);\n");
  fprintf(
      f,
      "    if (__builtin_expect(ip == QJS_AOT_SUSPEND, 0)) return "
      "qjs_aot_ret(f);\n");
  fprintf(
      f,
      "    return qjs_aot_deopt(ctx, f, fr, (uint32_t)(ip - code), "
      "QJS_AOT_NO_SITE);\n");
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
      int sid = q2_site();
      q2_guard(f, in->pc, sid);
      fprintf(f, "    { JSValue _o%d;\n", in->pc);
      fprintf(
          f,
          "      if (qjs_aot_num_fast(%d, Q2_REG(%d), Q2_REG(%d), &_o%d)) "
          "{ AOT_SET_B(ctx, fr, %d, _o%d); goto b_%d; }\n",
          sub, a, b, in->pc, d, in->pc, in->next);
      fprintf(f, "      else {\n");
      q2_miss(f, in->pc, sid);
      fprintf(f, "      } }\n");
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
      int sid = q2_site();
      q2_guard(f, in->pc, sid);
      fprintf(f, "    { JSValue _o%d;\n", in->pc);
      fprintf(
          f,
          "      if (qjs_aot_num_fast(%d, Q2_REG(%d), "
          "JS_MKVAL(JS_TAG_INT, %d), &_o%d)) { AOT_SET_B(ctx, fr, %d, _o%d); "
          "goto b_%d; }\n",
          sub, a, imm, in->pc, d, in->pc, in->next);
      fprintf(f, "      else {\n");
      q2_miss(f, in->pc, sid);
      fprintf(f, "      } }\n");
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
      int sid = q2_site();
      q2_guard(f, in->pc, sid);
      /* The second operation reads `dm` as the handler does, so the first
         store happens between the two predicate calls. */
      fprintf(f, "    { JSValue _t%d, _r%d;\n", in->pc, in->pc);
      fprintf(
          f,
          "      if (qjs_aot_num_fast(%d, Q2_REG(%d), Q2_REG(%d), &_t%d)) "
          "{ AOT_SET_B(ctx, fr, %d, _t%d);\n",
          REG_BIN_MUL, x, y, in->pc, dm, in->pc);
      fprintf(
          f,
          "        if (qjs_aot_num_fast(%d, Q2_REG(%d), Q2_REG(%d), &_r%d)) "
          "{ AOT_SET_B(ctx, fr, %d, _r%d); goto b_%d; } }\n",
          sub2, lhs2, rhs2, in->pc, da, in->pc, in->next);
      fprintf(f, "      else {\n");
      q2_miss(f, in->pc, sid);
      fprintf(f, "      } }\n");
      return 2;
    }

    /* property reads and the chain form: the shape-guarded own read */
    case ROP_get_field:
    case ROP_get_field_own: {
      int d = (int)in->arg[0].val, a = (int)in->arg[1].val;
      int site = (int)in->arg[3].val;
      if ((unsigned)site == REG_IC_NONE) return 0;
      int sid = q2_site();
      q2_guard(f, in->pc, sid);
      fprintf(f, "    { JSValue _o%d;\n", in->pc);
      fprintf(
          f,
          "      if (qjs_aot_own_read_hit(f, pic, Q2_REG(%d), %d, &_o%d)) "
          "{ AOT_SET_B(ctx, fr, %d, _o%d); goto b_%d; }\n",
          a, site, in->pc, d, in->pc, in->next);
      fprintf(f, "      else {\n");
      q2_miss(f, in->pc, sid);
      fprintf(f, "      } }\n");
      return 2;
    }
    case ROP_get_field_chain:
    case ROP_get_field_chain_own: {
      int d1 = (int)in->arg[0].val, r1 = (int)in->arg[1].val;
      int s1 = (int)in->arg[3].val, d2 = (int)in->arg[4].val;
      int s2 = (int)in->arg[7].val;
      if (s1 == REG_IC_NONE || s2 == REG_IC_NONE) return 0;
      int sid = q2_site();
      q2_guard(f, in->pc, sid);
      fprintf(f, "    { JSValue _v%d, _w%d;\n", in->pc, in->pc);
      fprintf(
          f,
          "      if (qjs_aot_own_read_hit(f, pic, Q2_REG(%d), %d, &_v%d) && "
          "qjs_aot_own_read_hit(f, pic, _v%d, %d, &_w%d)) { AOT_SET_B(ctx, "
          "fr, %d, _v%d); AOT_SET_B(ctx, fr, %d, _w%d); goto b_%d; }\n",
          r1, s1, in->pc, in->pc, s2, in->pc, d1, in->pc, d2, in->pc, in->next);
      fprintf(f, "      else {\n");
      q2_miss(f, in->pc, sid);
      fprintf(f, "      } }\n");
      return 2;
    }
    /* get_field2 is the "load a method for a call" form: the folded prototype
       read first (its own site id space), then the own read */
    case ROP_get_field2:
    case ROP_get_field2_own: {
      int d = (int)in->arg[0].val, a = (int)in->arg[1].val;
      int site = (int)in->arg[3].val;
      int fsid, sid;
      if ((unsigned)site == REG_IC_NONE) return 0;
      fsid = qjs_aot_site_new();
      sid = q2_site();
      q2_guard(f, in->pc, sid);
      fprintf(f, "    { JSValue _o%d;\n", in->pc);
      fprintf(
          f,
          "      if (qjs_aot_proto_fold(ctx, f, pic, %d, Q2_REG(%d), %d, "
          "&_o%d)) { qjs_aot_set_reg_p(ctx, fr, %d, _o%d); goto b_%d; }\n",
          fsid, a, site, in->pc, d, in->pc, in->next);
      fprintf(
          f,
          "      if (qjs_aot_own_read_hit(f, pic, Q2_REG(%d), %d, &_o%d)) "
          "{ AOT_SET_B(ctx, fr, %d, _o%d); goto b_%d; }\n",
          a, site, in->pc, d, in->pc, in->next);
      fprintf(f, "      else {\n");
      q2_miss(f, in->pc, sid);
      fprintf(f, "      } }\n");
      return 2;
    }

    /* get_element: the plain-array fast path, which returns an owned value */
    case ROP_get_element: {
      int d = (int)in->arg[0].val, a = (int)in->arg[1].val,
          b = (int)in->arg[2].val;
      int sid = q2_site();
      q2_guard(f, in->pc, sid);
      fprintf(f, "    { JSValue _o%d;\n", in->pc);
      fprintf(
          f,
          "      if (qjs_aot_get_element(Q2_REG(%d), Q2_REG(%d), &_o%d)) "
          "{ qjs_aot_set_reg_p(ctx, fr, %d, _o%d); goto b_%d; }\n",
          a, b, in->pc, d, in->pc, in->next);
      fprintf(f, "      else {\n");
      q2_miss(f, in->pc, sid);
      fprintf(f, "      } }\n");
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
      fprintf(f, " } else goto b_%d;\n", in->next);
      return 1;
    }
    case ROP_branch_nullish:
    case ROP_branch_not_nullish: {
      int a = (int)in->arg[0].val, tgt;
      int want = (in->rop == ROP_branch_nullish);
      if (!q2_target(in, 1, &tgt)) return 0;
      fprintf(f, "    if (qjs_aot_nullish(Q2_REG(%d)) == %d) {", a, want);
      q2_emit_edge(f, in->pc, tgt);
      fprintf(f, " } else goto b_%d;\n", in->next);
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
      fprintf(f, " } else goto b_%d; }\n", in->next);
      return 1;
    }
    case ROP_branch_cmp_reg_reg:
    case ROP_branch_cmp_reg_reg_s: {
      int s = (int)in->arg[0].val;
      int la = (int)in->arg[1].val, rb = (int)in->arg[2].val;
      int tgt, sid;
      if (!q2_target(in, 3, &tgt)) return 0;
      sid = q2_site();
      q2_guard(f, in->pc, sid);
      fprintf(f, "    { int _t%d;\n", in->pc);
      fprintf(
          f,
          "      if (qjs_aot_branch_cmp(%d, Q2_REG(%d), Q2_REG(%d), "
          "&_t%d)) {\n",
          s, la, rb, in->pc);
      fprintf(f, "        if (_t%d) {", in->pc);
      q2_emit_edge(f, in->pc, tgt);
      fprintf(f, " } else goto b_%d;\n", in->next);
      fprintf(f, "      } else {\n");
      q2_miss(f, in->pc, sid);
      fprintf(f, "      } }\n");
      return 2;
    }
    default:
      return 0;
  }
}

static void q2_emit_body(FILE *f, const Q2Func *fn, char hex[33]) {
  int i, j;

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
     on. That covers a generator resume and a frame re-entered after an
     unwind. */
  fprintf(
      f,
      "    if (__builtin_expect(entry_pc != 0, 0)) return "
      "qjs_aot_deopt(ctx, f, fr, entry_pc, QJS_AOT_NO_SITE);\n");

  /* The fast paths keep their results in locals declared once, so a goto never
     crosses a declaration. */
  for (i = 0; i < fn->ninsns; i++) {
    const Q2Insn *in = &fn->insns[i];
    switch (in->rop) {
      case ROP_add:
      case ROP_sub:
      case ROP_mul:
      case ROP_add_imm_s:
      case ROP_sub_imm_s:
      case ROP_binary_imm:
      case ROP_binary_imm_s:
      case ROP_get_field:
      case ROP_get_field_own:
      case ROP_get_element:
      case ROP_get_field2:
      case ROP_get_field2_own:
        fprintf(f, "    JSValue _o%d;\n", in->pc);
        break;
      case ROP_get_field_chain:
      case ROP_get_field_chain_own:
        fprintf(f, "    JSValue _v%d, _w%d;\n", in->pc, in->pc);
        break;
      case ROP_mul_add:
      case ROP_mul_sub:
      case ROP_add_mul:
      case ROP_sub_mul:
        fprintf(f, "    JSValue _t%d, _r%d;\n", in->pc, in->pc);
        break;
      default:
        break;
    }
  }

  for (i = 0; i < fn->nblocks; i++) {
    const Q2Block *blk = &fn->blocks[i];
    fprintf(f, "b_%d:\n", blk->start_pc);
    for (j = 0; j < blk->ninsns; j++) {
      const Q2Insn *in = &fn->insns[blk->first_insn + j];
      /* The unwind and every deopt read `pc`, and an operation may throw from
         anywhere, so it tracks the instruction. */
      fprintf(f, "    pc = %d;\n", in->pc);
      /* 1 and 2 are lowered (2 emits its own deopt on a miss); 0 falls back to
         the step routine. */
      if (q2_emit_native(f, in) == 0) q2_emit_fallback(f, in, fn->exit_pc);
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
