/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/* Region root selection; the model is described in select.h. */
#include "select.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fast.h"
#include "ir.h"

unsigned long long qjs_aot_prof_entries(const uint8_t dg[16]);
unsigned long long qjs_aot_prof_site_calls(const uint8_t dg[16], int next_pc);
unsigned long long qjs_aot_prof_pc_count(const uint8_t dg[16], int pc);
double qjs_aot_prof_weight(const uint8_t dg[16]);
int qjs_aot_prof_callees(const uint8_t dg[16], int pc, int *n, char hex[4][33], int kind[4], unsigned long long hits[4]);

#define SEL_MAXT 8

typedef struct {
  int pc, next;
  double cnt;         /* executions in the whole profile */
  int nt;             /* resolved targets, 0 for an exit-only call */
  int tgt[SEL_MAXT];  /* function indices (representatives) */
  double tc[SEL_MAXT]; /* calls to each */
} SSite;

typedef struct {
  const JSAotGenFunc *g;
  int ok;
  double self, ent, incl;
  int state; /* DFS colour */
  SSite *s;
  int ns;
} SFn;

typedef struct {
  int fn, site;
  double cnt, share;
  int expanded;
} SRef;

typedef struct {
  int root;
  double weight, incl;
  SRef *ref;
  int nref;
} SRoot;

static SFn *g_f;
static int g_nf;
static SRoot *g_r;
static int g_nr;
static double g_hot = 0.01, g_site = 0.01, g_cover = 0.5;
static int g_tinit;

void sel_set_thresholds(double hot, double site, double cover) {
  g_hot = hot;
  g_site = site;
  g_cover = cover;
  g_tinit = 1;
}

static void tinit(void) {
  const char *e;
  if (g_tinit) return;
  g_tinit = 1;
  if ((e = getenv("QJS_AOT_SEL_HOT")) != NULL) g_hot = strtod(e, NULL);
  if ((e = getenv("QJS_AOT_SEL_SITE")) != NULL) g_site = strtod(e, NULL);
  if ((e = getenv("QJS_AOT_SEL_COVER")) != NULL) g_cover = strtod(e, NULL);
}

/* The representative index of a digest: the last entry of its run in the digest-sorted array. */
static int idx_of_digest(const JSAotGenFunc *funcs, int n, const uint8_t dg[16]) {
  int lo = 0, hi = n - 1, r = -1;
  while (lo <= hi) {
    int mid = lo + (hi - lo) / 2, c = memcmp(funcs[mid].digest, dg, 16);
    if (c == 0) {
      r = mid;
      lo = mid + 1; /* keep going right: the last of the run */
    } else if (c < 0)
      lo = mid + 1;
    else
      hi = mid - 1;
  }
  return r;
}

static void hex16(const uint8_t dg[16], char out[33]) {
  int i;
  for (i = 0; i < 16; i++) snprintf(out + 2 * i, 3, "%02x", dg[i]);
}

static const char *fname(int i) {
  const char *nm = i >= 0 && i < g_nf ? g_f[i].g->name : NULL;
  return nm && nm[0] ? nm : "(anon)";
}

