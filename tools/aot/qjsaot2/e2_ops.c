/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * The generic body (E2 design D2): frame-resident, every instruction pc an
 * entry, no exits.
 *
 * The frame is the home of every value, so there is no value map, no
 * write-back and no entry loading. Every instruction start has a label and the
 * switch maps every pc to one, which makes exceptions, resumes and the op
 * call's "the handler returned a different pc" case the same three lines.
 *
 * A failed fast path does not leave: it calls the engine's step routine, which
 * runs the interpreter's own handler for that instruction, and then continues
 * at the pc the handler returned. That is what makes this body cover every op
 * without special cases, and it is why the body is small.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "e2.h"
#include "ir.h"
#include "e2_analysis.h"

/* The number of PIC sites the function uses: one more than the largest site
   index. The per-site stride in the PIC table is this count, because site `k`'s
   way `w` lives at `w * count + k`, so the read helper needs it to step between
   ways. */
int e2_pic_sites(const Q2Func *fn) {
  /* Unused: the stride is read from the running function's bytecode at runtime
     (q2_pic_sites in the prelude), so a unit cannot disagree with the bytecode
     it was compiled from. Kept only so the call sites keep their shape. */
  (void)fn;
  return 0;
}

/* The opcode's name, for the poison abort message. The role table already
   carries the engine's format; the name comes from the same generated source so
   a reader sees the op the way the engine does. */
void e2_rop_name(uint16_t rop, char *out, size_t n);

/* The profile's execution count at one pc (qjsaot.c): the op-call weight. */
unsigned long long qjs_aot_prof_pc_count(const uint8_t dg[16], int pc);

/* A numeric constant-pool entry, exactly as the bytecode holds it. Lets
   `binary_cpool` fold `x * 0.999` to a comparison and a multiply against a C
   double literal instead of op-calling the engine. */
int qjs_aot_pool_num(const struct JSAotGenFunc *g, uint32_t idx, double *out);

/* The entry's tag when it is a foldable number: E2_POOL_INT or
   E2_POOL_DOUBLE, else 0. (The generator does not include the engine's headers,
   so the two values it can see are named here.) */
int qjs_aot_pool_tag(const struct JSAotGenFunc *g, uint32_t idx);
#define E2_POOL_INT 1    /* QJS_AOT_POOL_INT */
#define E2_POOL_DOUBLE 2 /* QJS_AOT_POOL_DOUBLE */

/* Report one instruction the body leaves as an op call, weighted by the
   profile's count there. A pc that also has an inline fast path reports its
   weight as `missarm`: only its misses reach the op call, so counting it in the
   op's own row would make a lowered family look unlowered. */
static void note_opcall(const Q2Func *fn, const Q2Insn *in, int missarm) {
  unsigned long long w = qjs_aot_prof_pc_count(fn->g->digest, in->pc);
  if (w) e2_report_opcall(in->rop, (double)w, missarm);
}

/* The driver's profile and unit queries (qjsaot.c). A call site asks the
   profile which callees it saw and the unit whether each is compiled here and
   can be entered on the C stack. */
int qjs_aot_prof_callees(
    const uint8_t dg[16], int pc, int *n, char hex[4][33], int kind[4],
    unsigned long long hits[4]);
int qjs_aot_unit_has(const char hex[33]);
int qjs_aot_light_need(const char hex[33]);
int qjs_aot_unit_argc(const char hex[33]);

/* Whether register `r` may be written by this instruction: its declared
   destinations, plus `writes_regs` ops. */
static int role_writes_reg(
    const Q2Func *fn, const E2Role *role, const Q2Insn *in, int r) {
  int i;
  /* The role table indexes its def[]/use[] by *operand*, so a register that is
     both a destination and a source (a move the allocator coalesced onto one
     register, which the handler still stores) appears in both lists. A
     declared destination is a write either way. */
  for (i = 0; i < role->ndef; i++)
    if (role->def[i] < in->narg && in->arg[role->def[i]].kind == 'r' &&
        (int)(in->arg[role->def[i]].val >> 4) == r)
      return 1;
  if (role->writes_regs) return 1;
  /* The IR's own destinations are authoritative: a value the instruction
     defines lives in a register (`Q2Value.reg`), and that register is written
     even when the table's operand index and the allocator's coalescing do not
     line up. `def[]` holds value ids, not operand indices. */
  for (i = 0; i < in->ndef; i++)
    if (in->def[i] < (Q2ValueId)fn->nvalues && fn->values[in->def[i]].reg == r)
      return 1;
  return 0;
}

/* Whether register `r` is live after this instruction, from the IR's own live
   set, so a dead register can be poisoned and a live one left alone. */
static int reg_live_after(const Q2Func *fn, int insn, int r) {
  const uint64_t *live = fn->insn_live + (size_t)insn * fn->lwords;
  return (live[r >> 6] >> (r & 63)) & 1;
}

/* Whether this instruction reads `r`: a declared use or a window register.
   The live set does not say this, so it is asked separately; a read register
   is never poisoned and must still hold its value afterwards. */
static int role_reads_reg(const E2Role *role, const Q2Insn *in, int r) {
  int k;
  for (k = 0; k < role->nuse; k++)
    if (role->use[k] < in->narg && in->arg[role->use[k]].kind == 'r' &&
        (int)(in->arg[role->use[k]].val >> 4) == r)
      return 1;
  /* `win_reg` is a register index already, unlike an `r` operand's `val`. */
  if (in->win_n > 0 && r >= in->win_reg && r < in->win_reg + in->win_n)
    return 1;
  return 0;
}

/* Whether the unit being generated will be compiled with -DE2_POISON. The
   emitter sets it from the environment so `m1.sh e2` can build both units. */
static int g_poison;
int e2_poison_on(void) {
  return g_poison;
}
void e2_set_poison(int on) {
  g_poison = on;
}

/* Whether an op call gets the check at all. A fallback reads and writes
   whatever its handler needs, so there is nothing to declare or compare. A
   return or a suspend leaves the activation inside the handler, so the frame
   cannot be compared or restored afterwards. */
static int poison_applies(const Q2Insn *in, const E2Role *role) {
  if (in->op == Q2_OP_FALLBACK) return 0;
  if (role->control == E2_CF_RETURN || role->control == E2_CF_SUSPEND) return 0;
  return 1;
}

/* D12's dynamic check around one op call. The work is done by two helpers in
   the unit prelude (`e2_poison_pre`, `e2_poison_post`); each call site passes
   two per-register mask strings, so the check costs one line per op rather
   than one per register, and a large function still compiles quickly.

   Before: copy every register, then overwrite with the sentinel each register
   that is dead after the instruction, not read by it and not written by it
   (`pm`) -- but only when it holds a value without a reference count. A
   register may be the only root of a counted value (registers borrow under
   DRC), so replacing one could let a collection during the op free it.

   After, straight after the call and before any exit: every register the op
   may not write (`wm` is '0') must equal what it held going in, the sentinel
   if it was poisoned. Then the poisoned registers get their values back, so
   the sentinel never outlives the call: a wrong answer can only come from a
   read inside the op, never from a liveness approximation later on. */
static void emit_poison_masks(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const E2Role *role) {
  int r, insn = (int)(in - fn->insns);

  fprintf(f, "    static const char e2_pm_%d[] = \"", in->pc);
  for (r = 0; r < fn->nregs; r++)
    fputc(
        !role_writes_reg(fn, role, in, r) && !role_reads_reg(role, in, r) &&
                !reg_live_after(fn, insn, r)
            ? '1'
            : '0',
        f);
  fprintf(f, "\";\n");
  fprintf(f, "    static const char e2_wm_%d[] = \"", in->pc);
  for (r = 0; r < fn->nregs; r++)
    fputc(role_writes_reg(fn, role, in, r) ? '1' : '0', f);
  fprintf(f, "\";\n");
}

static void emit_opcall(
    FILE *f, const Q2Func *fn, const Q2Insn *in, int is_last, int with_label) {
  const E2Role *role = e2_roles[in->rop];
  char opname[64];

  int poison = g_poison && role && poison_applies(in, role);

  e2_rop_name(in->rop, opname, sizeof(opname));
  /* The masks are declarations, so they go before the label; a caller that
     emitted a native fast path has already placed them. */
  if (poison && with_label) emit_poison_masks(f, fn, in, role);
  if (with_label) fprintf(f, "g_%d:\n", in->pc);
  if (poison) {
    fprintf(
        f, "    e2_poison_pre(fr, %d, e2_poison_copy, e2_pz, e2_pm_%d);\n",
        fn->nregs, in->pc);
  }
  fprintf(f, "    ip = qjs_aot_step(code + %d, fr, f, ctx);\n", in->pc);
  if (poison)
    fprintf(
        f,
        "    e2_poison_post(fr, %d, e2_poison_copy, e2_pz, e2_wm_%d, "
        "\"%s\", %d);\n",
        fn->nregs, in->pc, opname, in->pc);
  fprintf(
      f,
      "    if (__builtin_expect(ip == NULL, 0)) { pc = %d; "
      "goto exception; }\n",
      in->pc);
  fprintf(
      f,
      "    if (__builtin_expect(ip == QJS_AOT_COLD_RET || "
      "ip == QJS_AOT_SUSPEND, 0)) return qjs_aot_ret(f);\n");
  /* The handler returns the next pc, which for a branch is the target and for
     anything else is the following instruction. Going through the switch in
     both cases needs no special case for either. */
  /* The op call may have returned a back edge's target; the poll has to happen
     before the jump, so it is emitted on the dispatch path rather than here,
     where the target is not known. A branch to a lower pc is the common case
     and is handled by the caller. */
  fprintf(f, "    if (__builtin_expect(ip != code + %d, 0)) {\n", in->next);
  fprintf(f, "        e2_from_pc = %d;\n", in->pc);
  fprintf(f, "        entry_pc = (uint32_t)(ip - code);\n");
  fprintf(f, "        goto dispatch;\n");
  fprintf(f, "    }\n");
  if (!is_last) fprintf(f, "    /* fall through to g_%d */\n", in->next);
}

/* A register operand's `val` is the byte offset into the register file, not an
   index: the IR's own accessor is `val >> 4`. It is used as the offset
   directly, never multiplied again. */
#define E2_ROFF(arg) ((int)(arg).val)

/* --- native lowerings (E5) ----------------------------------------------
 *
 * A native lowering is frame-to-frame: it reads the operand registers, runs
 * the op's fast path inline, and stores the result. A miss falls through to
 * the op call, which runs the interpreter's own handler and returns the next
 * pc, so every slow path, exception and coercing path is still the engine's.
 *
 * The tail after a native fast path is the same as after an op call's
 * fall-through: the store, then the next instruction's label. Keeping one
 * helper for it means a native op cannot forget the poison-free bookkeeping or
 * the fall-through comment.
 */
static void emit_next(FILE *f, const Q2Insn *in, int is_last) {
  if (!is_last) fprintf(f, "    /* fall through to g_%d */\n", in->next);
}

/* The offset of an instruction's first destination register, or -1. */
static int dest_off(const Q2Func *fn, const Q2Insn *in) {
  if (in->ndef < 1) return -1;
  if (in->def[0] >= (Q2ValueId)fn->nvalues) return -1;
  return fn->values[in->def[0]].reg * 16;
}

/* The offset of a register operand, by the role table's operand index. */
static int operand_off(const Q2Insn *in, int idx) {
  if (idx < 0 || idx >= in->narg || in->arg[idx].kind != 'r') return -1;
  return E2_ROFF(in->arg[idx]);
}

/* The function the generic body being emitted belongs to, for the site
   counters' names. A site is (function, pc, op), because a pc is only unique
   inside one function. */
static const char *g_cur_hex = "";

/* The tail every native lowering shares: the site counters (when the unit is
   built with -DE2_COUNT), then the fall-through comment. A lowering's fast path
   counts a hit before its `goto`; the op call below the block is the miss, so
   one `E2_CTR(..., 0)` at the top of that block records it. `hex` names the
   function the site belongs to. */
static void emit_site_hit(FILE *f, const Q2Insn *in) {
  fprintf(
      f, "        E2_CTR(\"%s\", %d, %d, 1);\n", g_cur_hex, in->pc, in->rop);
}
static void emit_site_miss(FILE *f, const Q2Insn *in) {
  fprintf(f, "    E2_CTR(\"%s\", %d, %d, 0);\n", g_cur_hex, in->pc, in->rop);
}

/* The arithmetic and compare ops, lowered natively: the operands come from the
   frame, `q2_arith` runs the engine's own number fast path, and the result is
   stored. A miss (a string, an object, a mixed tag the helper declines) falls
   into the op call below, which runs the interpreter's handler.

   `sub` is the engine's REG_BIN_* value, carried by the IR. */
static int emit_native_binary(
    FILE *f, const Q2Func *fn, const Q2Insn *in, int is_last) {
  const E2Role *role = e2_roles[in->rop];
  int d, a, b;

  if (in->ndef < 1 || in->sub < 0) return 0;
  /* The fused ops write two registers and read three; they get their own
     lowering, so they are op calls for now. */
  if (in->ndef != 1 || in->nuse != 2) return 0;
  /* The plain arithmetic and compare opcodes: one destination, two register
     sources, a real REG_BIN_* sub-operation. The compares are classified
     `Q2_OP_FALLBACK` on purpose -- their tagged path can call JS -- but their
     roles are fully declared, which is all this lowering needs: `q2_arith`
     declines any case it cannot do and the op call handles the rest.
     `instanceof`, `in` and `private_in` are excluded: the helper has no fast
     path for them. */
  /* A control-flow op is not a plain binary: `inc_branch_lt_s` adds an
     immediate and branches, and its "sub" is a compare kind that describes the
     branch, not a value to compute. Falling through to `next` would skip the
     branch, so only ops with no control effect are lowered here. */
  if (role == NULL || role->control != E2_CF_NONE) return 0;
  if (in->sub < 0 || in->sub >= 0x40)
    return 0; /* real REG_BIN_*, not a pseudo */
  if (in->sub == REG_BIN_INSTANCEOF || in->sub == REG_BIN_IN ||
      in->sub == REG_BIN_PRIVATE_IN)
    return 0;
  if (in->op == Q2_OP_FALLBACK &&
      (in->sub == REG_BIN_INSTANCEOF || in->sub == REG_BIN_IN ||
       in->sub == REG_BIN_PRIVATE_IN))
    return 0;

  d = dest_off(fn, in);
  if (d < 0) return 0;
  /* The two sources, by the role table's operand indices -- the same indices
     the IR used, so the order matches the engine's operand order. */
  if (role == NULL || role->nuse != 2) return 0;
  a = operand_off(in, role->use[0]);
  b = operand_off(in, role->use[1]);
  if (a < 0 || b < 0) return 0;

  fprintf(f, "g_%d: {\n", in->pc);
  fprintf(f, "    JSValue _r;\n");
  fprintf(
      f,
      "    if (__builtin_expect(q2_arith(%d, AOT_REG(fr, %d), "
      "AOT_REG(fr, %d), &_r), 1)) {\n",
      in->sub, a, b);
  emit_site_hit(f, in);
  fprintf(f, "        AOT_SET_B(ctx, fr, %d, _r);\n", d);
  fprintf(f, "        goto g_%d;\n", in->next);
  fprintf(f, "    }\n");
  emit_site_miss(f, in);
  /* The op call follows immediately and runs the handler for this pc. */
  fprintf(f, "}\n");
  return 1;
}

/* `REG_UN_TO_NUMERIC` as the emitted unit needs it: a number. The generator
   sees the engine's enum (`quickjs-regvm.h`), the unit does not, so the value is
   resolved here and written into the code. */
#define Q2_REG_UN_TO_NUMERIC ((int)REG_UN_TO_NUMERIC)

/* The immediate arithmetic forms: `binary_imm` ("rbrI": d, sub, a, imm) and
   `binary_imm_s` ("rbri": d, sub, a, imm8). The right operand is a constant the
   opcode carries, so the fast path is the same `q2_arith` with `js_int32(imm)`
   in place of a second frame read. A miss falls into the op call, which runs the
   engine's handler: a string operand, an object, or a sub-kind `q2_arith`
   declines (a compare against a non-number) still takes the engine's path.

   The `add_imm_s`/`sub_imm_s` forms ("rri": d, a, imm) are separate opcodes
   with the kind in the opcode; they go through the same arm. */
static int emit_native_binary_imm(
    FILE *f, const Q2Func *fn, const Q2Insn *in, int is_last) {
  const E2Role *role = e2_roles[in->rop];
  int d, a, imm_idx, sub;

  switch (in->rop) {
    case ROP_binary_imm:
    case ROP_binary_imm_s:
      if (in->narg < 4 || in->arg[1].kind != 'b') return 0;
      sub = (int)in->arg[1].val;
      a = operand_off(in, 2);
      imm_idx = 3;
      break;
    case ROP_add_imm_s:
    case ROP_sub_imm_s:
      if (in->narg < 3) return 0;
      sub = in->rop == ROP_add_imm_s ? REG_BIN_ADD : REG_BIN_SUB;
      a = operand_off(in, 1);
      imm_idx = 2;
      break;
    case ROP_binary_cpool: {
      /* "rbrbc": d, sub, a, kind, idx -- the second operand is a pool entry,
         not a register. A foldable numeric entry becomes a C literal; anything
         else stays an op call. */
      double c;
      uint32_t kind, idx;
      int tag;
      char lit[64];
      if (in->narg < 5) return 0;
      sub = (int)in->arg[1].val;
      a = operand_off(in, 2);
      kind = (uint32_t)in->arg[3].val;
      idx = (uint32_t)in->arg[4].val;
      if (kind != REG_CONST_CPOOL) return 0;
      if (!qjs_aot_pool_num(fn->g, idx, &c)) return 0;
      tag = qjs_aot_pool_tag(fn->g, idx);
      if (sub != REG_BIN_ADD && sub != REG_BIN_SUB && sub != REG_BIN_MUL &&
          sub != REG_BIN_DIV)
        return 0;
      if (role == NULL || role->control != E2_CF_NONE) return 0;
      if (in->ndef != 1 || a < 0) return 0;
      d = dest_off(fn, in);
      if (d < 0) return 0;
      /* An integer entry must stay an integer: `i + 100000` with a double
         literal would miss both inline paths, produce a float where the
         interpreter produces an int, and make later `a[i + 100000]` miss its
         integer element path. */
      if (tag == E2_POOL_INT) {
        snprintf(lit, sizeof(lit), "JS_MKVAL(JS_TAG_INT, %d)", (int32_t)c);
      } else if (tag == E2_POOL_DOUBLE) {
        /* %a prints an exact hex float, which round-trips every double
           including -0.0. `%.17g` would print negative zero as "-0" (which C
           reads as the integer 0, losing the sign) and infinity/NaN as
           "inf"/"nan", which do not compile. A non-finite entry is therefore
           refused and stays an op call. */
        if (!__builtin_isfinite(c)) return 0;
        snprintf(lit, sizeof(lit), "__JS_NewFloat64(%a)", c);
      } else {
        return 0;
      }
      fprintf(f, "g_%d: {\n", in->pc);
      fprintf(f, "    JSValue _r;\n");
      fprintf(
          f,
          "    if (__builtin_expect(q2_arith(%d, AOT_REG(fr, %d), "
          "%s, &_r), 1)) {\n",
          sub, a, lit);
      emit_site_hit(f, in);
      fprintf(f, "        AOT_SET_B(ctx, fr, %d, _r);\n", d);
      fprintf(f, "        goto g_%d;\n", in->next);
      fprintf(f, "    }\n");
      emit_site_miss(f, in);
      fprintf(f, "}\n");
      (void)is_last;
      return 1;
    }
    default:
      return 0;
  }
  if (role == NULL || role->control != E2_CF_NONE) return 0;
  if (in->ndef != 1 || a < 0 || imm_idx >= in->narg) return 0;
  if (in->arg[imm_idx].kind != 'I' && in->arg[imm_idx].kind != 'i') return 0;
  /* The same exclusions as the two-register arm: only a real REG_BIN_* that
     `q2_arith` has a fast path for. */
  if (sub < 0 || sub >= 0x40) return 0;
  if (sub == REG_BIN_INSTANCEOF || sub == REG_BIN_IN ||
      sub == REG_BIN_PRIVATE_IN)
    return 0;
  d = dest_off(fn, in);
  if (d < 0) return 0;

  fprintf(f, "g_%d: {\n", in->pc);
  fprintf(f, "    JSValue _r;\n");
  fprintf(
      f,
      "    if (__builtin_expect(q2_arith(%d, AOT_REG(fr, %d), "
      "JS_MKVAL(JS_TAG_INT, %d), &_r), 1)) {\n",
      sub, a, (int32_t)in->arg[imm_idx].val);
  emit_site_hit(f, in);
  fprintf(f, "        AOT_SET_B(ctx, fr, %d, _r);\n", d);
  fprintf(f, "        goto g_%d;\n", in->next);
  fprintf(f, "    }\n");
  emit_site_miss(f, in);
  fprintf(f, "}\n");
  (void)is_last;
  return 1;
}

/* A property read: `q2_own_read_hit` inlines the PIC's own-property hit, so a
   monomorphic read is a shape compare and a slot load. Any miss (a prototype
   hit, a getter, a non-object) falls into the op call, which runs the
   interpreter's handler and therefore the full lookup.

   `get_field` and `get_field2` are both "rras" -- destination, receiver, atom,
   site -- and read the same way; `get_field2` is the form the older stack-VM
   lowering used and `get_field` the one the register form prefers, but a
   function can contain either. The site selects the PIC entry the profile
   filled. */
static int emit_native_field_read(
    FILE *f, const Q2Func *fn, const Q2Insn *in, int is_last) {
  const E2Role *role = e2_roles[in->rop];
  int d, recv, site, nsites = e2_pic_sites(fn);

  if (in->op != Q2_OP_GET_FIELD) return 0;
  if (role == NULL || role->control != E2_CF_NONE) return 0;
  if (in->rop != ROP_get_field && in->rop != ROP_get_field_own &&
      in->rop != ROP_get_field2 && in->rop != ROP_get_field2_own)
    return 0;
  if (in->narg < 4 || in->arg[3].kind != 's') return 0;
  site = (int)in->arg[3].val;
  d = dest_off(fn, in);
  recv = operand_off(in, role->use[0]);
  if (d < 0 || recv < 0) return 0;

  fprintf(f, "g_%d: {\n", in->pc);
  fprintf(f, "    JSValue _f;\n");
  fprintf(
      f,
      "    if (__builtin_expect(q2_own_read_hit(pic, holders, "
      "AOT_REG(fr, %d), %d, q2_ns, &_f), 1)) {\n",
      recv, site);
  emit_site_hit(f, in);
  /* The value is borrowed from the object's slot, so the store duplicates it
     (AOT_SET_B does that in an owning build and is a plain assignment under
     DRC, where the slot keeps its own reference). */
  fprintf(f, "        AOT_SET_B(ctx, fr, %d, _f);\n", d);
  fprintf(f, "        goto g_%d;\n", in->next);
  fprintf(f, "    }\n");
  emit_site_miss(f, in);
  fprintf(f, "}\n");
  (void)is_last;
  return 1;
}

/* A global read: `q2_global_hit` inlines the filled-cell check (the PIC entry's
   recorded generation against the runtime's), then loads the value. A miss
   takes the engine's site fill (which probes the global's cell cache, fills the
   PIC entry when the probe hits, and otherwise reads the global, which can
   throw), then continues at the next instruction.

   The fill matters: the *interpreter's* step form for this op never calls
   `regvm_global_site_fill`, so an op-call-only lowering leaves the site empty
   for ever and the inline hit can never fire. Measured on richards before this
   arm was added: `get_global` 0 hits, 9.4M misses. The engine's step form is
   the thing that is missing the fill; this arm does not depend on it.

   `get_global` is "rabs": destination, atom, throwing flag, site. */
static int emit_native_global_read(
    FILE *f, const Q2Func *fn, const Q2Insn *in, int is_last) {
  const E2Role *role = e2_roles[in->rop];
  int d, site;
  int atom_off, throwing;

  if (in->rop != ROP_get_global) return 0;
  if (role == NULL || role->control != E2_CF_NONE) return 0;
  if (in->narg < 4 || in->arg[3].kind != 's') return 0;
  site = (int)in->arg[3].val;
  if ((unsigned)site == REG_IC_NONE) return 0;
  d = dest_off(fn, in);
  if (d < 0) return 0;
  if (in->arg[1].kind != 'a') return 0;
  atom_off = in->arg[1].off;
  throwing = (int)in->arg[2].val;

  fprintf(f, "g_%d: {\n", in->pc);
  fprintf(f, "    JSValue _g;\n");
  fprintf(
      f, "    if (__builtin_expect(q2_global_hit(ctx, pic, %d, &_g), 1)) {\n",
      site);
  emit_site_hit(f, in);
  fprintf(f, "        AOT_SET_B(ctx, fr, %d, _g);\n", d);
  fprintf(f, "        goto g_%d;\n", in->next);
  fprintf(f, "    }\n");
  emit_site_miss(f, in);
  /* The cold arm reads the global, which can throw and may run JS: it takes the
     owned result on one side and the borrowed slot on the other, exactly as the
     engine's accessor documents. `pic` is re-read because the fill may have
     allocated the entry. */
  fprintf(f, "    {\n");
  fprintf(
      f,
      "        int _r = qjs_aot_get_global_site(ctx, f, %d,\n"
      "            (int)qjs_aot_ld_u32(code + %d), %d, &_g);\n",
      site, atom_off, throwing);
  fprintf(f, "        if (_r < 0) { pc = %d; goto exception; }\n", in->pc);
  fprintf(f, "        if (_r) qjs_aot_set_reg_p(ctx, fr, %d, _g);\n", d);
  fprintf(f, "        else AOT_SET_B(ctx, fr, %d, _g);\n", d);
  fprintf(f, "        pic = q2_pic(f); holders = q2_pic_holder(f);\n");
  fprintf(f, "        goto g_%d;\n", in->next);
  fprintf(f, "    }\n");
  fprintf(f, "}\n");
  (void)is_last;
  return 1;
}

