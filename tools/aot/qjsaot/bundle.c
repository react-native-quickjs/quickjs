#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emit.h"
#include "fast.h"
void qjs_aot_bundle_report_receivers(FILE *f);
static void report_mshapes(FILE *f);
#include "ir.h"
#include "quickjs-aot-gen.h"
#include "quickjs-aot-rt.h"
#include "quickjs-regvm.h"
#include "quickjs.h"

/* ---- bundle analysis (AOT-REDESIGN.md, R0) ---------------------------------------------
   The whole bundle is one closed program. This pass reads every function's register program
   (as the Q2 IR the generator already builds) and computes, without changing any code:

   - an abstract heap with one abstract object per allocation site (object and array literals,
     constructs, closures and the prototype object each closure owns), cloned per call site for
     small callees so a helper like `inheritsFrom` does not merge its callers;
   - per field of each abstract object a slot lattice (undefined, null, bool, int, int-or-overflow,
     double, number, string, object(site set), unknown) and the order in which fields are added;
   - the prototype set of every object (protos[0..n]) from the flow of `prototype` fields;
   - the call graph: every call site's targets (static resolution of the points-to sets), the
     builtins it recognises, and the calls it cannot resolve;
   - effect summaries per function (field names written, element writes, global writes,
     allocation, unknown calls, throw), transitive over the call graph;
   - escape facts per site (stored, passed, returned, escaped to unknown code);
   - the bytecode's static hotness hints (functions called inside a loop; functions reachable
     from there).

   It runs twice. The "per-name" layer keys every field by its name alone (what the source scan
   `scan.c` does for slot claims); the "points-to" layer keys fields by (abstract object, name).
   The report lists the facts each layer keeps, so what the heap model buys is visible, and every
   place a fact can still break (eval, `with`, delete, prototype changes, computed keys, unknown
   calls that receive objects, closures stored in unknown places).

   Soundness: the model is flow-insensitive per function. Where it assumes something the
   running program must still guard (an assignment to `F.prototype` before the first
   construct of F), the report says so. Nothing here changes generated code. */

enum { K_UNDEF = 1, K_NULL = 2, K_BOOL = 4, K_INT = 8, K_INTOV = 16, K_DBL = 32, K_NUM = 64, K_STR = 128, K_OBJ = 256, K_UNK = 512 };
#define K_NUMERIC (K_INT | K_DBL | K_NUM | K_INTOV)

#define BN_W 16
#define BN_MAXSITES (BN_W * 64)
typedef struct BSet { uint64_t w[BN_W]; } BSet;
typedef struct BNode { BSet pts; uint16_t cls; } BNode;

static int bs_add(BSet *d, int b) {
  uint64_t m = 1ull << (b & 63);
  if (d->w[b >> 6] & m) return 0;
  d->w[b >> 6] |= m;
  return 1;
}
static int bs_has(const BSet *d, int b) { return (d->w[b >> 6] >> (b & 63)) & 1; }
static int bs_or(BSet *d, const BSet *s) {
  int i, ch = 0;
  for (i = 0; i < BN_W; i++) {
    uint64_t n = d->w[i] | s->w[i];
    if (n != d->w[i]) { d->w[i] = n; ch = 1; }
  }
  return ch;
}
static int bs_count(const BSet *d) {
  int i, n = 0;
  for (i = 0; i < BN_W; i++) n += __builtin_popcountll(d->w[i]);
  return n;
}
#define BS_FOR(s, set) for ((s) = 0; (s) < BN_MAXSITES; (s)++) if (bs_has((set), (s)))

enum { SK_EXT, SK_OBJ, SK_ARR, SK_FUNC, SK_PROTO, SK_INST, SK_BUILTIN, SK_NATIVE };
static const char *const sk_names[] = {"ext", "object", "array", "function", "prototype", "instance", "builtin", "native"};
enum { SF_STORED = 1, SF_PASSED = 2, SF_UNKARG = 4, SF_RET = 8, SF_RECV = 16, SF_THIS = 32, SF_GLOBAL = 64, SF_CLOSURE = 128 };

typedef struct BSite {
  int kind, fn;   /* FUNC: the function; INST: the constructor's function if known; BUILTIN: name id */
  int afn, apc;   /* the function and pc that allocates it */
  int ctx;        /* the call-site context of the allocating instance (0: none) */
  int owner;      /* PROTO: its function site */
  int flags;      /* SF_* */
  int proto_assigned; /* FUNC: the bundle assigns F.prototype */
  char label[80];
  int esc_fn, esc_pc, esc_phase;
  int ainst, ctor_inst;
  const char *esc_why;
  BSet proto;     /* explicit prototype objects */
} BSite;

typedef struct BField {
  int site, name, node;
  int firstfn, firstpc, firstblock, nwrites, order;
  int prog;   /* some store of the analysed program writes this field (set in every round, unlike nwrites): an engine-made link such as a default prototype's `constructor` has none */
} BField;

typedef struct BInst {
  int live; /* reachable: called from the bundle, escaped, or an entry */
  int fn, ctx, base, thisn, retn, locbase, nloc;
  int parn[17];
  int npar;
  int extra[16];   /* the actual argument at position k >= npar, kept apart (parn[16] merges them): what `f.apply(this, arguments)` forwards position by position */
} BInst;

typedef struct BEdge { int cfn, cpc, tfn, inloop; } BEdge;
/* the receivers of a field access, per function and pc (all instances of the function merged), for the code generator */
typedef struct BAcc { int fn, pc, kind, name; unsigned cls; BSet recv; } BAcc;
/* a method call: the receivers of the call at (fn, pc), and per receiver object the function the method name resolves to (dispatch);
   nodisp: some target was linked to the whole receiver set, not per object */
typedef struct BRcv { int fn, pc; unsigned cls; int nodisp; BSet recv; } BRcv;
typedef struct BRT { int fn, pc, site, tfn; } BRT;
/* what the transition walk needs: writes to a site in program order and the calls that carry it as `this` */
typedef struct BEv { int inst, pc, blk, site, name, callee, seq; BSet this_set; } BEv;
typedef struct BCall { int fn, pc, rop, targets[8], nt, unknown, builtin, inloop, thru; char callee[40]; } BCall;

#define BN_NAMES 1024
static char *g_bn_names[BN_NAMES];
static int g_bn_nnames;

typedef struct BFn {
  const JSAotGenFunc *g;
  Q2Func q;
  int ok;
  int has_loop;
  uint8_t *inloop; /* per block */
  uint8_t *uncond; /* per block: every path to a return passes through it */
  int hot;         /* 0 none, 1 reachable from hot code, 2 called inside a loop */
  int nexec;
  /* own effects */
  uint64_t wn[BN_NAMES / 64];
  uint64_t twn[BN_NAMES / 64];
  int welem, twelem, wglobal, twglobal, alloc, talloc, unkcall, tunkcall, thrw, tthrw, wvarref, twvarref;
  /* closure cells: the functions whose make_closure creates this one, and which of this function's own locals / arguments some inner function captures */
  int ncreat, creat[4];
  uint8_t *cap_loc, *cap_arg;
  int evalish;   /* a direct eval, a with, or a mapped arguments object: every variable it can see is written by code the analysis cannot follow */
} BFn;

typedef struct BKey { int afn, apc, ctx; } BKey;
static BKey *g_assigned;
static int g_nassigned, g_assigned_cap;
static int assigned_has(int afn, int apc, int ctx);

unsigned long long qjs_aot_prof_entries(const uint8_t dg[16]);
unsigned long long qjs_aot_prof_pc_count(const uint8_t dg[16], int pc);
static JSContext *g_bctx;
static const JSAotGenFunc *g_bfuncs;
static int g_bnfuncs;
static BFn *g_bfn;

/* ---- one analysis run --------------------------------------------------------------- */
typedef struct BRun {
  int pts;                  /* 0: per-name, 1: points-to */
  BNode *nd; int nnd, ndcap;
  BSite *st; int nst;
  BField *fl; int nfl, flcap;
  int *fhash; int fhcap;
  BInst *in; int nin;
  BSet esc;
  int *globals;             /* name id -> node, -1 */
  int varref_node, sink_node, tmp_in, tmp_out;
  int changed, rounds;
  int oracle_fixed; char oracle_names[400];   /* functions the profile oracle had to escape (lost flows), and their names */
  int seal, nseal;   /* the profile-oracle pass: an empty receiver at an executed instruction becomes unknown; how many it sealed */
  int collect;
  /* collected facts */
  BEdge *ed; int ned, edcap;
  BCall *cs; int ncs, cscap;
  int unmodeled[256];
  int dyn_key_stores, delete_ops, setproto_ops, eval_ops, with_ops, copyprops_ops, special_args, thru_calls, native_typed;
  int unk_calls, unk_calls_objargs, escaped_funcs;
  int overflow_sites, accessors;
  BEv *ev; int nev, evcap;
  BAcc *acc; int nacc, acccap;
  BRcv *rcv; int nrcv, rcvcap;
  BRT *rt; int nrt, rtcap;
  int *cellk, *celln, ncell, cellcap;   /* closure-variable cells: key (declaring function, is-arg, index) -> node */
  int phase; /* 0: only what the program defines; 1: also the effect of reads and calls that resolve to nothing */
} BRun;

static BRun *R;
static void ev_add(int inst, int pc, int blk, int site, int name, int callee, const struct BSet *this_set);
static int g_cur_inst = -1;
static int g_dbg_events;
static void cls_str_fwd(unsigned c, char *out, size_t n);
static void set_str_fwd(const BRun *r, const BSet *set, char *out, size_t n, int maxn);
static uint8_t g_gdef[BN_NAMES];
static uint8_t *g_made; /* per function: some make_closure in the bundle creates it */
static const char *fn_name(int fn);

static int bn_name(const char *s) {
  int i;
  for (i = 0; i < g_bn_nnames; i++)
    if (strcmp(g_bn_names[i], s) == 0) return i;
  if (g_bn_nnames >= BN_NAMES) return BN_NAMES - 1;
  g_bn_names[g_bn_nnames] = strdup(s);
  return g_bn_nnames++;
}

static int bn_atom(const Q2Insn *in, int *which, char *out, size_t cap, int skip) {
  int a, seen = 0;
  for (a = 0; a < in->narg; a++) {
    if (in->arg[a].kind != 'a') continue;
    if (seen++ < skip) continue;
    {
      size_t len;
      const char *nm = JS_AtomToCStringLen(g_bctx, &len, (JSAtom)in->arg[a].val);
      if (nm == NULL) return 0;
      snprintf(out, cap, "%s", nm);
      JS_FreeCString(g_bctx, nm);
    }
    if (which) *which = a;
    return 1;
  }
  return 0;
}

static int nd_new(void) {
  if (R->nnd >= R->ndcap) {
    R->ndcap = R->ndcap ? R->ndcap * 2 : 4096;
    R->nd = realloc(R->nd, (size_t)R->ndcap * sizeof(BNode));
  }
  memset(&R->nd[R->nnd], 0, sizeof(BNode));
  return R->nnd++;
}

static int nd_flow(int d, int s) {
  int ch = 0;
  if (d == s || s < 0 || d < 0) return 0;
  if (bs_or(&R->nd[d].pts, &R->nd[s].pts)) ch = 1;
  if ((R->nd[d].cls | R->nd[s].cls) != R->nd[d].cls) { R->nd[d].cls |= R->nd[s].cls; ch = 1; }
  if (ch) R->changed = 1;
  return ch;
}
static void nd_cls(int d, int k) {
  if (d < 0) return;
  if ((R->nd[d].cls | k) != R->nd[d].cls) { R->nd[d].cls |= k; R->changed = 1; }
}
static void nd_site(int d, int s) {
  if (d < 0) return;
  if (bs_add(&R->nd[d].pts, s)) R->changed = 1;
  nd_cls(d, K_OBJ);
}

/* sites */
static int st_new(int kind, int fn, int afn, int apc, int ctx, const char *label) {
  int i;
  for (i = 0; i < R->nst; i++)
    if (R->st[i].kind == kind && R->st[i].afn == afn && R->st[i].apc == apc && R->st[i].ctx == ctx && R->st[i].fn == fn &&
        (kind != SK_PROTO || 1))
      return i;
  if (R->nst >= BN_MAXSITES) { R->overflow_sites++; return 0; }
  memset(&R->st[R->nst], 0, sizeof(BSite));
  R->st[R->nst].kind = kind;
  R->st[R->nst].fn = fn;
  R->st[R->nst].afn = afn;
  R->st[R->nst].apc = apc;
  R->st[R->nst].ctx = ctx;
  R->st[R->nst].owner = -1;
  R->st[R->nst].ainst = g_cur_inst;
  R->st[R->nst].ctor_inst = -1;
  snprintf(R->st[R->nst].label, sizeof(R->st[R->nst].label), "%s", label);
  if (kind == SK_FUNC && R->pts && g_nassigned > 0 && assigned_has(afn, apc, ctx)) R->st[R->nst].proto_assigned = 1;
  R->changed = 1;
  return R->nst++;
}

/* fields */
static int fl_find(int site, int name, int create) {
  unsigned h = ((unsigned)site * 2654435761u) ^ ((unsigned)name * 40503u);
  int i;
  if (R->fhcap > 0) h %= (unsigned)R->fhcap;
  for (i = 0; i < R->fhcap; i++) {
    int e = R->fhash[(h + (unsigned)i) % (unsigned)R->fhcap];
    if (e < 0) break;
    if (R->fl[e].site == site && R->fl[e].name == name) return e;
  }
  if (!create) return -1;
  if (R->nfl >= R->flcap) {
    R->flcap = R->flcap ? R->flcap * 2 : 1024;
    R->fl = realloc(R->fl, (size_t)R->flcap * sizeof(BField));
    /* rehash into a table at least twice the entry count */
    free(R->fhash);
    R->fhcap = R->flcap * 4 + 1;
    R->fhash = malloc((size_t)R->fhcap * sizeof(int));
    memset(R->fhash, 0xff, (size_t)R->fhcap * sizeof(int));
    for (i = 0; i < R->nfl; i++) {
      unsigned hh = (((unsigned)R->fl[i].site * 2654435761u) ^ ((unsigned)R->fl[i].name * 40503u)) % (unsigned)R->fhcap;
      while (R->fhash[hh] >= 0) hh = (hh + 1) % (unsigned)R->fhcap;
      R->fhash[hh] = i;
    }
  }
  h = (((unsigned)site * 2654435761u) ^ ((unsigned)name * 40503u)) % (unsigned)R->fhcap;
  while (R->fhash[h] >= 0) h = (h + 1) % (unsigned)R->fhcap;
  memset(&R->fl[R->nfl], 0, sizeof(BField));   /* realloc'd memory: nwrites and the rest were read uninitialised (per-name layer output varied run to run) */
  R->fl[R->nfl].site = site;
  R->fl[R->nfl].name = name;
  R->fl[R->nfl].node = nd_new();
  R->fl[R->nfl].firstfn = -1;
  R->fl[R->nfl].order = R->nfl;
  R->fhash[h] = R->nfl;
  return R->nfl++;
}
/* the node of a field; per-name mode ignores the site */
static int fnode(int site, int name, int create) {
  int e = fl_find(R->pts ? site : -1, name, create);
  return e < 0 ? -1 : R->fl[e].node;
}

/* instances */
static int small_fn(int fn) { return g_bfn[fn].ok && g_bfn[fn].q.ninsns <= 110; }

static int inst_get(int fn, int ctx) {
  int i, k;
  if (!R->pts) ctx = 0;
  for (i = 0; i < R->nin; i++)
    if (R->in[i].fn == fn && R->in[i].ctx == ctx) return i;
  R->in = realloc(R->in, (size_t)(R->nin + 1) * sizeof(BInst));
  {
    BInst *x = &R->in[R->nin];
    const JSAotGenFunc *g = g_bfn[fn].g;
    memset(x, 0, sizeof(*x));
    x->fn = fn;
    x->ctx = ctx;
    x->base = R->nnd;
    for (k = 0; k < g_bfn[fn].q.nvalues; k++) nd_new();
    x->thisn = nd_new();
    x->retn = nd_new();
    x->npar = g->arg_count > 16 ? 16 : g->arg_count;
    for (k = 0; k < 17; k++) x->parn[k] = nd_new();
    for (k = 0; k < 16; k++) x->extra[k] = nd_new();
    x->nloc = g->var_count > 0 ? g->var_count : 0;
    x->locbase = R->nnd;
    for (k = 0; k < x->nloc; k++) nd_new();
  }
  R->changed = 1;
  return R->nin++;
}

static int vnode(const BInst *I, Q2ValueId v) {
  if (v == Q2_NO_VALUE || (int32_t)v < 0 || (int32_t)v >= g_bfn[I->fn].q.nvalues) return R->sink_node;
  return I->base + (int)v;
}
/* ---- closure-variable cells -------------------------------------------------------------------------------
   A variable captured by an inner function is one cell for the whole program: key (declaring function, is-argument, index). The declaring function's own
   accesses of a captured local or argument use the cell's node, and so does every get_var_ref / put_var_ref that resolves (through the closure-variable
   tables) to it. A variable some code the analysis cannot follow can write (a direct eval or with in a function that sees it) is a POISONED cell: unknown
   contents, and whatever is stored in it escapes. Only the points-to layer uses cells; the per-name layer keeps one shared node. */
static void escape_node(int n);
static int g_dbg_max = 40;
static const char *g_cur_why;
static int *g_poison; static int g_npoison, g_poisoncap;
static int g_cell_firstwrite = -1;
static int g_x_nodst2, g_x_noacc, g_x_nocomp, g_diag_noredef;   /* bisect knobs */
static int g_nolost;   /* vkit knock-out (unsound): no implicit-callee escape, no profile oracle */
static int g_diag_noforin;   /* vkit knock-out: for-in as an unknown op again */
static int g_diag_nomerge;   /* vkit knock-out: no merging of same-named empty literals */
static int g_diag_noelem;   /* vkit knock-out (unsound): computed keys are not consulted */
static int g_nocells;   /* QJS_AOT_BUNDLE_NOCELLS=1: the old model, one shared node for every closure variable */
#define CELLS_ON (R->pts && !g_nocells)
static int cell_key(int fn, int isarg, int idx) { return (fn << 17) | (isarg << 16) | idx; }
static int cell_poisoned(int key) { int i; for (i = 0; i < g_npoison; i++) if (g_poison[i] == key) return 1; return 0; }
static void cell_poison(int key) {
  if (cell_poisoned(key)) return;
  if (g_npoison == g_poisoncap) { g_poisoncap = g_poisoncap ? g_poisoncap * 2 : 64; g_poison = realloc(g_poison, (size_t)g_poisoncap * sizeof(int)); }
  g_poison[g_npoison++] = key;
}
static int cell_node(int fn, int isarg, int idx) {
  int key = cell_key(fn, isarg, idx), i, n;
  for (i = 0; i < R->ncell; i++) if (R->cellk[i] == key) return R->celln[i];
  if (R->ncell == R->cellcap) { R->cellcap = R->cellcap ? R->cellcap * 2 : 64; R->cellk = realloc(R->cellk, (size_t)R->cellcap * sizeof(int)); R->celln = realloc(R->celln, (size_t)R->cellcap * sizeof(int)); }
  n = nd_new();
  R->cellk[R->ncell] = key; R->celln[R->ncell] = n; R->ncell++;
  if (cell_poisoned(key)) { nd_cls(n, K_UNK); nd_site(n, 0); }
  return n;
}
static int cell_is_poisoned_node(int node) { int i; for (i = 0; i < R->ncell; i++) if (R->celln[i] == node) return cell_poisoned(R->cellk[i]); return 0; }
/* a store into a cell: the value flows in; into a poisoned cell it also escapes */
static void cell_store(int cell, int val) {
  if (g_cell_firstwrite < 0) g_cell_firstwrite = getenv("QJS_AOT_BUNDLE_DIAG_CELL_FIRSTWRITE") != NULL;   /* vkit knock-out, unsound: a cell keeps what its first writer stored */
  if (g_cell_firstwrite && (bs_count(&R->nd[cell].pts) > 0 || R->nd[cell].cls != 0)) return;
  nd_flow(cell, val);
  if (cell_is_poisoned_node(cell)) { g_cur_why = "stored into a variable that code the analysis cannot follow can write"; escape_node(val); }
}
/* closure variable `slot` of function `fn`: the cell it names, or 0 when the tables do not resolve it */
static int vref_cell(int fn, int slot, int depth, int *cfn, int *isarg, int *idx) {
  const JSAotGenFunc *g = g_bfn[fn].g;
  int t, ix, P;
  if (depth > 8 || g->closure_type == NULL || slot < 0 || slot >= g->closure_count || g_bfn[fn].ncreat != 1) return 0;
  t = g->closure_type[slot]; ix = g->closure_idx[slot]; P = g_bfn[fn].creat[0];
  if (t == 0 || t == 1) { *cfn = P; *isarg = t; *idx = ix; return 1; }
  if (t == 2) return vref_cell(P, ix, depth + 1, cfn, isarg, idx);
  return 0;
}
static int locnode(const BInst *I, int k) {
  if (k < 0 || k >= I->nloc) return R->sink_node;
  if (CELLS_ON && g_bfn[I->fn].cap_loc != NULL && g_bfn[I->fn].cap_loc[k]) return cell_node(I->fn, 0, k);
  return I->locbase + k;
}
static int parnode(const BInst *I, int k) { return k >= 0 && k < 16 ? I->parn[k] : I->parn[16]; }
/* the node a read or write of argument k of this instance uses: the cell when an inner function captures it */
static int argnode(const BInst *I, int k) {
  const BFn *F = &g_bfn[I->fn];
  if (CELLS_ON && F->cap_arg != NULL && k >= 0 && k < 16 && k < F->g->arg_count && F->cap_arg[k]) return cell_node(I->fn, 1, k);
  return parnode(I, k);
}
static int gnode(int name) {
  if (R->globals[name] < 0) R->globals[name] = nd_new();
  return R->globals[name];
}

static int g_cur_fn, g_cur_pc, g_cur_rop;
static const char *g_cur_why = "";

/* the objects in `src` reach unknown code; the first cause is kept for the report */
static void esc_add(const BSet *src) {
  int s;
  for (s = 0; s < BN_MAXSITES; s++)
    if (bs_has(src, s) && !bs_has(&R->esc, s)) {
      bs_add(&R->esc, s);
      R->changed = 1;
      if (getenv("QJS_AOT_BUNDLE_ESCALL") != NULL) fprintf(stderr, "escape-all: %s <- %s (fn %d pc %d) pts=%d phase %d\n", R->st[s].label, g_cur_why, g_cur_fn, g_cur_pc, R->pts, R->phase);
      if (R->st[s].esc_fn == 0 && R->st[s].esc_pc == 0 && R->st[s].esc_why == NULL) {
        R->st[s].esc_fn = g_cur_fn + 1;
        R->st[s].esc_pc = g_cur_pc;
        R->st[s].esc_why = g_cur_why;
        R->st[s].esc_phase = R->phase;
        if (R->pts && g_dbg_events < g_dbg_max && getenv("QJS_AOT_BUNDLE_EVENTS") != NULL) {
          g_dbg_events++;
          { char rn[40]; e_rop_name((uint16_t)g_cur_rop, rn, sizeof(rn)); fprintf(stderr, "bundle: ESCAPE %s <- %s (%s) at %s:%d phase %d\n", R->st[s].label, g_cur_why, rn, g_cur_fn >= 0 ? fn_name(g_cur_fn) : "-", g_cur_pc, R->phase); }
        }
      }
    }
}
static void escape_node(int n) {
  if (n < 0) return;
  esc_add(&R->nd[n].pts);
}

/* the sites a value may be: an object set with the external site 0 standing for anything not in the bundle */
static int site_is_ext(int s) { return s == 0 || R->st[s].kind == SK_EXT || R->st[s].kind == SK_NATIVE; }

enum { S_EXT = 0, S_OBJCTOR = 1, S_OBJPROTO = 2 };

static int builtin_site(const char *what, const char *name) {
  char lb[80];
  int id = bn_name(name);
  snprintf(lb, sizeof(lb), "%s.%s", what, name);
  return st_new(SK_BUILTIN, id, -1, -1, 0, lb);
}


/* The profile as a soundness oracle: an instruction the profiled run executed whose receiver set is EMPTY lost a flow in the analysis (an object was there). In the sealing rounds (after the
   fixpoint) such a receiver becomes unknown (top), never "no sites". Primitive-only receivers (a class but no sites) are left alone. */
static void seal_recv(int node, int fn, int pc) {
  if (!R->seal || !R->pts || node < 0 || node == R->sink_node) return;
  if (R->nd[node].cls != 0 || bs_count(&R->nd[node].pts) != 0) return;
  if (fn < 0 || !g_bfn[fn].g || qjs_aot_prof_pc_count(g_bfn[fn].g->digest, pc) == 0) return;
  nd_cls(node, K_UNK); nd_site(node, 0);
  R->nseal++; R->changed = 1;
}


/* Names the engine itself calls (ToPrimitive, the iterator and thenable protocols, JSON): a function stored under one of them can run from code the analysis cannot see, so it escapes
   (live with unknown this and arguments). get/set are accessor kinds, handled where accessors are defined, not by name (user classes have methods called get/set). */
static int implicit_callee_name(const char *nm) {
  static const char *const names[] = {"valueOf", "toString", "toLocaleString", "toJSON", "next", "return", "throw", "then", "catch", "finally", "constructor", "handleEvent", NULL};
  int i;
  for (i = 0; names[i]; i++) if (strcmp(nm, names[i]) == 0) return 1;
  return 0;
}

/* a read of `name` from the objects in `recv` into dst */
static void read_field(int dst, int recv, int name, int fn, int pc) {
  int s;
  const char *nm = g_bn_names[name];
  seal_recv(recv, fn, pc);
  if (!R->pts) {
    int f = fnode(-1, name, 1);
    nd_flow(dst, f);
    if (bs_count(&R->nd[recv].pts) > 0 || R->nd[recv].cls & K_UNK) {}
    /* a name no store ever gives stays an external read (builtins) */
    if (R->fl[fl_find(-1, name, 0)].nwrites == 0) { nd_cls(dst, K_UNK); nd_site(dst, 0); }
    return;
  }
  BS_FOR(s, &R->nd[recv].pts) {
    BSet seen;
    int stack[64], sp = 0, found = 0, depth = 0;
    if (site_is_ext(s) || bs_has(&R->esc, s)) {
      nd_cls(dst, K_UNK);
      nd_site(dst, 0);
      if (site_is_ext(s)) continue;
    }
    if (R->st[s].kind == SK_ARR && strcmp(nm, "length") == 0) { nd_cls(dst, K_INT); continue; }
    memset(&seen, 0, sizeof(seen));
    stack[sp++] = s;
    bs_add(&seen, s);
    while (sp > 0 && depth++ < 200) {
      int c = stack[--sp], f, p;
      f = fl_find(c, name, 0);
      if (f >= 0) {
        nd_flow(dst, R->fl[f].node);
        found = 1;
      }
      {
        /* a computed-key store on this site may have defined any name */
        int fe_ = R->st[c].kind == SK_ARR || g_diag_noelem ? -1 : fl_find(c, bn_name("[]"), 0);   /* an array's [] holds its indexed elements, not named properties */
        if (fe_ >= 0 && strcmp(nm, "[]") != 0) {
          static const char *const bi_[] = {"toString", "valueOf", "toLocaleString", "hasOwnProperty", "isPrototypeOf", "propertyIsEnumerable", "constructor", "__proto__",
                                            "__defineGetter__", "__defineSetter__", "__lookupGetter__", "__lookupSetter__", NULL};
          int k_, isb_ = 0;
          nd_flow(dst, R->fl[fe_].node);
          for (k_ = 0; bi_[k_]; k_++) if (strcmp(nm, bi_[k_]) == 0) isb_ = 1;
          if (R->st[c].kind == SK_FUNC && (strcmp(nm, "call") == 0 || strcmp(nm, "apply") == 0 || strcmp(nm, "bind") == 0 || strcmp(nm, "name") == 0 || strcmp(nm, "length") == 0)) isb_ = 1;   /* Function.prototype's own */
          if (!isb_) found = 1;   /* a name Object.prototype does not define: the computed store is the only candidate besides undefined (the miss branch's unknown inherited builtin is not one) */
        }
      }
      if (R->st[c].kind == SK_FUNC && strcmp(nm, "prototype") == 0 && !R->st[c].proto_assigned) {
        int pp;
        for (pp = 0; pp < R->nst; pp++)
          if (R->st[pp].kind == SK_PROTO && R->st[pp].owner == c) { nd_site(dst, pp); found = 1; }
      }
      BS_FOR(p, &R->st[c].proto) {
        if (!bs_has(&seen, p) && sp < 60) { bs_add(&seen, p); stack[sp++] = p; }
      }
      if (R->st[c].kind != SK_BUILTIN && c != S_OBJPROTO && !bs_has(&seen, S_OBJPROTO) && sp == 0 && R->st[c].kind != SK_EXT) {
        bs_add(&seen, S_OBJPROTO);
        stack[sp++] = S_OBJPROTO;
      }
      /* an instance's prototype comes from its constructor's `prototype` field; resolved in construct */
    }
    if (!found) {
      /* a builtin array method or an inherited builtin */
      if (R->st[s].kind == SK_FUNC && (strcmp(nm, "call") == 0 || strcmp(nm, "apply") == 0 || strcmp(nm, "bind") == 0)) {
        nd_site(dst, builtin_site("Function", nm));
      } else if (R->phase == 0 && R->st[s].kind != SK_ARR) {
        /* misses settle at the end of the first phase */
      } else if (R->st[s].kind == SK_ARR) {
        int b, bi = -1;
        for (b = 0; b < R->nst; b++)
          if (R->st[b].kind == SK_BUILTIN && R->st[b].fn == name) bi = b;
        if (bi < 0) {
          char lb[80];
          snprintf(lb, sizeof(lb), "Array.prototype.%s", nm);
          bi = st_new(SK_BUILTIN, name, -1, -1, 0, lb);
        }
        nd_site(dst, bi);
      } else {
        nd_cls(dst, K_UNDEF | K_UNK);
        nd_site(dst, 0);
        if (R->pts && R->phase == 1 && g_dbg_events < g_dbg_max && getenv("QJS_AOT_BUNDLE_EVENTS") != NULL)
        {g_dbg_events++;
          char sl[300];
          set_str_fwd(R, &R->nd[recv].pts, sl, sizeof(sl), 6);
          fprintf(stderr, "bundle: read .%s in %s:%d misses on %s (%s); receiver set [%s]\n", nm, fn_name(fn), pc, R->st[s].label, sk_names[R->st[s].kind], sl);
        }
      }
    }
  }
}

