/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * The `P` record the profiler writes per instruction, shared by the driver
 * (which parses it) and the IR (which reads it). One record covers every
 * register the instruction at `pc` writes, so the two destinations of a fused
 * operation share one.
 */
#ifndef QJSAOT_PROF_H
#define QJSAOT_PROF_H

#include <stdint.h>

/* Type classes the profiler records per instruction (mirrors quickjs.c's
   AOT_TYPE_*). */
#define PT_FLOAT 0x01u
#define PT_INT 0x02u
#define PT_OBJECT 0x04u
#define PT_STRING 0x08u
#define PT_BOOL 0x10u
#define PT_UNDEF 0x20u
#define PT_BIGINT 0x40u
#define PT_OTHER 0x80u
#define PT_ARITH (PT_FLOAT | PT_INT)

typedef struct ProfPc {
  int32_t pc;     /* the register-program offset of the defining instruction */
  unsigned types; /* the PT_* set */
  uint8_t has_num, frac, negzero;
  /* What an element pc stored or loaded. Same values the profiler writes
     (AOT_ELEM_* in quickjs.c); they are repeated here because the generator
     does not include the engine. */
#define Q2_ELEM_INT    (1u << 0)
#define Q2_ELEM_FLOAT  (1u << 1)
#define Q2_ELEM_OBJECT (1u << 2)
#define Q2_ELEM_MIXED  (1u << 3)
  /* AOT_ELEM_*: what an element pc stored or loaded. INT or FLOAT alone means
     every element there was a number, so a compiled read needs no hole or lazy
     test. */
  uint8_t elem;
  double num_min, num_max;
  /* A call-result (`K`) record only: the calls that entered the site and the
     returns recorded for it. Zero in a `P` record. */
  unsigned calls, results;
} ProfPc;

/* One register's `R` record: the merged PT_* set over the whole function.
   A register with no record has no entry at all here -- the IR must be able to
   tell "no record" from "a record saying nothing", so an absent register is
   absent, never an entry with types == 0. (Same reasoning as ProfCount below.) */
typedef struct Q2ProfReg {
  int32_t reg;
  unsigned types;
} Q2ProfReg;
/* `qjs_aot_prof_regs` hands back the generator's own ProfReg as a Q2ProfReg*.
   The two are layout-identical ({int, unsigned} and {int32_t, unsigned}); this
   makes a future divergence a compile error rather than a silent misread. */
_Static_assert(sizeof(Q2ProfReg) == 8, "ProfReg/Q2ProfReg must stay 8 bytes");

/* One instruction's execution count, as the profile's per-pc sample record
   gives it (`T` lines: one sample per 1024 dispatches of that pc). It is kept
   apart from ProfPc because a pc can have a sample record without a type
   record, and the IR must not see a type-less entry as "profiled". */
typedef struct ProfCount {
  int32_t pc;
  unsigned long long count;
} ProfCount;

#endif /* QJSAOT_PROF_H */
