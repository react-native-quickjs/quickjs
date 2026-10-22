/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * qjsaot's IR: a control-flow graph of blocks with parameters, in SSA form.
 *
 * Values carry a type set (what the value may be) and a representation (how it
 * is held). Operations carry a read and a write set over abstract locations;
 * the table in build.c is the single source of truth for both, and the
 * verifier checks every instruction against it.
 *
 * The IR is built from the *register program* (the encoded bytecode the engine
 * runs), never from JSFunctionDef: the generator only ever sees that program.
 */
#ifndef QJSAOT2_IR_H
#define QJSAOT2_IR_H

#include <stdint.h>
#include <stdio.h>

#include "prof.h"
#include "quickjs-aot-gen.h"

/* ---- representations ----------------------------------------------------- */
enum {
  Q2_R_TAGGED = 0, /* JSValue */
  Q2_R_I32,        /* int32_t */
  Q2_R_F64,        /* double */
  Q2_R_BOOL,       /* int */
  Q2_R_OBJ,        /* JSObject * */
  Q2_R_STR,        /* JSString * */
  Q2_R_COUNT
};

const char *q2_rep_name(int rep);

/* ---- types (a set; see the design's lattice) ----------------------------- */
enum {
  Q2_T_UNDEF = 1u << 0,
  Q2_T_NULL = 1u << 1,
  Q2_T_BOOL = 1u << 2,
  Q2_T_INT32 = 1u << 3,
  Q2_T_NUM = 1u << 4, /* any double-representable number */
  Q2_T_STR = 1u << 5,
  Q2_T_OBJ = 1u << 6,
  Q2_T_FUNC = 1u << 7,
  Q2_T_SYMBOL = 1u << 8,
  Q2_T_BIGINT = 1u << 9,
  Q2_T_HOLE = 1u << 10,
  Q2_T_ANY = (1u << 11) - 1,
};

void q2_type_str(uint32_t t, char *out, size_t n);

/* ---- effects (abstract locations an operation reads or writes) ----------- */
enum {
  Q2_E_SHAPE = 1u << 0,    /* object shapes; a write invalidates shape facts */
  Q2_E_SLOT = 1u << 1,     /* named property slots */
  Q2_E_ELEMENTS = 1u << 2, /* array / typed-array elements */
  Q2_E_LENGTH = 1u << 3,
  Q2_E_GLOBAL = 1u << 4,  /* global bindings */
  Q2_E_PROTO = 1u << 5,   /* prototype epoch */
  Q2_E_VARREF = 1u << 6,  /* captured cells */
  Q2_E_HEAP = 1u << 7,    /* everything a call can touch */
  Q2_E_CONTROL = 1u << 8, /* may throw, may transfer control */
};

/* ---- operations ---------------------------------------------------------- */
enum {
  Q2_OP_FALLBACK = 0, /* run the instruction through the step engine */
  Q2_OP_CONST,        /* load a constant (int/double/bool/null/undefined) */
  Q2_OP_MOVE,         /* d <- s, s dead afterwards when xfer */
  Q2_OP_GET_LOCAL,
  Q2_OP_PUT_LOCAL,
  Q2_OP_GET_ARG,
  Q2_OP_PUT_ARG,
  Q2_OP_GET_GLOBAL,
  Q2_OP_BINARY,     /* dst = a op b (numbers, the generic helper on a miss) */
  Q2_OP_COMPARE,    /* dst = bool of a compare */
  Q2_OP_UNARY,      /* dst = op a */
  Q2_OP_BRANCH,     /* conditional branch on a register */
  Q2_OP_BRANCH_CMP, /* conditional compare of two registers */
  Q2_OP_RETURN,
  Q2_OP_RETURN_UNDEF,
  Q2_OP_GET_FIELD,
  Q2_OP_GET_ELEMENT,
  Q2_OP_GET_ARG2,
  Q2_OP_GET_VAR_REF,
  Q2_OP_LOAD_THIS,
  Q2_OP_PUT_FIELD,
  Q2_OP_CALL,
  /* Ops the E2 projection named as unmodelled (`.local/E2-PROJECTION.md`
     recommendation 1). Before these existed, q2_rop_info returned FALLBACK
     with no operands, so the IR treated the instruction as redefining every
     live register and the arithmetic after it was boxed. */
  Q2_OP_POST_INC,    /* t, s, h: h = ++s, t = the value before -- 2 dests */
  Q2_OP_PUT_VAR_REF, /* slot, s: the closure cell takes s */
  Q2_OP_TO_PROPKEY,  /* d = ToPropertyKey(src); the 2-operand form adds a
                         null/undefined check on a third register */
  Q2_OP_SET_NAME,    /* sets the function name; writes nothing to a register */
  Q2_OP_COUNT
};