static void write_field(int recv, int name, int val, int fn, int pc, int blk) {
  int s;
  seal_recv(recv, fn, pc);
  if (!R->pts) {
    int f = fl_find(-1, name, 1);
    nd_flow(R->fl[f].node, val);
    if (R->collect) { R->fl[f].nwrites++; if (R->fl[f].firstfn < 0) { R->fl[f].firstfn = fn; R->fl[f].firstpc = pc; R->fl[f].firstblock = blk; } }
    return;
  }
  BS_FOR(s, &R->nd[recv].pts) {
    int f;
    if (site_is_ext(s) || bs_has(&R->esc, s)) {
      g_cur_why = "stored into an object that reached unknown code";
      escape_node(val);
      if (site_is_ext(s)) continue;
    }
    f = fl_find(s, name, 1);
    nd_flow(R->fl[f].node, val);
    if (implicit_callee_name(g_bn_names[name]) && getenv("QJS_AOT_BUNDLE_NOIMPLICIT") == NULL && !g_nolost && strcmp(g_bn_names[name], "constructor") != 0 && bs_count(&R->nd[val].pts) > 0) {
      /* only the FUNCTIONS stored there can be called (a `next` field that holds a list node is data) */
      BSet fs_; int q_; memset(&fs_, 0, sizeof(fs_));
      BS_FOR(q_, &R->nd[val].pts) if (R->st[q_].kind == SK_FUNC) bs_add(&fs_, q_);
      g_cur_why = "stored under a name the engine calls implicitly";
      esc_add(&fs_);
    }
    if (strcmp(g_bn_names[name], "prototype") == 0 && R->st[s].kind == SK_FUNC && bs_count(&R->nd[recv].pts) == 1 &&
        !R->st[s].proto_assigned) {
      R->st[s].proto_assigned = 1; /* assumed to run before the first construct; see the report */
      R->changed = 1;
    }
    R->fl[f].prog = 1;
    if (R->collect) {
      R->fl[f].nwrites++;
      if (R->fl[f].firstfn < 0) {
        R->fl[f].firstfn = fn; R->fl[f].firstpc = pc;
        R->fl[f].firstblock = g_bfn[fn].uncond != NULL && blk >= 0 && g_bfn[fn].uncond[blk] ? 0 : 1;
      }
      ev_add(g_cur_inst, pc, blk, s, name, -1, NULL);
    }
  }
}

static void acc_add(int fn, int pc, int kind, int name, int node) {
  int i;
  const BSet *recv = &R->nd[node].pts;
  for (i = 0; i < R->nacc; i++)
    if (R->acc[i].fn == fn && R->acc[i].pc == pc && R->acc[i].kind == kind && R->acc[i].name == name) {
      bs_or(&R->acc[i].recv, recv);
      R->acc[i].cls |= R->nd[node].cls;
      return;
    }
  if (R->nacc >= R->acccap) {
    R->acccap = R->acccap ? R->acccap * 2 : 256;
    R->acc = realloc(R->acc, (size_t)R->acccap * sizeof(BAcc));
  }
  memset(&R->acc[R->nacc], 0, sizeof(BAcc));
  R->acc[R->nacc].fn = fn; R->acc[R->nacc].pc = pc; R->acc[R->nacc].kind = kind; R->acc[R->nacc].name = name;
  R->acc[R->nacc].recv = *recv;
  R->acc[R->nacc].cls = R->nd[node].cls;
  R->nacc++;
}

static void rcv_add(int fn, int pc, int node, int nodisp) {
  int i;
  for (i = 0; i < R->nrcv; i++)
    if (R->rcv[i].fn == fn && R->rcv[i].pc == pc) break;
  if (i == R->nrcv) {
    if (R->nrcv >= R->rcvcap) {
      R->rcvcap = R->rcvcap ? R->rcvcap * 2 : 128;
      R->rcv = realloc(R->rcv, (size_t)R->rcvcap * sizeof(BRcv));
    }
    memset(&R->rcv[i], 0, sizeof(BRcv));
    R->rcv[i].fn = fn; R->rcv[i].pc = pc;
    R->nrcv++;
  }
  if (node >= 0) { bs_or(&R->rcv[i].recv, &R->nd[node].pts); R->rcv[i].cls |= R->nd[node].cls; }
  if (nodisp) R->rcv[i].nodisp = 1;
}
static void rt_add(int fn, int pc, int site, int tfn) {
  int i;
  for (i = 0; i < R->nrt; i++)
    if (R->rt[i].fn == fn && R->rt[i].pc == pc && R->rt[i].site == site && R->rt[i].tfn == tfn) return;
  if (R->nrt >= R->rtcap) {
    R->rtcap = R->rtcap ? R->rtcap * 2 : 128;
    R->rt = realloc(R->rt, (size_t)R->rtcap * sizeof(BRT));
  }
  R->rt[R->nrt].fn = fn; R->rt[R->nrt].pc = pc; R->rt[R->nrt].site = site; R->rt[R->nrt].tfn = tfn;
  R->nrt++;
}

static void ev_add(int inst, int pc, int blk, int site, int name, int callee, const BSet *this_set) {
  int i;
  for (i = 0; i < R->nev; i++)
    if (R->ev[i].inst == inst && R->ev[i].pc == pc && R->ev[i].site == site && R->ev[i].name == name && R->ev[i].callee == callee) {
      if (this_set) bs_or(&R->ev[i].this_set, this_set);
      return;
    }
  if (R->nev >= R->evcap) {
    R->evcap = R->evcap ? R->evcap * 2 : 1024;
    R->ev = realloc(R->ev, (size_t)R->evcap * sizeof(BEv));
  }
  memset(&R->ev[R->nev], 0, sizeof(BEv));
  R->ev[R->nev].seq = R->nev;
  R->ev[R->nev].inst = inst; R->ev[R->nev].pc = pc; R->ev[R->nev].blk = blk;
  R->ev[R->nev].site = site; R->ev[R->nev].name = name; R->ev[R->nev].callee = callee;
  if (this_set) R->ev[R->nev].this_set = *this_set;
  R->nev++;
}

/* what a value may be, for arithmetic results */
static unsigned arith_cls(int sub, unsigned x, unsigned y) {
  int ints;
  if (x == 0 || y == 0) return 0;
  if (sub == REG_BIN_ADD && ((x | y) & K_STR)) return K_STR;
  if (((x | y) & ~K_NUMERIC) != 0) return sub == REG_BIN_ADD ? K_STR | K_NUM | K_UNK : K_NUM | K_UNK;
  ints = (x & ~(K_INT | K_INTOV)) == 0 && (y & ~(K_INT | K_INTOV)) == 0;
  switch (sub) {
    case REG_BIN_ADD: case REG_BIN_SUB: case REG_BIN_MUL: case REG_BIN_MOD:
      return ints ? K_INTOV : K_NUM;
    case REG_BIN_AND: case REG_BIN_OR: case REG_BIN_XOR: case REG_BIN_SHL: case REG_BIN_SAR:
      return K_INTOV;
    case REG_BIN_SHR:
      return K_INTOV;
    default:
      return K_NUM;
  }
}

static int cls_of_const(const Q2Value *v) {
  switch (v->con_kind) {
    case Q2_CON_INT: return K_INT;
    case Q2_CON_DOUBLE: return K_DBL;
    case Q2_CON_TRUE: case Q2_CON_FALSE: return K_BOOL;
    case Q2_CON_NULL: return K_NULL;
    case Q2_CON_UNDEF: return K_UNDEF;
  }
  return K_UNK;
}

/* callee resolution */
static int fn_of_closure(const JSAotGenFunc *g, int cpool) {
  static const uint8_t zero[16];
  int i;
  if (g->cpool_fn == NULL || cpool < 0 || cpool >= g->cpool_count) return -1;
  if (memcmp(g->cpool_fn[cpool], zero, 16) == 0) return -1;
  for (i = 0; i < g_bnfuncs; i++)
    if (g_bfn[i].ok && memcmp(g_bfn[i].g->digest, g->cpool_fn[cpool], 16) == 0) return i;
  return -1;
}

static void note_edge(int cfn, int cpc, int tfn, int inloop) {
  int i;
  for (i = 0; i < R->ned; i++)
    if (R->ed[i].cfn == cfn && R->ed[i].cpc == cpc && R->ed[i].tfn == tfn) return;
  if (R->ned >= R->edcap) {
    R->edcap = R->edcap ? R->edcap * 2 : 256;
    R->ed = realloc(R->ed, (size_t)R->edcap * sizeof(BEdge));
  }
  R->ed[R->ned].cfn = cfn; R->ed[R->ned].cpc = cpc; R->ed[R->ned].tfn = tfn; R->ed[R->ned].inloop = inloop;
  R->ned++;
}

static BCall *note_call(int fn, int pc, int rop, int inloop, const char *callee) {
  int i;
  for (i = 0; i < R->ncs; i++)
    if (R->cs[i].fn == fn && R->cs[i].pc == pc) return &R->cs[i];
  if (R->ncs >= R->cscap) {
    R->cscap = R->cscap ? R->cscap * 2 : 256;
    R->cs = realloc(R->cs, (size_t)R->cscap * sizeof(BCall));
  }
  memset(&R->cs[R->ncs], 0, sizeof(BCall));
  R->cs[R->ncs].fn = fn; R->cs[R->ncs].pc = pc; R->cs[R->ncs].rop = rop; R->cs[R->ncs].inloop = inloop;
  snprintf(R->cs[R->ncs].callee, sizeof(R->cs[R->ncs].callee), "%s", callee);
  return &R->cs[R->ncs++];
}
static void call_add_target(BCall *c, int fn) {
  int i;
  for (i = 0; i < c->nt; i++) if (c->targets[i] == fn) return;
  if (c->nt < 8) c->targets[c->nt++] = fn; else c->nt = 9;
}

static void mark_flags(int node, int f) {
  int s;
  if (!R->collect || node < 0) return;
  BS_FOR(s, &R->nd[node].pts) R->st[s].flags |= f;
}

/* an unknown call: arguments reach unknown code */
static void unknown_call(const BInst *I, const Q2Insn *in, int dst, int recv_node, int is_obj_arg_check) {
  int k, any = 0;
  (void)is_obj_arg_check;
  for (k = 0; k < in->win_n && in->win_val != NULL; k++) {
    int n = vnode(I, in->win_val[k]);
    g_cur_why = "argument of an unknown call";
    esc_add(&R->nd[n].pts);
    if (bs_count(&R->nd[n].pts) > 0) any = 1;
    mark_flags(n, SF_UNKARG);
  }
  if (recv_node >= 0) { escape_node(recv_node); mark_flags(recv_node, SF_UNKARG); if (bs_count(&R->nd[recv_node].pts) > 0) any = 1; }
  if (dst >= 0) { nd_cls(dst, K_UNK); nd_site(dst, 0); }
  (void)any;
}

static int hash_sub(const char *a) { return bn_name(a); }

/* the instruction that defines a value, looking through moves */
/* The value v is a copy of (through moves). */
static Q2ValueId bn_base(const BInst *I, Q2ValueId v) {
  const Q2Func *q = &g_bfn[I->fn].q;
  int hop;
  for (hop = 0; hop < 8; hop++) {
    const Q2Insn *d;
    if ((int32_t)v < 0 || (int32_t)v >= q->nvalues || q->values[v].insn < 0 || q->values[v].insn >= q->ninsns) break;
    d = &q->insns[q->values[v].insn];
    if (d->op == Q2_OP_MOVE && d->nuse == 1) { v = d->use[0]; continue; }
    break;
  }
  return v;
}
static const Q2Insn *bn_def(const BInst *I, Q2ValueId v) {
  const Q2Func *q = &g_bfn[I->fn].q;
  int hop;
  for (hop = 0; hop < 8; hop++) {
    const Q2Insn *d;
    if ((int32_t)v < 0 || (int32_t)v >= q->nvalues || q->values[v].insn < 0 || q->values[v].insn >= q->ninsns) return NULL;
    d = &q->insns[q->values[v].insn];
    if (d->op == Q2_OP_MOVE && d->nuse == 1) { v = d->use[0]; continue; }
    return d;
  }
  return NULL;
}

/* the string a value is, when it is a constant atom */
static int bn_const_name(const BInst *I, Q2ValueId v, char *out, size_t cap) {
  const Q2Insn *d = bn_def(I, v);
  size_t len;
  const char *nm;
  if (d == NULL || d->rop != ROP_load_const || d->narg < 3 || d->arg[1].val != REG_CONST_ATOM) return 0;
  nm = JS_AtomToCStringLen(g_bctx, &len, (JSAtom)d->arg[2].val);
  if (nm == NULL) return 0;
  snprintf(out, cap, "%s", nm);
  JS_FreeCString(g_bctx, nm);
  return 1;
}

/* Object.defineProperty(target, "key", desc): the key's slot takes the descriptor's `value` (a getter makes the slot unknown) */
static void builtin_define_property(const BInst *I, const Q2Insn *in, int dst) {
  char key[64];
  int tgt, desc, s, d;
  if (in->win_n < 3 || in->win_val == NULL) { unknown_call(I, in, dst, -1, 0); return; }
  tgt = vnode(I, in->win_val[0]);
  desc = vnode(I, in->win_val[2]);
  if (!bn_const_name(I, in->win_val[1], key, sizeof(key))) {
    unknown_call(I, in, dst, -1, 0);
    if (R->collect) R->dyn_key_stores++;
    return;
  }
  BS_FOR(s, &R->nd[tgt].pts) {
    int fnn;
    if (site_is_ext(s)) continue;
    fnn = fnode(s, bn_name(key), 1);
    { int f0 = fl_find(R->pts ? s : -1, bn_name(key), 0); if (f0 >= 0) R->fl[f0].prog = 1; }
    if (R->collect) {
      int f = fl_find(R->pts ? s : -1, bn_name(key), 0);
      if (f >= 0) R->fl[f].nwrites++;
    }
    BS_FOR(d, &R->nd[desc].pts) {
      int fv = fl_find(R->pts ? d : -1, bn_name("value"), 0);
      if (fv >= 0) nd_flow(fnn, R->fl[fv].node);
      if (fl_find(R->pts ? d : -1, bn_name("get"), 0) >= 0 || fl_find(R->pts ? d : -1, bn_name("set"), 0) >= 0) {
        nd_cls(fnn, K_UNK); nd_site(fnn, 0);
        if (R->collect) R->accessors++;
      }
    }
    if (R->collect) { if (R->pts) { /* the definition belongs to the target */ } }
  }
  if (dst >= 0) nd_flow(dst, tgt);
}

/* the call of function site `s` from `in`: `shift` window registers are not arguments (X.call: the first is `this`) */
static int g_diag_noforcopy;
static int g_math_model = 1;
static int g_diag_nocoerce;
static int g_skip_elemchk;   /* DIAG_NOCOERCEELEM (a vkit knock-out): coerce_arg ignores computed-store content on the chain */
static int g_this_site = -1; /* method dispatch: the one receiver this target is called on */

static void math_builtin_call(const BInst *I, const Q2Insn *in, int dst, const char *nm);
static void link_target(const BInst *I, const Q2Insn *in, int s, int kind, int recv_node, int shift, int dst, int inloop, BCall *cs) {
  BSite *S = &R->st[s];
  int ti = inst_get(S->fn, small_fn(S->fn) ? (I->fn * 65536 + in->pc + 1) : 0);
  const BInst *T = &R->in[ti];
  int k;
  if (!R->in[ti].live) { R->in[ti].live = 1; R->changed = 1; }
  if (R->collect) { note_edge(I->fn, in->pc, S->fn, inloop); if (cs) call_add_target(cs, S->fn); }
  if (kind == 2) {
    int inst = st_new(SK_INST, S->fn, I->fn, in->pc, I->ctx, "");
    if (R->st[inst].ctor_inst < 0) R->st[inst].ctor_inst = ti;
    if (R->collect) { BSet one; memset(&one, 0, sizeof(one)); bs_add(&one, inst); ev_add(g_cur_inst, in->pc, -1, -1, -1, ti, &one); }
    if (R->st[inst].label[0] == 0) snprintf(R->st[inst].label, sizeof(R->st[inst].label), "new %s@%d", fn_name(S->fn), in->pc);
    nd_site(T->thisn, inst);
    nd_site(dst, inst);
    {
      int f = fl_find(R->pts ? s : -1, bn_name("prototype"), 0);
      if (f >= 0 && R->pts) {
        if (bs_or(&R->st[inst].proto, &R->nd[R->fl[f].node].pts)) R->changed = 1;
      }
      if (R->pts && !S->proto_assigned) {
        int pp;
        for (pp = 0; pp < R->nst; pp++)
          if (R->st[pp].kind == SK_PROTO && R->st[pp].owner == s)
            if (bs_add(&R->st[inst].proto, pp)) R->changed = 1;
      }
    }
  } else if (shift > 0 && in->win_n >= 1 && in->win_val != NULL) {
    if (R->collect) ev_add(g_cur_inst, in->pc, -1, -1, -1, ti, &R->nd[vnode(I, in->win_val[0])].pts);
    nd_flow(T->thisn, vnode(I, in->win_val[0]));
    mark_flags(vnode(I, in->win_val[0]), SF_THIS);
  } else if (g_this_site >= 0) {
    if (R->collect) { BSet one; memset(&one, 0, sizeof(one)); bs_add(&one, g_this_site); ev_add(g_cur_inst, in->pc, -1, -1, -1, ti, &one); }
    nd_site(T->thisn, g_this_site);
    R->st[g_this_site].flags |= SF_THIS;
  } else if (recv_node >= 0) {
    if (R->collect) ev_add(g_cur_inst, in->pc, -1, -1, -1, ti, &R->nd[recv_node].pts);
    nd_flow(T->thisn, recv_node);
    mark_flags(recv_node, SF_THIS);
  } else {
    nd_cls(T->thisn, K_UNDEF);
  }
  for (k = 0; k < 16; k++) {
    if (k + shift < in->win_n && in->win_val != NULL) {
      nd_flow(T->parn[k < T->npar ? k : 16], vnode(I, in->win_val[k + shift]));
      if (k >= T->npar) nd_flow(T->extra[k], vnode(I, in->win_val[k + shift]));
      mark_flags(vnode(I, in->win_val[k + shift]), SF_PASSED);
    } else if (k < T->npar) {
      nd_cls(T->parn[k], K_UNDEF);
    }
  }
  for (k = 16 + shift; k < in->win_n && in->win_val != NULL; k++) nd_flow(T->parn[16], vnode(I, in->win_val[k]));
  if (dst >= 0 && kind != 2) nd_flow(dst, T->retn);
  else if (dst >= 0 && bs_count(&R->nd[T->retn].pts)) nd_flow(dst, T->retn); /* a constructor that returns an object */
}

/* The result class of a call to a KNOWN NATIVE: the profile saw exactly one callee at the site, a C function of the engine's builtin table whose result is a primitive (never an object), and
   the analysis' callee set is only EXT (nothing the bundle defines can be the callee). Same site identity as e_intrinsic_site (the profile's callee key "symbol:magic"). 0 when it is not one.
   QJS_AOT_BUNDLE_DIAG_NONATIVETYPE turns it off. */
static int g_nonativetype = -1;
static unsigned native_result_cls(const BInst *I, const Q2Insn *in, int kind, const BSet *callee_set) {
  static const struct { const char *key; unsigned cls; } tab[] = {
    {"js_math_floor:0", K_NUM}, {"js_math_sqrt:0", K_NUM}, {"js_math_pow:0", K_NUM}, {"js_math_log:0", K_NUM},
    {"js_math_min_max:0", K_NUM}, {"js_math_min_max:1", K_NUM},
    {"js_array_push:0", K_INT}, {"js_array_indexOf:0", K_INT}, {"js_number_toFixed:0", K_STR}, {NULL, 0}};
  char hx[4][33];
  int kd[4], nn = 0, k, s;
  unsigned long long ht[4];
  if (g_nonativetype < 0) g_nonativetype = getenv("QJS_AOT_BUNDLE_NATIVETYPE") == NULL || getenv("QJS_AOT_BUNDLE_DIAG_NONATIVETYPE") != NULL;   /* the profile-keyed table is an unproven claim: opt in only */
  if (g_nonativetype || kind == 2 || !g_bfn[I->fn].g) return 0;
  BS_FOR(s, callee_set) if (!site_is_ext(s)) return 0;
  if (!qjs_aot_prof_callees(g_bfn[I->fn].g->digest, in->next, &nn, hx, kd, ht) || nn != 1 || kd[0] != 2) return 0;
  for (k = 0; tab[k].key; k++) if (strcmp(tab[k].key, hx[0]) == 0) return tab[k].cls;
  return 0;
}

