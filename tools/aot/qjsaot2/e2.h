/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * The E2 emitter's interface. `e2_emit_body` is the entry the unit builder
 * calls for each function; while the new emitter is being built it delegates
 * every function to the old one, so the harness, the report and the gates can
 * be exercised before a single instruction is emitted differently.
 */
#ifndef QJSAOT2_E2_H
#define QJSAOT2_E2_H

#include <stdio.h>

#include "quickjs-aot-gen.h"
#include "quickjs-regvm.h"

/* Emit one function's body in place of the old emitter's. Returns 0 on
   success, -1 on a generation error. */
int e2_emit_body(FILE *f, const JSAotGenFunc *g, char hex[33]);

/* The old emitter, unchanged. */
int q2_old_emit_body(FILE *f, const JSAotGenFunc *g, char hex[33]);

/* Called once per function by the unit builder: routes the function to the E2
   emitter when `--emit2` selects it, else to the old one. */
int e2_emit_body_if_on(FILE *f, const JSAotGenFunc *g, char hex[33]);
/* The inline form `e2i_HEX` of a small leaf (card E15), or 0 with `why`. */
int e2_emit_inline_func(
    FILE *f, const JSAotGenFunc *g, char hex[33], char *why, size_t whyn);
struct Q2Func;
int e2_emit_inline_body(
    FILE *f, const struct Q2Func *fn, char hex[33], char *why, size_t whyn);
int e2_inline_used_nx(void);
void e2_report_inline(const char *hex, int ok, const char *why);

/* The generic body (D2), the speculative body (D3) and the native entry. The
   generic body is emitted first, and the speculative one forwards to it until
   it has a body of its own. */
struct Q2Func;
int e2_emit_generic_body(FILE *f, const struct Q2Func *fn, char hex[33]);
int e2_emit_spec_body(FILE *f, const struct Q2Func *fn, char hex[33]);
/* The framed native entry, for a function a compiled caller may enter on the C
   stack: it builds the light frame and runs the speculative body on it. */
void e2_emit_native_entry(FILE *f, const struct Q2Func *fn, char hex[33]);
/* Whether the function contains a call site, so its entry polls (rule P) and
   checks the stack (rule K). */
int e2_has_call_site(const struct Q2Func *fn);
void e2_emit_frameless_exit_fn(FILE *f, const struct Q2Func *fn, char hex[33]);
void e2_emit_frameless_forward(FILE *f, const struct Q2Func *fn, char hex[33]);
int e2_fl_was_chosen(void);

/* The builtins a unit lowers inline (e2_intrinsics.c). */
void e2_report_intrinsic(const char *hex, int pc, int id, int argc, int ok);
const char *e2_intrinsic_name(int id);
int e2_intrinsic_id_for_key(const char *key);
int e2_intrinsic_site(const uint8_t digest[16], int pc);
int e2_intrinsic_argc_ok(int id, int argc);
int e2_intrinsic_pure(int id);
int e2_intrinsic_magic(int id);
struct Q2Func;
struct Q2Insn;
int e2_intrinsic_lowerable(const struct Q2Func *fn, const struct Q2Insn *in);
int qjs_aot_prof_callees(
    const uint8_t dg[16], int pc, int *n, char hex[4][33], int kind[4],
    unsigned long long hits[4]);
const char *e2_fl_why_not(void);

/* The registers live immediately BEFORE an instruction: live-out minus the
   instruction's own definitions, plus its uses and window. Use this when the exit
   is AT an instruction's pc, because that instruction has not run yet. */
void q2_live_before(const struct Q2Func *fn, int insn, uint64_t *out);

/* The registers live immediately AFTER an instruction (build.c stores them per
   instruction). Use this when the exit is at end_pc, after the last instruction
   ran: live-before would drop whatever that instruction defines. */
const uint64_t *q2_live_out(const struct Q2Func *fn, int insn);

/* Functions refused for the speculative body as irreducible. */
extern int e2_irreducible;

/* One more than the largest PIC site index the function uses: the stride between
   one site's caching ways in the PIC table. */
int e2_pic_sites(const struct Q2Func *fn);

/* `e2_undefined_value`: the `undefined` word the entry fills the frame with,
   defined once in the unit prelude. */

/* --- options, set by the unit builder's command line ------------------- */

/* --emit2: route every function through `e2_emit_body`. */
extern int e2_on;
/* --emit2-only HEX: restrict `--emit2` to this function, so one function can
   be switched to the new emitter while the rest stay on the old one. Empty
   means every function. */
extern char e2_only_hex[33];

int qjs_aot_emit2_on(void);

/* --- the role table (D12) --------------------------------------------- */

/* How an op is lowered. Every op starts as an op call; later cards promote
   them by measured weight. */
