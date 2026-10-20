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
  int d, recv, site;

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
      "AOT_REG(fr, %d), %d, &_f), 1)) {\n",
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
      "&_v), 1)) {\n",
      site);
  emit_site_hit(f, in);
  /* Borrowed from the object's slot, so the store duplicates it. */
  fprintf(f, "        AOT_SET_B(ctx, fr, %d, _v);\n", dfield);
  fprintf(f, "        goto g_%d;\n", in->next);
  fprintf(f, "    }\n");
  emit_site_miss(f, in);
  fprintf(f, "}\n");
  (void)is_last;
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

  fprintf(f, "g_%d: {\n", in->pc);
  fprintf(f, "    JSValue _c0, _c1;\n");
  fprintf(
      f,
      "    if (__builtin_expect(q2_own_read_hit(pic, holders, "
      "AOT_REG(fr, %d), %d, &_c0), 1)) {\n",
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
      "_c0, %d, &_c1), 1)) {\n",
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

/* An element read: `q2_elem_hit` inlines the array fast path (class, not
   holey, index in bounds, element not a hole or lazy marker). A miss falls into
   the op call, which handles a getter, a string key, a proxy, everything. */
static int emit_native_elem_read(
    FILE *f, const Q2Func *fn, const Q2Insn *in, int is_last) {
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
  fprintf(
      f,
      "    if (__builtin_expect(q2_elem_hit(AOT_REG(fr, %d), "
      "AOT_REG(fr, %d), &_e), 1)) {\n",
      arr, key);
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
    FILE *f, const Q2Func *fn, const Q2Insn *in, int is_last) {
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
  fprintf(
      f,
      "    if (__builtin_expect(q2_elem_put(AOT_REG(fr, %d), "
      "AOT_REG(fr, %d), AOT_REG(fr, %d)), 1)) {\n",
      arr, key, val);
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
   way to fail. Everything else is an op call for now. */
static int is_trivial(const Q2Insn *in) {
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
      fprintf(f, "    /* unhandled trivial op %d */\n", in->rop);
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

  for (i = 0; i < fn->ninsns; i++) {
    const Q2Insn *in = &fn->insns[i];
    const E2Role *role = e2_roles[in->rop];

    if (role == NULL) {
      fprintf(stderr, "e2: no role for op %d\n", in->rop);
      return -1;
    }
    if (is_trivial(in)) {
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
      continue;
    }
    if (emit_native_elem_read(f, fn, in, i + 1 == fn->ninsns) ||
        emit_native_elem_write(f, fn, in, i + 1 == fn->ninsns) ||
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

/* The speculative body (D3) until E8: forward to the generic body. One
   function, one body, and no second copy of the code to keep in step while the
   generic body is the thing being built. */
int e2_emit_spec_body(FILE *f, const Q2Func *fn, char hex[33]) {
  (void)fn;
  fprintf(
      f, "E2_TEXT %sJSValue aot_f_%s(JSContext *ctx, JSRegRun *f,\n",
      e2_linkage(), hex);
  fprintf(
      f, "                                 JSValue *fr, uint32_t entry_pc)\n");
  fprintf(f, "{\n");
  fprintf(
      f,
      "    /* The speculative body arrives in E8; until then every entry is\n");
  fprintf(
      f,
      "       the generic body's, which has a label for every instruction. "
      "*/\n");
  fprintf(f, "    return aot_g_%s(ctx, f, fr, entry_pc);\n", hex);
  fprintf(f, "}\n\n");
  return 0;
}