/* one call. `kind`: 0 call, 1 call_method, 2 construct */
static void do_call(const BInst *I, const Q2Insn *in, int kind, int inloop) {
  int callee_node = in->nuse >= 1 ? vnode(I, in->use[0]) : R->sink_node;
  int recv_node = kind == 1 && in->nuse >= 2 ? vnode(I, in->use[1]) : -1;
  int dst = in->ndef >= 1 ? vnode(I, in->def[0]) : -1;
  int s, nknown = 0, unknown = 0, builtins = 0, disp = 0;
  BCall *cs = NULL;
  char cname[40] = "?";
  BSet callee_set;
  if (kind == 1) seal_recv(recv_node, I->fn, in->pc);
  callee_set = R->nd[callee_node].pts;
  {
    /* the name the callee was read under, for the report */
    int vi = in->nuse >= 1 && (int32_t)in->use[0] >= 0 && in->use[0] < (uint32_t)g_bfn[I->fn].q.nvalues ? g_bfn[I->fn].q.values[in->use[0]].insn : -1;
    int hop = 0;
    while (vi >= 0 && vi < g_bfn[I->fn].q.ninsns && hop++ < 6) {
      const Q2Insn *d = &g_bfn[I->fn].q.insns[vi];
      if (d->op == Q2_OP_MOVE && d->nuse == 1 && (int32_t)d->use[0] >= 0) { vi = g_bfn[I->fn].q.values[d->use[0]].insn; continue; }
      if (d->rop == ROP_get_global || d->rop == ROP_get_field || d->rop == ROP_get_field2 || d->rop == ROP_get_field_own ||
          d->rop == ROP_get_field2_own || d->rop == ROP_get_this_field || d->rop == ROP_get_this_field2 ||
          d->rop == ROP_get_local_field || d->rop == ROP_get_local_field_0 || d->rop == ROP_get_local_field_1 ||
          d->rop == ROP_get_local_field_2 || d->rop == ROP_get_local_field_3) {
        char nm[36];
        if (bn_atom(d, NULL, nm, sizeof(nm), 0)) snprintf(cname, sizeof(cname), "%s%s", d->rop == ROP_get_global ? "::" : ".", nm);
      }
      break;
    }
  }
  if (R->collect) cs = note_call(I->fn, in->pc, in->rop, inloop, cname);
  if (kind != 1 && in->nuse >= 1) {
    /* `new Array(...)` and `Array(...)`: an array of this site */
    const Q2Insn *cd = bn_def(I, in->use[0]);
    char gn[32];
    if (cd != NULL && cd->rop == ROP_get_global && bn_atom(cd, NULL, gn, sizeof(gn), 0) && strcmp(gn, "Array") == 0 && !g_gdef[bn_name("Array")]) {
      int ns = st_new(SK_ARR, -4, I->fn, in->pc, I->ctx, "");
      int k;
      if (R->st[ns].label[0] == 0) snprintf(R->st[ns].label, sizeof(R->st[ns].label), "new Array@%s:%d", fn_name(I->fn), in->pc);
      nd_site(dst, ns);
      for (k = 0; in->win_n > 1 && k < in->win_n && in->win_val != NULL; k++) nd_flow(fnode(ns, bn_name("[]"), 1), vnode(I, in->win_val[k]));
      if (cs) cs->builtin = 1;
      if (R->collect) g_bfn[I->fn].alloc++;
      return;
    }
  }
  if (R->nd[callee_node].cls & K_UNK) unknown = 1;
  if (R->collect && kind == 1 && recv_node >= 0) rcv_add(I->fn, in->pc, recv_node, 0);
  /* A method call whose callee was read from its own receiver dispatches per receiver object: each target is called on the
     objects whose prototype chain finds it, not on every receiver of the call site. */
  if (R->pts && kind == 1 && recv_node >= 0 && in->nuse >= 2) {
    const Q2Insn *cd = bn_def(I, in->use[0]);
    char mn[64];
    int rs, same = 0;
    if (cd != NULL && (cd->rop == ROP_get_field || cd->rop == ROP_get_field2 || cd->rop == ROP_get_field_own || cd->rop == ROP_get_field2_own) &&
        cd->nuse >= 1 && vnode(I, cd->use[0]) == recv_node) same = 1;
    if (cd != NULL && (cd->rop == ROP_get_this_field || cd->rop == ROP_get_this_field2)) same = 1;
    /* `a.b.m()` fused as a chain read: the callee is the chain's second read and its receiver the first read's result */
    if (cd != NULL && (cd->rop == ROP_get_field_chain || cd->rop == ROP_get_field_chain_own) && cd->ndef >= 2 &&
        bn_base(I, in->use[0]) == cd->def[1] && vnode(I, cd->def[0]) == recv_node) same = 2;
    if (same && bn_atom(cd, NULL, mn, sizeof(mn), same == 2 ? 1 : 0)) {
      int mid = bn_name(mn);
      BSet rset = R->nd[recv_node].pts;
      disp = 1;
      BS_FOR(rs, &rset) {
        int save = R->changed, ts;
        BSet found;
        if (site_is_ext(rs) || bs_has(&R->esc, rs)) continue;
        memset(&R->nd[R->tmp_in], 0, sizeof(BNode));
        memset(&R->nd[R->tmp_out], 0, sizeof(BNode));
        bs_add(&R->nd[R->tmp_in].pts, rs);
        R->nd[R->tmp_in].cls = K_OBJ;
        read_field(R->tmp_out, R->tmp_in, mid, I->fn, in->pc);
        found = R->nd[R->tmp_out].pts;
        R->changed = save;
        BS_FOR(ts, &found)
          if (R->st[ts].kind == SK_FUNC && R->st[ts].fn >= 0 && g_bfn[R->st[ts].fn].ok) {
            g_this_site = rs;
            if (R->collect) { rcv_add(I->fn, in->pc, -1, 0); rt_add(I->fn, in->pc, rs, R->st[ts].fn); }
            nknown++;
            link_target(I, in, ts, kind, recv_node, 0, dst, inloop, cs);
            g_this_site = -1;
          }
      }
    }
  }
  BS_FOR(s, &callee_set) {
    BSite *S = &R->st[s];
    if (S->kind == SK_FUNC && S->fn >= 0 && g_bfn[S->fn].ok) {
      if (disp) continue;
      if (R->collect && kind == 1 && recv_node >= 0) rcv_add(I->fn, in->pc, -1, 1);
      nknown++;
      link_target(I, in, s, kind, recv_node, 0, dst, inloop, cs);
    } else if (S->kind == SK_BUILTIN) {
      const char *bn = g_bn_names[S->fn];
      int rs;
      builtins++;
      if (cs) cs->builtin = 1;
      if (strncmp(R->st[s].label, "Math.", 5) == 0 && g_math_model) {
        math_builtin_call(I, in, dst, bn);
      } else if (strcmp(bn, "push") == 0 || strcmp(bn, "unshift") == 0) {
        BS_FOR(rs, &R->nd[recv_node >= 0 ? recv_node : R->sink_node].pts) {
          int k;
          for (k = 0; k < in->win_n && in->win_val != NULL; k++) {
            int en = fnode(rs, bn_name("[]"), 1);
            nd_flow(en, vnode(I, in->win_val[k]));
            mark_flags(vnode(I, in->win_val[k]), SF_STORED);
          }
          if (R->collect) R->st[rs].flags |= 0;
        }
        nd_cls(dst, K_INT);
      } else if (strcmp(bn, "pop") == 0 || strcmp(bn, "shift") == 0) {
        BS_FOR(rs, &R->nd[recv_node >= 0 ? recv_node : R->sink_node].pts) nd_flow(dst, fnode(rs, bn_name("[]"), 1));
        nd_cls(dst, K_UNDEF);
      } else if (strcmp(bn, "slice") == 0 || strcmp(bn, "concat") == 0 || strcmp(bn, "splice") == 0) {
        int ns = st_new(SK_ARR, -1, I->fn, in->pc, I->ctx, "");
        if (R->st[ns].label[0] == 0) snprintf(R->st[ns].label, sizeof(R->st[ns].label), "[%s]@%s:%d", bn, fn_name(I->fn), in->pc);
        nd_site(dst, ns);
        BS_FOR(rs, &R->nd[recv_node >= 0 ? recv_node : R->sink_node].pts) nd_flow(fnode(ns, bn_name("[]"), 1), fnode(rs, bn_name("[]"), 1));
      } else if (strcmp(bn, "indexOf") == 0 || strcmp(bn, "lastIndexOf") == 0) {
        nd_cls(dst, K_INT);
      } else if (strcmp(bn, "join") == 0) {
        nd_cls(dst, K_STR);
      } else if (strcmp(bn, "call") == 0) {
        int ts, nt = 0;
        BS_FOR(ts, &R->nd[recv_node >= 0 ? recv_node : R->sink_node].pts) {
          if (R->st[ts].kind == SK_FUNC && R->st[ts].fn >= 0 && g_bfn[R->st[ts].fn].ok) { nt++; link_target(I, in, ts, 1, -1, 1, dst, inloop, cs); }
          else unknown = 1;
        }
        if (nt == 0) unknown = 1;
        if (cs) cs->thru = 1;
        if (R->collect) R->thru_calls++;
        builtins--; nknown++;
      } else if (strcmp(bn, "defineProperty") == 0) {
        builtin_define_property(I, in, dst);
      } else if (strcmp(bn, "create") == 0) {
        int ns = st_new(SK_OBJ, -3, I->fn, in->pc, I->ctx, "");
        if (R->st[ns].label[0] == 0) snprintf(R->st[ns].label, sizeof(R->st[ns].label), "Object.create@%s:%d", fn_name(I->fn), in->pc);
        nd_site(dst, ns);
        if (in->win_n >= 1 && in->win_val != NULL && bs_or(&R->st[ns].proto, &R->nd[vnode(I, in->win_val[0])].pts)) R->changed = 1;
      } else {
        unknown = 1;
        unknown_call(I, in, dst, recv_node, 0);
      }
    } else {
      unknown = 1;
      if (S->kind != SK_EXT && S->kind != SK_NATIVE && S->kind != SK_FUNC) {}
    }
  }
  if (R->phase == 1 && nknown == 0 && builtins == 0 && !unknown && R->pts && g_dbg_events < g_dbg_max && getenv("QJS_AOT_BUNDLE_EVENTS") != NULL) {
    g_dbg_events++;
    {
      char rl[300] = "-";
      if (recv_node >= 0) set_str_fwd(R, &R->nd[recv_node].pts, rl, sizeof(rl), 6);
      fprintf(stderr, "bundle: EVENT empty callee at %s:%d %s receivers [%s] disp=%d nknown=%d\n", fn_name(I->fn), in->pc, cname, rl, disp, nknown);
    }
  }
  if (unknown || (R->phase == 1 && nknown == 0 && builtins == 0)) {
    if (kind == 2) {
      int ns = st_new(SK_NATIVE, -1, I->fn, in->pc, I->ctx, "");
      if (R->st[ns].label[0] == 0) snprintf(R->st[ns].label, sizeof(R->st[ns].label), "native-new@%s:%d", fn_name(I->fn), in->pc);
      nd_site(dst, ns);
    }
    {
      unsigned ncls = unknown && builtins == 0 && nknown == 0 && dst >= 0 ? native_result_cls(I, in, kind, &callee_set) : 0;
      unknown_call(I, in, kind == 2 ? -1 : ncls ? -1 : dst, recv_node, 0);   /* the arguments still escape: only the result is typed */
      if (ncls) { nd_cls(dst, ncls); if (R->collect) R->native_typed++; }
    }
    if (R->collect && R->pts && getenv("QJS_AOT_BUNDLE_DEBUG") != NULL && strstr(fn_name(I->fn), getenv("QJS_AOT_BUNDLE_DEBUG")) != NULL) {
      char cl[128], sl[400];
      cls_str_fwd(R->nd[callee_node].cls, cl, sizeof(cl));
      set_str_fwd(R, &callee_set, sl, sizeof(sl), 6);
      { char rl_[300] = "-", rn_[40] = "-", cn_[40] = "-"; { const Q2Func *q_ = &g_bfn[I->fn].q; int vv_ = in->nuse >= 2 ? (int)in->use[1] : -1, cv_ = in->nuse >= 1 ? (int)in->use[0] : -1; if (vv_ >= 0 && vv_ < q_->nvalues && q_->values[vv_].insn >= 0) e_rop_name((uint16_t)q_->insns[q_->values[vv_].insn].rop, rn_, sizeof(rn_)); if (cv_ >= 0 && cv_ < q_->nvalues && q_->values[cv_].insn >= 0) e_rop_name((uint16_t)q_->insns[q_->values[cv_].insn].rop, cn_, sizeof(cn_)); } if (recv_node >= 0) set_str_fwd(R, &R->nd[recv_node].pts, rl_, sizeof(rl_), 6); fprintf(stderr, "bundle: recv defined by %s, callee defined by %s\n", rn_, cn_); { int ka_; for (ka_ = 0; ka_ < in->win_n && in->win_val != NULL && ka_ < 4; ka_++) { char al_[300] = "-"; set_str_fwd(R, &R->nd[vnode(I, in->win_val[ka_])].pts, al_, sizeof(al_), 6); fprintf(stderr, "bundle:   arg%d cls=%u [%s]\n", ka_, R->nd[vnode(I, in->win_val[ka_])].cls, al_); } } fprintf(stderr, "bundle: unknown call %s:%d %s callee cls=%s sites=[%s] recv=[%s] known=%d builtins=%d unknown=%d\n", fn_name(I->fn), in->pc, cname, cl, sl, rl_, nknown, builtins, unknown); }
    }
    if (R->collect) {
      R->unk_calls++;
      if (cs) cs->unknown = 1;
      g_bfn[I->fn].unkcall = 1;
    }
  }
  (void)hash_sub;
}


/* `f.apply(x, arguments)` with the current function's own arguments (apply_arguments; window = f, the `apply` method read, x): a call of f's targets with this = x and
   the caller's own parameters as the arguments (the arguments array holds exactly what this function was called with). Only this exact forward gets the precise model, and only when
   the method read is the builtin Function.prototype.apply (a user `apply` field would be called instead). Returns 0 when it is not (yet) a resolved forward, which leaves the instruction to
   the generic unknown-operand handling in phase 1; in phase 0 an incomplete set simply waits for the next round. */
/* The objects a coercion or a for-in can reach from `start` through prototype links, all of them plain and fully known: returns 0 when one is the external site, escaped, not a plain
   object / instance / prototype, or has computed-store content (its names are then not known). Builtin prototypes are skipped (nothing the program stored). */
static int plain_chain(const BSet *start, BSet *seen) {
  int stack[64], sp = 0, s, t, i;
  BS_FOR(s, start) { if (sp < 64) stack[sp++] = s; else return 0; }
  while (sp > 0) {
    int e;
    t = stack[--sp];
    if (bs_has(seen, t)) continue;
    bs_add(seen, t);
    if (site_is_ext(t) || bs_has(&R->esc, t)) return 0;
    if (R->st[t].kind == SK_BUILTIN) continue;
    if (R->st[t].kind != SK_OBJ && R->st[t].kind != SK_PROTO && R->st[t].kind != SK_INST) return 0;
    e = fl_find(t, bn_name("[]"), 0);
    if (!g_skip_elemchk && e >= 0 && (R->nd[R->fl[e].node].cls != 0 || bs_count(&R->nd[R->fl[e].node].pts) > 0)) return 0;
    BS_FOR(i, &R->st[t].proto) { if (sp < 64) stack[sp++] = i; else return 0; }
  }
  return 1;
}

/* An implicit call of the user function site `ts` (valueOf / toString run by a coercion): this = the object `thissite`, no arguments, the result is dropped. */
static void implicit_call(int ts, int thissite) {
  const BSite *S = &R->st[ts];
  int ti, k;
  if (S->kind != SK_FUNC || S->fn < 0 || !g_bfn[S->fn].ok) return;
  ti = inst_get(S->fn, 0);
  if (!R->in[ti].live) { R->in[ti].live = 1; R->changed = 1; }
  nd_site(R->in[ti].thisn, thissite);
  R->st[thissite].flags |= SF_THIS;
  for (k = 0; k < R->in[ti].npar; k++) nd_cls(R->in[ti].parn[k], K_UNDEF);
  /* a result that can be an object goes back into the coercion, which then tries the next method on it: the objects it can hold escape (conservative) */
  if ((R->nd[R->in[ti].retn].cls & (K_OBJ | K_UNK)) || bs_count(&R->nd[R->in[ti].retn].pts) > 0) { g_cur_why = "an object returned by a valueOf / toString a coercion runs"; escape_node(R->in[ti].retn); }
}

/* ToPrimitive / ToNumber on the argument node `an` of a modelled builtin: for each object, the valueOf and toString the chain defines (a name no chain object stores is the engine's own: nothing leaks,
   nothing to model) are run with this = that object. An argument the model cannot resolve escapes, as an unknown call's would. */
static void coerce_arg(int an) {
  BSet seen;
  if (g_diag_nocoerce) return;   /* DIAG_NOCOERCE (a vkit knock-out): the argument is not coerced at all */
  int s, ok = 1, i, nm;
  static const char *const names[] = {"valueOf", "toString"};
  if (getenv("QJS_AOT_BUNDLE_COERCEDBG") != NULL) { char sl_[200] = "-"; set_str_fwd(R, &R->nd[an].pts, sl_, sizeof(sl_), 6); fprintf(stderr, "coerce: arg cls=%u [%s] phase %d\n", R->nd[an].cls, sl_, R->phase); }
  if (R->nd[an].cls & K_UNK) { escape_node(an); return; }
  memset(&seen, 0, sizeof(seen));
  BS_FOR(s, &R->nd[an].pts) if (site_is_ext(s) || bs_has(&R->esc, s) || (R->st[s].kind != SK_OBJ && R->st[s].kind != SK_PROTO && R->st[s].kind != SK_INST)) { ok = 0; if (getenv("QJS_AOT_BUNDLE_COERCEDBG") != NULL) fprintf(stderr, "coerce: site %s ext=%d esc=%d kind=%d\n", R->st[s].label, site_is_ext(s), bs_has(&R->esc, s), R->st[s].kind); }
  g_skip_elemchk = getenv("QJS_AOT_BUNDLE_DIAG_NOCOERCEELEM") != NULL;
  if (ok && !plain_chain(&R->nd[an].pts, &seen)) ok = 0;
  g_skip_elemchk = 0;
  if (getenv("QJS_AOT_BUNDLE_COERCEDBG") != NULL) fprintf(stderr, "coerce: ok=%d\n", ok);
  if (!ok) { escape_node(an); return; }
  BS_FOR(s, &R->nd[an].pts) {
    BSet chain, one;
    memset(&chain, 0, sizeof(chain));
    memset(&one, 0, sizeof(one));
    bs_add(&one, s);
    plain_chain(&one, &chain);   /* this object and its prototypes (the call above proved them plain) */
    for (nm = 0; nm < 2; nm++) {
      int name = bn_name(names[nm]);
      BS_FOR(i, &chain) {
        int e = fl_find(i, name, 0), ts;
        if (e < 0) continue;
        if (R->nd[R->fl[e].node].cls & K_UNK) { escape_node(an); continue; }
        BS_FOR(ts, &R->nd[R->fl[e].node].pts) {
          if (R->st[ts].kind == SK_FUNC && R->st[ts].fn >= 0 && g_bfn[R->st[ts].fn].ok) implicit_call(ts, s);
          else escape_node(an);   /* something other than a user function stored under the name */
        }
      }
    }
  }
}

/* a call of a modelled Math function: a number; the arguments the specification coerces are coerced (max / min / hypot all of them, pow / atan2 / imul two, random none, the rest one) */
static void math_builtin_call(const BInst *I, const Q2Insn *in, int dst, const char *nm) {
  int nco = 1, k;
  if (strcmp(nm, "random") == 0) nco = 0;
  else if (strcmp(nm, "max") == 0 || strcmp(nm, "min") == 0 || strcmp(nm, "hypot") == 0) nco = in->win_n;
  else if (strcmp(nm, "pow") == 0 || strcmp(nm, "atan2") == 0 || strcmp(nm, "imul") == 0) nco = 2;
  g_cur_why = "an argument of a Math function the coercion model cannot resolve";
  for (k = 0; k < nco && k < in->win_n && in->win_val != NULL; k++) coerce_arg(vnode(I, in->win_val[k]));
  if (dst >= 0) nd_cls(dst, strcmp(nm, "random") == 0 ? K_DBL : K_NUM);
}

/* `for (k in src) dst[k] = src[k]` (Prototype.js's Object.extend, every mixin helper): the copy modelled per NAME. The idiom is exact: the key is the for-in key and has no other use than the two
   element operations, the value is `src[k]` read from the very object the loop enumerates and is stored only here. The names are the enumerable named fields of the source objects and of their
   prototype chain (fields the program stores: a field with no write is an engine link such as `constructor`), each copied into the destination under its own name. Anything else falls back to the
   old model (every value into the destination's `[]`): a source or destination that is escaped / EXT / not a plain object, or a source chain with computed stores (its names are not known).
   Returns 1 when the copy was applied. QJS_AOT_BUNDLE_DIAG_NOFORCOPY=1: the old model always. */
static int forin_uses_ok(const BInst *I, Q2ValueId base, int only_put_val) {
  const Q2Func *q = &g_bfn[I->fn].q;
  int j, u, n = 0;
  for (j = 0; j < q->ninsns; j++) {
    const Q2Insn *x = &q->insns[j];
    for (u = 0; u < x->nuse; u++) {
      if ((int32_t)x->use[u] < 0 || bn_base(I, x->use[u]) != (int)base) continue;
      if (x->rop == ROP_move || x->rop == ROP_move_s || x->rop == ROP_move_xfer) continue;
      if (only_put_val) { if ((x->rop == ROP_put_element || x->rop == ROP_put_element_xfer) && u == 2) { n++; continue; } return 0; }
      if (((x->rop == ROP_put_element || x->rop == ROP_put_element_xfer) && u == 1) || (x->rop == ROP_get_element && u == 1)) continue;
      return 0;
    }
    if (x->win_val != NULL) for (u = 0; u < x->win_n; u++) if ((int32_t)x->win_val[u] >= 0 && bn_base(I, x->win_val[u]) == (int)base) return 0;
  }
  return only_put_val ? n == 1 : 1;
}
static int forin_copy(const BInst *I, const Q2Insn *in) {
  const Q2Insn *kd, *vd;
  int w, k, en = -1, dstn, srcn, s, d, t, depth, i, nfl0, applied = 0;
  BSet seen;
  if (g_diag_noforcopy || !R->pts || in->nuse < 3) return 0;
  kd = bn_def(I, in->use[1]); vd = bn_def(I, in->use[2]);
  if (kd == NULL || vd == NULL || kd->rop != ROP_for_in_next || vd->rop != ROP_get_element || vd->nuse < 2) return 0;
  if (bn_base(I, vd->use[1]) != bn_base(I, in->use[1])) return 0;
  if (!forin_uses_ok(I, (Q2ValueId)bn_base(I, in->use[1]), 0) || !forin_uses_ok(I, (Q2ValueId)bn_base(I, in->use[2]), 1)) return 0;
  w = kd->narg > 0 && kd->arg[0].kind == 'r' ? (int)(kd->arg[0].val / 16) : -1;
  for (k = 0; k < kd->nredef; k++) if (kd->redef_reg[k] == w) en = vnode(I, kd->redef_val[k]);
  if (en < 0) return 0;
  srcn = vnode(I, vd->use[0]); dstn = vnode(I, in->use[0]);
  if (R->nd[srcn].cls & (K_UNK | K_STR | K_INT | K_INTOV | K_DBL | K_NUM | K_BOOL)) return 0;
  if (R->nd[dstn].cls & (K_UNK | K_STR | K_INT | K_INTOV | K_DBL | K_NUM | K_BOOL)) return 0;
  /* the sets grow during the fixpoint and only ever grow: an empty one has nothing to copy yet (the copy is applied again when it fills); the names come from the objects the loop enumerates AND
     the objects the value is read from (the same variable, so equal at the fixpoint; their union is a sound view of the rounds before it) */
  if (bs_count(&R->nd[srcn].pts) == 0 && bs_count(&R->nd[en].pts) == 0) return 1;
  if (bs_count(&R->nd[dstn].pts) == 0) return 1;
  BS_FOR(d, &R->nd[dstn].pts) if (site_is_ext(d) || bs_has(&R->esc, d) || (R->st[d].kind != SK_OBJ && R->st[d].kind != SK_PROTO && R->st[d].kind != SK_INST)) return 0;
  /* the sources and every object of their prototype chains: plain, known, no computed stores */
  memset(&seen, 0, sizeof(seen));
  { int stack[64], sp = 0;
    BS_FOR(s, &R->nd[srcn].pts) { if (sp < 64) stack[sp++] = s; else return 0; }
    BS_FOR(s, &R->nd[en].pts) { if (!bs_has(&R->nd[srcn].pts, s)) { if (sp < 64) stack[sp++] = s; else return 0; } }
    while (sp > 0) {
      int e;
      t = stack[--sp];
      if (bs_has(&seen, t)) continue;
      bs_add(&seen, t);
      if (site_is_ext(t) || bs_has(&R->esc, t)) return 0;
      if (R->st[t].kind == SK_BUILTIN) continue;   /* Object.prototype and the like: nothing enumerable the program stores */
      if (R->st[t].kind != SK_OBJ && R->st[t].kind != SK_PROTO && R->st[t].kind != SK_INST) return 0;
      e = fl_find(t, bn_name("[]"), 0);
      if (e >= 0 && (R->nd[R->fl[e].node].cls != 0 || bs_count(&R->nd[R->fl[e].node].pts) > 0)) return 0;
      BS_FOR(i, &R->st[t].proto) { if (sp < 64) stack[sp++] = i; else return 0; }
    }
  }
  nfl0 = R->nfl;
  if (getenv("QJS_AOT_BUNDLE_FORCOPYDBG") != NULL) fprintf(stderr, "forcopy: applied in %s pc %d: %d source sites, %d dest sites, %d chain sites\n", fn_name(I->fn), in->pc, bs_count(&R->nd[srcn].pts), bs_count(&R->nd[dstn].pts), bs_count(&seen));
  for (i = 0; i < nfl0; i++) {
    int site = R->fl[i].site, name = R->fl[i].name, node = R->fl[i].node;
    if (!bs_has(&seen, site) || name == bn_name("[]")) continue;
    if (R->nd[node].cls == 0 && bs_count(&R->nd[node].pts) == 0) continue;   /* nothing stored under this name (yet) */
    if (R->st[site].kind == SK_PROTO && name == bn_name("constructor") && !R->fl[i].prog) continue;   /* the engine's default link is not enumerable; a store of `constructor` made by the program is */
    BS_FOR(d, &R->nd[dstn].pts) {
      int e = fl_find(d, name, 1), fn_ = e >= 0 ? R->fl[e].node : -1;
      if (fn_ < 0) continue;
      nd_flow(fn_, node);
      if (R->collect) { R->fl[e].nwrites++; if (R->fl[e].firstfn < 0) { R->fl[e].firstfn = I->fn; R->fl[e].firstpc = in->pc; } }
    }
    applied = 1;
  }
  (void)depth; (void)applied;
  return 1;
}

static int apply_forward(const BInst *I, const Q2Insn *in, int dst, int inloop) {
  int fnd, apn, thn, s, k, ntarget = 0, apply_nm, disp_ok = 0, nodisp_ = 0, nrs = 0, rsl[64];
  BSet rfound[64];
  BCall *cs = NULL;
  BSet fset;
  if (!R->pts || in->win_n < 3 || in->win_val == NULL || dst < 0) return 0;
  fnd = vnode(I, in->win_val[0]); apn = vnode(I, in->win_val[1]); thn = vnode(I, in->win_val[2]);
  apply_nm = bn_name("apply");
  if (R->nd[apn].cls & K_UNK) return R->phase == 0 ? -1 : 0;   /* escapes are permanent: judge a set only once phase 0 has settled it */
  if (bs_count(&R->nd[apn].pts) == 0) return R->phase == 0 ? -1 : 0;
  BS_FOR(s, &R->nd[apn].pts) if (R->st[s].kind != SK_BUILTIN || R->st[s].fn != apply_nm || site_is_ext(s)) return R->phase == 0 ? -1 : 0;
  if ((R->nd[fnd].cls & K_UNK) || bs_has(&R->nd[fnd].pts, 0)) return R->phase == 0 ? -1 : 0;
  BS_FOR(s, &R->nd[fnd].pts) if (R->st[s].kind != SK_FUNC || R->st[s].fn < 0 || !g_bfn[R->st[s].fn].ok) return R->phase == 0 ? -1 : 0;
  if (bs_count(&R->nd[fnd].pts) == 0) return R->phase == 0 ? -1 : 0;
  if (R->collect) { cs = note_call(I->fn, in->pc, in->rop, inloop, ".apply"); cs->thru = 1; }
  /* `this.m.apply(this, arguments)`: the callee was read from the very object the call forwards as `this`, so each target is called on the objects whose prototype chain finds it (the
     per-object dispatch of a method call). The callee's field read is a read of `this`: get_this_field, get_field of the same node, or the get_local_field of a slot that holds exactly
     `this`. Without it (or with a receiver that is escaped / EXT) the whole receiver set flows to every target, as before. DIAG_NOAPPLYCLS: the old whole-set flow. */
  if (R->pts && getenv("QJS_AOT_BUNDLE_DIAG_NOAPPLYCLS") == NULL) {
    const Q2Insn *cd = bn_def(I, in->win_val[0]);
    char mn[64];
    int same = 0;
    if (cd != NULL && (cd->rop == ROP_get_this_field || cd->rop == ROP_get_this_field2)) same = 1;
    if (cd != NULL && (cd->rop == ROP_get_field || cd->rop == ROP_get_field2 || cd->rop == ROP_get_field_own || cd->rop == ROP_get_field2_own) && cd->nuse >= 1 && vnode(I, cd->use[0]) == thn) same = 1;
    if (cd != NULL && cd->rop >= ROP_get_local_field_0 && cd->rop <= ROP_get_local_field_3) {
      const BSet *lp = &R->nd[locnode(I, cd->rop - ROP_get_local_field_0)].pts, *tp = &R->nd[thn].pts;
      int a;
      if (bs_count(lp) == bs_count(tp) && bs_count(tp) > 0) { same = 1; BS_FOR(a, tp) if (!bs_has(lp, a)) same = 0; }
    }
    if (same && !(R->nd[thn].cls & K_UNK) && bs_count(&R->nd[thn].pts) <= 64 && bn_atom(cd, NULL, mn, sizeof(mn), 0)) {
      int mid = bn_name(mn), rs;
      BSet rset = R->nd[thn].pts;
      disp_ok = 1;
      BS_FOR(rs, &rset) {
        int save = R->changed;
        if (site_is_ext(rs) || bs_has(&R->esc, rs)) { nodisp_ = 1; continue; }
        memset(&R->nd[R->tmp_in], 0, sizeof(BNode));
        memset(&R->nd[R->tmp_out], 0, sizeof(BNode));
        bs_add(&R->nd[R->tmp_in].pts, rs);
        R->nd[R->tmp_in].cls = K_OBJ;
        read_field(R->tmp_out, R->tmp_in, mid, I->fn, in->pc);
        rsl[nrs] = rs; rfound[nrs] = R->nd[R->tmp_out].pts; nrs++;
        R->changed = save;
      }
    }
  }
  fset = R->nd[fnd].pts;   /* a copy: inst_get below can grow (move) the node array */
  BS_FOR(s, &fset) {
    BSite *S = &R->st[s];
    int ti = inst_get(S->fn, small_fn(S->fn) ? (I->fn * 65536 + in->pc + 1) : 0);
    const BInst *T = &R->in[ti];
    if (!R->in[ti].live) { R->in[ti].live = 1; R->changed = 1; }
    if (R->collect) { note_edge(I->fn, in->pc, S->fn, inloop); call_add_target(cs, S->fn); }
    if (R->collect && !(disp_ok && !nodisp_)) ev_add(g_cur_inst, in->pc, -1, -1, -1, ti, &R->nd[thn].pts);
    if (disp_ok && !nodisp_) {
      int q;
      for (q = 0; q < nrs; q++) if (bs_has(&rfound[q], s)) {
        if (R->collect) { BSet one; memset(&one, 0, sizeof(one)); bs_add(&one, rsl[q]); ev_add(g_cur_inst, in->pc, -1, -1, -1, ti, &one); rt_add(I->fn, in->pc, rsl[q], S->fn); }
        nd_site(T->thisn, rsl[q]);
        R->st[rsl[q]].flags |= SF_THIS;
      }
    } else {
      nd_flow(T->thisn, thn);
      mark_flags(thn, SF_THIS);
    }
    for (k = 0; k < 16; k++) {
      if (k < I->npar) { nd_flow(T->parn[k < T->npar ? k : 16], I->parn[k]); }
      else {
        if (getenv("QJS_AOT_BUNDLE_DIAG_NOPOSARGS") != NULL) nd_flow(T->parn[k < T->npar ? k : 16], I->parn[16]);   /* the old merge of every extra argument */
        else {
          nd_flow(T->parn[k < T->npar ? k : 16], I->extra[k]);   /* the argument the caller passed at position k beyond this function's declared parameters */
          if (k >= T->npar) nd_flow(T->extra[k], I->extra[k]);
        }
        if (k < T->npar) nd_cls(T->parn[k], K_UNDEF);
      }
    }
    nd_flow(T->parn[16], I->parn[16]);
    nd_flow(dst, T->retn);
    ntarget++;
  }
  (void)ntarget;
  if (R->collect) { if (disp_ok) rcv_add(I->fn, in->pc, thn, nodisp_ ? 1 : 0); else rcv_add(I->fn, in->pc, -1, 1); }
  return 1;
}


