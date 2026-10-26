/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * The per-function driver of the emitter and the native entry a compiled caller
 * enters a function through.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "analysis.h"
#include "emit.h"
#include "ir.h"

/* When set, compiled functions get external (hidden) linkage instead of
   `static`, so a unit split across files can call between them. Set by the
   unit builder for `--split`. */
static int e_split;
void e_set_split(int on) {
  e_split = on ? 1 : 0;
}
int e_split_on(void) {
  return e_split;
}
const char *e_linkage(void) {
  return e_split ? "__attribute__((visibility(\"hidden\"))) " : "static ";
}

/* The call-site A/B switch (--nocall). */
static int e_nocall;
void e_set_nocall(int on) {
  e_nocall = on ? 1 : 0;
}
int e_nocall_on(void) {
  return e_nocall;
}

int qjs_aot_inline_uses_nx(const char hex[33]);
/* The driver's profile, for the function's dynamic weight. */
double qjs_aot_prof_weight(const uint8_t dg[16]);

/* --- the entry rules: the poll and the stack check ---------------------- */

/* Whether the function contains a call site: an op that can reach a call. The
   role table marks one with an argument window (the format's trailing `b`),
   which `call`, `call_method`, `tail_call*`, `construct`, `apply*` and `eval`
   all carry and nothing else does. */
int e_has_call_site(const Q2Func *fn) {
  int i;
  for (i = 0; i < fn->ninsns; i++) {
    const ERole *r = e_roles[fn->insns[i].rop];
    if (r == NULL) continue;
    if (r->window >= 0) return 1;
  }
  return 0;
}

/* --- the framed native entry -------------------------------------------- */

/* Emit `aot_n_HEX` for a function a compiled caller may enter on the C stack:
   it builds the callee's light frame in its own stack buffer and runs the
   speculative body on it, so the whole call is one C call.

   The frame is the one `regvm_light_fill` builds, field for field, because
   every handler, cold arm and deopt path reads it. The few pieces that are not
   a plain store come from the engine (patch 0307): the seal, the stack-overflow
   RangeError and the variable-area release.

   The poll (an interrupt-counter decrement) and the stack check are emitted only for a function that
   contains a call site, or, for the stack check, when its own frame is over 16 KB.
   Recursion needs a call site in every function on the cycle, so every cycle
   still polls once per round; loops poll on their back edges; an op call that
   runs JS polls inside the engine. */
