/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * qjsaot2's IR: a control-flow graph of blocks with parameters, in SSA form.
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
  Q2_E_GLOBAL = 1u << 4,   /* global bindings */
  Q2_E_PROTO = 1u << 5,    /* prototype epoch */
  Q2_E_VARREF = 1u << 6,   /* captured cells */
  Q2_E_HEAP = 1u << 7,     /* everything a call can touch */
  Q2_E_CONTROL = 1u << 8,  /* may throw, may transfer control */
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
  Q2_OP_BINARY,    /* dst = a op b (numbers, the generic helper on a miss) */
  Q2_OP_COMPARE,   /* dst = bool of a compare */
  Q2_OP_UNARY,     /* dst = op a */
  Q2_OP_BRANCH,    /* conditional branch on a register */
  Q2_OP_BRANCH_CMP,/* conditional compare of two registers */
  Q2_OP_RETURN,
  Q2_OP_RETURN_UNDEF,
  Q2_OP_GET_FIELD,
  Q2_OP_PUT_FIELD,
  Q2_OP_COUNT
};

const char *q2_op_name(int op);

/* The operation a ROP maps to, its operand roles (which is the destination)
   and its effects. Unknown ROPs are Q2_OP_FALLBACK with a conservative
   read/write set; only classified ops are ever lowered natively. */
typedef struct Q2RopInfo {
  int op;
  int ndef, nuse;     /* entries in def[]/use[] (operand indices) */
  int def[2], use[3];
  uint32_t reads, writes;
} Q2RopInfo;

const Q2RopInfo *q2_rop_info(uint16_t rop);

/* ---- values -------------------------------------------------------------- */
typedef uint32_t Q2ValueId;
#define Q2_NO_VALUE ((Q2ValueId)-1)

typedef struct Q2Value {
  uint32_t type;      /* Q2_T_* set */
  uint8_t rep;        /* Q2_R_* */
  uint8_t is_const;   /* the value is a constant folded at build time */
  int32_t con_kind;   /* Q2_CON_* when is_const */
  int64_t con;        /* the constant payload (int or double bits) */
  int32_t reg;        /* register this value was read from / written to */
  int32_t block;      /* defining block */
  int32_t insn;       /* defining instruction, -1 for a block parameter */
  int32_t param_of;   /* parameter index in its block, else -1 */
} Q2Value;

enum { Q2_CON_INT = 0, Q2_CON_DOUBLE, Q2_CON_TRUE, Q2_CON_FALSE, Q2_CON_NULL, Q2_CON_UNDEF };

/* ---- instructions -------------------------------------------------------- */
typedef struct Q2Arg {
  char kind;    /* the decoded operand kind: r,a,b,u,i,I,s,c,t */
  int64_t val;
  int off;      /* byte offset inside the instruction; -1 for derived values */
} Q2Arg;

typedef struct Q2Insn {
  uint16_t rop;
  int8_t op;          /* Q2_OP_* */
  int32_t pc;         /* instruction start in the register program */
  int32_t next;       /* pc + size */
  int32_t narg;
  Q2Arg arg[8];
  int8_t nuse;
  Q2ValueId use[3];
  int8_t ndef;
  Q2ValueId def[2];
  int32_t fallback;    /* for Q2_OP_FALLBACK: the live registers it redefines */
} Q2Insn;

/* ---- blocks -------------------------------------------------------------- */
typedef struct Q2Block {
  int32_t start_pc, end_pc;   /* [start_pc, end_pc) in the register program */
  int32_t first_insn, ninsns; /* into the function's instruction array */
  int32_t nparams;
  Q2ValueId *param_val; /* one per live-in register, aligned with param_reg */
  int32_t *param_reg;
  int32_t nsucc;
  int32_t succ_pc[2]; /* successor pcs; a target may leave the function */
  int32_t succ_block[2];
  int32_t nedge_args[2];
  Q2ValueId **edge_arg;  /* per successor: one value per successor param */
  int32_t preds[2];      /* the block's predecessors (for the verifier) */
  int32_t npreds;
  int32_t ctl_in;        /* control-stack depth on entry */
} Q2Block;

/* ---- functions ----------------------------------------------------------- */
typedef struct Q2Func {
  const struct JSAotGenFunc *g;
  int ok;             /* the build succeeded */
  const char *why;    /* when it did not */
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
  int32_t lwords;
  int32_t max_ctl;
  int32_t entry_block;
  int32_t exit_pc;    /* the function's length */
} Q2Func;

/* Build the IR for one function. Returns 0 on success, -1 with why set. */
int q2_build(const struct JSAotGenFunc *g, Q2Func *out);
void q2_free(Q2Func *f);
void q2_dump(FILE *f, const Q2Func *fn);

/* Verify a built function: SSA dominance, edge arguments, representations and
   the effect table. Returns 0, or -1 with a message in msg. */
int q2_verify(const Q2Func *f, char *msg, size_t msg_len);

#endif /* QJSAOT2_IR_H */