const char *q2_op_name(int op);

/* The operation a ROP maps to, its operand roles (which is the destination)
   and its effects. Unknown ROPs are Q2_OP_FALLBACK with a conservative
   read/write set; only classified ops are ever lowered natively. */
typedef struct Q2RopInfo {
  int op;
  int ndef, nuse; /* entries in def[]/use[] (operand indices) */
  int def[2], use[8];
  int8_t window; /* operand index of a call's argument count, else -1 */
  uint32_t reads, writes;
} Q2RopInfo;

const Q2RopInfo *q2_rop_info(uint16_t rop);

/* The branch target of an instruction: the value of its first 't' operand. */
struct Q2Insn;
int64_t q2_first_target_pub(const struct Q2Insn *in);

/* The ROPs the lowering lowers natively. The classifier must classify every one
   of them (the IR verifier checks it), so the two cannot drift apart. */
int q2_lowered(uint16_t rop);

/* ---- values -------------------------------------------------------------- */
typedef uint32_t Q2ValueId;
#define Q2_NO_VALUE ((Q2ValueId)-1)

typedef struct Q2Value {
  uint32_t type;    /* Q2_T_* set */
  uint8_t rep;      /* Q2_R_* */
  uint8_t is_const; /* the value is a constant folded at build time */
  int32_t con_kind; /* Q2_CON_* when is_const */
  int64_t con;      /* the constant payload (int or double bits) */
  int32_t reg;      /* register this value was read from / written to */
  int32_t block;    /* defining block */
  int32_t insn;     /* defining instruction, -1 for a block parameter */
  int32_t param_of; /* parameter index in its block, else -1 */
  /* The profile's view of the definition's register set, mapped to Q2_T_*.
     Zero when the defining instruction has no record. */
  uint32_t ptypes;
  uint8_t has_prof, frac, negzero;
  /* Which profile record `has_prof` came from: 0 for the per-pc `P` record,
     1 for the per-register `R` record. Reported by `--emit2-types` so a test
     can assert which one a destination used. */
  uint8_t prof_src;
  double nmin, nmax;
  /* Representation planning (the inference fills these; see types.c). */
  uint8_t spec;   /* a speculated load: needs a tag guard at its definition */
  uint8_t synced; /* the register slot holds this value boxed */
  uint8_t ovf;    /* an i32 arithmetic result needing an overflow/-0 guard */
  uint8_t lbrep;  /* the inference's lattice result, before finalisation */
} Q2Value;

enum {
  Q2_CON_INT = 0,
  Q2_CON_DOUBLE,
  Q2_CON_TRUE,
  Q2_CON_FALSE,
  Q2_CON_NULL,
  Q2_CON_UNDEF
};

/* ---- instructions -------------------------------------------------------- */
typedef struct Q2Arg {
  char kind; /* the decoded operand kind: r,a,b,u,i,I,s,c,t */
  int64_t val;
  int off; /* byte offset inside the instruction; -1 for derived values */
} Q2Arg;

typedef struct Q2Insn {
  uint16_t rop;
  int8_t op;    /* Q2_OP_* */
  int8_t sub;   /* REG_BIN_* of a binary/compare, -1 for none */
  int8_t sub2;  /* REG_BIN_* of a fused op's second half, else -1 */
  int32_t pc;   /* instruction start in the register program */
  int32_t next; /* pc + size */
  int32_t narg;
  Q2Arg arg[8];
  /* The SSA value each operand holds at this instruction: for an 'r' operand
     the value currently defined for that register, else Q2_NO_VALUE. */
  Q2ValueId argval[8];
  int8_t nuse;
  Q2ValueId use[8];
  /* A call's argument window, if any: the registers the callee reads. Kept
     beside `use` because the count is dynamic (up to 255). */
  int32_t win_reg, win_n;
  Q2ValueId *win_val; /* one value per window register, after resolution */
  int8_t ndef;
  Q2ValueId def[2];
  /* For a fallback: the registers it redefines and the value each becomes, so
     the lowering can reload them from the frame after the step. */
  int32_t nredef;
  int32_t *redef_reg;
  Q2ValueId *redef_val;
} Q2Insn;