/* `get_this_field` / `get_this_field2`: "rras" -- destination, receiver slot,
   atom, site. Both register operands are destinations and neither is read: the
   handler stores `this` into the receiver register and then reads the field
   from it, so the IR gives the instruction two defs and no uses.

   `this` is loaded the way the handler loads it (`qjs_aot_load_this` mirrors
   `load_this` and the handler's own boxing): a borrowed value for an object or
   the global object, an owned one for a boxed primitive, and an exception for a
   failed box. The ownership decides the store: an owned value is adopted by
   `qjs_aot_set_reg_p`, a borrowed one is duplicated by `AOT_SET_B`, and getting
   those the wrong way round is a refcount crash.

   The read itself is the same own-slot hit as `get_field`, on that receiver; a
   miss falls into the op call, which boxes (again) and reads the slow way. */
static int emit_native_this_field(
    FILE *f, const Q2Func *fn, const Q2Insn *in, int is_last) {
  const E2Role *role = e2_roles[in->rop];
  int dfield, drecv, site;

  if (in->rop != ROP_get_this_field && in->rop != ROP_get_this_field2) return 0;
  if (role == NULL || role->control != E2_CF_NONE) return 0;
  if (in->narg < 4 || in->arg[3].kind != 's') return 0;
  site = (int)in->arg[3].val;
  if ((unsigned)site == REG_IC_NONE) return 0;
  /* Both register operands are destinations (`def[0]` is the read, `def[1]` the
     receiver), so the role table has no use to take an offset from. */
  if (in->ndef < 2 || in->arg[0].kind != 'r' || in->arg[1].kind != 'r')
    return 0;
  dfield = E2_ROFF(in->arg[0]);
  drecv = E2_ROFF(in->arg[1]);
  {
    int nsites = e2_pic_sites(fn);

  fprintf(f, "g_%d: {\n", in->pc);
  fprintf(f, "    JSValue _t, _v;\n");
  fprintf(f, "    int _own = qjs_aot_load_this(ctx, f, &_t);\n");
  fprintf(
      f,
      "    if (__builtin_expect(_own < 0, 0)) { pc = %d; "
      "goto exception; }\n",
      in->pc);
  /* The receiver register is a destination first: the read below uses `_t`
     directly, so this store is what makes the register hold `this`. */
  fprintf(f, "    if (_own) qjs_aot_set_reg_p(ctx, fr, %d, _t);\n", drecv);
  fprintf(f, "    else AOT_SET_B(ctx, fr, %d, _t);\n", drecv);
  fprintf(
      f,
      "    if (__builtin_expect(q2_own_read_hit(pic, holders, _t, %d, "
      "q2_ns, &_v), 1)) {\n",
      site);
  emit_site_hit(f, in);
  /* Borrowed from the object's slot, so the store duplicates it. */
  fprintf(f, "        AOT_SET_B(ctx, fr, %d, _v);\n", dfield);
  fprintf(f, "        goto g_%d;\n", in->next);
  fprintf(f, "    }\n");
  emit_site_miss(f, in);
  fprintf(f, "}\n");
  (void)is_last;
  }
  return 1;
}

/* `get_field_chain` / `get_field_chain_own`: two sequential `q2_own_read_hit`
   calls, the second receiver being the first link's result. The generic body
   stores each link to its own register as it is produced, so a miss on either
   link falls into the op call, which fills that link's cache and continues.
   Both results are borrowed from their objects' slots, so both stores duplicate.

   Format "rrasrras": d1, recv, atom1, site1, d2, recv2, atom2, site2. */
static int emit_native_field_chain(
    FILE *f, const Q2Func *fn, const Q2Insn *in, int is_last) {
  const E2Role *role = e2_roles[in->rop];
  int d0, d1, recv, s1, s2;

  if (in->rop != ROP_get_field_chain && in->rop != ROP_get_field_chain_own)
    return 0;
  if (role == NULL || role->control != E2_CF_NONE) return 0;
  if (in->narg < 8 || in->arg[3].kind != 's' || in->arg[7].kind != 's')
    return 0;
  s1 = (int)in->arg[3].val;
  s2 = (int)in->arg[7].val;
  if (s1 == 0xffff || s2 == 0xffff) return 0; /* REG_IC_NONE */
  d0 = dest_off(fn, in);
  recv = operand_off(in, role->use[0]);
  if (d0 < 0 || recv < 0) return 0;
  /* The second destination is the second defined value. */
  if (in->ndef < 2) return 0;
  d1 = fn->values[in->def[1]].reg * 16;
  {
    int nsites = e2_pic_sites(fn);

  fprintf(f, "g_%d: {\n", in->pc);
  fprintf(f, "    JSValue _c0, _c1;\n");
  fprintf(
      f,
      "    if (__builtin_expect(q2_own_read_hit(pic, holders, "
      "AOT_REG(fr, %d), %d, q2_ns, &_c0), 1)) {\n",
      recv, s1);
  /* Link 1 goes to its register, so link 2's receiver is available even if
     link 2 misses; both are borrowed, so the store duplicates.

     When the destination register *is* the receiver -- which the IR does
     whenever the chain's first result is written back over its own operand --
     that store overwrites the receiver before the op-call fallback. The op call
     re-executes `get_field_chain` from the start and would then read the wrong
     receiver, so the original is saved and put back on a link-2 miss. */
  fprintf(f, "        JSValue _rcv = AOT_REG(fr, %d);\n", recv);
  fprintf(f, "        AOT_SET_B(ctx, fr, %d, _c0);\n", d0);
  fprintf(
      f,
      "        if (__builtin_expect(q2_own_read_hit(pic, holders, "
      "_c0, %d, q2_ns, &_c1), 1)) {\n",
      s2);
  emit_site_hit(f, in);
  fprintf(f, "            AOT_SET_B(ctx, fr, %d, _c1);\n", d1);
  fprintf(f, "            goto g_%d;\n", in->next);
  fprintf(f, "        }\n");
  fprintf(f, "        AOT_SET_B(ctx, fr, %d, _rcv);\n", recv);
  fprintf(f, "    }\n");
  emit_site_miss(f, in);
  fprintf(f, "}\n");
  (void)is_last;
  }
  return 1;
}

/* JS_FUNC_NORMAL, from the engine's JSFunctionKindEnum. That enum is private to
   quickjs.c, so the value is repeated here with its name rather than pulled in.
   A generator or async function takes the suspend path on `return`, which a
   direct C return cannot do, so both return lowerings refuse them. */
#define E2_FUNC_NORMAL 0

/* A property write: `q2_put_field_hit` stores in place when the PIC entry says
   the slot is an own, writable, non-accessor property of a matching shape. Any
   other case falls into the op call.

   `put_field` is "rasr": receiver, atom, site, value. The op has no
   destination, so its fast path only has to continue. */
static int emit_native_field_write(
    FILE *f, const Q2Func *fn, const Q2Insn *in, int is_last) {
  const E2Role *role = e2_roles[in->rop];
  int recv, val, site;

  if (in->rop != ROP_put_field) return 0;
  if (role == NULL || role->control != E2_CF_NONE) return 0;
  if (in->narg < 4 || in->arg[2].kind != 's') return 0;
  site = (int)in->arg[2].val;
  recv = operand_off(in, role->use[0]);
  val = operand_off(in, role->use[1]);
  if (recv < 0 || val < 0) return 0;

  fprintf(f, "g_%d: {\n", in->pc);
  fprintf(
      f,
      "    if (__builtin_expect(q2_put_field_hit(ctx, pic, AOT_REG(fr, %d), "
      "%d, AOT_REG(fr, %d)), 1)) {\n",
      recv, site, val);
  emit_site_hit(f, in);
  fprintf(f, "        goto g_%d;\n", in->next);
  fprintf(f, "    }\n");
  emit_site_miss(f, in);
  fprintf(f, "}\n");
  (void)fn;
  (void)is_last;
  return 1;
}

/* Whether the profile saw only numbers at this instruction's pc. `elem` is the
   AOT_ELEM_* set the profiler recorded; INT or FLOAT alone means every element
   there was a number, so an inline read has a hope of hitting. Without a
   profile nothing is claimed. */
static int elem_pc_is_numeric(const Q2Func *fn, const Q2Insn *in) {
  int i;
  if (fn->prof == NULL) return 0;
  for (i = 0; i < fn->nprof; i++) {
    if (fn->prof[i].pc != in->pc) continue;
    return (fn->prof[i].elem & (Q2_ELEM_INT | Q2_ELEM_FLOAT)) != 0 &&
           (fn->prof[i].elem & (Q2_ELEM_OBJECT | Q2_ELEM_MIXED)) == 0;
  }
  return 0;
}

/* ---- guarded element regions ---------------------------------------------
   A block that accesses one array several times re-validates the receiver and
   reloads the array's `count` and `values` pointers on every access, because the
   compiler cannot prove a store into the elements did not change them -- a store
   can grow the array and move the storage. That is the elem class's cost:
   measured, 20 % of its cycles are memory stalls against the interpreter's 6.5 %,
   with IPC 1.56 against 3.44 and 43 % fewer instructions.

   The region keeps the validated storage and count in C locals across a run of
   consecutive element accesses to the same register. It is dropped whenever
   anything could change the array.

   `elem_run_len` answers: starting at instruction `i` of `fn`, how many
   consecutive instructions form a run over one array register, and which
   register is it? 0 means this instruction starts no run. */

/* The array register an element access reads, or -1. */
static int elem_arr_reg(const Q2Insn *in) {
  const E2Role *role = e2_roles[in->rop];
  if (in->rop != ROP_get_element && in->rop != ROP_put_element &&
      in->rop != ROP_put_element_xfer)
    return -1;
  /* `get_element` is "rrr" = destination, object, key, so the array is the
     second operand; `put_element` is "rrr" = object, key, value, so it is the
     first. The IR's use[0] names the same one for either. */
  if (role != NULL && role->nuse > 0) {
    int a = operand_off(in, role->use[0]);
    if (a >= 0) return a;
  }
  if (in->narg >= 3 && in->arg[0].kind == 'r') return E2_ROFF(in->arg[0]);
  return -1;
}

/* Whether `in` could change what a region holds: it runs JavaScript, it writes
   the array's register, or it may grow the array. */
static int elem_breaks_run(const Q2Func *fn, const Q2Insn *in, int arr) {
  const E2Role *role = e2_roles[in->rop];
  int k;
  if (role == NULL) return 1;
  if (in->op == Q2_OP_FALLBACK || role->runs_js || in->op == Q2_OP_CALL)
    return 1;
  if (role->control != E2_CF_NONE) return 1;
  for (k = 0; k < in->ndef; k++) {
    Q2ValueId v = in->def[k];
    if (v != Q2_NO_VALUE && fn->values[v].reg == arr) return 1;
  }
  for (k = 0; k < in->nredef; k++)
    if (in->redef_reg[k] == arr) return 1;
  if (in->win_reg >= 0 && in->win_n > 0 && arr >= in->win_reg &&
      arr < in->win_reg + in->win_n)
    return 1;
  /* An element store that is not proven in bounds may grow the array. */
  if (in->rop == ROP_put_element || in->rop == ROP_put_element_xfer) {
    if (!elem_pc_is_numeric(fn, in)) return 1;
  }
  return 0;
}

/* How many consecutive instructions starting at `i` form a run over one array
   register, and which register through *arrp. 0 when there is no run.

   The run stops at the first instruction that could change the array, at a
   different array, and at the end of the block: the region's C locals live in
   one emitted block, and a block boundary is emitted as a label that other code
   can jump into, so a value held across it would not be defined. */
static int elem_run_len(const Q2Func *fn, int i, int block_last, int *arrp) {
  int arr = elem_arr_reg(&fn->insns[i]);
  int j, n = 0;
  if (arr < 0) return 0;
  for (j = i; j < fn->ninsns; j++) {
    const Q2Insn *in = &fn->insns[j];
    if (j > block_last) break;
    if (elem_arr_reg(in) == arr) {
      if (!elem_pc_is_numeric(fn, in)) break;
      n++;
      continue;
    }
    if (elem_arr_reg(in) >= 0) break; /* a different array */
    if (elem_breaks_run(fn, in, arr)) break;
  }
  if (n < 2) return 0;
  *arrp = arr;
  /* The last instruction index of the run. Returning the index rather than a
     count keeps the close point independent of how the emitter walks the loop:
     an index test at the top of each iteration cannot be skipped by one of the
     emitter's many `continue`s, which a decremented counter silently was. */
  return j - 1;
}

/* An element read: `q2_elem_hit` inlines the array fast path (class, not
   holey, index in bounds, element not a hole or lazy marker). A miss falls into
   the op call, which handles a getter, a string key, a proxy, everything. */
static int emit_native_elem_read(
    FILE *f, const Q2Func *fn, const Q2Insn *in, int is_last, int run) {
  const E2Role *role = e2_roles[in->rop];
  int d, arr, key;

  if (in->rop != ROP_get_element) return 0;
  /* Only where the profile saw numeric elements: a holey array misses the
     inline test on every read, so lowering it there is pure cost. */
  if (!elem_pc_is_numeric(fn, in)) return 0;
  if (role == NULL || role->control != E2_CF_NONE || role->nuse != 2) return 0;
  d = dest_off(fn, in);
  arr = operand_off(in, role->use[0]);
  key = operand_off(in, role->use[1]);
  if (d < 0 || arr < 0 || key < 0) return 0;

  fprintf(f, "g_%d: {\n", in->pc);
  fprintf(f, "    JSValue _e;\n");
  if (run > 0) {
    /* Inside a run: the storage and count are already validated and in C
       locals, so this is a bounds test against the stored count and a load. */
    fprintf(
        f,
        "    if (__builtin_expect(q2_elem_region_get(&_reg, "
        "AOT_REG(fr, %d), &_e), 1)) {\n",
        key);
  } else {
    fprintf(
        f,
        "    if (__builtin_expect(q2_elem_hit(AOT_REG(fr, %d), "
        "AOT_REG(fr, %d), &_e), 1)) {\n",
        arr, key);
  }
  emit_site_hit(f, in);
  /* Borrowed from the array, so the store duplicates it. */
  fprintf(f, "        AOT_SET_B(ctx, fr, %d, _e);\n", d);
  fprintf(f, "        goto g_%d;\n", in->next);
  fprintf(f, "    }\n");
  emit_site_miss(f, in);
  fprintf(f, "}\n");
  (void)is_last;
  return 1;
}

/* An element write: the same fast path, storing in place. The op has no
   destination. */
static int emit_native_elem_write(
    FILE *f, const Q2Func *fn, const Q2Insn *in, int is_last, int run) {
  const E2Role *role = e2_roles[in->rop];
  int arr, key, val;

  /* `put_element` and its xfer form are the same "rrr": object, key, value. The
     xfer form only differs under an owning build, where the source register is
     emptied; under DRC both borrow and the container takes its own reference, so
     the inline store is correct for either. */
  if (in->rop != ROP_put_element && in->rop != ROP_put_element_xfer) return 0;
  if (role == NULL || role->control != E2_CF_NONE) return 0;
  /* The handler reads all three ("rrr" is object, key, value) and writes
     none. */
  if (in->narg >= 3 && in->arg[0].kind == 'r' && in->arg[1].kind == 'r' &&
      in->arg[2].kind == 'r') {
    arr = E2_ROFF(in->arg[0]);
    key = E2_ROFF(in->arg[1]);
    val = E2_ROFF(in->arg[2]);
  } else if (role->nuse == 3) {
    arr = operand_off(in, role->use[0]);
    key = operand_off(in, role->use[1]);
    val = operand_off(in, role->use[2]);
  } else {
    return 0;
  }
  if (arr < 0 || key < 0 || val < 0) return 0;

  fprintf(f, "g_%d: {\n", in->pc);
  if (run > 0) {
    fprintf(
        f,
        "    if (__builtin_expect(q2_elem_region_put(&_reg, "
        "AOT_REG(fr, %d), AOT_REG(fr, %d)), 1)) {\n",
        key, val);
  } else {
    fprintf(
        f,
        "    if (__builtin_expect(q2_elem_put(AOT_REG(fr, %d), "
        "AOT_REG(fr, %d), AOT_REG(fr, %d)), 1)) {\n",
        arr, key, val);
  }
  emit_site_hit(f, in);
  fprintf(f, "        goto g_%d;\n", in->next);
  fprintf(f, "    }\n");
  emit_site_miss(f, in);
  fprintf(f, "}\n");
  (void)fn;
  (void)is_last;
  return 1;
}

/* `branch_if` / `branch_if_not` ("rt": value, target): the truth test is
   `regvm_to_bool`, and its first branch -- any tag at or below UNDEFINED -- is a
   pure int test (`v != 0`), because int, bool, null and undefined all carry their
   truth in the int payload. That covers the hot case; every other tag (an object,
   a string, a BigInt) falls into the op call, because `regvm_to_bool` calls
   `JS_ToBool` for them, which can run `valueOf` and throw.

   The target is a relative offset from the instruction's end. */
static int emit_native_branch_bool(
    FILE *f, const Q2Func *fn, const Q2Insn *in, int is_last) {
  const E2Role *role = e2_roles[in->rop];
  int a, tgt, neg;

  if (in->rop != ROP_branch_if && in->rop != ROP_branch_if_not) return 0;
  if (role == NULL || role->control != E2_CF_BRANCH) return 0;
  a = operand_off(in, role->use[0]);
  tgt = (int)(in->next + q2_first_target_pub(in));
  if (a < 0 || tgt < 0) return 0;
  neg = in->rop == ROP_branch_if_not;

  fprintf(f, "g_%d: {\n", in->pc);
  fprintf(f, "    JSValue _v = AOT_REG(fr, %d);\n", a);
  fprintf(
      f,
      "    if (__builtin_expect((uint32_t)JS_VALUE_GET_TAG(_v) <= "
      "JS_TAG_UNDEFINED, 1)) {\n");
  fprintf(f, "        int _t = JS_VALUE_GET_INT(_v) != 0;\n");
  emit_site_hit(f, in);
  fprintf(f, "        if (_t%s) goto g_%d;\n", neg ? " == 0" : "", tgt);
  fprintf(f, "        goto g_%d;\n", in->next);
  fprintf(f, "    }\n");
  emit_site_miss(f, in);
  fprintf(f, "}\n");
  (void)is_last;
  return 1;
}

/* The branch family that is a pure test: an unconditional `branch`, and the
   null/undefined family, which compares a tag and cannot run JS. */
static int emit_native_branch(
    FILE *f, const Q2Func *fn, const Q2Insn *in, int is_last) {
  const E2Role *role = e2_roles[in->rop];
  int a, tgt;

  if (role == NULL || role->control != E2_CF_BRANCH) return 0;
  switch (in->rop) {
    case ROP_branch:
    case ROP_branch_s:
    case ROP_branch_null:
    case ROP_branch_not_null:
    case ROP_branch_nullish:
    case ROP_branch_not_nullish:
      break;
    default:
      return 0;
  }
  /* The 't' operand is a relative offset from the instruction's end. */
  tgt = (int)(in->next + q2_first_target_pub(in));
  if (tgt < 0) return 0;
  /* The tested register: the role table declares the short null family with no
     uses (it lists the operand as a destination, which the handler does not
     write), so the instruction's own first operand is the reliable source. It is
     "rt" for every op in this family: register then target. */
  a = role->nuse > 0 ? operand_off(in, role->use[0]) : -1;
  if (a < 0 && in->narg > 0 && in->arg[0].kind == 'r') a = E2_ROFF(in->arg[0]);
  if (a < 0 && in->rop != ROP_branch && in->rop != ROP_branch_s) return 0;

  fprintf(f, "g_%d: {\n", in->pc);
  if (in->rop == ROP_branch || in->rop == ROP_branch_s) {
    /* Unconditional: the poll for a back edge happens at the dispatch, so this
       only has to change the pc. */
    fprintf(f, "    goto g_%d;\n", tgt);
    fprintf(f, "}\n");
    return 1;
  }
  {
    const char *test =
        in->rop == ROP_branch_null
            ? "JS_VALUE_GET_TAG(AOT_REG(fr, %d)) == JS_TAG_NULL"
        : in->rop == ROP_branch_not_null
            ? "JS_VALUE_GET_TAG(AOT_REG(fr, %d)) != JS_TAG_NULL"
        : in->rop == ROP_branch_nullish
            ? "(JS_VALUE_GET_TAG(AOT_REG(fr, %d)) == JS_TAG_NULL || "
              "JS_VALUE_GET_TAG(AOT_REG(fr, %d)) == JS_TAG_UNDEFINED)"
            : "(JS_VALUE_GET_TAG(AOT_REG(fr, %d)) != JS_TAG_NULL && "
              "JS_VALUE_GET_TAG(AOT_REG(fr, %d)) != JS_TAG_UNDEFINED)";
    char buf[256];
    snprintf(buf, sizeof(buf), test, a, a);
    fprintf(f, "    if (%s) goto g_%d;\n", buf, tgt);
    fprintf(f, "    goto g_%d;\n", in->next);
  }
  fprintf(f, "}\n");
  (void)fn;
  (void)is_last;
  return 1;
}

/* The unary numeric ops (`neg`, `inc`, `dec`, `not`): one destination, one
   register source, a REG_UN_* sub-operation. `q2_unary_fast` runs the engine's
   own fast path; a miss (a string, a double it declines, an oversized int) falls
   into the op call.

   `dec` carries no sub-op byte in the IR and `q2_rop_sub` has no case for it,
   so its operation is spelled out here from the opcode itself; the handler's
   `REGVM_UN_OP(REG_UN_DEC)` is the same call this emits. */
static int emit_native_unary(
    FILE *f, const Q2Func *fn, const Q2Insn *in, int is_last) {
  const E2Role *role = e2_roles[in->rop];
  int d, a, sub;

  if (role == NULL || role->control != E2_CF_NONE) return 0;
  /* Only the numeric unaries: `typeof`, `to_number` and `to_propkey` take a
     different path and are left as op calls. */
  if (in->rop == ROP_dec)
    sub = REG_UN_DEC;
  else if (in->rop == ROP_neg || in->rop == ROP_inc || in->rop == ROP_not)
    sub = in->sub;
  else
    return 0;
  if (sub < 0) return 0;
  /* Both branches below are kept: `dec`, `not` and `to_numeric` used to reach
     here as operands-free fallbacks and are now modelled in the IR with roles,
     but an op whose role the table does not describe still has to fall back to
     the instruction's own decoded operands. "rr" is destination then source,
     which is also what the handler reads. */
  if (role->nuse > 0 && in->nuse == 1 && in->ndef == 1) {
    d = dest_off(fn, in);
    a = operand_off(in, role->use[0]);
  } else {
    if (in->narg < 2 || in->arg[0].kind != 'r' || in->arg[1].kind != 'r')
      return 0;
    d = E2_ROFF(in->arg[0]);
    a = E2_ROFF(in->arg[1]);
  }
  if (d < 0 || a < 0) return 0;

  fprintf(f, "g_%d: {\n", in->pc);
  fprintf(f, "    JSValue _u;\n");
  fprintf(
      f,
      "    if (__builtin_expect(q2_unary_fast(%d, AOT_REG(fr, %d), &_u), 1)) "
      "{\n",
      sub, a);
  emit_site_hit(f, in);
  fprintf(f, "        AOT_SET_B(ctx, fr, %d, _u);\n", d);
  fprintf(f, "        goto g_%d;\n", in->next);
  fprintf(f, "    }\n");
  emit_site_miss(f, in);
  fprintf(f, "}\n");
  (void)is_last;
  return 1;
}

/* `post_inc` / `post_dec` ("rrr": typed, source, numeric): `typed` gets the
   source converted with ToNumeric and `numeric` gets that value plus or minus
   one, which is why the op has two destinations. The handler runs two unaries
   in sequence (`REGVM_POST_OP`), so the fast path does the same, and a miss on
   either half falls into the op call, which repeats both -- a repeat is safe
   because neither half has a side effect beyond its result.

   The two results are uncounted numbers, so `AOT_SET_B` is the right store: the
   helper's output is a value, not a reference the frame should adopt. */
static int emit_native_post(
    FILE *f, const Q2Func *fn, const Q2Insn *in, int is_last) {
  const E2Role *role = e2_roles[in->rop];
  int d0, d1, src, sign;

  if (in->rop != ROP_post_inc && in->rop != ROP_post_dec) return 0;
  if (role == NULL || role->control != E2_CF_NONE) return 0;
  /* The operand order comes from the instruction, not the role table: "rrr" is
     typed, source, numeric, which is the order the handler reads
     (`REGVM_POST_OP`). The role table now agrees (`def = {0,2}`, `use = {1}`),
     but reading the op's own operands is what keeps this correct for any op the
     table has not been taught. */
  if (in->narg < 3 || in->arg[0].kind != 'r' || in->arg[1].kind != 'r' ||
      in->arg[2].kind != 'r')
    return 0;
  d0 = E2_ROFF(in->arg[0]);
  src = E2_ROFF(in->arg[1]);
  d1 = E2_ROFF(in->arg[2]);
  sign = in->rop == ROP_post_inc ? REG_UN_INC : REG_UN_DEC;

  fprintf(f, "g_%d: {\n", in->pc);
  fprintf(f, "    JSValue _n, _m;\n");
  /* ToNumeric first: the handler derives both results from it, and a string or
     an object it cannot convert falls into the op call. */
  /* ToNumeric is `REG_UN_TO_NUMERIC`, the unary sub-op the engine defines right
     after `REG_UN_DEC`; the layout header carries only `REG_UN_NEG` and
     `REG_UN_INC`, so the value is spelled out here and checked against the one
     constant the header does give (INC), which pins the enum's numbering. */
  fprintf(
      f,
      "    if (__builtin_expect((q2_unary_fast(%d, "
      "AOT_REG(fr, %d), &_n) &&\n",
      Q2_REG_UN_TO_NUMERIC, src);
  fprintf(
      f, "                        q2_unary_fast(%d, _n, &_m)), 1)) {\n", sign);
  emit_site_hit(f, in);
  fprintf(f, "        AOT_SET_B(ctx, fr, %d, _n);\n", d0);
  fprintf(f, "        AOT_SET_B(ctx, fr, %d, _m);\n", d1);
  fprintf(f, "        goto g_%d;\n", in->next);
  fprintf(f, "    }\n");
  emit_site_miss(f, in);
  fprintf(f, "}\n");
  (void)is_last;
  return 1;
}