void e_emit_native_entry(FILE *f, const Q2Func *fn, char hex[33]) {
  const JSAotGenFunc *g = fn->g;
  int vc = g->light_var_count;
  int call_site = e_has_call_site(fn);
  int check = call_site || g->light_need > 16 * 1024;
  int nregs = g->reg_count;
  /* The variable area exists only when the promotion map leaves a slot in it,
     and the frame's layout follows: the engine sizes `fr` the same way. */
  int has_var = vc > 0;
  int slots = (g->light_need + (int)sizeof(JSValue) - 1) / (int)sizeof(JSValue);

  e_report_entry(g, hex, 0, call_site, check);

  fprintf(
      f,
      "/* The native entry: the whole call is this C call. NEED = %d bytes,\n"
      "   %d slots, variable area %s (%d of %d slots), %d registers. */\n",
      g->light_need, slots, has_var ? "present" : "absent", has_var ? vc : 0,
      g->var_count, nregs);

  fprintf(
      f,
      "E_TEXT %sJSValue aot_n_%s(JSContext *ctx, JSRegRun *caller_f,\n"
      "                               JSObject *co, JSValue this_obj,\n"
      "                               JSValue *argv, int argc)\n"
      "{\n",
      e_linkage(), hex);
  /* The frame. Over-aligned because the register file inside it is read as
     JSValue and the engine's own frames are 16-byte aligned. */
  fprintf(
      f,
      "    JSValue mem[%d] __attribute__((aligned(16)));\n"
      "    uint8_t *nf = (uint8_t *)mem;\n"
      "    uint8_t *cb = *(uint8_t **)((uint8_t *)co + "
      "QJS_AOT_OFF_OBJECT_FUNC_BYTECODE);\n"
      "    JSContext *realm = *(JSContext **)(cb + QJS_AOT_OFF_FNBC_REALM);\n"
      "    uint8_t *rt = *(uint8_t **)((uint8_t *)ctx + QJS_AOT_OFF_CTX_RT);\n"
      "    JSValue *vb = mem + QJS_AOT_LIGHT_PREFIX_SLOTS;\n"
      "    JSValue *regs = vb + %d;\n"
      "    JSRegRun *prev;\n"
      "    JSValue ret;\n"
      "    int k;\n"
      "\n",
      slots, has_var ? g->var_count : 0);

  /* The stack check. The margin leaves room for the throw itself; a
     4 KB margin crashed before; 64 KB has held. */
  if (check) {
    fprintf(
        f,
        "    if (__builtin_expect((uintptr_t)__builtin_frame_address(0) <\n"
        "            *(uintptr_t *)(rt + QJS_AOT_OFF_RT_STACK_LIMIT) + "
        "E_STACK_MARGIN, 0))\n"
        "        return qjs_aot_throw_stack_overflow(ctx);\n");
  }
  /* The poll. `qjs_aot_poll` decrements the counter itself, so undo
     this one first: the counter must drop by exactly one per call. */
  if (call_site) {
    fprintf(
        f,
        "    if (__builtin_expect(--*(int *)((uint8_t *)ctx + "
        "QJS_AOT_OFF_CTX_INTERRUPT_COUNTER) <= 0, 0)) {\n"
        "        ++*(int *)((uint8_t *)ctx + "
        "QJS_AOT_OFF_CTX_INTERRUPT_COUNTER);\n"
        "        if (qjs_aot_poll(ctx) < 0) return JS_EXCEPTION;\n"
        "    }\n");
  }

  /* The callee's inline handlers read its property-cache table with no NULL
     test, so it must exist before the frame is published. This is a safepoint
     (it can allocate), which is why it is here rather than inside the body. */
  /* The common case is a table that already exists (a bound function with
     sites gets one at bind), so the test is inline and the helper -- a
     cross-unit call -- runs only when the table is actually missing. */
  /* Only a function with property-cache sites has a table to ensure: for any other
     the pointer is NULL on every call and the helper would run on every call. */
  if (g->pic_count > 0)
    fprintf(
        f,
        "    if (__builtin_expect(*(void **)(cb + QJS_AOT_OFF_FNBC_REGVM_PIC) == "
        "NULL, 0)) {\n"
        "        if (qjs_aot_pic_ensure(ctx, co) < 0) return JS_EXCEPTION;\n"
        "    }\n");

  /* The header, in the engine's own field order. `ret_ip` is NULL and the
     destination field of `hdr_pack` is 0: a compiled call returns to C, not
     into a register of an interpreted caller. */
  fprintf(
      f,
      "    *(JSRegRun **)(nf + QJS_AOT_OFF_REGRUN_CALLER) = caller_f;\n"
      "    *(void **)(nf + QJS_AOT_OFF_REGRUN_RET_IP) = NULL;\n"
      "    *(void **)(nf + QJS_AOT_OFF_REGRUN_B) = cb;\n"
      "    *(void **)(nf + QJS_AOT_OFF_REGRUN_PIC) = *(void **)(cb + "
      "QJS_AOT_OFF_FNBC_REGVM_PIC);\n"
      "    *(JSValue **)(nf + QJS_AOT_OFF_REGRUN_FR) = regs;\n"
      "    *(void **)(nf + QJS_AOT_OFF_REGRUN_VAR_REFS) =\n"
      "        *(void **)((uint8_t *)co + QJS_AOT_OFF_OBJECT_FUNC_VAR_REFS);\n"
      "    *(JSValue **)(nf + QJS_AOT_OFF_REGRUN_VAR_BUF) = %s;\n"
      "    *(JSValue **)(nf + QJS_AOT_OFF_REGRUN_ARG_BUF) = argv;\n"
      "    *(uint64_t *)(nf + QJS_AOT_OFF_REGRUN_HDR_PACK) =\n"
      "        ((uint64_t)(uint16_t)argc << 16) |\n"
      "        ((uint64_t)(uint32_t)(QJS_AOT_REGVM_FLAG_LIGHT |\n"
      "                    QJS_AOT_REGVM_FLAG_C_ENTRY |\n"
      "                    QJS_AOT_REGVM_FLAG_STACK | %s |\n"
      "                    (realm == ctx ? QJS_AOT_REGVM_FLAG_SAME_REALM : 0)) "
      "<< 32);\n"
      "    *(JSObject **)(nf + QJS_AOT_OFF_REGRUN_FUNC) = co;\n"
      "    *(void **)(nf + QJS_AOT_OFF_REGRUN_CAP_IP) = NULL;\n"
      "    *(void **)(nf + QJS_AOT_OFF_REGRUN_SF) = NULL;\n"
      "    *(JSValue *)(nf + QJS_AOT_OFF_REGRUN_THIS_OBJ) = this_obj;\n",
      has_var ? "vb" : "NULL",
      has_var ? "0" : "QJS_AOT_REGVM_FLAG_TRIVIAL_RET");

  /* Fill the variable area and the register file with `undefined`: a handler
     reads every register its format declares, so a stale stack word would be a
     value out of nowhere. Only the promoted variable slots are filled, as the
     engine fills only those. */
  fprintf(
      f,
      "    for (k = 0; k < %d; k++)\n"
      "        __builtin_memcpy(&vb[k], &e_undefined_value, "
      "sizeof(JSValue));\n",
      has_var ? g->var_count : 0);
  fprintf(
      f,
      "    for (k = 0; k < %d; k++)\n"
      "        __builtin_memcpy(&regs[k], &e_undefined_value, "
      "sizeof(JSValue));\n",
      nregs);
  fprintf(f, "    (void)k; (void)vb; (void)cb;\n");
  /* `regvm_cur` names the innermost light frame, so the collector finds its
     register window and a deopt or an uncaught throw inside the callee finds
     the chain. Restored on the way out. */
  fprintf(
      f,
      "    prev = *(JSRegRun **)(rt + QJS_AOT_OFF_RT_REGVM_CUR);\n"
      "    *(JSRegRun **)(rt + QJS_AOT_OFF_RT_REGVM_CUR) = (JSRegRun *)nf;\n"
      "    ret = aot_f_%s(realm, (JSRegRun *)nf, regs, 0);\n"
      "    *(JSRegRun **)(rt + QJS_AOT_OFF_RT_REGVM_CUR) = prev;\n",
      hex);
  /* The frame is the caller's buffer, so the teardown is the release of what
     the variable area owns and nothing else. */
  if (has_var)
    fprintf(f, "    qjs_aot_light_release(realm, (JSRegRun *)nf);\n");
  fprintf(f, "    return ret;\n}\n\n");
}

