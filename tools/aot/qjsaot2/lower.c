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
 *     `qjs_aot_deopt(ctx, fr, pc, site)` before the instruction, so the
 *     interpreter re-executes it there;
 *   - an operation the classifier does not know runs through the engine's step
 *     routine.
 *
 * Native state is canonical (R1/R2 in `.local/STEP5-REPORT.md` §10.4). Every
 * SSA value lives in a C local; the register slot is written only at a
 * safepoint -- a call, an allocation, a slow path that can allocate, free or
 * run user code, the poll, a deopt leave, a return, and the exception exit --
 * and never on a pure fast path. So:
 *
 *   - the frame is current exactly where something can observe it;
 *   - `fr` is the GC's root set under DRC at every such point, because the
 *     write-back is what puts a live value there;
 *   - a guard miss writes back before it leaves, which is why the miss path
 *     costs more than the hit path;
 *   - what would break it: a helper that can allocate or free but is not
 *     marked as a safepoint. `q2_nosafepoint` is an allow-list and the default
 *     is safepoint, so a missing entry costs speed, never correctness.
 */
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

#include "e2.h"
#include "ir.h"
#include "quickjs-aot-gen.h"
#include "prof.h"
#include "quickjs-regvm.h"

/* The driver's id spaces and sweep flag (qjsaot.c). */
int qjs_aot_sweep_on(void);
int qjs_aot_site_alloc(void);
int qjs_aot_site_new(void);
int qjs_aot_check_locals(void);
/* The dominant profiled call at `pc` of the function with this digest: 1 and
   *kind (1 bytecode, 2 C), *cproto and the callee digest hex, or 0. */
int qjs_aot_prof_call(const uint8_t dg[16], int pc, int *kind, int *cproto,
                      char hex[33]);
/* The shapes one property site saw (its `S` record): 1 and up to four shapes,
   or 0 when the site has no record. Values are profile addresses. */
const ProfPc *qjs_aot_prof_pcs(const uint8_t dg[16], int *n);
int qjs_aot_prof_site(const uint8_t dg[16], int site, int *nshapes,
                      uint64_t shapes[4]);
/* Whether the profiler never executed the instruction at `pc`. */
int qjs_aot_prof_unexecuted(const uint8_t dg[16], int pc);
/* Every callee one call site saw, up to four, hottest first: the count (0
   when there is no record) and parallel kind/digest/hits arrays. */
int qjs_aot_prof_callees(const uint8_t dg[16], int pc, int *n, char hex[4][33],
                         int kind[4], unsigned long long hits[4]);
/* Whether the digest's body is emitted in this unit, so a call to it can be a
   direct call rather than a fresh interpreter activation. */
int qjs_aot_unit_has(const char hex[33]);
int qjs_aot_body_over_budget(int ninsns);
/* The argument count of a unit function with a leaf body, else -1. */
int qjs_aot_leaf_argc(const char hex[33]);
/* The callee's light-frame size when a compiled caller may enter it on the C
   stack, else 0. */
int qjs_aot_light_need(const char hex[33]);
/* The function's numeric constant-pool entry `idx`: 1 with *out when the pool
   entry is an int or a float64, so `binary_cpool` on it can be a C literal. */
int qjs_aot_pool_num(const struct JSAotGenFunc *g, uint32_t idx, double *out);

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
    case ROP_inc_branch_lt:
    case ROP_inc_branch_lt_s:
    case ROP_add_imm_self_branch:
    case ROP_add_imm_self_branch_s:
    case ROP_return:
    case ROP_return_undefined:
    case ROP_get_this_field:
    case ROP_get_this_field2:
    case ROP_binary_cpool:
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
/* One out-of-line exit: the write-back it performs, then the deopt. The store
   list is fixed when the leave is emitted, because the live set and the frame
   cache are only valid then. */
typedef struct Q2Exit {
  int pc, sid;
  int nst;
  int *st_reg;
  Q2ValueId *st_val;
  /* The --check-locals assertion, run in the stub after the stores. */
  int nchk;
  int *chk_reg;
  Q2ValueId *chk_val;
} Q2Exit;

/* One out-of-line slow arm: the code a fast path jumps to when it misses and
   then continues from. The arm's text is collected in a memory stream, because
   it is printed after the body: the hot path keeps only the branch. */
typedef struct Q2Cold {
  FILE *m; /* the arm's stream while it is being built */
  char *buf;
  size_t len;
} Q2Cold;

typedef struct Q2Emit {
  const Q2Func *fn;
  Q2Cur cur; /* the value current after the instruction being emitted */
  Q2Cur pre; /* the value current before it, for a pre-instruction write-back */
  uint64_t *liveb; /* scratch: the live-before set */
  /* The value each register's slot is known to hold, so a store of a value
     already there is dropped. Valid only within a region with no branch and no
     safepoint: both are reset points. */
  Q2ValueId *fb;
  int cur_block;
  int cur_insn;
  /* The loop copy being emitted (see q2_emit_fast_loop): field accesses read
     and write the slot pointers its preheader resolved, and the back edge
     returns to the copy. */
  int fast, fast_lo, fast_hi; /* the copy being emitted, and its blocks */
  int gen_loop; /* the generic copy of a loop that has a fast copy */
  /* The leaf body (qjsaot2_emit_pure): no frame, so a guard that fails
     returns 0 and the caller makes the call it replaced. */
  int pure;
  uint8_t *fhoist; /* per instruction: the read is loop-invariant */
  /* Per instruction: the write group a read or write of the loop copy belongs
     to (the first write of the same receiver and property), else -1; and per
     group, whether every write stores a typed number, so a read of it takes the
     double the last write stored. */
  int32_t *fgroup;
  uint8_t *fdbl;
  /* The out-of-line exit stubs: every leave in the body appends one, and they
     are printed once after the body. A stub repeats the store list its
     write-back would have emitted, so the hot path is only a `goto`. */
  Q2Exit *exits;
  int nexits, capexits;
  /* The out-of-line slow arms, printed after the exits. `cold_buf` is the frame
     cache saved for the arm: within an arm the stores are printed as they would
     have been at the arm's start, so collect them before switching streams. */
  Q2Cold *colds;
  int ncolds, capcolds;
  /* The specialised emission (T1.4+): misses exit instead of taking a slow
     arm, and the facts the profile established are available. */
  int spec;
  const char *hex; /* the function's digest, for its per-function statics */
  /* Shape groups of the specialised body: per site, the group it belongs to
     (-1 when it has no monomorphic profile shape), and per group the
     representative shape pointer from the profile. Two sites share a group when
     the profile saw one shape at each of them, which is the emitter's claim
     that one shape check can stand for both. */
  int32_t *sgroup; /* nsites entries */
  size_t nsites;   /* the length of sgroup/sexec */
  int ngroups;
  int *gfirst; /* ngroups entries: the first site of each group */
  /* Shape facts: pair -> (receiver value, group), and the per-block bitsets. */
  int nfacts, fact_words;
  int32_t *fact_val, *fact_grp;
  uint64_t *fact_in, *fact_out, *fact_scratch;
  uint64_t *fact_cur; /* the facts true at the instruction being emitted */
  /* Per value: the polymorphic way its shape matched in this block, or -1.
     A later poly site on the same receiver tries that way first, which is the
     "one shape check per object" the monomorphic groups get, for the
     polymorphic case. Cleared at a block start and by a call. */
  int8_t *poly_way;
  /* While a frame store or an exit stub is being printed. A value that reaches
     the frame must be canonical, because the interpreter and the engine's
     helpers read frame slots; a value that stays in a C local need not be. */
  int in_wb;
  int next_poly; /* a unique number for each polymorphic compare chain */
  /* Per site, whether the site's pc was ever executed; an unexecuted site must
     not be validated at entry. */
  uint8_t *sexec;
  /* While an arm is being emitted: the stream it writes to, and the number of
     arms already finished (its label is that index). */
  FILE *cold_stream;
  int cold_active;
  Q2ValueId *cold_seen; /* the arm's private frame cache */
} Q2Emit;

/* One emitter runs at a time, so the state is a file static rather than a
   parameter on every native case. */
static Q2Emit g_em;

/* A leaf whose field reads are all of its `this` or its arguments has a
   second body, `aot_r_<digest>`, that reads them through slots its caller
   resolved: the receiver (-1 `this`, else the argument) and the callee's cache
   site of each, in order. */
#define Q2_LEAF_SITES 16
#define Q2_MAX_LEAVES 4096
typedef struct Q2Leaf {
  char hex[33];
  int used, bad, n;
  int8_t recv[Q2_LEAF_SITES];
  int32_t site[Q2_LEAF_SITES];
} Q2Leaf;
static Q2Leaf g_leafrec;
static Q2Leaf g_leaves[Q2_MAX_LEAVES];
static int g_nleaves;

static void q2_leaf_slot_read(FILE *f, const Q2Func *fn, const Q2Insn *in,
                              Q2ValueId recv, int site, Q2ValueId d);

static const Q2Leaf *q2_leaf_find(const char hex[33]) {
  int i;
  for (i = 0; i < g_nleaves; i++)
    if (!memcmp(g_leaves[i].hex, hex, 32)) return &g_leaves[i];
  return NULL;
}

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
    else if (v->rep == Q2_R_F64) {
      /* A specialised body's hot path boxes with the float64 constructor: the
         value stays in a C local, and no fitsi is needed to pick a tag. A frame
         store, an exit stub and the generic body keep the canonical form,
         because the interpreter and the engine's helpers read frame slots. */
      if (g_em.spec && !g_em.in_wb)
        snprintf(buf, n, "__JS_NewFloat64(d%u)", vv);
      else
        snprintf(buf, n, "qjs_aot_box_num(d%u, qjs_aot_fitsi(d%u))", vv, vv);
    }
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

/* The ROPs whose lowering runs no code that can allocate, free or observe the
   frame: an inline cache hit, an arithmetic fast path, a compare, a branch.
   Everything else is a safepoint by default (see R1 in the step-5 report),
   which is why a missing entry costs speed and never correctness. */
static int q2_nosafepoint(uint16_t rop) {
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
    case ROP_get_local:
    case ROP_get_arg:
    case ROP_get_arg2:
    case ROP_branch:
    case ROP_branch_s:
    case ROP_branch_if:
    case ROP_branch_if_not:
    case ROP_branch_null:
    case ROP_branch_not_null:
    case ROP_branch_nullish:
    case ROP_branch_not_nullish:
    case ROP_branch_cmp_reg_reg:
    case ROP_branch_cmp_reg_reg_s:
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
    case ROP_inc_branch_lt:
    case ROP_inc_branch_lt_s:
    case ROP_add_imm_self_branch:
    case ROP_add_imm_self_branch_s:
    case ROP_get_field:
    case ROP_get_field_own:
    case ROP_get_field2:
    case ROP_get_field2_own:
    case ROP_get_field_chain:
    case ROP_get_field_chain_own:
    case ROP_get_this_field:
    case ROP_get_this_field2:
    case ROP_put_field:
    case ROP_load_this:
      return 1;
    default:
      return 0;
  }
}

/* Store every value live in `bits` back to its register, boxed. `cur` is the
   value current in each register at this point. */
/* Forget what the frame slots hold. Called at every branch target and after
   every safepoint: from then on the slot's value is not something this
   straight-line region put there. */
static void q2_fb_reset(const Q2Func *fn) {
  int i;
  if (g_em.fb == NULL) return;
  for (i = 0; i < fn->nregs; i++) g_em.fb[i] = Q2_NO_VALUE;
}

/* Store one value to its slot, unless the slot already holds that same value
   and nothing since put it there. `mark` records the store in the cache, which
   is right only for a store every later instruction runs after: a store on a
   slow-path arm or a leave must not, or the fast path that skipped the arm
   would skip the store too. */
static void q2_store_slot(FILE *f, const Q2Func *fn, Q2ValueId v, int mark) {
  char e[64];
  int r = fn->values[v].reg;
  if (g_em.fb != NULL && g_em.fb[r] == v) return;
  q2_use(fn, v, Q2_R_TAGGED, e, sizeof(e));
  fprintf(f, "    AOT_SET_B(ctx, fr, %d, %s); /* wb v%u */\n",
          fn->values[v].reg * 16, e, v);
  if (mark && g_em.fb != NULL) g_em.fb[r] = v;
}

/* Whether a leave's write-back stores `v`, under the same rules q2_store_slot
   applies: a value already in its slot is skipped. `seen` is the frame cache as
   the leave sees it, so a value stored earlier in the same write-back is not
   stored twice. */
static int q2_wb_wants(const Q2Func *fn, Q2ValueId v, Q2ValueId *seen) {
  int r;
  if (v == Q2_NO_VALUE || v >= (Q2ValueId)fn->nvalues) return 0;
  r = fn->values[v].reg;
  if (r >= fn->nregs) return 0;
  return seen[r] != v;
}

/* Collect the exact store list a leave's write-back would emit, for an
   out-of-line exit stub: it must not depend on the frame cache or the live set
   still being what they were where the leave was. `seen` starts as the frame
   cache copy and is updated as pairs are collected, exactly like the inline
   write-back. */
static void q2_writeback_collect(
    const Q2Func *fn, const Q2Cur *c, const uint64_t *bits, Q2ValueId *seen,
    int *nst, int **st_reg, Q2ValueId **st_val) {
  int w, words = fn->lwords, n = 0, cap = 0;
  int *regs = NULL;
  Q2ValueId *vals = NULL;
  for (w = 0; w < words; w++) {
    uint64_t m = bits[w];
    while (m) {
      int r = w * 64 + __builtin_ctzll(m);
      Q2ValueId v;
      m &= m - 1;
      if (r >= fn->nregs) continue;
      v = c->cur[r];
      if (!q2_wb_wants(fn, v, seen)) continue;
      if (n == cap) {
        int nc = cap ? cap * 2 : 8;
        int *nr = realloc(regs, sizeof(*nr) * (size_t)nc);
        Q2ValueId *nv;
        if (!nr) break;
        regs = nr;
        nv = realloc(vals, sizeof(*nv) * (size_t)nc);
        if (!nv) break;
        vals = nv;
        cap = nc;
      }
      regs[n] = r;
      vals[n] = v;
      seen[r] = v;
      n++;
    }
  }
  *nst = n;
  *st_reg = regs;
  *st_val = vals;
}

static void q2_writeback_kind(FILE *f, const Q2Func *fn, const Q2Cur *c,
                              const uint64_t *bits, int all) {
  (void)all;
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
      q2_store_slot(f, fn, v, 0);
    }
  }
}

/* The write-back before a safepoint (or a leave): every live value. */
static void q2_writeback(
    FILE *f, const Q2Func *fn, const Q2Cur *c, const uint64_t *bits) {
  q2_writeback_kind(f, fn, c, bits, 1);
}

/* The write-back before a call. A callee reads the caller's argument window
   and the helper reads the callee and receiver registers, so those are stored
   whatever their representation; plus every tagged value, which may be a heap
   reference the collector must see. An unboxed number elsewhere is not a root
   and is not boxed just to make a call. `need` names the registers the call
   itself reads (callee, receiver, window) as a bit set. */
static void q2_writeback_call(FILE *f, const Q2Func *fn, const Q2Cur *c,
                             const uint64_t *bits, const uint64_t *need) {
  int w, words = fn->lwords;
  for (w = 0; w < words; w++) {
    uint64_t m = bits[w];
    while (m) {
      int r = w * 64 + __builtin_ctzll(m);
      Q2ValueId v;
      int forced;
      m &= m - 1;
      if (r >= fn->nregs) continue;
      v = c->cur[r];
      if (v == Q2_NO_VALUE || v >= (Q2ValueId)fn->nvalues) continue;
      forced = (need[w] >> (r & 63)) & 1;
      if (q2_typed(&fn->values[v]) && !forced) continue;
      /* A register the call itself reads must be in its slot whatever the
         cache says: the caller wrote it with the owning store, which the cache
         does not track. */
      if (forced) {
        char e[64];
        q2_use(fn, v, Q2_R_TAGGED, e, sizeof(e));
        fprintf(f, "    AOT_SET_B(ctx, fr, %d, %s); /* wb v%u */\n",
                fn->values[v].reg * 16, e, v);
        if (g_em.fb) g_em.fb[r] = v;
        continue;
      }
      q2_store_slot(f, fn, v, 1);
    }
  }
}

/* The registers a call instruction reads on its own: the callee, the receiver,
   and the argument window. */
static void q2_call_need_bits(
    const Q2Func *fn, const Q2Insn *in, uint64_t *need) {
  int words = fn->lwords, k;
  int is_m = (in->rop == ROP_call_method);
  memset(need, 0, (size_t)words * 8);
  {
    int c = (int)in->arg[1].val >> 4;
    if (c >= 0 && c < fn->nregs) need[c >> 6] |= 1ull << (c & 63);
  }
  if (is_m) {
    int r = (int)in->arg[2].val >> 4;
    if (r >= 0 && r < fn->nregs) need[r >> 6] |= 1ull << (r & 63);
  }
  for (k = 0; k < in->win_n; k++) {
    int r = in->win_reg + k;
    if (r < fn->nregs) need[r >> 6] |= 1ull << (r & 63);
  }
}

/* The debug check (--check-locals): at a leave, every live value's boxed form
   must equal its frame slot bitwise. A mismatch is a missing write-back or a
   stale local. It covers the call window too, which `insn_live` alone would
   miss. Collected with the leave's stores so the check runs in the stub, after
   the write-back, where the values are read from the same locals. */
static void q2_check_collect(
    const Q2Func *fn, const Q2Cur *c, const uint64_t *bits, int *nchk,
    int **chk_reg, Q2ValueId **chk_val) {
  int w, words = fn->lwords, n = 0, cap = 0;
  int *regs = NULL;
  Q2ValueId *vals = NULL;
  for (w = 0; w < words; w++) {
    uint64_t m = bits[w];
    while (m) {
      int r = w * 64 + __builtin_ctzll(m);
      Q2ValueId v;
      m &= m - 1;
      if (r >= fn->nregs) continue;
      v = c->cur[r];
      if (v == Q2_NO_VALUE || v >= (Q2ValueId)fn->nvalues) continue;
      if (fn->values[v].synced) continue;
      if (n == cap) {
        int nc = cap ? cap * 2 : 8;
        int *nr = realloc(regs, sizeof(*nr) * (size_t)nc);
        Q2ValueId *nv;
        if (!nr) break;
        regs = nr;
        nv = realloc(vals, sizeof(*nv) * (size_t)nc);
        if (!nv) break;
        vals = nv;
        cap = nc;
      }
      regs[n] = r;
      vals[n] = v;
      n++;
    }
  }
  *nchk = n;
  *chk_reg = regs;
  *chk_val = vals;
}

/* A leave: write back the live values in `bits`, then deopt at
   `pc`. `cur` may be NULL when no write-back is needed. The stores go to an
   out-of-line stub, so the hot path is `goto x_N`; the stub is printed once,
   after the body, by q2_emit_exits. */