/* The call sites of function i with their resolved targets and per-target counts. */
static void build_sites(const JSAotGenFunc *funcs, int n, int i) {
  SFn *F = &g_f[i];
  Q2Func q;
  int k, cap = 0;
  memset(&q, 0, sizeof(q));
  if (q2_build(F->g, &q) < 0 || !q.ok) {
    q2_free(&q);
    return;
  }
  F->ok = 1;
  for (k = 0; k < q.ninsns; k++) {
    const Q2Insn *in = &q.insns[k];
    const JSAotGenFunc *tg[SEL_MAXT];
    SSite *S;
    unsigned long long calls;
    int nt, j, m, cn = 0, kind[4];
    char hx[4][33], th[33];
    unsigned long long hits[4];
    double matched = 0;
    if (in->op != Q2_OP_CALL || in->own) continue;
    calls = qjs_aot_prof_site_calls(F->g->digest, in->next);
    if (calls == 0) calls = qjs_aot_prof_pc_count(F->g->digest, in->pc);
    if (F->ns == cap) {
      cap = cap ? cap * 2 : 8;
      F->s = realloc(F->s, (size_t)cap * sizeof(SSite));
    }
    S = &F->s[F->ns++];
    memset(S, 0, sizeof(*S));
    S->pc = in->pc;
    S->next = in->next;
    S->cnt = (double)calls;
    nt = qjs_aot_bundle_targets(F->g, in->pc, tg, SEL_MAXT);
    if (nt <= 0 || calls == 0) continue;
    memset(hx, 0, sizeof(hx));
    qjs_aot_prof_callees(F->g->digest, in->next, &cn, hx, kind, hits);
    if (cn > 4) cn = 4;
    for (j = 0; j < nt; j++) {
      int ti = idx_of_digest(funcs, n, tg[j]->digest);
      if (ti < 0) {
        S->nt = 0;
        break;
      }
      S->tgt[S->nt] = ti;
      S->tc[S->nt] = 0;
      hex16(tg[j]->digest, th);
      for (m = 0; m < cn; m++)
        if (kind[m] == 1 && strcmp(hx[m], th) == 0) {
          S->tc[S->nt] = (double)hits[m];
          matched += (double)hits[m];
        }
      S->nt++;
    }
    if (S->nt > 0 && matched == 0) {
      /* The profile names none of the targets: split the site's calls evenly. */
      for (j = 0; j < S->nt; j++) S->tc[j] = S->cnt / S->nt;
    }
  }
  q2_free(&q);
}

/* Inclusive cost, memoised; an edge into a function still on the DFS stack is a back edge and carries no cost. */
static double incl_of(int i) {
  SFn *F = &g_f[i];
  int k, j;
  double v;
  if (F->state == 2) return F->incl;
  if (F->state == 1) return 0;
  F->state = 1;
  v = F->self;
  for (k = 0; k < F->ns; k++)
    for (j = 0; j < F->s[k].nt; j++) {
      int t = F->s[k].tgt[j];
      double pc;
      if (g_f[t].state == 1) continue;
      pc = incl_of(t) / (g_f[t].ent > 1 ? g_f[t].ent : 1);
      v += F->s[k].tc[j] * pc;
    }
  F->incl = v;
  F->state = 2;
  return v;
}

/* Reverse postorder of the graph below r with back edges skipped. */
static void rpo_dfs(int i, char *seen, char *onstk, int *order, int *no) {
  SFn *F = &g_f[i];
  int k, j;
  seen[i] = 1;
  onstk[i] = 1;
  for (k = 0; k < F->ns; k++)
    for (j = 0; j < F->s[k].nt; j++) {
      int t = F->s[k].tgt[j];
      if (!seen[t]) rpo_dfs(t, seen, onstk, order, no);
    }
  onstk[i] = 0;
  order[(*no)++] = i;
}

static int is_back(const char *onstk_unused, int from, int to, const int *pos) {
  (void)onstk_unused;
  return pos[to] <= pos[from]; /* position in reverse postorder: a target not strictly after its caller */
}

