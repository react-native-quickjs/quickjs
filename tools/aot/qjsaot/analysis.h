/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * The two analyses the speculative body is built on, and their dump.
 *
 *   e_facts.c  forward dataflow over the speculative body's blocks: which
 *               object shapes are already checked (SHAPE) and which memory
 *               location still holds which SSA value (AVAIL), plus the
 *               monomorphic site groups they are expressed in.
 *   e_types.c  representation inference over the same blocks: which C type
 *               each SSA value is held in, why a value that stays boxed stays
 *               boxed, and which guard its definition needs.
 *
 * Neither pass emits anything. They produce a result the emitter reads (and
 * `--types` prints), so a representation or forwarding decision can be
 * inspected and gated before a single line of C is generated.
 *
 * The passes keep their results in their own structs rather than writing
 * through the IR's Q2Value/Q2Insn fields: the IR is shared with the old
 * lowering, whose inference already fills those fields with different (and, for
 * the speculative body, wrong) answers. An instruction's forwarding source is
 * therefore `facts->fwd[insn]`, not `insn->fwd`.
 */
#ifndef QJSAOT_ANALYSIS_H
#define QJSAOT_ANALYSIS_H

#include <stdio.h>

#include "ir.h"

/* ---- monomorphic groups ------------------------------------------------- */

#define E_GROUP_NONE (-1)

/* The program an instruction came from: the function itself, or a callee copied into it. */
static inline const uint8_t *e_dg(const Q2Func *fn, const Q2Insn *in) {
  return in->own ? fn->inl[in->own - 1].g->digest : fn->g->digest;
}
/* The program a property cache site number belongs to, and its number there. The copy's sites sit
   above the function's own. */
static inline const uint8_t *e_site_dg(const Q2Func *fn, int site, int *osite) {
  int k;
  for (k = 0; k < fn->ninl; k++) {
    const Q2Inl *iv = &fn->inl[k];
    if (site >= iv->sbase && site < iv->sbase + iv->g->pic_count) {
      *osite = site - iv->sbase;
      return iv->g->digest;
    }
  }
  *osite = site;
  return fn->g->digest;
}
/* The copy a site belongs to (1-based), or 0. */
static inline int e_site_own(const Q2Func *fn, int site) {
  int k;
  for (k = 0; k < fn->ninl; k++)
    if (site >= fn->inl[k].sbase && site < fn->inl[k].sbase + fn->inl[k].g->pic_count) return k + 1;
  return 0;
}

/* One property site the speculator knows a PIC cell for. `site` is the PIC
   index the encoder assigned; `insn` is the defining instruction so diagnostics
   can name a pc. */
typedef struct ESiteInfo {
  int site;
  int insn;
  int group;      /* index into EFacts.groups, or E_GROUP_NONE */
  Q2ValueId recv; /* the receiver value, or Q2_NO_VALUE */
  int poly;       /* the profile saw 2-4 shapes, all own slots: polymorphic cache */
  int proto;      /* a read served from the direct prototype: the holder is the shape's proto */
  int own;        /* 0, or the copied callee (1-based) the site belongs to */
  int write;      /* the site is a field store (its cell is checked for the writable kind) */
  int hot;        /* the profile sampled the site (1024+ executions): its cell is checked strictly at entry */
} ESiteInfo;

/* A group: every site whose profile saw exactly one shape, sharing it, so the
   speculative body validates the shape once at entry and then checks only the
   per-site slot offset. */
typedef struct EGroup {
  int own;        /* 0, or the copied callee (1-based) whose sites these are */
  int64_t shape;  /* the profile's shape id (an address in the profiled process) */
  int first_site; /* the site whose cell is read first at entry */
  int nsites;
  int *sites;
} EGroup;

/* ---- memory locations (the location table) ---------------------------- */

#define E_K_ARG 1    /* (ARG, slot) */
#define E_K_LOCAL 2  /* (LOC, slot) */
#define E_K_VARREF 3 /* (VREF, slot) */
#define E_K_FIELD 4  /* (FIELD, receiver, atom) */
#define E_K_ELEM 5   /* (ELEM, array, index) */
#define E_K_GLOBAL 6 /* (GLOB, atom) */
#define E_K_THIS 7   /* THIS */

/* The receiver of a field access that does not come from a register. A field
   fact keyed on one of these is killed by the store that replaces it, exactly
   as the register-keyed ones are killed by their store. */
#define E_RECV_THIS (-2)  /* `this`, fixed for the activation */
#define E_RECV_LOCAL(slot) (-3 - (slot)) /* the object in var_buf[slot] */
/* `get_length` has no index: it reads the element store's length. */
#define E_ELEM_LEN (-2)

typedef struct EKey {
  uint8_t kind; /* E_K_* */
  int64_t a;    /* a value id, a slot, an atom, or an E_RECV_* pseudo-id */
  int64_t b;    /* a value id, an atom, or E_ELEM_LEN; 0 when unused */
} EKey;

typedef struct EAvail {
  EKey key;
  Q2ValueId val; /* the SSA value the location holds */
} EAvail;