/* ---- blocks -------------------------------------------------------------- */
typedef struct Q2Block {
  int32_t start_pc, end_pc;   /* [start_pc, end_pc) in the register program */
  int32_t first_insn, ninsns; /* into the function's instruction array */
  int32_t nparams;
  Q2ValueId *param_val; /* one per live-in register, aligned with param_reg */
  int32_t *param_reg;
  /* Every live-in register and the value it holds on entry, parameter or not:
     trivial-parameter removal drops a parameter in favour of a dominating
     value, and code that needs a register's current value at a block start
     (the frame write-back) reads it here. */
  int32_t nlive_in;
  int32_t *in_reg;
  Q2ValueId *in_val;
  int32_t nsucc;
  int32_t succ_pc[2]; /* successor pcs; a target may leave the function */
  int32_t succ_block[2];
  int32_t nedge_args[2];
  Q2ValueId **edge_arg; /* per successor: one value per successor param */
  int32_t *preds;       /* the block's predecessors, sized by a counting pass */
  int32_t npreds;
  int32_t ctl_in; /* control-stack depth on entry */
} Q2Block;

/* ---- functions ----------------------------------------------------------- */
typedef struct Q2Func {
  /* The blocks in DFS reverse postorder, as block indices. Unreachable blocks
     are absent: they are never emitted. `rpo_pos[b]` is b's position in this
     order, or -1 when b is unreachable. */
  int32_t *rpo;
  int32_t nrpo;
  int32_t *rpo_pos;
  const struct JSAotGenFunc *g;
  int ok;          /* the build succeeded */
  const char *why; /* when it did not */
  int32_t nregs;
  Q2Block *blocks;
  int32_t nblocks;
  Q2Insn *insns;
  int32_t ninsns;
  Q2Value *values;
  int32_t nvalues, vcap;
  Q2ValueId *reg_def; /* per register: the value defined at the current point */
  uint64_t *live_in;  /* per block, nregs bits */
  uint64_t *live_out;
  uint64_t *insn_live; /* per instruction, nregs bits: the live set just after
                          it, from which the frame write-back set follows */
  int32_t lwords;
  int32_t max_ctl;
  int32_t nparams_before; /* block parameters before trivial removal */
  int32_t entry_block;
  int32_t exit_pc;    /* the function's length */
  const ProfPc *prof; /* the function's P records, sorted by pc, or NULL */
  int32_t nprof;
  /* The function's complete call-result records, keyed by the pc just past the
     call (a call's `next`), sorted by pc, or NULL. A call with no entry here has
     no trustworthy result record. */
  const ProfPc *cres;
  int32_t ncres;
  /* The function's R records, sorted by register. A register absent from this
     array has no record at all; do not read a missing register as types == 0. */
  const Q2ProfReg *prof_regs;
  int32_t nprof_regs;
} Q2Func;

/* Build the IR for one function. Returns 0 on success, -1 with why set. */
int q2_build(const struct JSAotGenFunc *g, Q2Func *out);
void q2_free(Q2Func *f);
void q2_dump(FILE *f, const Q2Func *fn);

/* Decide each value's representation from the profile (types.c). Fills rep,
   spec, synced and ovf. Returns 0, or -1 on allocation failure. */
int q2_infer(Q2Func *fn);
/* Whether an instruction's destinations take their representation from the
   profile record (a load, a call, or another op the interpreter runs whose
   result is guarded after it), rather than from their operands. */
int q2_def_from_record(const Q2Insn *in);
/* `inc` with no profile record: typed from its operand, not speculated. */
int q2_inc_by_operand(const Q2Func *fn, const Q2Insn *in);
/* Verify a built function: SSA dominance, edge arguments, representations and
   the effect table. Returns 0, or -1 with a message in msg. */
int q2_verify(const Q2Func *f, char *msg, size_t msg_len);

/* Whether every back edge (per the reverse postorder) goes to a dominator. 1
   reducible, 0 irreducible, -1 out of memory. An irreducible function is
   refused for the speculative body. */
int q2_is_reducible(const Q2Func *f);

/* Whether u -> v is a back edge, by reverse postorder position. The single
   definition: nothing else compares pcs or block indices to decide this. */
int q2_is_back_edge(const Q2Func *f, int u, int v);

#endif /* QJSAOT2_IR_H */
