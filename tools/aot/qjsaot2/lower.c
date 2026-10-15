/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * The first lowering: the IR's control flow drives the emitted C.
 *
 * What this pass is, and what it is not:
 *
 *   - Control flow is the CFG's own. A branch becomes a real `goto` between
 *     blocks and a loop becomes a real loop; the unit has no per-instruction
 *     dispatch and no `switch (pc)` after an instruction. The only switch left
 *     is the entry one (a resumed or deoptimised frame lands on a block start)
 *     plus a slow-dispatch safety net for a control transfer the graph does not
 *     model.
 *   - An operation the classifier does not know is a fallback: it runs through
 *     the engine's own step routine. That is one generic helper on tagged
 *     values, which is what this stage calls for; the specialised helpers come
 *     with types.
 *   - `fr` stays the home of every value. Under DRC the frame's register range
 *     is the engine's root range (`REGVM_SET_ROOT`), so a value that must
 *     survive a call has to be reachable from it; holding values only in C
 *     locals is a change that comes with the allocator and its ownership rule,
 *     not with the graph build. The liveness this IR computes is therefore
 *     exact rather than decorative: it is what a later pass needs to know
 *     before it moves a value out of the frame.
 *   - Returns go through the step engine for now. A native return needs an ABI
 *     entry point that hands the value out of the frame (the frame handle is
 *     opaque to the unit and `qjs_aot_ret` only reads it).
 *
 * The lowering is deliberately uniform: one shape for every instruction, so
 * there is nothing to get subtly wrong per opcode before the IR verifier and
 * the gates are looking at it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir.h"
#include "quickjs-aot-gen.h"
#include "quickjs-regvm.h"

/* The value of a register operand, as the frame holds it. */
/* Registers are byte offsets into the frame; AOT_REG is the ABI's accessor. */

/* The branch target of an instruction: an operand is relative to the end. */
static int q2_target(const Q2Insn *in, int arg, int *tgt) {
  if (arg >= in->narg || in->arg[arg].kind != 't') return 0;
  *tgt = in->next + (int)in->arg[arg].val;
  return 1;
}

/* A back edge polls the embedder's interrupt handler, the way every
   interpreted back edge does, so a compiled loop can still be stopped. The
   caller has already opened the arm this lands in, so it emits only the poll
   and the branch. */
static void q2_emit_edge(FILE *f, int off, int tgt) {
  if (tgt <= off)
    fprintf(
        f,
        " if (__builtin_expect(qjs_aot_poll(ctx) < 0, 0)) { pc = %d; "
        "goto exception; }",
        off);
  fprintf(f, " goto pc_%d;", tgt);
}

/* One instruction that runs through the shared step routine. */
static void q2_emit_fallback(FILE *f, const Q2Insn *in) {
  fprintf(f, "    ip = qjs_aot_step(code + %d, fr, f, ctx);\n", in->pc);
  fprintf(
      f,
      "    if (__builtin_expect(ip == NULL, 0)) { pc = %d; goto "
      "exception; }\n",
      in->pc);
  fprintf(f, "    if (ip == QJS_AOT_COLD_RET) return qjs_aot_ret(f);\n");
  fprintf(
      f,
      "    if (__builtin_expect(ip == QJS_AOT_SUSPEND, 0)) return "
      "qjs_aot_ret(f);\n");
  /* A yield or await resumes at its own pc: the twin detects the resume
     marker and returns the ip it was given, so the instruction runs again and
     takes its resumed arm. That is not a new location, so it loops back rather
     than leaving through the dispatch. */
  fprintf(
      f, "    if (__builtin_expect(ip == code + %d, 0)) goto pc_%d;\n", in->pc,
      in->pc);
  fprintf(
      f,
      "    if (__builtin_expect(ip != code + %d, 0)) { pc = (uint32_t)(ip "
      "- code); goto dispatch_ip; }\n",
      in->next);
}

/* Try to lower one instruction natively. Returns 1 when it did. */
static int q2_emit_native(FILE *f, const Q2Insn *in) {
  switch (in->rop) {
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
      fprintf(f, " }\n");
      fprintf(f, "      goto pc_%d; }\n", in->next);
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
      /* The generic comparison stays in the step engine, as the template
         does. */
      return 2;
    }
    default:
      return 0;
  }
}

/* The entry dispatch: a fresh entry (pc 0), a deoptimised or resumed frame and
   an unwind all arrive here with a pc, and every instruction has a label, so
   the switch covers them all. It is reached on entry and on the rare control
   transfers the graph does not model, never per instruction. */
static void q2_emit_dispatch(FILE *f, const Q2Func *fn) {
  int i;
  fprintf(f, "dispatch_ip:\n    switch (pc) {\n");
  for (i = 0; i < fn->ninsns; i++)
    fprintf(f, "    case %d: goto pc_%d;\n", fn->insns[i].pc, fn->insns[i].pc);
  fprintf(f, "    case %d: goto pc_end;\n", fn->exit_pc);
  fprintf(f, "    default: __builtin_unreachable();\n    }\n");
}

static void q2_emit_body(FILE *f, const Q2Func *fn, char hex[33]) {
  int i, k;

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
  fprintf(f, "    (void)code; (void)vb; (void)ab; (void)pic;\n");
  /* Registers are byte offsets into the frame. */
  fprintf(f, "#define Q2_REG(off) AOT_REG(fr, off)\n");
  fprintf(f, "    goto dispatch_ip;\n");

  for (i = 0; i < fn->ninsns; i++) {
    const Q2Insn *in = &fn->insns[i];
    int done;
    fprintf(f, "pc_%d:\n", in->pc);
    done = q2_emit_native(f, in);
    if (done == 2) {
      q2_emit_fallback(f, in);
    } else if (!done) {
      q2_emit_fallback(f, in);
    }
  }
  fprintf(f, "pc_end:\n    return qjs_aot_ret(f);\n");
  q2_emit_dispatch(f, fn);
  fprintf(f, "exception: {\n");
  fprintf(f, "        uint32_t rp;\n");
  fprintf(f, "        JSValue ex = qjs_aot_unwind(ctx, f, fr, pc, &rp);\n");
  fprintf(
      f, "        if (!JS_IsException(ex)) { pc = rp; goto dispatch_ip; }\n");
  fprintf(f, "        return JS_EXCEPTION;\n    }\n}\n\n");
}

/* The driver's body-emitter entry point. */
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
