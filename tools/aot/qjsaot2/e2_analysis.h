/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * The two analyses the speculative body is built on, and their dump.
 *
 *   e2_facts.c  forward dataflow over the speculative body's blocks: which
 *               object shapes are already checked (SHAPE) and which memory
 *               location still holds which SSA value (AVAIL), plus the
 *               monomorphic site groups they are expressed in.
 *   e2_types.c  representation inference over the same blocks: which C type
 *               each SSA value is held in, why a value that stays boxed stays
 *               boxed, and which guard its definition needs.
 *
 * Neither pass emits anything. They produce a result the emitter reads (and
 * `--emit2-types` prints), so a representation or forwarding decision can be
 * inspected and gated before a single line of C is generated.
 *
 * The passes keep their results in their own structs rather than writing
 * through the IR's Q2Value/Q2Insn fields: the IR is shared with the old
 * lowering, whose inference already fills those fields with different (and, for
 * the speculative body, wrong) answers. An instruction's forwarding source is
 * therefore `facts->fwd[insn]`, not `insn->fwd`.
 */
#ifndef QJSAOT2_E2_ANALYSIS_H
#define QJSAOT2_E2_ANALYSIS_H

#include <stdio.h>

#include "ir.h"

/* ---- monomorphic groups ------------------------------------------------- */

#define E2_GROUP_NONE (-1)

/* One property site the speculator knows a PIC cell for. `site` is the PIC
   index the encoder assigned; `insn` is the defining instruction so diagnostics
   can name a pc. */
typedef struct E2SiteInfo {
  int site;
  int insn;
  int group;      /* index into E2Facts.groups, or E2_GROUP_NONE */
  Q2ValueId recv; /* the receiver value, or Q2_NO_VALUE */
} E2SiteInfo;

/* A group: every site whose profile saw exactly one shape, sharing it, so the
   speculative body validates the shape once at entry and then checks only the
   per-site slot offset. */
typedef struct E2Group {
  int64_t shape;  /* the profile's shape id (an address in the profiled process) */
  int first_site; /* the site whose cell is read first at entry */
  int nsites;
  int *sites;
} E2Group;

/* ---- memory locations (the design's D6 table) ---------------------------- */

#define E2_K_ARG 1    /* (ARG, slot) */
#define E2_K_LOCAL 2  /* (LOC, slot) */
#define E2_K_VARREF 3 /* (VREF, slot) */
#define E2_K_FIELD 4  /* (FIELD, receiver, atom) */
#define E2_K_ELEM 5   /* (ELEM, array, index) */
#define E2_K_GLOBAL 6 /* (GLOB, atom) */
#define E2_K_THIS 7   /* THIS */

/* The receiver of a field access that does not come from a register. A field
   fact keyed on one of these is killed by the store that replaces it, exactly
   as the register-keyed ones are killed by their store. */
#define E2_RECV_THIS (-2)  /* `this`, fixed for the activation */
#define E2_RECV_LOCAL(slot) (-3 - (slot)) /* the object in var_buf[slot] */
/* `get_length` has no index: it reads the element store's length. */
#define E2_ELEM_LEN (-2)

typedef struct E2Key {
  uint8_t kind; /* E2_K_* */
  int64_t a;    /* a value id, a slot, an atom, or an E2_RECV_* pseudo-id */
  int64_t b;    /* a value id, an atom, or E2_ELEM_LEN; 0 when unused */
} E2Key;

typedef struct E2Avail {
  E2Key key;
  Q2ValueId val; /* the SSA value the location holds */
} E2Avail;

/* The facts true on entry to a block: the intersection of its predecessors'
   out-sets. A loop header starts empty, because its back edge is not visited
   when the single forward pass reaches it (loop-invariant facts are hoisting's
   job, not this pass's). */
typedef struct E2BlockFacts {
  int nshape;
  int *shape_val;
  int *shape_grp;
  int navail;
  E2Avail *avail;
} E2BlockFacts;

/* The result of the facts pass. */
typedef struct E2Facts {
  int ninsns;
  /* Per instruction: the value it is a copy of, or Q2_NO_VALUE. A forwarded
     load reads nothing, checks nothing and (for a tagged value) still stores. */
  Q2ValueId *fwd;
  uint8_t *access; /* a guarded access of a group site (tag + shape test) */
  uint8_t *skip;   /* ... whose test is redundant: SHAPE(receiver, group) holds */
  /* A grouped field access whose miss must leave the speculative body (an
     exit, D9 steps 3-4), never continue. The pass treats it as no safepoint
     only because of that: a miss that rejoins could run a getter or setter
     and invalidate every fact this pass carried past it. */
  uint8_t *miss_exits;
  int ngroups;
  E2Group *groups;
  int nsites;
  E2SiteInfo *sites;
  int nblocks;
  E2BlockFacts *blocks; /* the in-set of each block, for the dump */
  int nfwd;             /* loads forwarded, for the report */
  int nskip;            /* shape checks skipped, for the report */
} E2Facts;

