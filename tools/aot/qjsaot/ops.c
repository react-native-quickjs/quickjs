/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * The generic body: frame-resident, every instruction pc an
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
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emit.h"
#include "ir.h"
#include "analysis.h"

/* Inline mode: the frameless body of a small leaf, emitted as
   `static inline int ei_HEX(...)` for a compiled caller to run in place. It is the
   frameless emission with three changes: every exit is `return 0` (nothing has
   happened, so the caller's native call is correct), the return stores the result
   through `*out`, and no exit may follow a store on any path (checked on the
   text). Implies g_e_fl. */
static int g_e_fi = -1;
static int g_e_inl;
/* An inline body that makes calls roots what it holds in an array of its own, published for the
   length of each call, like a frameless body that calls. */
static int g_e_inl_r;
/* Set when the inline body being emitted makes a call or a construct (a safepoint). */
static int g_e_inl_safepoint;
static int g_e_last_inl_leaf;
static int g_e_inl_pending;
static const EFacts *g_e_facts;
/* Set when a body emitted in inline mode needs more than inline mode can give. */
static char g_e_inl_why[160];
/* Inline mode, effects so far. Once the body has stored or called, an exit can no
   longer be `return 0` (the caller's native call would repeat the effect); it builds
   the callee's frame (aot_nx_) and finishes in the generic body instead. The state is
   captured per instruction before it is emitted, and each exit stub reads the state
   of the instruction it belongs to. */
static int g_e_inl_dirty;
static uint8_t *g_e_dirty_ins;
static int g_e_exit_ins = -1;
static int g_e_exit_dirty;
static int g_e_inl_nx_used;
static int g_e_last_inl_nx;
static const char *nm_of_rop(uint16_t rop);
static void e_arr_region_start(FILE *f, Q2ValueId av, Q2ValueId root, const char *fail);
static void e_arr_region_start_grp(FILE *f, Q2ValueId av, Q2ValueId root, const char *fail, int grp);
static void e_cell_hoist_check(FILE *f, int site, int group, const char *kind, int pc);
static int e_pure_call_site(const Q2Func *fn, const Q2Insn *in);
static const char *e_slotfn(int site);
static const char *e_cellidx(char *buf, size_t n, int site);
/* The next index into the unit's table of held callees (see q2_call_entry_c). */
int g_cc_count;
/* A call site the profile saw with one callee tests the object it first accepted (see
   q2_call_entry_c); a site with several keeps the full test, which a cached object would
   only make slower. */
static int e_cc_mono(const Q2Func *fn, const Q2Insn *in) {
  int n = 0, kind[4];
  char hex[4][33];
  unsigned long long hits[4];
  return qjs_aot_prof_callees(fn->g->digest, in->next, &n, hex, kind, hits) && n == 1;
}
static const char *e_cc_fn(const Q2Func *fn, const Q2Insn *in) {
  return e_cc_mono(fn, in) ? "q2_call_entry_c(ctx, " : "q2_call_entry_u(ctx, ";
}
static int e_cc_ix(const Q2Func *fn, const Q2Insn *in) { return e_cc_mono(fn, in) ? g_cc_count++ : 0; }
static void e_slot_off(char *out, size_t n, int g, int site);
static int e_group_layout(int g);
static void e_group_test(
    char *out, size_t n, int skip, int cellskip, const char *recv, int g, int site, int write);
static int emit_hit_result(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId dv, int dreg,
    const char *raw);
/* The stores of copied callees. A copy that has stored and then has to leave cannot be replayed from the
   call, so each store keeps the slot it wrote, the value it replaced and the value it put there, in locals
   of the body (us/uo/un<n>): the replaced value stays alive until the copy ends, and a way out puts every
   slot back first, most recent store first. `g_e_undo_id` numbers the stores by instruction; the order the
   emitter reached them in is the order they run in, since a copy has no loops. */
static int *g_e_undo_id;
static int g_e_undo_total;
static int g_e_undo_seq[256], g_e_undo_seq_own[256], g_e_undo_seq_n;
static void e_undo_prepare(const Q2Func *fn) {
  int i;
  free(g_e_undo_id);
  g_e_undo_id = calloc((size_t)(fn->ninsns > 0 ? fn->ninsns : 1), sizeof(int));
  g_e_undo_total = 0;
  g_e_undo_seq_n = 0;
  for (i = 0; i < fn->ninsns; i++) {
    g_e_undo_id[i] = -1;
    if (fn->insns[i].own > 0 && fn->insns[i].rop == ROP_put_field) g_e_undo_id[i] = g_e_undo_total++;
  }
}
/* the copy's instance ends: its stores are final, and the values they replaced are released */
static void e_undo_release(FILE *f, int own) {
  int q;
  for (q = 0; q < g_e_undo_seq_n; q++) {
    int id = g_e_undo_seq[q];
    if (g_e_undo_seq_own[q] != own) continue;
    fprintf(
        f,
        "    if (us%d != NULL) { if (JS_VALUE_HAS_REF_COUNT(uo%d)) JS_FreeValue(ctx, uo%d); us%d = NULL; }\n", id, id,
        id, id);
  }
}
/* a way out of the copy before its end: every store it made is put back */
static void e_undo_restore(FILE *f, int own) {
  int q;
  for (q = g_e_undo_seq_n - 1; q >= 0; q--) {
    int id = g_e_undo_seq[q];
    if (g_e_undo_seq_own[q] != own) continue;
    fprintf(
        f,
        "    if (us%d != NULL) { *us%d = uo%d; if (JS_VALUE_HAS_REF_COUNT(un%d)) JS_FreeValue(ctx, un%d); us%d = NULL; }\n",
        id, id, id, id, id, id);
  }
}

static void e_inl_store_mark(FILE *f) {
  if (!g_e_inl) return;
  g_e_inl_dirty = 1;
  fprintf(f, "    /*E_STORE*/\n");
}


/* The number of PIC sites the function uses: one more than the largest site
   index. The per-site stride in the PIC table is this count, because site `k`'s
   way `w` lives at `w * count + k`, so the read helper needs it to step between
   ways. */
int e_pic_sites(const Q2Func *fn) {
  /* Unused: the stride is read from the running function's bytecode at runtime
     (q2_pic_sites in the prelude), so a unit cannot disagree with the bytecode
     it was compiled from. Kept only so the call sites keep their shape. */
  (void)fn;
  return 0;
}

/* The opcode's name, for the poison abort message. The role table already
   carries the engine's format; the name comes from the same generated source so
   a reader sees the op the way the engine does. */
void e_rop_name(uint16_t rop, char *out, size_t n);

/* The profile's execution count at one pc (qjsaot.c): the op-call weight. */
unsigned long long qjs_aot_prof_pc_count(const uint8_t dg[16], int pc);

/* A numeric constant-pool entry, exactly as the bytecode holds it. Lets
   `binary_cpool` fold `x * 0.999` to a comparison and a multiply against a C
   double literal instead of op-calling the engine. */
int qjs_aot_pool_num(const struct JSAotGenFunc *g, uint32_t idx, double *out);

/* The entry's tag when it is a foldable number: E_POOL_INT or
   E_POOL_DOUBLE, else 0. (The generator does not include the engine's headers,
   so the two values it can see are named here.) */
int qjs_aot_pool_tag(const struct JSAotGenFunc *g, uint32_t idx);
#define E_POOL_INT 1    /* QJS_AOT_POOL_INT */
#define E_POOL_DOUBLE 2 /* QJS_AOT_POOL_DOUBLE */

/* Report one instruction the body leaves as an op call, weighted by the
   profile's count there. A pc that also has an inline fast path reports its
   weight as `missarm`: only its misses reach the op call, so counting it in the
   op's own row would make a lowered family look unlowered. */
static void note_opcall(const Q2Func *fn, const Q2Insn *in, int missarm) {
  unsigned long long w = qjs_aot_prof_pc_count(e_dg(fn, in), in->opc);
  if (w) e_report_opcall(in->rop, (double)w, missarm);
}

/* The driver's profile and unit queries (qjsaot.c). A call site asks the
   profile which callees it saw and the unit whether each is compiled here and
   can be entered on the C stack. */
int qjs_aot_prof_callees(
    const uint8_t dg[16], int pc, int *n, char hex[4][33], int kind[4],
    unsigned long long hits[4]);
int qjs_aot_inline_has(const char hex[33]);
int qjs_aot_inline_leaf(const char hex[33]);
int qjs_aot_inline_pending(const char hex[33]);
int qjs_aot_unit_has(const char hex[33]);
int qjs_aot_light_need(const char hex[33]);
int e_inl_construct_allowed(const Q2Func *fn, const Q2Insn *ci);
int qjs_aot_native_capable(const char hex[33]);
int qjs_aot_unit_argc(const char hex[33]);
int qjs_aot_sweep_on(void);
int qjs_aot_site_alloc(void);

/* Whether register `r` may be written by this instruction: its declared
   destinations, plus `writes_regs` ops. */
static int role_writes_reg(
    const Q2Func *fn, const ERole *role, const Q2Insn *in, int r) {
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
  /* A window a callee takes as its argument slots is the callee's to write: a
     parameter assigned in the callee is a store into the caller's register. The
     window ops that stage values (append, the class definitions) write theirs too. */
  if (in->win_n > 0 && r >= in->win_reg && r < in->win_reg + in->win_n) return 1;
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
static int role_reads_reg(const ERole *role, const Q2Insn *in, int r) {
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

/* Whether the unit being generated will be compiled with -DE_POISON. The
   emitter sets it from the command line (`--poison`). */
static int g_poison;
int e_poison_on(void) {
  return g_poison;
}
void e_set_poison(int on) {
  g_poison = on;
}

/* Whether an op call gets the check at all. A fallback reads and writes
   whatever its handler needs, so there is nothing to declare or compare. A
   return or a suspend leaves the activation inside the handler, so the frame
   cannot be compared or restored afterwards. */
static int poison_applies(const Q2Insn *in, const ERole *role) {
  if (in->op == Q2_OP_FALLBACK) return 0;
  if (role->control == E_CF_RETURN || role->control == E_CF_SUSPEND) return 0;
  return 1;
}

/* The dynamic register check around one op call. The work is done by two helpers in
   the unit prelude (`e_poison_pre`, `e_poison_post`); each call site passes
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
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ERole *role) {
  int r, insn = (int)(in - fn->insns);

  fprintf(f, "    static const char e_pm_%d[] = \"", in->pc);
  for (r = 0; r < fn->nregs; r++)
    fputc(
        !role_writes_reg(fn, role, in, r) && !role_reads_reg(role, in, r) &&
                !reg_live_after(fn, insn, r)
            ? '1'
            : '0',
        f);
  fprintf(f, "\";\n");
  fprintf(f, "    static const char e_wm_%d[] = \"", in->pc);
  for (r = 0; r < fn->nregs; r++)
    fputc(role_writes_reg(fn, role, in, r) ? '1' : '0', f);
  fprintf(f, "\";\n");
}

static void emit_opcall(
    FILE *f, const Q2Func *fn, const Q2Insn *in, int is_last, int with_label) {
  const ERole *role = e_roles[in->rop];
  char opname[64];

  int poison = g_poison && role && poison_applies(in, role);

  e_rop_name(in->rop, opname, sizeof(opname));
  /* The masks are declarations, so they go before the label; a caller that
     emitted a native fast path has already placed them. */
  if (poison && with_label) emit_poison_masks(f, fn, in, role);
  if (with_label) fprintf(f, "g_%d:\n", in->pc);
  if (poison) {
    fprintf(
        f, "    e_poison_pre(fr, %d, e_poison_copy, e_pz, e_pm_%d);\n",
        fn->nregs, in->pc);
  }
  fprintf(f, "    ip = qjs_aot_step(code + %d, fr, f, ctx);\n", in->pc);
  if (poison)
    fprintf(
        f,
        "    e_poison_post(fr, %d, e_poison_copy, e_pz, e_wm_%d, "
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
  fprintf(f, "        e_from_pc = %d;\n", in->pc);
  fprintf(f, "        entry_pc = (uint32_t)(ip - code);\n");
  fprintf(f, "        goto dispatch;\n");
  fprintf(f, "    }\n");
  if (!is_last) fprintf(f, "    /* fall through to g_%d */\n", in->next);
}

/* A register operand's `val` is the byte offset into the register file, not an
   index: the IR's own accessor is `val >> 4`. It is used as the offset
   directly, never multiplied again. */
#define E_ROFF(arg) ((int)(arg).val)

/* --- native lowerings ----------------------------------------------
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
  return E_ROFF(in->arg[idx]);
}

/* The function the generic body being emitted belongs to, for the site
   counters' names. A site is (function, pc, op), because a pc is only unique
   inside one function. */
static const char *g_cur_hex = "";

/* The tail every native lowering shares: the site counters (when the unit is
   built with -DE_COUNT), then the fall-through comment. A lowering's fast path
   counts a hit before its `goto`; the op call below the block is the miss, so
   one `E_CTR(..., 0)` at the top of that block records it. `hex` names the
   function the site belongs to. */
static void emit_site_hit(FILE *f, const Q2Insn *in) {
  fprintf(
      f, "        E_CTR(\"%s\", %d, %d, 1);\n", g_cur_hex, in->pc, in->rop);
}
static void emit_site_miss(FILE *f, const Q2Insn *in) {
  fprintf(f, "    E_CTR(\"%s\", %d, %d, 0);\n", g_cur_hex, in->pc, in->rop);
}

/* The arithmetic and compare ops, lowered natively: the operands come from the
   frame, `q2_arith` runs the engine's own number fast path, and the result is
   stored. A miss (a string, an object, a mixed tag the helper declines) falls
   into the op call below, which runs the interpreter's handler.

   `sub` is the engine's REG_BIN_* value, carried by the IR. */
static int emit_native_binary(
    FILE *f, const Q2Func *fn, const Q2Insn *in, int is_last) {
  const ERole *role = e_roles[in->rop];
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
  if (role == NULL || role->control != E_CF_NONE) return 0;
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
  const ERole *role = e_roles[in->rop];
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
      if (!qjs_aot_pool_num(q2_insn_g(fn, in), idx, &c)) return 0;
      tag = qjs_aot_pool_tag(q2_insn_g(fn, in), idx);
      if (sub != REG_BIN_ADD && sub != REG_BIN_SUB && sub != REG_BIN_MUL &&
          sub != REG_BIN_DIV)
        return 0;
      if (role == NULL || role->control != E_CF_NONE) return 0;
      if (in->ndef != 1 || a < 0) return 0;
      d = dest_off(fn, in);
      if (d < 0) return 0;
      /* An integer entry must stay an integer: `i + 100000` with a double
         literal would miss both inline paths, produce a float where the
         interpreter produces an int, and make later `a[i + 100000]` miss its
         integer element path. */
      if (tag == E_POOL_INT) {
        snprintf(lit, sizeof(lit), "JS_MKVAL(JS_TAG_INT, %d)", (int32_t)c);
      } else if (tag == E_POOL_DOUBLE) {
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
  if (role == NULL || role->control != E_CF_NONE) return 0;
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
  const ERole *role = e_roles[in->rop];
  int d, recv, site, nsites = e_pic_sites(fn);

  if (in->op != Q2_OP_GET_FIELD) return 0;
  if (role == NULL || role->control != E_CF_NONE) return 0;
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
  const ERole *role = e_roles[in->rop];
  int d, site;
  int atom_off, throwing;

  if (in->rop != ROP_get_global) return 0;
  if (role == NULL || role->control != E_CF_NONE) return 0;
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
  const ERole *role = e_roles[in->rop];
  int dfield, drecv, site;

  if (in->rop != ROP_get_this_field && in->rop != ROP_get_this_field2) return 0;
  if (role == NULL || role->control != E_CF_NONE) return 0;
  if (in->narg < 4 || in->arg[3].kind != 's') return 0;
  site = (int)in->arg[3].val;
  if ((unsigned)site == REG_IC_NONE) return 0;
  /* Both register operands are destinations (`def[0]` is the read, `def[1]` the
     receiver), so the role table has no use to take an offset from. */
  if (in->ndef < 2 || in->arg[0].kind != 'r' || in->arg[1].kind != 'r')
    return 0;
  dfield = E_ROFF(in->arg[0]);
  drecv = E_ROFF(in->arg[1]);
  {
    int nsites = e_pic_sites(fn);

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
  const ERole *role = e_roles[in->rop];
  int d0, d1, recv, s1, s2;

  if (in->rop != ROP_get_field_chain && in->rop != ROP_get_field_chain_own)
    return 0;
  if (role == NULL || role->control != E_CF_NONE) return 0;
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
    int nsites = e_pic_sites(fn);

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
#define E_FUNC_NORMAL 0

/* A property write: `q2_put_field_hit` stores in place when the PIC entry says
   the slot is an own, writable, non-accessor property of a matching shape. Any
   other case falls into the op call.

   `put_field` is "rasr": receiver, atom, site, value. The op has no
   destination, so its fast path only has to continue. */
static int emit_native_field_write(
    FILE *f, const Q2Func *fn, const Q2Insn *in, int is_last) {
  const ERole *role = e_roles[in->rop];
  int recv, val, site;

  if (in->rop != ROP_put_field) return 0;
  if (role == NULL || role->control != E_CF_NONE) return 0;
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

/* Whether the profile saw a store at this pc that the inline store would not take: it grew
   the array or filled a hole. */
static int elem_pc_store_misses(const Q2Func *fn, const Q2Insn *in) {
  int i;
  if (fn->prof == NULL) return 0;
  for (i = 0; i < fn->nprof; i++)
    if (fn->prof[i].pc == in->pc) return (fn->prof[i].elem & Q2_ELEM_HOLEY) != 0;
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
  const ERole *role = e_roles[in->rop];
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
  if (in->narg >= 3 && in->arg[0].kind == 'r') return E_ROFF(in->arg[0]);
  return -1;
}

/* Whether `in` could change what a region holds: it runs JavaScript, it writes
   the array's register, or it may grow the array. */
static int elem_breaks_run(const Q2Func *fn, const Q2Insn *in, int arr) {
  const ERole *role = e_roles[in->rop];
  int k;
  if (role == NULL) return 1;
  if (in->op == Q2_OP_FALLBACK || role->runs_js || in->op == Q2_OP_CALL)
    return 1;
  if (role->control != E_CF_NONE) return 1;
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
  const ERole *role = e_roles[in->rop];
  int d, arr, key;

  if (in->rop != ROP_get_element) return 0;
  /* Only where the profile saw numeric elements: a holey array misses the
     inline test on every read, so lowering it there is pure cost. */
  if (!elem_pc_is_numeric(fn, in)) return 0;
  if (role == NULL || role->control != E_CF_NONE || role->nuse != 2) return 0;
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
  const ERole *role = e_roles[in->rop];
  int arr, key, val;

  /* `put_element` and its xfer form are the same "rrr": object, key, value. The
     xfer form only differs under an owning build, where the source register is
     emptied; under DRC both borrow and the container takes its own reference, so
     the inline store is correct for either. */
  if (in->rop != ROP_put_element && in->rop != ROP_put_element_xfer) return 0;
  if (role == NULL || role->control != E_CF_NONE) return 0;
  /* The handler reads all three ("rrr" is object, key, value) and writes
     none. */
  if (in->narg >= 3 && in->arg[0].kind == 'r' && in->arg[1].kind == 'r' &&
      in->arg[2].kind == 'r') {
    arr = E_ROFF(in->arg[0]);
    key = E_ROFF(in->arg[1]);
    val = E_ROFF(in->arg[2]);
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
  const ERole *role = e_roles[in->rop];
  int a, tgt, neg;

  if (in->rop != ROP_branch_if && in->rop != ROP_branch_if_not) return 0;
  if (role == NULL || role->control != E_CF_BRANCH) return 0;
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
  const ERole *role = e_roles[in->rop];
  int a, tgt;

  if (role == NULL || role->control != E_CF_BRANCH) return 0;
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
  if (a < 0 && in->narg > 0 && in->arg[0].kind == 'r') a = E_ROFF(in->arg[0]);
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
  const ERole *role = e_roles[in->rop];
  int d, a, sub;

  if (role == NULL || role->control != E_CF_NONE) return 0;
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
    d = E_ROFF(in->arg[0]);
    a = E_ROFF(in->arg[1]);
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
  const ERole *role = e_roles[in->rop];
  int d0, d1, src, sign;

  if (in->rop != ROP_post_inc && in->rop != ROP_post_dec) return 0;
  if (role == NULL || role->control != E_CF_NONE) return 0;
  /* The operand order comes from the instruction, not the role table: "rrr" is
     typed, source, numeric, which is the order the handler reads
     (`REGVM_POST_OP`). The role table now agrees (`def = {0,2}`, `use = {1}`),
     but reading the op's own operands is what keeps this correct for any op the
     table has not been taught. */
  if (in->narg < 3 || in->arg[0].kind != 'r' || in->arg[1].kind != 'r' ||
      in->arg[2].kind != 'r')
    return 0;
  d0 = E_ROFF(in->arg[0]);
  src = E_ROFF(in->arg[1]);
  d1 = E_ROFF(in->arg[2]);
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

/* `return` of the value in frame slot `off`: qjs_aot_return's body inline. The
   register borrows under DRC, so the caller gets its own reference (q2_dup, the
   inline count), and the slot is cleared. */
static void e_emit_return(FILE *f, int off) {
  if (g_e_inl) {
    /* An inlined body hands its value back borrowed (status 2): its frame is part of the
       caller's, so there is no teardown for the reference to survive, and the caller stores
       the value without adopting. */
    fprintf(f, "    { JSValue _rv = AOT_REG(fr, %d);\n      *out = _rv; return 2; }\n", off);
    return;
  }
  fprintf(
      f,
      "    { JSValue _rv = AOT_REG(fr, %d);\n"
      "#if QJS_DRC\n"
      "      q2_dup(_rv);\n"
      "#endif\n"
      "      AOT_REG(fr, %d) = JS_UNDEFINED;\n"
      "      return _rv; }\n",
      off, off);
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
  const ERole *role = e_roles[in->rop];
  int a;

  if (in->rop == ROP_return_undefined) {
    if (role == NULL || role->control != E_CF_RETURN) return 0;
    if (fn->g->func_kind != E_FUNC_NORMAL) return 0;
    fprintf(f, "g_%d:\n", in->pc);
    fprintf(f, "    return JS_UNDEFINED;\n");
    (void)a;
    (void)is_last;
    return 1;
  }
  if (in->rop != ROP_return) return 0;
  if (role == NULL || role->control != E_CF_RETURN) return 0;
  /* The generator check, and the same for the returned value: an op call would
     have taken the suspend path, which this direct return cannot. */
  if (fn->g->func_kind != E_FUNC_NORMAL) return 0;
  a = operand_off(in, role->use[0]);
  if (a < 0) return 0;
  fprintf(f, "g_%d:\n", in->pc);
  e_emit_return(f, a);
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

   `dm` is stored with the result, after both halves have hit: a miss re-runs the whole
   instruction from the frame, and `dm` may share a register with `x`, `y` or `c`. */
static int emit_native_fused(
    FILE *f, const Q2Func *fn, const Q2Insn *in, int is_last) {
  const ERole *role = e_roles[in->rop];
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
  if (role == NULL || role->control != E_CF_NONE) return 0;
  if (in->narg < 5 || in->arg[0].kind != 'r' || in->arg[1].kind != 'r' ||
      in->arg[2].kind != 'r' || in->arg[3].kind != 'r' ||
      in->arg[4].kind != 'r')
    return 0;
  dm = E_ROFF(in->arg[0]);
  x = E_ROFF(in->arg[1]);
  y = E_ROFF(in->arg[2]);
  da = E_ROFF(in->arg[3]);
  c = E_ROFF(in->arg[4]);

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
  /* The product stays in a local until both halves have hit: a miss re-runs the whole
     instruction from the frame, and the product's register may be one of its operands. */
  if (c_first)
    fprintf(
        f,
        "        if (__builtin_expect(q2_arith(%s, AOT_REG(fr, %d), "
        "_t, &_r), 1)) {\n",
        second == 1 ? "QJS_AOT_REG_BIN_ADD" : "QJS_AOT_REG_BIN_SUB", c);
  else
    fprintf(
        f,
        "        if (__builtin_expect(q2_arith(%s, _t, "
        "AOT_REG(fr, %d), &_r), 1)) {\n",
        second == 1 ? "QJS_AOT_REG_BIN_ADD" : "QJS_AOT_REG_BIN_SUB", c);
  emit_site_hit(f, in);
  fprintf(f, "            AOT_SET_B(ctx, fr, %d, _t);\n", dm);
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
  const ERole *role = e_roles[in->rop];
  int a, b, sub, tgt, inv;
  unsigned base;

  if (in->rop != ROP_branch_cmp_reg_reg && in->rop != ROP_branch_cmp_reg_reg_s)
    return 0;
  if (role == NULL || role->control != E_CF_BRANCH) return 0;
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
  a = E_ROFF(in->arg[1]);
  b = E_ROFF(in->arg[2]);
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
  const ERole *role = e_roles[in->rop];
  int d, b, tgt;

  if (in->rop != ROP_inc_branch_lt_s) return 0;
  if (role == NULL || role->control != E_CF_BRANCH) return 0;
  if (in->narg < 3 || in->arg[0].kind != 'r' || in->arg[1].kind != 'r')
    return 0;
  d = E_ROFF(in->arg[0]);
  b = E_ROFF(in->arg[1]);
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
  const ERole *role = e_roles[in->rop];
  int d, s;

  if (in->rop == ROP_load_this) {
    if (role == NULL || role->control != E_CF_NONE) return 0;
    if (in->narg < 1 || in->arg[0].kind != 'r') return 0;
    d = E_ROFF(in->arg[0]);
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
    if (role == NULL || role->control != E_CF_NONE) return 0;
    if (in->narg < 2 || in->arg[0].kind != 'r' || in->arg[1].kind != 'r')
      return 0;
    d = E_ROFF(in->arg[0]);
    s = E_ROFF(in->arg[1]);
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
  const ERole *role = e_roles[in->rop];
  int d0, d1, s0, s1;

  if (in->rop == ROP_get_arg) {
    if (role == NULL || role->control != E_CF_NONE) return 0;
    if (in->narg < 2 || in->arg[0].kind != 'r' || in->arg[1].kind != 'u')
      return 0;
    d0 = E_ROFF(in->arg[0]);
    s0 = (int)in->arg[1].val;
    fprintf(f, "g_%d:\n", in->pc);
    fprintf(f, "    AOT_SET_B(ctx, fr, %d, q2_arg_buf(f)[%d]);\n", d0, s0);
    (void)is_last;
    return 1;
  }
  if (in->rop == ROP_get_arg2) {
    if (role == NULL || role->control != E_CF_NONE) return 0;
    if (in->narg < 4 || in->arg[0].kind != 'r' || in->arg[1].kind != 'u' ||
        in->arg[2].kind != 'r' || in->arg[3].kind != 'u')
      return 0;
    d0 = E_ROFF(in->arg[0]);
    s0 = (int)in->arg[1].val;
    d1 = E_ROFF(in->arg[2]);
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
    if (role == NULL || role->control != E_CF_NONE) return 0;
    if (in->narg < 2 || in->arg[0].kind != 'r' || in->arg[1].kind != 'u')
      return 0;
    d0 = E_ROFF(in->arg[0]);
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
  const ERole *role = e_roles[in->rop];
  int d, recv;

  if (in->rop != ROP_get_length) return 0;
  if (role == NULL || role->control != E_CF_NONE) return 0;
  d = dest_off(fn, in);
  recv = operand_off(in, role->use[0]);
  if (d < 0 || recv < 0) return 0;

  fprintf(f, "g_%d: {\n", in->pc);
  fprintf(f, "    double _len;\n");
  fprintf(
      f,
      "    if (__builtin_expect(q2_array_length(ctx, AOT_REG(fr, %d), "
      "&_len, NULL, NULL), 1)) {\n",
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
static void emit_trivial(FILE *f, const Q2Insn *in, const ERole *role) {
  fprintf(f, "g_%d:\n", in->pc);
  switch (in->rop) {
    case ROP_move:
    case ROP_move_s: {
      /* move: dst = src. Both are registers, so the copy is one frame store. */
      int d = E_ROFF(in->arg[0]), s = E_ROFF(in->arg[1]);
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, AOT_REG(fr, %d));\n", d, s);
      break;
    }
    case ROP_load_i8:
    case ROP_load_i8_s:
    case ROP_load_i32: {
      int d = E_ROFF(in->arg[0]);
      int32_t v = (int32_t)in->arg[1].val;
      fprintf(
          f, "    AOT_SET_B(ctx, fr, %d, JS_MKVAL(JS_TAG_INT, %d));\n", d, v);
      break;
    }
    case ROP_load_undefined:
    case ROP_load_null:
    case ROP_load_true:
    case ROP_load_false: {
      int d = E_ROFF(in->arg[0]);
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
          "qjsaot: GENERIC emitter: generic_is_trivial accepted rop %d but emit_trivial has "
          "no case for it (pc %d)\n",
          in->rop, in->pc);
      abort();
      break;
  }
  (void)role;
}

/* --- native call sites -----------------------------------------------
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
   disabled. Bodies do not use that switch, so the callee is always a
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
  /* Guarded by `Q2_NATIVE_`: a call site names the callee's native entry only when
     the unit defines one; the macro is set for every function that has one. */
  fprintf(f, "#ifdef Q2_NATIVE_%s\n", hex);
  fprintf(
      f,
      "    if (__builtin_expect(%sAOT_REG(fr, %d), "
      "(const void *)aot_f_%s, %du), 1)) {\n",
      e_cc_fn(fn, in), cc, hex, e_cc_ix(fn, in));
  /* The callee's realm is the context its body runs in, which the entry takes
     from the bytecode; `ctx` here is only the caller's. */
  fprintf(
      f,
      "        E_CAP(f, %d);\n"
      "        JSValue _o = aot_n_%s(ctx, f,\n"
      "            (JSObject *)JS_VALUE_GET_PTR(AOT_REG(fr, %d)), %s,\n"
      "            &AOT_REG(fr, %d), %d);\n",
      in->next - 1, hex, cc, recv, w, na);
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

  if (e_nocall_on()) return 0;
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
    if (!qjs_aot_native_capable(hex[k])) continue;
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
    const ERole *role = e_roles[in->rop];
    if (role && poison_applies(in, role)) emit_poison_masks(f, fn, in, role);
  }
  fprintf(f, "g_%d:\n", in->pc);
  fprintf(f, "    { /* direct call site: %d known callee(s) */\n", nb);
  for (k = 0; k < n && k < 4; k++) {
    if (kind[k] != 1) continue;
    if (!qjs_aot_unit_has(hex[k])) continue;
    if (!qjs_aot_native_capable(hex[k])) continue;
    if (in->win_n < qjs_aot_unit_argc(hex[k])) continue;
    emit_one_direct_call(f, fn, in, hex[k], in->next);
  }
  fprintf(f, "    }\n");
  /* The label and the op call that a miss reaches are the caller's, so the
     block here is unlabelled and self-contained. */
  (void)is_last;
  return 1;
}

/* The loop headers of a function: the blocks a back edge enters, with their start pcs.
   Both bodies use the same list, so a loop the speculative body left can be re-entered
   at exactly the headers the generic body offers. */
static int e_loop_headers(const Q2Func *fn, int *blks, int *pcs, int max) {
  int b, k, n = 0, j;
  for (b = 0; b < fn->nblocks; b++) {
    const Q2Block *blk = &fn->blocks[b];
    for (k = 0; k < blk->nsucc && k < 2; k++) {
      int sb = blk->succ_block[k], dup = 0;
      if (sb < 0 || sb >= fn->nblocks || !q2_is_back_edge(fn, b, sb)) continue;
      for (j = 0; j < n; j++)
        if (blks[j] == sb) dup = 1;
      if (dup || n >= max) continue;
      blks[n] = sb;
      pcs[n++] = fn->blocks[sb].start_pc;
    }
  }
  return n;
}

static int e_emit_generic_body_raw(FILE *f, const Q2Func *fn, char hex[33]);
static int e_nosr_on(void);
/* The generic body, with the loop re-entry hook after each loop header's label: a call
   that left the speculative body inside a loop returns to it at the header. */
int e_emit_generic_body(FILE *f, const Q2Func *fn, char hex[33]) {
  char *mb = NULL;
  size_t ml = 0;
  FILE *m;
  int blks[16], pcs[16], nh, h, rc, fi = qjs_aot_cur_func_idx();
  char *txt, *outp = NULL;
  size_t outn = 0;
  FILE *o2;

  m = open_memstream(&mb, &ml);
  if (m == NULL) return e_emit_generic_body_raw(f, fn, hex);
  rc = e_emit_generic_body_raw(m, fn, hex);
  fclose(m);
  if (rc != 0 || fn->g->ctl_count > 0 || e_nosr_on()) {
    fwrite(mb, 1, ml, f);
    free(mb);
    return rc;
  }
  nh = e_loop_headers(fn, blks, pcs, 16);
  txt = mb;
  o2 = open_memstream(&outp, &outn);
  if (o2 == NULL) {
    fwrite(mb, 1, ml, f);
    free(mb);
    return rc;
  }
  {
    /* Insert the hook after the first "g_<pc>:" of each header. */
    char *cursor = txt;
    size_t len = ml;
    size_t pos = 0;
    while (pos < len) {
      int matched = -1;
      if ((pos == 0 || txt[pos - 1] == '\n') && txt[pos] == 'g' && pos + 2 < len && txt[pos + 1] == '_') {
        char *endp;
        long v = strtol(txt + pos + 2, &endp, 10);
        if (endp != txt + pos + 2 && *endp == ':') {
          for (h = 0; h < nh; h++)
            if (pcs[h] == v) matched = h;
          if (matched >= 0) {
            fwrite(txt + pos, 1, (size_t)(endp - (txt + pos)) + 1, o2);
            fprintf(o2, "\n    E_OSR_HOOK(%d, %d, aot_f_%s);", fi, pcs[matched], hex);
            pos = (size_t)(endp - txt) + 1;
            pcs[matched] = -1;
            continue;
          }
        }
      }
      fputc(txt[pos], o2);
      pos++;
    }
    (void)cursor;
  }
  fclose(o2);
  fwrite(outp, 1, outn, f);
  free(outp);
  free(mb);
  return rc;
}

static int e_emit_generic_body_raw(FILE *f, const Q2Func *fn, char hex[33]) {
  int i, has_ctl;
  g_cur_hex = hex;
  if (!fn->ok) return -1;
  has_ctl = fn->g->ctl_count > 0;

  /* Set by a speculative body just before it calls this one, so a loop hook below can hand
     back to it by returning instead of by a nested call. */
  fprintf(f, "static uint8_t e_gfs_%s;\n", hex);
  fprintf(
      f, "E_TEXT %sJSValue aot_g_%s(JSContext *ctx, JSRegRun *f,\n", e_linkage(), hex);
  fprintf(
      f, "                                 JSValue *fr, uint32_t entry_pc)\n");
  fprintf(f, "{\n");
  fprintf(f, "    uint8_t *code = q2_code(f);\n");
  fprintf(f, "    uint8_t *ip;\n");
  fprintf(f, "    uint32_t pc = 0;\n");
  fprintf(f, "    uint32_t e_from_pc = entry_pc;\n");
  fprintf(f, "    uint32_t osr_from = entry_pc; (void)osr_from;\n");
  fprintf(f, "    const int from_spec_ = e_gfs_%s; e_gfs_%s = 0; (void)from_spec_;\n", hex, hex);
  fprintf(f, "    const void *pic = q2_pic(f);\n    const uint8_t *ecb = q2_code(f); (void)ecb;\n");
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
    fprintf(f, "    JSValue e_poison_copy[%d];\n", fn->nregs ? fn->nregs : 1);
    fprintf(f, "    uint8_t e_pz[%d];\n", fn->nregs ? fn->nregs : 1);
    fprintf(f, "    (void)e_poison_copy; (void)e_pz;\n");
  }
  fprintf(f, "    E_SEAL(ctx, f);\n");
  /* The first dispatch is an entry, not a back edge, so it does not poll: a
     loop polls on its back edge, a native entry with a call site polls
     (the poll rule), and a call through the engine polls there. Polling here too cost
     a counter update on every call. Entry at pc 0 is the common case and skips
     the switch as well. */
  if (fn->ninsns > 0 && fn->insns[0].pc == 0)
    fprintf(f, "    if (__builtin_expect(entry_pc == 0, 1)) goto g_0;\n");
  fprintf(f, "    goto dispatch_switch;\n");
  fprintf(f, "dispatch:\n");
  /* A jump to a pc at or below the one that produced it is a back edge, so
     it polls: an inline decrement of the interrupt counter, and the engine only
     when the counter runs out. Without this a compiled loop never yields. */
  fprintf(f, "    if (__builtin_expect(entry_pc <= e_from_pc, 0)) {\n");
  fprintf(
      f, "        if (__builtin_expect(--*(int *)((unsigned char *)ctx +\n");
  fprintf(
      f, "                QJS_AOT_OFF_CTX_INTERRUPT_COUNTER) <= 0, 0)) {\n");
  fprintf(f, "            ++*(int *)((unsigned char *)ctx +\n");
  fprintf(f, "                QJS_AOT_OFF_CTX_INTERRUPT_COUNTER);\n");
  fprintf(
      f,
      "            if (qjs_aot_poll(ctx) < 0) { pc = e_from_pc; goto "
      "exception; }\n");
  fprintf(f, "        }\n");
  fprintf(f, "    }\n");
  fprintf(f, "    e_from_pc = entry_pc;\n");
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
  /* A pc that is not an instruction start must never be dispatched. The
     counters count it so the gate can require zero. */
  fprintf(f, "    default:\n");
  fprintf(f, "        e_g8_hits++;\n");
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
    const ERole *role = e_roles[in->rop];

    if (role == NULL) {
      fprintf(stderr, "qjsaot: no role for op %d\n", in->rop);
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
            "    if (__builtin_expect(!q2_elem_open(AOT_REG(fr, %d), &_reg), "
            "0)) _reg.count = 0;\n"
            "    {\n",
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

  /* With no control records nothing can catch and there is nothing to
     close, so the exception leaves directly; that also avoids the unwind path,
     which reads the JSStackFrame a light frame does not have. */
  fprintf(f, "exception:\n");
  if (!has_ctl) {
    fprintf(f, "    return qjs_aot_throw_here(ctx, f, pc);\n");
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

/* The guard-stub tables' size. A large top-level function has hundreds of guard
   pcs; running out aborts rather than leave a goto without its label. */
#define E_MAX_STUBS 65536
/* Overflow exits seen while emitting: each gets a stub after the blocks. The
   `cur[]` snapshot is the block-local state AT that instruction -- the stub must
   sync from it, or it writes nothing and the generic body reads the frame's old
   contents (which is what made a deleted overflow check look harmless: the
   resumed body then recomputed from stale-but-correct arguments). */
static int g_e_ox[E_MAX_STUBS];
static Q2ValueId *g_e_ox_cur[E_MAX_STUBS];
static int g_e_ox_blk[E_MAX_STUBS];
static int g_e_ox_n = 0;
/* Hand-off histogram: which rop a spec block gave up at, weighted by the profile's
   count for that pc. One line per rop in the generator report, sorted by weight, so
   the next thing to lower is chosen by data instead of found one at a time. */
#define E_HANDOFF_MAX 512
typedef struct EHandoffStat {
  uint16_t rop;
  unsigned long long weight;
  int blocks;
} EHandoffStat;
static EHandoffStat g_e_ho[E_HANDOFF_MAX];
static int g_e_ho_n = 0;

static int g_e_quiet;
static void e_handoff_note(const Q2Func *fn, const Q2Insn *in) {
  int i;
  if (in == NULL || g_e_quiet) return;
  for (i = 0; i < g_e_ho_n; i++) {
    if (g_e_ho[i].rop == in->rop) {
      g_e_ho[i].weight += qjs_aot_prof_pc_count(e_dg(fn, in), in->opc);
      g_e_ho[i].blocks++;
      return;
    }
  }
  if (g_e_ho_n < E_HANDOFF_MAX) {
    g_e_ho[g_e_ho_n].rop = in->rop;
    g_e_ho[g_e_ho_n].weight = qjs_aot_prof_pc_count(e_dg(fn, in), in->opc);
    g_e_ho[g_e_ho_n].blocks = 1;
    g_e_ho_n++;
  }
}

/* The report: `handoff rop=<n> name=<..> weight=<..> blocks=<..>`, heaviest
   first. */
void e_handoff_report(FILE *f) {
  int i, j;
  for (i = 0; i < g_e_ho_n; i++)
    for (j = i + 1; j < g_e_ho_n; j++)
      if (g_e_ho[j].weight > g_e_ho[i].weight) {
        EHandoffStat t = g_e_ho[i];
        g_e_ho[i] = g_e_ho[j];
        g_e_ho[j] = t;
      }
  for (i = 0; i < g_e_ho_n; i++) {
    char nm[64];
    e_rop_name(g_e_ho[i].rop, nm, sizeof(nm));
    fprintf(
        f, "handoff rop=%u name=%s weight=%llu blocks=%d\n",
        (unsigned)g_e_ho[i].rop, nm, g_e_ho[i].weight, g_e_ho[i].blocks);
  }
  g_e_ho_n = 0;
}
/* Argument-guard exits: same treatment. The guard fires before the argument is in
   a local and the generic body's g_<pc> reloads it, but anything defined earlier in
   the prefix is only in a local, so the stub syncs from its snapshot. */
static int g_e_ax[E_MAX_STUBS];
static Q2ValueId *g_e_ax_cur[E_MAX_STUBS];
static int g_e_ax_blk[E_MAX_STUBS];
/* The compare COLD ARM: a TAG operand whose tag is neither INT nor FLOAT64. It is a
   hand-off at the branch's own pc, not a deopt. */
/* the pcs after a suspension that the body can be entered at (they have an `osr_` label) */
static int g_e_osr_res[16], g_e_osr_res_n;
static int g_e_hx[E_MAX_STUBS];
static Q2ValueId *g_e_hx_cur[E_MAX_STUBS];
static int g_e_hx_blk[E_MAX_STUBS];
static int g_e_hx_n;
static int g_e_vx[E_MAX_STUBS];
static Q2ValueId *g_e_vx_cur[E_MAX_STUBS];
static int g_e_vx_blk[E_MAX_STUBS];
/* QJS_AOT_SPEC_MAXPC=<n>: do not lower any instruction at pc >= n. A planned
   hand-off, so the unit stays correct -- it just speculates less -- which is what
   makes it usable as a bisect probe INSIDE one function. Read once per process. */
static int e_spec_maxpc = -2;
static int e_spec_maxpc_get(void) {
  if (e_spec_maxpc == -2) {
    const char *v = getenv("QJS_AOT_SPEC_MAXPC");
    e_spec_maxpc = -1;
    if (v != NULL && v[0] != 0) {
      int n = -1;
      if (sscanf(v, "%d", &n) == 1 && n >= 0) e_spec_maxpc = n;
    }
  }
  return e_spec_maxpc;
}

static int g_e_dx[E_MAX_STUBS];
static Q2ValueId *g_e_dx_cur[E_MAX_STUBS];
static int g_e_dx_blk[E_MAX_STUBS];
static int g_e_dx_n;
static int g_e_vx_n;
static int g_e_ax_n = 0;

/* The function being emitted, for the helpers that need its value table. */
static const Q2Func *g_e_fn = NULL;
/* The block being emitted, recorded with each guard stub's snapshot. */
static int g_e_blk = -1;
/* The function index of the body being emitted; every exit's note must name it,
   or its exits are counted against another function's switch-off. */
/* The per-function report counts, reset at each spec body: op-call
   wrappers emitted, instructions lowered typed or through a wrapper, guard sites
   by kind (ax, vx, dx, ox), and exits by kind (deopt, hand-off). */
static unsigned g_e_opcalls;
static int g_e_n_typed, g_e_n_opcall;
static int g_e_n_guard[4];
static int g_e_n_exit, g_e_n_handoff;

/* The frameless body: the speculative body written for a function
   that has no frame. `g_e_fl` is set while one is being emitted; the first reason
   it cannot be one (an op that needs the engine's handler, an unlowered
   instruction) is `g_e_fl_block`, and the caller falls back to the framed body.
   `g_e_quiet` marks the trial emission whose output is thrown away: it must not
   print, count or take a sweep site. */
static int g_e_fl;

static char g_e_fl_block[160];
/* The reason the LAST function was not frameless, for the entry report line. */
static char g_e_fl_why[160];
/* Set by the spec body when it emitted a frameless function, read by the driver
   that decides which entry to emit. */
static int g_e_fl_chosen;
/* Set when the frameless body being emitted calls a compiled function: it then
   keeps an activation record (JSAotFlRec) for the backtrace, and polls and checks
   the C stack at its entry. Kept from the trial emission into the real one. */
static int g_e_fl_calls;
/* The next site number of a direct call to a builtin (see emit_typed_call). */
static int g_e_ccall_next;
/* The published root array of a frameless body that calls (see JSAotFlRec): the slots
   of unrooted TAG values that live across a call or an allocation come first, then
   the argument window of the call in progress. A value gets its slot at its first
   safepoint; the count of slots found by the trial emission sizes the array for the
   real one. */
static int *g_e_spill_slot;
static int g_e_spill_n, g_e_spill_total, g_e_spill_nv;
/* The inline chain shares one published root array, owned by the outermost frameless body:
   it is filled with undefined once at the outer entry, its record covers every slot for the
   whole activation, and an inlined body writes its roots into a slice at the end of its
   caller's own slots. A slot that is not overwritten pins its object until the outer frame
   exits: the release is delayed, an object is never freed under a pointer the scan reads.
   `g_e_child_max` is the largest slice a callee inlined so far needs; `g_e_child_decl` is the
   value the trial emission found, which sizes the array of the real one. */
static int g_e_child_max, g_e_child_decl, g_e_last_inl_rz;
/* The layout the inline body just emitted tests its `this` for at entry (0: none). A caller
   that knows its receiver is in that layout passes `this_ok_` and the test is not run. */
static int g_e_last_inl_tl;
int qjs_aot_inline_thislay(const char hex[33]);
static int e_win_max(const Q2Func *fn) {
  int wi, mw = 1;
  for (wi = 0; wi < fn->ninsns; wi++)
    if ((fn->insns[wi].rop == ROP_call || fn->insns[wi].rop == ROP_call_method ||
         fn->insns[wi].rop == ROP_array_from || fn->insns[wi].rop == ROP_object_fill) &&
        fn->insns[wi].win_n > mw)
      mw = fn->insns[wi].win_n;
  return mw;
}
int qjs_aot_inline_rz(const char hex[33]);
#define E_SPILL_MAX 16
static int e_spill_slot(Q2ValueId v) {
  if (g_e_spill_slot == NULL || (int)v >= g_e_spill_nv) return -1;
  if (g_e_spill_slot[v] < 0) {
    if (g_e_spill_n >= E_SPILL_MAX) return -1;
    g_e_spill_slot[v] = g_e_spill_n++;
  }
  return g_e_spill_slot[v];
}

/* Which blocks sit in a loop, and whether some loop of the function may run long
   (set by the prefilter). A miss exit emitted in such a block, for an access the
   profile cannot vouch for, keeps the function framed. */
static uint8_t *g_e_loop_blk;
static int g_e_fl_longloop;
/* Set around a miss exit whose cause the profile CAN vouch for (a number test on an
   operand it saw as a number, a global's cache). */
static int g_e_fl_miss_ok;

static void e_fl_block(const char *what, const Q2Insn *in) {
  char nm[64];
  if (!g_e_fl || g_e_fl_block[0] != 0) return;
  if (in != NULL) {
    e_rop_name(in->rop, nm, sizeof(nm));
    snprintf(g_e_fl_block, sizeof(g_e_fl_block), "%s %s@%d", what, nm, in->pc);
  } else {
    snprintf(g_e_fl_block, sizeof(g_e_fl_block), "%s", what);
  }
}

/* In a frameless body a miss is an exit, and a site whose reads the profile saw
   hit anything but the object's own property (a method on the prototype, a getter)
   misses the inline own-property test on EVERY execution: the function would leave
   every time and the 257-exit rule would switch it off. Such a read keeps the
   function framed, where the miss continues. */
int qjs_aot_prof_site_own(const uint8_t dg[16], int site);
int qjs_aot_prof_site_cold(const uint8_t dg[16], int site);
int qjs_aot_prof_site_read_hits(const uint8_t dg[16], int site);
static int e_fl_site_ok(const Q2Func *fn, const Q2Insn *in, int site) {
  if (!g_e_fl) return 1;
  {
    int osite;
    const uint8_t *dg = e_site_dg(fn, site, &osite);
    if (qjs_aot_prof_site_own(dg, osite)) return 1;
    if (qjs_aot_prof_site_read_hits(dg, osite)) return 1;
    if (qjs_aot_prof_site_cold(dg, osite) &&
        !(g_e_loop_blk != NULL && g_e_blk >= 0 && g_e_loop_blk[g_e_blk]))
      return 1;
  }
  e_fl_block("a read that is not an own or prototype hit", in);
  return 0;
}


/* A read site the function's cache holds every shape of: the profile saw at most four, all own or
   direct-prototype hits, so the inline walk of the ways misses only on a shape it never saw. */
int qjs_aot_prof_site(const uint8_t dg[16], int site, int *nshapes, uint64_t shapes[4]);
static int e_site_vouched(const Q2Func *fn, int site) {
  int ns = 0;
  uint64_t sh[4];
  /* In a loop that may run long a frameless body gives up the entry-validated cache cells the
     framed body hoists out of the loop, which costs more per iteration than the frame saves. */
  if (g_e_fl_longloop && g_e_loop_blk != NULL && g_e_blk >= 0 && g_e_loop_blk[g_e_blk]) return 0;
  {
    int osite;
    const uint8_t *dg = e_site_dg(fn, site, &osite);
    return qjs_aot_prof_site(dg, osite, &ns, sh) && ns >= 1 && ns <= 4 &&
           qjs_aot_prof_site_read_hits(dg, osite);
  }
}

/* The deopt-everywhere sweep at a spec guard: in a sweep build the guard can be
   forced to fail, so its stub runs even where the speculation always holds. It
   sits where the guard's own exit would be taken -- for dx_, after the op's raw
   result is in the frame. A normal build emits nothing. hx_ is a hand-off, not a
   deopt, and is not swept. */
static const char *g_e_sweep_store;

static void e_sweep_guard(FILE *f, const char *kind, int pc) {
  int sid;
  g_e_n_guard
      [kind[0] == 'a'   ? 0
       : kind[0] == 'v' ? 1
       : kind[0] == 'd' ? 2
                        : 3]++;
  if (!qjs_aot_sweep_on() || g_e_quiet) return;
  sid = qjs_aot_site_alloc();
  /* The unit's coverage bitmap is this size; a site past it would read as never
     reached. */
  if (sid >= (1 << 20)) {
    fprintf(stderr, "qjsaot: sweep site %d is past the coverage bitmap\n", sid);
    abort();
  }
  /* The trailing comment is the site map: the harness reads it from the unit to
     name an unreached site by function, kind and pc. */
  fprintf(
      f,
      "    if (__builtin_expect(e_sweep_hit(ctx, %d), 0)) { %s goto %s_%d; }"
      " /* sweep-site %d %s %d %d %s */\n",
      sid, g_e_sweep_store ? g_e_sweep_store : "", kind, pc, sid, kind, pc,
      g_e_fi, g_cur_hex);
}

/* The sweep at a guard whose raw result is stored to the frame only when the guard
   really misses: the forced exit stores it too. */
static void e_sweep_guard_set(
    FILE *f, const char *kind, int pc, int off, const char *raw) {
  char st[96];
  snprintf(st, sizeof(st), "AOT_SET_B(ctx, fr, %d, %s);", off, raw);
  g_e_sweep_store = st;
  e_sweep_guard(f, kind, pc);
  g_e_sweep_store = NULL;
}

/* The same for an owned result, which the register adopts. */
static void e_sweep_guard_owned(FILE *f, int pc, int off) {
  char st[160];
  snprintf(
      st, sizeof(st),
      "if (JS_VALUE_HAS_REF_COUNT(_o)) qjs_aot_set_reg_p(ctx, fr, %d, _o); "
      "else AOT_SET_B(ctx, fr, %d, _o);",
      off, off);
  g_e_sweep_store = st;
  e_sweep_guard(f, "dx", pc);
  g_e_sweep_store = NULL;
}

/* A register an op has already written to the frame: its raw result, before the
   guard that types it. Not a value id; the exit sync leaves the slot alone. */
#define E_INFRAME ((Q2ValueId)-3)

/* For QJS_AOT_EXIT_LOG: the C expression of the value a guard stub's guard
   tested, by (kind, pc), so the stub can print the tag it actually saw. The first
   guard registered at a pc names it. Reset per spec body. */
static struct {
  char kind;
  int pc;
  char expr[64];
} g_e_xtag[E_MAX_STUBS];
static int g_e_xtag_n;
static void e_note_tag(char kind, int pc, const char *expr) {
  int q;
  for (q = 0; q < g_e_xtag_n; q++)
    if (g_e_xtag[q].kind == kind && g_e_xtag[q].pc == pc) return;
  if (g_e_xtag_n >= E_MAX_STUBS) return;
  g_e_xtag[g_e_xtag_n].kind = kind;
  g_e_xtag[g_e_xtag_n].pc = pc;
  snprintf(g_e_xtag[g_e_xtag_n].expr, sizeof(g_e_xtag[0].expr), "%s", expr);
  g_e_xtag_n++;
}
/* A frameless body's TAG values are invisible to the collector, so a safepoint
   with one live is only safe when the value is kept alive by someone else. The
   caller's argument window and receiver are: `get_arg`, `get_arg2`, `load_this`
   and moves of those, and a block parameter whose every incoming value is one of
   them. A function that stores to an argument gives up the argument half (the
   window slot no longer holds what was read). Filled before the blocks are
   emitted; NULL in a framed body. */
static uint8_t *g_e_rooted;
/* Values whose property array pointer is held in a local `pp<N>`: a value of a contracted
   layout read at the function's entry block. While the value stays in its layout (the facts
   and the broken-bit rechecks guarantee it at every access) its property array is not moved,
   so every slot access loads nothing but the slot. */
static uint8_t *g_e_pp;
static int e_pp_of(const char *recv) {
  unsigned n;
  if (recv[0] != 'v' || sscanf(recv + 1, "%u", &n) != 1) return -1;
  return g_e_pp != NULL && (int)n < g_e_spill_nv && g_e_pp[n] ? (int)n : -1;
}

static void e_compute_rooted(const Q2Func *fn) {
  int nv = fn->nvalues, i, b, k, si, changed;
  int has_put_arg = 0;
  free(g_e_rooted);
  g_e_rooted = calloc((size_t)(nv > 0 ? nv : 1), 1);
  if (g_e_rooted == NULL) abort();
  for (i = 0; i < fn->ninsns; i++)
    if (fn->insns[i].rop == ROP_put_arg || fn->insns[i].rop == ROP_put_arg_xfer)
      has_put_arg = 1;
  for (i = 0; i < nv; i++) {
    int ins = fn->values[i].insn;
    if (ins < 0) {
      g_e_rooted[i] = 1; /* a parameter: optimistic, lowered below */
      continue;
    }
    switch (fn->insns[ins].rop) {
      case ROP_get_arg:
      case ROP_get_arg2:
        g_e_rooted[i] = !has_put_arg;
        break;
      case ROP_load_this:
        g_e_rooted[i] = 1;
        break;
      case ROP_move:
      case ROP_move_s:
        g_e_rooted[i] = 2; /* decided from its source below */
        break;
      default:
        g_e_rooted[i] = 0;
        break;
    }
  }
  do {
    changed = 0;
    /* moves follow their source */
    for (i = 0; i < nv; i++) {
      int ins = fn->values[i].insn;
      if (ins >= 0 && g_e_rooted[i] != 0 &&
          (fn->insns[ins].rop == ROP_move || fn->insns[ins].rop == ROP_move_s)) {
        Q2ValueId src =
            fn->insns[ins].nuse > 0 ? fn->insns[ins].use[0] : Q2_NO_VALUE;
        uint8_t r = (src != Q2_NO_VALUE && src < (Q2ValueId)nv &&
                     g_e_rooted[src] == 1)
                        ? 1
                        : 0;
        if (g_e_rooted[i] != r) {
          g_e_rooted[i] = r;
          changed = 1;
        }
      }
    }
    /* a parameter is rooted only if every incoming value is */
    for (b = 0; b < fn->nblocks; b++) {
      const Q2Block *blk = &fn->blocks[b];
      for (si = 0; si < 2; si++) {
        const Q2Block *sb;
        int np;
        if (si >= blk->nsucc || blk->succ_block[si] < 0 ||
            blk->succ_block[si] >= fn->nblocks)
          continue;
        sb = &fn->blocks[blk->succ_block[si]];
        np = sb->nparams;
        if (blk->nedge_args[si] >= 0 && blk->nedge_args[si] < np)
          np = blk->nedge_args[si];
        for (k = 0; k < np; k++) {
          Q2ValueId pv = sb->param_val[k], av = blk->edge_arg[si][k];
          if (pv == Q2_NO_VALUE || pv >= (Q2ValueId)nv) continue;
          if (g_e_rooted[pv] == 1 &&
              (av == Q2_NO_VALUE || av >= (Q2ValueId)nv ||
               g_e_rooted[av] != 1)) {
            g_e_rooted[pv] = 0;
            changed = 1;
          }
        }
      }
    }
  } while (changed);
  for (i = 0; i < nv; i++)
    if (g_e_rooted[i] == 2) g_e_rooted[i] = 0;
}

/* Whether a typed def is dead: its register is not live after the defining
   instruction. That is the set every exit syncs from, and it counts a call's
   argument window, so nothing -- no instruction, no exit -- reads such a local.
   Its guard is skipped: the op's raw result is already in the frame, which is all
   the generic body needs. (Use lists alone are not enough: a call reads its
   arguments through the window, which no use list names.) */
static int e_def_dead(const Q2Insn *in, Q2ValueId v) {
  const uint64_t *lo;
  int r;
  if (v >= (Q2ValueId)g_e_fn->nvalues) return 0;
  r = g_e_fn->values[v].reg;
  if (r < 0 || r >= g_e_fn->nregs) return 0;
  lo = q2_live_out(g_e_fn, (int)(in - g_e_fn->insns));
  if (lo == NULL) return 0;
  if ((lo[r >> 6] >> (r & 63)) & 1) return 0;
  /* A later load the facts forward is a copy of this value's local: the register
     being overwritten does not make the value unused. */
  if (g_e_facts != NULL && g_e_facts->fwd != NULL) {
    int k;
    for (k = 0; k < g_e_fn->ninsns; k++)
      if (g_e_facts->fwd[k] == v) return 0;
  }
  return 1;
}

static const char *e_tag_expr(char kind, int pc) {
  int q;
  for (q = 0; q < g_e_xtag_n; q++)
    if (g_e_xtag[q].kind == kind && g_e_xtag[q].pc == pc)
      return g_e_xtag[q].expr;
  return NULL;
}

/* Register a guard stub at `pc` ONCE per body, with a copy of cur[] at the guard.
   The stub syncs from that copy: a NULL sync set drops every value the prefix holds
   only in a local. `wrote`, when given, is the op whose defs are already in the
   frame -- its registers are marked E_INFRAME, since the stub would otherwise
   write a typed local the failed guard never assigned. */
static void e_note_stub(
    int *pcs, Q2ValueId **curs, int *blks, int *n, int pc, const Q2ValueId *cur,
    const Q2Insn *wrote) {
  Q2ValueId *snap;
  int q, nr = g_e_fn->nregs;
  for (q = 0; q < *n; q++)
    if (pcs[q] == pc) return;
  if (*n >= E_MAX_STUBS || cur == NULL) {
    fprintf(
        stderr, "qjsaot: guard stub at pc %d has no sync set (%s)\n", pc,
        cur == NULL ? "no cur" : "table full");
    abort();
  }
  snap = malloc(sizeof(*snap) * (size_t)(nr > 0 ? nr : 1));
  if (snap == NULL) abort();
  memcpy(snap, cur, sizeof(*snap) * (size_t)nr);
  if (wrote != NULL && wrote->ndef > 0 && wrote->def[0] != Q2_NO_VALUE) {
    char ex[64];
    snprintf(
        ex, sizeof(ex), "AOT_REG(fr, %d)",
        g_e_fn->values[wrote->def[0]].reg * 16);
    e_note_tag('d', pc, ex);
  }
  if (wrote != NULL)
    for (q = 0; q < wrote->ndef; q++) {
      Q2ValueId v = wrote->def[q];
      int r;
      if (v == Q2_NO_VALUE) continue;
      r = g_e_fn->values[v].reg;
      if (r >= 0 && r < nr) snap[r] = E_INFRAME;
    }
  /* The registers an op call rewrote beyond its defs hold the op's value in the
     frame too; the block's local for them is the old value. */
  if (wrote != NULL)
    for (q = 0; q < wrote->nredef; q++) {
      int r = wrote->redef_reg[q];
      if (r >= 0 && r < nr) snap[r] = E_INFRAME;
    }
  pcs[*n] = pc;
  curs[*n] = snap;
  blks[*n] = g_e_blk;
  (*n)++;
}

/* ---- monomorphic groups in the speculative body ------------------- */

/* The facts of the body being emitted, run in the grouped-exit mode: an access
   the pass marks `miss_exits` is emitted with its miss as an EXIT before the
   instruction, never as the op-call wrapper, because the pass treated it as no
   safepoint. NULL when the body has no analysis. */
static const EFacts *g_e_facts;
/* Which groups some access uses: only those are validated at entry and
   re-checked after a safepoint. */
static uint8_t *g_e_group_used;
/* The report counts: shape tests emitted and skipped, loads forwarded. */
static int g_e_n_shape, g_e_n_skip, g_e_n_fwd, g_e_n_hoist;
/* Set around an op-call wrapper whose caller has already written the live TAG
   values and the operands to the frame (a call site does that once for all its
   arms), so the wrapper does not write them again. */
static int g_e_presynced;
/* Set by a grouped emitter, so the dispatcher can check that an instruction the
   facts classed as a miss-exit was in fact emitted as one. */
static int g_e_miss_exit_seen;

static int e_insn_idx(const Q2Func *fn, const Q2Insn *in) {
  return (int)(in - fn->insns);
}

/* The group `site` belongs to when the facts say this instruction's miss exits,
   else -1. */
static int e_exit_group(const Q2Func *fn, const Q2Insn *in, int site) {
  if (g_e_facts == NULL || !g_e_facts->miss_exits[e_insn_idx(fn, in)])
    return -1;
  return e_facts_site_group(g_e_facts, site);
}

/* Whether the facts classed this instruction as a miss-exit: its miss must be
   an exit, so no emitter may give it the op-call wrapper. */
static int e_must_exit(const Q2Func *fn, const Q2Insn *in) {
  return g_e_facts != NULL && g_e_facts->miss_exits[e_insn_idx(fn, in)];
}

/* Set while a speculative body that assumes no lazy JSON value exists is emitted. */
static int g_e_nolazy = 0;

/* After a safepoint (an op call, a call): code in this body skips the lazy-JSON marker
   test on loaded values, which holds while no lazy JSON document has been created. A
   safepoint may create one (JSON.parse of a large text), so the flag is read again and a
   set flag exits at the next instruction, with the op's results already in the frame (the
   dx_ stub). One helper for every safepoint emitter. */
/* Set while a body is emitted whose entry validates the cache cells of its groups (and so defines
   `q2_ep`, the epoch it validated them at): an access to a hot site of such a group tests no cell, and
   every safepoint is followed by a comparison of the runtime's epoch with `q2_ep` that leaves the body
   when any cache entry has changed meanwhile. */
static int g_e_epoch_skip;
/* The prototype data properties a body reads through a non-contracted group: the entry reads
   each once, at the epoch its group shapes were validated at, so an access takes the value
   from a local. */
#define E_MC_MAX 32
static int g_e_mc_site[E_MC_MAX], g_e_mc_grp[E_MC_MAX], g_e_mc_n;
/* The value each cached read defines, so a call on it knows the callee's bound entry. */
#define E_MC_VALS 8192
static signed char g_e_mc_of[E_MC_VALS];
static int e_mc_of(Q2ValueId v) { return (unsigned)v < E_MC_VALS ? g_e_mc_of[v] : -1; }
static void e_mc_def(Q2ValueId v, int idx) {
  if ((unsigned)v < E_MC_VALS) g_e_mc_of[v] = (signed char)idx;
}
static int e_mc_index(int site) {
  int i;
  for (i = 0; i < g_e_mc_n; i++)
    if (g_e_mc_site[i] == site) return i;
  return -1;
}
static const ETypes *g_e_types;

static int g_e_recheck_done = -1;
static void e_emit_group_recheck(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const Q2ValueId *cur) {
  unsigned long long lm = g_e_facts != NULL ? (unsigned long long)g_e_facts->laymask[e_insn_idx(fn, in)] : 0;
  char lt[96] = "";
  if (!g_e_nolazy && !g_e_epoch_skip && lm == 0) return;
  /* The shape facts of these layouts were kept across the instruction: an object leaves a layout
     only by setting its bit. */
  if (lm != 0) snprintf(lt, sizeof(lt), "Q2_LAYOUT_BROKEN(ctx) & 0x%llxull", lm);
  e_note_stub(g_e_dx, g_e_dx_cur, g_e_dx_blk, &g_e_dx_n, in->next, cur, in);
  e_sweep_guard(f, "dx", in->next);
  fprintf(
      f, "    if (__builtin_expect(%s%s%s%s%s, 0)) goto dx_%d;\n", g_e_nolazy ? "E_LAZY_SEEN(ctx)" : "",
      g_e_nolazy && (g_e_epoch_skip || lm) ? " || " : "", g_e_epoch_skip ? "Q2_EPOCH_MOVED(ctx)" : "",
      g_e_epoch_skip && lm ? " || " : "", lt, in->next);
  g_e_recheck_done = in->pc;
}

/* A typed definition held only in its local, put in its frame register for the exit stub that reads it
   from there. */
static void e_store_typed_def(FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId dv) {
  int reg;
  if (dv == Q2_NO_VALUE || dv >= (Q2ValueId)t->nvalues || e_def_dead(in, dv)) return;
  reg = fn->values[dv].reg;
  if (reg < 0 || reg >= fn->nregs) return;
  switch (t->lb[dv]) {
    case E_LB_I32: fprintf(f, "        AOT_SET_B(ctx, fr, %d, JS_MKVAL(JS_TAG_INT, i%u));\n", reg * 16, (unsigned)dv); break;
    case E_LB_F64: fprintf(f, "        AOT_SET_B(ctx, fr, %d, E_BOX_F64(d%u));\n", reg * 16, (unsigned)dv); break;
    case E_LB_BOOL: fprintf(f, "        AOT_SET_B(ctx, fr, %d, JS_MKVAL(JS_TAG_BOOL, b%u));\n", reg * 16, (unsigned)dv); break;
    default: break;
  }
}

/* Whether the entry validated the cell of `site` in group `g`, which no safepoint has since been
   followed by a change of the epoch. */
static int e_cell_entry_valid(int g, int site) {
  int z;
  if (!g_e_epoch_skip || g_e_facts == NULL || g < 0 || g >= g_e_facts->ngroups || g_e_group_used == NULL) return 0;
  if (!g_e_group_used[g] || g_e_facts->groups[g].own) return 0;
  for (z = 0; z < g_e_facts->nsites; z++)
    if (g_e_facts->sites[z].site == site) return g_e_facts->sites[z].hot;
  return 0;
}

/* Defined with the other field emitters below; the chain and write emitters
   above them call these. */
static int emit_group_chain(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t,
    Q2ValueId *cur, int g1, int s1, int g2, int s2, Q2ValueId rv, Q2ValueId d0v,
    Q2ValueId d1v, int d0reg, int d1reg);
static int emit_group_write(
    FILE *f, const Q2Func *fn, const Q2Insn *in, Q2ValueId *cur, int g,
    int site, Q2ValueId rv, const char *boxed);

/* The analysis for the function being emitted. The spec body's representations
   come from HERE (ETypes.lb), never from Q2Value.rep: that holds the old
   emitter's inference, whose BOT reads as TAG and which disagrees with the
   analysis on values like a block parameter. */
static const ETypes *g_e_types = NULL;

/* The local-name prefix for a value, from the analysis. */
static const char *e_rep_prefix(Q2ValueId v) {
  int lb = (g_e_types != NULL && v < (Q2ValueId)g_e_types->nvalues)
               ? g_e_types->lb[v]
               : E_LB_TAG;
  switch (lb) {
    case E_LB_I32: return "i";
    case E_LB_F64: return "d";
    case E_LB_BOOL: return "b";
    default: return "v";
  }
}

/* A trivial instruction lowered by the spec body. It emits no `g_`
   label -- those belong to the generic body -- and it never calls the generic
   emitter's per-instruction functions. It is the same store `emit_trivial`
   makes, keyed on the role table so the operands are never decoded here. */
/* The name of a value's local, by its own representation: `i<v>`, `d<v>`,
   `b<v>` or `v<v>`. */
static void e_local_name(char *buf, size_t n, Q2ValueId v) {
  snprintf(buf, n, "%s%u", e_rep_prefix(v), (unsigned)v);
}

/* Load an argument slot into a value's local, converting by the value's
   representation: the argument buffer holds a JSValue, and a typed local is not
   one. */
static void e_load_arg_to_local(
    FILE *f, Q2ValueId v, int slot, int pc, const Q2ValueId *cur) {
  char dn[32];
  e_local_name(dn, sizeof(dn), v);
  /* One guard per pc, not per destination: get_arg2 has two and both jump to
     the same label, synced from the snapshot taken before the first. */
  if (e_rep_prefix(v)[0] != 'v') {
    char ex[64];
    e_note_stub(g_e_ax, g_e_ax_cur, g_e_ax_blk, &g_e_ax_n, pc, cur, NULL);
    snprintf(ex, sizeof(ex), "q2_arg_buf(f)[%d]", slot);
    e_note_tag('a', pc, ex);
  }
  /* An argument the analysis typed needs its TAG CHECK here: the profile said
     this slot holds an int, but the caller may pass a double, a string or
     nothing at all. Reading the payload without checking would use garbage bits
     as an integer. The exit is at this instruction's pc, which is safe because
     nothing has been written to the frame yet. */
  switch (e_rep_prefix(v)[0]) {
    case 'i':
      e_sweep_guard(f, "ax", pc);
      fprintf(
          f, "    if (__builtin_expect(!Q2_I32_OK(q2_arg_buf(f)[%d], %s), 0)) goto ax_%d;\n", slot, dn, pc);
      break;
    case 'd':
      e_sweep_guard(f, "ax", pc);
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
      e_sweep_guard(f, "ax", pc);
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
static void e_copy_local(FILE *f, Q2ValueId d, Q2ValueId sv) {
  char dn[32], sn[32];
  const char *dp = e_rep_prefix(d), *sp = e_rep_prefix(sv);
  e_local_name(dn, sizeof(dn), d);
  e_local_name(sn, sizeof(sn), sv);
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
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t,
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
  if (t->lb[d] == E_LB_F64 &&
      (t->lb[a] == E_LB_F64 || t->lb[a] == E_LB_I32) &&
      (t->lb[b] == E_LB_F64 || t->lb[b] == E_LB_I32)) {
    char an[48], bn[48];
    if (t->lb[a] == E_LB_F64) snprintf(an, sizeof(an), "d%u", (unsigned)a);
    else snprintf(an, sizeof(an), "(double)i%u", (unsigned)a);
    if (t->lb[b] == E_LB_F64) snprintf(bn, sizeof(bn), "d%u", (unsigned)b);
    else snprintf(bn, sizeof(bn), "(double)i%u", (unsigned)b);
    fprintf(f, "    d%u = %s %s %s;\n", (unsigned)d, an, op, bn);
    if (cur != NULL && ld >= 0 && ld < fn->nregs) cur[ld] = d;
    return 1;
  }
  if (t->lb[d] != E_LB_I32) return 0;
  if (t->lb[a] != E_LB_I32 || t->lb[b] != E_LB_I32) return 0;
  /* add/sub/mul in int64, then check the result fits: one widening op and one
     compare, and the compare is the exit condition the analysis recorded. */
  fprintf(
      f,
      "    { int64_t _t = (int64_t)i%u %s (int64_t)i%u;\n",
      (unsigned)in->use[0], op, (unsigned)in->use[1]);
  {
    int ovf = t->exits != NULL && (t->exits[d] & E_EX_OVF);
    int nz = t->exits != NULL && in->rop == ROP_mul && (t->exits[d] & E_EX_NEGZERO);
    if (ovf || nz) {
      e_sweep_guard(f, "ox", in->pc);
      if (ovf)
        fprintf(
            f,
            "      if (__builtin_expect(_t < INT32_MIN || _t > INT32_MAX, 0)) "
            "goto ox_%d;\n",
            in->pc);
      /* a zero product with a negative operand is -0, which no int32 holds */
      if (nz)
        fprintf(
            f,
            "      if (__builtin_expect(_t == 0 && (i%u < 0 || i%u < 0), 0)) "
            "goto ox_%d;\n",
            (unsigned)in->use[0], (unsigned)in->use[1], in->pc);
      e_note_stub(
          g_e_ox, g_e_ox_cur, g_e_ox_blk, &g_e_ox_n, in->pc, cur, NULL);
    }
  }
  fprintf(f, "      i%u = (int32_t)_t; }\n", (unsigned)d);
  if (cur != NULL && ld >= 0 && ld < fn->nregs) cur[ld] = d;
  return 1;
}

/* Generation-time switch: omit the spec back-edge poll (see the call site). */
static int e_nosr_on(void) {
  static int cached = -1;
  if (cached < 0) cached = getenv("QJS_AOT_NOOSR") != NULL;
  return cached;
}

static int e_nopoll(void) {
  static int cached = -1;
  if (cached < 0) {
    const char *v = getenv("QJS_AOT_NOPOLL");
    cached = (v != NULL && v[0] == '1');
  }
  return cached;
}

/* A register whose slot a transfer EMPTIED. E_UNDEF is not a value id, so it
   cannot collide with one; the exit sync writes JS_UNDEFINED for it. */
#define E_UNDEF ((Q2ValueId)-2)

/* THE bookkeeping for a lowered instruction: which value each register holds now.
   Called once, from the dispatcher, after every lowered instruction -- not from
   inside each lowering. Ten scattered updates meant a new lowering could silently
   forget its own, leaving cur[] pointing at an older value, and cur[] is what an
   exit syncs to the frame.
   A def whose register is out of range is a generator bug, never a reason to
   index register 0. */
static void e_note_defs(Q2ValueId *cur, const Q2Func *fn, const Q2Insn *in) {
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
          "qjsaot: def v%d of rop %d has register %d outside 0..%d\n", (int)d,
          in->rop, r, fn->nregs - 1);
      abort();
    }
    cur[r] = d;
  }
  /* The registers an op-call fallback rewrote beyond its declared defs: the
     wrapper reloads each into its value's local, so the block holds it now. */
  for (k = 0; k < in->nredef; k++) {
    int r = in->redef_reg[k];
    if (in->redef_val[k] == Q2_NO_VALUE || r < 0 || r >= fn->nregs) continue;
    cur[r] = in->redef_val[k];
  }
}

/* Load a block's live-in register from the frame into the value's local. This is
   the block's entry load: after it the block works on locals only. */
static void e_load_local_from_frame(
    FILE *f, Q2ValueId v, int reg, const ETypes *t) {
  int off = reg * 16;
  /* The representation comes from the analysis (ETypes.lb), never from
     Q2Value.rep: that holds the OLD emitter's inference, whose BOT means TAG. */
  int lb = (t != NULL && v < (Q2ValueId)t->nvalues) ? t->lb[v] : E_LB_TAG;
  switch (lb) {
    case E_LB_I32:
      fprintf(f, "    i%u = JS_VALUE_GET_INT(AOT_REG(fr, %d));\n", (unsigned)v,
              off);
      break;
    case E_LB_F64:
      fprintf(
          f,
          "    d%u = JS_VALUE_GET_TAG(AOT_REG(fr, %d)) == JS_TAG_INT ? "
          "JS_VALUE_GET_INT(AOT_REG(fr, %d)) : "
          "JS_VALUE_GET_FLOAT64(AOT_REG(fr, %d));\n",
          (unsigned)v, off, off, off);
      break;
    case E_LB_BOOL:
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
  /* A move's source is use[0] and its destination is def[0]; both are named
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
      const char *p = e_rep_prefix(in->def[0]);
      if (p[0] == 'i' || p[0] == 'd') return 0;
    }
  }
  return 1;
}

static void emit_trivial_own(
    FILE *f, const Q2Insn *in, const ERole *role, Q2ValueId *cur) {
  Q2ValueId d = Q2_NO_VALUE;
  (void)role;
  if (in->ndef > 0) d = in->def[0];
  switch (in->rop) {
    case ROP_move:
    case ROP_move_s:
    case ROP_move_xfer: {
      /* The source is use[0], named by value id.
         move_xfer is the TRANSFER form: the generic body also EMPTIES the source
         register, so the source slot must be marked empty here or an exit that
         syncs it writes the old value back (for an object, a second frame
         reference with no dup). Under QJS_DRC registers borrow, so there is no
         dup or free to mirror -- the copy plus the empty marker is the whole
         effect. */
      Q2ValueId sv = in->nuse > 0 ? in->use[0] : Q2_NO_VALUE;
      /* The source must already be in a local; if it is not, this move is not
         lowerable here and the block must exit at it instead. Each side is
         named by ITS OWN representation: a value held as i32 is `i<v>`, not
         `v<v>`. */
      if (d != Q2_NO_VALUE && sv != Q2_NO_VALUE && e_rep_prefix(sv)[0] == 'v' && e_rep_prefix(d)[0] != 'v') {
        /* A copy the profile saw hold one kind of number: held unboxed behind the guard of a typed
           load. A miss leaves the value in the register and the generic body resumes after the copy. */
        char raw[24];
        int dreg = g_e_fn->values[d].reg;
        snprintf(raw, sizeof(raw), "v%u", (unsigned)sv);
        e_note_stub(g_e_dx, g_e_dx_cur, g_e_dx_blk, &g_e_dx_n, in->next, cur, in);
        fprintf(f, "    {\n");
        if (!emit_hit_result(f, g_e_fn, in, g_e_types, d, dreg, raw)) {
          fprintf(stderr, "qjsaot: a guarded copy at pc %d has no guard for its def\n", in->pc);
          abort();
        }
        fprintf(f, "    }\n");
      } else if (d != Q2_NO_VALUE && sv != Q2_NO_VALUE)
        e_copy_local(f, d, sv);
      if (in->rop == ROP_move_xfer && sv != Q2_NO_VALUE) {
        int sr = g_e_fn->values[sv].reg;
        if (sr >= 0 && sr < g_e_fn->nregs && cur != NULL) cur[sr] = E_UNDEF;
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
      switch (e_rep_prefix(d)[0]) {
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
        e_load_arg_to_local(f, d, (int)in->arg[1].val, in->pc, cur);
      break;
    case ROP_get_arg2: {
      /* Two argument slots in one instruction: the form nearly every function
         begins with, so without it no block's prefix reaches its arithmetic. */
      Q2ValueId d0 = in->ndef > 0 ? in->def[0] : Q2_NO_VALUE;
      Q2ValueId d1 = in->ndef > 1 ? in->def[1] : Q2_NO_VALUE;
      char n0[32], n1[32];
      if (in->narg >= 4) {
        if (d0 != Q2_NO_VALUE) {
          e_load_arg_to_local(f, d0, (int)in->arg[1].val, in->pc, cur);
          cur[g_e_fn->values[d0].reg < g_e_fn->nregs ? g_e_fn->values[d0].reg : 0] = d0;
        }
        if (d1 != Q2_NO_VALUE) {
          e_load_arg_to_local(f, d1, (int)in->arg[3].val, in->pc, cur);
          cur[g_e_fn->values[d1].reg < g_e_fn->nregs ? g_e_fn->values[d1].reg : 0] = d1;
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
      if (e_rep_prefix(d)[0] == 'b')
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
    int r = g_e_fn->values[d].reg;
    if (r >= 0 && r < g_e_fn->nregs) cur[r] = d;
  }
}

/* ---- the spec body's exit primitive ---------------------- */



/* Write one value's local back to its frame register, in the frame's
   representation. A TAG value is already a JSValue, so it is a plain store; an
   i32 or f64 is boxed the way the interpreter would have it. */
/* The virtual objects: the web (1-based) each value belongs to, computed for the function being
   emitted, and the register snapshot of the exit stub being written (its argument registers hold
   what the object is made from). */
static const Q2Func *g_e_virt_fn;
static uint8_t *g_e_virt;
static const Q2ValueId *g_e_stub_cur;
static int g_e_stub_pc;

static void e_virt_compute(const Q2Func *fn) {
  int i, sp = 0, *stack;
  if (g_e_virt_fn == fn) return;
  free(g_e_virt);
  g_e_virt = calloc((size_t)(fn->nvalues > 0 ? fn->nvalues : 1), 1);
  g_e_virt_fn = fn;
  if (g_e_virt == NULL || fn->nvweb == 0) return;
  stack = malloc(sizeof(int) * (size_t)(fn->nvalues > 0 ? fn->nvalues : 1));
  if (stack == NULL) return;
  for (i = 0; i < fn->ninsns; i++) {
    const Q2Insn *in = &fn->insns[i];
    if (in->vk == Q2_VK_DEF && in->ndef == 1) {
      g_e_virt[in->def[0]] = in->vweb;
      if (getenv("QJS_AOT_DEBUG_VIRT")) fprintf(stderr, "virt: value %d is the object of web %d (construct pc %d)\n", (int)in->def[0], in->vweb, in->pc);
      stack[sp++] = (int)in->def[0];
    }
  }
  while (sp > 0) {
    int x = stack[--sp], j, n, k;
    for (j = 0; j < fn->ninsns; j++) {
      const Q2Insn *in = &fn->insns[j];
      if (in->op == Q2_OP_MOVE && in->vk == 0 && in->nuse >= 1 && in->use[0] == (Q2ValueId)x && in->ndef == 1 &&
          !g_e_virt[in->def[0]]) {
        g_e_virt[in->def[0]] = g_e_virt[x];
        if (getenv("QJS_AOT_DEBUG_VIRT")) fprintf(stderr, "virt: value %d member of web %d via move %d (rop %d pc %d) of %d\n", (int)in->def[0], g_e_virt[x], j, in->rop, in->pc, x);
        stack[sp++] = (int)in->def[0];
      }
    }
    for (j = 0; j < fn->nblocks; j++) {
      const Q2Block *b = &fn->blocks[j];
      for (n = 0; n < b->nsucc; n++) {
        const Q2Block *sb = b->succ_block[n] >= 0 && b->succ_block[n] < fn->nblocks ? &fn->blocks[b->succ_block[n]] : NULL;
        for (k = 0; sb != NULL && k < b->nedge_args[n] && k < sb->nparams; k++)
          if (b->edge_arg[n][k] == (Q2ValueId)x && !g_e_virt[sb->param_val[k]]) {
            g_e_virt[sb->param_val[k]] = g_e_virt[x];
            if (getenv("QJS_AOT_DEBUG_VIRT")) fprintf(stderr, "virt: value %d member of web %d via edge of block %d from %d\n", (int)sb->param_val[k], g_e_virt[x], j, x);
            stack[sp++] = (int)sb->param_val[k];
          }
      }
    }
  }
  free(stack);
}

static void e_exit_expr(char *buf, size_t n, Q2ValueId v);
/* The object a virtual value stands for, made where the generic body takes over: the constructor
   is called with the arguments it was going to get. */
static int e_virt_expr(char *buf, size_t n, Q2ValueId v) {
  const Q2VWeb *vw;
  char body[2400], one[240], cexp[120];
  int j, len = 0;
  Q2ValueId cv;
  if (g_e_virt_fn == NULL || g_e_virt == NULL || v >= (Q2ValueId)g_e_virt_fn->nvalues || !g_e_virt[v] ||
      g_e_stub_cur == NULL)
    return 0;
  vw = &g_e_virt_fn->vweb[g_e_virt[v] - 1];
  cv = g_e_stub_cur[vw->creg];
  if (cv == Q2_NO_VALUE || cv == E_UNDEF || cv == E_INFRAME || cv >= (Q2ValueId)g_e_types->nvalues) {
    if (getenv("QJS_AOT_DEBUG_VIRT"))
      fprintf(stderr, "virt: object %u of web %d: constructor register %d holds %d at stub pc %d\n", (unsigned)v, (int)g_e_virt[v], vw->creg, (int)cv, g_e_stub_pc);
    snprintf(cexp, sizeof(cexp), "JS_UNDEFINED");
  } else if (g_e_virt[cv]) {
    fprintf(stderr, "qjsaot: virtual object %u: its constructor value %u is itself virtual\n", (unsigned)v, (unsigned)cv);
    abort();
  } else
    e_exit_expr(cexp, sizeof(cexp), cv);
  body[0] = 0;
  for (j = 0; j < vw->nfields; j++) {
    Q2ValueId av = g_e_stub_cur[vw->areg[vw->argidx[j]]];
    char one2[260];
    if (av == Q2_NO_VALUE || av == E_UNDEF || av == E_INFRAME || av >= (Q2ValueId)g_e_types->nvalues) {
      if (getenv("QJS_AOT_DEBUG_VIRT"))
        fprintf(stderr, "virt: object %u of web %d: argument register %d holds %d at stub pc %d\n", (unsigned)v, (int)g_e_virt[v],
                vw->areg[vw->argidx[j]], (int)av, g_e_stub_pc);
      snprintf(one, sizeof(one), "JS_UNDEFINED");
    } else
      e_exit_expr(one, sizeof(one), av);
    snprintf(
        one2, sizeof(one2),
        "if (JS_DefinePropertyValue(ctx, t_, (JSAtom)qjs_aot_ld_u32(code_ + %d), JS_DupValue(ctx, %s), JS_PROP_C_W_E) < 0) { "
        "JS_FreeValue(ctx, t_); return JS_EXCEPTION; } ",
        vw->aoff[j], one);
    len += snprintf(body + len, sizeof(body) - (size_t)len, "%s", one2);
  }
  snprintf(
      buf, n,
      "({ JSValue c_ = %s, t_; const uint8_t *code_ = NULL; int rc_ = qjs_aot_construct_this(ctx, (JSObject *)JS_VALUE_GET_PTR(c_), &t_); "
      "if (rc_ < 0) return JS_EXCEPTION; "
      "if (rc_ == 0) { JS_ThrowTypeError(ctx, \"not a constructor\"); return JS_EXCEPTION; } "
      "code_ = *(const uint8_t *const *)(*(const uint8_t *const *)((const uint8_t *)JS_VALUE_GET_PTR(c_) + "
      "QJS_AOT_OFF_OBJECT_FUNC_BYTECODE) + QJS_AOT_OFF_FNBC_REGVM_CODE); "
      "%sqjs_aot_construct_done((JSObject *)JS_VALUE_GET_PTR(c_), t_); t_; })",
      cexp, body);
  return 1;
}

static void e_exit_store(FILE *f, Q2ValueId v, int reg) {
  /* The register to write is the one being synced, NOT the one the value was
     defined in: an edge can move a value into another register's param, and
     those are different slots. Writing val->reg here silently left the slot the
     generic body actually reads holding its previous contents. */
  int off = reg * 16;
  {
    char vx[1600];
    if (e_virt_expr(vx, sizeof(vx), v)) {
      fprintf(f, "    qjs_aot_set_reg_p(ctx, fr, %d, %s);\n", off, vx);
      return;
    }
  }
  switch (e_rep_prefix(v)[0]) {
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
   to the generic one, and it is correct at any instruction pc because
   the generic body has a label for every instruction.

   The sync writes every register live at `pc`, not only the ones that changed:
   a typed local may hold a value the frame has never seen, and the generic body
   reads the frame unconditionally. */
/* The expression that boxes value `v` for an exit, in the frame's representation:
   what e_exit_store would store. */
static void e_exit_expr(char *buf, size_t n, Q2ValueId v) {
  if (e_virt_expr(buf, n, v)) return;
  switch (e_rep_prefix(v)[0]) {
    case 'i':
      snprintf(buf, n, "JS_MKVAL(JS_TAG_INT, i%u)", (unsigned)v);
      break;
    case 'd':
      snprintf(
          buf, n, "qjs_aot_box_num(d%u, qjs_aot_fitsi(d%u))", (unsigned)v,
          (unsigned)v);
      break;
    case 'b':
      snprintf(buf, n, "JS_MKVAL(JS_TAG_BOOL, b%u)", (unsigned)v);
      break;
    default:
      snprintf(buf, n, "v%u", (unsigned)v);
      break;
  }
}

/* The guard that is about to exit, for the exit log: a framed body logs it in
   line; a frameless one hands the kind and tag to its exit function through the
   exit record, so the body keeps no call of its own on the way out. */
static char g_e_xk[16], g_e_xt[160];
static void e_emit_exit_log(
    FILE *f, char hex[33], const char *kind, int pc, const char *tagexpr) {
  if (g_e_fl) {
    snprintf(g_e_xk, sizeof(g_e_xk), "\"%s\"", kind);
    snprintf(g_e_xt, sizeof(g_e_xt), "%s", tagexpr);
    return;
  }
  fprintf(f, "    E_EXIT_LOG(\"%s\", \"%s\", %d, %s);\n", hex, kind, pc, tagexpr);
}

/* The generic body entered at `pc` from the speculative body. In a framed body
   that is a tail call on the live frame. A frameless body has none: the live
   registers travel as (offset, value) pairs and `aot_nx_` builds the frame. */
/* the speculative body has loop entries, so a generic body it calls can hand back to it by returning rather than by a nested call */
static int g_e_osr_loop;

static void e_emit_ret_g(FILE *f, const char *indent, const char *hex, const char *pcexpr) {
  if (g_e_osr_loop)
    fprintf(f, "%sE_RET_G(%s, %s);\n", indent, hex, pcexpr);
  else
    fprintf(f, "%sreturn aot_g_%s(ctx, f, fr, %s);\n", indent, hex, pcexpr);
}

static void e_emit_generic_call(
    FILE *f, char hex[33], int pc, int nlv, const char *indent) {
  if (!g_e_fl && !g_e_inl) {
    char pe[24];
    snprintf(pe, sizeof(pe), "%d", pc);
    e_emit_ret_g(f, indent, hex, pe);
    return;
  }
  if (g_e_inl && !g_e_exit_dirty) {
    g_e_xk[0] = g_e_xt[0] = 0;
    return;
  }
  /* The live registers are in `fr` by now; the record says where they are. */
  fprintf(
      f,
      "%se_xs = (ENx){ %d, %s, %s, %s, %s, %d, argc, rec_ };\n"
      "%s%saot_nx_%s(ctx_in, caller_f, co, this_obj, argv, &e_xs);\n",
      indent, pc, g_e_xt[0] ? g_e_xt : "-100", g_e_xk[0] ? g_e_xk : "NULL",
      nlv > 0 ? "e_xl" : "NULL", nlv > 0 ? "offs" : "NULL", nlv, indent,
      g_e_inl ? "*out = " : "return ", hex);
  if (g_e_inl) fprintf(f, "%sreturn 1;\n", indent);
  g_e_xk[0] = g_e_xt[0] = 0;
}

static void e_emit_exit(
    FILE *f, const Q2Func *fn, int pc, char hex[33], int fi,
    const Q2ValueId *cur, const uint64_t *live_set, int handoff, int blkidx) {
  int words = fn->lwords, w, r;
  /* Frameless: the values are collected, not stored. */
  int nlv = 0, ndefer = 0, defer[64];
  int *loff = NULL;
  char (*lex)[1600] = NULL;

  if (g_e_fl) {
    loff = malloc(sizeof(int) * (size_t)(fn->nregs > 0 ? fn->nregs : 1));
    lex = malloc(sizeof(*lex) * (size_t)(fn->nregs > 0 ? fn->nregs : 1));
    if (loff == NULL || lex == NULL) abort();
  }
  if (handoff)
    g_e_n_handoff++;
  else
    g_e_n_exit++;
  if (fi != g_e_fi) {
    fprintf(
        stderr, "qjsaot: exit names function %d inside function %d's body\n", fi,
        g_e_fi);
    abort();
  }
  g_e_exit_dirty = 0;
  e_virt_compute(fn);
  g_e_stub_cur = cur;
  g_e_stub_pc = pc;
  if (g_e_inl) {
    const char *ctl = getenv("QJS_AOT_INL_CONTROL");
    g_e_exit_dirty = g_e_dirty_ins != NULL && g_e_exit_ins >= 0 &&
                      g_e_dirty_ins[g_e_exit_ins] &&
                      !(ctl != NULL && strcmp(ctl, "clean") == 0);
    if (!g_e_exit_dirty) {
      /* Nothing has happened: the caller's native call does the whole call. */
      fprintf(
          f, "    { E_EXIT_LOG(\"%s\", \"ix\", %d, -100); %s(%d); return 0; }\n", hex, pc,
          handoff ? "E_HANDOFF_NOTE" : "E_INL_EXIT_NOTE", fi);
      free(loff);
      free(lex);
      return;
    }
    g_e_inl_nx_used = 1;
  }
  fprintf(f, "    {\n");
  if (live_set != NULL) {
    for (w = 0; w < words; w++) {
      uint64_t bits = live_set[w];
      while (bits != 0) {
        r = w * 64 + __builtin_ctzll(bits);
        bits &= bits - 1;
        if (r >= fn->norig) continue;
        /* A register the spec body holds in a local is written from it; one it
           never touched already has the right contents in the frame, so it is
           left alone. Writing a stale definition would corrupt exactly that. */
        if (cur == NULL) continue;
        if (cur[r] == E_INFRAME) {
          /* The op left its result in the frame. A frameless body's "frame" is
             its register array, which the new frame has to be given. */
          if (g_e_fl) {
            loff[nlv] = r * 16;
            snprintf(lex[nlv], sizeof(lex[0]), "AOT_REG(fr, %d)", r * 16);
            nlv++;
          }
          continue;
        }
        if (cur[r] == E_UNDEF) {
          if (g_e_fl) {
            loff[nlv] = r * 16;
            snprintf(lex[nlv], sizeof(lex[0]), "JS_UNDEFINED");
            nlv++;
          } else {
            fprintf(f, "    AOT_SET_B(ctx, fr, %d, JS_UNDEFINED);\n", r * 16);
          }
          continue;
        }
        if (cur[r] == Q2_NO_VALUE) {
          /* The entry block legitimately has function-start state; anywhere
             else a live register with no value means the bookkeeping lost it. */
          if (blkidx != 0) {
            fprintf(
                stderr,
                "qjsaot: live register %d has no value at an exit in a non-entry "
                "block (fn %s)\n",
                r, hex);
            abort();
          }
          continue;
        }
        if (g_e_fl) {
          loff[nlv] = r * 16;
          e_exit_expr(lex[nlv], sizeof(lex[0]), cur[r]);
          nlv++;
        } else if (cur[r] < (Q2ValueId)fn->nvalues && g_e_virt != NULL && g_e_virt[cur[r]]) {
          /* An object that is not made yet is built last: making it allocates, and the
             collector must find every other register in the frame by then. */
          if (ndefer >= 64) abort();
          defer[ndefer++] = r;
        } else {
          e_exit_store(f, cur[r], r);
        }
      }
    }
    for (w = 0; w < ndefer; w++) e_exit_store(f, cur[defer[w]], defer[w]);
  }
  if (nlv > 0) {
    int q;
    for (q = 0; q < nlv; q++) fprintf(f, "    e_xl[%d] = %s;\n", q, lex[q]);
    fprintf(f, "    static const uint16_t offs[%d] = {", nlv);
    for (q = 0; q < nlv; q++) fprintf(f, "%s%d", q ? ", " : " ", loff[q]);
    fprintf(f, " };\n");
  }
  free(loff);
  free(lex);
  /* A hand-off is the planned exit at an instruction the spec body does not lower,
     not a failed speculation. Counting it as a deopt would switch nearly every
     spec body off after 257 calls, and every measurement would time the generic
     body. */
  /* A suspension (yield, await) the speculative body reached: the live registers are in
     the frame, so the instruction runs through its handler right here and the function
     returns, instead of entering the generic body to do the same. */
  if (handoff && !g_e_fl && !g_e_inl) {
    int ii, sus = 0;
    for (ii = 0; ii < fn->ninsns; ii++)
      if (fn->insns[ii].pc == pc) {
        const ERole *rl = e_roles[fn->insns[ii].rop];
        sus = rl != NULL && rl->control == E_CF_SUSPEND;
        break;
      }
    if (sus) {
      fprintf(
          f,
          "    { uint8_t *code_ = q2_code(f), *ip_ = qjs_aot_step(code_ + %d, fr, f, ctx);\n"
          "      if (__builtin_expect(ip_ == NULL, 0)) return qjs_aot_throw_here(ctx, f, UINT32_MAX);\n"
          "      if (__builtin_expect(ip_ == QJS_AOT_COLD_RET || ip_ == QJS_AOT_SUSPEND, 1))\n"
          "          return qjs_aot_ret(f);\n",
          pc);
      {
        int q, nx = -1;
        for (q = 0; q < fn->ninsns; q++)
          if (fn->insns[q].pc == pc) nx = fn->insns[q].next;
        for (q = 0; q < g_e_osr_res_n; q++)
          if (g_e_osr_res[q] == nx)
            fprintf(f, "      if (ip_ == code_ + %d) goto osr_%d;\n", nx, nx);
      }
      e_emit_ret_g(f, "      ", hex, "(uint32_t)(ip_ - code_)");
      fprintf(f, "    } }\n");
      return;
    }
  }
  fprintf(
      f, "    %s(%d);\n", handoff ? "E_HANDOFF_NOTE" : "E_EXIT_NOTE", fi);
  e_emit_generic_call(f, hex, pc, nlv, "    ");
  fprintf(f, "    }\n");
}

/* An exit at an instruction's pc, computing the live set here. The arithmetic
   exit uses it: the operands are live, the result is not yet. */
static void e_emit_exit_at(
    FILE *f, const Q2Func *fn, int ins, int pc, char hex[33], int fi,
    const Q2ValueId *cur, int blkidx, int handoff) {
  uint64_t *lb = calloc((size_t)(fn->lwords > 0 ? fn->lwords : 1), 8);
  if (cur == NULL) {
    fprintf(
        stderr, "qjsaot: exit stub at pc %d has no sync set (fn %s)\n", pc, hex);
    abort();
  }
  if (fn->insns[ins].xpc >= 0) {
    /* An inserted instruction: it leaves before anything ran, to the pc it was placed
       before, with the state that instruction starts with. */
    int j;
    for (j = 0; j < fn->ninsns; j++)
      if (fn->insns[j].xpc < 0 && fn->insns[j].pc == fn->insns[ins].xpc) {
        if (lb != NULL) q2_live_before(fn, j, lb);
        break;
      }
    pc = fn->insns[ins].xpc;
  } else if (lb != NULL) {
    q2_live_before(fn, ins, lb);
  }
  if (fn->insns[ins].own > 0) e_undo_restore(f, fn->insns[ins].own);
  g_e_exit_ins = ins;
  e_emit_exit(f, fn, pc, hex, fi, cur, lb, handoff, blkidx);
  g_e_exit_ins = -1;
  free(lb);
}

/* The C expression that converts value `src` to the representation of value
   `dst`: a typed source is a C scalar, only a TAG source is a JSValue, and a
   TAG source read as a typed local has its tag checked by the caller's guard.
   Shared by the edge moves and by a forwarded load, which is a copy. The result
   is a static buffer: use it before the next call. */
static const char *e_copy_expr(Q2ValueId src, Q2ValueId dst) {
  static char buf[192];
  char sn[32];
  const char *sp = e_rep_prefix(src), *dp = e_rep_prefix(dst);
  e_local_name(sn, sizeof(sn), src);
  if (sp[0] == dp[0]) snprintf(buf, sizeof(buf), "%s", sn);
  /* numeric <-> numeric is a cast */
  else if (sp[0] == 'i' && dp[0] == 'd')
    snprintf(buf, sizeof(buf), "(double)%s", sn);
  else if (sp[0] == 'd' && dp[0] == 'i')
    snprintf(buf, sizeof(buf), "(int32_t)%s", sn);
  else if (sp[0] == 'i' && dp[0] == 'b')
    snprintf(buf, sizeof(buf), "(%s != 0)", sn);
  else if (sp[0] == 'b' && dp[0] == 'i')
    snprintf(buf, sizeof(buf), "(int32_t)%s", sn);
  else if (sp[0] == 'b' && dp[0] == 'd')
    snprintf(buf, sizeof(buf), "(double)%s", sn);
  else if (sp[0] == 'd' && dp[0] == 'b')
    snprintf(buf, sizeof(buf), "(%s != 0)", sn);
  /* a TAG source can be read as any typed local, with the tag checked */
  else if (sp[0] == 'v' && dp[0] == 'i')
    snprintf(buf, sizeof(buf), "(int32_t)JS_VALUE_GET_INT(%s)", sn);
  else if (sp[0] == 'v' && dp[0] == 'd')
    snprintf(
        buf, sizeof(buf),
        "JS_VALUE_GET_TAG(%s) == JS_TAG_INT ? (double)JS_VALUE_GET_INT(%s) : "
        "JS_VALUE_GET_FLOAT64(%s)",
        sn, sn, sn);
  else if (sp[0] == 'v' && dp[0] == 'b')
    snprintf(buf, sizeof(buf), "JS_VALUE_GET_BOOL(%s)", sn);
  /* a typed source boxed into a JSValue destination */
  else if (sp[0] == 'i')
    snprintf(buf, sizeof(buf), "JS_MKVAL(JS_TAG_INT, %s)", sn);
  else if (sp[0] == 'd')
    snprintf(
        buf, sizeof(buf), "qjs_aot_box_num(%s, qjs_aot_fitsi(%s))", sn, sn);
  else if (sp[0] == 'b')
    snprintf(buf, sizeof(buf), "JS_MKVAL(JS_TAG_BOOL, %s)", sn);
  else
    snprintf(buf, sizeof(buf), "%s", sn);
  return buf;
}

/* One edge u->s as a PARALLEL move: temps first, then assign, so a swap is
   order-independent. Pure locals -- only exits write the frame. */
/* An edge's parameter moves are correct by construction: every moved source is
   copied into its own temp FIRST, and only then is every parameter assigned from
   the temps. "All reads before any write" is right for any permutation, including
   a swap or a rotation -- so there is no symbolic check and no "skip the temp when
   it looks safe" case. The extra temps are free: the C compiler removes them. */
static void e_emit_edge_moves(
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
    switch (e_rep_prefix(pv)[0]) {
      case 'i': fprintf(f, "      int32_t %s_t%d = ", e_rep_prefix(pv), k); break;
      case 'd': fprintf(f, "      double %s_t%d = ", e_rep_prefix(pv), k); break;
      case 'b': fprintf(f, "      int %s_t%d = ", e_rep_prefix(pv), k); break;
      default: fprintf(f, "      JSValue %s_t%d = ", e_rep_prefix(pv), k); break;
    }
    fprintf(f, "%s;\n", e_copy_expr(blk->edge_arg[si][k], pv));
  }
  for (k = 0; k < np; k++) {
    Q2ValueId pv = s->param_val[k];
    char pn[32];
    if (blk->edge_arg[si][k] == Q2_NO_VALUE) continue;
    e_local_name(pn, sizeof(pn), pv);
    fprintf(f, "      %s = %s_t%d;\n", pn, e_rep_prefix(pv), k);
  }
  fprintf(f, "    }\n");
}

/* One arm of a lowered branch: the edge moves for successor k, then the jump to
   THAT successor. One function taking k, so the (succ_block, edge_arg) pairing
   cannot drift between the condition's sense and the moves it is paired with. */
/* `want_succ` is the block the CALLER believes this arm targets (it knows it:
   blk->succ_block[k] from the block it just lowered). The arm resolves its own
   successor from `b`, so comparing the two is the real check: if `b` is not the
   block the caller meant, they differ and generation aborts. (Comparing `b` with
   `&fn->blocks[b]` would be a tautology.) Pass -1 to skip. */
static void e_emit_arm(
    FILE *f, const Q2Func *fn, int b, int k, Q2ValueId *cur,
    const char *hex, int want_succ) {
  const Q2Block *blk = &fn->blocks[b];
  int sb = blk->succ_block[k];
  e_virt_compute(fn);
  g_e_stub_cur = cur;
  if (want_succ >= 0 && sb != want_succ) {
    fprintf(
        stderr,
        "qjsaot: arm %d of block %d targets block %d but the caller meant %d -- the "
        "RPO position and the block index have been conflated\n",
        k, b, sb, want_succ);
    abort();
  }
  if (sb < 0 || sb >= fn->nblocks) {
    /* A target that leaves the function: the caller hands off instead. */
    fprintf(f, "      /* arm %d leaves the function */\n", k);
    return;
  }
  e_emit_edge_moves(f, fn, b, k, cur);
  /* Shape checks hoisted out of the loop this edge enters. */
  if (g_e_facts != NULL && g_e_facts->nhoist > 0 && !q2_is_back_edge(fn, b, sb)) {
    int q, any = 0;
    for (q = 0; q < g_e_facts->nhoist; q++)
      if (g_e_facts->hoist_hdr[q] == sb && g_e_facts->hoist_pre[q] == b) any = 1;
    if (any) {
      const Q2Block *hb = &fn->blocks[sb];
      Q2ValueId *seed = malloc(sizeof(*seed) * (size_t)(fn->nregs > 0 ? fn->nregs : 1));
      int r0;
      if (seed == NULL) abort();
      for (r0 = 0; r0 < fn->nregs; r0++) seed[r0] = Q2_NO_VALUE;
      for (r0 = 0; r0 < hb->nlive_in; r0++) {
        int rr = hb->in_reg[r0];
        if (rr >= 0 && rr < fn->nregs) seed[rr] = hb->in_val[r0];
      }
      /* the exit is at the header's first instruction: nothing of the loop has run */
      e_note_stub(g_e_vx, g_e_vx_cur, g_e_vx_blk, &g_e_vx_n, hb->start_pc, seed, NULL);
      free(seed);
      for (q = 0; q < g_e_facts->nhoist; q++) {
        Q2ValueId rv;
        if (g_e_facts->hoist_hdr[q] != sb || g_e_facts->hoist_pre[q] != b) continue;
        if (g_e_facts->hoist_grp[q] <= -1000) {
          e_sweep_guard(f, "vx", hb->start_pc);
          e_cell_hoist_check(f, g_e_facts->hoist_val[q], -1000 - g_e_facts->hoist_grp[q], "vx", hb->start_pc);
          g_e_n_hoist++;
          continue;
        }
        rv = (Q2ValueId)g_e_facts->hoist_val[q];
        e_sweep_guard(f, "vx", hb->start_pc);
        if (g_e_facts->hoist_val[q] == 0x10000000 && g_e_facts->hoist_grp[q] >= 0) {
          fprintf(
              f,
              "      { JSValue _th = %s;\n"
              "        if (__builtin_expect(JS_VALUE_GET_TAG(_th) != JS_TAG_OBJECT ||\n"
              "            q2_shape_of(_th) != gsh%d, 0)) goto vx_%d; }\n",
              g_e_fl ? "this_obj" : "*(JSValue *)((uint8_t *)f + QJS_AOT_OFF_REGRUN_THIS_OBJ)",
              g_e_facts->hoist_grp[q], hb->start_pc);
          g_e_n_hoist++;
          continue;
        }
        if (g_e_types != NULL && rv < (Q2ValueId)g_e_types->nvalues &&
            g_e_types->lb[rv] == E_LB_TAG &&
            (g_e_facts->hoist_grp[q] == -2 || (g_e_facts->hoist_grp[q] <= -101 && g_e_facts->hoist_grp[q] >= -112) ||
             g_e_facts->hoist_grp[q] == E_GRP_PK_I32 || g_e_facts->hoist_grp[q] == E_GRP_PK_F64)) {
          char fl[32];
          snprintf(fl, sizeof(fl), "goto vx_%d", hb->start_pc);
          e_arr_region_start_grp(f, rv, rv, fl, g_e_facts->hoist_grp[q]);
        } else if (g_e_types != NULL && rv < (Q2ValueId)g_e_types->nvalues &&
            g_e_types->lb[rv] == E_LB_TAG) {
          fprintf(
              f, "      if (__builtin_expect(q2_shape_of(v%u) != gsh%d, 0)) goto vx_%d;\n",
              (unsigned)rv, g_e_facts->hoist_grp[q], hb->start_pc);
        } else {
          fprintf(f, "      goto vx_%d;\n", hb->start_pc);
        }
        g_e_n_hoist++;
      }
    }
  }
  /* The poll and the pre-safepoint sync: every back edge polls, like the generic dispatch. Without it a
     loop in the spec body cannot be interrupted, and clang proves the loop to be
     side-effect free and replaces it with a closed form -- which is why an
     "empty" loop measured 0 instructions per iteration.
     The sync before the poll is REQUIRED: qjs_aot_poll can run the interrupt
     handler and the GC, and a TAG value held only in a C local is invisible to
     the GC, so the frame must own every live value before the call. */
  /* QJS_AOT_NOPOLL=1 omits the SPEC back-edge poll only (the generic body keeps
     its own). It exists so `m1.sh gates` can build a variant and prove that the
     poll is what stops a spec-body loop; it is a GENERATION switch, so it is part
     of the unit cache key. */
  if (q2_is_back_edge(fn, b, sb) && !e_nopoll()) {
    const Q2Block *t = &fn->blocks[sb];
    int q;
    fprintf(
        f,
        "      if (__builtin_expect(Q2_POLL_HIT(ctx), 0)) {\n");
    fprintf(
        f,
        "          ++*(int *)((unsigned char *)ctx +\n"
        "              QJS_AOT_OFF_CTX_INTERRUPT_COUNTER);\n");
    int unsafe_tag = 0;
    if (g_e_fl)
      for (q = 0; q < t->nlive_in; q++) {
        int rr = t->in_reg[q];
        Q2ValueId vv = t->in_val[q];
        if (rr < 0 || rr >= fn->nregs || vv == Q2_NO_VALUE) continue;
        if (e_rep_prefix(vv)[0] == 'v' &&
            !(g_e_rooted != NULL && vv < (Q2ValueId)fn->nvalues &&
              g_e_rooted[vv] == 1))
          unsafe_tag = 1;
      }
    if (g_e_fl && unsafe_tag && !g_e_inl && getenv("QJS_AOT_NOPOLLSPILL") == NULL) {
      /* A borrowed value only this body holds: publish it in a root slot for the poll, so the
         body keeps running here instead of finishing the whole call in the generic body. */
      int ok = 1;
      for (q = 0; q < t->nlive_in; q++) {
        int rr = t->in_reg[q];
        Q2ValueId vv = t->in_val[q];
        if (rr < 0 || rr >= fn->nregs || vv == Q2_NO_VALUE) continue;
        if (e_rep_prefix(vv)[0] == 'v' &&
            !(g_e_rooted != NULL && vv < (Q2ValueId)fn->nvalues && g_e_rooted[vv] == 1) &&
            e_spill_slot(vv) < 0)
          ok = 0;
      }
      if (ok) {
        for (q = 0; q < t->nlive_in; q++) {
          int rr = t->in_reg[q];
          Q2ValueId vv = t->in_val[q];
          if (rr < 0 || rr >= fn->nregs || vv == Q2_NO_VALUE) continue;
          if (e_rep_prefix(vv)[0] == 'v' &&
              !(g_e_rooted != NULL && vv < (Q2ValueId)fn->nvalues && g_e_rooted[vv] == 1))
            fprintf(f, "          ra_[%d] = v%u;\n", e_spill_slot(vv), (unsigned)vv);
        }
        g_e_fl_calls = 1;
        unsafe_tag = 0;
      }
    }
    if (g_e_fl && !unsafe_tag) {
      /* Every live TAG value is kept alive by the caller (an argument, the
         receiver): the poll may run an interrupt handler and the collector
         without a frame to scan. */
      fprintf(
          f, g_e_inl ? "          if (qjs_aot_poll_fl(ctx) < 0) { *out = JS_EXCEPTION; return 1; }\n"
                      : "          if (qjs_aot_poll_fl(ctx) < 0) return JS_EXCEPTION;\n");
    } else if (g_e_fl) {
      /* A borrowed value only this body holds: leave at the loop header, where
         the generic body has a frame, and let it poll at its own next back edge
         (this body is not re-entered, so the rest of the call runs there). */
      char ex[1600];
      int nlv = 0;
      e_virt_compute(fn);
      g_e_stub_cur = cur;
      fprintf(f, "          {\n");
      for (q = 0; q < t->nlive_in; q++) {
        int rr = t->in_reg[q];
        Q2ValueId vv = t->in_val[q];
        if (rr < 0 || rr >= fn->norig || vv == Q2_NO_VALUE) continue;
        e_exit_expr(
            ex, sizeof(ex), cur != NULL && cur[rr] != Q2_NO_VALUE ? cur[rr] : vv);
        fprintf(f, "              e_xl[%d] = %s;\n", nlv++, ex);
      }
      fprintf(f, "          static const uint16_t offs[%d] = {", nlv + 1);
      for (q = 0; q < t->nlive_in; q++) {
        int rr = t->in_reg[q];
        Q2ValueId vv = t->in_val[q];
        if (rr < 0 || rr >= fn->norig || vv == Q2_NO_VALUE) continue;
        fprintf(f, " %d,", rr * 16);
      }
      fprintf(f, " 0 };\n");
      /* Not noted: an interrupt is neither a hand-off nor a failed speculation. */
      if (g_e_inl) g_e_inl_nx_used = 1;
      fprintf(
          f,
          "          e_xs = (ENx){ %d, -100, \"poll\", e_xl, offs, %d, argc, rec_ };\n"
          "          %saot_nx_%s(ctx_in, caller_f, co, this_obj, argv, &e_xs);\n"
          "%s          }\n",
          t->start_pc, nlv, g_e_inl ? "*out = " : "return ",
          hex, g_e_inl ? "          return 1;\n" : "");
    } else {
      for (q = 0; q < t->nlive_in; q++) {
        int rr = t->in_reg[q];
        Q2ValueId vv = t->in_val[q];
        if (rr < 0 || rr >= fn->norig || vv == Q2_NO_VALUE) continue;
        if (cur != NULL && cur[rr] != Q2_NO_VALUE) e_exit_store(f, cur[rr], rr);
        else e_exit_store(f, vv, rr);
      }
      fprintf(f, "          if (qjs_aot_poll_fl(ctx) < 0) return JS_EXCEPTION;\n");
    }
    fprintf(f, "      }\n");
  }
  fprintf(f, "      goto s_%d;\n", fn->blocks[sb].start_pc);
}

/* A closure-cell LOAD in the spec body: `get_var_ref` (rop 18, "ru"). The cell is
   heap and owns its value, so this BORROWS it -- no dup, no free (DRC also parks
   any free until a safepoint). The pre-safepoint sync still applies: a TAG local that survives a later
   poll or op call is synced to the frame at that point, like every other TAG local.
   A typed site checks the TAG directly rather than calling qjs_aot_to_num, because
   on an object that can run JS (valueOf) and allocate in the middle of the body --
   which would need the sync that is not yet done mid-block. A tag that does not
   match the site's representation is a DEOPT exit at the load's own pc, not a
   conversion: the cell may legitimately hold anything profiling did not see.
   JS_TAG_UNINITIALIZED (TDZ) exits too, so the generic body raises the
   ReferenceError the language requires. */
static int emit_typed_var_ref_load(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur,
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
     emitted exactly once. The snapshot is from before the load. */
  e_note_stub(
      g_e_vx, g_e_vx_cur, g_e_vx_blk, &g_e_vx_n, in->pc, cur, NULL);
  {
    char ex[64];
    snprintf(ex, sizeof(ex), "qjs_aot_get_var_ref(f, %d)", slot);
    e_note_tag('v', in->pc, ex);
  }
  if (t->lb[d] != E_LB_TAG && e_def_dead(in, d)) {
    /* nothing reads it, and a closure-cell read cannot throw: no load, no guard */
  } else if (t->lb[d] == E_LB_I32) {
    e_sweep_guard(f, "vx", in->pc);
    fprintf(
        f,
        "    { JSValue _c = qjs_aot_get_var_ref(f, %d);\n"
        "      if (__builtin_expect(!Q2_I32_OK(_c, i%u), 0)) "
        "goto vx_%d; }\n",
        slot, (unsigned)d, in->pc);
  } else if (t->lb[d] == E_LB_F64) {
    e_sweep_guard(f, "vx", in->pc);
    fprintf(
        f,
        "    { JSValue _c = qjs_aot_get_var_ref(f, %d);\n"
        "      int _tg = JS_VALUE_GET_TAG(_c);\n"
        "      if (_tg == JS_TAG_INT) d%u = (double)JS_VALUE_GET_INT(_c);\n"
        "      else if (_tg == JS_TAG_FLOAT64) d%u = JS_VALUE_GET_FLOAT64(_c);\n"
        "      else goto vx_%d; }\n",
        slot, (unsigned)d, (unsigned)d, in->pc);
  } else if (t->lb[d] == E_LB_BOOL) {
    e_sweep_guard(f, "vx", in->pc);
    fprintf(
        f,
        "    { JSValue _c = qjs_aot_get_var_ref(f, %d);\n"
        "      if (__builtin_expect(JS_VALUE_GET_TAG(_c) != JS_TAG_BOOL, 0)) "
        "goto vx_%d;\n"
        "      b%u = JS_VALUE_GET_BOOL(_c); }\n",
        slot, in->pc, (unsigned)d);
  } else {
    /* TAG: borrow the cell's value into a TAG local. No dup (the cell owns it),
       and the pre-safepoint sync writes it to the frame before any later safepoint. */
    fprintf(f, "    v%u = qjs_aot_get_var_ref(f, %d);\n", (unsigned)d, slot);
  }
  if (cur != NULL && ld >= 0 && ld < fn->nregs) cur[ld] = d;
  return 1;
}

/* Everything an op call must have in the frame before the engine's handler
   runs: the live TAG values (synced to the frame), the typed source operands, the typed values of
   the registers the op may rewrite, and a call-shaped op's argument window. */
static void e_emit_presync(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t,
    Q2ValueId *cur) {
  int r;
  /* 1. Sync every live TAG value to the frame before the call. */
  if (!g_e_presynced)
    fprintf(
        f,
        "    /* sync live TAG values to the frame before the op call (a safepoint) "
        "*/\n");
  for (r = 0; !g_e_presynced && r < fn->norig; r++) {
    Q2ValueId v;
    if (cur == NULL) break;
    v = cur[r];
    if (v == Q2_NO_VALUE || v == E_UNDEF) continue;
    if (v >= (Q2ValueId)t->nvalues) continue;
    if (t->lb[v] != E_LB_TAG) continue;
    fprintf(f, "    AOT_SET_B(ctx, fr, %d, v%u);\n", r * 16, (unsigned)v);
  }

  /* 2. Typed operands to the frame (the handler reads the register file). The
     operands are the IR's decoded register operands, each with the SSA value it
     holds here: the role table's use list is not authoritative (it gives
     put_local_xfer_s1 a destination and no source), so a typed source named only
     by the table never reached the frame. */
  for (r = 0; !g_e_presynced && r < in->narg; r++) {
    Q2ValueId v = in->argval[r];
    int is_def = 0, d, reg;
    if (in->arg[r].kind != 'r' || v == Q2_NO_VALUE || v >= (Q2ValueId)t->nvalues)
      continue;
    reg = E_ROFF(in->arg[r]);
    if (reg < 0 || reg >= fn->nregs * 16) continue;
    /* A source is the value the register HOLDS here; an operand holding a value
       the op itself defines is its destination. (A read-modify-write operand,
       like post_inc's counter, is named twice: once as the source it holds, once
       as the destination.) */
    for (d = 0; d < in->ndef; d++)
      if (in->def[d] == v) is_def = 1;
    if (is_def) continue;
    if (t->lb[v] == E_LB_I32)
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, JS_MKVAL(JS_TAG_INT, i%u));\n", reg,
              (unsigned)v);
    else if (t->lb[v] == E_LB_F64)
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, E_BOX_F64(d%u));\n", reg,
              (unsigned)v);
    else if (t->lb[v] == E_LB_BOOL)
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, JS_MKVAL(JS_TAG_BOOL, b%u));\n", reg,
              (unsigned)v);
  }

  /* An op whose roles the IR does not know may rewrite any register live after
     it, and the reload below takes every one of them from the frame. A register
     it does NOT in fact write must still hold its value there: a typed local
     (TAG ones went with the sync above) is boxed into its register first. */
  for (r = 0; !g_e_presynced && r < in->nredef; r++) {
    int reg = in->redef_reg[r];
    Q2ValueId v;
    if (cur == NULL || reg < 0 || reg >= fn->nregs) continue;
    v = cur[reg];
    if (v == Q2_NO_VALUE || v == E_UNDEF || v >= (Q2ValueId)t->nvalues) continue;
    if (t->lb[v] == E_LB_I32)
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, JS_MKVAL(JS_TAG_INT, i%u));\n",
              reg * 16, (unsigned)v);
    else if (t->lb[v] == E_LB_F64)
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, E_BOX_F64(d%u));\n", reg * 16,
              (unsigned)v);
    else if (t->lb[v] == E_LB_BOOL)
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, JS_MKVAL(JS_TAG_BOOL, b%u));\n",
              reg * 16, (unsigned)v);
  }

  /* The argument window of a call-shaped op: the registers it reads that no
     operand names. TAG values were written by the sync above; typed ones are
     boxed here. */
  for (r = 0; !g_e_presynced && in->win_n > 0 && r < in->win_n; r++) {
    Q2ValueId v;
    int wr = in->win_reg + r;
    if (cur == NULL || wr < 0 || wr >= fn->nregs) continue;
    v = cur[wr];
    if (v == Q2_NO_VALUE || v == E_UNDEF || v >= (Q2ValueId)t->nvalues) continue;
    if (t->lb[v] == E_LB_I32)
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, JS_MKVAL(JS_TAG_INT, i%u));\n",
              wr * 16, (unsigned)v);
    else if (t->lb[v] == E_LB_F64)
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, E_BOX_F64(d%u));\n", wr * 16,
              (unsigned)v);
    else if (t->lb[v] == E_LB_BOOL)
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, JS_MKVAL(JS_TAG_BOOL, b%u));\n",
              wr * 16, (unsigned)v);
  }

}

/* THE SPEC-BODY OP CALL. The op-call contract in generated code.
   The native emitters call it on their miss path, so that path is a thin wrapper.

   An op call is a SAFEPOINT, so the order is fixed:
     1. Sync: every live TAG value in cur[] is written to the frame FIRST. The call
        can run JS, allocate and collect, and a TAG value held only in a C local is
        invisible to the GC.
     2. Store the typed operands to the frame, since the handler reads its arguments
        from the register file.
     3. `ip = qjs_aot_step(code + pc, fr, f, ctx)` -- the generic body's own slow path,
        which already decodes every instruction and handles every type.
     4. The three sentinels, and the control-transfer test: an ip that is NOT
        `code + next` means a handler took an exception or the op jumped, which is the
        try rule. The frame is already synced, so that is an ordinary exit.
     5. Reload ONLY what the role may have written, BY VALUE ID, each with the tag
        guard at its definition: the handler returned a JSValue, so a def the analysis
        typed i32/f64/bool is not safe to read blindly. A mismatch is a deopt exit at
        the NEXT pc, because the op has run and its result is in the frame.
   Nothing is forwarded across the call. */
/* The speculative body keeps `ta_ok`/`ta_sh`: the shape of a typed array whose `length` read
   was checked in full, valid while nothing has run that could reshape an object or redefine
   `length`. It is cleared after every instruction that might (e_ta_keeps decides) and in
   the miss arm of every op call. Set by an emitter whose inline path runs no JS and whose
   miss goes through emit_typed_opcall. */
static int g_e_ta_used, g_e_ta_keep;

static void e_boxed_operand(char *buf, size_t n, const ETypes *t, Q2ValueId v);

static int emit_typed_opcall(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur,
    const uint64_t *live, char hex[33], int fi, int b);

/* The step of a for-of over a dense array, before the instruction's op call: when the iterator is an
   Array Iterator over a dense array whose `next` is the builtin, the element is read in place and the
   index advanced (q2_iter_next_fast), and the window registers the step writes take their new values
   in their locals without the frame or the step. Returns 1 when the test was written; the caller
   writes the op call after it and the label `it_<pc>` after that. */
static int e_emit_iter_fast(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, const Q2ValueId *cur) {
  int w, wr, r;
  char ie[48], ne[48], old[16][48];
  if (in->rop != ROP_iter_next) return 0;
  if (in->narg < 1 || in->nredef < 1 || in->nredef > 16 || cur == NULL) return 0;
  w = E_ROFF(in->arg[0]);
  wr = w / 16;
  if (w % 16 != 0 || wr < 0 || wr + 3 >= fn->nregs) return 0;
  /* The step writes the value and `done` registers (and clears the iterator slot at the end, which
     this path never reaches); the IR also lists the other live registers as redefined, and they keep
     the value they have. */
  for (r = 0; r < in->nredef; r++) {
    int rr = in->redef_reg[r];
    Q2ValueId nv = in->redef_val[r], cv;
    if (nv == Q2_NO_VALUE || nv >= (Q2ValueId)t->nvalues || t->lb[nv] != E_LB_TAG) return 0;
    if (rr == wr + 2 || rr == wr + 3) continue;
    if (rr < 0 || rr >= fn->nregs) return 0;
    cv = cur[rr];
    if (cv == E_INFRAME)
      snprintf(old[r], sizeof(old[r]), "AOT_REG(fr, %d)", rr * 16);
    else if (cv != Q2_NO_VALUE && cv != E_UNDEF && cv < (Q2ValueId)t->nvalues && t->lb[cv] == E_LB_TAG)
      snprintf(old[r], sizeof(old[r]), "v%u", (unsigned)cv);
    else
      return 0;
  }
  for (r = 0; r < 2; r++) {
    Q2ValueId cv = cur[wr + r];
    char *dst = r == 0 ? ie : ne;
    if (cv == E_INFRAME)
      snprintf(dst, 48, "AOT_REG(fr, %d)", (wr + r) * 16);
    else if (cv != Q2_NO_VALUE && cv != E_UNDEF && cv < (Q2ValueId)t->nvalues && t->lb[cv] == E_LB_TAG)
      snprintf(dst, 48, "v%u", (unsigned)cv);
    else
      return 0;
  }
  fprintf(f, "    { JSValue _ev;\n      if (q2_iter_next_fast(%s, %s, &_ev)) {\n", ie, ne);
  for (r = 0; r < in->nredef; r++) {
    int rr = in->redef_reg[r];
    fprintf(
        f, "        v%u = %s;\n", (unsigned)in->redef_val[r],
        rr == wr + 2 ? "_ev" : rr == wr + 3 ? "JS_FALSE" : old[r]);
  }
  fprintf(f, "        goto it_%d;\n      } }\n", in->pc);
  return 1;
}

static int emit_typed_opcall(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur,
    const uint64_t *live, char hex[33], int fi, int b) {
  const ERole *role = e_roles[in->rop];
  int r, any, iter_fast = 0;

  /* A copied callee has no step engine behind it. */
  if (in->own > 0) {
    char nm[64];
    e_rop_name(in->rop, nm, sizeof(nm));
    fprintf(stderr, "qjsaot: copied callee instruction %s at pc %d (rop %u, orig pc %d) needs the step engine (in %s, copy %d)\n",
            nm, in->pc, (unsigned)in->rop, (int)in->opc, fn->g->name, (int)in->own);
    abort();
  }
  /* The CALLER decides when to use this: a native spec emitter calls it on its
     miss path, and the dispatcher calls it for any instruction no emitter lowers.
     So the tier test lives there, not here -- one copy of the op-call sequence. */
  if (role == NULL) return 0;
  if (role->control != E_CF_NONE) return 0;
  /* An op that may write ANY register cannot be reloaded selectively here; the caller
     leaves it to the generic body. (The reload loop below handles the known set.) */
  (void)hex;
  (void)fi;
  (void)b;

  /* A frameless body has no handler to step. Its callers use this as the miss arm
     of an access that has a fast path, so the miss is an exit before the
     instruction (the generic body redoes it). An instruction that has NO fast path
     never gets here: the dispatcher declines to wrap it and the function is not
     frameless. */
  if (g_e_fl) {
    if (g_e_fl_longloop && !g_e_fl_miss_ok && g_e_loop_blk != NULL &&
        g_e_blk >= 0 && g_e_loop_blk[g_e_blk])
      e_fl_block("a miss-exit access in a loop", in);
    e_note_stub(
        g_e_vx, g_e_vx_cur, g_e_vx_blk, &g_e_vx_n, in->pc, cur, NULL);
    /* The hit's typed guard exits at the next pc with the raw result in the
       register array, exactly as after a wrapper. */
    e_note_stub(
        g_e_dx, g_e_dx_cur, g_e_dx_blk, &g_e_dx_n, in->next, cur, in);
    fprintf(f, "        goto vx_%d;\n", in->pc);
    return 1;
  }

  iter_fast = e_emit_iter_fast(f, fn, in, t, cur);
  e_emit_presync(f, fn, in, t, cur);
  (void)live;
  if (g_e_ta_used) fprintf(f, "    ta_ok = 0;\n");

  /* 3. The call, and 4. the sentinels plus the control-transfer test. */
  /* The spec body has no `code`/`ip` locals -- those names belong to the generic
     body -- so the wrapper brings its own. */
  fprintf(
      f,
      "    { uint8_t *code_ = q2_code(f), *ip_;\n"
      "    ip_ = %s(code_ + %d, fr, f, ctx);\n",
      in->rop == ROP_iter_next ? "qjs_aot_iter_next"
      : in->rop == ROP_for_in_next ? "qjs_aot_for_in_next"
                                   : "qjs_aot_step",
      in->pc);
  fprintf(
      f,
      "    if (__builtin_expect(ip_ == NULL, 0)) return qjs_aot_throw_here(ctx, f, UINT32_MAX);\n");
  fprintf(
      f,
      "    if (__builtin_expect(ip_ == QJS_AOT_COLD_RET || "
      "ip_ == QJS_AOT_SUSPEND, 0)) return qjs_aot_ret(f);\n");
  /* A handler that caught the exception (or an op that jumped) returns an ip
     that is not the next instruction. In the SPEC body this must be an ordinary exit
     (the spec body has no generic labels): there is no `dispatch` label in aot_f_<hex> -- that is the generic
     body's -- so the earlier version compiled only because nothing emitted this yet.
     No further sync is needed: the sync ran before the call and the op wrote its own
     results to the frame. */
  fprintf(f, "    if (__builtin_expect(ip_ != code_ + %d, 0)) {\n", in->next);
  fprintf(f, "        E_HANDOFF_NOTE(%d);\n", fi);
  e_emit_ret_g(f, "        ", hex, "(uint32_t)(ip_ - code_)");
  fprintf(f, "    } }\n");

  /* 5. Reload the defs the role says it wrote, by value id, with the tag guard. */
  /* The guard exits land at the NEXT pc: the op has run, so its result is already in
     the frame and the generic body resumes after it. */
  e_note_stub(
      g_e_dx, g_e_dx_cur, g_e_dx_blk, &g_e_dx_n, in->next, cur, in);
  any = 0;
  for (r = 0; r < in->ndef; r++) {
    Q2ValueId v = in->def[r];
    int reg;
    if (v == Q2_NO_VALUE || v >= (Q2ValueId)t->nvalues) continue;
    reg = fn->values[v].reg;
    if (reg < 0 || reg >= fn->nregs) continue;
    /* Nothing reads it: the op left it in the frame, and that is enough. */
    if (t->lb[v] != E_LB_TAG && e_def_dead(in, v)) continue;
    switch (t->lb[v]) {
      case E_LB_I32:
        e_sweep_guard(f, "dx", in->next);
        fprintf(
            f,
            "    { JSValue _r = AOT_REG(fr, %d);\n"
            "      if (__builtin_expect(!Q2_I32_OK(_r, i%u), 0)) "
            "goto dx_%d; }\n",
            reg * 16, (unsigned)v, in->next);
        any = 1;
        break;
      case E_LB_F64:
        e_sweep_guard(f, "dx", in->next);
        fprintf(
            f,
            "    { JSValue _r = AOT_REG(fr, %d); int _tg = JS_VALUE_GET_TAG(_r);\n"
            "      if (_tg == JS_TAG_INT) d%u = (double)JS_VALUE_GET_INT(_r);\n"
            "      else if (_tg == JS_TAG_FLOAT64) d%u = JS_VALUE_GET_FLOAT64(_r);\n"
            "      else goto dx_%d; }\n",
            reg * 16, (unsigned)v, (unsigned)v, in->next);
        any = 1;
        break;
      case E_LB_BOOL:
        e_sweep_guard(f, "dx", in->next);
        fprintf(
            f,
            "    { JSValue _r = AOT_REG(fr, %d);\n"
            "      if (__builtin_expect(JS_VALUE_GET_TAG(_r) != JS_TAG_BOOL, 0)) "
            "goto dx_%d;\n"
            "      b%u = JS_VALUE_GET_BOOL(_r); }\n",
            reg * 16, in->next, (unsigned)v);
        any = 1;
        break;
      case E_LB_TAG:
        fprintf(f, "    v%u = AOT_REG(fr, %d);\n", (unsigned)v, reg * 16);
        any = 1;
        break;
      default:
        break;
    }
  }
  /* The registers the op rewrote beyond its defs (the fallback's live set): each
     becomes a TAG local again from the frame. */
  for (r = 0; r < in->nredef; r++) {
    Q2ValueId v = in->redef_val[r];
    int reg = in->redef_reg[r];
    if (v == Q2_NO_VALUE || v >= (Q2ValueId)t->nvalues) continue;
    if (reg < 0 || reg >= fn->nregs || t->lb[v] != E_LB_TAG) continue;
    fprintf(f, "    v%u = AOT_REG(fr, %d);\n", (unsigned)v, reg * 16);
    any = 1;
  }
  /* The defs are in their locals now; cur[] is NOT updated here. The dispatcher's
     e_note_defs does that once the whole instruction is lowered, so an emitter
     with two miss paths gives the second one the pre-instruction state. */
  if (any) fprintf(f, "    /* op call done, defs reloaded with their guards */\n");
  e_emit_group_recheck(f, fn, in, cur);
  if (iter_fast) fprintf(f, "    it_%d: ;\n", in->pc);
  g_e_opcalls++;
  return 1;
}

/* The CALL family: `call`, `call_method`, `construct`, `tail_call`, the apply forms
   and anything else that runs JS through a callee frame. `call` and `call_method`
   have their own site emitter, tail calls and `eval` leave the function, and
   `construct` and `apply_arguments` are the two the wrapper may run (see
   `e_wrappable`). */
static int e_is_call_family(uint16_t rop) {
  switch (rop) {
    case ROP_call:
    case ROP_call_method:
    case ROP_construct:      /* `new`: a callee frame like the calls */
    case ROP_tail_call:
    case ROP_tail_call_method:
    case ROP_apply:
    case ROP_apply_arguments:
    case ROP_eval:           /* runs arbitrary JS in a fresh frame */
      return 1;
    default:
      return 0;
  }
}

/* Whether the op-call wrapper may run this instruction when no typed emitter took
   it: the role says it can be called (control NONE), it is not a call (those have
   their own site emitter), the facts did not require its miss to be an exit, and
   every register it writes is one the wrapper reloads (its defs and the IR's
   redefined set). A jump the op takes at run time is the wrapper's own `ip` test. */
/* The transfer stores move their source operand's reference into the container
   and empty the register; that is the whole of their "writes registers". The
   operand's register offset, or -1 for any other op. */
static int e_xfer_src_off(const Q2Insn *in) {
  int idx;
  switch (in->rop) {
    case ROP_put_arg_xfer: idx = 1; break;
    case ROP_put_element_xfer: idx = 2; break;
    case ROP_put_field_xfer: idx = 2; break;
    default: return -1;
  }
  if (idx >= in->narg || in->arg[idx].kind != 'r') return -1;
  return E_ROFF(in->arg[idx]);
}

static int e_wrappable(const Q2Func *fn, const Q2Insn *in, const ERole *role) {
  if (g_e_fl) return 0; /* a frameless body has no handler to step */
  if (role == NULL || role->control != E_CF_NONE) return 0;

  /* The call family has its own site emitter for call/call_method; construct and
     apply_arguments run synchronously through the step (the callee's frame is
     the engine's), so the wrapper does them; tail calls, eval and the rest of
     the family leave the function. */
  if (e_is_call_family(in->rop) && in->rop != ROP_construct &&
      in->rop != ROP_apply_arguments)
    return 0;
  if (e_must_exit(fn, in)) return 0;
  if (role->writes_regs && in->nredef == 0 && e_xfer_src_off(in) < 0) return 0;
  return 1;
}

/* A conditional terminator through the engine's own handler, deciding which way
   it went. The caller has an `int c_` in scope; this sets it to 1 when the step
   left `ip` at the branch target (successor 0) and 0 at the fall-through
   (successor 1), XORed with `inv` so a caller that tests `c_` against an
   inverted sense gets its own convention. Anything else -- an exception handler's
   address -- hands off at that ip, as the op-call wrapper does. The live values
   and the typed operands are written to the frame first. A terminator that
   defines nothing writes no register, so the locals stay as they are and both
   edges can take them as they are. */
static void e_emit_term_step(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t,
    const Q2Block *blk, Q2ValueId *cur, char hex[33], int fi, int inv) {
  if (!g_e_presynced) e_emit_presync(f, fn, in, t, cur);
  fprintf(
      f,
      "    { uint8_t *code_ = q2_code(f), *ip_;\n"
      "    ip_ = qjs_aot_step(code_ + %d, fr, f, ctx);\n"
      "    if (__builtin_expect(ip_ == NULL, 0)) return qjs_aot_throw_here(ctx, f, UINT32_MAX);\n"
      "    if (__builtin_expect(ip_ == QJS_AOT_COLD_RET || "
      "ip_ == QJS_AOT_SUSPEND, 0)) return qjs_aot_ret(f);\n"
      "    if (ip_ == code_ + %d) c_ = %d;\n"
      "    else if (ip_ == code_ + %d) c_ = %d;\n"
      "    else {\n"
      "        E_HANDOFF_NOTE(%d);\n",
      in->pc, blk->succ_pc[0], inv ? 0 : 1, blk->succ_pc[1], inv ? 1 : 0, fi);
  e_emit_ret_g(f, "        ", hex, "(uint32_t)(ip_ - code_)");
  fprintf(f, "    } }\n");
  /* the step may have run code: the cache cells the body assumes are valid only while the epoch holds */
  if (g_e_epoch_skip) {
    fprintf(f, "    if (__builtin_expect(Q2_EPOCH_MOVED(ctx), 0)) {\n        E_HANDOFF_NOTE(%d);\n", fi);
    {
      char tgt[96];
      snprintf(tgt, sizeof(tgt), "(uint32_t)((c_ ^ %d) ? %d : %d)", inv ? 1 : 0, blk->succ_pc[0], blk->succ_pc[1]);
      e_emit_ret_g(f, "        ", hex, tgt);
    }
    fprintf(f, "    }\n");
  }
}

/* Whether a terminator can run through the wrapper: a two-way branch that defines
   nothing, whose both successors are blocks of this function. */
static int e_term_wrappable(
    const Q2Func *fn, const Q2Insn *in, const ERole *role, const Q2Block *blk) {
  if (g_e_fl) return 0; /* a frameless body has no handler to step */
  if (role == NULL || role->control != E_CF_BRANCH) return 0;
  if (in->ndef != 0 || in->nredef != 0 || e_is_call_family(in->rop)) return 0;
  if (blk->nsucc < 2) return 0;
  if (blk->succ_block[0] < 0 || blk->succ_block[0] >= fn->nblocks) return 0;
  if (blk->succ_block[1] < 0 || blk->succ_block[1] >= fn->nblocks) return 0;
  return 1;
}

/* A terminator no typed emitter took (a truthiness test on a TAG value, a compare
   the typed forms decline): the whole branch runs through the step and both edges
   are the ordinary arms. Counts as an op call, not a hand-off. */
static int emit_term_wrapper(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, int b,
    Q2ValueId *cur, char hex[33], int fi) {
  const ERole *role = e_roles[in->rop];
  const Q2Block *blk = &fn->blocks[b];
  if (t == NULL || !e_term_wrappable(fn, in, role, blk)) return 0;
  fprintf(f, "    { int c_;\n");
  e_emit_term_step(f, fn, in, t, blk, cur, hex, fi, 0);
  g_e_opcalls++;
  fprintf(f, "    if (c_) {\n");
  e_emit_arm(f, fn, b, 0, cur, hex, blk->succ_block[0]);
  fprintf(f, "    } else {\n");
  e_emit_arm(f, fn, b, 1, cur, hex, blk->succ_block[1]);
  fprintf(f, "    }\n    }\n");
  return 1;
}

/* ---- fused compare-and-branch on a local slot, and the local read-modify-write
   branch -------------------------------------------------------------
   `branch_cmp_loc_reg` / `reg_loc` / `loc_loc` compare a variable slot (frame memory)
   with a register or another slot; `local_rmw_branch` updates a slot in place and
   branches (a loop's `i++`/`i--` and back edge). The int and double cases are inline;
   anything else runs the instruction through the step and takes the edge it chose. */
static void e_loc_operand(
    char *out, size_t n, const Q2Func *fn, const ETypes *t, const Q2Insn *in,
    int arg_idx, int use_idx) {
  if (in->arg[arg_idx].kind == 'u') {
    snprintf(out, n, "q2_var_buf(f)[%d]", (int)in->arg[arg_idx].val);
  } else {
    Q2ValueId v = in->use[use_idx];
    if (v < (Q2ValueId)t->nvalues && t->lb[v] == E_LB_I32)
      snprintf(out, n, "JS_MKVAL(JS_TAG_INT, i%u)", (unsigned)v);
    else if (v < (Q2ValueId)t->nvalues && t->lb[v] == E_LB_F64)
      snprintf(out, n, "E_BOX_F64(d%u)", (unsigned)v);
    else if (v < (Q2ValueId)t->nvalues && t->lb[v] == E_LB_BOOL)
      snprintf(out, n, "JS_MKVAL(JS_TAG_BOOL, b%u)", (unsigned)v);
    else
      snprintf(out, n, "v%u", (unsigned)v);
  }
  (void)fn;
}

static int emit_typed_branch_loc(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, int b,
    Q2ValueId *cur, int have_types, char hex[33], int fi) {
  const ERole *role = e_roles[in->rop];
  const Q2Block *blk = &fn->blocks[b];
  int sub, inv, base, ai, bi, ua = 0;
  char ea[96], eb[96];
  const char *cop;

  if (in->rop != ROP_branch_cmp_loc_reg && in->rop != ROP_branch_cmp_reg_loc &&
      in->rop != ROP_branch_cmp_loc_loc)
    return 0;
  if (!have_types || t == NULL || g_e_fl) return 0;
  if (role == NULL || role->control != E_CF_BRANCH || in->narg < 4) return 0;
  if (in->arg[0].kind != 'b' || blk->nsucc < 2 || in->ndef != 0 || in->nredef != 0) return 0;
  if (blk->succ_block[0] < 0 || blk->succ_block[0] >= fn->nblocks ||
      blk->succ_block[1] < 0 || blk->succ_block[1] >= fn->nblocks)
    return 0;
  sub = (int)in->arg[0].val;
  inv = (sub & REG_CMP_INVERT) != 0;
  base = sub & ~REG_CMP_INVERT;
  if (!(base >= REG_BIN_LT && base <= REG_BIN_GTE) && base != REG_BIN_EQ &&
      base != REG_BIN_NEQ && base != REG_BIN_STRICT_EQ && base != REG_BIN_STRICT_NEQ)
    return 0;
  ai = 1;
  bi = 2;
  if (in->rop == ROP_branch_cmp_loc_reg || in->rop == ROP_branch_cmp_reg_loc) {
    if (in->nuse < 1) return 0;
  }
  e_loc_operand(ea, sizeof(ea), fn, t, in, ai, 0);
  e_loc_operand(eb, sizeof(eb), fn, t, in, bi, 0);
  (void)ua;
  cop = base == REG_BIN_LT    ? "<"
        : base == REG_BIN_LTE ? "<="
        : base == REG_BIN_GT  ? ">"
        : base == REG_BIN_GTE ? ">="
        : (base == REG_BIN_NEQ || base == REG_BIN_STRICT_NEQ) ? "!="
                                                               : "==";
  fprintf(f, "    { int c_; JSValue _a = %s, _b = %s;\n", ea, eb);
  fprintf(
      f,
      "    if (__builtin_expect(JS_VALUE_IS_BOTH_INT(_a, _b), 1))\n"
      "        c_ = (JS_VALUE_GET_INT(_a) %s JS_VALUE_GET_INT(_b)) ^ %d;\n"
      "    else if ((JS_VALUE_GET_TAG(_a) == JS_TAG_INT || JS_VALUE_GET_TAG(_a) == JS_TAG_FLOAT64) &&\n"
      "             (JS_VALUE_GET_TAG(_b) == JS_TAG_INT || JS_VALUE_GET_TAG(_b) == JS_TAG_FLOAT64))\n"
      "        c_ = ((JS_VALUE_GET_TAG(_a) == JS_TAG_INT ? (double)JS_VALUE_GET_INT(_a) : JS_VALUE_GET_FLOAT64(_a)) %s\n"
      "              (JS_VALUE_GET_TAG(_b) == JS_TAG_INT ? (double)JS_VALUE_GET_INT(_b) : JS_VALUE_GET_FLOAT64(_b))) ^ %d;\n"
      "    else {\n",
      cop, inv, cop, inv);
  e_emit_term_step(f, fn, in, t, blk, cur, hex, fi, 0);
  fprintf(f, "    }\n    if (c_) {\n");
  e_emit_arm(f, fn, b, 0, cur, hex, blk->succ_block[0]);
  fprintf(f, "    } else {\n");
  e_emit_arm(f, fn, b, 1, cur, hex, blk->succ_block[1]);
  fprintf(f, "    }\n    }\n");
  g_e_opcalls += 0;
  return 1;
}

static int emit_typed_local_rmw_branch(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, int b,
    Q2ValueId *cur, int have_types, char hex[33], int fi) {
  const ERole *role = e_roles[in->rop];
  const Q2Block *blk = &fn->blocks[b];
  int sub, slot, imm;
  const char *expr;

  if (in->rop != ROP_local_rmw_branch || !have_types || t == NULL || g_e_fl) return 0;
  if (role == NULL || role->control != E_CF_BRANCH || in->narg < 4) return 0;
  if (in->arg[0].kind != 'b' || in->arg[1].kind != 'u' || blk->nsucc < 2) return 0;
  if (in->ndef != 0 || in->nredef != 0) return 0;
  if (blk->succ_block[0] < 0 || blk->succ_block[0] >= fn->nblocks ||
      blk->succ_block[1] < 0 || blk->succ_block[1] >= fn->nblocks)
    return 0;
  sub = (int)in->arg[0].val;
  slot = (int)in->arg[1].val;
  imm = (int)in->arg[2].val;
  if (sub & REG_RMW_UNARY) {
    int u = sub & ~REG_RMW_UNARY;
    if (u == REG_UN_INC) expr = "__builtin_add_overflow(_iv, 1, &_nv)";
    else if (u == REG_UN_DEC) expr = "__builtin_sub_overflow(_iv, 1, &_nv)";
    else return 0;
  } else if (sub == REG_BIN_ADD) {
    expr = "__builtin_add_overflow(_iv, _imm, &_nv)";
  } else if (sub == REG_BIN_SUB) {
    expr = "__builtin_sub_overflow(_iv, _imm, &_nv)";
  } else {
    return 0;
  }
  fprintf(
      f,
      "    { int c_; int32_t _imm = %d, _iv, _nv; JSValue *_vb = q2_var_buf(f);\n"
      "    (void)_imm;\n"
      "    if (__builtin_expect(JS_VALUE_GET_TAG(_vb[%d]) == JS_TAG_INT, 1) &&\n"
      "        (_iv = JS_VALUE_GET_INT(_vb[%d]), !%s)) {\n"
      "        _vb[%d] = JS_MKVAL(JS_TAG_INT, _nv);\n"
      "        c_ = 1;\n"
      "    } else {\n",
      imm, slot, slot, expr, slot);
  e_emit_term_step(f, fn, in, t, blk, cur, hex, fi, 0);
  fprintf(f, "    }\n    if (c_) {\n");
  e_emit_arm(f, fn, b, 0, cur, hex, blk->succ_block[0]);
  fprintf(f, "    } else {\n");
  e_emit_arm(f, fn, b, 1, cur, hex, blk->succ_block[1]);
  fprintf(f, "    }\n    }\n");
  return 1;
}

/* `div` / `mod` ("rrr": dest, a, b) as typed arithmetic. The f64 form needs no
   exit: a double division cannot trap, and `1/0` is Infinity exactly as JS says.
   `mod` on doubles is `fmod`, which IS JavaScript's `%` for doubles (the sign
   follows the dividend). An i32 result declines to the wrapper: JS integer
   division can produce a fraction, and `x % 0` is NaN, so there is no int32 form
   that is correct in general. */
static int emit_typed_divmod(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur,
    int have_types, char hex[33], int fi, int b,
    int (*opcall)(FILE *, const Q2Func *, const Q2Insn *, const ETypes *,
                  Q2ValueId *, const uint64_t *, char[33], int, int)) {
  Q2ValueId dv, av, bv;
  int ld;

  if (!have_types || t == NULL) return 0;
  if (in->rop != ROP_div && in->rop != ROP_mod) return 0;
  if (in->ndef < 1 || in->nuse < 2) return 0;
  dv = in->def[0];
  av = in->use[0];
  bv = in->use[1];
  if (dv >= (Q2ValueId)t->nvalues || av >= (Q2ValueId)t->nvalues ||
      bv >= (Q2ValueId)t->nvalues)
    return 0;
  /* f64 only: both operands and the result must be doubles. */
  if (t->lb[dv] != E_LB_F64 || t->lb[av] != E_LB_F64 ||
      t->lb[bv] != E_LB_F64)
    return 0;
  ld = fn->values[dv].reg;
  if (ld < 0 || ld >= fn->nregs) return 0;
  if (in->rop == ROP_div) {
    fprintf(f, "    d%u = d%u / d%u;\n", (unsigned)dv, (unsigned)av, (unsigned)bv);
  } else {
    /* fmod is JS `%` for doubles. It is in math.h, which the unit prelude already
       includes for fabs/floor. */
    fprintf(f, "    d%u = fmod(d%u, d%u);\n", (unsigned)dv, (unsigned)av, (unsigned)bv);
  }
  if (cur != NULL) cur[ld] = dv;
  (void)opcall;
  (void)hex;
  (void)fi;
  (void)b;
  return 1;
}

/* `load_const` ("rbc": d, kind, cpool index). A NUMERIC pool entry is a constant,
   not an op call: decode it with the same accessors `binary_cpool` uses and load it
   straight into the typed local -- an int entry into an i32 local, a double into an
   f64 local with an exact `%a` literal. Anything else (a string, an object, a
   non-finite double) declines to the wrapper. */
static int emit_typed_load_const(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur,
    int have_types) {
  Q2ValueId dv;
  double c;
  uint32_t idx;
  int tag, ld;

  if (!have_types || t == NULL) return 0;
  if (in->rop != ROP_load_const) return 0;
  if (in->ndef < 1 || in->narg < 3) return 0;
  if (in->arg[2].kind != 'c') return 0;  /* "rbc": the pool index */
  idx = (uint32_t)in->arg[2].val;
  if (!qjs_aot_pool_num(fn->g, idx, &c)) return 0;
  tag = qjs_aot_pool_tag(fn->g, idx);
  if (tag != E_POOL_INT && tag != E_POOL_DOUBLE) return 0;
  dv = in->def[0];
  if (dv >= (Q2ValueId)t->nvalues) return 0;
  ld = fn->values[dv].reg;
  if (ld < 0 || ld >= fn->nregs) return 0;
  if (t->lb[dv] == E_LB_I32 && tag == E_POOL_INT) {
    fprintf(f, "    i%u = %d;\n", (unsigned)dv, (int32_t)c);
  } else if (t->lb[dv] == E_LB_F64) {
    if (tag == E_POOL_DOUBLE && !__builtin_isfinite(c)) return 0;
    fprintf(f, "    d%u = (%a);\n", (unsigned)dv, c);
  } else {
    return 0;
  }
  if (cur != NULL) cur[ld] = dv;
  return 1;
}

/* The FUSED arithmetic ops as typed arithmetic in the spec body:
   `add_mul` ("rrrrr": dm, x, y, da, c) is `t = x * y; r = c + t`, exactly as the
   engine handler does it -- TWO operations, never an fma (the unit already builds
   with -ffp-contract=off; emitting two statements keeps it that way by construction).
   `mul_add` is the same pair with the add's operands swapped; `mul_sub`/`sub_mul`
   subtract instead.

   f64: both steps are the machine's, and a product or sum of doubles cannot
   overflow, so no exit. i32: each step is a widening int64 operation with its own
   overflow deopt at this pc, and the multiply also carries mul's -0 exit -- an int32
   cannot hold -0, so `0 * -1` must leave for the generic body. */
static int emit_typed_fused(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur,
    int have_types) {
  const ERole *role = e_roles[in->rop];
  Q2ValueId dv, xv, yv, cv;
  int is_add, ld, ldm;
  /* add_mul / sub_mul: da = c <op> dm; mul_add / mul_sub: da = dm <op> c. */
  int c_first = in->rop == ROP_add_mul || in->rop == ROP_sub_mul;
  char op[2];

  if (!have_types || t == NULL) return 0;
  switch (in->rop) {
    case ROP_add_mul:
    case ROP_mul_add:
      is_add = 1;
      op[0] = '+';
      break;
    case ROP_sub_mul:
    case ROP_mul_sub:
      is_add = 0;
      op[0] = '-';
      break;
    default:
      return 0;
  }
  op[1] = 0;
  if (role == NULL || role->control != E_CF_NONE) return 0;
  if (in->ndef < 2 || in->nuse < 3) return 0;
  dv = in->def[0];   /* the product register (dm) */
  xv = in->use[0];
  yv = in->use[1];
  cv = in->use[2];
  if (dv >= (Q2ValueId)t->nvalues || xv >= (Q2ValueId)t->nvalues ||
      yv >= (Q2ValueId)t->nvalues || cv >= (Q2ValueId)t->nvalues)
    return 0;
  ldm = fn->values[dv].reg;
  /* The instruction's SECOND def is where the sum/multiply result goes. */
  {
    Q2ValueId rv = in->def[1];
    if (rv >= (Q2ValueId)t->nvalues) return 0;
    ld = fn->values[rv].reg;
    if (ld < 0 || ld >= fn->nregs || ldm < 0 || ldm >= fn->nregs) return 0;

    /* A MIXED pair is fine in the f64 form: an i32 operand converts with
       (double), exactly as the register arithmetic form does. A product or sum of
       doubles cannot overflow, so still no exit. */
    if ((t->lb[dv] == E_LB_F64 || t->lb[dv] == E_LB_I32) &&
        (t->lb[rv] == E_LB_F64 || t->lb[rv] == E_LB_I32) &&
        (t->lb[xv] == E_LB_F64 || t->lb[xv] == E_LB_I32) &&
        (t->lb[yv] == E_LB_F64 || t->lb[yv] == E_LB_I32) &&
        (t->lb[cv] == E_LB_F64 || t->lb[cv] == E_LB_I32) &&
        (t->lb[xv] == E_LB_F64 || t->lb[yv] == E_LB_F64 ||
         t->lb[cv] == E_LB_F64 || t->lb[dv] == E_LB_F64)) {
      /* Two statements, in the engine's order. */
      {
        char xb[32], yb[32], cb[32], mb[32];
        if (t->lb[xv] == E_LB_F64) snprintf(xb, sizeof(xb), "d%u", (unsigned)xv);
        else snprintf(xb, sizeof(xb), "(double)i%u", (unsigned)xv);
        if (t->lb[yv] == E_LB_F64) snprintf(yb, sizeof(yb), "d%u", (unsigned)yv);
        else snprintf(yb, sizeof(yb), "(double)i%u", (unsigned)yv);
        if (t->lb[cv] == E_LB_F64) snprintf(cb, sizeof(cb), "d%u", (unsigned)cv);
        else snprintf(cb, sizeof(cb), "(double)i%u", (unsigned)cv);
        if (t->lb[dv] == E_LB_F64) snprintf(mb, sizeof(mb), "d%u", (unsigned)dv);
        else snprintf(mb, sizeof(mb), "(double)i%u", (unsigned)dv);
        fprintf(f, "    %s = %s * %s;\n", mb, xb, yb);
        if (c_first)
          fprintf(f, "    d%u = %s %s %s;\n", (unsigned)rv, cb, op, mb);
        else
          fprintf(f, "    d%u = %s %s %s;\n", (unsigned)rv, mb, op, cb);
      }
    } else if (t->lb[xv] == E_LB_I32 && t->lb[yv] == E_LB_I32 &&
               t->lb[cv] == E_LB_I32 && t->lb[dv] == E_LB_I32 &&
               t->lb[rv] == E_LB_I32) {
      /* Step 1: the product, with the -0 exit the multiply already has. */
      {
        int ovf1 = t->exits == NULL || (t->exits[dv] & E_EX_OVF);
        int nz1 = t->exits == NULL || (t->exits[dv] & E_EX_NEGZERO);
        int ovf2 = t->exits == NULL || (t->exits[rv] & E_EX_OVF);
        if (ovf1 || nz1 || ovf2) e_sweep_guard(f, "ox", in->pc);
        fprintf(f, "    { int64_t _t = (int64_t)i%u * (int64_t)i%u;\n", (unsigned)xv, (unsigned)yv);
        if (ovf1)
          fprintf(
              f,
              "      if (__builtin_expect(_t < INT32_MIN || _t > INT32_MAX, 0)) goto ox_%d;\n",
              in->pc);
        if (nz1)
          fprintf(
              f,
              "      if (__builtin_expect(_t == 0 && (i%u < 0 || i%u < 0), 0)) goto ox_%d;\n",
              (unsigned)xv, (unsigned)yv, in->pc);
        fprintf(f, "      i%u = (int32_t)_t; }\n", (unsigned)dv);
        /* Step 2: the sum or difference. */
        fprintf(f, "    { int64_t _t = (int64_t)i%u %s (int64_t)i%u;\n",
            (unsigned)(c_first ? cv : dv), is_add ? "+" : "-", (unsigned)(c_first ? dv : cv));
        if (ovf2)
          fprintf(
              f,
              "      if (__builtin_expect(_t < INT32_MIN || _t > INT32_MAX, 0)) goto ox_%d;\n",
              in->pc);
        fprintf(f, "      i%u = (int32_t)_t; }\n", (unsigned)rv);
        if (ovf1 || nz1 || ovf2)
          e_note_stub(g_e_ox, g_e_ox_cur, g_e_ox_blk, &g_e_ox_n, in->pc, cur, NULL);
      }
    } else {
      return 0;
    }
    (void)is_add;
    (void)op;
    if (cur != NULL) {
      cur[ldm] = dv;
      cur[ld] = rv;
    }
  }
  return 1;
}

/* The fused multiply-add forms on tagged operands (a global, a field, a parameter): both halves are
   the engine's number fast path, and a pair it declines runs the instruction through the op call,
   which repeats both halves (neither has a side effect beyond its results). */
static int emit_tag_fused(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur,
    int have_types, char hex[33], int fi, int b) {
  const ERole *role = e_roles[in->rop];
  Q2ValueId dv, rv, xv, yv, cv;
  int c_first = in->rop == ROP_add_mul || in->rop == ROP_sub_mul;
  int is_add = in->rop == ROP_add_mul || in->rop == ROP_mul_add;
  int ldm, ld, k;
  char xa[64], ya[64], ca[64];

  if (!have_types || t == NULL || cur == NULL) return 0;
  if (in->rop != ROP_add_mul && in->rop != ROP_mul_add && in->rop != ROP_sub_mul &&
      in->rop != ROP_mul_sub)
    return 0;
  if (role == NULL || role->control != E_CF_NONE) return 0;
  if (in->ndef < 2 || in->nuse < 3) return 0;
  dv = in->def[0];
  rv = in->def[1];
  xv = in->use[0];
  yv = in->use[1];
  cv = in->use[2];
  if (dv >= (Q2ValueId)t->nvalues || rv >= (Q2ValueId)t->nvalues ||
      xv >= (Q2ValueId)t->nvalues || yv >= (Q2ValueId)t->nvalues ||
      cv >= (Q2ValueId)t->nvalues)
    return 0;
  if (t->lb[dv] != E_LB_TAG || t->lb[rv] != E_LB_TAG) return 0;
  {
    Q2ValueId us[3];
    us[0] = xv; us[1] = yv; us[2] = cv;
    for (k = 0; k < 3; k++)
      if (t->lb[us[k]] != E_LB_TAG && t->lb[us[k]] != E_LB_I32 && t->lb[us[k]] != E_LB_F64)
        return 0;
  }
  ldm = fn->values[dv].reg;
  ld = fn->values[rv].reg;
  if (ld < 0 || ld >= fn->nregs || ldm < 0 || ldm >= fn->nregs) return 0;
  if (e_must_exit(fn, in)) return 0;
  e_boxed_operand(xa, sizeof(xa), t, xv);
  e_boxed_operand(ya, sizeof(ya), t, yv);
  e_boxed_operand(ca, sizeof(ca), t, cv);
  fprintf(f, "    { JSValue _m, _r;\n");
  fprintf(f, "      if (__builtin_expect(q2_arith(QJS_AOT_REG_BIN_MUL, %s, %s, &_m), 1) &&\n", xa, ya);
  fprintf(
      f, "          __builtin_expect(q2_arith(%s, %s, %s, &_r), 1)) {\n",
      is_add ? "QJS_AOT_REG_BIN_ADD" : "QJS_AOT_REG_BIN_SUB", c_first ? ca : "_m",
      c_first ? "_m" : ca);
  fprintf(f, "        v%u = _m;\n        v%u = _r;\n", (unsigned)dv, (unsigned)rv);
  fprintf(f, "      } else {\n");
  if (!emit_typed_opcall(f, fn, in, t, cur, NULL, hex, fi, b)) return 0;
  fprintf(f, "      }\n    }\n");
  cur[ldm] = dv;
  cur[ld] = rv;
  return 1;
}

/* `to_propkey` ("rr": d, key) and `to_propkey2` ("rrr": d, key, object): an integer key is
   already a property key, so the result is the key itself. The two-operand form first
   tests the object for null and undefined, which is an exit before the instruction (the
   generic body raises the error). A key that is not an integer stays with the wrapper. */
static int emit_typed_propkey(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur,
    int have_types) {
  Q2ValueId dv, kv;
  int dreg;
  char dn[32], kn[32];

  if (!have_types || t == NULL || cur == NULL) return 0;
  if (in->rop != ROP_to_propkey && in->rop != ROP_to_propkey2) return 0;
  if (in->ndef < 1 || in->nuse < 1) return 0;
  dv = in->def[0];
  kv = in->use[0];
  if (dv >= (Q2ValueId)t->nvalues || kv >= (Q2ValueId)t->nvalues) return 0;
  if (t->lb[dv] != E_LB_I32 || t->lb[kv] != E_LB_I32) return 0;
  if (in->rop == ROP_to_propkey2 && in->nuse < 2) return 0;
  dreg = fn->values[dv].reg;
  if (dreg < 0 || dreg >= fn->nregs) return 0;
  if (in->rop == ROP_to_propkey2) {
    Q2ValueId ov = in->use[1];
    if (ov >= (Q2ValueId)t->nvalues) return 0;
    if (t->lb[ov] == E_LB_TAG) {
      char ex[48];
      snprintf(ex, sizeof(ex), "v%u", (unsigned)ov);
      e_note_stub(g_e_vx, g_e_vx_cur, g_e_vx_blk, &g_e_vx_n, in->pc, cur, NULL);
      e_note_tag('v', in->pc, ex);
      e_sweep_guard(f, "vx", in->pc);
      fprintf(
          f,
          "    if (__builtin_expect(JS_VALUE_GET_TAG(v%u) == JS_TAG_NULL || "
          "JS_VALUE_GET_TAG(v%u) == JS_TAG_UNDEFINED, 0)) goto vx_%d;\n",
          (unsigned)ov, (unsigned)ov, in->pc);
    }
  }
  e_local_name(dn, sizeof(dn), dv);
  e_local_name(kn, sizeof(kn), kv);
  fprintf(f, "    %s = %s;\n", dn, kn);
  cur[dreg] = dv;
  return 1;
}

/* A speculative-body ARRAY LENGTH: `get_length` ("rr": dest, receiver). The hit
   helper is `q2_array_length`, which returns a double in an int range; the result is
   an i32 local (that is the only shape the analysis uses for it). A miss goes to the
   wrapper -- `.length` on a non-array can run a getter. */
static int emit_typed_length(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur,
    int have_types, char hex[33], int fi, int b) {
  const ERole *role = e_roles[in->rop];
  Q2ValueId dv, rv;
  int dreg, rreg;

  if (!have_types || t == NULL) return 0;
  if (in->rop != ROP_get_length) return 0;
  if (role == NULL || role->control != E_CF_NONE) return 0;
  if (in->ndef < 1 || in->nuse < 1) return 0;
  dv = in->def[0];
  rv = in->use[0];
  if (dv >= (Q2ValueId)t->nvalues || rv >= (Q2ValueId)t->nvalues) return 0;
  if (t->lb[rv] != E_LB_TAG || t->lb[dv] != E_LB_I32) return 0;
  dreg = fn->values[dv].reg;
  rreg = fn->values[rv].reg;
  if (dreg < 0 || dreg >= fn->nregs || rreg < 0 || rreg >= fn->nregs) return 0;

  fprintf(f, "    { double _len;\n");
  fprintf(
      f,
      "      if (__builtin_expect(!Q2_FORCE_MISS && q2_array_length(ctx, v%u, &_len, &ta_sh, &ta_ok), 1)) {\n",
      (unsigned)rv);
  fprintf(f, "        i%u = (int32_t)_len;\n", (unsigned)dv);
  fprintf(f, "      } else {\n");
  fprintf(f, "        AOT_SET_B(ctx, fr, %d, v%u);\n", rreg * 16, (unsigned)rv);
  if (!emit_typed_opcall(f, fn, in, t, cur, NULL, hex, fi, b)) return 0;
  fprintf(f, "      }\n");
  fprintf(f, "    }\n");
  if (cur != NULL) cur[dreg] = dv;
  g_e_ta_keep = 1;
  return 1;
}

static int emit_elem_region_read(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur);
static int emit_elem_region_write(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur);
static int emit_pk_region_read(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur, int kind);
static int emit_pk_region_write(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur, int kind);

/* A speculative-body ELEMENT READ: `get_element` ("rrr": dest, array, key). The
   SPEC body does NOT open the generic body's element region -- a region caches the
   array's storage and count for a run of accesses, and a spec block has no place to
   drop it. So this uses the plain `q2_elem_hit`, which revalidates per access.
   The array must be a TAG local; an i32 key is packed for the helper, a TAG key
   declines (it would need ToPropertyKey, i.e. the wrapper). */
static int emit_typed_elem_read(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur,
    int have_types, char hex[33], int fi, int b) {
  const ERole *role = e_roles[in->rop];
  Q2ValueId dv, av, kv;
  int dreg, areg, kreg;
  char kb[48];

  if (!have_types || t == NULL) return 0;
  if (in->rop != ROP_get_element) return 0;
  if (role == NULL || role->control != E_CF_NONE || role->nuse != 2) return 0;
  if (in->ndef < 1 || in->nuse < 2) return 0;
  if (g_e_facts != NULL && g_e_facts->elemreg[e_insn_idx(fn, in)])
    return emit_elem_region_read(f, fn, in, t, cur);
  /* A packed site: the region's own entry checks the kind and its miss exits the body, so it is
     the same kind of access as a dense region's, with the element in its own type. */
  {
    int pk = 0;
    if (e_pk_grp(fn, in, &pk) != 0) return emit_pk_region_read(f, fn, in, t, cur, pk);
  }
  dv = in->def[0];
  av = in->use[0];
  kv = in->use[1];
  if (dv >= (Q2ValueId)t->nvalues || av >= (Q2ValueId)t->nvalues ||
      kv >= (Q2ValueId)t->nvalues)
    return 0;
  if (t->lb[av] != E_LB_TAG) return 0;
  if (t->lb[kv] == E_LB_I32)
    snprintf(kb, sizeof(kb), "JS_MKVAL(JS_TAG_INT, i%u)", (unsigned)kv);
  else if (t->lb[kv] == E_LB_TAG)
    snprintf(kb, sizeof(kb), "v%u", (unsigned)kv); /* an int is a hit; any other key is the wrapper's */
  else
    return 0;
  dreg = fn->values[dv].reg;
  areg = fn->values[av].reg;
  kreg = fn->values[kv].reg;
  if (dreg < 0 || dreg >= fn->nregs || areg < 0 || areg >= fn->nregs ||
      kreg < 0 || kreg >= fn->nregs)
    return 0;

  /* EVERY check before the first byte (see the rule at the dispatcher): a TAG def is
     a plain copy, an i32/f64 def gets the post-hit tag guard. */
  if (t->lb[dv] != E_LB_TAG && t->lb[dv] != E_LB_I32 &&
      t->lb[dv] != E_LB_F64)
    return 0;
  fprintf(f, "    { JSValue _e;\n");
  fprintf(
      f,
      "      if (__builtin_expect(!Q2_FORCE_MISS && (q2_elem_hit(v%u, %s, &_e) ||\n"
      "          q2_str_elem(ctx, v%u, %s, &_e)), 1)) {\n",
      (unsigned)av, kb, (unsigned)av, kb);
  if (t->lb[dv] == E_LB_TAG) {
    fprintf(f, "        v%u = _e;\n", (unsigned)dv);
  } else {
    /* The raw result reaches the frame only when the guard misses, so a miss leaves the
       op's result where the generic body will look for it; a dead result is neither. */
    if (e_def_dead(in, dv)) {
      /* nothing reads it: no guard, no store */
    } else {
      e_sweep_guard_set(f, "dx", in->next, dreg * 16, "_e");
      if (t->lb[dv] == E_LB_I32)
        fprintf(
            f,
            "        if (__builtin_expect(!Q2_I32_OK(_e, i%u), "
            "0)) { AOT_SET_B(ctx, fr, %d, _e); goto dx_%d; }\n",
            (unsigned)dv, dreg * 16, in->next);
      else if (t->lb[dv] == E_LB_F64)
        fprintf(
            f,
            "        if (JS_VALUE_GET_TAG(_e) == JS_TAG_INT) d%u = "
            "(double)JS_VALUE_GET_INT(_e);\n"
            "        else if (JS_VALUE_GET_TAG(_e) == JS_TAG_FLOAT64) d%u = "
            "JS_VALUE_GET_FLOAT64(_e);\n"
            "        else { AOT_SET_B(ctx, fr, %d, _e); goto dx_%d; }\n",
            (unsigned)dv, (unsigned)dv, dreg * 16, in->next);
    }
  }
  fprintf(f, "      } else {\n");
  fprintf(f, "        AOT_SET_B(ctx, fr, %d, v%u);\n", areg * 16, (unsigned)av);
  fprintf(f, "        AOT_SET_B(ctx, fr, %d, %s);\n", kreg * 16, kb);
  if (!emit_typed_opcall(f, fn, in, t, cur, NULL, hex, fi, b)) return 0;
  fprintf(f, "      }\n");
  fprintf(f, "    }\n");
  if (cur != NULL) cur[dreg] = dv;
  g_e_ta_keep = 1;
  return 1;
}

/* A speculative-body ELEMENT WRITE: `put_element` / `put_element_xfer` ("rrr":
   array, key, value). Under DRC the xfer form is the same store. */
static int emit_typed_elem_write(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur,
    int have_types, char hex[33], int fi, int b) {
  char elemcall[384];
  const ERole *role = e_roles[in->rop];
  Q2ValueId av, kv, vv;
  int areg, kreg, vreg;
  char kb[48], vb[48];

  if (!have_types || t == NULL) return 0;
  if (in->rop != ROP_put_element && in->rop != ROP_put_element_xfer) return 0;
  if (role == NULL || role->control != E_CF_NONE) return 0;
  if (in->nuse < 3) return 0;
  if (g_e_facts != NULL && g_e_facts->elemreg[e_insn_idx(fn, in)])
    return emit_elem_region_write(f, fn, in, t, cur);
  {
    int pk = 0;
    if (e_pk_grp(fn, in, &pk) != 0) return emit_pk_region_write(f, fn, in, t, cur, pk);
  }
  av = in->use[0];
  kv = in->use[1];
  vv = in->use[2];
  if (av >= (Q2ValueId)t->nvalues || kv >= (Q2ValueId)t->nvalues ||
      vv >= (Q2ValueId)t->nvalues)
    return 0;
  /* The inline store (`q2_elem_put`) is a plain store: it takes a new value and
     an old one that carry no reference, and does no counting. A frameless body
     leaves on the other cases, so only a number or a bool is stored there: an
     object stored over a slot would leave on every execution. */
  if (t->lb[av] != E_LB_TAG) return 0;
  /* A frameless body leaves at a miss and cannot run the store's slow path: a store that
     grows the array or fills a hole would leave on every call. */
  if (g_e_fl && elem_pc_store_misses(fn, in)) {
    e_fl_block("a store that grows an array or fills a hole", in);
    return 0;
  }
  if (t->lb[kv] == E_LB_I32)
    snprintf(kb, sizeof(kb), "JS_MKVAL(JS_TAG_INT, i%u)", (unsigned)kv);
  else if (t->lb[kv] == E_LB_TAG)
    snprintf(kb, sizeof(kb), "v%u", (unsigned)kv);
  else
    return 0;
  if (t->lb[vv] == E_LB_I32)
    snprintf(vb, sizeof(vb), "JS_MKVAL(JS_TAG_INT, i%u)", (unsigned)vv);
  else if (t->lb[vv] == E_LB_F64)
    snprintf(vb, sizeof(vb), "E_BOX_F64(d%u)", (unsigned)vv);
  else if (t->lb[vv] == E_LB_BOOL)
    snprintf(vb, sizeof(vb), "JS_MKVAL(JS_TAG_BOOL, b%u)", (unsigned)vv);
  else
    snprintf(vb, sizeof(vb), "v%u", (unsigned)vv);
  areg = fn->values[av].reg;
  kreg = fn->values[kv].reg;
  vreg = fn->values[vv].reg;
  if (areg < 0 || areg >= fn->nregs || kreg < 0 || kreg >= fn->nregs ||
      vreg < 0 || vreg >= fn->nregs)
    return 0;

  fprintf(f, "    {\n");
  fprintf(
      f,
      "      if (__builtin_expect(!Q2_FORCE_MISS && %s, 1)) {\n",
      g_e_fl ? (snprintf(elemcall, sizeof(elemcall),
                          "q2_elem_put_rc(ctx, v%u, %s, %s)", (unsigned)av, kb, vb),
                 elemcall)
              : (snprintf(elemcall, sizeof(elemcall),
                          "(q2_elem_put(v%u, %s, %s) || q2_elem_append(ctx, v%u, %s, %s))",
                          (unsigned)av, kb, vb, (unsigned)av, kb, vb),
                 elemcall));
  fprintf(f, "      } else {\n");
  fprintf(f, "        AOT_SET_B(ctx, fr, %d, v%u);\n", areg * 16, (unsigned)av);
  fprintf(f, "        AOT_SET_B(ctx, fr, %d, %s);\n", kreg * 16, kb);
  fprintf(f, "        AOT_SET_B(ctx, fr, %d, %s);\n", vreg * 16, vb);
  if (!emit_typed_opcall(f, fn, in, t, cur, NULL, hex, fi, b)) return 0;
  fprintf(f, "      }\n");
  fprintf(f, "    }\n");
  e_inl_store_mark(f);
  g_e_ta_keep = 1;
  return 1;
}

static int emit_hit_result(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId dv,
    int dreg, const char *raw);
static void e_toi32_expr(char *buf, size_t n, const ETypes *t, Q2ValueId a);
/* The exit stubs (dx) that take the owned result of a call to compiled code, by pc, with
   the register offset it was written to. */
static int g_e_dx_own_pc[1024], g_e_dx_own_off[1024], g_e_dx_own_n;

/* Negative controls (never set in a real build): "off" lowers no intrinsic (the
   as-built baseline of a diff); "noident" drops the identity
   check, so a replaced builtin is called as if it were the original; "cont" makes
   the miss CONTINUE into the real call while the facts still treat the site as
   no safepoint, so a `valueOf` that changes the world leaves stale facts. Each
   makes its test case FAIL, which is what proves the case tests the property. */
static const char *e_intr_control(void) {
  static const char *v;
  static int init;
  if (!init) {
    v = getenv("QJS_AOT_INTR_CONTROL");
    init = 1;
  }
  return v;
}

/* ---- intrinsic calls ------------------------------------------

   A call whose profiled callee is one engine builtin (Math.floor, parseInt, ...)
   with numeric arguments cannot run JS, allocate or write memory, so in the
   speculative body it is NOT a safepoint: the callee is checked against the
   engine's own function pointer, the arguments are checked to be numbers, and the
   result is computed inline. Anything else -- a replaced builtin, a non-number
   argument (a `valueOf` could run) -- is an EXIT before the call (form (a) of the
   safepoint split): the generic body redoes the call, which is the safepoint it
   always was. The facts pass and the emitter share ONE predicate, so a site the
   facts treat as no safepoint is never given the op-call wrapper. */
static const uint8_t *g_e_intr_mask;

int e_intrinsic_lowerable(const Q2Func *fn, const Q2Insn *in) {
  int id;
  const char *ctl = e_intr_control();
  if (ctl != NULL && strcmp(ctl, "off") == 0) return 0;
  if (g_e_intr_mask != NULL && !g_e_intr_mask[in - fn->insns]) return 0;
  if (!call_site_shaped(fn, in)) return 0;
  if (call_callee_off(in) < 0) return 0;
  id = e_intrinsic_site(fn->g->digest, in->next);
  if (id < 0 || !e_intrinsic_pure(id) || !e_intrinsic_argc_ok(id, in->win_n))
    return 0;
  if (dest_off(fn, in) / 16 >= fn->nregs) return 0;
  return 1;
}

/* The number view of a window register, as a `Q2Num` expression. */
static void intr_num(
    char *out, size_t n, const Q2Func *fn, const ETypes *t,
    const Q2ValueId *cur, int reg) {
  Q2ValueId v = cur != NULL && reg < fn->nregs ? cur[reg] : Q2_NO_VALUE;
  if (v == E_UNDEF) {
    snprintf(out, n, "(Q2Num){3, 0, 0}");
  } else if (
      v != Q2_NO_VALUE && v != E_INFRAME && v < (Q2ValueId)t->nvalues &&
      t->lb[v] == E_LB_I32) {
    snprintf(out, n, "(Q2Num){1, i%u, (double)i%u}", (unsigned)v, (unsigned)v);
  } else if (
      v != Q2_NO_VALUE && v != E_INFRAME && v < (Q2ValueId)t->nvalues &&
      t->lb[v] == E_LB_F64) {
    snprintf(out, n, "(Q2Num){2, 0, d%u}", (unsigned)v);
  } else if (
      v != Q2_NO_VALUE && v != E_INFRAME && v < (Q2ValueId)t->nvalues &&
      t->lb[v] == E_LB_TAG) {
    snprintf(out, n, "q2_numv(v%u)", (unsigned)v);
  } else if (
      v != Q2_NO_VALUE && v != E_INFRAME && v < (Q2ValueId)t->nvalues) {
    /* a bool or an unplaced value: never a number */
    snprintf(out, n, "(Q2Num){0, 0, 0}");
  } else {
    snprintf(out, n, "q2_numv(AOT_REG(fr, %d))", reg * 16);
  }
}

static int g_e_intr_cont;
static int emit_typed_call(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur,
    int have_types, char hex[33], int fi, int b);

/* Whether every argument of the intrinsic site is a number by the analysis: an i32
   or f64 value (parseInt's radix: absent or the constant 0 or 10). A site whose
   argument is not would leave on every execution, so it keeps the call. */
static int intr_args_numeric(const Q2Func *fn, const Q2Insn *in, const ETypes *t) {
  int id = e_intrinsic_site(fn->g->digest, in->next), k, na = in->win_n;
  if (in->win_val == NULL) return 0;
  for (k = 0; k < na; k++) {
    Q2ValueId v = in->win_val[k];
    if (v == Q2_NO_VALUE || v >= (Q2ValueId)t->nvalues) return 0;
    if (id == QJS_AOT_INTR_PARSEINT && k == 1) {
      const Q2Value *rv = &fn->values[v];
      if (!rv->is_const || rv->con_kind != Q2_CON_INT ||
          (rv->con != 0 && rv->con != 10))
        return 0;
      continue;
    }
    if (t->lb[v] != E_LB_I32 && t->lb[v] != E_LB_F64) return 0;
  }
  return 1;
}

/* The facts and the types of one function. An intrinsic site is no safepoint only
   when its arguments are numbers, which the types decide and the facts feed; so the
   sites start optimistic and the ones whose arguments turn out not to be numbers
   are dropped until nothing changes (a site only ever leaves the set, so it ends). */
int e_analyze(const Q2Func *fn, EFacts *facts, ETypes *types) {
  uint8_t *mask = calloc((size_t)(fn->ninsns > 0 ? fn->ninsns : 1), 1);
  int i, rounds, rc = -1;
  if (mask == NULL) return -1;
  for (i = 0; i < fn->ninsns; i++) {
    const Q2Insn *in = &fn->insns[i];
    g_e_intr_mask = NULL;
    if (in->op == Q2_OP_CALL && e_intrinsic_lowerable(fn, in)) mask[i] = 1;
  }
  e_facts_poly_on = !g_e_fl && getenv("QJS_AOT_NOPOLY") == NULL;
  e_facts_pure_ok = !g_e_fl && !g_e_inl && getenv("QJS_AOT_NOPURE") == NULL;
  e_facts_pure_call = e_pure_call_site;
  for (rounds = 0; rounds < 8; rounds++) {
    int changed = 0;
    g_e_intr_mask = mask;
    if (e_facts_run(fn, facts, 1) < 0) goto done;
    if (e_types_run(fn, facts, types) < 0) {
      e_facts_free(facts);
      goto done;
    }
    for (i = 0; i < fn->ninsns; i++) {
      if (!mask[i] || intr_args_numeric(fn, &fn->insns[i], types)) continue;
      mask[i] = 0;
      changed = 1;
    }
    if (!changed) {
      rc = 0;
      goto done;
    }
    e_facts_free(facts);
    e_types_free(types);
  }
done:
  g_e_intr_mask = NULL;
  free(mask);
  return rc;
}

static int emit_intrinsic_call(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t,
    Q2ValueId *cur, char hex[33], int fi, int b) {
  const char *ctl = e_intr_control();
  int id = e_intrinsic_site(fn->g->digest, in->next);
  int cc = call_callee_off(in), off = dest_off(fn, in), dreg = off / 16;
  int na = in->win_n, magic = e_intrinsic_magic(id);
  Q2ValueId dv = in->def[0];
  Q2ValueId cv =
      cur != NULL && cc / 16 < fn->nregs ? cur[cc / 16] : Q2_NO_VALUE;
  char cexp[64], a0[128], a1[128], rexp[64], intcall[160];
  int is_str;

  /* ---- every check, before any output ---- */
  if (dv >= (Q2ValueId)t->nvalues) return 0;
  if (cv != Q2_NO_VALUE && cv != E_UNDEF && cv != E_INFRAME &&
      cv < (Q2ValueId)t->nvalues && t->lb[cv] == E_LB_TAG)
    snprintf(cexp, sizeof(cexp), "v%u", (unsigned)cv);
  else if (cv == E_UNDEF || (cv != Q2_NO_VALUE && cv != E_INFRAME))
    snprintf(cexp, sizeof(cexp), "JS_UNDEFINED"); /* not an object: always exits */
  else
    snprintf(cexp, sizeof(cexp), "AOT_REG(fr, %d)", cc);
  /* In a frameless body the miss is an exit. A replaced builtin is rare and the
     profile saw the callee; parseInt can also miss on its argument's value, so in a
     loop that may run long it keeps the function framed. */
  if (g_e_fl && !g_e_inl && id == QJS_AOT_INTR_PARSEINT && g_e_fl_longloop &&
      g_e_loop_blk != NULL && g_e_blk >= 0 && g_e_loop_blk[g_e_blk])
    e_fl_block("a parseInt in a loop", in);
  intr_num(a0, sizeof(a0), fn, t, cur, in->win_reg);
  if (na >= 2)
    intr_num(a1, sizeof(a1), fn, t, cur, in->win_reg + 1);
  else
    snprintf(a1, sizeof(a1), "(Q2Num){3, 0, 0}");
  /* A string method takes its receiver from the call's `this` register. */
  is_str = id == QJS_AOT_INTR_STR_CHARCODEAT || id == QJS_AOT_INTR_STR_CODEPOINTAT ||
           id == QJS_AOT_INTR_STR_CHARAT;
  if (is_str) {
    int th = call_this_off(in);
    Q2ValueId tv = th >= 0 && cur != NULL && th / 16 < fn->nregs ? cur[th / 16] : Q2_NO_VALUE;
    if (th < 0) return 0;
    if (tv != Q2_NO_VALUE && tv != E_UNDEF && tv != E_INFRAME && tv < (Q2ValueId)t->nvalues &&
        t->lb[tv] == E_LB_TAG)
      snprintf(rexp, sizeof(rexp), "v%u", (unsigned)tv);
    else if (tv == E_UNDEF || (tv != Q2_NO_VALUE && tv != E_INFRAME))
      snprintf(rexp, sizeof(rexp), "JS_UNDEFINED");
    else
      snprintf(rexp, sizeof(rexp), "AOT_REG(fr, %d)", th);
    snprintf(a1, sizeof(a1), "(Q2Num){3, 0, 0}");
  }

  /* ---- output ---- */
  e_note_stub(
      g_e_vx, g_e_vx_cur, g_e_vx_blk, &g_e_vx_n, in->pc, cur, NULL);
  e_note_tag('v', in->pc, cexp);
  e_sweep_guard(f, "vx", in->pc);
  if (t->lb[dv] != E_LB_TAG && t->lb[dv] != E_LB_BOT)
    e_note_stub(
        g_e_dx, g_e_dx_cur, g_e_dx_blk, &g_e_dx_n, in->next, cur, in);
  fprintf(f, "    { JSValue _o;\n");
  if (ctl != NULL && strcmp(ctl, "cont") == 0) {
    /* control: a miss falls through to the real call below */
    fprintf(
        f,
        "      if (q2_is_intr(%s, %d, %d) && %s) {\n",
        cexp, id, magic,
        is_str ? (snprintf(intcall, sizeof(intcall), "q2_intr_str(ctx, %d, %s, %s, &_o)", id, rexp, a0),
                  intcall)
               : (snprintf(intcall, sizeof(intcall), "q2_intr(%d, %s, %s, &_o)", id, a0, a1),
                  intcall));
    if (t->lb[dv] == E_LB_BOT)
      fprintf(f, "        AOT_SET_B(ctx, fr, %d, _o);\n", off);
    else if (!emit_hit_result(f, fn, in, t, dv, dreg, "_o"))
      abort();
    fprintf(f, "        goto cd_%d;\n      }\n    }\n", in->pc);
    g_e_intr_cont = 1;
    emit_typed_call(f, fn, in, t, cur, 1, hex, fi, b);
    g_e_intr_cont = 0;
    g_e_miss_exit_seen = 1;
    return 1;
  }
  if (ctl != NULL && strcmp(ctl, "noident") == 0)
    fprintf(f, "      (void)q2_is_intr;\n");
  else
    fprintf(
        f,
        "      if (__builtin_expect(!q2_is_intr(%s, %d, %d), 0)) goto vx_%d;\n",
        cexp, id, magic, in->pc);
  if (is_str)
    fprintf(
        f,
        "      if (__builtin_expect(!q2_intr_str(ctx, %d, %s, %s, &_o), 0)) goto vx_%d;\n",
        id, rexp, a0, in->pc);
  else if (na == 3 && (id == QJS_AOT_INTR_MIN || id == QJS_AOT_INTR_MAX)) {
    /* min(a, b, c) is min(min(a, b), c), NaN and the sign of zero included. */
    char a2[128];
    intr_num(a2, sizeof(a2), fn, t, cur, in->win_reg + 2);
    fprintf(
        f,
        "      if (__builtin_expect(!q2_intr(%d, %s, %s, &_o) ||\n"
        "          !q2_intr(%d, q2_numv(_o), %s, &_o), 0)) goto vx_%d;\n",
        id, a0, a1, id, a2, in->pc);
  } else
    fprintf(
        f,
        "      if (__builtin_expect(!q2_intr(%d, %s, %s, &_o), 0)) goto vx_%d;\n",
        id, a0, a1, in->pc);
  if (t->lb[dv] == E_LB_BOT) {
    fprintf(f, "        AOT_SET_B(ctx, fr, %d, _o);\n", off);
  } else if (!emit_hit_result(f, fn, in, t, dv, dreg, "_o")) {
    fprintf(stderr, "qjsaot: intrinsic at pc %d: no guard for its def\n", in->pc);
    abort();
  }
  fprintf(f, "    }\n");
  if (cur != NULL) cur[dreg] = dv;
  g_e_miss_exit_seen = 1;
  return 1;
}

/* ---- a call inside an inline body -------------------------

   The nested call is the ordinary compiled-to-compiled call -- the callee's bound
   entry checked against this unit's body, then `aot_n_` -- with no op-call wrapper
   behind it (an inline body has no frame to step): a miss is an EXIT before the call.
   The call is a safepoint, so:
   - every TAG value live across it must be rooted by someone else (the caller's
     argument window or receiver), because the body's locals are invisible to the
     collector; otherwise the function is not an inline candidate;
   - the result must be typed, or dead (a dead owned result is released here);
   - from here on the body has an effect, so its exits build the callee's frame
     (aot_nx_) and finish in the generic body (g_e_inl_dirty). */
/* Slot `idx` of the published window array, from register `wr`'s current value. */
static void e_emit_fl_window_store(
    FILE *f, const Q2Func *fn, const ETypes *t, const Q2ValueId *cur, int wr, int idx) {
  Q2ValueId v;
  if (wr >= fn->nregs || (v = cur[wr]) == Q2_NO_VALUE || v == E_UNDEF) {
    fprintf(f, "    ra_[SB_ + %d] = JS_UNDEFINED;\n", idx);
    return;
  }
  if (v == E_INFRAME || v >= (Q2ValueId)t->nvalues) {
    fprintf(f, "    ra_[SB_ + %d] = AOT_REG(fr, %d);\n", idx, wr * 16);
    return;
  }
  if (t->lb[v] == E_LB_TAG)
    fprintf(f, "    ra_[SB_ + %d] = v%u;\n", idx, (unsigned)v);
  else if (t->lb[v] == E_LB_I32)
    fprintf(f, "    ra_[SB_ + %d] = JS_MKVAL(JS_TAG_INT, i%u);\n", idx, (unsigned)v);
  else if (t->lb[v] == E_LB_F64)
    fprintf(f, "    ra_[SB_ + %d] = E_BOX_F64(d%u);\n", idx, (unsigned)v);
  else if (t->lb[v] == E_LB_BOOL)
    fprintf(f, "    ra_[SB_ + %d] = JS_MKVAL(JS_TAG_BOOL, b%u);\n", idx, (unsigned)v);
  else
    fprintf(f, "    ra_[SB_ + %d] = AOT_REG(fr, %d);\n", idx, wr * 16);
}

/* The owned result of a nested call, put where the rest of the body reads it. */
static int e_emit_nested_result(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId dv, int dreg,
    int dead, int resslot, const char *ind) {
  const int pub = !g_e_inl || g_e_inl_r;

  if (pub && t->lb[dv] == E_LB_TAG && !dead) {
    /* The owned result parks in the published window, then its count is dropped (the
       drop can run a collection); the next safepoint gives the value a slot. */
    fprintf(
        f,
        "%sif (_r == 1 && JS_VALUE_HAS_REF_COUNT(_o)) qjs_aot_set_reg_p(ctx, &ra_[%d], 0, _o);\n"
        "%selse ra_[%d] = _o;\n"
        "%sv%u = _o;\n"
        "%sAOT_SET_B(ctx, fr, %d, _o);\n",
        ind, resslot, ind, resslot, ind, (unsigned)dv, ind, dreg * 16);
  } else if (t->lb[dv] == E_LB_TAG || t->lb[dv] == E_LB_BOT) {
    fprintf(f, "%sif (_r == 1 && JS_VALUE_HAS_REF_COUNT(_o)) JS_FreeValue(ctx, _o);\n", ind);
  } else if (!emit_hit_result(f, fn, in, t, dv, dreg, "_o")) {
    fprintf(stderr, "qjsaot: nested call at pc %d: no guard for its def\n", in->pc);
    abort();
  }
  return 1;
}

static int emit_inl_nested_call(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur,
    char hex[33]) {
  char callee[4][33];
  unsigned long long hits[4];
  int kind[4], n = 0, k, cc, th, off, na, w, r, nb = 0;
  char recv[64], cexp[64];
  Q2ValueId dv;
  int dreg, dead;
  uint64_t *lb;
  Q2ValueId spv[E_SPILL_MAX + 1];
  int nsp, resslot;
  const char *excret = g_e_inl ? "{ *out = _o; return 1; }" : "return _o;";
  char winp[48], ib[512], pushr[200], spl[1024], idt[256];
  int allleaf, cmc = -1;
  const int pub = !g_e_inl || g_e_inl_r;
  char nrx[32], nrxe[64], rax[48];
  int tlay = g_e_facts != NULL ? g_e_facts->thislay[e_insn_idx(fn, in)] : 0;
  const char *popr = g_e_inl ? "        E_FL_TOP(ctx_in) = ir_.prev;\n" : "";

  if (t == NULL || cur == NULL) return 0;
  if (!call_site_shaped(fn, in)) {
    e_fl_block("an unshaped call", in);
    return 0;
  }
  if (!qjs_aot_prof_callees(fn->g->digest, in->next, &n, callee, kind, hits)) {
    e_fl_block("a call with no profile", in);
    return 0;
  }
  for (k = 0; k < n && k < 4; k++) {
    if (kind[k] != 1 || !qjs_aot_unit_has(callee[k])) continue;
    if (!qjs_aot_native_capable(callee[k])) continue;
    if (in->win_n < qjs_aot_unit_argc(callee[k])) continue;
    nb++;
    if (g_e_inl && !qjs_aot_inline_has(callee[k]) && qjs_aot_inline_pending(callee[k]))
      g_e_inl_pending = 1;
  }
  if (nb == 0) {
    e_fl_block("a call to no compiled callee", in);
    return 0;
  }
  cc = call_callee_off(in);
  th = call_this_off(in);
  off = dest_off(fn, in);
  na = in->win_n;
  if (g_e_inl_r)
    snprintf(nrx, sizeof(nrx), "%s", getenv("QJS_AOT_NOWINROOT") != NULL ? "0" : "SB_ + 0");
  if (g_e_inl_r && getenv("QJS_AOT_NOWINROOT") == NULL) snprintf(nrx, sizeof(nrx), "SB_ + %d", na);
  if (g_e_inl_r)
    snprintf(nrxe, sizeof(nrxe), "ra_x_ ? 0 : (%s)", nrx);
  else
    snprintf(nrxe, sizeof(nrxe), "0");
  snprintf(rax, sizeof(rax), "%s",
           !pub ? "NULL" : g_e_inl ? "(ra_x_ ? ra_ + RB_ : NULL)" : "ra_ + RB_");
  w = in->win_reg * 16;
  if (!pub)
    snprintf(winp, sizeof(winp), "&AOT_REG(fr, %d)", w);
  else
    snprintf(winp, sizeof(winp), "&ra_[SB_]");
  if (off < 0 || cc < 0 || in->ndef < 1) return 0;
  dv = in->def[0];
  if (dv >= (Q2ValueId)t->nvalues) return 0;
  dreg = off / 16;
  if (dreg < 0 || dreg >= fn->nregs) return 0;
  dead = e_def_dead(in, dv);
  if (t->lb[dv] == E_LB_TAG && !dead && !pub) {
    e_fl_block("a call whose result is a TAG value", in);
    return 0;
  }
  if (t->lb[dv] == E_LB_BOT && !dead) return 0;
  resslot = -1;
  if (pub && t->lb[dv] == E_LB_TAG && !dead) {
    resslot = e_spill_slot(dv);
    if (resslot < 0) {
      e_fl_block("too many values live across a call", in);
      return 0;
    }
  }
  /* Rooted: every TAG value live at the call, but the callee and the receiver. */
  lb = calloc((size_t)(fn->lwords > 0 ? fn->lwords : 1), 8);
  if (lb == NULL) return 0;
  q2_live_before(fn, (int)(in - fn->insns), lb);
  /* The window is published as a root range while the call runs, so an argument
     nothing reads after the call needs no other root. */
  if (pub && (int)(in - fn->insns) + 1 < fn->ninsns) {
    uint64_t *la = calloc((size_t)(fn->lwords > 0 ? fn->lwords : 1), 8);
    if (la == NULL) {
      free(lb);
      return 0;
    }
    q2_live_before(fn, (int)(in - fn->insns) + 1, la);
    for (r = in->win_reg; r < in->win_reg + in->win_n && r < fn->nregs; r++)
      if (!((la[r / 64] >> (r % 64)) & 1)) lb[r / 64] &= ~((uint64_t)1 << (r % 64));
    free(la);
  }
  nsp = 0;
  for (r = 0; r < fn->norig; r++) {
    Q2ValueId v;
    if (!((lb[r / 64] >> (r % 64)) & 1)) continue;
    v = cur[r];
    if (v == Q2_NO_VALUE || v == E_UNDEF || v == E_INFRAME ||
        v >= (Q2ValueId)t->nvalues || t->lb[v] != E_LB_TAG)
      continue;
    if (g_e_rooted != NULL && g_e_rooted[v]) continue;
    if (!pub) {
      if (r * 16 == cc || (th >= 0 && r * 16 == th)) continue;
      free(lb);
      e_fl_block("a value live across a call is not rooted", in);
      return 0;
    }
    /* Unrooted: the published array holds it for the call (the callee and the
       receiver too, which only this body refers to). */
    if (e_spill_slot(v) < 0) {
      free(lb);
      e_fl_block("too many values live across a call", in);
      return 0;
    }
    spv[nsp++] = v;
  }
  free(lb);
  {
    Q2ValueId cv = cc / 16 < fn->nregs ? cur[cc / 16] : Q2_NO_VALUE;
    Q2ValueId tv = th >= 0 && th / 16 < fn->nregs ? cur[th / 16] : Q2_NO_VALUE;
    if (cv != Q2_NO_VALUE && cv != E_UNDEF && cv != E_INFRAME && cv < (Q2ValueId)t->nvalues)
      cmc = e_mc_of(cv);
    if (cv != Q2_NO_VALUE && cv != E_UNDEF && cv != E_INFRAME &&
        cv < (Q2ValueId)t->nvalues && t->lb[cv] == E_LB_TAG)
      snprintf(cexp, sizeof(cexp), "v%u", (unsigned)cv);
    else
      snprintf(cexp, sizeof(cexp), "AOT_REG(fr, %d)", cc);
    if (th < 0)
      snprintf(recv, sizeof(recv), "JS_UNDEFINED");
    else if (tv != Q2_NO_VALUE && tv != E_UNDEF && tv != E_INFRAME &&
             tv < (Q2ValueId)t->nvalues && t->lb[tv] == E_LB_TAG)
      snprintf(recv, sizeof(recv), "v%u", (unsigned)tv);
    else
      snprintf(recv, sizeof(recv), "AOT_REG(fr, %d)", th);
  }

  /* ---- output ---- */
  if (g_e_inl) g_e_inl_safepoint = 1;
  e_note_stub(g_e_vx, g_e_vx_cur, g_e_vx_blk, &g_e_vx_n, in->pc, cur, NULL);
  e_note_tag('v', in->pc, cexp);
  if (t->lb[dv] != E_LB_TAG && t->lb[dv] != E_LB_BOT)
    e_note_stub(
        g_e_dx, g_e_dx_cur, g_e_dx_blk, &g_e_dx_n, in->next, cur, in);
  if (!g_e_inl && t->lb[dv] != E_LB_TAG && t->lb[dv] != E_LB_BOT) {
    if (g_e_dx_own_n >= 1024) {
      e_fl_block("too many call exits", in);
      return 0;
    }
    g_e_dx_own_pc[g_e_dx_own_n] = in->next;
    g_e_dx_own_off[g_e_dx_own_n++] = off;
  }
  /* the window, current in the body's own register array; a frameless body that is
     not inlined keeps it in an array of its own, so the rest of its register array
     is plain locals */
  for (r = 0; r < na; r++) {
    Q2ValueId v;
    int wr = in->win_reg + r;
    if (pub) {
      e_emit_fl_window_store(f, fn, t, cur, wr, r);
      continue;
    }
    if (wr >= fn->nregs) continue;
    v = cur[wr];
    if (v == Q2_NO_VALUE || v == E_UNDEF || v >= (Q2ValueId)t->nvalues) continue;
    if (t->lb[v] == E_LB_TAG)
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, v%u);\n", wr * 16, (unsigned)v);
    else if (t->lb[v] == E_LB_I32)
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, JS_MKVAL(JS_TAG_INT, i%u));\n",
              wr * 16, (unsigned)v);
    else if (t->lb[v] == E_LB_F64)
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, E_BOX_F64(d%u));\n", wr * 16,
              (unsigned)v);
    else if (t->lb[v] == E_LB_BOOL)
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, JS_MKVAL(JS_TAG_BOOL, b%u));\n",
              wr * 16, (unsigned)v);
  }
  e_sweep_guard(f, "vx", in->pc);
  /* Every candidate is an inlined body that makes no call: while it runs nothing can
     collect, so the roots and the record are written only for the call it falls back to. */
  allleaf = 0;
  if (pub) {
    allleaf = 1;
    for (k = 0; k < n && k < 4; k++) {
      if (kind[k] != 1 || !qjs_aot_unit_has(callee[k])) continue;
      if (!qjs_aot_native_capable(callee[k])) continue;
      if (in->win_n < qjs_aot_unit_argc(callee[k])) continue;
      if (!qjs_aot_inline_has(callee[k]) || !qjs_aot_inline_leaf(callee[k])) allleaf = 0;
    }
  }
  spl[0] = 0;
  if (pub) {
    /* QJS_AOT_NOWINROOT=1 leaves the window unpublished: the negative control. */
    size_t sl = 0;
    for (k = 0; k < nsp; k++)
      sl += (size_t)snprintf(
          spl + sl, sizeof(spl) - sl, "    ra_[%d] = v%u;\n", e_spill_slot(spv[k]),
          (unsigned)spv[k]);
    if (!g_e_inl) {
      g_e_fl_calls = 1;
      snprintf(
          spl + sl, sizeof(spl) - sl, "    rec_->pc = %d;\n", in->next - 1);
    }
    if (!allleaf) fputs(spl, f);
  }
  for (k = 0; k < n && k < 4; k++) {
    if (kind[k] != 1 || !qjs_aot_unit_has(callee[k])) continue;
    if (!qjs_aot_native_capable(callee[k])) continue;
    if (in->win_n < qjs_aot_unit_argc(callee[k])) continue;
    fprintf(f, "#ifdef Q2_NATIVE_%s\n", callee[k]);
    ib[0] = 0;
    if (cmc >= 0)
      snprintf(idt, sizeof(idt), "mce%d == (const void *)aot_f_%s", cmc, callee[k]);
    else
      snprintf(
          idt, sizeof(idt), "%s%s, (const void *)aot_f_%s, %du)", e_cc_fn(fn, in), cexp,
          callee[k], e_cc_ix(fn, in));
    /* An inlined body has no frame; its record puts it in the backtrace. */
    if (g_e_inl)
      snprintf(
          pushr, sizeof(pushr),
          "        JSAotFlRec ir_ = { E_FL_TOP(ctx_in), co, %d, %s, %s };\n"
          "        E_FL_TOP(ctx_in) = &ir_;\n",
          in->next - 1, nrxe, g_e_inl_r ? "ra_x_ ? NULL : ra_" : "NULL");
    else
      pushr[0] = 0;
    if (qjs_aot_inline_has(callee[k]) && qjs_aot_inline_rz(callee[k]) > g_e_child_max)
      g_e_child_max = qjs_aot_inline_rz(callee[k]);
    if (qjs_aot_inline_has(callee[k]))
      snprintf(
          ib, sizeof(ib),
          "#ifdef Q2_INLINE_%s\n"
          "        if (!(_r = ei_%s(ctx, caller_f, (JSObject *)JS_VALUE_GET_PTR(%s), %s,\n"
          "                    %s, %d, &_o, %s, %d)))\n"
          "#endif\n",
          callee[k], callee[k], cexp, recv, winp, na, rax,
          tlay != 0 && qjs_aot_inline_thislay(callee[k]) == tlay);
    if (getenv("QJS_AOT_INL_CONTROL") != NULL &&
        strcmp(getenv("QJS_AOT_INL_CONTROL"), "noident") == 0)
      fprintf(f, "    if (JS_VALUE_GET_TAG(%s) == JS_TAG_OBJECT) {\n"
                 "        JSValue _o; int _r = 1;\n%s"
                 "%s        _o = aot_n_%s(ctx, caller_f,\n"
                 "            (JSObject *)JS_VALUE_GET_PTR(%s), %s,\n"
                 "            %s, %d);\n"
                 "%s        if (__builtin_expect(JS_IsException(_o), 0)) "
                 "%s\n",
              cexp, ib, pushr, callee[k], cexp, recv, winp, na, popr, excret);
    else if (allleaf) {
      fprintf(
          f,
          "    if (__builtin_expect(%s, 1)) {\n"
          "        JSValue _o; int _r = 1;\n"
          "#ifdef Q2_INLINE_%s\n"
          "        if ((_r = ei_%s(ctx, caller_f, (JSObject *)JS_VALUE_GET_PTR(%s), %s,\n"
          "                    %s, %d, &_o, %s, %d))) {\n"
          "            if (__builtin_expect(JS_IsException(_o), 0)) %s\n",
          idt, callee[k], callee[k], cexp, recv, winp, na, rax,
          tlay != 0 && qjs_aot_inline_thislay(callee[k]) == tlay, excret);
      e_emit_nested_result(f, fn, in, t, dv, dreg, dead, resslot, "            ");
      fprintf(f, "            goto cd_%d;\n        }\n#endif\n", in->pc);
      fprintf(
          f,
          "        {\n%s%s"
          "        _r = 1;\n"
          "        _o = aot_n_%s(ctx, caller_f,\n"
          "            (JSObject *)JS_VALUE_GET_PTR(%s), %s,\n"
          "            %s, %d);\n"
          "%s%s"
          "        if (__builtin_expect(JS_IsException(_o), 0)) %s\n",
          spl, pushr, callee[k], cexp, recv, winp, na, popr,
          "", excret);
    }
    else
    fprintf(
        f,
        "    if (__builtin_expect(%s, 1)) "
        "{\n"
        "        JSValue _o; int _r = 1;\n%s%s"
        "        { _r = 1; _o = aot_n_%s(ctx, caller_f,\n"
        "            (JSObject *)JS_VALUE_GET_PTR(%s), %s,\n"
        "            %s, %d); }\n"
        "%s        if (__builtin_expect(JS_IsException(_o), 0)) "
        "%s\n",
        idt, pushr, ib, callee[k], cexp, recv, winp, na, popr, excret);
    g_e_inl_dirty = 1;
    e_emit_nested_result(f, fn, in, t, dv, dreg, dead, resslot, "        ");
    /* The result is in place (a TAG value in its local, a typed one in the frame
       slot the exit stub reads): the facts the call may have changed are checked now,
       so an exit here carries the result. */
    if (g_e_epoch_skip || g_e_nolazy) e_store_typed_def(f, fn, in, t, dv);
    e_emit_group_recheck(f, fn, in, cur);
    if (allleaf) fprintf(f, "        }\n");
    fprintf(f, "        goto cd_%d;\n    }\n#endif\n", in->pc);
  }
  /* No candidate matched. A frameless body that is not inlined calls any other
     function through the engine, so a changed callee costs the call and not the
     function; a value that is no function leaves before the call, and the generic
     body raises the error. */
  if (!g_e_inl) {
    fprintf(
        f,
        "    if (JS_IsFunction(ctx, %s)) {\n"
        "        JSValue _o = JS_Call(ctx, %s, %s, %d, %s);\n"
        "        if (__builtin_expect(JS_IsException(_o), 0)) return _o;\n",
        cexp, cexp, recv, na, winp);
    if (!g_e_inl && t->lb[dv] == E_LB_TAG && !dead) {
      /* The owned result parks in the published window, then its count is dropped (the
         drop can run a collection); the next safepoint gives the value a slot. */
      fprintf(
          f,
          "        if (JS_VALUE_HAS_REF_COUNT(_o)) qjs_aot_set_reg_p(ctx, &ra_[%d], 0, _o);\n"
          "        else ra_[%d] = _o;\n"
          "        v%u = _o;\n"
          "        AOT_SET_B(ctx, fr, %d, _o);\n",
          resslot, resslot, (unsigned)dv, dreg * 16);
    } else if (t->lb[dv] == E_LB_TAG || t->lb[dv] == E_LB_BOT) {
      fprintf(f, "        if (JS_VALUE_HAS_REF_COUNT(_o)) JS_FreeValue(ctx, _o);\n");
    } else if (!emit_hit_result(f, fn, in, t, dv, dreg, "_o")) {
      fprintf(stderr, "qjsaot: nested call at pc %d: no guard for its def\n", in->pc);
      abort();
    }
    if (g_e_epoch_skip || g_e_nolazy) e_store_typed_def(f, fn, in, t, dv);
    e_emit_group_recheck(f, fn, in, cur);
    fprintf(f, "        goto cd_%d;\n    }\n", in->pc);
  }
  /* The call has not run: leave before it. */
  fprintf(f, "    goto vx_%d;\n    cd_%d: ;\n", in->pc, in->pc);
  g_e_inl_dirty = 1;
  cur[dreg] = dv;
  g_e_miss_exit_seen = 0;
  (void)hex;
  (void)recv;
  return 1;
}

/* ---- `new C(...)` with a known constructor ----------------------
   The site checks the constructor's identity (bound entry), allocates `this` with the
   engine's own allocation for a plain light constructor (`qjs_aot_construct_this`),
   runs the compiled body with it, and applies the result rule: an object the
   constructor returns wins, otherwise `this`. new.target must be the constructor
   itself. Anything else (a derived class, another new.target, a miss) takes the
   op-call wrapper, which constructs through the engine. Allocation is a safepoint, so
   the live TAG values are in the frame first and the groups are re-checked after. */
/* A CONSTRUCT inside a copied body. The body has no frame of its own to hand the
   construct path (`f` in the emitted code), so the object is made and the constructor
   run with the same engine helpers the non-inlined path uses and the same result
   rule, on `this_obj` as the receiver: `qjs_aot_construct_this` allocates it from
   the constructor's prototype and learned capacity, the constructor's own entry runs
   it, and a non-object return leaves the object as the value. The guards are the
   construct path's own (the callee's entry identity and the constructor being the
   new.target), and a miss leaves the whole copied body for the call the body was
   copied from, which does the construct.
   The constructor runs arbitrary JavaScript, so this is a safepoint like any other
   call: every TAG value the body has live across it must already be rooted by
   someone else, exactly the rule an inlined call is held to. A body that would have
   to root one itself is not a candidate for this path. */
static int emit_inl_construct(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur,
    char hex[33]) {
  char callee[4][33];
  unsigned long long hits[4];
  int kind[4], n = 0, k, r, nb = 0, cc, nt, off, na, w, dreg;
  Q2ValueId dv, cv, nv;
  char cexp[64], ntexp[64];

  (void)hex;
  if (t == NULL || cur == NULL) return 0;
  if (in->rop != ROP_construct || in->ndef < 1 || in->narg < 5) return 0;
  if (e_roles[in->rop] == NULL || e_roles[in->rop]->control != E_CF_NONE) return 0;
  if (e_must_exit(fn, in)) return 0;
  if (!qjs_aot_prof_callees(fn->g->digest, in->next, &n, callee, kind, hits)) return 0;
  for (k = 0; k < n && k < 4; k++) {
    if (kind[k] != 1 || !qjs_aot_unit_has(callee[k])) continue;
    if (qjs_aot_light_need(callee[k]) <= 0) continue;
    if (in->win_n < qjs_aot_unit_argc(callee[k])) continue;
    nb++;
  }
  if (nb == 0) return 0;
  g_e_inl_safepoint = 1;
  cc = (int)in->arg[1].val;
  nt = (int)in->arg[2].val;
  off = dest_off(fn, in);
  na = in->win_n;
  w = in->win_reg * 16;
  if (off < 0 || cc < 0 || nt < 0 || (unsigned)(in->win_reg + na) > (unsigned)fn->nregs) return 0;
  dv = in->def[0];
  if (dv >= (Q2ValueId)t->nvalues) return 0;
  /* The object is the result. A TAG value is fine here (it is the object), and a
     typed or dead one is not this path's business. */
  if (t->lb[dv] != E_LB_TAG && !e_def_dead(in, dv)) return 0;
  dreg = off / 16;
  if (dreg < 0 || dreg >= fn->nregs) return 0;
  cv = cc / 16 < fn->nregs ? cur[cc / 16] : Q2_NO_VALUE;
  nv = nt / 16 < fn->nregs ? cur[nt / 16] : Q2_NO_VALUE;
  if (cv != Q2_NO_VALUE && cv != E_UNDEF && cv != E_INFRAME &&
      cv < (Q2ValueId)t->nvalues && t->lb[cv] == E_LB_TAG)
    snprintf(cexp, sizeof(cexp), "v%u", (unsigned)cv);
  else
    return 0; /* the callee is not a value this body holds in a register */
  if (nv != Q2_NO_VALUE && nv != E_UNDEF && nv != E_INFRAME &&
      nv < (Q2ValueId)t->nvalues && t->lb[nv] == E_LB_TAG)
    snprintf(ntexp, sizeof(ntexp), "v%u", (unsigned)nv);
  else
    return 0;

  /* The constructor's call is a safepoint: the body cannot root a value across it
     (it has no frame to publish), so every TAG value live across it must be rooted
     by someone else. The callee and the receiver are the exception, as in a call. */
  {
    uint64_t *lb = calloc((size_t)(fn->lwords > 0 ? fn->lwords : 1), 8);
    int r;
    if (lb == NULL) return 0;
    q2_live_before(fn, (int)(in - fn->insns), lb);
    for (r = 0; r < fn->norig; r++) {
      Q2ValueId v;
      if (!((lb[r / 64] >> (r % 64)) & 1)) continue;
      v = cur[r];
      if (v == Q2_NO_VALUE || v == E_UNDEF || v == E_INFRAME ||
          v >= (Q2ValueId)t->nvalues || t->lb[v] != E_LB_TAG)
        continue;
      if (g_e_rooted != NULL && g_e_rooted[v]) continue;
      if (r * 16 == cc || r * 16 == nt) continue;
      free(lb);
      e_fl_block("a value live across a construct is not rooted", in);
      return 0;
    }
    free(lb);
  }
  /* The constructor reads its arguments from the window it is given, which is the
     construct's own register window in the frame. This body holds those values in
     locals, so they are written to their slots first -- the same sync the
     non-inlined construct path does before it calls. */
  for (r = 0; r < na; r++) {
    Q2ValueId v;
    int wr = in->win_reg + r;
    if (wr >= fn->nregs) continue;
    v = cur[wr];
    if (v == Q2_NO_VALUE || v == E_UNDEF || v >= (Q2ValueId)t->nvalues) continue;
    if (t->lb[v] == E_LB_I32)
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, JS_MKVAL(JS_TAG_INT, i%u));\n", wr * 16, (unsigned)v);
    else if (t->lb[v] == E_LB_F64)
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, E_BOX_F64(d%u));\n", wr * 16, (unsigned)v);
    else if (t->lb[v] == E_LB_BOOL)
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, JS_MKVAL(JS_TAG_BOOL, b%u));\n", wr * 16, (unsigned)v);
  }
  for (k = 0; k < n && k < 4; k++) {
    if (kind[k] != 1 || !qjs_aot_unit_has(callee[k])) continue;
    if (qjs_aot_light_need(callee[k]) <= 0) continue;
    if (in->win_n < qjs_aot_unit_argc(callee[k])) continue;
    fprintf(f, "#ifdef Q2_NATIVE_%s\n", callee[k]);
    e_note_stub(g_e_vx, g_e_vx_cur, g_e_vx_blk, &g_e_vx_n, in->pc, cur, NULL);
    e_sweep_guard(f, "vx", in->pc);
    fprintf(
        f,
        "    if (__builtin_expect(q2_call_entry(%s, (const void *)aot_f_%s), 1) &&\n"
        "        JS_VALUE_GET_TAG(%s) == JS_TAG_OBJECT &&\n"
        "        JS_VALUE_GET_PTR(%s) == JS_VALUE_GET_PTR(%s)) {\n"
        "        JSValue _t, _o, _r;\n"
        "        int _rc = qjs_aot_construct_this(ctx, (JSObject *)JS_VALUE_GET_PTR(%s), &_t);\n"
        "        if (__builtin_expect(_rc < 0, 0)) { *out = JS_EXCEPTION; return 1; }\n"
        "        if (_rc > 0) {\n"
        "            _o = aot_n_%s(ctx, caller_f, (JSObject *)JS_VALUE_GET_PTR(%s), _t,\n"
        "                          &AOT_REG(fr, %d), %d);\n"
        "            if (__builtin_expect(JS_IsException(_o), 0)) {\n"
        "                JS_FreeValue(ctx, _t);\n"
        "                *out = _o; return 1;\n"
        "            }\n"
        "            if (JS_VALUE_GET_TAG(_o) == JS_TAG_OBJECT) {\n"
        "                JS_FreeValue(ctx, _t);\n"
        "                _r = _o;\n"
        "            } else {\n"
        "                if (JS_VALUE_HAS_REF_COUNT(_o)) JS_FreeValue(ctx, _o);\n"
        "                _r = _t;\n"
        "            }\n"
        "            if (JS_VALUE_GET_PTR(_r) == JS_VALUE_GET_PTR(_t))\n"
        "                qjs_aot_construct_done((JSObject *)JS_VALUE_GET_PTR(%s), _r);\n"
        "            /* The object goes into the frame slot, which AOT_SET_B below\n"
        "               writes with a duplicate: the construct path's own reference\n"
        "               is released here, or the object is never freed. */\n"
        "            if (JS_VALUE_HAS_REF_COUNT(_r)) JS_FreeValue(ctx, _r);\n",
        cexp, callee[k], ntexp, ntexp, cexp, cexp, callee[k], cexp, w, na, cexp);
    if (t->lb[dv] == E_LB_TAG) fprintf(f, "            v%u = _r;\n", (unsigned)dv);
    /* Neither guard held, or the object could not be made: the construct has not
       happened, so the whole copied body leaves for the call it was copied from,
       which does it. Falling through here would run the next instruction of the
       callee with the construct's result undefined. */
    fprintf(f, "            goto cd_%d;\n        }\n    }\n    goto vx_%d;\n#endif\n",
            in->pc, in->pc);
  }
  fprintf(f, "    cd_%d: ;\n", in->pc);
  e_inl_store_mark(f);
  g_e_miss_exit_seen = 1;
  cur[dreg] = dv;
  return 1;
}

static int emit_typed_construct(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur,
    int have_types, char hex[33], int fi, int b) {
  char callee[4][33];
  unsigned long long hits[4];
  int kind[4], n = 0, k, nb = 0, cc, nt, off, na, w, r, dreg;
  char cexp[64], ntexp[64];
  Q2ValueId dv, cv, nv;

  if (!have_types || t == NULL || g_e_fl || cur == NULL) return 0;
  if (in->rop != ROP_construct || in->ndef < 1 || in->narg < 5) return 0;
  if (e_roles[in->rop] == NULL || e_roles[in->rop]->control != E_CF_NONE) return 0;
  if (e_must_exit(fn, in)) return 0;
  if (!qjs_aot_prof_callees(fn->g->digest, in->next, &n, callee, kind, hits)) return 0;
  for (k = 0; k < n && k < 4; k++) {
    if (kind[k] != 1 || !qjs_aot_unit_has(callee[k])) continue;
    if (qjs_aot_light_need(callee[k]) <= 0) continue;
    if (in->win_n < qjs_aot_unit_argc(callee[k])) continue;
    nb++;
  }
  if (nb == 0) return 0;
  cc = (int)in->arg[1].val;
  nt = (int)in->arg[2].val;
  off = dest_off(fn, in);
  na = in->win_n;
  w = in->win_reg * 16;
  if (off < 0 || cc < 0 || nt < 0 || (unsigned)(in->win_reg + na) > (unsigned)fn->nregs) return 0;
  dv = in->def[0];
  if (dv >= (Q2ValueId)t->nvalues) return 0;
  if (t->lb[dv] != E_LB_TAG && !e_def_dead(in, dv)) return 0;
  dreg = off / 16;
  if (dreg < 0 || dreg >= fn->nregs) return 0;
  cv = cc / 16 < fn->nregs ? cur[cc / 16] : Q2_NO_VALUE;
  nv = nt / 16 < fn->nregs ? cur[nt / 16] : Q2_NO_VALUE;
  if (cv != Q2_NO_VALUE && cv != E_UNDEF && cv != E_INFRAME &&
      cv < (Q2ValueId)t->nvalues && t->lb[cv] == E_LB_TAG)
    snprintf(cexp, sizeof(cexp), "v%u", (unsigned)cv);
  else
    snprintf(cexp, sizeof(cexp), "AOT_REG(fr, %d)", cc);
  if (nv != Q2_NO_VALUE && nv != E_UNDEF && nv != E_INFRAME &&
      nv < (Q2ValueId)t->nvalues && t->lb[nv] == E_LB_TAG)
    snprintf(ntexp, sizeof(ntexp), "v%u", (unsigned)nv);
  else
    snprintf(ntexp, sizeof(ntexp), "AOT_REG(fr, %d)", nt);

  /* ---- output ---- */
  fprintf(f, "    /* sync live TAG values to the frame before the construct */\n");
  for (r = 0; r < fn->norig; r++) {
    Q2ValueId v = cur[r];
    if (v == Q2_NO_VALUE || v == E_UNDEF || v >= (Q2ValueId)t->nvalues) continue;
    if (t->lb[v] != E_LB_TAG) continue;
    fprintf(f, "    AOT_SET_B(ctx, fr, %d, v%u);\n", r * 16, (unsigned)v);
  }
  for (r = 0; r < na; r++) {
    Q2ValueId v;
    int wr = in->win_reg + r;
    if (wr >= fn->nregs) continue;
    v = cur[wr];
    if (v == Q2_NO_VALUE || v == E_UNDEF || v >= (Q2ValueId)t->nvalues) continue;
    if (t->lb[v] == E_LB_I32)
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, JS_MKVAL(JS_TAG_INT, i%u));\n", wr * 16, (unsigned)v);
    else if (t->lb[v] == E_LB_F64)
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, E_BOX_F64(d%u));\n", wr * 16, (unsigned)v);
    else if (t->lb[v] == E_LB_BOOL)
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, JS_MKVAL(JS_TAG_BOOL, b%u));\n", wr * 16, (unsigned)v);
  }
  for (k = 0; k < n && k < 4; k++) {
    if (kind[k] != 1 || !qjs_aot_unit_has(callee[k])) continue;
    if (qjs_aot_light_need(callee[k]) <= 0) continue;
    if (in->win_n < qjs_aot_unit_argc(callee[k])) continue;
    fprintf(f, "#ifdef Q2_NATIVE_%s\n", callee[k]);
    fprintf(
        f,
        "    if (__builtin_expect(q2_call_entry(%s, (const void *)aot_f_%s), 1) &&\n"
        "        JS_VALUE_GET_TAG(%s) == JS_TAG_OBJECT &&\n"
        "        JS_VALUE_GET_PTR(%s) == JS_VALUE_GET_PTR(%s)) {\n"
        "        JSValue _t, _o, _r;\n"
        "        int _rc = qjs_aot_construct_this(ctx, (JSObject *)JS_VALUE_GET_PTR(%s), &_t);\n"
        "        if (__builtin_expect(_rc < 0, 0)) return qjs_aot_throw_here(ctx, f, UINT32_MAX);\n"
        "        if (_rc > 0) {\n"
        "            E_CAP(f, %d);\n"
        "            _o = aot_n_%s(ctx, f, (JSObject *)JS_VALUE_GET_PTR(%s), _t,\n"
        "                          &AOT_REG(fr, %d), %d);\n"
        "            if (__builtin_expect(JS_IsException(_o), 0)) {\n"
        "                JS_FreeValue(ctx, _t);\n"
        "                return JS_EXCEPTION;\n"
        "            }\n"
        "            if (%s JS_VALUE_GET_TAG(_o) == JS_TAG_OBJECT) {\n"
        "                JS_FreeValue(ctx, _t);\n"
        "                _r = _o;\n"
        "            } else {\n"
        "                if (JS_VALUE_HAS_REF_COUNT(_o)) JS_FreeValue(ctx, _o);\n"
        "                _r = _t;\n"
        "            }\n"
        "            if (JS_VALUE_GET_PTR(_r) == JS_VALUE_GET_PTR(_t))\n"
        "                qjs_aot_construct_done((JSObject *)JS_VALUE_GET_PTR(%s), _r);\n"
        "            qjs_aot_set_reg_p(ctx, fr, %d, _r);\n",
        cexp, callee[k], ntexp, ntexp, cexp, cexp, in->next - 1, callee[k], cexp, w, na,
        getenv("QJS_AOT_CONSTRUCT_CONTROL") != NULL ? "0 &&" : "", cexp, off);
    e_emit_group_recheck(f, fn, in, cur);
    if (t->lb[dv] == E_LB_TAG) fprintf(f, "            v%u = _r;\n", (unsigned)dv);
    fprintf(f, "            goto cd_%d;\n        }\n    }\n#endif\n", in->pc);
  }
  g_e_presynced = 1;
  if (!emit_typed_opcall(f, fn, in, t, cur, NULL, hex, fi, b)) {
    fprintf(stderr, "qjsaot: construct at pc %d: the wrapper declined after output\n", in->pc);
    abort();
  }
  g_e_presynced = 0;
  fprintf(f, "    cd_%d: ;\n", in->pc);
  cur[dreg] = dv;
  return 1;
}

/* ---- allocating ops without the step handler -----------
   `new_object`, `object_template`, `array_from`, `special_object` (`arguments` and the
   family) and `apply_arguments` call the engine's own bodies for the op (patch 0317)
   and take the owned result, instead of stepping the handler. Allocation is a
   safepoint: the live TAG values and the window are in the frame first, the groups
   are re-checked after, an exception leaves like a call's. A case the helper leaves to
   the handler (import.meta, a reified `arguments`, a non-builtin `apply`) takes the
   op-call wrapper. Framed bodies only. */
/* Stores through an engine helper, with no result: `put_global` (the engine's global
   store, which can run a setter on the global object), `put_local_s0..3` and
   `put_local` (a variable slot, whose old value is released) and `local_uninit`.
   Each is a safepoint, so the live TAG values and the typed source are in the frame
   first, and the shape groups are rechecked after. */
/* `put_global` in a frameless body: the engine's global store of a borrowed value. The
   store can run a setter on the global object, so it is a safepoint: the live TAG values
   nothing else roots go into their slots of the published array first. The bytecode's
   atom operand is read the way a framed body reads it, from the live code. */
static int emit_fl_put_global(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur) {
  Q2ValueId spv[E_SPILL_MAX + 1], vv;
  int nsp = 0, r, k, vreg;
  uint64_t *lb;
  char vb[64];

  if (t == NULL || cur == NULL) return 0;
  if (in->narg < 3 || in->arg[0].kind != 'a' || in->arg[1].kind != 'r' || in->arg[0].off < 0)
    return 0;
  vreg = E_ROFF(in->arg[1]) / 16;
  if (vreg < 0 || vreg >= fn->nregs) return 0;
  vv = cur[vreg];
  if (vv == Q2_NO_VALUE || vv == E_UNDEF || vv == E_INFRAME || vv >= (Q2ValueId)t->nvalues) {
    if (vv == E_UNDEF) snprintf(vb, sizeof(vb), "JS_UNDEFINED");
    else return 0;
  } else if (t->lb[vv] == E_LB_TAG) {
    snprintf(vb, sizeof(vb), "v%u", (unsigned)vv);
  } else if (t->lb[vv] == E_LB_I32) {
    snprintf(vb, sizeof(vb), "JS_MKVAL(JS_TAG_INT, i%u)", (unsigned)vv);
  } else if (t->lb[vv] == E_LB_F64) {
    snprintf(vb, sizeof(vb), "E_BOX_F64(d%u)", (unsigned)vv);
  } else if (t->lb[vv] == E_LB_BOOL) {
    snprintf(vb, sizeof(vb), "JS_MKVAL(JS_TAG_BOOL, b%u)", (unsigned)vv);
  } else {
    return 0;
  }
  lb = calloc((size_t)(fn->lwords > 0 ? fn->lwords : 1), 8);
  if (lb == NULL) abort();
  q2_live_before(fn, (int)(in - fn->insns) + 1 < fn->ninsns ? (int)(in - fn->insns) + 1 : (int)(in - fn->insns), lb);
  for (r = 0; r < fn->norig; r++) {
    Q2ValueId v;
    if (!((lb[r / 64] >> (r % 64)) & 1)) continue;
    v = cur[r];
    if (v == Q2_NO_VALUE || v == E_UNDEF || v == E_INFRAME || v >= (Q2ValueId)t->nvalues ||
        t->lb[v] != E_LB_TAG)
      continue;
    if (g_e_rooted != NULL && g_e_rooted[v]) continue;
    if (e_spill_slot(v) < 0) {
      free(lb);
      e_fl_block("too many values live across a global store", in);
      return 0;
    }
    spv[nsp++] = v;
  }
  free(lb);
  /* The value stored is read by the store itself: it is rooted by its own register or
     local for the call. */
  if (t->lb[vv] == E_LB_TAG && (g_e_rooted == NULL || !g_e_rooted[vv]) &&
      e_spill_slot(vv) >= 0) {
    int dup = 0;
    for (k = 0; k < nsp; k++)
      if (spv[k] == vv) dup = 1;
    if (!dup) spv[nsp++] = vv;
  }
  g_e_fl_calls = 1;
  /* A store the global cache serves is a lookup and a store; anything else takes the
     engine's. The registers the decoder files as redefined keep their value, now under
     their new names. */
  if ((int)in->arg[2].val == 0)
    fprintf(
        f,
        "    if (__builtin_expect(!q2_put_global_hit(ctx, qjs_aot_ld_u32(*(const uint8_t *const *)(cb + "
        "QJS_AOT_OFF_FNBC_REGVM_CODE) + %d), %s), 0)) {\n",
        in->arg[0].off, vb);
  else
    fprintf(f, "    {\n");
  for (k = 0; k < nsp; k++)
    fprintf(f, "    ra_[%d] = v%u;\n", e_spill_slot(spv[k]), (unsigned)spv[k]);
  fprintf(
      f,
      "    rec_->pc = %d;\n"
      "    if (__builtin_expect(qjs_aot_put_global(ctx, (void *)cb, (int)qjs_aot_ld_u32(*(const uint8_t *const *)(cb + "
      "QJS_AOT_OFF_FNBC_REGVM_CODE) + %d), %s, %d) < 0, 0)) return JS_EXCEPTION;\n"
      "    }\n",
      in->next - 1, in->arg[0].off, vb, (int)in->arg[2].val);
  g_e_inl_dirty = 1;
  for (k = 0; k < in->nredef; k++) {
    Q2ValueId rv = in->redef_val[k], src;
    int rr = in->redef_reg[k];
    char nm[32];
    if (rv == Q2_NO_VALUE || rv >= (Q2ValueId)t->nvalues || rr < 0 || rr >= fn->nregs) continue;
    src = cur[rr];
    if (src == Q2_NO_VALUE || src == E_UNDEF || src == E_INFRAME || src >= (Q2ValueId)t->nvalues)
      continue;
    e_local_name(nm, sizeof(nm), rv);
    fprintf(f, "    %s = %s;\n", nm, e_copy_expr(src, rv));
    cur[rr] = rv;
  }
  e_emit_group_recheck(f, fn, in, cur);
  return 1;
}

static int emit_store_call(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur,
    int have_types) {
  int k, slot = -1;
  char call[300];
  if (g_e_fl && !g_e_inl && have_types && in->rop == ROP_put_global &&
      e_roles[in->rop] != NULL && e_roles[in->rop]->control == E_CF_NONE &&
      !e_must_exit(fn, in))
    return emit_fl_put_global(f, fn, in, t, cur);
  if (!have_types || t == NULL || g_e_fl || cur == NULL) return 0;
  if (e_roles[in->rop] == NULL || e_roles[in->rop]->control != E_CF_NONE) return 0;
  if (e_must_exit(fn, in)) return 0;
  switch (in->rop) {
    case ROP_put_global:
      if (in->narg < 3 || in->arg[0].kind != 'a' || in->arg[1].kind != 'r') return 0;
      if (E_ROFF(in->arg[1]) / 16 >= fn->nregs || in->arg[0].off < 0) return 0;
      snprintf(call, sizeof(call),
               "qjs_aot_put_global(ctx, *(void **)((uint8_t *)f + QJS_AOT_OFF_REGRUN_B), (int)qjs_aot_ld_u32(q2_code(f) + %d), AOT_REG(fr, %d), %d) < 0",
               in->arg[0].off, E_ROFF(in->arg[1]), (int)in->arg[2].val);
      break;
    case ROP_put_local_s0:
    case ROP_put_local_s1:
    case ROP_put_local_s2:
    case ROP_put_local_s3:
      slot = in->rop - ROP_put_local_s0;
      if (in->narg < 1 || in->arg[0].kind != 'r' || E_ROFF(in->arg[0]) / 16 >= fn->nregs)
        return 0;
      snprintf(call, sizeof(call), "(qjs_aot_put_local(ctx, q2_var_buf(f), %d, AOT_REG(fr, %d)), 0)",
               slot, E_ROFF(in->arg[0]));
      break;
    case ROP_put_local_xfer_s0:
    case ROP_put_local_xfer_s1:
    case ROP_put_local_xfer_s2:
    case ROP_put_local_xfer_s3:
      /* The register keeps its value (it only borrows), so this is `put_local`. */
      slot = in->rop - ROP_put_local_xfer_s0;
      if (in->narg < 1 || in->arg[0].kind != 'r' || E_ROFF(in->arg[0]) / 16 >= fn->nregs)
        return 0;
      snprintf(call, sizeof(call), "(qjs_aot_put_local(ctx, q2_var_buf(f), %d, AOT_REG(fr, %d)), 0)",
               slot, E_ROFF(in->arg[0]));
      break;
    case ROP_put_local:
    case ROP_put_local_xfer:
      if (in->narg < 2 || in->arg[0].kind != 'u' || in->arg[1].kind != 'r' ||
          E_ROFF(in->arg[1]) / 16 >= fn->nregs)
        return 0;
      snprintf(call, sizeof(call), "(qjs_aot_put_local(ctx, q2_var_buf(f), %d, AOT_REG(fr, %d)), 0)",
               (int)in->arg[0].val, E_ROFF(in->arg[1]));
      break;
    case ROP_local_uninit:
      if (in->narg < 1) return 0;
      snprintf(call, sizeof(call),
               "(qjs_aot_put_local(ctx, q2_var_buf(f), %d, JS_UNINITIALIZED), 0)",
               (int)in->arg[0].val);
      break;
    default:
      return 0;
  }
  /* A store the global cache serves is a lookup and a store, with no frame write. */
  if (in->rop == ROP_put_global && (int)in->arg[2].val == 0 && cur[E_ROFF(in->arg[1]) / 16] != Q2_NO_VALUE &&
      cur[E_ROFF(in->arg[1]) / 16] != E_INFRAME) {
    Q2ValueId vv = cur[E_ROFF(in->arg[1]) / 16];
    char vb[64];
    int ok = 1;
    if (vv == E_UNDEF) snprintf(vb, sizeof(vb), "JS_UNDEFINED");
    else if (vv >= (Q2ValueId)t->nvalues) ok = 0;
    else if (t->lb[vv] == E_LB_TAG) snprintf(vb, sizeof(vb), "v%u", (unsigned)vv);
    else if (t->lb[vv] == E_LB_I32) snprintf(vb, sizeof(vb), "JS_MKVAL(JS_TAG_INT, i%u)", (unsigned)vv);
    else if (t->lb[vv] == E_LB_F64) snprintf(vb, sizeof(vb), "E_BOX_F64(d%u)", (unsigned)vv);
    else if (t->lb[vv] == E_LB_BOOL) snprintf(vb, sizeof(vb), "JS_MKVAL(JS_TAG_BOOL, b%u)", (unsigned)vv);
    else ok = 0;
    if (ok) {
      fprintf(
          f, "    if (__builtin_expect(!q2_put_global_hit(ctx, qjs_aot_ld_u32(q2_code(f) + %d), %s), 0)) {\n",
          in->arg[0].off, vb);
      e_emit_presync(f, fn, in, t, cur);
      fprintf(f, "    if (__builtin_expect(%s, 0)) return qjs_aot_throw_here(ctx, f, %d);\n", call, in->pc);
      /* the registers the decoder files as redefined keep their value under their new names */
      for (k = 0; k < in->nredef; k++) {
        Q2ValueId rv = in->redef_val[k];
        int rr = in->redef_reg[k];
        if (rv == Q2_NO_VALUE || rv >= (Q2ValueId)t->nvalues || rr < 0 || rr >= fn->nregs ||
            t->lb[rv] != E_LB_TAG)
          continue;
        fprintf(f, "    v%u = AOT_REG(fr, %d);\n", (unsigned)rv, rr * 16);
      }
      /* only the engine's store is a safepoint: the cache hit runs no code, and keeps its values in locals */
      e_emit_group_recheck(f, fn, in, cur);
      fprintf(f, "    } else {\n");
      for (k = 0; k < in->nredef; k++) {
        Q2ValueId rv = in->redef_val[k], src;
        int rr = in->redef_reg[k];
        char nm[32];
        if (rv == Q2_NO_VALUE || rv >= (Q2ValueId)t->nvalues || rr < 0 || rr >= fn->nregs ||
            t->lb[rv] != E_LB_TAG)
          continue;
        src = cur[rr];
        if (src == Q2_NO_VALUE || src == E_UNDEF || src == E_INFRAME || src >= (Q2ValueId)t->nvalues)
          continue;
        e_local_name(nm, sizeof(nm), rv);
        fprintf(f, "    %s = %s;\n", nm, e_copy_expr(src, rv));
      }
      fprintf(f, "    }\n");
      return 1;
    }
  }
  e_emit_presync(f, fn, in, t, cur);
  fprintf(f, "    if (__builtin_expect(%s, 0)) return qjs_aot_throw_here(ctx, f, %d);\n", call, in->pc);
  /* The decoder files some of these as a fallback that redefines the live registers:
     each becomes a TAG local again from the frame, which the sync above filled. */
  for (k = 0; k < in->nredef; k++) {
    Q2ValueId rv = in->redef_val[k];
    int rr = in->redef_reg[k];
    if (rv == Q2_NO_VALUE || rv >= (Q2ValueId)t->nvalues || rr < 0 || rr >= fn->nregs ||
        t->lb[rv] != E_LB_TAG)
      continue;
    fprintf(f, "    v%u = AOT_REG(fr, %d);\n", (unsigned)rv, rr * 16);
  }
  e_emit_group_recheck(f, fn, in, cur);
  return 1;
}

/* Allocation in a frameless body: a new object (plain, from a template, or an array of
   registers) and the fill of a template object. The engine helpers take the function's
   bytecode, not a frame. The allocation is a safepoint, so the live TAG values nothing
   else roots go into their slots of the published array first, and the new object
   parks in the window before its count is dropped. */
static int emit_fl_alloc_op(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur) {
  Q2ValueId dv = Q2_NO_VALUE, spv[E_SPILL_MAX + 1], ov = Q2_NO_VALUE;
  int dreg = -1, na = 0, r, nsp = 0, k, resslot, fill = in->rop == ROP_object_fill;
  char call[256];
  uint64_t *lb, *la;

  if (t == NULL || cur == NULL) {
    e_fl_block("an allocation with no analysis", in);
    return 0;
  }
  if (e_must_exit(fn, in) || (fill && in->nredef > 0)) {
    e_fl_block("an allocation the facts keep for the handler", in);
    return 0;
  }
  if (in->win_n > 0) {
    if ((unsigned)(in->win_reg + in->win_n) > (unsigned)fn->nregs) return 0;
    na = in->win_n;
  }
  switch (in->rop) {
    case ROP_new_object:
      snprintf(call, sizeof(call), "(_o = JS_NewObject(ctx), JS_IsException(_o) ? -1 : 0)");
      break;
    case ROP_object_template:
      if (in->narg < 2) return 0;
      snprintf(
          call, sizeof(call),
          "qjs_aot_object_template_b(ctx, (struct JSFunctionBytecode *)cb, %d, &_o)",
          (int)in->arg[1].val);
      break;
    case ROP_array_from:
      if (in->win_n < 0) return 0;
      snprintf(call, sizeof(call), "qjs_aot_array_from(ctx, &ra_[SB_], %d, &_o)", na);
      break;
    case ROP_object_fill: {
      int oreg;
      if (in->narg < 4 || in->arg[0].kind != 'r' || in->win_n < 0) return 0;
      oreg = (int)in->arg[0].val / 16;
      if (oreg < 0 || oreg >= fn->nregs) return 0;
      ov = cur[oreg];
      if (ov == Q2_NO_VALUE || ov == E_UNDEF || ov == E_INFRAME ||
          ov >= (Q2ValueId)t->nvalues || t->lb[ov] != E_LB_TAG) {
        e_fl_block("a fill of an object that is not a TAG value", in);
        return 0;
      }
      snprintf(
          call, sizeof(call),
          "(_o = JS_UNDEFINED, qjs_aot_object_fill_b(ctx, (struct JSFunctionBytecode *)cb, "
          "v%u, &ra_[SB_], %d, %d))",
          (unsigned)ov, na, (int)in->arg[3].val);
      break;
    }
    default:
      e_fl_block("an allocation this body cannot make", in);
      return 0;
  }
  if (!fill) {
    if (in->ndef >= 1) {
      int off = dest_off(fn, in);
      if (off < 0) return 0;
      dv = in->def[0];
      dreg = off / 16;
    } else if (in->nredef >= 1 && in->redef_reg != NULL && in->redef_val != NULL &&
               in->narg >= 1 && in->arg[0].kind == 'r') {
      /* The decoder files the op as a fallback that redefines the registers it may
         rewrite: the destination is its first operand, the others keep their value. */
      int k2;
      dreg = (int)in->arg[0].val / 16;
      for (k2 = 0; k2 < in->nredef; k2++)
        if (in->redef_reg[k2] == dreg) dv = in->redef_val[k2];
      if (dv == Q2_NO_VALUE) return 0;
    } else {
      return 0;
    }
    if (dv >= (Q2ValueId)t->nvalues || dreg < 0 || dreg >= fn->nregs) return 0;
    if (t->lb[dv] != E_LB_TAG && !e_def_dead(in, dv)) {
      e_fl_block("an allocation whose result is typed", in);
      return 0;
    }
  }
  resslot = -1;
  if (!fill && t->lb[dv] == E_LB_TAG && !e_def_dead(in, dv)) {
    resslot = e_spill_slot(dv);
    if (resslot < 0) {
      e_fl_block("too many values live across an allocation", in);
      return 0;
    }
  }
  /* Spill: the TAG values live across the op that nothing roots. The window values are
     consumed by it (a fill clears them), so they are not live after unless read again. */
  lb = calloc((size_t)(fn->lwords > 0 ? fn->lwords : 1), 8);
  la = calloc((size_t)(fn->lwords > 0 ? fn->lwords : 1), 8);
  if (lb == NULL || la == NULL) abort();
  q2_live_before(fn, (int)(in - fn->insns), lb);
  if ((int)(in - fn->insns) + 1 < fn->ninsns)
    q2_live_before(fn, (int)(in - fn->insns) + 1, la);
  for (r = 0; r < fn->norig; r++) {
    Q2ValueId v;
    int is_win = r >= in->win_reg && r < in->win_reg + na;
    if (!((lb[r / 64] >> (r % 64)) & 1)) continue;
    if (is_win && !((la[r / 64] >> (r % 64)) & 1)) continue;
    v = cur[r];
    if (v == Q2_NO_VALUE || v == E_UNDEF || v == E_INFRAME ||
        v >= (Q2ValueId)t->nvalues || t->lb[v] != E_LB_TAG)
      continue;
    if (g_e_rooted != NULL && g_e_rooted[v]) continue;
    if (e_spill_slot(v) < 0) {
      free(lb);
      free(la);
      e_fl_block("too many values live across an allocation", in);
      return 0;
    }
    spv[nsp++] = v;
  }
  free(lb);
  free(la);

  /* ---- output ---- */
  g_e_fl_calls = 1;
  for (k = 0; k < nsp; k++)
    fprintf(f, "    ra_[%d] = v%u;\n", e_spill_slot(spv[k]), (unsigned)spv[k]);
  for (r = 0; r < na; r++) e_emit_fl_window_store(f, fn, t, cur, in->win_reg + r, r);
  fprintf(f, "    rec_->pc = %d;\n", in->next - 1);
  fprintf(f, "    { JSValue _o; int _rc = %s;\n", call);
  fprintf(f, "      if (__builtin_expect(_rc < 0, 0)) return JS_EXCEPTION;\n");
  if (!fill) {
    if (t->lb[dv] == E_LB_TAG && !e_def_dead(in, dv))
      fprintf(
          f,
          "      if (JS_VALUE_HAS_REF_COUNT(_o)) qjs_aot_set_reg_p(ctx, &ra_[%d], 0, _o);\n"
          "      else ra_[%d] = _o;\n"
          "      v%u = _o;\n"
          "      AOT_SET_B(ctx, fr, %d, _o);\n",
          resslot, resslot, (unsigned)dv, dreg * 16);
    else
      fprintf(f, "      if (JS_VALUE_HAS_REF_COUNT(_o)) JS_FreeValue(ctx, _o);\n");
  }
  fprintf(f, "    }\n");
  /* The other registers the fallback form lists keep their value. */
  for (k = 0; k < in->nredef; k++) {
    Q2ValueId rv = in->redef_val[k], src;
    int rr = in->redef_reg[k];
    char nm[32];
    if (rr == dreg || rv == Q2_NO_VALUE || rv >= (Q2ValueId)t->nvalues || rr < 0 ||
        rr >= fn->nregs)
      continue;
    src = cur[rr];
    if (src == Q2_NO_VALUE || src == E_UNDEF || src == E_INFRAME ||
        src >= (Q2ValueId)t->nvalues)
      continue;
    e_local_name(nm, sizeof(nm), rv);
    fprintf(f, "    %s = %s;\n", nm, e_copy_expr(src, rv));
    cur[rr] = rv;
  }
  e_emit_group_recheck(f, fn, in, cur);
  g_e_inl_dirty = 1;
  if (!fill) cur[dreg] = dv;
  return 1;
}

/* Whether the string that `s += <constant>` extends can be held by no other register. Compiled
   code then tells the engine to skip its scan of the frame for another holder. The proof is
   about the data flow of the function:
   - where the string comes from: either a constant (never a register-only string, so the first
     append makes a fresh one) or the result of this very append (a loop-carried accumulator);
   - where it goes: the append, a length read, a compare or a branch, and the loop-carried
     register itself. Any other use of the old or the new value (a move, a call argument or
     receiver, an edge into a different register) may leave a second register holding it. */
static int e_append_unique(const Q2Func *fn, const Q2Insn *in) {
  Q2ValueId vo, vn;
  int b, k, si, x, u, reg;

  if (in->nuse < 1 || in->ndef < 1) return 0;
  vo = in->use[0];
  vn = in->def[0];
  if (vo >= (Q2ValueId)fn->nvalues || vn >= (Q2ValueId)fn->nvalues) return 0;
  reg = fn->values[vo].reg;
  if (reg < 0 || reg != fn->values[vn].reg) return 0;
  /* the sources of the old value */
  {
    int is_param = 0;
    for (b = 0; b < fn->nblocks && !is_param; b++)
      for (k = 0; k < fn->blocks[b].nparams; k++)
        if (fn->blocks[b].param_val[k] == vo) {
          int pb;
          is_param = 1;
          for (pb = 0; pb < fn->nblocks; pb++) {
            const Q2Block *p = &fn->blocks[pb];
            for (si = 0; si < p->nsucc && si < 2; si++) {
              Q2ValueId av;
              const Q2Insn *ai;
              if (p->succ_block[si] != b || k >= p->nedge_args[si]) continue;
              av = p->edge_arg[si][k];
              if (av == vn || av == vo) continue;
              if (av == Q2_NO_VALUE || av >= (Q2ValueId)fn->nvalues ||
                  fn->values[av].insn < 0)
                return 0;
              ai = &fn->insns[fn->values[av].insn];
              if (ai->op != Q2_OP_CONST && ai->rop != ROP_load_const) return 0;
            }
          }
          break;
        }
    if (!is_param) {
      const Q2Insn *di;
      if (fn->values[vo].insn < 0) return 0;
      di = &fn->insns[fn->values[vo].insn];
      if (di->op != Q2_OP_CONST && di->rop != ROP_load_const) return 0;
    }
  }
  /* the uses of the old and the new value */
  for (x = 0; x < fn->ninsns; x++) {
    const Q2Insn *xi = &fn->insns[x];
    if (xi == in) continue;
    for (u = 0; u < xi->nuse; u++) {
      if (xi->use[u] != vo && xi->use[u] != vn) continue;
      if (xi->rop == ROP_get_length || xi->op == Q2_OP_BRANCH ||
          xi->op == Q2_OP_BRANCH_CMP || xi->op == Q2_OP_COMPARE)
        continue;
      return 0;
    }
    for (u = 0; u < xi->win_n && xi->win_val != NULL; u++)
      if (xi->win_val[u] == vo || xi->win_val[u] == vn) return 0;
  }
  for (b = 0; b < fn->nblocks; b++) {
    const Q2Block *p = &fn->blocks[b];
    for (si = 0; si < p->nsucc && si < 2; si++) {
      const Q2Block *sb;
      if (p->succ_block[si] < 0 || p->succ_block[si] >= fn->nblocks) continue;
      sb = &fn->blocks[p->succ_block[si]];
      for (k = 0; k < p->nedge_args[si] && k < sb->nparams; k++) {
        Q2ValueId av = p->edge_arg[si][k];
        if ((av == vo || av == vn) && sb->param_reg[k] != reg) return 0;
      }
    }
  }
  return 1;
}

/* The code offset of the atom operand of the `load_const` a `get_element` has absorbed
   (see e_try_fuse_atom_get), or -1. */
static int g_e_fuse_atom_off = -1;

static int emit_alloc_op(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur,
    int have_types, char hex[33], int fi, int b) {
  Q2ValueId dv;
  int off, dreg, r, na = 0, handler_ok = 0;
  const char *okc = "1";
  int redef_mode = 0, nodest = in->rop == ROP_object_fill, src_off = -1, src_off2 = -1;
  char call[256];

  if (g_e_fl && !g_e_inl && have_types &&
      (in->rop == ROP_new_object || in->rop == ROP_object_template ||
       in->rop == ROP_array_from || in->rop == ROP_object_fill))
    return emit_fl_alloc_op(f, fn, in, t, cur);
  if (!have_types || t == NULL || g_e_fl || cur == NULL) return 0;
  if (e_roles[in->rop] == NULL || e_roles[in->rop]->control != E_CF_NONE) return 0;
  if (e_must_exit(fn, in)) return 0;
  if (nodest) {
    /* object_fill stores into an existing object: no destination. */
    if (in->narg < 4 || in->arg[0].kind != 'r' || in->win_n < 0) return 0;
    if ((unsigned)(in->win_reg + in->win_n) > (unsigned)fn->nregs) return 0;
    dv = Q2_NO_VALUE;
    off = (int)in->arg[0].val;
    dreg = off / 16;
    redef_mode = 1;
  } else if (in->ndef >= 1) {
    dv = in->def[0];
    off = dest_off(fn, in);
  } else if (in->nredef >= 1 && in->redef_reg != NULL && in->redef_val != NULL) {
    /* The decoder files some ops as a fallback that redefines the destination. */
    /* The destination is the first operand's register; the redef list holds every
       register the fallback may rewrite. */
    int k2, dr;
    if (in->narg < 1 || in->arg[0].kind != 'r') return 0;
    dr = (int)in->arg[0].val / 16;
    dv = Q2_NO_VALUE;
    for (k2 = 0; k2 < in->nredef; k2++)
      if (in->redef_reg[k2] == dr) dv = in->redef_val[k2];
    if (dv == Q2_NO_VALUE) return 0;
    off = dr * 16;
    redef_mode = 1;
  } else {
    return 0;
  }
  if (!nodest && dv >= (Q2ValueId)t->nvalues) return 0;
  if (off < 0) return 0;
  dreg = off / 16;
  if (dreg >= fn->nregs) return 0;
  if (!nodest && !redef_mode && t->lb[dv] != E_LB_TAG && t->lb[dv] != E_LB_I32 &&
      t->lb[dv] != E_LB_F64 && t->lb[dv] != E_LB_BOOL && !e_def_dead(in, dv))
    return 0;
  if (in->win_n > 0 || in->rop == ROP_apply_arguments) {
    if ((unsigned)(in->win_reg + in->win_n) > (unsigned)fn->nregs) return 0;
    na = in->win_n;
  }
  switch (in->rop) {
    case ROP_new_object:
      snprintf(call, sizeof(call), "(_o = JS_NewObject(ctx), JS_IsException(_o) ? -1 : 0)");
      break;
    case ROP_object_template:
      if (in->narg < 2) return 0;
      snprintf(call, sizeof(call), "qjs_aot_object_template(ctx, f, %d, &_o)", (int)in->arg[1].val);
      break;
    case ROP_array_from:
      if (in->win_n < 0) return 0;
      snprintf(call, sizeof(call), "qjs_aot_array_from(ctx, &AOT_REG(fr, %d), %d, &_o)",
               in->win_reg * 16, in->win_n);
      break;
    case ROP_array_const:
      if (in->narg < 2) return 0;
      snprintf(call, sizeof(call), "qjs_aot_array_const(ctx, f, %d, &_o)", (int)in->arg[1].val);
      break;
    case ROP_get_super:
      /* "rr": destination, object: the object's prototype. */
      if (in->narg < 2 || in->arg[1].kind != 'r' || E_ROFF(in->arg[1]) / 16 >= fn->nregs)
        return 0;
      src_off = E_ROFF(in->arg[1]);
      snprintf(call, sizeof(call), "qjs_aot_get_proto(ctx, AOT_REG(fr, %d), &_o)", src_off);
      break;
    case ROP_get_element:
      /* Only the fused form (a constant string key, see e_try_fuse_atom_get): the read by
         atom with the object as the receiver. */
      if (g_e_fuse_atom_off < 0 || in->narg < 3 || in->arg[1].kind != 'r' ||
          E_ROFF(in->arg[1]) / 16 >= fn->nregs)
        return 0;
      src_off = E_ROFF(in->arg[1]);
      snprintf(call, sizeof(call),
               "qjs_aot_get_prop_atom(ctx, AOT_REG(fr, %d), qjs_aot_ld_u32(q2_code(f) + %d), &_o)",
               src_off, g_e_fuse_atom_off);
      break;
    case ROP_unary:
      /* "rbr": destination, sub-op, source. The engine's general unary op; the typed
         forms were tried first. */
      if (in->narg < 3 || in->arg[2].kind != 'r' || E_ROFF(in->arg[2]) / 16 >= fn->nregs)
        return 0;
      src_off = E_ROFF(in->arg[2]);
      snprintf(call, sizeof(call), "qjs_aot_unary(ctx, %d, AOT_REG(fr, %d), &_o)",
               (int)in->arg[1].val, src_off);
      break;
    case ROP_binary_cpool:
      /* "rbrbc": destination, sub-op, left operand, kind, pool index or atom. */
      if (in->narg < 5 || in->arg[2].kind != 'r' || E_ROFF(in->arg[2]) / 16 >= fn->nregs)
        return 0;
      src_off = E_ROFF(in->arg[2]);
      if (in->arg[3].val == 0)
        snprintf(call, sizeof(call), "qjs_aot_binary_cpool(ctx, f, %d, %uu, 0, AOT_REG(fr, %d), %d, %d, &_o)",
                 (int)in->arg[1].val, (unsigned)in->arg[4].val, src_off, dreg, src_off / 16);
      else if (in->arg[4].off >= 0)
        snprintf(call, sizeof(call),
                 "qjs_aot_binary_cpool(ctx, f, %d, qjs_aot_ld_u32(q2_code(f) + %d), 1, AOT_REG(fr, %d), %d, %d, &_o)",
                 (int)in->arg[1].val, in->arg[4].off, src_off, dreg, src_off / 16);
      else
        return 0;
      break;
    case ROP_add:
    case ROP_sub:
    case ROP_mul:
    case ROP_div:
    case ROP_mod: {
      /* "rrr": the arithmetic the typed forms did not take (a string, an object, a mixed
         pair) runs in the engine's own routine, not through the step handler. */
      int ao, bo;
      if (e_roles[in->rop]->nuse != 2 || in->sub < 0) return 0;
      ao = operand_off(in, e_roles[in->rop]->use[0]);
      bo = operand_off(in, e_roles[in->rop]->use[1]);
      if (ao < 0 || bo < 0 || ao / 16 >= fn->nregs || bo / 16 >= fn->nregs) return 0;
      src_off = ao;
      src_off2 = bo;
      snprintf(call, sizeof(call),
               "qjs_aot_binary_rr(ctx, f, %d, AOT_REG(fr, %d), AOT_REG(fr, %d), %d, %d, &_o)",
               (int)in->sub, ao, bo, dreg, ao / 16);
      break;
    }
    case ROP_load_const:
      if (in->narg < 3 || in->arg[2].kind != 'c') return 0;
      /* An atom operand is the code's own word: atoms are renumbered at load. */
      if (in->arg[1].val != 0 && in->arg[2].off < 0) return 0;
      if (in->arg[1].val == 0)
        snprintf(call, sizeof(call), "qjs_aot_load_const(ctx, f, 0, %uu, &_o)",
                 (unsigned)in->arg[2].val);
      else
        snprintf(call, sizeof(call),
                 "qjs_aot_load_const(ctx, f, 1, qjs_aot_ld_u32(q2_code(f) + %d), &_o)",
                 in->arg[2].off);
      break;
    case ROP_object_fill:
      snprintf(call, sizeof(call),
               "(_o = JS_UNDEFINED, qjs_aot_object_fill(ctx, f, AOT_REG(fr, %d), &AOT_REG(fr, %d), %d, %d))",
               off, in->win_reg * 16, in->win_n, (int)in->arg[3].val);
      break;
    case ROP_special_object:
      if (in->narg < 2) return 0;
      /* Negative control: "unmapped" builds the unmapped form for mapped arguments,
         so a store through `arguments` no longer reaches the parameter. */
      snprintf(call, sizeof(call), "qjs_aot_special_object(ctx, f, %d, &_o)",
               (getenv("QJS_AOT_ALLOC_CONTROL") != NULL && (int)in->arg[1].val == 1)
                   ? 0 : (int)in->arg[1].val);
      handler_ok = 1;
      okc = "_rc == 0"; /* 1: a kind the handler keeps */
      break;
    case ROP_apply_arguments:
      if (in->win_n < 3) return 0;
      snprintf(call, sizeof(call), "qjs_aot_apply_arguments(ctx, f, &AOT_REG(fr, %d), &_o)",
               in->win_reg * 16);
      handler_ok = 1;
      okc = "_rc > 0"; /* 0: not the builtin apply, or a reified arguments object */
      break;
    default:
      return 0;
  }

  /* ---- output ---- */
  /* Two tagged numbers combine inline; the engine's routine takes what is left. */
  if ((in->rop == ROP_add || in->rop == ROP_sub || in->rop == ROP_mul ||
       in->rop == ROP_div || in->rop == ROP_mod) && !nodest &&
      !redef_mode && in->nuse == 2 && t->lb[dv] == E_LB_TAG &&
      in->use[0] < (Q2ValueId)t->nvalues && in->use[1] < (Q2ValueId)t->nvalues &&
      (t->lb[in->use[0]] == E_LB_TAG || t->lb[in->use[0]] == E_LB_I32 ||
       t->lb[in->use[0]] == E_LB_F64) &&
      (t->lb[in->use[1]] == E_LB_TAG || t->lb[in->use[1]] == E_LB_I32 ||
       t->lb[in->use[1]] == E_LB_F64)) {
    char xa[64], xb[64];
    e_boxed_operand(xa, sizeof(xa), t, in->use[0]);
    e_boxed_operand(xb, sizeof(xb), t, in->use[1]);
    fprintf(f,
            "    { JSValue _r0;\n"
            "      if (__builtin_expect(q2_arith(%d, %s, %s, &_r0), 1)) {\n"
            "        v%u = _r0;\n"
            "        goto cd_%d;\n"
            "      }\n"
            "    }\n",
            (int)in->sub, xa, xb, (unsigned)dv, in->pc);
  }
  /* `s += 'constant'` with the result in the operand's register: try the in-place append
     before the general sequence. Only the operand needs to be in the frame for it. */
  /* `s += 'constant'` with the result in the operand's register: try the in-place append
     before the general sequence. Only the operand needs to be in the frame for it. */
  if (in->rop == ROP_binary_cpool && !nodest && !redef_mode && in->nuse == 1 && in->narg >= 5 &&
      in->arg[1].val == REG_BIN_ADD && in->arg[2].kind == 'r' && E_ROFF(in->arg[2]) == off &&
      t->lb[dv] == E_LB_TAG && in->use[0] < (Q2ValueId)t->nvalues &&
      t->lb[in->use[0]] == E_LB_TAG &&
      (in->arg[3].val == REG_CONST_CPOOL || in->arg[4].off >= 0) && e_append_unique(fn, in)) {
    char idx[96];
    if (in->arg[3].val == REG_CONST_CPOOL)
      snprintf(idx, sizeof(idx), "%uu", (unsigned)in->arg[4].val);
    else
      snprintf(idx, sizeof(idx), "qjs_aot_ld_u32(q2_code(f) + %d)", in->arg[4].off);
    fprintf(f,
            "    { int _ap;\n"
            "      AOT_SET_B(ctx, fr, %d, v%u);\n"
            "      _ap = qjs_aot_append_const(ctx, f, %d, %s, %d, %d);\n"
            "      if (_ap > 0) { v%u = AOT_REG(fr, %d); goto cd_%d; }\n"
            "      if (__builtin_expect(_ap < 0, 0)) return qjs_aot_throw_here(ctx, f, %d);\n"
            "    }\n",
            off, (unsigned)in->use[0], (int)in->arg[3].val, idx, dreg, 1,
            (unsigned)dv, off, in->pc, in->pc);
  }
  fprintf(f, "    /* sync live TAG values to the frame before the allocation */\n");
  for (r = 0; r < fn->norig; r++) {
    Q2ValueId v = cur[r];
    if (v == Q2_NO_VALUE || v == E_UNDEF || v >= (Q2ValueId)t->nvalues) continue;
    if (t->lb[v] != E_LB_TAG) continue;
    fprintf(f, "    AOT_SET_B(ctx, fr, %d, v%u);\n", r * 16, (unsigned)v);
  }
  /* Typed values the op may read (its window) or that the reload below takes from the
     frame (the registers a fallback-shaped op redefines) are boxed into the frame. */
  for (r = 0; r < fn->norig; r++) {
    Q2ValueId v;
    int need = (r >= in->win_reg && r < in->win_reg + na) || r * 16 == src_off ||
               r * 16 == src_off2;
    int k2;
    for (k2 = 0; k2 < in->nredef; k2++)
      if (in->redef_reg != NULL && in->redef_reg[k2] == r) need = 1;
    if (!need) continue;
    v = cur[r];
    if (v == Q2_NO_VALUE || v == E_UNDEF || v >= (Q2ValueId)t->nvalues) continue;
    if (t->lb[v] == E_LB_I32)
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, JS_MKVAL(JS_TAG_INT, i%u));\n", r * 16, (unsigned)v);
    else if (t->lb[v] == E_LB_F64)
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, E_BOX_F64(d%u));\n", r * 16, (unsigned)v);
    else if (t->lb[v] == E_LB_BOOL)
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, JS_MKVAL(JS_TAG_BOOL, b%u));\n", r * 16, (unsigned)v);
  }
  fprintf(f, "    { JSValue _o; int _rc = %s;\n", call);
  fprintf(f, "      if (__builtin_expect(_rc < 0, 0)) return qjs_aot_throw_here(ctx, f, %d);\n", in->pc);
  fprintf(f, "      if (%s) {\n", okc);
  if (!nodest)
    fprintf(
        f,
        "        if (_rc == 2) _o = AOT_REG(fr, %d);\n"
        "        else if (JS_VALUE_HAS_REF_COUNT(_o)) qjs_aot_set_reg_p(ctx, fr, %d, _o);\n"
        "        else AOT_SET_B(ctx, fr, %d, _o);\n",
        off, off, off);
  e_emit_group_recheck(f, fn, in, cur);
  if (redef_mode) {
    /* every register the op redefines becomes a TAG local again from the frame */
    int k2;
    for (k2 = 0; k2 < in->nredef; k2++) {
      Q2ValueId rv2 = in->redef_val[k2];
      int rr = in->redef_reg[k2];
      if (rv2 == Q2_NO_VALUE || rv2 >= (Q2ValueId)t->nvalues || rr < 0 || rr >= fn->nregs ||
          t->lb[rv2] != E_LB_TAG)
        continue;
      fprintf(f, "        v%u = AOT_REG(fr, %d);\n", (unsigned)rv2, rr * 16);
    }
  } else if (t->lb[dv] == E_LB_TAG) {
    fprintf(f, "        v%u = _o;\n", (unsigned)dv);
  } else if (e_def_dead(in, dv)) {
    /* nothing reads it */
  } else if (t->lb[dv] == E_LB_I32) {
    e_note_stub(g_e_dx, g_e_dx_cur, g_e_dx_blk, &g_e_dx_n, in->next, cur, in);
    e_sweep_guard(f, "dx", in->next);
    fprintf(
        f,
        "        if (__builtin_expect(!Q2_I32_OK(_o, i%u), 0)) goto dx_%d;\n",
        (unsigned)dv, in->next);
  } else if (t->lb[dv] == E_LB_BOOL) {
    e_note_stub(g_e_dx, g_e_dx_cur, g_e_dx_blk, &g_e_dx_n, in->next, cur, in);
    e_sweep_guard(f, "dx", in->next);
    fprintf(
        f,
        "        if (__builtin_expect(JS_VALUE_GET_TAG(_o) != JS_TAG_BOOL, 0)) goto dx_%d;\n"
        "        b%u = JS_VALUE_GET_BOOL(_o);\n",
        in->next, (unsigned)dv);
  } else {
    e_note_stub(g_e_dx, g_e_dx_cur, g_e_dx_blk, &g_e_dx_n, in->next, cur, in);
    e_sweep_guard(f, "dx", in->next);
    fprintf(
        f,
        "        if (JS_VALUE_GET_TAG(_o) == JS_TAG_INT) d%u = (double)JS_VALUE_GET_INT(_o);\n"
        "        else if (JS_VALUE_GET_TAG(_o) == JS_TAG_FLOAT64) d%u = JS_VALUE_GET_FLOAT64(_o);\n"
        "        else goto dx_%d;\n",
        (unsigned)dv, (unsigned)dv, in->next);
  }
  fprintf(f, "        goto cd_%d;\n      }\n    }\n", in->pc);
  if (handler_ok) {
    /* the handler takes the case the helper declined */
    g_e_presynced = 1;
    if (!emit_typed_opcall(f, fn, in, t, cur, NULL, hex, fi, b)) {
      fprintf(stderr, "qjsaot: alloc op at pc %d: the wrapper declined after output\n", in->pc);
      abort();
    }
    g_e_presynced = 0;
  }
  fprintf(f, "    cd_%d: ;\n", in->pc);
  if (!nodest) cur[dreg] = dv;
  return 1;
}

/* A SPECULATIVE-BODY CALL, mirroring `emit_native_call` + `emit_one_direct_call`:
   same eligibility, same call sequence, and the result taken with
   `qjs_aot_set_reg_p` because `aot_n_` hands back an OWNED reference.

   RULE: an emitter decides EVERYTHING before it writes a byte. This one used to
   emit the call and then return 0 for an unrepresentable def, so the dispatcher ALSO
   handed off -- and the callee ran twice. (Measured: smoke.js's `lc4` was one extra
   `lf1` call, i.e. exactly 2.5 too high per iteration.) So every check runs first.

   CONTROL FLOW: each candidate's success path ends at `cd_<pc>:`; the slow path -- no
   candidate matched, so the call has NOT run -- hands off at this pc, and `cd_<pc>:`
   follows. A failed `q2_call_entry` must never fall through with no call at all. */
/* ---- pure inline calls --------------------------------------------------
   A call whose profiled callees all have an inline body with no safepoint (no call, no
   allocation, no store that could add a property or grow an array) runs that body in place
   and otherwise leaves the speculative body at the call: no JS runs and no object changes
   shape, so the shape, cache-cell and array facts survive it. */
static char g_pure_inl[512][33];
static int g_pure_inl_n = 0;

void e_note_pure_inline(const char hex[33]) {
  if (g_pure_inl_n < 512) memcpy(g_pure_inl[g_pure_inl_n++], hex, 33);
}

static int pure_inline_is(const char *hex) {
  int i;
  for (i = 0; i < g_pure_inl_n; i++)
    if (strcmp(g_pure_inl[i], hex) == 0) return 1;
  return 0;
}

static int e_pure_call_site(const Q2Func *fn, const Q2Insn *in) {
  char callee[4][33];
  unsigned long long hits[4];
  int kind[4], n = 0, k;
  if (!call_site_shaped(fn, in)) return 0;
  if (e_nocall_on()) return 0;
  if (!qjs_aot_prof_callees(fn->g->digest, in->next, &n, callee, kind, hits)) return 0;
  if (n <= 0 || n > 4) return 0;
  for (k = 0; k < n; k++) {
    if (kind[k] != 1 || !qjs_aot_unit_has(callee[k]) || !qjs_aot_inline_has(callee[k]) ||
        !pure_inline_is(callee[k]) || in->win_n < qjs_aot_unit_argc(callee[k]))
      return 0;
  }
  return 1;
}

/* The guard of a copied callee: the callee value must be the compiled function the copy was made from
   (and, for a copy that reads `this`, the receiver an object, so the engine's boxing of a primitive
   receiver does not apply); anything else leaves for the call itself. */
static int emit_inl_guard(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur, int have_types) {
  const Q2Inl *iv = &fn->inl[in->inlg - 1];
  char callee[33], cexp[64], rexp[64];
  int cc = call_callee_off(in), th = call_this_off(in), q;
  Q2ValueId cv, tv;
  static const char *const hx = "0123456789abcdef";
  if (!have_types || t == NULL || cur == NULL) return 0;
  for (q = 0; q < 16; q++) {
    callee[2 * q] = hx[iv->g->digest[q] >> 4];
    callee[2 * q + 1] = hx[iv->g->digest[q] & 15];
  }
  callee[32] = 0;
  cv = cc / 16 < fn->nregs ? cur[cc / 16] : Q2_NO_VALUE;
  if (cv != Q2_NO_VALUE && cv != E_UNDEF && cv != E_INFRAME && cv < (Q2ValueId)t->nvalues &&
      t->lb[cv] == E_LB_TAG)
    snprintf(cexp, sizeof(cexp), "v%u", (unsigned)cv);
  else
    snprintf(cexp, sizeof(cexp), "AOT_REG(fr, %d)", cc);
  tv = th >= 0 && th / 16 < fn->nregs ? cur[th / 16] : Q2_NO_VALUE;
  if (th < 0)
    snprintf(rexp, sizeof(rexp), "JS_UNDEFINED");
  else if (tv != Q2_NO_VALUE && tv != E_UNDEF && tv != E_INFRAME && tv < (Q2ValueId)t->nvalues &&
           t->lb[tv] == E_LB_TAG)
    snprintf(rexp, sizeof(rexp), "v%u", (unsigned)tv);
  else
    snprintf(rexp, sizeof(rexp), "AOT_REG(fr, %d)", th);
  e_note_stub(g_e_vx, g_e_vx_cur, g_e_vx_blk, &g_e_vx_n, in->pc, cur, NULL);
  e_note_tag('v', in->pc, cexp);
  e_sweep_guard(f, "vx", in->pc);
  fprintf(
      f,
      "#ifdef Q2_SEL_%s\n"
      "    if (__builtin_expect(!%s%s, (const void *)aot_f_%s, %du), 0)) goto vx_%d;\n"
      "#else\n"
      "    goto vx_%d;\n"
      "#endif\n",
      callee, e_cc_fn(fn, in), cexp, callee, e_cc_ix(fn, in), in->pc, in->pc);
  e_virt_compute(fn);
  if (iv->uses_this && !(tv != Q2_NO_VALUE && tv != E_UNDEF && tv != E_INFRAME && tv < (Q2ValueId)fn->nvalues &&
                         g_e_virt != NULL && g_e_virt[tv]))
    fprintf(
        f, "    if (__builtin_expect(JS_VALUE_GET_TAG(%s) != JS_TAG_OBJECT, 0)) goto vx_%d;\n", rexp,
        in->pc);
  if (iv->g->pic_count > 0) {
    /* the callee's cache tables, shifted so its site numbers (above ours) index them */
    int ik = in->inlg - 1, gi, qq;
    fprintf(
        f,
        "    { const uint8_t *cbk_ = *(const uint8_t *const *)((const uint8_t *)JS_VALUE_GET_PTR(%s) + "
        "QJS_AOT_OFF_OBJECT_FUNC_BYTECODE);\n"
        "      const uint8_t *pr_ = *(const uint8_t *const *)(cbk_ + QJS_AOT_OFF_FNBC_REGVM_PIC);\n"
        "      if (__builtin_expect(pr_ == NULL, 0)) goto vx_%d;\n"
        "      ipic_%d = pr_ - (size_t)%d * QJS_AOT_SIZEOF_JSREGPIC;\n"
        "      iholders_%d = *(void *const *const *)(cbk_ + QJS_AOT_OFF_FNBC_REGVM_PIC_HOLDER) - %d;\n"
        "      ins_%d = (uint32_t)*(const uint16_t *)(cbk_ + QJS_AOT_OFF_FNBC_PIC_COUNT); }\n",
        cexp, in->pc, ik, iv->sbase, ik, iv->sbase, ik);
    /* each group of the copy takes the shape of its first filled cell; an access tests its own
       cell against it, and a group nothing filled never matches */
    for (gi = 0; g_e_facts != NULL && gi < g_e_facts->ngroups; gi++) {
      const EGroup *gr = &g_e_facts->groups[gi];
      if (gr->own != in->inlg || !g_e_group_used[gi]) continue;
      if (e_group_layout(gi) != 0) {
        fprintf(
            f, "    gsh%d = (const void *)q2_layout_shape[%d]; (void)%d;\n",
            gi, e_group_layout(gi) - 1, e_group_layout(gi) - 1);
        continue;
      }
      fprintf(f, "    gsh%d = NULL;\n", gi);
      for (qq = 0; qq < gr->nsites; qq++)
        if (e_facts_site_proto(g_e_facts, gr->sites[qq]) == 2)
          fprintf(f, "    if (gsh%d == NULL) gsh%d = q2_cell2_shape(ipic_%d, %d, (int)ins_%d);\n", gi, gi, ik, gr->sites[qq], ik);
        else
          fprintf(f, "    if (gsh%d == NULL) gsh%d = q2_cell_shape(ipic_%d, %d);\n", gi, gi, ik, gr->sites[qq]);
      fprintf(f, "    if (gsh%d == NULL) gsh%d = E_NO_SHAPE;\n", gi, gi);
      for (qq = 0; qq < gr->nsites; qq++)
        if (e_facts_site_proto(g_e_facts, gr->sites[qq]) == 2)
          fprintf(f, "    pci%d = q2_cell2_index(ipic_%d, %d, (int)ins_%d, gsh%d);\n", gr->sites[qq], ik, gr->sites[qq], ik, gi);
    }
  }
  g_e_miss_exit_seen = 1;
  return 1;
}

/* The construct of an object that is never made (see Q2VWeb): the constructor must be the one the
   web was planned for, and the engine's own caches must say that constructing it stores each
   field with no side effect. A miss leaves to the construct itself. The object is a name only. */
static int emit_virt_def(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur, int have_types) {
  const Q2VWeb *vw = &fn->vweb[in->vweb - 1];
  char callee[33], cexp[64];
  int cc, q, dreg;
  Q2ValueId cv, dv;
  static const char *const hx = "0123456789abcdef";
  if (!have_types || t == NULL || cur == NULL || in->ndef < 1 || in->narg < 2) return 0;
  dv = in->def[0];
  if (dv >= (Q2ValueId)t->nvalues || t->lb[dv] != E_LB_TAG) return 0;
  dreg = fn->values[dv].reg;
  if (dreg < 0 || dreg >= fn->nregs) return 0;
  for (q = 0; q < 16; q++) {
    callee[2 * q] = hx[vw->ctor->digest[q] >> 4];
    callee[2 * q + 1] = hx[vw->ctor->digest[q] & 15];
  }
  callee[32] = 0;
  cc = (int)in->arg[1].val;
  cv = cc / 16 < fn->nregs ? cur[cc / 16] : Q2_NO_VALUE;
  if (cv != Q2_NO_VALUE && cv != E_UNDEF && cv != E_INFRAME && cv < (Q2ValueId)t->nvalues &&
      t->lb[cv] == E_LB_TAG)
    snprintf(cexp, sizeof(cexp), "v%u", (unsigned)cv);
  else
    snprintf(cexp, sizeof(cexp), "AOT_REG(fr, %d)", cc);
  e_note_stub(g_e_vx, g_e_vx_cur, g_e_vx_blk, &g_e_vx_n, in->pc, cur, NULL);
  e_note_tag('v', in->pc, cexp);
  e_sweep_guard(f, "vx", in->pc);
  fprintf(
      f,
      "#ifdef Q2_SEL_%s\n"
      "    if (__builtin_expect(!%s%s, (const void *)aot_f_%s, %du), 0)) goto vx_%d;\n"
      "    { static const uint16_t sites_[%d] = {",
      callee, e_cc_fn(fn, in), cexp, callee, e_cc_ix(fn, in), in->pc, vw->nfields);
  for (q = 0; q < vw->nfields; q++) fprintf(f, "%s%d", q ? ", " : "", vw->site[q]);
  fprintf(
      f,
      "};\n"
      "      static JSAotCtorCache cc_;\n"
      "      const unsigned char *co_ = (const unsigned char *)JS_VALUE_GET_PTR(%s);\n"
      "      if (__builtin_expect(*(const void *const *)(co_ + QJS_AOT_OFF_JSOBJECT_SHAPE) != cc_.shape ||\n"
      "          *(const uint32_t *)(*(const unsigned char *const *)((const unsigned char *)ctx + QJS_AOT_OFF_CTX_RT) +\n"
      "                              QJS_AOT_OFF_RT_PIC_EPOCH) != cc_.pic_epoch ||\n"
      "          JS_VALUE_GET_PTR(*(const JSValue *)(*(const unsigned char *const *)(co_ + QJS_AOT_OFF_JSOBJECT_PROP) +\n"
      "                                              (size_t)cc_.slot * QJS_AOT_SIZEOF_JSPROPERTY)) != cc_.proto, 0))\n"
      "        if (!qjs_aot_ctor_replayable(ctx, (JSObject *)co_, sites_, %d, &cc_)) goto vx_%d;\n"
      "      if (__builtin_expect(vsh%d != NULL && vsh%d != cc_.final_shape, 0)) goto vx_%d;\n"
      "      vsh%d = cc_.final_shape; }\n"
      "#else\n"
      "    goto vx_%d;\n"
      "#endif\n"
      "    v%u = JS_UNDEFINED;\n",
      cexp, vw->nfields, in->pc, in->vweb, in->vweb, in->pc, in->vweb, in->pc, (unsigned)dv);
  cur[dreg] = dv;
  g_e_miss_exit_seen = 1;
  return 1;
}

static void e_group_exit_stub(FILE *f, const Q2Insn *in, const Q2ValueId *cur, Q2ValueId rv);
/* A method read from the prototype of an object that is not made: the object's shape is the one
   its construct recorded (`vsh`), which must be the shape the site's group was filled with; the
   prototype is that shape's, tested and read like any prototype read. */
static int emit_virt_proto(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur, int have_types) {
  int site = -1, g, a, dreg;
  Q2ValueId dv;
  char cib[24];
  if (!have_types || t == NULL || cur == NULL || g_e_facts == NULL || in->ndef < 1 || in->nuse < 1) return 0;
  for (a = 0; a < in->narg; a++)
    if (in->arg[a].kind == 's') site = (int)in->arg[a].val;
  if (site < 0) return 0;
  g = e_facts_site_group(g_e_facts, site);
  if (g < 0 || e_facts_site_proto(g_e_facts, site) != 1) return 0;
  dv = in->def[0];
  if (dv >= (Q2ValueId)t->nvalues) return 0;
  if (t->lb[dv] != E_LB_TAG && t->lb[dv] != E_LB_I32 && t->lb[dv] != E_LB_F64 && t->lb[dv] != E_LB_BOOL) return 0;
  dreg = fn->values[dv].reg;
  if (dreg < 0 || dreg >= fn->nregs) return 0;
  e_group_exit_stub(f, in, cur, in->use[0]);
  fprintf(f, "    { JSValue _f;\n");
  fprintf(
      f,
      "      if (__builtin_expect(vsh%d != gsh%d || q2_cell_raw(pic, %s) != gsh%d || "
      "q2_holder_bad_sh(vsh%d, holders, %d), 0)) goto vx_%d;\n",
      in->vweb, g, e_cellidx(cib, sizeof(cib), site), g, in->vweb, site, in->pc);
  fprintf(f, "      _f = *q2_hslotp_sh(vsh%d, q2_cell_off(pic, %s));\n", in->vweb, cib);
  if (!g_e_nolazy)
    fprintf(f, "      if (__builtin_expect(JS_VALUE_GET_TAG(_f) == QJS_AOT_TAG_LAZY_JSON, 0)) goto vx_%d;\n", in->pc);
  if (!emit_hit_result(f, fn, in, t, dv, dreg, "_f")) {
    fprintf(stderr, "qjsaot: virtual prototype read at pc %d: no guard for its def\n", in->pc);
    abort();
  }
  fprintf(f, "    }\n");
  cur[dreg] = dv;
  g_e_miss_exit_seen = 1;
  return 1;
}

static int emit_pure_call(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur,
    int have_types) {
  char callee[4][33];
  unsigned long long hits[4];
  int kind[4], n = 0, k, cc, th, off, na, w, r, dreg;
  char recv[64], cexp[64];
  Q2ValueId dv;
  if (!have_types || t == NULL || cur == NULL || g_e_fl || g_e_inl) return 0;
  if (!qjs_aot_prof_callees(fn->g->digest, in->next, &n, callee, kind, hits)) return 0;
  cc = call_callee_off(in);
  th = call_this_off(in);
  off = dest_off(fn, in);
  na = in->win_n;
  w = in->win_reg * 16;
  if (off < 0 || cc < 0 || in->ndef < 1) return 0;
  dv = in->def[0];
  if (dv >= (Q2ValueId)t->nvalues) return 0;
  if (t->lb[dv] != E_LB_TAG && t->lb[dv] != E_LB_I32 && t->lb[dv] != E_LB_F64 &&
      t->lb[dv] != E_LB_BOOL)
    return 0;
  dreg = off / 16;
  if (dreg < 0 || dreg >= fn->nregs) return 0;
  {
    Q2ValueId cv = cc / 16 < fn->nregs ? cur[cc / 16] : Q2_NO_VALUE;
    Q2ValueId tv = th >= 0 && th / 16 < fn->nregs ? cur[th / 16] : Q2_NO_VALUE;
    if (cv != Q2_NO_VALUE && cv != E_UNDEF && cv != E_INFRAME &&
        cv < (Q2ValueId)t->nvalues && t->lb[cv] == E_LB_TAG)
      snprintf(cexp, sizeof(cexp), "v%u", (unsigned)cv);
    else
      snprintf(cexp, sizeof(cexp), "AOT_REG(fr, %d)", cc);
    if (th < 0)
      snprintf(recv, sizeof(recv), "JS_UNDEFINED");
    else if (tv != Q2_NO_VALUE && tv != E_UNDEF && tv != E_INFRAME &&
             tv < (Q2ValueId)t->nvalues && t->lb[tv] == E_LB_TAG)
      snprintf(recv, sizeof(recv), "v%u", (unsigned)tv);
    else
      snprintf(recv, sizeof(recv), "AOT_REG(fr, %d)", th);
  }
  e_note_stub(g_e_vx, g_e_vx_cur, g_e_vx_blk, &g_e_vx_n, in->pc, cur, NULL);
  e_note_tag('v', in->pc, cexp);
  if (t->lb[dv] != E_LB_TAG)
    e_note_stub(g_e_dx, g_e_dx_cur, g_e_dx_blk, &g_e_dx_n, in->next, cur, in);
  /* The argument window is built from the locals into an array of this scope, and the receiver
     and callee are passed as values: the inline body reads them in place, nothing is stored to
     the frame, and nothing here can collect. */
  {
    char wexp[64];
    fprintf(f, "    {\n    JSValue args_[%d];\n", na > 0 ? na : 1);
    for (r = 0; r < na; r++) {
      Q2ValueId v;
      int wr = in->win_reg + r;
      if (wr < 0 || wr >= fn->nregs || (v = cur[wr]) == Q2_NO_VALUE || v == E_UNDEF) {
        snprintf(wexp, sizeof(wexp), "JS_UNDEFINED");
      } else if (v == E_INFRAME || v >= (Q2ValueId)t->nvalues) {
        snprintf(wexp, sizeof(wexp), "AOT_REG(fr, %d)", wr * 16);
      } else if (t->lb[v] == E_LB_TAG) {
        snprintf(wexp, sizeof(wexp), "v%u", (unsigned)v);
      } else if (t->lb[v] == E_LB_I32) {
        snprintf(wexp, sizeof(wexp), "JS_MKVAL(JS_TAG_INT, i%u)", (unsigned)v);
      } else if (t->lb[v] == E_LB_F64) {
        snprintf(wexp, sizeof(wexp), "E_BOX_F64(d%u)", (unsigned)v);
      } else if (t->lb[v] == E_LB_BOOL) {
        snprintf(wexp, sizeof(wexp), "JS_MKVAL(JS_TAG_BOOL, b%u)", (unsigned)v);
      } else {
        snprintf(wexp, sizeof(wexp), "AOT_REG(fr, %d)", wr * 16);
      }
      fprintf(f, "    args_[%d] = %s;\n", r, wexp);
    }
    (void)w;
  }
  e_sweep_guard(f, "vx", in->pc);
  for (k = 0; k < n && k < 4; k++) {
    fprintf(f, "#ifdef Q2_INLINE_%s\n", callee[k]);
    fprintf(
        f,
        "    if (__builtin_expect(%s%s, (const void *)aot_f_%s, %du), 1)) {\n"
        "        JSValue _o; int _r;\n"
        "        E_CAP(f, %d);\n"
        "        if (!(_r = ei_%s(ctx, f, (JSObject *)JS_VALUE_GET_PTR(%s), %s,\n"
        "                    args_, %d, &_o, NULL, 0))) goto vx_%d;\n",
        e_cc_fn(fn, in), cexp, callee[k], e_cc_ix(fn, in), in->next - 1, callee[k], cexp, recv, na, in->pc);
    /* A result that needs a guard reaches the frame only when the guard misses. */
    if (t->lb[dv] == E_LB_TAG) {
      fprintf(
          f,
          "        if (_r == 1 && JS_VALUE_HAS_REF_COUNT(_o)) qjs_aot_set_reg_p(ctx, fr, %d, _o);\n"
          "        else AOT_SET_B(ctx, fr, %d, _o);\n"
          "        v%u = _o;\n",
          off, off, (unsigned)dv);
    } else if (e_def_dead(in, dv)) {
      fprintf(
          f,
          "        if (_r == 1 && JS_VALUE_HAS_REF_COUNT(_o)) qjs_aot_set_reg_p(ctx, fr, %d, _o);\n"
          "        else AOT_SET_B(ctx, fr, %d, _o);\n",
          off, off);
    } else {
      const char *fl =
          "{ if (_r == 1 && JS_VALUE_HAS_REF_COUNT(_o)) qjs_aot_set_reg_p(ctx, fr, %d, _o); "
          "else AOT_SET_B(ctx, fr, %d, _o); goto dx_%d; }";
      char miss[200];
      snprintf(miss, sizeof(miss), fl, off, off, in->next);
      e_sweep_guard_owned(f, in->next, off);
      if (t->lb[dv] == E_LB_I32)
        fprintf(
            f,
            "        if (__builtin_expect(!Q2_I32_OK(_o, i%u), 0)) %s\n",
            (unsigned)dv, miss);
      else if (t->lb[dv] == E_LB_BOOL)
        fprintf(
            f,
            "        if (__builtin_expect(JS_VALUE_GET_TAG(_o) != JS_TAG_BOOL, 0)) %s\n"
            "        b%u = JS_VALUE_GET_BOOL(_o);\n",
            miss, (unsigned)dv);
      else
        fprintf(
            f,
            "        if (JS_VALUE_GET_TAG(_o) == JS_TAG_INT) d%u = (double)JS_VALUE_GET_INT(_o);\n"
            "        else if (JS_VALUE_GET_TAG(_o) == JS_TAG_FLOAT64) d%u = JS_VALUE_GET_FLOAT64(_o);\n"
            "        else %s\n",
            (unsigned)dv, (unsigned)dv, miss);
    }
    fprintf(f, "        goto cd_%d;\n    }\n#endif\n", in->pc);
  }
  fprintf(f, "    goto vx_%d;\n    }\n    cd_%d: ;\n", in->pc, in->pc);
  cur[dreg] = dv;
  g_e_miss_exit_seen = 1;
  return 1;
}

/* The result `_o` of a builtin called directly, taken as the call's definition: an owned result
   is adopted by the register, the groups are re-read, a typed definition is guarded, and the body
   continues after the call. */
static void e_builtin_call_tail(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, const Q2ValueId *cur, Q2ValueId dv,
    int off) {
  fprintf(
      f,
      "        if (JS_VALUE_HAS_REF_COUNT(_o)) qjs_aot_set_reg_p(ctx, fr, %d, _o);\n"
      "        else AOT_SET_B(ctx, fr, %d, _o);\n",
      off, off);
  e_emit_group_recheck(f, fn, in, cur);
  if (t->lb[dv] == E_LB_TAG) {
    fprintf(f, "        v%u = _o;\n", (unsigned)dv);
  } else if (e_def_dead(in, dv)) {
    /* nothing reads the result */
  } else if (t->lb[dv] == E_LB_I32) {
    e_sweep_guard(f, "dx", in->next);
    fprintf(
        f,
        "        if (__builtin_expect(!Q2_I32_OK(_o, i%u), 0)) "
        "goto dx_%d;\n",
        (unsigned)dv, in->next);
  } else if (t->lb[dv] == E_LB_BOOL) {
    e_sweep_guard(f, "dx", in->next);
    fprintf(
        f,
        "        if (__builtin_expect(JS_VALUE_GET_TAG(_o) != JS_TAG_BOOL, 0)) "
        "goto dx_%d;\n"
        "        b%u = JS_VALUE_GET_BOOL(_o);\n",
        in->next, (unsigned)dv);
  } else {
    e_sweep_guard(f, "dx", in->next);
    fprintf(
        f,
        "        if (JS_VALUE_GET_TAG(_o) == JS_TAG_INT) d%u = "
        "(double)JS_VALUE_GET_INT(_o);\n"
        "        else if (JS_VALUE_GET_TAG(_o) == JS_TAG_FLOAT64) d%u = "
        "JS_VALUE_GET_FLOAT64(_o);\n"
        "        else goto dx_%d;\n",
        (unsigned)dv, (unsigned)dv, in->next);
  }
  fprintf(f, "        goto cd_%d;\n", in->pc);
}

static int emit_typed_call(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur,
    int have_types, char hex[33], int fi, int b) {
  char callee[4][33];
  unsigned long long hits[4];
  int kind[4], n = 0, k, nb = 0, cc, th, off, na, w, r;
  char recv[64], cexp[64];
  Q2ValueId dv;
  int dreg;

  /* ---- every check, before any output ---- */
  if (!have_types || t == NULL) return 0;
  /* The facts classed this site as a pure inline call. */
  if (g_e_facts != NULL && g_e_facts->purecall[e_insn_idx(fn, in)])
    return emit_pure_call(f, fn, in, t, cur, have_types);
  /* The facts classed this site as an intrinsic with an exit for a miss. */
  if (e_must_exit(fn, in) && !g_e_intr_cont)
    return emit_intrinsic_call(f, fn, in, t, cur, hex, fi, b);
  /* An inline body may call a compiled callee; a frameless one makes no call but a
     lowered intrinsic. */
  if (g_e_inl || g_e_fl) return emit_inl_nested_call(f, fn, in, t, cur, hex);
  if (e_nocall_on()) return 0;
  if (!call_site_shaped(fn, in)) return 0;
  if (!qjs_aot_prof_callees(fn->g->digest, in->next, &n, callee, kind, hits))
    return 0;
  for (k = 0; k < n && k < 4; k++) {
    if (kind[k] != 1) continue;
    if (!qjs_aot_unit_has(callee[k])) continue;
    if (!qjs_aot_native_capable(callee[k])) continue;
    if (in->win_n < qjs_aot_unit_argc(callee[k])) continue;
    nb++;
  }
  /* No compiled callee to call directly is no reason to leave: the call is the
     wrapper alone, which any callee satisfies. The wrapper needs the role. */
  if (e_roles[in->rop] == NULL || e_roles[in->rop]->control != E_CF_NONE)
    return 0;
  cc = call_callee_off(in);
  th = call_this_off(in);
  off = dest_off(fn, in);
  na = in->win_n;
  w = in->win_reg * 16;
  if (off < 0 || cc < 0 || in->ndef < 1) return 0;
  dv = in->def[0];
  if (dv >= (Q2ValueId)t->nvalues) return 0;
  /* A TAG def is a plain local copy; a typed def needs the post-call tag guard. */
  if (t->lb[dv] != E_LB_TAG && t->lb[dv] != E_LB_I32 &&
      t->lb[dv] != E_LB_F64 && t->lb[dv] != E_LB_BOOL)
    return 0;
  dreg = off / 16;
  if (dreg < 0 || dreg >= fn->nregs) return 0;
  /* A typed def's guard exits at the NEXT pc (the call has run), so that exit must
     exist; the same registration the op-call wrapper uses. */
  if (t->lb[dv] != E_LB_TAG)
    e_note_stub(
        g_e_dx, g_e_dx_cur, g_e_dx_blk, &g_e_dx_n, in->next, cur, in);
  /* The callee and the receiver are TAG locals when the block holds them in one:
     test and pass those, not the frame slots just stored from them (a load that
     follows a store of the same bytes waits for it). */
  {
    Q2ValueId cv = cur != NULL && cc / 16 < fn->nregs ? cur[cc / 16] : Q2_NO_VALUE;
    Q2ValueId tv =
        th >= 0 && cur != NULL && th / 16 < fn->nregs ? cur[th / 16] : Q2_NO_VALUE;
    if (cv != Q2_NO_VALUE && cv != E_UNDEF && cv != E_INFRAME &&
        cv < (Q2ValueId)t->nvalues && t->lb[cv] == E_LB_TAG)
      snprintf(cexp, sizeof(cexp), "v%u", (unsigned)cv);
    else
      snprintf(cexp, sizeof(cexp), "AOT_REG(fr, %d)", cc);
    if (th < 0)
      snprintf(recv, sizeof(recv), "JS_UNDEFINED");
    else if (tv != Q2_NO_VALUE && tv != E_UNDEF && tv != E_INFRAME &&
             tv < (Q2ValueId)t->nvalues && t->lb[tv] == E_LB_TAG)
      snprintf(recv, sizeof(recv), "v%u", (unsigned)tv);
    else
      snprintf(recv, sizeof(recv), "AOT_REG(fr, %d)", th);
  }

  /* Array.prototype.push and pop on a dense array, done in place: the callee is checked to be the
     builtin, the receiver to be a plain dense array with room, and nothing else runs. Anything
     else takes the call below. */
  if (!g_e_inl && !g_e_fl && n == 1 && kind[0] == 2 && th >= 0 && cur != NULL && getenv("QJS_AOT_NOINLINEARR") == NULL) {
    int which = -1;
    if (strcmp(callee[0], "js_array_push:0") == 0 && na == 1) which = 0;
    else if (strcmp(callee[0], "js_array_pop:0") == 0 && na == 0) which = 1;
    if (which >= 0) {
      char ae[80] = "";
      int ok = 1;
      if (which == 0) {
        Q2ValueId av = in->win_reg < fn->nregs ? cur[in->win_reg] : Q2_NO_VALUE;
        if (av == E_UNDEF)
          snprintf(ae, sizeof(ae), ", JS_UNDEFINED");
        else if (av == E_INFRAME)
          snprintf(ae, sizeof(ae), ", AOT_REG(fr, %d)", in->win_reg * 16);
        else if (av != Q2_NO_VALUE && av < (Q2ValueId)t->nvalues) {
          switch (t->lb[av]) {
            case E_LB_I32: snprintf(ae, sizeof(ae), ", JS_MKVAL(JS_TAG_INT, i%u)", (unsigned)av); break;
            case E_LB_F64: snprintf(ae, sizeof(ae), ", E_BOX_F64(d%u)", (unsigned)av); break;
            case E_LB_BOOL: snprintf(ae, sizeof(ae), ", JS_MKVAL(JS_TAG_BOOL, b%u)", (unsigned)av); break;
            default: snprintf(ae, sizeof(ae), ", v%u", (unsigned)av); break;
          }
        } else
          ok = 0;
      }
      if (ok) {
        fprintf(
            f, "    { JSValue _o;\n      if (q2_array_%s(ctx, %s, %s%s, &_o)) {\n", which ? "pop0" : "push1", cexp,
            recv, ae);
        e_builtin_call_tail(f, fn, in, t, cur, dv, off);
        fprintf(f, "      }\n    }\n");
      }
    }
  }

  /* ---- output starts here; it must not be abandoned ---- */
  fprintf(f, "    /* sync live TAG values to the frame before the call */\n");
  for (r = 0; r < fn->norig; r++) {
    Q2ValueId v;
    if (cur == NULL) break;
    v = cur[r];
    if (v == Q2_NO_VALUE || v == E_UNDEF) continue;
    if (v >= (Q2ValueId)t->nvalues) continue;
    if (t->lb[v] != E_LB_TAG) continue;
    fprintf(f, "    AOT_SET_B(ctx, fr, %d, v%u);\n", r * 16, (unsigned)v);
  }
  /* The argument window: every slot the callee reads must be current. */
  for (r = 0; r < na; r++) {
    Q2ValueId v;
    int wr = in->win_reg + r;
    if (cur == NULL || wr >= fn->nregs) continue;
    v = cur[wr];
    if (v == Q2_NO_VALUE || v == E_UNDEF || v >= (Q2ValueId)t->nvalues) continue;
    if (t->lb[v] == E_LB_TAG)
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, v%u);\n", wr * 16, (unsigned)v);
    else if (t->lb[v] == E_LB_I32)
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, JS_MKVAL(JS_TAG_INT, i%u));\n",
              wr * 16, (unsigned)v);
    else if (t->lb[v] == E_LB_F64)
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, E_BOX_F64(d%u));\n", wr * 16,
              (unsigned)v);
    else if (t->lb[v] == E_LB_BOOL)
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, JS_MKVAL(JS_TAG_BOOL, b%u));\n",
              wr * 16, (unsigned)v);
  }

  for (k = 0; k < n && k < 4; k++) {
    if (kind[k] != 1) continue;
    if (!qjs_aot_unit_has(callee[k])) continue;
    if (!qjs_aot_native_capable(callee[k])) continue;
    if (in->win_n < qjs_aot_unit_argc(callee[k])) continue;
    fprintf(f, "#ifdef Q2_NATIVE_%s\n", callee[k]);
    /* Negative control: "noident" tests only that the callee is an object, so a
       replaced function runs the compiled callee's body. */
    if (getenv("QJS_AOT_INL_CONTROL") != NULL &&
        strcmp(getenv("QJS_AOT_INL_CONTROL"), "noident") == 0)
      fprintf(
          f, "    if (JS_VALUE_GET_TAG(%s) == JS_TAG_OBJECT) {\n", cexp);
    else
    fprintf(
        f,
        "    if (__builtin_expect(%s%s, "
        "(const void *)aot_f_%s, %du), 1)) {\n",
        e_cc_fn(fn, in), cexp, callee[k], e_cc_ix(fn, in));
    fprintf(f, "        JSValue _o; int _r = 1;\n");
    /* The callee's inline body first: it runs the callee in place when every
       check passes and has changed nothing when one does not, so the native call
       below is then correct. */
    fprintf(f, "        E_CAP(f, %d);\n", in->next - 1);
    if (qjs_aot_inline_has(callee[k]))
      fprintf(
          f,
          "#ifdef Q2_INLINE_%s\n"
          "        if (!(_r = ei_%s(ctx, f, (JSObject *)JS_VALUE_GET_PTR(%s), %s,\n"
          "                    &AOT_REG(fr, %d), %d, &_o, NULL, 0)))\n"
          "#endif\n",
          callee[k], callee[k], cexp, recv, w, na);
    fprintf(
        f,
        "        { _r = 1; _o = aot_n_%s(ctx, f,\n"
        "            (JSObject *)JS_VALUE_GET_PTR(%s), %s,\n"
        "            &AOT_REG(fr, %d), %d); }\n",
        callee[k], cexp, recv, w, na);
    fprintf(
        f,
        "        if (__builtin_expect(JS_IsException(_o), 0)) return JS_EXCEPTION;\n"
        "        /* Adopting an owned result drops its reference; a value that has\n"
        "           none (an int, a double, a bool) is a plain store. */\n"
        "        if (_r == 1 && JS_VALUE_HAS_REF_COUNT(_o)) qjs_aot_set_reg_p(ctx, fr, %d, _o);\n"
        "        else AOT_SET_B(ctx, fr, %d, _o);\n",
        off, off);
    /* The call ran JS: re-read the groups before anything reads a field. */
    e_emit_group_recheck(f, fn, in, cur);
    if (t->lb[dv] == E_LB_TAG) {
      fprintf(f, "        v%u = _o;\n", (unsigned)dv);
    } else if (e_def_dead(in, dv)) {
      /* Nothing reads the result: set_reg_p above left it in the frame. */
    } else if (t->lb[dv] == E_LB_I32) {
      /* The call has run, so a failed guard is an ordinary exit at the NEXT pc. */
      e_sweep_guard(f, "dx", in->next);
      fprintf(
          f,
          "        if (__builtin_expect(!Q2_I32_OK(_o, i%u), 0)) "
          "goto dx_%d;\n",
          (unsigned)dv, in->next);
    } else if (t->lb[dv] == E_LB_BOOL) {
      e_sweep_guard(f, "dx", in->next);
      fprintf(
          f,
          "        if (__builtin_expect(JS_VALUE_GET_TAG(_o) != JS_TAG_BOOL, "
          "0)) "
          "goto dx_%d;\n"
          "        b%u = JS_VALUE_GET_BOOL(_o);\n",
          in->next, (unsigned)dv);
    } else {
      e_sweep_guard(f, "dx", in->next);
      fprintf(
          f,
          "        if (JS_VALUE_GET_TAG(_o) == JS_TAG_INT) d%u = "
          "(double)JS_VALUE_GET_INT(_o);\n"
          "        else if (JS_VALUE_GET_TAG(_o) == JS_TAG_FLOAT64) d%u = "
          "JS_VALUE_GET_FLOAT64(_o);\n"
          "        else goto dx_%d;\n",
          (unsigned)dv, (unsigned)dv, in->next);
    }
    fprintf(f, "        goto cd_%d;\n", in->pc);
    fprintf(f, "    }\n");
    fprintf(f, "#endif\n");
  }
  /* A builtin (C function) the profile saw at this site: called directly, not through
     the step handler. The engine's call helper checks the callee against the one it
     bound, so a replaced builtin takes the wrapper below; the call runs arbitrary C
     (a callback, a getter), so it is a safepoint like any other call. */
  if (!g_e_quiet && n > 0 && n <= 3) {
    int all_c = 1;
    for (k = 0; k < n; k++)
      if (kind[k] != 2) all_c = 0;
    if (all_c) {
      int site = g_e_ccall_next++;
      fprintf(
          f,
          "    { JSValue _o;\n"
          "      E_CAP(f, %d);\n"
          "      int _rc = qjs_aot_call_c(ctx, %d, -1, %d, %s, %s, &AOT_REG(fr, %d), &_o);\n"
          "      if (__builtin_expect(_rc < 0, 0)) return JS_EXCEPTION;\n"
          "      if (_rc > 0) {\n",
          in->next - 1, site, na, cexp, recv, w);
      e_builtin_call_tail(f, fn, in, t, cur, dv, off);
      fprintf(f, "      }\n    }\n");
    }
  }
  /* No candidate matched (or none was offered): the call has NOT run, so the
     engine's own call handler does it, through the op-call wrapper. The live
     values and the window are already in the frame. Any callee takes this path,
     compiled or not, and the body continues. */
  g_e_presynced = 1;
  if (!emit_typed_opcall(f, fn, in, t, cur, NULL, hex, fi, b)) {
    fprintf(
        stderr, "qjsaot: call at pc %d: the wrapper declined after output\n",
        in->pc);
    abort();
  }
  g_e_presynced = 0;
  fprintf(f, "    cd_%d: ;\n", in->pc);
  if (cur != NULL) cur[dreg] = dv;
  return 1;
}

/* A read of a variable slot. A function with an `arguments` binding, a handler or a
   `with` keeps its variables in frame memory rather than registers; the read is the
   slot's value, borrowed. */
static int emit_typed_local_read(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur,
    int have_types) {
  Q2ValueId dv;
  int slot, dreg;

  if (!have_types || t == NULL || g_e_fl || cur == NULL) return 0;
  if (in->rop == ROP_get_local) {
    if (in->narg < 2 || in->arg[1].kind != 'u') return 0;
    slot = (int)in->arg[1].val;
  } else if (in->rop >= ROP_get_local_s0 && in->rop <= ROP_get_local_s3) {
    slot = in->rop - ROP_get_local_s0;
  } else {
    return 0;
  }
  if (in->ndef < 1) return 0;
  dv = in->def[0];
  if (dv >= (Q2ValueId)t->nvalues) return 0;
  dreg = fn->values[dv].reg;
  if (dreg < 0 || dreg >= fn->nregs) return 0;
  if (t->lb[dv] != E_LB_TAG && t->lb[dv] != E_LB_I32 && t->lb[dv] != E_LB_F64 &&
      t->lb[dv] != E_LB_BOOL)
    return 0;
  if (t->lb[dv] != E_LB_TAG)
    e_note_stub(g_e_dx, g_e_dx_cur, g_e_dx_blk, &g_e_dx_n, in->next, cur, in);
  fprintf(f, "    { JSValue _lv = q2_var_buf(f)[%d];\n", slot);
  if (!emit_hit_result(f, fn, in, t, dv, dreg, "_lv")) {
    fprintf(stderr, "qjsaot: local read at pc %d: no guard for the def\n", in->pc);
    abort();
  }
  fprintf(f, "    }\n");
  cur[dreg] = dv;
  return 1;
}

/* `binary_loc_reg`, `binary_reg_loc`, `binary_loc_loc`: arithmetic with a variable
   slot as an operand. Two numbers combine inline; anything else steps. */
static int emit_typed_binary_loc(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur,
    int have_types, char hex[33], int fi, int b) {
  Q2ValueId dv;
  int dreg, sub;
  char ea[96], eb[96];

  if (!have_types || t == NULL || g_e_fl || cur == NULL) return 0;
  if (in->rop != ROP_binary_loc_reg && in->rop != ROP_binary_reg_loc &&
      in->rop != ROP_binary_loc_loc)
    return 0;
  if (in->narg < 4 || in->arg[1].kind != 'b' || in->ndef < 1) return 0;
  if (e_must_exit(fn, in)) return 0;
  sub = (int)in->arg[1].val;
  if (in->rop == ROP_binary_loc_reg) {
    if (in->arg[2].kind != 'u' || in->arg[3].kind != 'r' || in->nuse < 1) return 0;
    e_loc_operand(ea, sizeof(ea), fn, t, in, 2, 0);
    e_loc_operand(eb, sizeof(eb), fn, t, in, 3, 0);
  } else if (in->rop == ROP_binary_reg_loc) {
    if (in->arg[2].kind != 'r' || in->arg[3].kind != 'u' || in->nuse < 1) return 0;
    e_loc_operand(ea, sizeof(ea), fn, t, in, 2, 0);
    e_loc_operand(eb, sizeof(eb), fn, t, in, 3, 0);
  } else {
    if (in->arg[2].kind != 'u' || in->arg[3].kind != 'u') return 0;
    e_loc_operand(ea, sizeof(ea), fn, t, in, 2, 0);
    e_loc_operand(eb, sizeof(eb), fn, t, in, 3, 0);
  }
  dv = in->def[0];
  if (dv >= (Q2ValueId)t->nvalues) return 0;
  dreg = fn->values[dv].reg;
  if (dreg < 0 || dreg >= fn->nregs) return 0;
  if (t->lb[dv] != E_LB_TAG && t->lb[dv] != E_LB_I32 && t->lb[dv] != E_LB_F64 &&
      t->lb[dv] != E_LB_BOOL)
    return 0;
  if (t->lb[dv] != E_LB_TAG)
    e_note_stub(g_e_dx, g_e_dx_cur, g_e_dx_blk, &g_e_dx_n, in->next, cur, in);
  fprintf(f, "    { JSValue _r;\n");
  fprintf(f, "      if (__builtin_expect(q2_arith(%d, %s, %s, &_r), 1)) {\n", sub, ea, eb);
  if (!emit_hit_result(f, fn, in, t, dv, dreg, "_r")) {
    fprintf(stderr, "qjsaot: local arithmetic at pc %d: no guard for the def\n", in->pc);
    abort();
  }
  fprintf(f, "      } else {\n");
  if (!emit_typed_opcall(f, fn, in, t, cur, NULL, hex, fi, b)) {
    fprintf(stderr, "qjsaot: local arithmetic at pc %d: the wrapper declined after output\n", in->pc);
    abort();
  }
  fprintf(f, "      }\n    }\n");
  cur[dreg] = dv;
  return 1;
}

/* A global read the profile saw hold one int32 value throughout: the program's own instruction
   (an inlined callee's atom is not in this function's code), its destination typed by the
   instruction's own record. The body that folds it holds the value as a literal; its entry
   confirms the live property (`qjs_aot_global_fold`) and every write to the property moves the
   epoch the body tests after each safepoint. */
static int e_gfold_val(const Q2Func *fn, const Q2Insn *in, int32_t *val) {
  const Q2Value *v;
  if (in->rop != ROP_get_global) return 0;
  if (in->own != 0 || in->ndef < 1 || in->narg < 4) return 0;
  if (in->arg[1].kind != 'a' || in->arg[3].kind != 's') return 0;
  if ((unsigned)in->arg[3].val == REG_IC_NONE) return 0;
  if (in->def[0] >= (Q2ValueId)fn->nvalues) return 0;
  v = &fn->values[in->def[0]];
  if (!v->has_prof || v->prof_src != 0 || v->ptypes != Q2_T_INT32 || v->frac) return 0;
  if (v->nmin != v->nmax || v->nmin < -2147483648.0 || v->nmin > 2147483647.0) return 0;
  *val = (int32_t)v->nmin;
  return 1;
}

/* A speculative-body GLOBAL READ: `get_global` ("rabs": dest, atom offset, throw
   flag, site). `q2_global_hit` is the same inline test the generic body uses; a
   miss goes to the wrapper, which performs the real lookup (that path can run a
   getter, throw, and fill the cache -- none of which a spec body may do inline).
   The result is a JSValue: a TAG def is a plain copy, a typed def writes the raw
   result to the frame first and then guards into the local. */
static int emit_typed_global_read(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur,
    int have_types, char hex[33], int fi, int b) {
  const ERole *role = e_roles[in->rop];
  Q2ValueId dv;
  int site, dreg;

  if (!have_types || t == NULL) return 0;
  if (in->rop != ROP_get_global) return 0;
  if (role == NULL || role->control != E_CF_NONE) return 0;
  if (in->ndef < 1 || in->narg < 4 || in->arg[3].kind != 's') return 0;
  site = (int)in->arg[3].val;
  dv = in->def[0];
  if (dv >= (Q2ValueId)t->nvalues) return 0;
  dreg = fn->values[dv].reg;
  if (dreg < 0 || dreg >= fn->nregs) return 0;
  /* EVERY check before the first byte: a TAG def is a plain copy, an i32/f64 def
     gets the post-hit tag guard. */
  if (t->lb[dv] != E_LB_TAG && t->lb[dv] != E_LB_I32 &&
      t->lb[dv] != E_LB_F64)
    return 0;

  {
    int32_t gv;
    if (t->lb[dv] == E_LB_I32 && g_e_epoch_skip && e_gfold_val(fn, in, &gv)) {
      fprintf(f, "    i%u = %d;\n", (unsigned)dv, (int)gv);
      if (cur != NULL) cur[dreg] = dv;
      g_e_miss_exit_seen = 1; /* nothing here can miss */
      return 1;
    }
  }

  if (in->xpc >= 0) {
    /* Moved before a loop: every way out leaves to the pc it was placed before. */
    e_note_stub(g_e_vx, g_e_vx_cur, g_e_vx_blk, &g_e_vx_n, in->pc, cur, NULL);
    e_sweep_guard(f, "vx", in->pc);
    fprintf(f, "    { JSValue _g;\n");
    fprintf(
        f,
        "      if (__builtin_expect(Q2_FORCE_MISS || !q2_global_hit(ctx, pic, %d, &_g), 0)) "
        "goto vx_%d;\n",
        site, in->pc);
    if (t->lb[dv] == E_LB_TAG)
      fprintf(f, "      v%u = _g;\n", (unsigned)dv);
    else if (t->lb[dv] == E_LB_I32)
      fprintf(
          f,
          "      if (__builtin_expect(!Q2_I32_OK(_g, i%u), 0)) goto vx_%d;\n",
          (unsigned)dv, in->pc);
    else
      fprintf(
          f,
          "      if (JS_VALUE_GET_TAG(_g) == JS_TAG_INT) d%u = (double)JS_VALUE_GET_INT(_g);\n"
          "      else if (JS_VALUE_GET_TAG(_g) == JS_TAG_FLOAT64) d%u = JS_VALUE_GET_FLOAT64(_g);\n"
          "      else goto vx_%d;\n",
          (unsigned)dv, (unsigned)dv, in->pc);
    fprintf(f, "    }\n");
    if (cur != NULL) cur[dreg] = dv;
    g_e_miss_exit_seen = 1;
    return 1;
  }
  if (t->lb[dv] != E_LB_TAG && !e_def_dead(in, dv))
    e_note_stub(g_e_dx, g_e_dx_cur, g_e_dx_blk, &g_e_dx_n, in->next, cur, in);
  fprintf(f, "    { JSValue _g;\n");
  fprintf(
      f,
      "      if (__builtin_expect(!Q2_FORCE_MISS && q2_global_hit(ctx, pic, %d, "
      "&_g), 1)) {\n",
      site);
  if (t->lb[dv] == E_LB_TAG) {
    fprintf(f, "        v%u = _g;\n", (unsigned)dv);
  } else if (e_def_dead(in, dv)) {
    /* nothing reads it: no guard, no store */
  } else {
    /* The raw result reaches the frame only when the guard misses. */
    e_sweep_guard_set(f, "dx", in->next, dreg * 16, "_g");
    if (t->lb[dv] == E_LB_I32)
      fprintf(
          f,
          "        if (__builtin_expect(!Q2_I32_OK(_g, i%u), 0)) "
          "{ AOT_SET_B(ctx, fr, %d, _g); goto dx_%d; }\n",
          (unsigned)dv, dreg * 16, in->next);
    else if (t->lb[dv] == E_LB_F64)
      fprintf(
          f,
          "        if (JS_VALUE_GET_TAG(_g) == JS_TAG_INT) d%u = "
          "(double)JS_VALUE_GET_INT(_g);\n"
          "        else if (JS_VALUE_GET_TAG(_g) == JS_TAG_FLOAT64) d%u = "
          "JS_VALUE_GET_FLOAT64(_g);\n"
          "        else { AOT_SET_B(ctx, fr, %d, _g); goto dx_%d; }\n",
          (unsigned)dv, (unsigned)dv, dreg * 16, in->next);
  }
  fprintf(f, "      } else {\n");
  if (e_must_exit(fn, in)) {
    /* The cache cell does not hold: the generic body does the lookup. */
    e_note_stub(g_e_vx, g_e_vx_cur, g_e_vx_blk, &g_e_vx_n, in->pc, cur, NULL);
    e_sweep_guard(f, "vx", in->pc);
    fprintf(f, "        goto vx_%d;\n", in->pc);
    g_e_miss_exit_seen = 1;
  } else {
    /* A global's cache hit holds until a global changes: not a systematic miss. */
    g_e_fl_miss_ok = 1;
    if (!emit_typed_opcall(f, fn, in, t, cur, NULL, hex, fi, b)) {
      g_e_fl_miss_ok = 0;
      return 0;
    }
    g_e_fl_miss_ok = 0;
  }
  fprintf(f, "      }\n");
  fprintf(f, "    }\n");
  if (cur != NULL) cur[dreg] = dv;
  g_e_ta_keep = 1;
  return 1;
}

static void e_cmp_expr(
    char *out, size_t cap, int base, const ETypes *t, Q2ValueId av, Q2ValueId bv,
    int f64rep, int tag_arm);
/* A speculative-body `load_this` ("r", no operands): `this` comes from the
   function's own this-slot through `qjs_aot_load_this`, exactly as the generic
   emitter does. `_own` says whether the value is owned (a sloppy-mode function's
   boxed primitive): then the frame slot adopts it with qjs_aot_set_reg_p, as the
   generic body does, and the local is a borrowed view of a value the frame keeps
   alive -- like every other TAG local. A borrowed value is a plain store. */
static int emit_typed_load_this(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur,
    int have_types) {
  Q2ValueId dv;
  int ld;

  if (!have_types || t == NULL) return 0;
  if (in->rop != ROP_load_this) return 0;
  if (in->ndef < 1) return 0;
  dv = in->def[0];
  if (dv >= (Q2ValueId)t->nvalues) return 0;
  if (t->lb[dv] != E_LB_TAG) return 0;
  ld = fn->values[dv].reg;
  if (ld < 0 || ld >= fn->nregs) return 0;

  if (g_e_fl) {
    /* An object `this` is the same in strict and sloppy code; anything else
       may need boxing or the global object, which allocates, so it exits. */
    e_note_stub(
        g_e_vx, g_e_vx_cur, g_e_vx_blk, &g_e_vx_n, in->pc, cur, NULL);
    fprintf(
        f,
        "    if (__builtin_expect(JS_VALUE_GET_TAG(this_obj) != JS_TAG_OBJECT, "
        "0)) goto vx_%d;\n"
        "    v%u = this_obj;\n"
        "    AOT_SET_B(ctx, fr, %d, this_obj);\n",
        in->pc, (unsigned)dv, ld * 16);
    if (cur != NULL) cur[ld] = dv;
    return 1;
  }
  /* An object `this` is what qjs_aot_load_this returns for it, strict or sloppy:
     read in line; anything else takes the engine's helper (it may box). */
  fprintf(
      f,
      "    { JSValue _t = *(JSValue *)((uint8_t *)f + "
      "QJS_AOT_OFF_REGRUN_THIS_OBJ);\n"
      "      int _own = 0;\n"
      "      if (__builtin_expect(JS_VALUE_GET_TAG(_t) != JS_TAG_OBJECT, 0)) {\n"
      "        _own = qjs_aot_load_this(ctx, f, &_t);\n"
      "        if (__builtin_expect(_own < 0, 0)) return JS_EXCEPTION;\n"
      "      }\n");
  fprintf(
      f,
      "      if (_own) qjs_aot_set_reg_p(ctx, fr, %d, _t);\n"
      "      else AOT_SET_B(ctx, fr, %d, _t);\n",
      ld * 16, ld * 16);
  fprintf(f, "      v%u = _t;\n", (unsigned)dv);
  fprintf(f, "    }\n");
  if (cur != NULL) cur[ld] = dv;
  return 1;
}

/* A speculative-body `strict_eq` / `strict_neq` producing a BOOL value (not a
   branch). `===` never coerces, so: two typed operands compare directly (as
   doubles when either is f64 -- `==` is false for NaN, which is JS `===`), and a
   TAG operand is dispatched on its tag with a COLD ARM that hands off, because
   `"1" === 1` and `{} === {}` cannot be answered from a tag alone. */
static int emit_typed_strict_eq(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur,
    int have_types, char hex[33], int fi, int b) {
  Q2ValueId dv, av, bv;
  int ld, neg, f64;
  char xa[48], xb[48], ex[128];

  if (!have_types || t == NULL) return 0;
  if (in->rop != ROP_strict_eq && in->rop != ROP_strict_neq) return 0;
  if (in->ndef < 1 || in->nuse < 2) return 0;
  dv = in->def[0];
  av = in->use[0];
  bv = in->use[1];
  if (dv >= (Q2ValueId)t->nvalues || av >= (Q2ValueId)t->nvalues ||
      bv >= (Q2ValueId)t->nvalues)
    return 0;
  if (t->lb[dv] != E_LB_BOOL) return 0;
  neg = in->rop == ROP_strict_neq;
  ld = fn->values[dv].reg;
  if (ld < 0 || ld >= fn->nregs) return 0;
  f64 = t->lb[av] == E_LB_F64 || t->lb[bv] == E_LB_F64;

  /* A BOOL operand is equal only to a BOOL: `true === 1` is false. Two BOOLs
     compare as their flags; any other mix is left to the generic body. */
  if (t->lb[av] == E_LB_BOOL || t->lb[bv] == E_LB_BOOL) {
    if (t->lb[av] != E_LB_BOOL || t->lb[bv] != E_LB_BOOL) return 0;
    fprintf(f, "    b%u = %s(b%u == b%u);\n", (unsigned)dv, neg ? "!" : "",
            (unsigned)av, (unsigned)bv);
    if (cur != NULL) cur[ld] = dv;
    return 1;
  }
  e_cmp_expr(ex, sizeof(ex), REG_BIN_STRICT_EQ, t, av, bv, f64, 2);

  if (t->lb[av] == E_LB_TAG || t->lb[bv] == E_LB_TAG) {
    Q2ValueId tv = t->lb[av] == E_LB_TAG ? av : bv;
    int treg = fn->values[tv].reg;
    if (treg < 0 || treg >= fn->nregs) return 0;
    /* A second TAG operand makes the dispatch two-deep; decline rather than emit a
       half-answer, which is the shape that cost us two rounds on slice 2d. */
    if (t->lb[av] == E_LB_TAG && t->lb[bv] == E_LB_TAG) return 0;
    e_cmp_expr(ex, sizeof(ex), REG_BIN_STRICT_EQ, t, av, bv, f64, 0);
    fprintf(f, "    { int _e;\n");
    fprintf(
        f,
        "      if (__builtin_expect(JS_VALUE_GET_TAG(v%u) == JS_TAG_INT, 1))\n"
        "          _e = %s;\n",
        (unsigned)tv, ex);
    e_cmp_expr(ex, sizeof(ex), REG_BIN_STRICT_EQ, t, av, bv, 1, 1);
    fprintf(
        f,
        "      else if (JS_VALUE_GET_TAG(v%u) == JS_TAG_FLOAT64)\n"
        "          _e = %s;\n",
        (unsigned)tv, ex);
    fprintf(f, "      else goto hx_%d;\n", in->pc);
    e_note_stub(
        g_e_hx, g_e_hx_cur, g_e_hx_blk, &g_e_hx_n, in->pc, cur, NULL);
    fprintf(f, "      b%u = %s_e;\n", (unsigned)dv, neg ? "!" : "");
    fprintf(f, "    }\n");
  } else {
    /* Both typed. `==` on doubles is false for NaN, which is JS `===`. */
    fprintf(f, "    { int _e = %s;\n", ex);
    fprintf(f, "      b%u = %s_e; }\n", (unsigned)dv, neg ? "!" : "");
  }
  if (cur != NULL) cur[ld] = dv;
  (void)xa; (void)xb;
  (void)hex; (void)fi; (void)b;
  return 1;
}

/* A comparison as a VALUE (`lt`, `lte`, `gt`, `gte`, `eq`, `neq`, `strict_eq`, `strict_neq`)
   of two tagged operands: two numbers compare inline, two flat strings by their
   characters; anything else is the engine's (it can run a valueOf). */
static int emit_tag_cmp_value(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur,
    int have_types, char hex[33], int fi, int b) {
  Q2ValueId dv, av, bv;
  int dreg, rel, k;
  const char *cop;
  static const struct { int rop; const char *op; int rel; } tab[] = {
      {ROP_lt, "<", 1}, {ROP_lte, "<=", 1}, {ROP_gt, ">", 1}, {ROP_gte, ">=", 1},
      {ROP_eq, "==", 0}, {ROP_neq, "!=", 0}, {ROP_strict_eq, "==", 0},
      {ROP_strict_neq, "!=", 0}};

  if (!have_types || t == NULL || g_e_fl || cur == NULL) return 0;
  cop = NULL;
  rel = 0;
  for (k = 0; k < (int)(sizeof(tab) / sizeof(tab[0])); k++)
    if (tab[k].rop == in->rop) { cop = tab[k].op; rel = tab[k].rel; }
  if (cop == NULL || in->ndef < 1 || in->nuse < 2) return 0;
  if (e_must_exit(fn, in)) return 0;
  dv = in->def[0];
  av = in->use[0];
  bv = in->use[1];
  if (dv >= (Q2ValueId)t->nvalues || av >= (Q2ValueId)t->nvalues ||
      bv >= (Q2ValueId)t->nvalues)
    return 0;
  if (t->lb[av] != E_LB_TAG || t->lb[bv] != E_LB_TAG) return 0;
  if (t->lb[dv] != E_LB_TAG && t->lb[dv] != E_LB_BOOL) return 0;
  dreg = fn->values[dv].reg;
  if (dreg < 0 || dreg >= fn->nregs) return 0;
  fprintf(f, "    { int c_;\n");
  fprintf(
      f,
      "      if (__builtin_expect(JS_VALUE_IS_BOTH_INT(v%u, v%u), 1))\n"
      "        c_ = JS_VALUE_GET_INT(v%u) %s JS_VALUE_GET_INT(v%u);\n"
      "      else if ((JS_VALUE_GET_TAG(v%u) == JS_TAG_INT || JS_VALUE_GET_TAG(v%u) == JS_TAG_FLOAT64) &&\n"
      "               (JS_VALUE_GET_TAG(v%u) == JS_TAG_INT || JS_VALUE_GET_TAG(v%u) == JS_TAG_FLOAT64))\n"
      "        c_ = (JS_VALUE_GET_TAG(v%u) == JS_TAG_INT ? (double)JS_VALUE_GET_INT(v%u) : JS_VALUE_GET_FLOAT64(v%u)) %s\n"
      "             (JS_VALUE_GET_TAG(v%u) == JS_TAG_INT ? (double)JS_VALUE_GET_INT(v%u) : JS_VALUE_GET_FLOAT64(v%u));\n",
      (unsigned)av, (unsigned)bv, (unsigned)av, cop, (unsigned)bv, (unsigned)av, (unsigned)av,
      (unsigned)bv, (unsigned)bv, (unsigned)av, (unsigned)av, (unsigned)av, cop, (unsigned)bv,
      (unsigned)bv, (unsigned)bv);
  if (rel)
    fprintf(
        f,
        "      else if (q2_str_cmp(v%u, v%u, &c_))\n        c_ = c_ %s 0;\n",
        (unsigned)av, (unsigned)bv, cop);
  else
    fprintf(
        f,
        "      else if (q2_str_eq(v%u, v%u, &c_))\n        c_ = %sc_;\n",
        (unsigned)av, (unsigned)bv, strcmp(cop, "!=") == 0 ? "!" : "");
  {
    int mk = in->rop == ROP_lt ? 0 : in->rop == ROP_lte ? 1 : in->rop == ROP_gt ? 2
           : in->rop == ROP_gte ? 3 : in->rop == ROP_eq ? 4 : in->rop == ROP_neq ? 5
           : in->rop == ROP_strict_eq ? 6 : 7;
    fprintf(f, "      else if (q2_mixed_cmp(v%u, v%u, %d, &c_)) { }\n", (unsigned)av, (unsigned)bv, mk);
    fprintf(f, "      else if (q2_ident_cmp(v%u, v%u, %d, &c_)) { }\n", (unsigned)av, (unsigned)bv, mk);
  }
  fprintf(f, "      else c_ = -1;\n");
  fprintf(f, "      if (__builtin_expect(c_ >= 0, 1)) {\n");
  if (t->lb[dv] == E_LB_TAG)
    fprintf(f, "        v%u = JS_MKVAL(JS_TAG_BOOL, c_);\n", (unsigned)dv);
  else
    fprintf(f, "        b%u = c_;\n", (unsigned)dv);
  fprintf(f, "      } else {\n");
  if (!emit_typed_opcall(f, fn, in, t, cur, NULL, hex, fi, b)) {
    fprintf(stderr, "qjsaot: compare at pc %d: the wrapper declined after output\n", in->pc);
    abort();
  }
  fprintf(f, "      }\n    }\n");
  cur[dreg] = dv;
  return 1;
}

/* A speculative-body `this` FIELD READ: `get_this_field` / `get_this_field2`
   ("rras", but BOTH register operands are destinations -- def[0] is the read and
   def[1] the receiver -- so there is no use to take a receiver from). `this` comes
   from the function's own this-slot via `qjs_aot_load_this`, exactly as the generic
   emitter does; the value is owned when `_own` is set, otherwise borrowed. */
static int emit_group_this_read(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur, int g,
    int site, Q2ValueId fv, Q2ValueId rv, int freg, int rreg) {
  char test[320], off[96];
  int skip = g_e_facts->skip[e_insn_idx(fn, in)];
  Q2ValueId *c2 = NULL;
  if (cur == NULL) return 0;
  if (t->lb[rv] != E_LB_TAG) return 0;
  if (t->lb[fv] != E_LB_TAG && t->lb[fv] != E_LB_I32 && t->lb[fv] != E_LB_F64 &&
      t->lb[fv] != E_LB_BOOL)
    return 0;
  e_note_stub(g_e_vx, g_e_vx_cur, g_e_vx_blk, &g_e_vx_n, in->pc, cur, NULL);
  e_note_tag(
      'v', in->pc,
      g_e_fl ? "this_obj" : "*(JSValue *)((uint8_t *)f + QJS_AOT_OFF_REGRUN_THIS_OBJ)");
  e_sweep_guard(f, "vx", in->pc);
  if (t->lb[fv] != E_LB_TAG && !e_def_dead(in, fv)) {
    /* A guard that misses exits at the next instruction with `this` current: the stub syncs
       it from its local. */
    c2 = malloc(sizeof(*c2) * (size_t)(fn->nregs > 0 ? fn->nregs : 1));
    if (c2 == NULL) abort();
    memcpy(c2, cur, sizeof(*c2) * (size_t)fn->nregs);
    c2[rreg] = rv;
    e_note_stub(g_e_dx, g_e_dx_cur, g_e_dx_blk, &g_e_dx_n, in->next, c2, in);
    free(c2);
  }
  e_group_test(
      test, sizeof(test), skip, g_e_facts->cellskip[e_insn_idx(fn, in)] & 1, "_t", g, site, 0);
  e_slot_off(off, sizeof(off), g, site);
  {
    /* An inlined body's first test of `this`: its caller may already know the answer. */
    const int okt = g_e_inl && g_e_fl && e_group_layout(g) != 0 && fn->nrpo > 0 &&
                    g_e_blk == fn->rpo[0] && g_e_last_inl_tl == 0;
    if (okt) g_e_last_inl_tl = e_group_layout(g);
    fprintf(
        f,
        "    { JSValue _t = %s, _f;\n"
        "      if (__builtin_expect(%sJS_VALUE_GET_TAG(_t) != JS_TAG_OBJECT, 0)) goto vx_%d;\n"
        "      if (__builtin_expect(%s%s, 0)) goto vx_%d;\n"
        "      _f = *%s(_t, %s);\n",
        g_e_fl ? "this_obj" : "*(JSValue *)((uint8_t *)f + QJS_AOT_OFF_REGRUN_THIS_OBJ)",
        okt ? "!this_ok_ && " : "", in->pc, okt ? "!this_ok_ && " : "", test, in->pc,
        e_slotfn(site), off);
  }
  if (!g_e_nolazy)
    fprintf(f, "      if (__builtin_expect(JS_VALUE_GET_TAG(_f) == QJS_AOT_TAG_LAZY_JSON, 0)) goto vx_%d;\n", in->pc);
  fprintf(f, "      v%u = _t;\n", (unsigned)rv);
  if (g_e_fl && e_group_layout(g) != 0 && fn->nrpo > 0 && g_e_blk == fn->rpo[0] && (int)rv < g_e_spill_nv) {
    fprintf(f, "      pp%u = q2_propp(_t);\n", (unsigned)rv);
    g_e_pp[rv] = 1;
  }
  if (!emit_hit_result(f, fn, in, t, fv, freg, "_f")) {
    fprintf(stderr, "qjsaot: grouped this read at pc %d: no guard for its def\n", in->pc);
    abort();
  }
  fprintf(f, "    }\n");
  cur[freg] = fv;
  cur[rreg] = rv;
  g_e_miss_exit_seen = 1;
  return 1;
}

/* The hit test of a plain (ungrouped) field read: a contracted site compares its
   layout's shape and loads a constant slot; any other reads through the PIC. */
int qjs_aot_layout_site(const uint8_t dg[16], int site, int *layout, int *slot);
int qjs_aot_prof_site_proto(const uint8_t dg[16], int site);
static void e_fprint_hit(FILE *f, const Q2Func *fn, const char *ind, const char *recv,
                         int site, const char *out) {
  int osite = 0, lay = 0, slot = 0;
  const uint8_t *dg = e_site_dg(fn, site, &osite);
  if (dg != NULL && qjs_aot_layout_site(dg, osite, &lay, &slot))
    fprintf(f,
            "%sif (__builtin_expect(!Q2_FORCE_MISS && q2_cslot_hit_lz(%s, %du, %du, "
            "%s, Q2_LZ), 1)) {\n",
            ind, recv, lay, slot, out);
  else if (dg != NULL && qjs_aot_prof_site_proto(dg, osite))
    fprintf(f,
            "%sif (__builtin_expect(!Q2_FORCE_MISS && q2_poly_read_lz(pic, holders, "
            "%s, %d, (int)q2_ns, %s, Q2_LZ, ({ static Q2PolyC c_; &c_; }), "
            "(const uint32_t *)(*(const uint8_t *const *)((const uint8_t *)ctx + "
            "QJS_AOT_OFF_CTX_RT) + QJS_AOT_OFF_RT_PIC_EPOCH)), 1)) {\n",
            ind, recv, site, out);
  else
    fprintf(f,
            "%sif (__builtin_expect(!Q2_FORCE_MISS && q2_own_read_hit(pic, holders, "
            "%s, %d, (int)q2_ns, %s), 1)) {\n",
            ind, recv, site, out);
}

static int emit_typed_this_field(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur,
    int have_types, char hex[33], int fi, int b) {
  const ERole *role = e_roles[in->rop];
  Q2ValueId fv, rv;
  int site, freg, rreg;

  if (!have_types || t == NULL) return 0;
  if (in->rop != ROP_get_this_field && in->rop != ROP_get_this_field2) return 0;
  if (role == NULL || role->control != E_CF_NONE) return 0;
  if (in->narg < 4 || in->arg[3].kind != 's') return 0;
  site = (int)in->arg[3].val;
  if ((unsigned)site == REG_IC_NONE) return 0;
  if (in->ndef < 2) return 0;
  fv = in->def[0];
  rv = in->def[1];
  if (fv >= (Q2ValueId)t->nvalues || rv >= (Q2ValueId)t->nvalues) return 0;
  freg = fn->values[fv].reg;
  rreg = fn->values[rv].reg;
  if (freg < 0 || freg >= fn->nregs || rreg < 0 || rreg >= fn->nregs) return 0;
  /* A grouped access (the facts say its miss exits) takes the group path. */
  {
    int g = e_exit_group(fn, in, site);
    if (g >= 0) return emit_group_this_read(f, fn, in, t, cur, g, site, fv, rv, freg, rreg);
    if (e_must_exit(fn, in)) return 0;
  }
  if (t->lb[fv] != E_LB_TAG) return 0;

  if (!e_fl_site_ok(fn, in, site)) return 0;
  if (g_e_fl) {
    e_note_stub(
        g_e_vx, g_e_vx_cur, g_e_vx_blk, &g_e_vx_n, in->pc, cur, NULL);
    fprintf(
        f,
        "    if (__builtin_expect(JS_VALUE_GET_TAG(this_obj) != JS_TAG_OBJECT, "
        "0)) goto vx_%d;\n"
        "    { JSValue _t = this_obj, _v;\n"
        "      v%u = _t;\n"
        "      AOT_SET_B(ctx, fr, %d, _t);\n",
        in->pc, (unsigned)rv, rreg * 16);
    e_fprint_hit(f, fn, "      ", "_t", site, "&_v");
    fprintf(
        f,
        "        v%u = _v;\n"
        "      } else {\n",
        (unsigned)fv);
    if (!emit_typed_opcall(f, fn, in, t, cur, NULL, hex, fi, b)) return 0;
    fprintf(f, "      }\n    }\n");
    if (cur != NULL) {
      cur[freg] = fv;
      cur[rreg] = rv;
    }
    return 1;
  }
  fprintf(
      f,
      "    { JSValue _t = *(JSValue *)((uint8_t *)f + "
      "QJS_AOT_OFF_REGRUN_THIS_OBJ), _v;\n"
      "      int _own = 0;\n"
      "      if (__builtin_expect(JS_VALUE_GET_TAG(_t) != JS_TAG_OBJECT, 0)) {\n"
      "        _own = qjs_aot_load_this(ctx, f, &_t);\n"
      "        if (__builtin_expect(_own < 0, 0)) return JS_EXCEPTION;\n"
      "      }\n");
  /* The receiver's local is the second def, and the frame slot is what the
     wrapper reads if the hit below misses. */
  fprintf(f, "      v%u = _t;\n", (unsigned)rv);
  fprintf(f, "      AOT_SET_B(ctx, fr, %d, _t);\n", rreg * 16);
  e_fprint_hit(f, fn, "      ", "_t", site, "&_v");
  fprintf(f, "        v%u = _v;\n", (unsigned)fv);
  /* The hit keeps `this`: an owned one is adopted by its frame slot, as the
     generic body's load_this does (the slot already holds it borrowed). The miss
     releases it instead, because the wrapper loads `this` again itself. */
  fprintf(
      f, "        if (_own) qjs_aot_set_reg_p(ctx, fr, %d, _t);\n", rreg * 16);
  fprintf(f, "      } else {\n");
  fprintf(f, "        if (_own) JS_FreeValue(ctx, _t);\n");
  if (!emit_typed_opcall(f, fn, in, t, cur, NULL, hex, fi, b)) return 0;
  fprintf(f, "      }\n");
  fprintf(f, "    }\n");
  if (cur != NULL) {
    cur[freg] = fv;
    cur[rreg] = rv;
  }
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
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur,
    int have_types, char hex[33], int fi, int b) {
  const ERole *role = e_roles[in->rop];
  Q2ValueId rv, d0v, d1v;
  int s1, s2, rreg, d0reg, d1reg;

  if (!have_types || t == NULL) return 0;
  if (in->rop != ROP_get_field_chain && in->rop != ROP_get_field_chain_own)
    return 0;
  if (role == NULL || role->control != E_CF_NONE) return 0;
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
  /* All three must be TAG. A typed second def means the IR decided the receiver
     register holds an int; storing the raw JSValue there is a miscompile (measured:
     21 -> 12), and doing it properly needs the same guard machinery the other
     emitters use. Decline, so the block hands off -- correct, merely slower. */
  if (t->lb[rv] != E_LB_TAG || t->lb[d0v] != E_LB_TAG) return 0;
  /* A typed SECOND def is a guard on the second link's raw result (the first
     link and the raw result go to the frame first, so an exit at the next
     instruction finds both). Only the grouped form has that guard. */
  if (t->lb[d1v] != E_LB_TAG &&
      !(e_exit_group(fn, in, s1) >= 0 && e_exit_group(fn, in, s2) >= 0))
    return 0;
  rreg = fn->values[rv].reg;
  d0reg = fn->values[d0v].reg;
  d1reg = fn->values[d1v].reg;
  if (rreg < 0 || rreg >= fn->nregs || d0reg < 0 || d0reg >= fn->nregs ||
      d1reg < 0 || d1reg >= fn->nregs)
    return 0;

  {
    int g1 = e_exit_group(fn, in, s1), g2 = e_exit_group(fn, in, s2);
    if (g1 >= 0 && g2 >= 0)
      return emit_group_chain(
          f, fn, in, t, cur, g1, s1, g2, s2, rv, d0v, d1v, d0reg, d1reg);
    /* Classed as a miss-exit by the facts but not groupable here: decline, so
       the block hands off (an exit) rather than emit the wrapper. */
    if (e_must_exit(fn, in)) return 0;
  }
  if (!e_fl_site_ok(fn, in, s1) || !e_fl_site_ok(fn, in, s2)) return 0;
  fprintf(f, "    { JSValue _c0, _c1;\n");
  fprintf(f, "      JSValue _rcv = v%u;\n", (unsigned)rv);
  e_fprint_hit(f, fn, "      ", "_rcv", s1, "&_c0");
  /* Link 1's result goes to its local; the frame slot is written too, because a
     link-2 miss hands the WHOLE instruction to the wrapper. */
  fprintf(f, "        v%u = _c0;\n", (unsigned)d0v);
  fprintf(f, "        AOT_SET_B(ctx, fr, %d, _c0);\n", d0reg * 16);
  e_fprint_hit(f, fn, "        ", "_c0", s2, "&_c1");
  fprintf(f, "          v%u = _c1;\n", (unsigned)d1v);
  fprintf(f, "        } else {\n");
  /* Link 2 missed: restore the receiver and hand the whole instruction over. */
  fprintf(f, "          AOT_SET_B(ctx, fr, %d, _rcv);\n", rreg * 16);
  g_e_fl_miss_ok = e_site_vouched(fn, s1) && e_site_vouched(fn, s2);
  if (!emit_typed_opcall(f, fn, in, t, cur, NULL, hex, fi, b)) {
    g_e_fl_miss_ok = 0;
    return 0;
  }
  fprintf(f, "        }\n");
  fprintf(f, "      } else {\n");
  fprintf(f, "        AOT_SET_B(ctx, fr, %d, _rcv);\n", rreg * 16);
  /* The same wrapper on the same instruction: having emitted once, it cannot
     decline now, and a decline here would leave half an instruction written. */
  if (!emit_typed_opcall(f, fn, in, t, cur, NULL, hex, fi, b)) {
    fprintf(
        stderr, "qjsaot: field chain at pc %d: second wrapper declined\n", in->pc);
    abort();
  }
  g_e_fl_miss_ok = 0;
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
   Operand registers are read from their LOCALS (a def is not a use): a TAG operand is synced to
   its frame slot first because the helper takes JSValues from the frame. */
static int e_poly_site_ok(const Q2Func *fn, const Q2Insn *in, int site);
static int emit_poly_write(
    FILE *f, const Q2Func *fn, const Q2Insn *in, Q2ValueId *cur, int site,
    Q2ValueId rv, const char *boxed);
static int emit_inl_field_write(
    FILE *f, const Q2Func *fn, const Q2Insn *in, Q2ValueId *cur, int site,
    Q2ValueId rv, const char *boxed);
static int emit_typed_field_write(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur,
    int have_types, char hex[33], int fi, int b) {
  const ERole *role = e_roles[in->rop];
  Q2ValueId rv, vv;
  int site, rreg, vreg, r;

  if (!have_types || t == NULL) return 0;
  if (in->rop != ROP_put_field) return 0;
  if (role == NULL || role->control != E_CF_NONE) return 0;
  if (in->nuse < 2 || in->narg < 4 || in->arg[2].kind != 's') return 0;
  site = (int)in->arg[2].val;
  rv = in->use[0];
  vv = in->use[1];
  if (rv >= (Q2ValueId)t->nvalues || vv >= (Q2ValueId)t->nvalues) return 0;
  /* The RECEIVER must be a TAG local (a number receiver misses the PIC anyway), but
     the VALUE may be typed: it is boxed for the frame slot the helper reads. */
  if (t->lb[rv] != E_LB_TAG) return 0;
  rreg = fn->values[rv].reg;
  vreg = fn->values[vv].reg;
  if (rreg < 0 || rreg >= fn->nregs || vreg < 0 || vreg >= fn->nregs) return 0;

  {
    char boxed[64];
    if (t->lb[vv] == E_LB_I32)
      snprintf(boxed, sizeof(boxed), "JS_MKVAL(JS_TAG_INT, i%u)", (unsigned)vv);
    else if (t->lb[vv] == E_LB_F64)
      snprintf(boxed, sizeof(boxed), "E_BOX_F64(d%u)", (unsigned)vv);
    else if (t->lb[vv] == E_LB_BOOL)
      snprintf(boxed, sizeof(boxed), "JS_MKVAL(JS_TAG_BOOL, b%u)", (unsigned)vv);
    else
      snprintf(boxed, sizeof(boxed), "v%u", (unsigned)vv);
    {
      int g = e_exit_group(fn, in, site);
      /* No reference counting inline: a store of a TAG value (an object, null)
         over what is usually a reference leaves on nearly every execution. */
      if (g >= 0) return emit_group_write(f, fn, in, cur, g, site, rv, boxed);
      if (e_poly_site_ok(fn, in, site))
        return emit_poly_write(f, fn, in, cur, site, rv, boxed);
      /* An inline body takes the same two cache forms a plain store does, with a
         miss that leaves for the call the body was copied from. */
      if (g_e_inl) return emit_inl_field_write(f, fn, in, cur, site, rv, boxed);
      if (e_must_exit(fn, in)) return 0;
      /* The inline store counts references. */
      if (g_e_fl) return 0;
    }
  fprintf(f, "    {\n");
  /* A typed value is boxed once for the hit test, the replay and the engine's store. */
  if (boxed[0] != 'v') {
    fprintf(f, "      JSValue _bx = %s;\n", boxed);
    snprintf(boxed, sizeof(boxed), "_bx");
  }
  fprintf(
      f,
      "      if (__builtin_expect(!Q2_FORCE_MISS && q2_put_field_hit(ctx, pic, "
      "v%u, %d, %s), 1)) {\n",
      (unsigned)rv, site, boxed);
  fprintf(f, "      } else {\n");
  /* An add-property store replays its cached transition first: that only grows the
     property array, so the registers need not be published. */
  if (!g_e_fl && in->narg > 1 && in->arg[1].kind == 'a')
    fprintf(f, "        int _ar = Q2_FORCE_MISS ? 0 : (q2_put_add(ctx, pic, holders, q2_ns, v%u, %d, %s) ? 1 :\n"
               "            qjs_aot_put_add(ctx, f, v%u, %s, %d));\n"
               "        if (_ar <= 0) {\n", (unsigned)rv, site, boxed, (unsigned)rv, boxed, site);
  /* A miss (an add-property store, a prototype receiver, a polymorphic site) runs
     the engine's cache-filling store directly, as the generic body does, instead of
     stepping the handler: the same safepoint, without the step. */
  if (!g_e_fl && in->narg > 1 && in->arg[1].kind == 'a') {
    int r2;
    for (r2 = 0; r2 < fn->norig; r2++) {
      Q2ValueId lv2 = cur != NULL ? cur[r2] : Q2_NO_VALUE;
      if (lv2 == Q2_NO_VALUE || lv2 == E_UNDEF || lv2 >= (Q2ValueId)t->nvalues)
        continue;
      if (t->lb[lv2] == E_LB_TAG)
        fprintf(f, "        AOT_SET_B(ctx, fr, %d, v%u);\n", r2 * 16, (unsigned)lv2);
    }
    fprintf(f, "        if (_ar < 0) return qjs_aot_throw_here(ctx, f, %d);\n", in->pc);
    fprintf(
        f,
        "        { JSValue _pv = %s;\n"
        "          if (qjs_aot_put_field_slow(ctx, f, q2_code(f) + %d, v%u,\n"
        "                  (int)qjs_aot_ld_u32(q2_code(f) + %d), %d, _pv) < 0)\n"
        "              return qjs_aot_throw_here(ctx, f, %d); }\n",
        boxed, in->pc, (unsigned)rv, in->arg[1].off, site, in->pc);
    fprintf(f, "        }\n");
    e_emit_group_recheck(f, fn, in, cur);
    fprintf(f, "      }\n");
    fprintf(f, "    }\n");
    return 1;
  }
  /* The frame must hold the operands before the wrapper syncs and steps. */
  fprintf(f, "        AOT_SET_B(ctx, fr, %d, v%u);\n", rreg * 16, (unsigned)rv);
  if (t->lb[vv] == E_LB_I32)
    fprintf(f, "        AOT_SET_B(ctx, fr, %d, JS_MKVAL(JS_TAG_INT, i%u));\n",
            vreg * 16, (unsigned)vv);
  else if (t->lb[vv] == E_LB_F64)
    fprintf(f, "        AOT_SET_B(ctx, fr, %d, E_BOX_F64(d%u));\n",
            vreg * 16, (unsigned)vv);
  else if (t->lb[vv] == E_LB_BOOL)
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

/* A field read's raw result `_f` becomes the def's local: a TAG def is a plain
   copy (borrowed under DRC); a typed def writes `_f` to the frame slot FIRST --
   so the generic body finds the op's result there if the guard fails -- then
   guards it into the local, a mismatch exiting at the NEXT pc. Returns 0 when
   the def's representation has no guard. */
static int emit_hit_result(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId dv,
    int dreg, const char *raw) {
  (void)fn;
  if (t->lb[dv] == E_LB_TAG) {
    fprintf(f, "        v%u = %s;\n", (unsigned)dv, raw);
    return 1;
  }
  /* The raw result reaches the frame only when the guard misses (the generic body finds
     the op's result there); a result nothing reads is neither guarded nor stored. */
  if (e_def_dead(in, dv)) return 1; /* nothing reads it: no guard, no store */
  e_sweep_guard_set(f, "dx", in->next, dreg * 16, raw);
  if (t->lb[dv] == E_LB_I32)
    fprintf(
        f,
        "        if (__builtin_expect(!Q2_I32_OK(%s, i%u), 0)) "
        "{ AOT_SET_B(ctx, fr, %d, %s); goto dx_%d; }\n",
        raw, (unsigned)dv, dreg * 16, raw, in->next);
  else if (t->lb[dv] == E_LB_F64)
    fprintf(
        f,
        "        if (JS_VALUE_GET_TAG(%s) == JS_TAG_INT) d%u = "
        "(double)JS_VALUE_GET_INT(%s);\n"
        "        else if (JS_VALUE_GET_TAG(%s) == JS_TAG_FLOAT64) d%u = "
        "JS_VALUE_GET_FLOAT64(%s);\n"
        "        else { AOT_SET_B(ctx, fr, %d, %s); goto dx_%d; }\n",
        raw, (unsigned)dv, raw, raw, (unsigned)dv, raw, dreg * 16, raw, in->next);
  else if (t->lb[dv] == E_LB_BOOL)
    fprintf(
        f,
        "        if (__builtin_expect(JS_VALUE_GET_TAG(%s) != JS_TAG_BOOL, 0)) "
        "{ AOT_SET_B(ctx, fr, %d, %s); goto dx_%d; }\n"
        "        b%u = JS_VALUE_GET_BOOL(%s);\n",
        raw, dreg * 16, raw, in->next, (unsigned)dv, raw);
  else
    return 0;
  return 1;
}

/* The exit stub of a grouped access: its guard failed BEFORE the instruction, so
   the generic body redoes it (a `vx_` stub, state from before the access). The
   tag logged is the receiver's. */
static void e_group_exit_stub(
    FILE *f, const Q2Insn *in, const Q2ValueId *cur, Q2ValueId rv) {
  char ex[64];
  e_note_stub(
      g_e_vx, g_e_vx_cur, g_e_vx_blk, &g_e_vx_n, in->pc, cur, NULL);
  snprintf(ex, sizeof(ex), "v%u", (unsigned)rv);
  e_note_tag('v', in->pc, ex);
  e_sweep_guard(f, "vx", in->pc);
}

/* ---- numeric element accesses on a checked dense-array region ----------------
   The first access to an array in a region checks its class and holey flag and loads its
   base pointer and count into the C locals ap<v> / an<v> (v = the receiver's root value);
   the facts say which later accesses may use them (`arrskip`). A miss of any kind -- not
   an array, a hole, an index out of range, a number of the wrong kind -- leaves the body
   at the instruction (the vx stub), which is why these accesses are no safepoint.

   The cached base and count of ANY array are dropped (the facts are killed, so the next
   access starts a new region) at every safepoint: a call, a getter, valueOf, an allocation
   that can run finalizers, and every element store the profile saw miss or grow (those are
   not region accesses at all, so they are op calls and safepoints), which covers a store
   that appends, grows or sets the length of an array that aliases the region's. A store
   to an existing in-range index of a checked array leaves base and count alone, so it
   keeps the region; a store to `length` is a field store whose cache never holds the
   length property, hence also a safepoint. */
static Q2ValueId e_region_root(const Q2Func *fn, const Q2Insn *in) {
  int rv = (int)in->use[0];
  while (rv >= 0 && rv < fn->nvalues && fn->values[rv].insn >= 0 &&
         g_e_facts->fwd[fn->values[rv].insn] != Q2_NO_VALUE)
    rv = (int)g_e_facts->fwd[fn->values[rv].insn];
  return (Q2ValueId)rv;
}

/* The array check and the loads, from the TAG local v<av>; `fail` is the statement that
   leaves. */
static void e_arr_region_start(FILE *f, Q2ValueId av, Q2ValueId root, const char *fail) {
  fprintf(
      f,
      "      if (__builtin_expect(Q2_FORCE_MISS || JS_VALUE_GET_TAG(v%u) != JS_TAG_OBJECT, 0)) %s;\n"
      "      { const unsigned char *_ap = (const unsigned char *)JS_VALUE_GET_PTR(v%u);\n"
      "        if (__builtin_expect(*(const uint16_t *)(_ap + QJS_AOT_OFF_JSOBJECT_CLASS_ID) !=\n"
      "                QJS_AOT_CLASS_ARRAY ||\n"
      "            QJS_AOT_ARR_ODD(_ap), 0)) %s;\n"
      "        ap%u = *(JSValue *const *)(_ap + QJS_AOT_OFF_JSOBJECT_ARRAY_VALUES);\n"
      "        an%u = *(const uint32_t *)(_ap + QJS_AOT_OFF_JSOBJECT_ARRAY_COUNT); }\n",
      (unsigned)av, fail, (unsigned)av, fail, (unsigned)root, (unsigned)root);
}

/* The element type of a typed-array class (1 + index among the typed arrays; see e_ta_class). */
typedef struct ETaInfo {
  const char *cname, *ctype;
  int is_float, is_u32, clamped;
} ETaInfo;
static int e_ta_info(int cls, ETaInfo *o) {
  memset(o, 0, sizeof(*o));
  switch (cls) {
    case 1: o->cname = "QJS_AOT_CLASS_UINT8C_ARRAY"; o->ctype = "uint8_t"; o->clamped = 1; return 1;
    case 2: o->cname = "QJS_AOT_CLASS_INT8_ARRAY"; o->ctype = "int8_t"; return 1;
    case 3: o->cname = "QJS_AOT_CLASS_UINT8_ARRAY"; o->ctype = "uint8_t"; return 1;
    case 4: o->cname = "QJS_AOT_CLASS_INT16_ARRAY"; o->ctype = "int16_t"; return 1;
    case 5: o->cname = "QJS_AOT_CLASS_UINT16_ARRAY"; o->ctype = "uint16_t"; return 1;
    case 6: o->cname = "QJS_AOT_CLASS_INT32_ARRAY"; o->ctype = "int32_t"; return 1;
    case 7: o->cname = "QJS_AOT_CLASS_UINT32_ARRAY"; o->ctype = "uint32_t"; o->is_u32 = 1; return 1;
    case 11: o->cname = "QJS_AOT_CLASS_FLOAT32_ARRAY"; o->ctype = "float"; o->is_float = 1; return 1;
    case 12: o->cname = "QJS_AOT_CLASS_FLOAT64_ARRAY"; o->ctype = "double"; o->is_float = 1; return 1;
    default: return 0;
  }
}

/* The typed array's check and the loads of its base pointer and length, from the TAG local v<av>: the
   class is the one the site was profiled with, and a detached or shrunken view has length 0 or less
   than the index, so the bounds test is the whole story. The element storage is `ap<root>` (as a
   byte pointer), the length `an<root>`. */
static void e_arr_region_start_cls(FILE *f, Q2ValueId av, Q2ValueId root, const char *fail, int cls) {
  ETaInfo ti;
  if (!e_ta_info(cls, &ti)) abort();
  fprintf(
      f,
      "      if (__builtin_expect(Q2_FORCE_MISS || JS_VALUE_GET_TAG(v%u) != JS_TAG_OBJECT, 0)) %s;\n"
      "      { const unsigned char *_ap = (const unsigned char *)JS_VALUE_GET_PTR(v%u);\n"
      "        if (__builtin_expect(*(const uint16_t *)(_ap + QJS_AOT_OFF_JSOBJECT_CLASS_ID) != %s, 0)) %s;\n"
      "        ap%u = *(JSValue *const *)(_ap + QJS_AOT_OFF_JSOBJECT_ARRAY_VALUES);\n"
      "        an%u = *(const uint32_t *)(_ap + QJS_AOT_OFF_JSOBJECT_ARRAY_COUNT); }\n",
      (unsigned)av, fail, (unsigned)av, ti.cname, fail, (unsigned)root, (unsigned)root);
}
/* The entry of a region over a packed array of kind `kind` (1 int32, 2 float64): the receiver is
   an object of class Array whose holey byte is clear and whose storage kind is exactly this one.
   The element storage becomes `pkp<root>` and the count `pkn<root>`, function-level locals, so a
   later access in the same region reuses them. */
static void e_arr_region_start_pk(FILE *f, Q2ValueId av, Q2ValueId root, const char *fail, int kind) {
  fprintf(
      f,
      "      if (__builtin_expect(Q2_FORCE_MISS || JS_VALUE_GET_TAG(v%u) != JS_TAG_OBJECT, 0)) %s;\n"
      "      { const unsigned char *_ap = (const unsigned char *)JS_VALUE_GET_PTR(v%u);\n"
      "        if (__builtin_expect(*(const uint16_t *)(_ap + QJS_AOT_OFF_JSOBJECT_CLASS_ID) !=\n"
      "                QJS_AOT_CLASS_ARRAY ||\n"
      "            (*(_ap + QJS_AOT_OFF_JSOBJECT_HOLEY_BYTE) & QJS_AOT_JSOBJECT_HOLEY_MASK) != 0 ||\n"
      "            *(_ap + QJS_AOT_OFF_JSOBJECT_ARR_KIND) != %d, 0)) %s;\n"
      "        pkp%u = *(void *const *)(_ap + QJS_AOT_OFF_JSOBJECT_ARRAY_VALUES);\n"
      "        pkn%u = *(const uint32_t *)(_ap + QJS_AOT_OFF_JSOBJECT_ARRAY_COUNT); }\n",
      (unsigned)av, fail, (unsigned)av, kind, fail, (unsigned)root, (unsigned)root);
}

static void e_arr_region_start_grp(FILE *f, Q2ValueId av, Q2ValueId root, const char *fail, int grp) {
  if (grp <= -101 && grp >= -112)
    e_arr_region_start_cls(f, av, root, fail, -100 - grp);
  else if (grp == E_GRP_PK_I32)
    e_arr_region_start_pk(f, av, root, fail, 1);
  else if (grp == E_GRP_PK_F64)
    e_arr_region_start_pk(f, av, root, fail, 2);
  else
    e_arr_region_start(f, av, root, fail);
}

/* A cache cell checked on the edge into a loop (or at its OSR entry): it must still hold the group's
   shape, in the kind its site needs. `kind` "vx" leaves at the loop header, "osr" returns to the
   generic body. */
static void e_cell_hoist_check(FILE *f, int site, int group, const char *kind, int pc) {
  int z, wr = 0;
  /* a contracted group never consults a cache cell */
  if (e_group_layout(group) != 0) return;
  /* a cell the entry validated is valid while the epoch has not moved, which every safepoint tests */
  if (e_cell_entry_valid(group, site)) return;
  for (z = 0; z < g_e_facts->nsites; z++)
    if (g_e_facts->sites[z].site == site) wr = g_e_facts->sites[z].write;
  if (strcmp(kind, "osr") == 0)
    fprintf(
        f, "      if (__builtin_expect(%s(pic, %d) != gsh%d, 0)) goto osr_fail;\n",
        wr ? "q2_cell_wshape" : "q2_cell_shape", site, group);
  else
    fprintf(
        f, "      if (__builtin_expect(%s(pic, %d) != gsh%d, 0)) goto vx_%d;\n",
        wr ? "q2_cell_wshape" : "q2_cell_shape", site, group, pc);
}

/* The index as `_i`, from an i32 local or a TAG local (which must hold an int). */
static int e_region_index(
    FILE *f, const ETypes *t, Q2ValueId kv, int pc) {
  if (t->lb[kv] == E_LB_I32) {
    fprintf(f, "      uint32_t _i = (uint32_t)i%u;\n", (unsigned)kv);
    return 1;
  }
  if (t->lb[kv] == E_LB_TAG) {
    fprintf(
        f,
        "      if (__builtin_expect(JS_VALUE_GET_TAG(v%u) != JS_TAG_INT, 0)) goto vx_%d;\n"
        "      uint32_t _i = (uint32_t)JS_VALUE_GET_INT(v%u);\n",
        (unsigned)kv, pc, (unsigned)kv);
    return 1;
  }
  return 0;
}

/* The statement that stores the number `expr` (an int32 when `isd` is 0, a double otherwise) into element
   `_i` of the typed array whose storage is ap<root>: the conversion each class makes on a store. */
static void e_ta_store_stmt(FILE *f, const ETaInfo *ti, unsigned root, int isd, const char *expr) {
  if (ti->is_float) {
    fprintf(f, "      ((%s *)ap%u)[_i] = (%s)(%s);\n", ti->ctype, root, ti->ctype, expr);
  } else if (ti->clamped) {
    if (isd)
      fprintf(
          f,
          "      { double _c = %s; ((uint8_t *)ap%u)[_i] = (_c != _c || _c <= 0) ? 0 : _c >= 255 ? 255 : "
          "(uint8_t)__builtin_rint(_c); }\n",
          expr, root);
    else
      fprintf(
          f, "      { int32_t _c = %s; ((uint8_t *)ap%u)[_i] = _c < 0 ? 0 : _c > 255 ? 255 : (uint8_t)_c; }\n", expr,
          root);
  } else if (isd) {
    fprintf(f, "      ((%s *)ap%u)[_i] = (%s)q2_toi32(%s);\n", ti->ctype, root, ti->ctype, expr);
  } else {
    fprintf(f, "      ((%s *)ap%u)[_i] = (%s)(%s);\n", ti->ctype, root, ti->ctype, expr);
  }
}

/* A numeric element read on a checked typed array: the class is a compile-time fact, the length and the
   storage pointer are the region's locals, and the element goes to the result in the result's own
   representation (an exit where it does not fit). */
static int emit_ta_region_read(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur, int cls) {
  Q2ValueId dv = in->def[0], av = in->use[0], kv = in->use[1], root;
  int dreg, idx = e_insn_idx(fn, in);
  ETaInfo ti;
  char fail[32];
  if (!e_ta_info(cls, &ti)) return 0;
  if (dv >= (Q2ValueId)t->nvalues || av >= (Q2ValueId)t->nvalues || kv >= (Q2ValueId)t->nvalues ||
      t->lb[av] != E_LB_TAG)
    return 0;
  if (t->lb[dv] != E_LB_TAG && t->lb[dv] != E_LB_I32 && t->lb[dv] != E_LB_F64 && t->lb[dv] != E_LB_BOOL)
    return 0;
  if (t->lb[kv] != E_LB_I32 && t->lb[kv] != E_LB_TAG) return 0;
  dreg = fn->values[dv].reg;
  if (dreg < 0 || dreg >= fn->nregs) return 0;
  root = e_region_root(fn, in);
  snprintf(fail, sizeof(fail), "goto vx_%d", in->pc);
  e_group_exit_stub(f, in, cur, av);
  e_sweep_guard(f, "vx", in->pc);
  fprintf(f, "    {\n");
  if (!g_e_facts->arrskip[idx]) e_arr_region_start_cls(f, av, root, fail, cls);
  e_region_index(f, t, kv, in->pc);
  fprintf(
      f,
      "      if (__builtin_expect(_i >= an%u, 0)) goto vx_%d;\n"
      "      %s _x = ((const %s *)ap%u)[_i];\n",
      (unsigned)root, in->pc, ti.ctype, ti.ctype, (unsigned)root);
  if (t->lb[dv] == E_LB_I32) {
    if (ti.is_float)
      fprintf(
          f,
          "      { double _d = (double)_x; int32_t _y = (int32_t)_d;\n"
          "        if (__builtin_expect(!(_y == _d && !(_y == 0 && signbit(_d))), 0)) goto vx_%d;\n"
          "        i%u = _y; }\n",
          in->pc, (unsigned)dv);
    else if (ti.is_u32)
      fprintf(
          f, "      if (__builtin_expect(_x > 2147483647u, 0)) goto vx_%d;\n      i%u = (int32_t)_x;\n", in->pc,
          (unsigned)dv);
    else
      fprintf(f, "      i%u = (int32_t)_x;\n", (unsigned)dv);
  } else if (t->lb[dv] == E_LB_F64) {
    fprintf(f, "      d%u = (double)_x;\n", (unsigned)dv);
  } else if (t->lb[dv] == E_LB_BOOL) {
    fprintf(f, "      goto vx_%d;\n", in->pc);
  } else if (ti.is_float) {
    fprintf(f, "      v%u = E_TA_F64((double)_x);\n", (unsigned)dv);
  } else if (ti.is_u32) {
    fprintf(
        f, "      v%u = _x <= 2147483647u ? JS_MKVAL(JS_TAG_INT, (int32_t)_x) : qjs_aot_box_num((double)_x, 0);\n",
        (unsigned)dv);
  } else {
    fprintf(f, "      v%u = JS_MKVAL(JS_TAG_INT, (int32_t)_x);\n", (unsigned)dv);
  }
  fprintf(f, "    }\n");
  if (cur != NULL) cur[dreg] = dv;
  g_e_miss_exit_seen = 1;
  return 1;
}

/* A numeric element store on a checked typed array. */
static int emit_ta_region_write(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur, int cls) {
  Q2ValueId av = in->use[0], kv = in->use[1], vv = in->use[2], root;
  int idx = e_insn_idx(fn, in);
  ETaInfo ti;
  char fail[32], ex[48];
  (void)cur;
  if (!e_ta_info(cls, &ti)) return 0;
  if (av >= (Q2ValueId)t->nvalues || kv >= (Q2ValueId)t->nvalues || vv >= (Q2ValueId)t->nvalues ||
      t->lb[av] != E_LB_TAG)
    return 0;
  if (t->lb[kv] != E_LB_I32 && t->lb[kv] != E_LB_TAG) return 0;
  root = e_region_root(fn, in);
  snprintf(fail, sizeof(fail), "goto vx_%d", in->pc);
  e_group_exit_stub(f, in, cur, av);
  e_sweep_guard(f, "vx", in->pc);
  fprintf(f, "    {\n");
  if (!g_e_facts->arrskip[idx]) e_arr_region_start_cls(f, av, root, fail, cls);
  e_region_index(f, t, kv, in->pc);
  fprintf(f, "      if (__builtin_expect(_i >= an%u, 0)) goto vx_%d;\n", (unsigned)root, in->pc);
  if (t->lb[vv] == E_LB_I32) {
    snprintf(ex, sizeof(ex), "i%u", (unsigned)vv);
    e_ta_store_stmt(f, &ti, (unsigned)root, 0, ex);
  } else if (t->lb[vv] == E_LB_BOOL) {
    snprintf(ex, sizeof(ex), "(int32_t)b%u", (unsigned)vv);
    e_ta_store_stmt(f, &ti, (unsigned)root, 0, ex);
  } else if (t->lb[vv] == E_LB_F64) {
    snprintf(ex, sizeof(ex), "d%u", (unsigned)vv);
    e_ta_store_stmt(f, &ti, (unsigned)root, 1, ex);
  } else if (t->lb[vv] == E_LB_TAG) {
    fprintf(f, "      if (JS_VALUE_GET_TAG(v%u) == JS_TAG_INT) {\n", (unsigned)vv);
    fprintf(f, "        int32_t _xi = JS_VALUE_GET_INT(v%u);\n", (unsigned)vv);
    e_ta_store_stmt(f, &ti, (unsigned)root, 0, "_xi");
    fprintf(f, "      } else if (JS_VALUE_GET_TAG(v%u) == JS_TAG_FLOAT64) {\n", (unsigned)vv);
    fprintf(f, "        double _xd = JS_VALUE_GET_FLOAT64(v%u);\n", (unsigned)vv);
    e_ta_store_stmt(f, &ti, (unsigned)root, 1, "_xd");
    fprintf(f, "      } else goto vx_%d;\n", in->pc);
  } else {
    return 0;
  }
  fprintf(f, "    }\n");
  e_inl_store_mark(f);
  g_e_miss_exit_seen = 1;
  return 1;
}

/* A read inside a packed region: a bounds test and a load of the element in its own type, with no
   tag test (the kind's element type is a compile-time fact). An index at or past the count is a
   hole (or past the length): it leaves the region, and the generic code does the prototype
   lookup. */
static int emit_pk_region_read(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur, int kind) {
  Q2ValueId dv = in->def[0], av = in->use[0], kv = in->use[1], root;
  int dreg, idx = e_insn_idx(fn, in);
  char fail[32];
  if (kind != 1 && kind != 2) return 0;
  if (dv >= (Q2ValueId)t->nvalues || av >= (Q2ValueId)t->nvalues ||
      kv >= (Q2ValueId)t->nvalues || t->lb[av] != E_LB_TAG)
    return 0;
  if (t->lb[dv] != E_LB_TAG && t->lb[dv] != E_LB_I32 && t->lb[dv] != E_LB_F64 &&
      t->lb[dv] != E_LB_BOOL)
    return 0;
  if (t->lb[kv] != E_LB_I32 && t->lb[kv] != E_LB_TAG) return 0;
  dreg = fn->values[dv].reg;
  if (dreg < 0 || dreg >= fn->nregs) return 0;
  root = e_region_root(fn, in);
  snprintf(fail, sizeof(fail), "goto vx_%d", in->pc);
  e_group_exit_stub(f, in, cur, av);
  e_sweep_guard(f, "vx", in->pc);
  fprintf(f, "    {\n");
  if (!g_e_facts->arrskip[idx]) e_arr_region_start_pk(f, av, root, fail, kind);
  e_region_index(f, t, kv, in->pc);
  fprintf(f, "      if (__builtin_expect(_i >= pkn%u, 0)) goto vx_%d;\n", (unsigned)root, in->pc);
  if (kind == 1) {
    /* the element is an int32: every result representation can hold it */
    fprintf(f, "      { int32_t _x = ((const int32_t *)pkp%u)[_i];\n", (unsigned)root);
    if (t->lb[dv] == E_LB_I32)
      fprintf(f, "        i%u = _x; }\n", (unsigned)dv);
    else if (t->lb[dv] == E_LB_F64)
      fprintf(f, "        d%u = (double)_x; }\n", (unsigned)dv);
    else if (t->lb[dv] == E_LB_BOOL)
      fprintf(f, "        goto vx_%d; }\n", in->pc);
    else
      fprintf(f, "        v%u = JS_MKVAL(JS_TAG_INT, _x); }\n", (unsigned)dv);
  } else {
    /* the element is a double: boxing canonicalises NaN, and an int32 result needs the integral
       in-range test (with -0 excluded) that the packed store uses */
    fprintf(f, "      { double _x = ((const double *)pkp%u)[_i];\n", (unsigned)root);
    if (t->lb[dv] == E_LB_I32)
      fprintf(
          f,
          "        int32_t _y = (int32_t)_x;\n"
          "        if (__builtin_expect(!(_y == _x && !(_y == 0 && signbit(_x))), 0)) goto vx_%d;\n"
          "        i%u = _y; }\n",
          in->pc, (unsigned)dv);
    else if (t->lb[dv] == E_LB_F64)
      fprintf(f, "        d%u = _x; }\n", (unsigned)dv);
    else if (t->lb[dv] == E_LB_BOOL)
      fprintf(f, "        goto vx_%d; }\n", in->pc);
    else
      fprintf(f, "        v%u = E_BOX_F64(_x); }\n", (unsigned)dv);
  }
  fprintf(f, "    }\n");
  if (cur != NULL) cur[dreg] = dv;
  g_e_miss_exit_seen = 1;
  return 1;
}

static int emit_elem_region_read(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur) {
  Q2ValueId dv = in->def[0], av = in->use[0], kv = in->use[1], root;
  int dreg, idx = e_insn_idx(fn, in);
  char fail[32];
  if (e_ta_class(fn, in)) return emit_ta_region_read(f, fn, in, t, cur, e_ta_class(fn, in));
  {
    int pk = 0;
    if (e_pk_grp(fn, in, &pk) != 0) return emit_pk_region_read(f, fn, in, t, cur, pk);
  }
  if (dv >= (Q2ValueId)t->nvalues || av >= (Q2ValueId)t->nvalues ||
      kv >= (Q2ValueId)t->nvalues || t->lb[av] != E_LB_TAG)
    return 0;
  if (t->lb[dv] != E_LB_TAG && t->lb[dv] != E_LB_I32 && t->lb[dv] != E_LB_F64 &&
      t->lb[dv] != E_LB_BOOL)
    return 0;
  if (t->lb[kv] != E_LB_I32 && t->lb[kv] != E_LB_TAG) return 0;
  dreg = fn->values[dv].reg;
  if (dreg < 0 || dreg >= fn->nregs) return 0;
  root = e_region_root(fn, in);
  snprintf(fail, sizeof(fail), "goto vx_%d", in->pc);
  e_group_exit_stub(f, in, cur, av);
  e_sweep_guard(f, "vx", in->pc);
  fprintf(f, "    {\n");
  if (!g_e_facts->arrskip[idx]) e_arr_region_start(f, av, root, fail);
  e_region_index(f, t, kv, in->pc);
  fprintf(
      f,
      "      if (__builtin_expect(_i >= an%u, 0)) goto vx_%d;\n"
      "      JSValue _e = ap%u[_i];\n",
      (unsigned)root, in->pc, (unsigned)root);
  if (t->lb[dv] == E_LB_I32)
    fprintf(
        f,
        "      if (__builtin_expect(!Q2_I32_OK(_e, i%u), 0)) goto vx_%d;\n",
        (unsigned)dv, in->pc);
  else if (t->lb[dv] == E_LB_F64)
    fprintf(
        f,
        "      if (JS_VALUE_GET_TAG(_e) == JS_TAG_INT) d%u = (double)JS_VALUE_GET_INT(_e);\n"
        "      else if (JS_VALUE_GET_TAG(_e) == JS_TAG_FLOAT64) d%u = JS_VALUE_GET_FLOAT64(_e);\n"
        "      else goto vx_%d;\n",
        (unsigned)dv, (unsigned)dv, in->pc);
  else if (t->lb[dv] == E_LB_BOOL)
    fprintf(
        f,
        "      if (__builtin_expect(JS_VALUE_GET_TAG(_e) != JS_TAG_BOOL, 0)) goto vx_%d;\n"
        "      b%u = JS_VALUE_GET_BOOL(_e);\n",
        in->pc, (unsigned)dv);
  else {
    /* Any element but a hole (and a lazy marker while one can exist). */
    fprintf(
        f,
        "      if (__builtin_expect(JS_VALUE_GET_TAG(_e) == JS_TAG_UNINITIALIZED%s, 0)) goto vx_%d;\n"
        "      v%u = _e;\n",
        g_e_nolazy ? "" : " || JS_VALUE_GET_TAG(_e) == QJS_AOT_TAG_LAZY_JSON", in->pc,
        (unsigned)dv);
  }
  fprintf(f, "    }\n");
  if (cur != NULL) cur[dreg] = dv;
  g_e_miss_exit_seen = 1;
  return 1;
}

/* A store inside a packed region. A value of the kind stores in place and the region continues; a
   value that does not fit (a double into int32, a string, an out-of-bounds index) leaves the
   region, and the op call widens, unpacks or grows the array as the engine decides. */
static int emit_pk_region_write(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur, int kind) {
  Q2ValueId av = in->use[0], kv = in->use[1], vv = in->use[2], root;
  int idx = e_insn_idx(fn, in);
  char fail[32];
  if (kind != 1 && kind != 2) return 0;
  if (av >= (Q2ValueId)t->nvalues || kv >= (Q2ValueId)t->nvalues ||
      vv >= (Q2ValueId)t->nvalues || t->lb[av] != E_LB_TAG)
    return 0;
  if (t->lb[kv] != E_LB_I32 && t->lb[kv] != E_LB_TAG) return 0;
  root = e_region_root(fn, in);
  snprintf(fail, sizeof(fail), "goto vx_%d", in->pc);
  e_group_exit_stub(f, in, cur, av);
  e_sweep_guard(f, "vx", in->pc);
  fprintf(f, "    {\n");
  if (!g_e_facts->arrskip[idx]) e_arr_region_start_pk(f, av, root, fail, kind);
  e_region_index(f, t, kv, in->pc);
  fprintf(f, "      if (__builtin_expect(_i >= pkn%u, 0)) goto vx_%d;\n", (unsigned)root, in->pc);
  if (kind == 1) {
    if (t->lb[vv] == E_LB_I32)
      fprintf(f, "      ((int32_t *)pkp%u)[_i] = i%u;\n", (unsigned)root, (unsigned)vv);
    else if (t->lb[vv] == E_LB_BOOL)
      fprintf(f, "      ((int32_t *)pkp%u)[_i] = (int32_t)b%u;\n", (unsigned)root, (unsigned)vv);
    else if (t->lb[vv] == E_LB_F64)
      /* a double that is not an int32 widens the array: only the integral, in-range, non -0 case
         stays in this region, and it stores the int32 the widening would have produced */
      fprintf(
          f,
          "      { double _d = d%u; int32_t _y = (int32_t)_d;\n"
          "        if (__builtin_expect(!(_y == _d && !(_y == 0 && signbit(_d))), 0)) goto vx_%d;\n"
          "        ((int32_t *)pkp%u)[_i] = _y; }\n",
          (unsigned)vv, in->pc, (unsigned)root);
    else if (t->lb[vv] == E_LB_TAG)
      fprintf(
          f,
          "      if (__builtin_expect(JS_VALUE_GET_TAG(v%u) != JS_TAG_INT, 0)) goto vx_%d;\n"
          "      ((int32_t *)pkp%u)[_i] = JS_VALUE_GET_INT(v%u);\n",
          (unsigned)vv, in->pc, (unsigned)root, (unsigned)vv);
    else
      return 0;
  } else {
    if (t->lb[vv] == E_LB_I32)
      fprintf(f, "      ((double *)pkp%u)[_i] = (double)i%u;\n", (unsigned)root, (unsigned)vv);
    else if (t->lb[vv] == E_LB_BOOL)
      fprintf(f, "      ((double *)pkp%u)[_i] = (double)b%u;\n", (unsigned)root, (unsigned)vv);
    else if (t->lb[vv] == E_LB_F64)
      fprintf(f, "      ((double *)pkp%u)[_i] = d%u;\n", (unsigned)root, (unsigned)vv);
    else if (t->lb[vv] == E_LB_TAG)
      fprintf(
          f,
          "      if (__builtin_expect(JS_VALUE_GET_TAG(v%u) == JS_TAG_INT, 0)) ((double *)pkp%u)[_i] = (double)JS_VALUE_GET_INT(v%u);\n"
          "      else if (JS_VALUE_GET_TAG(v%u) == JS_TAG_FLOAT64) ((double *)pkp%u)[_i] = JS_VALUE_GET_FLOAT64(v%u);\n"
          "      else goto vx_%d;\n",
          (unsigned)vv, (unsigned)root, (unsigned)vv, (unsigned)vv, (unsigned)root, (unsigned)vv, in->pc);
    else
      return 0;
  }
  fprintf(f, "    }\n");
  e_inl_store_mark(f);
  g_e_miss_exit_seen = 1;
  return 1;
}

static int emit_elem_region_write(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur) {
  Q2ValueId av = in->use[0], kv = in->use[1], vv = in->use[2], root;
  int idx = e_insn_idx(fn, in);
  char fail[32], vb[48];
  if (e_ta_class(fn, in)) return emit_ta_region_write(f, fn, in, t, cur, e_ta_class(fn, in));
  {
    int pk = 0;
    if (e_pk_grp(fn, in, &pk) != 0) return emit_pk_region_write(f, fn, in, t, cur, pk);
  }
  (void)cur;
  if (av >= (Q2ValueId)t->nvalues || kv >= (Q2ValueId)t->nvalues ||
      vv >= (Q2ValueId)t->nvalues || t->lb[av] != E_LB_TAG)
    return 0;
  if (t->lb[kv] != E_LB_I32 && t->lb[kv] != E_LB_TAG) return 0;
  if (t->lb[vv] == E_LB_I32)
    snprintf(vb, sizeof(vb), "JS_MKVAL(JS_TAG_INT, i%u)", (unsigned)vv);
  else if (t->lb[vv] == E_LB_F64)
    snprintf(vb, sizeof(vb), "E_BOX_F64(d%u)", (unsigned)vv);
  else if (t->lb[vv] == E_LB_BOOL)
    snprintf(vb, sizeof(vb), "JS_MKVAL(JS_TAG_BOOL, b%u)", (unsigned)vv);
  else if (t->lb[vv] == E_LB_TAG)
    snprintf(vb, sizeof(vb), "v%u", (unsigned)vv);
  else
    return 0;
  root = e_region_root(fn, in);
  snprintf(fail, sizeof(fail), "goto vx_%d", in->pc);
  e_group_exit_stub(f, in, cur, av);
  e_sweep_guard(f, "vx", in->pc);
  fprintf(f, "    {\n");
  if (!g_e_facts->arrskip[idx]) e_arr_region_start(f, av, root, fail);
  e_region_index(f, t, kv, in->pc);
  fprintf(
      f,
      "      if (__builtin_expect(_i >= an%u, 0)) goto vx_%d;\n"
      "      JSValue *_s = &ap%u[_i];\n"
      "      if (__builtin_expect((JS_VALUE_GET_TAG(*_s) & ~(int)(JS_TAG_FLOAT64 | JS_TAG_BOOL)) != 0, 0)) goto vx_%d;\n"
      "      { JSValue _nv = %s;\n",
      (unsigned)root, in->pc, (unsigned)root, in->pc, vb);
  if (t->lb[vv] == E_LB_TAG)
    fprintf(f, "        if (__builtin_expect(JS_VALUE_GET_TAG(_nv) < 0, 0)) goto vx_%d;\n", in->pc);
  fprintf(f, "        *_s = _nv; }\n    }\n");
  e_inl_store_mark(f);
  g_e_miss_exit_seen = 1;
  return 1;
}

/* The slot accessor of a grouped read: the receiver's own slots, or the holder's for a
   prototype read. */
static const char *e_slotfn(int site) {
  int pr = g_e_facts != NULL ? e_facts_site_proto(g_e_facts, site) : 0;
  return pr == 2 ? "q2_hslot2p" : pr == 1 ? "q2_hslotp" : "q2_slotp";
}
/* The cache entry index of a site's cell: the site itself, or for a holder two prototypes up
   the way the entry was found in at entry. */
static const char *e_cellidx(char *buf, size_t n, int site) {
  if (g_e_facts != NULL && e_facts_site_proto(g_e_facts, site) == 2)
    snprintf(buf, n, "pci%d", site);
  else
    snprintf(buf, n, "%d", site);
  return buf;
}

/* The contracted layout a group reads, per group, or 0. A group is contracted when
   every one of its sites is an own data access of one layout's shape, so its shape
   IS that layout's shape (the unit holds a reference to it, so it never changes in
   place) and each site's field sits at a constant slot of it. Decided once per
   body, before any access is emitted. */
int qjs_aot_layout_site(const uint8_t dg[16], int site, int *layout, int *slot);
static int *g_e_glay;
static int *g_e_gret; /* per group: the layout its shape equals, though it reads prototype sites too */
static int g_e_glay_n;

static void e_compute_group_layouts(const Q2Func *fn, int ng) {
  int g, q;
  free(g_e_glay);
  free(g_e_gret);
  g_e_glay = calloc((size_t)(ng > 0 ? ng : 1), sizeof(*g_e_glay));
  g_e_gret = calloc((size_t)(ng > 0 ? ng : 1), sizeof(*g_e_gret));
  g_e_glay_n = g_e_glay != NULL && g_e_gret != NULL ? ng : 0;
  if (g_e_glay == NULL || g_e_gret == NULL || g_e_facts == NULL || getenv("QJS_AOT_NOCONTRACT") != NULL) return;
  for (g = 0; g < ng; g++) {
    const EGroup *gr = &g_e_facts->groups[g];
    int lay = 0;
    if (!g_e_group_used[g] || gr->nsites <= 0) continue;
    for (q = 0; q < gr->nsites; q++) {
      int osite, l = 0, sl = 0;
      const uint8_t *dg = e_site_dg(fn, gr->sites[q], &osite);
      if (e_facts_site_proto(g_e_facts, gr->sites[q]) != 0 ||
          !qjs_aot_layout_site(dg, osite, &l, &sl) || (lay != 0 && lay != l)) {
        lay = 0;
        break;
      }
      lay = l;
    }
    g_e_glay[g] = lay;
    if (lay == 0) {
      int pure = 0, rl = e_facts_group_layout(fn, g_e_facts, g, &pure);
      if (rl != 0 && !pure) g_e_gret[g] = rl;
    }
    if (lay != 0 && getenv("QJS_AOT_LAYOUTCENSUS") != NULL)
      fprintf(stderr, "qjsaot: group %d contracted to layout %d (%d sites)\n", g, lay, gr->nsites);
  }
}

static int e_group_layout(int g) {
  return g >= 0 && g < g_e_glay_n ? g_e_glay[g] : 0;
}
static int e_group_ret(int g) {
  return g >= 0 && g < g_e_glay_n ? g_e_gret[g] : 0;
}

/* A group whose shape the entry does not read from the cache cells: a copied callee's
   (set where its guard runs) and a contracted one (its layout's shape). */
static int e_entry_skip(int g) {
  return g_e_facts->groups[g].own || e_group_layout(g) != 0;
}

/* The byte offset of `site`'s field in a receiver of group `g`'s shape: a constant
   for a contracted group, otherwise the offset the site's cache cell holds. */
static const Q2Func *g_e_glay_fn;
static void e_slot_off(char *out, size_t n, int g, int site) {
  int l = 0, sl = 0, osite;
  if (e_group_layout(g) != 0 && g_e_glay_fn != NULL) {
    const uint8_t *dg = e_site_dg(g_e_glay_fn, site, &osite);
    if (qjs_aot_layout_site(dg, osite, &l, &sl)) {
      snprintf(out, n, "%uu * QJS_AOT_SIZEOF_JSPROPERTY", (unsigned)sl);
      return;
    }
  }
  {
    char ci[24];
    snprintf(out, n, "q2_cell_off(pic, %s)", e_cellidx(ci, sizeof(ci), site));
  }
}

/* The test that sends a grouped access to its exit: the receiver must be an
   object of the group's shape (skipped when the facts already hold that), and
   this site's own cell must still say the same (a refill after entry would move
   the offset). `write` selects the own-writable cell kind.

   Nothing is rechecked after a call or an op call. A safepoint drops every shape
   fact, so no access after it skips its shape test, and every access compares its
   OWN cell against the group's shape and reads its offset from that same cell, never
   from an entry-time local: a cell that was refilled or emptied meanwhile makes the
   access miss, it cannot make it use another shape's slot. Keep it that way: an
   access that read its offset early but tested the current cell would be unsound. */
static void e_group_test(
    char *out, size_t n, int skip, int cellskip, const char *recv, int g, int site,
    int write) {
  char cell[200], ci[24];
  int proto = !write && g_e_facts != NULL ? e_facts_site_proto(g_e_facts, site) : 0;
  /* A contracted group's shape is the layout's, held by the unit: the receiver's
     shape alone decides, and no cell is consulted. */
  if (e_group_layout(g) != 0) {
    if (skip)
      snprintf(out, n, "0");
    else
      snprintf(out, n, "q2_shape_of(%s) != gsh%d", recv, g);
    if (skip)
      g_e_n_skip++;
    else
      g_e_n_shape++;
    return;
  }
  snprintf(
      cell, sizeof(cell), "%s(pic, %s) != gsh%d",
      write ? "q2_cell_wshape" : proto == 2 ? "q2_cell_shape" : "q2_cell_raw",
      e_cellidx(ci, sizeof(ci), site), g);
  /* A prototype read also needs the holder's shape to be the one the cell was filled with. */
  if (proto == 1)
    snprintf(
        cell + strlen(cell), sizeof(cell) - strlen(cell), " || q2_holder_bad(%s, holders, %d)",
        recv, site);
  else if (proto == 2)
    snprintf(
        cell + strlen(cell), sizeof(cell) - strlen(cell),
        " || q2_holder2_bad(%s, holders, (int)q2_ns, %s)", recv, ci);
  /* A cell the entry validated and no change of the epoch has followed needs no test, and neither does
     the holder of a prototype read: everything that changes a prototype in a way a cached read cares
     about (an addition, a deletion, an accessor, a read-only property, a new prototype) moves the epoch. */
  if (e_cell_entry_valid(g, site)) cellskip = 1;
  if (skip && cellskip) {
    snprintf(out, n, "0");
  } else if (skip) {
    snprintf(out, n, "%s", cell);
  } else if (cellskip) {
    snprintf(out, n, "q2_shape_of(%s) != gsh%d", recv, g);
  } else {
    snprintf(out, n, "q2_shape_of(%s) != gsh%d || %s", recv, g, cell);
  }
  if (skip)
    g_e_n_skip++;
  else
    g_e_n_shape++;
}

/* A grouped field READ: tag and shape test (unless implied), the slot load at
   the group's offset, the lazy-JSON test; every failure exits before the
   instruction. */
static int emit_group_read(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t,
    Q2ValueId *cur, int g, int site, Q2ValueId dv, Q2ValueId rv, int dreg) {
  char test[320], recv[32], cib[24];
  int skip = g_e_facts->skip[e_insn_idx(fn, in)];
  /* Decide everything before writing a byte. */
  if (t->lb[dv] != E_LB_TAG && t->lb[dv] != E_LB_I32 &&
      t->lb[dv] != E_LB_F64 && t->lb[dv] != E_LB_BOOL)
    return 0;
  snprintf(recv, sizeof(recv), "v%u", (unsigned)rv);
  e_group_exit_stub(f, in, cur, rv);
  e_group_test(test, sizeof(test), skip, g_e_facts->cellskip[e_insn_idx(fn, in)] & 1, recv, g, site, 0);
  fprintf(f, "    { JSValue _f;\n");
  fprintf(f, "      if (__builtin_expect(%s, 0)) goto vx_%d;\n", test, in->pc);
  {
    char off[96];
    e_slot_off(off, sizeof(off), g, site);
    if (e_mc_index(site) >= 0)
      fprintf(f, "      _f = mcv%d;\n", e_mc_index(site));
    else
      if (e_pp_of(recv) >= 0)
        fprintf(f, "      _f = *q2_slotpp(pp%d, %s);\n", e_pp_of(recv), off);
      else
        fprintf(f, "      _f = *%s(%s, %s);\n", e_slotfn(site), recv, off);
  }
  if (!g_e_nolazy)
    fprintf(
      f,
      "      if (__builtin_expect(JS_VALUE_GET_TAG(_f) == "
      "QJS_AOT_TAG_LAZY_JSON, 0)) goto vx_%d;\n",
      in->pc);
  if (!emit_hit_result(f, fn, in, t, dv, dreg, "_f")) {
    fprintf(
        stderr, "qjsaot: grouped read at pc %d: no guard for its def\n", in->pc);
    abort();
  }
  fprintf(f, "    }\n");
  if (cur != NULL) cur[dreg] = dv;
  g_e_miss_exit_seen = 1;
  return 1;
}

/* A grouped CHAINED read (`get_field_chain`: o.a.b): two grouped reads, the
   second on the first's result. Any failure exits before the instruction, so
   neither result is written until both loads have passed. Bit k of the facts' skip
   flag says link k's receiver test is implied. */
static int emit_group_chain(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t,
    Q2ValueId *cur, int g1, int s1, int g2, int s2, Q2ValueId rv, Q2ValueId d0v,
    Q2ValueId d1v, int d0reg, int d1reg) {
  char test[320], recv[32], cib[24];
  (void)fn;
  snprintf(recv, sizeof(recv), "v%u", (unsigned)rv);
  e_group_exit_stub(f, in, cur, rv);
  fprintf(f, "    { JSValue _c0, _c1;\n");
  e_group_test(test, sizeof(test), g_e_facts->skip[e_insn_idx(fn, in)] & 1, g_e_facts->cellskip[e_insn_idx(fn, in)] & 1, recv, g1, s1, 0);
  fprintf(f, "      if (__builtin_expect(%s, 0)) goto vx_%d;\n", test, in->pc);
  {
    char off[96];
    e_slot_off(off, sizeof(off), g1, s1);
    fprintf(f, "      _c0 = *%s(%s, %s);\n", e_slotfn(s1), recv, off);
  }
  if (!g_e_nolazy)
    fprintf(
      f,
      "      if (__builtin_expect(JS_VALUE_GET_TAG(_c0) == "
      "QJS_AOT_TAG_LAZY_JSON, 0)) goto vx_%d;\n",
      in->pc);
  e_group_test(test, sizeof(test), (g_e_facts->skip[e_insn_idx(fn, in)] >> 1) & 1, (g_e_facts->cellskip[e_insn_idx(fn, in)] >> 1) & 1, "_c0", g2, s2, 0);
  fprintf(f, "      if (__builtin_expect(%s, 0)) goto vx_%d;\n", test, in->pc);
  {
    char off[96];
    e_slot_off(off, sizeof(off), g2, s2);
    if (e_mc_index(s2) >= 0)
      fprintf(f, "      _c1 = mcv%d;\n", e_mc_index(s2));
    else
      fprintf(f, "      _c1 = *%s(_c0, %s);\n", e_slotfn(s2), off);
  }
  if (!g_e_nolazy)
    fprintf(
      f,
      "      if (__builtin_expect(JS_VALUE_GET_TAG(_c1) == "
      "QJS_AOT_TAG_LAZY_JSON, 0)) goto vx_%d;\n",
      in->pc);
  fprintf(f, "      v%u = _c0;\n", (unsigned)d0v);
  if (e_mc_index(s2) >= 0 && t->lb[d1v] == E_LB_TAG) e_mc_def(d1v, e_mc_index(s2));
  if (t->lb[d1v] == E_LB_TAG) {
    fprintf(f, "      v%u = _c1;\n    }\n", (unsigned)d1v);
  } else {
    /* The first link's result is current in the frame before the second def's
       guard can exit at the next instruction. */
    e_note_stub(
        g_e_dx, g_e_dx_cur, g_e_dx_blk, &g_e_dx_n, in->next, cur, in);
    fprintf(f, "      AOT_SET_B(ctx, fr, %d, _c0);\n", d0reg * 16);
    if (!emit_hit_result(f, fn, in, t, d1v, d1reg, "_c1")) {
      fprintf(
          stderr, "qjsaot: grouped chain at pc %d: no guard for its def\n", in->pc);
      abort();
    }
    fprintf(f, "    }\n");
  }
  if (cur != NULL) {
    cur[d0reg] = d0v;
    cur[d1reg] = d1v;
  }
  g_e_miss_exit_seen = 1;
  return 1;
}

/* A grouped field WRITE: the same tests, plus the prototype flag, then the
   engine's own store order (duplicate the new value, store, free the old one). */
static int emit_group_write(
    FILE *f, const Q2Func *fn, const Q2Insn *in, Q2ValueId *cur, int g,
    int site, Q2ValueId rv, const char *boxed) {
  char test[320], recv[32], woff[96];
  int skip = g_e_facts->skip[e_insn_idx(fn, in)];
  snprintf(recv, sizeof(recv), "v%u", (unsigned)rv);
  e_slot_off(woff, sizeof(woff), g, site);
  e_group_exit_stub(f, in, cur, rv);
  e_group_test(test, sizeof(test), skip, g_e_facts->cellskip[e_insn_idx(fn, in)] & 1, recv, g, site, 1);
  fprintf(f, "    {\n");
  fprintf(
      f, "      if (__builtin_expect(%s || q2_is_proto(%s), 0)) goto vx_%d;\n",
      test, recv, in->pc);
  if (in->own > 0 && g_e_undo_id != NULL && g_e_undo_id[e_insn_idx(fn, in)] >= 0) {
    /* a store of a copied callee: kept so that a way out of the copy can put it back */
    int id = g_e_undo_id[e_insn_idx(fn, in)];
    fprintf(f, "      JSValue _nv = %s;\n", boxed);
    fprintf(
        f,
        "      JSValue *_sp = q2_slotp(%s, %s);\n"
        "      uo%d = *_sp;\n"
        "      us%d = _sp;\n"
        "      q2_dup(_nv);\n"
        "      *_sp = _nv;\n"
        "      un%d = _nv;\n",
        recv, woff, id, id, id);
    fprintf(f, "    }\n");
    if (g_e_undo_seq_n < 256) {
      g_e_undo_seq[g_e_undo_seq_n] = id;
      g_e_undo_seq_own[g_e_undo_seq_n++] = in->own;
    } else {
      fprintf(stderr, "qjsaot: too many stores in copied callees\n");
      abort();
    }
    g_e_miss_exit_seen = 1;
    return 1;
  }
  /* The counted store the framed body makes: the new value duplicated before the
     old one is released. The release is an effect, which a frameless or inline
     body's exits account for. A local first: the boxed value may be a compound
     literal, whose commas would split a macro argument. */
  fprintf(f, "      JSValue _nv = %s;\n", boxed);
  if (boxed[0] != 'v') {
    /* A number or a bool: nothing to duplicate, only the old value to release. */
    fprintf(
        f,
        "      JSValue *_sp = q2_slotp(%s, %s);\n"
        "      JSValue _old = *_sp;\n"
        "      *_sp = _nv;\n"
        "      if (__builtin_expect(JS_VALUE_HAS_REF_COUNT(_old), 0)) "
        "JS_FreeValue(ctx, _old);\n",
        recv, woff);
  } else {
    int ov = g_e_facts->oldval[e_insn_idx(fn, in)] - 1;
    if (e_pp_of(recv) >= 0 && ov >= 0 && g_e_types != NULL && (unsigned)ov < (unsigned)g_e_types->nvalues &&
        g_e_types->lb[ov] == E_LB_TAG)
      fprintf(f, "      q2_put_slotpp_k%s(ctx, pp%d, %s, _nv, v%d);\n",
              g_e_facts->oldobj[e_insn_idx(fn, in)] ? "o" : "", e_pp_of(recv), woff, ov);
    else if (e_pp_of(recv) >= 0)
      fprintf(f, "      q2_put_slotpp(ctx, pp%d, %s, _nv);\n", e_pp_of(recv), woff);
    else
      fprintf(
          f, "      q2_put_slot(ctx, %s, %s, _nv);\n", recv, woff);
  }
  fprintf(f, "    }\n");
  e_inl_store_mark(f);
  g_e_miss_exit_seen = 1;
  return 1;
}

/* ---- polymorphic property sites -------------------------------
   A site the profile saw with 2-4 shapes, all own slots, is served by the per-site
   polymorphic cache: a compare chain over its four (shape, offset) entries, and on a
   miss `qjs_aot_poly_fill`, which runs no JS and only caches an own data slot, so
   the fill arm continues and the access is no safepoint. A property that is not own
   data (or an unhashed shape) is an EXIT before the instruction. Framed bodies
   only: the fill needs the frame. */
static int e_poly_site_ok(const Q2Func *fn, const Q2Insn *in, int site) {
  (void)fn;
  return e_facts_poly_on && g_e_facts != NULL && e_facts_poly_op(in) &&
         e_facts_site_poly(g_e_facts, site) && e_must_exit(fn, in);
}

/* The chain: leaves `_po` set or jumps to the exit. `write` selects the entry kind
   (a read accepts either kind, a write only a writable one). */
static void e_poly_chain(FILE *f, const char *recv, int site, int atom_off, int write, int pc) {
  const char *ctl = getenv("QJS_AOT_POLY_CONTROL");
  int k;
  fprintf(
      f,
      "      const void *_pe = q2_poly(f, %d), *_psh = q2_shape_of(%s);\n"
      "      uint32_t _po = 0;\n"
      "      if (__builtin_expect(_psh == NULL, 0)) goto vx_%d;\n"
      "      if (_pe != NULL && (\n",
      site, recv, pc);
  for (k = 0; k < 4; k++) {
    /* Negative control: "nocmp" drops the last compare, so any shape that misses
       the first three uses the fourth entry's offset. */
    if (ctl != NULL && strcmp(ctl, "nocmp") == 0 && k == 3)
      fprintf(f, "          (_po = q2_poly_off(_pe, 3), 1)");
    else if (write)
      fprintf(
          f,
          "          (q2_poly_shape(_pe, %d) == _psh && q2_poly_kind(_pe, %d) == 2 &&\n"
          "           (_po = q2_poly_off(_pe, %d), 1))",
          k, k, k);
    else
      fprintf(
          f, "          (q2_poly_shape(_pe, %d) == _psh && (_po = q2_poly_off(_pe, %d), 1))",
          k, k);
    fprintf(f, k < 3 ? " ||\n" : "\n");
  }
  fprintf(
      f,
      "          )) { }\n"
      "      else if (__builtin_expect(!qjs_aot_poly_fill(ctx, f, %d, %s,\n"
      "                   (int)qjs_aot_ld_u32(q2_code(f) + %d), %d, &_po), 0))\n"
      "          goto vx_%d;\n",
      site, recv, atom_off, write, pc);
}

static int emit_poly_read(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur,
    int site, Q2ValueId dv, Q2ValueId rv, int dreg) {
  char recv[32];
  if (t->lb[dv] != E_LB_TAG && t->lb[dv] != E_LB_I32 &&
      t->lb[dv] != E_LB_F64 && t->lb[dv] != E_LB_BOOL)
    return 0;
  snprintf(recv, sizeof(recv), "v%u", (unsigned)rv);
  e_group_exit_stub(f, in, cur, rv);
  fprintf(f, "    { JSValue _f;\n");
  e_poly_chain(f, recv, site, in->arg[2].off, 0, in->pc);
  fprintf(f, "      _f = *q2_slotp(%s, _po);\n", recv);
  if (!g_e_nolazy)
  fprintf(
        f,
        "      if (__builtin_expect(JS_VALUE_GET_TAG(_f) == QJS_AOT_TAG_LAZY_JSON, 0)) "
        "goto vx_%d;\n",
        in->pc);
  if (!emit_hit_result(f, fn, in, t, dv, dreg, "_f")) {
    fprintf(stderr, "qjsaot: polymorphic read at pc %d: no guard for its def\n", in->pc);
    abort();
  }
  fprintf(f, "    }\n");
  if (cur != NULL) cur[dreg] = dv;
  g_e_miss_exit_seen = 1;
  return 1;
}

static int emit_poly_write(
    FILE *f, const Q2Func *fn, const Q2Insn *in, Q2ValueId *cur, int site,
    Q2ValueId rv, const char *boxed) {
  char recv[32];
  (void)fn;
  snprintf(recv, sizeof(recv), "v%u", (unsigned)rv);
  e_group_exit_stub(f, in, cur, rv);
  fprintf(f, "    {\n");
  e_poly_chain(f, recv, site, in->arg[1].off, 1, in->pc);
  fprintf(
      f, "      if (__builtin_expect(q2_is_proto(%s), 0)) goto vx_%d;\n", recv,
      in->pc);
  fprintf(f, "      { JSValue _nv = %s;\n", boxed);
  if (boxed[0] != 'v') {
    fprintf(
        f,
        "        JSValue *_sp = q2_slotp(%s, _po);\n"
        "        JSValue _old = *_sp;\n"
        "        *_sp = _nv;\n"
        "        if (__builtin_expect(JS_VALUE_HAS_REF_COUNT(_old), 0)) "
        "JS_FreeValue(ctx, _old); }\n",
        recv);
  } else {
    fprintf(f, "        q2_put_slot(ctx, %s, _po, _nv); }\n", recv);
  }
  fprintf(f, "    }\n");
  g_e_miss_exit_seen = 1;
  return 1;
}

/* A FIELD WRITE inside an inline body: the two forms the engine's store cache keeps,
   tested and taken without the frame. The own-slot hit is `q2_put_field_hit`; the add
   of a new field replays the site's cached transition with `q2_put_add`, which only
   grows the property array, so nothing has left the body when it succeeds. A miss is
   an exit before the store, which is the call the callee was copied from: the store
   may run a setter, may grow the property array, or may not land at all on a frozen
   object, and with nothing done the caller's native call does all of that.

   The store that lands is a real effect, and the release it makes can free an object
   whose finalizer runs or park one, so the body is marked as having stored: its
   exits build the callee's frame instead of returning to the call. */
static int emit_inl_field_write(
    FILE *f, const Q2Func *fn, const Q2Insn *in, Q2ValueId *cur, int site,
    Q2ValueId rv, const char *boxed) {
  char recv[32];
  (void)fn;
  (void)cur;
  snprintf(recv, sizeof(recv), "v%u", (unsigned)rv);
  e_note_stub(g_e_vx, g_e_vx_cur, g_e_vx_blk, &g_e_vx_n, in->pc, cur, NULL);
  e_note_tag('v', in->pc, recv);
  e_sweep_guard(f, "vx", in->pc);
  fprintf(f, "    {\n");
  fprintf(f, "      JSValue _nv = %s;\n", boxed);
  fprintf(
      f,
      "      if (__builtin_expect(!Q2_FORCE_MISS && q2_put_field_hit(ctx, pic, %s, "
      "%d, _nv), 1)) {\n",
      recv, site);
  fprintf(f, "      } else if (__builtin_expect(!Q2_FORCE_MISS && q2_put_add(ctx, pic, "
             "holders, q2_ns, %s, %d, _nv), 0)) {\n",
          recv, site);
  fprintf(f, "      } else {\n");
  fprintf(f, "        goto vx_%d;\n", in->pc);
  fprintf(f, "      }\n");
  fprintf(f, "    }\n");
  e_inl_store_mark(f);
  g_e_miss_exit_seen = 1;
  return 1;
}

/* `get_local_field_0..3` ("ras": d, atom, site) and `get_local_field` ("ruas": d, slot,
   atom, site): a named read whose receiver is a variable slot in the frame. The inline
   cache hit is the same one a register receiver gets; a miss is the step. Framed
   bodies (a frameless body has no variable area). */
static int emit_typed_local_field(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur,
    int have_types, char hex[33], int fi, int b) {
  Q2ValueId dv;
  int site, slot, dreg;

  if (!have_types || t == NULL || g_e_fl) return 0;
  if (in->ndef < 1 || in->narg < 3 || e_must_exit(fn, in)) return 0;
  if (in->rop >= ROP_get_local_field_0 && in->rop <= ROP_get_local_field_3) {
    slot = in->rop - ROP_get_local_field_0;
    if (in->arg[2].kind != 's') return 0;
    site = (int)in->arg[2].val;
  } else if (in->rop == ROP_get_local_field) {
    if (in->narg < 4 || in->arg[1].kind != 'u' || in->arg[3].kind != 's') return 0;
    slot = (int)in->arg[1].val;
    site = (int)in->arg[3].val;
  } else {
    return 0;
  }
  if ((unsigned)site == REG_IC_NONE) return 0;
  dv = in->def[0];
  if (dv >= (Q2ValueId)t->nvalues) return 0;
  dreg = fn->values[dv].reg;
  if (dreg < 0 || dreg >= fn->nregs) return 0;
  if (t->lb[dv] != E_LB_TAG && t->lb[dv] != E_LB_I32 && t->lb[dv] != E_LB_F64 &&
      t->lb[dv] != E_LB_BOOL)
    return 0;
  e_note_stub(g_e_dx, g_e_dx_cur, g_e_dx_blk, &g_e_dx_n, in->next, cur, in);
  fprintf(f, "    { JSValue _f;\n");
  fprintf(
      f,
      "      if (__builtin_expect(!Q2_FORCE_MISS && q2_own_read_hit(pic, holders, "
      "q2_var_buf(f)[%d], %d, (int)q2_ns, &_f), 1)) {\n",
      slot, site);
  if (!emit_hit_result(f, fn, in, t, dv, dreg, "_f")) return 0;
  fprintf(f, "      } else {\n");
  if (!emit_typed_opcall(f, fn, in, t, cur, NULL, hex, fi, b)) return 0;
  fprintf(f, "      }\n    }\n");
  if (cur != NULL) cur[dreg] = dv;
  return 1;
}

/* A speculative-body FIELD READ: `get_field` / `get_field2` ("rras": dest,
   receiver, atom, site). This is a SEPARATE emitter from the generic body's, on
   purpose: the shared part is the runtime helper (q2_own_read_hit), not the emitted
   text, and refactoring the generic emitter to share its continuation broke the
   generic body the one time it was tried. So the body never reads a frame slot another body owns, by construction --
   no g_<pc> label, no jump to a generic label:
       hit  -> store into the def's local (TAG: plain copy, borrowed under DRC;
               typed: write _f to the frame slot FIRST so the generic body sees the
               result, then guard into the local, mismatch -> deopt at the NEXT pc)
       miss -> emit_typed_opcall, which syncs, stores operands, steps, and reloads.
   The receiver must already be a TAG local (never AOT_REG in the spec body); a
   typed receiver declines, because a number receiver misses the PIC anyway. */
int qjs_aot_layout_site(const uint8_t dg[16], int site, int *layout, int *slot);
static int emit_typed_field_read(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur,
    int have_types, char hex[33], int fi, int b) {
  const ERole *role = e_roles[in->rop];
  Q2ValueId dv, rv;
  int site, dreg, rreg;

  if (!have_types || t == NULL) return 0;
  if (in->op != Q2_OP_GET_FIELD) return 0;
  if (in->rop != ROP_get_field && in->rop != ROP_get_field_own &&
      in->rop != ROP_get_field2 && in->rop != ROP_get_field2_own)
    return 0;
  if (role == NULL || role->control != E_CF_NONE) return 0;
  if (in->ndef < 1 || in->nuse < 1 || in->narg < 4 || in->arg[3].kind != 's')
    return 0;
  site = (int)in->arg[3].val;
  if (getenv("E_DBG5"))
    fprintf(stderr, "pc%d get_field: site=%d role=%p nuse=%d ndef=%d\n", in->pc, site,
            (void *)role, in->nuse, in->ndef);
  dv = in->def[0];
  rv = in->use[0];
  if (dv >= (Q2ValueId)t->nvalues || rv >= (Q2ValueId)t->nvalues) return 0;
  /* The receiver is a JSValue; a typed receiver would be a number, which misses
     the PIC, so decline and let the op-call path take it. */
  if (t->lb[rv] != E_LB_TAG) {
    if (getenv("E_DBG5")) fprintf(stderr, "pc%d get_field: receiver v%u is not a TAG value (lb %d)\n", in->pc, (unsigned)rv, t->lb[rv]);
    return 0;
  }
  dreg = fn->values[dv].reg;
  rreg = fn->values[rv].reg;
  if (dreg < 0 || dreg >= fn->nregs || rreg < 0 || rreg >= fn->nregs) return 0;

  /* A grouped access (the facts say its miss exits) takes the group path: the
     op-call wrapper below would make it a safepoint the facts did not assume. */
  {
    int g = e_exit_group(fn, in, site);
    if (g >= 0) {
      /* Register the guard exits' pcs ONCE per body, like the other dx_ users. */
      e_note_stub(
          g_e_dx, g_e_dx_cur, g_e_dx_blk, &g_e_dx_n, in->next, cur, in);
      return emit_group_read(f, fn, in, t, cur, g, site, dv, rv, dreg);
    }
    if (e_poly_site_ok(fn, in, site)) {
      e_note_stub(
          g_e_dx, g_e_dx_cur, g_e_dx_blk, &g_e_dx_n, in->next, cur, in);
      return emit_poly_read(f, fn, in, t, cur, site, dv, rv, dreg);
    }
    if (e_must_exit(fn, in)) return 0;
  }
  if (!e_fl_site_ok(fn, in, site)) return 0;
  /* Register the guard exits' pcs ONCE per body, like the other dx_ users. */
  e_note_stub(
      g_e_dx, g_e_dx_cur, g_e_dx_blk, &g_e_dx_n, in->next, cur, in);
  fprintf(f, "    { JSValue _f;\n");
  {
    /* A site that read one constant slot of one contracted layout: the hit is a
       compare against that layout's shape and a load at a fixed slot. */
    int osite_c, lay_c = 0, slot_c = 0;
    const uint8_t *dg_c = e_site_dg(fn, site, &osite_c);
    if (qjs_aot_layout_site(dg_c, osite_c, &lay_c, &slot_c))
      fprintf(
          f,
          "      if (__builtin_expect(!Q2_FORCE_MISS && q2_cslot_hit_lz(v%u, %du, %du, "
          "&_f, Q2_LZ), 1)) {\n",
          (unsigned)rv, lay_c, slot_c);
    else
      fprintf(
          f,
          "      if (__builtin_expect(!Q2_FORCE_MISS && q2_own_read_hit(pic, holders, "
          "v%u, %d, (int)q2_ns, &_f), 1)) {\n",
          (unsigned)rv, site);
  }
  if (!emit_hit_result(f, fn, in, t, dv, dreg, "_f")) return 0;
  /* The MISS path is the wrapper -- one copy of the op-call sequence, never an
     inlined duplicate. A string receiver's method comes
     from String.prototype, which the PIC does not cache: a per-site cell keeps its
     shape and the property's offset, and the value is read from the slot. */
  fprintf(f, "      } else {\n");
  if (!g_e_fl && !g_e_quiet && in->arg[2].kind == 'a' && in->arg[2].off >= 0) {
    fprintf(
        f,
        "        static Q2StrCell sc_%d;\n"
        "        if (q2_strproto_hit(ctx, v%u, &sc_%d, qjs_aot_ld_u32(q2_code(f) + %d), &_f)) {\n",
        in->pc, (unsigned)rv, in->pc, in->arg[2].off);
    if (!emit_hit_result(f, fn, in, t, dv, dreg, "_f")) return 0;
    fprintf(f, "        } else {\n");
    if (!emit_typed_opcall(f, fn, in, t, cur, NULL, hex, fi, b)) return 0;
    fprintf(f, "        }\n");
  } else {
    int ok;
    g_e_fl_miss_ok = e_site_vouched(fn, site);
    ok = emit_typed_opcall(f, fn, in, t, cur, NULL, hex, fi, b);
    g_e_fl_miss_ok = 0;
    if (!ok) return 0;
  }
  fprintf(f, "      }\n");
  fprintf(f, "    }\n");
  if (cur != NULL) cur[dreg] = dv;
  return 1;
}

/* `binary_cpool` (rop 169, "rbrbc": d, sub, a, kind, idx): the second operand is
   a constant-POOL entry, not a register. The decoding below is the generic body's
   (see its ROP_binary_cpool arm) -- same arg positions, same pool accessors, same
   refusal of a non-finite double -- so the two cannot drift apart on the layout.
   Keyed on ETypes: an f64 result takes the double form (no exit; doubles cannot
   overflow), an i32 result takes the int64 form with the overflow exit, and
   anything else declines so the block hands off. */
static int emit_typed_arith_cpool(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur,
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
  if (!qjs_aot_pool_num(q2_insn_g(fn, in), idx, &c)) return 0;
  tag = qjs_aot_pool_tag(q2_insn_g(fn, in), idx);
  if (tag != E_POOL_INT && tag != E_POOL_DOUBLE) return 0;
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
  if (t->lb[d] == E_LB_F64) {
    if (tag == E_POOL_DOUBLE && !__builtin_isfinite(c)) return 0;
    if (t->lb[a] == E_LB_F64)
      fprintf(f, "    d%u = d%u %s (%a);\n", (unsigned)d, (unsigned)a, op, c);
    else if (t->lb[a] == E_LB_I32)
      fprintf(f, "    d%u = (double)i%u %s (%a);\n", (unsigned)d, (unsigned)a, op,
              c);
    else
      return 0;
    if (cur != NULL && ld >= 0 && ld < fn->nregs) cur[ld] = d;
    return 1;
  }
  /* i32 result: only an INT pool entry can keep the value an int. */
  if (t->lb[d] == E_LB_I32 && tag == E_POOL_INT &&
      t->lb[a] == E_LB_I32) {
    int32_t ic = (int32_t)c;
    char nz[96] = "";
    /* a zero product with a negative operand is -0, which no int32 holds */
    if (sub == REG_BIN_MUL)
      snprintf(nz, sizeof(nz),
               "      if (__builtin_expect(_t == 0 && (i%u < 0 || %d < 0), 0)) goto ox_%d;\n",
               (unsigned)a, ic, in->pc);
    e_sweep_guard(f, "ox", in->pc);
    fprintf(
        f,
        "    { int64_t _t = (int64_t)i%u %s (int64_t)%d;\n"
        "      if (__builtin_expect(_t < INT32_MIN || _t > INT32_MAX, 0)) "
        "goto ox_%d;\n"
        "%s"
        "      i%u = (int32_t)_t; }\n",
        (unsigned)a, op, ic, in->pc, nz, (unsigned)d);
    e_note_stub(
        g_e_ox, g_e_ox_cur, g_e_ox_blk, &g_e_ox_n, in->pc, cur, NULL);
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
   No sync is needed before this store. Freeing the old value does not call into
   JS (a free never runs a finalizer inline here, and DRC parks objects anyway), and
   JS_DupValue allocates nothing. The box is a JS_MKVAL or the float64 constructor,
   neither of which can GC. If that ever changes, the sync must come FIRST.
   `JS_DupValue` is a no-op for ints, bools and doubles; the call is kept so the shape
   is uniform with the TAG case. */
static int emit_typed_var_ref_store(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur,
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
    case E_LB_I32:
      snprintf(box, sizeof(box), "JS_MKVAL(JS_TAG_INT, i%u)", (unsigned)a);
      break;
    case E_LB_F64:
      snprintf(box, sizeof(box), "E_BOX_F64(d%u)", (unsigned)a);
      break;
    case E_LB_BOOL:
      snprintf(box, sizeof(box), "JS_MKVAL(JS_TAG_BOOL, b%u)", (unsigned)a);
      break;
    case E_LB_TAG:
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
  e_inl_store_mark(f);
  (void)cur;
  return 1;
}

/* The i32 bitwise and shift group: `bitand`/`bitor`/`bitxor`/`shl`/`sar`/`shr`.
   A typed f64 operand is converted with ToInt32 first (q2_toi32, total, no exit);
   a typed i32 operand is used as is. The result is always i32, which is what the
   interpreter produces. `shr` is the logical shift, so it needs `(uint32_t)`
   operands to fill with zeros rather than sign bits. */
static int emit_typed_bitwise(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur,
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
  if (t->lb[d] != E_LB_I32 && !(in->rop == ROP_shr && t->lb[d] == E_LB_F64)) return 0;
  switch (in->rop) {
    case ROP_bitand: op = "&"; break;
    case ROP_bitor: op = "|"; break;
    case ROP_bitxor: op = "^"; break;
    case ROP_shl: op = "<<"; break;
    case ROP_sar: op = ">>"; break;
    case ROP_shr: op = NULL; break;  /* logical: needs the unsigned cast */
    default: return 0;
  }
  if (t->lb[a] != E_LB_I32 && t->lb[a] != E_LB_F64) return 0;
  if (t->lb[b] != E_LB_I32 && t->lb[b] != E_LB_F64) return 0;
  {
    char an[48], bn[48];
    e_toi32_expr(an, sizeof(an), t, a);
    e_toi32_expr(bn, sizeof(bn), t, b);
    ld = fn->values[d].reg;
    if (in->rop == ROP_shl)
      fprintf(
          f, "    i%u = (int32_t)((uint32_t)%s << ((uint32_t)%s & 31));\n",
          (unsigned)d, an, bn);
    else if (in->rop == ROP_sar)
      fprintf(f, "    i%u = %s >> ((uint32_t)%s & 31);\n", (unsigned)d, an, bn);
    else if (op != NULL)
      fprintf(f, "    i%u = %s %s %s;\n", (unsigned)d, an, op, bn);
    else if (t->lb[d] == E_LB_F64)
      fprintf(
          f, "    d%u = (double)((uint32_t)%s >> ((uint32_t)%s & 31));\n",
          (unsigned)d, an, bn);
    else {
      /* the result is an int only below 2^31: above it the generic body redoes the shift */
      e_sweep_guard(f, "ox", in->pc);
      fprintf(
          f,
          "    { uint32_t _u = (uint32_t)%s >> ((uint32_t)%s & 31);\n"
          "      if (__builtin_expect(_u > (uint32_t)INT32_MAX, 0)) goto ox_%d;\n"
          "      i%u = (int32_t)_u; }\n",
          an, bn, in->pc, (unsigned)d);
      e_note_stub(g_e_ox, g_e_ox_cur, g_e_ox_blk, &g_e_ox_n, in->pc, cur, NULL);
    }
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
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, int b,
    Q2ValueId *cur, int have_types, char hex[33]) {
  const ERole *role = e_roles[in->rop];
  const Q2Block *blk = &fn->blocks[b];
  Q2ValueId d, a;
  int32_t imm;
  int ld;

  if (in->rop != ROP_add_imm_self_branch &&
      in->rop != ROP_add_imm_self_branch_s)
    return 0;
  if (role == NULL || role->control != E_CF_BRANCH) return 0;
  if (!have_types || t == NULL) return 0;
  if (in->ndef < 1 || in->nuse < 1 || in->narg < 3) return 0;
  if (blk->nsucc != 1) return 0;
  d = in->def[0];
  a = in->use[0];
  if (d >= (Q2ValueId)t->nvalues || a >= (Q2ValueId)t->nvalues) return 0;
  if (t->lb[d] != E_LB_I32 || t->lb[a] != E_LB_I32) return 0;
  /* RD_OFF8: a signed 8-bit immediate. */
  imm = (int32_t)(int8_t)(in->arg[1].val & 0xff);
  ld = fn->values[d].reg;
  e_sweep_guard(f, "ox", in->pc);
  fprintf(
      f,
      "    { int32_t _s;\n"
      "      if (__builtin_expect(__builtin_add_overflow(i%u, (int32_t)%d, &_s), 0))\n"
      "        goto ox_%d;\n"
      "      i%u = _s; }\n",
      (unsigned)a, imm, in->pc, (unsigned)d);
  e_note_stub(
      g_e_ox, g_e_ox_cur, g_e_ox_blk, &g_e_ox_n, in->pc, cur, NULL);
  if (cur != NULL && ld >= 0 && ld < fn->nregs) cur[ld] = d;
  fprintf(f, "    /* fused update, then the unconditional back edge */\n");
  e_emit_arm(f, fn, b, 0, cur, hex, blk->succ_block[0]);
  return 1;
}

static int emit_typed_inc_branch(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, int b,
    Q2ValueId *cur, int have_types, char hex[33], int fi) {
  const ERole *role = e_roles[in->rop];
  const Q2Block *blk = &fn->blocks[b];
  Q2ValueId dv, av;
  int bv, si_take, si_else;
  char cn[32], cn2[32], bn[32];

  /* Both forms of the counter test: `_s` (short offset) and the register form.
     Also `add_imm_self_branch(_s)` -- `i += k` fused with the same test, which is
     how `for (i = s; i < e; i += 3)` compiles. */
  if (in->rop != ROP_inc_branch_lt_s && in->rop != ROP_inc_branch_lt) return 0;
  if (role == NULL || role->control != E_CF_BRANCH) return 0;
  if (!have_types || t == NULL || blk->nsucc < 2) return 0;
  if (in->ndef < 1 || in->nuse < 2) return 0;
  dv = in->def[0];
  /* The counter's PREVIOUS value is use[0] -- the block parameter that carries it
     round the loop. def[0] is the NEW value. Reading def as the source emitted
     `i5 = i5 + 1` on an uninitialised local (a def is not a use). */
  av = in->use[0];
  bv = in->use[1];
  /* The bound may be TAG (a field or a length read the profile saw as mixed): its
     tag is tested below, and a non-number goes to the cold arm. */
  if (t->lb[dv] == E_LB_TAG && t->lb[av] == E_LB_TAG &&
      (t->lb[bv] == E_LB_I32 || t->lb[bv] == E_LB_TAG)) {
    /* A counter that is a TAG value (an allocation or call earlier in the loop
       reloaded it from the frame): an int below INT32_MAX against an int bound is the
       whole fast path; anything else is a hand-off at this pc with the old counter. */
    int si_t = 0, si_e = 1;
    e_local_name(cn, sizeof(cn), dv);
    e_local_name(cn2, sizeof(cn2), av);
    e_local_name(bn, sizeof(bn), bv);
    fprintf(f, "    { int c_;\n");
    if (t->lb[bv] == E_LB_I32)
      fprintf(
          f,
          "    if (__builtin_expect(JS_VALUE_GET_TAG(%s) == JS_TAG_INT && "
          "JS_VALUE_GET_INT(%s) != INT32_MAX, 1)) {\n"
          "      int32_t n_ = JS_VALUE_GET_INT(%s) + 1;\n"
          "      %s = JS_MKVAL(JS_TAG_INT, n_);\n"
          "      c_ = n_ < %s;\n"
          "    } else goto hx_%d;\n",
          cn2, cn2, cn2, cn, bn, in->pc);
    else
      fprintf(
          f,
          "    if (__builtin_expect(JS_VALUE_GET_TAG(%s) == JS_TAG_INT && "
          "JS_VALUE_GET_INT(%s) != INT32_MAX && JS_VALUE_GET_TAG(%s) == JS_TAG_INT, 1)) {\n"
          "      int32_t n_ = JS_VALUE_GET_INT(%s) + 1;\n"
          "      %s = JS_MKVAL(JS_TAG_INT, n_);\n"
          "      c_ = n_ < JS_VALUE_GET_INT(%s);\n"
          "    } else goto hx_%d;\n",
          cn2, cn2, bn, cn2, cn, bn, in->pc);
    e_note_stub(
        g_e_hx, g_e_hx_cur, g_e_hx_blk, &g_e_hx_n, in->pc, cur, NULL);
    fprintf(f, "    if (c_) {\n");
    e_emit_arm(f, fn, b, si_t, cur, hex, blk->succ_block[si_t]);
    fprintf(f, "    } else {\n");
    e_emit_arm(f, fn, b, si_e, cur, hex, blk->succ_block[si_e]);
    fprintf(f, "    }\n    }\n");
    (void)fi;
    return 1;
  }
  if (t->lb[dv] != E_LB_I32 || t->lb[av] != E_LB_I32 ||
      (t->lb[bv] != E_LB_I32 && t->lb[bv] != E_LB_TAG))
    return 0;
  e_local_name(cn, sizeof(cn), dv);
  e_local_name(cn2, sizeof(cn2), av);
  e_local_name(bn, sizeof(bn), bv);
  /* succ[0] is the branch target: taken while the incremented counter is still
     below the bound. */
  si_take = 0;
  si_else = 1;
  e_sweep_guard(f, "ox", in->pc);
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
  if (t->lb[bv] == E_LB_TAG) {
    /* INT and FLOAT64 compare; anything else hands off at this instruction, with
       the state from before the increment (the counter's register still holds
       the old value), so the generic body redoes it. */
    fprintf(f, "    { int c_;\n");
    fprintf(
        f,
        "    if (__builtin_expect(JS_VALUE_GET_TAG(%s) == JS_TAG_INT, 1))\n"
        "        c_ = (%s < JS_VALUE_GET_INT(%s));\n"
        "    else if (JS_VALUE_GET_TAG(%s) == JS_TAG_FLOAT64)\n"
        "        c_ = ((double)%s < JS_VALUE_GET_FLOAT64(%s));\n"
        "    else goto hx_%d;\n",
        bn, cn, bn, bn, cn, bn, in->pc);
    e_note_stub(
        g_e_hx, g_e_hx_cur, g_e_hx_blk, &g_e_hx_n, in->pc, cur, NULL);
    fprintf(f, "    if (c_) {\n");
  } else {
    fprintf(f, "    if (%s < %s) {\n", cn, bn);
  }
  e_emit_arm(f, fn, b, si_take, cur, hex, blk->succ_block[si_take]);
  fprintf(f, "    } else {\n");
  e_emit_arm(f, fn, b, si_else, cur, hex, blk->succ_block[si_else]);
  fprintf(f, "    }\n");
  if (t->lb[bv] == E_LB_TAG) fprintf(f, "    }\n");
  /* The overflow exit lives after the block, keyed by this pc. */
  e_note_stub(
      g_e_ox, g_e_ox_cur, g_e_ox_blk, &g_e_ox_n, in->pc, cur, NULL);
  (void)fi;
  return 1;
}

/* The C expression for ToInt32 of value `a` (an I32 or F64 local). A double that is the sum or
   difference of two int32 values is exact, so its ToInt32 is the wrapped machine add. */
static void e_toi32_expr(char *buf, size_t n, const ETypes *t, Q2ValueId a) {
  if (t->lb[a] == E_LB_I32) {
    snprintf(buf, n, "i%u", (unsigned)a);
    return;
  }
  if (g_e_fn != NULL && a < (Q2ValueId)g_e_fn->nvalues && g_e_fn->values[a].insn >= 0) {
    const Q2Insn *di = &g_e_fn->insns[g_e_fn->values[a].insn];
    if (di->op == Q2_OP_BINARY && di->own == 0 && di->ndef == 1 && di->def[0] == a && di->nuse == 2 &&
        (di->sub == REG_BIN_ADD || di->sub == REG_BIN_SUB) &&
        di->use[0] < (Q2ValueId)t->nvalues && di->use[1] < (Q2ValueId)t->nvalues &&
        t->lb[di->use[0]] == E_LB_I32 && t->lb[di->use[1]] == E_LB_I32) {
      snprintf(
          buf, n, "(int32_t)((uint32_t)i%u %c (uint32_t)i%u)", (unsigned)di->use[0],
          di->sub == REG_BIN_ADD ? '+' : '-', (unsigned)di->use[1]);
      return;
    }
  }
  snprintf(buf, n, "q2_toi32(d%u)", (unsigned)a);
}

/* A typed binary with an IMMEDIATE operand: `binary_imm_s`, `add_imm_s`,
   `sub_imm_s` and `binary_cpool` whose pool entry is a foldable number. One
   operand is a local, the other a C literal, so the operation is the machine's
   with the same int64 overflow check as the register form. Returns 1 when it
   emitted the instruction. */
static int emit_typed_arith_imm(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur,
    int have_types) {
  const ERole *role = e_roles[in->rop];
  Q2ValueId d, a;
  int sub, ld;
  int32_t imm = 0;
  const char *op;
  int have_imm = 0;

  if (!have_types || t == NULL) return 0;
  if (role == NULL || role->control != E_CF_NONE) return 0;
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
      if (t->lb[a] == E_LB_I32) {
        snprintf(xn, sizeof(xn), "i%u", (unsigned)a);
        xrep = 'i';
      } else if (t->lb[a] == E_LB_F64) {
        e_toi32_expr(xn, sizeof(xn), t, a);
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
          if (t->lb[d] != E_LB_I32) return 0;
          fprintf(f, "    i%u = %s %s %d;\n", (unsigned)d, xn, bop, imm);
          break;
        }
        case REG_BIN_SHL:
          if (t->lb[d] != E_LB_I32) return 0;
          fprintf(
              f, "    i%u = (int32_t)((uint32_t)%s << ((uint32_t)%d & 31));\n",
              (unsigned)d, xn, imm);
          break;
        case REG_BIN_SAR:
          if (t->lb[d] != E_LB_I32) return 0;
          fprintf(f, "    i%u = %s >> (%d & 31);\n", (unsigned)d, xn, imm);
          break;
        default: /* REG_BIN_SHR */
          if (t->lb[d] == E_LB_I32) {
            e_sweep_guard(f, "ox", in->pc);
            fprintf(
                f,
                "    { uint32_t _u = (uint32_t)%s >> (%d & 31);\n"
                "      if (__builtin_expect(_u > (uint32_t)INT32_MAX, 0)) "
                "goto ox_%d;\n"
                "      i%u = (int32_t)_u; }\n",
                xn, imm, in->pc, (unsigned)d);
            e_note_stub(
                g_e_ox, g_e_ox_cur, g_e_ox_blk, &g_e_ox_n, in->pc, cur,
                NULL);
          } else if (t->lb[d] == E_LB_F64) {
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
  {
    int base = sub & ~REG_CMP_INVERT, inv = (sub & REG_CMP_INVERT) != 0;
    if ((base >= REG_BIN_LT && base <= REG_BIN_GTE) || base == REG_BIN_EQ || base == REG_BIN_NEQ ||
        base == REG_BIN_STRICT_EQ || base == REG_BIN_STRICT_NEQ) {
      /* a comparison of a number with a constant */
      const char *cop = base == REG_BIN_LT ? "<" : base == REG_BIN_LTE ? "<=" : base == REG_BIN_GT ? ">"
                        : base == REG_BIN_GTE ? ">=" : (base == REG_BIN_NEQ || base == REG_BIN_STRICT_NEQ) ? "!=" : "==";
      if (t->lb[d] != E_LB_BOOL || (t->lb[a] != E_LB_I32 && t->lb[a] != E_LB_F64)) return 0;
      ld = fn->values[d].reg;
      fprintf(
          f, "    b%u = (%s%u %s %s%d)%s;\n", (unsigned)d, t->lb[a] == E_LB_I32 ? "i" : "d", (unsigned)a, cop,
          t->lb[a] == E_LB_I32 ? "" : "(double)", imm, inv ? " ^ 1" : "");
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
  if (t->lb[d] == E_LB_F64 &&
      (t->lb[a] == E_LB_F64 || t->lb[a] == E_LB_I32)) {
    const char *fop = sub == REG_BIN_ADD ? "+"
                      : sub == REG_BIN_SUB ? "-"
                                           : "*";
    ld = fn->values[d].reg;
    if (t->lb[a] == E_LB_F64)
      fprintf(f, "    d%u = d%u %s (double)%d;\n", (unsigned)d, (unsigned)a,
              fop, imm);
    else
      fprintf(f, "    d%u = (double)i%u %s (double)%d;\n", (unsigned)d,
              (unsigned)a, fop, imm);
    if (cur != NULL && ld >= 0 && ld < fn->nregs) cur[ld] = d;
    return 1;
  }
  if (t->lb[d] != E_LB_I32) return 0;
  if (t->lb[a] != E_LB_I32) return 0;
  op = sub == REG_BIN_ADD ? "+" : (sub == REG_BIN_SUB ? "-" : "*");
  ld = fn->values[d].reg;
  fprintf(
      f, "    { int64_t _t = (int64_t)i%u %s (int64_t)%d;\n", (unsigned)a, op,
      imm);
  {
    int ovf = t->exits == NULL || (t->exits[d] & E_EX_OVF);
    int nz = sub == REG_BIN_MUL && (t->exits == NULL || (t->exits[d] & E_EX_NEGZERO));
    if (ovf || nz) e_sweep_guard(f, "ox", in->pc);
    if (ovf)
      fprintf(
          f,
          "      if (__builtin_expect(_t < INT32_MIN || _t > INT32_MAX, 0)) "
          "goto ox_%d;\n",
          in->pc);
    if (nz)
      fprintf(
          f, "      if (__builtin_expect(_t == 0 && (i%u < 0 || %d < 0), 0)) goto ox_%d;\n",
          (unsigned)a, imm, in->pc);
  }
  fprintf(f, "      i%u = (int32_t)_t; }\n", (unsigned)d);
  /* The exit hands over the state BEFORE the instruction: the destination register
     may be the operand's, and the generic body redoes the operation from it. */
  e_note_stub(
      g_e_ox, g_e_ox_cur, g_e_ox_blk, &g_e_ox_n, in->pc, cur, NULL);
  if (cur && ld >= 0 && ld < fn->nregs) cur[ld] = d;
  return 1;
}

/* `branch_if` / `branch_if_not` on a value the analysis holds as a BOOL: the
   local is already 0 or 1, so the test is the machine's. Both arms go through the
   one-function arm emitter. A TAG value needs a truthiness test (the generic
   body's `JS_VALUE_GET_INT(v) != 0` after a tag range check), which can run
   getters for an object -- not this lowering. */
static int emit_typed_branch_bool(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, int b,
    Q2ValueId *cur, int have_types, const char *hex) {
  const ERole *role = e_roles[in->rop];
  const Q2Block *blk = &fn->blocks[b];
  Q2ValueId a;
  int neg, si_take, si_else;
  char an[32];

  if (in->rop != ROP_branch_if && in->rop != ROP_branch_if_not) return 0;
  if (role == NULL || role->control != E_CF_BRANCH) return 0;
  if (!have_types || t == NULL || blk->nsucc < 2) return 0;
  if (in->nuse < 1) return 0;
  a = in->use[0];
  if (a >= (Q2ValueId)t->nvalues || t->lb[a] != E_LB_BOOL) return 0;
  neg = in->rop == ROP_branch_if_not;
  si_take = 0;
  si_else = 1;
  e_local_name(an, sizeof(an), a);
  fprintf(f, "    if (%s%s) {\n", neg ? "!" : "", an);
  e_emit_arm(f, fn, b, si_take, cur, hex, blk->succ_block[si_take]);
  fprintf(f, "    } else {\n");
  e_emit_arm(f, fn, b, si_else, cur, hex, blk->succ_block[si_else]);
  fprintf(f, "    }\n");
  return 1;
}

/* ---- TAG-operand fast paths ------------------------------------

   An arithmetic, bitwise, compare or unary op whose operand the analysis left
   TAG (the profile has no record for it, or saw only numbers through a path the
   typing could not use) used to be an op call. This is the one place that gives
   it the engine's own number fast path instead: box the operands, run
   `q2_arith` / `q2_unary_fast` (the interpreter's predicate, int and float64
   tags inline; in a frameless body only the inline cases), and on a hit move the
   result to the def's local exactly as a field read's hit does. A miss is the
   op-call wrapper in a framed body and an exit before the instruction in a
   frameless one -- the same `emit_typed_opcall` every accessor's miss arm uses.

   A TAG operand the profile says is not a number (a string it concatenates, an
   object) is declined here: its miss path would be the common case. */

/* Whether a value may be tested for a number at its use: no profile record, or a
   record that names a number and no string, big integer or symbol. An object in
   the set is allowed: the fused `get_this_field` and field-chain ops write one
   record for all their destinations, so the receiver's type is in the field's. */
static int e_operand_numeric(const Q2Func *fn, Q2ValueId v) {
  const Q2Value *x;
  if (v >= (Q2ValueId)fn->nvalues) return 0;
  x = &fn->values[v];
  if (!x->has_prof) return 1;
  if (x->ptypes & (Q2_T_STR | Q2_T_BIGINT | Q2_T_SYMBOL)) return 0;
  return (x->ptypes & (Q2_T_INT32 | Q2_T_NUM)) != 0;
}

/* The JSValue expression of a value, whatever its representation. */
static void e_boxed_operand(
    char *buf, size_t n, const ETypes *t, Q2ValueId v) {
  switch (t->lb[v]) {
    case E_LB_I32:
      snprintf(buf, n, "JS_MKVAL(JS_TAG_INT, i%u)", (unsigned)v);
      break;
    case E_LB_F64:
      snprintf(buf, n, "E_BOX_F64(d%u)", (unsigned)v);
      break;
    case E_LB_BOOL:
      snprintf(buf, n, "JS_MKVAL(JS_TAG_BOOL, b%u)", (unsigned)v);
      break;
    default:
      snprintf(buf, n, "v%u", (unsigned)v);
      break;
  }
}

/* The shape of the op, if it is one this handles: 0 none, 1 two-operand, 2 one
   operand and an immediate, 3 unary. Fills `sub` and the immediate. */
static int e_tag_numeric_shape(
    const Q2Insn *in, const ERole *role, int *sub, int32_t *imm) {
  if (role == NULL || role->control != E_CF_NONE || in->ndef != 1) return 0;
  switch (in->rop) {
    case ROP_binary_imm:
    case ROP_binary_imm_s:
      if (in->narg < 4 || in->arg[1].kind != 'b' || in->nuse != 1) return 0;
      if (in->arg[3].kind != 'i' && in->arg[3].kind != 'I') return 0;
      *sub = (int)in->arg[1].val;
      *imm = (int32_t)in->arg[3].val;
      break;
    case ROP_add_imm_s:
    case ROP_add_imm_self_s:
    case ROP_sub_imm_s:
    case ROP_sub_imm_self_s:
      if (in->narg < 3 || in->nuse != 1) return 0;
      if (in->arg[2].kind != 'i' && in->arg[2].kind != 'I') return 0;
      *sub = (in->rop == ROP_add_imm_s || in->rop == ROP_add_imm_self_s)
                 ? REG_BIN_ADD
                 : REG_BIN_SUB;
      *imm = (int32_t)in->arg[2].val;
      break;
    case ROP_dec:
      *sub = REG_UN_DEC;
      return in->nuse == 1 ? 3 : 0;
    case ROP_neg:
    case ROP_inc:
    case ROP_not:
      if (in->sub < 0 || in->nuse != 1) return 0;
      *sub = in->sub;
      return 3;
    default:
      /* The two-register arithmetic, bitwise and compare ops: a real REG_BIN_*. */
      if (in->nuse != 2 || role->nuse != 2) return 0;
      if (in->sub < 0 || in->sub >= 0x40) return 0;
      *sub = in->sub;
      if (*sub == REG_BIN_INSTANCEOF || *sub == REG_BIN_IN ||
          *sub == REG_BIN_PRIVATE_IN)
        return 0;
      return 1;
  }
  if (*sub < 0 || *sub >= 0x40) return 0;
  if (*sub == REG_BIN_INSTANCEOF || *sub == REG_BIN_IN ||
      *sub == REG_BIN_PRIVATE_IN)
    return 0;
  return 2;
}

/* `post_inc` / `post_dec` ("rrr": the ToNumeric value, the source, the value plus
   or minus one) on a TAG source: ToNumeric of a number is the number, so the
   first result is the source itself and the second is the unary fast path's. Both
   results reach the frame before either guard, because an exit at the next pc
   finds them there (the dx stub marks both registers as already stored). */
static int emit_tag_post(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t,
    Q2ValueId *cur, char hex[33], int fi, int b) {
  Q2ValueId d0 = in->def[0], d1 = in->def[1], src = in->use[0];
  int r0, r1, k, sign;
  char sn[64];

  if (in->ndef != 2 || in->nuse != 1) return 0;
  if (d0 >= (Q2ValueId)t->nvalues || d1 >= (Q2ValueId)t->nvalues ||
      src >= (Q2ValueId)t->nvalues)
    return 0;
  if (t->lb[src] != E_LB_TAG || !e_operand_numeric(fn, src)) return 0;
  r0 = fn->values[d0].reg;
  r1 = fn->values[d1].reg;
  if (r0 < 0 || r0 >= fn->nregs || r1 < 0 || r1 >= fn->nregs) return 0;
  for (k = 0; k < 2; k++) {
    int lb = t->lb[k ? d1 : d0];
    if (lb != E_LB_TAG && lb != E_LB_I32 && lb != E_LB_F64 && lb != E_LB_BOOL)
      return 0;
  }
  if (e_must_exit(fn, in)) return 0;
  sign = in->rop == ROP_post_inc ? REG_UN_INC : REG_UN_DEC;
  e_boxed_operand(sn, sizeof(sn), t, src);
  e_note_stub(
      g_e_dx, g_e_dx_cur, g_e_dx_blk, &g_e_dx_n, in->next, cur, in);
  fprintf(f, "    { JSValue _n = %s, _m;\n", sn);
  fprintf(
      f,
      "      if (__builtin_expect((JS_VALUE_GET_TAG(_n) == JS_TAG_INT ||\n"
      "                            JS_VALUE_GET_TAG(_n) == JS_TAG_FLOAT64) &&\n"
      "                           q2_unary_fast(%d, _n, &_m), 1)) {\n",
      sign);
  fprintf(f, "        AOT_SET_B(ctx, fr, %d, _n);\n", r0 * 16);
  fprintf(f, "        AOT_SET_B(ctx, fr, %d, _m);\n", r1 * 16);
  for (k = 0; k < 2; k++) {
    Q2ValueId dv = k ? d1 : d0;
    const char *raw = k ? "_m" : "_n";
    if (t->lb[dv] == E_LB_TAG) {
      fprintf(f, "        v%u = %s;\n", (unsigned)dv, raw);
      continue;
    }
    if (e_def_dead(in, dv)) continue;
    e_sweep_guard(f, "dx", in->next);
    if (t->lb[dv] == E_LB_I32)
      fprintf(
          f,
          "        if (__builtin_expect(!Q2_I32_OK(%s, i%u), 0)) "
          "goto dx_%d;\n",
          raw, (unsigned)dv, in->next);
    else if (t->lb[dv] == E_LB_F64)
      fprintf(
          f,
          "        if (JS_VALUE_GET_TAG(%s) == JS_TAG_INT) d%u = "
          "(double)JS_VALUE_GET_INT(%s);\n"
          "        else if (JS_VALUE_GET_TAG(%s) == JS_TAG_FLOAT64) d%u = "
          "JS_VALUE_GET_FLOAT64(%s);\n"
          "        else goto dx_%d;\n",
          raw, (unsigned)dv, raw, raw, (unsigned)dv, raw, in->next);
    else
      fprintf(
          f,
          "        if (__builtin_expect(JS_VALUE_GET_TAG(%s) != JS_TAG_BOOL, 0)) "
          "goto dx_%d;\n"
          "        b%u = JS_VALUE_GET_BOOL(%s);\n",
          raw, in->next, (unsigned)dv, raw);
  }
  fprintf(f, "      } else {\n");
  g_e_fl_miss_ok = 1;
  if (!emit_typed_opcall(f, fn, in, t, cur, NULL, hex, fi, b)) {
    g_e_fl_miss_ok = 0;
    return 0;
  }
  g_e_fl_miss_ok = 0;
  fprintf(f, "      }\n    }\n");
  if (cur != NULL) {
    cur[r0] = d0;
    cur[r1] = d1;
  }
  return 1;
}

static int emit_tag_numeric(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t,
    Q2ValueId *cur, int have_types, char hex[33], int fi, int b) {
  const ERole *role = e_roles[in->rop];
  int sub = 0, shape, ntag = 0, k, dreg;
  int32_t imm = 0;
  Q2ValueId dv;
  char aa[64], bb[64];

  if (!have_types || t == NULL) return 0;
  if ((in->rop == ROP_post_inc || in->rop == ROP_post_dec) && role != NULL &&
      role->control == E_CF_NONE)
    return emit_tag_post(f, fn, in, t, cur, hex, fi, b);
  shape = e_tag_numeric_shape(in, role, &sub, &imm);
  if (shape == 0) return 0;
  /* `**` has no fast path in the engine's predicate at all: it would leave on
     every execution. (The rest is pure arithmetic with no allocation, so even the
     out-of-line part of `q2_arith` is fine in a frameless body.) */
  if (shape != 3 && sub == REG_BIN_POW) return 0;
  dv = in->def[0];
  if (dv >= (Q2ValueId)t->nvalues) return 0;
  dreg = fn->values[dv].reg;
  if (dreg < 0 || dreg >= fn->nregs) return 0;
  if (t->lb[dv] != E_LB_TAG && t->lb[dv] != E_LB_I32 &&
      t->lb[dv] != E_LB_F64 && t->lb[dv] != E_LB_BOOL)
    return 0;
  for (k = 0; k < in->nuse; k++) {
    Q2ValueId v = in->use[k];
    if (v >= (Q2ValueId)t->nvalues) return 0;
    if (t->lb[v] == E_LB_TAG) {
      ntag++;
      if (!e_operand_numeric(fn, v)) return 0;
    }
  }
  /* Only reached where every typed lowering declined: an operand or the result
     is TAG, or the combination has no typed form. */
  (void)ntag;
  if (e_must_exit(fn, in)) return 0;

  e_boxed_operand(aa, sizeof(aa), t, in->use[0]);
  if (shape == 1)
    e_boxed_operand(bb, sizeof(bb), t, in->use[1]);
  else if (shape == 2)
    snprintf(bb, sizeof(bb), "JS_MKVAL(JS_TAG_INT, %d)", imm);
  if (t->lb[dv] != E_LB_TAG)
    e_note_stub(
        g_e_dx, g_e_dx_cur, g_e_dx_blk, &g_e_dx_n, in->next, cur, in);
  fprintf(f, "    { JSValue _r;\n");
  if (shape == 3)
    fprintf(
        f, "      if (__builtin_expect(q2_unary_fast(%d, %s, &_r), 1)) {\n", sub,
        aa);
  else
    fprintf(
        f, "      if (__builtin_expect(%s(%d, %s, %s, &_r), 1)) {\n",
        "q2_arith", sub, aa, bb);
  if (!emit_hit_result(f, fn, in, t, dv, dreg, "_r")) return 0;
  fprintf(f, "      } else {\n");
  g_e_fl_miss_ok = 1;
  if (!emit_typed_opcall(f, fn, in, t, cur, NULL, hex, fi, b)) {
    g_e_fl_miss_ok = 0;
    return 0;
  }
  g_e_fl_miss_ok = 0;
  fprintf(f, "      }\n");
  fprintf(f, "    }\n");
  if (cur != NULL) cur[dreg] = dv;
  return 1;
}

/* `branch_if` / `branch_if_not` on a value of any representation but BOOL (the
   typed form takes that): the interpreter's `regvm_to_bool`. The tags up to
   undefined (int, bool, null, undefined) are `payload != 0`; a float64 is
   `d != 0 && d == d`; the rest -- an object (the one HTMLDDA bit), a string, a
   symbol -- is the engine's `qjs_aot_to_bool`, which cannot fail and runs no JS,
   so there is no miss path and no hand-off. */
static int emit_tag_branch_truthy(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, int b,
    Q2ValueId *cur, int have_types, char hex[33]) {
  const ERole *role = e_roles[in->rop];
  const Q2Block *blk = &fn->blocks[b];
  Q2ValueId a;
  int neg;

  if (in->rop != ROP_branch_if && in->rop != ROP_branch_if_not) return 0;
  if (role == NULL || role->control != E_CF_BRANCH) return 0;
  if (!have_types || t == NULL || blk->nsucc < 2 || in->nuse < 1) return 0;
  a = in->use[0];
  if (a >= (Q2ValueId)t->nvalues) return 0;
  neg = in->rop == ROP_branch_if_not;
  fprintf(f, "    { int c_;\n");
  switch (t->lb[a]) {
    case E_LB_TAG:
      fprintf(
          f,
          "    if (__builtin_expect((uint32_t)JS_VALUE_GET_TAG(v%u) <= "
          "JS_TAG_UNDEFINED, 1))\n"
          "        c_ = JS_VALUE_GET_INT(v%u) != 0;\n"
          "    else if (JS_VALUE_GET_TAG(v%u) == JS_TAG_FLOAT64)\n"
          "        c_ = JS_VALUE_GET_FLOAT64(v%u) != 0 && "
          "JS_VALUE_GET_FLOAT64(v%u) == JS_VALUE_GET_FLOAT64(v%u);\n"
          "    else c_ = qjs_aot_to_bool(ctx, v%u) != 0;\n",
          (unsigned)a, (unsigned)a, (unsigned)a, (unsigned)a, (unsigned)a,
          (unsigned)a, (unsigned)a);
      break;
    case E_LB_I32:
      fprintf(f, "    c_ = i%u != 0;\n", (unsigned)a);
      break;
    case E_LB_F64:
      fprintf(f, "    c_ = d%u != 0 && d%u == d%u;\n", (unsigned)a, (unsigned)a,
              (unsigned)a);
      break;
    default:
      return 0;
  }
  fprintf(f, "    if (%sc_) {\n", neg ? "!" : "");
  e_emit_arm(f, fn, b, 0, cur, hex, blk->succ_block[0]);
  fprintf(f, "    } else {\n");
  e_emit_arm(f, fn, b, 1, cur, hex, blk->succ_block[1]);
  fprintf(f, "    }\n    }\n");
  return 1;
}

/* `branch_null` / `branch_not_null` (the tag is null) and `branch_nullish` /
   `branch_not_nullish` (null or undefined, or the HTMLDDA object, which the
   engine's `qjs_aot_nullish` decides and which runs no JS): a tag test with no
   miss path. A typed operand is a number or a bool, so the answer is constant. */
static int emit_tag_branch_null(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, int b,
    Q2ValueId *cur, int have_types, char hex[33]) {
  const ERole *role = e_roles[in->rop];
  const Q2Block *blk = &fn->blocks[b];
  Q2ValueId a;
  int nullish, want;

  switch (in->rop) {
    case ROP_branch_null: nullish = 0; want = 1; break;
    case ROP_branch_not_null: nullish = 0; want = 0; break;
    case ROP_branch_nullish: nullish = 1; want = 1; break;
    case ROP_branch_not_nullish: nullish = 1; want = 0; break;
    default: return 0;
  }
  if (role == NULL || role->control != E_CF_BRANCH) return 0;
  if (!have_types || t == NULL || blk->nsucc < 2 || in->nuse < 1) return 0;
  a = in->use[0];
  if (a >= (Q2ValueId)t->nvalues) return 0;
  fprintf(f, "    { int c_;\n");
  if (t->lb[a] != E_LB_TAG)
    fprintf(f, "    c_ = 0;\n");
  else if (nullish)
    fprintf(
        f,
        "    { uint32_t tg_ = (uint32_t)JS_VALUE_GET_TAG(v%u);\n"
        "      c_ = tg_ == JS_TAG_NULL || tg_ == JS_TAG_UNDEFINED ||\n"
        "           (tg_ == (uint32_t)JS_TAG_OBJECT && q2_nullish_obj(v%u)); }\n",
        (unsigned)a, (unsigned)a);
  else
    fprintf(f, "    c_ = JS_VALUE_GET_TAG(v%u) == JS_TAG_NULL;\n", (unsigned)a);
  fprintf(f, "    if (c_ == %d) {\n", want);
  e_emit_arm(f, fn, b, 0, cur, hex, blk->succ_block[0]);
  fprintf(f, "    } else {\n");
  e_emit_arm(f, fn, b, 1, cur, hex, blk->succ_block[1]);
  fprintf(f, "    }\n    }\n");
  return 1;
}

/* An UNCONDITIONAL branch (`branch`, `branch_s`): one successor, no condition, so
   it is arm 0 and nothing else. Loops whose back edge is a plain goto still handed
   off at this instruction, which is a large part of the remaining hand-off weight. */
static int emit_typed_goto(
    FILE *f, const Q2Func *fn, const Q2Insn *in, int b, Q2ValueId *cur,
    const char *hex) {
  const ERole *role = e_roles[in->rop];
  const Q2Block *blk = &fn->blocks[b];
  if (in->rop != ROP_branch && in->rop != ROP_branch_s) return 0;
  if (role == NULL || role->control != E_CF_BRANCH) return 0;
  if (blk->nsucc < 1) return 0;
  if (blk->succ_block[0] < 0 || blk->succ_block[0] >= fn->nblocks) return 0;
  fprintf(f, "    /* unconditional branch */\n");
  e_emit_arm(f, fn, b, 0, cur, hex, blk->succ_block[0]);
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

/* One comparison expression for the operand rep `f64`. A TAG side is read as its
   int payload (converted when the rep is f64) or as a double, depending on which
   tag arm the caller is in: `tag_arm` is 0 for INT, 1 for FLOAT64, 2 for "not TAG". */
static void e_cmp_expr(
    char *out, size_t cap, int base, const ETypes *t, Q2ValueId av, Q2ValueId bv,
    int f64rep, int tag_arm) {
  char xa[48], xb[48];
  int a_tag = t->lb[av] == E_LB_TAG;
  int b_tag = t->lb[bv] == E_LB_TAG;
  const char *cop;

  /* A bit-test reads both sides as i32, so an f64 goes through q2_toi32. */
  if (f64rep && (base == REG_BIN_AND || base == REG_BIN_OR || base == REG_BIN_XOR))
    f64rep = 0;

  if (a_tag) {
    /* In the FLOAT64 arm a TAG side reads its double payload; in the INT arm (and
       for a bit-test) it reads the int payload, converted when the rep is f64. */
    if (tag_arm == 1)
      snprintf(xa, sizeof(xa), "JS_VALUE_GET_FLOAT64(v%u)", (unsigned)av);
    else if (f64rep)
      snprintf(xa, sizeof(xa), "(double)JS_VALUE_GET_INT(v%u)", (unsigned)av);
    else
      snprintf(xa, sizeof(xa), "JS_VALUE_GET_INT(v%u)", (unsigned)av);
  } else if (t->lb[av] == E_LB_F64)
    snprintf(xa, sizeof(xa), f64rep ? "d%u" : "q2_toi32(d%u)", (unsigned)av);
  else
    snprintf(xa, sizeof(xa), f64rep ? "(double)i%u" : "i%u", (unsigned)av);

  if (b_tag) {
    if (tag_arm == 1)
      snprintf(xb, sizeof(xb), "JS_VALUE_GET_FLOAT64(v%u)", (unsigned)bv);
    else if (f64rep)
      snprintf(xb, sizeof(xb), "(double)JS_VALUE_GET_INT(v%u)", (unsigned)bv);
    else
      snprintf(xb, sizeof(xb), "JS_VALUE_GET_INT(v%u)", (unsigned)bv);
  } else if (t->lb[bv] == E_LB_F64)
    snprintf(xb, sizeof(xb), f64rep ? "d%u" : "q2_toi32(d%u)", (unsigned)bv);
  else
    snprintf(xb, sizeof(xb), f64rep ? "(double)i%u" : "i%u", (unsigned)bv);

  switch (base) {
    case REG_BIN_LT: cop = "<"; break;
    case REG_BIN_LTE: cop = "<="; break;
    case REG_BIN_GT: cop = ">"; break;
    case REG_BIN_GTE: cop = ">="; break;
    case REG_BIN_AND: cop = "&"; break;
    case REG_BIN_OR: cop = "|"; break;
    case REG_BIN_XOR: cop = "^"; break;
    default: cop = "=="; break;
  }
  if (base == REG_BIN_AND || base == REG_BIN_OR || base == REG_BIN_XOR)
    snprintf(out, cap, "((%s %s %s) != 0)", xa, cop, xb);
  else if (base == REG_BIN_NEQ || base == REG_BIN_STRICT_NEQ)
    snprintf(out, cap, "(%s != %s)", xa, xb);
  else
    snprintf(out, cap, "(%s %s %s)", xa, cop, xb);
}

static int emit_typed_branch(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, int b,
    Q2ValueId *cur, int have_types, char hex[33], int fi) {
  static const char *const ops[4] = {"<", "<=", ">", ">="};
  const ERole *role = e_roles[in->rop];
  const Q2Block *blk = &fn->blocks[b];
  int sub, inv, base, av, bv, si_take, si_else, sb_take, sb_else;
  char an[32], bn[32];
  (void)hex;
  (void)fi;

  if (in->rop != ROP_branch_cmp_reg_reg && in->rop != ROP_branch_cmp_reg_reg_s)
    return 0;
  if (role == NULL || role->control != E_CF_BRANCH) return 0;
  if (in->narg < 4 || in->arg[0].kind != 'b' || in->nuse < 2) return 0;
  if (blk->nsucc < 2 || !have_types || t == NULL) return 0;
  sub = (int)in->arg[0].val;
  inv = (sub & REG_CMP_INVERT) != 0;
  base = sub & ~REG_CMP_INVERT;
  /* The fused strict compare against undefined: the first operand's tag is the
     whole test (the handler's `take = (tag == UNDEFINED) == (sub == IS_UNDEF)`).
     A typed operand is a number or a bool, never undefined, so the branch is
     constant for it. */
  if (base == REG_BIN_IS_UNDEF || base == REG_BIN_IS_NOT_UNDEF) {
    Q2ValueId uv = in->use[0];
    char ex[96];
    if (blk->nsucc < 2 || !have_types || t == NULL) return 0;
    if (uv >= (Q2ValueId)t->nvalues) return 0;
    sb_take = blk->succ_block[0];
    if (sb_take < 0 || sb_take >= fn->nblocks) return 0;
    if (t->lb[uv] == E_LB_TAG)
      snprintf(
          ex, sizeof(ex), "(JS_VALUE_GET_TAG(v%u) == JS_TAG_UNDEFINED) == %d",
          (unsigned)uv, base == REG_BIN_IS_UNDEF);
    else
      snprintf(ex, sizeof(ex), "%d", base != REG_BIN_IS_UNDEF);
    fprintf(f, "    if (%s) {\n", ex);
    e_emit_arm(f, fn, b, 0, cur, hex, blk->succ_block[0]);
    fprintf(f, "    } else {\n");
    e_emit_arm(f, fn, b, 1, cur, hex, blk->succ_block[1]);
    fprintf(f, "    }\n");
    return 1;
  }
  /* Ordering compares, equalities, and the BIT-TEST kinds: `if (i & 1)` fuses
     the `&` and the truthiness test into the branch, so its sub is a bitwise
     kind. Declining those kept every `if (i & 1)` off the spec path. */
  {
    int is_order = base >= REG_BIN_LT && base <= REG_BIN_GTE;
    int is_eq = base == REG_BIN_EQ || base == REG_BIN_NEQ ||
                base == REG_BIN_STRICT_EQ || base == REG_BIN_STRICT_NEQ;
    int is_bit = base == REG_BIN_AND || base == REG_BIN_OR ||
                 base == REG_BIN_XOR;
    if (!is_order && !is_eq && !is_bit && base != REG_BIN_INSTANCEOF) return 0;
  }
  av = in->use[0];
  bv = in->use[1];
  if (av >= (Q2ValueId)t->nvalues || bv >= (Q2ValueId)t->nvalues) return 0;
  if (base == REG_BIN_INSTANCEOF && (t->lb[av] != E_LB_TAG || t->lb[bv] != E_LB_TAG))
    return 0;
  /* An equality with a boolean operand: two booleans compare as booleans, and a TAG value
     against one is strictly equal only when it is a boolean too. The loose forms leave a
     non-boolean TAG to the engine (it converts). */
  if ((base == REG_BIN_EQ || base == REG_BIN_NEQ || base == REG_BIN_STRICT_EQ ||
       base == REG_BIN_STRICT_NEQ) &&
      ((t->lb[av] == E_LB_BOOL && (t->lb[bv] == E_LB_BOOL || t->lb[bv] == E_LB_TAG)) ||
       (t->lb[bv] == E_LB_BOOL && t->lb[av] == E_LB_TAG)) &&
      blk->nsucc >= 2 && blk->succ_block[0] >= 0 && blk->succ_block[0] < fn->nblocks &&
      !g_e_fl) {
    int neg = base == REG_BIN_NEQ || base == REG_BIN_STRICT_NEQ;
    int strict = base == REG_BIN_STRICT_EQ || base == REG_BIN_STRICT_NEQ;
    int sb2 = blk->succ_block[0];
    (void)sb2;
    if (t->lb[av] == E_LB_BOOL && t->lb[bv] == E_LB_BOOL) {
      fprintf(f, "    { int c_ = (b%u == b%u)%s;\n", (unsigned)av, (unsigned)bv, neg ? " ^ 1" : "");
    } else {
      Q2ValueId tv = t->lb[av] == E_LB_TAG ? av : bv;
      Q2ValueId bo = t->lb[av] == E_LB_TAG ? bv : av;
      if (strict) {
        fprintf(
            f, "    { int c_ = (JS_VALUE_GET_TAG(v%u) == JS_TAG_BOOL && JS_VALUE_GET_BOOL(v%u) == b%u)%s;\n",
            (unsigned)tv, (unsigned)tv, (unsigned)bo, neg ? " ^ 1" : "");
      } else {
        fprintf(f, "    { int c_;\n");
        fprintf(
            f, "    if (__builtin_expect(JS_VALUE_GET_TAG(v%u) == JS_TAG_BOOL, 1)) c_ = (JS_VALUE_GET_BOOL(v%u) == b%u)%s;\n",
            (unsigned)tv, (unsigned)tv, (unsigned)bo, neg ? " ^ 1" : "");
        if (e_term_wrappable(fn, in, role, blk)) {
          fprintf(f, "    else {\n");
          e_emit_term_step(f, fn, in, t, blk, cur, hex, fi, inv);
          fprintf(f, "    }\n");
        } else {
          fprintf(f, "    else goto hx_%d;\n", in->pc);
          e_note_stub(g_e_hx, g_e_hx_cur, g_e_hx_blk, &g_e_hx_n, in->pc, cur, NULL);
        }
      }
    }
    fprintf(f, "    if (%sc_) {\n", inv ? "!" : "");
    e_emit_arm(f, fn, b, 0, cur, hex, blk->succ_block[0]);
    fprintf(f, "    } else {\n");
    e_emit_arm(f, fn, b, 1, cur, hex, blk->succ_block[1]);
    fprintf(f, "    }\n    }\n");
    return 1;
  }
  /* TAG is admitted: the dispatch below handles it. */
  if ((t->lb[av] != E_LB_I32 && t->lb[av] != E_LB_F64 &&
       t->lb[av] != E_LB_TAG) ||
      (t->lb[bv] != E_LB_I32 && t->lb[bv] != E_LB_F64 &&
       t->lb[bv] != E_LB_TAG))
    return 0;
  /* succ[0] is where the branch GOES when taken; succ[1] is the fall-through. */
  si_take = 0;
  si_else = 1;
  sb_take = blk->succ_block[si_take];
  sb_else = blk->succ_block[si_else];
  if (sb_take < 0 || sb_take >= fn->nblocks) return 0;

  /* THE COMPARISON IS COMPUTED ONCE into an int, then the two arms are emitted
     ONCE through e_emit_arm -- so edge moves, back-edge polls and pre-safepoint syncs are
     exactly as before and only the way `c_` is computed changes.

     A TAG operand is dispatched on its tag: INT compares as i32 (or
     as a double when the other side is f64), FLOAT64 compares as a double, and
     anything else jumps to the function's COLD ARM `hx_<pc>`, a hand-off at the
     branch's own pc. It is a hand-off, not a deopt: a string compare is not a
     failed speculation and must not count toward the switch-off. */
  {
    int tag_side = -1, ntags = 0;
    char ex[128];
    if (t->lb[av] == E_LB_TAG) { tag_side = 0; ntags++; }
    if (t->lb[bv] == E_LB_TAG) { tag_side = 1; ntags++; }
    /* A frameless body leaves at the cold arm. An operand the profile says is not
       a number (strings, objects) reaches it on most executions, which would be
       an exit per call: such a compare keeps the function framed. */
    if (g_e_fl && ((t->lb[av] == E_LB_TAG && !e_operand_numeric(fn, av)) ||
                    (t->lb[bv] == E_LB_TAG && !e_operand_numeric(fn, bv)))) {
      e_fl_block("a compare of non-numbers", in);
      return 0;
    }
    /* A bit-test on a TAG operand would need a toi32 of a JSValue payload; decline
       instead of emitting `&` on doubles. */
    if (ntags > 0 && (base == REG_BIN_AND || base == REG_BIN_OR ||
                      base == REG_BIN_XOR))
      return 0;
    {
      int want_f64 = (t->lb[av] == E_LB_F64 || t->lb[bv] == E_LB_F64);
      if (ntags == 0) {
        e_cmp_expr(ex, sizeof(ex), base, t, av, bv, want_f64, 2);
        fprintf(f, "    if (%s%s) {\n", inv ? "!" : "", ex);
      } else if (ntags == 2 && base == REG_BIN_INSTANCEOF) {
        /* A plain constructor on the right is answered by the engine's lean routine; any
           other right side is the operator itself. */
        fprintf(f, "    { int c_ = qjs_aot_instanceof(ctx, v%u, v%u);\n", (unsigned)av, (unsigned)bv);
        fprintf(f, "    if (__builtin_expect(c_ < 0, 0)) {\n");
        if (e_term_wrappable(fn, in, role, blk)) {
          e_emit_term_step(f, fn, in, t, blk, cur, hex, fi, inv);
          fprintf(f, "    }\n");
        } else {
          fprintf(f, "    goto hx_%d;\n    }\n", in->pc);
          e_note_stub(
              g_e_hx, g_e_hx_cur, g_e_hx_blk, &g_e_hx_n, in->pc, cur, NULL);
        }
        fprintf(f, "    if (%sc_) {\n", inv ? "!" : "");
      } else if (ntags == 2) {
        /* Both operands are TAG: each tag is tested. Two ints compare as ints,
           any two numbers as doubles; everything else (strings, objects) is the
           engine's own handler. */
        const char *cop = base == REG_BIN_LT    ? "<"
                          : base == REG_BIN_LTE ? "<="
                          : base == REG_BIN_GT  ? ">"
                          : base == REG_BIN_GTE ? ">="
                          : (base == REG_BIN_NEQ || base == REG_BIN_STRICT_NEQ)
                              ? "!="
                              : "==";
        fprintf(f, "    { int c_;\n");
        fprintf(
            f,
            "    if (__builtin_expect(JS_VALUE_IS_BOTH_INT(v%u, v%u), 1))\n"
            "        c_ = (JS_VALUE_GET_INT(v%u) %s JS_VALUE_GET_INT(v%u));\n"
            "    else if ((JS_VALUE_GET_TAG(v%u) == JS_TAG_INT || "
            "JS_VALUE_GET_TAG(v%u) == JS_TAG_FLOAT64) &&\n"
            "             (JS_VALUE_GET_TAG(v%u) == JS_TAG_INT || "
            "JS_VALUE_GET_TAG(v%u) == JS_TAG_FLOAT64))\n"
            "        c_ = ((JS_VALUE_GET_TAG(v%u) == JS_TAG_INT ? "
            "(double)JS_VALUE_GET_INT(v%u) : JS_VALUE_GET_FLOAT64(v%u)) %s\n"
            "              (JS_VALUE_GET_TAG(v%u) == JS_TAG_INT ? "
            "(double)JS_VALUE_GET_INT(v%u) : JS_VALUE_GET_FLOAT64(v%u)));\n",
            (unsigned)av, (unsigned)bv, (unsigned)av, cop, (unsigned)bv,
            (unsigned)av, (unsigned)av, (unsigned)bv, (unsigned)bv, (unsigned)av,
            (unsigned)av, (unsigned)av, cop, (unsigned)bv, (unsigned)bv,
            (unsigned)bv);
        if (base == REG_BIN_EQ || base == REG_BIN_NEQ || base == REG_BIN_STRICT_EQ ||
            base == REG_BIN_STRICT_NEQ)
          fprintf(
              f,
              "    else if (q2_str_eq(v%u, v%u, &c_))\n"
              "        c_ = %sc_;\n",
              (unsigned)av, (unsigned)bv,
              (base == REG_BIN_NEQ || base == REG_BIN_STRICT_NEQ) ? "!" : "");
        else if (base >= REG_BIN_LT && base <= REG_BIN_GTE)
          fprintf(
              f,
              "    else if (q2_str_cmp(v%u, v%u, &c_))\n"
              "        c_ = c_ %s 0;\n",
              (unsigned)av, (unsigned)bv, cop);
        {
          int mk = base == REG_BIN_LT ? 0 : base == REG_BIN_LTE ? 1 : base == REG_BIN_GT ? 2
                 : base == REG_BIN_GTE ? 3 : base == REG_BIN_EQ ? 4 : base == REG_BIN_NEQ ? 5
                 : base == REG_BIN_STRICT_EQ ? 6 : 7;
          fprintf(
              f,
              "    else if (q2_mixed_cmp(v%u, v%u, %d, &c_)) { }\n"
              "    else if (q2_ident_cmp(v%u, v%u, %d, &c_)) { }\n",
              (unsigned)av, (unsigned)bv, mk, (unsigned)av, (unsigned)bv, mk);
        }
        if (e_term_wrappable(fn, in, role, blk)) {
          fprintf(f, "    else {\n");
          e_emit_term_step(f, fn, in, t, blk, cur, hex, fi, inv);
          fprintf(f, "    }\n");
        } else {
          fprintf(f, "    else goto hx_%d;\n", in->pc);
          e_note_stub(
              g_e_hx, g_e_hx_cur, g_e_hx_blk, &g_e_hx_n, in->pc, cur, NULL);
        }
        fprintf(f, "    if (%sc_) {\n", inv ? "!" : "");
      } else {
        Q2ValueId tv = tag_side == 0 ? av : bv;
        int treg = fn->values[tv].reg;
        if (treg < 0 || treg >= fn->nregs) return 0;
        e_cmp_expr(ex, sizeof(ex), base, t, av, bv, want_f64, 0);
        fprintf(f, "    { int c_;\n");
        fprintf(
            f,
            "    if (__builtin_expect(JS_VALUE_GET_TAG(v%u) == JS_TAG_INT, 1))\n"
            "        c_ = %s;\n",
            (unsigned)tv, ex);
        if (want_f64) {
          /* The other side is a double, so INT converts -- and FLOAT64 is the
             only other numeric tag. */
          e_cmp_expr(ex, sizeof(ex), base, t, av, bv, 1, 1);
          fprintf(
              f,
              "    else if (JS_VALUE_GET_TAG(v%u) == JS_TAG_FLOAT64)\n"
              "        c_ = %s;\n",
              (unsigned)tv, ex);
        } else {
          e_cmp_expr(ex, sizeof(ex), base, t, av, bv, 1, 1);
          fprintf(
              f,
              "    else if (JS_VALUE_GET_TAG(v%u) == JS_TAG_FLOAT64)\n"
              "        c_ = %s;\n",
              (unsigned)tv, ex);
        }
        /* Neither number: a string compare, an object with valueOf. The engine's
           own handler decides which way the branch goes (it may run JS, so the
           live values are in the frame first) and the arms below take it. */
        if (e_term_wrappable(fn, in, role, blk)) {
          fprintf(f, "    else {\n");
          e_emit_term_step(f, fn, in, t, blk, cur, hex, fi, inv);
          fprintf(f, "    }\n");
        } else {
          fprintf(f, "    else goto hx_%d;\n", in->pc);
          e_note_stub(
              g_e_hx, g_e_hx_cur, g_e_hx_blk, &g_e_hx_n, in->pc, cur, NULL);
        }
        fprintf(f, "    if (%sc_) {\n", inv ? "!" : "");
      }
    }
    e_emit_arm(f, fn, b, si_take, cur, hex, blk->succ_block[si_take]);
    fprintf(f, "    } else {\n");
    e_emit_arm(f, fn, b, si_else, cur, hex, blk->succ_block[si_else]);
    fprintf(f, "    }\n");
    if (t->lb[av] == E_LB_TAG || t->lb[bv] == E_LB_TAG) fprintf(f, "    }\n");
    (void)an; (void)bn; (void)ops;
  }
  (void)sb_else;
  return 1;
}

/* The unary operations the spec body lowers.

   `inc`/`dec` on an i32 local: the machine's `+ 1` / `- 1`, with the one value
   that overflows (`INT32_MAX` / `INT32_MIN`) taking the deopt exit at this
   instruction, so the generic body redoes it as a double. `neg` likewise
   (`INT32_MIN` overflows; `-0` is a double and comes out of the f64 form).
   `not` on a bool is a `!`, `bitnot` on an i32 is a `~`, neither can fail.
   Returns 1 when it emitted the instruction. */
static int emit_typed_unary(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur,
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
  if (!is_inc && !is_dec && !is_neg && !is_not && in->rop != ROP_to_numeric &&
      in->rop != ROP_add_imm_self_s && in->rop != ROP_sub_imm_self_s)
    return 0;
  if (in->ndef < 1 || in->nuse < 1) return 0;
  d = in->def[0];
  a = in->use[0];
  if (d >= (Q2ValueId)t->nvalues || a >= (Q2ValueId)t->nvalues) return 0;
  if (in->rop == ROP_to_numeric) {
    /* a number is its own ToNumeric */
    char dn[32], an[32];
    int ld = fn->values[d].reg;
    if (t->lb[a] != t->lb[d] || (t->lb[a] != E_LB_I32 && t->lb[a] != E_LB_F64)) return 0;
    e_local_name(dn, sizeof(dn), d);
    e_local_name(an, sizeof(an), a);
    fprintf(f, "    %s = %s;\n", dn, an);
    if (cur && ld >= 0 && ld < fn->nregs) cur[ld] = d;
    return 1;
  }
  {
    char dn[32], an[32];
    int ld = fn->values[d].reg;
    /* not: a bool in, a bool out. */
    if (is_not) {
      if (t->lb[a] != E_LB_BOOL) return 0;
      e_local_name(dn, sizeof(dn), d);
      e_local_name(an, sizeof(an), a);
      fprintf(f, "    %s = !%s;\n", dn, an);
      if (cur && ld >= 0 && ld < fn->nregs) cur[ld] = d;
      return 1;
    }
    if (is_bitnot) {
      if (t->lb[a] != E_LB_I32) return 0;
      e_local_name(dn, sizeof(dn), d);
      e_local_name(an, sizeof(an), a);
      fprintf(f, "    %s = ~%s;\n", dn, an);
      if (cur && ld >= 0 && ld < fn->nregs) cur[ld] = d;
      return 1;
    }
    if (in->rop == ROP_add_imm_self_s || in->rop == ROP_sub_imm_self_s) {
      /* "ri": arg[0] is the register (def AND use), arg[1] a SIGNED 8-bit immediate
         (RD_OFF8). `i = i +/- k` with the int32 overflow deopt -- the inc path with
         a constant instead of 1. This is add_imm_self_branch without the jump. */
      int32_t k = (int32_t)(int8_t)(in->narg > 1 ? (in->arg[1].val & 0xff) : 1);
      if (t->lb[d] == E_LB_F64 && (t->lb[a] == E_LB_F64 || t->lb[a] == E_LB_I32)) {
        /* A double result needs no exit: the sum of a number and a small constant. */
        e_local_name(dn, sizeof(dn), d);
        e_local_name(an, sizeof(an), a);
        fprintf(
            f, "    %s = %s%s + (double)%d;\n", dn, t->lb[a] == E_LB_I32 ? "(double)" : "", an,
            in->rop == ROP_add_imm_self_s ? k : -k);
        if (cur && ld >= 0 && ld < fn->nregs) cur[ld] = d;
        return 1;
      }
      if (t->lb[d] != E_LB_I32) return 0;
      e_local_name(dn, sizeof(dn), d);
      e_local_name(an, sizeof(an), a);
      e_sweep_guard(f, "ox", in->pc);
      fprintf(
          f,
          "    { int32_t _s;\n"
          "      if (__builtin_expect(__builtin_add_overflow(%s, (int32_t)%d, &_s), 0))\n"
          "        goto ox_%d;\n"
          "      %s = _s; }\n",
          an, in->rop == ROP_add_imm_self_s ? k : -k, in->pc, dn);
      e_note_stub(
          g_e_ox, g_e_ox_cur, g_e_ox_blk, &g_e_ox_n, in->pc, cur, NULL);
      if (cur && ld >= 0 && ld < fn->nregs) cur[ld] = d;
      return 1;
    }
    if (is_inc || is_dec) {
      /* The DEF's rep must be i32 too, not just the operand's: writing an i32
         result into a TAG or f64 local is a miscompile. */
      if (t->lb[a] != E_LB_I32 || t->lb[d] != E_LB_I32) return 0;
      e_local_name(dn, sizeof(dn), d);
      e_local_name(an, sizeof(an), a);
      e_sweep_guard(f, "ox", in->pc);
      fprintf(
          f,
          "    if (__builtin_expect(%s != %s, 1)) { %s = %s %s 1; }\n", an,
          is_inc ? "INT32_MAX" : "INT32_MIN", dn, an, is_inc ? "+" : "-");
      fprintf(f, "    else { goto ox_%d; }\n", in->pc);
      e_note_stub(
          g_e_ox, g_e_ox_cur, g_e_ox_blk, &g_e_ox_n, in->pc, cur, NULL);
      if (cur && ld >= 0 && ld < fn->nregs) cur[ld] = d;
      return 1;
    }
    /* neg. The DEF's rep decides, not the operand's: `-0` of integer 0 is NOT an
       int32 -- JS gives -0 and an int32 holds +0, so an i32 form would make
       `1 / -0` +Infinity instead of -Infinity. The analysis already records
       NEGZERO in neg's exit set; this emitter used to ignore it. So an I32 def
       exits on 0 (NEGZERO) as well as INT32_MIN (SIGNEDMIN), and an F64 def simply
       negates, which produces -0.0 for 0 with no exit at all. */
    if (t->lb[a] == E_LB_I32 && t->lb[d] == E_LB_I32) {
      e_local_name(dn, sizeof(dn), d);
      e_local_name(an, sizeof(an), a);
      e_sweep_guard(f, "ox", in->pc);
      fprintf(
          f,
          "    if (__builtin_expect(%s != INT32_MIN && %s != 0, 1)) "
          "{ %s = -%s; }\n",
          an, an, dn, an);
      fprintf(f, "    else { goto ox_%d; }\n", in->pc);
      e_note_stub(
          g_e_ox, g_e_ox_cur, g_e_ox_blk, &g_e_ox_n, in->pc, cur, NULL);
      if (cur && ld >= 0 && ld < fn->nregs) cur[ld] = d;
      return 1;
    }
    if (t->lb[a] == E_LB_F64 && t->lb[d] == E_LB_F64) {
      e_local_name(dn, sizeof(dn), d);
      e_local_name(an, sizeof(an), a);
      fprintf(f, "    %s = -%s;\n", dn, an);
      if (cur && ld >= 0 && ld < fn->nregs) cur[ld] = d;
      return 1;
    }
  }
  (void)hex;
  return 0;
}

/* `post_inc` / `post_dec` ("rrr": the ToNumeric value, the source, the source plus
   or minus one) on a typed source: the first result is the source, the second is
   the source stepped. An i32 source leaves at INT32_MAX / INT32_MIN (the result is
   no int); an f64 source cannot overflow. */
static int emit_typed_post(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t,
    Q2ValueId *cur, int have_types) {
  Q2ValueId d0, d1, a;
  int r0, r1, inc;

  if (!have_types || t == NULL) return 0;
  if (in->rop != ROP_post_inc && in->rop != ROP_post_dec) return 0;
  if (in->ndef != 2 || in->nuse != 1) return 0;
  d0 = in->def[0];
  d1 = in->def[1];
  a = in->use[0];
  if (d0 >= (Q2ValueId)t->nvalues || d1 >= (Q2ValueId)t->nvalues ||
      a >= (Q2ValueId)t->nvalues)
    return 0;
  r0 = fn->values[d0].reg;
  r1 = fn->values[d1].reg;
  if (r0 < 0 || r0 >= fn->nregs || r1 < 0 || r1 >= fn->nregs) return 0;
  inc = in->rop == ROP_post_inc;
  if (t->lb[a] == E_LB_I32 && t->lb[d0] == E_LB_I32 &&
      t->lb[d1] == E_LB_I32) {
    e_sweep_guard(f, "ox", in->pc);
    fprintf(
        f,
        "    if (__builtin_expect(i%u == %s, 0)) goto ox_%d;\n"
        "    i%u = i%u %s 1;\n"
        "    i%u = i%u;\n",
        (unsigned)a, inc ? "INT32_MAX" : "INT32_MIN", in->pc, (unsigned)d1,
        (unsigned)a, inc ? "+" : "-", (unsigned)d0, (unsigned)a);
    e_note_stub(
        g_e_ox, g_e_ox_cur, g_e_ox_blk, &g_e_ox_n, in->pc, cur, NULL);
  } else if (t->lb[a] == E_LB_F64 && t->lb[d0] == E_LB_F64 &&
             t->lb[d1] == E_LB_F64) {
    fprintf(
        f, "    d%u = d%u %s 1;\n    d%u = d%u;\n", (unsigned)d1, (unsigned)a,
        inc ? "+" : "-", (unsigned)d0, (unsigned)a);
  } else {
    return 0;
  }
  if (cur != NULL) {
    cur[r0] = d0;
    cur[r1] = d1;
  }
  return 1;
}

/* `return` in the spec body: box the returned value from its local into its frame
   slot, then use the generic body's own return path (`qjs_aot_return` reads a
   frame slot). Freeing nothing else is correct: frame registers are released by
   the caller exactly as for the generic body. `return_undefined` needs no slot.

   This is what the hand/ent column has been showing as 8/8 -- every call handed
   off at its return, so no function ever completed on the spec path. */
static int emit_typed_return(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur,
    int have_types) {
  const ERole *role = e_roles[in->rop];
  (void)fn;
  (void)t;
  (void)have_types;
  e_virt_compute(fn);
  g_e_stub_cur = cur;
  if (in->rop == ROP_return_undefined) {
    if (role == NULL || role->control != E_CF_RETURN) return 0;
    if (g_e_inl)
      fprintf(f, "    { *out = JS_UNDEFINED; return 1; }\n");
    else
      fprintf(f, "    return JS_UNDEFINED;\n");
    return 1;
  }
  if (in->rop != ROP_return) return 0;
  if (role == NULL || role->control != E_CF_RETURN) return 0;
  if (in->nuse < 1) return 0;
  {
    Q2ValueId v = in->use[0];
    int r;
    if (v >= (Q2ValueId)g_e_fn->nvalues) return 0;
    r = g_e_fn->values[v].reg;
    if (r < 0 || r >= g_e_fn->nregs) return 0;
    /* Box into the frame slot the generic return reads, by representation. */
    e_exit_store(f, v, r);
    e_emit_return(f, r * 16);
  }
  return 1;
}

/* The speculative body.

   The switch-off counters, the entry (which refuses anything but a fresh entry at
   pc 0, and refuses once speculation has been switched off for this function), the
   locals, and the blocks in reverse postorder with their edges -- a parameter move
   per successor parameter, through a temporary so parameters may swap, exactly as
   `q2_emit_edge` does it in the generic body. Each block's body is the typed
   lowerings; an instruction none of them takes hands off to the generic body at its
   own pc, which is the correct body, so a mismatch between the two bodies cannot
   hide behind a missing lowering. */
/* How many functions were refused for the speculative body because they are
   irreducible (a back edge to a block that does not dominate its source). The
   report prints it; it should stay 0 for structured bytecode. */
int e_irreducible = 0;

/* `load_const` of an atom followed by the `get_element` that reads with it and nothing
   else: `super.m`, `obj['name']`. The string is never built and the property is found by
   the atom, with the object as the receiver, as the element read does. The key's register
   must be dead after the read. Returns 1 when both instructions were emitted. */
static uint8_t *g_e_pre_done;
/* Whether the typed-array shape check survives this instruction: nothing it runs can execute
   JS or reshape an object. The role table says what the interpreter's handler may do; a
   numeric instruction whose operands and results are all typed runs only machine code. */
static int e_ta_keeps(const Q2Func *fn, const Q2Insn *in, const ETypes *t) {
  const ERole *role = e_roles[in->rop];
  int u, d;

  if (role == NULL) return 0;
  if (!role->runs_js && !role->allocates) return 1;
  if (t == NULL) return 0;
  if (in->op != Q2_OP_BINARY && in->op != Q2_OP_COMPARE && in->op != Q2_OP_UNARY &&
      in->op != Q2_OP_BRANCH && in->op != Q2_OP_BRANCH_CMP && in->op != Q2_OP_POST_INC)
    return 0;
  for (u = 0; u < in->nuse; u++)
    if (in->use[u] >= (Q2ValueId)t->nvalues || t->lb[in->use[u]] == E_LB_TAG ||
        t->lb[in->use[u]] == E_LB_BOT)
      return 0;
  for (d = 0; d < in->ndef; d++)
    if (in->def[d] >= (Q2ValueId)t->nvalues || t->lb[in->def[d]] == E_LB_TAG ||
        t->lb[in->def[d]] == E_LB_BOT)
      return 0;
  (void)fn;
  return 1;
}

static int e_try_fuse_atom_get(
    FILE *f, const Q2Func *fn, int i2, int b, int blk_end, const ETypes *t, Q2ValueId *cur,
    int have_types, char hex[33], int fi) {
  const Q2Insn *l = &fn->insns[i2], *e;
  int ok, ei, same;

  if (!have_types || t == NULL || g_e_fl || cur == NULL || g_e_pre_done == NULL) return 0;
  if (l->rop != ROP_load_const || l->narg < 3 || l->arg[1].val == 0 || l->arg[2].off < 0 ||
      l->ndef < 1)
    return 0;
  if (i2 + 1 < blk_end) {
    ei = i2 + 1;
    same = 1;
  } else {
    /* The block ends after the load and falls through to a block of its own that starts
       with the read (the IR cuts blocks after instructions it does not lower). */
    const Q2Block *blk = &fn->blocks[b], *nb;

    if (blk->nsucc != 1 || blk->succ_block[0] < 0 || blk->succ_block[0] >= fn->nblocks ||
        blk->nedge_args[0] != 0)
      return 0;
    nb = &fn->blocks[blk->succ_block[0]];
    if (nb->npreds != 1 || nb->nparams != 0 || nb->ninsns < 1) return 0;
    ei = nb->first_insn;
    same = 0;
  }
  e = &fn->insns[ei];
  if (e->rop != ROP_get_element || e->nuse < 2 || e->use[1] != l->def[0] || e->ndef < 1)
    return 0;
  if (e_def_dead(e, l->def[0]) == 0) return 0;
  if (g_e_facts != NULL && (g_e_facts->fwd[i2] != Q2_NO_VALUE || g_e_facts->fwd[ei] != Q2_NO_VALUE))
    return 0;
  if (e_must_exit(fn, e)) return 0;
  g_e_fuse_atom_off = l->arg[2].off;
  ok = emit_alloc_op(f, fn, e, t, cur, have_types, hex, fi, b);
  g_e_fuse_atom_off = -1;
  if (!ok) return 0;
  if (!same) g_e_pre_done[ei] = 1;
  return same ? 1 : 2;
}

/* ============================ the rop -> emitter table ====================
   ONE emitter per rop. A long `else if` chain's ORDER was the bug several times
   (add_imm_self_s, the wrapper's tier gate, the fused ops, load_const): with a chain, an op silently stops lowering when an earlier,
   wider test claims it, and a restructure can resurrect one of these without any
   test noticing. Here an op appears exactly once and the compiler checks the cases.

   The emitters keep their own rep checks (they are what decides `decline`); only
   the "is this my rop" question is answered by the table. Anything the table does
   not name falls to the op-call wrapper, which is still the catch-all.

   Returns 1 when the instruction was emitted, 0 to hand off at this pc. */

/* Emitters whose rop test is now redundant with the table still keep it: the same
   function is called from more than one place (the generic body, tests). */
static int emit_inl_guard(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur, int have_types);
static int e_dispatch_rop(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const ETypes *t, Q2ValueId *cur,
    int have_types, char hex[33], int fi, int b, const ERole *role) {
  if (in->inlg) return emit_inl_guard(f, fn, in, t, cur, have_types);
  if (in->vk == Q2_VK_DEF) return emit_virt_def(f, fn, in, t, cur, have_types);
  if (in->vk == Q2_VK_PROTO) return emit_virt_proto(f, fn, in, t, cur, have_types);
  switch (in->rop) {
    /* -- the spec body's own trivial forms, separate because they are shared with
          the generic emitter and must not be added there -- */
    case ROP_load_const:
      return emit_typed_load_const(f, fn, in, t, cur, have_types) ||
             emit_alloc_op(f, fn, in, t, cur, have_types, hex, fi, b);
    case ROP_unary:
      return emit_tag_numeric(f, fn, in, t, cur, have_types, hex, fi, b) ||
             emit_alloc_op(f, fn, in, t, cur, have_types, hex, fi, b);
    case ROP_put_global:
    case ROP_put_local_s0:
    case ROP_put_local_s1:
    case ROP_put_local_s2:
    case ROP_put_local_s3:
    case ROP_put_local_xfer_s0:
    case ROP_put_local_xfer_s1:
    case ROP_put_local_xfer_s2:
    case ROP_put_local_xfer_s3:
    case ROP_put_local:
    case ROP_put_local_xfer:
    case ROP_local_uninit:
      return emit_store_call(f, fn, in, t, cur, have_types);

    /* -- variable slots in frame memory -- */
    case ROP_get_local:
    case ROP_get_local_s0:
    case ROP_get_local_s1:
    case ROP_get_local_s2:
    case ROP_get_local_s3:
      return emit_typed_local_read(f, fn, in, t, cur, have_types);
    case ROP_binary_loc_reg:
    case ROP_binary_reg_loc:
    case ROP_binary_loc_loc:
      return emit_typed_binary_loc(f, fn, in, t, cur, have_types, hex, fi, b);

    /* -- fused arithmetic (Q2_OP_BINARY, but a five-operand shape) -- */
    case ROP_add_mul:
    case ROP_mul_add:
    case ROP_mul_sub:
    case ROP_sub_mul:
      return emit_typed_fused(f, fn, in, t, cur, have_types) ||
             emit_tag_fused(f, fn, in, t, cur, have_types, hex, fi, b);

    /* -- arithmetic -- */
    case ROP_div:
    case ROP_mod:
      return emit_typed_divmod(f, fn, in, t, cur, have_types, hex, fi, b, NULL) ||
             emit_alloc_op(f, fn, in, t, cur, have_types, hex, fi, b);
    case ROP_add:
    case ROP_sub:
    case ROP_mul:
      return (have_types && emit_typed_arith(f, fn, in, t, cur)) ||
             emit_alloc_op(f, fn, in, t, cur, have_types, hex, fi, b);
    case ROP_binary_cpool:
      return emit_typed_arith_cpool(f, fn, in, t, cur, have_types) ||
             emit_tag_numeric(f, fn, in, t, cur, have_types, hex, fi, b) ||
             emit_alloc_op(f, fn, in, t, cur, have_types, hex, fi, b);
    case ROP_binary_imm:
    case ROP_binary_imm_s:
    case ROP_add_imm_s:
    case ROP_sub_imm_s:
      return emit_typed_arith_imm(f, fn, in, t, cur, have_types);
    case ROP_add_imm_self_s:
    case ROP_sub_imm_self_s:
      /* Both emit_typed_arith_imm and emit_typed_unary accept these; the arith
         form is the one that was reached first in the old chain, so keep it. */
      return emit_typed_arith_imm(f, fn, in, t, cur, have_types) ||
             emit_typed_unary(f, fn, in, t, cur, have_types, hex, fi);
    case ROP_inc:
    case ROP_dec:
    case ROP_neg:
    case ROP_not:
    case ROP_to_numeric:
      return emit_typed_unary(f, fn, in, t, cur, have_types, hex, fi);
    case ROP_post_inc:
    case ROP_post_dec:
      return emit_typed_post(f, fn, in, t, cur, have_types);
    case ROP_bitand:
    case ROP_bitor:
    case ROP_bitxor:
    case ROP_shl:
    case ROP_shr:
    case ROP_sar:
      return emit_typed_bitwise(f, fn, in, t, cur, have_types);

    /* -- fields -- */
    case ROP_get_field:
    case ROP_get_field2:
    case ROP_get_field_own:
    case ROP_get_field2_own:
      return emit_typed_field_read(f, fn, in, t, cur, have_types, hex, fi, b);
    case ROP_put_field:
      return emit_typed_field_write(f, fn, in, t, cur, have_types, hex, fi, b);
    case ROP_get_field_chain:
    case ROP_get_field_chain_own:
      return emit_typed_field_chain(f, fn, in, t, cur, have_types, hex, fi, b);
    case ROP_get_this_field:
    case ROP_get_this_field2:
      return emit_typed_this_field(f, fn, in, t, cur, have_types, hex, fi, b);

    case ROP_to_propkey:
    case ROP_to_propkey2:
      return emit_typed_propkey(f, fn, in, t, cur, have_types);

    /* -- elements and length -- */
    case ROP_get_element:
      return emit_typed_elem_read(f, fn, in, t, cur, have_types, hex, fi, b);
    case ROP_put_element:
    case ROP_put_element_xfer:
      return emit_typed_elem_write(f, fn, in, t, cur, have_types, hex, fi, b);
    case ROP_get_length:
      return emit_typed_length(f, fn, in, t, cur, have_types, hex, fi, b);

    /* -- calls -- */
    case ROP_call:
    case ROP_call_method:
      return emit_typed_call(f, fn, in, t, cur, have_types, hex, fi, b);
    case ROP_construct:
      if (g_e_inl) return emit_inl_construct(f, fn, in, t, cur, hex);
      return emit_typed_construct(f, fn, in, t, cur, have_types, hex, fi, b);
    case ROP_get_local_field_0:
    case ROP_get_local_field_1:
    case ROP_get_local_field_2:
    case ROP_get_local_field_3:
    case ROP_get_local_field:
      return emit_typed_local_field(f, fn, in, t, cur, have_types, hex, fi, b);

    /* -- allocation without the step handler -- */
    case ROP_new_object:
    case ROP_object_template:
    case ROP_array_from:
    case ROP_array_const:
    case ROP_object_fill:
    case ROP_special_object:
    case ROP_apply_arguments:
    case ROP_get_super:
      return emit_alloc_op(f, fn, in, t, cur, have_types, hex, fi, b);

    /* -- this, and strict equality as a value -- */
    case ROP_load_this:
      return emit_typed_load_this(f, fn, in, t, cur, have_types);
    case ROP_strict_eq:
    case ROP_strict_neq:
      return emit_typed_strict_eq(f, fn, in, t, cur, have_types, hex, fi, b) ||
             emit_tag_cmp_value(f, fn, in, t, cur, have_types, hex, fi, b);
    case ROP_lt:
    case ROP_lte:
    case ROP_gt:
    case ROP_gte:
    case ROP_eq:
    case ROP_neq:
      return emit_tag_cmp_value(f, fn, in, t, cur, have_types, hex, fi, b);

    /* -- globals and closure cells -- */
    case ROP_get_global:
      return emit_typed_global_read(f, fn, in, t, cur, have_types, hex, fi, b);
    case ROP_get_var_ref:
      return emit_typed_var_ref_load(f, fn, in, t, cur, have_types);
    case ROP_put_var_ref:
      return emit_typed_var_ref_store(f, fn, in, t, cur, have_types);

    default:
      break;
  }
  (void)role;
  /* NO CATCH-ALL YET. This is a REFACTOR: the histogram must be identical before
     and after, and adding the general op-call fallback here would lower ops
     (object_fill, object_template, make_closure, ...) that the chain handed off --
     which is a decision to make explicitly, not a side effect of making the
     dispatch table-driven. */
  return 0;
}

static int e_emit_spec_body_pass_inner(FILE *f, const Q2Func *fn, char hex[33]);
/* A framed speculative body assumes no lazy JSON document exists (it is re-checked at entry
   and after every safepoint), so its loads carry no lazy-marker test; the cache-hit helpers
   take the choice from Q2_LZ. Frameless and inline bodies keep the tests. */
static int e_emit_spec_body_pass(FILE *f, const Q2Func *fn, char hex[33]) {
  int rc;
  g_e_nolazy = !g_e_inl;
  g_e_epoch_skip = 0;
  if (g_e_nolazy) fprintf(f, "#undef Q2_LZ\n#define Q2_LZ 0\n");
  rc = e_emit_spec_body_pass_inner(f, fn, hex);
  if (g_e_nolazy) fprintf(f, "#undef Q2_LZ\n#define Q2_LZ 1\n");
  g_e_nolazy = 0;
  g_e_epoch_skip = 0;
  return rc;
}
static int e_emit_spec_body_pass_inner(FILE *f, const Q2Func *fn, char hex[33]) {
  int fi = qjs_aot_cur_func_idx();
  g_e_fn = fn;
  g_e_virt_fn = NULL;
  g_e_stub_cur = NULL;
  g_e_fi = fi;
  g_e_opcalls = 0;
  g_e_n_typed = g_e_n_opcall = 0;
  memset(g_e_n_guard, 0, sizeof(g_e_n_guard));
  g_e_n_exit = g_e_n_handoff = 0;
  g_e_xtag_n = 0;
  g_e_facts = NULL;
  g_e_inl_dirty = 0;
  g_e_dx_own_n = 0;
  free(g_e_pre_done);
  g_e_pre_done = calloc((size_t)(fn->ninsns > 0 ? fn->ninsns : 1), 1);
  free(g_e_spill_slot);
  g_e_spill_nv = fn->nvalues > 0 ? fn->nvalues : 1;
  g_e_spill_slot = malloc(sizeof(int) * (size_t)g_e_spill_nv);
  if (g_e_spill_slot == NULL) abort();
  {
    int q_;
    for (q_ = 0; q_ < g_e_spill_nv; q_++) g_e_spill_slot[q_] = -1;
  }
  free(g_e_pp);
  g_e_pp = calloc((size_t)g_e_spill_nv, 1);
  if (g_e_pp == NULL) abort();
  g_e_spill_n = 0;
  g_e_inl_nx_used = 0;
  free(g_e_dirty_ins);
  g_e_dirty_ins = g_e_inl ? calloc((size_t)(fn->ninsns > 0 ? fn->ninsns : 1), 1) : NULL;
  free(g_e_group_used);
  g_e_group_used = NULL;
  g_e_n_shape = g_e_n_skip = g_e_n_fwd = g_e_n_hoist = 0;
  int n_left = 0;
  char gen0[400], gene[160];
  int osr_blk[16], osr_pc[16], osr_ypc[16], osr_n = 0;
  FILE *out = NULL;
  char *mbuf = NULL;
  size_t mlen = 0;
  EFacts facts;
  /* STATIC on purpose: `g_e_types` publishes `&types` for the whole emission, and
     the helpers that name locals read it. As a stack local the address escaped this
     call and the next function's e_rep_prefix read a reused slot -- naming locals
     from another function's reps (`v28 = i27 - 1`). Static gives it the lifetime the
     published pointer requires. */
  static ETypes types;
  int b, k, bi;
  int have_types = 0;

  /* `types` is static, so it survives between calls: reset it before each function
     or the previous function's reps would be visible where this one has none. */
  memset(&types, 0, sizeof(types));
  g_e_types = NULL;

  /* The speculative body emits blocks in reverse postorder and relies on every
     back edge going to a dominator; a function that is irreducible keeps the
     generic body. Checked, not assumed. */
  if (q2_is_reducible(fn) == 1) {
    /* The analyses the typed lowerings read. The emitter never reads
       Q2Value.rep/spec or Q2Insn.fwd: those hold the old lowering's inference,
       which treats BOT as TAG. The facts run in the grouped-exit mode: a grouped
       field access is no safepoint because its miss is an exit, and the field
       emitters emit it that way (a check in the dispatcher enforces it). */
    if (e_analyze(fn, &facts, &types) == 0) {
      have_types = 1;
      g_e_types = &types;
      g_e_facts = &facts;
    } else {
      /* CLEAR the global when this function has no analysis. Leaving the previous
         function's types in place made e_rep_prefix name locals with ANOTHER
         function's reps, so a `dec` wrote an int into what the emitter thought was
         a TAG local and the unit failed to compile (measured: crypto.js). */
      g_e_types = NULL;
    }
  }
  if (!have_types) {
    if (g_e_fl) {
      e_fl_block("no analysis", NULL);
      return 0;
    }
    e_irreducible++;
    fprintf(
        f, "E_TEXT %sJSValue aot_f_%s(JSContext *ctx, JSRegRun *f,\n",
        e_linkage(), hex);
    fprintf(
        f, "                                 JSValue *fr, uint32_t entry_pc)\n");
    fprintf(f, "{\n    return aot_g_%s(ctx, f, fr, entry_pc);\n}\n\n", hex);
    return 0;
  }

  /* The body is written to memory first and copied out at the end, so the report
     can give its size in lines. */
  out = f;
  f = open_memstream(&mbuf, &mlen);
  if (f == NULL) {
    f = out;
    out = NULL;
  }
  /* What the body returns through to enter the generic body at pc 0 (a refusal at
     entry, or speculation switched off). */
  if (g_e_inl)
    snprintf(gen0, sizeof(gen0), "0");
  else if (g_e_fl)
    snprintf(
        gen0, sizeof(gen0),
        "(e_xs = (ENx){ 0, -100, NULL, NULL, NULL, 0, argc, rec_ }, "
        "aot_nx_%s(ctx_in, caller_f, co, this_obj, argv, &e_xs))",
        hex);
  else
    snprintf(gen0, sizeof(gen0), "aot_g_%s(ctx, f, fr, 0)", hex);
  snprintf(gene, sizeof(gene), "aot_g_%s(ctx, f, fr, entry_pc)", hex);
  /* Loop re-entry: a framed body can be entered at a loop header after the generic body
     ran the loop's first iterations. */
  osr_n = 0;
  g_e_osr_loop = 0;
  g_e_osr_res_n = 0;
  if (!g_e_fl && !g_e_inl && fn->g->ctl_count == 0 && !e_nosr_on()) {
    int ib;
    osr_n = e_loop_headers(fn, osr_blk, osr_pc, 16);
    for (ib = 0; ib < osr_n; ib++) osr_ypc[ib] = -1;
    g_e_osr_res_n = 0;
    /* A generator or async function is resumed at the instruction after each suspension,
       with every live register in the frame: those blocks are entries too. */
    for (ib = 0; ib < fn->ninsns && osr_n < 16; ib++) {
      const Q2Insn *si = &fn->insns[ib];
      const ERole *sr = e_roles[si->rop];
      int sb, dup = 0, j;
      if (sr == NULL || sr->control != E_CF_SUSPEND) continue;
      for (sb = 0; sb < fn->nblocks; sb++)
        if (fn->blocks[sb].start_pc == si->next && fn->blocks[sb].npreds == 0) break;
      if (sb >= fn->nblocks) continue;
      for (j = 0; j < osr_n; j++)
        if (osr_blk[j] == sb) dup = 1;
      if (dup) continue;
      osr_blk[osr_n] = sb;
      osr_ypc[osr_n] = si->pc;
      g_e_osr_res[g_e_osr_res_n++] = fn->blocks[sb].start_pc;
      osr_pc[osr_n++] = fn->blocks[sb].start_pc;
    }
  }
  if (osr_n > 0) g_e_osr_loop = 1;
  if (!g_e_fl && !g_e_inl) snprintf(gen0, sizeof(gen0), "aot_g_%s(ctx, f, fr, entry_pc)", hex);
  if (g_e_fl) {
    /* The exit function comes first: the body names it. */
    if (!g_e_inl) e_emit_frameless_exit_fn(out != NULL ? out : f, fn, hex);
    if (!g_e_inl && g_e_fl_calls) e_pure_emit_fn(out != NULL ? out : f);
    fprintf(
        f,
        "#define q2_arg_buf(f_) (argv)\n"
        "#define qjs_aot_get_var_ref(f_, i_) q2_vref_get(co, (i_))\n"
        "#define qjs_aot_var_ref_ptr(f_, i_) q2_vref_ptr(co, (i_))\n");
    if (g_e_inl) {
      fprintf(
          f,
          "static inline __attribute__((always_inline)) int ei_%s(JSContext "
          "*ctx_in, JSRegRun *caller_f,\n"
          "                               JSObject *co, JSValue this_obj,\n"
          "                               JSValue *argv, int argc, JSValue *out, JSValue *ra_x_, int this_ok_)\n"
          "{\n"
          "    _Static_assert(QJS_DRC, \"an inline body needs deferred counting\");\n"
          "    uint8_t *cb = *(uint8_t **)((uint8_t *)co + "
          "QJS_AOT_OFF_OBJECT_FUNC_BYTECODE);\n"
          "    JSContext *ctx = *(JSContext **)(cb + QJS_AOT_OFF_FNBC_REALM);\n"
          "    JSAotFlRec *const rec_ = NULL;\n"
          "    JSValue fr[%d];\n"
          "    Q2_FILL_UNDEF(fr, %d);\n",
          hex, fn->nregs > 0 ? fn->nregs : 1, fn->nregs > 0 ? fn->nregs : 1);
    if (g_e_inl && g_e_inl_r) {
      int mw = e_win_max(fn);
      fprintf(
          f,
          "    enum { SB_ = %d, RB_ = SB_ + %d };\n"
          "    JSValue ra_own_[RB_];\n"
          "    JSValue *ra_ = ra_x_;\n"
          "    if (ra_x_ == NULL) { Q2_FILL_UNDEF(ra_own_, SB_); ra_ = ra_own_; }\n",
          g_e_spill_total, mw);
      g_e_last_inl_rz = g_e_spill_total + mw + g_e_child_decl;
    } else if (g_e_inl) {
      g_e_last_inl_rz = 0;
      fprintf(f, "    (void)ra_x_;\n");
    }
    if (g_e_inl) fprintf(f, "    (void)this_ok_;\n");
    } else
    fprintf(
        f,
        "%s%sJSValue aot_%s_%s(JSContext *ctx_in, JSRegRun *caller_f,\n"
        "                               JSObject *co, JSValue this_obj,\n"
        "                               JSValue *argv, int argc%s)\n"
        "{\n"
        "%s"
        "    _Static_assert(QJS_DRC, \"a frameless body needs deferred counting\");\n"
        "    uint8_t *cb = *(uint8_t **)((uint8_t *)co + "
        "QJS_AOT_OFF_OBJECT_FUNC_BYTECODE);\n"
        "    JSContext *ctx = *(JSContext **)(cb + QJS_AOT_OFF_FNBC_REALM);\n"
        "    JSValue fr[%d];\n"
        "    Q2_FILL_UNDEF(fr, %d);\n",
        g_e_fl_calls ? "static inline __attribute__((always_inline)) " : "E_TEXT ",
        g_e_fl_calls ? "" : e_linkage(), g_e_fl_calls ? "nb" : "n", hex,
        g_e_fl_calls ? ", JSAotFlRec *rec_" : "",
        g_e_fl_calls ? "" : "    JSAotFlRec *const rec_ = NULL; (void)rec_;\n",
        fn->nregs > 0 ? fn->nregs : 1, fn->nregs > 0 ? fn->nregs : 1);
    if (g_e_fl_calls && !g_e_inl) {
      int mw = e_win_max(fn);
      fprintf(
          f,
          "    enum { SB_ = %d, RB_ = SB_ + %d, RT_ = RB_ + %d };\n"
          "    JSValue ra_[RT_];\n"
          "    Q2_FILL_UNDEF(ra_, RT_);\n"
          "    rec_->roots = ra_;\n    rec_->nroots = RT_;\n",
          g_e_spill_total, mw, g_e_child_decl);
    }
  } else {
    /* With loop entries the body is `aot_s_`, and `aot_f_` calls it again whenever the generic
       body hands a loop back (see E_RET_G), so a re-entry is a return and a call: not a jump
       back into the body, and no frame that is not already there. */
    fprintf(
        f, "E_TEXT %sJSValue aot_%c_%s(JSContext *ctx, JSRegRun *f,\n",
        e_linkage(), osr_n > 0 ? 's' : 'f', hex);
    fprintf(
        f, "                                 JSValue *fr, uint32_t entry_pc)\n");
    fprintf(f, "{\n");
  }

  /* Entry. A compiled call passes entry_pc 0; anything else is a resume that
     belongs to the generic body, which has a label for every pc. The switch-off
     counter is checked first and costs one load.

     THE SPEC SWITCHES MUST BE HONOURED HERE TOO. `q2_spec_on`/`q2_no_spec` are set
     from QJS_AOT_SPEC/QJS_AOT_NO_SPEC, but the OLD emitter's spec entry was the only
     reader -- so the spec body ran even with QJS_AOT_SPEC=0, and the harness's
     "nospec" pass was never a generic-only run. */
  if (g_e_inl)
    (void)0;
  else if (g_e_fl)
    fprintf(
        f,
        "    if (__builtin_expect(e_gate[%d], 0))\n"
        "        return %s;\n",
        fi, gen0);
  else if (osr_n > 0) {
    int h;
    fprintf(f, "    if (__builtin_expect(e_gate[%d] != 0", fi);
    fprintf(f, " || (entry_pc != 0");
    for (h = 0; h < osr_n; h++)
      if (osr_ypc[h] != 0) fprintf(f, " && entry_pc != %d", osr_ypc[h] >= 0 ? osr_ypc[h] : osr_pc[h]);
    fprintf(f, "), 0))\n        return %s;\n", gene);
  } else
    fprintf(
        f,
        "    if (__builtin_expect(entry_pc != 0 || e_gate[%d], 0))\n"
        "        return %s;\n",
        fi, gene);
  /* The index must be inside the switch-off arrays; the generator sized them from the
     function count, so this can only fail if the two disagree. */
  fprintf(
      f, "    _Static_assert(%d < QJS_AOT_NFUNCS, \"function index\");\n",
      fi);
  if (!g_e_fl) fprintf(f, "    E_SEAL(ctx, f);\n");

  /* A NULL pic means there is no profile to speculate on and the generic body is
     the only body. */
  fprintf(f, "    {\n");
  if (g_e_fl) {
    fprintf(
        f,
        "    const void *pic = *(void **)(cb + QJS_AOT_OFF_FNBC_REGVM_PIC);\n"
        "    void *const *holders = *(void *const **)(cb +\n"
        "                               QJS_AOT_OFF_FNBC_REGVM_PIC_HOLDER);\n"
        "    const uint32_t q2_ns = (uint32_t)*(const uint16_t *)(cb +\n"
        "                               QJS_AOT_OFF_FNBC_PIC_COUNT);\n"
        "    const uint8_t *ecb = *(const uint8_t *const *)(cb + QJS_AOT_OFF_FNBC_REGVM_CODE); (void)ecb;\n");
  } else {
    fprintf(f, "    const void *pic = q2_pic(f);\n    const uint8_t *ecb = q2_code(f); (void)ecb;\n");
    fprintf(f, "    void *const *holders = q2_pic_holder(f);\n");
    fprintf(f, "    const uint32_t q2_ns = (uint32_t)q2_pic_sites(f);\n");
  }
  fprintf(f, "    (void)q2_ns; (void)holders;\n");
  /* The environment of each copied callee: its cache tables (set where its guard passes) and a scratch
     frame for the writes its instructions make to registers the real frame does not have. */
  {
    int ik;
    for (ik = 0; ik < fn->ninl; ik++)
      fprintf(
          f,
          "    const uint8_t *ipic_%d = NULL; void *const *iholders_%d = NULL; uint32_t ins_%d = 0;\n"
          "    JSValue ifr_%d[%d];\n"
          "    (void)ipic_%d; (void)iholders_%d; (void)ins_%d; (void)ifr_%d;\n",
          ik, ik, ik, ik, fn->nregs, ik, ik, ik, ik);
  }
  /* A function with no PIC sites -- a leaf arithmetic function, say -- has no
     PIC table at all, so `pic` is NULL on EVERY call. Refusing on that refused
     every such function for good (E_REFUSE_NOTE switches a function off after
     enough entries), and the speculative body never ran once. The table is only
     needed by accesses, so the check is emitted only when the function has
     sites; and the count is known here, at generation time. */
  if (e_pic_sites(fn) > 0)
    fprintf(
        f,
        "    if (__builtin_expect(pic == NULL, 0)) {\n"
        "        E_REFUSE_NOTE(%d);\n"
        "        return %s;\n"
        "    }\n",
        fi, gen0);
  /* Entry: each group some access uses takes the shape its first
     site's cell holds, and every other site's cell must agree. A NULL first cell
     means the profile's shape is not in the cache yet (the generic body will
     fill it); a filled cell that names ANOTHER shape means this run does not
     see the profiled one. Either refuses, and the generic body serves the call.
     An unfilled cell on a later site is tolerated: that access misses and exits
     until the generic body fills it. */
  /* A loop entry handed back by the generic body starts here: the group shapes are read again
     from the cells it has filled. */
  {
    int g, q;
    int ng = g_e_facts != NULL ? g_e_facts->ngroups : 0;
    g_e_group_used = calloc((size_t)(ng > 0 ? ng : 1), 1);
    for (q = 0; g_e_group_used != NULL && q < g_e_facts->nsites; q++) {
      const ESiteInfo *si = &g_e_facts->sites[q];
      if (si->group >= 0 && g_e_facts->miss_exits[si->insn])
        g_e_group_used[si->group] = 1;
    }
    /* A table that is not there refuses the entry. Stating it here, once, tells
       the compiler every cell read below has a table, which a non-NULL shape used
       to imply and the all-empty sentinel does not. */
    if (e_pic_sites(fn) == 0)
      for (g = 0; g < ng; g++)
        if (g_e_group_used[g]) {
          fprintf(
              f,
              "    if (__builtin_expect(pic == NULL, 0)) {\n"
              "        E_REFUSE_NOTE(%d);\n"
              "        return %s;\n"
              "    }\n",
              fi, gen0);
          break;
        }
    for (g = 0; g < fn->nvweb; g++) fprintf(f, "    const void *vsh%d = NULL;\n", g + 1);
    {
      g_e_glay_fn = fn;
      e_compute_group_layouts(fn, ng);
      /* The groups of copied callees are set up where their guard runs. */
      for (g = 0; g < ng; g++) {
        const EGroup *gr0 = &g_e_facts->groups[g];
        int q0;
        if (!g_e_group_used[g] || !gr0->own) continue;
        fprintf(f, "    const void *gsh%d = E_NO_SHAPE;\n", g);
        for (q0 = 0; q0 < gr0->nsites; q0++)
          if (e_facts_site_proto(g_e_facts, gr0->sites[q0]) == 2)
            fprintf(f, "    int pci%d = (int)QJS_AOT_REG_IC_NONE;\n", gr0->sites[q0]);
      }
      /* The validation of the cells is done again only when a cache entry has changed since it
         last passed for this bytecode. */
      int ngu = 0, ngf = 0, npci = 0, gi, qq;
      for (g = 0; g < ng; g++)
        if (g_e_group_used[g] && !e_entry_skip(g)) ngu++;
      for (g = 0; g < ng; g++) {
        const EGroup *gr = &g_e_facts->groups[g];
        if (!g_e_group_used[g] || e_entry_skip(g)) continue;
        for (qq = 0; qq < gr->nsites; qq++)
          if (e_facts_site_proto(g_e_facts, gr->sites[qq]) == 2) npci++;
      }
      for (g = 0; g < ng; g++)
        if (g_e_group_used[g] && !g_e_facts->groups[g].own && e_group_layout(g) != 0)
          fprintf(f, "    const void *gsh%d;\n", g);
      /* A body that holds a global as a literal tests the epoch after each safepoint like one
         that holds cache cells. */
      for (g = 0; g < fn->ninsns; g++) {
        int32_t gv_;
        if (e_gfold_val(fn, &fn->insns[g], &gv_)) ngf++;
      }
      g_e_epoch_skip = (ngu > 0 || ngf > 0) && getenv("QJS_AOT_NOEPOCHSKIP") == NULL;
      g_e_mc_n = 0;
      memset(g_e_mc_of, -1, sizeof(g_e_mc_of));
      if (g_e_epoch_skip && getenv("QJS_AOT_NOMETHCACHE") == NULL)
        for (g = 0; g < ng; g++) {
          const EGroup *gr = &g_e_facts->groups[g];
          if (!g_e_group_used[g] || e_entry_skip(g)) continue;
          for (qq = 0; qq < gr->nsites; qq++) {
            int hot = 0, z;
            for (z = 0; z < g_e_facts->nsites; z++)
              if (g_e_facts->sites[z].site == gr->sites[qq]) hot = g_e_facts->sites[z].hot;
            if (hot && e_facts_site_proto(g_e_facts, gr->sites[qq]) == 1 && g_e_mc_n < E_MC_MAX) {
              g_e_mc_site[g_e_mc_n] = gr->sites[qq];
              g_e_mc_grp[g_e_mc_n++] = g;
            }
          }
        }
      if (ngu > 0 || ngf > 0) {
        int gk = 0, pk = 0;
        fprintf(
            f,
            "    static const void *e_vg[%d]; static int e_vp[%d]; static uint32_t e_vep; static const void *e_vcb;\n"
            "    static JSValue e_vm[%d]; static const void *e_vme[%d];\n"
            "    uint32_t q2_ep = *(const uint32_t *)(*(const uint8_t *const *)((const uint8_t *)ctx + "
            "QJS_AOT_OFF_CTX_RT) + QJS_AOT_OFF_RT_PIC_EPOCH);\n",
            ngu > 0 ? ngu : 1, npci > 0 ? npci : 1, g_e_mc_n > 0 ? g_e_mc_n : 1, g_e_mc_n > 0 ? g_e_mc_n : 1);
        for (g = 0; g < g_e_mc_n; g++) fprintf(f, "    JSValue mcv%d; const void *mce%d;\n", g, g);
        for (g = 0; g < ng; g++)
          if (g_e_group_used[g] && !e_entry_skip(g)) fprintf(f, "    const void *gsh%d;\n", g);
        for (g = 0; g < ng; g++) {
          const EGroup *gr = &g_e_facts->groups[g];
          if (!g_e_group_used[g] || e_entry_skip(g)) continue;
          for (qq = 0; qq < gr->nsites; qq++)
            if (e_facts_site_proto(g_e_facts, gr->sites[qq]) == 2)
              fprintf(f, "    int pci%d;\n", gr->sites[qq]);
        }
        fprintf(f, "    if (__builtin_expect(q2_ep == e_vep && (const void *)pic == e_vcb, 1)) {\n");
        for (g = 0, gk = 0; g < ng; g++) {
          const EGroup *gr = &g_e_facts->groups[g];
          if (!g_e_group_used[g] || e_entry_skip(g)) continue;
          fprintf(f, "      gsh%d = e_vg[%d];\n", g, gk++);
          for (qq = 0; qq < gr->nsites; qq++)
            if (e_facts_site_proto(g_e_facts, gr->sites[qq]) == 2)
              fprintf(f, "      pci%d = e_vp[%d];\n", gr->sites[qq], pk++);
        }
        for (g = 0; g < g_e_mc_n; g++) fprintf(f, "      mcv%d = e_vm[%d]; mce%d = e_vme[%d];\n", g, g, g, g);
        fprintf(f, "    } else {\n");
        /* The globals the body holds as literals are confirmed first: the first confirmation
           of one moves the epoch, so the epoch this entry validates at is read after it. */
        {
          int ik, nfold = 0;
          for (ik = 0; ik < fn->ninsns; ik++) {
            int32_t gv;
            const Q2Insn *gi_ = &fn->insns[ik];
            if (!e_gfold_val(fn, gi_, &gv)) continue;
            fprintf(
                f,
                "      if (__builtin_expect(!qjs_aot_global_fold(ctx, (int)qjs_aot_ld_u32(ecb + %d), %d), 0)) {\n"
                "        E_REFUSE_NOTE(%d);\n"
                "        return %s;\n"
                "      }\n",
                gi_->arg[1].off, (int)gv, fi, gen0);
            nfold++;
          }
          if (nfold > 0)
            fprintf(
                f,
                "      q2_ep = *(const uint32_t *)(*(const uint8_t *const *)((const uint8_t *)ctx + "
                "QJS_AOT_OFF_CTX_RT) + QJS_AOT_OFF_RT_PIC_EPOCH);\n");
        }
        (void)gi;
      }
      for (g = 0; g < ng; g++) {
      const EGroup *gr = &g_e_facts->groups[g];
      if (!g_e_group_used[g] || e_entry_skip(g)) continue;
      /* The shape is the first filled cell among the group's sites. */
      fprintf(f, "    gsh%d = NULL;\n", g);
      for (q = 0; q < gr->nsites; q++)
        if (e_facts_site_proto(g_e_facts, gr->sites[q]) == 2)
          fprintf(
              f, "    if (gsh%d == NULL) gsh%d = q2_cell2_shape(pic, %d, (int)q2_ns);\n", g, g,
              gr->sites[q]);
        else
          fprintf(
              f, "    if (gsh%d == NULL) gsh%d = q2_cell_shape(pic, %d);\n", g, g,
              gr->sites[q]);
      /* A group none of whose cells has been filled has not run yet, which is no reason to
         refuse the entry. The shape becomes one no object has, so each access of the group
         takes its miss exit if it is ever reached (and the 257-exit rule switches the
         function off if that turns out to be common), and the generic body fills the cells
         for the next entry. Once any cell holds a shape, a site the profile sampled (hot)
         must have a cell holding it, in the kind it needs (a store needs the writable
         kind): its accesses and the facts assume it until a safepoint. An empty cell or one
         of another shape refuses the entry. A cold site's cell may be empty (its accesses
         test it themselves); only a cell of another shape refuses. */
      fprintf(f, "    if (__builtin_expect(gsh%d == NULL, 0)) gsh%d = E_NO_SHAPE;\n", g, g);
      for (q = 0; q < gr->nsites; q++) {
        int wr = 0, hot = 0, z;
        for (z = 0; z < g_e_facts->nsites; z++)
          if (g_e_facts->sites[z].site == gr->sites[q]) {
            wr = g_e_facts->sites[z].write;
            hot = g_e_facts->sites[z].hot;
          }
        if (e_facts_site_proto(g_e_facts, gr->sites[q]) == 2)
          fprintf(
              f, "    pci%d = q2_cell2_index(pic, %d, (int)q2_ns, gsh%d);\n",
              gr->sites[q], gr->sites[q], g);
        {
          char cib[24];
          fprintf(
              f,
              "    { const void *_s = %s(pic, %s);\n"
              "      if (__builtin_expect(gsh%d != E_NO_SHAPE && %s_s != gsh%d, 0)) {\n"
              "        E_EXIT_LOG(\"%s\", \"refuse-site\", %d, -2);\n"
              "        E_REFUSE_NOTE(%d);\n"
              "        return %s;\n"
              "      } }\n",
              wr && hot ? "q2_cell_wshape" : "q2_cell_shape",
              e_cellidx(cib, sizeof(cib), gr->sites[q]), g, hot ? "" : "_s != NULL && ", g, hex,
              gr->sites[q], fi, gen0);
        }
      }
      /* A shape kept across calls on the strength of its layout must be that layout's. */
      if (e_group_ret(g) != 0)
        fprintf(
            f,
            "    if (__builtin_expect(gsh%d != E_NO_SHAPE && gsh%d != (const void *)q2_layout_shape[%d], 0)) {\n"
            "        E_REFUSE_NOTE(%d);\n"
            "        return %s;\n"
            "    }\n",
            g, g, e_group_ret(g) - 1, fi, gen0);
      }
      if (ngu > 0 || ngf > 0) {
        int gk = 0, pk = 0;
        for (g = 0; g < ng; g++) {
          const EGroup *gr = &g_e_facts->groups[g];
          if (!g_e_group_used[g] || e_entry_skip(g)) continue;
          fprintf(f, "      e_vg[%d] = gsh%d;\n", gk++, g);
          for (qq = 0; qq < gr->nsites; qq++)
            if (e_facts_site_proto(g_e_facts, gr->sites[qq]) == 2)
              fprintf(f, "      e_vp[%d] = pci%d;\n", pk++, gr->sites[qq]);
        }
        for (g = 0; g < g_e_mc_n; g++)
          fprintf(
              f,
              "      mcv%d = JS_MKVAL(QJS_AOT_TAG_LAZY_JSON, 0);\n"
              "      if (gsh%d != E_NO_SHAPE) {\n"
              "        if (__builtin_expect(q2_holder_bad_sh(gsh%d, holders, %d), 0)) {\n"
              "          E_REFUSE_NOTE(%d);\n"
              "          return %s;\n"
              "        }\n"
              "        mcv%d = *q2_hslotp_sh(gsh%d, q2_cell_off(pic, %d));\n"
              "      }\n"
              "      mce%d = q2_entry_of(mcv%d);\n"
              "      e_vm[%d] = mcv%d; e_vme[%d] = mce%d;\n",
              g, g_e_mc_grp[g], g_e_mc_grp[g], g_e_mc_site[g], fi, gen0, g, g_e_mc_grp[g],
              g_e_mc_site[g], g, g, g, g, g, g);
        fprintf(f, "      e_vcb = pic; e_vep = q2_ep;\n    }\n");
      }
      /* A contracted group's shape is its layout's, read on every entry: the layout may
         be built after the shape the cache cells would have remembered. A layout not
         built yet gives a shape no object has, so each access takes its exit. */
      for (g = 0; g < ng; g++)
        if (e_group_layout(g) != 0)
          fprintf(
              f, "    gsh%d = (const void *)q2_layout_shape[%d]; (void)%d;\n",
              g, e_group_layout(g) - 1, e_group_layout(g) - 1);
      /* The holder of a prototype read can change without a cache entry changing: tested on
         every entry. */
      for (g = 0; g < ng; g++) {
        const EGroup *gr = &g_e_facts->groups[g];
        if (!g_e_group_used[g] || g_e_facts->groups[g].own) continue;
        for (q = 0; q < gr->nsites; q++) {
          int hot = 0, z;
          for (z = 0; z < g_e_facts->nsites; z++)
            if (g_e_facts->sites[z].site == gr->sites[q]) hot = g_e_facts->sites[z].hot;
          if (hot && e_facts_site_proto(g_e_facts, gr->sites[q]) == 2)
            fprintf(
                f,
                "    if (__builtin_expect(gsh%d != E_NO_SHAPE &&\n"
                "            q2_holder2_bad_sh(gsh%d, holders, (int)q2_ns, pci%d), 0)) {\n"
                "        E_REFUSE_NOTE(%d);\n"
                "        return %s;\n"
                "    }\n",
                g, g, gr->sites[q], fi, gen0);
          else if (hot && e_facts_site_proto(g_e_facts, gr->sites[q]) && e_mc_index(gr->sites[q]) < 0)
            fprintf(
                f,
                "    if (__builtin_expect(gsh%d != E_NO_SHAPE &&\n"
                "            q2_holder_bad_sh(gsh%d, holders, %d), 0)) {\n"
                "        E_REFUSE_NOTE(%d);\n"
                "        return %s;\n"
                "    }\n",
                g, g, gr->sites[q], fi, gen0);
        }
      }
    }
  }
  if (g_e_nolazy)
    fprintf(
        f,
        "    if (__builtin_expect(E_LAZY_SEEN(ctx), 0)) {\n"
        "        E_REFUSE_NOTE(%d);\n"
        "        return %s;\n"
        "    }\n",
        fi, gen0);
  if (g_e_fl && g_e_fl_calls && !g_e_inl) e_pure_emit_entry(f);
  /* Counted only now: everything that could refuse the entry has been passed, so
     this measures speculative bodies that ACTUALLY RAN. */
  if (!g_e_inl) fprintf(f, "    E_ENTER_NOTE(%d);\n", fi);


  /* One C local per value, by representation. A value that is not live is
     not declared: the IR keeps values that trivial-parameter removal dropped. */
  {
    int nv = fn->nvalues;
    int any = 0;
    for (k = 0; k < nv; k++) {
      /* Declared by the ANALYSIS's representation, the same source the emitters
         read: a declaration and a use that disagreed on a value's local name was
         the `b79`/`v79` class of bug. */
      if (g_e_facts != NULL && k < g_e_facts->narr_vals && g_e_facts->arr_used[k])
        fprintf(f, "    JSValue *ap%u = NULL; uint32_t an%u = 0;\n", (unsigned)k, (unsigned)k);
      if (g_e_facts != NULL && k < g_e_facts->narr_vals && g_e_facts->arr_pk[k])
        fprintf(f, "    void *pkp%u = NULL; uint32_t pkn%u = 0;\n", (unsigned)k, (unsigned)k);
      switch (e_rep_prefix((Q2ValueId)k)[0]) {
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
          fprintf(f, "    const unsigned char *pp%u = NULL; (void)pp%u;\n", (unsigned)k, (unsigned)k);
          break;
      }
      any = 1;
    }
    (void)any;
  }
  e_undo_prepare(fn);
  for (k = 0; k < g_e_undo_total; k++)
    fprintf(f, "    JSValue *us%d = NULL; JSValue uo%d, un%d; (void)uo%d; (void)un%d;\n", k, k, k, k, k);
  g_e_ta_used = 0;
  for (k = 0; k < fn->ninsns; k++)
    if (fn->insns[k].rop == ROP_get_length) g_e_ta_used = 1;
  if (g_e_ta_used)
    fprintf(f, "    const void *ta_sh = NULL; int ta_ok = 0; (void)ta_sh; (void)ta_ok;\n");
  if (g_e_fl) e_compute_rooted(fn);
  if (g_e_fl)
    fprintf(f, "    (void)fr; (void)ctx; (void)caller_f; (void)this_obj;\n");
  else
    fprintf(f, "    (void)fr; (void)ctx; (void)f;\n");
  if (osr_n > 0) {
    int h;
    fprintf(f, "    if (entry_pc != 0) {\n        switch (entry_pc) {\n");
    for (h = 0; h < osr_n; h++) {
      if (osr_ypc[h] == 0) continue; /* the first entry is the normal one */
      if (osr_ypc[h] > 0)
        fprintf(f, "        case %d: goto osrs_%d;\n", osr_ypc[h], osr_ypc[h]);
      else
        fprintf(f, "        case %d: goto osr_%d;\n", osr_pc[h], osr_pc[h]);
    }
    fprintf(f, "        default: return %s;\n        }\n    }\n", gene);
  }

  /* The blocks in reverse postorder. `fn->blocks` is in pc order, which is a
     reverse postorder here: a block starts at every branch target and after
     every terminator, so only a back edge points at a lower index. */
  for (bi = 0; bi < fn->nrpo; bi++) {
    /* `bi` is the RPO POSITION; `b` is the BLOCK INDEX, which is what every
       callee takes. Conflating them made the fall-through comment name one block
       and the arm jump to another (bnpFromString returned early, which is what
       broke crypto). */
    b = fn->rpo[bi];
    g_e_blk = b;
    const Q2Block *blk = &fn->blocks[b];
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
       the spec body does not lower. An instruction is either lowered
       here or IS the exit point -- never "emitted as nothing". */
    /* DISPATCH ORDER: MOST SPECIFIC FIRST. The chain below goes from the narrowest
       emitter to the widest, and the generic op-call wrapper is LAST, as the
       catch-all. This order has been the bug three times (add_imm_self_s, the
       wrapper's tier gate, the fused ops), always silently: an op taken by an
       earlier, narrower test just stops lowering. */
    for (i2 = blk->first_insn; i2 < blk->first_insn + blk->ninsns; i2++) {
      const Q2Insn *in = &fn->insns[i2];
      unsigned oc0 = g_e_opcalls;
      if (g_e_dirty_ins != NULL) g_e_dirty_ins[i2] = (uint8_t)g_e_inl_dirty;
      const ERole *role = e_roles[in->rop];
      /* A terminator ends the block: a return, branch or switch lowers here, any
         other exits at the terminator's pc. */
      g_e_ta_keep = 0;
      if (in->inlret) e_undo_release(f, in->own);
      if (role != NULL && role->control != E_CF_NONE) {
        if (g_e_ta_used && !e_ta_keeps(fn, in, &types)) fprintf(f, "    ta_ok = 0;\n");
        if (emit_typed_return(f, fn, in, &types, cur, have_types)) {
          lowered_to = i2 + 1;
          if (g_e_opcalls != oc0)
            g_e_n_opcall++;
          else
            g_e_n_typed++;
          term_lowered = 1;
          break;
        }
        if (emit_typed_goto(f, fn, in, b, cur, hex)) {
          lowered_to = i2 + 1;
          if (g_e_opcalls != oc0)
            g_e_n_opcall++;
          else
            g_e_n_typed++;
          term_lowered = 1;
          break;
        }
        if (emit_typed_branch_bool(
                f, fn, in, &types, b, cur, have_types, hex) ||
            emit_typed_add_imm_branch(
                f, fn, in, &types, b, cur, have_types, hex) ||
            emit_typed_inc_branch(
                f, fn, in, &types, b, cur, have_types, hex, fi) ||
            emit_typed_branch(
                f, fn, in, &types, b, cur, have_types, hex, fi) ||
            emit_typed_branch_loc(
                f, fn, in, &types, b, cur, have_types, hex, fi) ||
            emit_typed_local_rmw_branch(
                f, fn, in, &types, b, cur, have_types, hex, fi) ||
            emit_tag_branch_truthy(
                f, fn, in, &types, b, cur, have_types, hex) ||
            emit_tag_branch_null(
                f, fn, in, &types, b, cur, have_types, hex) ||
            (have_types &&
             emit_term_wrapper(f, fn, in, &types, b, cur, hex, fi))) {
          lowered_to = i2 + 1;
          if (g_e_opcalls != oc0)
            g_e_n_opcall++;
          else
            g_e_n_typed++;
          term_lowered = 1;
        }
        break;
      }
      if (g_e_pre_done != NULL && g_e_pre_done[i2]) {
        /* emitted at the end of the block before it, with the load it absorbed */
        e_note_defs(cur, fn, in);
        lowered_to = i2 + 1;
        continue;
      }
      {
        int fz = e_try_fuse_atom_get(f, fn, i2, b, blk->first_insn + blk->ninsns, &types, cur,
                                     have_types, hex, fi);
        if (fz && g_e_ta_used) fprintf(f, "    ta_ok = 0;\n");
        if (fz == 1) {
          e_note_defs(cur, fn, &fn->insns[i2 + 1]);
          g_e_n_opcall++;
          i2++;
          lowered_to = i2 + 1;
          continue;
        }
        if (fz == 2) {
          g_e_n_opcall++;
          lowered_to = i2 + 1;
          continue;
        }
      }
      /* A load the facts forward is a copy of the value the location already
         holds: no access, no shape test, no tag guard. The result is current in
         its register through cur[] like any other def. */
      if (g_e_facts != NULL && in->ndef == 1 && in->def[0] != Q2_NO_VALUE &&
          g_e_facts->fwd[i2] != Q2_NO_VALUE) {
        char dn[32];
        e_local_name(dn, sizeof(dn), in->def[0]);
        fprintf(
            f, "    %s = %s;\n", dn,
            e_copy_expr(g_e_facts->fwd[i2], in->def[0]));
        e_note_defs(cur, fn, in);
        g_e_n_fwd++;
        g_e_n_typed++;
        lowered_to = i2 + 1;
        continue;
      }
      /* The spec body's OWN list. `generic_is_trivial` is shared with the generic
         emitter, which has no case for these, so adding one there emits nothing and
         silently drops the instruction. */
      if (generic_is_trivial(in) || in->rop == ROP_get_arg ||
          in->rop == ROP_get_arg2 || in->rop == ROP_move_xfer) {
        if (!trivial_is_lowerable(in, cur)) break;
        emit_trivial_own(f, in, role, cur);
        e_note_defs(cur, fn, in);
      } else if (e_spec_maxpc_get() >= 0 && in->pc >= e_spec_maxpc_get()) {
        /* The maxpc probe: a planned hand-off at this pc. Correctness is
           unaffected, so a bisect over `pc` cannot produce a wrong answer. */
        break;
      } else {
        /* RULE: an emitter either emits or declines -- never both. An emitter that
           writes and then returns 0 makes the dispatcher ALSO hand off, so an op
           runs twice (measured: one extra call per iteration in smoke.js's `lc4`).
           The output position is the witness, so a violation aborts here instead of
           producing a wrong unit. */
        long before = ftell(f);
        int must_exit = e_must_exit(fn, in), emitted;
        g_e_miss_exit_seen = 0;
        if (in->own > 0)
          fprintf(
              f,
              "#define pic ipic_%d\n#define holders iholders_%d\n#define q2_ns ins_%d\n#define fr ifr_%d\n",
              in->own - 1, in->own - 1, in->own - 1, in->own - 1);
        emitted = e_dispatch_rop(
            f, fn, in, &types, cur, have_types, hex, fi, b, role);
        if (!emitted)
          emitted = emit_tag_numeric(
              f, fn, in, &types, cur, have_types, hex, fi, b);
        /* DECLINE MEANS "TRY THE NEXT EMITTER", and the op-call wrapper is the
           last: a typed emitter that declined (a TAG operand, an unsupported
           representation) or an op with no emitter at all runs through the
           engine's own handler and the block goes on. Only an op the wrapper
           cannot run hands off. */
        if (!emitted && have_types && e_wrappable(fn, in, role)) {
          emitted = emit_typed_opcall(f, fn, in, &types, cur, NULL, hex, fi, b);
          /* A transfer store emptied its source register. */
          if (emitted && cur != NULL && e_xfer_src_off(in) >= 0) {
            int sr = e_xfer_src_off(in) / 16;
            if (sr >= 0 && sr < fn->nregs) cur[sr] = E_UNDEF;
          }
        }
        if (in->own > 0) fprintf(f, "#undef pic\n#undef holders\n#undef q2_ns\n#undef fr\n");
        /* The facts treated this access as no safepoint only because its miss
           exits; an emitter that gave it a continuing miss would leave facts the
           miss may have broken. */
        if (emitted && must_exit && !g_e_miss_exit_seen) {
          fprintf(
              stderr,
              "qjsaot: pc %d (rop %u) is a miss-exit access but was not emitted as "
              "one\n",
              in->pc, (unsigned)in->rop);
          abort();
        }
        if (!emitted) {
          if (ftell(f) != before) {
            fprintf(
                stderr,
                "qjsaot: emitter for rop %u (%s, pc %d) wrote %ld bytes and then "
                "declined; it must decide everything before emitting\n",
                (unsigned)in->rop,
                e_roles[in->rop] ? "see the role table" : "?", in->pc,
                ftell(f) - before);
            abort();
          }
          break;
        }
        e_note_defs(cur, fn, in);
        if (g_e_ta_used && !g_e_ta_keep && !e_ta_keeps(fn, in, &types))
          fprintf(f, "    ta_ok = 0;\n");
      }
      lowered_to = i2 + 1;
      if (g_e_opcalls != oc0)
        g_e_n_opcall++;
      else
        g_e_n_typed++;
    }
    n_left += blk->first_insn + blk->ninsns - lowered_to;
    if (term_lowered) {
      free(cur);
      continue;
    }
    if (lowered_to < blk->first_insn + blk->ninsns) {
      /* EXIT AT an instruction's pc: that instruction has not run, so the live
         set is its live-before (live-out minus its defs, plus its uses). */
      uint64_t *lb =
          calloc((size_t)(fn->lwords > 0 ? fn->lwords : 1), 8);
      e_handoff_note(fn, &fn->insns[lowered_to]);
      e_fl_block("unlowered", &fn->insns[lowered_to]);
      /* NAME THE REASON. The reps come from ETypes.lb -- never Q2Value.rep,
         which reads BOT as TAG. A decline is
         almost always a representation some emitter will not accept. */
      if (!g_e_quiet || getenv("QJS_AOT_FL_VERBOSE")) {
        const Q2Insn *hi = &fn->insns[lowered_to];
        char nm[64];
        int u, dd;
        e_rop_name(hi->rop, nm, sizeof(nm));
        fprintf(
            stderr,
            "handoff-at hex=%s pc=%d rop=%u name=%s tier=%d defs=",
            g_cur_hex, hi->pc, (unsigned)hi->rop, nm,
            e_roles[hi->rop] ? (int)e_roles[hi->rop]->tier : -1);
        for (dd = 0; dd < hi->ndef; dd++) {
          Q2ValueId v = hi->def[dd];
          fprintf(stderr, "%s%d:%s", dd ? "," : "",
                  (int)v, v < (Q2ValueId)types.nvalues ? e_rep_prefix(v) : "?");
        }
        fprintf(stderr, " uses=");
        for (u = 0; u < hi->nuse; u++) {
          Q2ValueId v = hi->use[u];
          fprintf(stderr, "%s%d:%s", u ? "," : "",
                  (int)v, v < (Q2ValueId)types.nvalues ? e_rep_prefix(v) : "?");
        }
        fprintf(stderr, "\n");
      }
      fprintf(f, "    /* exit at the first unlowered instruction */\n");
      if (lb != NULL) q2_live_before(fn, lowered_to, lb);
      e_emit_exit(
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
          blk->ninsns > 0 && e_roles[fn->insns[last].rop] != NULL &&
          e_roles[fn->insns[last].rop]->control != E_CF_NONE;
      if (set != NULL) q2_live_before(fn, last, set);
      if (is_term) {
        e_handoff_note(fn, &fn->insns[last]);
        e_fl_block("terminator", &fn->insns[last]);
        fprintf(f, "    /* the terminator: exit at its own pc */\n");
        e_emit_exit(f, fn, fn->insns[last].pc, hex, fi, cur, set, 1, b);
      } else if (blk->nsucc == 1 && blk->succ_block[0] >= 0 &&
                 blk->succ_block[0] < fn->nblocks) {
        /* Fall-through: the block has ONE successor and no terminator, so this is
           a lowered edge like any other. Handing off here instead -- which is what
           this case used to do -- meant a fully lowered block still exited, so no
           function ever completed on the spec path. */
        fprintf(f, "    /* fall through to s_%d */\n",
                fn->blocks[blk->succ_block[0]].start_pc);
        /* The arm must target the SAME block the comment named. `b` is a block
           index (see the loop head); passing an RPO position here silently jumped
           to another block -- the bug that made bnpFromString return early and broke
           crypto's RSA. An abort here is worth more than a wrong unit. */
        e_emit_arm(f, fn, b, 0, cur, hex, blk->succ_block[0]);
      } else {
        const uint64_t *lo = q2_live_out(fn, last);
        e_fl_block("no single successor", NULL);
        fprintf(f, "    /* fall through, no single successor: exit */\n");
        e_emit_exit(f, fn, blk->end_pc, hex, fi, cur, lo, 1, b);
      }
      free(set);
    }
    free(cur);
  }
  /* The argument-guard exits: a typed argument arrived with the wrong tag. The
     frame still holds the caller's arguments; the stub syncs what the prefix
     defined before the guard. */
  {
    int o;
    for (o = 0; o < g_e_ax_n; o++) {
      int pc2 = g_e_ax[o], ii, ins = -1;
      for (ii = 0; ii < fn->ninsns; ii++)
        if (fn->insns[ii].pc == pc2) { ins = ii; break; }
      if (ins < 0) continue;
      fprintf(f, "    ax_%d:;\n", pc2);
      {
        const char *tx = e_tag_expr('a', pc2);
        char te[200];
        snprintf(
            te, sizeof(te), "%s%s%s", tx != NULL ? "JS_VALUE_GET_TAG(" : "",
            tx != NULL ? tx : "-100", tx != NULL ? ")" : "");
        e_emit_exit_log(f, hex, "ax", pc2, te);
      }
      e_emit_exit_at(
          f, fn, ins, pc2, hex, fi, g_e_ax_cur[o], g_e_ax_blk[o], 0);
    }
    for (o = 0; o < g_e_ax_n; o++) free(g_e_ax_cur[o]);
    g_e_ax_n = 0;
  }
  /* The closure-cell load's deopt exits: the cell holds a value whose tag the
     site does not accept, which is an EXPECTED outcome (profiling cannot promise
     what a cell holds), so the exit syncs and hands over like any other. */
  {
    int o;
    for (o = 0; o < g_e_vx_n; o++) {
      int pc2 = g_e_vx[o], ii, ins = -1;
      for (ii = 0; ii < fn->ninsns; ii++)
        if (fn->insns[ii].pc == pc2) { ins = ii; break; }
      if (ins < 0) continue;
      fprintf(f, "    vx_%d:;\n", pc2);
      {
        const char *tx = e_tag_expr('v', pc2);
        char te[200];
        snprintf(
            te, sizeof(te), "%s%s%s", tx != NULL ? "JS_VALUE_GET_TAG(" : "",
            tx != NULL ? tx : "-100", tx != NULL ? ")" : "");
        e_emit_exit_log(f, hex, "vx", pc2, te);
      }
      e_emit_exit_at(
          f, fn, ins, pc2, hex, fi, g_e_vx_cur[o], g_e_vx_blk[o], 0);
    }
    for (o = 0; o < g_e_vx_n; o++) free(g_e_vx_cur[o]);
    g_e_vx_n = 0;
  }
  /* The op-call reload guards: the handler returned a JSValue whose tag the site's
     representation does not accept. The op has run, so the exit is at the NEXT pc,
     and the op's result is already in the frame (E_INFRAME in the snapshot). */
  {
    int o, q2;
    for (o = 0; o < g_e_dx_n; o++) {
      int pc2 = g_e_dx[o], ii, ins = -1;
      for (ii = 0; ii < fn->ninsns; ii++)
        if (fn->insns[ii].pc == pc2) { ins = ii; break; }
      if (ins < 0) continue;
      fprintf(f, "    dx_%d:;\n", pc2);
      /* The result of a call to compiled code is owned. A frameless body has no
         register to adopt it into, so it is parked in the published window and only
         then released (the release can run a collection). */
      for (q2 = 0; q2 < g_e_dx_own_n; q2++)
        if (g_e_dx_own_pc[q2] == pc2)
          fprintf(
              f,
              "    if (JS_VALUE_HAS_REF_COUNT(AOT_REG(fr, %d))) {\n"
              "        qjs_aot_set_reg_p(ctx, &ra_[SB_], 0, AOT_REG(fr, %d));\n"
              "    }\n",
              g_e_dx_own_off[q2], g_e_dx_own_off[q2]);
      {
        const char *tx = e_tag_expr('d', pc2);
        char te[200];
        snprintf(
            te, sizeof(te), "%s%s%s", tx != NULL ? "JS_VALUE_GET_TAG(" : "",
            tx != NULL ? tx : "-100", tx != NULL ? ")" : "");
        e_emit_exit_log(f, hex, "dx", pc2, te);
      }
      e_emit_exit_at(
          f, fn, ins, pc2, hex, fi, g_e_dx_cur[o], g_e_dx_blk[o], 0);
    }
    for (o = 0; o < g_e_dx_n; o++) free(g_e_dx_cur[o]);
    g_e_dx_n = 0;
    g_e_dx_own_n = 0;
  }
  /* The overflow exits: the arithmetic check jumps here, and the exit does the
     sync and the hand-over. Nothing was written to the frame by the arithmetic
     itself, so the generic body redoes the operation in full precision. */
  {
    int o;
    for (o = 0; o < g_e_ox_n; o++) {
      int pc2 = g_e_ox[o], ii, ins = -1;
      for (ii = 0; ii < fn->ninsns; ii++)
        if (fn->insns[ii].pc == pc2) { ins = ii; break; }
      if (ins < 0) continue;
      fprintf(f, "    ox_%d:;\n", pc2);
      {
        const char *tx = e_tag_expr('o', pc2);
        char te[200];
        snprintf(
            te, sizeof(te), "%s%s%s", tx != NULL ? "JS_VALUE_GET_TAG(" : "",
            tx != NULL ? tx : "-100", tx != NULL ? ")" : "");
        e_emit_exit_log(f, hex, "ox", pc2, te);
      }
      e_emit_exit_at(
          f, fn, ins, pc2, hex, fi, g_e_ox_cur[o], g_e_ox_blk[o], 0);
    }
    for (o = 0; o < g_e_ox_n; o++) free(g_e_ox_cur[o]);
    g_e_ox_n = 0;
  }
  /* The compare COLD ARM: a TAG operand whose tag is neither INT nor FLOAT64 jumps
     here. A hand-off at the branch's own pc -- the arithmetic has not run and the
     branch has not been taken, so the generic body redoes the compare -- and NOT a
     deopt: a string compare is not a failed speculation and must not count toward
     the switch-off. */
  {
    int o;
    for (o = 0; o < g_e_hx_n; o++) {
      int pc2 = g_e_hx[o], ii, ins = -1;
      for (ii = 0; ii < fn->ninsns; ii++)
        if (fn->insns[ii].pc == pc2) { ins = ii; break; }
      if (ins < 0) continue;
      fprintf(f, "    hx_%d:;\n", pc2);
      e_emit_exit_at(
          f, fn, ins, pc2, hex, fi, g_e_hx_cur[o], g_e_hx_blk[o], 1);
    }
    for (o = 0; o < g_e_hx_n; o++) free(g_e_hx_cur[o]);
    g_e_hx_n = 0;
  }
  /* The loop entries: the live registers come from the frame (the generic body keeps
     the true state there), each checked for the representation the loop assumes, and
     the shape checks hoisted out of the loop run again. Any failure goes back to the
     generic body at the same pc. */
  if (osr_n > 0) {
    int h, q, o2;
    for (h = 0; h < osr_n; h++) {
      const Q2Block *hb = &fn->blocks[osr_blk[h]];
      if (osr_ypc[h] > 0)
        fprintf(
            f,
            "    osrs_%d:;\n"
            "    { uint8_t *code_ = q2_code(f), *ip_ = qjs_aot_step(code_ + %d, fr, f, ctx);\n"
            "      if (__builtin_expect(ip_ == NULL, 0)) return qjs_aot_throw_here(ctx, f, UINT32_MAX);\n"
            "      if (__builtin_expect(ip_ != code_ + %d, 0)) {\n"
            "          if (ip_ == QJS_AOT_COLD_RET || ip_ == QJS_AOT_SUSPEND) return qjs_aot_ret(f);\n"
            "          return aot_g_%s(ctx, f, fr, (uint32_t)(ip_ - code_)); } }\n",
            osr_ypc[h], osr_ypc[h], osr_pc[h], hex);
      fprintf(f, "    osr_%d:;\n    {\n", osr_pc[h]);
      for (q = 0; q < hb->nlive_in; q++) {
        int rr = hb->in_reg[q];
        Q2ValueId vv = hb->in_val[q];
        if (rr < 0 || rr >= fn->nregs || vv == Q2_NO_VALUE || vv >= (Q2ValueId)types.nvalues)
          continue;
        if (rr >= fn->norig) {
          /* A register only this IR has: the load that was moved before the loop runs again
             here, and a miss returns to the generic body. */
          const Q2Value *dvv = &fn->values[vv];
          const Q2Insn *sin = dvv->insn >= 0 ? &fn->insns[dvv->insn] : NULL;
          if (sin == NULL || sin->rop != ROP_get_global || sin->narg < 4) {
            fprintf(f, "      goto osr_fail;\n");
            continue;
          }
          fprintf(
              f,
              "      { JSValue _g; if (!q2_global_hit(ctx, pic, %d, &_g)) goto osr_fail;\n",
              (int)sin->arg[3].val);
          if (types.lb[vv] == E_LB_TAG)
            fprintf(f, "        v%u = _g; }\n", (unsigned)vv);
          else if (types.lb[vv] == E_LB_I32)
            fprintf(
                f,
                "        if (!Q2_I32_OK(_g, i%u)) goto osr_fail; }\n",
                (unsigned)vv);
          else
            fprintf(
                f,
                "        if (JS_VALUE_GET_TAG(_g) == JS_TAG_INT) d%u = (double)JS_VALUE_GET_INT(_g);\n"
                "        else if (JS_VALUE_GET_TAG(_g) == JS_TAG_FLOAT64) d%u = JS_VALUE_GET_FLOAT64(_g);\n"
                "        else goto osr_fail; }\n",
                (unsigned)vv, (unsigned)vv);
          continue;
        }
        switch (types.lb[vv]) {
          case E_LB_I32:
            fprintf(
                f,
                "      if (!Q2_I32_OK(AOT_REG(fr, %d), i%u)) goto osr_fail;\n",
                rr * 16, (unsigned)vv);
            break;
          case E_LB_F64:
            fprintf(
                f,
                "      { JSValue _x = AOT_REG(fr, %d);\n"
                "        if (JS_VALUE_GET_TAG(_x) == JS_TAG_INT) d%u = (double)JS_VALUE_GET_INT(_x);\n"
                "        else if (JS_VALUE_GET_TAG(_x) == JS_TAG_FLOAT64) d%u = JS_VALUE_GET_FLOAT64(_x);\n"
                "        else goto osr_fail; }\n",
                rr * 16, (unsigned)vv, (unsigned)vv);
            break;
          case E_LB_BOOL:
            fprintf(
                f,
                "      if (JS_VALUE_GET_TAG(AOT_REG(fr, %d)) != JS_TAG_BOOL) goto osr_fail;\n"
                "      b%u = JS_VALUE_GET_BOOL(AOT_REG(fr, %d));\n",
                rr * 16, (unsigned)vv, rr * 16);
            break;
          case E_LB_TAG:
            fprintf(f, "      v%u = AOT_REG(fr, %d);\n", (unsigned)vv, rr * 16);
            break;
          default:
            break;
        }
      }
      if (g_e_facts != NULL)
        for (o2 = 0; o2 < g_e_facts->nhoist; o2++) {
          Q2ValueId rv;
          if (g_e_facts->hoist_hdr[o2] != osr_blk[h]) continue;
          if (g_e_facts->hoist_grp[o2] <= -1000) {
            e_cell_hoist_check(f, g_e_facts->hoist_val[o2], -1000 - g_e_facts->hoist_grp[o2], "osr", 0);
            continue;
          }
          rv = (Q2ValueId)g_e_facts->hoist_val[o2];
          if (g_e_facts->hoist_val[o2] == 0x10000000 && g_e_facts->hoist_grp[o2] >= 0) {
            fprintf(
                f,
                "      { JSValue _th = %s;\n"
                "        if (__builtin_expect(JS_VALUE_GET_TAG(_th) != JS_TAG_OBJECT ||\n"
                "            q2_shape_of(_th) != gsh%d, 0)) goto osr_fail; }\n",
                g_e_fl ? "this_obj" : "*(JSValue *)((uint8_t *)f + QJS_AOT_OFF_REGRUN_THIS_OBJ)",
                g_e_facts->hoist_grp[o2]);
            continue;
          }
          if (rv < (Q2ValueId)types.nvalues && types.lb[rv] == E_LB_TAG &&
              (g_e_facts->hoist_grp[o2] == -2 || (g_e_facts->hoist_grp[o2] <= -101 && g_e_facts->hoist_grp[o2] >= -112) ||
               g_e_facts->hoist_grp[o2] == E_GRP_PK_I32 || g_e_facts->hoist_grp[o2] == E_GRP_PK_F64))
            e_arr_region_start_grp(f, rv, rv, "goto osr_fail", g_e_facts->hoist_grp[o2]);
          else if (rv < (Q2ValueId)types.nvalues && types.lb[rv] == E_LB_TAG)
            fprintf(
                f, "      if (__builtin_expect(q2_shape_of(v%u) != gsh%d, 0)) goto osr_fail;\n",
                (unsigned)rv, g_e_facts->hoist_grp[o2]);
          else
            fprintf(f, "      goto osr_fail;\n");
        }
      fprintf(f, "      goto s_%d;\n    }\n", osr_pc[h]);
    }
    fprintf(f, "    osr_fail:;\n    {\n      e_osr_n[%d] += 8;\n      return %s;\n    }\n", fi, gene);
  }

  fprintf(f, "    }\n");
  fprintf(f, "}\n");
  if (osr_n > 0 && !g_e_fl && !g_e_inl)
    fprintf(
        f,
        "E_TEXT %sJSValue aot_f_%s(JSContext *ctx, JSRegRun *f, JSValue *fr, uint32_t entry_pc)\n"
        "{\n"
        "    JSValue r_ = aot_s_%s(ctx, f, fr, entry_pc);\n"
        "    while (__builtin_expect(JS_VALUE_GET_TAG(r_) == JS_TAG_UNINITIALIZED, 0))\n"
        "        r_ = aot_s_%s(ctx, f, fr, (uint32_t)JS_VALUE_GET_INT(r_));\n"
        "    return r_;\n"
        "}\n",
        e_linkage(), hex, hex, hex);
  if (osr_n > 0)
    fprintf(
        f,
        "static void __attribute__((constructor)) e_osr_reg_%s(void)\n"
        "{ e_osr_ok[%d] = 1; }\n",
        hex, fi);
  if (g_e_fl && !g_e_inl && g_e_fl_calls)
    fprintf(
        f,
        "E_TEXT %sJSValue aot_n_%s(JSContext *ctx_in, JSRegRun *caller_f,\n"
        "                               JSObject *co, JSValue this_obj,\n"
        "                               JSValue *argv, int argc)\n"
        "{\n"
        "    uint8_t *rt = *(uint8_t **)((uint8_t *)ctx_in + QJS_AOT_OFF_CTX_RT);\n"
        "    JSAotFlRec rec;\n"
        "    JSValue r;\n"
        "    if (__builtin_expect((uintptr_t)__builtin_frame_address(0) <\n"
        "            *(uintptr_t *)(rt + QJS_AOT_OFF_RT_STACK_LIMIT) + "
        "E_STACK_MARGIN, 0))\n"
        "        return qjs_aot_throw_stack_overflow(ctx_in);\n"
        "    if (__builtin_expect(--*(int *)((uint8_t *)ctx_in + "
        "QJS_AOT_OFF_CTX_INTERRUPT_COUNTER) <= 0, 0)) {\n"
        "        ++*(int *)((uint8_t *)ctx_in + "
        "QJS_AOT_OFF_CTX_INTERRUPT_COUNTER);\n"
        "        if (qjs_aot_poll_fl(ctx_in) < 0) return JS_EXCEPTION;\n"
        "    }\n"
        "    rec.prev = *(JSAotFlRec **)(rt + QJS_AOT_OFF_RT_FL_TOP);\n"
        "    rec.func = co;\n"
        "    rec.pc = 0;\n"
        "    rec.nroots = 0;\n"
        "    *(JSAotFlRec **)(rt + QJS_AOT_OFF_RT_FL_TOP) = &rec;\n"
        "    r = aot_nb_%s(ctx_in, caller_f, co, this_obj, argv, argc, &rec);\n"
        "    *(JSAotFlRec **)(rt + QJS_AOT_OFF_RT_FL_TOP) = rec.prev;\n"
        "    return r;\n"
        "}\n",
        e_linkage(), hex, hex);
  if (g_e_fl)
    fprintf(
        f,
        "#undef q2_arg_buf\n#undef qjs_aot_get_var_ref\n"
        "#undef qjs_aot_var_ref_ptr\n");
  if (g_e_fl && !g_e_inl)
    fprintf(
        f,
        "static void __attribute__((constructor)) e_fl_reg_%s(void)\n"
        "{ e_nfl[%d] = 1; }\n",
        hex, fi);
  fprintf(f, "\n");
  (void)k;

  /* Step 8's report lines for this function. */
  {
    int vals[4] = {0, 0, 0, 0}, tiers[4], checks[9], v;
    long lines = -1, bytes = -1;
    for (v = 0; v < types.nvalues; v++) switch (types.lb[v]) {
        case E_LB_I32:
          vals[0]++;
          break;
        case E_LB_F64:
          vals[1]++;
          break;
        case E_LB_BOOL:
          vals[2]++;
          break;
        default:
          vals[3]++;
          break;
      }
    tiers[0] = g_e_n_typed;
    tiers[1] = g_e_n_opcall;
    tiers[2] = n_left;
    tiers[3] = fn->ninsns - g_e_n_typed - g_e_n_opcall - n_left;
    checks[0] = g_e_n_guard[0];
    checks[1] = g_e_n_guard[1];
    checks[2] = g_e_n_guard[2];
    checks[3] = g_e_n_guard[3];
    checks[4] = g_e_n_exit;
    checks[5] = g_e_n_handoff;
    checks[6] = g_e_n_shape;
    checks[7] = g_e_n_skip;
    checks[8] = g_e_n_fwd;
    if (out != NULL) {
      size_t i;
      fclose(f);
      lines = 0;
      for (i = 0; i < mlen; i++)
        if (mbuf[i] == '\n') lines++;
      bytes = (long)mlen;
      fwrite(mbuf, 1, mlen, out);
      free(mbuf);
      f = out;
    }
    if (g_e_fl && !g_e_inl) e_emit_frameless_forward(f, fn, hex);
    if (!g_e_quiet && !g_e_inl) e_report_spec(hex, vals, tiers, checks, lines, bytes);
    if (!g_e_quiet && !g_e_inl && g_e_n_hoist > 0) e_report_hoist(hex, g_e_n_hoist);
  }
  e_facts_free(&facts);
  g_e_facts = NULL;
  return 0;
}

/* Whether a function is worth trying as a frameless one: the static rules of D10.
   1. a native entry exists at all (`light_ok`) and its frame is small;
   2. no variable area (it holds counted values that must be released);
   3. no call site (a call needs the caller's frame, and rules P and K stay off);
   4. no op that allocates.
   Rules 5 and 6 (every instruction has a fast path whose slow paths are exits)
   are decided by emitting the body and seeing whether anything needed a handler. */
/* QJS_AOT_FL_LOOPS=1 lifts the loop rule, for the cases that exercise the
   in-loop poll of a frameless body. */
static int e_fl_loops(void) {
  static int cached = -1;
  if (cached < 0) {
    const char *v = getenv("QJS_AOT_FL_LOOPS");
    cached = v != NULL && v[0] == '1';
  }
  return cached;
}

/* The loop blocks of the function and whether any loop may run long (see below). */
static void e_fl_loop_setup(const Q2Func *fn) {
  const JSAotGenFunc *g = fn->g;
  {
    unsigned long long entries = 0;
    int b, si, q;
    free(g_e_loop_blk);
    g_e_loop_blk = calloc((size_t)(fn->nblocks > 0 ? fn->nblocks : 1), 1);
    if (g_e_loop_blk == NULL) abort();
    g_e_fl_longloop = 0;
    for (q = 0; q < fn->ninsns; q++) {
      const Q2Insn *ii = &fn->insns[q];
      unsigned long long c;
      if (ii->pc >= fn->blocks[0].end_pc) break;
      c = qjs_aot_prof_pc_count(g->digest, ii->pc);
      if (c > entries) entries = c;
    }
    for (b = 0; b < fn->nblocks; b++) {
      const Q2Block *blk = &fn->blocks[b];
      for (si = 0; si < blk->nsucc && si < 2; si++) {
        int sb = blk->succ_block[si], m;
        unsigned long long hdr = 0;
        if (sb < 0 || sb >= fn->nblocks || !q2_is_back_edge(fn, b, sb)) continue;
        for (m = sb; m <= b; m++) g_e_loop_blk[m] = 1;
        for (q = 0; q < fn->ninsns; q++) {
          const Q2Insn *ii = &fn->insns[q];
          unsigned long long c;
          if (ii->pc < fn->blocks[sb].start_pc || ii->pc >= blk->end_pc) continue;
          c = qjs_aot_prof_pc_count(g->digest, ii->pc);
          if (c > hdr) hdr = c;
        }
        /* A loop is long past 16 iterations per entry. Profile counts for a kernel near
           16 move with every re-profile, so the 12..20 band is decided on the safe
           (long) side and only a loop clearly below it is short. */
        if (entries == 0 || hdr > 12 * entries) g_e_fl_longloop = 1;
      }
    }
    if (e_fl_loops()) g_e_fl_longloop = 0;
  }
}

/* Whether the profile saw the call reach a callee this unit compiled and enters natively:
   the call then becomes a direct call in a frameless body. */
static int e_call_has_compiled_callee(const Q2Func *fn, const Q2Insn *in) {
  char callee[4][33];
  unsigned long long hits[4];
  int kind[4], n = 0, k;
  if (!qjs_aot_prof_callees(fn->g->digest, in->next, &n, callee, kind, hits))
    return 0;
  for (k = 0; k < n && k < 4; k++)
    if (kind[k] == 1 && qjs_aot_unit_has(callee[k]) &&
        qjs_aot_native_capable(callee[k]) &&
        in->win_n >= qjs_aot_unit_argc(callee[k]))
      return 1;
  return 0;
}

static const char *e_fl_prefilter(const Q2Func *fn) {
  const JSAotGenFunc *g = fn->g;
  int i, has_calls;
  if (!g->light_ok &&
      !(g->func_kind == E_FUNC_NORMAL && g->ctl_count == 0))
    return "not light";
  if (g->light_need > 16 * 1024) return "frame over 16 KB";
  if (g->light_var_count > 0) return "variable area";
  if (g->func_kind != E_FUNC_NORMAL) return "not a plain function";
  /* A call site keeps the function framed, unless every one is an intrinsic the
     analysis may lower: its fallback is an exit, so the body needs no frame. A
     site the analysis then keeps as a call blocks the emission below. */
  has_calls = 0;
  for (i = 0; i < fn->ninsns; i++) {
    const ERole *r = e_roles[fn->insns[i].rop];
    if (r == NULL || r->window < 0) continue;
    if (fn->insns[i].op == Q2_OP_CALL && e_intrinsic_lowerable(fn, &fn->insns[i]))
      continue;
    if (fn->insns[i].rop == ROP_array_from || fn->insns[i].rop == ROP_object_fill)
      continue;
    if ((fn->insns[i].rop == ROP_call || fn->insns[i].rop == ROP_call_method) &&
        getenv("QJS_AOT_NOFLCALLS") == NULL &&
        e_call_has_compiled_callee(fn, &fn->insns[i])) {
      has_calls = 1;
      continue;
    }
    return "call site";
  }
  /* A frameless body cannot go back: an exit hands the REST of the call to the
     generic body, so one miss inside a long loop costs every remaining iteration.
     So the body keeps out any access that can miss for a reason the profile cannot
     see (an element, an ungrouped field, a length) when it sits in a loop that is
     not known to be short: the profile's tick samples give the trips per call (the
     busiest sampled instruction of the loop against the busiest one of the entry
     block, both samples at one rate). The loop blocks and the verdict are set up
     here and consulted where the miss exit is emitted. */
  e_fl_loop_setup(fn);
  /* A function that calls does a call's worth of work between its accesses, and an
     exit repeats all of it in the generic body: every block counts as inside a long
     loop, so an access the profile cannot vouch for keeps the function framed. */
  for (i = 0; i < fn->ninsns && !has_calls; i++) {
    uint16_t rp = fn->insns[i].rop;
    if (rp == ROP_new_object || rp == ROP_object_template || rp == ROP_array_from ||
        rp == ROP_object_fill)
      has_calls = 1;
  }
  if (has_calls) {
    memset(g_e_loop_blk, 1, (size_t)(fn->nblocks > 0 ? fn->nblocks : 1));
    g_e_fl_longloop = 1;
  }
  for (i = 0; i < fn->ninsns; i++) {
    static const char *const names[] = {
        "array_const",  "make_closure", "define_class", "define_class_computed",
        "regexp",       "special_object", "to_object"};
    char nm[64];
    size_t k;
    e_rop_name(fn->insns[i].rop, nm, sizeof(nm));
    for (k = 0; k < sizeof(names) / sizeof(names[0]); k++)
      if (strcmp(nm, names[k]) == 0) {
        static char buf[96];
        snprintf(buf, sizeof(buf), "allocates %s@%d", nm, fn->insns[i].pc);
        return buf;
      }
  }
  return NULL;
}

static int e_fl_enabled(void) {
  static int cached = -1;
  if (cached < 0) {
    const char *v = getenv("QJS_AOT_NOFRAMELESS");
    cached = !(v != NULL && v[0] != '0');
  }
  return cached;
}

/* The speculative body. A function that qualifies is emitted frameless: tried
   first into a buffer whose output is discarded, and kept only if nothing in it
   needed the engine's handler. Otherwise the framed body is emitted as before. */
/* A function over this many instructions keeps the generic body only. The speculative
   body keeps a C local per value, so a function with thousands of straight-line
   values would take a C frame of hundreds of kilobytes (the interpreter's frame for
   it lives on the heap), and a second body doubles its code. box2d's largest
   function is about 5,000 instructions. */
#define E_SPEC_BODY_MAX_INSNS 6000

int e_emit_spec_body(FILE *f, const Q2Func *fn, char hex[33]) {
  const char *why;
  int rc;
  g_e_fl_chosen = 0;
  g_e_fl_why[0] = 0;
  if (fn->ninsns > E_SPEC_BODY_MAX_INSNS) {
    snprintf(g_e_fl_why, sizeof(g_e_fl_why), "over the size budget");
    fprintf(
        f,
        "%sJSValue aot_f_%s(JSContext *ctx, JSRegRun *f, JSValue *fr, uint32_t entry_pc)\n"
        "{\n    return aot_g_%s(ctx, f, fr, entry_pc);\n}\n\n",
        e_linkage(), hex, hex);
    return 0;
  }
  if (!e_fl_enabled()) {
    snprintf(g_e_fl_why, sizeof(g_e_fl_why), "disabled");
    return e_emit_spec_body_pass(f, fn, hex);
  }
  why = e_fl_prefilter(fn);
  if (why != NULL) {
    snprintf(g_e_fl_why, sizeof(g_e_fl_why), "%s", why);
    return e_emit_spec_body_pass(f, fn, hex);
  }
  {
    char *mb = NULL;
    size_t ml = 0;
    FILE *m = open_memstream(&mb, &ml);
    if (m == NULL) return e_emit_spec_body_pass(f, fn, hex);
    g_e_fl = 1;
    g_e_quiet = 1;
    g_e_fl_calls = 0;
    g_e_spill_total = 0;
    g_e_child_max = 0;
    g_e_fl_block[0] = 0;
    rc = e_emit_spec_body_pass(m, fn, hex);
    fclose(m);
    free(mb);
    g_e_quiet = 0;
    g_e_spill_total = g_e_spill_n;
    g_e_child_decl = g_e_child_max;
    g_e_child_max = 0;

    if (rc == 0 && g_e_fl_block[0] == 0) {
      g_e_fl_chosen = 1;
      rc = e_emit_spec_body_pass(f, fn, hex);
      g_e_fl = 0;
      return rc;
    }
    g_e_fl = 0;
    snprintf(g_e_fl_why, sizeof(g_e_fl_why), "%s", g_e_fl_block);
  }
  return e_emit_spec_body_pass(f, fn, hex);
}

/* Whether the body just emitted is a frameless one, and why not otherwise. */
int e_fl_was_chosen(void) { return g_e_fl_chosen; }
const char *e_fl_why_not(void) { return g_e_fl_why; }

/* ---- inline bodies ---------------------------------------

   A small leaf, emitted as `ei_HEX` for a compiled caller to run in place of the
   call. See g_e_inl. The candidate rule is the rule: `light_ok`, no loop, at most 40
   instructions, no call but an intrinsic, no op that needs the engine's handler (the
   frameless emission blocks on any), every value typed by its own speculative body.
   The checks of every block come before any store, which the text is checked for. */

/* Every `return` in the emitted text is the exit (`return 0;`), the result (`return 1;`) or the
   borrowed result (`return 2;`). Returns 1 when the text is fine. */
static int e_inl_text_ok(const char *txt, size_t n, char *why, size_t whyn) {
  const char *p;
  char *copy = malloc(n + 1);
  if (copy == NULL) return 0;
  memcpy(copy, txt, n);
  copy[n] = 0;
  for (p = copy; (p = strstr(p, "return")) != NULL; p += 6) {
    const char *q = p + 6;
    if (p > copy && (isalnum((unsigned char)p[-1]) || p[-1] == '_')) continue;
    if (isalnum((unsigned char)*q) || *q == '_') continue;
    while (*q == ' ') q++;
    if (strncmp(q, "0;", 2) != 0 && strncmp(q, "1;", 2) != 0 && strncmp(q, "2;", 2) != 0) {
      snprintf(why, whyn, "a return that is not 0, 1 or 2");
      free(copy);
      return 0;
    }
  }
  free(copy);
  return 1;
}

int e_emit_inline_body(
    FILE *f, const Q2Func *fn, char hex[33], char *why, size_t whyn) {
  const JSAotGenFunc *g = fn->g;
  static const char *const alloc_ops[] = {
      "new_object",   "object_template", "array_const",  "array_from",
      "make_closure", "define_class",    "define_class_computed",
      "regexp",       "special_object",  "to_object",    "object_fill"};
  char *mb = NULL;
  size_t ml = 0;
  FILE *m;
  int i, rc, ncalls = 0;
  (void)0;

  /* QJS_AOT_NOINLINE=1 emits no inline body: the baseline of a comparison. */
  if (getenv("QJS_AOT_NOINLINE") != NULL &&
      getenv("QJS_AOT_NOINLINE")[0] == '1') {
    snprintf(why, whyn, "disabled");
    return 0;
  }
  if (!g->light_ok && !(g->func_kind == E_FUNC_NORMAL && g->ctl_count == 0)) {
    /* The engine's light rule: a plain function, no closure variables, no control
       records (try), no captured arguments, a light-safe opcode set. */
    int vr = 0, ap = 0;
    for (i = 0; i < fn->ninsns; i++) {
      char nm2[64];
      e_rop_name(fn->insns[i].rop, nm2, sizeof(nm2));
      if (strstr(nm2, "var_ref") != NULL || strcmp(nm2, "make_closure") == 0) vr = 1;
      if (strcmp(nm2, "apply_arguments") == 0) ap = 1;
    }
    snprintf(
        why, whyn, "not light: %s",
        g->func_kind != E_FUNC_NORMAL ? "generator or async"
        : g->ctl_count > 0             ? "try/catch"
        : vr                           ? "closure variables"
        : ap                           ? "apply_arguments"
                                       : "other (captured arguments or an op that is not light-safe)");
    return 0;
  }
  if (g->light_need > 16 * 1024) {
    snprintf(why, whyn, "frame over 16 KB");
    return 0;
  }
  if (g->light_var_count > 0) {
    snprintf(why, whyn, "variable area");
    return 0;
  }
  if (g->func_kind != E_FUNC_NORMAL) {
    snprintf(why, whyn, "not a plain function");
    return 0;
  }
  if (fn->ninsns > 64) {
    snprintf(why, whyn, "%d instructions", fn->ninsns);
    return 0;
  }
  for (i = 0; i < fn->ninsns; i++) {
    const Q2Insn *in = &fn->insns[i];
    char nm[64];
    size_t k;
    /* A construct is a call family op, and the allowance is consulted first: a
       construct the caller could lower itself (the engine helper, the constructor's
       entry, the result rule) is a call this body can make, not a refusal. Anything
       the allowance declines keeps the refusal the call-family test would give it. */
    if (in->rop == ROP_construct && e_inl_construct_allowed(fn, in)) {
      ncalls++; /* it runs the constructor's compiled entry, so it is a call */
      continue;
    }
    if (e_is_call_family(in->rop) && in->rop != ROP_call &&
        in->rop != ROP_call_method) {
      snprintf(why, whyn, "%s at pc %d", nm_of_rop(in->rop), in->pc);
      return 0;
    }
    if (in->rop == ROP_call || in->rop == ROP_call_method) {
      ncalls++;
      if (getenv("QJS_AOT_NONEST") != NULL &&
          e_intrinsic_site(g->digest, in->next) < 0) {
        snprintf(why, whyn, "nested calls disabled");
        return 0;
      }
    }
    e_rop_name(in->rop, nm, sizeof(nm));
    for (k = 0; k < sizeof(alloc_ops) / sizeof(alloc_ops[0]); k++)
      if (strcmp(nm, alloc_ops[k]) == 0) {
        snprintf(why, whyn, "allocates %s@%d", nm, in->pc);
        return 0;
      }
  }
  m = open_memstream(&mb, &ml);
  if (m == NULL) {
    snprintf(why, whyn, "out of memory");
    return 0;
  }
  if (ncalls > 6) {
    snprintf(why, whyn, "%d calls", ncalls);
    fclose(m);
    free(mb);
    return 0;
  }
  e_fl_loop_setup(fn);
  g_e_fl = 1;
  g_e_inl = 1;
  g_e_quiet = 1;
  g_e_inl_r = ncalls > 0;
  g_e_last_inl_tl = 0;
  g_e_inl_safepoint = 0;
  g_e_inl_pending = 0;
  g_e_spill_total = 0;
  g_e_child_max = 0;
  g_e_child_decl = 0;
  g_e_fl_block[0] = 0;
  if (g_e_inl_r) {
    char *tb = NULL;
    size_t tl = 0;
    FILE *tm = open_memstream(&tb, &tl);
    if (tm != NULL) {
      rc = e_emit_spec_body_pass(tm, fn, hex);
      fclose(tm);
      free(tb);
      g_e_spill_total = g_e_spill_n;
      g_e_child_decl = g_e_child_max;
      g_e_child_max = 0;
      g_e_fl_block[0] = 0;
    }
  }
  rc = e_emit_spec_body_pass(m, fn, hex);
  g_e_inl_r = 0;
  fclose(m);
  g_e_fl = 0;
  g_e_inl = 0;
  g_e_quiet = 0;
  if (rc != 0 || g_e_fl_block[0] != 0) {
    snprintf(why, whyn, "%s", g_e_fl_block[0] ? g_e_fl_block : "no analysis");
    free(mb);
    return 0;
  }
  if (!e_inl_text_ok(mb, ml, why, whyn)) {
    free(mb);
    return 0;
  }
  g_e_last_inl_nx = g_e_inl_nx_used;
  g_e_last_inl_leaf = !g_e_inl_safepoint;
  fwrite(mb, 1, ml, f);
  free(mb);
  return 1;
}

/* Whether the inline body just emitted exits through aot_nx_. */
int e_inline_used_nx(void) { return g_e_last_inl_nx; }

/* Whether the inline body just emitted makes no call and no construct. */
int e_inline_leaf(void) { return g_e_last_inl_leaf; }

/* The slots the inline body just emitted needs in the shared root array: its own, its window
   and the largest slice of a body it inlines; 0 for a body that makes no call. */
int e_inline_rz(void) { return g_e_last_inl_rz; }
int e_inline_thislay(void) { return g_e_last_inl_tl; }

/* Whether the inline body just emitted calls a function whose inline form is not decided. */
int e_inline_pending(void) { return g_e_inl_pending; }

static const char *nm_of_rop(uint16_t rop) {
  static char buf[64];
  e_rop_name(rop, buf, sizeof(buf));
  return buf;
}