/* The facts true on entry to a block: the intersection of its predecessors'
   out-sets. A loop header starts empty, because its back edge is not visited
   when the single forward pass reaches it (loop-invariant facts are hoisting's
   job, not this pass's). */
typedef struct EBlockFacts {
  int nshape;
  int *shape_val;
  int *shape_grp;
  int navail;
  EAvail *avail;
} EBlockFacts;

/* The result of the facts pass. */
typedef struct EFacts {
  int ninsns;
  /* Per instruction: the value it is a copy of, or Q2_NO_VALUE. A forwarded
     load reads nothing, checks nothing and (for a tagged value) still stores. */
  Q2ValueId *fwd;
  uint8_t *access; /* a guarded access of a group site (tag + shape test) */
  uint8_t *skip;   /* ... whose test is redundant: SHAPE(receiver, group) holds */
  /* Per instruction, bit r: the cell of its r-th site is known to hold the group's shape
     (validated at entry or by an earlier access, with no safepoint since), so the access
     does not test it again. */
  uint8_t *cellskip;
  /* A numeric element access (get_element / put_element) whose miss exits the speculative
     body, so it is no safepoint; and, for the ones among them whose receiver is a dense
     array already checked in this region, `arrskip`: the receiver's class, holey flag, base
     pointer and count are in the C locals ap<v> / an<v>. `arr_used[v]` marks the receivers
     that have locals. */
  uint8_t *elemreg;
  uint8_t *arrskip;
  uint8_t *arr_used;
  int narr_vals;
  /* A call to compiled callees that all have a safepoint-free inline body: it runs the body
     in place or exits at the call, so it is no safepoint for the shape, cache-cell and array
     facts (the callee may store, so the loaded-value facts still die). */
  uint8_t *purecall;
  /* A grouped field access whose miss must leave the speculative body (an
     exit), never continue. The pass treats it as no safepoint
     only because of that: a miss that rejoins could run a getter or setter
     and invalidate every fact this pass carried past it. */
  uint8_t *miss_exits;
  int ngroups;
  EGroup *groups;
  int nsites;
  ESiteInfo *sites;
  int nblocks;
  EBlockFacts *blocks; /* the in-set of each block, for the dump */
  /* Shape checks hoisted to a loop preheader: the loop header, its
     preheader, the receiver value and the group. */
  int nloops, loop_cap, *loop_hdr, *loop_pre;
  uint8_t **loop_mem;
  int nhoist, hoist_cap;
  int *hoist_hdr, *hoist_pre, *hoist_val, *hoist_grp;
  int nfwd;             /* loads forwarded, for the report */
  int nskip;            /* shape checks skipped, for the report */
} EFacts;

/* Run the facts pass and build the site groups. Returns 0, or
   -1 on allocation failure. */
/* `grouped_exit` selects the mode. 0: a grouped field access is an ordinary
   safepoint and `miss_exits` stays 0 -- in the speculative body its miss
   continues into the generic lowering, so whatever the access ran may have
   broken every fact. 1: a grouped access's miss exits, so the access is a
   safepoint-free fast path. The speculative body passes 0; `--types` passes 1 so
   its numbers describe the exiting mode.

   A parameter rather than a global on purpose: a run that both dumps types and
   emits would otherwise leave the flag set for the functions emitted after the
   first dump, and the body would be emitted with facts that assume misses exit
   while its code continues on a miss. */
/* Set by the emitter: whether a call at `in` is a pure inline call (see EFacts.purecall), and
   whether this pass may use that (a framed speculative body). */
extern int e_facts_pure_ok;
extern int (*e_facts_pure_call)(const Q2Func *fn, const Q2Insn *in);
/* Whether a function has no safepoint at all in its speculative form. */
int e_facts_safepoint_free(const Q2Func *fn);
int e_facts_run(const Q2Func *fn, EFacts *out, int grouped_exit);
/* The loop-invariant global reads worth moving before their loop (see facts.c). */
int e_plan_licm(const Q2Func *fn, Q2Extra **out);
/* The calls worth replacing by a copy of their callee (see facts.c). */
int e_plan_inline(const Q2Func *fn, Q2InlReq **out);
/* The objects that never escape (see facts.c). */
extern int e_inline_allow_construct;
int e_ta_class(const Q2Func *fn, const Q2Insn *in);
int e_plan_virtual(const Q2Func *fn, Q2VWeb **webs, int *nwebs, Q2Rewrite **rw, int *nrw);
/* Whether polymorphic sites are served by the polymorphic cache in this body (a
   framed speculative body; set by the caller around e_facts_run). */
extern int e_facts_poly_on;
int e_facts_site_poly(const EFacts *f, int site);
/* The instructions a polymorphic site may be served by: a plain read or store. */
int e_facts_poly_op(const Q2Insn *in);void e_facts_free(EFacts *f);

/* The group a site index belongs to, or E_GROUP_NONE. */
int e_facts_site_group(const EFacts *f, int site);
/* Whether a site is a prototype-read site of its group. */
int e_facts_site_proto(const EFacts *f, int site);

/* ---- representations ---------------------------------------------------- */

