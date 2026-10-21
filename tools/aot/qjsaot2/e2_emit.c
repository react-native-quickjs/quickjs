/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * The choice of emitter for one function, and the native entry a compiled
 * caller enters it through.
 *
 * With `--emit2` off nothing changes: the unit builder calls the old emitter
 * directly through `qjs_aot_emit_body`. With it on, every function (or the one
 * named by `--emit2-only`) comes here first, and this file decides.
 */
#include <stdio.h>
#include <string.h>

#include "e2.h"
#include "ir.h"

int e2_on;
char e2_only_hex[33];

/* When set, compiled functions get external (hidden) linkage instead of
   `static`, so a unit split across files can call between them. Set by the
   unit builder for `--emit2-split`. */
static int e2_split;
void e2_set_split(int on) {
  e2_split = on ? 1 : 0;
}
int e2_split_on(void) {
  return e2_split;
}
const char *e2_linkage(void) {
  return e2_split ? "__attribute__((visibility(\"hidden\"))) " : "static ";
}

/* The call-site A/B switch (--emit2-nocall). */
static int e2_nocall;
void e2_set_nocall(int on) {
  e2_nocall = on ? 1 : 0;
}
int e2_nocall_on(void) {
  return e2_nocall;
}

int qjs_aot_emit2_on(void) {
  return e2_on;
}

/* Whether this function is one the new emitter should handle. A function is in
   scope when `--emit2-only` names it, or when no name was given. */
static int e2_in_scope(const char *hex) {
  if (!e2_on) return 0;
  if (e2_only_hex[0] == 0) return 1;
  return strcmp(hex, e2_only_hex) == 0;
}

/* The driver's profile, for the function's dynamic weight. */
double qjs_aot_prof_weight(const uint8_t dg[16]);

/* --- the entry rules (D10 rules P and K) -------------------------------- */

/* Whether the function contains a call site: an op that can reach a call. The
   role table marks one with an argument window (the format's trailing `b`),
   which `call`, `call_method`, `tail_call*`, `construct`, `apply*` and `eval`
   all carry and nothing else does. */