/* Compute the region of root r. `ref` gets one entry per call site reachable in the tree. */
static int region(int r, SRoot *out, double *ctx2) {
  int *order = malloc((size_t)g_nf * sizeof(int)), *pos = malloc((size_t)g_nf * sizeof(int)), no = 0, a, k, j, nref = 0, capr = 0;
  int *rs = calloc((size_t)g_nf, sizeof(int));
  char *seen = calloc((size_t)g_nf, 1), *onstk = calloc((size_t)g_nf, 1), *reach = calloc((size_t)g_nf, 1);
  double *ctx = calloc((size_t)g_nf, sizeof(double)), T = 0, W = 0;
  SRef *ref = NULL;
  rpo_dfs(r, seen, onstk, order, &no);
  /* order is postorder; reverse it */
  for (a = 0; a < no / 2; a++) {
    int t = order[a];
    order[a] = order[no - 1 - a];
    order[no - 1 - a] = t;
  }
  for (a = 0; a < g_nf; a++) pos[a] = g_nf + 1;
  for (a = 0; a < no; a++) pos[order[a]] = a;
  ctx[r] = g_f[r].ent > 1 ? g_f[r].ent : 1;
  for (a = 0; a < no; a++) {
    SFn *F = &g_f[order[a]];
    double sc = ctx[order[a]] / (F->ent > 1 ? F->ent : 1);
    rs[order[a]] = nref;
    for (k = 0; k < F->ns; k++) {
      SSite *S = &F->s[k];
      if (nref == capr) {
        capr = capr ? capr * 2 : 32;
        ref = realloc(ref, (size_t)capr * sizeof(SRef));
      }
      ref[nref].fn = order[a];
      ref[nref].site = k;
      ref[nref].cnt = S->cnt * sc;
      ref[nref].share = 0;
      ref[nref].expanded = 0;
      T += ref[nref].cnt;
      nref++;
      for (j = 0; j < S->nt; j++)
        if (!is_back(NULL, order[a], S->tgt[j], pos)) ctx[S->tgt[j]] += S->tc[j] * sc;
    }
  }
  /* expand: a site that runs at least g_site of the tree's call executions, in a function the region reaches */
  reach[r] = 1;
  memset(ctx, 0, (size_t)g_nf * sizeof(double));
  ctx[r] = g_f[r].ent > 1 ? g_f[r].ent : 1;
  {
    int ri;
    for (a = 0; a < no; a++) {
      int f = order[a];
      SFn *F = &g_f[f];
      double sc = ctx[f] / (F->ent > 1 ? F->ent : 1);
      ri = rs[f];
      for (k = 0; k < F->ns; k++, ri++) {
        SSite *S = &F->s[k];
        ref[ri].share = T > 0 ? ref[ri].cnt / T : 0;
        if (!reach[f] || S->nt == 0 || S->cnt <= 0 || ref[ri].share < g_site) continue;
        ref[ri].expanded = 1;
        for (j = 0; j < S->nt; j++)
          if (!is_back(NULL, f, S->tgt[j], pos)) {
            reach[S->tgt[j]] = 1;
            ctx[S->tgt[j]] += S->tc[j] * sc;
          }
      }
      if (reach[f]) W += F->self * sc;
    }
  }
  memcpy(ctx2, ctx, (size_t)g_nf * sizeof(double));
  out->root = r;
  out->weight = W;
  out->incl = g_f[r].incl;
  out->ref = ref;
  out->nref = nref;
  free(rs);
  free(order);
  free(pos);
  free(seen);
  free(onstk);
  free(reach);
  free(ctx);
  return 0;
}

static double *g_sortkey;
static int cmp_key(const void *a, const void *b) {
  double x = g_sortkey[*(const int *)a], y = g_sortkey[*(const int *)b];
  return x < y ? 1 : x > y ? -1 : (*(const int *)a - *(const int *)b);
}

int sel_roots(const JSAotGenFunc *funcs, int n, int *roots, int max) {
  int i, c, nc = 0, *cand, nr = 0;
  double *cw, *cov;
  SRoot *tmp;
  tinit();
  g_f = calloc((size_t)(n > 0 ? n : 1), sizeof(SFn));
  g_nf = n;
  for (i = 0; i < g_nr; i++) free(g_r[i].ref);
  free(g_r);
  g_r = NULL;
  g_nr = 0;
  for (i = 0; i < n; i++) {
    SFn *F = &g_f[i];
    F->g = &funcs[i];
    if (i + 1 < n && memcmp(funcs[i].digest, funcs[i + 1].digest, 16) == 0) continue; /* not the representative */
    if (funcs[i].eligible != AOT_GEN_ELIGIBLE) continue;
    F->ent = (double)qjs_aot_prof_entries(funcs[i].digest);
    F->self = qjs_aot_prof_weight(funcs[i].digest) / 100.0;
    if (F->ent > 0 || F->self > 0) build_sites(funcs, n, i);
  }
  for (i = 0; i < n; i++)
    if (g_f[i].ok) incl_of(i);
  cand = malloc((size_t)(n > 0 ? n : 1) * sizeof(int));
  cw = calloc((size_t)(n > 0 ? n : 1), sizeof(double));
  cov = calloc((size_t)(n > 0 ? n : 1), sizeof(double));
  tmp = calloc((size_t)(n > 0 ? n : 1), sizeof(SRoot));
  {
    double *ctx2 = malloc((size_t)(n > 0 ? n : 1) * sizeof(double));
    for (i = 0; i < n; i++) {
      if (!g_f[i].ok || g_f[i].ent <= 0 || g_f[i].incl < g_hot) continue;
      region(i, &tmp[i], ctx2);
      cw[i] = tmp[i].weight;
      if (cw[i] >= g_hot) cand[nc++] = i;
      else free(tmp[i].ref), tmp[i].ref = NULL;
    }
    g_sortkey = cw;
    qsort(cand, (size_t)nc, sizeof(int), cmp_key);
    g_r = calloc((size_t)(nc > 0 ? nc : 1), sizeof(SRoot));
    for (c = 0; c < nc; c++) {
      int r = cand[c], covered;
      SFn *F = &g_f[r];
      covered = cov[r] >= g_cover * (F->ent > 1 ? F->ent : 1);
      if (covered) {
        free(tmp[r].ref);
        tmp[r].ref = NULL;
        continue;
      }
      /* recompute the invocations this region gives each function, over its expanded sites only */
      {
        int k;
        double *ctx = ctx2;
        region(r, &g_r[g_nr], ctx);
        free(tmp[r].ref);
        tmp[r].ref = NULL;
        for (k = 0; k < n; k++)
          if (k != r) cov[k] += ctx[k];
      }
      g_nr++;
      if (nr < max) roots[nr++] = r;
    }
    free(ctx2);
  }
  free(cand);
  free(cw);
  free(cov);
  free(tmp);
  return nr;
}