/* `return` ("r": value): the body leaves with the register's value. The engine's
   `qjs_aot_return` is the whole lowering -- it takes the reference the frame's
   register was borrowing and clears the slot -- so the fast path is one call and
   there is no miss arm at all.

   A generator's `return` is not this: the handler suspends with an undefined
   code and parks the frame instead of leaving, so a generator keeps the op call.
   `JS_FUNC_GENERATOR` is bit 0 of the function kind. */
static int emit_native_return(
    FILE *f, const Q2Func *fn, const Q2Insn *in, int is_last) {
  const E2Role *role = e2_roles[in->rop];
  int a;

  if (in->rop == ROP_return_undefined) {
    if (role == NULL || role->control != E2_CF_RETURN) return 0;
    if (fn->g->func_kind != E2_FUNC_NORMAL) return 0;
    fprintf(f, "g_%d:\n", in->pc);
    fprintf(f, "    return JS_UNDEFINED;\n");
    (void)a;
    (void)is_last;
    return 1;
  }
  if (in->rop != ROP_return) return 0;
  if (role == NULL || role->control != E2_CF_RETURN) return 0;
  /* The generator check, and the same for the returned value: an op call would
     have taken the suspend path, which this direct return cannot. */
  if (fn->g->func_kind != E2_FUNC_NORMAL) return 0;
  a = operand_off(in, role->use[0]);
  if (a < 0) return 0;
  fprintf(f, "g_%d:\n", in->pc);
  fprintf(f, "    return qjs_aot_return(fr, %d);\n", a);
  (void)is_last;
  return 1;
}

/* The fused multiply-and-add family ("rrrrr": dm, x, y, da, c): `dm` gets
   `x * y` and `da` gets that product combined with `c`. Four opcodes, and the
   only difference is which side `c` is on:

     mul_add / mul_sub   da = dm <op> c
     add_mul / sub_mul   da = c  <op> dm

   The order is not symmetric for `-` and it is the order the handler uses, so
   each spells its own. Both halves are `q2_arith`, which declines anything that
   is not a number pair; a miss falls into the op call, which repeats both halves
   (safe: neither has a side effect beyond its results).

   `dm` is stored even when the second half misses, because the handler stores it
   too and a fallback that re-runs the whole instruction reads it back from the
   frame. */
static int emit_native_fused(
    FILE *f, const Q2Func *fn, const Q2Insn *in, int is_last) {
  const E2Role *role = e2_roles[in->rop];
  int dm, x, y, da, c, second;
  int c_first;

  switch (in->rop) {
    case ROP_mul_add:
      second = 1;
      c_first = 0;
      break; /* the layout's ADD */
    case ROP_mul_sub:
      second = 2;
      c_first = 0;
      break; /* the layout's SUB */
    case ROP_add_mul:
      second = 1;
      c_first = 1;
      break;
    case ROP_sub_mul:
      second = 2;
      c_first = 1;
      break;
    default:
      return 0;
  }
  if (role == NULL || role->control != E2_CF_NONE) return 0;
  if (in->narg < 5 || in->arg[0].kind != 'r' || in->arg[1].kind != 'r' ||
      in->arg[2].kind != 'r' || in->arg[3].kind != 'r' ||
      in->arg[4].kind != 'r')
    return 0;
  dm = E2_ROFF(in->arg[0]);
  x = E2_ROFF(in->arg[1]);
  y = E2_ROFF(in->arg[2]);
  da = E2_ROFF(in->arg[3]);
  c = E2_ROFF(in->arg[4]);

  fprintf(f, "g_%d: {\n", in->pc);
  fprintf(f, "    JSValue _t, _r;\n");
  /* The sub-op numbers are the layout header's constants, not the engine's enum
     names: the unit is compiled against the header, and `QJS_AOT_REG_BIN_MUL` is
     0 (the engine's enum starts at MUL). */
  fprintf(
      f,
      "    if (__builtin_expect(q2_arith(QJS_AOT_REG_BIN_MUL, "
      "AOT_REG(fr, %d), AOT_REG(fr, %d), &_t), 1)) {\n",
      x, y);
  fprintf(f, "        AOT_SET_B(ctx, fr, %d, _t);\n", dm);
  if (c_first)
    fprintf(
        f,
        "        if (__builtin_expect(q2_arith(%s, AOT_REG(fr, %d), "
        "AOT_REG(fr, %d), &_r), 1)) {\n",
        second == 1 ? "QJS_AOT_REG_BIN_ADD" : "QJS_AOT_REG_BIN_SUB", c, dm);
  else
    fprintf(
        f,
        "        if (__builtin_expect(q2_arith(%s, AOT_REG(fr, %d), "
        "AOT_REG(fr, %d), &_r), 1)) {\n",
        second == 1 ? "QJS_AOT_REG_BIN_ADD" : "QJS_AOT_REG_BIN_SUB", dm, c);
  emit_site_hit(f, in);
  fprintf(f, "            AOT_SET_B(ctx, fr, %d, _r);\n", da);
  fprintf(f, "            goto g_%d;\n", in->next);
  fprintf(f, "        }\n");
  fprintf(f, "    }\n");
  emit_site_miss(f, in);
  fprintf(f, "}\n");
  (void)is_last;
  return 1;
}

/* A compare-and-branch: `branch_cmp_reg_reg` ("brrt": sub, a, b, target) and
   `branch_cmp_reg_reg_s` ("brrt" narrow). Two ints or two doubles decide inline
   through the prelude's compare; anything else (a string, a mixed pair, a
   BigInt) goes to the op call, because a generic compare can call `valueOf` and
   throw.

   `sub` carries `REG_CMP_INVERT` for the `n`-sense forms: `!(a > b)` is true for
   NaN where `a <= b` is false, so the sense is flipped on the result rather than
   by using the complementary comparison.

   The target is a relative offset from the instruction's end. */
static int emit_native_branch_cmp(
    FILE *f, const Q2Func *fn, const Q2Insn *in, int is_last) {
  const E2Role *role = e2_roles[in->rop];
  int a, b, sub, tgt, inv;
  unsigned base;

  if (in->rop != ROP_branch_cmp_reg_reg && in->rop != ROP_branch_cmp_reg_reg_s)
    return 0;
  if (role == NULL || role->control != E2_CF_BRANCH) return 0;
  if (in->narg < 4 || in->arg[0].kind != 'b' || in->arg[1].kind != 'r' ||
      in->arg[2].kind != 'r')
    return 0;
  sub = (int)in->arg[0].val;
  inv = (sub & REG_CMP_INVERT) != 0;
  base = (unsigned)sub & ~(unsigned)REG_CMP_INVERT;
  /* The fused undefined tests are their own single-tag path (see the handler);
     they are not a compare of two values and are left as op calls. */
  if (base >= (unsigned)REG_BIN_IS_UNDEF &&
      base < (unsigned)REG_BIN_IS_UNDEF + 2)
    return 0;
  a = E2_ROFF(in->arg[1]);
  b = E2_ROFF(in->arg[2]);
  tgt = (int)(in->next + q2_first_target_pub(in));
  if (tgt < 0) return 0;

  fprintf(f, "g_%d: {\n", in->pc);
  fprintf(f, "    JSValue _a = AOT_REG(fr, %d), _b = AOT_REG(fr, %d);\n", a, b);
  fprintf(f, "    int _c = -1, _take;\n");
  /* Braced throughout: the harness refuses a brace-less `if` in emitted C,
     because a guard without a block can silently swallow the statement after
     it -- which for a write-back means an unconditional write turning
     conditional. */
  fprintf(f, "    if (__builtin_expect(JS_VALUE_IS_BOTH_INT(_a, _b), 1)) {\n");
  fprintf(
      f,
      "        _c = q2_cmp_int(%d, JS_VALUE_GET_INT(_a), "
      "JS_VALUE_GET_INT(_b));\n",
      base);
  fprintf(f, "    } else if (JS_VALUE_GET_TAG(_a) == JS_TAG_FLOAT64 &&\n");
  fprintf(f, "             JS_VALUE_GET_TAG(_b) == JS_TAG_FLOAT64) {\n");
  fprintf(
      f,
      "        _c = q2_cmp_double(%d, JS_VALUE_GET_FLOAT64(_a), "
      "JS_VALUE_GET_FLOAT64(_b));\n",
      base);
  fprintf(f, "    }\n");
  fprintf(f, "    if (__builtin_expect(_c >= 0, 1)) {\n");
  emit_site_hit(f, in);
  fprintf(f, "        _take = _c ^ %d;\n", inv ? 1 : 0);
  fprintf(f, "        if (_take) goto g_%d;\n", tgt);
  fprintf(f, "        goto g_%d;\n", in->next);
  fprintf(f, "    }\n");
  emit_site_miss(f, in);
  fprintf(f, "}\n");
  (void)is_last;
  return 1;
}

/* `inc_branch_lt_s` ("rrt": d, b, off): `d = d + 1`, then branch to the target
   when the new value is still `< b`. It is the fused loop increment-and-test, so
   in a loop with an empty body it *is* the body.

   The destination is the same operand as the first use: the increment is in
   place. The IR assigns the definition to that operand's register, so storing to
   it is the edge copy -- the target block's parameter is that same register, and
   the label is emitted after the store, so the jump does not reload a stale
   value. The interrupt poll that every back edge needs is emitted inline before
   the jump, because a self-loop's target is its own pc and would otherwise skip
   the dispatch that polls.

   The fast path is the handler's: both operands `int` and `d` not `INT32_MAX`
   (that one value would overflow). Anything else goes to the op call, which does
   the generic `++` and compare. The store is `AOT_SET_B`: `js_int32` yields a new
   int and a register borrows under DRC, so nothing is released. */
static int emit_native_inc_branch(
    FILE *f, const Q2Func *fn, const Q2Insn *in, int is_last) {
  const E2Role *role = e2_roles[in->rop];
  int d, b, tgt;

  if (in->rop != ROP_inc_branch_lt_s) return 0;
  if (role == NULL || role->control != E2_CF_BRANCH) return 0;
  if (in->narg < 3 || in->arg[0].kind != 'r' || in->arg[1].kind != 'r')
    return 0;
  d = E2_ROFF(in->arg[0]);
  b = E2_ROFF(in->arg[1]);
  tgt = (int)(in->next + q2_first_target_pub(in));
  if (tgt < 0) return 0;

  fprintf(f, "g_%d: {\n", in->pc);
  fprintf(f, "    JSValue _v = AOT_REG(fr, %d), _b = AOT_REG(fr, %d);\n", d, b);
  fprintf(f, "    if (__builtin_expect(JS_VALUE_GET_TAG(_v) == JS_TAG_INT &&\n");
  fprintf(f, "                            JS_VALUE_GET_TAG(_b) == JS_TAG_INT &&\n");
  fprintf(f, "                            JS_VALUE_GET_INT(_v) != INT32_MAX, 1)) {\n");
  fprintf(f, "        int32_t _n = JS_VALUE_GET_INT(_v) + 1;\n");
  emit_site_hit(f, in);
  /* In-place: this is the edge copy into the target block's parameter. */
  fprintf(f, "        AOT_SET_B(ctx, fr, %d, JS_MKVAL(JS_TAG_INT, _n));\n", d);
  fprintf(f, "        if (_n < JS_VALUE_GET_INT(_b)) {\n");
  /* The back edge polls before it jumps, as the dispatch would. */
  fprintf(f, "            if (__builtin_expect(--*(int *)((unsigned char *)ctx +\n");
  fprintf(f, "                    QJS_AOT_OFF_CTX_INTERRUPT_COUNTER) <= 0, 0)) {\n");
  fprintf(f, "                ++*(int *)((unsigned char *)ctx +\n");
  fprintf(f, "                    QJS_AOT_OFF_CTX_INTERRUPT_COUNTER);\n");
  fprintf(f, "                if (qjs_aot_poll(ctx) < 0) { pc = %d; goto exception; }\n", in->pc);
  fprintf(f, "            }\n");
  fprintf(f, "            goto g_%d;\n", tgt);
  fprintf(f, "        }\n");
  fprintf(f, "        goto g_%d;\n", in->next);
  fprintf(f, "    }\n");
  emit_site_miss(f, in);
  fprintf(f, "}\n");
  (void)is_last;
  return 0 == 0;
}

/* `load_this` ("r": d): the frame's receiver, boxed for a non-strict function
   exactly as the handler boxes it. `qjs_aot_load_this` is the same code, so the
   fast path is one call and there is no miss arm; only a boxing failure (which
   can throw) needs the arm and it is an exception, not a fallthrough.

   `move_xfer` ("rr": d, s): copy and empty the source. Under DRC a register
   borrows, so nothing is released and `qjs_aot_move_xfer` is the same two lines
   the handler runs. It cannot fail. */
static int emit_native_frame_ops(
    FILE *f, const Q2Func *fn, const Q2Insn *in, int is_last) {
  const E2Role *role = e2_roles[in->rop];
  int d, s;

  if (in->rop == ROP_load_this) {
    if (role == NULL || role->control != E2_CF_NONE) return 0;
    if (in->narg < 1 || in->arg[0].kind != 'r') return 0;
    d = E2_ROFF(in->arg[0]);
    fprintf(f, "g_%d: {\n", in->pc);
    fprintf(f, "    JSValue _t;\n");
    fprintf(f, "    int _own = qjs_aot_load_this(ctx, f, &_t);\n");
    fprintf(
        f,
        "    if (__builtin_expect(_own < 0, 0)) { pc = %d; "
        "goto exception; }\n",
        in->pc);
    fprintf(f, "    if (_own) qjs_aot_set_reg_p(ctx, fr, %d, _t);\n", d);
    fprintf(f, "    else AOT_SET_B(ctx, fr, %d, _t);\n", d);
    fprintf(f, "    goto g_%d;\n", in->next);
    fprintf(f, "}\n");
    (void)is_last;
    return 1;
  }
  if (in->rop == ROP_move_xfer) {
    if (role == NULL || role->control != E2_CF_NONE) return 0;
    if (in->narg < 2 || in->arg[0].kind != 'r' || in->arg[1].kind != 'r')
      return 0;
    d = E2_ROFF(in->arg[0]);
    s = E2_ROFF(in->arg[1]);
    /* Emitted inline rather than as a call: under QJS_DRC the engine's helper
       is `*dst = *src` (registers borrow, so the dead source is left alone),
       and a call to a function that does one two-word copy is pure overhead --
       the unit is a separate translation unit, so the engine's always_inline
       has no effect here. A coalesced move (d == s) still needs no code. */
    fprintf(f, "g_%d:\n", in->pc);
    /* The DRC test is emitted *into* the unit, so the unit's own build decides
       it -- the generator's flags say nothing about how the unit is compiled.
       Under DRC a register borrows and the helper is one two-word copy; without
       it the source is emptied, which is `AOT_SET_B` plus the helper. A
       coalesced move (d == s) writes nothing in either build. */
    if (d != s) {
      fprintf(
          f,
          "#if QJS_DRC\n    AOT_REG(fr, %d) = AOT_REG(fr, %d);\n#else\n"
          "    AOT_SET_B(ctx, fr, %d, AOT_REG(fr, %d));\n"
          "    qjs_aot_move_xfer(ctx, fr, %d, %d);\n#endif\n",
          d, s, d, s, d, s);
    }
    (void)is_last;
    (void)is_last;
    return 1;
  }
  return 0;
}

/* `get_arg` ("ru": d, slot) and `get_arg2` ("ruru": d1, slot1, d2, slot2): a
   copy out of the argument buffer. The buffer outlives the instruction and a
   register borrows under DRC, so the store duplicates and nothing can fail.

   `get_length` ("rrs": d, recv) reads an own `length`: the fast path is a dense
   fast array's length, and anything else (a string, a typed array, a holey
   array, an object with its own or an inherited `length`) falls into the op
   call, which runs the engine's property read and can therefore run JS.

   The fast path reads the length *property* and requires the element count to
   match it. That is not the same as using `u.array.count` as the length: a
   `new Array(n)` has `count == 0` and `length == n`, so a count-based arm
   returned 0 for it -- a wrong answer, not a missed fast path (`e2ops`
   `call-00.js` caught it). */
static int emit_native_arg_read(
    FILE *f, const Q2Func *fn, const Q2Insn *in, int is_last) {
  const E2Role *role = e2_roles[in->rop];
  int d0, d1, s0, s1;

  if (in->rop == ROP_get_arg) {
    if (role == NULL || role->control != E2_CF_NONE) return 0;
    if (in->narg < 2 || in->arg[0].kind != 'r' || in->arg[1].kind != 'u')
      return 0;
    d0 = E2_ROFF(in->arg[0]);
    s0 = (int)in->arg[1].val;
    fprintf(f, "g_%d:\n", in->pc);
    fprintf(f, "    AOT_SET_B(ctx, fr, %d, q2_arg_buf(f)[%d]);\n", d0, s0);
    (void)is_last;
    return 1;
  }
  if (in->rop == ROP_get_arg2) {
    if (role == NULL || role->control != E2_CF_NONE) return 0;
    if (in->narg < 4 || in->arg[0].kind != 'r' || in->arg[1].kind != 'u' ||
        in->arg[2].kind != 'r' || in->arg[3].kind != 'u')
      return 0;
    d0 = E2_ROFF(in->arg[0]);
    s0 = (int)in->arg[1].val;
    d1 = E2_ROFF(in->arg[2]);
    s1 = (int)in->arg[3].val;
    fprintf(f, "g_%d:\n", in->pc);
    fprintf(f, "    AOT_SET_B(ctx, fr, %d, q2_arg_buf(f)[%d]);\n", d0, s0);
    fprintf(f, "    AOT_SET_B(ctx, fr, %d, q2_arg_buf(f)[%d]);\n", d1, s1);
    fprintf(f, "    goto g_%d;\n", in->next);
    (void)is_last;
    return 1;
  }
  /* "ru": a closure variable read. The engine's own helper is always_inline and
     is exactly `*f->var_refs[slot]->pvalue`, so this is a load through two
     pointers rather than an op call. The value is borrowed from the cell (the
     cell owns it), so the store duplicates. */
  if (in->rop == ROP_get_var_ref) {
    if (role == NULL || role->control != E2_CF_NONE) return 0;
    if (in->narg < 2 || in->arg[0].kind != 'r' || in->arg[1].kind != 'u')
      return 0;
    d0 = E2_ROFF(in->arg[0]);
    s0 = (int)in->arg[1].val;
    fprintf(f, "g_%d:\n", in->pc);
    fprintf(
        f, "    AOT_SET_B(ctx, fr, %d, qjs_aot_get_var_ref(f, %d));\n", d0, s0);
    fprintf(f, "    goto g_%d;\n", in->next);
    (void)is_last;
    return 1;
  }
  return 0;
}

static int emit_native_length(
    FILE *f, const Q2Func *fn, const Q2Insn *in, int is_last) {
  const E2Role *role = e2_roles[in->rop];
  int d, recv;

  if (in->rop != ROP_get_length) return 0;
  if (role == NULL || role->control != E2_CF_NONE) return 0;
  d = dest_off(fn, in);
  recv = operand_off(in, role->use[0]);
  if (d < 0 || recv < 0) return 0;

  fprintf(f, "g_%d: {\n", in->pc);
  fprintf(f, "    double _len;\n");
  fprintf(
      f,
      "    if (__builtin_expect(q2_array_length(AOT_REG(fr, %d), "
      "&_len), 1)) {\n",
      recv);
  emit_site_hit(f, in);
  fprintf(
      f, "        AOT_SET_B(ctx, fr, %d, JS_MKVAL(JS_TAG_INT, (int)_len));\n",
      d);
  fprintf(f, "        goto g_%d;\n", in->next);
  fprintf(f, "    }\n");
  emit_site_miss(f, in);
  fprintf(f, "}\n");
  (void)is_last;
  return 1;
}

/* A move or a constant writes the frame directly: no helper, no call, and no
   way to fail. Everything else is an op call for now.

   READ BY THE GENERIC EMITTER ONLY. The spec body has its own predicate at its
   block-prefix loop. Adding an op here whose lowering exists only in the SPEC body
   makes this emitter drop it silently -- that produced four miscompiles
   (get_arg, get_arg2, move_xfer, and the original get_arg NaN). If a new op is
   spec-body-only, add it to that loop's condition, not here. */
static int generic_is_trivial(const Q2Insn *in) {
  switch (in->rop) {
    case ROP_move:
    case ROP_move_s:
    case ROP_load_i8:
    case ROP_load_i8_s:
    case ROP_load_i32:
    case ROP_load_undefined:
    case ROP_load_null:
    case ROP_load_true:
    case ROP_load_false:
      return 1;
    default:
      return 0;
  }
}

/* A trivial op's constant or copy. The operand indices come from the role
   table, so the emitter never decodes an operand itself. */
static void emit_trivial(FILE *f, const Q2Insn *in, const E2Role *role) {
  fprintf(f, "g_%d:\n", in->pc);
  switch (in->rop) {
    case ROP_move:
    case ROP_move_s: {
      /* move: dst = src. Both are registers, so the copy is one frame store. */
      int d = E2_ROFF(in->arg[0]), s = E2_ROFF(in->arg[1]);
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, AOT_REG(fr, %d));\n", d, s);
      break;
    }
    case ROP_load_i8:
    case ROP_load_i8_s:
    case ROP_load_i32: {
      int d = E2_ROFF(in->arg[0]);
      int32_t v = (int32_t)in->arg[1].val;
      fprintf(
          f, "    AOT_SET_B(ctx, fr, %d, JS_MKVAL(JS_TAG_INT, %d));\n", d, v);
      break;
    }
    case ROP_load_undefined:
    case ROP_load_null:
    case ROP_load_true:
    case ROP_load_false: {
      int d = E2_ROFF(in->arg[0]);
      const char *v = in->rop == ROP_load_undefined ? "JS_UNDEFINED"
                      : in->rop == ROP_load_null    ? "JS_NULL"
                      : in->rop == ROP_load_true    ? "JS_TRUE"
                                                    : "JS_FALSE";
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, %s);\n", d, v);
      break;
    }
    default:
      /* NEVER a comment in place of an instruction. ``generic_is_trivial` said this op is
         trivial and this switch has no case for it, so the generated body would
         silently drop it -- which is how move_xfer lost a transfer and produced
         four separate miscompiles. Abort at generation instead. */
      fprintf(
          stderr,
          "e2: GENERIC emitter: generic_is_trivial accepted rop %d but emit_trivial has "
          "no case for it (pc %d)\n",
          in->rop, in->pc);
      abort();
      break;
  }
  (void)role;
}

/* --- native call sites (E6) -----------------------------------------------
 *
 * A call the profile shows as one known bytecode callee that is compiled into
 * this unit and can be entered on the C stack becomes a plain C call to the
 * callee's native entry: no arena, no ancestor walk, no interpreter activation
 * in between. The identity check is the callee's bound entry against the
 * callee this unit's digest produces, which also covers "disabled", since
 * unbinding clears the entry.
 *
 * Everything else -- an unknown callee, a callee that is not compiled here, a
 * `construct`, a wrong argument count, a receiver that is not the expected
 * function -- runs the generic call path below the fast path: the interpreter's
 * own handler for the instruction, through the op call. That is the same
 * fallback every other native lowering uses, so a miss is never a guess.
 */

/* The register offset of a call's callee operand and of its receiver. `call` is
   "rrrb" (destination, callee, window start, count) and `call_method` is
   "rrrrb" (destination, callee, receiver, window start, count), so the callee
   is operand 1 and a method's receiver is operand 2. */
static int call_callee_off(const Q2Insn *in) {
  return (int)in->arg[1].val;
}
static int call_this_off(const Q2Insn *in) {
  return in->rop == ROP_call_method ? (int)in->arg[2].val : -1;
}

/* Whether this instruction is a call this can lower directly: `call` or
   `call_method` with a destination and a real argument window. `construct`,
   `tail_call`, `apply*` and `eval` keep the generic path (the design's call
   site is written for the two plain forms). */
static int call_site_shaped(const Q2Func *fn, const Q2Insn *in) {
  if (in->rop != ROP_call && in->rop != ROP_call_method) return 0;
  if (in->ndef < 1 || in->win_n < 0) return 0;
  if (in->narg < (in->rop == ROP_call_method ? 5 : 4)) return 0;
  if (dest_off(fn, in) < 0) return 0;
  if ((unsigned)(in->win_reg + in->win_n) > (unsigned)fn->nregs) return 0;
  return 1;
}

/* One direct call to a callee whose digest is `hex`: the guarded C call. The
   guard is the callee's bound entry against the entry this unit's body for that
   digest has. It does not see the engine's per-function disable bit, which
   leaves the entry bound: a direct call still enters a callee the engine
   disabled. E2 bodies do not use that switch (D4), so the callee is always a
   body that may run. A miss falls through to the next link of the chain. */
static void emit_one_direct_call(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const char *hex, int next) {
  int cc = call_callee_off(in);
  int th = call_this_off(in);
  int off = dest_off(fn, in);
  int na = in->win_n;
  int w = in->win_reg * 16;
  char recv[64];

  (void)fn;
  if (th >= 0)
    snprintf(recv, sizeof(recv), "AOT_REG(fr, %d)", th);
  else
    snprintf(recv, sizeof(recv), "JS_UNDEFINED");
  /* Guarded by `Q2_SEL_`: a unit built with `--emit2-only` emits one function
     and still contains call sites in the *other* functions, which the unit does
     not define. The macro exists for exactly this and is set for every emitted
     function. */
  fprintf(f, "#ifdef Q2_NATIVE_%s\n", hex);
  fprintf(
      f,
      "    if (__builtin_expect(q2_call_entry(AOT_REG(fr, %d), "
      "(const void *)aot_f_%s), 1)) {\n",
      cc, hex);
  /* The callee's realm is the context its body runs in, which the entry takes
     from the bytecode; `ctx` here is only the caller's. */
  fprintf(
      f,
      "        JSValue _o = aot_n_%s(ctx, f,\n"
      "            (JSObject *)JS_VALUE_GET_PTR(AOT_REG(fr, %d)), %s,\n"
      "            &AOT_REG(fr, %d), %d);\n",
      hex, cc, recv, w, na);
  fprintf(
      f,
      "        if (__builtin_expect(JS_IsException(_o), 0)) { pc = %d; "
      "goto exception; }\n"
      "        qjs_aot_set_reg_p(ctx, fr, %d, _o);\n"
      "        goto g_%d;\n"
      "    }\n",
      in->pc, off, next);
  fprintf(f, "#endif\n");
}