/* The lattice: BOT is "not computed yet", never "tagged". */
#define E_LB_BOT 0
#define E_LB_BOOL 1
#define E_LB_I32 2
#define E_LB_F64 3
#define E_LB_TAG 4

/* Why a value ends up boxed. The report follows these back to a root. */
enum {
  E_WHY_NONE = 0,
  E_WHY_NO_PROFILE,     /* a speculated definition with no P record */
  E_WHY_MIXED,          /* the record holds more than numbers */
  E_WHY_OPERAND_TAGGED, /* an operand is tagged */
  E_WHY_UNKNOWN_RESULT, /* no transfer rule for the operation */
  E_WHY_PHI_TAGGED,     /* a block parameter joins a tagged incoming value */
  E_WHY_UNREACHABLE,    /* still BOT: nothing defines it on any path */
  E_WHY_INCOMING,       /* a parameter of the entry block: the caller passed
                            it in a JSValue, so the frame holds a JSValue */
  E_WHY_OPCALL_REDEF,   /* an op call whose destinations the role table does
                            not declare: the interpreter wrote the whole live
                            set, so which register holds what is not a
                            question about one operation's result */
  E_WHY_CONST_NONNUM    /* a constant that is not a number: null, undefined,
                            a string or a bigint */
};

/* The exits an i32 result needs (the "exits" column), so the emitter does not
   have to re-derive them from the operation. */
#define E_EX_OVF (1u << 0)      /* add/sub/mul/inc/dec overflow */
#define E_EX_ZERODIV (1u << 1)  /* b == 0 */
#define E_EX_NOTEXACT (1u << 2) /* a % b != 0 */
#define E_EX_INTMIN (1u << 3)   /* INT32_MIN / -1 */
#define E_EX_NEGZERO (1u << 4)  /* a zero result with a negative operand */
#define E_EX_SHRBIG (1u << 5)   /* a >>> result >= 2^31 */
#define E_EX_SIGNEDMIN (1u << 6) /* neg(INT32_MIN), mod(a<0, 0) */

/* What an instruction turned out to be, for the counters and the gate. */
#define E_AK_NONE 0
#define E_AK_ARITH 1 /* a binary/unary arithmetic result: the gate counts these */
#define E_AK_CMP 2
#define E_AK_FUSED 3

typedef struct ETypes {
  int nvalues;
  uint8_t *lb;    /* the final lattice value: E_LB_* */
  uint8_t *spec;  /* the representation came from a record: guard at its def */
  uint8_t *exits; /* E_EX_* for an i32 result */
  uint8_t *why;   /* E_WHY_* for a value that stays TAG */
  Q2ValueId *whyv; /* the operand/parameter a WHY_*_TAGGED names */
  uint8_t *arith; /* per value: E_AK_* of its defining instruction */
  /* A block parameter the analysis classified as a loop counter: every incoming
     argument is an increment of the parameter itself. It has no seed, so the
     cycle has no least fixpoint above BOT and the pass breaks it optimistically
     (the lowering guards the overflow). */
  uint8_t *loopcnt;
  /* Whether the value is part of the program the emitter will see: it has a
     defining instruction, or it is a live parameter of some block. The IR keeps
     value objects that trivial-parameter removal dropped from the graph (a
     parameter replaced by a dominating value), and counting those would report a
     function as much more boxed than it is. */
  uint8_t *live;
  /* Per function: the number of arithmetic results and how many are typed.
     Filled for the gate's summary line. Only live values are counted. */
  int arith_total, arith_typed, arith_tagged;
  /* The same over SSA values: a fused op defines two results. */
  int arithv_total, arithv_typed;
  int ntyped[4]; /* by lattice value: bool, i32, f64 (index 1..3) */
  int ntagged;
  int nlive;     /* live values, i.e. what the counters above sum to */
} ETypes;

/* Run representation inference over a built IR, reading the facts pass's
   forwarding decisions. Returns 0, or -1 on allocation failure. */
int e_types_run(const Q2Func *fn, const EFacts *facts, ETypes *out);
/* Drop the overflow exits an interval proof makes unnecessary. */
void e_ranges_run(const Q2Func *fn, ETypes *t);
void e_types_free(ETypes *t);
/* Facts then types, with the intrinsic sites whose arguments are not numbers
   dropped (e_ops.c). 0 on success; on failure nothing is left allocated. */
int e_analyze(const Q2Func *fn, EFacts *facts, ETypes *types);


/* Whether the value is held unboxed (i32, f64 or bool). */
int e_types_typed(const ETypes *t, Q2ValueId v);
/* The C representation name: i32, f64, bool or tag. */
const char *e_types_rep_name(const ETypes *t, Q2ValueId v);

/* ---- the dump ----------------------------------------------------------- */

/* `--types`: one block per function with the groups, the facts on entry
   to each block, one line per value, the loop counters and the tagged roots.
   Returns 0, or -1 on allocation failure. */
int e_analysis_dump(FILE *f, const Q2Func *fn, const char *hex, double weight);

#endif /* QJSAOT_ANALYSIS_H */