/* The value register `reg` holds just before instruction `ii` of block `blk`: the latest definition (or redefinition) above it in the block, else the block's live-in value. */
static Q2ValueId reg_value_before(const Q2Func *q, int blk, int ii, int reg) {
  const Q2Block *B = &q->blocks[blk];
  int x, d, k;
  for (x = ii - 1; x >= B->first_insn; x--) {
    const Q2Insn *p = &q->insns[x];
    for (d = 0; d < p->ndef; d++) if ((int32_t)p->def[d] >= 0 && (int32_t)p->def[d] < q->nvalues && q->values[p->def[d]].reg == reg) return p->def[d];
    for (k = 0; k < p->nredef; k++) if (p->redef_reg[k] == reg) return p->redef_val[k];
  }
  for (k = 0; k < B->nlive_in; k++) if (B->in_reg[k] == reg) return B->in_val[k];
  for (k = 0; k < B->nparams; k++) if (B->param_reg[k] == reg) return B->param_val[k];
  return Q2_NO_VALUE;
}


/* An empty object literal `{}` that is stored straight into a global or a property (`NS = {}`, `NS.Sub = {}`): the property name it is stored under, or -1. Namespace declarations repeated in
   every file of a concatenated program (`if (typeof NS == 'undefined') NS = {}`) create one abstract object per file though only the first runs; sites stored under the same name are merged
   into ONE abstract object (a coarser heap abstraction: sound, and it keeps the first file's fields visible to a read that the other files' sites would miss). */
static int literal_store_name(const BFn *F, int ii, Q2ValueId v) {
  int x, hop = 0;
  for (x = ii + 1; x < F->q.ninsns && hop < 12; x++, hop++) {
    const Q2Insn *p = &F->q.insns[x];
    char nm[64];
    int u;
    if (p->rop == ROP_move || p->rop == ROP_move_s || p->rop == ROP_move_xfer) {
      if (p->nuse >= 1 && p->use[0] == v && p->ndef >= 1) { v = p->def[0]; continue; }
      continue;
    }
    for (u = 0; u < p->nuse; u++) if (p->use[u] == v) break;
    if (u == p->nuse) continue;
    if (p->rop == ROP_put_global && u == 0 && bn_atom(p, NULL, nm, sizeof(nm), 0)) return bn_name(nm);
    if ((p->rop == ROP_put_field || p->rop == ROP_put_field_xfer) && u == p->nuse - 1 && p->nuse >= 2 && bn_atom(p, NULL, nm, sizeof(nm), 0)) return bn_name(nm);
    return -1;   /* some other first use */
  }
  return -1;
}

/* one instruction */
static void do_insn(const BInst *I, int ii, int blk, int inloop) {
  const BFn *F = &g_bfn[I->fn];
  const Q2Insn *in = &F->q.insns[ii];
  int dst = in->ndef >= 1 ? vnode(I, in->def[0]) : -1;
  int dst2 = in->ndef >= 2 ? vnode(I, in->def[1]) : -1;
  char nm[64];
  int k, handled = 1;
  BFn *WF = &g_bfn[I->fn];
  g_cur_fn = I->fn;
  g_cur_pc = in->pc;
  g_cur_rop = in->rop;
  g_cur_why = "operand of an instruction the model does not know";
  switch (in->rop) {
    case ROP_load_undefined: nd_cls(dst, K_UNDEF); break;
    case ROP_load_null: nd_cls(dst, K_NULL); break;
    case ROP_load_true: case ROP_load_false: nd_cls(dst, K_BOOL); break;
    case ROP_load_i8: case ROP_load_i8_s: case ROP_load_i32: nd_cls(dst, K_INT); break;
    case ROP_load_const: {
      int kind = in->narg > 1 ? (int)in->arg[1].val : 0, idx = in->narg > 2 ? (int)in->arg[2].val : 0;
      if (kind == REG_CONST_ATOM) nd_cls(dst, K_STR);
      else if (idx >= 0 && idx < F->g->cpool_count && F->g->cpool_numeric != NULL && F->g->cpool_numeric[idx] != 0)
        nd_cls(dst, F->g->cpool_numeric[idx] == JS_TAG_INT ? K_INT : K_DBL);
      else nd_cls(dst, K_STR | K_UNK);
      break;
    }
    case ROP_load_this:
      nd_flow(dst, I->thisn);
      if (R->collect && R->pts && getenv("QJS_AOT_BUNDLE_THISDBG") != NULL && strstr(fn_name(I->fn), getenv("QJS_AOT_BUNDLE_THISDBG")) != NULL) {
        char sl_[200]; set_str_fwd(R, &R->nd[I->thisn].pts, sl_, sizeof(sl_), 6);
        fprintf(stderr, "bundle: load_this %s:%d ctx %d thisn=[%s] dst=[%d] live=%d\n", fn_name(I->fn), in->pc, I->ctx, sl_, dst, I->live);
      }
      break;
    case ROP_move: case ROP_move_s: case ROP_move_xfer:
      if (in->nuse >= 1) nd_flow(dst, vnode(I, in->use[0]));
      break;
    case ROP_get_local:
      nd_flow(dst, locnode(I, in->narg > 1 ? (int)in->arg[1].val : -1));
      break;
    case ROP_get_local_s0: case ROP_get_local_s1: case ROP_get_local_s2: case ROP_get_local_s3:
      nd_flow(dst, locnode(I, in->rop - ROP_get_local_s0));
      break;
    case ROP_put_local: case ROP_put_local_xfer: case ROP_put_local_probe:
      if (in->nuse >= 1) nd_flow(locnode(I, in->narg > 0 ? (int)in->arg[0].val : -1), vnode(I, in->use[0]));
      break;
    case ROP_put_local_s0: case ROP_put_local_s1: case ROP_put_local_s2: case ROP_put_local_s3:
      if (in->nuse >= 1) nd_flow(locnode(I, in->rop - ROP_put_local_s0), vnode(I, in->use[0]));
      break;
    case ROP_put_local_xfer_s0: case ROP_put_local_xfer_s1: case ROP_put_local_xfer_s2: case ROP_put_local_xfer_s3:
      if (in->nuse >= 1) nd_flow(locnode(I, in->rop - ROP_put_local_xfer_s0), vnode(I, in->use[0]));
      break;
    case ROP_local_rmw_imm: case ROP_local_rmw_branch: {
      int ln = locnode(I, in->narg > 0 ? (int)in->arg[0].val : -1);
      nd_cls(ln, arith_cls(REG_BIN_ADD, R->nd[ln].cls, K_INT));
      break;
    }
    case ROP_get_arg: nd_flow(dst, argnode(I, in->narg > 1 ? (int)in->arg[1].val : -1)); break;
    case ROP_get_arg2:
      nd_flow(dst, argnode(I, in->narg > 1 ? (int)in->arg[1].val : -1));
      nd_flow(dst2, argnode(I, in->narg > 3 ? (int)in->arg[3].val : -1));
      break;
    case ROP_put_arg: case ROP_put_arg_xfer:
      if (in->nuse >= 1) { int an = argnode(I, in->narg > 0 ? (int)in->arg[0].val : -1); if (an != parnode(I, in->narg > 0 ? (int)in->arg[0].val : -1)) cell_store(an, vnode(I, in->use[0])); else nd_flow(an, vnode(I, in->use[0])); }
      break;
    case ROP_get_var_ref:
      if (CELLS_ON) {
        int cf, ia, ix;
        if (in->narg >= 2 && vref_cell(I->fn, (int)in->arg[1].val, 0, &cf, &ia, &ix)) nd_flow(dst, cell_node(cf, ia, ix));
        else { nd_cls(dst, K_UNK); nd_site(dst, 0); }   /* a variable the tables do not resolve: unknown contents */
      } else nd_flow(dst, R->varref_node);
      break;
    case ROP_put_var_ref:
      if (in->nuse >= 1 && CELLS_ON) {
        int cf, ia, ix;
        if (in->narg >= 1 && vref_cell(I->fn, (int)in->arg[0].val, 0, &cf, &ia, &ix)) cell_store(cell_node(cf, ia, ix), vnode(I, in->use[0]));
        else { g_cur_why = "stored into a variable the closure tables do not resolve"; escape_node(vnode(I, in->use[0])); }
        mark_flags(vnode(I, in->use[0]), SF_CLOSURE);
      } else if (in->nuse >= 1) { nd_flow(R->varref_node, vnode(I, in->use[0])); mark_flags(vnode(I, in->use[0]), SF_CLOSURE); }
      if (in->nuse >= 1 && R->collect && R->pts && getenv("QJS_AOT_BUNDLE_VARREFDBG") != NULL) {   /* diagnostic: what flows into the one shared closure-variable cell */
        char cl[128], sl[300];
        cls_str_fwd(R->nd[vnode(I, in->use[0])].cls, cl, sizeof(cl));
        set_str_fwd(R, &R->nd[vnode(I, in->use[0])].pts, sl, sizeof(sl), 6);
        fprintf(stderr, "bundle: put_var_ref %s:%d value cls=%s sites=[%s]\n", fn_name(I->fn), in->pc, cl, sl);
      }
      if (R->collect) WF->wvarref = 1;
      break;
    case ROP_get_global:
      if (bn_atom(in, NULL, nm, sizeof(nm), 0)) {
        int nid = bn_name(nm), g = gnode(nid);
        nd_flow(dst, g);
        /* nothing in the bundle defines it: a builtin or an external binding */
        if (!g_gdef[nid] && strcmp(nm, "Object") != 0 && !(g_math_model && strcmp(nm, "Math") == 0)) { nd_cls(dst, K_UNK); nd_site(dst, 0); }
        else if (R->phase == 1 && R->nd[g].cls == 0 && bs_count(&R->nd[g].pts) == 0) {
          nd_cls(dst, K_UNK); nd_site(dst, 0);
          if (g_dbg_events < g_dbg_max && getenv("QJS_AOT_BUNDLE_EVENTS") != NULL) { g_dbg_events++; fprintf(stderr, "bundle: EVENT empty global %s at %s:%d\n", nm, fn_name(I->fn), in->pc); }
        }
      } else { nd_cls(dst, K_UNK); nd_site(dst, 0); }
      break;
    case ROP_put_global: case ROP_define_global: case ROP_define_global_func:
      if (bn_atom(in, NULL, nm, sizeof(nm), 0) && in->nuse >= 1) {
        nd_flow(gnode(bn_name(nm)), vnode(I, in->use[0]));
        mark_flags(vnode(I, in->use[0]), SF_GLOBAL);
        if (R->collect) WF->wglobal = 1;
      }
      break;
    case ROP_make_closure: {
      int ci = -1, fn;
      for (k = 0; k < in->narg; k++) if (in->arg[k].kind == 'c') { ci = (int)in->arg[k].val; break; }
      fn = fn_of_closure(F->g, ci);
      if (fn < 0) { nd_cls(dst, K_UNK); nd_site(dst, 0); R->unmodeled[ROP_make_closure & 255]++; break; }
      {
        char lb[80];
        int fs, ps;
        snprintf(lb, sizeof(lb), "fn %s@%s:%d", fn_name(fn), fn_name(I->fn), in->pc);
        fs = st_new(SK_FUNC, fn, I->fn, in->pc, I->ctx, lb);
        if (R->st[fs].label[0] == 0) snprintf(R->st[fs].label, sizeof(R->st[fs].label), "%s", lb);
        ps = st_new(SK_PROTO, -1, I->fn, in->pc, I->ctx, "");
        if (R->st[ps].label[0] == 0) snprintf(R->st[ps].label, sizeof(R->st[ps].label), "%s.prototype", fn_name(fn));
        R->st[ps].owner = fs;
        nd_site(dst, fs);
        nd_site(fnode(ps, bn_name("constructor"), 1), fs);
        if (R->collect) WF->alloc++;
      }
      break;
    }
    case ROP_new_object: case ROP_object_template: case ROP_array_from: case ROP_array_const: case ROP_regexp: {
      int arr = in->rop == ROP_array_from || in->rop == ROP_array_const;
      int ns, lname = (in->rop == ROP_new_object && !g_diag_nomerge && R->pts && in->ndef >= 1) ? literal_store_name(F, ii, in->def[0]) : -1;
      if (lname >= 0) {
        ns = st_new(SK_OBJ, -1, -7, lname, 0, "");   /* every empty literal stored under this name is one abstract object */
        if (R->st[ns].label[0] == 0) snprintf(R->st[ns].label, sizeof(R->st[ns].label), "{}#%s", g_bn_names[lname]);
      } else {
        ns = st_new(arr ? SK_ARR : SK_OBJ, -1, I->fn, in->pc, I->ctx, "");
        if (R->st[ns].label[0] == 0)
          snprintf(R->st[ns].label, sizeof(R->st[ns].label), "%s@%s:%d", arr ? "[]" : "{}", fn_name(I->fn), in->pc);
      }
      nd_site(dst, ns);
      if (in->rop == ROP_array_from)
        for (k = 0; k < in->win_n && in->win_val != NULL; k++) {
          nd_flow(fnode(ns, bn_name("[]"), 1), vnode(I, in->win_val[k]));
          mark_flags(vnode(I, in->win_val[k]), SF_STORED);
        }
      else if (in->rop == ROP_array_const) nd_cls(fnode(ns, bn_name("[]"), 1), K_UNK);

      if (R->collect) WF->alloc++;
      break;
    }
    case ROP_object_fill: {
      int ti = in->narg > 3 ? (int)in->arg[3].val : -1, n = in->narg > 2 ? (int)in->arg[2].val : 0;
      if (in->nuse < 1 || ti < 0 || ti >= F->g->objlit_count || F->g->objlit_atoms == NULL || F->g->objlit_atoms[ti] == NULL ||
          n > F->g->objlit_nprops[ti] || in->win_val == NULL) {
        R->unmodeled[ROP_object_fill]++;
        escape_node(in->nuse >= 1 ? vnode(I, in->use[0]) : -1);
        for (k = 0; k < in->win_n && in->win_val != NULL; k++) escape_node(vnode(I, in->win_val[k]));
        break;
      }
      for (k = 0; k < n && k < in->win_n; k++) {
        size_t len;
        const char *an = JS_AtomToCStringLen(g_bctx, &len, F->g->objlit_atoms[ti][k]);
        if (an == NULL) continue;
        write_field(vnode(I, in->use[0]), bn_name(an), vnode(I, in->win_val[k]), I->fn, in->pc, blk);
        mark_flags(vnode(I, in->win_val[k]), SF_STORED);
        if (R->collect) { int w = bn_name(an); WF->wn[w >> 6] |= 1ull << (w & 63); }
        JS_FreeCString(g_bctx, an);
      }
      break;
    }
    case ROP_get_field: case ROP_get_field2: case ROP_get_field_own: case ROP_get_field2_own: {
      int recv = in->nuse >= 1 ? vnode(I, in->use[0]) : R->sink_node;
      if (!bn_atom(in, NULL, nm, sizeof(nm), 0)) { nd_cls(dst, K_UNK); nd_site(dst, 0); break; }
      if (!g_x_nodst2 && (in->rop == ROP_get_field2 || in->rop == ROP_get_field2_own) && dst2 >= 0) nd_flow(dst2, recv);   /* the receiver copy a method call takes as its `this` */
      read_field(dst, recv, bn_name(nm), I->fn, in->pc);
      if (R->collect) acc_add(I->fn, in->pc, 0, bn_name(nm), recv);
      mark_flags(recv, SF_RECV);
      break;
    }
    case ROP_get_this_field: case ROP_get_this_field2:
      if (!bn_atom(in, NULL, nm, sizeof(nm), 0)) { nd_cls(dst, K_UNK); nd_site(dst, 0); break; }
      nd_flow(dst2, I->thisn);
      read_field(dst, I->thisn, bn_name(nm), I->fn, in->pc);
      if (R->collect) acc_add(I->fn, in->pc, 0, bn_name(nm), I->thisn);
      mark_flags(I->thisn, SF_RECV);
      break;
    case ROP_get_local_field: case ROP_get_local_field_0: case ROP_get_local_field_1: case ROP_get_local_field_2:
    case ROP_get_local_field_3: {
      int slot = in->rop == ROP_get_local_field ? (int)in->arg[1].val : in->rop - ROP_get_local_field_0;
      if (!bn_atom(in, NULL, nm, sizeof(nm), 0)) { nd_cls(dst, K_UNK); nd_site(dst, 0); break; }
      read_field(dst, locnode(I, slot), bn_name(nm), I->fn, in->pc);
      mark_flags(locnode(I, slot), SF_RECV);
      break;
    }
    case ROP_get_arg_field: {
      if (!bn_atom(in, NULL, nm, sizeof(nm), 0)) { nd_cls(dst, K_UNK); nd_site(dst, 0); break; }
      read_field(dst, argnode(I, (int)in->arg[1].val), bn_name(nm), I->fn, in->pc);
      break;
    }
    case ROP_get_field_chain: case ROP_get_field_chain_own: {
      char n2[64];
      int mid = R->sink_node;
      if (!bn_atom(in, NULL, nm, sizeof(nm), 0) || !bn_atom(in, NULL, n2, sizeof(n2), 1)) {
        nd_cls(dst, K_UNK); nd_site(dst, 0); nd_cls(dst2, K_UNK); nd_site(dst2, 0); break;
      }
      read_field(dst, in->nuse >= 1 ? vnode(I, in->use[0]) : R->sink_node, bn_name(nm), I->fn, in->pc);
      if (R->collect && in->nuse >= 1) acc_add(I->fn, in->pc, 0, bn_name(nm), vnode(I, in->use[0]));
      mid = dst; /* the second read's receiver is the first read's destination (operand 5 is unused) */
      read_field(dst2, mid, bn_name(n2), I->fn, in->pc);
      if (R->collect) acc_add(I->fn, in->pc, 0, bn_name(n2), mid);
      break;
    }
    case ROP_put_field: case ROP_put_field_xfer: case ROP_define_field: {
      int recv, val;
      if (in->nuse < 2) { handled = 0; break; }
      recv = vnode(I, in->use[0]);
      val = vnode(I, in->use[1]);
      if (!bn_atom(in, NULL, nm, sizeof(nm), 0)) { escape_node(recv); escape_node(val); break; }
      if (strcmp(nm, "__proto__") == 0 && R->collect) R->setproto_ops++;
      write_field(recv, bn_name(nm), val, I->fn, in->pc, blk);
      if (R->collect) acc_add(I->fn, in->pc, 1, bn_name(nm), recv);
      mark_flags(val, SF_STORED);
      mark_flags(recv, SF_RECV);
      if (R->collect) { int w = bn_name(nm); WF->wn[w >> 6] |= 1ull << (w & 63); }
      break;
    }
    case ROP_get_element: case ROP_get_length: {
      int recv = in->nuse >= 1 ? vnode(I, in->use[0]) : R->sink_node, s;
      if (in->rop == ROP_get_element) seal_recv(recv, I->fn, in->pc);
      if (in->rop == ROP_get_length) {
        BS_FOR(s, &R->nd[recv].pts) {
          if (R->st[s].kind == SK_ARR) nd_cls(dst, K_INT);
          else if (site_is_ext(s) || R->st[s].kind == SK_BUILTIN) nd_cls(dst, K_UNK);
          else read_field(dst, recv, bn_name("length"), I->fn, in->pc);
        }
        if (R->nd[recv].cls & K_STR) nd_cls(dst, K_INT);
        if (!R->pts) nd_cls(dst, K_INT | K_UNK);
        break;
      }
      BS_FOR(s, &R->nd[recv].pts) {
        if (site_is_ext(s) || bs_has(&R->esc, s)) { nd_cls(dst, K_UNK); nd_site(dst, 0); if (site_is_ext(s)) continue; }
        nd_flow(dst, fnode(s, bn_name("[]"), 1));
        {
          /* a computed key can name any field of the object: the element read sees every named field of the site (and so the methods a for-in copy moves between objects) */
          int fi_;
          for (fi_ = 0; fi_ < R->nfl && R->st[s].kind != SK_ARR && !g_diag_noelem; fi_++)
            if (R->fl[fi_].site == s && R->fl[fi_].name != bn_name("[]")) nd_flow(dst, R->fl[fi_].node);
        }
        {
          /* a string key or a computed key can name any field: the element node also holds them in this model */
        }
      }
      if (R->nd[recv].cls & K_UNK) { nd_cls(dst, K_UNK); nd_site(dst, 0); }
      if (R->nd[recv].cls & K_STR) nd_cls(dst, K_STR);
      mark_flags(recv, SF_RECV);
      break;
    }
    case ROP_put_element: case ROP_put_element_xfer: case ROP_define_element: {
      int recv, val, s;
      if (in->nuse < 3) { handled = 0; break; }
      recv = vnode(I, in->use[0]);
      val = vnode(I, in->use[2]);
      seal_recv(recv, I->fn, in->pc);
      {
        const Q2Value *kv = in->narg > 1 && in->arg[1].kind == 'r' && in->argval[1] != Q2_NO_VALUE ? &F->q.values[in->argval[1]] : NULL;
        if (R->collect && (kv == NULL || !kv->is_const || kv->con_kind != Q2_CON_INT)) {
          int rs2, nonarr = 0;
          BS_FOR(rs2, &R->nd[recv].pts) if (R->st[rs2].kind != SK_ARR) nonarr = 1;
          if (nonarr) R->dyn_key_stores++;
        }
      }
      if (in->rop != ROP_define_element && forin_copy(I, in)) { if (R->collect) WF->welem = 1; break; }
      BS_FOR(s, &R->nd[recv].pts) {
        if (site_is_ext(s) || bs_has(&R->esc, s)) { escape_node(val); if (site_is_ext(s)) continue; }
        nd_flow(fnode(s, bn_name("[]"), 1), val);
      }
      mark_flags(val, SF_STORED);
      if (R->collect) WF->welem = 1;
      break;
    }
    case ROP_define_class: case ROP_define_class_computed: {
      int ci = in->nuse >= 1 ? vnode(I, in->use[0]) : R->sink_node;
      if (dst >= 0) nd_flow(dst, ci);
      if (in->win_n > 0 && in->win_val != NULL)
        for (k = 0; k < in->win_n; k++) if (dst2 >= 0) nd_flow(dst2, vnode(I, in->win_val[k]));
      if (bn_atom(in, NULL, nm, sizeof(nm), 0) && dst >= 0) nd_flow(gnode(bn_name(nm)), dst);
      R->unmodeled[ROP_define_class]++;
      for (k = 0; k < in->nredef; k++) { nd_cls(vnode(I, in->redef_val[k]), K_UNK); nd_site(vnode(I, in->redef_val[k]), 0); }
      break;
    }
    case ROP_define_method: case ROP_define_method_computed: {
      if (in->narg > 2 && in->arg[0].kind == 'r' && in->arg[2].kind == 'r' && bn_atom(in, NULL, nm, sizeof(nm), 0)) {
        int tgt = vnode(I, in->argval[0]), val = vnode(I, in->argval[2]), s;
        int acc_kind = in->narg > 3 ? (int)(in->arg[3].val & 3) : 0;   /* 0 method, 1 getter, 2 setter (the engine's op flags) */
        if (acc_kind != 0 && !g_x_noacc) {
          /* an accessor: reading or writing the property RUNS the function, from code the analysis cannot see: the function escapes and the slot is unknown */
          g_cur_why = "an accessor function";
          escape_node(val);
          BS_FOR(s, &R->nd[tgt].pts) { int fn_ = fnode(s, bn_name(nm), 1); nd_cls(fn_, K_UNK); nd_site(fn_, 0); }
          if (R->collect) R->accessors++;
          if (dst >= 0) nd_flow(dst, tgt);
          break;
        }
        BS_FOR(s, &R->nd[tgt].pts) {
          write_field(tgt, bn_name(nm), val, I->fn, in->pc, blk);
          if (R->st[s].kind == SK_FUNC) {
            int pp;
            for (pp = 0; pp < R->nst; pp++) if (R->st[pp].kind == SK_PROTO && R->st[pp].owner == s) nd_site(fnode(pp, bn_name(nm), 1), 0);
          }
        }
        if (dst >= 0) nd_flow(dst, tgt);
        mark_flags(val, SF_STORED);
      } else {
        /* a method under a computed key (Symbol.iterator, Symbol.toPrimitive, ...): the engine may call it */
        if (!g_x_nocomp && in->narg > 2 && in->arg[2].kind == 'r') { g_cur_why = "a method stored under a computed key"; escape_node(vnode(I, in->argval[2])); }
        if (in->narg > 2 && in->arg[0].kind == 'r' && in->arg[2].kind == 'r' && in->argval[0] != Q2_NO_VALUE && in->argval[2] != Q2_NO_VALUE) {
          /* it is also stored in the target's computed-key content ([]): a coercion that finds content there cannot resolve valueOf / toString / @@toPrimitive by name */
          int tgt_ = vnode(I, in->argval[0]), s_;
          BS_FOR(s_, &R->nd[tgt_].pts) if (!site_is_ext(s_)) nd_flow(fnode(s_, bn_name("[]"), 1), vnode(I, in->argval[2]));
        }
        if (dst >= 0) { nd_cls(dst, K_UNK); nd_site(dst, 0); }
      }
      R->unmodeled[ROP_define_method]++;
      break;
    }
    case ROP_set_proto: {
      int s;
      if (in->nuse >= 2) {
        BS_FOR(s, &R->nd[vnode(I, in->use[0])].pts) if (bs_or(&R->st[s].proto, &R->nd[vnode(I, in->use[1])].pts)) R->changed = 1;
      }
      if (R->collect) R->setproto_ops++;
      break;
    }
    case ROP_call: if (in->nuse >= 1) do_call(I, in, 0, inloop); else handled = 0; break;
    case ROP_call_method: if (in->nuse >= 2) do_call(I, in, 1, inloop); else handled = 0; break;
    case ROP_construct: if (in->nuse >= 1) do_call(I, in, 2, inloop); else handled = 0; break;
    case ROP_tail_call: case ROP_tail_call_method:
      handled = 0;
      break;
    case ROP_return:
      if (in->nuse >= 1) {
        nd_flow(I->retn, vnode(I, in->use[0]));
        mark_flags(vnode(I, in->use[0]), SF_RET);
      }
      break;
    case ROP_return_undefined: nd_cls(I->retn, K_UNDEF); break;
    case ROP_throw: case ROP_throw_error:
      if (R->collect) WF->thrw = 1;
      escape_node(in->nuse >= 1 ? vnode(I, in->use[0]) : -1);
      break;
    case ROP_check_ctor_return: case ROP_init_ctor:
      if (dst >= 0 && in->nuse >= 1) nd_flow(dst, vnode(I, in->use[0]));
      else if (dst >= 0) nd_flow(dst, I->thisn);
      break;
    case ROP_special_object: nd_cls(dst, K_UNK); nd_site(dst, 0); if (R->collect) R->special_args++; break;
    case ROP_lt: case ROP_lte: case ROP_gt: case ROP_gte: case ROP_eq: case ROP_neq: case ROP_strict_eq: case ROP_strict_neq:
    case ROP_instanceof: case ROP_in:
      nd_cls(dst, K_BOOL);
      break;
    case ROP_mul: case ROP_add: case ROP_sub: case ROP_div: case ROP_mod: case ROP_pow: case ROP_bitand: case ROP_bitor:
    case ROP_bitxor: case ROP_shl: case ROP_sar: case ROP_shr: {
      unsigned x = in->nuse >= 1 ? R->nd[vnode(I, in->use[0])].cls : K_INT, y = in->nuse >= 2 ? R->nd[vnode(I, in->use[1])].cls : K_INT;
      int sub = in->rop == ROP_mul ? REG_BIN_MUL : in->rop == ROP_add ? REG_BIN_ADD : in->rop == ROP_sub ? REG_BIN_SUB :
                in->rop == ROP_div ? REG_BIN_DIV : in->rop == ROP_mod ? REG_BIN_MOD : in->rop == ROP_pow ? REG_BIN_POW :
                in->rop == ROP_bitand ? REG_BIN_AND : in->rop == ROP_bitor ? REG_BIN_OR : in->rop == ROP_bitxor ? REG_BIN_XOR :
                in->rop == ROP_shl ? REG_BIN_SHL : in->rop == ROP_sar ? REG_BIN_SAR : REG_BIN_SHR;
      nd_cls(dst, arith_cls(sub, x, y));
      break;
    }
    case ROP_neg: case ROP_inc: case ROP_dec: case ROP_not: case ROP_to_numeric: {
      unsigned x = in->nuse >= 1 ? R->nd[vnode(I, in->use[0])].cls : 0;
      if (x) nd_cls(dst, arith_cls(REG_BIN_SUB, x, K_INT));
      break;
    }
    case ROP_check_define: case ROP_sentinel_drop: case ROP_nip_catch:
      break;
    case ROP_catch_enter:
      for (k = 0; k < in->ndef && k < 2; k++) { int n = vnode(I, in->def[k]); nd_cls(n, K_UNK); nd_site(n, 0); }
      break;
    case ROP_eval: case ROP_apply_eval: if (R->collect) R->eval_ops++; handled = 0; break;
    case ROP_with_get: case ROP_with_put: case ROP_with_delete: case ROP_with_make_ref: case ROP_with_get_ref:
      if (getenv("QJS_AOT_BUNDLE_DIAG_WITHAPPLY") == NULL) { if (R->collect) R->with_ops++; handled = 0; break; }   /* a with_* op is not an apply: it never enters apply_forward (DIAG_WITHAPPLY restores the old stacking) */
      /* fall through */
    case ROP_apply_arguments: {
      int r_ = getenv("QJS_AOT_BUNDLE_DIAG_NOAPPLY") != NULL ? 0 : apply_forward(I, in, dst, inloop);
      if (r_ == 0 && R->collect) { BCall *c_ = note_call(I->fn, in->pc, in->rop, inloop, ".apply"); c_->unknown = 1; g_bfn[I->fn].unkcall = 1; R->unk_calls++; }
      if (r_ == 0) handled = 0;   /* not the exact forward of builtin apply to resolved functions: operands escape, result unknown, as before */
      break;
    }
    case ROP_for_in_start: case ROP_for_in_next: {
      /* `for (k in o)`: a bundle-only side table for the two ops (the IR keeps them as roles-free fallbacks that redefine the live registers). They work in place on a window at register w:
         start turns the object in w into the enumerator; next reads the enumerator in w and writes the key to w+1 and the done flag to w+2. The source object is READ, not escaped (a
         computed read `o[k]` sees all its named fields); the key is a string. Every other redefined register keeps the value it had. QJS_AOT_BUNDLE_DIAG_NOFORIN=1: the old unknown-op handling. */
      int w = in->narg > 0 && in->arg[0].kind == 'r' ? (int)(in->arg[0].val / 16) : -1, kk;
      if (g_diag_noforin || w < 0 || in->nredef <= 0) { handled = 0; break; }
      for (kk = 0; kk < in->nredef; kk++) {
        int reg = in->redef_reg[kk], vn = vnode(I, in->redef_val[kk]);
        Q2ValueId pv = reg_value_before(&F->q, blk, ii, reg);
        if (in->rop == ROP_for_in_start && reg == w) {
          if (pv != Q2_NO_VALUE) { nd_flow(vn, vnode(I, pv)); mark_flags(vnode(I, pv), SF_RECV); }   /* the enumerator stands for the sites it enumerates */
        } else if (in->rop == ROP_for_in_next && reg == w + 1) nd_cls(vn, K_STR);
        else if (in->rop == ROP_for_in_next && reg == w + 2) nd_cls(vn, K_BOOL);
        else if (pv != Q2_NO_VALUE) nd_flow(vn, vnode(I, pv));
        else { nd_cls(vn, K_UNK); nd_site(vn, 0); }
      }
      break;
    }
    case ROP_with_get_ref_undef: if (R->collect) R->with_ops++; handled = 0; break;
    case ROP_delete: if (R->collect) R->delete_ops++; handled = 0; break;
    case ROP_copy_props: if (R->collect) R->copyprops_ops++; handled = 0; break;
    default: handled = 0; break;
  }
  if (handled) {
    /* a store to a global binding changes no register, but the IR gives the registers live after it new versions (see build.c): each is the value the register held before, so the
       flow continues through it. (Left empty, every later use of `this` or a local was an empty set: calls through them went unresolved and their targets looked unreachable.) */
    if (in->nredef > 0 && !g_diag_noredef && (in->rop == ROP_put_global || in->rop == ROP_define_global || in->rop == ROP_define_global_func)) {
      for (k = 0; k < in->nredef; k++) {
        Q2ValueId pv = reg_value_before(&F->q, blk, ii, in->redef_reg[k]);
        if (pv != Q2_NO_VALUE) nd_flow(vnode(I, in->redef_val[k]), vnode(I, pv));
        else { int n = vnode(I, in->redef_val[k]); nd_cls(n, K_UNK); nd_site(n, 0); }
      }
    }
    return;
  }
  /* generic ops by class */
  switch (in->op) {
    case Q2_OP_CONST:
      if (dst >= 0 && in->ndef >= 1 && (int32_t)in->def[0] >= 0 && F->q.values[in->def[0]].is_const) nd_cls(dst, cls_of_const(&F->q.values[in->def[0]]));
      return;
    case Q2_OP_BINARY: {
      unsigned x = in->nuse >= 1 ? R->nd[vnode(I, in->use[0])].cls : K_INT;
      unsigned y = in->nuse >= 2 ? R->nd[vnode(I, in->use[1])].cls : K_INT;
      if (in->sub >= REG_BIN_LT && in->sub <= REG_BIN_PRIVATE_IN) { nd_cls(dst, K_BOOL); return; }
      if (in->sub2 >= 0) {
        /* a fused pair: def 0 is the first half (x <sub> y), def 1 the second (that result <sub2> the third operand); the second result used to stay EMPTY, which a store
           of it read as "no value" (a field fed only by fused arithmetic looked int-only) */
        unsigned z = in->nuse >= 3 ? R->nd[vnode(I, in->use[2])].cls : K_INT, m = arith_cls(in->sub, x, y);
        nd_cls(dst, m);
        if (dst2 >= 0) nd_cls(dst2, arith_cls(in->sub2, m, z));
        return;
      }
      nd_cls(dst, arith_cls(in->sub, x, y));
      return;
    }
    case Q2_OP_COMPARE: nd_cls(dst, K_BOOL); return;
    case Q2_OP_UNARY: {
      unsigned x = in->nuse >= 1 ? R->nd[vnode(I, in->use[0])].cls : 0;
      switch (in->sub) {
        case REG_UN_LNOT: case REG_UN_IS_UNDEFINED: case REG_UN_IS_NULL: case REG_UN_TYPEOF_IS_UNDEFINED:
        case REG_UN_TYPEOF_IS_FUNCTION: case REG_UN_IS_UNDEFINED_OR_NULL:
          nd_cls(dst, K_BOOL); return;
        case REG_UN_TYPEOF: nd_cls(dst, K_STR); return;
        case REG_UN_NEG: case REG_UN_NOT: case REG_UN_INC: case REG_UN_DEC:
          if (x) nd_cls(dst, arith_cls(REG_BIN_SUB, x, K_INT));
          return;
        default: nd_cls(dst, K_NUM | K_UNK); return;
      }
    }
    case Q2_OP_BRANCH: case Q2_OP_BRANCH_CMP: case Q2_OP_RETURN: case Q2_OP_RETURN_UNDEF: case Q2_OP_SET_NAME:
      return;
    case Q2_OP_POST_INC:
      if (in->nuse >= 1) {
        unsigned x = R->nd[vnode(I, in->use[0])].cls;
        if (dst >= 0) nd_flow(dst, vnode(I, in->use[0]));
        if (dst2 >= 0 && x) nd_cls(dst2, arith_cls(REG_BIN_ADD, x, K_INT));
      }
      return;
    default: break;
  }
  /* an op the model does not know: results unknown, operands escape */
  if (R->collect) R->unmodeled[in->rop & 255]++;
  for (k = 0; k < in->ndef && k < 2; k++) { int n = vnode(I, in->def[k]); nd_cls(n, K_UNK); nd_site(n, 0); }
  for (k = 0; k < in->nuse; k++) escape_node(vnode(I, in->use[k]));
  for (k = 0; k < in->win_n && in->win_val != NULL; k++) escape_node(vnode(I, in->win_val[k]));
  for (k = 0; k < in->nredef; k++) { int n = vnode(I, in->redef_val[k]); nd_cls(n, K_UNK); nd_site(n, 0); }
}