static void q2_emit_leave(
    FILE *f, const Q2Func *fn, const Q2Cur *c, const uint64_t *bits, int pc,
    int sid) {
  Q2Exit *e;
  if (g_em.pure) {
    /* Nothing has happened yet: the caller makes the call instead. */
    fprintf(f, "    { return 0; }\n");
    return;
  }
  if (g_em.nexits == g_em.capexits) {
    int nc = g_em.capexits ? g_em.capexits * 2 : 16;
    Q2Exit *ne = realloc(g_em.exits, sizeof(*ne) * (size_t)nc);
    if (!ne) {
      /* Out of memory is not a reason to emit a wrong body: fall back to the
         inline form. */
      fprintf(f, "    {\n");
      if (c && bits) q2_writeback(f, fn, c, bits);
      fprintf(f, "    return qjs_aot_deopt(ctx, f, fr, %d, %d); }\n", pc, sid);
      return;
    }
    g_em.exits = ne;
    g_em.capexits = nc;
  }
  e = &g_em.exits[g_em.nexits];
  e->pc = pc;
  e->sid = sid;
  e->nst = 0;
  e->st_reg = NULL;
  e->st_val = NULL;
  e->nchk = 0;
  e->chk_reg = NULL;
  e->chk_val = NULL;
  if (c && bits) {
    Q2ValueId *seen =
        malloc(sizeof(Q2ValueId) * (size_t)(fn->nregs > 0 ? fn->nregs : 1));
    if (seen) {
      /* The frame cache as the leave sees it: a value already there is not
         stored, exactly as the inline write-back decided. */
      memcpy(seen, g_em.fb, sizeof(Q2ValueId) * (size_t)fn->nregs);
      q2_writeback_collect(
          fn, c, bits, seen, &e->nst, &e->st_reg, &e->st_val);
      free(seen);
    }
    if (qjs_aot_check_locals())
      q2_check_collect(fn, c, bits, &e->nchk, &e->chk_reg, &e->chk_val);
  }
  fprintf(f, "    { goto x_%d; }\n", g_em.nexits);
  g_em.nexits++;
}

/* The stubs q2_emit_leave collected, printed after the body. */
static void q2_emit_exits(FILE *f, const Q2Func *fn) {
  int i, k;
  for (i = 0; i < g_em.nexits; i++) {
    Q2Exit *e = &g_em.exits[i];
    fprintf(f, "x_%d: {\n", i);
    for (k = 0; k < e->nst; k++) {
      char buf[64];
      Q2ValueId v = e->st_val[k];
      q2_use(fn, v, Q2_R_TAGGED, buf, sizeof(buf));
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, %s);\n", e->st_reg[k] * 16, buf);
    }
    for (k = 0; k < e->nchk; k++) {
      char buf[64];
      Q2ValueId v = e->chk_val[k];
      q2_use(fn, v, Q2_R_TAGGED, buf, sizeof(buf));
      /* Compared by kind, not with memcmp: an int or a special value
         initialises only the union's int32, so the rest of its payload bytes
         are unspecified and two equal values can differ there. */
      fprintf(
          f,
          "    { JSValue _c = %s, _r = AOT_REG(fr, %d); int _t = JS_VALUE_GET_TAG(_c), _bad;\n"
          "      if (_t != JS_VALUE_GET_TAG(_r)) _bad = 1;\n"
          "      else if (_t < 0) _bad = JS_VALUE_GET_PTR(_c) != JS_VALUE_GET_PTR(_r);\n"
          "      else if (_t == JS_TAG_FLOAT64) { double _x = JS_VALUE_GET_FLOAT64(_c), _y = JS_VALUE_GET_FLOAT64(_r); _bad = memcmp(&_x, &_y, sizeof(double)) != 0; }\n"
          "      else _bad = JS_VALUE_GET_INT(_c) != JS_VALUE_GET_INT(_r);\n"
          "      if (_bad) { fprintf(stderr, \"check-locals: pc %%d r %%d v%u\\n\", __LINE__, %d); abort(); } }\n",
          buf, e->chk_reg[k] * 16, v, e->chk_reg[k]);
    }
    /* A specialised body that keeps leaving is slower than the generic one,
       so after a burst of exits it switches itself off. The generic body has
       no counter: its misses continue in compiled code. */
    if (g_em.spec)
      fprintf(
          f,
          "    qjs_aot_spec_exits++;\n"
          "    if (++q2_spec_fail_%s > Q2_SPEC_FAIL_MAX) { q2_spec_off_%s = "
          "1; q2_spec_fail_%s = 0; }\n",
          g_em.hex, g_em.hex, g_em.hex);
    fprintf(
        f, "    return qjs_aot_deopt(ctx, f, fr, %d, %d);\n    }\n", e->pc,
        e->sid);
  }
}

/* The arm's stream is closed by q2_cold_end; a failure here only costs the
   out-of-line form, never correctness. */
static FILE *q2_cold_begin(FILE *f, const char *cond, int wb, int *k) {
  FILE *m;
  Q2Cold *c;
  /* A leaf body has no labels and no continuation to return to: a failing
     guard there returns 0 and its caller makes the call. So it never takes an
     out-of-line arm; the caller keeps its inline form. */
  if (g_em.pure) return NULL;
  if (g_em.ncolds == g_em.capcolds) {
    int nc = g_em.capcolds ? g_em.capcolds * 2 : 16;
    Q2Cold *nn = realloc(g_em.colds, sizeof(*nn) * (size_t)nc);
    if (!nn) return NULL;
    g_em.colds = nn;
    g_em.capcolds = nc;
  }
  *k = g_em.ncolds;
  c = &g_em.colds[*k];
  c->m = open_memstream(&c->buf, &c->len);
  if (!c->m) return NULL;
  g_em.ncolds++;
  /* The braced branch: a brace-less `if (cond)` would make only the `goto`
     conditional and let the hot path fall into the label. */
  fprintf(f, "    if (__builtin_expect(!(%s), 0)) { goto s_%d; }\n", cond, *k);
  fprintf(f, " r_%d:;\n", *k);
  if (wb) q2_writeback(c->m, g_em.fn, &g_em.pre, g_em.liveb);
  return c->m;
}

/* Finish an arm: return to the hot path's continuation label. */
static void q2_cold_end(int k) {
  Q2Cold *c;
  if (k < 0 || k >= g_em.ncolds) return;
  c = &g_em.colds[k];
  if (!c->m) return;
  fprintf(c->m, "    { goto r_%d; }\n", k);
  fclose(c->m);
  c->m = NULL;
}

/* The arms, printed after the exits so they sit out of the hot layout. */
static void q2_emit_colds(FILE *f) {
  int i;
  for (i = 0; i < g_em.ncolds; i++) {
    Q2Cold *c = &g_em.colds[i];
    /* A label is always printed: an arm whose stream was lost must still give
       its branch somewhere to land, and an empty arm re-joins the hot path at
       its `r_` label, which is exactly what the inline form did. */
    if (!c->buf) {
      fprintf(f, "s_%d: {\n    { goto r_%d; }\n    }\n", i, i);
      continue;
    }
    fprintf(f, "s_%d: {\n%s    }\n", i, c->buf);
  }
}

static void q2_miss(FILE *f, int off, int sid);
static int q2_poly_site(const Q2Func *fn, int site);

/* A field read that misses: fill the site's cache and continue compiled, out of
   line. `cond` is the hit test the caller already printed; the arm repeats the
   pre-instruction write-back, runs the engine's read (which may run a getter
   and throw) and adopts the owned result. If the stream cannot be opened the
   arm is emitted inline, which is slower but identical. */
static void q2_cold_read(
    FILE *f, const Q2Func *fn, const Q2Insn *in, const char *cond, int atom_off,
    int site, const char *recv, Q2ValueId defv, int off) {
  int k;
  FILE *m;
  /* The specialised body has no slow reads: a miss is an exit to the
     interpreter, which fills the cache and resumes. That is the whole point of
     the second body -- the generic one keeps the cache-filling arm so it never
     has to leave. */
  if (g_em.spec) {
    /* Nothing has happened yet, so the exit resumes at this instruction and
       the interpreter does the read itself. */
    fprintf(f, "    if (__builtin_expect(!(%s), 0)) {\n", cond);
    q2_miss(f, in->pc, q2_site());
    fprintf(f, "    }\n");
    return;
  }
  m = q2_cold_begin(f, cond, 1, &k);
  if (m) {
    fprintf(
        m,
        "      if (qjs_aot_read_field_slow(ctx, f, code + %d, %s, "
        "(int)qjs_aot_ld_u32(code + %d), %d, &v%u) < 0) { pc = %d; goto "
        "exception; }\n"
        "      pic = q2_pic(f); holders = q2_pic_holder(f);\n"
        "      qjs_aot_set_reg_p(ctx, fr, %d, v%u);\n",
        in->pc, recv, atom_off, site, defv, in->pc, off, defv);
    q2_cold_end(k);
    return;
  }
  fprintf(f, "    if (__builtin_expect(!(%s), 0)) {\n", cond);
  q2_writeback(f, fn, &g_em.pre, g_em.liveb);
  fprintf(
      f,
      "      if (qjs_aot_read_field_slow(ctx, f, code + %d, %s, "
      "(int)qjs_aot_ld_u32(code + %d), %d, &v%u) < 0) { pc = %d; goto "
      "exception; }\n"
      "      pic = q2_pic(f); holders = q2_pic_holder(f);\n"
      "      qjs_aot_set_reg_p(ctx, fr, %d, v%u);\n"
      "    }\n",
      in->pc, recv, atom_off, site, defv, in->pc, off, defv);
}

/* A generic slow arm for an operation that continues compiled when it
   succeeds and throws when it fails: the hot path tests `cond`, and the arm is
   the text `body` (already formatted with the arm's stream in mind). Used for
   the binary, global and compare paths, whose arms all end by continuing. */
static void q2_cold_arm(
    FILE *f, const Q2Func *fn, const char *cond, const char *body) {
  int k;
  FILE *m = q2_cold_begin(f, cond, 1, &k);
  if (m) {
    fputs(body, m);
    q2_cold_end(k);
    return;
  }
  fprintf(f, "    if (__builtin_expect(!(%s), 0)) {\n", cond);
  q2_writeback(f, fn, &g_em.pre, g_em.liveb);
  fputs(body, f);
  fprintf(f, "    }\n");
}

/* The frame as the interpreter must see it on the way out of an exception:
   every value that is live anywhere in the function, stored to
   its slot. Emitted once, at the function's `exception:` label, because the
   unwind can resume the interpreter at a catch pc whose live set is a
   different instruction's -- a union over the function is a correct superset,
   and it only runs on the throw path. */
static void q2_exception_sync(FILE *f, const Q2Func *fn, const Q2Cur *c) {
  int w, words = fn->lwords;
  for (w = 0; w < words; w++) {
    uint64_t m = 0;
    int i;
    for (i = 0; i < fn->ninsns; i++)
      m |= fn->insn_live[(size_t)i * words + w];
    while (m) {
      int r = w * 64 + __builtin_ctzll(m);
      Q2ValueId v;
      m &= m - 1;
      if (r >= fn->nregs) continue;
      v = c->cur[r];
      if (v == Q2_NO_VALUE || v >= (Q2ValueId)fn->nvalues) continue;
      {
        char e[64];
        q2_use(fn, v, Q2_R_TAGGED, e, sizeof(e));
        fprintf(f, "      AOT_SET_B(ctx, fr, %d, %s); /* xsync v%u */\n",
                fn->values[v].reg * 16, e, v);
      }
    }
  }
}

/* The live set before an instruction: (insn_live \ defs) | uses. */
/* The live set immediately after an instruction: what build.c computed and
   stored. Equal to the successor's live-in for a fall-through edge. */
const uint64_t *q2_live_out(const Q2Func *fn, int insn) {
  return fn->insn_live + (size_t)insn * (size_t)fn->lwords;
}

void q2_live_before(const Q2Func *fn, int insn, uint64_t *out) {
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

/* A definition: the C local, then the frame slot. A tagged value is written
   through as it always was, because a block reached later reads slot state
   rather than this block's locals. A **typed** value is not: it carries no
   reference, so nothing needs to see it, and the write-back before a safepoint
   or a leave boxes it once. That split is R1's win on `mb-loop`, which is all
   typing and no heap traffic. */
static void q2_def(
    FILE *f, const Q2Func *fn, const Q2Insn *in, int defi, const char *expr,
    int adopt) {
  Q2ValueId v = in->def[defi];
  int off = fn->values[v].reg * 16;
  fprintf(f, "    v%u = %s;\n", v, expr);
  if (adopt)
    fprintf(f, "    qjs_aot_set_reg_p(ctx, fr, %d, v%u);\n", off, v);
  else if (!g_em.fast) /* the loop copy keeps no frame; see q2_emit_edge */
    fprintf(f, "    AOT_SET_B(ctx, fr, %d, v%u);\n", off, v);
}

/* A definition of a typed value: assign its unboxed local, and its boxed form
   too when the value is *synced* (a constant, a move of a synced value, a
   speculated load). A computed typed value is not stored: it is boxed once by
   the write-back before the next safepoint or leave, which is R1 in the step-5
   report. The synced case must keep its store -- a successor block reads slot
   state, not this block's locals. */
static void q2_def_typed(
    FILE *f, const Q2Func *fn, const Q2Insn *in, int defi, const char *expr) {
  Q2ValueId v = in->def[defi];
  const Q2Value *val = &fn->values[v];
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
  if (val->synced && !g_em.fast) {
    char e[64];
    q2_use(fn, v, Q2_R_TAGGED, e, sizeof(e));
    fprintf(f, "    AOT_SET_B(ctx, fr, %d, %s);\n", val->reg * 16, e);
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
  /* In the loop copy, an edge to a block of the loop stays in the copy; the
     back edge to its header is `fself`. */
  int fin = g_em.fast && blk->succ_block[si] >= g_em.fast_lo &&
            blk->succ_block[si] <= g_em.fast_hi;
  int fself = fin && blk->succ_block[si] == g_em.fast_lo;
  if (!s) {
    fprintf(f, " return qjs_aot_ret(f);");
    return;
  }
  /* The loop copy keeps no frame (its leaves and the generic copy's calls
     write back what they need), so an edge out of it stores what the code
     after it may read from the slots. */
  if (g_em.fast && !fin)
    q2_writeback(f, fn, &g_em.cur, q2_live_after(g_em.cur_insn));
  /* A back edge polls as the interpreter's does: an inline decrement, and the
     engine only when the counter runs out. */
  if (q2_is_back_edge(fn, b, blk->succ_block[si]) && !fself)
    fprintf(
        f,
        " if (__builtin_expect(--*(int *)((unsigned char *)ctx + "
        "QJS_AOT_OFF_CTX_INTERRUPT_COUNTER) <= 0, 0)) {"
        " ++*(int *)((unsigned char *)ctx + QJS_AOT_OFF_CTX_INTERRUPT_COUNTER);"
        " if (qjs_aot_poll(ctx) < 0) { pc = %d; goto exception; } }",
        blk->start_pc);
  /* The parameters of a successor that was not an edge when the arguments were
     built (the exit, or a dead target) have no arguments: read them from the
     frame, which is the same value. Those values may be tagged and therefore
     not in the frame right now, so the live set is stored first. */
  if (si >= 2 || !blk->edge_arg[si]) {
    q2_writeback(f, fn, &g_em.cur, q2_live_after(g_em.cur_insn));
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
    fprintf(f, fin ? " goto ff_%d;" : " goto b_%d;", s->start_pc);
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
        /* Tagged parameters are locals now: nothing is stored on the edge. A
           later safepoint or leave writes the live ones back. */
        fprintf(f, " v%u = ev%d;", pv, k);
        break;
      }
    }
  }
  if (fself) {
    /* The back edge of the loop copy: the interpreter's poll, inline. Its slow
       arm is a collector safepoint and can run the interrupt handler, so the
       live values are stored first, and the loop goes back through its
       preheader afterwards and re-checks every guard. */
    fprintf(
        f,
        " if (__builtin_expect(--*(int *)((unsigned char *)ctx + "
        "QJS_AOT_OFF_CTX_INTERRUPT_COUNTER) <= 0, 0)) {"
        " ++*(int *)((unsigned char *)ctx + QJS_AOT_OFF_CTX_INTERRUPT_COUNTER);\n");
    q2_writeback(f, fn, &g_em.cur, q2_live_after(g_em.cur_insn));
    fprintf(
        f,
        " if (qjs_aot_poll(ctx) < 0) { pc = %d; goto exception; }"
        " goto b_%d; }"
        " goto ff_%d;",
        blk->start_pc, s->start_pc, s->start_pc);
    return;
  }
  fprintf(f, fin ? " goto ff_%d;" : " goto b_%d;", s->start_pc);
}

static int q2_target(const Q2Insn *in, int arg, int *tgt) {
  if (arg >= in->narg || in->arg[arg].kind != 't') return 0;
  *tgt = in->next + (int)in->arg[arg].val;
  return 1;
}

static void q2_emit_fallback(
    FILE *f, const Q2Func *fn, int blk_index, const Q2Insn *in, int exit_pc);

/* One instruction through the shared step routine. The frame is current, so
   the step sees the state it expects; afterwards the locals of whatever it may
   have written are reloaded from the frame. */
static void q2_emit_fallback_liveb(const Q2Func *fn, const Q2Insn *in) {
  int w, r;
  for (w = 0; w < fn->lwords; w++)
    g_em.liveb[w] |= q2_live_after(g_em.cur_insn)[w];
  /* The step reads a call's argument window from the frame. Its registers were
     stored when the arguments were moved there, except on a way in from a
     loop copy, which keeps no frame, so they are stored here. */
  for (r = in->win_reg; in->win_n > 0 && r < in->win_reg + in->win_n; r++)
    if (r >= 0 && r < fn->nregs) g_em.liveb[r >> 6] |= 1ull << (r & 63);
  {
    /* A step whose roles are unknown may read any register, a window
       included, so every value this point holds goes back to the frame. */
    const Q2RopInfo *ri = q2_rop_info(in->rop);
    if (ri->op == Q2_OP_FALLBACK && ri->ndef == 0 && ri->nuse == 0 &&
        ri->window < 0)
      for (w = 0; w < fn->lwords; w++) g_em.liveb[w] = ~0ull;
  }
}

/* The step itself and everything after it: reload, speculation guard, the
   successor edge the returned ip selects, and the deopt for anything else.
   The caller has already written the live values back. When `hit_pc` is not
   -1, a label `q2hit_<hit_pc>:` is emitted after the reload so a specialised
   call that already produced the result jumps straight to the edge. */