/* --- the frameless entry -------------------------------------- */

/* `aot_nx_HEX`: the exit of a frameless body. It builds the frame the framed
   native entry builds, field for field, puts the live registers the body hands
   over (`x->lv`, at the frame offsets `x->offs`) where the generic body reads
   them, and continues in the generic body at `x->pc`. Cold: the fast path never
   gets here. The function has no variable area and no call site (rules 2 and 3),
   so there is no stack check, no poll and no release; the frame is small (rule 1).
   It takes ONE pointer for everything about the exit, so that leaving costs the
   body no more arguments than entering did. */
void e_emit_frameless_exit_fn(FILE *f, const Q2Func *fn, char hex[33]) {
  const JSAotGenFunc *g = fn->g;
  int nregs = g->reg_count;
  int slots = (g->light_need + (int)sizeof(JSValue) - 1) / (int)sizeof(JSValue);
  /* A function the engine does not call light has no frame size of its own; the
     exit builds the same frame (prefix, then the registers; no variable area, which
     frameless entry rules out). */
  char slotsx[64];
  if (g->light_ok)
    snprintf(slotsx, sizeof(slotsx), "%d", slots);
  else
    snprintf(slotsx, sizeof(slotsx), "QJS_AOT_LIGHT_PREFIX_SLOTS + %d", nregs > 0 ? nregs : 1);

  fprintf(
      f,
      "/* The exit of the frameless body: builds the frame and runs the generic\n"
      "   body from x->pc with the registers' values handed over in x->lv. */\n"
      "E_TEXT %s__attribute__((noinline, cold)) JSValue aot_nx_%s(\n"
      "    JSContext *ctx, JSRegRun *caller_f, JSObject *co, JSValue this_obj,\n"
      "    JSValue *argv, const ENx *xp)\n"
      "{\n"
      "    const ENx xv = *xp;\n"
      "    const ENx *x = &xv;\n"
      "    JSValue mem[%s] __attribute__((aligned(16)));\n"
      "    uint8_t *nf = (uint8_t *)mem;\n"
      "    uint8_t *cb = *(uint8_t **)((uint8_t *)co + "
      "QJS_AOT_OFF_OBJECT_FUNC_BYTECODE);\n"
      "    JSContext *realm = *(JSContext **)(cb + QJS_AOT_OFF_FNBC_REALM);\n"
      "    uint8_t *rt = *(uint8_t **)((uint8_t *)ctx + QJS_AOT_OFF_CTX_RT);\n"
      "    JSValue *regs = mem + QJS_AOT_LIGHT_PREFIX_SLOTS;\n"
      "    int argc = x->argc;\n"
      "    JSRegRun *prev;\n"
      "    JSValue ret;\n"
      "    int k;\n"
      "\n"
      "    if (x->rec != NULL) E_FL_TOP(ctx) = x->rec->prev;\n"
      "    if (__builtin_expect(e_exit_log_v != 0, 0) && x->kind != NULL) {\n"
      "        static int seen_[1024];\n"
      "        int *sp_ = &seen_[(x->pc * 7 + x->kind[0]) & 1023];\n"
      "        if (!*sp_) e_exit_log(\"%s\", x->kind, x->pc, x->tag, sp_);\n"
      "    }\n"
      "    if (%s__builtin_expect(*(void **)(cb + QJS_AOT_OFF_FNBC_REGVM_PIC) == "
      "NULL, 0)) {\n"
      "        if (qjs_aot_pic_ensure(ctx, co) < 0) return JS_EXCEPTION;\n"
      "    }\n"
      "    *(JSRegRun **)(nf + QJS_AOT_OFF_REGRUN_CALLER) = caller_f;\n"
      "    *(void **)(nf + QJS_AOT_OFF_REGRUN_RET_IP) = NULL;\n"
      "    *(void **)(nf + QJS_AOT_OFF_REGRUN_B) = cb;\n"
      "    *(void **)(nf + QJS_AOT_OFF_REGRUN_PIC) = *(void **)(cb + "
      "QJS_AOT_OFF_FNBC_REGVM_PIC);\n"
      "    *(JSValue **)(nf + QJS_AOT_OFF_REGRUN_FR) = regs;\n"
      "    *(void **)(nf + QJS_AOT_OFF_REGRUN_VAR_REFS) =\n"
      "        *(void **)((uint8_t *)co + QJS_AOT_OFF_OBJECT_FUNC_VAR_REFS);\n"
      "    *(JSValue **)(nf + QJS_AOT_OFF_REGRUN_VAR_BUF) = NULL;\n"
      "    *(JSValue **)(nf + QJS_AOT_OFF_REGRUN_ARG_BUF) = argv;\n"
      "    *(uint64_t *)(nf + QJS_AOT_OFF_REGRUN_HDR_PACK) =\n"
      "        ((uint64_t)(uint16_t)argc << 16) |\n"
      "        ((uint64_t)(uint32_t)(QJS_AOT_REGVM_FLAG_LIGHT |\n"
      "                    QJS_AOT_REGVM_FLAG_C_ENTRY |\n"
      "                    QJS_AOT_REGVM_FLAG_STACK |\n"
      "                    QJS_AOT_REGVM_FLAG_TRIVIAL_RET |\n"
      "                    (realm == ctx ? QJS_AOT_REGVM_FLAG_SAME_REALM : 0)) "
      "<< 32);\n"
      "    *(JSObject **)(nf + QJS_AOT_OFF_REGRUN_FUNC) = co;\n"
      "    *(void **)(nf + QJS_AOT_OFF_REGRUN_CAP_IP) = NULL;\n"
      "    *(void **)(nf + QJS_AOT_OFF_REGRUN_SF) = NULL;\n"
      "    *(JSValue *)(nf + QJS_AOT_OFF_REGRUN_THIS_OBJ) = this_obj;\n"
      "    for (k = 0; k < %d; k++)\n"
      "        __builtin_memcpy(&regs[k], &e_undefined_value, "
      "sizeof(JSValue));\n"
      "    for (k = 0; k < x->n; k++) AOT_REG(regs, x->offs[k]) = x->lv[k];\n"
      "    prev = *(JSRegRun **)(rt + QJS_AOT_OFF_RT_REGVM_CUR);\n"
      "    *(JSRegRun **)(rt + QJS_AOT_OFF_RT_REGVM_CUR) = (JSRegRun *)nf;\n"
      "    ret = aot_g_%s(realm, (JSRegRun *)nf, regs, (uint32_t)x->pc);\n"
      "    *(JSRegRun **)(rt + QJS_AOT_OFF_RT_REGVM_CUR) = prev;\n"
      "    return ret;\n"
      "}\n\n",
      e_linkage(), hex, slotsx, hex, g->pic_count > 0 ? "" : "0 && ", nregs, hex);
}