static void do_instance(int ii) {
  BInst I = R->in[ii];
  if (!I.live) return;
  g_cur_inst = ii;
  const BFn *F = &g_bfn[I.fn];
  int bi, k;
  for (k = 0; k < F->q.nvalues; k++)
    if (F->q.values[k].is_const) nd_cls(I.base + k, cls_of_const(&F->q.values[k]));
  if (CELLS_ON && F->cap_arg != NULL)
    for (k = 0; k < 16 && k < F->g->arg_count; k++) if (F->cap_arg[k]) nd_flow(cell_node(I.fn, 1, k), I.parn[k]);   /* the callers' arguments reach the captured argument's cell */
  for (bi = 0; bi < F->q.nblocks; bi++) {
    const Q2Block *B = &F->q.blocks[bi];
    int s, j, il = F->inloop != NULL && F->inloop[bi] ? 1 : 0;
    if (F->q.rpo_pos != NULL && F->q.rpo_pos[bi] < 0) continue;
    for (j = 0; j < B->ninsns; j++) {
      do_insn(&I, B->first_insn + j, bi, il);
      if (getenv("QJS_AOT_BUNDLE_ROUNDDBG") != NULL && getenv("QJS_AOT_BUNDLE_ROUNDDBG")[0] == '2' && !R->pts && R->phase == 0 && R->nin > 0 && ii == 30) {
        unsigned long long h = 1469598103934665603ull; int q; char rn_[40];
        for (q = 0; q < R->nnd; q++) { h = (h ^ (unsigned)R->nd[q].cls) * 1099511628211ull; h = (h ^ (unsigned)bs_count(&R->nd[q].pts)) * 1099511628211ull; }
        e_rop_name((uint16_t)F->q.insns[B->first_insn + j].rop, rn_, sizeof(rn_));
        fprintf(stderr, "insndbg pc %d %s hash %016llx\n", F->q.insns[B->first_insn + j].pc, rn_, h);
        if (getenv("QJS_AOT_BUNDLE_ROUNDDBG")[1] == '3' && F->q.insns[B->first_insn + j].pc == 1796) {
          const Q2Insn *ii2 = &F->q.insns[B->first_insn + j];
          fprintf(stderr, "insn1796: nuse %d ndef %d nredef %d use0 %d def0 %d def1 %d\n", ii2->nuse, ii2->ndef, ii2->nredef, ii2->nuse > 0 ? (int)ii2->use[0] : -1, ii2->ndef > 0 ? (int)ii2->def[0] : -1, ii2->ndef > 1 ? (int)ii2->def[1] : -1);
          for (q = 0; q < R->nnd; q++) fprintf(stderr, "nodedbg %d cls %u cnt %d\n", q, (unsigned)R->nd[q].cls, bs_count(&R->nd[q].pts));
        }
      }
    }
    for (s = 0; s < B->nsucc; s++) {
      const Q2Block *T;
      if (B->succ_block[s] < 0 || B->succ_block[s] >= F->q.nblocks) continue;
      T = &F->q.blocks[B->succ_block[s]];
      for (j = 0; j < B->nedge_args[s] && j < T->nparams; j++)
        nd_flow(vnode(&I, T->param_val[j]), vnode(&I, B->edge_arg[s][j]));
    }
  }
}

/* functions called from unknown code: the closure escaped */
static void apply_escapes(void) {
  int s, f, changed = 1, guard = 0;
  while (changed && guard++ < 64) {
    changed = 0;
    BS_FOR(s, &R->esc) {
      if (R->pts) {
        for (f = 0; f < R->nfl; f++)
          if (R->fl[f].site == s) {
            g_cur_fn = -1; g_cur_pc = s; g_cur_why = "reachable from an escaped object";
            { int before = bs_count(&R->esc); esc_add(&R->nd[R->fl[f].node].pts); if (bs_count(&R->esc) != before) changed = 1; }
            if ((R->nd[R->fl[f].node].cls & K_UNK) == 0 || !bs_has(&R->nd[R->fl[f].node].pts, 0)) { nd_cls(R->fl[f].node, K_UNK); nd_site(R->fl[f].node, 0); }
          }
        {
          int before = bs_count(&R->esc);
          g_cur_fn = -1; g_cur_pc = s; g_cur_why = "prototype of an escaped object";
          esc_add(&R->st[s].proto);
          if (bs_count(&R->esc) != before) changed = 1;
        }
      }
      if (R->st[s].kind == SK_FUNC && R->st[s].fn >= 0) {
        int ti = inst_get(R->st[s].fn, 0), k;
        const BInst *T = &R->in[ti];
        if (!R->in[ti].live) { R->in[ti].live = 1; R->changed = 1; }
        nd_cls(T->thisn, K_UNK); nd_site(T->thisn, 0);
        for (k = 0; k < 17; k++) { nd_cls(T->parn[k], K_UNK); nd_site(T->parn[k], 0); if (k < 16) { nd_cls(T->extra[k], K_UNK); nd_site(T->extra[k], 0); } }
      }
      if (R->st[s].kind == SK_PROTO || R->st[s].kind == SK_INST) {}
    }
    if (changed) R->changed = 1;
  }
  if (!R->pts) {
    /* per-name: any name may be written by unknown code once anything escaped */
  }
}


/* The closure-variable cells' static side: who creates each function, which locals / arguments inner functions capture, which functions can see variables through
   code the analysis cannot follow (a direct eval, a with, a mapped arguments object), and so which cells are poisoned. */
static void compute_cells(void) {
  int i, k, a, j;
  g_npoison = 0;
  g_nocells = getenv("QJS_AOT_BUNDLE_NOCELLS") != NULL;
  g_diag_noelem = getenv("QJS_AOT_BUNDLE_DIAG_NOELEM") != NULL;
  g_diag_nomerge = getenv("QJS_AOT_BUNDLE_DIAG_NOMERGE") != NULL;
  g_diag_noforin = getenv("QJS_AOT_BUNDLE_DIAG_NOFORIN") != NULL;
  g_diag_noforcopy = getenv("QJS_AOT_BUNDLE_DIAG_NOFORCOPY") != NULL;
  g_math_model = getenv("QJS_AOT_BUNDLE_DIAG_NOMATH") == NULL;
  g_diag_nocoerce = getenv("QJS_AOT_BUNDLE_DIAG_NOCOERCE") != NULL;
  if (getenv("QJS_AOT_BUNDLE_EVENTMAX") != NULL) g_dbg_max = atoi(getenv("QJS_AOT_BUNDLE_EVENTMAX"));
  g_nolost = getenv("QJS_AOT_BUNDLE_DIAG_NOLOST") != NULL || g_diag_noelem;
  g_x_nodst2 = getenv("QJS_AOT_BUNDLE_X_NODST2") != NULL; g_x_noacc = getenv("QJS_AOT_BUNDLE_X_NOACC") != NULL; g_x_nocomp = getenv("QJS_AOT_BUNDLE_X_NOCOMP") != NULL; g_diag_noredef = getenv("QJS_AOT_BUNDLE_DIAG_NOREDEF") != NULL;
  if (g_nocells) { for (i = 0; i < g_bnfuncs; i++) { free(g_bfn[i].cap_loc); free(g_bfn[i].cap_arg); g_bfn[i].cap_loc = g_bfn[i].cap_arg = NULL; g_bfn[i].ncreat = 0; } return; }
  for (i = 0; i < g_bnfuncs; i++) {
    BFn *F = &g_bfn[i];
    free(F->cap_loc); free(F->cap_arg);
    F->cap_loc = F->cap_arg = NULL;
    F->ncreat = 0; F->evalish = 0;
    if (!F->ok) continue;
    F->cap_loc = calloc((size_t)(F->g->var_count > 0 ? F->g->var_count : 1), 1);
    F->cap_arg = calloc((size_t)(F->g->arg_count > 0 ? F->g->arg_count : 1), 1);
  }
  for (i = 0; i < g_bnfuncs; i++) {
    if (!g_bfn[i].ok) continue;
    for (k = 0; k < g_bfn[i].q.ninsns; k++) {
      const Q2Insn *in = &g_bfn[i].q.insns[k];
      int ch;
      if (in->rop == ROP_eval || in->rop == ROP_apply_eval || in->rop == ROP_with_get_ref_undef || in->rop == ROP_special_object) g_bfn[i].evalish = 1;
      if (in->rop != ROP_make_closure) continue;
      for (a = 0; a < in->narg && in->arg[a].kind != 'c'; a++) {}
      if (a >= in->narg || (ch = fn_of_closure(g_bfn[i].g, (int)in->arg[a].val)) < 0) continue;
      for (j = 0; j < g_bfn[ch].ncreat && j < 4 && g_bfn[ch].creat[j] != i; j++) {}
      if (j == g_bfn[ch].ncreat) { if (j < 4) g_bfn[ch].creat[j] = i; g_bfn[ch].ncreat++; }
    }
  }
  /* captured locals / arguments of each creator, from its children's tables (a child with several creators marks each of them) */
  for (i = 0; i < g_bnfuncs; i++) {
    const BFn *C = &g_bfn[i];
    if (!C->ok || C->g->closure_type == NULL) continue;
    for (j = 0; j < C->ncreat && j < 4; j++) {
      BFn *P = &g_bfn[C->creat[j]];
      for (k = 0; k < C->g->closure_count; k++) {
        int t = C->g->closure_type[k], ix = C->g->closure_idx[k];
        if (t == 0 && ix < P->g->var_count) P->cap_loc[ix] = 1;
        else if (t == 1 && ix < P->g->arg_count) { P->cap_arg[ix] = 1; if (ix >= 16) cell_poison(cell_key(C->creat[j], 1, ix)); }
      }
    }
  }
  if (getenv("QJS_AOT_BUNDLE_CELLDBG") != NULL)
    for (i = 0; i < g_bnfuncs; i++) {
      const BFn *F = &g_bfn[i];
      if (!F->ok) continue;
      fprintf(stderr, "cells: fn %d %s args=%d vars=%d creators=%d(%d) evalish=%d closure[", i, fn_name(i), F->g->arg_count, F->g->var_count, F->ncreat, F->ncreat > 0 ? F->creat[0] : -1, F->evalish);
      for (k = 0; k < F->g->closure_count; k++) fprintf(stderr, " %d:%d", F->g->closure_type[k], F->g->closure_idx[k]);
      fprintf(stderr, " ] captured locals:");
      for (k = 0; k < F->g->var_count; k++) if (F->cap_loc[k]) fprintf(stderr, " %d", k);
      fprintf(stderr, " args:");
      for (k = 0; k < F->g->arg_count; k++) if (F->cap_arg[k]) fprintf(stderr, " %d", k);
      fprintf(stderr, "\n");
      for (k = 0; k < F->q.ninsns; k++) {
        const Q2Insn *in = &F->q.insns[k];
        if (getenv("QJS_AOT_BUNDLE_CELLDBG")[0] == '2') { char rn_[40]; e_rop_name((uint16_t)in->rop, rn_, sizeof(rn_)); fprintf(stderr, "cells:    fn %d pc %d %s narg=%d a0=%d a1=%d\n", i, in->pc, rn_, in->narg, in->narg > 0 ? (int)in->arg[0].val : -1, in->narg > 1 ? (int)in->arg[1].val : -1); }
                if (in->rop == ROP_get_local || in->rop == ROP_put_local || in->rop == ROP_put_local_xfer) fprintf(stderr, "cells:    fn %d pc %d local op rop=%d slot=%d\n", i, in->pc, (int)in->rop, in->narg > 1 && in->rop == ROP_get_local ? (int)in->arg[1].val : (int)in->arg[0].val);
        else if (in->rop >= ROP_get_local_s0 && in->rop <= ROP_get_local_s3) fprintf(stderr, "cells:    fn %d pc %d get_local_s slot=%d\n", i, in->pc, in->rop - ROP_get_local_s0);
        else if (in->rop >= ROP_put_local_s0 && in->rop <= ROP_put_local_s3) fprintf(stderr, "cells:    fn %d pc %d put_local_s slot=%d\n", i, in->pc, in->rop - ROP_put_local_s0);
        else if (in->rop == ROP_get_var_ref) fprintf(stderr, "cells:    fn %d pc %d get_var_ref slot=%d\n", i, in->pc, in->narg > 1 ? (int)in->arg[1].val : -1);
        else if (in->rop == ROP_get_arg) fprintf(stderr, "cells:    fn %d pc %d get_arg slot=%d\n", i, in->pc, in->narg > 1 ? (int)in->arg[1].val : -1);
      }
    }
  if (getenv("QJS_AOT_BUNDLE_DIAG_CELL_NOPOISON") != NULL) return;   /* vkit knock-out (unsound): eval / with do not poison the cells they can write */
  /* a function with a direct eval / with / mapped arguments: all its own variables, and every variable its closure table names, are poisoned */
  for (i = 0; i < g_bnfuncs; i++) {
    BFn *F = &g_bfn[i];
    if (!F->ok || !F->evalish) continue;
    for (k = 0; k < F->g->var_count; k++) { F->cap_loc[k] = 1; cell_poison(cell_key(i, 0, k)); }
    for (k = 0; k < F->g->arg_count; k++) { F->cap_arg[k] = 1; cell_poison(cell_key(i, 1, k)); }
    if (F->g->closure_type != NULL)
      for (k = 0; k < F->g->closure_count; k++) {
        int cf, ia, ix;
        if (vref_cell(i, k, 0, &cf, &ia, &ix)) { if (ia ? ix < g_bfn[cf].g->arg_count : ix < g_bfn[cf].g->var_count) { (ia ? g_bfn[cf].cap_arg : g_bfn[cf].cap_loc)[ix] = 1; cell_poison(cell_key(cf, ia, ix)); } }
      }
  }
}


/* The profile as a soundness oracle, after the fixpoint: (1) a function the profiled run executed that no instance reaches (a flow the model lost: an implicit call, a callback through a native
   it does not model...) is treated as escaped, i.e. live with unknown this and arguments; (2) in the sealing rounds an executed instruction with an empty receiver gets an unknown one
   (seal_recv). Both only add unknown, never remove information; the count of case (1) is still reported ("stranded ... EXECUTED"), so the lost flows get fixed rather than hidden.
   QJS_AOT_BUNDLE_NOORACLE=1 turns this off (the diagnostics then show the raw gaps). */
static int g_oracle_fixed;
static void oracle_passes(void) {
  int pass, i, j, k, round;
  if (getenv("QJS_AOT_BUNDLE_NOORACLE") != NULL || g_nolost) return;
  for (pass = 0; pass < 8; pass++) {
    int fixed = 0;
    for (i = 0; i < g_bnfuncs; i++) {
      int live = 0, ti;
      const BInst *T;
      if (!g_bfn[i].ok || !g_made[i]) continue;
      for (j = 0; j < R->nin; j++) if (R->in[j].fn == i && R->in[j].live) live = 1;
      if (live || qjs_aot_prof_entries(g_bfn[i].g->digest) == 0) continue;
      ti = inst_get(i, 0);
      T = &R->in[ti];
      R->in[ti].live = 1;
      nd_cls(T->thisn, K_UNK); nd_site(T->thisn, 0);
      for (k = 0; k < 17; k++) { nd_cls(T->parn[k], K_UNK); nd_site(T->parn[k], 0); if (k < 16) { nd_cls(T->extra[k], K_UNK); nd_site(T->extra[k], 0); } }
      fixed++;
      R->oracle_fixed++;
      if (strlen(R->oracle_names) < sizeof(R->oracle_names) - 70) { char nb_[80]; snprintf(nb_, sizeof(nb_), " %s(%llu)", fn_name(i), qjs_aot_prof_entries(g_bfn[i].g->digest)); strcat(R->oracle_names, nb_); }
    }
    g_oracle_fixed += fixed;
    R->seal = 1; R->nseal = 0;
    for (round = 0; round < 200; round++) {
      R->changed = 0;
      for (i = 0; i < R->nin; i++) do_instance(i);
      apply_escapes();
      if (!R->changed) break;
    }
    R->seal = 0;
    if (!fixed && !R->nseal) break;
  }
}

static void run_fixpoint(BRun *run) {
  int round, i;
  R = run;
  R->sink_node = nd_new();
  R->varref_node = nd_new();
  R->tmp_in = nd_new();
  R->tmp_out = nd_new();
  R->globals = malloc(sizeof(int) * BN_NAMES);
  for (i = 0; i < BN_NAMES; i++) R->globals[i] = -1;
  R->st = calloc(BN_MAXSITES, sizeof(BSite));
  R->fhcap = 0;
  R->nst = 0;
  {
    int e = st_new(SK_EXT, -1, -1, -1, 0, "EXT");
    (void)e;
    st_new(SK_OBJ, -2, -1, -1, 0, "Object (builtin constructor)");
    st_new(SK_PROTO, -2, -1, -1, 0, "Object.prototype");
    R->st[S_OBJPROTO].owner = -1;
    nd_site(fnode(S_OBJCTOR, bn_name("prototype"), 1), S_OBJPROTO);
    nd_site(fnode(S_OBJCTOR, bn_name("defineProperty"), 1), builtin_site("Object", "defineProperty"));
    nd_site(fnode(S_OBJCTOR, bn_name("create"), 1), builtin_site("Object", "create"));
    nd_site(gnode(bn_name("Object")), S_OBJCTOR);
    if (g_math_model) {
      /* Math: a modelled builtin object. Its methods are builtin sites; a program store into it is an ordinary write to this site's field, and an escaped Math forces its fields unknown */
      static const char *const mf[] = {"abs", "acos", "asin", "atan", "atan2", "ceil", "cos", "exp", "floor", "hypot", "imul", "log", "log10", "log2", "max", "min", "pow", "random", "round", "sign", "sin", "sqrt", "tan", "trunc", NULL};
      static const char *const mc[] = {"E", "PI", "LN2", "LN10", "LOG2E", "LOG10E", "SQRT1_2", "SQRT2", NULL};
      int ms = st_new(SK_OBJ, -6, -1, -1, 0, "Math (builtin object)"), j;   /* a distinct key: st_new merges sites with the same kind and origin */
      for (j = 0; mf[j]; j++) nd_site(fnode(ms, bn_name(mf[j]), 1), builtin_site("Math", mf[j]));
      for (j = 0; mc[j]; j++) nd_cls(fnode(ms, bn_name(mc[j]), 1), K_DBL);
      nd_site(gnode(bn_name("Math")), ms);
    }
  }
  for (i = 0; i < g_bnfuncs; i++)
    if (g_bfn[i].ok) {
      int ti = inst_get(i, 0);
      if (!g_made[i]) R->in[ti].live = 1; /* the engine runs it: the script, or a function no closure creation names */
      if (g_made[i] && getenv("QJS_AOT_BUNDLE_DIAG_NET") != NULL) {   /* DIAGNOSTIC: the 'safety net' of the harness design: every made function is live with unknown this/arguments */
        const BInst *T = &R->in[ti];
        int k;
        R->in[ti].live = 1;
        nd_cls(T->thisn, K_UNK); nd_site(T->thisn, 0);
        for (k = 0; k < 17; k++) { nd_cls(T->parn[k], K_UNK); nd_site(T->parn[k], 0); if (k < 16) { nd_cls(T->extra[k], K_UNK); nd_site(T->extra[k], 0); } }
      }
    }
  for (R->phase = 0; R->phase <= 1; R->phase++) {
    for (round = 0; round < 200; round++) {
      R->changed = 0;
      for (i = 0; i < R->nin; i++) {
        do_instance(i);
        if (getenv("QJS_AOT_BUNDLE_ROUNDDBG") != NULL && !R->pts) {   /* determinism diagnostic: the per-name layer's state after each instance */
          unsigned long long h = 1469598103934665603ull; int q;
          for (q = 0; q < R->nnd; q++) { h = (h ^ (unsigned)R->nd[q].cls) * 1099511628211ull; h = (h ^ (unsigned)bs_count(&R->nd[q].pts)) * 1099511628211ull; }
          fprintf(stderr, "rounddbg phase %d round %d inst %d fn %d: nnd %d nfl %d hash %016llx changed %d\n", R->phase, round, i, R->in[i].fn, R->nnd, R->nfl, h, R->changed);
        }
      }
      apply_escapes();
      if (!R->changed) break;
    }
    R->rounds += round;
  }
  R->phase = 1;
  oracle_passes();
}