static void q2_emit_step_body(
    FILE *f, const Q2Func *fn, int blk_index, const Q2Insn *in, int exit_pc,
    int hit_pc) {
  int i;
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
     one writes only its own destinations. A specialised call that already
     produced its result jumps here: it stored the frame, and the reload makes
     the local current. */
  if (hit_pc >= 0) fprintf(f, "q2hit_%d:\n", hit_pc);
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

static void q2_emit_fallback(
    FILE *f, const Q2Func *fn, int blk_index, const Q2Insn *in, int exit_pc) {
  q2_emit_fallback_liveb(fn, in);
  q2_fb_reset(fn);
  q2_writeback(f, fn, &g_em.pre, g_em.liveb);
  q2_emit_step_body(f, fn, blk_index, in, exit_pc, -1);
}

/* A call the profile shows as one known callee: the engine's specialised call
   helper instead of the step. The frame is written back first (the callee runs
   user code), the helper runs, a hit stores the owned result in the frame and
   jumps to the edge the step body would have taken, a miss falls through to
   the step. Returns 1 when it emitted an attempt. A typed result is left to
   the step so its speculation guard stays in one place. */
/* Dispatch a call whose site saw 2-4 bytecode callees. Each branch checks the
   callee's bound entry against one of the known digests and calls it on a light
   frame; a callee that matches none falls through to the generic helper, which
   is what the single-callee path does on a miss anyway. `qjs_aot_call_light`
   revalidates the callee's eligibility, so a wrong guess is safe. */
static void q2_emit_poly_call_impl(FILE *f, const Q2Func *fn, const Q2Insn *in) {
  int n = 0, kind[4], k, nb = 0, is_m = (in->rop == ROP_call_method);
  int na = in->win_n;
  int abc = (int)in->arg[is_m ? 3 : 2].val;
  int cc = (int)in->arg[1].val;
  int off = fn->values[in->def[0]].reg * 16;
  char hex[4][33];
  unsigned long long hits[4];
  if (!qjs_aot_prof_callees(fn->g->digest, in->next, &n, hex, kind, hits))
    return;
  /* A call is a safepoint: store what the call reads and the live heap
     references, exactly as the single-callee path does. */
  q2_emit_fallback_liveb(fn, in);
  {
    uint64_t *need = calloc((size_t)fn->lwords, 8);
    if (need) {
      q2_call_need_bits(fn, in, need);
      q2_writeback_call(f, fn, &g_em.pre, g_em.liveb, need);
      free(need);
    } else {
      q2_writeback(f, fn, &g_em.pre, g_em.liveb);
    }
  }
  fprintf(f, "    { JSValue _o; int _h;\n");
  for (k = 0; k < n && nb < 4; k++) {
    if (kind[k] != 1 || !qjs_aot_unit_has(hex[k])) continue;
    fprintf(f, "#ifdef Q2_SEL_%s\n", hex[k]);
    fprintf(
        f,
        "      if (q2_entry_is(AOT_REG(fr, %d), (const void *)aot_f_%s)) {\n"
        "        _h = qjs_aot_call_light(ctx, f, %d, AOT_REG(fr, %d), ",
        cc, hex[k], na, cc);
    if (is_m)
      fprintf(f, "AOT_REG(fr, %d), ", (int)in->arg[2].val);
    else
      fprintf(f, "JS_UNDEFINED, ");
    fprintf(
        f,
        "&AOT_REG(fr, %d), &_o);\n"
        "        if (_h > 0) { qjs_aot_set_reg_p(ctx, fr, %d, _o); ip = code + "
        "%d; goto q2hit_%d; }\n"
        "        if (_h < 0) { pc = %d; goto exception; }\n"
        "      }\n"
        "#endif\n",
        abc, off, in->next, in->pc, in->pc);
    nb++;
  }
  fprintf(f, "    }\n");
}

/* Whether a call site has 2-4 bytecode callees, so it can be dispatched by a
   compare chain instead of the dominant-callee path. */
static int q2_poly_call_site(const Q2Func *fn, const Q2Insn *in) {
  int n = 0, kind[4], k, nbc = 0;
  char hex[4][33];
  unsigned long long hits[4];
  if (!qjs_aot_prof_callees(fn->g->digest, in->next, &n, hex, kind, hits))
    return 0;
  if (n < 2) return 0;
  for (k = 0; k < n; k++)
    if (kind[k] == 1 && qjs_aot_unit_has(hex[k])) nbc++;
  return nbc >= 2;
}

static int q2_emit_poly_call(FILE *f, const Q2Func *fn, const Q2Insn *in) {
  q2_emit_poly_call_impl(f, fn, in);
  return 1;
}

static int q2_emit_call_special(
    FILE *f, const Q2Func *fn, int blk_index, const Q2Insn *in) {
  int is_m, kind = 0, cproto = -1, sid, na, abc, off, cc, leaf = 0;
  char hex[33];
  Q2ValueId dv;

  (void)blk_index;
  if (in->rop != ROP_call && in->rop != ROP_call_method) return 0;
  dv = in->def[0];
  if (fn->values[dv].rep != Q2_R_TAGGED) return 0;
  if (in->nuse < 1 || in->win_n < 0) return 0;
  if (!qjs_aot_prof_call(fn->g->digest, in->next, &kind, &cproto, hex)) {
    /* No dominant-callee record, but the site may still have one: the
       polymorphic list below covers a site whose callees tie or whose record
       the merge dropped. */
    kind = 0;
  }
  if (kind != 1 && kind != 2 && !q2_poly_call_site(fn, in)) return 0;
  if (kind != 1 && kind != 2) return q2_emit_poly_call(f, fn, in);
  if ((unsigned)(in->win_reg + in->win_n) > (unsigned)fn->nregs) return 0;

  is_m = (in->rop == ROP_call_method);
  na = in->win_n;
  abc = (int)in->arg[is_m ? 3 : 2].val; /* window base register offset */
  cc = (int)in->arg[1].val;             /* callee register offset */
  off = fn->values[dv].reg * 16;

  /* A callee with a leaf body runs in place of the call: no frame, no
     write-back, nothing for the collector to see. It returns 0 before doing
     anything when one of its guards fails, and the call below runs instead. */
  {
    int largc = kind == 1 ? qjs_aot_leaf_argc(hex) : -1;
    leaf = largc >= 0 && largc <= na && in->win_val && !g_em.pure;
    if (leaf) {
      char cal[64], th[64], e[64];
      int k;
      q2_op(in, 1, cal, sizeof(cal));
      if (is_m)
        q2_op(in, 2, th, sizeof(th));
      else
        snprintf(th, sizeof(th), "JS_UNDEFINED");
      fprintf(f, "#ifdef Q2_SEL_%s\n", hex);
      fprintf(
          f,
          "    { JSValue _o, _a[%d]; const void *_lp; void *const *_lh;\n",
          na > 0 ? na : 1);
      for (k = 0; k < na; k++) {
        if (in->win_val[k] == Q2_NO_VALUE) {
          fprintf(f, "      _a[%d] = JS_UNDEFINED;\n", k);
          continue;
        }
        q2_use(fn, in->win_val[k], Q2_R_TAGGED, e, sizeof(e));
        fprintf(f, "      _a[%d] = %s;\n", k, e);
      }
      fprintf(
          f,
          "      if (q2_leaf_callee(%s, (const void *)aot_f_%s, &_lp, &_lh) &&\n"
          "          aot_p_%s(ctx, _lp, _lh, %s, %d, _a, &_o)) {\n"
          "        AOT_SET_B(ctx, fr, %d, _o);\n"
          "        ip = code + %d; goto q2hit_%d;\n"
          "      }\n"
          "    }\n#endif\n",
          cal, hex, hex, th, na, off, in->next, in->pc);
    }
  }

  /* A call is a safepoint: the callee runs user code and can allocate. Only
     what the call reads and the heap references live here are stored. The
     callee cannot write the caller's frame, so the cache survives the call. */
  q2_emit_fallback_liveb(fn, in);
  {
    uint64_t *need = calloc((size_t)fn->lwords, 8);
    if (need) {
      q2_call_need_bits(fn, in, need);
      q2_writeback_call(f, fn, &g_em.pre, g_em.liveb, need);
      free(need);
    } else {
      q2_writeback(f, fn, &g_em.pre, g_em.liveb);
    }
  }

  /* The callee and `this` are read from the frame, and the window holds the
     arguments. */
  if (kind == 1 && qjs_aot_unit_has(hex)) {
    /* A callee compiled into this unit is called directly, on a light frame the
       engine builds: no interpreter activation in between. A miss (a different
       closure, a disabled callee, an arena that cannot supply the frame) falls
       through to the generic helper.

       When the callee's frame fits a stack buffer the caller allocates, the
       frame is built there instead (R2): no arena bump, no seal, no free walk.
       A deopt or an uncaught throw inside the callee still finishes through the
       interpreter on that same frame. */
    int leaf = qjs_aot_light_need(hex);
    fprintf(f, "    { JSValue _o; int _h;\n");
    if (leaf > 0) {
      fprintf(
          f,
          "      struct { unsigned long long a[%d]; } _fb;\n",
          (leaf + 7) / 8);
      fprintf(f, "      _h = qjs_aot_call_leaf(ctx, f, %d, AOT_REG(fr, %d), ",
              na, cc);
      if (is_m)
        fprintf(f, "AOT_REG(fr, %d), ", (int)in->arg[2].val);
      else
        fprintf(f, "JS_UNDEFINED, ");
      fprintf(f, "&AOT_REG(fr, %d), &_o, &_fb, sizeof(_fb));\n", abc);
    } else {
      fprintf(f, "      _h = qjs_aot_call_light(ctx, f, %d, AOT_REG(fr, %d), ",
              na, cc);
      if (is_m)
        fprintf(f, "AOT_REG(fr, %d), ", (int)in->arg[2].val);
      else
        fprintf(f, "JS_UNDEFINED, ");
      fprintf(f, "&AOT_REG(fr, %d), &_o);\n", abc);
    }
    fprintf(
        f,
        "      if (_h > 0) { qjs_aot_set_reg_p(ctx, fr, %d, _o); ip = code + "
        "%d; goto q2hit_%d; }\n"
        "      if (_h < 0) { pc = %d; goto exception; }\n",
        off, in->next, in->pc, in->pc);
    fprintf(f, "    }\n");
  }
  /* The result is stored by the engine's owning store, so forget what this
     slot held. */
  if (g_em.fb && off / 16 < fn->nregs) g_em.fb[off / 16] = Q2_NO_VALUE;
  fprintf(f, "    { JSValue _o; int _h;\n");
  if (kind == 2) {
    sid = qjs_aot_site_new();
    fprintf(f, "      _h = qjs_aot_call_c(ctx, %d, %d, %d, AOT_REG(fr, %d), ",
            sid, cproto, na, cc);
  } else {
    fprintf(f, "      _h = qjs_aot_call_b(ctx, %d, AOT_REG(fr, %d), ", na, cc);
  }
  if (is_m)
    fprintf(f, "AOT_REG(fr, %d), ", (int)in->arg[2].val);
  else
    fprintf(f, "JS_UNDEFINED, ");
  fprintf(f, "&AOT_REG(fr, %d), &_o);\n", abc);
  fprintf(
      f,
      "      if (_h > 0) { qjs_aot_set_reg_p(ctx, fr, %d, _o); ip = code + %d; "
      "goto q2hit_%d; }\n"
      "      if (_h < 0) { pc = %d; goto exception; }\n",
      off, in->next, in->pc, in->pc);
  fprintf(f, "    }\n");
  /* A leaf hit joins after the call having skipped its write-back, so what
     the write-back stored is not known to be in the frame after it. */
  if (leaf) q2_fb_reset(fn);
  return 1;
}

/* A binary/compare whose result is typed, dispatched before the opcode switch
   so the generic ROP_binary and the immediate forms share one path. Returns 1
   when it emitted the instruction. */
/* ---- the loop copy ------------------------------------------------------ */
/* A loop -- a header block and the blocks up to its one back edge, entered only
   through the header -- runs as two copies when its field accesses all hit
   data properties of receivers defined outside it. The preheader checks every
   access's cache entry against its receiver's shape once and resolves the slot
   each one reads or writes (an own slot, or the prototype holder's for a
   method), and reads the globals the loop reads; the copy then loads and
   stores through those pointers with no per-iteration check. A read whose slot
   no write in the loop can reach is loaded once, and a read of a slot the loop
   writes takes the value last written. A call to a leaf runs the leaf; when it
   declines, the copy continues in the generic copy at that call. When a
   preheader check fails the generic copy runs.

   The copy is sound only if nothing in it can change a shape, a slot's
   address, a global, or a written slot's kind between iterations: it runs no
   user code, calls no engine helper that returns into the loop, its writes are
   own data stores of non-counted values to objects that are not prototypes,
   and a leaf writes nothing. q2_fast_scan checks the emitted copy for any call
   outside a small set of pure ones, and the copy is dropped if it finds one.
   A guard that fails inside the copy leaves to the interpreter as it would in
   the generic copy. */

/* Whether value `v` is defined outside the loop being copied. */
static int q2_fast_outside(const Q2Func *fn, Q2ValueId v) {
  int b = fn->values[v].block;
  return b < g_em.fast_lo || b > g_em.fast_hi;
}

/* What the loop copy's preheader resolves for `in`: 1 a field read, 0 a field
   write, 2 a global read, -1 nothing. */
static int q2_fast_access(const Q2Func *fn, const Q2Insn *in) {
  int read, k, site;
  Q2ValueId r;
  if (in->rop == ROP_get_global) {
    site = (int)in->arg[3].val;
    if ((unsigned)site == REG_IC_NONE || in->ndef < 1) return -1;
    return 2;
  }
  if (in->rop == ROP_get_field || in->rop == ROP_get_field_own ||
      in->rop == ROP_get_field2 || in->rop == ROP_get_field2_own)
    read = 1;
  else if (in->rop == ROP_put_field)
    read = 0;
  else
    return -1;
  k = read ? 1 : 0;
  site = (int)in->arg[read ? 3 : 2].val;
  if ((unsigned)site == REG_IC_NONE || k >= in->narg ||
      in->arg[k].kind != 'r')
    return -1;
  r = in->argval[k];
  if (r == Q2_NO_VALUE || !q2_fast_outside(fn, r)) return -1;
  if (!read && (3 >= in->narg || in->arg[3].kind != 'r' ||
                in->argval[3] == Q2_NO_VALUE))
    return -1;
  return read;
}

/* Whether a read of the loop copy takes a double already known, in place of a
   tag guard. */
static int q2_fast_direct(const Q2Func *fn, const Q2Insn *in) {
  int ix = (int)(in - fn->insns), g, acc;
  if (!g_em.fast || !g_em.fgroup || in->ndef < 1) return 0;
  acc = q2_fast_access(fn, in);
  if (acc != 1 && acc != 2) return 0;
  if (!fn->values[in->def[0]].spec || fn->values[in->def[0]].rep != Q2_R_F64)
    return 0;
  /* An invariant read was unboxed by the preheader. */
  if (g_em.fhoist[ix]) return 1;
  g = g_em.fgroup[ix];
  return g >= 0 && g_em.fdbl[g];
}

/* A field or global access inside the loop copy: a load or store through the
   slot the preheader resolved, or the value it read. Returns 0 to emit the
   generic access (the scan then drops the copy). */
static int q2_fast_field(
    FILE *f, const Q2Func *fn, const Q2Insn *in, int read) {
  int ix = (int)(in - fn->insns), sid, acc = q2_fast_access(fn, in);
  char v[64];
  if (acc < 0 || (acc == 0) != (read == 0)) return 0;
  if (read) {
    int g = g_em.fgroup[ix];
    q2_guard(f, in->pc, q2_site());
    if (g_em.fhoist[ix])
      fprintf(f, "    v%u = fv%d;\n", in->def[0], ix);
    else if (g >= 0)
      fprintf(f, "    v%u = fc%d;\n", in->def[0], g);
    else
      fprintf(f, "    v%u = *fs%d;\n", in->def[0], ix);
    /* The number's double is already known -- the preheader unboxed an
       invariant read, or the last write stored it -- so the read takes it and
       its tag guard, which that number passes, is not emitted. */
    if (q2_fast_direct(fn, in)) {
      if (g_em.fhoist[ix])
        fprintf(f, "    d%u = fvd%d;\n", in->def[0], ix);
      else
        fprintf(f, "    d%u = fcd%d;\n", in->def[0], g);
    }
    return 1;
  }
  /* The old value is not counted (the preheader checked it, and every write in
     the loop keeps it so); a counted new value leaves before the store. */
  sid = q2_site();
  q2_guard(f, in->pc, sid);
  q2_op(in, 3, v, sizeof(v));
  fprintf(f, "    if (__builtin_expect(JS_VALUE_GET_TAG(%s) < 0, 0))\n", v);
  q2_miss(f, in->pc, sid);
  fprintf(f, "    *fs%d = %s;\n", ix, v);
  if (g_em.fgroup[ix] >= 0) {
    fprintf(f, "    fc%d = %s;\n", g_em.fgroup[ix], v);
    if (g_em.fdbl[g_em.fgroup[ix]]) {
      q2_use(fn, in->argval[3], Q2_R_F64, v, sizeof(v));
      fprintf(f, "    fcd%d = %s;\n", g_em.fgroup[ix], v);
    }
  }
  return 1;
}

static void q2_emit_edge(FILE *f, const Q2Func *fn, int b, int si);

/* The C expression of value `v` in the loop copy's preheader, when the value
   is fixed for the whole loop: defined before it, a move of such a value, or
   a read the preheader made. Returns 0 when it is not. */
static int q2_fast_inv(const Q2Func *fn, Q2ValueId v, char *buf, size_t n) {
  int depth;
  for (depth = 0; depth < 8 && v != Q2_NO_VALUE; depth++) {
    int ix, acc;
    const Q2Insn *in;
    if (q2_fast_outside(fn, v)) {
      q2_use(fn, v, Q2_R_TAGGED, buf, n);
      return 1;
    }
    ix = fn->values[v].insn;
    if (ix < 0 || ix >= fn->ninsns) return 0;
    in = &fn->insns[ix];
    if (in->op == Q2_OP_MOVE && in->nuse >= 1) {
      v = in->use[0];
      continue;
    }
    acc = q2_fast_access(fn, in);
    if ((acc == 1 || acc == 2) && g_em.fhoist[ix] && in->def[0] == v) {
      snprintf(buf, n, "fv%d", ix);
      return 1;
    }
    return 0;
  }
  return 0;
}

/* Whether call `in` of the loop copy runs a leaf with its callee and its
   field reads' receivers fixed for the loop, so the preheader resolves both;
   *leaf is the leaf's record and hex its digest. */
static int q2_fast_rcall(const Q2Func *fn, const Q2Insn *in,
                         const Q2Leaf **leaf, char hex[33]) {
  int kind = 0, cproto = -1, k, is_m;
  char e[64];
  const Q2Leaf *lf;
  if (in->rop != ROP_call && in->rop != ROP_call_method) return 0;
  if (in->ndef < 1 || fn->values[in->def[0]].rep != Q2_R_TAGGED ||
      in->nuse < 1 || in->win_n < 0 || !in->win_val)
    return 0;
  if (!qjs_aot_prof_call(fn->g->digest, in->next, &kind, &cproto, hex) ||
      kind != 1 || qjs_aot_leaf_argc(hex) < 0 ||
      qjs_aot_leaf_argc(hex) > in->win_n)
    return 0;
  lf = q2_leaf_find(hex);
  if (!lf) return 0;
  is_m = (in->rop == ROP_call_method);
  if (in->arg[1].kind != 'r' || !q2_fast_inv(fn, in->argval[1], e, sizeof(e)))
    return 0;
  for (k = 0; k < lf->n; k++) {
    int r = lf->recv[k];
    if (r == -1) {
      if (!is_m || in->arg[2].kind != 'r' ||
          !q2_fast_inv(fn, in->argval[2], e, sizeof(e)))
        return 0;
    } else if (r >= in->win_n || in->win_val[r] == Q2_NO_VALUE ||
               !q2_fast_inv(fn, in->win_val[r], e, sizeof(e))) {
      return 0;
    }
  }
  *leaf = lf;
  return 1;
}

/* The preheader's half of a resolved leaf call: the callee checked once, and
   each of the leaf's reads resolved against the callee's cache to its slot,
   own or the prototype holder's. */
static void q2_fast_rcall_pre(FILE *f, const Q2Func *fn, const Q2Insn *in,
                              int ix, int pc) {
  const Q2Leaf *lf;
  char hex[33], cal[64], r[64];
  int k;
  if (!q2_fast_rcall(fn, in, &lf, hex)) return;
  q2_fast_inv(fn, in->argval[1], cal, sizeof(cal));
  fprintf(f, "#ifdef Q2_SEL_%s\n", hex);
  /* A leaf with no field reads has no cache to resolve against. */
  fprintf(
      f,
      "      if (!q2_leaf_callee(%s, (const void *)aot_f_%s, &flp%d, &flh%d) ||\n"
      "          (%d && flp%d == NULL)) goto fx_%d;\n",
      cal, hex, ix, ix, lf->n > 0, ix, pc);
  for (k = 0; k < lf->n; k++) {
    if (lf->recv[k] == -1)
      q2_fast_inv(fn, in->argval[2], r, sizeof(r));
    else
      q2_fast_inv(fn, in->win_val[lf->recv[k]], r, sizeof(r));
    fprintf(
        f,
        "      if (JS_VALUE_GET_TAG(%s) != JS_TAG_OBJECT) goto fx_%d;\n"
        "      _o = (const unsigned char *)JS_VALUE_GET_PTR(%s);\n"
        "      _e = (const unsigned char *)flp%d + (size_t)%d * "
        "QJS_AOT_SIZEOF_JSREGPIC;\n"
        "      if (*(const void *const *)_e != *(const void *const *)(_o + "
        "QJS_AOT_OFF_JSOBJECT_SHAPE)) goto fx_%d;\n"
        "      if (*(const uint16_t *)(_e + QJS_AOT_OFF_JSREGPIC_KIND) != "
        "QJS_AOT_REG_PIC_OWN) {\n"
        "        _o = *(const unsigned char *const *)(*(const unsigned char "
        "*const *)(_o + QJS_AOT_OFF_JSOBJECT_SHAPE) + "
        "QJS_AOT_OFF_JSSHAPE_PROTO);\n"
        "        if (_o == NULL || flh%d == NULL || *(const void *const *)(_o + "
        "QJS_AOT_OFF_JSOBJECT_SHAPE) != flh%d[%d]) goto fx_%d;\n"
        "      }\n"
        "      fls%d[%d] = (JSValue *)(*(unsigned char *const *)(_o + "
        "QJS_AOT_OFF_JSOBJECT_PROP) + *(const uint32_t *)(_e + "
        "QJS_AOT_OFF_JSREGPIC_SLOT_OFF));\n",
        r, pc, r, ix, lf->site[k], pc, ix, ix, lf->site[k], pc, ix, k);
  }
  fprintf(f, "#endif\n");
}

/* A call inside the loop copy: the callee's leaf, and on a hit the block's
   edge, which stays in the copy. A leaf that declines, a callee with no leaf,
   or any other callee continues in the generic copy at this call, whose state
   is the copy's: the same locals, and the same frame stores before it. */
static void q2_fast_call(FILE *f, const Q2Func *fn, int b, const Q2Insn *in) {
  int ix = (int)(in - fn->insns), kind = 0, cproto = -1, na, largc, k, is_m;
  int off;
  char hex[33], cal[64], th[64], e[64];
  Q2ValueId dv = in->ndef >= 1 ? in->def[0] : Q2_NO_VALUE;
  const Q2Leaf *lf;

  if (fn->blocks[b].nsucc == 1 &&
      ix == fn->blocks[b].first_insn + fn->blocks[b].ninsns - 1 &&
      q2_fast_rcall(fn, in, &lf, hex)) {
    /* The callee and the leaf's slots were resolved by the preheader. */
    is_m = (in->rop == ROP_call_method);
    na = in->win_n;
    if (is_m)
      q2_op(in, 2, th, sizeof(th));
    else
      snprintf(th, sizeof(th), "JS_UNDEFINED");
    fprintf(f, "#ifdef Q2_SEL_%s\n", hex);
    fprintf(f, "    { JSValue _o, _a[%d];\n", na > 0 ? na : 1);
    for (k = 0; k < na; k++) {
      if (in->win_val[k] == Q2_NO_VALUE) {
        fprintf(f, "      _a[%d] = JS_UNDEFINED;\n", k);
        continue;
      }
      q2_use(fn, in->win_val[k], Q2_R_TAGGED, e, sizeof(e));
      fprintf(f, "      _a[%d] = %s;\n", k, e);
    }
    fprintf(f,
            "      if (aot_r_%s(ctx, %s, %d, _a, fls%d, &_o)) {\n"
            "        v%u = _o;\n"
            "       ",
            hex, th, na, ix, dv);
    q2_emit_edge(f, fn, b, 0);
    fprintf(f, "\n      }\n    }\n#endif\n");
    fprintf(f, "    goto gi_%d;\n", ix);
    return;
  }
  if (dv != Q2_NO_VALUE && fn->values[dv].rep == Q2_R_TAGGED &&
      in->nuse >= 1 && in->win_n >= 0 && in->win_val &&
      qjs_aot_prof_call(fn->g->digest, in->next, &kind, &cproto, hex) &&
      kind == 1 && fn->blocks[b].nsucc == 1 &&
      ix == fn->blocks[b].first_insn + fn->blocks[b].ninsns - 1 &&
      (largc = qjs_aot_leaf_argc(hex)) >= 0 && largc <= in->win_n) {
    is_m = (in->rop == ROP_call_method);
    na = in->win_n;
    off = fn->values[dv].reg * 16;
    q2_op(in, 1, cal, sizeof(cal));
    if (is_m)
      q2_op(in, 2, th, sizeof(th));
    else
      snprintf(th, sizeof(th), "JS_UNDEFINED");
    fprintf(f, "#ifdef Q2_SEL_%s\n", hex);
    fprintf(
        f,
        "    { JSValue _o, _a[%d]; const void *_lp; void *const *_lh;\n",
        na > 0 ? na : 1);
    for (k = 0; k < na; k++) {
      if (in->win_val[k] == Q2_NO_VALUE) {
        fprintf(f, "      _a[%d] = JS_UNDEFINED;\n", k);
        continue;
      }
      q2_use(fn, in->win_val[k], Q2_R_TAGGED, e, sizeof(e));
      fprintf(f, "      _a[%d] = %s;\n", k, e);
    }
    fprintf(
        f,
        "      if (q2_leaf_callee(%s, (const void *)aot_f_%s, &_lp, &_lh) &&\n"
        "          aot_p_%s(ctx, _lp, _lh, %s, %d, _a, &_o)) {\n"
        "        v%u = _o;\n"
        "       ",
        cal, hex, hex, th, na, dv);
    (void)off;
    if (g_em.fb && off / 16 < fn->nregs) g_em.fb[off / 16] = Q2_NO_VALUE;
    q2_emit_edge(f, fn, b, 0);
    fprintf(f, "\n      }\n    }\n#endif\n");
  }
  fprintf(f, "    goto gi_%d;\n", ix);
}

static int q2_typed_binary(FILE *f, const Q2Func *fn, const Q2Insn *in) {
  Q2ValueId dv;
  const Q2Value *dval;
  int sub = in->sub;
  int aoff, boff, imm;
  int is_imm;

  if (in->op != Q2_OP_BINARY || in->ndef < 1) return 0;
  /* a constant-pool operand has its own lowering, which folds the literal */
  if (in->rop == ROP_binary_cpool) return 0;
  /* the fused family has its own lowering (two definitions) */
  if (in->rop == ROP_mul_add || in->rop == ROP_mul_sub ||
      in->rop == ROP_add_mul || in->rop == ROP_sub_mul)
    return 0;
  dv = in->def[0];
  dval = &fn->values[dv];
  if (dval->rep == Q2_R_TAGGED) return 0;
  /* Every register operand must be typed: a tagged one would need a guard the
     typed path does not emit, so fall back instead. A boolean is not a
     number — it has no unboxed numeric form that `===`/`<` agree with — so a
     boolean operand also falls back to the generic binary. */
  {
    int k;
    for (k = 0; k < in->nuse; k++) {
      int r = fn->values[in->use[k]].rep;
      if (r == Q2_R_TAGGED || r == Q2_R_BOOL) return 0;
    }
  }
  if (dval->rep == Q2_R_BOOL) return 0;

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
  return 0;
}

/* 1 lowered it fully, 2 lowered its fast path and deopts on a miss, 0 to fall
   back to the step routine. */
static int q2_fact_has(const Q2Insn *in, Q2ValueId v, int g);

static int q2_emit_native(
    FILE *f, const Q2Func *fn, int cur_block, const Q2Insn *in) {
  char a[64], b[64];
  FILE *m;
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
      /* The destination slot takes the value, not the source slot: a value
         read on a fast path (a field, a chained field) is in its local but
         not necessarily in its slot. Registers do not own under deferred
         counting, so that is a plain store; the counted form stores the
         source first and moves it with the handler's ownership transfer. */
      if (d != s && !g_em.fast)
        fprintf(
            f,
            "#if QJS_DRC\n    AOT_REG(fr, %d) = v%u;\n#else\n"
            "    AOT_SET_B(ctx, fr, %d, v%u);\n"
            "    qjs_aot_move_xfer(ctx, fr, %d, %d);\n#endif\n",
            d, dv, s, dv, d, s);
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
      /* A strict function's `this` is the frame's, borrowed; a non-strict one
         may box a primitive, which allocates, so it is a safepoint. The pair
         is inlined because the common case is one tag test. */
      char t[64];
      int off = fn->values[in->def[0]].reg * 16;
      if (g_em.pure) {
        fprintf(
            f,
            "    if (JS_VALUE_GET_TAG(this_obj) != JS_TAG_OBJECT) return 0;\n"
            "    v%u = this_obj;\n",
            in->def[0]);
        return 1;
      }
      fprintf(
          f,
          "    { JSValue _o; int _r;\n"
          "      _r = qjs_aot_load_this(ctx, f, &_o);\n"
          "      if (_r < 0) { pc = %d; goto exception; }\n"
          "      v%u = _o;\n",
          in->pc, in->def[0]);
      (void)t;
      (void)off;
      fprintf(f, "    }\n");
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
      /* A miss is not a deopt: a string concatenation, a coercion or a mixed
         tag takes the interpreter's generic binary and continues. It can run
         valueOf/toString, so the frame is written back first; the arm is out
         of line. */
      {
        char cond[200], body[256];
        const char *la = q2_op(in, 1, a, sizeof(a));
        const char *lb = q2_op(in, 2, b, sizeof(b));
        snprintf(
            cond, sizeof(cond), "q2_arith(%d, %s, %s, &v%u)", sub, la, lb, dv);
        snprintf(
            body, sizeof(body),
            "      if (qjs_aot_binary(ctx, %d, %s, %s, &v%u)) { pc = %d; goto "
            "exception; }\n",
            sub, la, lb, dv, in->pc);
        q2_cold_arm(f, fn, cond, body);
      }
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
      {
        char cond[200], body[256];
        const char *la = q2_op(in, ai, a, sizeof(a));
        snprintf(
            cond, sizeof(cond), "q2_arith(%d, %s, JS_MKVAL(JS_TAG_INT, %d), "
            "&v%u)",
            sub, la, imm, dv);
        snprintf(
            body, sizeof(body),
            "      if (qjs_aot_binary(ctx, %d, %s, JS_MKVAL(JS_TAG_INT, %d), "
            "&v%u)) { pc = %d; goto exception; }\n",
            sub, la, imm, dv, in->pc);
        q2_cold_arm(f, fn, cond, body);
      }
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
      /* As the interpreter's fused form: the first half through the generic
         binary on a miss, then the second. Neither half is a deopt. */
      fprintf(
          f,
          "    if (__builtin_expect(!q2_arith(%d, %s, %s, &v%u), "
          "0)) {\n",
          REG_BIN_MUL, q2_op(in, 1, a, sizeof(a)), q2_op(in, 2, b, sizeof(b)),
          in->def[0]);
      q2_writeback(f, fn, &g_em.pre, g_em.liveb);
      fprintf(
          f,
          "      if (qjs_aot_binary(ctx, %d, %s, %s, &v%u)) { pc = %d; goto "
          "exception; }\n"
          "    }\n",
          REG_BIN_MUL, q2_op(in, 1, a, sizeof(a)), q2_op(in, 2, b, sizeof(b)),
          in->def[0], in->pc);
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
            "&v%u), 0)) {\n",
            sub2, l, r, in->def[1]);
        q2_writeback(f, fn, &g_em.pre, g_em.liveb);
        fprintf(
            f,
            "        if (qjs_aot_binary(ctx, %d, %s, %s, &v%u)) { pc = %d; goto "
            "exception; }\n"
            "      }\n",
            sub2, l, r, in->def[1], in->pc);
      }
      return 1;
    }

    case ROP_get_field:
    case ROP_get_field_own: {
      int site = (int)in->arg[3].val;
      int sid, off;
      if ((unsigned)site == REG_IC_NONE) return 0;
      if (g_em.fast && q2_fast_field(f, fn, in, 1)) return 1;
      sid = q2_site();
      off = fn->values[in->def[0]].reg * 16;
      q2_guard(f, in->pc, sid);
      q2_op(in, 1, a, sizeof(a));
      if (g_em.pure == 2) {
        q2_leaf_slot_read(f, fn, in, in->argval[1], site, in->def[0]);
        return 1;
      }
      if (g_em.pure) {
        fprintf(
            f,
            "    if (__builtin_expect(!q2_own_read_hit(pic, holders, %s, %d, q2_pic_sites(f), "
            "&v%u), 0)) return 0;\n",
            a, site, in->def[0]);
        return 1;
      }
      /* A miss fills the cache and continues compiled rather than leaving for
         the interpreter, so a cold or polymorphic site never disables the
         function. The arm is out of line: the hot path pays only the branch. */
      {
        /* A grouped site in a specialised body: the receiver's shape against
           the group's validated shape, then a constant-offset load. A mismatch
           or a lazy marker exits at this instruction; there is no cache to
           fill, because the generic body serves the case. */
        int g = (g_em.spec && g_em.sgroup && (size_t)site < g_em.nsites &&
                 g_em.sgroup[site] >= 0 && g_em.sexec[site])
                    ? g_em.sgroup[site]
                    : -1;
        if (g >= 0) {
          /* Two checks, both needed: the receiver must still have the group's
             shape, and this site's own cell must still be that shape, because a
             step or a call after entry can refill the cell and move the offset
             (invariant 7). The offset is then this site's own, not the
             representative's. */
          Q2ValueId rv0 = in->argval[in->rop == ROP_put_field ? 0 : 1];
          if (q2_fact_has(in, rv0, g))
            /* The receiver's shape was checked earlier in this region (no call
               or step since): only this site's own cell is checked. */
            fprintf(
                f,
                "    if (__builtin_expect(q2_cell_shape(pic, %d) != gsh%d, 0)) {\n",
                site, g);
          else
            fprintf(
                f,
                "    if (__builtin_expect(q2_shape_of(%s) != gsh%d || "
                "q2_cell_shape(pic, %d) != gsh%d, 0)) {\n",
                a, g, site, g);
          q2_miss(f, in->pc, sid);
          fprintf(f, "    }\n");
          fprintf(
              f,
              "    v%u = *q2_slotp(%s, q2_cell_off(pic, %d));\n"
              "    if (__builtin_expect(JS_VALUE_GET_TAG(v%u) == "
              "QJS_AOT_TAG_LAZY_JSON, 0)) {\n",
              in->def[0], a, site, in->def[0]);
          q2_miss(f, in->pc, sid);
          fprintf(f, "    }\n");
          /* The check just established this pair for the rest of the block. */
          if (g_em.fact_cur) {
            int k;
            Q2ValueId rv = in->argval[1 - (in->rop == ROP_put_field ? 1 : 0)];
            for (k = 0; k < g_em.nfacts; k++)
              if (g_em.fact_val[k] == (int32_t)rv && g_em.fact_grp[k] == g)
                g_em.fact_cur[k >> 6] |= 1ull << (k & 63);
          }
          return 1;
        }
        if (g_em.spec && q2_poly_site(fn, site)) {
          /* A site with a 2-4 shape profile record: resolve it with a compare
             chain over the polymorphic cache instead of leaving for the
             interpreter. The fill arm calls the engine, which runs no
             JavaScript, so the chain keeps the function compiled; a property
             that is not own data exits instead. */
          int k = g_em.next_poly++;
          Q2ValueId rcv = in->argval[1];
          /* The receiver's known way, if a previous poly site matched it in
             this block, else -1. */
          fprintf(
              f, "    _pw = %d;\n",
              (g_em.poly_way && rcv != Q2_NO_VALUE && rcv < (Q2ValueId)fn->nvalues)
                  ? g_em.poly_way[rcv]
                  : -1);
          fprintf(
              f,
              "    _pe = q2_poly(f, %d);\n"
              "    _psh = q2_shape_of(%s);\n"
              "    if (__builtin_expect(_pe == NULL || _psh == NULL, 0)) "
              "{ goto pfill%d; }\n"
              "    else if (q2_poly_shape(_pe, 0) == _psh) _po = q2_poly_off(_pe, 0);\n"
              "    else if (q2_poly_shape(_pe, 1) == _psh) _po = q2_poly_off(_pe, 1);\n"
              "    else if (q2_poly_shape(_pe, 2) == _psh) _po = q2_poly_off(_pe, 2);\n"
              "    else if (q2_poly_shape(_pe, 3) == _psh) _po = q2_poly_off(_pe, 3);\n"
              "    else { goto pfill%d; }\n"
              "    goto pdone%d;\n"
              "pfill%d: {\n",
              site, a, k, k, k, k);
          /* The fill may allocate the cache, which is a safepoint: every live
             tagged value must be in the frame before it runs. */
          q2_writeback(f, fn, &g_em.pre, g_em.liveb);
          fprintf(
              f,
              "      if (!qjs_aot_poly_fill(ctx, f, %d, %s, "
              "(int)qjs_aot_ld_u32(code + %d), 0, &_po)) {\n",
              site, a, in->arg[2].off);
          q2_miss(f, in->pc, sid);
          fprintf(f, "      }\n    }\n");
          fprintf(f, "pdone%d:;\n", k);
          fprintf(
              f,
              "    v%u = *q2_slotp(%s, _po);\n"
              "    if (__builtin_expect(JS_VALUE_GET_TAG(v%u) == "
              "QJS_AOT_TAG_LAZY_JSON, 0)) {\n",
              in->def[0], a, in->def[0]);
          q2_miss(f, in->pc, sid);
          fprintf(f, "    }\n");
          return 1;
        }
        {
          char cond[160];
          snprintf(
              cond, sizeof(cond), "q2_own_read_hit(pic, holders, %s, %d, q2_ns, &v%u)",
              a, site, in->def[0]);
          q2_cold_read(
              f, fn, in, cond, in->arg[2].off, site, a, in->def[0], off);
        }
      }
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
      {
        char cond[160], recv[16];
        snprintf(
            cond, sizeof(cond),
            "q2_own_read_hit(pic, holders, %s, %d, q2_ns, &v%u)", a, s1,
            in->def[0]);
        q2_cold_read(
            f, fn, in, cond, in->arg[2].off, s1, a, in->def[0], off0);
        /* The second receiver is the first link's fresh value. */
        snprintf(recv, sizeof(recv), "v%u", in->def[0]);
        snprintf(
            cond, sizeof(cond),
            "q2_own_read_hit(pic, holders, %s, %d, q2_ns, &v%u)", recv, s2,
            in->def[1]);
        q2_cold_read(
            f, fn, in, cond, in->arg[6].off, s2, recv, in->def[1], off1);
      }
      return 1;
    }
    case ROP_get_field2:
    case ROP_get_field2_own: {
      int site = (int)in->arg[3].val;
      int fsid, sid, off;
      if ((unsigned)site == REG_IC_NONE) return 0;
      if (g_em.fast && q2_fast_field(f, fn, in, 1)) return 1;
      fsid = qjs_aot_site_new();
      sid = q2_site();
      off = fn->values[in->def[0]].reg * 16;
      q2_guard(f, in->pc, sid);
      q2_op(in, 1, a, sizeof(a));
      if (g_em.pure == 2) {
        q2_leaf_slot_read(f, fn, in, in->argval[1], site, in->def[0]);
        return 1;
      }
      if (g_em.pure) {
        fprintf(
            f,
            "    if (__builtin_expect(!q2_own_read_hit(pic, holders, %s, %d, q2_pic_sites(f), "
            "&v%u), 0)) return 0;\n",
            a, site, in->def[0]);
        return 1;
      }
      /* The inline hit first: it checks the receiver's and the prototype
         holder's shapes, which is what the fold checks before it reads the
         same slot. */
      fprintf(
          f,
          "    if (__builtin_expect(q2_own_read_hit(pic, holders, %s, %d, q2_pic_sites(f), "
          "&v%u), 1)) {\n"
          "    } else if (qjs_aot_proto_fold(ctx, f, pic, %d, %s, %d, &v%u)) {\n"
          "      qjs_aot_set_reg_p(ctx, fr, %d, v%u);\n"
          "    } else {\n",
          a, site, in->def[0], fsid, a, site, in->def[0], off, in->def[0]);
      q2_writeback(f, fn, &g_em.pre, g_em.liveb);
      fprintf(
          f,
          "      if (qjs_aot_read_field_slow(ctx, f, code + %d, %s, "
          "(int)qjs_aot_ld_u32(code + %d), %d, &v%u) < 0) { pc = %d; goto "
          "exception; }\n"
          "      pic = q2_pic(f); holders = q2_pic_holder(f);\n"
          "      qjs_aot_set_reg_p(ctx, fr, %d, v%u);\n",
          in->pc, a, in->arg[2].off, site, in->def[0], in->pc, off, in->def[0]);
      fprintf(f, "    }\n");
      return 1;
    }
    /* (get_field2 keeps its proto-fold chain inline: the fold and the engine's
       read share one arm, which is already cold for a plain own read.) */
    case ROP_put_field: { /* "rasr": receiver, atom, site, value */
      int site = (int)in->arg[2].val;
      char v[64];
      if ((unsigned)site == REG_IC_NONE) return 0;
      if (g_em.fast && q2_fast_field(f, fn, in, 0)) return 1;
      q2_guard(f, in->pc, q2_site());
      q2_op(in, 0, a, sizeof(a));
      q2_op(in, 3, v, sizeof(v));
      /* The own-slot overwrite inline; a prototype receiver
         takes the engine's hit. A miss runs the engine's store, which can call
         a setter and throw, fills the cache and continues compiled. Both
         fallbacks are one out-of-line arm. This arm stays even in a specialised
         body: making the miss an exit instead crashed earleyboyer (6 of 6) with
         a dangling receiver in the interpreter's own read, so the store's cache
         refill is what keeps the frame and the site consistent. */
      {
        char cond[256], arm[256];
        int k;
        FILE *m;
        snprintf(
            cond, sizeof(cond),
            "q2_put_field_hit(ctx, pic, %s, %d, %s) || "
            "qjs_aot_put_field_own(ctx, f, %s, %d, %s)",
            a, site, v, a, site, v);
        snprintf(
            arm, sizeof(arm),
            "      if (qjs_aot_put_field_slow(ctx, f, code + %d, %s, "
            "(int)qjs_aot_ld_u32(code + %d), %d, %s) < 0) { pc = %d; goto "
            "exception; }\n"
            "      pic = q2_pic(f); holders = q2_pic_holder(f);\n",
            in->pc, a, in->arg[1].off, site, v, in->pc);
        m = q2_cold_begin(f, cond, 1, &k);
        if (m) {
          fputs(arm, m);
          q2_cold_end(k);
        } else {
          fprintf(f, "    if (__builtin_expect(!(%s), 0)) {\n", cond);
          q2_writeback(f, fn, &g_em.pre, g_em.liveb);
          fputs(arm, f);
          fprintf(f, "    }\n");
        }
      }
      return 1;
    }
    case ROP_get_global: { /* "rabs": d, atom, throwing, site */
      int site = (int)in->arg[3].val;
      int off = fn->values[in->def[0]].reg * 16;
      if ((unsigned)site == REG_IC_NONE) return 0;
      if (g_em.fast && q2_fast_field(f, fn, in, 1)) return 1;
      q2_guard(f, in->pc, q2_site());
      /* The site-cache hit borrows the global's slot. A miss probes the cell
         cache and then reads the global, which can throw; that arm is out of
         line. */
      {
        char cond[160], body[320];
        snprintf(
            cond, sizeof(cond), "q2_global_hit(ctx, pic, %d, &v%u)", site,
            in->def[0]);
        snprintf(
            body, sizeof(body),
            "      int _r;\n"
            "      _r = qjs_aot_get_global_site(ctx, f, %d, "
            "(int)qjs_aot_ld_u32(code + %d), %d, &v%u);\n"
            "      if (_r < 0) { pc = %d; goto exception; }\n"
            "      if (_r) qjs_aot_set_reg_p(ctx, fr, %d, v%u);\n"
            "      else AOT_SET_B(ctx, fr, %d, v%u);\n"
            "      pic = q2_pic(f); holders = q2_pic_holder(f);\n",
            site, in->arg[1].off, (int)in->arg[2].val, in->def[0], in->pc, off,
            in->def[0], off, in->def[0]);
        q2_cold_arm(f, fn, cond, body);
      }
      return 1;
    }
    case ROP_return: {
      /* The returned value may be typed and therefore not in its slot, so it is
         stored first, along with whatever else is live: the value then moves
         out of its register and the frame is finished by the entry that called
         it. */
      int k;
      Q2ValueId rv = in->use[0];
      if (g_em.pure) {
        /* An uncounted result, so the caller's slot need not own it. */
        char r[64];
        q2_use(fn, rv, Q2_R_TAGGED, r, sizeof(r));
        fprintf(
            f,
            "    *out = %s;\n"
            "    if (__builtin_expect(JS_VALUE_GET_TAG(*out) < 0, 0)) return 0;\n"
            "    return 1;\n",
            r);
        return 1;
      }
      q2_live_before(fn, g_em.cur_insn, g_em.liveb);
      for (k = 0; k < in->nuse; k++) {
        int r = fn->values[in->use[k]].reg;
        g_em.liveb[r >> 6] |= 1ull << (r & 63);
      }
      /* `cur` names the returned value in its register; `pre` does not, and the
         return reads the slot. */
      q2_writeback(f, fn, &g_em.cur, g_em.liveb);
      fprintf(
          f,
          "    return qjs_aot_return(fr, %d);\n",
          fn->values[rv].reg * 16);
      return 1;
    }
    case ROP_return_undefined: {
      if (g_em.pure) {
        fprintf(f, "    *out = JS_UNDEFINED;\n    return 1;\n");
        return 1;
      }
      fprintf(f, "    return JS_UNDEFINED;\n");
      return 1;
    }
    case ROP_inc_branch_lt:
    case ROP_inc_branch_lt_s: {
      /* `d = d + 1; if (d < bound) goto target`. The int fast path is the whole
         loop; anything else takes the engine's increment and compare and
         continues compiled. */
      int tgt, sid;
      Q2ValueId dv = in->def[0];
      const Q2Value *dval = &fn->values[dv];
      int off = dval->reg * 16;
      if (!q2_target(in, 2, &tgt)) return 0;
      sid = q2_site();
      q2_guard(f, in->pc, sid);
      if (dval->rep == Q2_R_I32 && in->nuse >= 2 &&
          fn->values[in->use[1]].rep == Q2_R_I32) {
        /* The bound is an int, so the whole test is one compare. A bound of
           any other kind takes the generic path below, where the engine's
           compare handles it. The result is a fresh SSA value, so the
           increment reads the operand, not the destination. */
        char x[64], y[64];
        q2_use_arg(fn, in, 0, Q2_R_I32, x, sizeof(x));
        q2_use_arg(fn, in, 1, Q2_R_I32, y, sizeof(y));
        fprintf(f, "    if (__builtin_expect(%s == INT32_MAX, 0))\n", x);
        q2_miss(f, in->pc, sid);
        fprintf(f, "    i%u = %s + 1;\n", dv, x);
        fprintf(f, "    if (i%u < %s) {", dv, y);
        q2_emit_edge(f, fn, cur_block, 0);
        fprintf(f, " }\n");
        return 1;
      }
      if (dval->rep == Q2_R_I32 && in->nuse >= 2 &&
          fn->values[in->use[1]].rep == Q2_R_TAGGED) {
        /* An int counter against a bound of unknown type (typically an
           argument): test the bound's tag inline, so a number bound costs a
           compare, as in the interpreter's handler. Only a bound that is not a
           number takes the engine's compare below, which can call. */
        char x[64];
        q2_use_arg(fn, in, 0, Q2_R_I32, x, sizeof(x));
        q2_op(in, 1, b, sizeof(b));
        fprintf(f, "    if (__builtin_expect(%s == INT32_MAX, 0))\n", x);
        q2_miss(f, in->pc, sid);
        fprintf(f,
                "    if (__builtin_expect(JS_VALUE_GET_TAG(%s) == JS_TAG_INT, 1) ||\n"
                "        JS_VALUE_GET_TAG(%s) == JS_TAG_FLOAT64) {\n"
                "      i%u = %s + 1;\n"
                "      if (JS_VALUE_GET_TAG(%s) == JS_TAG_INT ? i%u < JS_VALUE_GET_INT(%s) : (double)i%u < JS_VALUE_GET_FLOAT64(%s)) {",
                b, b, dv, x, b, dv, b, dv, b);
        q2_emit_edge(f, fn, cur_block, 0);
        fprintf(f, " }\n    } else {\n");
        q2_writeback(f, fn, &g_em.pre, g_em.liveb);
        q2_op(in, 0, a, sizeof(a));
        fprintf(
            f,
            "    { JSValue _n; int _t;\n"
            "      if (qjs_aot_unary(ctx, QJS_AOT_REG_UN_INC, %s, &_n) < 0) { pc "
            "= %d; goto exception; }\n"
            "      qjs_aot_set_reg_p(ctx, fr, %d, _n);\n"
            "      i%u = JS_VALUE_GET_INT(_n);\n"
            "      _t = qjs_aot_cmp_take(ctx, QJS_AOT_REG_BIN_LT, _n, %s);\n"
            "      if (_t < 0) { pc = %d; goto exception; }\n"
            "      if (_t) {",
            a, in->pc, off, dv, b, in->pc);
        q2_emit_edge(f, fn, cur_block, 0);
        fprintf(f, " } }\n    }\n");
        return 1;
      }
      /* Tagged or typed: the engine's increment and compare, which both
         continue compiled on a non-int. The frame is written back first because
         both can call. */
      q2_writeback(f, fn, &g_em.pre, g_em.liveb);
      q2_op(in, 0, a, sizeof(a));
      q2_op(in, 1, b, sizeof(b));
      fprintf(
          f,
          "    { JSValue _n; int _t;\n"
          "      if (qjs_aot_unary(ctx, QJS_AOT_REG_UN_INC, %s, &_n) < 0) { pc "
          "= %d; goto exception; }\n"
          "      qjs_aot_set_reg_p(ctx, fr, %d, _n);\n",
          a, in->pc, off);
      if (dval->rep == Q2_R_I32)
        fprintf(f, "      i%u = JS_VALUE_GET_INT(_n);\n", dv);
      else if (dval->rep == Q2_R_F64)
        /* the engine returns an int when the result fits */
        fprintf(f,
                "      d%u = JS_VALUE_GET_TAG(_n) == JS_TAG_INT ? "
                "(double)JS_VALUE_GET_INT(_n) : JS_VALUE_GET_FLOAT64(_n);\n",
                dv);
      else
        fprintf(f, "      v%u = _n;\n", dv);
      fprintf(
          f,
          "      _t = qjs_aot_cmp_take(ctx, QJS_AOT_REG_BIN_LT, _n, %s);\n"
          "      if (_t < 0) { pc = %d; goto exception; }\n",
          b, in->pc);
      fprintf(f, "      if (_t) {");
      q2_emit_edge(f, fn, cur_block, 0);
      fprintf(f, " } }\n");
      return 1;
    }
    case ROP_add_imm_self_branch:
    case ROP_add_imm_self_branch_s: {
      /* `d = d + imm; goto target`, with the int fast path first. */
      int tgt, sid;
      Q2ValueId dv = in->def[0];
      const Q2Value *dval = &fn->values[dv];
      int off = dval->reg * 16;
      int32_t imm = (int32_t)in->arg[1].val;
      if (!q2_target(in, 2, &tgt)) return 0;
      sid = q2_site();
      q2_guard(f, in->pc, sid);
      if (dval->rep == Q2_R_I32) {
        char x[64];
        q2_use_arg(fn, in, 0, Q2_R_I32, x, sizeof(x));
        fprintf(
            f,
            "    if (__builtin_expect(__builtin_add_overflow(%s, %d, &i%u), "
            "0))\n",
            x, imm, dv);
        q2_miss(f, in->pc, sid);
        q2_emit_edge(f, fn, cur_block, 0);
        fputc('\n', f);
        return 1;
      }
      q2_writeback(f, fn, &g_em.pre, g_em.liveb);
      q2_op(in, 0, a, sizeof(a));
      fprintf(
          f,
          "    if (qjs_aot_binary(ctx, QJS_AOT_REG_BIN_ADD, %s, "
          "JS_MKVAL(JS_TAG_INT, %d), &v%u)) { pc = %d; goto exception; }\n",
          a, imm, dv, in->pc);
      fprintf(f, "    qjs_aot_set_reg_p(ctx, fr, %d, v%u);\n", off, dv);
      q2_emit_edge(f, fn, cur_block, 0);
      fputc('\n', f);
      return 1;
    }
    case ROP_binary_cpool: {
      /* `d = a <sub> const`: the second operand is a constant-pool entry or an
         atom named by the instruction, so it has no register to read. A numeric
         pool entry folds to a C literal here (R4); anything else goes through
         the engine and continues compiled. An atom is remapped when the
         bytecode is loaded, so it is re-read from the live code, as
         `get_global` does. */
      int sub = (int)in->arg[1].val;
      int kind = (int)in->arg[3].val;
      uint32_t idx = (uint32_t)in->arg[4].val;
      Q2ValueId dv = in->def[0];
      const Q2Value *dval = &fn->values[dv];
      int off = dval->reg * 16;
      double cnum;
      char x[64];
      if (kind == REG_CONST_CPOOL && qjs_aot_pool_num(fn->g, idx, &cnum) &&
          in->nuse >= 1 && fn->values[in->use[0]].rep != Q2_R_TAGGED &&
          (sub == REG_BIN_ADD || sub == REG_BIN_SUB || sub == REG_BIN_MUL ||
           sub == REG_BIN_DIV)) {
        char lit[48];
        const char *opx = sub == REG_BIN_ADD   ? "+"
                          : sub == REG_BIN_SUB ? "-"
                          : sub == REG_BIN_MUL ? "*"
                                               : "/";
        int rep = fn->values[in->use[0]].rep;
        /* %a prints an exact hex float, which round-trips every double
           including -0.0. `%.17g` would print negative zero as "-0", which C
           reads as the integer 0 and so loses the sign, and infinities or NaN
           as "inf"/"nan", which do not compile: those are refused below and
           take the generic path. */
        if (!__builtin_isfinite(cnum)) goto cpool_generic;
        snprintf(lit, sizeof(lit), "%a", cnum);
        /* An int constant is only an int operation when it *is* an int: 0.5 on
           an int operand is a float multiply, so it takes the generic path. */
        if (rep == Q2_R_I32 && (dval->rep == Q2_R_I32 ||
                                dval->rep == Q2_R_TAGGED) &&
            (cnum != (double)(int32_t)cnum || cnum == 0.0 ||
             !(sub == REG_BIN_ADD || sub == REG_BIN_SUB || sub == REG_BIN_MUL))) {
          goto cpool_generic;
        }
        q2_guard(f, in->pc, q2_site());
        if (dval->rep == Q2_R_F64 || dval->rep == Q2_R_I32) {
          if (dval->rep == Q2_R_F64) {
            q2_use_arg(fn, in, 2, Q2_R_F64, x, sizeof(x));
            fprintf(f, "    d%u = %s %s %s;\n", dv, x, opx, lit);
          } else {
            q2_use_arg(fn, in, 2, Q2_R_I32, x, sizeof(x));
            fprintf(f, "    i%u = %s %s (int32_t)%s;\n", dv, x, opx, lit);
          }
        } else {
          /* Tagged result from a typed operand: compute in the operand's
             representation and box, with no engine call. */
          if (rep == Q2_R_I32) {
            q2_use_arg(fn, in, 2, Q2_R_I32, x, sizeof(x));
            fprintf(f, "    v%u = qjs_aot_box_num((double)(%s %s (int32_t)%s), 1);\n",
                    dv, x, opx, lit);
          } else {
            q2_use_arg(fn, in, 2, Q2_R_F64, x, sizeof(x));
            fprintf(
                f,
                "    { double _c = %s %s %s; v%u = qjs_aot_box_num(_c, "
                "qjs_aot_fitsi(_c)); }\n",
                x, opx, lit, dv);
          }
        }
        return 1;
      }
    cpool_generic:
      q2_guard(f, in->pc, q2_site());
      q2_op(in, 2, a, sizeof(a));
      q2_writeback(f, fn, &g_em.pre, g_em.liveb);
      if (kind == REG_CONST_CPOOL) {
        fprintf(
            f,
            "    if (qjs_aot_binary_cpool(ctx, f, %d, %u, %d, %s, &v%u) < 0) { "
            "pc = %d; goto exception; }\n",
            sub, idx, kind, a, dv, in->pc);
      } else {
        fprintf(
            f,
            "    if (qjs_aot_binary_cpool(ctx, f, %d, "
            "(int)qjs_aot_ld_u32(code + %d), %d, %s, &v%u) < 0) { pc = %d; goto "
            "exception; }\n",
            sub, in->arg[4].off, kind, a, dv, in->pc);
      }
      fprintf(f, "    qjs_aot_set_reg_p(ctx, fr, %d, v%u);\n", off, dv);
      return 1;
    }
    case ROP_put_element:
    case ROP_put_element_xfer: { /* "rrr": object, key, value */
      /* A dense array's existing element, stored inline; anything else
         (a hole, a counted value, another class, a setter) is the step. */
      char c[64];
      if (g_em.pure || g_em.fast) return 0;
      q2_op(in, 0, a, sizeof(a));
      q2_op(in, 1, b, sizeof(b));
      q2_op(in, 2, c, sizeof(c));
      fprintf(f, "    if (__builtin_expect(!q2_elem_put(%s, %s, %s), 0)) {\n",
              a, b, c);
      q2_emit_fallback(f, fn, cur_block, in, fn->exit_pc);
      fprintf(f, "    }\n");
      /* the step's write-back ran only on the slow arm */
      q2_fb_reset(fn);
      /* A step ends its block: the slow arm left through the block's edge, so
         the inline store must take the same edge, with its parameter moves. */
      {
        const Q2Block *blk = &fn->blocks[cur_block];
        if (in == &fn->insns[blk->first_insn + blk->ninsns - 1]) {
          if (blk->nsucc >= 1) {
            fprintf(f, "    {");
            q2_emit_edge(f, fn, cur_block, 0);
            fprintf(f, " }\n");
          } else if (in->next >= fn->exit_pc) {
            fprintf(f, "    return qjs_aot_ret(f);\n");
          }
        }
      }
      return 1;
    }
    case ROP_neg:
    case ROP_inc: { /* "rr": d, a */
      int sub = in->rop == ROP_neg ? REG_UN_NEG : REG_UN_INC;
      int off = fn->values[in->def[0]].reg * 16;
      int rep = fn->values[in->def[0]].rep;
      if (q2_inc_by_operand(fn, in) && (rep == Q2_R_I32 || rep == Q2_R_F64)) {
        /* Typed from its operand: an int increment that exits on overflow, or
           a double one. Nothing goes to the frame. */
        int sid = q2_site();
        char x[64];
        q2_guard(f, in->pc, sid);
        q2_use_arg(fn, in, 1, rep, x, sizeof(x));
        if (rep == Q2_R_I32) {
          fprintf(f, "    if (__builtin_expect(%s == INT32_MAX, 0)) {\n", x);
          q2_miss(f, in->pc, sid);
          fprintf(f, "    }\n    i%u = %s + 1;\n", in->def[0], x);
        } else {
          fprintf(f, "    d%u = %s + 1.0;\n", in->def[0], x);
        }
        return 1;
      }
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
    case ROP_get_this_field:
    case ROP_get_this_field2: {
      /* `recv = this; d = recv.atom`. The read is the same inline own-slot hit
         as `get_field`, on a receiver that is the frame's `this`. */
      int site = (int)in->arg[3].val;
      int offr = fn->values[in->def[1]].reg * 16;
      int offd = fn->values[in->def[0]].reg * 16;
      if ((unsigned)site == REG_IC_NONE) return 0;
      q2_guard(f, in->pc, q2_site());
      if (g_em.pure == 2) {
        fprintf(
            f,
            "    if (JS_VALUE_GET_TAG(this_obj) != JS_TAG_OBJECT) return 0;\n"
            "    v%u = this_obj;\n",
            in->def[1]);
        q2_leaf_slot_read(f, fn, in, in->def[1], site, in->def[0]);
        (void)offr;
        (void)offd;
        return 1;
      }
      if (g_em.pure) {
        /* An object `this` is the same strict or not. */
        fprintf(
            f,
            "    if (JS_VALUE_GET_TAG(this_obj) != JS_TAG_OBJECT) return 0;\n"
            "    v%u = this_obj;\n"
            "    if (__builtin_expect(!q2_own_read_hit(pic, holders, this_obj, "
            "%d, (uint32_t)q2_pic_sites(f), &v%u), 0)) return 0;\n",
            in->def[1], site, in->def[0]);
        (void)offr;
        (void)offd;
        return 1;
      }
      fprintf(
          f,
          "    { JSValue _t; int _r = qjs_aot_this(ctx, f, &_t);\n"
          "      if (_r < 0) { pc = %d; goto exception; }\n",
          in->pc);
      q2_writeback(f, fn, &g_em.pre, g_em.liveb);
      fprintf(
          f,
          "      if (_r) qjs_aot_set_reg_p(ctx, fr, %d, _t);\n"
          "      else AOT_SET_B(ctx, fr, %d, _t);\n"
          "      v%u = _t;\n"
          "      if (__builtin_expect(!q2_own_read_hit(pic, holders, _t, %d, q2_pic_sites(f), "
          "&v%u), 0)) {\n"
          "        if (qjs_aot_read_field_slow(ctx, f, code + %d, _t, "
          "(int)qjs_aot_ld_u32(code + %d), %d, &v%u) < 0) { pc = %d; goto "
          "exception; }\n"
          "        pic = q2_pic(f); holders = q2_pic_holder(f);\n"
          "        qjs_aot_set_reg_p(ctx, fr, %d, v%u);\n"
          "      } else {\n"
          "        AOT_SET_B(ctx, fr, %d, v%u);\n"
          "      }\n"
          "    }\n",
          offr, offr, in->def[1], site, in->def[0], in->pc,
          in->arg[2].off, site, in->def[0], in->pc, offd, in->def[0], offd,
          in->def[0]);
      return 1;
    }
    case ROP_get_element: {
      int sid = q2_site();
      int ns_pcs = 0;
      int off = fn->values[in->def[0]].reg * 16;
      q2_guard(f, in->pc, sid);
      /* The specialised body reads a plain fast array's element inline: a
         class check, a bounds check, and the hole and lazy-marker tests. A
         miss (another class, out of bounds, a hole, a lazy JSON marker, or the
         object array underneath) exits at this instruction. */
      if (g_em.spec) {
        /* A pc whose profile says every element was a number reads with the
           numeric hit: no hole test, no lazy test, one tag check for the
           number itself. */
        const ProfPc *pp = qjs_aot_prof_pcs(fn->g->digest, &ns_pcs);
        int numeric = 0;
        {
          int q;
          for (q = 0; q < ns_pcs; q++)
            if (pp[q].pc == in->pc) {
              numeric = (pp[q].elem == 1u || pp[q].elem == 2u);
              break;
            }
        }
        fprintf(
            f,
            "    if (__builtin_expect(!%s(%s, %s, &v%u), 0)) {\n",
            numeric ? "q2_elem_num" : "q2_elem_hit", q2_op(in, 1, a, sizeof(a)),
            q2_op(in, 2, b, sizeof(b)), in->def[0]);
        q2_miss(f, in->pc, sid);
        fprintf(f, "    }\n");
        /* The element is borrowed from the array, so nothing is adopted here:
           the local holds it and the next write-back duplicates it, exactly as
           the inline field-read hit leaves its slot alone. */
        (void)off;
        return 1;
      }
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
      /* A boolean is not a number: `1 === true` is false while `1 == 1` is
         true, and `true < 2` compares as 1 but is not an int. A boolean
         operand stays tagged and takes the generic compare, which decides by
         tag. */
      if (in->nuse >= 2 &&
          fn->values[in->use[0]].rep != Q2_R_TAGGED &&
          fn->values[in->use[0]].rep != Q2_R_BOOL &&
          fn->values[in->use[1]].rep != Q2_R_TAGGED &&
          fn->values[in->use[1]].rep != Q2_R_BOOL &&
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
      q2_op(in, 1, a, sizeof(a));
      q2_op(in, 2, b, sizeof(b));
      /* A miss is not a deopt: mixed tags, strings, objects and `==` coercion
         take the interpreter's generic compare and stay compiled. It can run
         valueOf, so the frame is written back first and it can throw. The arm
         is out of line; `_t` is the result either way. */
      {
        char cond[160], body[224];
        /* `_t` must live in the function scope, not this block: the out-of-line
           arm is printed after the body and writes the same variable. */
        snprintf(
            cond, sizeof(cond), "q2_branch_cmp(%d, %s, %s, &_t)", s, a, b);
        snprintf(
            body, sizeof(body),
            "      _t = qjs_aot_cmp_take(ctx, %d, %s, %s);\n"
            "      if (_t < 0) { pc = %d; goto exception; }\n",
            s, a, b, in->pc);
        q2_cold_arm(f, fn, cond, body);
      }
      fprintf(f, "      if (_t) {");
      q2_emit_edge(f, fn, cur_block, 0);
      fprintf(f, " }\n");
      return 1;
    }
    default:
      return 0;
  }
}