/* `aot_f_HEX` for a frameless function: the engine still enters through it with a
   frame of its own, and there is no second speculative body, so it forwards to
   `aot_n_HEX` with what the frame holds. The frame's header is the engine's:
   `hdr_pack` carries the argument count in bits 16..31, and `arg_buf` points at
   the arguments the caller passed (`argc` of them). A call with fewer than the
   declared arguments, or a resume at a pc, takes the generic body. */
void e_emit_frameless_forward(FILE *f, const Q2Func *fn, char hex[33]) {
  fprintf(
      f,
      "E_TEXT %sJSValue aot_f_%s(JSContext *ctx, JSRegRun *f,\n"
      "                                 JSValue *fr, uint32_t entry_pc)\n"
      "{\n"
      "    int argc = (uint16_t)(*(uint64_t *)((uint8_t *)f +\n"
      "                          QJS_AOT_OFF_REGRUN_HDR_PACK) >> 16);\n"
      "    if (__builtin_expect(entry_pc != 0 || argc < %d, 0))\n"
      "        return aot_g_%s(ctx, f, fr, entry_pc);\n"
      "    return aot_n_%s(ctx, f,\n"
      "        *(JSObject **)((uint8_t *)f + QJS_AOT_OFF_REGRUN_FUNC),\n"
      "        *(JSValue *)((uint8_t *)f + QJS_AOT_OFF_REGRUN_THIS_OBJ),\n"
      "        *(JSValue **)((uint8_t *)f + QJS_AOT_OFF_REGRUN_ARG_BUF), argc);\n"
      "}\n\n",
      e_linkage(), hex, fn->g->arg_count, hex, hex);
}