/* A call site, in the generic body: a chain of identity checks, one direct call
   each, exactly as the design's polymorphic form. A site whose profile names
   one callee emits one link; a site with two to four emits one per callee, in
   the profile's order (hottest first). The chain's last arm falls into the op
   call, which is the engine's own handler for the instruction. */
static int emit_native_call(
    FILE *f, const Q2Func *fn, const Q2Insn *in, int is_last) {
  char hex[4][33];
  unsigned long long hits[4];
  int kind[4], n = 0, k, nb = 0;

  if (e2_nocall_on()) return 0;
  if (!call_site_shaped(fn, in)) return 0;
  /* `hits` is filled unconditionally by the driver, so it cannot be NULL even
     though a call site does not use it. */
  if (!qjs_aot_prof_callees(fn->g->digest, in->next, &n, hex, kind, hits))
    return 0;
  for (k = 0; k < n && k < 4; k++) {
    if (kind[k] != 1) continue;              /* not a bytecode callee */
    if (!qjs_aot_unit_has(hex[k])) continue; /* not compiled into this unit */
    /* A callee a compiled caller can enter on the C stack: its `aot_n_` exists
       only then, and its frame size must be known. The site must also supply
       every declared argument, because the callee reads its parameters from
       the window and publishes `argc`; a short window would read a register
       the caller never wrote. */
    if (qjs_aot_light_need(hex[k]) <= 0) continue;
    if (in->win_n < qjs_aot_unit_argc(hex[k])) continue;
    nb++;
  }
  if (nb == 0) return 0;

  /* The instruction's own label comes first, then the site, then the op call.
     Every path into the instruction -- a `goto g_<pc>` from the previous
     instruction's fast path, the dispatch switch, and a fall-through -- lands
     on the label, so all of them reach the site. Emitting the site as a bare
     block *before* the label is silent breakage: any `goto g_<pc>` in the
     preceding instruction jumps over it, and the call still happens, through
     the op call, without the site ever running. */
  if (g_poison) {
    const E2Role *role = e2_roles[in->rop];
    if (role && poison_applies(in, role)) emit_poison_masks(f, fn, in, role);
  }
  fprintf(f, "g_%d:\n", in->pc);
  fprintf(f, "    { /* direct call site: %d known callee(s) */\n", nb);
  for (k = 0; k < n && k < 4; k++) {
    if (kind[k] != 1) continue;
    if (!qjs_aot_unit_has(hex[k])) continue;
    if (qjs_aot_light_need(hex[k]) <= 0) continue;
    if (in->win_n < qjs_aot_unit_argc(hex[k])) continue;
    emit_one_direct_call(f, fn, in, hex[k], in->next);
  }
  fprintf(f, "    }\n");
  /* The label and the op call that a miss reaches are the caller's, so the
     block here is unlabelled and self-contained. */
  (void)is_last;
  return 1;
}

int e2_emit_generic_body(FILE *f, const Q2Func *fn, char hex[33]) {
  int i, has_ctl;
  g_cur_hex = hex;
  if (!fn->ok) return -1;
  has_ctl = fn->g->ctl_count > 0;

  fprintf(
      f, "E2_TEXT %sJSValue aot_g_%s(JSContext *ctx, JSRegRun *f,\n", e2_linkage(), hex);
  fprintf(
      f, "                                 JSValue *fr, uint32_t entry_pc)\n");
  fprintf(f, "{\n");
  fprintf(f, "    uint8_t *code = q2_code(f);\n");
  fprintf(f, "    uint8_t *ip;\n");
  fprintf(f, "    uint32_t pc = 0;\n");
  fprintf(f, "    uint32_t e2_from_pc = entry_pc;\n");
  fprintf(f, "    const void *pic = q2_pic(f);\n");
  /* The PIC way stride, read once per activation. Every compiled store into the
     frame goes through a pointer, so the compiler cannot prove those stores
     leave the bytecode alone and would reload it at each probe. */
  fprintf(f, "    const uint32_t q2_ns = (uint32_t)q2_pic_sites(f);\n");
  /* The element region: ONE declaration for the whole body, zeroed so that a
     count of 0 means "closed" and every access misses (correct, just slower).
     A mid-body declaration could be bypassed by any entry that skipped its open,
     leaving a wild `vals` pointer. */
  fprintf(f, "    Q2ElemRegion _reg = { 0 };\n");
  fprintf(f, "    void *const *holders = q2_pic_holder(f);\n");
  fprintf(f, "    (void)ip; (void)pc; (void)pic; (void)holders;\n");
  if (g_poison) {
    /* One slot per register: the before-copy the after-check compares with. */
    fprintf(f, "    JSValue e2_poison_copy[%d];\n", fn->nregs ? fn->nregs : 1);
    fprintf(f, "    uint8_t e2_pz[%d];\n", fn->nregs ? fn->nregs : 1);
    fprintf(f, "    (void)e2_poison_copy; (void)e2_pz;\n");
  }
  fprintf(f, "    E2_SEAL(ctx, f);\n");
  /* The first dispatch is an entry, not a back edge, so it does not poll: a
     loop polls on its back edge, a native entry with a call site polls
     (rule P), and a call through the engine polls there. Polling here too cost
     a counter update on every call. Entry at pc 0 is the common case and skips
     the switch as well. */
  if (fn->ninsns > 0 && fn->insns[0].pc == 0)
    fprintf(f, "    if (__builtin_expect(entry_pc == 0, 1)) goto g_0;\n");
  fprintf(f, "    goto dispatch_switch;\n");
  fprintf(f, "dispatch:\n");
  /* G7. A jump to a pc at or below the one that produced it is a back edge, so
     it polls: an inline decrement of the interrupt counter, and the engine only
     when the counter runs out. Without this a compiled loop never yields. */
  fprintf(f, "    if (__builtin_expect(entry_pc <= e2_from_pc, 0)) {\n");
  fprintf(
      f, "        if (__builtin_expect(--*(int *)((unsigned char *)ctx +\n");
  fprintf(
      f, "                QJS_AOT_OFF_CTX_INTERRUPT_COUNTER) <= 0, 0)) {\n");
  fprintf(f, "            ++*(int *)((unsigned char *)ctx +\n");
  fprintf(f, "                QJS_AOT_OFF_CTX_INTERRUPT_COUNTER);\n");
  fprintf(
      f,
      "            if (qjs_aot_poll(ctx) < 0) { pc = e2_from_pc; goto "
      "exception; }\n");
  fprintf(f, "        }\n");
  fprintf(f, "    }\n");
  fprintf(f, "    e2_from_pc = entry_pc;\n");
  fprintf(f, "dispatch_switch:\n");
  /* A jump here may land inside an element region that is no longer open: a spec
     body exit at a mid-run pc, or a resume whose ip fell inside a run. The
     region's cached count is only valid while its run is executing, so it is
     closed here; a closed region misses and takes the op call, which is correct
     and merely slower. */
  fprintf(f, "    _reg.count = 0;\n");
  fprintf(f, "    switch (entry_pc) {\n");
  for (i = 0; i < fn->ninsns; i++)
    fprintf(f, "    case %d: goto g_%d;\n", fn->insns[i].pc, fn->insns[i].pc);
  /* G8: a pc that is not an instruction start must never be dispatched. The
     counters count it so the gate can require zero. */
  fprintf(f, "    default:\n");
  fprintf(f, "        e2_g8_hits++;\n");
  fprintf(
      f,
      "        return qjs_aot_deopt(ctx, f, fr, entry_pc, QJS_AOT_NO_SITE);\n");
  fprintf(f, "    }\n");

  /* Guarded element regions: run_left counts the accesses still served by the
     region opened for this run, run_arr is its array register, and run_open says
     whether the block's opening brace and the `_reg` declaration have been
     emitted for it. */
  {
    int run_last = -1, run_arr = -1, run_open = 0, blk_last = -1;
  for (i = 0; i < fn->ninsns; i++) {
    const Q2Insn *in = &fn->insns[i];
    const E2Role *role = e2_roles[in->rop];

    if (role == NULL) {
      fprintf(stderr, "e2: no role for op %d\n", in->rop);
      return -1;
    }
    /* Close a guarded region as soon as the instruction after its last access
       is reached. Done first, so every path through the body below -- including
       the emitters that `continue` -- sees the closed brace before emitting the
       instruction that ends the run (a call, a store that can grow the array,
       a write to the array's register). */
    if (run_open && i > run_last) {
      fprintf(f, "    }\n");
      run_open = 0;
    }
    if (generic_is_trivial(in)) {
      emit_trivial(f, in, role);
      continue;
    }
    /* A native lowering emits its own label and fast path and returns 1; the
       op call that follows is the miss path and carries the label every
       other instruction uses. */
    /* The call lowering draws no label of its own -- its miss arm falls into
       the op call -- so the op call after it must emit the label. */
    if (emit_native_call(f, fn, in, i + 1 == fn->ninsns)) {
      /* The call site is emitted *after* the instruction's label and the op
         call that follows it carries that label, so every path into the
         instruction -- a `goto g_<pc>` from the previous instruction's fast
         path, the dispatch switch, and a fall-through -- reaches the site. A
         site emitted as a bare fall-through block before the label is skipped
         by any `goto g_<pc>` in the preceding instruction, which is silent: the
         call still happens, through the op call, and the site simply never
         runs. */
      emit_opcall(f, fn, in, i + 1 == fn->ninsns, 0);
      /* The op call may have resized the array (a growth or shrink goes through
         the engine), so the region's cached storage and count are stale: close it
         and let the run's remaining accesses miss to the op call. */
      if (run_open) fprintf(f, "    _reg.count = 0;\n");
      continue;
    }
    /* The block this instruction belongs to, for the run planner: a region's C
       locals live in one emitted block, so a run never crosses a boundary. */
    if (i > blk_last) {
      int b;
      blk_last = fn->ninsns - 1;
      for (b = 0; b < fn->nblocks; b++)
        if (i >= fn->blocks[b].first_insn &&
            i < fn->blocks[b].first_insn + fn->blocks[b].ninsns) {
          blk_last = fn->blocks[b].first_insn + fn->blocks[b].ninsns - 1;
          break;
        }
    }
    if (!run_open) {
      /* Start a guarded region over this array for the run beginning here, so
         the count and the element storage are read once instead of after every
         store. The run ends at the first instruction that could change the
         array (see elem_breaks_run). */
      int last = elem_run_len(fn, i, blk_last, &run_arr);
      if (last > 0) {
        run_last = last;
        /* No label here: the emitter that lowers this instruction emits its own
           `g_<pc>`, and the region's body is the block that follows it. */
        /* Nothing here: `_reg` is declared ONCE at the top of the body, zeroed.
           A mid-body declaration meant any entry that bypassed this open (a spec
           exit at a mid-run pc, or a dispatch resume landing inside the run) saw
           an uninitialised region -- a wild `vals` pointer that corrupted the
           interpreter several frames away. */
        fprintf(
            f,
            "    if (__builtin_expect(q2_elem_open(AOT_REG(fr, %d), &_reg), "
            "1)) {\n",
            run_arr);
        emit_site_hit(f, in);
        run_open = 1;
      }
    }
    if (emit_native_elem_read(f, fn, in, i + 1 == fn->ninsns, run_open) ||
        emit_native_elem_write(f, fn, in, i + 1 == fn->ninsns, run_open) ||
        emit_native_field_read(f, fn, in, i + 1 == fn->ninsns) ||
        emit_native_field_write(f, fn, in, i + 1 == fn->ninsns) ||
        emit_native_field_chain(f, fn, in, i + 1 == fn->ninsns) ||
        emit_native_this_field(f, fn, in, i + 1 == fn->ninsns) ||
        emit_native_global_read(f, fn, in, i + 1 == fn->ninsns) ||
        emit_native_branch(f, fn, in, i + 1 == fn->ninsns) ||
        emit_native_branch_bool(f, fn, in, i + 1 == fn->ninsns) ||
        emit_native_binary(f, fn, in, i + 1 == fn->ninsns) ||
        emit_native_binary_imm(f, fn, in, i + 1 == fn->ninsns) ||
        emit_native_unary(f, fn, in, i + 1 == fn->ninsns) ||
        emit_native_post(f, fn, in, i + 1 == fn->ninsns) ||
        emit_native_length(f, fn, in, i + 1 == fn->ninsns) ||
        emit_native_inc_branch(f, fn, in, i + 1 == fn->ninsns) ||
        emit_native_branch_cmp(f, fn, in, i + 1 == fn->ninsns) ||
        emit_native_fused(f, fn, in, i + 1 == fn->ninsns)) {
      note_opcall(fn, in, 1);
      if (g_poison && role && poison_applies(in, role))
        emit_poison_masks(f, fn, in, role);
      emit_opcall(f, fn, in, i + 1 == fn->ninsns, 0);
      continue;
    }
    /* A return has no miss arm: the fast path is the whole op, so nothing of
       its weight is left as an op call. */
    if (emit_native_return(f, fn, in, i + 1 == fn->ninsns)) continue;
    if (emit_native_frame_ops(f, fn, in, i + 1 == fn->ninsns)) continue;
    if (emit_native_arg_read(f, fn, in, i + 1 == fn->ninsns)) continue;
    note_opcall(fn, in, 0);
    emit_opcall(f, fn, in, i + 1 == fn->ninsns, 1);
  }
  /* A run that reached the last instruction leaves the region open. */
  if (run_open) {
    fprintf(f, "    }\n");
    run_open = 0;
  }
  }

  /* G5: with no control records nothing can catch and there is nothing to
     close, so the exception leaves directly; that also avoids the unwind path,
     which reads the JSStackFrame a light frame does not have. */
  fprintf(f, "exception:\n");
  if (!has_ctl) {
    fprintf(f, "    return JS_EXCEPTION;\n");
  } else {
    fprintf(f, "    {\n");
    fprintf(f, "        uint32_t rp = 0;\n");
    fprintf(f, "        JSValue ex = qjs_aot_unwind(ctx, f, fr, pc, &rp);\n");
    fprintf(f, "        if (JS_IsException(ex)) return JS_EXCEPTION;\n");
    fprintf(f, "        entry_pc = rp;\n");
    fprintf(f, "        goto dispatch;\n");
    fprintf(f, "    }\n");
  }
  fprintf(f, "}\n\n");
  return 0;
}

/* Overflow exits seen while emitting: each gets a stub after the blocks. The
   `cur[]` snapshot is the block-local state AT that instruction -- the stub must
   sync from it, or it writes nothing and the generic body reads the frame's old
   contents (which is what made a deleted overflow check look harmless: the
   resumed body then recomputed from stale-but-correct arguments). */
static int g_e2_ox[256];
static Q2ValueId *g_e2_ox_cur[256];
static int g_e2_ox_regs = 0;
static int g_e2_ox_n = 0;
/* Hand-off histogram: which rop a spec block gave up at, weighted by the profile's
   count for that pc. One line per rop in the generator report, sorted by weight, so
   the next thing to lower is chosen by data instead of found one at a time. */
#define E2_HANDOFF_MAX 512
typedef struct E2HandoffStat {
  uint16_t rop;
  unsigned long long weight;
  int blocks;
} E2HandoffStat;
static E2HandoffStat g_e2_ho[E2_HANDOFF_MAX];
static int g_e2_ho_n = 0;

static void e2_handoff_note(const Q2Func *fn, const Q2Insn *in) {
  int i;
  if (in == NULL) return;
  for (i = 0; i < g_e2_ho_n; i++) {
    if (g_e2_ho[i].rop == in->rop) {
      g_e2_ho[i].weight += qjs_aot_prof_pc_count(fn->g->digest, in->pc);
      g_e2_ho[i].blocks++;
      return;
    }
  }
  if (g_e2_ho_n < E2_HANDOFF_MAX) {
    g_e2_ho[g_e2_ho_n].rop = in->rop;
    g_e2_ho[g_e2_ho_n].weight = qjs_aot_prof_pc_count(fn->g->digest, in->pc);
    g_e2_ho[g_e2_ho_n].blocks = 1;
    g_e2_ho_n++;
  }
}

/* The report: `e2 handoff rop=<n> name=<..> weight=<..> blocks=<..>`, heaviest
   first. */
void e2_handoff_report(FILE *f) {
  int i, j;
  for (i = 0; i < g_e2_ho_n; i++)
    for (j = i + 1; j < g_e2_ho_n; j++)
      if (g_e2_ho[j].weight > g_e2_ho[i].weight) {
        E2HandoffStat t = g_e2_ho[i];
        g_e2_ho[i] = g_e2_ho[j];
        g_e2_ho[j] = t;
      }
  for (i = 0; i < g_e2_ho_n; i++) {
    char nm[64];
    e2_rop_name(g_e2_ho[i].rop, nm, sizeof(nm));
    fprintf(
        f, "e2 handoff rop=%u name=%s weight=%llu blocks=%d\n",
        (unsigned)g_e2_ho[i].rop, nm, g_e2_ho[i].weight, g_e2_ho[i].blocks);
  }
  g_e2_ho_n = 0;
}
/* Argument-guard exits: same treatment, but the sync set is empty -- the guard
   fires before anything is in a local, so the frame still holds what the caller
   wrote and the generic body's g_<pc> reloads the arguments itself. */
static int g_e2_ax[256];
static int g_e2_vx[256];
static int g_e2_dx[256];
static int g_e2_dx_n;
static int g_e2_vx_n;
static int g_e2_ax_n = 0;

/* The function being emitted, for the helpers that need its value table. */
static const Q2Func *g_e2_fn = NULL;
/* The analysis for the function being emitted. The spec body's representations
   come from HERE (E2Types.lb), never from Q2Value.rep: that holds the old
   emitter's inference, whose BOT reads as TAG and which disagrees with the
   analysis on values like a block parameter. */
static const E2Types *g_e2_types = NULL;

/* The local-name prefix for a value, from the analysis. */
static const char *e2_rep_prefix(Q2ValueId v) {
  int lb = (g_e2_types != NULL && v < (Q2ValueId)g_e2_types->nvalues)
               ? g_e2_types->lb[v]
               : E2_LB_TAG;
  switch (lb) {
    case E2_LB_I32: return "i";
    case E2_LB_F64: return "d";
    case E2_LB_BOOL: return "b";
    default: return "v";
  }
}

/* A trivial instruction lowered by the spec body. Rule R1: it emits no `g_`
   label -- those belong to the generic body -- and it never calls the generic
   emitter's per-instruction functions. It is the same store `emit_trivial`
   makes, keyed on the role table so the operands are never decoded here. */
/* The name of a value's local, by its own representation: `i<v>`, `d<v>`,
   `b<v>` or `v<v>`. */
static void e2_local_name(char *buf, size_t n, Q2ValueId v) {
  snprintf(buf, n, "%s%u", e2_rep_prefix(v), (unsigned)v);
}

/* Load an argument slot into a value's local, converting by the value's
   representation: the argument buffer holds a JSValue, and a typed local is not
   one. */
static void e2_load_arg_to_local(FILE *f, Q2ValueId v, int slot, int pc) {
  char dn[32];
  e2_local_name(dn, sizeof(dn), v);
  if (e2_rep_prefix(v)[0] != 'v' &&
      g_e2_ax_n < (int)(sizeof(g_e2_ax) / sizeof(g_e2_ax[0]))) {
    /* One guard per pc, not per destination: get_arg2 has two and both jump to
       the same label. */
    int q, seen = 0;
    for (q = 0; q < g_e2_ax_n; q++)
      if (g_e2_ax[q] == pc) seen = 1;
    if (!seen) g_e2_ax[g_e2_ax_n++] = pc;
  }
  /* An argument the analysis typed needs its TAG CHECK here: the profile said
     this slot holds an int, but the caller may pass a double, a string or
     nothing at all. Reading the payload without checking would use garbage bits
     as an integer. The exit is at this instruction's pc, which is safe because
     nothing has been written to the frame yet. */
  switch (e2_rep_prefix(v)[0]) {
    case 'i':
      fprintf(
          f,
          "    if (__builtin_expect(JS_VALUE_GET_TAG(q2_arg_buf(f)[%d]) != "
          "JS_TAG_INT, 0)) goto ax_%d;\n",
          slot, pc);
      fprintf(f, "    %s = JS_VALUE_GET_INT(q2_arg_buf(f)[%d]);\n", dn, slot);
      break;
    case 'd':
      fprintf(
          f,
          "    if (__builtin_expect(JS_VALUE_GET_TAG(q2_arg_buf(f)[%d]) != "
          "JS_TAG_INT && JS_VALUE_GET_TAG(q2_arg_buf(f)[%d]) != "
          "JS_TAG_FLOAT64, 0)) goto ax_%d;\n",
          slot, slot, pc);
      fprintf(
          f,
          "    %s = JS_VALUE_GET_TAG(q2_arg_buf(f)[%d]) == JS_TAG_INT ? "
          "JS_VALUE_GET_INT(q2_arg_buf(f)[%d]) : "
          "JS_VALUE_GET_FLOAT64(q2_arg_buf(f)[%d]);\n",
          dn, slot, slot, slot);
      break;
    case 'b':
      fprintf(
          f,
          "    if (__builtin_expect(JS_VALUE_GET_TAG(q2_arg_buf(f)[%d]) != "
          "JS_TAG_BOOL, 0)) goto ax_%d;\n",
          slot, pc);
      fprintf(f, "    %s = JS_VALUE_GET_BOOL(q2_arg_buf(f)[%d]);\n", dn, slot);
      break;
    default:
      fprintf(f, "    %s = q2_arg_buf(f)[%d];\n", dn, slot);
      break;
  }
}


/* Copy one local to another across representations. The destination is a JSValue
   local unless both sides agree, so a typed source is boxed on the way -- the
   same conversion the interpreter would perform on the frame. */
static void e2_copy_local(FILE *f, Q2ValueId d, Q2ValueId sv) {
  char dn[32], sn[32];
  const char *dp = e2_rep_prefix(d), *sp = e2_rep_prefix(sv);
  e2_local_name(dn, sizeof(dn), d);
  e2_local_name(sn, sizeof(sn), sv);
  if (dp[0] == sp[0]) {
    fprintf(f, "    %s = %s;\n", dn, sn);
    return;
  }
  if (dp[0] == 'i') {
    /* to i32: a JSValue source can only be narrowed when it is an int, which
       the spec body cannot assume -- so this copy is not lowerable. */
    fprintf(f, "    %s = (int32_t)JS_VALUE_GET_INT(%s);\n", dn, sn);
    return;
  }
  if (dp[0] == 'd') {
    fprintf(f, "    %s = (double)JS_VALUE_GET_INT(%s);\n", dn, sn);
    return;
  }
  /* to a JSValue local: box by the source's representation. */
  if (sp[0] == 'i')
    fprintf(f, "    %s = JS_MKVAL(JS_TAG_INT, %s);\n", dn, sn);
  else if (sp[0] == 'd')
    fprintf(f, "    %s = qjs_aot_box_num(%s, qjs_aot_fitsi(%s));\n", dn, sn, sn);
  else if (sp[0] == 'b')
    fprintf(f, "    %s = JS_MKVAL(JS_TAG_BOOL, %s);\n", dn, sn);
  else
    fprintf(f, "    %s = %s;\n", dn, sn);
}

/* A typed binary arithmetic instruction, speculatively: both operands are i32
   locals and the result fits, so the addition is the machine's. The overflow
   check is the exit `exits[v]` names -- when it fires, nothing has been written
   to the frame yet (the store happens at an exit or an op call), so the generic
   body resumes at this instruction and redoes it in full precision.

   Returns 1 when it emitted the instruction, 0 when the operands or the result
   are not typed i32 and the caller must exit instead. */
static int emit_typed_arith(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const E2Types *t,
    Q2ValueId *cur) {
  Q2ValueId d, a, b;
  const char *op;
  int ld;

  if (in->rop != ROP_add && in->rop != ROP_sub && in->rop != ROP_mul) return 0;
  if (in->ndef < 1 || in->nuse < 2) return 0;
  d = in->def[0];
  a = in->use[0];
  b = in->use[1];
  if (d >= (Q2ValueId)t->nvalues) return 0;
  ld = fn->values[d].reg;
  op = in->rop == ROP_add ? "+" : (in->rop == ROP_sub ? "-" : "*");
  /* (a) The f64 form: the result is a double, so the machine's op is exact and
     cannot overflow -- no exit. An i32 operand converts with (double), which is
     what the interpreter does for a mixed pair. */
  if (t->lb[d] == E2_LB_F64 &&
      (t->lb[a] == E2_LB_F64 || t->lb[a] == E2_LB_I32) &&
      (t->lb[b] == E2_LB_F64 || t->lb[b] == E2_LB_I32)) {
    char an[48], bn[48];
    if (t->lb[a] == E2_LB_F64) snprintf(an, sizeof(an), "d%u", (unsigned)a);
    else snprintf(an, sizeof(an), "(double)i%u", (unsigned)a);
    if (t->lb[b] == E2_LB_F64) snprintf(bn, sizeof(bn), "d%u", (unsigned)b);
    else snprintf(bn, sizeof(bn), "(double)i%u", (unsigned)b);
    fprintf(f, "    d%u = %s %s %s;\n", (unsigned)d, an, op, bn);
    if (cur != NULL && ld >= 0 && ld < fn->nregs) cur[ld] = d;
    return 1;
  }
  if (t->lb[d] != E2_LB_I32) return 0;
  if (t->lb[a] != E2_LB_I32 || t->lb[b] != E2_LB_I32) return 0;
  /* add/sub/mul in int64, then check the result fits: one widening op and one
     compare, and the compare is the exit condition the analysis recorded. */
  fprintf(
      f,
      "    { int64_t _t = (int64_t)i%u %s (int64_t)i%u;\n",
      (unsigned)in->use[0], op, (unsigned)in->use[1]);
  if (t->exits != NULL && (t->exits[d] & E2_EX_OVF)) {
    fprintf(
        f,
        "      if (__builtin_expect(_t < INT32_MIN || _t > INT32_MAX, 0)) "
        "goto ox_%d;\n",
        in->pc);
    if (g_e2_ox_n < (int)(sizeof(g_e2_ox) / sizeof(g_e2_ox[0])) && cur != NULL) {
      Q2ValueId *snap = malloc(sizeof(*snap) * (size_t)(fn->nregs > 0 ? fn->nregs : 1));
      if (snap != NULL) {
        memcpy(snap, cur, sizeof(*snap) * (size_t)fn->nregs);
        g_e2_ox[g_e2_ox_n] = in->pc;
        g_e2_ox_cur[g_e2_ox_n] = snap;
        g_e2_ox_n++;
      }
    }
  }
  fprintf(f, "      i%u = (int32_t)_t; }\n", (unsigned)d);
  if (cur != NULL && ld >= 0 && ld < fn->nregs) cur[ld] = d;
  return 1;
}