/* One block's instructions and its fall-through edge, after its label. */
static void q2_emit_block(FILE *f, const Q2Func *fn, int b) {
  const Q2Block *blk = &fn->blocks[b];
  int j;
  g_em.cur_block = b;
  /* Each block starts from its own live-in values, never from whatever block
     happened to be emitted before it. The frame cache is reset for the same
     reason: a block can be reached from any predecessor, so what the slot
     holds is unknown until this block writes it. */
  q2_cur_block(fn, &g_em.cur, b);
  /* The facts true at this block's entry; q2_emit_native adds the ones each
     instruction establishes as it emits. */
  if (g_em.fact_cur && g_em.fact_in && g_em.fact_words > 0)
    memcpy(
        g_em.fact_cur, g_em.fact_in + (size_t)b * g_em.fact_words,
        (size_t)g_em.fact_words * 8);
  else if (g_em.fact_cur && g_em.fact_words > 0)
    memset(g_em.fact_cur, 0, (size_t)g_em.fact_words * 8);
  if (g_em.poly_way && fn->nvalues > 0)
    memset(g_em.poly_way, 0xff, (size_t)fn->nvalues);
  /* The frame cache survives into a block only when that block has exactly
     one predecessor and it is the block just emitted, i.e. a plain
     fall-through. Any other entry (a branch target, a loop header) can be
     reached from somewhere the cache knows nothing about. */
  {
    const Q2Block *pb = &fn->blocks[b];
    int keep = 0;
    if (b > 0 && pb->npreds == 1 && pb->preds[0] == b - 1 &&
        fn->blocks[b - 1].nsucc >= 1 &&
        fn->blocks[b - 1].succ_block[0] == b &&
        fn->blocks[b - 1].end_pc == pb->start_pc)
      keep = 1;
    if (!keep) q2_fb_reset(fn);
  }
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
    /* A pc the profiler never executed needs no body: the specialised body
       exits at it and the interpreter runs it. This is Phase 6's code-size
       rule, and it is also a correctness neutral win -- the first execution
       pays one deopt. */
    if (g_em.spec && !g_em.fast && qjs_aot_prof_unexecuted(fn->g->digest, in->pc)) {
      q2_miss(f, in->pc, q2_site());
      continue;
    }
    if ((in->rop == ROP_call || in->rop == ROP_call_method) &&
        (g_em.fast || g_em.gen_loop)) {
      /* A call of a copied loop: the leaf in the copy, and the place the copy
         continues from in the generic one. */
      if (g_em.fast) {
        q2_fast_call(f, fn, b, in);
        continue;
      }
      fprintf(f, "gi_%d:\n", g_em.cur_insn);
    }
    /* A call or a step can replace every shape a fact is about. */
    if (g_em.fact_cur && g_em.fact_words > 0 &&
        (in->rop == ROP_call || in->rop == ROP_call_method ||
         in->rop == ROP_throw || in->op == Q2_OP_FALLBACK)) {
      memset(g_em.fact_cur, 0, (size_t)g_em.fact_words * 8);
      if (g_em.poly_way && fn->nvalues > 0)
        memset(g_em.poly_way, 0xff, (size_t)fn->nvalues);
    }
    if (q2_emit_native(f, fn, b, in) != 0) {
      /* lowered */
    } else if (q2_emit_call_special(f, fn, b, in)) {
      q2_emit_step_body(f, fn, b, in, fn->exit_pc, in->pc);
    } else {
      q2_emit_fallback(f, fn, b, in, fn->exit_pc);
    }
    if (!q2_fast_direct(fn, in))
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

/* Whether block `h` heads a loop the copy can take: blocks h..*hi, whose last
   ends with the one back edge to h, that are entered only through h and hold
   no other back edge. */
static int q2_loop_range(const Q2Func *fn, int h, int *hi) {
  int b, k, last = -1;
  for (b = h; b < fn->nblocks; b++)
    for (k = 0; k < fn->blocks[b].nsucc; k++)
      if (fn->blocks[b].succ_block[k] == h) last = b;
  if (last < 0) return 0;
  for (b = h; b <= last; b++) {
    const Q2Block *blk = &fn->blocks[b];
    if (b > h)
      for (k = 0; k < blk->npreds; k++)
        if (blk->preds[k] < h || blk->preds[k] > last) return 0;
    for (k = 0; k < blk->nsucc; k++) {
      int t = blk->succ_block[k];
      if (t >= h && t <= last && t <= b && t != h) return 0;
      if (t == h && b != last) return 0;
    }
  }
  *hi = last;
  return 1;
}

/* Whether the emitted loop copy calls only what cannot run user code or
   return into the loop with a shape changed: the tag and box macros, the pure
   inline number helpers, and the exits (a leave, the poll, the exception
   path). Anything else -- an engine helper, a cache refill, a step -- drops the
   copy. */
static int q2_scan(const char *t, const char *const *ok);
/* Whether the scan running accepts calls to leaf bodies (the loop copy's). */
static int q2_scan_leaves;

static int q2_fast_scan(const char *t) {
  int r;
  static const char *const ok[] = {
      "JS_VALUE_GET_TAG", "JS_VALUE_GET_INT", "JS_VALUE_GET_FLOAT64",
      "JS_VALUE_GET_PTR", "JS_VALUE_GET_BOOL", "JS_MKVAL", "JS_TAG_IS_FLOAT64",
      "JS_VALUE_IS_BOTH_INT", "JS_VALUE_IS_BOTH_FLOAT", "__JS_NewFloat64",
      "AOT_SET_B", "AOT_REG", "__builtin_expect", "__builtin_add_overflow",
      "__builtin_sub_overflow", "__builtin_mul_overflow", "if", "while",
      "for", "return", "sizeof", "qjs_aot_ret", "qjs_aot_deopt",
      "qjs_aot_ld_u32", "qjs_aot_fitsi", "qjs_aot_box_num",
      "qjs_aot_should_deopt", "qjs_aot_poll", "q2_num_fast", "q2_unary_fast",
      "q2_cmp_int", "q2_cmp_double", "q2_branch_cmp", "int", "double",
      "int32_t", "uint32_t", "JSValue", "unsigned", "q2_leaf_callee",
      /* the counted build's register move: a release, never user code */
      "qjs_aot_move_xfer", NULL};
  q2_scan_leaves = 1;
  r = q2_scan(t, ok);
  q2_scan_leaves = 0;
  return r;
}

/* Whether every call in `t` names one of `ok`. */
static int q2_scan(const char *t, const char *const *ok) {
  const char *p;
  for (p = t; *p; p++) {
    const char *q;
    size_t n;
    int i, good = 0;
    if (*p != '(') continue;
    q = p;
    while (q > t && (q[-1] == ' ')) q--;
    n = 0;
    while (q > t && (isalnum((unsigned char)q[-1]) || q[-1] == '_')) q--, n++;
    if (n == 0) continue; /* a parenthesis, not a call */
    for (i = 0; ok[i]; i++)
      if (strlen(ok[i]) == n && !memcmp(ok[i], q, n)) good = 1;
    /* a leaf body: it passed the same kind of scan when it was emitted */
    if (q2_scan_leaves && n > 6 &&
        (!memcmp(q, "aot_p_", 6) || !memcmp(q, "aot_r_", 6)))
      good = 1;
    if (!good) return 0;
  }
  return 1;
}

/* The instructions of the loop being copied: [*lo, *hi). */
static void q2_fast_insns(const Q2Func *fn, int *lo, int *hi) {
  *lo = fn->blocks[g_em.fast_lo].first_insn;
  *hi = fn->blocks[g_em.fast_hi].first_insn + fn->blocks[g_em.fast_hi].ninsns;
}

/* The preheader of the loop copy at `b_<pc>`: check every field access's cache
   entry, resolve its slot, read the globals and the invariant reads, and enter
   the copy at `ff_<pc>`; anything unexpected runs the generic copy at
   `g_<pc>`. A small budget per activation stops a loop whose guards never
   hold from paying the checks on every iteration. */
static void q2_emit_preheader(FILE *f, const Q2Func *fn) {
  int pc = fn->blocks[g_em.fast_lo].start_pc, ix, w, lo, hi;
  char a[64];
  q2_fast_insns(fn, &lo, &hi);
  fprintf(f, "    if (fl_%d > 0) {\n", pc);
  fprintf(f, "      const unsigned char *_e, *_o;\n");
  fprintf(f, "      if (pic == NULL) goto fx_%d;\n", pc);
  for (ix = lo; ix < hi; ix++) {
    const Q2Insn *in = &fn->insns[ix];
    int read = q2_fast_access(fn, in), site;
    if (read < 0) continue;
    if (read == 2) {
      /* A global the loop reads: nothing in the copy can write it. */
      fprintf(f, "      if (!q2_global_hit(ctx, pic, %d, &fv%d)) goto fx_%d;\n",
              (int)in->arg[3].val, ix, pc);
      continue;
    }
    site = (int)in->arg[read ? 3 : 2].val;
    q2_use(fn, in->argval[read ? 1 : 0], Q2_R_TAGGED, a, sizeof(a));
    fprintf(
        f,
        "      if (JS_VALUE_GET_TAG(%s) != JS_TAG_OBJECT) goto fx_%d;\n"
        "      _o = (const unsigned char *)JS_VALUE_GET_PTR(%s);\n"
        "      _e = (const unsigned char *)pic + (size_t)%d * "
        "QJS_AOT_SIZEOF_JSREGPIC;\n"
        "      if (*(const void *const *)_e != *(const void *const *)(_o + "
        "QJS_AOT_OFF_JSOBJECT_SHAPE)) goto fx_%d;\n",
        a, pc, a, site, pc);
    if (read)
      /* An own slot, or the prototype holder's, as the inline hit reads. */
      fprintf(
          f,
          "      if (*(const uint16_t *)(_e + QJS_AOT_OFF_JSREGPIC_KIND) != "
          "QJS_AOT_REG_PIC_OWN) {\n"
          "        _o = *(const unsigned char *const *)(*(const unsigned char "
          "*const *)(_o + QJS_AOT_OFF_JSOBJECT_SHAPE) + "
          "QJS_AOT_OFF_JSSHAPE_PROTO);\n"
          "        if (_o == NULL || holders == NULL || *(const void *const "
          "*)(_o + QJS_AOT_OFF_JSOBJECT_SHAPE) != holders[%d]) goto fx_%d;\n"
          "      }\n",
          site, pc);
    else
      fprintf(
          f,
          "      if (*(const uint16_t *)(_e + QJS_AOT_OFF_JSREGPIC_KIND) != "
          "QJS_AOT_REG_PIC_OWN_W ||\n"
          "          (_o[QJS_AOT_OFF_JSOBJECT_FLAGS_BYTE] & "
          "QJS_AOT_JSOBJECT_IS_PROTOTYPE_MASK) != 0) goto fx_%d;\n",
          pc);
    fprintf(
        f,
        "      fs%d = (JSValue *)(*(unsigned char *const *)(_o + "
        "QJS_AOT_OFF_JSOBJECT_PROP) + *(const uint32_t *)(_e + "
        "QJS_AOT_OFF_JSREGPIC_SLOT_OFF));\n",
        ix);
    if (read)
      fprintf(
          f,
          "      if (JS_VALUE_GET_TAG(*fs%d) == QJS_AOT_TAG_LAZY_JSON) goto "
          "fx_%d;\n"
          "#if !QJS_DRC\n"
          "      if (JS_VALUE_GET_TAG(*fs%d) < 0) goto fx_%d;\n"
          "#endif\n",
          ix, pc, ix, pc);
    else
      fprintf(f, "      if (JS_VALUE_GET_TAG(*fs%d) < 0) goto fx_%d;\n", ix, pc);
  }
  /* An invariant read must not share its slot with any write in the loop. */
  for (ix = lo; ix < hi; ix++) {
    const Q2Insn *in = &fn->insns[ix];
    int acc = q2_fast_access(fn, in);
    if ((acc != 1 && acc != 2) || !g_em.fhoist[ix]) continue;
    if (acc == 1) {
      for (w = lo; w < hi; w++) {
        if (q2_fast_access(fn, &fn->insns[w]) != 0) continue;
        fprintf(f, "      if (fs%d == fs%d) goto fx_%d;\n", ix, w, pc);
      }
      fprintf(f, "      fv%d = *fs%d;\n", ix, ix);
    }
    if (fn->values[in->def[0]].spec && fn->values[in->def[0]].rep == Q2_R_F64)
      fprintf(
          f,
          "      if (JS_VALUE_GET_TAG(fv%d) == JS_TAG_INT) fvd%d = "
          "JS_VALUE_GET_INT(fv%d);\n"
          "      else if (JS_VALUE_GET_TAG(fv%d) == JS_TAG_FLOAT64) fvd%d = "
          "JS_VALUE_GET_FLOAT64(fv%d);\n"
          "      else goto fx_%d;\n",
          ix, ix, ix, ix, ix, ix, pc);
  }
  /* The leaf calls whose callee and receivers the loop holds fixed. */
  for (ix = lo; ix < hi; ix++) q2_fast_rcall_pre(f, fn, &fn->insns[ix], ix, pc);
  /* A forwarded read shares its group's slot; the group starts from what the
     slot holds now, a number when its reads take the double. */
  for (ix = lo; ix < hi; ix++) {
    int g = g_em.fgroup[ix], acc;
    if (g < 0) continue;
    acc = q2_fast_access(fn, &fn->insns[ix]);
    if (acc == 1) {
      fprintf(f, "      if (fs%d != fs%d) goto fx_%d;\n", ix, g, pc);
    } else if (acc == 0 && g == ix) {
      fprintf(f, "      fc%d = *fs%d;\n", ix, ix);
      if (g_em.fdbl[ix])
        fprintf(
            f,
            "      if (JS_VALUE_GET_TAG(fc%d) == JS_TAG_INT) fcd%d = "
            "JS_VALUE_GET_INT(fc%d);\n"
            "      else if (JS_VALUE_GET_TAG(fc%d) == JS_TAG_FLOAT64) fcd%d = "
            "JS_VALUE_GET_FLOAT64(fc%d);\n"
            "      else goto fx_%d;\n",
            ix, ix, ix, ix, ix, ix, pc);
    }
  }
  fprintf(f, "      goto ff_%d;\n", pc);
  fprintf(f, "    fx_%d:\n      fl_%d--;\n    }\n", pc, pc);
  fprintf(f, "    goto g_%d;\n", pc);
}

/* Which reads of the loop copy are invariant, and which take a written value.
   A read of a receiver and property that a write in the loop names takes the
   value that write group last stored; any other read is invariant, and the
   preheader rules out a write to the same property of another receiver that
   is the same object by comparing the slots. */
static void q2_fast_hoist(const Q2Func *fn) {
  int lo, hi, ix, w;
  q2_fast_insns(fn, &lo, &hi);
  for (ix = lo; ix < hi; ix++) {
    g_em.fhoist[ix] = 0;
    g_em.fgroup[ix] = -1;
    g_em.fdbl[ix] = 0;
  }
  /* Writes: the group is the first write of the same receiver and property. */
  for (ix = lo; ix < hi; ix++) {
    const Q2Insn *in = &fn->insns[ix];
    if (q2_fast_access(fn, in) != 0) continue;
    for (w = lo; w <= ix; w++) {
      const Q2Insn *wn = &fn->insns[w];
      if (q2_fast_access(fn, wn) != 0) continue;
      if (wn->argval[0] == in->argval[0] && wn->arg[1].val == in->arg[1].val) {
        g_em.fgroup[ix] = w;
        break;
      }
    }
  }
  for (ix = lo; ix < hi; ix++) {
    if (q2_fast_access(fn, &fn->insns[ix]) == 0 && g_em.fgroup[ix] == ix) {
      int typed = 1;
      for (w = lo; w < hi; w++)
        if (g_em.fgroup[w] == ix && q2_fast_access(fn, &fn->insns[w]) == 0 &&
            !q2_typed(&fn->values[fn->insns[w].argval[3]]))
          typed = 0;
      g_em.fdbl[ix] = (uint8_t)typed;
    }
  }
  for (ix = lo; ix < hi; ix++) {
    const Q2Insn *in = &fn->insns[ix];
    int acc = q2_fast_access(fn, in), hoist = 1;
    if (acc == 2) {
      g_em.fhoist[ix] = 1;
      continue;
    }
    if (acc != 1) continue;
    for (w = lo; w < hi; w++) {
      const Q2Insn *wn = &fn->insns[w];
      if (q2_fast_access(fn, wn) != 0) continue;
      if (wn->argval[0] == in->argval[1] && wn->arg[1].val == in->arg[2].val) {
        hoist = 0;
        g_em.fgroup[ix] = g_em.fgroup[w];
      }
    }
    g_em.fhoist[ix] = (uint8_t)hoist;
  }
}

/* Emit blocks lo..hi as a loop copy and a generic copy if the copy passes the
   scan; returns 0, having emitted nothing, if it does not. */
static int q2_emit_fast_loop(FILE *f, const Q2Func *fn, int lo, int hi) {
  char *buf = NULL;
  size_t len = 0;
  FILE *m;
  int pc = fn->blocks[lo].start_pc, ok, b;
  if (qjs_aot_check_locals()) return 0;
  m = open_memstream(&buf, &len);
  if (!m) return 0;
  g_em.fast_lo = lo;
  g_em.fast_hi = hi;
  q2_fast_hoist(fn);
  g_em.fast = 1;
  /* A loop-copy trial may collect exits and cold arms; if the copy is
     rejected they are abandoned along with its text. */
  {
    int ex0 = g_em.nexits, c0 = g_em.ncolds;
    for (b = lo; b <= hi; b++) {
      fprintf(m, "ff_%d:\n", fn->blocks[b].start_pc);
      q2_emit_block(m, fn, b);
    }
    g_em.fast = 0;
    fclose(m);
    ok = buf && q2_fast_scan(buf);
    if (!ok) {
      int i;
      for (i = ex0; i < g_em.nexits; i++) {
        free(g_em.exits[i].st_reg);
        free(g_em.exits[i].st_val);
        free(g_em.exits[i].chk_reg);
        free(g_em.exits[i].chk_val);
      }
      g_em.nexits = ex0;
      for (i = c0; i < g_em.ncolds; i++) {
        if (g_em.colds[i].m) fclose(g_em.colds[i].m);
        free(g_em.colds[i].buf);
      }
      g_em.ncolds = c0;
    }
  }
  if (ok) {
    fprintf(f, "b_%d:\n", pc);
    q2_emit_preheader(f, fn);
    fputs(buf, f);
    g_em.gen_loop = 1;
    for (b = lo; b <= hi; b++) {
      if (b == lo)
        fprintf(f, "g_%d:\n", pc);
      else
        fprintf(f, "b_%d:\n", fn->blocks[b].start_pc);
      q2_emit_block(f, fn, b);
    }
    g_em.gen_loop = 0;
  }
  free(buf);
  return ok;
}

/* Number the shape groups of a specialised body. A site is in a group only when
   its profile record is monomorphic, its pc ran, and its cache cell is an own
   or own-writable hit; sites whose receiver shares the profile shape share the
   group, so one entry check can serve all of them. */
/* Whether a site should use the polymorphic cache: the profile saw 2-4 shapes
   there, and the site's pc ran. */
static int q2_poly_site(const Q2Func *fn, int site) {
  int ns = 0;
  uint64_t shapes[4];
  if (!g_em.spec || !g_em.sexec) return 0;
  if ((unsigned)site >= (unsigned)fn->ninsns || !g_em.sexec[site]) return 0;
  if (!qjs_aot_prof_site(fn->g->digest, site, &ns, shapes)) return 0;
  return ns >= 2 && ns <= 4;
}

/* Whether the pair (value, group) is known at the instruction being emitted. */
static int q2_fact_has(const Q2Insn *in, Q2ValueId v, int g) {
  int k;
  (void)in;
  if (!g_em.fact_cur || v == Q2_NO_VALUE) return 0;
  for (k = 0; k < g_em.nfacts; k++)
    if (g_em.fact_val[k] == (int32_t)v && g_em.fact_grp[k] == g)
      return (g_em.fact_cur[k >> 6] >> (k & 63)) & 1;
  return 0;
}

/* Shape facts (T1.7): a (receiver value, group) pair that a check has already
   established. The pairs are numbered once per function, and each block carries
   a bitset of the pairs true at its entry: the intersection over its
   predecessors, since a fact may only be relied on if every way in
   established it. A read in a group generates its pair; a call or a step
   kills them all (either can replace the shape the pair is about). */
static void q2_facts_build(const Q2Func *fn) {
  int i, b, npairs = 0, words;
  int32_t *pair_val = NULL; /* pair -> receiver value */
  int32_t *pair_grp = NULL; /* pair -> group */
  g_em.nfacts = 0;
  g_em.fact_words = (fn->ninsns > 0 ? 1 : 0);
  if (!g_em.sgroup) return;
  for (i = 0; i < fn->ninsns; i++) {
    const Q2Insn *in = &fn->insns[i];
    int site = -1;
    if (in->rop == ROP_get_field || in->rop == ROP_get_field_own ||
        in->rop == ROP_get_field2 || in->rop == ROP_get_field2_own)
      site = (int)in->arg[3].val;
    else if (in->rop == ROP_put_field)
      site = (int)in->arg[2].val;
    if ((unsigned)site == REG_IC_NONE || site < 0) continue;
    if (!g_em.sgroup || (size_t)site >= g_em.nsites) continue;
    if (g_em.sgroup[site] < 0 || !g_em.sexec[site]) continue;
    if (in->narg < 1 || in->arg[1 - (in->rop == ROP_put_field ? 1 : 0)].kind != 'r')
      continue;
    npairs++;
  }
  if (npairs <= 0) return;
  pair_val = malloc(sizeof(*pair_val) * (size_t)npairs);
  pair_grp = malloc(sizeof(*pair_grp) * (size_t)npairs);
  if (!pair_val || !pair_grp) {
    free(pair_val);
    free(pair_grp);
    return;
  }
  npairs = 0;
  for (i = 0; i < fn->ninsns; i++) {
    const Q2Insn *in = &fn->insns[i];
    int site = -1, op;
    Q2ValueId rv;
    if (in->rop == ROP_get_field || in->rop == ROP_get_field_own ||
        in->rop == ROP_get_field2 || in->rop == ROP_get_field2_own) {
      site = (int)in->arg[3].val;
      op = 1;
    } else if (in->rop == ROP_put_field) {
      site = (int)in->arg[2].val;
      op = 0;
    } else
      continue;
    if ((unsigned)site == REG_IC_NONE || site < 0) continue;
    if (!g_em.sgroup || (size_t)site >= g_em.nsites) continue;
    if (g_em.sgroup[site] < 0 || !g_em.sexec[site]) continue;
    if (op >= in->narg || in->arg[op].kind != 'r') continue;
    rv = in->argval[op];
    if (rv == Q2_NO_VALUE) continue;
    pair_val[npairs] = (int32_t)rv;
    pair_grp[npairs] = g_em.sgroup[site];
    npairs++;
  }
  words = (npairs + 63) / 64;
  g_em.nfacts = npairs;
  g_em.fact_words = words;
  g_em.fact_val = pair_val;
  g_em.fact_grp = pair_grp;
  /* Per block: the facts true at entry, and the ones leaving it. */
  g_em.fact_in = calloc((size_t)(fn->nblocks > 0 ? fn->nblocks : 1) *
                            (size_t)words,
                        sizeof(uint64_t));
  g_em.fact_out = calloc((size_t)(fn->nblocks > 0 ? fn->nblocks : 1) *
                             (size_t)words,
                         sizeof(uint64_t));
  g_em.fact_scratch = calloc((size_t)words, sizeof(uint64_t));
  if (!g_em.fact_in || !g_em.fact_out || !g_em.fact_scratch) {
    free(g_em.fact_in);
    free(g_em.fact_out);
    free(g_em.fact_scratch);
    g_em.fact_in = g_em.fact_out = g_em.fact_scratch = NULL;
    g_em.nfacts = 0;
    return;
  }
  /* out[b] = (in[b] U gen) - kill, over the block's instructions in order. */
  for (b = 0; b < fn->nblocks; b++) {
    const Q2Block *blk = &fn->blocks[b];
    uint64_t *out = g_em.fact_out + (size_t)b * words;
    int j, k;
    for (j = 0; j < blk->ninsns; j++) {
      const Q2Insn *in = &fn->insns[blk->first_insn + j];
      if (in->rop == ROP_call || in->rop == ROP_call_method) {
        memset(out, 0, (size_t)words * 8);
        continue;
      }
      {
        int site = -1, op = 1;
        if (in->rop == ROP_get_field || in->rop == ROP_get_field_own ||
            in->rop == ROP_get_field2 || in->rop == ROP_get_field2_own)
          site = (int)in->arg[3].val;
        else if (in->rop == ROP_put_field) {
          site = (int)in->arg[2].val;
          op = 0;
        }
        if ((unsigned)site == REG_IC_NONE || site < 0) continue;
        if (!g_em.sgroup || (size_t)site >= g_em.nsites) continue;
        if (g_em.sgroup[site] < 0 || !g_em.sexec[site]) continue;
        if (op >= in->narg || in->arg[op].kind != 'r') continue;
        for (k = 0; k < npairs; k++)
          if (g_em.fact_val[k] == (int32_t)in->argval[op] &&
              g_em.fact_grp[k] == g_em.sgroup[site])
            out[k >> 6] |= 1ull << (k & 63);
      }
    }
  }
  /* The intersection fixpoint. Entry starts from nothing (a fact must be
     established on every path), and the other blocks from all-ones. */
  {
    int changed = 1, guard = 0;
    for (b = 1; b < fn->nblocks; b++)
      memset(
          g_em.fact_in + (size_t)b * words, 0xff, (size_t)words * 8);
    while (changed && guard++ < 64) {
      changed = 0;
      for (b = 0; b < fn->nblocks; b++) {
        uint64_t *in = g_em.fact_in + (size_t)b * words;
        const Q2Block *blk = &fn->blocks[b];
        int p, w;
        if (b == 0) continue;
        if (blk->npreds <= 0) continue;
        for (w = 0; w < words; w++) {
          uint64_t m = ~0ull;
          for (p = 0; p < blk->npreds; p++) {
            int pb = blk->preds[p];
            if (pb < 0 || pb >= fn->nblocks) {
              m = 0;
              break;
            }
            m &= g_em.fact_out[(size_t)pb * words + w];
          }
          if (in[w] != m) {
            in[w] = m;
            changed = 1;
          }
        }
      }
    }
  }
}

static void q2_groups(const Q2Func *fn) {
  int i, k, nsites = 0, n = 0;
  g_em.sgroup = NULL;
  g_em.sexec = NULL;
  g_em.nsites = 0;
  g_em.ngroups = 0;
  for (i = 0; i < fn->ninsns; i++) {
    const Q2Insn *in = &fn->insns[i];
    int site = -1;
    if (in->rop == ROP_get_field || in->rop == ROP_get_field_own ||
        in->rop == ROP_get_field2 || in->rop == ROP_get_field2_own ||
        in->rop == ROP_get_global)
      site = (int)in->arg[3].val;
    else if (in->rop == ROP_put_field)
      site = (int)in->arg[2].val;
    if ((unsigned)site != REG_IC_NONE && site + 1 > nsites) nsites = site + 1;
  }
  if (nsites <= 0) return;
  g_em.nsites = (size_t)nsites;
  g_em.sgroup = malloc(sizeof(*g_em.sgroup) * (size_t)nsites);
  g_em.sexec = malloc((size_t)nsites);
  if (!g_em.sgroup || !g_em.sexec) {
    free(g_em.sgroup);
    free(g_em.sexec);
    g_em.sgroup = NULL;
    g_em.sexec = NULL;
    return;
  }
  for (k = 0; k < nsites; k++) {
    g_em.sgroup[k] = -1;
    g_em.sexec[k] = 1;
  }
  for (i = 0; i < fn->ninsns; i++) {
    const Q2Insn *in = &fn->insns[i];
    int site = -1, ns, g = -1, j;
    uint64_t shapes[4], sh[4];
    if (in->rop == ROP_get_field || in->rop == ROP_get_field_own ||
        in->rop == ROP_get_field2 || in->rop == ROP_get_field2_own ||
        in->rop == ROP_get_global)
      site = (int)in->arg[3].val;
    else if (in->rop == ROP_put_field)
      site = (int)in->arg[2].val;
    if ((unsigned)site == REG_IC_NONE || site < 0) continue;
    if (qjs_aot_prof_unexecuted(fn->g->digest, in->pc)) {
      if ((size_t)site >= g_em.nsites) {
        fprintf(stderr, "GROUPS OOB site=%d nsites=%zu ninsns=%d\n", site,
                g_em.nsites, fn->ninsns);
        continue;
      }
      g_em.sexec[site] = 0;
      continue;
    }
    if (!qjs_aot_prof_site(fn->g->digest, site, &ns, shapes)) continue;
    if (ns != 1) continue; /* polymorphic or unseen: no group */
    /* Sites whose profile shape is the same share a group, so one entry check
       validates all of them. */
    for (j = 0; j < site && g < 0; j++) {
      int nsj;
      if ((size_t)j >= g_em.nsites) break;
      if (g_em.sgroup[j] < 0) continue;
      if (!qjs_aot_prof_site(fn->g->digest, j, &nsj, sh)) continue;
      if (nsj == 1 && sh[0] == shapes[0]) g = g_em.sgroup[j];
    }
    if (g < 0) g = n++;
    if ((size_t)site < g_em.nsites) g_em.sgroup[site] = g;
  }
  g_em.ngroups = n;
  g_em.gfirst = calloc((size_t)(n > 0 ? n : 1), sizeof(*g_em.gfirst));
  if (g_em.gfirst) {
    int g2;
    for (g2 = 0; g2 < n; g2++) g_em.gfirst[g2] = -1;
    for (k = 0; k < nsites; k++)
      if (g_em.sgroup[k] >= 0 && g_em.gfirst[g_em.sgroup[k]] < 0)
        g_em.gfirst[g_em.sgroup[k]] = k;
  }
}

/* One emission: `name` is the C function to define (`aot_g_<hex>` or, later,
   the specialised body), and `spec` selects the specialised rules. Everything
   it collects (exits, cold arms) is per emission. */
static void q2_emit_one(
    FILE *f, const Q2Func *fn, char hex[33], const char *name, int spec) {
  int b, i, j;

  memset(&g_em, 0, sizeof(g_em));
  g_em.fn = fn;
  g_em.spec = spec;
  g_em.hex = hex;
  if (spec) {
    q2_groups(fn);
    q2_facts_build(fn);
    g_em.poly_way = malloc((size_t)(fn->nvalues > 0 ? fn->nvalues : 1));
    if (g_em.poly_way)
      memset(g_em.poly_way, 0xff, (size_t)(fn->nvalues > 0 ? fn->nvalues : 1));
    g_em.fact_cur = calloc(
        (size_t)(g_em.fact_words > 0 ? g_em.fact_words : 1), sizeof(uint64_t));
  }
  g_em.liveb = calloc((size_t)(fn->lwords > 0 ? fn->lwords : 1), 8);
  g_em.fb = malloc(sizeof(Q2ValueId) * (size_t)(fn->nregs > 0 ? fn->nregs : 1));
  if (g_em.fb)
    for (i = 0; i < fn->nregs; i++) g_em.fb[i] = Q2_NO_VALUE;

  {
    /* Where the function is defined, so a profile of the unit names it. */
    const char *file = strrchr(fn->g->file, '/');
    char who[160];
    size_t k;
    snprintf(who, sizeof(who), "%s %s:%d:%d",
             fn->g->name[0] ? fn->g->name : "<anon>",
             file ? file + 1 : fn->g->file, fn->g->line, fn->g->col);
    for (k = 0; who[k]; k++)
      if (who[k] == '*' || who[k] == '\n') who[k] = '_';
    fprintf(f, "/* %s */\n", who);
  }
  if (!spec)
    fprintf(
        f,
        "static int q2_spec_off_%s, q2_spec_fail_%s;\n",
        hex, hex);
  fprintf(
      f,
      "E2_TEXT static JSValue %s_%s(JSContext *ctx, JSRegRun *f,\n"
      "                                 JSValue *fr, uint32_t entry_pc)\n"
      "{\n",
      name, hex);
  fprintf(f, "    uint8_t *code = q2_code(f);\n");
  fprintf(f, "    uint8_t *ip;\n    uint32_t pc = 0;\n");
  /* The compare result: written by a hot path and, when its slow arm is out of
     line, by the arm too, so it is function-scoped. */
  fprintf(f, "    int _t;\n");
  /* A polymorphic compare chain's temporaries. They are function-scoped
     because the chain's fill arm is reached by a goto; each chain finishes
     before the next instruction starts, so one set serves them all. */
  fprintf(
      f,
      "    const void *_pe; uint32_t _po; int _pw; const void *_psh;\n");
  fprintf(f, "    JSValue *vb = q2_var_buf(f);\n");
  fprintf(f, "    JSValue *ab = q2_arg_buf(f);\n");
  fprintf(f, "    const void *pic = q2_pic(f);\n");
  /* The PIC way stride, read once per activation. Every compiled store into the
     frame goes through a pointer, so the compiler cannot prove those stores
     leave the bytecode alone and would reload it at each probe. */
  fprintf(f, "    const uint32_t q2_ns = (uint32_t)q2_pic_sites(f);\n");
  fprintf(f, "    void *const *holders = q2_pic_holder(f);\n");
  fprintf(
      f,
      "    (void)code; (void)ip; (void)vb; (void)ab; (void)pic; "
      "(void)holders;\n");
  fprintf(f, "#define Q2_REG(off) AOT_REG(fr, off)\n");
  /* Only pc 0 runs compiled; anything else is the interpreter's, from that pc
     on. The specialised body also refuses to run once it has exited too often
     or when the A/B switch is on: its refusals go to the generic body, which
     serves the same frame. */
  if (spec)
    fprintf(
        f,
        "    if (__builtin_expect(entry_pc != 0 || !q2_spec_on || "
        "q2_no_spec || q2_spec_off_%s, 0))\n"
        "        { qjs_aot_spec_refused++; return aot_g_%s(ctx, f, fr, "
        "entry_pc); }\n",
        hex, hex);
  else
    /* The generic body still has to refuse a mid-function entry: the
       interpreter owns every pc but the function's first. */
    fprintf(
        f,
        "    if (__builtin_expect(entry_pc != 0, 0)) return "
        "qjs_aot_deopt(ctx, f, fr, entry_pc, QJS_AOT_NO_SITE);\n");
  /* The group shapes, declared here so both the entry validation below and
     every grouped access can name them. */
  if (g_em.ngroups > 0) {
    int g;
    for (g = 0; g < g_em.ngroups; g++)
      fprintf(f, "    const void *gsh%d = NULL;\n", g);
  }
  /* Entry validation: each group's representative cache cell must still hold
     the shape the profile saw. Every other site of the group goes through the
     same cell, so one read per group stands for all of them; a NULL or a
     disagreeing cell hands the function to the generic body, which has no
     assumptions about it. */
  if (g_em.sgroup && g_em.gfirst && g_em.ngroups > 0) {
    int g;
    for (g = 0; g < g_em.ngroups; g++) {
      int first = g_em.gfirst[g];
      if (first < 0) continue;
      fprintf(
          f,
          "    gsh%d = q2_cell_shape(pic, %d);\n"
          "    if (gsh%d == NULL) return aot_g_%s(ctx, f, fr, 0);\n",
          g, first, g, hex);
    }
  }

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
    /* A speculated load keeps the JSValue it produced alongside the unboxed
       one, so a tagged consumer reads it and no re-boxing is needed. */
    if (v->spec) fprintf(f, "    JSValue v%d = JS_UNDEFINED;\n", i);
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

  /* The loop copies' slot pointers, invariant reads and entry budgets. */
  g_em.fhoist = calloc((size_t)(fn->ninsns > 0 ? fn->ninsns : 1), 1);
  g_em.fdbl = calloc((size_t)(fn->ninsns > 0 ? fn->ninsns : 1), 1);
  g_em.fgroup = calloc((size_t)(fn->ninsns > 0 ? fn->ninsns : 1), sizeof(int32_t));
  for (b = 0; b < fn->nblocks; b++) {
    int hi, lo_i, hi_i, ix;
    if (!q2_loop_range(fn, b, &hi)) continue;
    fprintf(f, "    int fl_%d = 8;\n", fn->blocks[b].start_pc);
    g_em.fast_lo = b;
    g_em.fast_hi = hi;
    q2_fast_hoist(fn);
    q2_fast_insns(fn, &lo_i, &hi_i);
    for (ix = lo_i; ix < hi_i; ix++) {
      const Q2Leaf *lf;
      char lhex[33];
      if (q2_fast_rcall(fn, &fn->insns[ix], &lf, lhex))
        fprintf(f, "    const void *flp%d = NULL; void *const *flh%d = NULL; "
                   "JSValue *fls%d[%d];\n",
                ix, ix, ix, lf->n > 0 ? lf->n : 1);
      if (q2_fast_access(fn, &fn->insns[ix]) < 0) continue;
      fprintf(f, "    JSValue *fs%d = NULL;\n    JSValue fv%d = JS_UNDEFINED;\n",
              ix, ix);
      fprintf(f, "    JSValue fc%d = JS_UNDEFINED;\n    double fcd%d = 0.0, "
                 "fvd%d = 0.0;\n",
              ix, ix, ix);
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
    int hi;
    if (q2_loop_range(fn, b, &hi) && q2_emit_fast_loop(f, fn, b, hi)) {
      b = hi;
      continue;
    }
    fprintf(f, "b_%d:\n", fn->blocks[b].start_pc);
    q2_emit_block(f, fn, b);
  }
  fprintf(f, "    return qjs_aot_ret(f);\n");
  q2_emit_exits(f, fn);
  q2_emit_colds(f);
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
  free(g_em.fb);
  g_em.fb = NULL;
  free(g_em.fhoist);
  g_em.fhoist = NULL;
  free(g_em.fdbl);
  g_em.fdbl = NULL;
  free(g_em.fgroup);
  g_em.fgroup = NULL;
  for (i = 0; i < g_em.nexits; i++) {
    free(g_em.exits[i].st_reg);
    free(g_em.exits[i].st_val);
    free(g_em.exits[i].chk_reg);
    free(g_em.exits[i].chk_val);
  }
  free(g_em.exits);
  g_em.exits = NULL;
  g_em.nexits = g_em.capexits = 0;
  /* A cold arm that never got q2_cold_end (an emitter that returned early)
     still owns its stream; closing it keeps the buffer valid and freed. */
  for (i = 0; i < g_em.ncolds; i++) {
    if (g_em.colds[i].m) fclose(g_em.colds[i].m);
    free(g_em.colds[i].buf);
  }
  free(g_em.colds);
  g_em.colds = NULL;
  g_em.ncolds = g_em.capcolds = 0;
  free(g_em.sgroup);
  g_em.sgroup = NULL;
  free(g_em.sexec);
  g_em.sexec = NULL;
  g_em.nsites = 0;
  free(g_em.gfirst);
  g_em.gfirst = NULL;
  g_em.ngroups = 0;
  free(g_em.fact_val);
  free(g_em.fact_grp);
  free(g_em.fact_in);
  free(g_em.fact_out);
  free(g_em.fact_scratch);
  free(g_em.fact_cur);
  g_em.fact_val = g_em.fact_grp = NULL;
  g_em.fact_in = g_em.fact_out = g_em.fact_scratch = NULL;
  g_em.fact_cur = NULL;
  g_em.nfacts = g_em.fact_words = 0;
}

/* A function's two bodies. `aot_g_` is today's emitter, the one every fact
   still holds for; `aot_f_` is the entry the table names. Until the
   specialised emission exists it only forwards, governed by the A/B switch and
   by the exit-rate trip. */
static void q2_emit_body(FILE *f, const Q2Func *fn, char hex[33]) {
  q2_emit_one(f, fn, hex, "aot_g", 0);
  /* A function over the code-size budget keeps the generic body only: the
     second copy would cost more I-cache than the shape checks save. The entry
     still exists and forwards, so nothing else changes. */
  if (!qjs_aot_body_over_budget(fn->ninsns)) {
    q2_emit_one(f, fn, hex, "aot_f", 1);
    return;
  }
  fprintf(
      f,
      "static int q2_spec_off_%s, q2_spec_fail_%s;\n"
      "E2_TEXT %sJSValue aot_f_%s(JSContext *ctx, JSRegRun *f,\n"
      "                                 JSValue *fr, uint32_t entry_pc)\n"
      "{\n"
      "    (void)q2_spec_off_%s; (void)q2_spec_fail_%s;\n"
      "    return aot_g_%s(ctx, f, fr, entry_pc);\n"
      "}\n\n",
      hex, hex, e2_linkage(), hex, hex, hex, hex);
}



/* ---- leaf bodies -------------------------------------------------------- */
/* A function whose compiled body has no side effect, no safepoint and no loop
   on its fast path gets a second body, `aot_p_<digest>`, which a compiled
   caller runs in place of the call: `this` and the arguments are C values,
   there is no frame, and the result is returned uncounted. Every guard of the
   function returns 0 before anything has happened, so the caller then makes
   the call it replaced and the function runs from its start. The body is
   static and small, so the C compiler inlines it into its callers.

   The emitted text is checked the way the loop copy is: any call outside the
   tag macros, the pure inline number helpers and the cache-hit test drops the
   leaf, as does any read of the frame or the variable area. */
static int q2_pure_scan(const char *t) {
  static const char *const ok[] = {
      "JS_VALUE_GET_TAG", "JS_VALUE_GET_INT", "JS_VALUE_GET_FLOAT64",
      "JS_VALUE_GET_PTR", "JS_VALUE_GET_BOOL", "JS_MKVAL", "JS_TAG_IS_FLOAT64",
      "JS_VALUE_IS_BOTH_INT", "JS_VALUE_IS_BOTH_FLOAT", "__JS_NewFloat64",
      "AOT_SET_B", "__builtin_expect", "__builtin_add_overflow",
      "__builtin_sub_overflow", "__builtin_mul_overflow", "if", "while",
      "for", "return", "sizeof", "qjs_aot_fitsi", "qjs_aot_box_num",
      "qjs_aot_should_deopt", "q2_own_read_hit", "q2_num_fast", "q2_arith",
      "qjs_aot_binary",
      "q2_unary_fast", "q2_cmp_int", "q2_cmp_double", "q2_branch_cmp", "int",
      "double", "int32_t", "uint32_t", "JSValue", "unsigned", NULL};
  if (strstr(t, "vb[") || strstr(t, "code")) return 0;
  return q2_scan(t, ok);
}

/* The receiver of a leaf's field read: -1 its `this`, k >= 0 its argument k,
   -2 anything else. */
static int q2_leaf_recv(const Q2Func *fn, Q2ValueId r) {
  int depth;
  for (depth = 0; depth < 8 && r != Q2_NO_VALUE; depth++) {
    const Q2Insn *in;
    int ix = fn->values[r].insn;
    if (ix < 0 || ix >= fn->ninsns) return -2;
    in = &fn->insns[ix];
    if (in->rop == ROP_get_arg) return (int)in->arg[1].val;
    if (in->rop == ROP_get_arg2)
      return (int)(in->def[0] == r ? in->arg[1].val : in->arg[3].val);
    if (in->rop == ROP_load_this) return -1;
    if ((in->rop == ROP_get_this_field || in->rop == ROP_get_this_field2) &&
        in->ndef >= 2 && in->def[1] == r)
      return -1;
    if (in->op == Q2_OP_MOVE && in->nuse >= 1) {
      r = in->use[0];
      continue;
    }
    return -2;
  }
  return -2;
}

/* A field read of the resolved-slot leaf (aot_r_): the slot the caller's loop
   preheader resolved for this site. */
static void q2_leaf_slot_read(FILE *f, const Q2Func *fn, const Q2Insn *in,
                              Q2ValueId recv, int site, Q2ValueId d) {
  int k = g_leafrec.n, rk = q2_leaf_recv(fn, recv);
  if (k >= Q2_LEAF_SITES || rk == -2) {
    g_leafrec.bad = 1;
    return;
  }
  g_leafrec.recv[k] = (int8_t)rk;
  g_leafrec.site[k] = site;
  g_leafrec.n++;
  (void)in;
  fprintf(
      f,
      "    v%u = *fls[%d];\n"
      "    if (JS_VALUE_GET_TAG(v%u) == QJS_AOT_TAG_LAZY_JSON) return 0;\n"
      "#if !QJS_DRC\n"
      "    if (JS_VALUE_GET_TAG(v%u) < 0) return 0;\n"
      "#endif\n",
      d, k, d, d);
}

/* One leaf body's text in the current mode (g_em.pure), or NULL. */
static char *q2_pure_text(Q2Func *fn, size_t *lenp) {
  char *buf = NULL;
  size_t len = 0;
  FILE *m;
  int b, i;
  m = open_memstream(&buf, &len);
  if (!m) return NULL;
  g_em.fn = fn;
  g_em.liveb = calloc((size_t)(fn->lwords > 0 ? fn->lwords : 1), 8);
  g_em.fb = malloc(sizeof(Q2ValueId) * (size_t)(fn->nregs > 0 ? fn->nregs : 1));
  if (g_em.fb)
    for (i = 0; i < fn->nregs; i++) g_em.fb[i] = Q2_NO_VALUE;
  /* A leaf body has no exits (a failed guard returns 0); anything collected
     here would belong to another emission. */
  g_em.nexits = 0;
  g_operand_err = 0;
  for (i = 0; i < fn->nvalues; i++) {
    const Q2Value *v = &fn->values[i];
    switch (v->rep) {
      case Q2_R_I32:
        fprintf(m, "    int32_t i%d = 0;\n", i);
        break;
      case Q2_R_F64:
        fprintf(m, "    double d%d = 0.0;\n", i);
        break;
      case Q2_R_BOOL:
        fprintf(m, "    int b%d = 0;\n", i);
        break;
      default:
        fprintf(m, "    JSValue v%d = JS_UNDEFINED;\n", i);
        break;
    }
    if (v->spec) fprintf(m, "    JSValue v%d = JS_UNDEFINED;\n", i);
  }
  {
    int maxp = 0;
    for (b = 0; b < fn->nblocks; b++)
      if (fn->blocks[b].nparams > maxp) maxp = fn->blocks[b].nparams;
    for (i = 0; i < maxp; i++)
      fprintf(m, "    JSValue ev%d; int32_t ei%d = 0; double ed%d = 0.0; "
                 "int eb%d = 0;\n", i, i, i, i);
  }

  /* The generic compare writes this, and a leaf body reaches it too; its slow
     arm stays inline there (q2_cold_begin refuses a leaf), so the variable is
     local to the body's own text. */
  fprintf(m, "    int _t;\n");
  for (b = 0; b < fn->nblocks; b++) {
    fprintf(m, "b_%d:\n", fn->blocks[b].start_pc);
    q2_emit_block(m, fn, b);
  }
  fclose(m);
  free(g_em.liveb);
  g_em.liveb = NULL;
  free(g_em.fb);
  g_em.fb = NULL;
  if (!buf || g_operand_err || !q2_pure_scan(buf)) {
    free(buf);
    return NULL;
  }
  *lenp = len;
  return buf;
}

/* The leaf bodies, after the frame-free macros a leaf's text relies on. */
static void q2_pure_out(FILE *out, const char *name, const char *hex,
                        const char *params, int argc, const char *buf,
                        size_t len) {
  fprintf(
      out,
      "#pragma push_macro(\"AOT_SET_B\")\n#undef AOT_SET_B\n"
      "#define AOT_SET_B(c, r, o, v) ((void)0)\n"
      "#pragma push_macro(\"q2_arith\")\n"
      "#define q2_arith(s, a, b, o) q2_num_fast(s, a, b, o)\n"
      "#pragma push_macro(\"qjs_aot_binary\")\n"
      "#define qjs_aot_binary(c, s, a, b, o) 1\n"
      "static inline %sint %s_%s(%s)\n"
      "{\n"
      "    uint32_t pc = 0;\n"
      "    (void)ctx; (void)this_obj; (void)ab;\n"
      "    (void)pc;\n"
      "    if (argc < %d) return 0;\n",
      /* A small leaf is inlined into every caller, which is where its
         argument array and result pointer go away. */
      len < 8192 ? "__attribute__((always_inline)) " : "", name, hex, params,
      argc);
  fputs(buf, out);
  /* A miss of the generic arithmetic lands here, having done nothing. */
  fprintf(out,
          "    return 0;\n"
          "exception: __attribute__((unused));\n"
          "    return 0;\n}\n"
          "#pragma pop_macro(\"qjs_aot_binary\")\n"
          "#pragma pop_macro(\"q2_arith\")\n"
          "#pragma pop_macro(\"AOT_SET_B\")\n\n");
}

int qjsaot2_emit_pure(FILE *out, const JSAotGenFunc *g, char hex[33]) {
  Q2Func fn;
  char msg[256], *buf, *rbuf;
  size_t len = 0, rlen = 0;
  int b;

  if (qjs_aot_check_locals()) return 0;
  if (q2_build(g, &fn) < 0) return 0;
  if (q2_infer(&fn) < 0 || q2_verify(&fn, msg, sizeof(msg)) < 0 ||
      fn.blocks[0].nparams > 0) {
    q2_free(&fn);
    return 0;
  }
  /* No loop: no back edge anywhere in the function. */
  for (b = 0; b < fn.nblocks; b++) {
    int k;
    for (k = 0; k < fn.blocks[b].nsucc; k++)
      if (q2_is_back_edge(&fn, b, fn.blocks[b].succ_block[k])) {
        q2_free(&fn);
        return 0;
      }
  }
  memset(&g_em, 0, sizeof(g_em));
  g_em.pure = 1;
  buf = q2_pure_text(&fn, &len);
  if (!buf) {
    g_em.pure = 0;
    q2_free(&fn);
    return 0;
  }
  /* The same body reading the slots its caller resolved, for a caller whose
     loop holds the receivers fixed. */
  memset(&g_leafrec, 0, sizeof(g_leafrec));
  g_em.pure = 2;
  rbuf = q2_pure_text(&fn, &rlen);
  g_em.pure = 0;
  q2_pure_out(out, "aot_p", hex,
              "JSContext *ctx, const void *pic, void *const *holders,\n"
              "    JSValue this_obj, int argc, const JSValue *ab, JSValue *out",
              g->arg_count, buf, len);
  if (rbuf && !g_leafrec.bad && !strstr(rbuf, "q2_own_read_hit") &&
      g_nleaves < Q2_MAX_LEAVES) {
    q2_pure_out(out, "aot_r", hex,
                "JSContext *ctx, JSValue this_obj, int argc, const JSValue *ab,\n"
                "    JSValue *const *fls, JSValue *out",
                g->arg_count, rbuf, rlen);
    g_leafrec.used = 1;
    memcpy(g_leafrec.hex, hex, 33);
    g_leaves[g_nleaves++] = g_leafrec;
  }
  free(buf);
  free(rbuf);
  q2_free(&fn);
  return 1;
}

/* The old emitter. `qjsaot2_emit_body` below chooses between this and the E2
   emitter per function. */
int q2_old_emit_body(FILE *f, const JSAotGenFunc *g, char hex[33]) {
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

/* The unit builder's per-function hook. With `--emit2` off this is the old
   emitter and nothing changes. With it on, the function goes to the E2 emitter,
   which decides for itself whether it can produce a body yet. */
int qjsaot2_emit_body(FILE *f, const JSAotGenFunc *g, char hex[33]) {
  return e2_emit_body_if_on(f, g, hex);
}

