/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * The builtins a unit lowers inline and what each one may do.
 *
 * One row per intrinsic id, in the engine's numbering (`QJS_AOT_INTR_*`, the ids
 * of `qjs_aot_intrinsic_fn`). The table is the contract an inline lowering
 * implements: what it takes, what it yields, and whether a call can run JS,
 * allocate or write memory.
 *
 * All three flags are 0 for NUMERIC arguments only. A non-number argument can
 * call `valueOf` (or, for parseInt, convert a string), so that case is never
 * inline: it takes the ordinary call, which is a safepoint.
 *
 * The profile names a C callee by its symbol and magic ("js_math_floor:0"); the
 * mapping from that key to an id lives here, so a profile from any run maps the
 * same way.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emit.h"
#include "quickjs-aot-rt.h"

/* Result kinds: the type a lowering gives the call's result from its argument
   types. `RES_F64` is always a double (boxed as the interpreter does, so an
   integral value becomes an int); `RES_SAME` is an i32 for an i32 argument and
   an f64 otherwise; `RES_ABS` is `RES_SAME` except that abs(INT32_MIN) is an f64,
   so an i32 result needs an exit or a double. */
enum { E_RES_F64, E_RES_SAME, E_RES_ABS, E_RES_MINMAX, E_RES_PARSEINT };

typedef struct EIntrinsic {
  int id;
  const char *name;   /* Math.floor */
  const char *sym;    /* js_math_floor: the profile's symbol */
  int magic;          /* the magic number the callee must carry, or -1 */
  int argc_min, argc_max;
  int result;         /* E_RES_* */
  /* For numeric arguments: */
  uint8_t runs_js, allocs, writes;
} EIntrinsic;

static const EIntrinsic e_intr[QJS_AOT_INTR_COUNT] = {
    {QJS_AOT_INTR_SQRT, "Math.sqrt", "js_math_sqrt", 0, 1, 1, E_RES_F64, 0, 0, 0},
    {QJS_AOT_INTR_ABS, "Math.abs", "js_math_fabs", 0, 1, 1, E_RES_ABS, 0, 0, 0},
    {QJS_AOT_INTR_FLOOR, "Math.floor", "js_math_floor", 0, 1, 1, E_RES_SAME, 0, 0, 0},
    {QJS_AOT_INTR_CEIL, "Math.ceil", "js_math_ceil", 0, 1, 1, E_RES_SAME, 0, 0, 0},
    {QJS_AOT_INTR_ROUND, "Math.round", "js_math_round", 0, 1, 1, E_RES_SAME, 0, 0, 0},
    {QJS_AOT_INTR_SIN, "Math.sin", "js_math_sin", 0, 1, 1, E_RES_F64, 0, 0, 0},
    {QJS_AOT_INTR_COS, "Math.cos", "js_math_cos", 0, 1, 1, E_RES_F64, 0, 0, 0},
    {QJS_AOT_INTR_MIN, "Math.min", "js_math_min_max", 0, 2, 3, E_RES_MINMAX, 0, 0, 0},
    {QJS_AOT_INTR_MAX, "Math.max", "js_math_min_max", 1, 2, 3, E_RES_MINMAX, 0, 0, 0},
    {QJS_AOT_INTR_PARSEINT, "parseInt", "js_parseInt", 0, 1, 2, E_RES_PARSEINT, 0, 0, 0},
    {QJS_AOT_INTR_STR_CHARCODEAT, "String.prototype.charCodeAt", "js_string_charCodeAt", 0, 1, 1,
     E_RES_F64, 0, 0, 0},
    {QJS_AOT_INTR_STR_CODEPOINTAT, "String.prototype.codePointAt", "js_string_codePointAt", 0, 1,
     1, E_RES_F64, 0, 0, 0},
    {QJS_AOT_INTR_STR_CHARAT, "String.prototype.charAt", "js_string_charAt", 0, 1, 1, E_RES_F64,
     0, 0, 0},
};

const char *e_intrinsic_name(int id) {
  return id >= 0 && id < QJS_AOT_INTR_COUNT ? e_intr[id].name : "?";
}

/* The id for a profile callee key "symbol:magic", or -1. Only `min`/`max` carry
   a meaningful magic; for every other row it is whatever the profile saw (0). */
int e_intrinsic_id_for_key(const char *key) {
  char sym[40];
  const char *colon = strrchr(key, ':');
  int magic, i;
  size_t n;
  if (colon == NULL) return -1;
  n = (size_t)(colon - key);
  if (n == 0 || n >= sizeof(sym)) return -1;
  memcpy(sym, key, n);
  sym[n] = 0;
  magic = (int)strtol(colon + 1, NULL, 10);
  for (i = 0; i < QJS_AOT_INTR_COUNT; i++)
    if (strcmp(e_intr[i].sym, sym) == 0 &&
        (e_intr[i].id == QJS_AOT_INTR_MIN || e_intr[i].id == QJS_AOT_INTR_MAX
             ? e_intr[i].magic == magic
             : 1))
      return i;
  return -1;
}

/* Whether the call whose profile record sits at `pc` (the pc just past the call)
   is an intrinsic site: every callee the profile saw is a C function that maps to
   the same id, and the set was not cut short (a full callee list may have lost
   one). Returns the id, or -1. */
int e_intrinsic_site(const uint8_t digest[16], int pc) {
  char hex[4][33];
  int kind[4], n = 0, k, id = -1;
  unsigned long long hits[4];
  if (!qjs_aot_prof_callees(digest, pc, &n, hex, kind, hits) || n <= 0 || n > 3)
    return -1;
  for (k = 0; k < n; k++) {
    int i;
    if (kind[k] != 2) return -1;
    i = e_intrinsic_id_for_key(hex[k]);
    if (i < 0 || (id >= 0 && i != id)) return -1;
    id = i;
  }
  return id;
}

int e_intrinsic_argc_ok(int id, int argc) {
  return id >= 0 && id < QJS_AOT_INTR_COUNT && argc >= e_intr[id].argc_min &&
         argc <= e_intr[id].argc_max;
}

/* The flags for numeric arguments (all 0 today; a row that cannot promise that
   must not be in the table). */
int e_intrinsic_pure(int id) {
  return id >= 0 && id < QJS_AOT_INTR_COUNT && !e_intr[id].runs_js &&
         !e_intr[id].allocs && !e_intr[id].writes;
}

/* The magic a callee must carry for `id`, or -1 when none is checked. */
int e_intrinsic_magic(int id) {
  if (id == QJS_AOT_INTR_MIN) return 0;
  if (id == QJS_AOT_INTR_MAX) return 1;
  return -1;
}
