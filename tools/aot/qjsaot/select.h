/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * Region root selection (AOT-REDESIGN.md R3): which functions get a region, and which call sites of each
 * region are expanded. Nothing is hand-picked: both come from the profile (call counts per site, per-function
 * ops) and the bundle's resolved call graph.
 *
 *   cost     self(f) = f's share of the profile's executed ops; per call of f, incl(f)/entries(f) with
 *            incl(f) = self(f) + sum over f's call sites of count(site, callee) * incl(callee)/entries(callee)
 *            (back edges of recursion contribute their count but no further cost).
 *   region   for a candidate root r, the invocations of every function reachable from r are propagated down
 *            the DAG (ctx[r] = entries(r); a site runs count*ctx[f]/entries(f) times, split over its targets by
 *            the profile's per-callee hits). A site is EXPANDED when it runs at least SITE (default 1%) of all
 *            call executions in r's tree and its caller is itself in the region; there is no depth limit and
 *            a cold site stays a call/exit. The region's weight is the self cost it covers.
 *   roots    candidates are ordered by region weight; one is a root when its weight is at least HOT (default
 *            1% of the whole profile) and it is not COVER (default 50%) or more inlined, by invocation count,
 *            into the regions already chosen.
 * Thresholds: QJS_AOT_SEL_HOT, QJS_AOT_SEL_SITE, QJS_AOT_SEL_COVER (fractions), or sel_set_thresholds.
 */
#ifndef QJSAOT_SELECT_H
#define QJSAOT_SELECT_H

#include <stdio.h>

#include "ir.h"

/* Choose the roots: indices into `funcs` (digest-sorted, as the driver holds them; a body shared by several entries is
   represented by the last of them), hottest first, at most `max`. Returns the count. Needs the profile loaded and the bundle
   initialised (qjs_aot_bundle_init). Call once; sel_expand_site and sel_report answer for the last selection. */
int sel_roots(const JSAotGenFunc *funcs, int n, int *roots, int max);

/* 1 when the call at `pc` (the call instruction's own pc) of `g` is expanded in the region of `root`, 0 when it stays a
   call/exit (also 0 for a function that is not a root). */
int sel_expand_site(const JSAotGenFunc *root, const JSAotGenFunc *g, int pc);

/* The roots with their region and inclusive weights, and every expanded and pruned site. */
void sel_report(FILE *f);

/* Generator hook (QJS_AOT_REGION only): choose the roots over the unit and remember them; sel_is_root(fi) then answers for the function
   index the emitter is on, and the roots are written to `rep` (may be NULL). */
void sel_prepare(const JSAotGenFunc *funcs, int n, FILE *rep);
int sel_is_root(int fi);

void sel_set_thresholds(double hot, double site, double cover);

#endif