/* Generation-time switch: omit the spec back-edge poll (see the call site). */
static int e2_nopoll(void) {
  static int cached = -1;
  if (cached < 0) {
    const char *v = getenv("QJS_AOT_E2_NOPOLL");
    cached = (v != NULL && v[0] == '1');
  }
  return cached;
}

/* A register whose slot a transfer EMPTIED. E2_UNDEF is not a value id, so it
   cannot collide with one; the exit sync writes JS_UNDEFINED for it. */
#define E2_UNDEF ((Q2ValueId)-2)

/* THE bookkeeping for a lowered instruction: which value each register holds now.
   Called once, from the dispatcher, after every lowered instruction -- not from
   inside each lowering. Ten scattered updates meant a new lowering could silently
   forget its own, leaving cur[] pointing at an older value, and cur[] is what an
   exit syncs to the frame.
   A def whose register is out of range is a generator bug, never a reason to
   index register 0. */
static void e2_note_defs(Q2ValueId *cur, const Q2Func *fn, const Q2Insn *in) {
  int k;
  if (cur == NULL) return;
  for (k = 0; k < in->ndef; k++) {
    Q2ValueId d = in->def[k];
    int r;
    if (d == Q2_NO_VALUE || d >= (Q2ValueId)fn->nvalues) continue;
    r = fn->values[d].reg;
    if (r < 0 || r >= fn->nregs) {
      fprintf(
          stderr,
          "e2: def v%d of rop %d has register %d outside 0..%d\n", (int)d,
          in->rop, r, fn->nregs - 1);
      abort();
    }
    cur[r] = d;
  }
}

/* Load a block's live-in register from the frame into the value's local. This is
   the block's entry load (S1): after it the block works on locals only. */
static void e2_load_local_from_frame(
    FILE *f, Q2ValueId v, int reg, const E2Types *t) {
  int off = reg * 16;
  /* The representation comes from the analysis (E2Types.lb), never from
     Q2Value.rep: that holds the OLD emitter's inference, whose BOT means TAG. */
  int lb = (t != NULL && v < (Q2ValueId)t->nvalues) ? t->lb[v] : E2_LB_TAG;
  switch (lb) {
    case E2_LB_I32:
      fprintf(f, "    i%u = JS_VALUE_GET_INT(AOT_REG(fr, %d));\n", (unsigned)v,
              off);
      break;
    case E2_LB_F64:
      fprintf(
          f,
          "    d%u = JS_VALUE_GET_TAG(AOT_REG(fr, %d)) == JS_TAG_INT ? "
          "JS_VALUE_GET_INT(AOT_REG(fr, %d)) : "
          "JS_VALUE_GET_FLOAT64(AOT_REG(fr, %d));\n",
          (unsigned)v, off, off, off);
      break;
    case E2_LB_BOOL:
      fprintf(f, "    b%u = JS_VALUE_GET_BOOL(AOT_REG(fr, %d));\n", (unsigned)v,
              off);
      break;
    default:
      fprintf(f, "    v%u = AOT_REG(fr, %d);\n", (unsigned)v, off);
      break;
  }
}

/* Whether this trivial instruction can be lowered given what the block holds in
   locals. A move whose source is in no local cannot: its value lives only in the
   frame (or nowhere yet), so the block must exit at the move instead. */
static int trivial_is_lowerable(const Q2Insn *in, const Q2ValueId *cur) {
  (void)cur;
  /* R5: a move's source is use[0] and its destination is def[0]; both are named
     by VALUE ID. Consulting cur[] (register -> value) for an operand was wrong:
     cur[] exists only to decide what an EXIT writes back, and it is not
     populated for a value the block itself just defined. */
  if (in->rop == ROP_move || in->rop == ROP_move_s ||
      in->rop == ROP_move_xfer) {
    if (in->nuse < 1 || in->ndef < 1) return 0;
  }
  /* A constant only has a local of its own representation: undefined, null and
     the booleans are a JSValue or an int, never an i32/f64. */
  if (in->rop == ROP_load_undefined || in->rop == ROP_load_null ||
      in->rop == ROP_load_true || in->rop == ROP_load_false) {
    if (in->ndef > 0) {
      const char *p = e2_rep_prefix(in->def[0]);
      if (p[0] == 'i' || p[0] == 'd') return 0;
    }
  }
  return 1;
}

static void emit_trivial_own(
    FILE *f, const Q2Insn *in, const E2Role *role, Q2ValueId *cur) {
  Q2ValueId d = Q2_NO_VALUE;
  (void)role;
  if (in->ndef > 0) d = in->def[0];
  switch (in->rop) {
    case ROP_move:
    case ROP_move_s:
    case ROP_move_xfer: {
      /* The source is use[0], named by value id (R5).
         move_xfer is the TRANSFER form: the generic body also EMPTIES the source
         register, so the source slot must be marked empty here or an exit that
         syncs it writes the old value back (for an object, a second frame
         reference with no dup). Under QJS_DRC registers borrow, so there is no
         dup or free to mirror -- the copy plus the empty marker is the whole
         effect. */
      Q2ValueId sv = in->nuse > 0 ? in->use[0] : Q2_NO_VALUE;
      /* The source must already be in a local; if it is not, this move is not
         lowerable here and the block must exit at it instead. Each side is
         named by ITS OWN representation (S1): a value held as i32 is `i<v>`, not
         `v<v>`. */
      if (d != Q2_NO_VALUE && sv != Q2_NO_VALUE)
        e2_copy_local(f, d, sv);
      if (in->rop == ROP_move_xfer && sv != Q2_NO_VALUE) {
        int sr = g_e2_fn->values[sv].reg;
        if (sr >= 0 && sr < g_e2_fn->nregs && cur != NULL) cur[sr] = E2_UNDEF;
      }
      break;
    }
    case ROP_load_i8:
    case ROP_load_i8_s:
    case ROP_load_i32: {
      int32_t cv = (int32_t)in->arg[1].val;
      if (d == Q2_NO_VALUE) break;
      /* The local's name follows the value's representation, which the analysis
         may have set to TAG even for an integer constant. */
      switch (e2_rep_prefix(d)[0]) {
        case 'i':
          fprintf(f, "    i%u = %d;\n", (unsigned)d, cv);
          break;
        case 'd':
          fprintf(f, "    d%u = %d;\n", (unsigned)d, cv);
          break;
        case 'b':
          fprintf(f, "    b%u = %d;\n", (unsigned)d, cv);
          break;
        default:
          fprintf(f, "    v%u = JS_MKVAL(JS_TAG_INT, %d);\n", (unsigned)d, cv);
          break;
      }
      break;
    }
    case ROP_get_arg:
      if (d != Q2_NO_VALUE)
        e2_load_arg_to_local(f, d, (int)in->arg[1].val, in->pc);
      break;
    case ROP_get_arg2: {
      /* Two argument slots in one instruction: the form nearly every function
         begins with, so without it no block's prefix reaches its arithmetic. */
      Q2ValueId d0 = in->ndef > 0 ? in->def[0] : Q2_NO_VALUE;
      Q2ValueId d1 = in->ndef > 1 ? in->def[1] : Q2_NO_VALUE;
      char n0[32], n1[32];
      if (in->narg >= 4) {
        if (d0 != Q2_NO_VALUE) {
          e2_load_arg_to_local(f, d0, (int)in->arg[1].val, in->pc);
          cur[g_e2_fn->values[d0].reg < g_e2_fn->nregs ? g_e2_fn->values[d0].reg : 0] = d0;
        }
        if (d1 != Q2_NO_VALUE) {
          e2_load_arg_to_local(f, d1, (int)in->arg[3].val, in->pc);
          cur[g_e2_fn->values[d1].reg < g_e2_fn->nregs ? g_e2_fn->values[d1].reg : 0] = d1;
        }
      }
      return;
    }
    /* The constants below name a local, and which name depends on the value's
       representation: the analysis may hold `true` as an `int bN`, and writing
       `vN` would be both undefined and a different local. */
    case ROP_load_undefined:
      if (d != Q2_NO_VALUE)
        fprintf(f, "    v%u = JS_UNDEFINED;\n", (unsigned)d);
      break;
    case ROP_load_null:
      if (d != Q2_NO_VALUE) fprintf(f, "    v%u = JS_NULL;\n", (unsigned)d);
      break;
    case ROP_load_true:
    case ROP_load_false: {
      int t = in->rop == ROP_load_true;
      if (d == Q2_NO_VALUE) break;
      if (e2_rep_prefix(d)[0] == 'b')
        fprintf(f, "    b%u = %d;\n", (unsigned)d, t);
      else
        fprintf(f, "    v%u = %s;\n", (unsigned)d, t ? "JS_TRUE" : "JS_FALSE");
      break;
    }
    default:
      break;
  }
  /* Record which local now holds this register's value. */
  if (d != Q2_NO_VALUE && in->ndef > 0) {
    int r = g_e2_fn->values[d].reg;
    if (r >= 0 && r < g_e2_fn->nregs) cur[r] = d;
  }
}

/* ---- the spec body's exit primitive (E8 step 2a-1) ---------------------- */



/* Write one value's local back to its frame register, in the frame's
   representation. A TAG value is already a JSValue, so it is a plain store; an
   i32 or f64 is boxed the way the interpreter would have it. */
static void e2_exit_store(FILE *f, Q2ValueId v, int reg) {
  /* The register to write is the one being synced, NOT the one the value was
     defined in: an edge can move a value into another register's param, and
     those are different slots. Writing val->reg here silently left the slot the
     generic body actually reads holding its previous contents. */
  int off = reg * 16;
  switch (e2_rep_prefix(v)[0]) {
    case 'i':
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, JS_MKVAL(JS_TAG_INT, i%u));\n", off,
              (unsigned)v);
      break;
    case 'd':
      fprintf(
          f,
          "    AOT_SET_B(ctx, fr, %d, "
          "qjs_aot_box_num(d%u, qjs_aot_fitsi(d%u)));\n",
          off, (unsigned)v, (unsigned)v);
      break;
    case 'b':
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, JS_MKVAL(JS_TAG_BOOL, b%u));\n",
              off, (unsigned)v);
      break;
    default:
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, v%u);\n", off, (unsigned)v);
      break;
  }
}

/* The exit: sync the state the generic body will read, count it, and hand the
   frame to the generic body at `pc`. This is the ONLY bridge from the spec body
   to the generic one (rule R1), and it is correct at any instruction pc because
   the generic body has a label for every instruction.

   The sync writes every register live at `pc`, not only the ones that changed:
   a typed local may hold a value the frame has never seen, and the generic body
   reads the frame unconditionally. */
static void e2_emit_exit(
    FILE *f, const Q2Func *fn, int pc, char hex[33], int fi,
    const Q2ValueId *cur, const uint64_t *live_set, int handoff, int blkidx) {
  int words = fn->lwords, w, r;

  fprintf(f, "    {\n");
  if (live_set != NULL) {
    for (w = 0; w < words; w++) {
      uint64_t bits = live_set[w];
      while (bits != 0) {
        r = w * 64 + __builtin_ctzll(bits);
        bits &= bits - 1;
        if (r >= fn->nregs) continue;
        /* A register the spec body holds in a local is written from it; one it
           never touched already has the right contents in the frame, so it is
           left alone. Writing a stale definition would corrupt exactly that. */
        if (cur == NULL) continue;
        if (cur[r] == E2_UNDEF) {
          fprintf(f, "    AOT_SET_B(ctx, fr, %d, JS_UNDEFINED);\n", r * 16);
          continue;
        }
        if (cur[r] == Q2_NO_VALUE) {
          /* The entry block legitimately has function-start state; anywhere
             else a live register with no value means the bookkeeping lost it. */
          if (blkidx != 0) {
            fprintf(
                stderr,
                "e2: live register %d has no value at an exit in a non-entry "
                "block (fn %s)\n",
                r, hex);
            abort();
          }
          continue;
        }
        e2_exit_store(f, cur[r], r);
      }
    }
  }
  /* A hand-off is the planned exit at an instruction the spec body does not lower
     yet, not a failed speculation. Counting it as a deopt switched nearly every
     spec body off after 257 calls while E8 is incomplete, which would have made
     every measurement in this step time the generic body. */
  fprintf(
      f, "    %s(%d);\n", handoff ? "E2_HANDOFF_NOTE" : "E2_EXIT_NOTE", fi);
  fprintf(f, "    return aot_g_%s(ctx, f, fr, %d);\n", hex, pc);
  fprintf(f, "    }\n");
}

/* An exit at an instruction's pc, computing the live set here. The arithmetic
   exit uses it: the operands are live, the result is not yet. */
static void e2_emit_exit_at(
    FILE *f, const Q2Func *fn, int ins, int pc, char hex[33], int fi,
    const Q2ValueId *cur) {
  uint64_t *lb = calloc((size_t)(fn->lwords > 0 ? fn->lwords : 1), 8);
  if (lb != NULL) q2_live_before(fn, ins, lb);
  e2_emit_exit(f, fn, pc, hex, fi, cur, lb, 0, -1);
  free(lb);
}

/* One edge u->s as a PARALLEL move: temps first, then assign, so a swap is
   order-independent. Pure locals -- only exits write the frame. */
/* An edge's parameter moves are correct by construction: every moved source is
   copied into its own temp FIRST, and only then is every parameter assigned from
   the temps. "All reads before any write" is right for any permutation, including
   a swap or a rotation -- so there is no symbolic check and no "skip the temp when
   it looks safe" case. The extra temps are free: the C compiler removes them. */
static void e2_emit_edge_moves(
    FILE *f, const Q2Func *fn, int b, int si, Q2ValueId *cur) {
  const Q2Block *blk = &fn->blocks[b];
  const Q2Block *s = blk->succ_block[si] >= 0 && blk->succ_block[si] < fn->nblocks
                         ? &fn->blocks[blk->succ_block[si]]
                         : NULL;
  int k, np;
  if (s == NULL) return;
  np = s->nparams;
  if (blk->nedge_args[si] >= 0 && blk->nedge_args[si] < np)
    np = blk->nedge_args[si];
  if (np <= 0) return;
  fprintf(f, "    {\n");
  for (k = 0; k < np; k++) {
    Q2ValueId pv = s->param_val[k];
    if (blk->edge_arg[si][k] == Q2_NO_VALUE) continue;
    switch (e2_rep_prefix(pv)[0]) {
      case 'i': fprintf(f, "      int32_t %s_t%d = ", e2_rep_prefix(pv), k); break;
      case 'd': fprintf(f, "      double %s_t%d = ", e2_rep_prefix(pv), k); break;
      case 'b': fprintf(f, "      int %s_t%d = ", e2_rep_prefix(pv), k); break;
      default: fprintf(f, "      JSValue %s_t%d = ", e2_rep_prefix(pv), k); break;
    }
    {
      Q2ValueId av = blk->edge_arg[si][k];
      char sn[32];
      const char *sp = e2_rep_prefix(av), *dp = e2_rep_prefix(pv);
      e2_local_name(sn, sizeof(sn), av);
      /* Convert from the SOURCE's representation to the destination's, using
         the value each side actually is: a typed source is a C scalar, only a
         TAG source is a JSValue. */
      if (sp[0] == dp[0]) fprintf(f, "%s;\n", sn);
      /* numeric <-> numeric is a cast */
      else if (sp[0] == 'i' && dp[0] == 'd') fprintf(f, "(double)%s;\n", sn);
      else if (sp[0] == 'd' && dp[0] == 'i') fprintf(f, "(int32_t)%s;\n", sn);
      else if (sp[0] == 'i' && dp[0] == 'b') fprintf(f, "(%s != 0);\n", sn);
      else if (sp[0] == 'b' && dp[0] == 'i') fprintf(f, "(int32_t)%s;\n", sn);
      else if (sp[0] == 'b' && dp[0] == 'd') fprintf(f, "(double)%s;\n", sn);
      else if (sp[0] == 'd' && dp[0] == 'b') fprintf(f, "(%s != 0);\n", sn);
      /* a TAG source can be read as any typed local, with the tag checked */
      else if (sp[0] == 'v' && dp[0] == 'i')
        fprintf(f, "(int32_t)JS_VALUE_GET_INT(%s);\n", sn);
      else if (sp[0] == 'v' && dp[0] == 'd')
        fprintf(f, "JS_VALUE_GET_TAG(%s) == JS_TAG_INT ? (double)JS_VALUE_GET_INT(%s) : JS_VALUE_GET_FLOAT64(%s);\n", sn, sn, sn);
      else if (sp[0] == 'v' && dp[0] == 'b')
        fprintf(f, "JS_VALUE_GET_BOOL(%s);\n", sn);
      /* a typed source boxed into a JSValue destination */
      else if (sp[0] == 'i') fprintf(f, "JS_MKVAL(JS_TAG_INT, %s);\n", sn);
      else if (sp[0] == 'd') fprintf(f, "qjs_aot_box_num(%s, qjs_aot_fitsi(%s));\n", sn, sn);
      else if (sp[0] == 'b') fprintf(f, "JS_MKVAL(JS_TAG_BOOL, %s);\n", sn);
      else fprintf(f, "%s;\n", sn);
    }
  }
  for (k = 0; k < np; k++) {
    Q2ValueId pv = s->param_val[k];
    char pn[32];
    if (blk->edge_arg[si][k] == Q2_NO_VALUE) continue;
    e2_local_name(pn, sizeof(pn), pv);
    fprintf(f, "      %s = %s_t%d;\n", pn, e2_rep_prefix(pv), k);
  }
  fprintf(f, "    }\n");
}

/* One arm of a lowered branch: the edge moves for successor k, then the jump to
   THAT successor. One function taking k, so the (succ_block, edge_arg) pairing
   cannot drift between the condition's sense and the moves it is paired with. */
static void e2_emit_arm(
    FILE *f, const Q2Func *fn, int b, int k, Q2ValueId *cur,
    const char *hex) {
  const Q2Block *blk = &fn->blocks[b];
  int sb = blk->succ_block[k];
  if (sb < 0 || sb >= fn->nblocks) {
    /* A target that leaves the function: the caller hands off instead. */
    fprintf(f, "      /* arm %d leaves the function */\n", k);
    return;
  }
  e2_emit_edge_moves(f, fn, b, k, cur);
  /* RULE P / R6: every back edge polls, like the generic dispatch. Without it a
     loop in the spec body cannot be interrupted, and clang proves the loop to be
     side-effect free and replaces it with a closed form -- which is why an
     "empty" loop measured 0 instructions per iteration.
     The sync before the poll is REQUIRED: qjs_aot_poll can run the interrupt
     handler and the GC, and a TAG value held only in a C local is invisible to
     the GC, so the frame must own every live value before the call. */
  /* QJS_AOT_E2_NOPOLL=1 omits the SPEC back-edge poll only (the generic body keeps
     its own). It exists so `m1.sh gates` can build a variant and prove that the
     poll is what stops a spec-body loop; it is a GENERATION switch, so it is part
     of the unit cache key. */
  if (q2_is_back_edge(fn, b, sb) && !e2_nopoll()) {
    const Q2Block *t = &fn->blocks[sb];
    int q;
    fprintf(
        f,
        "      if (__builtin_expect(--*(int *)((unsigned char *)ctx +\n"
        "              QJS_AOT_OFF_CTX_INTERRUPT_COUNTER) <= 0, 0)) {\n");
    fprintf(
        f,
        "          ++*(int *)((unsigned char *)ctx +\n"
        "              QJS_AOT_OFF_CTX_INTERRUPT_COUNTER);\n");
    for (q = 0; q < t->nlive_in; q++) {
      int rr = t->in_reg[q];
      Q2ValueId vv = t->in_val[q];
      if (rr < 0 || rr >= fn->nregs || vv == Q2_NO_VALUE) continue;
      if (cur != NULL && cur[rr] != Q2_NO_VALUE) e2_exit_store(f, cur[rr], rr);
      else e2_exit_store(f, vv, rr);
    }
    fprintf(f, "          if (qjs_aot_poll(ctx) < 0) return JS_EXCEPTION;\n");
    fprintf(f, "      }\n");
  }
  fprintf(f, "      goto s_%d;\n", fn->blocks[sb].start_pc);
}

/* A closure-cell LOAD in the spec body: `get_var_ref` (rop 18, "ru"). The cell is
   heap and owns its value, so this BORROWS it -- no dup, no free (DRC also parks
   any free until a safepoint). R6 still applies: a TAG local that survives a later
   poll or op call is synced to the frame at that point, like every other TAG local.
   A typed site checks the TAG directly rather than calling qjs_aot_to_num, because
   on an object that can run JS (valueOf) and allocate in the middle of the body --
   which would need the sync R6 is not yet doing mid-block. A tag that does not
   match the site's representation is a DEOPT exit at the load's own pc, not a
   conversion: the cell may legitimately hold anything profiling did not see.
   JS_TAG_UNINITIALIZED (TDZ) exits too, so the generic body raises the
   ReferenceError the language requires. */
static int emit_typed_var_ref_load(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const E2Types *t, Q2ValueId *cur,
    int have_types) {
  Q2ValueId d;
  int slot, ld;

  if (!have_types || t == NULL) return 0;
  if (in->rop != ROP_get_var_ref) return 0;
  if (in->ndef < 1 || in->narg < 2) return 0;
  if (in->arg[1].kind != 'u' && in->arg[1].kind != 'i') return 0;
  d = in->def[0];
  slot = (int)in->arg[1].val;
  if (d >= (Q2ValueId)t->nvalues) return 0;
  ld = fn->values[d].reg;

  /* Register the exit pc ONCE per function body, so the vx_ label below is
     emitted exactly once. */
  if (g_e2_vx_n < (int)(sizeof(g_e2_vx) / sizeof(g_e2_vx[0]))) {
    int q, seen = 0;
    for (q = 0; q < g_e2_vx_n; q++)
      if (g_e2_vx[q] == in->pc) seen = 1;
    if (!seen) g_e2_vx[g_e2_vx_n++] = in->pc;
  }
  if (t->lb[d] == E2_LB_I32) {
    fprintf(
        f,
        "    { JSValue _c = qjs_aot_get_var_ref(f, %d);\n"
        "      if (__builtin_expect(JS_VALUE_GET_TAG(_c) != JS_TAG_INT, 0)) "
        "goto vx_%d;\n"
        "      i%u = JS_VALUE_GET_INT(_c); }\n",
        slot, in->pc, (unsigned)d);
  } else if (t->lb[d] == E2_LB_F64) {
    fprintf(
        f,
        "    { JSValue _c = qjs_aot_get_var_ref(f, %d);\n"
        "      int _tg = JS_VALUE_GET_TAG(_c);\n"
        "      if (_tg == JS_TAG_INT) d%u = (double)JS_VALUE_GET_INT(_c);\n"
        "      else if (_tg == JS_TAG_FLOAT64) d%u = JS_VALUE_GET_FLOAT64(_c);\n"
        "      else goto vx_%d; }\n",
        slot, (unsigned)d, (unsigned)d, in->pc);
  } else if (t->lb[d] == E2_LB_BOOL) {
    fprintf(
        f,
        "    { JSValue _c = qjs_aot_get_var_ref(f, %d);\n"
        "      if (__builtin_expect(JS_VALUE_GET_TAG(_c) != JS_TAG_BOOL, 0)) "
        "goto vx_%d;\n"
        "      b%u = JS_VALUE_GET_BOOL(_c); }\n",
        slot, in->pc, (unsigned)d);
  } else {
    /* TAG: borrow the cell's value into a TAG local. No dup (the cell owns it),
       and R6 syncs it before any later safepoint. */
    fprintf(f, "    v%u = qjs_aot_get_var_ref(f, %d);\n", (unsigned)d, slot);
  }
  if (cur != NULL && ld >= 0 && ld < fn->nregs) cur[ld] = d;
  return 1;
}

/* THE SPEC-BODY OP CALL (step 5). The D12 contract in generated code.
   Unused at the moment: the first user is 5b's field miss path. It is committed
   ahead of its user so that path is a thin wrapper, as the plan said.

   An op call is a SAFEPOINT, so the order is fixed:
     1. R6 sync: every live TAG value in cur[] is written to the frame FIRST. The call
        can run JS, allocate and collect, and a TAG value held only in a C local is
        invisible to the GC.
     2. Store the typed operands to the frame, since the handler reads its arguments
        from the register file.
     3. `ip = qjs_aot_step(code + pc, fr, f, ctx)` -- the generic body's own slow path,
        which already decodes every instruction and handles every type.
     4. The three sentinels, and the control-transfer test: an ip that is NOT
        `code + next` means a handler took an exception or the op jumped, which is the
        D11 try rule. The frame is already synced, so that is an ordinary exit.
     5. Reload ONLY what the role may have written, BY VALUE ID, each with the tag
        guard at its definition: the handler returned a JSValue, so a def the analysis
        typed i32/f64/bool is not safe to read blindly. A mismatch is a deopt exit at
        the NEXT pc, because the op has run and its result is in the frame.
   Nothing is forwarded across the call. */