/* ---- static hotness ---------------------------------------------------------------- */
static void mark_loops(BFn *F) {
  int b, s;
  Q2Func *q = &F->q;
  F->inloop = calloc((size_t)(q->nblocks > 0 ? q->nblocks : 1), 1);
  for (b = 0; b < q->nblocks; b++) {
    if (q->rpo_pos == NULL || q->rpo_pos[b] < 0) continue;
    for (s = 0; s < q->blocks[b].nsucc; s++) {
      int h = q->blocks[b].succ_block[s];
      if (h < 0 || h >= q->nblocks || q->rpo_pos[h] < 0 || q->rpo_pos[h] > q->rpo_pos[b]) continue;
      /* back edge b -> h: the natural loop is h and everything that reaches b without passing h */
      {
        int *stack = malloc(sizeof(int) * (size_t)q->nblocks), sp = 0;
        uint8_t *in = calloc((size_t)q->nblocks, 1);
        in[h] = 1;
        F->has_loop = 1;
        if (!in[b]) { in[b] = 1; stack[sp++] = b; }
        while (sp > 0) {
          int c = stack[--sp], p;
          for (p = 0; p < q->blocks[c].npreds; p++) {
            int pb = q->blocks[c].preds[p];
            if (pb >= 0 && pb < q->nblocks && !in[pb]) { in[pb] = 1; stack[sp++] = pb; }
          }
        }
        {
          int x;
          for (x = 0; x < q->nblocks; x++) if (in[x]) F->inloop[x] = 1;
        }
        free(stack);
        free(in);
      }
    }
  }
}

static int blk_of_pc(const Q2Func *q, int pc) {
  int b;
  for (b = 0; b < q->nblocks; b++) if (q->blocks[b].start_pc <= pc && pc < q->blocks[b].end_pc) return b;
  return -1;
}

static void mark_uncond(BFn *F) {
  const Q2Func *q = &F->q;
  int b, nb = q->nblocks;
  uint8_t *seen = malloc((size_t)(nb > 0 ? nb : 1));
  int *stack = malloc(sizeof(int) * (size_t)(nb > 0 ? nb : 1));
  F->uncond = calloc((size_t)(nb > 0 ? nb : 1), 1);
  for (b = 0; b < nb; b++) {
    int sp = 0, reached_exit = 0, c;
    if (q->rpo_pos != NULL && q->rpo_pos[b] < 0) continue;
    if (b == q->entry_block) { F->uncond[b] = 1; continue; }
    memset(seen, 0, (size_t)nb);
    seen[b] = 1;
    if (q->entry_block >= 0 && q->entry_block < nb) { seen[q->entry_block] = 1; stack[sp++] = q->entry_block; }
    while (sp > 0 && !reached_exit) {
      int cb = stack[--sp];
      const Q2Block *B = &q->blocks[cb];
      if (B->nsucc == 0) { reached_exit = 1; break; }
      for (c = 0; c < B->nsucc; c++) {
        int sb = B->succ_block[c];
        if (sb < 0 || sb >= nb) { reached_exit = 1; break; }
        if (!seen[sb]) { seen[sb] = 1; stack[sp++] = sb; }
      }
    }
    F->uncond[b] = reached_exit ? 0 : 1;
  }
  free(seen);
  free(stack);
}

/* ---- runs ----------------------------------------------------------------------------- */
static int assigned_has(int afn, int apc, int ctx) {
  int i;
  for (i = 0; i < g_nassigned; i++)
    if (g_assigned[i].afn == afn && g_assigned[i].apc == apc && g_assigned[i].ctx == ctx) return 1;
  return 0;
}
static int assigned_add(int afn, int apc, int ctx) {
  if (assigned_has(afn, apc, ctx)) return 0;
  if (g_nassigned >= g_assigned_cap) {
    g_assigned_cap = g_assigned_cap ? g_assigned_cap * 2 : 32;
    g_assigned = realloc(g_assigned, (size_t)g_assigned_cap * sizeof(BKey));
  }
  g_assigned[g_nassigned].afn = afn; g_assigned[g_nassigned].apc = apc; g_assigned[g_nassigned].ctx = ctx;
  g_nassigned++;
  return 1;
}

static void run_free(BRun *r) {
  free(r->nd); free(r->st); free(r->fl); free(r->fhash); free(r->in); free(r->globals); free(r->ed); free(r->cs); free(r->acc); free(r->ev); free(r->rcv); free(r->rt); free(r->cellk); free(r->celln);
  memset(r, 0, sizeof(*r));
}

static void reset_effects(void) {
  int i;
  for (i = 0; i < g_bnfuncs; i++) {
    BFn *F = &g_bfn[i];
    memset(F->wn, 0, sizeof(F->wn));
    F->welem = F->wglobal = F->alloc = F->unkcall = F->thrw = F->wvarref = 0;
  }
}

static void run_collect(BRun *r) {
  int i;
  R = r;
  r->collect = 1;
  reset_effects();
  r->changed = 0;
  for (i = 0; i < r->nin; i++) do_instance(i);
  r->collect = 0;
}

/* the facts each run keeps */
static void cls_str(unsigned c, char *out, size_t n) {
  static const struct { unsigned k; const char *s; } tab[] = {
    {K_UNDEF, "undef"}, {K_NULL, "null"}, {K_BOOL, "bool"}, {K_INT, "int"}, {K_INTOV, "ovf"}, {K_DBL, "dbl"},
    {K_NUM, "num"}, {K_STR, "str"}, {K_OBJ, "obj"}, {K_UNK, "UNK"}};
  size_t o = 0;
  unsigned i;
  out[0] = 0;
  if (c == 0) { snprintf(out, n, "none"); return; }
  for (i = 0; i < sizeof(tab) / sizeof(tab[0]); i++)
    if (c & tab[i].k) o += (size_t)snprintf(out + o, o < n ? n - o : 0, "%s%s", o ? "|" : "", tab[i].s);
}

static int cls_precise(unsigned c) {
  if (c == 0 || (c & K_UNK)) return 0;
  {
    int groups = 0;
    if (c & K_UNDEF) groups++;
    if (c & K_NULL) groups++;
    if (c & K_BOOL) groups++;
    if (c & (K_INT | K_INTOV)) groups++;
    if (c & (K_DBL | K_NUM)) groups++;
    if (c & K_STR) groups++;
    if (c & K_OBJ) groups++;
    return groups == 1;
  }
}
static int cls_numeric_only(unsigned c) { return c != 0 && (c & ~(K_INT | K_INTOV | K_DBL | K_NUM)) == 0; }

static char **g_fnlabel;

/* Anonymous functions are named by where the bundle stores them: `X.prototype.m = function`, `X.m = function`, a method of a class. */
static void label_functions(void) {
  int i, k;
  g_fnlabel = calloc((size_t)(g_bnfuncs > 0 ? g_bnfuncs : 1), sizeof(char *));
  for (i = 0; i < g_bnfuncs; i++) {
    const BFn *F = &g_bfn[i];
    BInst fake;
    if (!F->ok) continue;
    memset(&fake, 0, sizeof(fake));
    fake.fn = i;
    for (k = 0; k < F->q.ninsns; k++) {
      const Q2Insn *in = &F->q.insns[k];
      const Q2Insn *m;
      char nm[48], rn[48];
      int ch = -1;
      if (in->rop != ROP_put_field && in->rop != ROP_define_field && in->rop != ROP_define_method) continue;
      if (in->rop == ROP_define_method) {
        if (in->narg < 3 || in->arg[2].kind != 'r') continue;
        m = bn_def(&fake, in->argval[2]);
      } else {
        if (in->nuse < 2) continue;
        m = bn_def(&fake, in->use[1]);
      }
      if (m == NULL || m->rop != ROP_make_closure) continue;
      {
        int ci = -1, a;
        for (a = 0; a < m->narg; a++) if (m->arg[a].kind == 'c') { ci = (int)m->arg[a].val; break; }
        ch = fn_of_closure(F->g, ci);
      }
      if (ch < 0 || g_fnlabel[ch] != NULL || !bn_atom(in, NULL, nm, sizeof(nm), 0)) continue;
      rn[0] = 0;
      if (in->rop != ROP_define_method && in->nuse >= 1) {
        const Q2Insn *r = bn_def(&fake, in->use[0]);
        if (r != NULL && r->rop == ROP_get_global && bn_atom(r, NULL, rn, sizeof(rn), 0)) {
          strncat(rn, ".", sizeof(rn) - strlen(rn) - 1);
        } else if (r != NULL && (r->rop == ROP_get_field || r->rop == ROP_get_field_own) && r->nuse >= 1) {
          char pn[48];
          const Q2Insn *g2 = bn_def(&fake, r->use[0]);
          if (bn_atom(r, NULL, pn, sizeof(pn), 0) && g2 != NULL && g2->rop == ROP_get_global && bn_atom(g2, NULL, rn, sizeof(rn), 0)) {
            strncat(rn, ".", sizeof(rn) - strlen(rn) - 1);
            strncat(rn, pn, sizeof(rn) - strlen(rn) - 1);
            strncat(rn, ".", sizeof(rn) - strlen(rn) - 1);
          }
        }
      }
      g_fnlabel[ch] = malloc(160);
      snprintf(g_fnlabel[ch], 160, "%s%s", rn, nm);
    }
  }
}

static const char *fn_name(int fn) {
  static char buf[8][160];
  static int rot;
  char *b = buf[rot++ & 7];
  if (fn < 0 || fn >= g_bnfuncs) return "?";
  if (g_bfn[fn].g->name[0] && strcmp(g_bfn[fn].g->name, "<null>") != 0) snprintf(b, 160, "%s", g_bfn[fn].g->name);
  else if (g_fnlabel != NULL && g_fnlabel[fn] != NULL) snprintf(b, 160, "%s", g_fnlabel[fn]);
  else snprintf(b, 160, "<anon#%d>", fn);
  return b;
}

static void set_str(const BRun *r, const BSet *set, char *out, size_t n, int maxn) {
  int s, c = 0;
  size_t o = 0;
  out[0] = 0;
  for (s = 0; s < BN_MAXSITES; s++) {
    if (!bs_has(set, s)) continue;
    if (c == maxn) { o += (size_t)snprintf(out + o, o < n ? n - o : 0, ", +%d", bs_count(set) - maxn); break; }
    o += (size_t)snprintf(out + o, o < n ? n - o : 0, "%s%s", c ? ", " : "", s < r->nst ? r->st[s].label : "?");
    c++;
  }
}

static int fl_find_ro_name(BRun *r, int name) {
  BRun *keep = R;
  int e;
  R = r;
  e = fl_find(-1, name, 0);
  R = keep;
  return e;
}

static void run_one(BRun *r, int pts) {
  R = r;
  memset(r, 0, sizeof(*r));
  r->pts = pts;
  run_fixpoint(r);
  run_collect(r);
}

static int count_if_changed(void) { return 0; }

/* ---- hotness and effects ------------------------------------------------------------ */
static void compute_hotness(const BRun *P) {
  int i, e, changed = 1, guard = 0;
  for (i = 0; i < g_bnfuncs; i++) g_bfn[i].hot = 0;
  for (e = 0; e < P->ned; e++) if (P->ed[e].inloop && P->ed[e].tfn >= 0) g_bfn[P->ed[e].tfn].hot = 2;
  while (changed && guard++ < 64) {
    changed = 0;
    for (e = 0; e < P->ned; e++) {
      const BEdge *E = &P->ed[e];
      int src_hot = g_bfn[E->cfn].hot > 0 || g_bfn[E->cfn].has_loop;
      if (src_hot && g_bfn[E->tfn].hot == 0) { g_bfn[E->tfn].hot = 1; changed = 1; }
    }
  }
}

static void compute_effects(const BRun *P) {
  int i, e, changed = 1, guard = 0, w;
  for (i = 0; i < g_bnfuncs; i++) {
    BFn *F = &g_bfn[i];
    memcpy(F->twn, F->wn, sizeof(F->wn));
    F->twelem = F->welem; F->twglobal = F->wglobal; F->talloc = F->alloc; F->tunkcall = F->unkcall;
    F->tthrw = F->thrw; F->twvarref = F->wvarref;
  }
  while (changed && guard++ < 64) {
    changed = 0;
    for (e = 0; e < P->ned; e++) {
      BFn *C = &g_bfn[P->ed[e].cfn], *T = &g_bfn[P->ed[e].tfn];
      for (w = 0; w < BN_NAMES / 64; w++)
        if ((C->twn[w] | T->twn[w]) != C->twn[w]) { C->twn[w] |= T->twn[w]; changed = 1; }
      if (T->twelem && !C->twelem) { C->twelem = 1; changed = 1; }
      if (T->twglobal && !C->twglobal) { C->twglobal = 1; changed = 1; }
      if (T->talloc && !C->talloc) { C->talloc = 1; changed = 1; }
      if (T->tunkcall && !C->tunkcall) { C->tunkcall = 1; changed = 1; }
      if (T->tthrw && !C->tthrw) { C->tthrw = 1; changed = 1; }
      if (T->twvarref && !C->twvarref) { C->twvarref = 1; changed = 1; }
    }
  }
}

static void print_wnames(FILE *f, const uint64_t *wn, int maxn) {
  int n = 0, i, total = 0;
  for (i = 0; i < BN_NAMES; i++) if ((wn[i >> 6] >> (i & 63)) & 1) total++;
  for (i = 0; i < g_bn_nnames; i++)
    if ((wn[i >> 6] >> (i & 63)) & 1) {
      if (n < maxn) fprintf(f, "%s%s", n ? "," : "", g_bn_names[i]);
      n++;
    }
  if (n > maxn) fprintf(f, ",+%d", n - maxn);
  if (n == 0) fprintf(f, "-");
  (void)total;
}

static const char *kind_str(const BSite *s) { return sk_names[s->kind]; }

static void flags_str(int fl, char *out, size_t n) {
  size_t o = 0;
  out[0] = 0;
  if (fl & SF_STORED) o += (size_t)snprintf(out + o, n - o, "stored ");
  if (fl & SF_GLOBAL) o += (size_t)snprintf(out + o, n - o, "global ");
  if (fl & SF_CLOSURE) o += (size_t)snprintf(out + o, n - o, "closure ");
  if (fl & SF_PASSED) o += (size_t)snprintf(out + o, n - o, "passed ");
  if (fl & SF_UNKARG) o += (size_t)snprintf(out + o, n - o, "UNKNOWN-ARG ");
  if (fl & SF_RET) o += (size_t)snprintf(out + o, n - o, "returned ");
  if (fl & SF_THIS) o += (size_t)snprintf(out + o, n - o, "this ");
  if (fl & SF_RECV) o += (size_t)snprintf(out + o, n - o, "recv ");
  if (o == 0) snprintf(out, n, "-");
}

static int fcmp_order(const void *a, const void *b) {
  const BField *x = a, *y = b;
  return x->order - y->order;
}


static char g_late_buf[4000];
static int g_late_sites;

typedef struct BAdd { int name, fn, pc, uncond; int nalt, alt[8]; } BAdd; /* alt: the other stores that add the same field on a joined path (the two arms of an if/else) */

static int ev_cmp_pc(const void *a, const void *b) {
  const BEv *x = a, *y = b;
  if (x->pc != y->pc) return x->pc - y->pc;
  return x->seq - y->seq;
}


/* The constructor adds a field in each arm of an if/else (or several): the class has ONE final shape when every path from the entry to an
   exit of the function adds the same fields in the same order, each exactly once. A dataflow over the blocks carries the sequence of
   names added so far; where two paths meet their sequences must be equal, and every exit (a return, a throw, a branch out of the function)
   must end in the same sequence. A store of a name already in the sequence (an add twice, or an overwrite), a loop that adds, an add in a
   block the walk never reaches, a catch, and stores of a different kind (define_field vs put_field) all make the join refuse (-> the old
   rule, per site). On success `final` is the sequence and `alt` collects, per name, every store that may be the add. */
static int g_nojoin = -1;
static int rop_at_pc(const Q2Func *q, int pc) {
  int k;
  for (k = 0; k < q->ninsns; k++) if (q->insns[k].pc == pc) return (int)q->insns[k].rop;
  return -1;
}
static int join_adds(const BFn *F, const BEv *ev, int m, int *final, int *nfinal) {
  const Q2Func *q = &F->q;
  int nb = q->nblocks, i, k, ok = 1, nexit = 0, *seqlen, *seqs, *stack, sp = 0, visited_ev = 0;
  int exitseq[64], exitlen = -1;
  uint8_t *has;
  if (g_nojoin < 0) g_nojoin = getenv("QJS_AOT_BUNDLE_NOJOIN") != NULL;
  if (g_nojoin || m <= 0 || nb <= 0 || q->entry_block < 0 || q->entry_block >= nb) return 0;
  for (k = 0; k < q->ninsns; k++) if (q->insns[k].rop == ROP_catch_enter) return 0;
  for (i = 0; i < m; i++) if (ev[i].callee >= 0 || ev[i].blk < 0 || ev[i].blk >= nb) return 0;
  seqs = malloc(sizeof(int) * 64 * (size_t)nb);
  seqlen = malloc(sizeof(int) * (size_t)nb);
  stack = malloc(sizeof(int) * (size_t)(nb * 4 + 4));
  has = calloc((size_t)nb, 1);
  has[q->entry_block] = 1; seqlen[q->entry_block] = 0; stack[sp++] = q->entry_block;
  (void)nexit;
  while (sp > 0 && ok) {
    int b = stack[--sp], len = seqlen[b], c;
    int cur[64];
    const Q2Block *B = &q->blocks[b];
    memcpy(cur, seqs + 64 * (size_t)b, sizeof(int) * (size_t)len);
    for (i = 0; i < m && ok; i++) {
      int d;
      if (ev[i].blk != b) continue;
      visited_ev++;
      for (d = 0; d < len; d++) if (cur[d] == ev[i].name) ok = 0; /* added twice on one path */
      if (len >= 64) ok = 0;
      if (ok) cur[len++] = ev[i].name;
    }
    if (!ok) break;
    /* (events of one block are in pc order: ev is sorted by pc) */
    for (c = 0; c < (B->nsucc > 0 ? B->nsucc : 1) && ok; c++) {
      int sb = B->nsucc > 0 ? B->succ_block[c] : -1;
      if (sb < 0 || sb >= nb) { /* an exit */
        if (exitlen < 0) { exitlen = len; memcpy(exitseq, cur, sizeof(int) * (size_t)len); }
        else if (exitlen != len || memcmp(exitseq, cur, sizeof(int) * (size_t)len) != 0) ok = 0;
      } else if (!has[sb]) {
        has[sb] = 1; seqlen[sb] = len; memcpy(seqs + 64 * (size_t)sb, cur, sizeof(int) * (size_t)len); stack[sp++] = sb;
      } else if (seqlen[sb] != len || memcmp(seqs + 64 * (size_t)sb, cur, sizeof(int) * (size_t)len) != 0) ok = 0;
    }
  }
  /* an add in a block no path from the entry reaches, or no exit seen */
  if (ok && (visited_ev != m || exitlen < 0)) ok = 0;
  if (ok) { /* the stores of one name must agree in kind */
    for (i = 0; i < m && ok; i++) {
      int r1 = rop_at_pc(q, ev[i].pc), r2;
      for (k = 0; k < i && ok; k++) {
        if (ev[k].name != ev[i].name) continue;
        r2 = rop_at_pc(q, ev[k].pc);
        if ((r1 == ROP_define_field) != (r2 == ROP_define_field)) ok = 0;
      }
      if (r1 != ROP_put_field && r1 != ROP_put_field_xfer && r1 != ROP_define_field) ok = 0;
    }
  }
  if (ok) { memcpy(final, exitseq, sizeof(int) * (size_t)exitlen); *nfinal = exitlen; }
  free(seqs); free(seqlen); free(stack); free(has);
  return ok;
}

/* The fields added to `site`, in program order: the writes of the instance that makes it, then, at each call that carries the site as
   `this` (a super constructor, a helper), the writes of that callee. */
static void walk_site(const BRun *P, int site, int inst, int cond, BAdd *out, int *n, int depth) {
  BEv *tmp;
  int i, m = 0;
  if (inst < 0 || inst >= P->nin || depth > 6) return;
  tmp = malloc(sizeof(BEv) * (size_t)(P->nev > 0 ? P->nev : 1));
  for (i = 0; i < P->nev; i++)
    if (P->ev[i].inst == inst && ((P->ev[i].site == site && P->ev[i].callee < 0) || (P->ev[i].callee >= 0 && bs_has(&P->ev[i].this_set, site))))
      tmp[m++] = P->ev[i];
  qsort(tmp, (size_t)m, sizeof(BEv), ev_cmp_pc);
  if (!cond && m > 0) {
    const BFn *F = &g_bfn[P->in[inst].fn];
    int any_cond = 0, final[64], nfinal = 0;
    for (i = 0; i < m; i++) {
      int blk = tmp[i].callee >= 0 ? blk_of_pc(&F->q, tmp[i].pc) : tmp[i].blk;
      if (!(F->uncond != NULL && blk >= 0 && F->uncond[blk])) any_cond = 1;
    }
    if (any_cond && join_adds(F, tmp, m, final, &nfinal)) {
      int fresh = 1, f;
      for (f = 0; f < nfinal; f++) { int j; for (j = 0; j < *n; j++) if (out[j].name == final[f]) fresh = 0; }
      if (fresh && *n + nfinal <= 64) {
        for (f = 0; f < nfinal; f++) {
          BAdd *a = &out[*n];
          int first = 1;
          memset(a, 0, sizeof(*a));
          a->name = final[f]; a->uncond = 1; a->fn = P->in[inst].fn;
          for (i = 0; i < m; i++) {
            if (tmp[i].name != final[f]) continue;
            if (first) { a->pc = tmp[i].pc; first = 0; }
            else if (a->nalt < 8) a->alt[a->nalt++] = tmp[i].pc;
            else fresh = 0;
          }
          (*n)++;
        }
        if (fresh) { free(tmp); return; }
        *n -= nfinal; /* too many stores of one name: the old rule */
      }
    }
  }
  for (i = 0; i < m; i++) {
    const BFn *F = &g_bfn[P->in[inst].fn];
    int blk = tmp[i].callee >= 0 ? blk_of_pc(&F->q, tmp[i].pc) : tmp[i].blk;
    int un = !cond && F->uncond != NULL && blk >= 0 && F->uncond[blk];
    if (tmp[i].callee >= 0) {
      walk_site(P, site, tmp[i].callee, !un, out, n, depth + 1);
    } else {
      int j;
      for (j = 0; j < *n; j++) if (out[j].name == tmp[i].name) break;
      if (j == *n && *n < 64) { memset(&out[*n], 0, sizeof(out[*n])); out[*n].name = tmp[i].name; out[*n].fn = P->in[inst].fn; out[*n].pc = tmp[i].pc; out[*n].uncond = un; (*n)++; }
    }
  }
  free(tmp);
}

static void print_protos(FILE *f, const BRun *P, int s) {
  BSet seen, lvl, next;
  int depth, p, first = 1;
  char b2[512];
  memset(&seen, 0, sizeof(seen));
  lvl = P->st[s].proto;
  fprintf(f, "    protos:");
  for (depth = 0; depth < 8; depth++) {
    int any = 0;
    memset(&next, 0, sizeof(next));
    BS_FOR(p, &lvl) if (!bs_has(&seen, p)) { any = 1; bs_add(&seen, p); bs_or(&next, &P->st[p].proto); }
    if (!any) break;
    set_str(P, &lvl, b2, sizeof(b2), 4);
    fprintf(f, "%s [%s]", first ? "" : " ->", b2);
    first = 0;
    lvl = next;
  }
  if (P->st[s].kind != SK_EXT && s != 2) fprintf(f, "%s Object.prototype", first ? "" : " ->");
  fprintf(f, "\n");
}


/* the facts each layer keeps, in a block at the top of the report */
static void summarize(FILE *f, BRun *A, BRun *P) {
  int i, mono_p = 0, poly_p = 0, unk_p = 0, bi_p = 0, mono_a = 0, poly_a = 0, unk_a = 0;
  int nslots = 0, a_prec = 0, p_prec = 0, a_int = 0, p_int = 0, a_num = 0, p_num = 0, a_obj1 = 0, p_obj1 = 0, objslots = 0, init_only = 0;
  int nsites = 0, final_shape = 0, loopfns = 0, hotfns = 0, hot2 = 0, nok = 0;
  for (i = 0; i < P->ncs; i++) {
    const BCall *c = &P->cs[i], *ca = NULL;
    int j;
    for (j = 0; j < A->ncs; j++) if (A->cs[j].fn == c->fn && A->cs[j].pc == c->pc) ca = &A->cs[j];
    if (c->unknown) unk_p++; else if (c->builtin && c->nt == 0) bi_p++; else if (c->nt == 1) mono_p++; else poly_p++;
    if (ca == NULL || ca->unknown) unk_a++; else if (ca->nt == 1) mono_a++; else poly_a++;
  }
  for (i = 0; i < P->nfl; i++) {
    const BField *pf = &P->fl[i];
    int af, kind;
    unsigned pc, ac;
    if (pf->site <= 0 || pf->name == bn_name("[]") || pf->nwrites == 0) continue;
    kind = P->st[pf->site].kind;
    if (kind == SK_BUILTIN || kind == SK_EXT || kind == SK_NATIVE) continue;
    af = fl_find_ro_name(A, pf->name);
    pc = P->nd[pf->node].cls;
    ac = af >= 0 ? A->nd[A->fl[af].node].cls : K_UNK;
    nslots++;
    if (cls_precise(ac)) a_prec++;
    if (cls_precise(pc)) p_prec++;
    if (ac == K_INT) a_int++;
    if (pc == K_INT) p_int++;
    if (cls_numeric_only(ac)) a_num++;
    if (cls_numeric_only(pc)) p_num++;
    if ((pc & K_OBJ) && !(pc & K_UNK)) { objslots++; if (bs_count(&P->nd[pf->node].pts) == 1) p_obj1++; }
    if (af >= 0 && (A->nd[A->fl[af].node].cls & K_OBJ) && !(ac & K_UNK) && bs_count(&A->nd[A->fl[af].node].pts) == 1) a_obj1++;
  }
  for (i = 0; i < g_bnfuncs; i++) if (g_bfn[i].ok) { nok++; if (g_bfn[i].has_loop) loopfns++; if (g_bfn[i].hot) hotfns++; if (g_bfn[i].hot == 2) hot2++; }
  for (i = 1; i < P->nst; i++) if (P->st[i].kind == SK_INST || P->st[i].kind == SK_OBJ || P->st[i].kind == SK_ARR) { nsites++; if (!(P->st[i].flags & SF_UNKARG) && !bs_has(&P->esc, i)) final_shape++; }
  (void)init_only;
  fprintf(f, "== summary ==\n");
  fprintf(f, "functions: %d analysed; %d contain a loop; static hotness: %d called inside a loop, %d reachable from hot code or loops\n", nok, loopfns, hot2, hotfns);
  fprintf(f, "abstract heap: %d sites (%d instances/objects/arrays, %d of which never reach unknown code), %d instances of functions (clones per call site for small callees)\n", P->nst - 1, nsites, final_shape, P->nin);
  fprintf(f, "call sites: %d\n", P->ncs);
  fprintf(f, "    exactly one target:  per-name %4d   points-to %4d\n", mono_a, mono_p);
  fprintf(f, "    several targets:     per-name %4d   points-to %4d\n", poly_a, poly_p);
  fprintf(f, "    unresolved/unknown:  per-name %4d   points-to %4d   (builtins resolved: %d)\n", unk_a, unk_p, bi_p);
  fprintf(f, "written slots (object, field): %d\n", nslots);
  fprintf(f, "    one class:           per-name %4d   points-to %4d\n", a_prec, p_prec);
  fprintf(f, "    exactly int:         per-name %4d   points-to %4d\n", a_int, p_int);
  fprintf(f, "    numbers only:        per-name %4d   points-to %4d\n", a_num, p_num);
  fprintf(f, "    one known object:    per-name %4d   points-to %4d   (of %d object slots)\n", a_obj1, p_obj1, objslots);
  fprintf(f, "can still break: eval=%d with=%d delete=%d proto-mutation=%d computed-key stores=%d unknown calls=%d accessors=%d, objects that reach unknown code=%d\n",
          P->eval_ops, P->with_ops, P->delete_ops, P->setproto_ops, P->dyn_key_stores, P->unk_calls, P->accessors, bs_count(&P->esc));
  fprintf(f, "native calls with a typed (primitive) result: %d\n", P->native_typed);
  fprintf(f, "\n");
}

