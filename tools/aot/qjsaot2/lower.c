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
int qjs_aot_check_locals(void);

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
    case ROP_put_field:
    case ROP_get_global:
    case ROP_neg:
    case ROP_inc:
      return 1;
    default:
      return 0;
  }
}

/* The value current in a register at a point in the lowering, for the
   registers a write-back has to store but no operand names. */
typedef struct Q2Cur {
  const Q2Func *fn;
  Q2ValueId cur[4096];
} Q2Cur;

/* The lowering's per-function state, threaded through the instruction
   emitters: the write-back cursor and its scratch. */
typedef struct Q2Emit {
  const Q2Func *fn;
  Q2Cur cur; /* the value current after the instruction being emitted */
  Q2Cur pre; /* the value current before it, for a pre-instruction write-back */
  uint64_t *liveb; /* scratch: the live-before set */
  int cur_block;
  int cur_insn;
} Q2Emit;

/* One emitter runs at a time, so the state is a file static rather than a
   parameter on every native case. */
static Q2Emit g_em;

/* A fresh deopt site. */
static int q2_site(void) {
  return qjs_aot_site_alloc();
}

/* The live set just after the current instruction, for a post-definition
   leave. */
static const uint64_t *q2_live_after(int insn) {
  return g_em.fn->insn_live + (size_t)insn * g_em.fn->lwords;
}

/* ---- typed values (step 3) ---------------------------------------------- */

/* Whether a value holds its number unboxed in a typed local. */
static int q2_typed(const Q2Value *v) {
  return v->rep == Q2_R_I32 || v->rep == Q2_R_F64 || v->rep == Q2_R_BOOL;
}

/* The C expression for value `vv` seen as `want` (one of Q2_R_*). */
static int g_operand_err; /* an 'r' operand with no SSA value */
static char g_operand_err_msg[160];

static void q2_operand_fail(const Q2Func *fn, int pc, int k) {
  if (g_operand_err) return;
  g_operand_err = 1;
  snprintf(
      g_operand_err_msg, sizeof(g_operand_err_msg),
      "%s pc %d: operand %d has no SSA value",
      fn->g->name[0] ? fn->g->name : "<anon>", pc, k);
}

static const char *q2_use(
    const Q2Func *fn, Q2ValueId vv, int want, char *buf, size_t n) {
  const Q2Value *v = &fn->values[vv];
  if (v->rep == Q2_R_TAGGED) {
    /* tagged -> tagged only (the verifier rejects the rest) */
    snprintf(buf, n, "v%u", vv);
    return buf;
  }
  if (want == v->rep) {
    switch (v->rep) {
      case Q2_R_I32:
        snprintf(buf, n, "i%u", vv);
        break;
      case Q2_R_F64:
        snprintf(buf, n, "d%u", vv);
        break;
      case Q2_R_BOOL:
        snprintf(buf, n, "b%u", vv);
        break;
      default:
        break;
    }
    return buf;
  }
  if (v->rep == Q2_R_I32 && want == Q2_R_F64) {
    snprintf(buf, n, "(double)i%u", vv);
    return buf;
  }
  if (want == Q2_R_TAGGED) {
    /* box: a speculated load reads the JSValue the load produced, so the tag
       the interpreter would have seen is preserved; a computed typed value is
       boxed. */
    if (v->spec)
      snprintf(buf, n, "v%u", vv);
    else if (v->rep == Q2_R_I32)
      snprintf(buf, n, "JS_MKVAL(JS_TAG_INT, i%u)", vv);
    else if (v->rep == Q2_R_F64)
      snprintf(buf, n, "qjs_aot_box_num(d%u, qjs_aot_fitsi(d%u))", vv, vv);
    else
      snprintf(buf, n, "JS_MKVAL(JS_TAG_BOOL, b%u)", vv);
    return buf;
  }
  /* An unrepresentable conversion is a compiler bug, not a fallback. */
  g_operand_err = 1;
  snprintf(
      g_operand_err_msg, sizeof(g_operand_err_msg),
      "value v%u: no conversion to %s", vv, q2_rep_name(want));
  snprintf(buf, n, "?");
  return buf;
}

/* The C expression for an operand of `in` seen as `want`. */
static const char *q2_use_arg(
    const Q2Func *fn, const Q2Insn *in, int k, int want, char *buf, size_t n) {
  if (k < 0 || k >= in->narg || in->arg[k].kind != 'r' ||
      in->argval[k] == Q2_NO_VALUE) {
    q2_operand_fail(fn, in->pc, k);
    snprintf(buf, n, "JS_UNDEFINED");
    return buf;
  }
  return q2_use(fn, in->argval[k], want, buf, n);
}

/* A definition: the local, then the frame, with the handler's ownership. */
/* ---- the frame write-back (3.6) ----------------------------------------- */

static void q2_cur_block(const Q2Func *fn, Q2Cur *c, int b) {
  int k;
  c->fn = fn;
  for (k = 0; k < fn->nregs; k++) c->cur[k] = Q2_NO_VALUE;
  for (k = 0; k < fn->blocks[b].nlive_in; k++)
    c->cur[fn->blocks[b].in_reg[k]] = fn->blocks[b].in_val[k];
}

static void q2_cur_def(Q2Cur *c, int reg, Q2ValueId v) {
  c->cur[reg] = v;
}

/* Store every not-synced value live in `bits` back to its register, boxed.
   `cur` is the value current in each register at this point. */
static void q2_writeback(
    FILE *f, const Q2Func *fn, const Q2Cur *c, const uint64_t *bits) {
  int w, words = fn->lwords;
  for (w = 0; w < words; w++) {
    uint64_t m = bits[w];
    while (m) {
      int r = w * 64 + __builtin_ctzll(m);
      Q2ValueId v;
      m &= m - 1;
      if (r >= fn->nregs) continue;
      v = c->cur[r];
      if (v == Q2_NO_VALUE) continue;
      if (v >= (Q2ValueId)fn->nvalues) continue;
      if (!q2_typed(&fn->values[v])) continue;
      if (fn->values[v].synced) continue;
      {
        char e[64], o[64];
        q2_use(fn, v, Q2_R_TAGGED, e, sizeof(e));
        fprintf(
            f, "    AOT_SET_B(ctx, fr, %d, %s); /* wb v%u */\n",
            fn->values[v].reg * 16, e, v);
        (void)o;
      }
    }
  }
}

/* The debug check (--check-locals): at a leave, every live value's boxed form
   must equal its frame slot bitwise. A mismatch is a missing write-back or a
   stale local. It covers the call window too, which `insn_live` alone would
   miss. */