/* Run the facts pass (D7) and build the site groups (D9 step 1). Returns 0, or
   -1 on allocation failure. */
/* `grouped_exit` selects the mode. 0: a grouped field access is an ordinary
   safepoint and `miss_exits` stays 0 -- in the speculative body its miss
   continues into the generic lowering, so whatever the access ran may have
   broken every fact. 1: a grouped access's miss exits, so the access is a
   safepoint-free fast path. E8's body passes 0; `--emit2-types` passes 1 so its
   numbers describe the E9 mode.

   A parameter rather than a global on purpose: a run that both dumps types and
   emits would otherwise leave the flag set for the functions emitted after the
   first dump, and the body would be emitted with facts that assume misses exit
   while its code continues on a miss. */
int e2_facts_run(const Q2Func *fn, E2Facts *out, int grouped_exit);
void e2_facts_free(E2Facts *f);

/* The group a site index belongs to, or E2_GROUP_NONE. */
int e2_facts_site_group(const E2Facts *f, int site);

/* ---- representations ---------------------------------------------------- */

/* D8's lattice: BOT is "not computed yet", never "tagged". */
#define E2_LB_BOT 0
#define E2_LB_BOOL 1
#define E2_LB_I32 2
#define E2_LB_F64 3
#define E2_LB_TAG 4

/* Why a value ends up boxed. The report follows these back to a root. */
enum {
  E2_WHY_NONE = 0,
  E2_WHY_NO_PROFILE,     /* a speculated definition with no P record */
  E2_WHY_MIXED,          /* the record holds more than numbers */
  E2_WHY_OPERAND_TAGGED, /* an operand is tagged */
  E2_WHY_UNKNOWN_RESULT, /* no transfer rule for the operation */
  E2_WHY_PHI_TAGGED,     /* a block parameter joins a tagged incoming value */
  E2_WHY_UNREACHABLE,    /* still BOT: nothing defines it on any path */
  E2_WHY_INCOMING,       /* a parameter of the entry block: the caller passed
                            it in a JSValue, so the frame holds a JSValue */
  E2_WHY_OPCALL_REDEF,   /* an op call whose destinations the role table does
                            not declare: the interpreter wrote the whole live
                            set, so which register holds what is not a
                            question about one operation's result */
  E2_WHY_CONST_NONNUM    /* a constant that is not a number: null, undefined,
                            a string or a bigint */
};

/* The exits an i32 result needs (D8's "exits" column), so the emitter does not
   have to re-derive them from the operation. */
#define E2_EX_OVF (1u << 0)      /* add/sub/mul/inc/dec overflow */
#define E2_EX_ZERODIV (1u << 1)  /* b == 0 */
#define E2_EX_NOTEXACT (1u << 2) /* a % b != 0 */
#define E2_EX_INTMIN (1u << 3)   /* INT32_MIN / -1 */
#define E2_EX_NEGZERO (1u << 4)  /* a zero result with a negative operand */
#define E2_EX_SHRBIG (1u << 5)   /* a >>> result >= 2^31 */
#define E2_EX_SIGNEDMIN (1u << 6) /* neg(INT32_MIN), mod(a<0, 0) */

/* What an instruction turned out to be, for the counters and the gate. */
#define E2_AK_NONE 0
#define E2_AK_ARITH 1 /* a binary/unary arithmetic result: the gate counts these */
#define E2_AK_CMP 2
#define E2_AK_FUSED 3

typedef struct E2Types {
  int nvalues;
  uint8_t *lb;    /* the final lattice value: E2_LB_* */
  uint8_t *spec;  /* the representation came from a record: guard at its def */
  uint8_t *exits; /* E2_EX_* for an i32 result */
  uint8_t *why;   /* E2_WHY_* for a value that stays TAG */
  Q2ValueId *whyv; /* the operand/parameter a WHY_*_TAGGED names */
  uint8_t *arith; /* per value: E2_AK_* of its defining instruction */
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
} E2Types;

/* Run representation inference (D8) over a built IR, reading the facts pass's
   forwarding decisions. Returns 0, or -1 on allocation failure. */
int e2_types_run(const Q2Func *fn, const E2Facts *facts, E2Types *out);
void e2_types_free(E2Types *t);

/* Whether the value is held unboxed (i32, f64 or bool). */
int e2_types_typed(const E2Types *t, Q2ValueId v);
/* The C representation name: i32, f64, bool or tag. */
const char *e2_types_rep_name(const E2Types *t, Q2ValueId v);

/* ---- the dump ----------------------------------------------------------- */

/* `--emit2-types`: one block per function with the groups, the facts on entry
   to each block, one line per value, the loop counters and the tagged roots.
   Returns 0, or -1 on allocation failure. */
int e2_analysis_dump(FILE *f, const Q2Func *fn, const char *hex, double weight);

#endif /* QJSAOT2_E2_ANALYSIS_H */