int e2_has_call_site(const Q2Func *fn) {
  int i;
  for (i = 0; i < fn->ninsns; i++) {
    const E2Role *r = e2_roles[fn->insns[i].rop];
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
   RangeError, the variable-area release and, in check builds, the frame
   cross-check.

   Rule P (poll) and rule K (stack check) are emitted only for a function that
   contains a call site, or, for rule K, when its own frame is over 16 KB.
   Recursion needs a call site in every function on the cycle, so every cycle
   still polls once per round; loops poll on their back edges; an op call that
   runs JS polls inside the engine. */
void e2_emit_native_entry(FILE *f, const Q2Func *fn, char hex[33]) {
  const JSAotGenFunc *g = fn->g;
  int vc = g->light_var_count;
  int call_site = e2_has_call_site(fn);
  int check = call_site || g->light_need > 16 * 1024;
  int nregs = g->reg_count;
  /* The variable area exists only when the promotion map leaves a slot in it,
     and the frame's layout follows: the engine sizes `fr` the same way. */
  int has_var = vc > 0;
  int slots = (g->light_need + (int)sizeof(JSValue) - 1) / (int)sizeof(JSValue);

  e2_report_entry(g, hex, 0, call_site, check);

  fprintf(
      f,
      "/* The native entry: the whole call is this C call. NEED = %d bytes,\n"
      "   %d slots, variable area %s (%d of %d slots), %d registers. */\n",
      g->light_need, slots, has_var ? "present" : "absent", has_var ? vc : 0,
      g->var_count, nregs);

  fprintf(
      f,
      "E2_TEXT %sJSValue aot_n_%s(JSContext *ctx, JSRegRun *caller_f,\n"
      "                               JSObject *co, JSValue this_obj,\n"
      "                               JSValue *argv, int argc)\n"
      "{\n",
      e2_linkage(), hex);
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

  /* Rule K: the stack check. The margin leaves room for the throw itself; a
     4 KB margin crashed before, and 64 KB is what the E0 experiments used. */
  if (check) {
    fprintf(
        f,
        "    if (__builtin_expect((uintptr_t)__builtin_frame_address(0) <\n"
        "            *(uintptr_t *)(rt + QJS_AOT_OFF_RT_STACK_LIMIT) + "
        "E2_STACK_MARGIN, 0))\n"
        "        return qjs_aot_throw_stack_overflow(ctx);\n");
  }
  /* Rule P: the poll. `qjs_aot_poll` decrements the counter itself, so undo
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
      "        __builtin_memcpy(&vb[k], &e2_undefined_value, "
      "sizeof(JSValue));\n",
      has_var ? g->var_count : 0);
  fprintf(
      f,
      "    for (k = 0; k < %d; k++)\n"
      "        __builtin_memcpy(&regs[k], &e2_undefined_value, "
      "sizeof(JSValue));\n",
      nregs);
  fprintf(f, "    (void)k; (void)vb; (void)cb;\n");
  fprintf(
      f,
      "#if E2_CHECK\n"
      "    qjs_aot_frame_check(ctx, (JSRegRun *)nf, co, caller_f, argv, argc,\n"
      "                        this_obj);\n"
      "#endif\n");
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

/* --- the frameless entry (card E9) -------------------------------------- */

/* `aot_nx_HEX`: the exit of a frameless body. It builds the frame the framed
   native entry builds, field for field, puts the live registers the body hands
   over (`x->lv`, at the frame offsets `x->offs`) where the generic body reads
   them, and continues in the generic body at `x->pc`. Cold: the fast path never
   gets here. The function has no variable area and no call site (rules 2 and 3),
   so there is no stack check, no poll and no release; the frame is small (rule 1).
   It takes ONE pointer for everything about the exit, so that leaving costs the
   body no more arguments than entering did. */
void e2_emit_frameless_exit_fn(FILE *f, const Q2Func *fn, char hex[33]) {
  const JSAotGenFunc *g = fn->g;
  int nregs = g->reg_count;
  int slots = (g->light_need + (int)sizeof(JSValue) - 1) / (int)sizeof(JSValue);

  fprintf(
      f,
      "/* The exit of the frameless body: builds the frame and runs the generic\n"
      "   body from x->pc with the registers' values handed over in x->lv. */\n"
      "E2_TEXT static __attribute__((noinline, cold)) JSValue aot_nx_%s(\n"
      "    JSContext *ctx, JSRegRun *caller_f, JSObject *co, JSValue this_obj,\n"
      "    JSValue *argv, const E2Nx *x)\n"
      "{\n"
      "    JSValue mem[%d] __attribute__((aligned(16)));\n"
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
      "    if (__builtin_expect(e2_exit_log_v != 0, 0) && x->kind != NULL) {\n"
      "        static int seen_[1024];\n"
      "        int *sp_ = &seen_[(x->pc * 7 + x->kind[0]) & 1023];\n"
      "        if (!*sp_) e2_exit_log(\"%s\", x->kind, x->pc, x->tag, sp_);\n"
      "    }\n"
      "    if (__builtin_expect(*(void **)(cb + QJS_AOT_OFF_FNBC_REGVM_PIC) == "
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
      "        __builtin_memcpy(&regs[k], &e2_undefined_value, "
      "sizeof(JSValue));\n"
      "    for (k = 0; k < x->n; k++) AOT_REG(regs, x->offs[k]) = x->lv[k];\n"
      "#if E2_CHECK\n"
      "    qjs_aot_frame_check(ctx, (JSRegRun *)nf, co, caller_f, argv, argc,\n"
      "                        this_obj);\n"
      "#endif\n"
      "    prev = *(JSRegRun **)(rt + QJS_AOT_OFF_RT_REGVM_CUR);\n"
      "    *(JSRegRun **)(rt + QJS_AOT_OFF_RT_REGVM_CUR) = (JSRegRun *)nf;\n"
      "    ret = aot_g_%s(realm, (JSRegRun *)nf, regs, (uint32_t)x->pc);\n"
      "    *(JSRegRun **)(rt + QJS_AOT_OFF_RT_REGVM_CUR) = prev;\n"
      "    return ret;\n"
      "}\n\n",
      hex, slots, hex, nregs, hex);
}

/* `aot_f_HEX` for a frameless function: the engine still enters through it with a
   frame of its own, and there is no second speculative body, so it forwards to
   `aot_n_HEX` with what the frame holds. The frame's header is the engine's:
   `hdr_pack` carries the argument count in bits 16..31, and `arg_buf` points at
   the arguments the caller passed (`argc` of them). A call with fewer than the
   declared arguments, or a resume at a pc, takes the generic body. */
void e2_emit_frameless_forward(FILE *f, const Q2Func *fn, char hex[33]) {
  fprintf(
      f,
      "E2_TEXT %sJSValue aot_f_%s(JSContext *ctx, JSRegRun *f,\n"
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
      e2_linkage(), hex, fn->g->arg_count, hex, hex);
}

/* --- the driver --------------------------------------------------------- */

int e2_emit_body(FILE *f, const JSAotGenFunc *g, char hex[33]) {
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
        stderr, "e2: %s: the IR could not be built: %s\n",
        g->name[0] ? g->name : "?", fn.why ? fn.why : "(no reason recorded)");
    q2_free(&fn);
    return -1;
  }
  if (q2_infer(&fn) < 0) {
    fprintf(
        stderr, "e2: %s: the IR could not be inferred: %s\n",
        g->name[0] ? g->name : "?", fn.why ? fn.why : "(no reason recorded)");
    q2_free(&fn);
    return -1;
  }
  if (q2_verify(&fn, msg, sizeof(msg)) < 0) {
    fprintf(
        stderr, "e2: %s: the IR failed to verify: %s\n",
        g->name[0] ? g->name : "?", msg);
    q2_free(&fn);
    return -1;
  }

  /* The generic body first: the speculative body forwards to it, and until E8
     the speculative body is that forward. */
  rc = e2_emit_generic_body(f, &fn, hex);
  if (rc != 0) {
    fprintf(
        stderr, "e2: %s: the generic body emitter returned %d\n",
        g->name[0] ? g->name : "?", rc);
  } else {
    rc = e2_emit_spec_body(f, &fn, hex);
    if (rc != 0) {
      fprintf(
          stderr, "e2: %s: the speculative body emitter returned %d\n",
          g->name[0] ? g->name : "?", rc);
    }
  }
  /* The unit builder must know a native entry exists, because a call site
     elsewhere in the unit may only name an `aot_n_` that some body defines. */
  if (rc == 0 && g->light_ok && e2_fl_was_chosen()) {
    /* The body IS the native entry: it was emitted as `aot_n_`, with its exit
       function and the forwarding `aot_f_`. */
    e2_report_entry(g, hex, 1, 0, 0);
    e2_report_func(g, hex, qjs_aot_prof_weight(g->digest), 0);
  } else if (rc == 0 && g->light_ok) {
    e2_emit_native_entry(f, &fn, hex);
    e2_report_func(g, hex, qjs_aot_prof_weight(g->digest), 0);
  } else if (rc == 0) {
    e2_report_func(g, hex, qjs_aot_prof_weight(g->digest), 0);
  }
  ninsns = fn.ninsns;
  q2_free(&fn);
  if (rc != 0) return rc;
  if (g->light_ok) qjs_aot_note_native_entry(qjs_aot_cur_func_idx());
  return ninsns;
}

/* The unit builder's hook: called once per function, with the option state
   already parsed. */
int e2_emit_body_if_on(FILE *f, const JSAotGenFunc *g, char hex[33]) {
  if (!e2_in_scope(hex)) return q2_old_emit_body(f, g, hex);
  return e2_emit_body(f, g, hex);
}