static int emit_typed_opcall(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const E2Types *t, Q2ValueId *cur,
    const uint64_t *live, char hex[33], int fi, int b);
static int emit_typed_opcall(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const E2Types *t, Q2ValueId *cur,
    const uint64_t *live, char hex[33], int fi, int b) {
  const E2Role *role = e2_roles[in->rop];
  int r, any;

  /* The CALLER decides when to use this: a native spec emitter calls it on its
     miss path, and Batch 4's general rule calls it for any non-lowered instruction.
     So the tier test lives there, not here -- one copy of the op-call sequence. */
  if (role == NULL) return 0;
  if (role->control != E2_CF_NONE) return 0;
  /* An op that may write ANY register cannot be reloaded selectively here; the caller
     leaves it to the generic body. (The reload loop below handles the known set.) */
  (void)hex;
  (void)fi;
  (void)b;

  /* 1. R6 sync: every live TAG value to the frame before the call. */
  fprintf(f, "    /* R6: sync live TAG values before the op call (a safepoint) */\n");
  for (r = 0; r < fn->nregs; r++) {
    Q2ValueId v;
    if (cur == NULL) break;
    v = cur[r];
    if (v == Q2_NO_VALUE || v == E2_UNDEF) continue;
    if (v >= (Q2ValueId)t->nvalues) continue;
    if (t->lb[v] != E2_LB_TAG) continue;
    fprintf(f, "    AOT_SET_B(ctx, fr, %d, v%u);\n", r * 16, (unsigned)v);
  }
  (void)live;

  /* 2. Typed operands to the frame (the handler reads the register file). */
  for (r = 0; r < role->nuse; r++) {
    Q2ValueId v = in->nuse > r ? in->use[r] : Q2_NO_VALUE;
    int reg;
    if (v == Q2_NO_VALUE || v >= (Q2ValueId)t->nvalues) continue;
    reg = fn->values[v].reg;
    if (reg < 0 || reg >= fn->nregs) continue;
    if (t->lb[v] == E2_LB_I32)
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, JS_MKVAL(JS_TAG_INT, i%u));\n",
              reg * 16, (unsigned)v);
    else if (t->lb[v] == E2_LB_F64)
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, __JS_NewFloat64(d%u));\n", reg * 16,
              (unsigned)v);
    else if (t->lb[v] == E2_LB_BOOL)
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, JS_MKVAL(JS_TAG_BOOL, b%u));\n",
              reg * 16, (unsigned)v);
  }

  /* 3. The call, and 4. the sentinels plus the control-transfer test. */
  /* The spec body has no `code`/`ip` locals -- those names belong to the generic
     body -- so the wrapper brings its own. */
  fprintf(
      f,
      "    { uint8_t *code_ = q2_code(f), *ip_;\n"
      "    ip_ = qjs_aot_step(code_ + %d, fr, f, ctx);\n",
      in->pc);
  fprintf(
      f,
      "    if (__builtin_expect(ip_ == NULL, 0)) return JS_EXCEPTION;\n");
  fprintf(
      f,
      "    if (__builtin_expect(ip_ == QJS_AOT_COLD_RET || "
      "ip_ == QJS_AOT_SUSPEND, 0)) return qjs_aot_ret(f);\n");
  /* D11: a handler that caught the exception (or an op that jumped) returns an ip
     that is not the next instruction. In the SPEC body this must be an ordinary exit
     (rule R1): there is no `dispatch` label in aot_f_<hex> -- that is the generic
     body's -- so the earlier version compiled only because nothing emitted this yet.
     No further sync is needed: R6 ran before the call and the op wrote its own
     results to the frame. */
  fprintf(f, "    if (__builtin_expect(ip_ != code_ + %d, 0)) {\n", in->next);
  fprintf(f, "        E2_HANDOFF_NOTE(%d);\n", fi);
  fprintf(
      f, "        return aot_g_%s(ctx, f, fr, (uint32_t)(ip_ - code_));\n", hex);
  fprintf(f, "    } }\n");

  /* 5. Reload the defs the role says it wrote, by value id, with the tag guard. */
  /* The guard exits land at the NEXT pc: the op has run, so its result is already in
     the frame and the generic body resumes after it. */
  if (g_e2_dx_n < (int)(sizeof(g_e2_dx) / sizeof(g_e2_dx[0]))) {
    int q, seen = 0;
    for (q = 0; q < g_e2_dx_n; q++)
      if (g_e2_dx[q] == in->next) seen = 1;
    if (!seen) g_e2_dx[g_e2_dx_n++] = in->next;
  }
  any = 0;
  for (r = 0; r < in->ndef; r++) {
    Q2ValueId v = in->def[r];
    int reg;
    if (v == Q2_NO_VALUE || v >= (Q2ValueId)t->nvalues) continue;
    reg = fn->values[v].reg;
    if (reg < 0 || reg >= fn->nregs) continue;
    switch (t->lb[v]) {
      case E2_LB_I32:
        fprintf(
            f,
            "    { JSValue _r = AOT_REG(fr, %d);\n"
            "      if (__builtin_expect(JS_VALUE_GET_TAG(_r) != JS_TAG_INT, 0)) "
            "goto dx_%d;\n"
            "      i%u = JS_VALUE_GET_INT(_r); }\n",
            reg * 16, in->next, (unsigned)v);
        any = 1;
        break;
      case E2_LB_F64:
        fprintf(
            f,
            "    { JSValue _r = AOT_REG(fr, %d); int _tg = JS_VALUE_GET_TAG(_r);\n"
            "      if (_tg == JS_TAG_INT) d%u = (double)JS_VALUE_GET_INT(_r);\n"
            "      else if (_tg == JS_TAG_FLOAT64) d%u = JS_VALUE_GET_FLOAT64(_r);\n"
            "      else goto dx_%d; }\n",
            reg * 16, (unsigned)v, (unsigned)v, in->next);
        any = 1;
        break;
      case E2_LB_BOOL:
        fprintf(
            f,
            "    { JSValue _r = AOT_REG(fr, %d);\n"
            "      if (__builtin_expect(JS_VALUE_GET_TAG(_r) != JS_TAG_BOOL, 0)) "
            "goto dx_%d;\n"
            "      b%u = JS_VALUE_GET_BOOL(_r); }\n",
            reg * 16, in->next, (unsigned)v);
        any = 1;
        break;
      case E2_LB_TAG:
        fprintf(f, "    v%u = AOT_REG(fr, %d);\n", (unsigned)v, reg * 16);
        any = 1;
        break;
      default:
        break;
    }
    if (cur != NULL) cur[reg] = v;
  }
  if (any) fprintf(f, "    /* op call done, defs reloaded with their guards */\n");
  return 1;
}

/* A speculative-body CHAINED FIELD READ: `get_field_chain` ("rrasrras": recv,
   atom1, site1, recv2, atom2, site2, ...). Two hit tests; a miss at EITHER link
   goes through the op-call wrapper for the WHOLE instruction, which re-executes it
   from the start. That is why the receiver is saved before link 1 writes its
   result over it: when the destination register IS the receiver (which the IR does
   whenever the chain's first result is written back over its own operand), the
   wrapper would otherwise re-read the wrong receiver -- the `71901cd` bug. */
static int emit_typed_field_chain(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const E2Types *t, Q2ValueId *cur,
    int have_types, char hex[33], int fi, int b) {
  const E2Role *role = e2_roles[in->rop];
  Q2ValueId rv, d0v, d1v;
  int s1, s2, rreg, d0reg, d1reg;

  if (!have_types || t == NULL) return 0;
  if (in->rop != ROP_get_field_chain && in->rop != ROP_get_field_chain_own)
    return 0;
  if (role == NULL || role->control != E2_CF_NONE) return 0;
  if (in->narg < 8 || in->arg[3].kind != 's' || in->arg[7].kind != 's')
    return 0;
  s1 = (int)in->arg[3].val;
  s2 = (int)in->arg[7].val;
  if (s1 == 0xffff || s2 == 0xffff) return 0;
  if (in->ndef < 2 || in->nuse < 1) return 0;
  rv = in->use[0];
  d0v = in->def[0];
  d1v = in->def[1];
  if (rv >= (Q2ValueId)t->nvalues || d0v >= (Q2ValueId)t->nvalues ||
      d1v >= (Q2ValueId)t->nvalues)
    return 0;
  /* All three are JSValues: the receiver is borrowed from its cell and the
     results are borrowed from the object's slots, so TAG locals throughout. */
  if (t->lb[rv] != E2_LB_TAG || t->lb[d0v] != E2_LB_TAG ||
      t->lb[d1v] != E2_LB_TAG)
    return 0;
  rreg = fn->values[rv].reg;
  d0reg = fn->values[d0v].reg;
  d1reg = fn->values[d1v].reg;
  if (rreg < 0 || rreg >= fn->nregs || d0reg < 0 || d0reg >= fn->nregs ||
      d1reg < 0 || d1reg >= fn->nregs)
    return 0;

  fprintf(f, "    { JSValue _c0, _c1;\n");
  fprintf(f, "      JSValue _rcv = v%u;\n", (unsigned)rv);
  fprintf(
      f,
      "      if (__builtin_expect(!Q2_FORCE_MISS && q2_own_read_hit(pic, holders, "
      "_rcv, %d, (int)q2_ns, &_c0), 1)) {\n",
      s1);
  /* Link 1's result goes to its local; the frame slot is written too, because a
     link-2 miss hands the WHOLE instruction to the wrapper. */
  fprintf(f, "        v%u = _c0;\n", (unsigned)d0v);
  fprintf(f, "        AOT_SET_B(ctx, fr, %d, _c0);\n", d0reg * 16);
  fprintf(
      f,
      "        if (__builtin_expect(!Q2_FORCE_MISS && q2_own_read_hit(pic, holders, "
      "_c0, %d, (int)q2_ns, &_c1), 1)) {\n",
      s2);
  fprintf(f, "          v%u = _c1;\n", (unsigned)d1v);
  fprintf(f, "        } else {\n");
  /* Link 2 missed: restore the receiver and hand the whole instruction over. */
  fprintf(f, "          AOT_SET_B(ctx, fr, %d, _rcv);\n", rreg * 16);
  if (!emit_typed_opcall(f, fn, in, t, cur, NULL, hex, fi, b)) return 0;
  fprintf(f, "        }\n");
  fprintf(f, "      } else {\n");
  fprintf(f, "        AOT_SET_B(ctx, fr, %d, _rcv);\n", rreg * 16);
  if (!emit_typed_opcall(f, fn, in, t, cur, NULL, hex, fi, b)) return 0;
  fprintf(f, "      }\n");
  fprintf(f, "    }\n");
  if (cur != NULL) {
    cur[d0reg] = d0v;
    cur[d1reg] = d1v;
  }
  return 1;
}

/* A speculative-body FIELD WRITE: `put_field` ("rasr": receiver, atom, value,
   site). The hit helper is a plain store (`q2_put_field_hit`), so under DRC there
   is no ownership question on the hit path -- the same call the generic body makes.
   A miss goes to the op-call wrapper: the store may be a setter, which runs JS.
   Operand registers are read from their LOCALS (R4/R5): a TAG operand is synced to
   its frame slot first because the helper takes JSValues from the frame. */
static int emit_typed_field_write(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const E2Types *t, Q2ValueId *cur,
    int have_types, char hex[33], int fi, int b) {
  const E2Role *role = e2_roles[in->rop];
  Q2ValueId rv, vv;
  int site, rreg, vreg, r;

  if (!have_types || t == NULL) return 0;
  if (in->rop != ROP_put_field) return 0;
  if (role == NULL || role->control != E2_CF_NONE) return 0;
  if (in->nuse < 2 || in->narg < 4 || in->arg[2].kind != 's') return 0;
  site = (int)in->arg[2].val;
  rv = in->use[0];
  vv = in->use[1];
  if (rv >= (Q2ValueId)t->nvalues || vv >= (Q2ValueId)t->nvalues) return 0;
  /* The RECEIVER must be a TAG local (a number receiver misses the PIC anyway), but
     the VALUE may be typed: it is boxed for the frame slot the helper reads. */
  if (t->lb[rv] != E2_LB_TAG) return 0;
  rreg = fn->values[rv].reg;
  vreg = fn->values[vv].reg;
  if (rreg < 0 || rreg >= fn->nregs || vreg < 0 || vreg >= fn->nregs) return 0;

  {
    char boxed[64];
    if (t->lb[vv] == E2_LB_I32)
      snprintf(boxed, sizeof(boxed), "JS_MKVAL(JS_TAG_INT, i%u)", (unsigned)vv);
    else if (t->lb[vv] == E2_LB_F64)
      snprintf(boxed, sizeof(boxed), "__JS_NewFloat64(d%u)", (unsigned)vv);
    else if (t->lb[vv] == E2_LB_BOOL)
      snprintf(boxed, sizeof(boxed), "JS_MKVAL(JS_TAG_BOOL, b%u)", (unsigned)vv);
    else
      snprintf(boxed, sizeof(boxed), "v%u", (unsigned)vv);
  fprintf(f, "    {\n");
  fprintf(
      f,
      "      if (__builtin_expect(!Q2_FORCE_MISS && q2_put_field_hit(ctx, pic, "
      "v%u, %d, %s), 1)) {\n",
      (unsigned)rv, site, boxed);
  fprintf(f, "      } else {\n");
  /* The frame must hold the operands before the wrapper syncs and steps. */
  fprintf(f, "        AOT_SET_B(ctx, fr, %d, v%u);\n", rreg * 16, (unsigned)rv);
  if (t->lb[vv] == E2_LB_I32)
    fprintf(f, "        AOT_SET_B(ctx, fr, %d, JS_MKVAL(JS_TAG_INT, i%u));\n",
            vreg * 16, (unsigned)vv);
  else if (t->lb[vv] == E2_LB_F64)
    fprintf(f, "        AOT_SET_B(ctx, fr, %d, __JS_NewFloat64(d%u));\n",
            vreg * 16, (unsigned)vv);
  else if (t->lb[vv] == E2_LB_BOOL)
    fprintf(f, "        AOT_SET_B(ctx, fr, %d, JS_MKVAL(JS_TAG_BOOL, b%u));\n",
            vreg * 16, (unsigned)vv);
  else
    fprintf(f, "        AOT_SET_B(ctx, fr, %d, v%u);\n", vreg * 16, (unsigned)vv);
  if (!emit_typed_opcall(f, fn, in, t, cur, NULL, hex, fi, b)) return 0;
  fprintf(f, "      }\n");
  fprintf(f, "    }\n");
  }
  (void)r;
  return 1;
}

/* A speculative-body FIELD READ: `get_field` / `get_field2` ("rras": dest,
   receiver, atom, site). This is a SEPARATE emitter from the generic body's, on
   purpose: the shared part is the runtime helper (q2_own_read_hit), not the emitted
   text, and refactoring the generic emitter to share its continuation broke the
   generic body the one time it was tried. So rule R1 is honoured by construction --
   no g_<pc> label, no jump to a generic label:
       hit  -> store into the def's local (TAG: plain copy, borrowed under DRC;
               typed: write _f to the frame slot FIRST so the generic body sees the
               result, then guard into the local, mismatch -> deopt at the NEXT pc)
       miss -> emit_typed_opcall, which syncs, stores operands, steps, and reloads.
   The receiver must already be a TAG local (R4: never AOT_REG in the spec body); a
   typed receiver declines, because a number receiver misses the PIC anyway. */
static int emit_typed_field_read(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const E2Types *t, Q2ValueId *cur,
    int have_types, char hex[33], int fi, int b) {
  const E2Role *role = e2_roles[in->rop];
  Q2ValueId dv, rv;
  int site, dreg, rreg;

  if (!have_types || t == NULL) return 0;
  if (in->op != Q2_OP_GET_FIELD) return 0;
  if (in->rop != ROP_get_field && in->rop != ROP_get_field_own &&
      in->rop != ROP_get_field2 && in->rop != ROP_get_field2_own)
    return 0;
  if (role == NULL || role->control != E2_CF_NONE) return 0;
  if (in->ndef < 1 || in->nuse < 1 || in->narg < 4 || in->arg[3].kind != 's')
    return 0;
  site = (int)in->arg[3].val;
  if (getenv("E2_DBG5"))
    fprintf(stderr, "pc%d get_field: site=%d role=%p nuse=%d ndef=%d\n", in->pc, site,
            (void *)role, in->nuse, in->ndef);
  dv = in->def[0];
  rv = in->use[0];
  if (dv >= (Q2ValueId)t->nvalues || rv >= (Q2ValueId)t->nvalues) return 0;
  /* The receiver is a JSValue; a typed receiver would be a number, which misses
     the PIC, so decline and let the op-call path take it. */
  if (t->lb[rv] != E2_LB_TAG) return 0;
  dreg = fn->values[dv].reg;
  rreg = fn->values[rv].reg;
  if (dreg < 0 || dreg >= fn->nregs || rreg < 0 || rreg >= fn->nregs) return 0;

  /* Register the guard exits' pcs ONCE per body, like the other dx_ users. */
  if (g_e2_dx_n < (int)(sizeof(g_e2_dx) / sizeof(g_e2_dx[0]))) {
    int q, seen = 0;
    for (q = 0; q < g_e2_dx_n; q++)
      if (g_e2_dx[q] == in->next) seen = 1;
    if (!seen) g_e2_dx[g_e2_dx_n++] = in->next;
  }
  fprintf(f, "    { JSValue _f;\n");
  fprintf(
      f,
      "      if (__builtin_expect(!Q2_FORCE_MISS && q2_own_read_hit(pic, holders, "
      "v%u, %d, (int)q2_ns, &_f), 1)) {\n",
      (unsigned)rv, site);
  if (t->lb[dv] == E2_LB_TAG) {
    fprintf(f, "        v%u = _f;\n", (unsigned)dv);
  } else {
    /* Write the raw result to the frame FIRST: on a guard mismatch the exit is at
       the next pc and the generic body must find the op's result there. */
    fprintf(f, "        AOT_SET_B(ctx, fr, %d, _f);\n", dreg * 16);
    if (t->lb[dv] == E2_LB_I32)
      fprintf(
          f,
          "        if (__builtin_expect(JS_VALUE_GET_TAG(_f) != JS_TAG_INT, 0)) "
          "goto dx_%d;\n"
          "        i%u = JS_VALUE_GET_INT(_f);\n",
          in->next, (unsigned)dv);
    else if (t->lb[dv] == E2_LB_F64)
      fprintf(
          f,
          "        if (JS_VALUE_GET_TAG(_f) == JS_TAG_INT) d%u = "
          "(double)JS_VALUE_GET_INT(_f);\n"
          "        else if (JS_VALUE_GET_TAG(_f) == JS_TAG_FLOAT64) d%u = "
          "JS_VALUE_GET_FLOAT64(_f);\n"
          "        else goto dx_%d;\n",
          (unsigned)dv, (unsigned)dv, in->next);
    else if (t->lb[dv] == E2_LB_BOOL)
      fprintf(
          f,
          "        if (__builtin_expect(JS_VALUE_GET_TAG(_f) != JS_TAG_BOOL, 0)) "
          "goto dx_%d;\n"
          "        b%u = JS_VALUE_GET_BOOL(_f);\n",
          in->next, (unsigned)dv);
    else
      return 0;
  }
  /* The MISS path is the wrapper -- one copy of the op-call sequence, never an
     inlined duplicate (the reviewer's ruling). */
  fprintf(f, "      } else {\n");
  if (!emit_typed_opcall(f, fn, in, t, cur, NULL, hex, fi, b)) return 0;
  fprintf(f, "      }\n");
  fprintf(f, "    }\n");
  if (cur != NULL) cur[dreg] = dv;
  return 1;
}

/* `binary_cpool` (rop 169, "rbrbc": d, sub, a, kind, idx): the second operand is
   a constant-POOL entry, not a register. The decoding below is the generic body's
   (see its ROP_binary_cpool arm) -- same arg positions, same pool accessors, same
   refusal of a non-finite double -- so the two cannot drift apart on the layout.
   Keyed on E2Types: an f64 result takes the double form (no exit; doubles cannot
   overflow), an i32 result takes the int64 form with the overflow exit, and
   anything else declines so the block hands off. */
static int emit_typed_arith_cpool(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const E2Types *t, Q2ValueId *cur,
    int have_types) {
  Q2ValueId d, a;
  const char *op;
  double c;
  uint32_t kind, idx;
  int tag, ld, sub;

  if (!have_types || t == NULL) return 0;
  if (in->rop != ROP_binary_cpool) return 0;
  if (in->narg < 5 || in->ndef < 1 || in->nuse < 1) return 0;
  sub = (int)in->arg[1].val;
  kind = (uint32_t)in->arg[3].val;
  idx = (uint32_t)in->arg[4].val;
  if (kind != REG_CONST_CPOOL) return 0;
  if (sub != REG_BIN_ADD && sub != REG_BIN_SUB && sub != REG_BIN_MUL &&
      sub != REG_BIN_DIV)
    return 0;
  if (!qjs_aot_pool_num(fn->g, idx, &c)) return 0;
  tag = qjs_aot_pool_tag(fn->g, idx);
  if (tag != E2_POOL_INT && tag != E2_POOL_DOUBLE) return 0;
  d = in->def[0];
  a = in->use[0];
  if (d >= (Q2ValueId)t->nvalues || a >= (Q2ValueId)t->nvalues) return 0;
  ld = fn->values[d].reg;
  op = sub == REG_BIN_ADD ? "+"
       : sub == REG_BIN_SUB ? "-"
       : sub == REG_BIN_MUL ? "*"
                            : "/";
  /* f64 result: the machine's op on a double literal. %a prints an exact hex
     float, which round-trips every double including -0.0; a non-finite entry is
     refused (it does not compile as a literal) and the block hands off. */
  if (t->lb[d] == E2_LB_F64) {
    if (tag == E2_POOL_DOUBLE && !__builtin_isfinite(c)) return 0;
    if (t->lb[a] == E2_LB_F64)
      fprintf(f, "    d%u = d%u %s (%a);\n", (unsigned)d, (unsigned)a, op, c);
    else if (t->lb[a] == E2_LB_I32)
      fprintf(f, "    d%u = (double)i%u %s (%a);\n", (unsigned)d, (unsigned)a, op,
              c);
    else
      return 0;
    if (cur != NULL && ld >= 0 && ld < fn->nregs) cur[ld] = d;
    return 1;
  }
  /* i32 result: only an INT pool entry can keep the value an int. */
  if (t->lb[d] == E2_LB_I32 && tag == E2_POOL_INT &&
      t->lb[a] == E2_LB_I32) {
    int32_t ic = (int32_t)c;
    fprintf(
        f,
        "    { int64_t _t = (int64_t)i%u %s (int64_t)%d;\n"
        "      if (__builtin_expect(_t < INT32_MIN || _t > INT32_MAX, 0)) "
        "goto ox_%d;\n"
        "      i%u = (int32_t)_t; }\n",
        (unsigned)a, op, ic, in->pc, (unsigned)d);
    if (g_e2_ox_n < (int)(sizeof(g_e2_ox) / sizeof(g_e2_ox[0])) && cur != NULL) {
      Q2ValueId *snap =
          malloc(sizeof(*snap) * (size_t)(fn->nregs > 0 ? fn->nregs : 1));
      if (snap != NULL) {
        memcpy(snap, cur, sizeof(*snap) * (size_t)fn->nregs);
        g_e2_ox[g_e2_ox_n] = in->pc;
        g_e2_ox_cur[g_e2_ox_n] = snap;
        g_e2_ox_n++;
      }
    }
    if (cur != NULL && ld >= 0 && ld < fn->nregs) cur[ld] = d;
    return 1;
  }
  return 0;
}

/* A closure-cell STORE in the spec body: `put_var_ref` (rop 19, "ur"). The engine
   handler is
       set_value(ctx, var_refs[slot]->pvalue, js_dup(regs[s]));
   and `set_value` is `{ old = *pval; *pval = new; JS_FreeValue(ctx, old); }`. So this
   is the handler spelled with public names:
       { JSValue *p_ = qjs_aot_var_ref_ptr(f, slot);
         JSValue old_ = *p_;
         *p_ = JS_DupValue(ctx, <boxed>);
         JS_FreeValue(ctx, old_); }
   R6: no sync is needed before this store. Freeing the old value does not call into
   JS (a free never runs a finalizer inline here, and DRC parks objects anyway), and
   JS_DupValue allocates nothing. The box is a JS_MKVAL or the float64 constructor,
   neither of which can GC. If that ever changes, the sync must come FIRST.
   `JS_DupValue` is a no-op for ints, bools and doubles; the call is kept so the shape
   is uniform with the TAG case. */
static int emit_typed_var_ref_store(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const E2Types *t, Q2ValueId *cur,
    int have_types) {
  Q2ValueId a;
  int slot;
  char box[64];

  (void)fn;
  if (!have_types || t == NULL) return 0;
  if (in->rop != ROP_put_var_ref) return 0;
  /* "ur": arg[0] is the SLOT and arg[1] the value register (build.c sets use[0]=1),
     so the slot is arg[0] -- reading arg[1] took the value register as the slot. */
  if (in->nuse < 1 || in->narg < 2) return 0;
  if (in->arg[0].kind != 'u' && in->arg[0].kind != 'i') return 0;
  a = in->use[0];
  slot = (int)in->arg[0].val;
  if (a >= (Q2ValueId)t->nvalues) return 0;
  switch (t->lb[a]) {
    case E2_LB_I32:
      snprintf(box, sizeof(box), "JS_MKVAL(JS_TAG_INT, i%u)", (unsigned)a);
      break;
    case E2_LB_F64:
      snprintf(box, sizeof(box), "__JS_NewFloat64(d%u)", (unsigned)a);
      break;
    case E2_LB_BOOL:
      snprintf(box, sizeof(box), "JS_MKVAL(JS_TAG_BOOL, b%u)", (unsigned)a);
      break;
    case E2_LB_TAG:
      snprintf(box, sizeof(box), "v%u", (unsigned)a);
      break;
    default:
      return 0;
  }
  /* The cell's OLD value is not forwarded: a later read of the cell is a fresh load
     with its own value id, so the IR already says the old value is gone. */
  fprintf(
      f,
      "    { JSValue *_p = qjs_aot_var_ref_ptr(f, %d);\n"
      "      JSValue _old = *_p;\n"
      "      *_p = JS_DupValue(ctx, %s);\n"
      "      JS_FreeValue(ctx, _old); }\n",
      slot, box);
  (void)cur;
  return 1;
}

