/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */
#ifndef QJSAOT_REGEXP_H
#define QJSAOT_REGEXP_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Writes the 8-bit (and, when want16 and *has16 is set, the 16-bit) matcher of one compiled pattern as functions rxf_<name>_8 and rxf_<name>_16;
   returns 0 and writes nothing when the bytecode has an opcode that is not translated. */
int rx_emit(FILE *o, const char *name, const uint8_t *bc, size_t len, int want16, int *has16);
void rx_preamble(FILE *o);
#endif