/* --- the driver --------------------------------------------------------- */

/* Whether every arithmetic instruction of a copied callee is typed: a copy has no step engine
   behind it, so an operation on a value of unknown type could not fall back. */
static int e_inline_typed_ok(const Q2Func *fn) {
  EFacts facts;
  ETypes types;
  int i, k, ok = 1;
  if (fn->ninl == 0) return 1;
  memset(&facts, 0, sizeof(facts));
  memset(&types, 0, sizeof(types));
  if (e_analyze(fn, &facts, &types) < 0) {
    if (getenv("QJS_AOT_DEBUG_INL")) fprintf(stderr, "  copy: analysis failed\n");
    return 0;
  }
  for (i = 0; ok && i < fn->ninsns; i++) {
    const Q2Insn *in = &fn->insns[i];
    if (in->own == 0) continue;
    /* a call that stays a call, and the store of its result, are made by the call's own emission */
    if (in->extcall) continue;
    /* a forwarded load is a copy of a value already held: no access, nothing to miss */
    if (facts.fwd != NULL && facts.fwd[i] != Q2_NO_VALUE) continue;
    /* a field read of a copy has to be a grouped access whose miss exits */
    if (in->op == Q2_OP_GET_FIELD && !facts.miss_exits[i]) {
      if (getenv("QJS_AOT_DEBUG_INL")) fprintf(stderr, "  copy: field read at clone pc %d (orig %d) is not a grouped exit\n", in->pc, in->opc);
      ok = 0;
      continue;
    }
    /* a store of a copy is a grouped write whose miss leaves before it */
    if (in->op == Q2_OP_PUT_FIELD && !facts.miss_exits[i]) {
      if (getenv("QJS_AOT_DEBUG_INL")) fprintf(stderr, "  copy: field write at clone pc %d (orig %d) is not a grouped exit\n", in->pc, in->opc);
      ok = 0;
      continue;
    }
    /* a polymorphic site is served by a cache that fills from the frame, which a copy has none of */
    if (in->op == Q2_OP_GET_FIELD || in->op == Q2_OP_PUT_FIELD) {
      int sa;
      for (sa = 0; sa < in->narg; sa++)
        if (in->arg[sa].kind == 's' && (unsigned)in->arg[sa].val != 0xffff && e_facts_site_poly(&facts, (int)in->arg[sa].val)) {
          if (getenv("QJS_AOT_DEBUG_INL")) fprintf(stderr, "  copy: polymorphic field access at clone pc %d\n", in->pc);
          ok = 0;
        }
      if (!ok) continue;
    }
    /* a nullish test reads the tag of any value */
    if (in->rop == ROP_branch_nullish || in->rop == ROP_branch_not_nullish) continue;
    if (in->op != Q2_OP_BINARY && in->op != Q2_OP_COMPARE && in->op != Q2_OP_UNARY &&
        in->op != Q2_OP_BRANCH_CMP && in->op != Q2_OP_BRANCH &&
        !(in->op == Q2_OP_FALLBACK && (in->rop == ROP_eq || in->rop == ROP_neq ||
                                       in->rop == ROP_strict_eq || in->rop == ROP_strict_neq)))
      continue;
    /* a remainder has no typed form: it is lowered only on tagged operands, with the step
       engine behind it */
    if (in->op == Q2_OP_BINARY && in->sub == REG_BIN_MOD) {
      if (getenv("QJS_AOT_DEBUG_INL")) fprintf(stderr, "  copy: remainder at clone pc %d\n", in->pc);
      ok = 0;
      continue;
    }
    for (k = 0; k < in->nuse; k++)
      if (in->use[k] >= (Q2ValueId)types.nvalues || types.lb[in->use[k]] == E_LB_TAG ||
          types.lb[in->use[k]] == E_LB_BOT) {
        if (getenv("QJS_AOT_DEBUG_INL")) {
          const Q2Value *uv = &fn->values[in->use[k]];
          fprintf(stderr, "  copy: operand v%d of clone pc %d rop %u (orig %d) is not typed (lb=%d); defined at insn %d rop %u pc %d (has_prof=%d ptypes=0x%x ppc=%d)\n",
                  (int)in->use[k], in->pc, in->rop, in->opc, types.lb[in->use[k]], uv->insn, uv->insn >= 0 ? fn->insns[uv->insn].rop : 0, uv->insn >= 0 ? fn->insns[uv->insn].pc : -1, uv->has_prof, uv->ptypes, uv->insn >= 0 ? fn->insns[uv->insn].ppc : -1);
        }
        ok = 0;
      }
    for (k = 0; k < in->ndef; k++)
      if (in->def[k] >= (Q2ValueId)types.nvalues || types.lb[in->def[k]] == E_LB_TAG ||
          types.lb[in->def[k]] == E_LB_BOT) {
        if (getenv("QJS_AOT_DEBUG_INL"))
          fprintf(stderr, "  copy: result v%d of clone pc %d rop %u (orig %d) is not typed (lb=%d) op=%d sub=%d nuse=%d use0lb=%d\n", (int)in->def[k], in->pc, in->rop, in->opc, in->def[k] < (Q2ValueId)types.nvalues ? types.lb[in->def[k]] : -1, in->op, in->sub, in->nuse, in->nuse ? types.lb[in->use[0]] : -1);
        ok = 0;
      }
  }
  if (!ok && getenv("QJS_AOT_DEBUG_INL")) fprintf(stderr, "  copy: not typed\n");
  if (!ok && getenv("QJS_AOT_DUMP_INL")) e_analysis_dump(stderr, fn, "inl", 0.0);
  e_facts_free(&facts);
  e_types_free(&types);
  return ok;
}