static const SRoot *root_of(const JSAotGenFunc *root) {
  int i, r;
  if (g_f == NULL || root == NULL) return NULL;
  for (i = 0; i < g_nr; i++) {
    r = g_r[i].root;
    if (memcmp(g_f[r].g->digest, root->digest, 16) == 0) return &g_r[i];
  }
  return NULL;
}

int sel_expand_site(const JSAotGenFunc *root, const JSAotGenFunc *g, int pc) {
  const SRoot *R = root_of(root);
  int i;
  if (R == NULL || g == NULL) return 0;
  for (i = 0; i < R->nref; i++) {
    const SRef *e = &R->ref[i];
    if (g_f[e->fn].s[e->site].pc == pc && memcmp(g_f[e->fn].g->digest, g->digest, 16) == 0) return e->expanded;
  }
  return 0;
}

void sel_report(FILE *f) {
  int i, k;
  fprintf(f, "# region roots: %d (hot >= %.3g of the profile, site >= %.3g of the tree's call executions, cover %.3g)\n", g_nr, g_hot, g_site, g_cover);
  for (i = 0; i < g_nr; i++) {
    const SRoot *R = &g_r[i];
    int ne = 0, np = 0;
    for (k = 0; k < R->nref; k++) {
      const SRef *e = &R->ref[k];
      if (g_f[e->fn].s[e->site].cnt <= 0) continue;
      if (e->expanded) ne++;
      else np++;
    }
    {
      char hx[33];
      int q;
      for (q = 0; q < 16; q++) snprintf(hx + 2 * q, 3, "%02x", g_f[R->root].g->digest[q]);
      fprintf(f, "root-hex %s\n", hx);
    }
    fprintf(f, "root %s (%s): region %.2f%%, inclusive %.2f%%, %d sites expanded, %d pruned\n", fname(R->root), g_f[R->root].g->file, 100 * R->weight,
        100 * R->incl, ne, np);
    for (k = 0; k < R->nref; k++) {
      const SRef *e = &R->ref[k];
      const SSite *S = &g_f[e->fn].s[e->site];
      int j;
      if (S->cnt <= 0) continue;
      fprintf(f, "  %s %s:%d  %.3g%% of calls", e->expanded ? "expand" : "prune ", fname(e->fn), S->pc, 100 * e->share);
      if (S->nt == 0) fprintf(f, "  (exit: unresolved)");
      for (j = 0; j < S->nt; j++) fprintf(f, "%s%s", j ? "," : "  -> ", fname(S->tgt[j]));
      fprintf(f, "\n");
    }
  }
}

static int *g_rootidx;
static int g_nrootidx;

void sel_prepare(const JSAotGenFunc *funcs, int n, FILE *rep) {
  int *roots;
  if (qjsaot_region_mode() == 0) return;
  free(g_rootidx);
  g_nrootidx = 0;
  roots = malloc((size_t)(n > 0 ? n : 1) * sizeof(int));
  g_rootidx = roots;
  if (roots == NULL) return;
  g_nrootidx = sel_roots(funcs, n, roots, n);
  if (rep != NULL) sel_report(rep);
}

int sel_is_root(int fi) {
  int i;
  for (i = 0; i < g_nrootidx; i++)
    if (g_rootidx[i] == fi) return 1;
  return 0;
}