static void q2_check_locals(
    FILE *f, const Q2Func *fn, const Q2Cur *c, const uint64_t *bits) {
  int w, words = fn->lwords;
  for (w = 0; w < words; w++) {
    uint64_t m = bits[w];
    while (m) {
      int r = w * 64 + __builtin_ctzll(m);
      Q2ValueId v;
      m &= m - 1;
      if (r >= fn->nregs) continue;
      v = c->cur[r];
      if (v == Q2_NO_VALUE || v >= (Q2ValueId)fn->nvalues) continue;
      if (!q2_typed(&fn->values[v])) continue;
      if (fn->values[v].synced) continue;
      {
        char e[64];
        q2_use(fn, v, Q2_R_TAGGED, e, sizeof(e));
        fprintf(
            f,
            "    { JSValue _c = %s; if (memcmp(&_c, &AOT_REG(fr, %d), "
            "sizeof(JSValue)) != 0) { fprintf(stderr, \"check-locals: pc %%d r "
            "%%d v%u\\n\", __LINE__, %d); abort(); } }\n",
            e, fn->values[v].reg * 16, v, fn->values[v].reg);
      }
    }
  }
}

/* A leave: write back the not-synced values live in `bits`, then deopt at
   `pc`. `cur` may be NULL when no write-back is needed. */
static void q2_emit_leave(
    FILE *f, const Q2Func *fn, const Q2Cur *c, const uint64_t *bits, int pc,
    int sid) {
  fprintf(f, "    {\n");
  if (c && bits) q2_writeback(f, fn, c, bits);
  if (c && bits && qjs_aot_check_locals()) q2_check_locals(f, fn, c, bits);
  fprintf(f, "    return qjs_aot_deopt(ctx, f, fr, %d, %d); }\n", pc, sid);
}