/* The i32 bitwise and shift group: `bitand`/`bitor`/`bitxor`/`shl`/`sar`/`shr`.
   A typed f64 operand is converted with ToInt32 first (q2_toi32, total, no exit);
   a typed i32 operand is used as is. The result is always i32, which is what the
   interpreter produces. `shr` is the logical shift, so it needs `(uint32_t)`
   operands to fill with zeros rather than sign bits. */
static int emit_typed_bitwise(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const E2Types *t, Q2ValueId *cur,
    int have_types) {
  const char *op;
  Q2ValueId d, a, b;
  int ld;

  if (!have_types || t == NULL) return 0;
  if (in->ndef < 1 || in->nuse < 2) return 0;
  d = in->def[0];
  a = in->use[0];
  b = in->use[1];
  if (d >= (Q2ValueId)t->nvalues) return 0;
  if (t->lb[d] != E2_LB_I32) return 0;
  switch (in->rop) {
    case ROP_bitand: op = "&"; break;
    case ROP_bitor: op = "|"; break;
    case ROP_bitxor: op = "^"; break;
    case ROP_shl: op = "<<"; break;
    case ROP_sar: op = ">>"; break;
    case ROP_shr: op = NULL; break;  /* logical: needs the unsigned cast */
    default: return 0;
  }
  if (t->lb[a] != E2_LB_I32 && t->lb[a] != E2_LB_F64) return 0;
  if (t->lb[b] != E2_LB_I32 && t->lb[b] != E2_LB_F64) return 0;
  {
    char an[48], bn[48];
    if (t->lb[a] == E2_LB_F64) snprintf(an, sizeof(an), "q2_toi32(d%u)", (unsigned)a);
    else snprintf(an, sizeof(an), "i%u", (unsigned)a);
    if (t->lb[b] == E2_LB_F64) snprintf(bn, sizeof(bn), "q2_toi32(d%u)", (unsigned)b);
    else snprintf(bn, sizeof(bn), "i%u", (unsigned)b);
    ld = fn->values[d].reg;
    if (op != NULL)
      fprintf(f, "    i%u = %s %s %s;\n", (unsigned)d, an, op, bn);
    else
      fprintf(
          f, "    i%u = (int32_t)((uint32_t)%s >> ((uint32_t)%s & 31));\n",
          (unsigned)d, an, bn);
    if (cur != NULL && ld >= 0 && ld < fn->nregs) cur[ld] = d;
  }
  return 1;
}

/* `inc_branch_lt_s` ("rrt": d, b, off) as a loop counter: `d = d + 1`, branch
   to the target while `d < b`. It is the fused increment-and-test, so in a loop
   with an empty body it *is* the body.

   The increment is in place and the destination is the same value as the first
   use, so the block's parameter is the counter and the arm's edge move carries
   the incremented value round the back edge. `INT32_MAX` is the one value whose
   increment overflows, and it takes the deopt exit (not a hand-off: the analysis
   said this is an int counter and it is not), which the generic body redoes in
   full precision. */
/* `add_imm_self_branch(_s)` ("rit": d, imm, off). Read the engine handler
   (quickjs.c L_ROP_add_imm_self_branch): it does
       d = d + imm;  BACKEDGE(off); goto target;
   -- there is NO compare. It is the loop UPDATE fused with the UNCONDITIONAL back
   edge; the `i < e` test is a separate instruction at the target block. Treating it
   as a counter test (like inc_branch_lt, which DOES compare) invented a comparison
   and produced a wrong count. So: the i32 add with its overflow deopt, then the one
   successor edge through the arm emitter (edge moves, back-edge poll, goto).
   `imm` is a SIGNED 8-bit operand (RD_OFF8). */
static int emit_typed_add_imm_branch(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const E2Types *t, int b,
    Q2ValueId *cur, int have_types, char hex[33]) {
  const E2Role *role = e2_roles[in->rop];
  const Q2Block *blk = &fn->blocks[b];
  Q2ValueId d, a;
  int32_t imm;
  int ld;

  if (in->rop != ROP_add_imm_self_branch &&
      in->rop != ROP_add_imm_self_branch_s)
    return 0;
  if (role == NULL || role->control != E2_CF_BRANCH) return 0;
  if (!have_types || t == NULL) return 0;
  if (in->ndef < 1 || in->nuse < 1 || in->narg < 3) return 0;
  if (blk->nsucc != 1) return 0;
  d = in->def[0];
  a = in->use[0];
  if (d >= (Q2ValueId)t->nvalues || a >= (Q2ValueId)t->nvalues) return 0;
  if (t->lb[d] != E2_LB_I32 || t->lb[a] != E2_LB_I32) return 0;
  /* RD_OFF8: a signed 8-bit immediate. */
  imm = (int32_t)(int8_t)(in->arg[1].val & 0xff);
  ld = fn->values[d].reg;
  fprintf(
      f,
      "    { int32_t _s;\n"
      "      if (__builtin_expect(__builtin_add_overflow(i%u, (int32_t)%d, &_s), 0))\n"
      "        goto ox_%d;\n"
      "      i%u = _s; }\n",
      (unsigned)a, imm, in->pc, (unsigned)d);
  if (g_e2_ox_n < (int)(sizeof(g_e2_ox) / sizeof(g_e2_ox[0])) && cur != NULL) {
    Q2ValueId *snap =
        malloc(sizeof(*snap) * (size_t)(fn->nregs > 0 ? fn->nregs : 1));
    if (snap != NULL) {
      memcpy(snap, cur, sizeof(*snap) * (size_t)fn->nregs);
      g_e2_ox[g_e2_ox_n] = in->pc;
      g_e2_ox_cur[g_e2_ox_n] = snap;
      g_e2_ox_n++;
    }
  }
  if (cur != NULL && ld >= 0 && ld < fn->nregs) cur[ld] = d;
  fprintf(f, "    /* fused update, then the unconditional back edge */\n");
  e2_emit_arm(f, fn, b, 0, cur, hex);
  return 1;
}

static int emit_typed_inc_branch(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const E2Types *t, int b,
    Q2ValueId *cur, int have_types, char hex[33], int fi) {
  const E2Role *role = e2_roles[in->rop];
  const Q2Block *blk = &fn->blocks[b];
  Q2ValueId dv, av;
  int bv, si_take, si_else;
  char cn[32], cn2[32], bn[32];

  /* Both forms of the counter test: `_s` (short offset) and the register form.
     Also `add_imm_self_branch(_s)` -- `i += k` fused with the same test, which is
     how `for (i = s; i < e; i += 3)` compiles. */
  if (in->rop != ROP_inc_branch_lt_s && in->rop != ROP_inc_branch_lt) return 0;
  if (role == NULL || role->control != E2_CF_BRANCH) return 0;
  if (!have_types || t == NULL || blk->nsucc < 2) return 0;
  if (in->ndef < 1 || in->nuse < 2) return 0;
  dv = in->def[0];
  /* The counter's PREVIOUS value is use[0] -- the block parameter that carries it
     round the loop. def[0] is the NEW value. Reading def as the source emitted
     `i5 = i5 + 1` on an uninitialised local (R5: a def is not a use). */
  av = in->use[0];
  bv = in->use[1];
  if (t->lb[dv] != E2_LB_I32 || t->lb[bv] != E2_LB_I32) return 0;
  e2_local_name(cn, sizeof(cn), dv);
  e2_local_name(cn2, sizeof(cn2), av);
  e2_local_name(bn, sizeof(bn), bv);
  /* succ[0] is the branch target: taken while the incremented counter is still
     below the bound. */
  si_take = 0;
  si_else = 1;
  if (0) {
    /* "rit": arg[0] is the destination register, arg[1] the immediate, arg[2]
       the branch target. Reading arg[2] here would use the TARGET as the step. */
    int32_t k = in->narg > 1 ? (int32_t)in->arg[1].val : 1;
    fprintf(
        f, "    if (__builtin_expect((int64_t)%s + (int64_t)%d <= INT32_MAX &&\n"
           "                             (int64_t)%s + (int64_t)%d >= INT32_MIN, 1)) {\n",
        cn2, k, cn2, k);
    fprintf(f, "      %s = %s + %d;\n", cn, cn2, k);
  } else {
    fprintf(f, "    if (__builtin_expect(%s != INT32_MAX, 1)) {\n", cn2);
    fprintf(f, "      %s = %s + 1;\n", cn, cn2);
  }
  fprintf(f, "    } else {\n");
  /* The counter would overflow: exit at this instruction's own pc, before the
     increment, so the generic body redoes it as a double. */
  fprintf(f, "      /* counter overflow */\n");
  {
    uint64_t *lb = calloc((size_t)(fn->lwords > 0 ? fn->lwords : 1), 8);
    if (lb != NULL) q2_live_before(fn, blk->first_insn + blk->ninsns - 1, lb);
    fprintf(f, "      goto ox_%d;\n", in->pc);
    free(lb);
  }
  fprintf(f, "    }\n");
  fprintf(f, "    if (%s < %s) {\n", cn, bn);
  e2_emit_arm(f, fn, b, si_take, cur, hex);
  fprintf(f, "    } else {\n");
  e2_emit_arm(f, fn, b, si_else, cur, hex);
  fprintf(f, "    }\n");
  /* The overflow exit lives after the block, keyed by this pc. */
  if (g_e2_ox_n < (int)(sizeof(g_e2_ox) / sizeof(g_e2_ox[0]))) {
    Q2ValueId *snap =
        malloc(sizeof(*snap) * (size_t)(fn->nregs > 0 ? fn->nregs : 1));
    if (snap != NULL && cur != NULL) {
      memcpy(snap, cur, sizeof(*snap) * (size_t)fn->nregs);
      g_e2_ox[g_e2_ox_n] = in->pc;
      g_e2_ox_cur[g_e2_ox_n] = snap;
      g_e2_ox_n++;
    } else {
      free(snap);
    }
  }
  (void)fi;
  return 1;
}

/* A typed binary with an IMMEDIATE operand: `binary_imm_s`, `add_imm_s`,
   `sub_imm_s` and `binary_cpool` whose pool entry is a foldable number. One
   operand is a local, the other a C literal, so the operation is the machine's
   with the same int64 overflow check as the register form. Returns 1 when it
   emitted the instruction. */
static int emit_typed_arith_imm(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const E2Types *t, Q2ValueId *cur,
    int have_types) {
  const E2Role *role = e2_roles[in->rop];
  Q2ValueId d, a;
  int sub, ld;
  int32_t imm = 0;
  const char *op;
  int have_imm = 0;

  if (!have_types || t == NULL) return 0;
  if (role == NULL || role->control != E2_CF_NONE) return 0;
  if (in->ndef < 1) return 0;
  switch (in->rop) {
    case ROP_binary_imm_s:
    case ROP_binary_imm:
      if (in->narg < 4 || in->arg[1].kind != 'b') return 0;
      sub = (int)in->arg[1].val;
      if (in->arg[3].kind != 'i' && in->arg[3].kind != 'I') return 0;
      imm = (int32_t)in->arg[3].val;
      have_imm = 1;
      break;
    case ROP_add_imm_s:
    case ROP_add_imm_self_s:
      sub = REG_BIN_ADD;
      if (in->narg < 3 || (in->arg[2].kind != 'i' && in->arg[2].kind != 'I'))
        return 0;
      imm = (int32_t)in->arg[2].val;
      have_imm = 1;
      break;
    case ROP_sub_imm_s:
    case ROP_sub_imm_self_s:
      sub = REG_BIN_SUB;
      if (in->narg < 3 || (in->arg[2].kind != 'i' && in->arg[2].kind != 'I'))
        return 0;
      imm = (int32_t)in->arg[2].val;
      have_imm = 1;
      break;
    default:
      return 0;
  }
  if (!have_imm) return 0;
  d = in->def[0];
  if (d >= (Q2ValueId)t->nvalues) return 0;
  if (in->nuse < 1) return 0;
  a = in->use[0];
  if (a >= (Q2ValueId)t->nvalues) return 0;
  /* A BITWISE or SHIFT immediate (`| 0`, `& 3`, `>> 1`, `>>> 0`, ...) is not an
     arithmetic op: the operand is ToInt32-converted, then the integer op runs
     with the immediate. Branching here, BEFORE the add/sub/mul filter, is what
     makes those reachable -- the filter used to reject every one of them, which
     is why q2_toi32 had no call sites anywhere. */
  {
    int is_bit = sub == REG_BIN_OR || sub == REG_BIN_AND || sub == REG_BIN_XOR ||
                 sub == REG_BIN_SHL || sub == REG_BIN_SAR || sub == REG_BIN_SHR;
    if (is_bit) {
      char xn[48];
      char xrep;
      if (t->lb[a] == E2_LB_I32) {
        snprintf(xn, sizeof(xn), "i%u", (unsigned)a);
        xrep = 'i';
      } else if (t->lb[a] == E2_LB_F64) {
        snprintf(xn, sizeof(xn), "q2_toi32(d%u)", (unsigned)a);
        xrep = 'd';
      } else {
        return 0;
      }
      (void)xrep;
      ld = fn->values[d].reg;
      switch (sub) {
        case REG_BIN_OR:
        case REG_BIN_AND:
        case REG_BIN_XOR: {
          const char *bop = sub == REG_BIN_OR  ? "|"
                            : sub == REG_BIN_AND ? "&"
                                                 : "^";
          if (t->lb[d] != E2_LB_I32) return 0;
          fprintf(f, "    i%u = %s %s %d;\n", (unsigned)d, xn, bop, imm);
          break;
        }
        case REG_BIN_SHL:
          if (t->lb[d] != E2_LB_I32) return 0;
          fprintf(
              f, "    i%u = (int32_t)((uint32_t)%s << ((uint32_t)%d & 31));\n",
              (unsigned)d, xn, imm);
          break;
        case REG_BIN_SAR:
          if (t->lb[d] != E2_LB_I32) return 0;
          fprintf(f, "    i%u = %s >> (%d & 31);\n", (unsigned)d, xn, imm);
          break;
        default: /* REG_BIN_SHR */
          if (t->lb[d] == E2_LB_I32) {
            fprintf(
                f,
                "    { uint32_t _u = (uint32_t)%s >> (%d & 31);\n"
                "      if (__builtin_expect(_u > (uint32_t)INT32_MAX, 0)) "
                "goto ox_%d;\n"
                "      i%u = (int32_t)_u; }\n",
                xn, imm, in->pc, (unsigned)d);
            if (g_e2_ox_n < (int)(sizeof(g_e2_ox) / sizeof(g_e2_ox[0])) &&
                cur != NULL) {
              Q2ValueId *snap = malloc(
                  sizeof(*snap) * (size_t)(fn->nregs > 0 ? fn->nregs : 1));
              if (snap != NULL) {
                memcpy(snap, cur, sizeof(*snap) * (size_t)fn->nregs);
                g_e2_ox[g_e2_ox_n] = in->pc;
                g_e2_ox_cur[g_e2_ox_n] = snap;
                g_e2_ox_n++;
              }
            }
          } else if (t->lb[d] == E2_LB_F64) {
            fprintf(f, "    d%u = (double)((uint32_t)%s >> (%d & 31));\n",
                    (unsigned)d, xn, imm);
          } else {
            return 0;
          }
          break;
      }
      if (cur != NULL && ld >= 0 && ld < fn->nregs) cur[ld] = d;
      return 1;
    }
  }
  if (sub != REG_BIN_ADD && sub != REG_BIN_SUB && sub != REG_BIN_MUL) return 0;
  if (d >= (Q2ValueId)t->nvalues) return 0;
  if (in->nuse < 1) return 0;
  a = in->use[0];
  if (a >= (Q2ValueId)t->nvalues) return 0;
  /* The f64 form (no exit: doubles cannot overflow). */
  if (t->lb[d] == E2_LB_F64 &&
      (t->lb[a] == E2_LB_F64 || t->lb[a] == E2_LB_I32)) {
    const char *fop = sub == REG_BIN_ADD ? "+"
                      : sub == REG_BIN_SUB ? "-"
                                           : "*";
    ld = fn->values[d].reg;
    if (t->lb[a] == E2_LB_F64)
      fprintf(f, "    d%u = d%u %s (double)%d;\n", (unsigned)d, (unsigned)a,
              fop, imm);
    else
      fprintf(f, "    d%u = (double)i%u %s (double)%d;\n", (unsigned)d,
              (unsigned)a, fop, imm);
    if (cur != NULL && ld >= 0 && ld < fn->nregs) cur[ld] = d;
    return 1;
  }
  if (t->lb[d] != E2_LB_I32) return 0;
  if (t->lb[a] != E2_LB_I32) return 0;
  op = sub == REG_BIN_ADD ? "+" : (sub == REG_BIN_SUB ? "-" : "*");
  ld = fn->values[d].reg;
  fprintf(
      f, "    { int64_t _t = (int64_t)i%u %s (int64_t)%d;\n", (unsigned)a, op,
      imm);
  fprintf(
      f,
      "      if (__builtin_expect(_t < INT32_MIN || _t > INT32_MAX, 0)) "
      "goto ox_%d;\n",
      in->pc);
  fprintf(f, "      i%u = (int32_t)_t; }\n", (unsigned)d);
  if (cur && ld >= 0 && ld < fn->nregs) cur[ld] = d;
  if (g_e2_ox_n < (int)(sizeof(g_e2_ox) / sizeof(g_e2_ox[0])) && cur) {
    Q2ValueId *snap =
        malloc(sizeof(*snap) * (size_t)(fn->nregs > 0 ? fn->nregs : 1));
    if (snap) {
      memcpy(snap, cur, sizeof(*snap) * (size_t)fn->nregs);
      g_e2_ox[g_e2_ox_n] = in->pc;
      g_e2_ox_cur[g_e2_ox_n] = snap;
      g_e2_ox_n++;
    }
  }
  return 1;
}

/* `branch_if` / `branch_if_not` on a value the analysis holds as a BOOL: the
   local is already 0 or 1, so the test is the machine's. Both arms go through the
   one-function arm emitter. A TAG value needs a truthiness test (the generic
   body's `JS_VALUE_GET_INT(v) != 0` after a tag range check), which can run
   getters for an object -- not this lowering. */
static int emit_typed_branch_bool(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const E2Types *t, int b,
    Q2ValueId *cur, int have_types, const char *hex) {
  const E2Role *role = e2_roles[in->rop];
  const Q2Block *blk = &fn->blocks[b];
  Q2ValueId a;
  int neg, si_take, si_else;
  char an[32];

  if (in->rop != ROP_branch_if && in->rop != ROP_branch_if_not) return 0;
  if (role == NULL || role->control != E2_CF_BRANCH) return 0;
  if (!have_types || t == NULL || blk->nsucc < 2) return 0;
  if (in->nuse < 1) return 0;
  a = in->use[0];
  if (a >= (Q2ValueId)t->nvalues || t->lb[a] != E2_LB_BOOL) return 0;
  neg = in->rop == ROP_branch_if_not;
  si_take = 0;
  si_else = 1;
  e2_local_name(an, sizeof(an), a);
  fprintf(f, "    if (%s%s) {\n", neg ? "!" : "", an);
  e2_emit_arm(f, fn, b, si_take, cur, hex);
  fprintf(f, "    } else {\n");
  e2_emit_arm(f, fn, b, si_else, cur, hex);
  fprintf(f, "    }\n");
  return 1;
}

/* An UNCONDITIONAL branch (`branch`, `branch_s`): one successor, no condition, so
   it is arm 0 and nothing else. Loops whose back edge is a plain goto still handed
   off at this instruction, which is a large part of the remaining hand-off weight. */
static int emit_typed_goto(
    FILE *f, const Q2Func *fn, const Q2Insn *in, int b, Q2ValueId *cur,
    const char *hex) {
  const E2Role *role = e2_roles[in->rop];
  const Q2Block *blk = &fn->blocks[b];
  if (in->rop != ROP_branch && in->rop != ROP_branch_s) return 0;
  if (role == NULL || role->control != E2_CF_BRANCH) return 0;
  if (blk->nsucc < 1) return 0;
  if (blk->succ_block[0] < 0 || blk->succ_block[0] >= fn->nblocks) return 0;
  fprintf(f, "    /* unconditional branch */\n");
  e2_emit_arm(f, fn, b, 0, cur, hex);
  return 1;
}

/* A typed compare-and-branch on two i32 locals.

   The sense is the reference lowering's, not re-derived: REG_CMP_INVERT in the
   sub-op means the branch is taken on the NEGATED result, so with the invert bit
   set the taken arm is on `!(a op b)`. Successor order is the IR builder's --
   succ[0] is the branch's target, succ[1] the fall-through (build.c), and
   edge_arg[] is per successor in that same order. */
/* Whether the generic body opens an element region at this block's start. Such a
   block cannot be entered by a spec-body label: the generic body declares and
   opens its `_reg` inside `g_<start_pc>`, and jumping to `s_<start_pc>` would
   skip the open and use an uninitialised region -- which showed up as a wild
   pointer in `_reg.vals` and an opcode-0 error deep in the interpreter. */
static int block_opens_region(const Q2Func *fn, int b) {
  const Q2Block *blk = &fn->blocks[b];
  int run_arr = -1;
  int last;
  if (blk->ninsns <= 0) return 0;
  last = blk->first_insn + blk->ninsns - 1;
  /* Does an element region open ANYWHERE in this block? If so, the block's
     instructions after that point read `_reg`, which the generic body declared
     and opened in this block -- so neither a `goto s_<start>` nor an exit at a
     later pc can enter it safely. */
  {
    int i;
    for (i = blk->first_insn; i <= last; i++) {
      if (elem_arr_reg(&fn->insns[i]) >= 0 &&
          elem_run_len(fn, i, last, &run_arr) > 0)
        return 1;
    }
  }
  return 0;
}

static int emit_typed_branch(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const E2Types *t, int b,
    Q2ValueId *cur, int have_types, char hex[33], int fi) {
  static const char *const ops[4] = {"<", "<=", ">", ">="};
  const E2Role *role = e2_roles[in->rop];
  const Q2Block *blk = &fn->blocks[b];
  int sub, inv, base, av, bv, si_take, si_else, sb_take, sb_else;
  char an[32], bn[32];
  (void)hex;
  (void)fi;

  if (in->rop != ROP_branch_cmp_reg_reg && in->rop != ROP_branch_cmp_reg_reg_s)
    return 0;
  if (role == NULL || role->control != E2_CF_BRANCH) return 0;
  if (in->narg < 4 || in->arg[0].kind != 'b' || in->nuse < 2) return 0;
  if (blk->nsucc < 2 || !have_types || t == NULL) return 0;
  sub = (int)in->arg[0].val;
  inv = (sub & REG_CMP_INVERT) != 0;
  base = sub & ~REG_CMP_INVERT;
  /* Ordering compares, equalities, and the BIT-TEST kinds: `if (i & 1)` fuses
     the `&` and the truthiness test into the branch, so its sub is a bitwise
     kind. Declining those kept every `if (i & 1)` off the spec path. */
  {
    int is_order = base >= REG_BIN_LT && base <= REG_BIN_GTE;
    int is_eq = base == REG_BIN_EQ || base == REG_BIN_NEQ ||
                base == REG_BIN_STRICT_EQ || base == REG_BIN_STRICT_NEQ;
    int is_bit = base == REG_BIN_AND || base == REG_BIN_OR ||
                 base == REG_BIN_XOR;
    if (!is_order && !is_eq && !is_bit) return 0;
  }
  av = in->use[0];
  bv = in->use[1];
  if (av >= (Q2ValueId)t->nvalues || bv >= (Q2ValueId)t->nvalues) return 0;
  if ((t->lb[av] != E2_LB_I32 && t->lb[av] != E2_LB_F64) ||
      (t->lb[bv] != E2_LB_I32 && t->lb[bv] != E2_LB_F64))
    return 0;
  /* succ[0] is where the branch GOES when taken; succ[1] is the fall-through. */
  si_take = 0;
  si_else = 1;
  sb_take = blk->succ_block[si_take];
  sb_else = blk->succ_block[si_else];
  if (sb_take < 0 || sb_take >= fn->nblocks) return 0;

  /* The operand expressions, converted to the shared rep: f64 when either side
     is a double, else i32. A bit-test on f64 goes through q2_toi32 first. */
  {
    int want_f64 = t->lb[av] == E2_LB_F64 || t->lb[bv] == E2_LB_F64;
    char xa[48], xb[48];
    if (want_f64 && (base == REG_BIN_AND || base == REG_BIN_OR ||
                     base == REG_BIN_XOR)) {
      snprintf(xa, sizeof(xa), "q2_toi32(%s%u)", t->lb[av] == E2_LB_F64 ? "d" : "i", (unsigned)av);
      snprintf(xb, sizeof(xb), "q2_toi32(%s%u)", t->lb[bv] == E2_LB_F64 ? "d" : "i", (unsigned)bv);
      want_f64 = 0;
    } else if (want_f64) {
      snprintf(xa, sizeof(xa), "%s%u", t->lb[av] == E2_LB_F64 ? "d" : "(double)i", (unsigned)av);
      snprintf(xb, sizeof(xb), "%s%u", t->lb[bv] == E2_LB_F64 ? "d" : "(double)i", (unsigned)bv);
    } else {
      snprintf(xa, sizeof(xa), "i%u", (unsigned)av);
      snprintf(xb, sizeof(xb), "i%u", (unsigned)bv);
    }
    {
      const char *cop;
      switch (base) {
        case REG_BIN_LT: cop = "<"; break;
        case REG_BIN_LTE: cop = "<="; break;
        case REG_BIN_GT: cop = ">"; break;
        case REG_BIN_GTE: cop = ">="; break;
        case REG_BIN_EQ: cop = "=="; break;
        case REG_BIN_NEQ: cop = "!="; break;
        case REG_BIN_STRICT_EQ: cop = "=="; break;
        case REG_BIN_STRICT_NEQ: cop = "!="; break;
        default: cop = "!="; break;  /* bit-test: (a & b) != 0 */
      }
      if (base == REG_BIN_AND)
        fprintf(f, "    if (%s((%s & %s) != 0)) {\n", inv ? "!" : "", xa, xb);
      else if (base == REG_BIN_OR)
        fprintf(f, "    if (%s((%s | %s) != 0)) {\n", inv ? "!" : "", xa, xb);
      else if (base == REG_BIN_XOR)
        fprintf(f, "    if (%s((%s ^ %s) != 0)) {\n", inv ? "!" : "", xa, xb);
      else
        fprintf(f, "    if (%s(%s %s %s)) {\n", inv ? "!" : "", xa, cop, xb);
    }
    e2_emit_arm(f, fn, b, si_take, cur, hex);
    fprintf(f, "    } else {\n");
    e2_emit_arm(f, fn, b, si_else, cur, hex);
    fprintf(f, "    }\n");
    (void)an; (void)bn; (void)ops;
  }
  (void)sb_else;
  return 1;
}

