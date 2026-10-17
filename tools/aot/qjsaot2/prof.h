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
#ifndef QJSAOT2_PROF_H
#define QJSAOT2_PROF_H

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
  /* AOT_ELEM_*: what an element pc stored or loaded. INT or FLOAT alone means
     every element there was a number, so a compiled read needs no hole or lazy
     test. */
  uint8_t elem;
  double num_min, num_max;
} ProfPc;

#endif /* QJSAOT2_PROF_H */