/* The analysis, run once and kept for the code generator (qjs_aot_bundle_field) and the report. */
static BRun g_A, g_P;
static int g_prep_state, g_nok, g_nloops;
#define A g_A
#define P g_P

static int bundle_prepare(void) {
  int i, s, pass, nok = 0, nloops = 0;
  if (g_prep_state != 0) return g_prep_state > 0 ? 0 : -1;
  if (g_bfuncs == NULL || g_bctx == NULL) { g_prep_state = -1; return -1; }
  g_bfn = calloc((size_t)(g_bnfuncs > 0 ? g_bnfuncs : 1), sizeof(*g_bfn));
  if (g_bfn == NULL) { g_prep_state = -1; return -1; }
  for (i = 0; i < g_bnfuncs; i++) {
    const JSAotGenFunc *g = &g_bfuncs[i];
    if (i + 1 < g_bnfuncs && memcmp(g->digest, g_bfuncs[i + 1].digest, 16) == 0) continue;
    if (g->eligible != AOT_GEN_ELIGIBLE) continue;
    if (q2_build(g, &g_bfn[i].q) < 0) { q2_free(&g_bfn[i].q); continue; }
    g_bfn[i].g = g;
    g_bfn[i].ok = 1;
    mark_loops(&g_bfn[i]);
    mark_uncond(&g_bfn[i]);
    nok++;
    if (g_bfn[i].has_loop) nloops++;
  }
  for (i = 0; i < g_bnfuncs; i++) if (!g_bfn[i].ok) g_bfn[i].g = &g_bfuncs[i];
  /* layer 1: per-name joins */
  label_functions();
  g_made = calloc((size_t)(g_bnfuncs > 0 ? g_bnfuncs : 1), 1);
  for (i = 0; i < g_bnfuncs; i++) {
    int k;
    if (!g_bfn[i].ok) continue;
    for (k = 0; k < g_bfn[i].q.ninsns; k++) {
      const Q2Insn *in = &g_bfn[i].q.insns[k];
      int a, ch;
      if (in->rop != ROP_make_closure) continue;
      for (a = 0; a < in->narg && in->arg[a].kind != 'c'; a++) {}
      if (a < in->narg && (ch = fn_of_closure(g_bfn[i].g, (int)in->arg[a].val)) >= 0) g_made[ch] = 1;
    }
  }
  compute_cells();
  for (i = 0; i < g_bnfuncs; i++) {
    int k;
    if (!g_bfn[i].ok) continue;
    for (k = 0; k < g_bfn[i].q.ninsns; k++) {
      const Q2Insn *in = &g_bfn[i].q.insns[k];
      char nm[64];
      if ((in->rop == ROP_define_global_func || in->rop == ROP_put_global || in->rop == ROP_define_global || in->rop == ROP_define_class ||
           in->rop == ROP_define_class_computed) && bn_atom(in, NULL, nm, sizeof(nm), 0))
        g_gdef[bn_name(nm)] = 1;
    }
  }
  run_one(&A, 0);
  /* layer 2: points-to; an assignment to F.prototype is assumed to precede F's first construct, and the run is repeated with that known */
  for (pass = 0; pass < 5; pass++) {
    int grew = 0;
    if (pass > 0) run_free(&P);
    R = &P;
    memset(&P, 0, sizeof(P));
    P.pts = 1;
    /* sites flagged in earlier passes keep their flag: st_new is told through g_assigned at creation */
    run_fixpoint(&P);
    for (s = 0; s < P.nst; s++)
      if (P.st[s].kind == SK_FUNC && P.st[s].proto_assigned && assigned_add(P.st[s].afn, P.st[s].apc, P.st[s].ctx)) grew = 1;
    if (!grew) break;
  }
  run_collect(&P);
  compute_hotness(&P);
  compute_effects(&P);

  g_nok = nok;
  g_nloops = nloops;
  g_prep_state = 1;
  return 0;
}

int qjs_aot_bundle_report(const char *path) {
  FILE *f;
  int i, e, s, nok, nloops;
  char b1[256], b2[1024];
  if (path == NULL) return 0;
  if (bundle_prepare() != 0) return -1;
  nok = g_nok;
  nloops = g_nloops;
  f = fopen(path, "w");
  if (f == NULL) return -1;
  fprintf(f, "# bundle facts report (R0): %d functions, %d analysed, %d with loops\n", g_bnfuncs, nok, nloops);
  fprintf(f, "# per-name layer: %d rounds, %d nodes; points-to layer: %d rounds, %d nodes, %d sites, %d instances, %d field nodes\n",
          A.rounds, A.nnd, P.rounds, P.nnd, P.nst, P.nin, P.nfl);
  if (P.overflow_sites) fprintf(f, "# WARNING: %d sites over the %d-site cap were merged into EXT\n", P.overflow_sites, BN_MAXSITES);
  fprintf(f, "# functions with identical bodies share one name below (the analysis treats them as one function)\n\n");
  summarize(f, &A, &P);

  /* ---- sites ---- */
  fprintf(f, "\n== abstract heap (points-to layer) ==\n");
  for (s = 1; s < P.nst; s++) {
    const BSite *S = &P.st[s];
    BField *fs = malloc(sizeof(BField) * (size_t)(P.nfl > 0 ? P.nfl : 1));
    int nf = 0, j;
    if (S->kind == SK_BUILTIN) continue;
    for (j = 0; j < P.nfl; j++) if (P.fl[j].site == s) fs[nf++] = P.fl[j];
    qsort(fs, (size_t)nf, sizeof(BField), fcmp_order);
    flags_str(S->flags, b1, sizeof(b1));
    fprintf(f, "site %d %-9s %s%s%s\n", s, kind_str(S), S->label, S->ctx ? "" : "", bs_has(&P.esc, s) ? "  ESCAPES-TO-UNKNOWN" : "");
    print_protos(f, &P, s);
    {
      char own[300] = "";
      size_t oo = 0;
      int q2, nown = 0;
      for (q2 = 1; q2 < P.nst; q2++)
        if (bs_has(&P.st[q2].proto, s) && q2 != s) {
          if (nown < 3) oo += (size_t)snprintf(own + oo, sizeof(own) - oo, "%s%s", nown ? ", " : "", P.st[q2].label);
          nown++;
        }
      if (nown > 0) fprintf(f, "    prototype of: %s%s\n", own, nown > 3 ? ", ..." : "");
      /* an object that is the `prototype` field of a function is that function's instances' prototype */
      for (q2 = 1; q2 < P.nst; q2++) {
        int fe = fl_find(q2, bn_name("prototype"), 0);
        if (fe >= 0 && P.st[q2].kind == SK_FUNC && bs_has(&P.nd[P.fl[fe].node].pts, s))
          fprintf(f, "    is %s.prototype\n", fn_name(P.st[q2].fn));
      }
    }
    fprintf(f, "    flow: %s%s\n", b1, S->kind == SK_FUNC && S->proto_assigned ? "  (F.prototype assigned in bundle: assumes assignment precedes first construct)" : "");
    {
      BAdd adds[64];
      int na = 0, an, ev;
      walk_site(&P, s, S->kind == SK_INST ? S->ctor_inst : S->ainst, 0, adds, &na, 0);
      if (na > 0) {
        fprintf(f, "    transitions: {}");
        for (an = 0; an < na; an++) fprintf(f, " -%s%s->", g_bn_names[adds[an].name], adds[an].uncond ? "" : "?");
        fprintf(f, " (%d fields%s)\n", na, "");
      }
      /* writes made by other functions after construction */
      {
        char late[400] = "";
        size_t lo = 0;
        int nl = 0, nupd = 0;
        for (ev = 0; ev < P.nev; ev++) {
          int j, inwalk = 0, wfn, known = 0;
          if (P.ev[ev].callee >= 0 || P.ev[ev].site != s) continue;
          wfn = P.in[P.ev[ev].inst].fn;
          for (j = 0; j < na; j++) {
            if (adds[j].name == P.ev[ev].name) known = 1;
            if (adds[j].name == P.ev[ev].name && adds[j].fn == wfn) inwalk = 1;
          }
          if (inwalk) continue;
          if (known) { nupd++; continue; }
          if (nl < 6) lo += (size_t)snprintf(late + lo, sizeof(late) - lo, "%s.%s@%s:%d", nl ? ", " : "", g_bn_names[P.ev[ev].name], fn_name(wfn), P.ev[ev].pc);
          nl++;
        }
        if (nl > 0) {
          fprintf(f, "    ADDS AFTER CONSTRUCTION (%d): %s%s\n", nl, late, nl > 6 ? ", ..." : "");
          g_late_sites++;
          if (strlen(g_late_buf) + strlen(S->label) + strlen(late) + 8 < sizeof(g_late_buf)) {
            strcat(g_late_buf, "  "); strcat(g_late_buf, S->label); strcat(g_late_buf, ": "); strcat(g_late_buf, late); strcat(g_late_buf, "\n");
          }
        }
        if (nupd > 0) fprintf(f, "    updates of constructed fields from other code: %d write sites\n", nupd);
      }
    }
    for (j = 0; j < nf; j++) {
      const BNode *N = &P.nd[fs[j].node];
      char c1[96], o1[256];
      cls_str(N->cls & ~(unsigned)(bs_count(&N->pts) > 0 ? 0 : K_OBJ), c1, sizeof(c1));
      set_str(&P, &N->pts, o1, sizeof(o1), 3);
      fprintf(f, "    .%-18s %-18s writes=%d first=%s:%d%s%s%s\n", g_bn_names[fs[j].name], c1, fs[j].nwrites,
              fs[j].firstfn >= 0 ? fn_name(fs[j].firstfn) : "-", fs[j].firstpc, fs[j].firstblock == 0 ? "" : " (not on every path)",
              o1[0] ? " -> " : "", o1);
    }
    free(fs);
  }

  /* ---- functions ---- */
  fprintf(f, "\n== functions: static hotness, call targets, effects ==\n");
  for (i = 0; i < g_bnfuncs; i++) {
    const BFn *F = &g_bfn[i];
    int ncallers = 0, ntargets = 0;
    if (!F->ok) continue;
    for (e = 0; e < P.ned; e++) { if (P.ed[e].tfn == i) ncallers++; if (P.ed[e].cfn == i) ntargets++; }
    fprintf(f, "fn %d %s hot=%d loop=%d callers=%d callees=%d insns=%d\n", i, fn_name(i), F->hot, F->has_loop, ncallers, ntargets, F->q.ninsns);
    {
      int ii, k;
      BNode tn, pn[16], rn;
      memset(&tn, 0, sizeof(tn)); memset(pn, 0, sizeof(pn)); memset(&rn, 0, sizeof(rn));
      for (ii = 0; ii < P.nin; ii++) {
        const BInst *X = &P.in[ii];
        if (X->fn != i || !X->live) continue;
        bs_or(&tn.pts, &P.nd[X->thisn].pts); tn.cls |= P.nd[X->thisn].cls;
        bs_or(&rn.pts, &P.nd[X->retn].pts); rn.cls |= P.nd[X->retn].cls;
        for (k = 0; k < X->npar; k++) { bs_or(&pn[k].pts, &P.nd[X->parn[k]].pts); pn[k].cls |= P.nd[X->parn[k]].cls; }
      }
      cls_str(tn.cls, b1, sizeof(b1)); set_str(&P, &tn.pts, b2, sizeof(b2), 5);
      fprintf(f, "    this: %s {%s}\n", b1, b2);
      for (k = 0; k < F->g->arg_count && k < 16; k++) {
        cls_str(pn[k].cls, b1, sizeof(b1)); set_str(&P, &pn[k].pts, b2, sizeof(b2), 5);
        fprintf(f, "    arg%d: %s {%s}\n", k, b1, b2);
      }
      cls_str(rn.cls, b1, sizeof(b1)); set_str(&P, &rn.pts, b2, sizeof(b2), 5);
      fprintf(f, "    returns: %s {%s}\n", b1, b2);
    }
    fprintf(f, "    writes: ");
    print_wnames(f, F->twn, 14);
    fprintf(f, "%s%s%s%s%s%s\n", F->twelem ? " +elements" : "", F->twglobal ? " +globals" : "", F->twvarref ? " +closure-vars" : "",
            F->talloc ? " +allocates" : "", F->tunkcall ? " +UNKNOWN-CALL" : "", F->tthrw ? " +throws" : "");
  }

  /* ---- call sites ---- */
  fprintf(f, "\n== call sites: targets per layer ==\n");
  {
    int mono_p = 0, poly_p = 0, unk_p = 0, mono_a = 0, poly_a = 0, unk_a = 0, bi_p = 0;
    for (i = 0; i < P.ncs; i++) {
      const BCall *c = &P.cs[i], *ca = NULL;
      int j;
      char nm[400] = "";
      size_t o = 0;
      for (j = 0; j < A.ncs; j++) if (A.cs[j].fn == c->fn && A.cs[j].pc == c->pc) ca = &A.cs[j];
      for (j = 0; j < c->nt && j < 8; j++) o += (size_t)snprintf(nm + o, sizeof(nm) - o, "%s%s", j ? "," : "", fn_name(c->targets[j]));
      if (c->nt > 8) o += (size_t)snprintf(nm + o, sizeof(nm) - o, ",+");
      if (c->unknown) unk_p++; else if (c->builtin && c->nt == 0) bi_p++; else if (c->nt == 1) mono_p++; else poly_p++;
      if (ca == NULL || ca->unknown) unk_a++; else if (ca->nt == 1) mono_a++; else poly_a++;
      fprintf(f, "call %s:%d %s%s%s -> [%s]%s   per-name: %s\n", fn_name(c->fn), c->pc, c->inloop ? "(loop) " : "", c->callee,
              c->builtin ? " (builtin)" : "", nm, c->unknown ? " +UNKNOWN" : "",
              ca == NULL ? "-" : ca->unknown ? "unknown" : ca->nt == 1 ? "1 target" : ca->nt == 0 ? "none" : "many");
    }
    fprintf(f, "# call sites: %d. points-to: %d one target, %d several, %d builtin, %d unresolved. per-name: %d one target, %d several, %d unresolved\n",
            P.ncs, mono_p, poly_p, bi_p, unk_p, mono_a, poly_a, unk_a);
  }

  /* ---- slot facts, per layer ---- */
  fprintf(f, "\n== slot facts kept: per-name vs points-to ==\n");
  {
    int nslots = 0, a_prec = 0, p_prec = 0, a_int = 0, p_int = 0, a_num = 0, p_num = 0, gained = 0, a_obj1 = 0, p_obj1 = 0, objslots = 0;
    for (i = 0; i < P.nfl; i++) {
      const BField *pf = &P.fl[i];
      int af, kind;
      unsigned pc, ac;
      char c1[96], c2[96];
      if (pf->site <= 0 || pf->name == bn_name("[]")) continue;
      kind = P.st[pf->site].kind;
      if (kind == SK_BUILTIN || kind == SK_EXT || kind == SK_NATIVE) continue;
      if (pf->nwrites == 0) continue;
      af = fl_find_ro_name(&A, pf->name);
      pc = P.nd[pf->node].cls;
      ac = af >= 0 ? A.nd[A.fl[af].node].cls : K_UNK;
      nslots++;
      if (cls_precise(ac)) a_prec++;
      if (cls_precise(pc)) p_prec++;
      if (ac == K_INT) a_int++;
      if (pc == K_INT) p_int++;
      if (cls_numeric_only(ac)) a_num++;
      if (cls_numeric_only(pc)) p_num++;
      if ((pc & K_OBJ) && !(pc & K_UNK)) { objslots++; if (bs_count(&P.nd[pf->node].pts) == 1) p_obj1++; }
      if (af >= 0 && (A.nd[A.fl[af].node].cls & K_OBJ) && !(ac & K_UNK) && bs_count(&A.nd[A.fl[af].node].pts) == 1) a_obj1++;
      if (cls_precise(pc) && !cls_precise(ac)) {
        gained++;
        cls_str(ac, c1, sizeof(c1)); cls_str(pc, c2, sizeof(c2));
        fprintf(f, "gain %s.%s: per-name %s -> points-to %s\n", P.st[pf->site].label, g_bn_names[pf->name], c1, c2);
      }
    }
    fprintf(f, "# written slots (site,field): %d\n", nslots);
    fprintf(f, "#   one class:      per-name %d, points-to %d (gained %d)\n", a_prec, p_prec, gained);
    fprintf(f, "#   exactly int:    per-name %d, points-to %d\n", a_int, p_int);
    fprintf(f, "#   numbers only:   per-name %d, points-to %d\n", a_num, p_num);
    fprintf(f, "#   object slots:   %d, of which a single known object: per-name %d, points-to %d\n", objslots, a_obj1, p_obj1);
  }

  /* ---- where facts can still break ---- */
  fprintf(f, "\n== where a fact can still break ==\n");
  fprintf(f, "eval=%d with=%d delete=%d set_proto_or___proto__=%d copy_props=%d arguments/special_object=%d computed-key element stores=%d\n",
          P.eval_ops, P.with_ops, P.delete_ops, P.setproto_ops, P.copyprops_ops, P.special_args, P.dyn_key_stores);
  fprintf(f, "accessor properties defined through descriptors (slot unknown): %d\n", P.accessors);
  fprintf(f, "unresolved or unknown calls: %d\n", P.unk_calls);
  for (i = 0; i < P.ncs; i++)
    if (P.cs[i].unknown) fprintf(f, "  unknown call %s:%d %s\n", fn_name(P.cs[i].fn), P.cs[i].pc, P.cs[i].callee);
  fprintf(f, "closure variables are one shared cell in this model (precision loss only): %d functions write them\n", ({ int c = 0; for (i = 0; i < g_bnfuncs; i++) if (g_bfn[i].ok && g_bfn[i].wvarref) c++; c; }));
  {
    /* the audit of the design that replaced a "safety net": a function some make_closure creates, that no instance reaches (no resolved call, not escaped), while unresolved calls exist
       in the program. In a sound model that function cannot run; one listed here is a lost flow to look at (kit/vkit print the count). */
    int nstr = 0, nstrx = 0, fi2, in2;
    char names[600] = "", namesx[600] = "";
    size_t no = 0, nox = 0;
    for (fi2 = 0; fi2 < g_bnfuncs; fi2++) {
      int live = 0;
      if (!g_bfn[fi2].ok || !g_made[fi2]) continue;
      for (in2 = 0; in2 < P.nin; in2++) if (P.in[in2].fn == fi2 && P.in[in2].live) live = 1;
      if (live || P.unk_calls == 0) continue;
      nstr++;
      if (no < sizeof(names) - 60) no += (size_t)snprintf(names + no, sizeof(names) - no, " %s", fn_name(fi2));
      if (qjs_aot_prof_entries(g_bfn[fi2].g->digest) > 0) {   /* the profile ran it: a flow the analysis lost */
        nstrx++;
        if (nox < sizeof(namesx) - 60) nox += (size_t)snprintf(namesx + nox, sizeof(namesx) - nox, " %s(%llu)", fn_name(fi2), qjs_aot_prof_entries(g_bfn[fi2].g->digest));
      }
    }
    fprintf(f, "stranded functions (made, never reached, not escaped, unresolved calls present): %d%s\n", nstr, names);
    fprintf(f, "stranded functions the profile EXECUTED (lost flows; must be 0): %d%s\n", nstrx + P.oracle_fixed, nstrx ? namesx : P.oracle_names);
  }
  fprintf(f, "objects that reach unknown code (their fields may change under any fact):\n");
  for (s = 1; s < P.nst; s++)
    if (bs_has(&P.esc, s) && P.st[s].kind != SK_BUILTIN) {
      if (P.st[s].esc_fn > 0) fprintf(f, "  %s %s  <- %s at %s:%d [phase %d]\n", kind_str(&P.st[s]), P.st[s].label, P.st[s].esc_why, fn_name(P.st[s].esc_fn - 1), P.st[s].esc_pc, P.st[s].esc_phase);
      else fprintf(f, "  %s %s  <- %s via %s\n", kind_str(&P.st[s]), P.st[s].label, P.st[s].esc_why ? P.st[s].esc_why : "?", P.st[s].esc_pc >= 0 && P.st[s].esc_pc < P.nst ? P.st[P.st[s].esc_pc].label : "?");
    }
  fprintf(f, "sites that gain fields after construction (their shape is not final at the end of the constructor): %d\n%s", g_late_sites, g_late_buf);
  fprintf(f, "prototype assignments assumed to precede the first construct (guard: the proto watchpoint):\n");
  for (s = 1; s < P.nst; s++) if (P.st[s].kind == SK_FUNC && P.st[s].proto_assigned) fprintf(f, "  %s\n", P.st[s].label);

  /* ---- escapes ---- */
  fprintf(f, "\n== allocation sites: escape facts ==\n");
  {
    int local = 0, total = 0;
    for (s = 1; s < P.nst; s++) {
      const BSite *S = &P.st[s];
      if (S->kind == SK_EXT || S->kind == SK_BUILTIN || S->kind == SK_NATIVE) continue;
      total++;
      flags_str(S->flags, b1, sizeof(b1));
      if (!bs_has(&P.esc, s) && !(S->flags & (SF_STORED | SF_GLOBAL | SF_CLOSURE | SF_RET | SF_UNKARG))) local++;
      fprintf(f, "escape %s %s: %s%s\n", kind_str(S), S->label, b1, bs_has(&P.esc, s) ? "ESCAPES" : "");
    }
    fprintf(f, "# %d sites, %d never stored, returned, captured or passed to unknown code\n", total, local);
  }

  /* ---- unmodeled ---- */
  fprintf(f, "\n== instructions the model treats as unknown (results UNK, operands escape) ==\n");
  for (i = 0; i < 256; i++)
    if (P.unmodeled[i]) {
      char nmb[64];
      e_rop_name((uint16_t)i, nmb, sizeof(nmb));
      fprintf(f, "rop %d %s: %d\n", i, nmb, P.unmodeled[i]);
    }
  qjs_aot_bundle_report_receivers(f);
  report_mshapes(f);
  fclose(f);
  (void)count_if_changed;
  return 0;
}

void qjs_aot_bundle_init(JSContext *ctx, const JSAotGenFunc *funcs, int n) {
  g_bctx = ctx;
  g_bfuncs = funcs;
  g_bnfuncs = n;
}

static void cls_str_fwd(unsigned c, char *out, size_t n) { cls_str(c, out, n); }
static void set_str_fwd(const BRun *r, const BSet *set, char *out, size_t n, int maxn) { set_str(r, set, out, n, maxn); }

static int fl_find_in(BRun *r, int site, int name) {
  BRun *keep = R;
  int e;
  R = r;
  e = fl_find(site, name, 0);
  R = keep;
  return e;
}

/* ---- the code generator's view (fast.c) ----------------------------------------------------------- */
void qjs_aot_bundle_report_receivers(FILE *f);
static int bundle_fn_index(const JSAotGenFunc *g) {
  int i;
  if (g >= g_bfuncs && g < g_bfuncs + g_bnfuncs) return (int)(g - g_bfuncs);
  for (i = 0; i < g_bnfuncs; i++)
    if (g_bfn[i].ok && memcmp(g_bfn[i].g->digest, g->digest, 16) == 0) return i;
  return -1;
}

/* Where the field accessed at `pc` of `g` lives: the position of the property in every receiver the bundle knows (the same in all of them), and the class
   of the values stored there. `kind` 0: a read, 1: a write. Returns 1 when every known receiver is an object of an allocation site (not an array, a
   function or a prototype) that adds the field in its construction walk, on every path and after only such fields. *is_add: the access is itself the add.
   Whatever it says is a hypothesis about the receivers; the code built on it verifies each receiver's shape before use. */
static const char *g_field_filter; /* when set, only accesses of this name count (the second read of a chain shares its pc with the first) */
int qjs_aot_bundle_field(const JSAotGenFunc *g, int pc, int kind, int *slot, unsigned *cls, int *is_add);
int qjs_aot_bundle_field_named(const JSAotGenFunc *g, int pc, int kind, const char *name, int *slot, unsigned *cls, int *is_add) {
  int r;
  g_field_filter = name;
  r = qjs_aot_bundle_field(g, pc, kind, slot, cls, is_add);
  g_field_filter = NULL;
  return r;
}
int qjs_aot_bundle_field(const JSAotGenFunc *g, int pc, int kind, int *slot, unsigned *cls, int *is_add) {
  int fi, i, s, idx = -1, name = -1, ext = 0;
  BSet recv;
  unsigned c = 0, unkcls = 0;
  if (bundle_prepare() != 0) return 0;
  fi = bundle_fn_index(g);
  if (fi < 0) return 0;
  memset(&recv, 0, sizeof(recv));
  *is_add = 0;
  for (i = 0; i < P.nacc; i++)
    if (P.acc[i].fn == fi && P.acc[i].pc == pc && P.acc[i].kind == kind && (g_field_filter == NULL || strcmp(g_bn_names[P.acc[i].name], g_field_filter) == 0)) {
      bs_or(&recv, &P.acc[i].recv);
      unkcls |= P.acc[i].cls;
      name = P.acc[i].name;
    }
  if (name < 0) { if (getenv("QJS_AOT_DEBUG_FAST") != NULL) fprintf(stderr, "field %s:%d kind %d .%s: no access recorded at this pc\n", fn_name(fi), pc, kind, g_field_filter ? g_field_filter : "?"); return 0; }
  if (unkcls & K_UNK) { if (getenv("QJS_AOT_DEBUG_FAST") != NULL) fprintf(stderr, "field %s:%d .%s: UNPROVEN receiver holds an unknown value\n", fn_name(fi), pc, g_bn_names[name]); return 0; }
  BS_FOR(s, &recv) {
    const BSite *S = &P.st[s];
    BAdd adds[64];
    int na = 0, j, k, f;
    int dbg = getenv("QJS_AOT_DEBUG_FAST") != NULL;
    if (S->kind == SK_EXT || S->kind == SK_NATIVE) { if (dbg) fprintf(stderr, "field %s:%d .%s: UNPROVEN receiver set holds %s (%s)\n", fn_name(fi), pc, g_bn_names[name], S->label, sk_names[S->kind]); return 0; }
    (void)ext;
    if (S->kind != SK_INST && S->kind != SK_OBJ) { if (dbg) fprintf(stderr, "field %s:%d .%s: receiver %s is a %s\n", fn_name(fi), pc, g_bn_names[name], S->label, sk_names[S->kind]); return 0; }
    walk_site(&P, s, S->kind == SK_INST ? S->ctor_inst : S->ainst, 0, adds, &na, 0);
    for (j = 0; j < na && adds[j].name != name; j++) {}
    if (j == na) { if (dbg) fprintf(stderr, "field %s:%d .%s: receiver %s does not add it (%d adds)\n", fn_name(fi), pc, g_bn_names[name], S->label, na); return 0; }
    for (k = 0; k <= j; k++) if (!adds[k].uncond) { if (dbg) fprintf(stderr, "field %s:%d .%s: receiver %s: add #%d (%s) is not on every path\n", fn_name(fi), pc, g_bn_names[name], S->label, k, g_bn_names[adds[k].name]); return 0; }
    if (idx >= 0 && idx != j) { if (dbg) fprintf(stderr, "field %s:%d .%s: receiver %s has it at %d, others at %d\n", fn_name(fi), pc, g_bn_names[name], S->label, j, idx); return 0; }
    idx = j;
    if (adds[j].fn == fi && adds[j].pc == pc) *is_add = 1;
    for (k = 0; k < adds[j].nalt; k++) if (adds[j].fn == fi && adds[j].alt[k] == pc) *is_add = 1;
    f = fl_find_in(&P, s, name);
    if (f >= 0) c |= P.nd[P.fl[f].node].cls;
  }
  (void)ext;
  if (idx < 0) { if (getenv("QJS_AOT_DEBUG_FAST") != NULL) fprintf(stderr, "field %s:%d .%s: EMPTY receiver set (nothing flows into the receiver; unreachable or lost flow)\n", fn_name(fi), pc, g_bn_names[name]); return 0; }
  *slot = idx;
  *cls = c;
  return 1;
}