/* The function the speculative body is emitted from, when it is not the program itself: its
   loop-invariant global reads hoisted, hot calls to small callees replaced by copies of their
   instructions, and objects of plain constructors that never escape left unmade. A copy that
   builds an object is kept only if every such object is left unmade; otherwise the callees that
   build objects are not copied. Returns 1 with *out built, 0 when the program is used as it is. */
static int e_build_spec_fn(
    const JSAotGenFunc *g, Q2Func *fn, int allow_ctor, Q2Func *out, char *msg, size_t msglen) {
  Q2Extra *ex = NULL;
  Q2InlReq *ir = NULL;
  int nir, nex, have2 = 0, dbg = getenv("QJS_AOT_DEBUG_VIRT") != NULL;
  Q2Func fn2;
  e_inline_allow_construct = allow_ctor;
  nir = e_plan_inline(fn, &ir);
  nex = nir > 0 ? 0 : e_plan_licm(fn, &ex);
  if (getenv("QJS_AOT_DEBUG_LICM") != NULL) fprintf(stderr, "licm %s: %d hoists\n", g->name, nex);
  if (nex > 0 || nir > 0) {
    int st = 0;
    if (q2_build_inl(g, ex, nex, ir, nir, &fn2) == 0 && (st = 1) && q2_infer(&fn2) == 0 && (st = 2) &&
        q2_verify(&fn2, msg, msglen) == 0 && (st = 3) && e_inline_typed_ok(&fn2))
      have2 = 1;
    if (!have2) {
      if (getenv("QJS_AOT_DEBUG_LICM") != NULL)
        fprintf(stderr, "licm %s: stage %d failed: %s %s\n", g->name, st, fn2.why ? fn2.why : "-", msg);
      q2_free(&fn2);
    }
  }
  {
    /* Objects of a plain constructor that never escape are not made. The plan reads the function
       the speculative body would be built from, and the body is built again with the objects
       rewritten. */
    Q2VWeb *webs = NULL;
    Q2Rewrite *rws = NULL;
    int nwebs = 0, nrws = 0;
    if (e_plan_virtual(have2 ? &fn2 : fn, &webs, &nwebs, &rws, &nrws) > 0 && nwebs > 0) {
      Q2Func fn3;
      int st = 0, ok3 = 0;
      if (q2_build_ea(g, ex, nex, ir, nir, webs, nwebs, rws, nrws, &fn3) == 0 && (st = 1) && q2_infer(&fn3) == 0 &&
          (st = 2) && q2_verify(&fn3, msg, msglen) == 0 && (st = 3) && e_inline_typed_ok(&fn3))
        ok3 = 1;
      if (!ok3) {
        if (dbg) fprintf(stderr, "virt %s: stage %d failed: %s %s\n", g->name, st, fn3.why ? fn3.why : "-", msg);
        q2_free(&fn3);
      } else {
        if (dbg) fprintf(stderr, "virt %s: %d webs\n", g->name, nwebs);
        if (getenv("QJS_AOT_DUMP_VIRT") != NULL) {
          int di, dk;
          for (di = 0; di < fn3.ninsns; di++) {
            const Q2Insn *x = &fn3.insns[di];
            fprintf(stderr, "  [%d] pc %d bpc %d rop %u vk %d own %d xpc %d def", di, x->pc, x->bpc, x->rop, x->vk, x->own, x->xpc);
            for (dk = 0; dk < x->ndef; dk++) fprintf(stderr, " v%d(r%d)", (int)x->def[dk], fn3.values[x->def[dk]].reg);
            fprintf(stderr, " use");
            for (dk = 0; dk < x->nuse; dk++) fprintf(stderr, " v%d(r%d)", (int)x->use[dk], fn3.values[x->use[dk]].reg);
            fprintf(stderr, "\n");
          }
          for (di = 0; di < fn3.nblocks; di++) {
            fprintf(stderr, "  block %d pc %d..%d live-in:", di, fn3.blocks[di].start_pc, fn3.blocks[di].end_pc);
            for (dk = 0; dk < fn3.blocks[di].nlive_in; dk++) fprintf(stderr, " r%d=v%d", fn3.blocks[di].in_reg[dk], (int)fn3.blocks[di].in_val[dk]);
            fprintf(stderr, " succ:");
            for (dk = 0; dk < fn3.blocks[di].nsucc; dk++) fprintf(stderr, " %d", fn3.blocks[di].succ_block[dk]);
            fprintf(stderr, "\n");
          }
        }
        if (have2) q2_free(&fn2);
        fn2 = fn3;
        have2 = 1;
      }
    }
    free(webs);
    free(rws);
  }
  free(ex);
  free(ir);
  if (have2 && allow_ctor) {
    /* a copy that builds an object nobody removed would run an allocation without a frame behind it */
    int i;
    for (i = 0; i < fn2.ninsns; i++)
      if (fn2.insns[i].own > 0 && fn2.insns[i].rop == ROP_construct && fn2.insns[i].vk != Q2_VK_DEF) {
        if (dbg) fprintf(stderr, "virt %s: a copied constructor call stays; copying without constructors\n", g->name);
        q2_free(&fn2);
        return e_build_spec_fn(g, fn, 0, out, msg, msglen);
      }
  }
  if (have2) *out = fn2;
  return have2;
}

