/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * The generator's report: one line per function, and a summary.
 *
 * It exists so a run says which functions were compiled, which were left to the
 * interpreter, and how much of the program's executed ops they carry. Without
 * the weight a reader cannot tell "all functions delegated" from "all functions
 * delegated except the 60 % that matter".
 */
#include <stdio.h>
#include <stdlib.h>

#include "emit.h"

static FILE *e_rep;
static int e_rep_nfuncs;
static int e_rep_entries, e_rep_with_poll, e_rep_with_k;
static double e_rep_weight;
static int e_rep_delegated;
static double e_rep_weight_delegated;

/* Op-call weight per opcode: the profile's count summed over every pc the unit
   leaves as an op call. A pc that also has an inline fast path contributes to
   the op's `missarm` row instead, because only its misses reach the op call.
   Counting both in one total keeps the denominator stable across lowerings, so
   a before/after comparison of one op's share is meaningful. */
typedef struct {
  unsigned long long weight;
  unsigned long long missarm;
} EOpWeight;
static EOpWeight e_opw[ROP_COUNT];

FILE *e_report_file(void) { return e_rep; }

void e_report_open(const char *path) {
  if (path == NULL || path[0] == 0) return;
  e_rep = fopen(path, "w");
  if (e_rep == NULL) {
    fprintf(stderr, "qjsaot: cannot open the report %s\n", path);
    return;
  }
}

void e_report_func(const JSAotGenFunc *g, const char *hex, double weight,
                    int delegated) {
  char pos[300];

  e_rep_nfuncs++;
  e_rep_weight += weight;
  if (delegated) {
    e_rep_delegated++;
    e_rep_weight_delegated += weight;
  }
  if (e_rep == NULL) return;
  if (g->line > 0)
    snprintf(pos, sizeof(pos), "%s:%d:%d", g->file, g->line, g->col);
  else
    snprintf(pos, sizeof(pos), "%s", g->file[0] ? g->file : "?");
  /* name file:line ops=N weight=W% delegated|native hex=... */
  fprintf(e_rep, "func %s %s ops=%d weight=%.1f%% %s hex=%s\n",
          g->name[0] ? g->name : "<anon>", pos, g->len / 4, weight,
          delegated ? "delegated" : "native", hex);
}

void e_report_opcall(uint16_t rop, double weight, int missarm) {
  if (rop >= ROP_COUNT) return;
  if (missarm)
    e_opw[rop].missarm += (unsigned long long)weight;
  else
    e_opw[rop].weight += (unsigned long long)weight;
}

void e_report_entry(const JSAotGenFunc *g, const char *hex, int frameless,
                     int poll, int stack_check) {
  (void)hex;
  e_rep_entries++;
  if (poll) e_rep_with_poll++;
  if (stack_check) e_rep_with_k++;
  if (e_rep == NULL) return;
  /* One line per entry, so a reader sees which of rules P and K a function's
     entry carries and can check the rule's assumption (only functions that
     call, plus frames over 16 KB, poll and check). */
  if (frameless)
    fprintf(e_rep, "entry %s frameless\n",
            g->name[0] ? g->name : "<anon>");
  else
    fprintf(e_rep, "entry %s framed (P%s)(K%s) blocked-by=%s\n",
            g->name[0] ? g->name : "<anon>",
            poll ? "" : "-", stack_check ? "" : "-",
            e_fl_why_not()[0] ? e_fl_why_not() : "-");
}

/* A call site the profile marks as an intrinsic site (e_intrinsics.c). */
void e_report_intrinsic(const char *hex, int pc, int id, int argc, int ok) {
  if (e_rep == NULL) return;
  fprintf(e_rep, "intrinsic hex=%s pc=%d id=%d name=%s argc=%d lowerable=%d\n",
          hex, pc, id, e_intrinsic_name(id), argc, ok);
}

void e_report_hoist(const char *hex, int n) {
  if (e_rep == NULL) return;
  fprintf(e_rep, "hoist hex=%s n=%d\n", hex, n);
}

/* A call that stayed a call because the copies in `hex` reached their budget. */
void e_report_copycap(const char *hex, const char *callee, int copied, int len, int cap) {
  if (e_rep == NULL) return;
  fprintf(e_rep, "copycap hex=%s callee=%s copied=%d len=%d cap=%d\n", hex, callee, copied, len, cap);
}

void e_report_inline(const char *hex, int ok, const char *why) {
  if (e_rep == NULL) return;
  fprintf(e_rep, "inline hex=%s %s reason=%s\n", hex, ok ? "yes" : "no",
          why != NULL && why[0] ? why : "-");
}

void e_report_spec(
    const char *hex, const int vals[4], const int tiers[4], const int checks[9],
    long lines, long bytes) {
  if (e_rep == NULL) return;
  fprintf(
      e_rep, "values hex=%s i32=%d f64=%d bool=%d tag=%d\n", hex, vals[0],
      vals[1], vals[2], vals[3]);
  fprintf(
      e_rep, "tiers hex=%s typed=%d opcall=%d handoff=%d other=%d\n", hex,
      tiers[0], tiers[1], tiers[2], tiers[3]);
  fprintf(
      e_rep,
      "checks hex=%s ax=%d vx=%d dx=%d ox=%d exits=%d handoffs=%d "
      "shapes=%d skipped=%d fwd=%d\n",
      hex, checks[0], checks[1], checks[2], checks[3], checks[4], checks[5],
      checks[6], checks[7], checks[8]);
  fprintf(e_rep, "code hex=%s lines=%ld bytes=%ld\n", hex, lines, bytes);
}

void e_report_close(void) {
  int r, n = 0;
  if (e_rep == NULL) return;
  /* The summary answers "how much of this unit is compiled", by dynamic weight
     rather than by function count: a unit can be 90 % compiled functions by count
     and 5 % by executed ops. */
  fprintf(e_rep, "summary functions=%d delegated=%d weight=%.1f%% "
                  "weight_delegated=%.1f%% entries=%d poll=%d stackcheck=%d\n",
          e_rep_nfuncs, e_rep_delegated, e_rep_weight,
          e_rep_weight_delegated, e_rep_entries, e_rep_with_poll,
          e_rep_with_k);
  /* The op-call weight table, which ranks the lowerings: one line per op the
     unit leaves as an op call, and a separate `missarm` line for an op that
     also has an inline fast path. `weight` is the profile's execution count
     summed over those pcs, so a reader ranks the ops by what an op call still
     costs rather than by how many sites the emitter produced. The profile comes
     from a `run-prof` run of the same script, so this needs no timing run. */
  for (r = 0; r < ROP_COUNT; r++) {
    char name[64];
    if (e_opw[r].weight == 0 && e_opw[r].missarm == 0) continue;
    e_rop_name((uint16_t)r, name, sizeof(name));
    if (e_opw[r].weight)
      fprintf(e_rep, "opcall %s weight=%llu\n", name, e_opw[r].weight);
    if (e_opw[r].missarm)
      fprintf(e_rep, "opcall %s missarm weight=%llu\n", name,
              e_opw[r].missarm);
    n++;
  }
  fprintf(e_rep, "opcall-ops ops=%d\n", n);
  fclose(e_rep);
  e_rep = NULL;
}

void e_report_fast(const char *hex, const char *name, int ok, const char *why) {
  if (e_rep == NULL) return;
  if (ok) fprintf(e_rep, "fast %s hex=%s ok\n", name[0] ? name : "<anon>", hex);
  else fprintf(e_rep, "fast %s hex=%s not: %s\n", name[0] ? name : "<anon>", hex, why);
}