/* The D8 unary operations the spec body lowers.

   `inc`/`dec` on an i32 local: the machine's `+ 1` / `- 1`, with the one value
   that overflows (`INT32_MAX` / `INT32_MIN`) taking the deopt exit at this
   instruction, so the generic body redoes it as a double. `neg` likewise
   (`INT32_MIN` overflows; `-0` is a double and comes out of the f64 form).
   `not` on a bool is a `!`, `bitnot` on an i32 is a `~`, neither can fail.
   Returns 1 when it emitted the instruction. */
static int emit_typed_unary(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const E2Types *t, Q2ValueId *cur,
    int have_types, char hex[33], int fi) {
  Q2ValueId d, a;
  int is_inc, is_dec, is_neg, is_not, is_bitnot;
  (void)fi;

  if (!have_types || t == NULL) return 0;
  is_inc = in->rop == ROP_inc;
  is_dec = in->rop == ROP_dec;
  is_neg = in->rop == ROP_neg;
  is_not = in->rop == ROP_not;
  is_bitnot = 0;  /* bitwise not is a binary op (REG_BIN_NOT), not a unary rop */
  /* `add_imm_self_s`/`sub_imm_self_s` are Q2_OP_UNARY with their own rops, so they
     must be admitted here or the branch below is unreachable. */
  if (!is_inc && !is_dec && !is_neg && !is_not &&
      in->rop != ROP_add_imm_self_s && in->rop != ROP_sub_imm_self_s)
    return 0;
  if (in->ndef < 1 || in->nuse < 1) return 0;
  d = in->def[0];
  a = in->use[0];
  if (d >= (Q2ValueId)t->nvalues || a >= (Q2ValueId)t->nvalues) return 0;
  {
    char dn[32], an[32];
    int ld = fn->values[d].reg;
    /* not: a bool in, a bool out. */
    if (is_not) {
      if (t->lb[a] != E2_LB_BOOL) return 0;
      e2_local_name(dn, sizeof(dn), d);
      e2_local_name(an, sizeof(an), a);
      fprintf(f, "    %s = !%s;\n", dn, an);
      if (cur && ld >= 0 && ld < fn->nregs) cur[ld] = d;
      return 1;
    }
    if (is_bitnot) {
      if (t->lb[a] != E2_LB_I32) return 0;
      e2_local_name(dn, sizeof(dn), d);
      e2_local_name(an, sizeof(an), a);
      fprintf(f, "    %s = ~%s;\n", dn, an);
      if (cur && ld >= 0 && ld < fn->nregs) cur[ld] = d;
      return 1;
    }
    if (in->rop == ROP_add_imm_self_s || in->rop == ROP_sub_imm_self_s) {
      /* "ri": arg[0] is the register (def AND use), arg[1] a SIGNED 8-bit immediate
         (RD_OFF8). `i = i +/- k` with the int32 overflow deopt -- the inc path with
         a constant instead of 1. This is add_imm_self_branch without the jump. */
      int32_t k = (int32_t)(int8_t)(in->narg > 1 ? (in->arg[1].val & 0xff) : 1);
      if (t->lb[d] != E2_LB_I32) return 0;
      e2_local_name(dn, sizeof(dn), d);
      e2_local_name(an, sizeof(an), a);
      fprintf(
          f,
          "    { int32_t _s;\n"
          "      if (__builtin_expect(__builtin_add_overflow(%s, (int32_t)%d, &_s), 0))\n"
          "        goto ox_%d;\n"
          "      %s = _s; }\n",
          an, in->rop == ROP_add_imm_self_s ? k : -k, in->pc, dn);
      if (g_e2_ox_n < (int)(sizeof(g_e2_ox) / sizeof(g_e2_ox[0])) && cur) {
        Q2ValueId *snap =
            malloc(sizeof(*snap) * (size_t)(fn->nregs > 0 ? fn->nregs : 1));
        if (snap) {
          memcpy(snap, cur, sizeof(*snap) * (size_t)fn->nregs);
          g_e2_ox[g_e2_ox_n] = in->pc;
          g_e2_ox_cur[g_e2_ox_n] = snap;
          g_e2_ox_n++;
        }
      }
      if (cur && ld >= 0 && ld < fn->nregs) cur[ld] = d;
      return 1;
    }
    if (is_inc || is_dec) {
      if (t->lb[a] != E2_LB_I32) return 0;
      e2_local_name(dn, sizeof(dn), d);
      e2_local_name(an, sizeof(an), a);
      fprintf(
          f,
          "    if (__builtin_expect(%s != %s, 1)) { %s = %s %s 1; }\n", an,
          is_inc ? "INT32_MAX" : "INT32_MIN", dn, an, is_inc ? "+" : "-");
      fprintf(f, "    else { goto ox_%d; }\n", in->pc);
      if (g_e2_ox_n < (int)(sizeof(g_e2_ox) / sizeof(g_e2_ox[0])) && cur) {
        Q2ValueId *snap =
            malloc(sizeof(*snap) * (size_t)(fn->nregs > 0 ? fn->nregs : 1));
        if (snap) {
          memcpy(snap, cur, sizeof(*snap) * (size_t)fn->nregs);
          g_e2_ox[g_e2_ox_n] = in->pc;
          g_e2_ox_cur[g_e2_ox_n] = snap;
          g_e2_ox_n++;
        }
      }
      if (cur && ld >= 0 && ld < fn->nregs) cur[ld] = d;
      return 1;
    }
    /* neg: i32 overflows only on INT32_MIN; the f64 form cannot fail. */
    if (t->lb[a] == E2_LB_I32) {
      e2_local_name(dn, sizeof(dn), d);
      e2_local_name(an, sizeof(an), a);
      fprintf(
          f, "    if (__builtin_expect(%s != INT32_MIN, 1)) { %s = -%s; }\n",
          an, dn, an);
      fprintf(f, "    else { goto ox_%d; }\n", in->pc);
      if (g_e2_ox_n < (int)(sizeof(g_e2_ox) / sizeof(g_e2_ox[0])) && cur) {
        Q2ValueId *snap =
            malloc(sizeof(*snap) * (size_t)(fn->nregs > 0 ? fn->nregs : 1));
        if (snap) {
          memcpy(snap, cur, sizeof(*snap) * (size_t)fn->nregs);
          g_e2_ox[g_e2_ox_n] = in->pc;
          g_e2_ox_cur[g_e2_ox_n] = snap;
          g_e2_ox_n++;
        }
      }
      if (cur && ld >= 0 && ld < fn->nregs) cur[ld] = d;
      return 1;
    }
    if (t->lb[a] == E2_LB_F64) {
      e2_local_name(dn, sizeof(dn), d);
      e2_local_name(an, sizeof(an), a);
      fprintf(f, "    %s = -%s;\n", dn, an);
      if (cur && ld >= 0 && ld < fn->nregs) cur[ld] = d;
      return 1;
    }
  }
  (void)hex;
  return 0;
}

/* `return` in the spec body: box the returned value from its local into its frame
   slot, then use the generic body's own return path (`qjs_aot_return` reads a
   frame slot). Freeing nothing else is correct: frame registers are released by
   the caller exactly as for the generic body. `return_undefined` needs no slot.

   This is what the hand/ent column has been showing as 8/8 -- every call handed
   off at its return, so no function ever completed on the spec path. */
static int emit_typed_return(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const E2Types *t, Q2ValueId *cur,
    int have_types) {
  const E2Role *role = e2_roles[in->rop];
  (void)fn;
  (void)t;
  (void)cur;
  (void)have_types;
  if (in->rop == ROP_return_undefined) {
    if (role == NULL || role->control != E2_CF_RETURN) return 0;
    fprintf(f, "    return JS_UNDEFINED;\n");
    return 1;
  }
  if (in->rop != ROP_return) return 0;
  if (role == NULL || role->control != E2_CF_RETURN) return 0;
  if (in->nuse < 1) return 0;
  {
    Q2ValueId v = in->use[0];
    int r;
    if (v >= (Q2ValueId)g_e2_fn->nvalues) return 0;
    r = g_e2_fn->values[v].reg;
    if (r < 0 || r >= g_e2_fn->nregs) return 0;
    /* Box into the frame slot the generic return reads, by representation. */
    e2_exit_store(f, v, r);
    fprintf(f, "    return qjs_aot_return(fr, %d);\n", r * 16);
  }
  return 1;
}

/* The speculative body (E8).

   Step 1 is the skeleton: the D4 switch-off counters, the S7 entry (which
   refuses anything but a fresh entry at pc 0, and refuses once speculation has
   been switched off for this function), the S1 locals, and the blocks in
   reverse postorder with their edges -- a parameter move per successor
   parameter, through a temporary so parameters may swap, exactly as
   `q2_emit_edge` does it in the generic body.

   Step 2 replaces each block's body with the typed lowerings. Until then a
   reached block forwards to the generic body at its own start pc, so the
   skeleton is exercised (entry, counters, classification, edges) without
   claiming a single typed value: a mismatch between the two bodies cannot hide
   behind unfinished lowerings, because the fallback is the correct body. */
/* How many functions were refused for the speculative body because they are
   irreducible (a back edge to a block that does not dominate its source). The
   report prints it; it should stay 0 for structured bytecode. */
int e2_irreducible = 0;

int e2_emit_spec_body(FILE *f, const Q2Func *fn, char hex[33]) {
  int fi = qjs_aot_cur_func_idx();
  g_e2_fn = fn;
  E2Facts facts;
  E2Types types;
  int b, k;
  int have_types = 0;

  /* The speculative body emits blocks in reverse postorder and relies on every
     back edge going to a dominator; a function that is irreducible keeps the
     generic body. Checked, not assumed. */
  if (q2_is_reducible(fn) == 1) {
    /* The analyses the typed lowerings read. The emitter never reads
       Q2Value.rep/spec or Q2Insn.fwd: those hold the old lowering's inference,
       which treats BOT as TAG. E8's facts mode is the continuing-miss one, so a
       grouped field access is an ordinary safepoint. */
    if (e2_facts_run(fn, &facts, 0) == 0) {
      if (e2_types_run(fn, &facts, &types) == 0) {
        have_types = 1;
        g_e2_types = &types;
      } else {
        e2_facts_free(&facts);
      }
    }
  }
  if (!have_types) {
    e2_irreducible++;
    fprintf(
        f, "E2_TEXT %sJSValue aot_f_%s(JSContext *ctx, JSRegRun *f,\n",
        e2_linkage(), hex);
    fprintf(
        f, "                                 JSValue *fr, uint32_t entry_pc)\n");
    fprintf(f, "{\n    return aot_g_%s(ctx, f, fr, entry_pc);\n}\n\n", hex);
    return 0;
  }

  fprintf(
      f, "E2_TEXT %sJSValue aot_f_%s(JSContext *ctx, JSRegRun *f,\n",
      e2_linkage(), hex);
  fprintf(
      f, "                                 JSValue *fr, uint32_t entry_pc)\n");
  fprintf(f, "{\n");

  /* S7. Entry. A compiled call passes entry_pc 0; anything else is a resume
     that belongs to the generic body, which has a label for every pc. The
     switch-off counter is checked first and costs one load. */
  fprintf(
      f,
      "    if (__builtin_expect(entry_pc != 0 || e2_off[%d], 0))\n"
      "        return aot_g_%s(ctx, f, fr, entry_pc);\n",
      fi, hex);
  /* The index must be inside the D4 arrays; the generator sized them from the
     function count, so this can only fail if the two disagree. */
  fprintf(
      f, "    _Static_assert(%d < QJS_AOT_E2_NFUNCS, \"e2: function index\");\n",
      fi);
  fprintf(f, "    E2_SEAL(ctx, f);\n");

  /* D9's group cells arrive in E9; until then a NULL pic means there is no
     profile to speculate on and the generic body is the only body. */
  fprintf(f, "    {\n");
  fprintf(f, "    const void *pic = q2_pic(f);\n");
  fprintf(f, "    void *const *holders = q2_pic_holder(f);\n");
  fprintf(f, "    const uint32_t q2_ns = (uint32_t)q2_pic_sites(f);\n");
  fprintf(f, "    (void)q2_ns; (void)holders;\n");
  /* A function with no PIC sites -- a leaf arithmetic function, say -- has no
     PIC table at all, so `pic` is NULL on EVERY call. Refusing on that refused
     every such function for good (E2_REFUSE_NOTE switches a function off after
     enough entries), and the speculative body never ran once. The table is only
     needed by accesses, so the check is emitted only when the function has
     sites; and the count is known here, at generation time. */
  if (e2_pic_sites(fn) > 0)
    fprintf(
        f,
        "    if (__builtin_expect(pic == NULL, 0)) {\n"
        "        E2_REFUSE_NOTE(%d);\n"
        "        return aot_g_%s(ctx, f, fr, 0);\n"
        "    }\n",
        fi, hex);
  /* Counted only now: everything that could refuse the entry has been passed, so
     this measures speculative bodies that ACTUALLY RAN. */
  fprintf(f, "    E2_ENTER_NOTE(%d);\n", fi);

  /* S1. One C local per value, by representation. A value that is not live is
     not declared: the IR keeps values that trivial-parameter removal dropped. */
  {
    int nv = fn->nvalues;
    int any = 0;
    for (k = 0; k < nv; k++) {
      /* Declared by the ANALYSIS's representation, the same source the emitters
         read: a declaration and a use that disagreed on a value's local name was
         the `b79`/`v79` class of bug. */
      switch (e2_rep_prefix((Q2ValueId)k)[0]) {
        case 'i':
          fprintf(f, "    int32_t i%u = 0;\n", (unsigned)k);
          break;
        case 'd':
          fprintf(f, "    double d%u = 0;\n", (unsigned)k);
          break;
        case 'b':
          fprintf(f, "    int b%u = 0;\n", (unsigned)k);
          break;
        default:
          fprintf(f, "    JSValue v%u = JS_UNDEFINED;\n", (unsigned)k);
          break;
      }
      any = 1;
    }
    (void)any;
  }
  fprintf(f, "    (void)fr; (void)ctx; (void)f;\n");

  /* The blocks in reverse postorder. `fn->blocks` is in pc order, which is a
     reverse postorder here: a block starts at every branch target and after
     every terminator, so only a back edge points at a lower index. */
  for (b = 0; b < fn->nrpo; b++) {
    const Q2Block *blk = &fn->blocks[fn->rpo[b]];
    int i2, lowered_to = blk->first_insn, term_lowered = 0;
    Q2ValueId *cur = NULL;
    int r0;

    fprintf(f, "    s_%d: (void)0;\n", blk->start_pc);
    /* The state a block starts with: which value each register holds. The IR
       already knows this exactly -- blk->in_reg/in_val list every live-in
       register and its value, parameter or not -- so `cur` is seeded from it
       rather than by reloading the frame.

       No block needs a frame reload. The spec body is entered at exactly one
       point (aot_f_* sends entry_pc != 0 to the generic body) and is never
       re-entered after an exit, so every spec block is reached through edges the
       spec body itself emitted. In SSA every value a block uses is either one of
       its own parameters or was defined in a block that dominates it, so on every
       path into the block the defining code has already run and set its local.
       The frame is therefore never the source of truth for a block's state, and
       reading it here was what let a stale frame slot overwrite a value an edge
       had just moved.

       The entry block needs nothing special either: its arguments arrive as
       get_arg/get_arg2 instructions inside it, which the prefix lowers (with
       their tag guards), and its other live-ins are undefined at function start,
       which the local initialisers already are. */
    cur = malloc(sizeof(*cur) * (size_t)(fn->nregs > 0 ? fn->nregs : 1));
    if (cur != NULL) {
      for (r0 = 0; r0 < fn->nregs; r0++) cur[r0] = Q2_NO_VALUE;
      for (r0 = 0; r0 < blk->nlive_in; r0++) {
        int rr = blk->in_reg[r0];
        if (rr >= 0 && rr < fn->nregs) cur[rr] = blk->in_val[r0];
      }
    }
    /* 2a-2: the block's lowered prefix, then an exit at the first instruction
       the spec body does not lower. Rule R3: an instruction is either lowered
       here or IS the exit point -- never "emitted as nothing". */
    for (i2 = blk->first_insn; i2 < blk->first_insn + blk->ninsns; i2++) {
      const Q2Insn *in = &fn->insns[i2];
      const E2Role *role = e2_roles[in->rop];
      /* A terminator ends the block; branches are 2b's, so for now the block
         exits at the terminator's pc. */
      if (role != NULL && role->control != E2_CF_NONE) {
        if (emit_typed_return(f, fn, in, &types, cur, have_types)) {
          lowered_to = i2 + 1;
          term_lowered = 1;
          break;
        }
        if (emit_typed_goto(f, fn, in, fn->rpo[b], cur, hex)) {
          lowered_to = i2 + 1;
          term_lowered = 1;
          break;
        }
        if (emit_typed_branch_bool(
                f, fn, in, &types, fn->rpo[b], cur, have_types, hex) ||
            emit_typed_add_imm_branch(
                f, fn, in, &types, fn->rpo[b], cur, have_types, hex) ||
            emit_typed_inc_branch(
                f, fn, in, &types, fn->rpo[b], cur, have_types, hex, fi) ||
            emit_typed_branch(
                f, fn, in, &types, fn->rpo[b], cur, have_types, hex, fi)) {
          lowered_to = i2 + 1;
          term_lowered = 1;
        }
        break;
      }
      /* The spec body's OWN list. `generic_is_trivial` is shared with the generic
         emitter, which has no case for these, so adding one there emits nothing
         and silently drops the instruction. */
      if (generic_is_trivial(in) || in->rop == ROP_get_arg ||
          in->rop == ROP_get_arg2 || in->rop == ROP_move_xfer) {
        if (!trivial_is_lowerable(in, cur)) break;
        emit_trivial_own(f, in, role, cur);
        e2_note_defs(cur, fn, in);
      } else if (emit_typed_arith_cpool(f, fn, in, &types, cur, have_types)) {
        e2_note_defs(cur, fn, in);
      } else if (emit_typed_arith_imm(f, fn, in, &types, cur, have_types)) {
        e2_note_defs(cur, fn, in);
      } else if (emit_typed_unary(
                     f, fn, in, &types, cur, have_types, hex, fi)) {
        e2_note_defs(cur, fn, in);
      } else if (emit_typed_var_ref_store(f, fn, in, &types, cur, have_types)) {
        e2_note_defs(cur, fn, in);
      } else if (emit_typed_var_ref_load(f, fn, in, &types, cur, have_types)) {
        e2_note_defs(cur, fn, in);
      } else if (emit_typed_bitwise(f, fn, in, &types, cur, have_types)) {
        e2_note_defs(cur, fn, in);
      } else if (have_types &&
                 emit_typed_arith(f, fn, in, &types, cur)) {
        e2_note_defs(cur, fn, in);
      } else if (emit_typed_field_read(
                     f, fn, in, &types, cur, have_types, hex, fi, b)) {
        e2_note_defs(cur, fn, in);
      } else if (emit_typed_field_chain(
                     f, fn, in, &types, cur, have_types, hex, fi, b)) {
        e2_note_defs(cur, fn, in);
      } else if (emit_typed_field_write(
                     f, fn, in, &types, cur, have_types, hex, fi, b)) {
        e2_note_defs(cur, fn, in);
      } else {
        break;
      }
      lowered_to = i2 + 1;
    }
    if (term_lowered) {
      free(cur);
      continue;
    }
    if (lowered_to < blk->first_insn + blk->ninsns) {
      /* EXIT AT an instruction's pc: that instruction has not run, so the live
         set is its live-before (live-out minus its defs, plus its uses). */
      uint64_t *lb =
          calloc((size_t)(fn->lwords > 0 ? fn->lwords : 1), 8);
      e2_handoff_note(fn, &fn->insns[lowered_to]);
      fprintf(f, "    /* exit at the first unlowered instruction */\n");
      if (lb != NULL) q2_live_before(fn, lowered_to, lb);
      e2_emit_exit(
          f, fn, fn->insns[lowered_to].pc, hex, fi, cur, lb, 1, b);
      free(lb);
    } else {
      /* The whole non-terminator prefix was lowered. Two sub-cases, and they
         need DIFFERENT live sets:
           - the block ends in a terminator: exit AT the terminator's pc, which
             has not run, so live-before;
           - the block falls through: the last instruction HAS run, so the exit
             is at its next pc and the set is live-OUT. Using live-before there
             drops whatever the last instruction defines, which is exactly the
             `i = 0` in 01-arith's loop entry. */
      int last = blk->first_insn + blk->ninsns - 1;
      uint64_t *set = calloc((size_t)(fn->lwords > 0 ? fn->lwords : 1), 8);
      int is_term =
          blk->ninsns > 0 && e2_roles[fn->insns[last].rop] != NULL &&
          e2_roles[fn->insns[last].rop]->control != E2_CF_NONE;
      if (set != NULL) q2_live_before(fn, last, set);
      if (is_term) {
        e2_handoff_note(fn, &fn->insns[last]);
        fprintf(f, "    /* the terminator is 2b's: exit at its own pc */\n");
        e2_emit_exit(f, fn, fn->insns[last].pc, hex, fi, cur, set, 1, b);
      } else if (blk->nsucc == 1 && blk->succ_block[0] >= 0 &&
                 blk->succ_block[0] < fn->nblocks) {
        /* Fall-through: the block has ONE successor and no terminator, so this is
           a lowered edge like any other. Handing off here instead -- which is what
           this case used to do -- meant a fully lowered block still exited, so no
           function ever completed on the spec path. */
        fprintf(f, "    /* fall through to s_%d */\n",
                fn->blocks[blk->succ_block[0]].start_pc);
        e2_emit_arm(f, fn, b, 0, cur, hex);
      } else {
        const uint64_t *lo = q2_live_out(fn, last);
        fprintf(f, "    /* fall through, no single successor: exit */\n");
        e2_emit_exit(f, fn, blk->end_pc, hex, fi, cur, lo, 1, b);
      }
      free(set);
    }
    free(cur);
  }
  /* The argument-guard exits: a typed argument arrived with the wrong tag, so
     nothing is in a local yet and the frame still holds the caller's values. The
     stub is a plain exit with an empty sync set. */
  {
    int o;
    for (o = 0; o < g_e2_ax_n; o++) {
      int pc2 = g_e2_ax[o], ii, ins = -1;
      for (ii = 0; ii < fn->ninsns; ii++)
        if (fn->insns[ii].pc == pc2) { ins = ii; break; }
      if (ins < 0) continue;
      fprintf(f, "    ax_%d:;\n", pc2);
      e2_emit_exit_at(f, fn, ins, pc2, hex, fi, NULL);
    }
    g_e2_ax_n = 0;
  }
  /* The closure-cell load's deopt exits: the cell holds a value whose tag the
     site does not accept, which is an EXPECTED outcome (profiling cannot promise
     what a cell holds), so the exit syncs and hands over like any other. */
  {
    int o;
    for (o = 0; o < g_e2_vx_n; o++) {
      int pc2 = g_e2_vx[o], ii, ins = -1;
      for (ii = 0; ii < fn->ninsns; ii++)
        if (fn->insns[ii].pc == pc2) { ins = ii; break; }
      if (ins < 0) continue;
      fprintf(f, "    vx_%d:;\n", pc2);
      e2_emit_exit_at(f, fn, ins, pc2, hex, fi, NULL);
    }
    g_e2_vx_n = 0;
  }
  /* The op-call reload guards: the handler returned a JSValue whose tag the site's
     representation does not accept. The op has run, so the exit is at the NEXT pc. */
  {
    int o;
    for (o = 0; o < g_e2_dx_n; o++) {
      int pc2 = g_e2_dx[o], ii, ins = -1;
      for (ii = 0; ii < fn->ninsns; ii++)
        if (fn->insns[ii].pc == pc2) { ins = ii; break; }
      if (ins < 0) continue;
      fprintf(f, "    dx_%d:;\n", pc2);
      e2_emit_exit_at(f, fn, ins, pc2, hex, fi, NULL);
    }
    g_e2_dx_n = 0;
  }
  /* The overflow exits: the arithmetic check jumps here, and the exit does the
     sync and the hand-over. Nothing was written to the frame by the arithmetic
     itself, so the generic body redoes the operation in full precision. */
  {
    int o;
    for (o = 0; o < g_e2_ox_n; o++) {
      int pc2 = g_e2_ox[o], ii, ins = -1;
      for (ii = 0; ii < fn->ninsns; ii++)
        if (fn->insns[ii].pc == pc2) { ins = ii; break; }
      if (ins < 0) continue;
      fprintf(f, "    ox_%d:;\n", pc2);
      e2_emit_exit_at(f, fn, ins, pc2, hex, fi, g_e2_ox_cur[o]);
    }
    for (o = 0; o < g_e2_ox_n; o++) free(g_e2_ox_cur[o]);
    g_e2_ox_n = 0;
  }
  fprintf(f, "    }\n");
  fprintf(f, "}\n\n");
  (void)k;
  return 0;
}