int e_emit_body(FILE *f, const JSAotGenFunc *g, char hex[33]) {
  Q2Func fn;
  char msg[256];
  int rc, ninsns;

  /* Every failure below names its own reason. The caller only knows the body
     emitter returned -1, and reports that as "malformed program" -- which is
     true of none of these cases. `q2_build` and `q2_infer` set `fn.why`, and a
     diagnostic that says only "malformed" sends the reader to the decoder when
     the cause may be a role table, a verify rule or a lowered switch. */
  if (q2_build(g, &fn) < 0) {
    fprintf(
        stderr, "qjsaot: %s: the IR could not be built: %s\n",
        g->name[0] ? g->name : "?", fn.why ? fn.why : "(no reason recorded)");
    q2_free(&fn);
    return -1;
  }
  if (q2_infer(&fn) < 0) {
    fprintf(
        stderr, "qjsaot: %s: the IR could not be inferred: %s\n",
        g->name[0] ? g->name : "?", fn.why ? fn.why : "(no reason recorded)");
    q2_free(&fn);
    return -1;
  }
  if (q2_verify(&fn, msg, sizeof(msg)) < 0) {
    fprintf(
        stderr, "qjsaot: %s: the IR failed to verify: %s\n",
        g->name[0] ? g->name : "?", msg);
    q2_free(&fn);
    return -1;
  }

  /* The generic body first: the speculative body forwards to it for everything
     it does not lower. */
  rc = e_emit_generic_body(f, &fn, hex);
  if (rc != 0) {
    fprintf(
        stderr, "qjsaot: %s: the generic body emitter returned %d\n",
        g->name[0] ? g->name : "?", rc);
  } else {
    /* The speculative body is built from the program with its loop-invariant global reads
       moved before their loops, copies of small callees, and objects that never escape left
       unmade; the generic body keeps the program as it is. */
    Q2Func fn2;
    int have2 = e_build_spec_fn(g, &fn, 1, &fn2, msg, sizeof(msg));
    e_pure_plan(&fn, hex);
    rc = e_emit_spec_body(f, have2 ? &fn2 : &fn, hex);
    e_pure_clear();
    /* An inline body of this function that exits through aot_nx_ needs its
       definition; a frameless body has emitted it already. */
    if (rc == 0 && !e_fl_was_chosen() && qjs_aot_inline_uses_nx(hex))
      e_emit_frameless_exit_fn(f, have2 ? &fn2 : &fn, hex);
    if (have2) q2_free(&fn2);
    if (rc != 0) {
      fprintf(
          stderr, "qjsaot: %s: the speculative body emitter returned %d\n",
          g->name[0] ? g->name : "?", rc);
    }
  }
  /* The unit builder must know a native entry exists, because a call site
     elsewhere in the unit may only name an `aot_n_` that some body defines. */
  if (rc == 0 && e_fl_was_chosen()) {
    /* The body IS the native entry: it was emitted as `aot_n_`, with its exit
       function and the forwarding `aot_f_`. */
    e_report_entry(g, hex, 1, 0, 0);
    e_report_func(g, hex, qjs_aot_prof_weight(g->digest), 0);
  } else if (rc == 0 && g->light_ok && g->light_need <= E_NATIVE_FRAME_MAX) {
    e_emit_native_entry(f, &fn, hex);
    e_report_func(g, hex, qjs_aot_prof_weight(g->digest), 0);
  } else if (rc == 0) {
    e_report_func(g, hex, qjs_aot_prof_weight(g->digest), 0);
  }
  /* The intrinsic sites, for the report. */
  if (rc == 0) {
    int ii;
    for (ii = 0; ii < fn.ninsns; ii++) {
      const Q2Insn *in = &fn.insns[ii];
      int id;
      if (in->rop != ROP_call && in->rop != ROP_call_method) continue;
      id = e_intrinsic_site(g->digest, in->next);
      if (id >= 0)
        e_report_intrinsic(
            hex, in->pc, id, in->win_n,
            e_intrinsic_argc_ok(id, in->win_n) && e_intrinsic_pure(id));
    }
  }
  ninsns = fn.ninsns;
  q2_free(&fn);
  if (rc != 0) return rc;
  if ((g->light_ok && g->light_need <= E_NATIVE_FRAME_MAX) || e_fl_was_chosen())
    qjs_aot_note_native_entry(qjs_aot_cur_func_idx());
  return ninsns;
}