/* Whether a class set (qjs_aot_bundle_field's cls) is non-empty and holds no reference-counted value and nothing unknown. */
int qjs_aot_bundle_cls_plain(unsigned cls) { return cls != 0 && !(cls & (K_STR | K_OBJ | K_UNK)); }

/* The name of an atom, for the generated text. */
int qjs_aot_bundle_atom_name(uint32_t atom, char *out, size_t cap) {
  size_t len;
  const char *nm;
  if (g_bctx == NULL) return 0;
  nm = JS_AtomToCStringLen(g_bctx, &len, (JSAtom)atom);
  if (nm == NULL) return 0;
  snprintf(out, cap, "%s", nm);
  JS_FreeCString(g_bctx, nm);
  return 1;
}

/* The functions the call at `pc` of `g` can reach, as the bundle resolves it (points-to targets): up to `max` of them (the representative of functions that
   share a body), or -1 when the call is not fully resolved (an unknown or builtin callee, or more than eight targets). */
int qjs_aot_bundle_targets(const JSAotGenFunc *g, int pc, const JSAotGenFunc **out, int max) {
  int fi, i, j, n = 0;
  if (bundle_prepare() != 0) return -1;
  fi = bundle_fn_index(g);
  if (fi < 0) return -1;
  for (i = 0; i < P.ncs; i++) {
    const BCall *c = &P.cs[i];
    if (c->fn != fi || c->pc != pc) continue;
    if (c->unknown || c->builtin || c->nt > 8 || c->nt == 0) return -1;
    for (j = 0; j < c->nt && n < max; j++) out[n++] = g_bfn[c->targets[j]].g;
    return n;
  }
  return -1;
}

/* The functions an `X.call(thisArg, ...)` at `pc` of `g` can reach (X is the call's receiver value, thisArg the first window register): up to `max`, or -1 when the
   call is not such a call or X is not fully resolved. */
int qjs_aot_bundle_thru_targets(const JSAotGenFunc *g, int pc, const JSAotGenFunc **out, int max) {
  int fi, i, j, n = 0;
  if (bundle_prepare() != 0) return -1;
  fi = bundle_fn_index(g);
  if (fi < 0) return -1;
  for (i = 0; i < P.ncs; i++) {
    const BCall *c = &P.cs[i];
    if (c->fn != fi || c->pc != pc) continue;
    if (!c->thru || c->unknown || c->nt > 8 || c->nt == 0) return -1;
    for (j = 0; j < c->nt && n < max; j++) out[n++] = g_bfn[c->targets[j]].g;
    return n;
  }
  return -1;
}

/* The JSAotGenFunc of function index `i` (for the report of a region). */
const JSAotGenFunc *qjs_aot_bundle_func(int i) { return i >= 0 && i < g_bnfuncs && g_bfn != NULL && g_bfn[i].ok ? g_bfn[i].g : NULL; }

/* ---- class-keyed facts (the region compiler) ------------------------------------------------------------ */
static int bn_lookup(const char *s) {
  int i;
  for (i = 0; i < g_bn_nnames; i++) if (strcmp(g_bn_names[i], s) == 0) return i;
  return -1;
}

/* The adds of `site` in its construction walk. */
static int site_adds(int s, BAdd *adds) {
  const BSite *S = &P.st[s];
  int na = 0;
  if (S->kind != SK_INST && S->kind != SK_OBJ) return -1;
  walk_site(&P, s, S->kind == SK_INST ? S->ctor_inst : S->ainst, 0, adds, &na, 0);
  return na;
}

int qjs_aot_bundle_site_field(int site, const char *name, int *slot, unsigned *cls) {
  BAdd adds[64];
  int na, nm, j, k, f;
  if (bundle_prepare() != 0 || site <= 0 || site >= P.nst || name == NULL) return 0;
  nm = bn_lookup(name);
  if (nm < 0) return 0;
  na = site_adds(site, adds);
  if (na < 0) return 0;
  for (j = 0; j < na && adds[j].name != nm; j++) {}
  if (j == na) return 0;
  for (k = 0; k <= j; k++) if (!adds[k].uncond) return 0;
  *slot = j;
  *cls = 0;
  f = fl_find_in(&P, site, nm);
  if (f >= 0) *cls = P.nd[P.fl[f].node].cls;
  return 1;
}

/* The name of slot `slot` of objects of `site` (the slot-th field the construction walk adds); 0 if that add is conditional or absent. */
int qjs_aot_bundle_site_field_name(int site, int slot, char *out, size_t cap) {
  BAdd adds[64];
  int na, k;
  if (bundle_prepare() != 0 || site <= 0 || site >= P.nst || slot < 0) return 0;
  na = site_adds(site, adds);
  if (na < 0 || slot >= na) return 0;
  for (k = 0; k <= slot; k++) if (!adds[k].uncond) return 0;
  if (adds[slot].name < 0 || adds[slot].name >= g_bn_nnames) return 0;
  snprintf(out, cap, "%s", g_bn_names[adds[slot].name]);
  return 1;
}

static void recv_ctor_global(int s, char *out, size_t cap) {
  const BSite *S = &P.st[s];
  int nm, t;
  out[0] = 0;
  if (S->kind != SK_INST || S->fn < 0) return;
  for (nm = 0; nm < g_bn_nnames; nm++) {
    if (P.globals[nm] < 0) continue;
    BS_FOR(t, &P.nd[P.globals[nm]].pts) if (P.st[t].kind == SK_FUNC && P.st[t].fn == S->fn) { snprintf(out, cap, "%s", g_bn_names[nm]); return; }
  }
}

static int g_recv_only = -1;   /* qjs_aot_bundle_receivers_for: only this allocation site's class is wanted */
int qjs_aot_bundle_receivers(const JSAotGenFunc *g, int pc, int kind, QjsAotRecvClass *out, int max) {
  int fi, i, s, n = 0;
  BSet recv;
  unsigned cls = 0;
  int found = 0, nodisp = 0;
  if (bundle_prepare() != 0) return -1;
  fi = bundle_fn_index(g);
  if (fi < 0) return -1;
  memset(&recv, 0, sizeof(recv));
  if (kind == 2) {
    for (i = 0; i < P.nrcv; i++)
      if (P.rcv[i].fn == fi && P.rcv[i].pc == pc) { bs_or(&recv, &P.rcv[i].recv); cls |= P.rcv[i].cls; nodisp |= P.rcv[i].nodisp; found = 1; }
  } else {
    for (i = 0; i < P.nacc; i++)
      if (P.acc[i].fn == fi && P.acc[i].pc == pc && P.acc[i].kind == kind) { bs_or(&recv, &P.acc[i].recv); cls |= P.acc[i].cls; found = 1; }
  }
  if (!found) return -1;
  if (g_recv_only < 0 && (cls & (K_UNK | K_BOOL | K_INT | K_INTOV | K_DBL | K_NUM | K_STR))) return -1;
  if (g_recv_only < 0 && kind == 2 && nodisp) return -1;
  if (g_recv_only >= 0 && (!bs_has(&recv, g_recv_only) || site_is_ext(g_recv_only) || bs_has(&P.esc, g_recv_only))) return -1;
  BS_FOR(s, &recv) {
    if (g_recv_only >= 0 && s != g_recv_only) continue;
    const BSite *S = &P.st[s];
    BAdd adds[64];
    QjsAotRecvClass *c;
    int na, j, late = 0, ev, tfn = -1, nt = 0, p;
    if (S->kind != SK_INST && S->kind != SK_OBJ) return -1;
    if (S->afn < 0) return -1; /* a modelled builtin object (Object, ...) */
    if (n >= max) return -1;
    c = &out[n++];
    memset(c, 0, sizeof(*c));
    c->site = s;
    c->proto_site = -1;
    snprintf(c->label, sizeof(c->label), "%s", S->label);
    recv_ctor_global(s, c->ctor_global, sizeof(c->ctor_global));
    if (bs_count(&S->proto) == 1) BS_FOR(p, &S->proto) c->proto_site = p;
    na = site_adds(s, adds);
    c->nfields = na;
    c->one_shape = 1;
    for (j = 0; j < na; j++) if (!adds[j].uncond) c->one_shape = 0;
    for (ev = 0; ev < P.nev; ev++) {
      if (P.ev[ev].callee >= 0 || P.ev[ev].site != s) continue;
      for (j = 0; j < na && adds[j].name != P.ev[ev].name; j++) {}
      if (j == na) late = 1;
    }
    if (late) c->one_shape = 0;
    c->escapes = bs_has(&P.esc, s) ? 1 : 0;
    if (kind == 2) {
      for (j = 0; j < P.nrt; j++)
        if (P.rt[j].fn == fi && P.rt[j].pc == pc && P.rt[j].site == s && P.rt[j].tfn != tfn) { tfn = P.rt[j].tfn; nt++; }
      c->ntargets = nt;
      if (nt == 1) c->target = g_bfn[tfn].g;
    }
  }
  return n;
}

/* The functions a method call at (g, pc) may reach on objects of `site` (the analysis's per-object resolution; more than one when it cannot
   tell an override from what it overrides): up to max in out, the count returned. */
int qjs_aot_bundle_class_targets(const JSAotGenFunc *g, int pc, int site, const JSAotGenFunc **out, int max) {
  int fi, j, k, n = 0;
  if (bundle_prepare() != 0) return 0;
  fi = bundle_fn_index(g);
  if (fi < 0) return 0;
  for (j = 0; j < P.nrt; j++) {
    if (P.rt[j].fn != fi || P.rt[j].pc != pc || P.rt[j].site != site) continue;
    for (k = 0; k < n && out[k] != g_bfn[P.rt[j].tfn].g; k++) {}
    if (k < n) continue;
    if (n >= max) return -1;
    out[n++] = g_bfn[P.rt[j].tfn].g;
  }
  return n;
}

/* How many times the bundle writes the global `name` (a function or class declaration counts as one); -1 if it is not a global the bundle knows. */
int qjs_aot_bundle_global_writes(const char *name) {
  int i, k, n = 0;
  if (bundle_prepare() != 0 || name == NULL) return -1;
  /* the stores of a value to the binding: a `var` declaration alone (define_global) gives none, its initialiser is a put_global */
  for (i = 0; i < g_bnfuncs; i++) {
    if (!g_bfn[i].ok) continue;
    for (k = 0; k < g_bfn[i].q.ninsns; k++) {
      const Q2Insn *in = &g_bfn[i].q.insns[k];
      char nm[64];
      if ((in->rop == ROP_define_global_func || in->rop == ROP_put_global || in->rop == ROP_define_class || in->rop == ROP_define_class_computed) &&
          bn_atom(in, NULL, nm, sizeof(nm), 0) && strcmp(nm, name) == 0)
        n++;
    }
  }
  return n;
}

/* The same count with the profile's view: how many times the stores ran in the profiled run (`*exec`, summed over the sites). A global with several store sites that ran about once each is
   an initialisation idiom (`if (typeof NS == 'undefined') var NS = {}` repeated per file, `var NS = NS || {}`), not a mutable variable. */
/* Whether every store to the global is in TOP-LEVEL code (the script's entry function, never inside a function that can run again): the namespace idiom (`if (typeof NS == 'undefined')
   var NS = {}`, `NS.X = NS.X || {}`) always satisfies it, a rebinding in hot code (`current = node`) never does. */
int qjs_aot_bundle_global_toplevel_only(const char *name) {
  int i, k, n = 0;
  if (bundle_prepare() != 0 || name == NULL) return 0;
  for (i = 0; i < g_bnfuncs; i++) {
    if (!g_bfn[i].ok) continue;
    for (k = 0; k < g_bfn[i].q.ninsns; k++) {
      const Q2Insn *in = &g_bfn[i].q.insns[k];
      char nm[64];
      if ((in->rop == ROP_define_global_func || in->rop == ROP_put_global || in->rop == ROP_define_class || in->rop == ROP_define_class_computed) &&
          bn_atom(in, NULL, nm, sizeof(nm), 0) && strcmp(nm, name) == 0) {
        n++;
        if (g_made[i] || strcmp(g_bfn[i].g->name, "<eval>") != 0) return 0;
      }
    }
  }
  return n > 0;
}
/* Whether every value the program stores in the global is an object (sites, no number / string / undefined / unknown): a namespace or a class binding, never a counter or a flag. */
int qjs_aot_bundle_global_objonly(const char *name) {
  int nid, g;
  if (bundle_prepare() != 0 || name == NULL) return 0;
  nid = bn_lookup(name);
  if (nid < 0 || P.globals == NULL || P.globals[nid] < 0) return 0;
  g = P.globals[nid];
  return P.nd[g].cls == K_OBJ && bs_count(&P.nd[g].pts) > 0 && !bs_has(&P.nd[g].pts, 0);
}
int qjs_aot_bundle_global_writes_exec(const char *name, unsigned long long *exec) {
  int i, k, n = 0;
  *exec = 0;
  if (bundle_prepare() != 0 || name == NULL) return -1;
  for (i = 0; i < g_bnfuncs; i++) {
    if (!g_bfn[i].ok) continue;
    for (k = 0; k < g_bfn[i].q.ninsns; k++) {
      const Q2Insn *in = &g_bfn[i].q.insns[k];
      char nm[64];
      if ((in->rop == ROP_define_global_func || in->rop == ROP_put_global || in->rop == ROP_define_class || in->rop == ROP_define_class_computed) &&
          bn_atom(in, NULL, nm, sizeof(nm), 0) && strcmp(nm, name) == 0) {
        n++;
        *exec += qjs_aot_prof_pc_count(g_bfn[i].g->digest, in->pc);
      }
    }
  }
  return n;
}

/* The value of the constant path `G.F` when the bundle stores F exactly once, anywhere, and that store is `G.F = <int constant>` on the global G
   (read straight from the binding, through moves): 1 with the int in *out, 0 otherwise. A guess for the code: the region binds the value at init
   with a fact on it and checks it equals the guess. */
int qjs_aot_bundle_const_int(const char *gname, const char *fname, int32_t *out) {
  int i, k, nst = 0, ok = 0;
  int32_t val = 0;
  if (bundle_prepare() != 0 || gname == NULL || fname == NULL) return 0;
  for (i = 0; i < g_bnfuncs; i++) {
    const Q2Func *q;
    if (!g_bfn[i].ok) continue;
    q = &g_bfn[i].q;
    for (k = 0; k < q->ninsns; k++) {
      const Q2Insn *in = &q->insns[k];
      char nm[64];
      int rv, vv, hop;
      if (!(in->rop == ROP_put_field || in->rop == ROP_put_field_xfer || in->rop == ROP_define_field)) continue;
      if (!bn_atom(in, NULL, nm, sizeof(nm), 0) || strcmp(nm, fname) != 0) continue;
      nst++;
      if (in->nuse < 2) continue;
      rv = (int)in->use[0];
      vv = (int)in->use[1];
      for (hop = 0; hop < 8; hop++) {
        const Q2Insn *d;
        if (rv < 0 || rv >= q->nvalues || q->values[rv].insn < 0) break;
        d = &q->insns[q->values[rv].insn];
        if (d->op == Q2_OP_MOVE && d->nuse == 1) { rv = (int)d->use[0]; continue; }
        break;
      }
      if (rv < 0 || rv >= q->nvalues || q->values[rv].insn < 0) continue;
      {
        const Q2Insn *gd = &q->insns[q->values[rv].insn];
        char gn[64];
        if (gd->rop != ROP_get_global || !bn_atom(gd, NULL, gn, sizeof(gn), 0) || strcmp(gn, gname) != 0) continue;
      }
      for (hop = 0; hop < 8; hop++) {
        const Q2Insn *d;
        if (vv < 0 || vv >= q->nvalues || q->values[vv].insn < 0) break;
        d = &q->insns[q->values[vv].insn];
        if (d->op == Q2_OP_MOVE && d->nuse == 1) { vv = (int)d->use[0]; continue; }
        break;
      }
      if (vv < 0 || vv >= q->nvalues) continue;
      if (q->values[vv].is_const && q->values[vv].con_kind == Q2_CON_INT) val = (int32_t)q->values[vv].con;
      else {
        /* the bundle's IR is not inferred: an int immediate load (`const` with an `i` operand) */
        const Q2Insn *cd = q->values[vv].insn >= 0 ? &q->insns[q->values[vv].insn] : NULL;
        int ai;
        if (cd == NULL || cd->op != Q2_OP_CONST) continue;
        for (ai = 0; ai < cd->narg && cd->arg[ai].kind != 'i'; ai++) {}
        if (ai == cd->narg) continue;
        val = (int32_t)cd->arg[ai].val;
      }
      ok = 1;
    }
  }
  if (nst != 1 || !ok) return 0;
  *out = val;
  return 1;
}

/* The allocation site of `new F(...)` at (g, pc): its id when every context of it adds the same fields in the same order, all of them
   unconditionally in the construction walk and none later anywhere in the bundle (the class has one shape); -1 otherwise. */
int qjs_aot_bundle_alloc_site(const JSAotGenFunc *g, int pc) {
  int fi, s, first = -1, nf0 = -1, j, ev;
  BAdd a0[64];
  if (bundle_prepare() != 0) return -1;
  fi = bundle_fn_index(g);
  if (fi < 0) return -1;
  for (s = 1; s < P.nst; s++) {
    const BSite *S = &P.st[s];
    BAdd adds[64];
    int na;
    if (S->kind != SK_INST || S->afn != fi || S->apc != pc) continue;
    na = site_adds(s, adds);
    if (na < 0) return -1;
    for (j = 0; j < na; j++) if (!adds[j].uncond) return -1;
    for (ev = 0; ev < P.nev; ev++) {
      if (P.ev[ev].callee >= 0 || P.ev[ev].site != s) continue;
      for (j = 0; j < na && adds[j].name != P.ev[ev].name; j++) {}
      if (j == na) return -1;
    }
    if (first < 0) { first = s; nf0 = na; memcpy(a0, adds, sizeof(BAdd) * (size_t)na); continue; }
    if (na != nf0) return -1;
    for (j = 0; j < na; j++) if (adds[j].name != a0[j].name) return -1;
  }
  return first;
}


/* The constructor's digest of an allocation site. */
int qjs_aot_bundle_site_ctor_digest(int site, char hex[33]) {
  int h;
  if (bundle_prepare() != 0 || site <= 0 || site >= P.nst || P.st[site].kind != SK_INST || P.st[site].fn < 0 || g_bfn[P.st[site].fn].g == NULL) return 0;
  for (h = 0; h < 16; h++) snprintf(hex + 2 * h, 3, "%02x", g_bfn[P.st[site].fn].g->digest[h]);
  return 1;
}

/* The names added to objects of `site` outside its construction walk, in first-event order. */
int qjs_aot_bundle_site_late_names(int site, char (*names)[40], int max) {
  BAdd adds[64];
  int na, j, ev, n = 0, k;
  if (bundle_prepare() != 0 || site <= 0 || site >= P.nst || P.st[site].kind != SK_INST) return -1;
  na = site_adds(site, adds);
  if (na < 0) return -1;
  for (j = 0; j < na; j++) if (!adds[j].uncond) return -1;
  for (ev = 0; ev < P.nev; ev++) {
    int nm = P.ev[ev].name;
    if (P.ev[ev].callee >= 0 || P.ev[ev].site != site || nm < 0 || nm >= g_bn_nnames) continue;
    for (j = 0; j < na && adds[j].name != nm; j++) {}
    if (j < na) continue;
    for (k = 0; k < n && strcmp(names[k], g_bn_names[nm]) != 0; k++) {}
    if (k < n) continue;
    if (n >= max || strlen(g_bn_names[nm]) >= 40) return -1;
    snprintf(names[n++], 40, "%s", g_bn_names[nm]);
  }
  return n;
}

/* The late names of the CLASS of `site`: the union over every allocation site made by the same constructor function with the same construction-walk fields (the sites of one
   class are separate abstract objects, each with the late adds its own flow shows; the class table is per class). First-event order of the sites in order. */
int qjs_aot_bundle_class_late_names(int site, char (*names)[40], int max) {
  BAdd adds[64], adds2[64];
  int na, s, n = 0, j, k, m;
  if (bundle_prepare() != 0 || site <= 0 || site >= P.nst || P.st[site].kind != SK_INST) return -1;
  na = site_adds(site, adds);
  if (na < 0) return -1;
  for (s = 1; s < P.nst; s++) {
    char l2[16][40];
    int n2, same = 1;
    if (P.st[s].kind != SK_INST || P.st[s].fn != P.st[site].fn) continue;
    if (s != site) {
      if (site_adds(s, adds2) != na) continue;
      for (j = 0; j < na; j++) if (adds2[j].name != adds[j].name) same = 0;
      if (!same) continue;
    }
    n2 = qjs_aot_bundle_site_late_names(s, l2, 16);
    if (n2 < 0) { if (s == site) return -1; continue; }
    for (m = 0; m < n2; m++) {
      for (k = 0; k < n && strcmp(names[k], l2[m]) != 0; k++) {}
      if (k < n) continue;
      if (n >= max) return -1;
      snprintf(names[n++], 40, "%s", l2[m]);
    }
  }
  return n;
}

/* The audit section for classes that gain fields late: the shapes the profile observed for each (the class table's shape list). */
static void report_mshapes(FILE *f) {
  int s, head = 0;
  if (!qjs_aot_mshape_on()) return;
  for (s = 1; s < P.nst; s++) {
    char late[16][40], base[32][40], hex[33];
    int nl, nb, k;
    BAdd adds[64];
    QjsAotClassShapes cs;
    if (P.st[s].kind != SK_INST || P.st[s].afn < 0) continue;
    nl = qjs_aot_bundle_class_late_names(s, late, 16);
    if (nl <= 0 || !qjs_aot_bundle_site_ctor_digest(s, hex)) continue;
    nb = site_adds(s, adds);
    if (nb < 0 || nb > 32) continue;
    for (k = 0; k < nb; k++) snprintf(base[k], 40, "%s", g_bn_names[adds[k].name]);
    qjs_aot_prof_class_shapes(hex, base, nb, late, nl, &cs);
    if (!head) { fprintf(f, "\n== classes that gain fields late: profile-observed shapes (class table) ==\n"); head = 1; }
    fprintf(f, "class site %d %s  ctor=%.8s  late:", s, P.st[s].label, hex);
    for (k = 0; k < nl; k++) fprintf(f, " %s", late[k]);
    fprintf(f, "  status=%d pk=%016llx shapes=%d\n", cs.status, cs.pk, cs.n);
    for (k = 0; k < cs.n; k++) {
      int q;
      fprintf(f, "    shape %d parent=%d add=%s [", k, cs.parent[k], cs.add[k][0] ? cs.add[k] : "-");
      for (q = 0; q < cs.nn[k]; q++) fprintf(f, "%s%s", q ? "," : "", cs.names[k][q]);
      fprintf(f, "]\n");
    }
  }
}

/* ---- the audit section of the facts report (only with --facts) ------------------------------------------ */
void qjs_aot_bundle_report_receivers(FILE *f) {
  int i, nsites = 0, ok = 0, unproven = 0;
  fprintf(f, "\n== class-keyed facts: receivers of method calls and per-class slots (audit) ==\n");
  for (i = 0; i < P.nrcv; i++) {
    const BRcv *r = &P.rcv[i];
    QjsAotRecvClass cl[32];
    int n, j;
    if (!g_bfn[r->fn].ok) continue;
    nsites++;
    n = qjs_aot_bundle_receivers(g_bfn[r->fn].g, r->pc, 2, cl, 32);
    if (n < 0) { unproven++; fprintf(f, "call %s:%d  UNPROVEN (ext/native/unknown/non-instance receiver%s)\n", fn_name(r->fn), r->pc, r->nodisp ? ", or a target not resolved per object" : ""); continue; }
    ok++;
    fprintf(f, "call %s:%d  %d receiver class%s\n", fn_name(r->fn), r->pc, n, n == 1 ? "" : "es");
    for (j = 0; j < n; j++) {
      BAdd adds[64];
      int na = site_adds(cl[j].site, adds), k;
      fprintf(f, "    site %d %s  ctor=%s proto=%d shape=%s%s  ->", cl[j].site, cl[j].label, cl[j].ctor_global[0] ? cl[j].ctor_global : "-", cl[j].proto_site,
              cl[j].one_shape ? "one" : "several", cl[j].escapes ? " ESCAPES" : "");
      if (cl[j].target) fprintf(f, " %s", fn_name(bundle_fn_index(cl[j].target)));
      else fprintf(f, " %s", cl[j].ntargets > 1 ? "(several targets)" : "(none)");
      fprintf(f, "\n      slots:");
      for (k = 0; k < na; k++) {
        int slot;
        unsigned c;
        char cs[96] = "";
        if (qjs_aot_bundle_site_field(cl[j].site, g_bn_names[adds[k].name], &slot, &c)) cls_str(c, cs, sizeof(cs));
        fprintf(f, " %d:%s%s[%s]", k, g_bn_names[adds[k].name], adds[k].uncond ? "" : "?", cs);
      }
      fprintf(f, "\n");
    }
  }
  fprintf(f, "# %d method-call sites: %d with a proven receiver-class set, %d unproven\n", nsites, ok, unproven);
}

/* The function and pc that allocate `site` (an instance site): the construct a class table entry is named after. 1 with *g and *pc, else 0. */
int qjs_aot_bundle_site_alloc(int site, const JSAotGenFunc **g, int *pc) {
  if (bundle_prepare() != 0 || site <= 0 || site >= P.nst || P.st[site].kind != SK_INST || P.st[site].afn < 0 || !g_bfn[P.st[site].afn].g) return 0;
  *g = g_bfn[P.st[site].afn].g;
  *pc = P.st[site].apc;
  return 1;
}

/* The receiver class of ONE allocation site at a call: for a forwarding constructor shared by many classes, whose object is known to be of `site`'s class. The other receivers of the call are
   other objects (other construct sites) and do not matter to this one's dispatch; the site itself must be known (not external, not escaped). */
int qjs_aot_bundle_receivers_for(const JSAotGenFunc *g, int pc, int kind, int site, QjsAotRecvClass *out, int max) {
  int r;
  g_recv_only = site;
  r = qjs_aot_bundle_receivers(g, pc, kind, out, max);
  g_recv_only = -1;
  return r;
}

/* Whether an object of `site` inherits a stored value under `name` from its prototype chain (a default such as `isHit: false` in a prototype literal): the chain objects are the ones the site's
   prototype links name; a field node with content counts. The engine's default `constructor` link is not a stored value. */
int qjs_aot_bundle_site_proto_field(int site, const char *name) {
  BSet seen;
  int stack[64], sp = 0, t, i, nm;
  if (bundle_prepare() != 0 || site <= 0 || site >= P.nst) return 1;
  memset(&seen, 0, sizeof(seen));
  nm = -1;
  { int q; for (q = 0; q < g_bn_nnames; q++) if (strcmp(g_bn_names[q], name) == 0) { nm = q; break; } }
  BS_FOR(i, &P.st[site].proto) if (sp < 64) stack[sp++] = i;
  while (sp > 0) {
    int e;
    t = stack[--sp];
    if (bs_has(&seen, t)) continue;
    bs_add(&seen, t);
    if (nm >= 0) {
      e = fl_find_in(&P, t, nm);
      if (e >= 0 && (P.nd[P.fl[e].node].cls != 0 || bs_count(&P.nd[P.fl[e].node].pts) > 0) && !(P.st[t].kind == SK_PROTO && strcmp(name, "constructor") == 0 && !P.fl[e].prog)) {
        /* only a default written by the program's top-level script is there when the first region learns the class (the prototype literal of a class definition); one stored from a function
           later is caught at run time by the learn's own absent facts, which is what the vkit cases of that kind test */
        if (P.fl[e].firstfn >= 0 && strcmp(fn_name(P.fl[e].firstfn), "<eval>") != 0) continue;
        return 1;
      }
    }
    BS_FOR(i, &P.st[t].proto) if (sp < 64) stack[sp++] = i; else return 1;
  }
  return 0;
}
