/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * The generator's E2 report: one line per function, and a summary.
 *
 * It exists so a run says which functions the new emitter handled, which it
 * left to the old one, and how much of the program's executed ops they carry.
 * Without the weight a reader cannot tell "all functions delegated" from "all
 * functions delegated except the 60 % that matter".
 */
#include <stdio.h>
#include <stdlib.h>

#include "e2.h"

static FILE *e2_rep;
static int e2_rep_nfuncs;
static int e2_rep_entries, e2_rep_with_poll, e2_rep_with_k;
static double e2_rep_weight;
static int e2_rep_delegated;
static double e2_rep_weight_delegated;

/* Op-call weight per opcode: the profile's count summed over every pc the unit
   leaves as an op call. A pc that also has an inline fast path contributes to
   the op's `missarm` row instead, because only its misses reach the op call.
   Counting both in one total keeps the denominator stable across lowerings, so
   a before/after comparison of one op's share is meaningful. */
typedef struct {
  unsigned long long weight;
  unsigned long long missarm;
} E2OpWeight;
static E2OpWeight e2_opw[ROP_COUNT];

void e2_report_open(const char *path) {
  if (path == NULL || path[0] == 0) return;
  e2_rep = fopen(path, "w");
  if (e2_rep == NULL) {
    fprintf(stderr, "qjsaot2: cannot open the E2 report %s\n", path);
    return;
  }
}

void e2_report_func(const JSAotGenFunc *g, const char *hex, double weight,
                    int delegated) {
  char pos[300];

  e2_rep_nfuncs++;
  e2_rep_weight += weight;
  if (delegated) {
    e2_rep_delegated++;
    e2_rep_weight_delegated += weight;
  }
  if (e2_rep == NULL) return;
  if (g->line > 0)
    snprintf(pos, sizeof(pos), "%s:%d:%d", g->file, g->line, g->col);
  else
    snprintf(pos, sizeof(pos), "%s", g->file[0] ? g->file : "?");
  /* name file:line ops=N weight=W% delegated|native [scope] hex=... */
  fprintf(e2_rep, "E2 %s %s ops=%d weight=%.1f%% %s%s hex=%s\n",
          g->name[0] ? g->name : "<anon>", pos, g->len / 4, weight,
          delegated ? "delegated" : "native",
          e2_only_hex[0] ? " scope" : "", hex);
}

void e2_report_opcall(uint16_t rop, double weight, int missarm) {
  if (rop >= ROP_COUNT) return;
  if (missarm)
    e2_opw[rop].missarm += (unsigned long long)weight;
  else
    e2_opw[rop].weight += (unsigned long long)weight;
}

void e2_report_entry(const JSAotGenFunc *g, const char *hex, int frameless,
                     int poll, int stack_check) {
  (void)hex;
  e2_rep_entries++;
  if (poll) e2_rep_with_poll++;
  if (stack_check) e2_rep_with_k++;
  if (e2_rep == NULL) return;
  /* One line per entry, so a reader sees which of rules P and K a function's
     entry carries and can check the rule's assumption (only functions that
     call, plus frames over 16 KB, poll and check). */
  fprintf(e2_rep, "E2 entry %s %s (P%s)(K%s)%s\n",
          g->name[0] ? g->name : "<anon>",
          frameless ? "frameless" : "framed",
          poll ? "" : "-", stack_check ? "" : "-",
          e2_only_hex[0] ? " scope" : "");
}

void e2_report_close(void) {
  int r, n = 0;
  if (e2_rep == NULL) return;
  /* The summary answers "how much of this unit is the new emitter actually
     responsible for", by dynamic weight rather than by function count: a unit
     can be 90 % new functions by count and 5 % by executed ops. */
  fprintf(e2_rep, "E2-summary functions=%d delegated=%d weight=%.1f%% "
                  "weight_delegated=%.1f%% entries=%d poll=%d stackcheck=%d\n",
          e2_rep_nfuncs, e2_rep_delegated, e2_rep_weight,
          e2_rep_weight_delegated, e2_rep_entries, e2_rep_with_poll,
          e2_rep_with_k);
  /* The op-call weight table, which ranks the lowerings: one line per op the
     unit leaves as an op call, and a separate `missarm` line for an op that
     also has an inline fast path. `weight` is the profile's execution count
     summed over those pcs, so a reader ranks the ops by what an op call still
     costs rather than by how many sites the emitter produced. The profile comes
     from a `run-prof` run of the same script, so this needs no timing run. */
  for (r = 0; r < ROP_COUNT; r++) {
    char name[64];
    if (e2_opw[r].weight == 0 && e2_opw[r].missarm == 0) continue;
    e2_rop_name((uint16_t)r, name, sizeof(name));
    if (e2_opw[r].weight)
      fprintf(e2_rep, "e2 opcall %s weight=%llu\n", name, e2_opw[r].weight);
    if (e2_opw[r].missarm)
      fprintf(e2_rep, "e2 opcall %s missarm weight=%llu\n", name,
              e2_opw[r].missarm);
    n++;
  }
  fprintf(e2_rep, "E2-opcall ops=%d\n", n);
  fclose(e2_rep);
  e2_rep = NULL;
}