/* The inline form of a function: `ei_HEX` written to `f` when the function
   is a candidate, else 0 with `why` set. Called once per function before any body
   is emitted, so a caller's emission can ask whether its callee has one. */
int e_emit_inline_func(
    FILE *f, const JSAotGenFunc *g, char hex[33], char *why, size_t whyn) {
  Q2Func fn;
  char msg[256];
  int rc;
  if (q2_build(g, &fn) < 0) {
    q2_free(&fn);
    snprintf(why, whyn, "the IR could not be built");
    return 0;
  }
  if (q2_infer(&fn) < 0 || q2_verify(&fn, msg, sizeof(msg)) < 0) {
    q2_free(&fn);
    snprintf(why, whyn, "the IR did not verify");
    return 0;
  }
  if (getenv("QJS_AOT_NOEICOPY") == NULL) {
    Q2Func fn2;
    int have2;
    e_inline_allow_ext = 0;
    have2 = e_build_spec_fn(g, &fn, 1, &fn2, msg, sizeof(msg));
    e_inline_allow_ext = 1;
    rc = e_emit_inline_body(f, have2 ? &fn2 : &fn, hex, why, whyn);
    if (rc && e_facts_safepoint_free(&fn)) e_note_pure_inline(hex);
    if (have2) q2_free(&fn2);
    q2_free(&fn);
    return rc;
  }
  rc = e_emit_inline_body(f, &fn, hex, why, whyn);
  if (rc && e_facts_safepoint_free(&fn)) e_note_pure_inline(hex);
  q2_free(&fn);
  return rc;
}