#define E2_OPCALL 0
#define E2_HELPER 1
#define E2_NATIVE 2

/* What an op does to control flow. */
#define E2_CF_NONE 0
#define E2_CF_BRANCH 1
#define E2_CF_RETURN 2
#define E2_CF_THROW 3
#define E2_CF_SUSPEND 4
#define E2_CF_TRY 5

/* The D6 location an op reads or writes, or 0. The generator uses this to
   decide what kills a cached value, so an op that touches memory must not say
   0: a wrong 0 makes the cache stale, which is a miscompile. */
#define E2_MEM_NONE 0
#define E2_MEM_ARG 1
#define E2_MEM_LOCAL 2
#define E2_MEM_VARREF 3
#define E2_MEM_FIELD 4
#define E2_MEM_ELEMENT 5
#define E2_MEM_GLOBAL 6
#define E2_MEM_THIS 7

typedef struct E2Role {
  uint16_t rop;
  const char *fmt;      /* the engine's operand format */
  int8_t ndef, def[2];  /* destination operand indices */
  /* D12 says use[4]. `iterator_next` has seven register operands
     ("rrrrrrrr"), so four is too small; the table is generated and the struct
     is internal, so it holds the widest op the engine actually has. */
  int8_t nuse, use[8];  /* source operand indices */
  int8_t window;        /* operand index of a call's argument count, or -1 */
  uint8_t tier;
  uint8_t can_throw, runs_js, allocates;
  uint8_t mem;
  uint8_t control;
  uint8_t writes_regs;  /* writes registers beyond def[] */
  uint8_t pure;         /* on D12's allow-list */
} E2Role;

/* One entry per ROP_*, indexed by the opcode. NULL means the table has no
   entry for that opcode, which `e2_roles_check` treats as a hard error. */
extern const E2Role *const e2_roles[ROP_COUNT];

/* Abort at generator start when an entry is missing or disagrees with the
   engine's format. */
void e2_roles_check(void);

/* The opcode's name, for diagnostics. */
void e2_rop_name(uint16_t rop, char *out, size_t n);

/* Print the hand-off histogram (which rop a spec block gave up at, weighted
   by the profile) to the generator report. */
void e2_handoff_report(FILE *f);

/* --emit2-poison: generate the units with D12's dynamic register check. The
   unit must also be compiled with -DE2_POISON, which the poison run does. */
void e2_set_poison(int on);

/* --emit2-nocall: omit the native call sites, so the call-cost benchmark can
   measure the entry without them. A/B switch, not a shipping mode. */
void e2_set_nocall(int on);

/* Split units: compiled functions get hidden external linkage, and the caller
   writes one .c per function. */
void e2_set_split(int on);
int e2_split_on(void);
const char *e2_linkage(void);
int e2_nocall_on(void);

/* --- the report ------------------------------------------------------- */

/* --emit2-report FILE: open it once, write one line per function. */
void e2_report_open(const char *path);
void e2_report_close(void);
/* `weight` is the function's share of the profile's executed ops, in percent;
   `delegated` says the old emitter produced the body in the end. */
void e2_report_func(const JSAotGenFunc *g, const char *hex, double weight,
                    int delegated);

/* One instruction the unit leaves as an op call: its opcode and the profile's
   execution count there. `missarm` marks a fast path's miss arm, which shares
   the instruction's pc with the fast path and is counted separately so a
   lowered family does not hide what it still costs. */
void e2_report_opcall(uint16_t rop, double weight, int missarm);

/* The entry-kind line the report carries per function: which entry the
   function got, and which of rules P and K its entry emits. A function with
   `light_ok` 0 gets no native entry at all and no line. */
void e2_report_entry(const JSAotGenFunc *g, const char *hex, int frameless,
                     int poll, int stack_check);

/* The spec body's four report lines (step 8), keyed by digest: `values` by
   representation (i32, f64, bool, tag); `tiers`, instructions lowered typed,
   through an op-call wrapper, left to a hand-off, and other (fused into a
   neighbour, or in a block the body never reaches); `checks`, guard sites by kind
   (ax, vx, dx, ox) and exits (deopt, hand-off); `code`, the body's size in lines
   and bytes (-1 when unknown). */
void e2_report_spec(
    const char *hex, const int vals[4], const int tiers[4], const int checks[9],
    long lines, long bytes);

/* --- the unit builder's native-entry registry --------------------------- */

/* Tell the unit builder that the function at unit index `i` got a native entry,
   so a call site elsewhere may name its `aot_n_`. A unit built with
   `--emit2-only` emits the other functions with the old emitter, which emits
   no native entry, and a call site that named one would not link. */
void qjs_aot_note_native_entry(int index);
/* The unit index of the function being emitted. */
int qjs_aot_cur_func_idx(void);

#endif /* QJSAOT2_E2_H */