/* The live set before an instruction: (insn_live \ defs) | uses. */
static void q2_live_before(const Q2Func *fn, int insn, uint64_t *out) {
  int words = fn->lwords, w, k;
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

/* The guard test, emitted only in a sweep build: a normal unit has no sweep and
   would pay a call per guard. A sweep deopt writes back too, so it goes through
   the same leave. */
static void q2_guard(FILE *f, int off, int sid) {
  if (qjs_aot_sweep_on()) {
    q2_live_before(g_em.fn, g_em.cur_insn, g_em.liveb);
    fprintf(
        f, "      if (__builtin_expect(qjs_aot_should_deopt(ctx, %d), 0)) {\n",
        sid);
    q2_emit_leave(f, g_em.fn, &g_em.pre, g_em.liveb, off, sid);
    fprintf(f, "      }\n");
  }
}

/* The miss of a guard: leave to the interpreter at this pc, before the
   instruction, so its effects have not happened. */
static void q2_miss(FILE *f, int off, int sid) {
  q2_live_before(g_em.fn, g_em.cur_insn, g_em.liveb);
  q2_emit_leave(f, g_em.fn, &g_em.pre, g_em.liveb, off, sid);
}

/* The value operand `k` holds at this instruction as a tagged C expression:
   the boxed form of a typed value. An `'r'` operand with no SSA value is a
   compiler bug, never a frame read. */
static const char *q2_op(const Q2Insn *in, int k, char *buf, size_t n) {
  if (k >= 0 && k < in->narg && in->arg[k].kind == 'r' &&
      in->argval[k] != Q2_NO_VALUE)
    return q2_use(g_em.fn, in->argval[k], Q2_R_TAGGED, buf, n);
  if (k >= 0 && k < in->narg && in->arg[k].kind != 'r') {
    snprintf(buf, n, "JS_UNDEFINED");
    return buf;
  }
  q2_operand_fail(g_em.fn, in->pc, k);
  snprintf(buf, n, "JS_UNDEFINED");
  return buf;
}

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

/* A definition of a typed value: assign its unboxed local, and store the boxed
   form only when the value is synced (a constant, a move of a synced value, a
   speculated load). A typed computed value is written back at a leave. */
static void q2_def_typed(
    FILE *f, const Q2Func *fn, const Q2Insn *in, int defi, const char *expr) {
  Q2ValueId v = in->def[defi];
  const Q2Value *val = &fn->values[v];
  int off = val->reg * 16;
  switch (val->rep) {
    case Q2_R_I32:
      fprintf(f, "    i%u = %s;\n", v, expr);
      break;
    case Q2_R_F64:
      fprintf(f, "    d%u = %s;\n", v, expr);
      break;
    case Q2_R_BOOL:
      fprintf(f, "    b%u = %s;\n", v, expr);
      break;
    default:
      fprintf(f, "    v%u = %s;\n", v, expr);
      break;
  }
  if (val->synced) {
    char e[64];
    q2_use(fn, v, Q2_R_TAGGED, e, sizeof(e));
    fprintf(f, "    AOT_SET_B(ctx, fr, %d, %s);\n", off, e);
  }
}

/* A speculated load's tag guard, after its write-through: it leaves at the
   *next* instruction, because the load's effect is done and `fr` holds the
   loaded value, so the interpreter continues from there and the call is not
   repeated. */
static void q2_spec_guard(FILE *f, const Q2Insn *in, Q2ValueId v) {
  const Q2Value *val = &g_em.fn->values[v];
  int sid = q2_site();
  q2_guard(f, in->next, sid);
  if (val->rep == Q2_R_I32) {
    fprintf(
        f,
        "    if (__builtin_expect(JS_VALUE_GET_TAG(v%u) != JS_TAG_INT, 0)) {\n",
        v);
    q2_emit_leave(
        f, g_em.fn, &g_em.cur, q2_live_after(g_em.cur_insn), in->next, sid);
    fprintf(f, "    }\n");
    fprintf(f, "    i%u = JS_VALUE_GET_INT(v%u);\n", v, v);
  } else {
    /* F64: an int or a float64 unboxes; anything else leaves. */
    fprintf(
        f,
        "    if (JS_VALUE_GET_TAG(v%u) == JS_TAG_INT) d%u = "
        "JS_VALUE_GET_INT(v%u);\n"
        "    else if (__builtin_expect(JS_VALUE_GET_TAG(v%u) == "
        "JS_TAG_FLOAT64, 1)) d%u = JS_VALUE_GET_FLOAT64(v%u);\n"
        "    else {\n",
        v, v, v, v, v, v);
    q2_emit_leave(
        f, g_em.fn, &g_em.cur, q2_live_after(g_em.cur_insn), in->next, sid);
    fprintf(f, "    }\n");
  }
}

/* Emit a spec guard for a load's definition, if it is one. */
static void q2_spec_after(
    FILE *f, const Q2Func *fn, const Q2Insn *in, int defi) {
  Q2ValueId v = in->def[defi];
  if (!fn->values[v].spec) return;
  q2_spec_guard(f, in, v);
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
   copy, in the parameter's representation), then jump. `si` is the successor's
   index in `b`. */
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
    for (k = 0; k < s->nparams; k++) {
      Q2ValueId pv = s->param_val[k];
      fprintf(f, " v%u = AOT_REG(fr, %d);", pv, s->param_reg[k] * 16);
      if (fn->values[pv].rep == Q2_R_I32)
        fprintf(f, " i%u = JS_VALUE_GET_INT(v%u);", pv, pv);
      else if (fn->values[pv].rep == Q2_R_F64)
        fprintf(
            f,
            " d%u = JS_VALUE_GET_TAG(v%u) == JS_TAG_INT ? "
            "JS_VALUE_GET_INT(v%u) : JS_VALUE_GET_FLOAT64(v%u);",
            pv, pv, pv, pv);
    }
    fprintf(f, " goto b_%d;", s->start_pc);
    return;
  }
  np = s->nparams;
  if (blk->nedge_args[si] < np) np = blk->nedge_args[si];
  /* First into the temps, converting to each parameter's representation. */
  for (k = 0; k < np; k++) {
    Q2ValueId av = blk->edge_arg[si][k];
    Q2ValueId pv = s->param_val[k];
    char e[64];
    switch (fn->values[pv].rep) {
      case Q2_R_I32:
        q2_use(fn, av, Q2_R_I32, e, sizeof(e));
        fprintf(f, " ei%d = %s;", k, e);
        break;
      case Q2_R_F64:
        q2_use(fn, av, Q2_R_F64, e, sizeof(e));
        fprintf(f, " ed%d = %s;", k, e);
        break;
      case Q2_R_BOOL:
        q2_use(fn, av, Q2_R_BOOL, e, sizeof(e));
        fprintf(f, " eb%d = %s;", k, e);
        break;
      default:
        q2_use(fn, av, Q2_R_TAGGED, e, sizeof(e));
        fprintf(f, " ev%d = %s;", k, e);
        break;
    }
  }
  for (k = 0; k < np; k++) {
    Q2ValueId pv = s->param_val[k];
    switch (fn->values[pv].rep) {
      case Q2_R_I32:
        fprintf(f, " i%u = ei%d;", pv, k);
        break;
      case Q2_R_F64:
        fprintf(f, " d%u = ed%d;", pv, k);
        break;
      case Q2_R_BOOL:
        fprintf(f, " b%u = eb%d;", pv, k);
        break;
      default: {
        /* Tagged values are written through. An argument from the same
           register that is itself in the frame needs no store; anything else
           (a number boxed on this edge, or another register's value) does, or
           the slot stays stale for every later leave. */
        Q2ValueId av = blk->edge_arg[si][k];
        fprintf(f, " v%u = ev%d;", pv, k);
        if (av == Q2_NO_VALUE || q2_typed(&fn->values[av]) ||
            !fn->values[av].synced ||
            fn->values[av].reg != fn->values[pv].reg)
          fprintf(f, " AOT_SET_B(ctx, fr, %d, v%u);", s->param_reg[k] * 16, pv);
        break;
      }
    }
  }
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
  int i, w;
  /* The step reads its own uses, and if it throws or leaves, the unwind or
     deopt reads the rest: write back every live value, before and after. */
  for (w = 0; w < fn->lwords; w++)
    g_em.liveb[w] |= q2_live_after(g_em.cur_insn)[w];
  {
    /* A step whose roles are unknown may read any register, a window
       included, so every value this point holds goes back to the frame. */
    const Q2RopInfo *ri = q2_rop_info(in->rop);
    if (ri->op == Q2_OP_FALLBACK && ri->ndef == 0 && ri->nuse == 0 &&
        ri->window < 0)
      for (w = 0; w < fn->lwords; w++) g_em.liveb[w] = ~0ull;
  }
  q2_writeback(f, fn, &g_em.pre, g_em.liveb);
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
  /* A speculated fallback result guards after the step: the callee has run, so
     the leave is at the next instruction and the call is not repeated. */
  for (i = 0; i < in->ndef; i++) {
    Q2ValueId v = in->def[i];
    if (fn->values[v].spec) {
      q2_cur_def(&g_em.cur, fn->values[v].reg, v);
      q2_spec_guard(f, in, v);
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

/* A binary/compare whose result is typed, dispatched before the opcode switch
   so the generic ROP_binary and the immediate forms share one path. Returns 1
   when it emitted the instruction. */
static int q2_typed_binary(FILE *f, const Q2Func *fn, const Q2Insn *in) {
  Q2ValueId dv;
  const Q2Value *dval;
  int sub = in->sub;
  int aoff, boff, imm;
  int is_imm;

  if (in->op != Q2_OP_BINARY || in->ndef < 1) return 0;
  /* the fused family has its own lowering (two definitions) */
  if (in->rop == ROP_mul_add || in->rop == ROP_mul_sub ||
      in->rop == ROP_add_mul || in->rop == ROP_sub_mul)
    return 0;
  dv = in->def[0];
  dval = &fn->values[dv];
  if (dval->rep == Q2_R_TAGGED) return 0;
  /* Every register operand must be typed: a tagged one would need a guard the
     typed path does not emit, so fall back instead. */
  {
    int k;
    for (k = 0; k < in->nuse; k++)
      if (fn->values[in->use[k]].rep == Q2_R_TAGGED) return 0;
  }

  is_imm =
      (in->rop == ROP_binary_imm || in->rop == ROP_binary_imm_s ||
       in->rop == ROP_add_imm_s || in->rop == ROP_sub_imm_s);
  if (in->rop == ROP_add_imm_s || in->rop == ROP_sub_imm_s) {
    aoff = 1;
    imm = (int)(int32_t)in->arg[2].val;
  } else if (is_imm) {
    aoff = 2;
    imm = (int)(int32_t)in->arg[3].val;
  } else if (in->rop == ROP_binary || in->rop == ROP_compare) {
    aoff = 2; /* "rbrr": d, sub, a, b */
    boff = 3;
    imm = 0;
  } else {
    aoff = 1; /* "rrr": d, a, b */
    boff = 2;
    imm = 0;
  }

  if (dval->rep == Q2_R_I32) {
    char x[48], y[48];
    int sid = q2_site();
    q2_use_arg(fn, in, aoff, Q2_R_I32, x, sizeof(x));
    if (is_imm)
      snprintf(y, sizeof(y), "%d", imm);
    else
      q2_use_arg(fn, in, boff, Q2_R_I32, y, sizeof(y));
    q2_guard(f, in->pc, sid);
    switch (sub) {
      case REG_BIN_ADD:
        fprintf(
            f,
            "    if (__builtin_expect(__builtin_add_overflow(%s, %s, &i%u), "
            "0))\n",
            x, y, dv);
        q2_miss(f, in->pc, sid);
        return 1;
      case REG_BIN_SUB:
        fprintf(
            f,
            "    if (__builtin_expect(__builtin_sub_overflow(%s, %s, &i%u), "
            "0))\n",
            x, y, dv);
        q2_miss(f, in->pc, sid);
        return 1;
      case REG_BIN_MUL:
        fprintf(
            f,
            "    if (__builtin_expect(__builtin_mul_overflow(%s, %s, &i%u), "
            "0))\n",
            x, y, dv);
        q2_miss(f, in->pc, sid);
        fprintf(
            f, "    if (__builtin_expect(i%u == 0 && (%s | %s) < 0, 0))\n", dv,
            x, y);
        q2_miss(f, in->pc, sid);
        return 1;
      case REG_BIN_AND:
        fprintf(f, "    i%u = %s & %s;\n", dv, x, y);
        return 1;
      case REG_BIN_OR:
        fprintf(f, "    i%u = %s | %s;\n", dv, x, y);
        return 1;
      case REG_BIN_XOR:
        fprintf(f, "    i%u = %s ^ %s;\n", dv, x, y);
        return 1;
      case REG_BIN_SHL:
        fprintf(
            f, "    i%u = (int32_t)((uint32_t)%s << (%s & 31));\n", dv, x, y);
        return 1;
      case REG_BIN_SAR:
        fprintf(f, "    i%u = %s >> (%s & 31);\n", dv, x, y);
        return 1;
      default:
        return 0;
    }
  }
  if (dval->rep == Q2_R_F64) {
    char x[48], y[48];
    const char *opx = sub == REG_BIN_ADD   ? "+"
                      : sub == REG_BIN_SUB ? "-"
                      : sub == REG_BIN_MUL ? "*"
                                           : "/";
    q2_use_arg(fn, in, aoff, Q2_R_F64, x, sizeof(x));
    if (is_imm)
      snprintf(y, sizeof(y), "%.17g", (double)imm);
    else
      q2_use_arg(fn, in, boff, Q2_R_F64, y, sizeof(y));
    if (sub != REG_BIN_ADD && sub != REG_BIN_SUB && sub != REG_BIN_MUL &&
        sub != REG_BIN_DIV)
      return 0;
    fprintf(f, "    d%u = %s %s %s;\n", dv, x, opx, y);
    return 1;
  }
  if (dval->rep == Q2_R_BOOL) {
    char x[48], y[48];
    const char *opx = sub == REG_BIN_LT                                 ? "<"
                      : sub == REG_BIN_LTE                              ? "<="
                      : sub == REG_BIN_GT                               ? ">"
                      : sub == REG_BIN_GTE                              ? ">="
                      : sub == REG_BIN_NEQ || sub == REG_BIN_STRICT_NEQ ? "!="
                                                                        : "==";
    int want = (fn->values[in->use[0]].rep == Q2_R_F64 ||
                (in->nuse > 1 && fn->values[in->use[1]].rep == Q2_R_F64))
                   ? Q2_R_F64
                   : Q2_R_I32;
    q2_use_arg(fn, in, aoff, want, x, sizeof(x));
    if (is_imm)
      snprintf(y, sizeof(y), "%d", imm);
    else
      q2_use_arg(fn, in, boff, want, y, sizeof(y));
    fprintf(f, "    b%u = (%s %s %s);\n", dv, x, opx, y);
    return 1;
  }
  return 0;
}

/* 1 lowered it fully, 2 lowered its fast path and deopts on a miss, 0 to fall
   back to the step routine. */
static int q2_emit_native(
    FILE *f, const Q2Func *fn, int cur_block, const Q2Insn *in) {
  char a[64], b[64];
  if (q2_typed_binary(f, fn, in)) return 1;
  switch (in->rop) {
    case ROP_load_undefined:
    case ROP_load_null:
    case ROP_load_true:
    case ROP_load_false: {
      Q2ValueId dv = in->def[0];
      const Q2Value *dval = &fn->values[dv];
      if (dval->rep == Q2_R_BOOL) {
        q2_def_typed(f, fn, in, 0, in->rop == ROP_load_true ? "1" : "0");
      } else {
        q2_def(
            f, fn, in, 0,
            in->rop == ROP_load_undefined ? "JS_UNDEFINED"
            : in->rop == ROP_load_null    ? "JS_NULL"
            : in->rop == ROP_load_true    ? "JS_TRUE"
                                          : "JS_FALSE",
            1);
      }
      return 1;
    }
    case ROP_load_i8:
    case ROP_load_i8_s: {
      char e[64];
      if (fn->values[in->def[0]].rep == Q2_R_I32) {
        snprintf(e, sizeof(e), "%d", (int)(int8_t)in->arg[1].val);
        q2_def_typed(f, fn, in, 0, e);
      } else {
        snprintf(
            e, sizeof(e), "JS_MKVAL(JS_TAG_INT, %d)",
            (int)(int8_t)in->arg[1].val);
        q2_def(f, fn, in, 0, e, 1);
      }
      return 1;
    }
    case ROP_load_i32: {
      char e[64];
      if (fn->values[in->def[0]].rep == Q2_R_I32) {
        snprintf(e, sizeof(e), "(int32_t)%d", (int32_t)in->arg[1].val);
        q2_def_typed(f, fn, in, 0, e);
      } else {
        snprintf(
            e, sizeof(e), "JS_MKVAL(JS_TAG_INT, (int32_t)%d)",
            (int32_t)in->arg[1].val);
        q2_def(f, fn, in, 0, e, 1);
      }
      return 1;
    }
    case ROP_move:
    case ROP_move_s: {
      int d = (int)in->arg[0].val, s = (int)in->arg[1].val;
      Q2ValueId dv = in->def[0];
      const Q2Value *dval = &fn->values[dv];
      if (d != s) {
        if (dval->rep != Q2_R_TAGGED) {
          char e[64];
          q2_use(fn, in->use[0], dval->rep, e, sizeof(e));
          q2_def_typed(f, fn, in, 0, e);
        } else {
          q2_def(f, fn, in, 0, q2_op(in, 1, a, sizeof(a)), 0);
        }
        return 1;
      }
      /* A self-move is a no-op in the frame, but the IR still gives it a fresh
         definition that later instructions read, so that local must hold the
         value the register already has. The frame slot already does, so assign
         the local only: storing would strand a reference under an owning build,
         which is why the interpreter skips the store too. */
      {
        char e[64];
        q2_use(fn, in->use[0], dval->rep, e, sizeof(e));
        switch (dval->rep) {
          case Q2_R_I32:
            fprintf(f, "    i%u = %s;\n", dv, e);
            break;
          case Q2_R_F64:
            fprintf(f, "    d%u = %s;\n", dv, e);
            break;
          case Q2_R_BOOL:
            fprintf(f, "    b%u = %s;\n", dv, e);
            break;
          default:
            fprintf(f, "    v%u = %s;\n", dv, e);
            break;
        }
      }
      return 1;
    }
    case ROP_move_xfer: {
      int d = (int)in->arg[0].val, s = (int)in->arg[1].val;
      Q2ValueId dv = in->def[0];
      const Q2Value *dval = &fn->values[dv];
      if (q2_typed(dval)) {
        /* A typed value carries no reference, so this is a local move. */
        char e[48];
        q2_use(fn, in->use[0], dval->rep, e, sizeof(e));
        q2_def_typed(f, fn, in, 0, e);
        return 1;
      }
      q2_guard(f, in->pc, q2_site());
      fprintf(f, "    v%u = %s;\n", dv, q2_op(in, 1, a, sizeof(a)));
      fprintf(f, "    qjs_aot_move_xfer(ctx, fr, %d, %d);\n", d, s);
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
      Q2ValueId dv = in->def[0];
      const Q2Value *dval = &fn->values[dv];
      int off = dval->reg * 16;
      if (dval->rep == Q2_R_I32) {
        /* int-only: plain C with an overflow and -0 guard that leaves before
           the instruction. */
        char x[64], y[64];
        q2_use_arg(fn, in, 1, Q2_R_I32, x, sizeof(x));
        q2_use_arg(fn, in, 2, Q2_R_I32, y, sizeof(y));
        q2_guard(f, in->pc, sid);
        if (in->rop == ROP_add)
          fprintf(
              f,
              "    if (__builtin_expect(__builtin_add_overflow(%s, %s, &i%u), "
              "0))\n",
              x, y, dv);
        else if (in->rop == ROP_sub)
          fprintf(
              f,
              "    if (__builtin_expect(__builtin_sub_overflow(%s, %s, &i%u), "
              "0))\n",
              x, y, dv);
        else
          fprintf(
              f,
              "    if (__builtin_expect(__builtin_mul_overflow(%s, %s, &i%u), "
              "0))\n",
              x, y, dv);
        q2_miss(f, in->pc, sid);
        if (in->rop == ROP_mul)
          fprintf(
              f, "    if (__builtin_expect(i%u == 0 && (%s | %s) < 0, 0))\n",
              dv, x, y),
              q2_miss(f, in->pc, sid);
        return 1;
      }
      if (dval->rep == Q2_R_F64) {
        char x[64], y[64];
        const char *opx = in->rop == ROP_add   ? "+"
                          : in->rop == ROP_sub ? "-"
                                               : "*";
        q2_use_arg(fn, in, 1, Q2_R_F64, x, sizeof(x));
        q2_use_arg(fn, in, 2, Q2_R_F64, y, sizeof(y));
        q2_guard(f, in->pc, sid);
        fprintf(f, "    d%u = %s %s %s;\n", dv, x, opx, y);
        return 1;
      }
      q2_guard(f, in->pc, sid);
      fprintf(
          f,
          "    if (__builtin_expect(!q2_arith(%d, %s, %s, &v%u), "
          "0))\n",
          sub, q2_op(in, 1, a, sizeof(a)), q2_op(in, 2, b, sizeof(b)), dv);
      q2_miss(f, in->pc, sid);
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, v%u);\n", off, dv);
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
      Q2ValueId dv = in->def[0];
      const Q2Value *dval = &fn->values[dv];
      int off = dval->reg * 16;
      if (dval->rep == Q2_R_I32) {
        char x[64];
        q2_use_arg(fn, in, ai, Q2_R_I32, x, sizeof(x));
        q2_guard(f, in->pc, sid);
        fprintf(
            f,
            "    if (__builtin_expect(__builtin_add_overflow(%s, %d, &i%u), "
            "0))\n",
            x, imm, dv);
        q2_miss(f, in->pc, sid);
        return 1;
      }
      if (dval->rep == Q2_R_F64) {
        char x[64];
        const char *opx = (sub == REG_BIN_SUB) ? "-" : "+";
        q2_use_arg(fn, in, ai, Q2_R_F64, x, sizeof(x));
        q2_guard(f, in->pc, sid);
        fprintf(f, "    d%u = %s %s %d;\n", dv, x, opx, imm);
        return 1;
      }
      q2_guard(f, in->pc, sid);
      fprintf(
          f,
          "    if (__builtin_expect(!q2_arith(%d, %s, "
          "JS_MKVAL(JS_TAG_INT, %d), &v%u), 0))\n",
          sub, q2_op(in, ai, a, sizeof(a)), imm, dv);
      q2_miss(f, in->pc, sid);
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, v%u);\n", off, dv);
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
      /* The typed form: both halves are plain C. The first result is in a
         local the second half reads, and neither is written to the frame until
         a leave, so a guard before the first assignment cannot see half an
         instruction. */
      if (fn->values[in->def[0]].rep == Q2_R_F64 &&
          fn->values[in->def[1]].rep == Q2_R_F64) {
        char x[48], y[48], c[48];
        Q2ValueId d0 = in->def[0], d1 = in->def[1];
        char op2 = (sub2 == REG_BIN_ADD) ? '+' : '-';
        q2_guard(f, in->pc, sid);
        q2_use_arg(fn, in, 1, Q2_R_F64, x, sizeof(x));
        q2_use_arg(fn, in, 2, Q2_R_F64, y, sizeof(y));
        q2_use_arg(fn, in, 4, Q2_R_F64, c, sizeof(c));
        if (mul_first)
          fprintf(
              f, "    d%u = %s * %s; d%u = d%u %c %s;\n", d0, x, y, d1, d0, op2,
              c);
        else
          fprintf(
              f, "    d%u = %s * %s; d%u = %s %c d%u;\n", d0, x, y, d1, c, op2,
              d0);
        return 1;
      }
      if (fn->values[in->def[0]].rep == Q2_R_I32 &&
          fn->values[in->def[1]].rep == Q2_R_I32) {
        char x[48], y[48], c[48];
        Q2ValueId d0 = in->def[0], d1 = in->def[1];
        q2_guard(f, in->pc, sid);
        q2_use_arg(fn, in, 1, Q2_R_I32, x, sizeof(x));
        q2_use_arg(fn, in, 2, Q2_R_I32, y, sizeof(y));
        q2_use_arg(fn, in, 4, Q2_R_I32, c, sizeof(c));
        if (mul_first) {
          fprintf(
              f,
              "    if (__builtin_expect(__builtin_mul_overflow(%s, %s, &i%u), "
              "0))\n",
              x, y, d0);
          q2_miss(f, in->pc, sid);
          fprintf(
              f, "    if (__builtin_expect(i%u == 0 && (%s | %s) < 0, 0))\n",
              d0, x, y);
          q2_miss(f, in->pc, sid);
          if (sub2 == REG_BIN_ADD)
            fprintf(
                f,
                "    if (__builtin_expect(__builtin_add_overflow(i%u, %s, "
                "&i%u), 0))\n",
                d0, c, d1);
          else
            fprintf(
                f,
                "    if (__builtin_expect(__builtin_sub_overflow(i%u, %s, "
                "&i%u), 0))\n",
                d0, c, d1);
        } else {
          fprintf(
              f,
              "    if (__builtin_expect(__builtin_mul_overflow(%s, %s, &i%u), "
              "0))\n",
              c, x, d0);
          q2_miss(f, in->pc, sid);
          fprintf(
              f, "    if (__builtin_expect(i%u == 0 && (%s | %s) < 0, 0))\n",
              d0, c, x);
          q2_miss(f, in->pc, sid);
          if (sub2 == REG_BIN_ADD)
            fprintf(
                f,
                "    if (__builtin_expect(__builtin_add_overflow(i%u, %s, "
                "&i%u), 0))\n",
                d0, y, d1);
          else
            fprintf(
                f,
                "    if (__builtin_expect(__builtin_sub_overflow(i%u, %s, "
                "&i%u), 0))\n",
                d0, y, d1);
        }
        q2_miss(f, in->pc, sid);
        return 1;
      }
      q2_guard(f, in->pc, sid);
      fprintf(
          f,
          "    if (__builtin_expect(!q2_arith(%d, %s, %s, &v%u), "
          "0))\n",
          REG_BIN_MUL, q2_op(in, 1, a, sizeof(a)), q2_op(in, 2, b, sizeof(b)),
          in->def[0]);
      q2_miss(f, in->pc, sid);
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, v%u);\n", dm_off, in->def[0]);
      q2_guard(f, in->pc, q2_site());
      {
        char l[64], r[64];
        if (mul_first) {
          snprintf(l, sizeof(l), "v%u", in->def[0]);
          q2_op(in, 4, r, sizeof(r));
        } else {
          q2_op(in, 4, l, sizeof(l));
          snprintf(r, sizeof(r), "v%u", in->def[0]);
        }
        fprintf(
            f,
            "      if (__builtin_expect(!q2_arith(%d, %s, %s, "
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
      q2_op(in, 1, a, sizeof(a));
      fprintf(
          f,
          "    if (__builtin_expect(!q2_own_read_hit(pic, holders, %s, %d, "
          "&v%u), 0)) {\n",
          a, site, in->def[0]);
      /* A miss fills the cache and continues compiled rather than leaving for
         the interpreter, so a cold or polymorphic site never disables the
         function. The read may run a getter, so it can throw; it returns an
         owned value, so the store adopts it. */
      q2_writeback(f, fn, &g_em.pre, g_em.liveb);
      fprintf(
          f,
          "      if (qjs_aot_read_field_slow(ctx, f, code + %d, %s, "
          "(int)qjs_aot_ld_u32(code + %d), %d, &v%u) < 0) { pc = %d; goto "
          "exception; }\n"
          "      pic = qjs_aot_pic(f); holders = qjs_aot_pic_holder(f);\n"
          "      qjs_aot_set_reg_p(ctx, fr, %d, v%u);\n",
          in->pc, a, in->arg[2].off, site, in->def[0], in->pc, off, in->def[0]);
      fprintf(f, "    } else {\n      AOT_SET_B(ctx, fr, %d, v%u);\n    }\n", off,
              in->def[0]);
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
      q2_op(in, 1, a, sizeof(a));
      /* The second receiver is the first link's result, as in the handler. Each
         link is stored as it is produced, so the next reads it from the frame
         and a miss on either fills that link's cache and continues. */
      fprintf(
          f,
          "    if (__builtin_expect(!q2_own_read_hit(pic, holders, %s, %d, "
          "&v%u), 0)) {\n",
          a, s1, in->def[0]);
      q2_writeback(f, fn, &g_em.pre, g_em.liveb);
      fprintf(
          f,
          "      if (qjs_aot_read_field_slow(ctx, f, code + %d, %s, "
          "(int)qjs_aot_ld_u32(code + %d), %d, &v%u) < 0) { pc = %d; goto "
          "exception; }\n"
          "      pic = qjs_aot_pic(f); holders = qjs_aot_pic_holder(f);\n"
          "      qjs_aot_set_reg_p(ctx, fr, %d, v%u);\n",
          in->pc, a, in->arg[2].off, s1, in->def[0], in->pc, off0, in->def[0]);
      fprintf(f, "    } else {\n      AOT_SET_B(ctx, fr, %d, v%u);\n    }\n", off0,
              in->def[0]);
      fprintf(
          f,
          "    if (__builtin_expect(!q2_own_read_hit(pic, holders, v%u, %d, "
          "&v%u), 0)) {\n",
          in->def[0], s2, in->def[1]);
      q2_writeback(f, fn, &g_em.pre, g_em.liveb);
      fprintf(
          f,
          "      if (qjs_aot_read_field_slow(ctx, f, code + %d, v%u, "
          "(int)qjs_aot_ld_u32(code + %d), %d, &v%u) < 0) { pc = %d; goto "
          "exception; }\n"
          "      pic = qjs_aot_pic(f); holders = qjs_aot_pic_holder(f);\n"
          "      qjs_aot_set_reg_p(ctx, fr, %d, v%u);\n",
          in->pc, in->def[0], in->arg[6].off, s2, in->def[1], in->pc, off1, in->def[1]);
      fprintf(f, "    } else {\n      AOT_SET_B(ctx, fr, %d, v%u);\n    }\n", off1,
              in->def[1]);
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
      q2_op(in, 1, a, sizeof(a));
      fprintf(
          f,
          "    if (qjs_aot_proto_fold(ctx, f, pic, %d, %s, %d, &v%u)) {\n"
          "      qjs_aot_set_reg_p(ctx, fr, %d, v%u);\n"
          "    } else if (__builtin_expect(!q2_own_read_hit(pic, holders, "
          "%s, %d, &v%u), 0)) {\n",
          fsid, a, site, in->def[0], off, in->def[0], a, site, in->def[0]);
      q2_writeback(f, fn, &g_em.pre, g_em.liveb);
      fprintf(
          f,
          "      if (qjs_aot_read_field_slow(ctx, f, code + %d, %s, "
          "(int)qjs_aot_ld_u32(code + %d), %d, &v%u) < 0) { pc = %d; goto "
          "exception; }\n"
          "      pic = qjs_aot_pic(f); holders = qjs_aot_pic_holder(f);\n"
          "      qjs_aot_set_reg_p(ctx, fr, %d, v%u);\n",
          in->pc, a, in->arg[2].off, site, in->def[0], in->pc, off, in->def[0]);
      fprintf(f, "    } else {\n      AOT_SET_B(ctx, fr, %d, v%u);\n    }\n", off,
              in->def[0]);
      return 1;
    }
    case ROP_put_field: { /* "rasr": receiver, atom, site, value */
      int site = (int)in->arg[2].val;
      char v[64];
      if ((unsigned)site == REG_IC_NONE) return 0;
      q2_guard(f, in->pc, q2_site());
      q2_op(in, 0, a, sizeof(a));
      q2_op(in, 3, v, sizeof(v));
      /* The own-slot overwrite inline; a counted value or a prototype receiver
         takes the engine's hit. A miss runs the engine's store, which can call
         a setter and throw, fills the cache and continues compiled. */
      fprintf(
          f,
          "    if (__builtin_expect(!q2_put_field_hit(pic, %s, %d, %s), 0) &&\n"
          "        !qjs_aot_put_field_own(ctx, f, %s, %d, %s)) {\n",
          a, site, v, a, site, v);
      q2_writeback(f, fn, &g_em.pre, g_em.liveb);
      fprintf(
          f,
          "      if (qjs_aot_put_field_slow(ctx, f, code + %d, %s, "
          "(int)qjs_aot_ld_u32(code + %d), %d, %s) < 0) { pc = %d; goto "
          "exception; }\n"
          "      pic = qjs_aot_pic(f); holders = qjs_aot_pic_holder(f);\n"
          "    }\n",
          in->pc, a, in->arg[1].off, site, v, in->pc);
      return 1;
    }
    case ROP_get_global: { /* "rabs": d, atom, throwing, site */
      int site = (int)in->arg[3].val;
      int off = fn->values[in->def[0]].reg * 16;
      if ((unsigned)site == REG_IC_NONE) return 0;
      q2_guard(f, in->pc, q2_site());
      /* The site-cache hit borrows the global's slot. A miss probes the cell
         cache and then reads the global, which can throw. */
      fprintf(
          f,
          "    if (__builtin_expect(qjs_aot_get_global_hit(ctx, f, %d, &v%u), 1)) "
          "{\n"
          "      AOT_SET_B(ctx, fr, %d, v%u);\n"
          "    } else {\n"
          "      int _r;\n",
          site, in->def[0], off, in->def[0]);
      q2_writeback(f, fn, &g_em.pre, g_em.liveb);
      fprintf(
          f,
          "      _r = qjs_aot_get_global_site(ctx, f, %d, "
          "(int)qjs_aot_ld_u32(code + %d), %d, &v%u);\n"
          "      if (_r < 0) { pc = %d; goto exception; }\n"
          "      if (_r) qjs_aot_set_reg_p(ctx, fr, %d, v%u);\n"
          "      else AOT_SET_B(ctx, fr, %d, v%u);\n"
          "      pic = qjs_aot_pic(f); holders = qjs_aot_pic_holder(f);\n"
          "    }\n",
          site, in->arg[1].off, (int)in->arg[2].val, in->def[0], in->pc, off,
          in->def[0], off, in->def[0]);
      return 1;
    }
    case ROP_neg:
    case ROP_inc: { /* "rr": d, a */
      int sub = in->rop == ROP_neg ? REG_UN_NEG : REG_UN_INC;
      int off = fn->values[in->def[0]].reg * 16;
      q2_guard(f, in->pc, q2_site());
      q2_op(in, 1, a, sizeof(a));
      /* An int or a float64 inline; -0, an int overflow and a coercion take
         the engine's unary, which can run valueOf and throw. */
      fprintf(
          f,
          "    if (__builtin_expect(q2_unary_fast(%d, %s, &v%u), 1)) {\n"
          "      AOT_SET_B(ctx, fr, %d, v%u);\n"
          "    } else {\n",
          sub, a, in->def[0], off, in->def[0]);
      q2_writeback(f, fn, &g_em.pre, g_em.liveb);
      fprintf(
          f,
          "      if (!qjs_aot_unary_num_fast(%d, %s, &v%u) &&\n"
          "          qjs_aot_unary(ctx, %d, %s, &v%u) < 0) { pc = %d; goto "
          "exception; }\n"
          "      qjs_aot_set_reg_p(ctx, fr, %d, v%u);\n"
          "    }\n",
          sub, a, in->def[0], sub, a, in->def[0], in->pc, off, in->def[0]);
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
      if (in->nuse >= 1 && in->use[0] < (Q2ValueId)fn->nvalues) {
        const Q2Value *uv = &fn->values[in->use[0]];
        if (uv->rep == Q2_R_BOOL) {
          fprintf(f, "    if (b%u == %d) {", in->use[0], want);
          q2_emit_edge(f, fn, cur_block, 0);
          fprintf(f, " }\n");
          return 1;
        }
        if (uv->rep == Q2_R_I32) {
          fprintf(f, "    if ((i%u != 0) == %d) {", in->use[0], want);
          q2_emit_edge(f, fn, cur_block, 0);
          fprintf(f, " }\n");
          return 1;
        }
        if (uv->rep == Q2_R_F64) {
          fprintf(
              f, "    if (((d%u == d%u && d%u != 0.0) == %d)) {", in->use[0],
              in->use[0], in->use[0], want);
          q2_emit_edge(f, fn, cur_block, 0);
          fprintf(f, " }\n");
          return 1;
        }
      }
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
      int sub = s & ~REG_CMP_INVERT;
      int invert = (s & REG_CMP_INVERT) != 0;
      if (!q2_target(in, 3, &tgt)) return 0;
      /* `=== undefined` / `!== undefined` tests `a` alone. A typed value is a
         number or a boolean, never undefined, so the branch is decided. */
      if ((unsigned)(s - REG_BIN_IS_UNDEF) < 2 && in->nuse >= 1 &&
          fn->values[in->use[0]].rep != Q2_R_TAGGED) {
        if (s == REG_BIN_IS_NOT_UNDEF) {
          fprintf(f, "    {");
          q2_emit_edge(f, fn, cur_block, 0);
          fprintf(f, " }\n");
        }
        return 1;
      }
      if (in->nuse >= 2 && fn->values[in->use[0]].rep != Q2_R_TAGGED &&
          fn->values[in->use[1]].rep != Q2_R_TAGGED &&
          (sub == REG_BIN_LT || sub == REG_BIN_LTE || sub == REG_BIN_GT ||
           sub == REG_BIN_GTE || sub == REG_BIN_EQ || sub == REG_BIN_NEQ ||
           sub == REG_BIN_STRICT_EQ || sub == REG_BIN_STRICT_NEQ)) {
        const char *opx = sub == REG_BIN_LT    ? "<"
                          : sub == REG_BIN_LTE ? "<="
                          : sub == REG_BIN_GT  ? ">"
                          : sub == REG_BIN_GTE ? ">="
                          : sub == REG_BIN_NEQ || sub == REG_BIN_STRICT_NEQ
                              ? "!="
                              : "==";
        int want = (fn->values[in->use[0]].rep == Q2_R_F64 ||
                    fn->values[in->use[1]].rep == Q2_R_F64)
                       ? Q2_R_F64
                       : Q2_R_I32;
        char x[64], y[64];
        q2_use_arg(fn, in, 1, want, x, sizeof(x));
        q2_use_arg(fn, in, 2, want, y, sizeof(y));
        sid = q2_site();
        q2_guard(f, in->pc, sid);
        fprintf(f, "    if (%s(%s %s %s)) {", invert ? "!" : "", x, opx, y);
        q2_emit_edge(f, fn, cur_block, 0);
        fprintf(f, " }\n");
        return 1;
      }
      sid = q2_site();
      q2_guard(f, in->pc, sid);
      fprintf(f, "    { int _t = 0;\n");
      q2_op(in, 1, a, sizeof(a));
      q2_op(in, 2, b, sizeof(b));
      /* A miss is not a deopt: mixed tags, strings, objects and `==` coercion
         take the interpreter's generic compare and stay compiled. It can run
         valueOf, so the frame is written back first and it can throw. */
      fprintf(
          f,
          "      if (__builtin_expect(!q2_branch_cmp(%d, %s, %s, "
          "&_t), 0)) {\n",
          s, a, b);
      q2_writeback(f, fn, &g_em.pre, g_em.liveb);
      fprintf(
          f,
          "      _t = qjs_aot_cmp_take(ctx, %d, %s, %s);\n"
          "      if (_t < 0) { pc = %d; goto exception; }\n"
          "      }\n",
          s, a, b, in->pc);
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

  memset(&g_em, 0, sizeof(g_em));
  g_em.fn = fn;
  g_em.liveb = calloc((size_t)(fn->lwords > 0 ? fn->lwords : 1), 8);

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
  fprintf(f, "    void *const *holders = qjs_aot_pic_holder(f);\n");
  fprintf(
      f,
      "    (void)code; (void)ip; (void)vb; (void)ab; (void)pic; "
      "(void)holders;\n");
  fprintf(f, "#define Q2_REG(off) AOT_REG(fr, off)\n");
  /* Only pc 0 runs compiled; anything else is the interpreter's, from that pc
     on. */
  fprintf(
      f,
      "    if (__builtin_expect(entry_pc != 0, 0)) return "
      "qjs_aot_deopt(ctx, f, fr, entry_pc, QJS_AOT_NO_SITE);\n");

  /* Every SSA value is a C local, declared once so a goto never crosses a
     declaration: a boxed `JSValue` for a tagged value, an `int32_t`/`double`
     for a typed one, and both for a speculated load. */
  for (i = 0; i < fn->nvalues; i++) {
    const Q2Value *v = &fn->values[i];
    switch (v->rep) {
      case Q2_R_I32:
        fprintf(f, "    int32_t i%d = 0;\n", i);
        break;
      case Q2_R_F64:
        fprintf(f, "    double d%d = 0.0;\n", i);
        break;
      case Q2_R_BOOL:
        fprintf(f, "    int b%d = 0;\n", i);
        break;
      default:
        fprintf(f, "    JSValue v%d = JS_UNDEFINED;\n", i);
        break;
    }
    /* A synced typed value keeps its boxed form in a JSValue local, so a
       tagged consumer reads it instead of re-boxing, and a write-back never
       touches it. */
    if (v->spec || (v->rep != Q2_R_TAGGED && v->synced))
      fprintf(f, "    JSValue v%d = JS_UNDEFINED;\n", i);
  }
  {
    int maxp = 0;
    for (b = 0; b < fn->nblocks; b++)
      if (fn->blocks[b].nparams > maxp) maxp = fn->blocks[b].nparams;
    for (i = 0; i < maxp; i++) {
      fprintf(f, "    JSValue ev%d;\n", i);
      fprintf(f, "    int32_t ei%d = 0;\n", i);
      fprintf(f, "    double ed%d = 0.0;\n", i);
      fprintf(f, "    int eb%d = 0;\n", i);
    }
  }

  /* The entry block reads its parameters from the frame. */
  q2_cur_block(fn, &g_em.cur, 0);
  for (j = 0; j < fn->blocks[0].nparams; j++) {
    Q2ValueId pv = fn->blocks[0].param_val[j];
    const Q2Value *v = &fn->values[pv];
    int off = fn->blocks[0].param_reg[j] * 16;
    if (v->rep == Q2_R_TAGGED) {
      fprintf(f, "    v%u = AOT_REG(fr, %d);\n", pv, off);
    } else {
      fprintf(f, "    v%u = AOT_REG(fr, %d);\n", pv, off);
      if (v->rep == Q2_R_I32)
        fprintf(f, "    i%u = JS_VALUE_GET_INT(v%u);\n", pv, pv);
      else if (v->rep == Q2_R_F64)
        fprintf(
            f,
            "    if (JS_VALUE_GET_TAG(v%u) == JS_TAG_INT) d%u = "
            "JS_VALUE_GET_INT(v%u);\n"
            "    else d%u = JS_VALUE_GET_FLOAT64(v%u);\n",
            pv, pv, pv, pv, pv);
      else
        fprintf(f, "    b%u = JS_VALUE_GET_BOOL(v%u);\n", pv, pv);
    }
  }

  for (b = 0; b < fn->nblocks; b++) {
    const Q2Block *blk = &fn->blocks[b];
    fprintf(f, "b_%d:\n", blk->start_pc);
    g_em.cur_block = b;
    /* Each block starts from its own live-in values, never from whatever block
       happened to be emitted before it. */
    q2_cur_block(fn, &g_em.cur, b);
    for (j = 0; j < blk->ninsns; j++) {
      const Q2Insn *in = &fn->insns[blk->first_insn + j];
      int k;
      g_em.cur_insn = blk->first_insn + j;
      /* `pre` is the state before this instruction; `cur` gains its
         definitions before the emitter runs, because a fallback's write-back
         stores what `cur` names. */
      g_em.pre = g_em.cur;
      for (k = 0; k < in->ndef; k++)
        q2_cur_def(&g_em.cur, fn->values[in->def[k]].reg, in->def[k]);
      for (k = 0; k < in->nredef; k++)
        q2_cur_def(&g_em.cur, in->redef_reg[k], in->redef_val[k]);
      /* The live-before set is the write-back set of a pre-instruction leave. */
      q2_live_before(fn, g_em.cur_insn, g_em.liveb);
      if (q2_emit_native(f, fn, b, in) == 0)
        q2_emit_fallback(f, fn, b, in, fn->exit_pc);
      for (k = 0; k < in->ndef; k++) q2_spec_after(f, fn, in, k);
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
  free(g_em.liveb);
  g_em.liveb = NULL;
}

int qjsaot2_emit_body(FILE *f, const JSAotGenFunc *g, char hex[33]) {
  Q2Func fn;
  char msg[256];
  int rc;

  if (q2_build(g, &fn) < 0) return -1;
  if (q2_infer(&fn) < 0) {
    q2_free(&fn);
    return -1;
  }
  if (q2_verify(&fn, msg, sizeof(msg)) < 0) {
    fprintf(
        stderr, "qjsaot2: %s: the IR failed to verify: %s\n",
        g->name[0] ? g->name : "?", msg);
    q2_free(&fn);
    return -1;
  }
  g_operand_err = 0;
  q2_emit_body(f, &fn, hex);
  if (g_operand_err) {
    fprintf(
        stderr, "qjsaot2: %s: the IR failed to verify: %s\n",
        g->name[0] ? g->name : "?", g_operand_err_msg);
    q2_free(&fn);
    return -1;
  }
  rc = fn.ninsns;
  q2_free(&fn);
  return rc;
}
