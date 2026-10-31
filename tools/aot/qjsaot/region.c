#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emit.h"
#include "fast.h"
#include "ir.h"
#include "quickjs-aot-gen.h"
#include "quickjs-regvm.h"
#include "quickjs.h"
#include "select.h"

/* ---- the region compiler, R3 (AOT-REDESIGN.md sections 1, 5, 8; reviewer MSG 73) ---------------------
   A REGION is a root function and everything it inlines: every call in it that the bundle analysis resolves is expanded in place from the callee's own IR, so
   nothing depends on a callee's cache cells. The region body replaces the root's speculative body (`aot_f_HEX`); the generic body is the exit target.

   What it proves and how it is checked at run time:
   - A call `r.m(...)` is a dispatch on the receiver's shape. The first time a shape is seen the learner looks the method up (qjs_aot_shape_method), maps the
     function it finds to one of the targets the bundle resolved (by its bound entry), and verifies, in the shape, the property position of every field the target (and
     everything inlined into it) reads or writes through the receiver. The shape is held (a counted reference) and remembered; later calls with that shape are a pointer
     compare and a jump. The tables are valid for one prototype epoch (aot_pic_epoch), checked at entry and after a poll that ran a handler.
   - A loaded object that is used as a receiver of fields is guarded the same way (its own learned shapes).
   - Globals of the form `G.F` are read once at entry.
   Exits (AOT-REDESIGN.md section 5): any test that can fail leaves the region with the frame state of its pc. The root's registers are written to the real frame; for
   an exit inside inlined code every inlined frame on the path is materialised as well (rg_xit builds the table, rg_chain_HEX runs it): outermost first, each with its
   function object, receiver, arguments and registers, linked through the frames' caller field and published through regvm_cur. The innermost callee continues in its
   generic body (it has an entry at every pc) at the exit pc; when it returns its value goes into the destination register of the call in the frame below and that
   frame's generic body goes on after the call, out to the root, whose generic body continues after the call (E_RET_G). Nothing is undone and nothing is replayed, so
   the fast path keeps no bookkeeping, and stores, pushes, pops and constructs may come before an exit. */

#define RG_MAXG 1024
#define RG_MAXQ 32
#define RG_MAXT 8
#define RG_MAXK 8
#define RG_MAXX 8192

typedef struct RReq { char name[40]; int slot; int write; } RReq;

typedef struct RGuard {
  int inst, val, pc;
  int dispatch;
  int thru;               /* `X.call(thisArg, ...)`: the dispatch is on the function value X itself (a held pointer per target), the callee's receiver is the first window register */
  int fwd;                /* the call is a wrapper's `this.m.apply(this, arguments)`: the dispatch is on the class of `this`, the arguments are the wrapper's own, `apply` is the builtin (a fact per arm) */
  int intr;               /* an intrinsic call site (Array.prototype.push on a proven array): no targets, a method fact instead */
  char method[40];
  int ntg;
  const JSAotGenFunc *tg[RG_MAXT];
  int cls_mode, cls_site[RG_MAXT];   /* the dispatch is by receiver CLASS (allocation site, found by its constructor's prototype) */
  char cls_ctor[RG_MAXT][48];
  RReq common[RG_MAXQ];
  int ncommon;
  RReq req[RG_MAXT][RG_MAXQ];
  int nreq[RG_MAXT];
  int kcls[RG_MAXT];      /* TRUST: the class (index in the region's class table) of each arm */
  int known;              /* TRUST: the receiver is `this` of an arm whose class is known: one arm, no compare */
  int tcp;                /* TRUST, thru: the constant path (G.f...) whose value X is; crecv: the receiver's */
  int crecv;              /* TRUST: the receiver is a constant path (a static method `G.m()`): one arm, the method bound at init */
} RGuard;

typedef struct RInst {
  Q2Func q;
  const JSAotGenFunc *g;
  char pfx[8];
  int id, depth;
  char self[40];
  char arg[8][40];
  int nargs;
  int par_dv;             /* the value of the parent's call (or construct) result that this instance's result becomes */
  int cls1;               /* 1 + the allocation site of the receiver class this instance was made for, 0 when the instance serves several classes */
  int owner_g, owner_t;   /* the guard whose receiver is this instance's `this`, and the target it dispatched to */
  int par;                /* the instance whose call (or construct) expanded this one, -1 for the root */
  int par_call_pc, par_pc_after, par_read_pc, par_dst, par_kind, par_cons;  /* in the parent: the pc of the call, the pc after it, the register that receives its result, 1 for a construct (of region object rn_<par_cons>) */
  char co[112];           /* the C expression of this instance's function object (an exit builds its frame from it) */
  char (*cur)[40];        /* the current value (as C text) of each register of this function, the frame state an exit writes; NULL in the first walk */
  int fresh;              /* `this` is an object made by a construct of this region that nothing else can see yet: stores to it are not effects */
  int cons;               /* 1 + the construct (RCons) whose object is this instance's `this` (fresh instances), 0 otherwise */
  int hver;               /* the receiver's shape was checked at the region's entry (a hoisted check): reads of its fixed slots may be hoisted too */
  int ploop, curloop;     /* the instance was expanded in a loop of its parent; the block being walked is in a loop of this function */
  int pend_end, pend_pc;  /* a method read whose call is the instruction pend_end-1: what is evaluated in between (the arguments) resumes at the read (pend_pc) */
  /* the method-read pairs whose call is still ahead (nested: `a.f(b.g(x))` has two): an exit inside the arguments writes the callee (and the receiver register, when the read
     left it out) of every one it does not resume at the read of */
  struct RPend { int end, ridx, pc, creg, treg; char name[40], recv[40], this_t[40]; } pend[6];
  int npend, curidx, lastcall, nextcall;
  char (*mname)[40];      /* per value: the method name when the value is a method read that a call uses (the dispatch is made at the call) */
  int *mpc;               /* ... and the pc of that read */
  int kcls;               /* TRUST: 1 + the class (region class table) of `this`, known from the arm that expanded this instance */
  int ctp;                /* TRUST: 1 + the constant path that `this` is (an arm of a static method `G.m()`) */
  int ool;                /* TRUST: the arm is called out of line (a callee with a loop of its own runs its own compiled body), not expanded */
  int wrap;               /* a forwarding constructor `function () { this.m.apply(this, arguments); }`: its variable area is the two slots the pattern uses (see rg_wrapper) */
  int wr_uninit_pc, wr_put_pc, wr_read_pc;
  char wr_method[40];   /* the pcs of its local_uninit, put_local_s0 and the read of `this.m` */
  uint8_t rty;            /* F64: the type of this instance's result (RT_*; RT_ANY or 0 when it is a JSValue `res`) */
  uint8_t *ty, *rw;       /* F64 (region_fn_var.inc): the type of each value (RT_*), and whether a double was made by + - * neg inc dec (stored as the engine stores it, not normalised) */
} RInst;

typedef struct RGlob { char g[40], f[40]; int obj; } RGlob;   /* obj: the value is a function (a thru-call's X), held by the unit; else an int */

/* A construct (`new F(args)` of a global F) in the region: the object is owned by the region (`rn_K`) until it returns or leaves. */
#define RG_MAXC 256
typedef struct RCons { int inst, pc, kind; char gname[48]; char path[4][40]; int npath; const JSAotGenFunc *tg; int nn; char names[16][40]; int site, direct; unsigned seen; int esc; int stack, sk_pair, knock; unsigned pre; int nrbs; char rbs[8][40]; } RCons;   /* stack: lives in the region's C frame (kind 0 object, kind 1 array); sk_pair: the cons index of its array / its object, else -1 */

/* TRUST: a class of the region (AOT-REDESIGN.md, the class table): the allocation site, its constructor's global, and its fields in add order. At the region's first
   entry the init binds it once: the constructor (a fact on the global binding), its prototype (a fact on F.prototype), the class shape built from the prototype and
   the field list (qjs_aot_class_shape, held), and the shape's layout (a dependency of the word: an instance leaving the shape breaks it). Dispatch is a compare of
   the receiver's shape with these, field reads and writes are at the slots of the list; nothing is learned at run time. */
#define RG_MAXCL 24
/* nsh > 1: a class whose instances gain fields late (QJS_AOT_REGION_MSHAPE=1, opt-in): `ms` is its table of up to QJS_AOT_MAXSHAPES profile-observed shapes (fast.h QjsAotClassShapes); shape 0
   is the construction walk's final shape (`nn`/`names` above, rk_s_), the others extend it. nsh is 0 for every other class, whose emission is unchanged. */
typedef struct RCls { int site; char ctor[48]; int nn; char names[32][40]; int need_layout; int nsh; QjsAotClassShapes ms; } RCls;
#define RG_MAXGV 8
#define RG_MAXCP 64

/* An exit stub: the root's frame state (`root`, AOT_SET_B lines), and for an exit inside inlined code the chain of inlined frames to materialise (`rest`). */
typedef struct RXit { int pc, d, rpc; unsigned long long h; char *root, *rest; } RXit;
typedef struct RStr { char *p; size_t n, cap; } RStr;

typedef struct RCtx {
  FILE *o;
  const char *hex;
  int fi;
  RInst **in;             /* the instances, allocated one by one (the table grows) */
  int nin, nincap, nin_total;
  RGuard gd[RG_MAXG];
  int ngd;
  int nnative;   /* out-of-line native calls in the region (pass 1) */
  RGlob gl[RG_MAXK];
  int ngl;
  RXit *xit;
  int nxit, xitcap, bad;
  char pmn[64][40]; int npmn;   /* the method names the exit stubs re-read (RG_PM): a table, so the text in a cur[] slot is short whatever the name */
  int emit;
  int nsite;
  struct { int xi; char what[64]; } site[4096];
  int gseq;               /* the guard being used in this walk (the same order in both passes) */
  RCons cons[RG_MAXC];
  int ncons;
  int npop;               /* pops met in this walk */
  int ncmk;               /* loops whose back edge frees the values parked in the loop and not carried (the owned-list mark each keeps in rgmk_[]) */
  int cmk_ii[48], cmk_t[48];
  uint8_t park[256];       /* the instance (or one below it) has a site that parks a value in the owned list: only then does a back edge need to compact */
  char why[200];
  char ownres[256][24];   /* TRUST: the names of call results every path of whose call put in the owned list (held to the region's end) */
  int nownres;
  RStr ools;              /* TRUST: the frame states the out-of-line calls hold (their values must be set where their instances begin) */
  int nsc; int sc_site[96]; char sc_text[96][48]; char sc_dg[96][33];   /* TRUST: the constructor path (`NS.P`) of an allocation site whose constructor is not a global, from the construct that makes it */
  RCls cls[RG_MAXCL];     /* TRUST: the class table */
  int ncls;
  char gv[RG_MAXGV][40];  /* TRUST: globals read as values (their atoms are bound at init) */
  int ngv;
  char cp[RG_MAXCP][4][40];  /* TRUST: constant paths G.f1.f2.f3 (a global and own data properties), bound at init with a fact on every link */
  int cpn[RG_MAXCP], ncp;
  int cpk[RG_MAXCP]; int32_t cpv[RG_MAXCP];
  int cpi[RG_MAXCP], cpm[RG_MAXCP];   /* a path that must be the engine's intrinsic (id + 1, magic): Math.sqrt and the like (inlined) */
  /* TRUST, loop-invariant reads: the field names some store of the region writes, whether it changes an array's elements or length, whether it runs
     code out of line (any of which ends invariance); the values found invariant (C names) and the memo slots of the second pass */
  char mutn[96][40]; int nmut, arr_mut, opaque, mut_over;
  int amq[16], namq, amunk;   /* F64: the constant paths whose arrays the region changes (push, pop, element store), and whether some change is of an array that is not one */
  /* TRUST, hoisted memos: an invariant value whose origin is the root's `this` or a hoisted memo (name -> "f_this" / "rgm_K") is read once at the region's entry (a
     failing check there leaves for the generic body before anything ran), so the loop body has no flag test; one memo per (origin, kind, slot) */
  struct { char nm[24]; char ex[24]; } *orig; int norig, origcap;
  RStr hoist;
  struct { int inst, v; } skal[64]; int nskal;   /* the values that are a stack array (instance, value id) */
  int nstack;                                   /* stack objects (pairs) in this region */
  int loopinl;            /* TRUST: a callee with a loop of its own is expanded in place when it is small and the call has one target (else it is called out of line); the retry without it is region_emit's */
  char hk[256][120]; int hkm[256], nhk;
  char (*invn)[24]; int ninv, invcap; int nmemo;   /* TRUST: a path whose value the bundle shows is one int constant (the code uses it, the init checks it) */
} RCtx;

static RCtx *g_rgc;       /* the region being compiled (the field-position query registers the classes it relies on) */
static int rg_trust_on(void);

static int rg_cls_find(RCtx *c, const char *ctor) {
  int k;
  for (k = 0; k < c->ncls; k++) if (strcmp(c->cls[k].ctor, ctor) == 0) return k;
  return -1;
}
/* The class of allocation site `site` (constructor global `ctor`), added in the first walk; -1 when it cannot be bound once (no global constructor, a field
   list the analysis does not give in full, or a second walk that meets a class the first did not). */
/* Every class the region compiler has put in a class table, with its ordered fields: the driver names a layout row after the class (its constant path) so the unit builds the row against that class's closure. */
static struct { char ctor[48]; char names[32][40]; int nn; } g_rg_clreg[256];
static int g_rg_nclreg;
static void rg_clreg_add(const char *ctor, char (*names)[40], int nn) {
  int i, k;
  for (i = 0; i < g_rg_nclreg; i++) {
    if (g_rg_clreg[i].nn != nn || strcmp(g_rg_clreg[i].ctor, ctor) != 0) continue;
    for (k = 0; k < nn && strcmp(g_rg_clreg[i].names[k], names[k]) == 0; k++) {}
    if (k == nn) return;
  }
  if (g_rg_nclreg >= 256) return;
  snprintf(g_rg_clreg[g_rg_nclreg].ctor, 48, "%s", ctor);
  for (k = 0; k < nn; k++) snprintf(g_rg_clreg[g_rg_nclreg].names[k], 40, "%s", names[k]);
  g_rg_clreg[g_rg_nclreg].nn = nn;
  g_rg_nclreg++;
}
/* The constructor text of the one class registered with exactly these ordered fields; 0 when there is none or the list belongs to several. */
int region_class_path(char (*names)[40], int nn, char *out, size_t cap) {
  int i, k, found = -1;
  for (i = 0; i < g_rg_nclreg; i++) {
    if (g_rg_clreg[i].nn != nn) continue;
    for (k = 0; k < nn && strcmp(g_rg_clreg[i].names[k], names[k]) == 0; k++) {}
    if (k != nn) continue;
    if (found >= 0 && strcmp(g_rg_clreg[found].ctor, g_rg_clreg[i].ctor) != 0) return 0;
    found = i;
  }
  if (found < 0) return 0;
  snprintf(out, cap, "%s", g_rg_clreg[found].ctor);
  return 1;
}
static int rg_cls_add(RCtx *c, int site, const char *ctor, int nfields, int multi) {
  int k = rg_cls_find(c, ctor), j;
  RCls *K;
  if (k >= 0) return k;
  if (c->emit || ctor == NULL || !ctor[0] || strlen(ctor) >= sizeof(c->cls[0].ctor) || c->ncls >= RG_MAXCL || nfields < 0 || nfields > 32) return -1;
  K = &c->cls[c->ncls];
  memset(K, 0, sizeof(*K));
  K->site = site;
  snprintf(K->ctor, sizeof(K->ctor), "%s", ctor);
  for (j = 0; j < nfields; j++) if (!qjs_aot_bundle_site_field_name(site, j, K->names[j], sizeof(K->names[0]))) return -1;
  K->nn = nfields;
  if (multi) {
    /* a class that gains fields late: the profile's observed shapes (E1/E2); the base alone, or an ambiguous or over-full table, is not a multi-shape class */
    char hex[33], late[16][40];
    int nl, t;
    if (!qjs_aot_mshape_on() || !qjs_aot_bundle_site_ctor_digest(site, hex) || (nl = qjs_aot_bundle_class_late_names(site, late, 16)) <= 0) return -1;
    qjs_aot_prof_class_shapes(hex, K->names, K->nn, late, nl, &K->ms);
    if (getenv("QJS_AOT_REGION_WHY")) fprintf(stderr, "why: class %s: profile shapes %d status %d\n", K->ctor, K->ms.n, K->ms.status);
    if (K->ms.status != QJS_AOT_MS_OK || K->ms.n < 2) return -1;
    K->nsh = K->ms.n;
    for (t = 1; t < K->nsh; t++) rg_clreg_add(K->ctor, K->ms.names[t], K->ms.nn[t]);
  }
  rg_clreg_add(K->ctor, K->names, K->nn);
  return c->ncls++;
}
/* A receiver class qualifies for the table: one shape, a global constructor, all fields known. */
int qjs_aot_bundle_site_alloc(int site, const JSAotGenFunc **g, int *pc);
int qjs_aot_bundle_targets(const JSAotGenFunc *g, int pc, const JSAotGenFunc **out, int max);
static int rg_cpath(const Q2Func *fn, int v, char (*out)[40], int *n);
static int rg_base(const Q2Func *fn, Q2ValueId v);
static int rg_atom(const Q2Insn *in, char *out, size_t cap);
/* The class's constructor as the table names it: its global name, or (TRUST) the constant path `NS.P` of the construct that allocates the site. Cached per site. */
static const char *rg_clctor(const RCtx *c0, const QjsAotRecvClass *cl) {
  RCtx *c = (RCtx *)c0;
  const JSAotGenFunc *g;
  int j, pc, ii;
  Q2Func q;
  if (cl->ctor_global[0]) return cl->ctor_global;
  for (j = 0; j < c->nsc; j++) if (c->sc_site[j] == cl->site) return c->sc_text[j];
  if (!rg_trust_on() || getenv("QJS_AOT_REGION_NOCTORPATH") != NULL || c->nsc >= 96 || !qjs_aot_bundle_site_alloc(cl->site, &g, &pc)) return "";
  memset(&q, 0, sizeof(q));
  if (q2_build(g, &q) < 0 || !q.ok) { q2_free(&q); return ""; }
  for (ii = 0; ii < q.ninsns; ii++) {
    const Q2Insn *in = &q.insns[ii];
    char pth[4][40]; int pn = 0, k, o = 0, fits = 1;
    if (in->pc != pc) continue;
    if (in->rop != ROP_construct || in->nuse < 1 || !rg_cpath(&q, (int)rg_base(&q, in->use[0]), pth, &pn) || pn < 2) { if (getenv("QJS_AOT_REGION_WHY")) { int bb_ = (int)rg_base(&q, in->use[0]), di_ = bb_ >= 0 && bb_ < q.nvalues ? q.values[bb_].insn : -1; char rn_[40] = "?", an_[40] = ""; if (di_ >= 0) { e_rop_name((uint16_t)q.insns[di_].rop, rn_, sizeof(rn_)); rg_atom(&q.insns[di_], an_, sizeof(an_)); } fprintf(stderr, "why: class site %d allocated at %s pc %d: constructor is %s .%s (not a constant path of >=2 links)\n", cl->site, g->name[0] ? g->name : "<anon>", pc, rn_, an_); } break; }
    c->sc_site[c->nsc] = cl->site;
    c->sc_dg[c->nsc][0] = 0;
    { const JSAotGenFunc *tg_[2]; if (qjs_aot_bundle_targets(g, pc, tg_, 2) == 1) { int h_; for (h_ = 0; h_ < 16; h_++) snprintf(c->sc_dg[c->nsc] + 2 * h_, 3, "%02x", tg_[0]->digest[h_]); } }
    for (k = 0; k < pn && fits; k++) { int n_ = snprintf(c->sc_text[c->nsc] + o, 48 - o, "%s%s", k ? "." : "", pth[k]); if (n_ < 0 || o + n_ >= 48) fits = 0; else o += n_; }
    q2_free(&q);
    if (!fits) return "";
    return c->sc_text[c->nsc++];
  }
  q2_free(&q);
  return "";
}
static int rg_cls_ok(const RCtx *c, const QjsAotRecvClass *cl) {
  return cl->one_shape && rg_clctor(c, cl)[0] && cl->nfields >= 0 && cl->nfields <= 32;
}
static int rg_gv_add(RCtx *c, const char *name) {
  int q;
  for (q = 0; q < c->ngv; q++) if (strcmp(c->gv[q], name) == 0) return q;
  if (c->emit || c->ngv >= RG_MAXGV || strlen(name) >= sizeof(c->gv[0])) return -1;
  snprintf(c->gv[c->ngv], sizeof(c->gv[0]), "%s", name);
  return c->ngv++;
}
static int rg_cp_add(RCtx *c, char (*names)[40], int n) {
  int q, j;
  for (q = 0; q < c->ncp; q++) {
    if (c->cpn[q] != n) continue;
    for (j = 0; j < n && strcmp(c->cp[q][j], names[j]) == 0; j++) {}
    if (j == n) return q;
  }
  if (c->emit || c->ncp >= RG_MAXCP) return -1;
  for (j = 0; j < n; j++) snprintf(c->cp[c->ncp][j], 40, "%s", names[j]);
  c->cpn[c->ncp] = n;
  return c->ncp++;
}

static int rg_mut(RCtx *c, const char *nm) {
  int i;
  if (c->mut_over) return 1;
  for (i = 0; i < c->nmut; i++) if (strcmp(c->mutn[i], nm) == 0) return 1;
  return 0;
}
static void rg_mut_add(RCtx *c, const char *nm) {
  if (c->emit || rg_mut(c, nm)) return;
  if (c->nmut >= 96) { c->mut_over = 1; return; }
  snprintf(c->mutn[c->nmut++], 40, "%s", nm);
}
static int rg_inv_on(RCtx *c) { static int off = -1; if (off < 0) off = getenv("QJS_AOT_REGION_NOINV") != NULL; return !off && rg_trust_on() && c->emit && !c->opaque; }
static int rg_inv_has(RCtx *c, const char *nm) {
  int i;
  if (strcmp(nm, "f_this") == 0) return 1;   /* the root's `this` is the same object on every pass */
  for (i = 0; i < c->ninv; i++) if (strcmp(c->invn[i], nm) == 0) return 1;
  return 0;
}
static void rg_inv_add(RCtx *c, const char *nm) {
  if (!rg_inv_on(c) || rg_inv_has(c, nm) || strlen(nm) >= 24) return;
  if (c->ninv >= c->invcap) { c->invcap = c->invcap ? c->invcap * 2 : 64; c->invn = realloc(c->invn, (size_t)c->invcap * sizeof(*c->invn)); if (c->invn == NULL) abort(); }
  snprintf(c->invn[c->ninv++], 24, "%s", nm);
}

static void rs_add(RStr *s, const char *fmt, ...);
/* a vkit knock-out: the hoisted reads skip the region-wide check that nothing in the region writes the field / the array */
static int rg_hoist_nomut(void) { static int v = -1; if (v < 0) v = getenv("QJS_AOT_REGION_HOISTNOMUT") != NULL; return v; }
static int rg_hoist_on(RCtx *c) { static int off = -1; if (off < 0) off = getenv("QJS_AOT_REGION_NOHOIST") != NULL; return !off && rg_inv_on(c); }
static const char *rg_orig(RCtx *c, const char *nm) {
  int i;
  if (strcmp(nm, "f_this") == 0) return "f_this";
  for (i = 0; i < c->norig; i++) if (strcmp(c->orig[i].nm, nm) == 0) return c->orig[i].ex;
  return NULL;
}
static void rg_orig_set(RCtx *c, const char *nm, const char *ex) {
  int i;
  if (!rg_hoist_on(c) || ex == NULL || strlen(nm) >= 24 || strlen(ex) >= 24) return;
  for (i = 0; i < c->norig; i++) if (strcmp(c->orig[i].nm, nm) == 0) { snprintf(c->orig[i].ex, 24, "%s", ex); return; }
  if (c->norig >= c->origcap) { c->origcap = c->origcap ? c->origcap * 2 : 64; c->orig = realloc(c->orig, (size_t)c->origcap * sizeof(*c->orig)); if (c->orig == NULL) abort(); }
  snprintf(c->orig[c->norig].nm, 24, "%s", nm); snprintf(c->orig[c->norig].ex, 24, "%s", ex); c->norig++;
}
/* The memo of a hoisted read: 1 when the key was met before (*k is its memo), else 0 with a new memo in *k whose entry code the caller adds to c->hoist. -1: no room. */
static int rg_hk(RCtx *c, const char *key, int *k) {
  int i;
  for (i = 0; i < c->nhk; i++) if (strcmp(c->hk[i], key) == 0) { *k = c->hkm[i]; return 1; }
  if (c->nhk >= 256 || strlen(key) >= sizeof(c->hk[0])) return -1;
  snprintf(c->hk[c->nhk], sizeof(c->hk[0]), "%s", key);
  *k = c->hkm[c->nhk++] = c->nmemo++;
  return 0;
}
/* rg_hk for a hoist the region-wide check refuses (only the vkit knock-out build, QJS_AOT_REGION_HOISTNOMUT, makes one): its entry code leaves for the generic body unless
   the run sets QJS_AOT_REGION_HOISTKNOCK, so the case's ordinary run is right and the knocked run shows what the check protects. */
static int rg_hk_unsafe(RCtx *c, const char *key, int *k, int unsafe) {
  int nw = rg_hk(c, key, k);
  if (nw == 0 && unsafe) rs_add(&c->hoist, "    if (!getenv(\"QJS_AOT_REGION_HOISTKNOCK\")) return aot_g_%s(ctx, f, fr, entry_pc);\n", c->hex);
  return nw;
}
/* Whether reads of the fixed slots of instance I's receiver may be made at the region's entry: its shape is checked there (the root's `this` of a known class, or a
   receiver whose dispatch check was hoisted). */
static int rg_hver(RCtx *c, RInst *I) {
  const char *os;
  if (I->id == 0 || I->hver) return I->id == 0 ? c->in[0]->kcls > 0 : 1;
  os = rg_orig(c, I->self);
  return os != NULL && strcmp(os, "f_this") == 0 && c->in[0]->kcls > 0;   /* the root's `this`, checked at the entry */
}

/* A fixed-slot read `dst = base.f` (TRUST): when base is the same object on every pass of the region (an invariant value) and no store of the region
   writes a field named f, the value is read once and kept in a local (the C compiler cannot tell the region's other stores from this slot, so it
   would reload it after each); the value is invariant too. */
static void rg_inv_read(RCtx *c, RInst *I, FILE *o, int dv, const char *base, const char *off, const char *nm) {
  char d_[24];
  snprintf(d_, sizeof(d_), "%sv%d", I->pfx, dv);
  if (getenv("QJS_AOT_INV_DBG")) fprintf(stderr, "inv %s read %s.%s on=%d has=%d mut=%d opaque=%d ninv=%d\n", c->hex, base, nm, rg_inv_on(c), rg_inv_has(c, base), rg_mut(c, nm), c->opaque, c->ninv);
  if (rg_hoist_on(c) && (I->curloop || I->ploop) && rg_inv_has(c, base) && (rg_hoist_nomut() || !rg_mut(c, nm)) && strstr(off, "gix_") == NULL && rg_hver(c, I)) {
    const char *ob = rg_orig(c, base), *os = rg_orig(c, I->self);
    char key[120];
    int k, nw;
    if (ob != NULL && os != NULL && strcmp(ob, os) == 0) {
      snprintf(key, sizeof(key), "F|%s|%s", ob, off);
      nw = rg_hk(c, key, &k);
      if (nw >= 0) {
        if (nw == 0) {
          if (rg_mut(c, nm)) rs_add(&c->hoist, "    if (!getenv(\"QJS_AOT_REGION_HOISTKNOCK\")) return aot_g_%s(ctx, f, fr, entry_pc);   /* the knock-out build: a hoist the region-wide check refuses */\n", c->hex);
          rs_add(&c->hoist, "    rgm_%d = *q2_slotp(%s, %s); rgmv_%d = 1;\n", k, ob, off, k);
        }
        fprintf(o, "        %s = rgm_%d;\n", d_, k);
        rg_inv_add(c, d_);
        snprintf(key, sizeof(key), "rgm_%d", k);
        rg_orig_set(c, d_, key);
        return;
      }
    }
  }
  if (rg_inv_on(c) && (I->curloop || I->ploop) && rg_inv_has(c, base) && !rg_mut(c, nm)) {
    int k = c->nmemo++;
    fprintf(o, "        if (__builtin_expect(!rgmv_%d, 0)) { rgm_%d = *q2_slotp(%s, %s); rgmv_%d = 1; }\n        %s = rgm_%d;\n", k, k, base, off, k, d_, k);
    rg_inv_add(c, d_);
  } else {
    fprintf(o, "        %s = *q2_slotp(%s, %s);\n", d_, base, off);
    if (rg_inv_on(c) && rg_inv_has(c, base) && !rg_mut(c, nm)) rg_inv_add(c, d_);   /* the same value on every pass, read where it is not worth keeping */
  }
}

static int rg_any_construct(RCtx *c) {
  int k;
  for (k = 0; k < c->ncons; k++) if (c->cons[k].kind != 2) return 1;   /* kind 2 is an element store's slot, not an object made */
  return 0;
}
/* Whether some instance stores a value the profile saw as a non-integer number into a field or element: the region keeps a stored number boxed in its slot, while the
   ordinary body keeps a double in a register. */
static int rg_stores_double(RCtx *c) {
  int k, i;
  for (k = 0; k < c->nin; k++) {
    const Q2Func *q;
    if (c->in[k] == NULL) continue;
    q = &c->in[k]->q;
    for (i = 0; i < q->ninsns; i++) {
      const Q2Insn *in = &q->insns[i];
      int vi = -1;
      if (in->rop == ROP_put_field || in->rop == ROP_put_field_xfer) vi = in->nuse >= 2 ? (int)in->use[1] : -1;
      else if (in->rop == ROP_put_element || in->rop == ROP_put_element_xfer) vi = in->nuse >= 3 ? (int)in->use[2] : -1;
      if (vi >= 0 && vi < q->nvalues && (q->values[vi].ptypes & Q2_T_NUM)) return 1;
    }
  }
  return 0;
}
static int rg_any_ool(RCtx *c) {
  int k;
  for (k = 0; k < c->nin; k++) if (c->in[k] != NULL && c->in[k]->ool) return 1;
  return 0;
}
static int rg_any_dispatch(RCtx *c) {
  int g;
  for (g = 0; g < c->ngd; g++) if (c->gd[g].dispatch) return 1;
  return 0;
}

/* The C text of constant path q's value: the int the bundle stores there (checked at init), or the value bound at init. */
static int rg_f64_on(void);
static const char *rg_cp_text(RCtx *c, int q, char *buf, size_t cap) {
  if (q >= 0 && q < c->ncp && c->cpk[q]) snprintf(buf, cap, "JS_MKVAL(JS_TAG_INT, %d)", (int)c->cpv[q]);
  else if (c->emit && q >= 0 && q < c->ncp && rg_f64_on() && getenv("QJS_AOT_REGION_NOCPLOCAL") == NULL) snprintf(buf, cap, "rgq_%d", q);   /* a local copy made at the entry (the table is memory a call in the loop may change) */
  else snprintf(buf, cap, "rgp_%s[%d]", c->hex, q);
  return buf;
}

static int rg_fail(RCtx *c, const char *fmt, ...) {
  va_list ap;
  if (c->why[0] == 0) {
    va_start(ap, fmt);
    vsnprintf(c->why, sizeof(c->why), fmt, ap);
    va_end(ap);
  }
  return 0;
}

static void rg_cstr(FILE *o, const char *s) {
  fputc('"', o);
  for (; *s; s++) {
    if (*s == '"' || *s == '\\') fputc('\\', o);
    fputc(*s, o);
  }
  fputc('"', o);
}

static int rg_atom(const Q2Insn *in, char *out, size_t cap) {
  int a;
  for (a = 0; a < in->narg && in->arg[a].kind != 'a'; a++) {}
  if (a >= in->narg) return 0;
  return qjs_aot_bundle_atom_name((uint32_t)in->arg[a].val, out, cap);
}

static void rg_hexof(const JSAotGenFunc *g, char hex[33]) {
  int i;
  for (i = 0; i < 16; i++) snprintf(hex + 2 * i, 3, "%02x", g->digest[i]);
}

/* the guard of a value (created on first use), in a stable order across the two passes */
static int rg_guard(RCtx *c, int inst, int val, int pc, int dispatch, const char *method) {
  int i;
  for (i = 0; i < c->ngd; i++)
    if (c->gd[i].inst == inst && c->gd[i].val == val && c->gd[i].pc == pc) return i;
  if (c->ngd >= RG_MAXG) return -1;
  memset(&c->gd[c->ngd], 0, sizeof(RGuard));
  c->gd[c->ngd].inst = inst;
  c->gd[c->ngd].val = val;
  c->gd[c->ngd].pc = pc;
  if (dispatch) { c->gd[c->ngd].dispatch = 1; snprintf(c->gd[c->ngd].method, sizeof(c->gd[c->ngd].method), "%s", method); }
  return c->ngd++;
}

static int rg_addreq(RReq *l, int *n, const char *name, int slot, int write) {
  int i;
  for (i = 0; i < *n; i++)
    if (strcmp(l[i].name, name) == 0) {
      if (l[i].slot != slot) return 0;
      l[i].write |= write;
      return 1;
    }
  if (*n >= RG_MAXQ || strlen(name) >= sizeof(l[0].name)) return 0;
  snprintf(l[*n].name, sizeof(l[0].name), "%s", name);
  l[*n].slot = slot;
  l[*n].write = write;
  (*n)++;
  return 1;
}

/* A value is `this` of an instance created by a dispatch: its field requirements belong to that guard and target. */
/* E0 (QJS_AOT_REGION_MEASURE=1): the walk does not refuse an unproven field position; it classifies the access (S: the receiver is an object this region is building, its shape is known by construction;
   D: any other receiver, its shape would be dispatched) and counts the distinct (instance, value) pairs of the dispatched ones, which is what a one-test-per-object CSE would leave. Nothing is emitted. */
static int g_meas_on = -1, g_meas_s, g_meas_d, g_meas_pairs, g_meas_np;
static struct { int inst, val; } g_meas_pr[512];
static char g_meas_nm[64][48];
static int g_meas_nms[64], g_meas_nmd[64], g_meas_nn;
/* E0 source tracing: the objects a receiver value can be, followed through moves, block parameters (phis), inlined calls' returns and inlined arguments. The sources are constructs of this
   region (a shape known by construction: 1 source = S, several = J, an arm id would pick the shape); anything else (the root's arguments/this, a field read, an opaque call) is D. */
typedef struct { int ns, unk, nv, why, ln; int src[16][2]; int vis[64][2]; } RgSrc;
static void rg_ms_trace(RCtx *c, int n, int v, int depth, RgSrc *r) {
  const RInst *A;
  const Q2Func *fn;
  const Q2Value *V;
  const Q2Insn *d;
  int k, j;
  if (r->unk) return;
  if (n < 0 || n >= c->nin || c->in[n] == NULL || v < 0 || depth > 24) { { r->unk = 1; r->ln = __LINE__; } return; }
  A = c->in[n]; fn = &A->q;
  if (v >= fn->nvalues) { { r->unk = 1; r->ln = __LINE__; } return; }
  for (k = 0; k < r->nv; k++) if (r->vis[k][0] == n && r->vis[k][1] == v) return;
  if (r->nv < 64) { r->vis[r->nv][0] = n; r->vis[r->nv][1] = v; r->nv++; } else { { r->unk = 1; r->ln = __LINE__; } return; }
  V = &fn->values[v];
  if (V->insn < 0) {
    int b, s, found = 0;
    if (V->param_of < 0) { { r->unk = 1; r->ln = __LINE__; } return; }
    for (b = 0; b < fn->nblocks; b++) {
      const Q2Block *B = &fn->blocks[b];
      if (fn->rpo_pos != NULL && fn->rpo_pos[b] < 0) continue;
      for (s = 0; s < B->nsucc; s++) {
        if (B->succ_block[s] != V->block || V->param_of >= B->nedge_args[s]) continue;
        found = 1;
        rg_ms_trace(c, n, (int)B->edge_arg[s][V->param_of], depth + 1, r);
      }
    }
    if (!found) { r->unk = 1; r->ln = __LINE__; }
    return;
  }
  d = &fn->insns[V->insn];
  if ((d->rop == ROP_move || d->rop == ROP_move_s || d->rop == ROP_move_xfer) && d->nuse >= 1) { rg_ms_trace(c, n, (int)d->use[0], depth + 1, r); return; }
  if (d->rop == ROP_construct) {
    for (k = 0; k < r->ns; k++) if (r->src[k][0] == n && r->src[k][1] == v) return;
    if (r->ns < 16) { r->src[r->ns][0] = n; r->src[r->ns][1] = v; r->ns++; } else { r->unk = 1; r->ln = __LINE__; }
    return;
  }
  if (d->rop == ROP_call_method || d->rop == ROP_call) {
    int cnt = 0, kk;
    for (kk = 1; kk < c->nin; kk++) {
      int ret;
      if (c->in[kk] == NULL || c->in[kk]->par != n || c->in[kk]->par_call_pc != d->pc) continue;
      cnt++;
      if (c->in[kk]->ool || c->in[kk]->g == NULL) { { r->unk = 1; r->ln = __LINE__; } return; }
      for (ret = 0; ret < c->in[kk]->q.ninsns; ret++)
        if (c->in[kk]->q.insns[ret].rop == ROP_return && c->in[kk]->q.insns[ret].nuse >= 1) rg_ms_trace(c, kk, (int)c->in[kk]->q.insns[ret].use[0], depth + 1, r);
    }
    if (cnt == 0) { { r->unk = 1; r->ln = __LINE__; } r->why = 100000 + d->rop; }
    return;
  }
  if (d->rop == ROP_get_arg || d->rop == ROP_get_arg2) {
    int ak, pn, pm;
    if (n == 0) { { r->unk = 1; r->ln = __LINE__; } r->why = 200000 + d->rop; return; }
    ak = d->rop == ROP_get_arg2 && d->ndef >= 2 && d->def[1] == (Q2ValueId)v ? (d->narg >= 4 ? (int)d->arg[3].val : -1) : (d->narg >= 2 ? (int)d->arg[1].val : -1);
    if (ak < 0 || ak >= A->nargs || sscanf(A->arg[ak], "r%d_v%d", &pn, &pm) != 2) { { r->unk = 1; r->ln = __LINE__; } return; }
    rg_ms_trace(c, pn, pm, depth + 1, r);
    return;
  }
  if (d->rop == ROP_load_this && n > 0) {
    int pn, pm;
    if (sscanf(A->self, "r%d_v%d", &pn, &pm) != 2) { { r->unk = 1; r->ln = __LINE__; } return; }
    rg_ms_trace(c, pn, pm, depth + 1, r);
    return;
  }
  (void)j;
  { r->unk = 1; r->ln = __LINE__; } r->why = d->rop;
}
static int g_meas_kind[3];   /* S, J, D accesses over all fields */
static void rg_measure_note(RCtx *c, RInst *I, int val, const char *name) {
  int k, kind;
  RgSrc r;
  memset(&r, 0, sizeof(r));
  rg_ms_trace(c, I->id, val, 0, &r);
  kind = (r.unk || r.ns == 0) ? 2 : r.ns == 1 ? 0 : 1;
  if (kind == 2 && getenv("QJS_AOT_REGION_WHY")) { char rn_[48] = "-"; if (r.why % 100000 > 0) e_rop_name((uint16_t)(r.why % 100000), rn_, sizeof(rn_)); fprintf(stderr, "measure: D .%s in inst %d v%d: unk=%d line=%d ns=%d why=%d(%s)\n", name, I->id, val, r.unk, r.ln, r.ns, r.why / 100000, rn_); }
  g_meas_kind[kind]++;
  for (k = 0; k < g_meas_nn && strcmp(g_meas_nm[k], name) != 0; k++) {}
  if (k == g_meas_nn && g_meas_nn < 64) { snprintf(g_meas_nm[g_meas_nn], 48, "%s", name); g_meas_nms[g_meas_nn] = g_meas_nmd[g_meas_nn] = 0; g_meas_nn++; }
  if (kind != 2) { g_meas_s++; if (k < g_meas_nn) g_meas_nms[k]++; return; }
  g_meas_d++; if (k < g_meas_nn) g_meas_nmd[k]++;
  for (k = 0; k < g_meas_np; k++) if (g_meas_pr[k].inst == I->id && g_meas_pr[k].val == val) return;
  if (g_meas_np < 512) { g_meas_pr[g_meas_np].inst = I->id; g_meas_pr[g_meas_np].val = val; g_meas_np++; }
}
static void rg_measure_report(const char *hex, const JSAotGenFunc *g) {
  int k;
  if (g_meas_s + g_meas_d == 0) return;
  fprintf(stderr, "measure: root %.8s [%s@%d]: %d accesses with an unproven position: %d on objects built here (static shape), %d dispatched; distinct (instance,object) pairs = %d dispatch tests without CSE across blocks\n",
          hex, g->name[0] ? g->name : "<anon>", g->line, g_meas_s + g_meas_d, g_meas_s, g_meas_d, g_meas_np);
  { extern void qjs_aot_prof_site_dump(const uint8_t dg[16], const char *tag); qjs_aot_prof_site_dump(g->digest, hex); }
  fprintf(stderr, "measure: kinds S(one source) %d J(several constructs) %d D(dispatched) %d\n", g_meas_kind[0], g_meas_kind[1], g_meas_kind[2]);
  for (k = 0; k < g_meas_nn; k++) fprintf(stderr, "measure:   .%s static %d dispatched %d\n", g_meas_nm[k], g_meas_nms[k], g_meas_nmd[k]);
}
static int rg_note_field(RCtx *c, RInst *I, int val, const char *name, int slot, int write, int is_this) {
  int gi, t = 0;
  if (g_meas_on < 0) g_meas_on = getenv("QJS_AOT_REGION_MEASURE") != NULL;
  if (g_meas_on && !c->emit && rg_trust_on()) { rg_measure_note(c, I, val, name); return 1; }
  /* TRUST: a position the analysis does not prove would be learned from a shape at run time; the region is refused instead */
  if (rg_trust_on()) {
    if (getenv("QJS_AOT_REGION_WHY")) { int a_ = I->id, d_ = 0; fprintf(stderr, "why: note .%s in", name); while (a_ >= 0 && d_++ < 16) { char hx_[33]; rg_hexof(c->in[a_]->g, hx_); fprintf(stderr, " <- %.6s(%s@%d)%s", hx_, c->in[a_]->g->name, c->in[a_]->g->line, c->in[a_]->fresh ? "[fresh]" : ""); a_ = c->in[a_]->par; } fprintf(stderr, "\n"); }
    return rg_fail(c, ".%s: a field position the analysis does not prove (it would be learned at run time)", name);
  }
  if (is_this && I->owner_g >= 0) {
    gi = I->owner_g;
    t = I->owner_t;
    return rg_addreq(c->gd[gi].req[t], &c->gd[gi].nreq[t], name, slot, write);
  }
  gi = rg_guard(c, I->id, val, -1, 0, NULL);
  if (gi < 0) return 0;
  return rg_addreq(c->gd[gi].common, &c->gd[gi].ncommon, name, slot, write);
}

/* Whether a value is the instance's `this` (a load_this, or the copy get_this_field leaves). */
static int rg_is_this(const Q2Func *fn, Q2ValueId v) {
  int di;
  const Q2Insn *d;
  if ((int32_t)v < 0 || (int32_t)v >= fn->nvalues) return 0;
  di = fn->values[v].insn;
  if (di < 0 || di >= fn->ninsns) return 0;
  d = &fn->insns[di];
  if (d->rop == ROP_load_this) return 1;
  if ((d->rop == ROP_get_this_field || d->rop == ROP_get_this_field2) && d->ndef >= 2 && d->def[1] == v) return 1;
  if ((d->rop == ROP_move || d->rop == ROP_move_s || d->rop == ROP_move_xfer) && d->nuse >= 1) return rg_is_this(fn, d->use[0]);
  return 0;
}

static void rs_add(RStr *s, const char *fmt, ...) {
  va_list ap;
  int k;
  for (;;) {
    size_t room = s->p != NULL ? s->cap - s->n : 0;
    va_start(ap, fmt);
    k = vsnprintf(s->p != NULL ? s->p + s->n : NULL, room, fmt, ap);
    va_end(ap);
    if (k < 0) return;
    if (s->p != NULL && (size_t)k < room) { s->n += (size_t)k; return; }
    s->cap = (s->n + (size_t)k + 1) * 2;
    s->p = realloc(s->p, s->cap);
    if (s->p == NULL) abort();
    s->p[s->n] = 0;
  }
}

/* The instance table grows: an instance is allocated when it is first asked for and never moves. */
static RInst *rg_newinst(RCtx *c, int i) {
  if (i >= c->nincap) {
    int ncap = c->nincap ? c->nincap * 2 : 32, k;
    c->in = realloc(c->in, (size_t)ncap * sizeof(RInst *));
    if (c->in == NULL) abort();
    for (k = c->nincap; k < ncap; k++) c->in[k] = NULL;
    c->nincap = ncap;
  }
  if (c->in[i] == NULL) {
    c->in[i] = calloc(1, sizeof(RInst));
    if (c->in[i] == NULL) abort();
    c->in[i]->par = -1;
  }
  return c->in[i];
}

static void rg_hexof(const JSAotGenFunc *g, char hex[33]);

/* The exit stub for a leave at `pc` of instance `ii` (made on first use; returns its number). The frame state is every instance on the path from the root to ii:
   the root's registers are written to the real frame; each inlined frame is built (outermost first) with its function, receiver, arguments and registers, and the
   innermost continues in its generic body at `pc`, each outer one after the call it was in. This is what V8 does at a deopt, for every depth. */
/* The pending-pair stack with exit-time materialisation is the default (QJS_AOT_REGION_NOMATERIALISE=1 turns it off: the vkit knock). It cost +47% on deltablue while the RG_PM text overflowed the 40-char cur[] slot and refused regions silently (c->bad); the names now live in a per-region table (c->pmn) and an overflow aborts the generator. */
static int rg_pstack(void) { return getenv("QJS_AOT_REGION_NOMATERIALISE") == NULL; }

/* A method read whose call is the instruction call_idx joins the pending pairs. */
static int rg_pend_push(RInst *I, const Q2Func *fn, int ridx, int callee, int call_idx, const char *name, int pc, int this_val, const char *recv) {
  struct RPend *e;
  int reg = callee >= 0 ? fn->values[callee].reg : -1;
  if (I->npend >= 6) return 0;
  e = &I->pend[I->npend++];
  e->end = call_idx + 1; e->ridx = ridx; e->pc = pc; e->creg = reg >= 0 && reg < 256 ? reg : -1;
  e->treg = this_val >= 0 && fn->values[this_val].reg >= 0 && fn->values[this_val].reg < 256 ? fn->values[this_val].reg : -1;
  snprintf(e->name, 40, "%s", name);
  snprintf(e->recv, 40, "%s", recv);
  if (this_val >= 0) snprintf(e->this_t, 40, "%s", recv); else e->this_t[0] = 0;
  return 1;
}

static int rg_xit(RCtx *c, int ii, int pc) {
  int chain[16], d = 0, k, i, j, r, rpc, total = 0, off, nlv;
  RInst *I0 = c->in[0], *C1 = NULL;
  RStr rt = {0}, rs = {0};
  unsigned long long h = 1469598103934665603ull;
  const char *cp;
  RXit *x;
  for (k = ii; k > 0; k = c->in[k]->par) {
    if (d >= 15) { c->bad = 1; return 0; }
    chain[d++] = k;
  }
  for (i = 0; i < d / 2; i++) { k = chain[i]; chain[i] = chain[d - 1 - i]; chain[d - 1 - i] = k; }
  if (d > 0) C1 = c->in[chain[0]];
  rpc = d ? C1->par_pc_after : pc;
  if (c->nstack > 0) {
    /* a stack object alive at this exit moves to the heap first (its references move: qjs_aot_stack_migrate); sk_ho / sk_ha are the heap object and Array, and every
       name below is read through SKFIX, which maps a stack block to them. The region then owns the heap objects in rnh_ (freed with the rest at the stub's end);
       AOT_SET_B takes the frame's own reference to each name, as for any other object. */
    int k0;
    /* the vkit knock-out build (QJS_AOT_REGION_NOMIGRATE at generation): the stub migrates unless the run sets QJS_AOT_REGION_MIGKNOCK, which leaves the stack object un-migrated */
    if (getenv("QJS_AOT_REGION_NOMIGRATE") != NULL) rs_add(&rt, "    if (!getenv(\"QJS_AOT_REGION_MIGKNOCK\")) {\n");
    for (k0 = 0; k0 < c->ncons; k0++)
      if (c->cons[k0].stack && c->cons[k0].kind == 0) {
        int k1 = c->cons[k0].sk_pair;
        if (k1 >= 0)
          rs_add(&rt, "    if (sk_%d || sk_%d) {\n      if (!qjs_aot_stack_migrate(ctx, sk_%d ? SKV_%d : JS_UNDEFINED, sk_%d ? SKV_%d : JS_UNDEFINED, &sk_ho, &sk_ha)) { RG_FREE_OWNED(); return JS_EXCEPTION; }\n      rnh_%d = sk_ho; rnh_%d = sk_ha; sk_%d = 0; sk_%d = 0;\n    }\n",
                  k0, k1, k0, k0, k1, k1, k0, k1, k0, k1);
        else
          rs_add(&rt, "    if (sk_%d) {\n      if (!qjs_aot_stack_migrate(ctx, SKV_%d, JS_UNDEFINED, &sk_ho, &sk_ha)) { RG_FREE_OWNED(); return JS_EXCEPTION; }\n      rnh_%d = sk_ho; sk_%d = 0;\n    }\n", k0, k0, k0, k0);
      }
    if (getenv("QJS_AOT_REGION_NOMIGRATE") != NULL) rs_add(&rt, "    }\n");
  }
  /* the pairs still pending in the arguments (`a.f(b.g(x))`): each one's callee and receiver registers get their values for this exit (the callee read again from the receiver, the
     method being a fact of the word), except the pair the exit resumes at the read of */
  struct { RInst *I; int reg; char old[40]; } pmsave[24];
  int npm = 0;
  if (rg_pstack()) {
    int ci;
    for (ci = -1; ci < d; ci++) {
      RInst *X = ci < 0 ? I0 : c->in[chain[ci]];
      int pj;
      if (ci < 0 && d == 0) {}
      for (pj = 0; pj < X->npend; pj++) {
        struct RPend *e = &X->pend[pj];
        int innermost = (ci < 0 && d == 0) || (ci == d - 1 && d > 0);
        if (e->end <= X->curidx + 1) continue;
        if (innermost && e->pc == pc) continue;
        if (npm + 2 > 24) { c->bad = 1; break; }
        if (e->creg >= 0) {
          pmsave[npm].I = X; pmsave[npm].reg = e->creg; memcpy(pmsave[npm].old, X->cur[e->creg], 40); npm++;
          { int pi_; for (pi_ = 0; pi_ < c->npmn && strcmp(c->pmn[pi_], e->name) != 0; pi_++) {}
            if (pi_ == c->npmn) { if (pi_ >= 64) { fprintf(stderr, "qjsaot: FATAL more than 64 distinct pending-pair method names in one region\n"); abort(); } snprintf(c->pmn[pi_], 40, "%s", e->name); c->npmn++; }
            if (snprintf(X->cur[e->creg], 40, "RG_PM(%s,%d)", e->recv, pi_) >= 40) { fprintf(stderr, "qjsaot: FATAL pending-pair callee text overflows the 40-char slot: RG_PM(%s,%d)\n", e->recv, pi_); abort(); } }
        }
        if (e->treg >= 0 && e->this_t[0]) {
          pmsave[npm].I = X; pmsave[npm].reg = e->treg; memcpy(pmsave[npm].old, X->cur[e->treg], 40); npm++;
          snprintf(X->cur[e->treg], 40, "%s", e->this_t);
        }
      }
    }
  }
  for (r = 0; r < 256; r++) if (I0->cur[r][0]) rs_add(&rt, c->nstack > 0 ? "    AOT_SET_B(ctx, fr, %d, SKFIX(%s));\n" : "    AOT_SET_B(ctx, fr, %d, %s);\n", r * 16, I0->cur[r]);
  if (d > 0) {
    for (j = 0; j < d; j++) {
      RInst *J = c->in[chain[j]];
      nlv = 0;
      for (r = 0; r < 256; r++) if (J->cur[r][0]) nlv++;
      total += 1 + J->nargs + nlv;
    }
    /* a vkit knob: leave at the root's call as the replaying design did (no frames), so the effects the callee had made are made twice */
    rs_add(&rs, "    if (__builtin_expect(rg_nof_%s(), 0)) { RG_FREE_OWNED(); E_RET_G(%s, %d); }\n", c->hex, c->hex, C1->par_read_pc);
    rs_add(&rs, "    {\n      JSValue rr_, dv_[%d]; JSObject *cos_[%d];\n", total, d);
    if (C1->par_kind == 1) rs_add(&rs, c->nstack > 0 ? "      AOT_SET_B(ctx, fr, %d, SKFIX(rn_%d));\n" : "      AOT_SET_B(ctx, fr, %d, rn_%d);\n", C1->par_dst * 16, C1->par_cons);
    rs_add(&rs, "      *(const uint8_t **)((uint8_t *)f + QJS_AOT_OFF_REGRUN_CAP_IP) = *(const uint8_t *const *)(*(uint8_t **)((uint8_t *)f + QJS_AOT_OFF_REGRUN_B) + QJS_AOT_OFF_FNBC_REGVM_CODE) + %d;\n", C1->par_call_pc);
    off = 0;
    for (j = 0; j < d; j++) {
      RInst *J = c->in[chain[j]];
      int n = 0;
      rs_add(&rs, c->nstack > 0 && !(J->wrap && getenv("QJS_AOT_REGION_NOWRAPSKFIX") != NULL) ? "      cos_[%d] = %s;\n      dv_[%d] = SKFIX(%s);\n" : "      cos_[%d] = %s;\n      dv_[%d] = %s;\n", j, J->co, off, J->self);   /* NOWRAPSKFIX: a vkit knock-out (a wrapper's slot 0 would hold the stale stack pointer) */
      for (i = 0; i < J->nargs; i++) rs_add(&rs, c->nstack > 0 ? "      dv_[%d] = SKFIX(%s);\n" : "      dv_[%d] = %s;\n", off + 1 + i, J->arg[i]);
      nlv = 0;
      for (r = 0; r < 256; r++) if (J->cur[r][0]) { rs_add(&rs, c->nstack > 0 ? "      dv_[%d] = SKFIX(%s);\n" : "      dv_[%d] = %s;\n", off + 1 + J->nargs + nlv, J->cur[r]); nlv++; }
      rs_add(&rs, "      static const uint16_t of_%d[] = {", j);
      for (r = 0; r < 256; r++) if (J->cur[r][0]) rs_add(&rs, "%s%d", n++ ? ", " : " ", r * 16);
      if (n == 0) rs_add(&rs, " 0");
      rs_add(&rs, " };\n");
      off += 1 + J->nargs + nlv;
    }
    rs_add(&rs, "      static const RgFC fc_[%d] = {\n", d);
    for (j = 0; j < d; j++) {
      RInst *J = c->in[chain[j]], *N = j + 1 < d ? c->in[chain[j + 1]] : NULL;
      char hx[33];
      rg_hexof(J->g, hx);
      nlv = 0;
      for (r = 0; r < 256; r++) if (J->cur[r][0]) nlv++;
      {
        /* a forwarding constructor's frame has its two-slot variable area: slot 0 holds `this` once the put_local_s0 has run, slot 1 is uninitialised once the local_uninit has: by the pc the generic body
           resumes at (static, not a run-time flag). QJS_AOT_REGION_NOWRAPVARS (a vkit knock-out build): the area is not built. */
        int rj_ = N ? N->par_pc_after : pc, nv_ = J->wrap && getenv("QJS_AOT_REGION_NOWRAPVARS") == NULL ? J->g->var_count : 0;
        rs_add(&rs, "        { aot_g_%s, %du, %du, %d, QJS_AOT_LIGHT_PREFIX_SLOTS + %d + Q2_FR_%s, %d, %d, %d, %d, %d, %d, %d, of_%d },\n", hx,
               rj_, N ? N->par_call_pc : 0, N ? N->par_dst * 16 : 0xffff, nv_, hx, nlv, J->nargs, J->par_kind == 1, J->g->pic_count > 0,
               nv_, nv_ && rj_ > J->wr_put_pc, nv_ && rj_ > J->wr_uninit_pc, j);
      }
    }
    rs_add(&rs, "      };\n      rr_ = rg_chain_%s(ctx, f, fc_, cos_, dv_, %d);\n", c->hex, d);
    rs_add(&rs, "      if (__builtin_expect(JS_IsException(rr_), 0)) { RG_FREE_OWNED(); return JS_EXCEPTION; }\n");
    if (C1->par_kind == 1) rs_add(&rs, c->nstack > 0 && getenv("QJS_AOT_REGION_NOSKFIXCTOR") == NULL ? "      JS_FreeValue(ctx, rr_); AOT_SET_B(ctx, fr, %d, SKFIX(rn_%d));\n" : "      JS_FreeValue(ctx, rr_); AOT_SET_B(ctx, fr, %d, rn_%d);\n", C1->par_dst * 16, C1->par_cons);   /* a stack object was migrated above: the frame gets the heap object, not the stale stack pointer */
    else rs_add(&rs, "      if (JS_VALUE_HAS_REF_COUNT(rr_)) qjs_aot_set_reg_p(ctx, fr, %d, rr_); else AOT_SET_B(ctx, fr, %d, rr_);\n", C1->par_dst * 16, C1->par_dst * 16);
    rs_add(&rs, "    }\n");
  }
  while (npm > 0) { npm--; memcpy(pmsave[npm].I->cur[pmsave[npm].reg], pmsave[npm].old, 40); }
  /* the stub of a pc is shared by the exits whose frame state is the same (several IR instructions can come from one bytecode op) */
  for (cp = rt.p ? rt.p : ""; *cp; cp++) h = (h ^ (unsigned char)*cp) * 1099511628211ull;
  for (cp = rs.p ? rs.p : ""; *cp; cp++) h = (h ^ (unsigned char)*cp) * 1099511628211ull;
  h = (h ^ (unsigned)rpc) * 1099511628211ull;
  for (i = 0; i < c->nxit; i++) {
    x = &c->xit[i];
    if (x->h != h || x->rpc != rpc || x->d != d) continue;
    if (strcmp(x->root, rt.p ? rt.p : "") != 0 || strcmp(x->rest, rs.p ? rs.p : "") != 0) continue;
    free(rt.p);
    free(rs.p);
    return i;
  }
  if (c->nxit >= RG_MAXX) { c->bad = 1; free(rt.p); free(rs.p); return 0; }
  if (c->nxit >= c->xitcap) {
    c->xitcap = c->xitcap ? c->xitcap * 2 : 256;
    c->xit = realloc(c->xit, (size_t)c->xitcap * sizeof(RXit));
    if (c->xit == NULL) abort();
  }
  x = &c->xit[c->nxit];
  x->pc = pc;
  x->d = d;
  x->rpc = rpc;
  x->h = h;
  x->root = rt.p ? rt.p : strdup("");
  x->rest = rs.p ? rs.p : strdup("");
  return c->nxit++;
}

/* In a stats build every exit source gets its own label, counted at the stub, so an exit can be traced to the instruction that took it. */
static const char *rg_xlabel(RCtx *c, int xi, int pc, const char *what, char *buf, size_t cap) {
  if (getenv("QJS_AOT_FASTSTATS") == NULL || c->nsite >= 4096) { snprintf(buf, cap, "rx_%d", xi); return buf; }
  c->site[c->nsite].xi = xi;
  snprintf(c->site[c->nsite].what, sizeof(c->site[0].what), "pc %d %s", pc, what);
  snprintf(buf, cap, "rgs_%d", c->nsite);
  c->nsite++;
  return buf;
}

typedef struct RWalk {
  char (*cur)[40];        /* the root's current value per register (as C text), NULL inside an inlined instance */
  int root_pc;            /* the root pc exits resume at (the call pair being expanded), -1 at root level */
} RWalk;

static int rg_walk_inst(RCtx *c, int ii, char (*rootcur)[40], const Q2Func *root);
void qjs_aot_region_dep(const char root_hex[33], const char callee_hex[33]);
int qjs_aot_pool_num(const JSAotGenFunc *g, uint32_t idx, double *out);
int qjs_aot_pool_tag(const JSAotGenFunc *g, uint32_t idx);
int qjs_aot_prof_unexecuted(const uint8_t dg[16], int pc);

/* Whether the instruction lies where the profile never executed it: what the region cannot do there becomes an exit to the generic body at that pc, taken before any effect
   of the instruction (QJS_AOT_REGION_NOCOLD turns it off). */
unsigned long long qjs_aot_prof_entries(const uint8_t dg[16]);
static int rg_cold(const RInst *I, const Q2Insn *in) {
  static int off = -1;
  if (off < 0) off = getenv("QJS_AOT_REGION_NOCOLD") != NULL;
  return !off && I->g != NULL && qjs_aot_prof_unexecuted(I->g->digest, in->pc);
}

/* The index of a requirement in the combined list a guard verifies for target t: the common ones, then the target's own. */
static int rg_qidx(const RGuard *G, int t, const char *name) {
  int i;
  for (i = 0; i < G->ncommon; i++) if (strcmp(G->common[i].name, name) == 0) return i;
  for (i = 0; i < G->nreq[t]; i++) if (strcmp(G->req[t][i].name, name) == 0) return G->ncommon + i;
  return -1;
}

/* Find (or learn) the entry of `val`'s shape in guard gi: leaves `gix_<gi>`, the entry index, in scope; leaves the region through `xl` when the value is not an
   object the guard can verify. */
static void rg_lookup(RCtx *c, int gi, const char *val, const char *xl, const char *ind) {
  FILE *o = c->o;
  const char *h = c->hex;
  fprintf(o, "%sint gix_%d;\n", ind, gi);
  fprintf(o, "%sif (__builtin_expect(JS_VALUE_GET_TAG(%s) != JS_TAG_OBJECT, 0)) goto %s;\n", ind, val, xl);
  fprintf(o, "%s{ const void *sh_ = *(const void *const *)((const char *)JS_VALUE_GET_PTR(%s) + QJS_AOT_OFF_JSOBJECT_SHAPE);\n", ind, val);
  fprintf(o, "%s  if (rg_sh_%s_%d[0] == sh_) gix_%d = 0;\n", ind, h, gi, gi);
  fprintf(o, "%s  else if (rg_sh_%s_%d[1] == sh_) gix_%d = 1;\n", ind, h, gi, gi);
  fprintf(o, "%s  else if (rg_sh_%s_%d[2] == sh_) gix_%d = 2;\n", ind, h, gi, gi);
  fprintf(o, "%s  else if (rg_sh_%s_%d[3] == sh_) gix_%d = 3;\n", ind, h, gi, gi);
  fprintf(o, "%s  else { gix_%d = rg_learn_%s_%d(ctx, %s); if (gix_%d < 0) goto %s; } }\n", ind, gi, h, gi, val, gi, xl);
}


/* TRUST mode (the default; QJS_AOT_REGION_TRUST=0 turns it off): a field the bundle places at one position in every receiver it knows is read at that constant offset,
   with no shape load and no table; a call with one resolved target is expanded with no dispatch. Soundness is the facts' (R1), not a per-access check. */
static int g_rg_plain; /* the last trusted field holds only values that are not reference counted */
static unsigned g_rg_cls; /* ... and the whole-program kinds of the last trusted field (bundle.c K_*): F64 stores narrow into a field that may be claimed an int */
static int rg_atom_n(const Q2Insn *in, int which, char *out, size_t cap);
static int rg_trust_field_n(const RInst *I, const Q2Insn *in, int write, const char *name, char *buf, size_t cap) {
  int slot, add = 0;
  unsigned cls = 0;
  if (qjsaot_region_mode() != 2) return 0;
  if (I->cls1 > 0) {
    char nm_[48];
    int is_this = in->rop == ROP_get_this_field || in->rop == ROP_get_this_field2 || (in->nuse >= 1 && rg_is_this(&I->q, in->use[0]));
    char n2_[48];
    /* a chain's second read is on the first read's result, not on `this` */
    if (is_this && name != NULL && (in->rop == ROP_get_field_chain || in->rop == ROP_get_field_chain_own) && rg_atom_n(in, 1, n2_, sizeof(n2_)) && strcmp(n2_, name) == 0) {
      char n1_[48];
      if (!rg_atom_n(in, 0, n1_, sizeof(n1_)) || strcmp(n1_, name) != 0) is_this = 0;
    }
    if (is_this && name == NULL) { if (!rg_atom(in, nm_, sizeof(nm_))) return 0; name = nm_; }
    if (is_this) {
      if (!qjs_aot_bundle_site_field(I->cls1 - 1, name, &slot, &cls)) { if (getenv("QJS_AOT_REGION_WHY")) { char hx_[33]; rg_hexof(I->g, hx_); fprintf(stderr, "why: %s %s pc %d rop %d this.%s: not a field of class site %d\n", hx_, I->g->name, in->pc, in->rop, name, I->cls1 - 1); } return 0; }
      snprintf(buf, cap, "%du * QJS_AOT_SIZEOF_JSPROPERTY", slot);
      g_rg_plain = qjs_aot_bundle_cls_plain(cls);
      g_rg_cls = cls;
      return 1;
    }
  }
  if (!qjs_aot_bundle_field_named(I->g, in->pc, write, name, &slot, &cls, &add) || add) {
    if (getenv("QJS_AOT_REGION_WHY")) fprintf(stderr, "why: %s pc %d .%s: no single position (add=%d)\n", I->g->name, in->pc, name ? name : "?", add);
    return 0;
  }
  if (g_rgc != NULL) {
    /* the read is at a fixed slot with no check: every class that can reach it is in the class table, whose layouts the word depends on (an instance that
       leaves its class shape breaks the word before the next entry) */
    QjsAotRecvClass cl[32];
    int nr = qjs_aot_bundle_receivers(I->g, in->pc, write ? 1 : 0, cl, 32), u;
    if (nr <= 0) { if (getenv("QJS_AOT_REGION_WHY")) fprintf(stderr, "why: %s pc %d .%s: receivers unproven (%d)\n", I->g->name, in->pc, name ? name : "?", nr); return 0; }
    for (u = 0; u < nr; u++) {
      int k = rg_cls_ok(g_rgc, &cl[u]) ? rg_cls_add(g_rgc, cl[u].site, rg_clctor(g_rgc, &cl[u]), cl[u].nfields, 0)
            : (!cl[u].one_shape && rg_clctor(g_rgc, &cl[u])[0] && cl[u].nfields >= 0 && cl[u].nfields <= 32 && qjs_aot_mshape_on()) ? rg_cls_add(g_rgc, cl[u].site, rg_clctor(g_rgc, &cl[u]), cl[u].nfields, 1) : -1;
      if (k < 0) { if (getenv("QJS_AOT_REGION_WHY")) fprintf(stderr, "why: %s pc %d .%s: class %s not bindable (one_shape %d ctor '%s' nf %d)\n", I->g->name, in->pc, name ? name : "?", cl[u].label, cl[u].one_shape, rg_clctor(g_rgc, &cl[u]), cl[u].nfields); return 0; }
      if (g_rgc->cls[k].nsh > 1) {
        /* a fixed-slot access to an object of a class with several shapes needs a shape test (the class has no layout the word could depend on): the shape join is the integrator's (E3) */
        if (getenv("QJS_AOT_REGION_MSHAPE_INITONLY") == NULL) { if (getenv("QJS_AOT_REGION_WHY")) fprintf(stderr, "why: %s pc %d .%s: class %s has %d shapes; a fixed-slot access needs the shape join\n", I->g->name, in->pc, name ? name : "?", cl[u].label, g_rgc->cls[k].nsh); return 0; }
        continue;   /* test knob: bind the tables, access the base fields unguarded (UNSOUND for a base field that is deleted or reordered) */
      }
      g_rgc->cls[k].need_layout = 1;
    }
  }
  snprintf(buf, cap, "%du * QJS_AOT_SIZEOF_JSPROPERTY", slot);
  g_rg_plain = qjs_aot_bundle_cls_plain(cls);
  g_rg_cls = cls;
  return 1;
}
static int rg_trust_field(const RInst *I, const Q2Insn *in, int write, char *buf, size_t cap) { return rg_trust_field_n(I, in, write, NULL, buf, cap); }
static void rg_verify(RCtx *c, const RInst *I, const Q2Insn *in, int write, const char *val, const char *nm, const char *off) {
  if (getenv("QJS_AOT_REGION_VERIFY") == NULL) return;
  fprintf(c->o, "        rg_vf_%s(ctx, %s, \"%s\", %s, %d, %d, \"%s\");\n", c->hex, val, nm, off, write, in->pc, I->g->name ? I->g->name : "?");
}
int qjsaot_region_mode(void) {
  static int mode = -1;
  if (mode < 0) {
    const char *r = getenv("QJS_AOT_REGION"), *t = getenv("QJS_AOT_REGION_TRUST");
    mode = (r != NULL && strcmp(r, "0") == 0) ? 0 : (t != NULL && strcmp(t, "0") == 0) ? 1 : 2;
  }
  return mode;
}

static int rg_trust_on(void) {
  return qjsaot_region_mode() == 2;
}

/* The value a chain of moves starts from. */
static int rg_base(const Q2Func *fn, Q2ValueId v) {
  int hop;
  for (hop = 0; hop < 8; hop++) {
    int di;
    const Q2Insn *d;
    if ((int32_t)v < 0 || (int32_t)v >= fn->nvalues) break;
    di = fn->values[v].insn;
    if (di < 0 || di >= fn->ninsns) break;
    d = &fn->insns[di];
    if ((d->rop == ROP_move || d->rop == ROP_move_s || d->rop == ROP_move_xfer) && d->nuse >= 1) { v = d->use[0]; continue; }
    break;
  }
  return (int)v;
}

/* TRUST: a read of `.name` whose receiver classes hold it at different slots (u.strength over the constraint classes): the receiver's shape is compared with
   the class shapes bound at init and the slot is that class's; a receiver of no listed class leaves. Returns 0 when the receivers are not all classes of the
   table that add the field unconditionally. */
static int rg_poly_field(RCtx *c, RInst *I, const Q2Insn *in, const char *name, const char *val, const char *dst, const char *xl) {
  QjsAotRecvClass cl[32];
  int nr = qjs_aot_bundle_receivers(I->g, in->pc, 0, cl, 32), u, slot[32], kk[32];
  unsigned cls;
  if (nr <= 0) return 0;
  for (u = 0; u < nr; u++) {
    if (!rg_cls_ok(c, &cl[u]) || !qjs_aot_bundle_site_field(cl[u].site, name, &slot[u], &cls)) return 0;
    kk[u] = rg_cls_add(c, cl[u].site, rg_clctor(c, &cl[u]), cl[u].nfields, 0);
    if (kk[u] < 0) return 0;
  }
  if (c->emit) {
    fprintf(c->o, "        { uint32_t off_; const void *sh_;\n          if (__builtin_expect(JS_VALUE_GET_TAG(%s) != JS_TAG_OBJECT, 0)) goto %s;\n", val, xl);
    fprintf(c->o, "          sh_ = *(const void *const *)((const char *)JS_VALUE_GET_PTR(%s) + QJS_AOT_OFF_JSOBJECT_SHAPE);\n          ", val);
    for (u = 0; u < nr; u++) fprintf(c->o, "if (sh_ == rk_s_%s[%d]) off_ = %du * QJS_AOT_SIZEOF_JSPROPERTY; else ", c->hex, kk[u], slot[u]);
    fprintf(c->o, "goto %s;\n          %s = *q2_slotp(%s, off_); }\n", xl, dst, val);
  }
  return 1;
}

/* The n-th atom operand of an instruction (a chain read has two). */
static int rg_atom_n(const Q2Insn *in, int which, char *out, size_t cap) {
  int a, seen = 0;
  for (a = 0; a < in->narg; a++)
    if (in->arg[a].kind == 'a' && seen++ == which) return qjs_aot_bundle_atom_name((uint32_t)in->arg[a].val, out, cap);
  return 0;
}

/* TRUST: whether value v is a constant path `G.f1...fk` (a global read, then field reads of plain properties, k <= 3): the names in out[0..*n). */
int qjs_aot_bundle_global_objonly(const char *name);
int qjs_aot_bundle_site_proto_field(int site, const char *name);
int qjs_aot_bundle_global_toplevel_only(const char *name);
static int rg_cpath(const Q2Func *fn, int v, char (*out)[40], int *n) {
  int b = rg_base(fn, (Q2ValueId)v), di;
  const Q2Insn *d;
  if (b < 0 || b >= fn->nvalues) return 0;
  di = fn->values[b].insn;
  if (di < 0 || di >= fn->ninsns) return 0;
  d = &fn->insns[di];
  if (d->rop == ROP_get_global) {
    int w_;
    *n = 1;
    if (!rg_atom(d, out[0], 40)) return 0;
    w_ = qjs_aot_bundle_global_writes(out[0]);
    if (w_ > 1 && getenv("QJS_AOT_REGION_NOINITIDIOM") == NULL && qjs_aot_bundle_global_objonly(out[0]) && qjs_aot_bundle_global_toplevel_only(out[0])) {
      /* A global the program stores several times, all of them OBJECTS (a namespace declared per file: `if (typeof NS == 'undefined') var NS = {}`, `var NS = NS || {}`), every store in top-level code (never inside a function): bound at the
         region's init like any other path. PERFORMANCE ASSUMPTION, not a proof: that the binding is not replaced in hot code. Soundness does not rest on it: every link of the path is
         watched (0445 reports global stores; a store of the SAME object does not break a fact, a different value does and the region leaves), so a wrong assumption costs a region that
         stays off, never a wrong answer. Numbers, strings and unknown values never take this way (a mutable counter is not a constant). */
      return 1;
    }
    if (w_ != 1 && getenv("QJS_AOT_REGION_WHY")) fprintf(stderr, "why: global %s written %d times: not a constant path\n", out[0], w_);
    if (getenv("QJS_AOT_REGION_DIAG_ANYWRITES") != NULL) return 1;   /* DIAGNOSTIC (RT1): treat a global written several times as constant, to see the next blocker */
    return w_ == 1 || w_ == 0;   /* a global written once (a declaration), or one the bundle never writes (the engine's own binding, Math): its facts watch it */
  }
  if (d->rop == ROP_get_field || d->rop == ROP_get_field_own || d->rop == ROP_get_field2 || d->rop == ROP_get_field2_own) {
    if (d->nuse < 1 || !rg_cpath(fn, (int)d->use[0], out, n) || *n >= 4) return 0;
    return rg_atom(d, out[(*n)++], 40);
  }
  if ((d->rop == ROP_get_field_chain || d->rop == ROP_get_field_chain_own) && d->nuse >= 1 && d->ndef >= 2) {
    if (!rg_cpath(fn, (int)d->use[0], out, n) || *n >= 4) return 0;
    if (!rg_atom_n(d, 0, out[(*n)++], 40)) return 0;
    if ((int)d->def[1] == b) { if (*n >= 4) return 0; return rg_atom_n(d, 1, out[(*n)++], 40); }
    return (int)d->def[0] == b;
  }
  return 0;
}

/* The call_method (as an instruction index) whose callee is v, looking after instruction k of block B and on through blocks that follow on one path. */
static int rg_find_call(const Q2Func *fn, const Q2Block *B, int k, int v) {
  const Q2Block *T = B;
  int start = k + 1, hop, j;
  for (hop = 0; hop < 4; hop++) {
    for (j = start; j < T->ninsns; j++) {
      const Q2Insn *cj = &fn->insns[T->first_insn + j];
      if (cj->rop == ROP_call_method && cj->nuse >= 2 && rg_base(fn, cj->use[0]) == v) return T->first_insn + j;
    }
    if (T->nsucc != 1 || T->succ_block[0] < 0 || T->succ_block[0] >= fn->nblocks) return -1;
    T = &fn->blocks[T->succ_block[0]];
    start = 0;
  }
  return -1;
}

/* Whether block b can run again after it ran (it is in a cycle of the function's own CFG). */
/* Whether value v of fn is used after instruction k (an index): later in its block, on an edge out of it, or live into a successor. */
static int rg_live_after(const Q2Func *fn, int k, Q2ValueId v) {
  int b, j, u, si;
  const Q2Block *B = NULL;
  for (b = 0; b < fn->nblocks; b++) if (k >= fn->blocks[b].first_insn && k < fn->blocks[b].first_insn + fn->blocks[b].ninsns) { B = &fn->blocks[b]; break; }
  if (B == NULL) return 1;
  for (j = k + 1; j < B->first_insn + B->ninsns; j++) {
    const Q2Insn *in = &fn->insns[j];
    for (u = 0; u < in->nuse; u++) if (in->use[u] == v) return 1;
    for (u = 0; u < in->win_n && in->win_val != NULL; u++) if (in->win_val[u] == v) return 1;
  }
  for (si = 0; si < B->nsucc; si++) {
    const Q2Block *T;
    for (j = 0; j < B->nedge_args[si]; j++) if (B->edge_arg[si][j] == v) return 1;
    if (B->succ_block[si] < 0 || B->succ_block[si] >= fn->nblocks) return 1;
    T = &fn->blocks[B->succ_block[si]];
    for (j = 0; j < T->nlive_in; j++) if (T->in_val[j] == v) return 1;
  }
  return 0;
}
static int rg_insn_at(const Q2Func *fn, int pc) {
  int i;
  for (i = 0; i < fn->ninsns; i++) if (fn->insns[i].pc == pc) return i;
  return -1;
}

static int rg_has_loop(const Q2Func *fn) {
  int b, s;
  for (b = 0; b < fn->nblocks; b++) {
    if (fn->rpo_pos != NULL && fn->rpo_pos[b] < 0) continue;
    for (s = 0; s < fn->blocks[b].nsucc; s++)
      if (fn->rpo_pos != NULL && fn->rpo_pos[fn->blocks[b].succ_block[s]] <= fn->rpo_pos[b]) return 1;
  }
  return 0;
}

static int rg_in_loop(const Q2Func *fn, int b) {
  int stack[128], sp = 0, i;
  uint8_t seen[128];
  if (fn->nblocks > 128 || b < 0 || b >= fn->nblocks) return 1;
  memset(seen, 0, sizeof(seen));
  for (i = 0; i < fn->blocks[b].nsucc; i++) {
    int s = fn->blocks[b].succ_block[i];
    if (s >= 0 && s < 128 && !seen[s]) { seen[s] = 1; stack[sp++] = s; }
  }
  while (sp > 0) {
    int x = stack[--sp];
    if (x == b) return 1;
    for (i = 0; i < fn->blocks[x].nsucc; i++) {
      int s = fn->blocks[x].succ_block[i];
      if (s >= 0 && s < 128 && !seen[s]) { seen[s] = 1; stack[sp++] = s; }
    }
  }
  return 0;
}

/* Whether value v (as a base through moves) is the receiver X of a call `X.call(...)`: the call_method's receiver, whose callee is a read of the atom `call`. */
static int rg_is_thru_recv(const Q2Func *fn, int v) {
  int i;
  for (i = 0; i < fn->ninsns; i++) {
    const Q2Insn *cm = &fn->insns[i];
    int cb, di;
    char an[16];
    if (cm->rop != ROP_call_method || cm->nuse < 2 || cm->win_n < 1 || rg_base(fn, cm->use[1]) != v) continue;
    cb = rg_base(fn, cm->use[0]);
    di = (int32_t)cb >= 0 && cb < fn->nvalues ? fn->values[cb].insn : -1;
    if (di < 0 || di >= fn->ninsns) continue;
    {
      const Q2Insn *cd = &fn->insns[di];
      int ai, seen = 0, want = (cd->rop == ROP_get_field_chain || cd->rop == ROP_get_field_chain_own) ? 1 : 0;   /* a chain's callee is its second read */
      an[0] = 0;
      for (ai = 0; ai < cd->narg; ai++)
        if (cd->arg[ai].kind == 'a' && seen++ == want) qjs_aot_bundle_atom_name((uint32_t)cd->arg[ai].val, an, sizeof(an));
      if (strcmp(an, "call") == 0) return 1;
    }
  }
  return 0;
}

/* Whether a constructor body keeps `this` to itself: the only uses of it (or of a copy) are as the object of a field read or store, a move, and the
   plain return. A call, an argument, a stored `this`, a branch on it, a `return this` all let it be seen, so a store to it afterwards is an effect. */
static int rg_this_contained(const Q2Func *fn) {
  int i, k, j;
  for (i = 0; i < fn->ninsns; i++) {
    const Q2Insn *in = &fn->insns[i];
    if (in->rop == ROP_return) return 0;
    for (j = 0; j < in->win_n && in->win_val != NULL; j++) if (rg_is_this(fn, in->win_val[j])) return 0;
    for (k = 0; k < in->nuse; k++) {
      if (!rg_is_this(fn, in->use[k])) continue;
      switch (in->rop) {
        case ROP_put_field: case ROP_put_field_xfer:
        case ROP_get_field: case ROP_get_field_own: case ROP_get_field2: case ROP_get_field2_own:
        case ROP_get_field_chain: case ROP_get_field_chain_own:
          if (k == 0) continue;
          return 0;
        case ROP_move: case ROP_move_s: case ROP_move_xfer: continue;
        default: return 0;
      }
    }
  }
  return 1;
}

/* TRUST: a constructor body that returns no value (`return x` replaces the object, which the construct does not model). */
static int rg_ctor_noreturn(const Q2Func *fn) {
  int i;
  for (i = 0; i < fn->ninsns; i++) if (fn->insns[i].rop == ROP_return) return 0;
  return 1;
}

/* The layout claim check of an object the region made in its class's final shape, once its fields are stored: each slot against the unit's claim
   table inline; only an object a slot contradicts goes to the engine (whose check breaks the layout). */
static void rg_emit_filled(RCtx *c, FILE *o, int ki) {
  RCons *K = &c->cons[ki];
  int j;
  fprintf(o, "        if (*(const uint16_t *)((const unsigned char *)rgc_s_%s_%d + QJS_AOT_OFF_SHAPE_LAYOUT_ID) != 0 && (0", c->hex, ki);
  for (j = 0; j < K->nn; j++) fprintf(o, " || Q2_CLAIM_BAD(rn_%d, %du * QJS_AOT_SIZEOF_JSPROPERTY, *q2_slotp(rn_%d, %du * QJS_AOT_SIZEOF_JSPROPERTY))", ki, j, ki, j);
  fprintf(o, ")) qjs_aot_object_filled(ctx, rn_%d);\n", ki);
}

/* TRUST: the object a construct of this region makes is seen (an argument, a receiver, a stored value, an out-of-line callee's `this`). Before that point no one
   but the constructor's stores touches it; from it on, the object must be complete: made in its class's final shape (direct), every field of the site's list
   stored, and no store of the constructor's own after it (those are unchecked). The layout's claim check of the object runs here, once. */
static int rg_cons_seen(RCtx *c, const RInst *I, int pc, FILE *o) {
  RCons *K = &c->cons[I->cons - 1];
  char fn_[40];
  int n = 0, j;
  if (K->esc) return 1;
  if (K->site <= 0) return rg_fail(c, "pc %d: the constructed object is seen before it is complete (no single allocation site)", pc);
  while (n < 32 && qjs_aot_bundle_site_field_name(K->site, n, fn_, sizeof(fn_))) n++;
  for (j = 0; j < n; j++) {
    int ni;
    if (!qjs_aot_bundle_site_field_name(K->site, j, fn_, sizeof(fn_))) break;
    for (ni = 0; ni < K->nn && strcmp(K->names[ni], fn_) != 0; ni++) {}
    if (ni == K->nn || !(K->seen & (1u << ni))) return rg_fail(c, "pc %d: the constructed object is seen before .%s is stored", pc, fn_);
  }
  K->esc = 1;
  if (c->emit && K->direct) rg_emit_filled(c, o, I->cons - 1);
  return 1;
}

static int rg_carried(const Q2Func *fn, Q2ValueId v0);
/* Whether the value v of instance ii (or the result it becomes in the instances above) can be carried over a back edge: the object a construct or pop leaves in the
   region's slot is replaced when the site runs again, which is safe only if nothing borrowed from it is still live then. */
/* Whether value v of fn (a copy of it, or a block parameter it is passed to) is what fn returns. */
static int rg_returned(const Q2Func *fn, Q2ValueId v0) {
  uint8_t *in;
  int changed = 1, i, b, s, j, ret = 0;
  if ((int32_t)v0 < 0 || (int32_t)v0 >= fn->nvalues) return 1;
  in = calloc((size_t)fn->nvalues, 1);
  if (in == NULL) return 1;
  in[v0] = 1;
  while (changed) {
    changed = 0;
    for (i = 0; i < fn->ninsns; i++) {
      const Q2Insn *d = &fn->insns[i];
      if ((d->rop == ROP_move || d->rop == ROP_move_s || d->rop == ROP_move_xfer) && d->nuse >= 1 && d->ndef >= 1 && in[d->use[0]] && !in[d->def[0]]) { in[d->def[0]] = 1; changed = 1; }
    }
    for (b = 0; b < fn->nblocks; b++) {
      const Q2Block *B = &fn->blocks[b];
      for (s = 0; s < B->nsucc; s++) {
        const Q2Block *T = &fn->blocks[B->succ_block[s]];
        for (j = 0; j < B->nedge_args[s] && j < T->nparams; j++)
          if (in[B->edge_arg[s][j]] && !in[T->param_val[j]]) { in[T->param_val[j]] = 1; changed = 1; }
      }
    }
  }
  for (i = 0; i < fn->ninsns && !ret; i++) if (fn->insns[i].rop == ROP_return && fn->insns[i].nuse >= 1 && in[fn->insns[i].use[0]]) ret = 1;
  free(in);
  return ret;
}

/* Whether the object a site of instance ii makes (value v) can still be live when the site runs again: within one run of ii only when the site is in a
   loop of ii and the value is carried over a back edge; in a later run (ii expanded in a loop above) only when ii returns it and the caller keeps it so. */
static int rg_carried_up(RCtx *c, int ii, int v) {
  for (;;) {
    RInst *I = c->in[ii];
    if (rg_trust_on()) {
      if (I->curloop && rg_carried(&I->q, (Q2ValueId)v)) return 1;
      if (ii == 0 || I->par < 0 || !rg_returned(&I->q, (Q2ValueId)v)) return 0;
    } else {
      if (rg_carried(&I->q, (Q2ValueId)v)) return 1;
      if (ii == 0 || I->par < 0) return 0;
    }
    v = I->par_dv;
    ii = I->par;
    if (rg_trust_on() && rg_carried(&c->in[ii]->q, (Q2ValueId)v)) return 1;
  }
}

/* Retention: a value parked in the owned list stays there to the end of the region. In a loop that grows without bound (a construct per iteration, carried one iteration
   and dropped the next). A loop whose carried object values are all made by the region itself (a construct, a literal, null/numbers, or the loop's own parameters)
   frees, at its back edge, the entries parked since the loop was entered that none of the loop's parameters still holds. Any carried value of another origin (a field read,
   a call result, an argument) may point into one of the parked objects, so such a loop keeps the old rule. */
static int rg_srcok(const Q2Func *fn, int T, Q2ValueId a, uint8_t *seen, int depth) {
  const Q2Value *v;
  int b, s;
  if ((int32_t)a < 0 || (int32_t)a >= fn->nvalues || depth > 16) return 0;
  v = &fn->values[a];
  if (!(v->type & (Q2_T_STR | Q2_T_OBJ | Q2_T_FUNC | Q2_T_SYMBOL | Q2_T_BIGINT)) || v->is_const) return 1;
  /* made before the loop was entered: it cannot point at an object parked in the loop */
  if (v->block >= 0 && v->block < fn->nblocks && v->block != T && fn->rpo_pos[v->block] >= 0 && fn->rpo_pos[v->block] < fn->rpo_pos[T]) return 1;
  if (seen[a]) return 1;
  seen[a] = 1;
  if (v->param_of >= 0) {
    int found = 0;
    for (b = 0; b < fn->nblocks; b++) {
      const Q2Block *B = &fn->blocks[b];
      if (fn->rpo_pos != NULL && fn->rpo_pos[b] < 0) continue;
      for (s = 0; s < B->nsucc; s++) {
        if (B->succ_block[s] != v->block || v->param_of >= B->nedge_args[s]) continue;
        if (fn->rpo_pos[b] < fn->rpo_pos[T]) continue;   /* from outside the loop */
        found = 1;
        if (!rg_srcok(fn, T, B->edge_arg[s][v->param_of], seen, depth + 1)) return 0;
      }
    }
    return 1;   /* every incoming edge is from outside the loop (or the parameter has none in the loop) */
  }
  if (v->insn < 0 || v->insn >= fn->ninsns) return 0;
  {
    const Q2Insn *d = &fn->insns[v->insn];
    if ((d->rop == ROP_move || d->rop == ROP_move_s || d->rop == ROP_move_xfer) && d->nuse >= 1) return rg_srcok(fn, T, d->use[0], seen, depth + 1);
    /* arithmetic and bit operations (the fused increments and branches included) give primitives (a number, a string, a bigint), never an object */
    {
      char nm_[48];
      e_rop_name((uint16_t)d->rop, nm_, sizeof(nm_));
      if (!strncmp(nm_, "add", 3) || !strncmp(nm_, "sub", 3) || !strncmp(nm_, "mul", 3) || !strncmp(nm_, "div", 3) || !strncmp(nm_, "mod", 3) || !strncmp(nm_, "inc", 3) ||
          !strncmp(nm_, "dec", 3) || !strncmp(nm_, "bit", 3) || !strncmp(nm_, "shl", 3) || !strncmp(nm_, "sar", 3) || !strncmp(nm_, "shr", 3) || !strncmp(nm_, "neg", 3) ||
          !strncmp(nm_, "not", 3) || !strncmp(nm_, "binary", 6) || !strncmp(nm_, "to_numeric", 10) || !strncmp(nm_, "plus", 4))
        return 1;
    }
    return d->rop == ROP_construct || d->rop == ROP_new_object || d->rop == ROP_object_template || d->rop == ROP_array_from;
  }
}
/* Whether the back edges of loop header T of fn may free what was parked since the loop was entered: T is entered by a forward edge (so the mark can be taken there) and
   every value that reaches one of its parameters is made by the region. */
static int rg_compact_ok(const Q2Func *fn, int T) {
  int b, s, j, fwd = 0, any = 0;
  uint8_t *seen;
  const Q2Block *H;
  if (T <= 0 || T >= fn->nblocks || fn->rpo_pos == NULL || getenv("QJS_AOT_REGION_NOCOMPACT") != NULL) return 0;
  H = &fn->blocks[T];
  seen = calloc((size_t)fn->nvalues + 1, 1);
  if (seen == NULL) return 0;
  for (b = 0; b < fn->nblocks; b++) {
    const Q2Block *B = &fn->blocks[b];
    if (fn->rpo_pos[b] < 0) continue;
    for (s = 0; s < B->nsucc; s++) {
      if (B->succ_block[s] != T) continue;
      if (fn->rpo_pos[T] > fn->rpo_pos[b]) { fwd = 1; continue; }   /* an edge from outside the loop: its values were made before it */
      for (j = 0; j < B->nedge_args[s] && j < H->nparams; j++) {
        any = 1;
        if (!rg_srcok(fn, T, B->edge_arg[s][j], seen, 0)) { if (getenv("QJS_AOT_REGION_WHY")) { const Q2Value *v_ = &fn->values[B->edge_arg[s][j]]; fprintf(stderr, "compact: block %d param %d from block %d: value %d type 0x%x param_of %d insn %d rop %d refused\n", T, j, b, (int)B->edge_arg[s][j], (unsigned)v_->type, v_->param_of, v_->insn, v_->insn >= 0 && v_->insn < fn->ninsns ? (int)fn->insns[v_->insn].rop : -1); if (v_->insn >= 0 && v_->insn < fn->ninsns) { char rn_[48]; e_rop_name(fn->insns[v_->insn].rop, rn_, sizeof(rn_)); fprintf(stderr, "compact:   defined by %s\n", rn_); } } free(seen); return 0; }
      }
    }
  }
  free(seen);
  return fwd && any;
}
static void rg_note_park(RCtx *c, int ii) {
  int guard = 0;
  while (ii >= 0 && ii < 256 && guard++ < 64) { c->park[ii] = 1; if (ii == 0 || c->in[ii] == NULL) break; ii = c->in[ii]->par; }
}
static int rg_cmk_id(RCtx *c, int ii, int T) {
  int i;
  for (i = 0; i < c->ncmk; i++) if (c->cmk_ii[i] == ii && c->cmk_t[i] == T) return i;
  if (c->ncmk >= 48) return -1;
  c->cmk_ii[c->ncmk] = ii;
  c->cmk_t[c->ncmk] = T;
  return c->ncmk++;
}

/* The construct record of (instance, pc), made on first use. */
static int rg_cons(RCtx *c, int inst, int pc) {
  int i;
  for (i = 0; i < c->ncons; i++) if (c->cons[i].inst == inst && c->cons[i].pc == pc) return i;
  if (c->ncons >= RG_MAXC) return -1;
  memset(&c->cons[c->ncons], 0, sizeof(RCons));
  c->cons[c->ncons].inst = inst;
  c->cons[c->ncons].pc = pc;
  return c->ncons++;
}

/* `arr.push(x)` with no resolved target: the builtin, on an array in the engine's plain form (the helper tests it: initial array shape, length == count,
   standard array prototype), where Array.prototype.push is the builtin by a fact of the region's word. The helper writes nothing unless it succeeds, and
   its growth path runs no JavaScript, so a miss leaves before any effect. */
static void rg_amut_note(RCtx *c, const RInst *I, int base);
static int rg_push(RCtx *c, int ii, const Q2Insn *call, char (*rootcur)[40], const Q2Func *root, int exit_pc) {
  if (!c->emit) { c->arr_mut = 1; rg_amut_note(c, c->in[ii], (int)call->use[1]); }
  RInst *I = c->in[ii];
  int gi = rg_guard(c, I->id, (int)call->use[1], call->pc, 1, "push");
  FILE *o = c->o;
  if (gi < 0) return rg_fail(c, "too many guards");
  c->gd[gi].intr = 1;
  if (c->emit) {
    char xl2[32];
    rg_xlabel(c, rg_xit(c, ii, exit_pc), call->pc, "push", xl2, sizeof(xl2));
    fprintf(o, "    { /* %s.push() at pc %d */\n", I->pfx, call->pc);
    /* (TRUST: the builtin and the facts that it stays the one found were bound at the region's init) */
    if (!rg_trust_on()) fprintf(o, "      if (__builtin_expect(!rg_sk_%s_%d, 0)) { if (rg_learn_%s_%d(ctx, %sv%u) < 0) goto %s; rg_sk_%s_%d = 1; }\n", c->hex, gi, c->hex, gi, I->pfx, (unsigned)call->use[1], xl2, c->hex, gi);
    /* An Array that lives in the region's frame (a stack object) is pushed to by this same helper: a push past its inline room goes to the engine's growth, expand_fast_array,
       which for an inline-valued Array allocates a NEW heap element block, copies, and clears values_inline; it never reallocs or frees the inline room or the object itself.
       qjs_aot_stack_release / _migrate free that heap block. So no exit is needed for growth. */
    fprintf(o, "      { JSValue o_; if (__builtin_expect(!q2_array_push1(ctx, rg_pf_%s, %sv%u, %sv%u, &o_), 0)) goto %s; %sv%u = o_; }\n    }\n",
            c->hex, I->pfx, (unsigned)call->use[1], I->pfx, (unsigned)call->win_val[0], xl2, I->pfx, (unsigned)call->def[0]);
  }
  return 1;
}

/* Whether the value v of fn, a copy of it or a block parameter it is passed to can be carried over a back edge (so it may be live when the code that made it runs again). */
static int rg_carried(const Q2Func *fn, Q2ValueId v0) {
  uint8_t *in;
  int changed = 1, i, b, s, j, carried = 0;
  if ((int32_t)v0 < 0 || (int32_t)v0 >= fn->nvalues) return 1;
  in = calloc((size_t)fn->nvalues, 1);
  if (in == NULL) return 1;
  in[v0] = 1;
  while (changed) {
    changed = 0;
    for (i = 0; i < fn->ninsns; i++) {
      const Q2Insn *d = &fn->insns[i];
      if ((d->rop == ROP_move || d->rop == ROP_move_s || d->rop == ROP_move_xfer) && d->nuse >= 1 && d->ndef >= 1 && in[d->use[0]] && !in[d->def[0]]) { in[d->def[0]] = 1; changed = 1; }
    }
    for (b = 0; b < fn->nblocks; b++) {
      const Q2Block *B = &fn->blocks[b];
      if (fn->rpo_pos != NULL && fn->rpo_pos[b] < 0) continue;
      for (s = 0; s < B->nsucc; s++) {
        const Q2Block *T = &fn->blocks[B->succ_block[s]];
        for (j = 0; j < B->nedge_args[s] && j < T->nparams; j++) {
          if (!in[B->edge_arg[s][j]]) continue;
          if (fn->rpo_pos != NULL && fn->rpo_pos[B->succ_block[s]] <= fn->rpo_pos[b]) carried = 1;
          if (!in[T->param_val[j]]) { in[T->param_val[j]] = 1; changed = 1; }
        }
      }
    }
  }
  free(in);
  return carried;
}

/* `arr.pop()`: the element moves out of the array and the region owns it (`rn_K`) until the site runs again or the region ends. That is safe only when the value
   cannot be live when the site runs again: the caller checks, at the root, that it is not carried over a back edge. */
static int rg_pop(RCtx *c, int ii, const Q2Insn *call, char (*rootcur)[40], const Q2Func *root, int exit_pc) {
  if (!c->emit) { c->arr_mut = 1; rg_amut_note(c, c->in[ii], (int)call->use[1]); }
  RInst *I = c->in[ii];
  /* an element still live when the site runs again (carried over a loop edge) is parked in the owned list, as a construct's object is, instead of released */
  int held_ = (I->ploop || I->curloop) && (getenv("QJS_AOT_REGION_PARKALL") != NULL || rg_carried_up(c, ii, (int)call->def[0]));
  if (held_) rg_note_park(c, ii);
  int gi = rg_guard(c, I->id, (int)call->use[1], call->pc, 1, "pop"), ki;
  FILE *o = c->o;
  if (gi < 0) return rg_fail(c, "too many guards");
  c->gd[gi].intr = 1;
  ki = rg_cons(c, I->id, call->pc);
  if (ki < 0) return rg_fail(c, "too many constructs");
  c->cons[ki].kind = 2;
  c->npop++;
  if (c->emit) {
    char xl2[32], rel_[96];
    snprintf(rel_, sizeof(rel_), held_ ? "rg_own_put(ctx, &rgo_, &rgon_, &rgoc_, rn_%d)" : "JS_FreeValue(ctx, rn_%d)", ki);
    rg_xlabel(c, rg_xit(c, ii, exit_pc), call->pc, "pop", xl2, sizeof(xl2));
    fprintf(o, "    { /* %s.pop() at pc %d */\n", I->pfx, call->pc);
    if (held_) fprintf(o, "      if (__builtin_expect(rgon_ >= RG_OWNCAP, 0)) goto %s;\n", xl2);
    if (!rg_trust_on()) fprintf(o, "      if (__builtin_expect(!rg_sk_%s_%d, 0)) { if (rg_learn_%s_%d(ctx, %sv%u) < 0) goto %s; rg_sk_%s_%d = 1; }\n", c->hex, gi, c->hex, gi, I->pfx, (unsigned)call->use[1], xl2, c->hex, gi);
    fprintf(o, "      { JSValue o_; if (__builtin_expect(!q2_array_pop0(ctx, rg_pp_%s, %sv%u, &o_), 0)) goto %s; %s; rn_%d = o_; %sv%u = o_; }\n    }\n",
            c->hex, I->pfx, (unsigned)call->use[1], xl2, rel_, ki, I->pfx, (unsigned)call->def[0]);
  }
  return 1;
}

/* The text that releases the stack object of construct k0 and its Array (a no-op when neither is alive). */
static void rg_sk_release_text(RCtx *c, int k0, RStr *out) {
  int k1 = c->cons[k0].sk_pair;
  if (k1 >= 0) rs_add(out, "if (sk_%d || sk_%d) { qjs_aot_stack_release(ctx, sk_%d ? SKV_%d : JS_UNDEFINED, sk_%d ? SKV_%d : JS_UNDEFINED); sk_%d = 0; sk_%d = 0; } ", k0, k1, k0, k0, k1, k1, k0, k1);
  else rs_add(out, "if (sk_%d) { qjs_aot_stack_release(ctx, SKV_%d, JS_UNDEFINED); sk_%d = 0; } ", k0, k0, k0);
}
/* The objects the region's constructs made (`rn_K`) belong to the region until it returns or leaves: whoever kept one took a reference of its own. */
static void rg_free_owned(RCtx *c, FILE *o, const char *ind) {
  int k;
  for (k = 0; k < c->ncons; k++) {
    if (c->cons[k].stack) {
      /* a stack object is released as a pair (its block is the region's frame); once migrated, rnh_K holds the heap object */
      if (c->cons[k].kind == 0) { RStr t = {0}; rg_sk_release_text(c, k, &t); fprintf(o, "%s%s\n", ind, t.p ? t.p : ""); free(t.p); }
      fprintf(o, "%sJS_FreeValue(ctx, rnh_%d);\n", ind, k);
    } else fprintf(o, "%sJS_FreeValue(ctx, rn_%d);\n", ind, k);
  }
  if (rg_trust_on()) fprintf(o, "%sif (rgon_) rg_own_free(ctx, rgo_, rgon_, rgoc_ > 0);\n", ind);
}

/* Emit the dispatch of a call pair and the expansion of its targets. */
/* Whether the root's value named `cv` is its `this` or an argument as the frame holds it: a reference the caller's frame keeps for the whole run (the root
   never writes an argument and makes no `arguments` object, so the slot is not replaced while the region runs). */
static int rg_frame_owned(const Q2Func *fn, const char *cv) {
  unsigned vn;
  int i, di;
  if (sscanf(cv, "r0_v%u", &vn) != 1 || (int)vn >= fn->nvalues) return 0;
  di = fn->values[vn].insn;
  if (di < 0 || di >= fn->ninsns) return 0;
  if (fn->insns[di].rop == ROP_load_this) return 1;
  if (fn->insns[di].rop != ROP_get_arg && fn->insns[di].rop != ROP_get_arg2) return 0;
  for (i = 0; i < fn->ninsns; i++) {
    uint16_t r = fn->insns[i].rop;
    if (r == ROP_put_arg || r == ROP_put_arg_xfer || r == ROP_special_object) return 0;
  }
  return 1;
}

/* Whether the value named `cv` (C text of a region value) is kept alive for the region's whole run by something the callee cannot release: the
   root's `this` and arguments (the caller's frame holds them), the object of a construct of the region (its slot rn_K), or, in an inlined frame,
   its `this` or an argument that is one of those in the frame that expanded it. */
static int rg_held_name(RCtx *c, const char *cv, int depth) {
  int n, k;
  unsigned m;
  RInst *A;
  const Q2Insn *d;
  if (depth > 16) return 0;
  if (strcmp(cv, "f_this") == 0) return 1;
  for (k = 0; k < c->nownres; k++) if (strcmp(c->ownres[k], cv) == 0) return 1;   /* the result of an out-of-line call: in the owned list since the call */
  if (sscanf(cv, "rn_%d", &n) == 1) return 1;
  if (sscanf(cv, "r%d_v%u", &n, &m) != 2 || n < 0 || n >= c->nin_total || c->in[n] == NULL) return 0;
  A = c->in[n];
  {
    char px[16];
    snprintf(px, sizeof(px), "r%d_", n);
    if (strcmp(px, A->pfx) != 0) return 0;
  }
  if ((int)m >= A->q.nvalues || A->q.values[m].insn < 0 || A->q.values[m].insn >= A->q.ninsns) return 0;
  d = &A->q.insns[A->q.values[m].insn];
  if (d->rop == ROP_move || d->rop == ROP_move_s || d->rop == ROP_move_xfer) {
    char nx[24];
    if (d->nuse < 1) return 0;
    snprintf(nx, sizeof(nx), "%sv%u", A->pfx, (unsigned)d->use[0]);
    return rg_held_name(c, nx, depth + 1);
  }
  /* the object of a construct is held by the site's slot (rn_K) or, carried round a loop, parked in the owned list; nothing but the region's end releases either */
  if (d->rop == ROP_construct) return 1;
  /* an inlined call's result is the value its callee returns: held when every arm's returned value is */
  if (d->rop == ROP_call_method || d->rop == ROP_call) {
    int kk, cnt = 0;
    for (kk = 1; kk < c->nin_total; kk++) {
      int jj, ret = -1, nret = 0;
      char rn[24];
      if (c->in[kk] == NULL || c->in[kk]->par != n || c->in[kk]->par_call_pc != d->pc) continue;
      cnt++;
      if (c->in[kk]->ool || c->in[kk]->g == NULL) return 0;
      for (jj = 0; jj < c->in[kk]->q.ninsns; jj++) if (c->in[kk]->q.insns[jj].rop == ROP_return) { ret = jj; nret++; }
      if (nret != 1 || c->in[kk]->q.insns[ret].nuse < 1) return 0;
      snprintf(rn, sizeof(rn), "%sv%u", c->in[kk]->pfx, (unsigned)c->in[kk]->q.insns[ret].use[0]);
      if (!rg_held_name(c, rn, depth + 1)) return 0;
    }
    return cnt > 0;
  }
  /* the receiver half of a method read is the holder itself */
  if ((d->rop == ROP_get_this_field2 || d->rop == ROP_get_field2 || d->rop == ROP_get_field2_own) && d->ndef >= 2 && d->def[1] == (Q2ValueId)m) {
    char hn[24];
    if (d->rop == ROP_get_this_field2) snprintf(hn, sizeof(hn), "%s", A->self);
    else if (d->nuse >= 1) snprintf(hn, sizeof(hn), "%sv%u", A->pfx, (unsigned)d->use[0]);
    else return 0;
    return rg_held_name(c, hn, depth + 1);
  }
  if (d->rop == ROP_load_this) return n == 0 ? 1 : rg_held_name(c, A->self, depth + 1);
  if (d->rop != ROP_get_arg && d->rop != ROP_get_arg2) return 0;
  for (k = 0; k < A->q.ninsns; k++) {
    uint16_t r = A->q.insns[k].rop;
    if (r == ROP_put_arg || r == ROP_put_arg_xfer || r == ROP_special_object) return 0;
  }
  if (n == 0) return 1;
  k = d->rop == ROP_get_arg2 && d->ndef >= 2 && d->def[1] == (Q2ValueId)m ? (d->narg >= 4 ? (int)d->arg[3].val : -1) : (d->narg >= 2 ? (int)d->arg[1].val : -1);
  if (k < 0) return 0;
  if (k >= A->nargs) return 1;   /* an argument the call did not pass: undefined */
  return rg_held_name(c, A->arg[k], depth + 1);
}

/* The root's pc a call in instance ii stands at for a backtrace: the call's own pc in the root, else the root call the inlined chain hangs from. */
static void rg_emit_cap(RCtx *c, int ii, int pc, FILE *o) {
  int k = ii, rpc = pc;
  while (k > 0 && c->in[k]->par > 0) k = c->in[k]->par;
  if (k > 0) rpc = c->in[k]->par_call_pc;
  fprintf(o, "        *(const uint8_t **)((uint8_t *)f + QJS_AOT_OFF_REGRUN_CAP_IP) = *(const uint8_t *const *)(*(uint8_t **)((uint8_t *)f + QJS_AOT_OFF_REGRUN_B) + QJS_AOT_OFF_FNBC_REGVM_CODE) + %d;\n", rpc);
}

/* Before an out-of-line call (TRUST): every value a frame on the path still uses after the call (the call itself in instance ii, the call that
   expanded each frame above) is a borrow the callee could free, so each gets a reference held to the region's end. */
static void rg_ool_live(RCtx *c, int ii, const Q2Insn *call, FILE *o) {
  RStr live = {0};
  RInst *I = c->in[ii];
  int at = (int)(call - I->q.insns), prev = -1, a2, reg;
  for (a2 = ii; a2 >= 0; prev = a2, a2 = c->in[a2]->par) {
    RInst *A = c->in[a2];
    int kidx = a2 == ii ? at : rg_insn_at(&A->q, c->in[prev]->par_call_pc);
    if (A->cur == NULL) break;
    for (reg = 0; reg < 256; reg++) {
      char px_[12];
      unsigned vn_;
      if (!A->cur[reg][0]) continue;
      if (sscanf(A->cur[reg], "%11[^v]v%u", px_, &vn_) == 2 && strcmp(px_, A->pfx) == 0 && kidx >= 0 && !rg_live_after(&A->q, kidx, (Q2ValueId)vn_)) continue;
      if (rg_held_name(c, A->cur[reg], 0)) continue;   /* a reference the call cannot drop: the root's frame, the region's own construct or owned call result, an inlined result of those */
      if (getenv("QJS_AOT_REGION_OWNSTAT") != NULL && c->emit) {
        /* a temporary per-site counter: the generator names the site, the unit counts its executions and prints them at exit */
        static int osn_;
        char rn_[40] = "-";
        unsigned dv_ = 0;
        if (sscanf(A->cur[reg], "%*[^v]v%u", &dv_) == 1 && (int)dv_ < A->q.nvalues && A->q.values[dv_].insn >= 0 && A->q.values[dv_].insn < A->q.ninsns) e_rop_name(A->q.insns[A->q.values[dv_].insn].rop, rn_, sizeof(rn_));
        fprintf(stderr, "OWNSTAT site %d: root %s call pc %d in inst %d (fn pc of def in its frame): value %s defined by %s\n", osn_, c->hex, call->pc, a2, A->cur[reg], rn_);
        fprintf(o, "        q2_os_[%d]++;\n", osn_++);
      }
      rs_add(&live, "        rg_own_dup(ctx, &rgo_, &rgon_, &rgoc_, %s);\n", A->cur[reg]);
    }
    if (a2 == 0) break;
  }
  if (live.p) { fputs(live.p, o); rs_add(&c->ools, "%s", live.p); free(live.p); }
}


/* Whether a value the region borrows (a field, element or method read without a dup, an inlined result of one, an argument borrowed from a parent) is
   still used after instruction `in` of instance ii, in that frame or in any frame above it: freeing a reference here could then free what it points into
   (a free cascades through the freed object's fields). Conservative: every live value that rg_held_name cannot vouch for counts. */
static int rg_borrow_live(RCtx *c, int ii, const Q2Insn *in) {
  RInst *I = c->in[ii];
  int at = (int)(in - I->q.insns), prev = -1, a2, reg;
  if (getenv("QJS_AOT_REGION_NOBORROWPARK") != NULL) return 0;
  for (a2 = ii; a2 >= 0; prev = a2, a2 = c->in[a2]->par) {
    RInst *A = c->in[a2];
    int kidx = a2 == ii ? at : rg_insn_at(&A->q, c->in[prev]->par_call_pc);
    if (A->cur == NULL) return 1;
    for (reg = 0; reg < 256; reg++) {
      char px_[12];
      unsigned vn_;
      if (!A->cur[reg][0]) continue;
      if (sscanf(A->cur[reg], "%11[^v]v%u", px_, &vn_) == 2 && strcmp(px_, A->pfx) == 0 && kidx >= 0 && !rg_live_after(&A->q, kidx, (Q2ValueId)vn_)) continue;
      if (rg_held_name(c, A->cur[reg], 0)) continue;
      return 1;
    }
    if (a2 == 0) break;
  }
  return 0;
}
/* The release of the value a store replaced: at once when nothing borrowed is live after the store, else parked in the owned list (to a compaction, the cap or the region's end). */
static void rg_store_release(RCtx *c, int ii, const Q2Insn *in) {
  FILE *o = c->o;
  if (!rg_borrow_live(c, ii, in)) { if (c->emit) fprintf(o, "          if (JS_VALUE_HAS_REF_COUNT(old_)) JS_FreeValue(ctx, old_);\n"); return; }
  rg_note_park(c, ii);
  if (c->emit) {
    char xo[32];
    rg_xlabel(c, rg_xit(c, ii, in->next), in->pc, "store-cap", xo, sizeof(xo));
    fprintf(o, "          if (JS_VALUE_HAS_REF_COUNT(old_)) { rg_own_put(ctx, &rgo_, &rgon_, &rgoc_, old_); if (__builtin_expect(rgon_ >= RG_OWNCAP, 0)) goto %s; }\n", xo);
  }
}
/* The forwarding constructor Prototype.js's Class.create returns: `function () { this.initialize.apply(this, arguments); }`. In the register program it is exactly
       local_uninit slot1; load_this; put_local_s0 (this); get_local_field_0 .m; get_field2 .apply; (moves); apply_arguments(window [m, apply, this], slot 1); return_undefined
   and the interpreter forwards the frame's own arguments to m when `apply` is the builtin, never building an arguments object (slot 1 stays uninitialised). Strict: no formals, those
   instructions and nothing else (so nothing else reads or writes either slot, no closure can see them), the three window values are the three loads. Anything else is refused. */
typedef struct RWrap { int uninit_pc, put_pc, read_pc, apply_pc; char method[40]; } RWrap;
static int rg_wrapper(const Q2Func *q, const JSAotGenFunc *g, RWrap *w) {
  int k, step = 0, lt = -1, gf = -1, ga = -1;
  if (getenv("QJS_AOT_REGION_NOWRAPPER") != NULL) return 0;
  if (!q->ok || g->func_kind != 0 || g->ctl_count != 0 || g->arg_count != 0 || g->defined_arg_count != 0 || g->var_count != 2 || g->light_var_count != 2) return 0;
  for (k = 0; k < q->ninsns; k++) {
    const Q2Insn *in = &q->insns[k];
    if (in->rop == ROP_move || in->rop == ROP_move_s || in->rop == ROP_move_xfer) continue;
    switch (step) {
      case 0: if (in->rop != ROP_local_uninit || in->narg < 1 || in->arg[0].val != 1) return 0; w->uninit_pc = in->pc; break;
      case 1: if (in->rop != ROP_load_this || in->ndef < 1) return 0; lt = (int)in->def[0]; break;
      case 2: if (in->rop != ROP_put_local_s0 || in->nuse < 1 || rg_base(q, in->use[0]) != lt) return 0; w->put_pc = in->pc; break;
      case 3: if (in->rop != ROP_get_local_field_0 || in->ndef < 1 || !rg_atom(in, w->method, sizeof(w->method))) return 0; gf = (int)in->def[0]; w->read_pc = in->pc; break;
      case 4: { char nm[16]; if (in->rop != ROP_get_field2 || in->ndef < 1 || in->nuse < 1 || rg_base(q, in->use[0]) != gf || !rg_atom(in, nm, sizeof(nm)) || strcmp(nm, "apply") != 0) return 0; ga = (int)in->def[0]; break; }
      case 5: if (in->rop != ROP_apply_arguments || in->win_n != 3 || in->win_val == NULL || in->narg < 3 || in->arg[2].val != 1 || in->ndef < 1) return 0;
              if (rg_base(q, in->win_val[0]) != gf || rg_base(q, in->win_val[1]) != ga || rg_base(q, in->win_val[2]) != lt) return 0;
              w->apply_pc = in->pc; break;
      case 6: if (in->rop != ROP_return_undefined) return 0; break;
      default: return 0;
    }
    step++;
  }
  return step == 7;
}
static int g_rg_fwd;   /* rg_call is expanding a wrapper's apply_arguments as a call of `this.m` (set around the call) */
static int rg_native_call(RCtx *c, int ii, const char *method, const Q2Insn *call, const char *resvar, int exit_pc);
static int rg_rty_typed(const RInst *I);   /* region_fn_var.inc: an instance whose result is a typed local */
static void rg_res_decl(FILE *o, const RInst *I);
static void rg_res_set(FILE *o, const RInst *I, int u0);
static void rg_res_get(FILE *o, const RInst *P, int dv, const char *resvar, const RInst *J);
static int rg_math_site(RCtx *c, RInst *I, const Q2Insn *call, const char *method, int *id, int *magic);   /* region_fn_native.inc */
static int rg_math_call(RCtx *c, int ii, const char *method, const Q2Insn *call, const char *resvar, int exit_pc, int id, int magic);   /* region_fn_native.inc */
static int rg_call(RCtx *c, int ii, const char *method_in, const Q2Insn *call, char (*rootcur)[40], const Q2Func *root, int exit_pc, const char *resvar) {
  RInst *I = c->in[ii];
  const JSAotGenFunc *tg[RG_MAXT];
  int nt, t, gi, recv_val = (int)call->use[1], k, clsmode = 0, clsite[RG_MAXT], thru = 0, kc[RG_MAXT], known = 0, tcp = -1, crecv = 0;
  char clctor[RG_MAXT][48];
  char method[40];
  FILE *o = c->o;
  if (call->nuse < 2) return rg_fail(c, "pc %d: a call without a receiver", call->pc);
  snprintf(method, sizeof(method), "%s", method_in);
  if (rg_trust_on() && rg_cold(I, call)) {
    /* a call the profile never executed: its whole subtree is pruned, the call leaves at its method read (AOT inlining principles: cold subtrees are exits) */
    if (c->emit) {
      char xl3[32];
      rg_xlabel(c, rg_xit(c, ii, exit_pc), call->pc, "cold-call", xl3, sizeof(xl3));
      fprintf(o, "    goto %s; /* .%s() at pc %d: never executed in the profile */\n", xl3, method, call->pc);
    }
    return 1;
  }
  nt = g_rg_fwd && I->cls1 > 0 ? qjs_aot_bundle_class_targets(I->g, call->pc, I->cls1 - 1, tg, RG_MAXT) : qjs_aot_bundle_targets(I->g, call->pc, tg, RG_MAXT);   /* a forwarding constructor is shared by every class Class.create made: the targets are the building object's class's */
  if (nt <= 0 && strcmp(method, "push") == 0 && rg_trust_on() && call->win_n == 1 && call->nuse >= 2 && call->ndef >= 1 && call->win_val != NULL)
    return rg_push(c, ii, call, rootcur, root, exit_pc);
  if (nt <= 0 && strcmp(method, "pop") == 0 && rg_trust_on() && call->win_n == 0 && call->nuse >= 2 && call->ndef >= 1) {
    if (!rg_pop(c, ii, call, rootcur, root, exit_pc)) return 0;
    return 1;   /* an element carried over a loop edge is parked in the owned list at the pop (rg_pop) */
  }
  if (nt <= 0 && strcmp(method, "call") == 0 && rg_trust_on() && call->win_n >= 1 && call->win_val != NULL && call->ndef >= 1) {
    nt = qjs_aot_bundle_thru_targets(I->g, call->pc, tg, RG_MAXT);
    thru = nt > 0;
  }
  if (nt <= 0 && rg_cold(I, call)) {
    /* a call nothing is known about, where the profile never went: leave at the method read (the generic body does the read and the call) */
    if (c->emit) {
      char xl3[32];
      rg_xlabel(c, rg_xit(c, ii, exit_pc), call->pc, "cold-call", xl3, sizeof(xl3));
      fprintf(o, "    goto %s; /* .%s() at pc %d: never executed in the profile */\n", xl3, method, call->pc);
    }
    return 1;
  }
  if (nt <= 0) {
    int mid_, mmg_;
    if (rg_math_site(c, I, call, method, &mid_, &mmg_)) return rg_math_call(c, ii, method, call, resvar, exit_pc, mid_, mmg_);
  }
  if (nt <= 0) return rg_native_call(c, ii, method, call, resvar, exit_pc);   /* M5: a generic call out of line (region_fn_native.inc) */
  if (rg_trust_on() && !thru) {
    /* TRUST: the receiver's classes are the analysis's (each one shape, a global constructor, one target); the dispatch compares the receiver's shape with the
       class shapes the region's init built, and `this` of an arm whose class is known needs no compare at all */
    QjsAotRecvClass cl[32];
    int nr = g_rg_fwd && I->cls1 > 0 ? qjs_aot_bundle_receivers_for(I->g, call->pc, 2, I->cls1 - 1, cl, 32) : qjs_aot_bundle_receivers(I->g, call->pc, 2, cl, 32), u, n = 0;
    /* a receiver that is one object a constant path names (`Color.prototype.multiplyScalar(...)`, a namespace of functions) and is no class with a global constructor: the static-method form below */
    if (nr == 1 && nt == 1 && !rg_cls_ok(c, &cl[0]) && getenv("QJS_AOT_REGION_CRECV") != NULL) {
      char pn0[4][40];
      int pnn0 = 0;
      if (rg_cpath(&I->q, recv_val, pn0, &pnn0)) nr = 0;
    }
    if (nr <= 0) {
      char pn[4][40];
      int pnn = 0;
      int this_cp = I->ctp > 0 && rg_is_this(&I->q, (Q2ValueId)recv_val);
      if (nt == 1 && (this_cp || rg_cpath(&I->q, recv_val, pn, &pnn))) {
        /* a static method `G.f.m()` (or `this.m()` inside one): the receiver is the value of a constant path, the method one function found from it */
        tcp = this_cp ? I->ctp - 1 : rg_cp_add(c, pn, pnn);
        if (tcp < 0) return rg_fail(c, "pc %d: too many constant paths", call->pc);
        kc[0] = -1;
        crecv = 1;
        known = 1;
        clsmode = 0;
        goto recv_done;
      }
      if (getenv("QJS_AOT_REGION_WHY")) { char pn2[4][40]; int pn2n = 0, cp_ = rg_cpath(&I->q, recv_val, pn2, &pn2n); fprintf(stderr, "why: .%s pc %d: nr %d nt %d cpath %d (%s)\n", method, call->pc, nr, nt, cp_, pn2n ? pn2[0] : ""); }
      return rg_fail(c, "pc %d: .%s: the receiver classes are not proven (the dispatch would be learned at run time)", call->pc, method);
    }
    for (u = 0; u < nr; u++) {
      const JSAotGenFunc *cand[RG_MAXT];
      int k, w, nc, ci_;
      if (g_rg_fwd && I->cls1 > 0 && cl[u].site != I->cls1 - 1) continue;   /* the object being built is of one class */
      if (!rg_cls_ok(c, &cl[u]) && !(g_meas_on > 0 && !c->emit && !cl[u].one_shape && rg_clctor(c, &cl[u])[0] && cl[u].nfields >= 0 && cl[u].nfields <= 32 && qjs_aot_mshape_on()))
        return rg_fail(c, "pc %d: .%s: receiver class %s is not bound once (%s)", call->pc, method, cl[u].label,
                       !cl[u].one_shape ? "more than one shape" : "no global constructor");
      /* the method's candidates for this class: one, or an override and what it overrides when the analysis cannot order them; the init binds the one the
         class's prototype chain finds (an arm per candidate, the others never taken) */
      if (cl[u].ntargets == 1 && cl[u].target != NULL) { cand[0] = cl[u].target; nc = 1; }
      else nc = qjs_aot_bundle_class_targets(I->g, call->pc, cl[u].site, cand, RG_MAXT);
      if (nc <= 0) return rg_fail(c, "pc %d: .%s: receiver class %s has no resolved method", call->pc, method, cl[u].label);
      k = rg_cls_add(c, cl[u].site, rg_clctor(c, &cl[u]), cl[u].nfields, !cl[u].one_shape);
      if (k < 0) return rg_fail(c, "pc %d: .%s: class %s is not in the class table", call->pc, method, rg_clctor(c, &cl[u]));
      for (ci_ = 0; ci_ < nc; ci_++) {
        for (w = 0; w < n && !(kc[w] == k && tg[w] == cand[ci_]); w++) {}
        if (w < n) continue;
        if (n >= RG_MAXT) return rg_fail(c, "pc %d: .%s: more than %d dispatch arms", call->pc, method, RG_MAXT);
        kc[n] = k; tg[n] = cand[ci_]; clsite[n] = cl[u].site; snprintf(clctor[n], sizeof(clctor[0]), "%s", rg_clctor(c, &cl[u])); n++;
      }
    }
    if (g_rg_fwd && I->cls1 > 0) { if (n == 0) return rg_fail(c, "pc %d: .%s: the object being built is not among the forward's receiver classes", call->pc, method); known = 1; }
    if (ii == 0 && I->kcls == 0 && !c->emit && n > 0 && rg_is_this(&I->q, (Q2ValueId)recv_val) && getenv("QJS_AOT_REGION_NOROOTK") == NULL) {
      /* the root's `this`, when every arm of its first dispatch is one class: its shape is compared once at entry, and its own dispatches need no
         compare. The class's layout is a dependency of the word (an object leaving the shape breaks it, tested after every out-of-line call) */
      for (u = 1; u < n && kc[u] == kc[0]; u++) {}
      if (u == n) { I->kcls = kc[0] + 1; c->cls[kc[0]].need_layout = 1; }
    }
    if (I->kcls > 0 && rg_is_this(&I->q, (Q2ValueId)recv_val)) {
      int m = 0;
      for (u = 0; u < n; u++)
        if (kc[u] == I->kcls - 1) { kc[m] = kc[u]; tg[m] = tg[u]; clsite[m] = clsite[u]; snprintf(clctor[m], sizeof(clctor[0]), "%s", clctor[u]); m++; }
      if (m == 0) return rg_fail(c, "pc %d: .%s on `this` of class %s, which is not among the call's receivers", call->pc, method, c->cls[I->kcls - 1].ctor);
      n = m; known = 1;
    }
    if (n > 1 && getenv("QJS_AOT_REGION_NOARMSORT") == NULL) {
      /* the arms most often taken come first: the dispatch compares the receiver's shape with the class shapes in this order (a stable sort by how many times the profile
         entered each target) */
      unsigned long long hot[RG_MAXT];
      int a, b;
      for (a = 0; a < n; a++) hot[a] = qjs_aot_prof_entries(tg[a]->digest);
      for (a = 1; a < n; a++) {
        const JSAotGenFunc *tg_ = tg[a];
        int kc_ = kc[a], cs_ = clsite[a];
        char cc_[sizeof(clctor[0])];
        unsigned long long h_ = hot[a];
        memcpy(cc_, clctor[a], sizeof(cc_));
        for (b = a; b > 0 && hot[b - 1] < h_; b--) {
          tg[b] = tg[b - 1]; kc[b] = kc[b - 1]; clsite[b] = clsite[b - 1]; hot[b] = hot[b - 1];
          memcpy(clctor[b], clctor[b - 1], sizeof(clctor[0]));
        }
        tg[b] = tg_; kc[b] = kc_; clsite[b] = cs_; hot[b] = h_;
        memcpy(clctor[b], cc_, sizeof(cc_));
      }
    }
    nt = n;
    clsmode = 1;
  recv_done:;
  } else if (rg_trust_on() && thru) {
    char pn[4][40];
    int pnn = 0;
    if (nt != 1) return rg_fail(c, "pc %d: .call with %d targets", call->pc, nt);
    if (!rg_cpath(&I->q, recv_val, pn, &pnn)) return rg_fail(c, "pc %d: .call on a function that is not a constant path G.f...", call->pc);
    tcp = rg_cp_add(c, pn, pnn);
    if (tcp < 0) return rg_fail(c, "pc %d: too many constant paths", call->pc);
  } else {
    QjsAotRecvClass cl[32];
    int nr = rg_trust_on() && !thru ? qjs_aot_bundle_receivers(I->g, call->pc, 2, cl, 8) : -1, ok = nr > 0 && nr <= RG_MAXT, u;
    for (u = 0; ok && u < nr; u++) {
      int w;
      if (cl[u].ntargets != 1 || cl[u].target == NULL || cl[u].escapes || !cl[u].one_shape || !rg_clctor(c, &cl[u])[0]) ok = 0;
      for (w = 0; ok && w < u; w++) if (strcmp(rg_clctor(c, &cl[w]), rg_clctor(c, &cl[u])) == 0) ok = 0;
    }
    if (ok) {
      nt = nr;
      for (u = 0; u < nr; u++) { tg[u] = cl[u].target; clsite[u] = cl[u].site; snprintf(clctor[u], sizeof(clctor[0]), "%s", rg_clctor(c, &cl[u])); }
      clsmode = 1;
    }
  }
  gi = rg_guard(c, I->id, recv_val, call->pc, 1, method);
  if (gi < 0) return rg_fail(c, "too many guards");
  if (!c->emit) {
    RGuard *G = &c->gd[gi];
    G->thru = thru;
    G->fwd = g_rg_fwd;
    if (G->ntg == 0) { G->ntg = nt; for (t = 0; t < nt; t++) G->tg[t] = tg[t]; G->cls_mode = clsmode; for (t = 0; clsmode && t < nt; t++) { G->cls_site[t] = clsite[t]; snprintf(G->cls_ctor[t], sizeof(G->cls_ctor[0]), "%s", clctor[t]); G->kcls[t] = kc[t]; } G->known = known; G->tcp = tcp; G->crecv = crecv; if (crecv) G->kcls[0] = -1; }
    else if (G->ntg != nt) return rg_fail(c, "pc %d: .%s dispatches differently at two sites", call->pc, method);
    else for (t = 0; t < nt; t++) if (G->tg[t] != tg[t]) return rg_fail(c, "pc %d: .%s dispatches differently at two sites", call->pc, method);
  }
  /* the arms */
  for (t = 0; t < nt; t++) {
    int ci;
    RInst *J;
    ci = c->nin++;
    J = rg_newinst(c, ci);
    if (!c->emit || J->g == NULL) {
      memset(J, 0, sizeof(*J));
      if (q2_build(tg[t], &J->q) < 0 || !J->q.ok) { q2_free(&J->q); J->g = NULL; return rg_fail(c, "pc %d: the callee's IR does not build", call->pc); }
      J->g = tg[t];
    }
    if (call->win_n > 8) return rg_fail(c, "pc %d: a call with more than 8 arguments", call->pc);
    {
      /* a receiver that is a global's value is never loaded by the region (it is undefined here): a callee that reads `this` would see the wrong object */
      int rb_ = rg_base(&I->q, (Q2ValueId)recv_val), di_ = (int32_t)rb_ >= 0 && rb_ < I->q.nvalues ? I->q.values[rb_].insn : -1, ji_;
      if (!rg_trust_on() && di_ >= 0 && di_ < I->q.ninsns && I->q.insns[di_].rop == ROP_get_global)
        for (ji_ = 0; ji_ < J->q.ninsns; ji_++) {
          int rp_ = J->q.insns[ji_].rop;
          if (rp_ == ROP_load_this || rp_ == ROP_get_this_field || rp_ == ROP_get_this_field2) return rg_fail(c, "pc %d: .%s is called on a global the region does not load, and it reads `this`", call->pc, method);
        }
    }
    if (J->g->light_var_count > 0 || J->g->ctl_count != 0 || J->g->func_kind != 0) return rg_fail(c, "pc %d: the callee has a variable area or control records", call->pc);
    if ((int32_t)call->def[0] < 0 || (int32_t)call->def[0] >= I->q.nvalues || I->q.values[call->def[0]].reg < 0) return rg_fail(c, "pc %d: the call's result has no register", call->pc);
    J->id = ci;
    J->cls1 = clsmode ? clsite[t] + 1 : 0;
    J->depth = I->depth + 1;
    snprintf(J->pfx, sizeof(J->pfx), "r%d_", ci);
    snprintf(J->self, sizeof(J->self), "%sv%u", I->pfx, (unsigned)(thru ? call->win_val[0] : (Q2ValueId)recv_val));
    if (g_rg_fwd) {
      /* the wrapper's own arguments, exactly as its caller passed them (argc included) */
      J->nargs = I->nargs;
      for (k = 0; k < J->nargs; k++) snprintf(J->arg[k], sizeof(J->arg[0]), "%s", I->arg[k]);
    } else {
    J->nargs = (thru ? call->win_n - 1 : call->win_n) > 8 ? 8 : (thru ? call->win_n - 1 : call->win_n);
    for (k = 0; k < J->nargs; k++) snprintf(J->arg[k], sizeof(J->arg[0]), "%sv%u", I->pfx, (unsigned)call->win_val[k + (thru ? 1 : 0)]);
    }
    J->owner_g = thru ? -1 : gi;
    J->owner_t = thru ? 0 : t;
    J->par = ii;
    J->par_call_pc = call->pc;
    J->par_read_pc = exit_pc;
    J->par_pc_after = call->next;
    J->par_dst = I->q.values[call->def[0]].reg;
    J->par_dv = (int)call->def[0];
    J->par_kind = 0;
    J->par_cons = 0;
    if (thru) snprintf(J->co, sizeof(J->co), "rg_tf_%s_%d[%d]", c->hex, gi, t);
    else if (rg_trust_on()) snprintf(J->co, sizeof(J->co), "rg_fv_%s_%d[%d]", c->hex, gi, t);   /* bound and held at init */
    else snprintf(J->co, sizeof(J->co), "rg_fo_%s_%d(ctx, %s, %d)", c->hex, gi, J->self, t);
    J->kcls = rg_trust_on() && !thru && !crecv ? kc[t] + 1 : 0;
    J->ctp = rg_trust_on() && crecv ? tcp + 1 : 0;
    J->ool = rg_trust_on() && getenv("QJS_AOT_REGION_NOOOL") == NULL && rg_has_loop(&J->q);
    if (J->ool && c->loopinl && nt == 1 && J->q.nvalues <= 400) {
      /* a small loop callee with one target, not on the call chain already (a recursion): expanded in place, its loop a loop of the region */
      int a_, rec_ = 0;
      for (a_ = ii; a_ >= 0 && !rec_; a_ = c->in[a_]->par) { if (c->in[a_]->g == J->g) rec_ = 1; if (a_ == 0) break; }
      if (!rec_) J->ool = 0;
      if (getenv("QJS_AOT_REGION_WHY")) fprintf(stderr, "loopinl: %s (%d values) at pc %d: %s\n", J->g->name[0] ? J->g->name : "<anon>", J->q.nvalues, call->pc, rec_ ? "recursive, out of line" : "inlined");
    } else if (J->ool && c->loopinl && getenv("QJS_AOT_REGION_WHY")) fprintf(stderr, "loopinl: %s (%d values) at pc %d: out of line (%s)\n", J->g->name[0] ? J->g->name : "<anon>", J->q.nvalues, call->pc, nt != 1 ? "several targets" : "too large");
    if (J->ool && !c->emit) c->opaque = 1;
    if (rg_trust_on() && thru && I->kcls > 0 && call->win_n >= 1 && rg_is_this(&I->q, call->win_val[0])) { J->kcls = I->kcls; J->cls1 = I->cls1; }
    /* a super-constructor call on the object this region is constructing: the callee builds the same object */
    J->fresh = (thru && I->fresh && I->cons > 0 && rg_is_this(&I->q, call->win_val[0])) || (g_rg_fwd && I->fresh && I->cons > 0 && rg_is_this(&I->q, (Q2ValueId)recv_val));
    if (J->fresh && J->ool) {
      /* an out-of-line super-constructor would see the object: only once it is complete, and then it is an ordinary object to it */
      if (!c->cons[I->cons - 1].esc) return rg_fail(c, "pc %d: an out-of-line super-constructor call on an object still being built", call->pc);
      J->fresh = 0;
    }
    J->cons = J->fresh ? I->cons : 0;
    J->ploop = I->ploop || I->curloop;
    if (!J->ool && J->depth > 14) return rg_fail(c, "inlining deeper than 14 (the exit chain holds 16 frames)");
  }
  /* In the second pass the instances created above are the ones the first pass made, in the same order, so index them from the first of this call. */
  if (c->emit) {
    int base = c->nin - nt;
    RGuard *G = &c->gd[gi];
    /* The unguarded form (one resolved target, nothing to verify) takes any receiver for the one the bundle knows. A receiver the bundle cannot see (an object a host
       inserted, whose own method of that name shadows) would run the target's code on it. So the receiver's shape is still checked against the learned shapes
       (rg_lookup: a load and a compare), and only the table switch is dropped; QJS_AOT_REGION_UNGUARDED restores the bare form (a vkit knob: t-trust-delete aborts or
       computes a wrong value with it). */
    int skipl_ = nt == 1 && rg_trust_on() && G->ncommon == 0 && G->nreq[0] == 0 && getenv("QJS_AOT_REGION_UNGUARDED") != NULL;
    int sw0_ = nt == 1 && rg_trust_on() && G->ncommon == 0 && G->nreq[0] == 0;
    fprintf(o, "    { /* %s.%s() at pc %d */\n", I->pfx, method, call->pc);
    if (rg_trust_on()) {
      /* TRUST: no learner. The receiver's shape against the class shapes bound at init (a held shape is its class: the prototype is part of it, and the
         methods found through it are facts of the word); X of `X.call` against the function the constant path named; `this` of a known class: one arm */
      char val[40], xl2[32];
      int tt;
      snprintf(val, sizeof(val), "%sv%u", I->pfx, (unsigned)recv_val);
      rg_xlabel(c, rg_xit(c, ii, exit_pc), call->pc, "dispatch-class", xl2, sizeof(xl2));
      if (g_rg_fwd && getenv("QJS_AOT_REGION_WRAPEXIT_BUILD") != NULL) fprintf(o, "      if (__builtin_expect(rg_wrapexit_%s(), 0)) goto %s;   /* vkit: a forced exit at the forward's method read */\n", c->hex, xl2);
      if (nt == 1 && !G->known && !thru && c->nhk < 250 && rg_hoist_on(c) && (I->curloop || I->ploop) && rg_inv_has(c, val) && rg_orig(c, val) != NULL) {
        /* the same, the check made once at the region's entry (a receiver of another shape sends the whole run to the generic body) */
        const char *ov = rg_orig(c, val);
        char key[120];
        int k_;
        snprintf(key, sizeof(key), "S|%s|%d", ov, gi);
        if (rg_hk(c, key, &k_) == 0)
          rs_add(&c->hoist, "    if (__builtin_expect(JS_VALUE_GET_TAG(%s) != JS_TAG_OBJECT || *(const void *const *)((const char *)JS_VALUE_GET_PTR(%s) + QJS_AOT_OFF_JSOBJECT_SHAPE) != rk_sa_%s_%d[0], 0)) return aot_g_%s(ctx, f, fr, entry_pc);\n    rgmv_%d = 1;\n", ov, ov, c->hex, gi, c->hex, k_);
        c->in[base]->hver = 1;
        fprintf(o, "      switch (0) {\n");
      } else
      if (nt == 1 && !G->known && !thru && rg_inv_on(c) && (I->curloop || I->ploop) && rg_inv_has(c, val)) {
        /* one arm on a receiver that is the same object on every pass (its shape cannot change: the region adds no field to an object it did not make) */
        int k_ = c->nmemo++;
        fprintf(o, "      if (__builtin_expect(!rgmv_%d, 0)) { if (__builtin_expect(JS_VALUE_GET_TAG(%s) != JS_TAG_OBJECT || *(const void *const *)((const char *)JS_VALUE_GET_PTR(%s) + QJS_AOT_OFF_JSOBJECT_SHAPE) != rk_sa_%s_%d[0], 0)) goto %s; rgmv_%d = 1; }\n", k_, val, val, c->hex, gi, xl2, k_);
        fprintf(o, "      switch (0) {\n");
      } else if (nt == 1 && !G->known) {
        /* one arm: a compare and an exit (no arm index to select) */
        if (thru) fprintf(o, "      if (__builtin_expect(JS_VALUE_GET_TAG(%s) != JS_TAG_OBJECT || JS_VALUE_GET_PTR(%s) != (const void *)rg_tf_%s_%d[0], 0)) goto %s;\n", val, val, c->hex, gi, xl2);
        else fprintf(o, "      if (__builtin_expect(JS_VALUE_GET_TAG(%s) != JS_TAG_OBJECT || *(const void *const *)((const char *)JS_VALUE_GET_PTR(%s) + QJS_AOT_OFF_JSOBJECT_SHAPE) != rk_sa_%s_%d[0], 0)) goto %s;\n", val, val, c->hex, gi, xl2);
        fprintf(o, "      switch (0) {\n");
      } else if (thru) {
        fprintf(o, "      int gix_%d = -1;\n      if (__builtin_expect(JS_VALUE_GET_TAG(%s) != JS_TAG_OBJECT, 0)) goto %s;\n      { const void *xp_ = JS_VALUE_GET_PTR(%s);\n", gi, val, xl2, val);
        for (tt = 0; tt < nt; tt++) fprintf(o, "        %sif ((const void *)rg_tf_%s_%d[%d] == xp_) gix_%d = %d;\n", tt ? "else " : "", c->hex, gi, tt, gi, tt);
        fprintf(o, "      }\n      switch (gix_%d) {\n", gi);
      } else if (G->known) {
        /* `this` of a known class: the arm is the candidate the init bound */
        if (nt == 1) fprintf(o, "      switch (0) {\n");
        else fprintf(o, "      switch (rg_kt_%s_%d) {\n", c->hex, gi);
      } else {
        fprintf(o, "      int gix_%d = -1;\n      if (__builtin_expect(JS_VALUE_GET_TAG(%s) != JS_TAG_OBJECT, 0)) goto %s;\n", gi, val, xl2);
        fprintf(o, "      { const void *sh_ = *(const void *const *)((const char *)JS_VALUE_GET_PTR(%s) + QJS_AOT_OFF_JSOBJECT_SHAPE);\n", val);
        /* an arm's shape is its class's, or NULL when the init found another candidate's method on that class */
        for (tt = 0; tt < nt; tt++) fprintf(o, "        %sif (sh_ == rk_sa_%s_%d[%d]) gix_%d = %d;\n", tt ? "else " : "", c->hex, gi, tt, gi, tt);
        fprintf(o, "      }\n      switch (gix_%d) {\n", gi);
      }
    } else {
    {
      char val[40];
      char xl2[32];
      snprintf(val, sizeof(val), "%sv%u", I->pfx, (unsigned)recv_val);
      rg_xlabel(c, rg_xit(c, ii, exit_pc), call->pc, "dispatch-lookup", xl2, sizeof(xl2));
      if (thru) {
        int tt;
        fprintf(o, "      int gix_%d;\n      if (__builtin_expect(JS_VALUE_GET_TAG(%s) != JS_TAG_OBJECT, 0)) goto %s;\n      { const void *xp_ = JS_VALUE_GET_PTR(%s);\n", gi, val, xl2, val);
        for (tt = 0; tt < nt; tt++) fprintf(o, "        %sif ((const void *)rg_tf_%s_%d[%d] == xp_) gix_%d = %d;\n", tt ? "else " : "", c->hex, gi, tt, gi, tt);
        fprintf(o, "        else { gix_%d = rg_learn_%s_%d(ctx, %s); if (gix_%d < 0) goto %s; } }\n", gi, c->hex, gi, val, gi, xl2);
      } else if (!(skipl_)) rg_lookup(c, gi, val, xl2, "      ");
      else {
        /* no shape lookup, but the word must hold this dispatch's facts (the layout of the receiver's shape, the method chain): learn once, from the first receiver */
        fprintf(o, "      int gix_%d = 0;\n      if (__builtin_expect(!rg_sk_%s_%d, 0)) { if (rg_learn_%s_%d(ctx, %s) < 0) goto %s; rg_sk_%s_%d = 1; }\n", gi, c->hex, gi, c->hex, gi, val, xl2, c->hex, gi);
      }
    }
    if (thru) fprintf(o, "      switch (gix_%d) {\n", gi);
    else if (skipl_ || sw0_) fprintf(o, "      switch (0) {\n");
    else fprintf(o, "      switch (rg_tg_%s_%d[gix_%d]) {\n", c->hex, gi, gi);
    }
    for (t = 0; t < nt; t++) {
      RInst *J = c->in[base + t];
      fprintf(o, "      case %d: {\n", t);
      if (J->ool) {
        /* an out-of-line call (TRUST): the callee runs its own compiled body. Every value the region's frames hold is a borrow the callee could free, so
           each is kept alive (a reference held to the region's end); the result is owned and held the same way; after the call the word is tested
           (the callee may have broken a fact) and a broken word leaves after the call with the result in place */
        int ka, reg;
        char xo[32];
        rg_note_park(c, ii);   /* the result is owned to the region's end (or until a back edge frees it) */
        {
          /* the callee's own native entry when the unit has one (what JS_Call reaches after its dispatch), its argument buffer padded to the
             callee's parameter count as the engine's call would */
          int nb_ = J->nargs > J->g->arg_count ? J->nargs : J->g->arg_count;
          char hx_[33];
          rg_hexof(J->g, hx_);
          fprintf(o, "        JSValue av_[%d];\n", nb_ > 0 ? nb_ : 1);
          for (ka = 0; ka < J->nargs; ka++) fprintf(o, "        av_[%d] = %s;\n", ka, J->arg[ka]);
          for (; ka < nb_; ka++) fprintf(o, "        av_[%d] = JS_UNDEFINED;\n", ka);
          rg_ool_live(c, ii, call, o);
          rg_emit_cap(c, ii, call->pc, o);
          fprintf(o, "#ifdef Q2_NATIVE_%s\n        { JSValue r_ = %s ? aot_n_%s(ctx, f, (JSObject *)(%s), %s, av_, %d) : JS_Call(ctx, JS_MKPTR(JS_TAG_OBJECT, %s), %s, %d, av_);\n#else\n",
                  hx_, getenv("QJS_AOT_REGION_NODIRECTCALL") ? "0" : "1", hx_, J->co, J->self, J->nargs, J->co, J->self, J->nargs);
          fprintf(o, "        { JSValue r_ = JS_Call(ctx, JS_MKPTR(JS_TAG_OBJECT, %s), %s, %d, av_);\n#endif\n", J->co, J->self, J->nargs);
        }
        fprintf(o, "          if (__builtin_expect(JS_IsException(r_), 0)) { RG_FREE_OWNED(); return JS_EXCEPTION; }\n");
        fprintf(o, "          rg_own_put(ctx, &rgo_, &rgon_, &rgoc_, r_); %s = r_; }\n", resvar);
        reg = (int32_t)call->def[0] >= 0 && (int32_t)call->def[0] < I->q.nvalues ? I->q.values[call->def[0]].reg : -1;
        if (I->cur != NULL && reg >= 0 && reg < 256) snprintf(I->cur[reg], 40, "%s", resvar);
        rg_xlabel(c, rg_xit(c, ii, call->next), call->pc, "ool-facts", xo, sizeof(xo));
        fprintf(o, "        if (__builtin_expect(!QJS_AOT_FACTS_OK(&rg_fw_%s, 1) || rgon_ >= RG_OWNCAP, 0)) goto %s;\n        break; }\n", c->hex, xo);
        continue;
      }
      if (!rg_walk_inst(c, base + t, rootcur, root)) return 0;
      rg_res_get(o, I, (int)call->def[0], resvar, J);
      fprintf(o, "        break; }\n");
    }
    {
      /* every arm called out of line: whichever ran put its result in the owned list, so the result needs no reference of its own across a later call */
      int all_ = 1, t2, k2;
      for (t2 = 0; t2 < nt; t2++) if (!c->in[base + t2]->ool) all_ = 0;
      for (k2 = 0; all_ && k2 < c->nownres; k2++) if (strcmp(c->ownres[k2], resvar) == 0) all_ = 0;
      if (all_ && c->nownres < 256 && strlen(resvar) < sizeof(c->ownres[0])) snprintf(c->ownres[c->nownres++], sizeof(c->ownres[0]), "%s", resvar);
    }
    {
      char xd[32];
      rg_xlabel(c, rg_xit(c, ii, exit_pc), call->pc, "dispatch-default", xd, sizeof(xd));
      fprintf(o, "      default: goto %s;\n      }\n    }\n", xd);
    }
    (void)G;
  } else {
    int base = c->nin - nt;
    for (t = 0; t < nt; t++)
      if (!c->in[base + t]->ool && !rg_walk_inst(c, base + t, rootcur, root)) return 0;
  }
  return 1;
}

static const char *rg_cmpop(int rop) {
  switch (rop) {
    case ROP_lt: return "<";
    case ROP_lte: return "<=";
    case ROP_gt: return ">";
    case ROP_gte: return ">=";
    case ROP_eq: case ROP_strict_eq: return "==";
    case ROP_neq: case ROP_strict_neq: return "!=";
  }
  return NULL;
}

static const char *rg_subop(int sub) {
  switch (sub & 0x3f) {
    case REG_BIN_LT: return "<";
    case REG_BIN_LTE: return "<=";
    case REG_BIN_GT: return ">";
    case REG_BIN_GTE: return ">=";
    case REG_BIN_EQ: case REG_BIN_STRICT_EQ: return "==";
    case REG_BIN_NEQ: case REG_BIN_STRICT_NEQ: return "!=";
  }
  return NULL;
}

/* `a <sub> b` on two values of C text (the arithmetic of the add/sub/mul/div cases above, and the number comparisons): an int pair gives an int when the result is one,
   anything else that is a number gives a double, any other operand leaves through `xl`. */
static void rg_arith(FILE *o, int sub, const char *dst, const char *a, const char *b, const char *xl) {
  const char *opc = sub == REG_BIN_ADD ? "+" : sub == REG_BIN_SUB ? "-" : sub == REG_BIN_MUL ? "*" : sub == REG_BIN_DIV ? "/" : NULL;
  const char *cmp = rg_subop(sub);
  fprintf(o, "        { double x_, y_; int ia_ = JS_VALUE_GET_TAG(%s) == JS_TAG_INT, ib_ = JS_VALUE_GET_TAG(%s) == JS_TAG_INT;\n", a, b);
  if (opc != NULL) {
    fprintf(o, "          if (ia_ && ib_) {\n");
    if (sub == REG_BIN_DIV) {
      fprintf(o, "            double d_ = (double)JS_VALUE_GET_INT(%s) / (double)JS_VALUE_GET_INT(%s); int32_t t_ = (int32_t)d_;\n", a, b);
      fprintf(o, "            %s = (d_ == (double)t_ && !(t_ == 0 && __builtin_signbit(d_)) && d_ >= -2147483648.0 && d_ <= 2147483647.0) ? JS_MKVAL(JS_TAG_INT, t_) : __JS_NewFloat64(d_);\n", dst);
    } else {
      fprintf(o, "            int64_t r_ = (int64_t)JS_VALUE_GET_INT(%s) %s (int64_t)JS_VALUE_GET_INT(%s);\n", a, opc, b);
      if (sub == REG_BIN_MUL)
        fprintf(o, "            if (r_ == 0 && (JS_VALUE_GET_INT(%s) < 0 || JS_VALUE_GET_INT(%s) < 0)) %s = __JS_NewFloat64(-0.0); else\n", a, b, dst);
      fprintf(o, "            %s = (r_ >= INT32_MIN && r_ <= INT32_MAX) ? JS_MKVAL(JS_TAG_INT, (int32_t)r_) : __JS_NewFloat64((double)r_);\n", dst);
    }
    fprintf(o, "          } else {\n");
  }
  fprintf(o, "          if (ia_) x_ = JS_VALUE_GET_INT(%s); else if (JS_VALUE_GET_TAG(%s) == JS_TAG_FLOAT64) x_ = JS_VALUE_GET_FLOAT64(%s); else goto %s;\n", a, a, a, xl);
  fprintf(o, "          if (ib_) y_ = JS_VALUE_GET_INT(%s); else if (JS_VALUE_GET_TAG(%s) == JS_TAG_FLOAT64) y_ = JS_VALUE_GET_FLOAT64(%s); else goto %s;\n", b, b, b, xl);
  if (opc != NULL) fprintf(o, "          %s = __JS_NewFloat64(x_ %s y_); } }\n", dst, opc);
  else fprintf(o, "          %s = x_ %s y_ ? JS_TRUE : JS_FALSE; }\n", dst, cmp);
}

/* int32 payload of a value, or exit */
#define RG_INT(var, expr, xl) fprintf(o, "        int32_t %s; if (__builtin_expect(JS_VALUE_GET_TAG(%s) != JS_TAG_INT, 0)) goto %s; %s = JS_VALUE_GET_INT(%s);\n", var, expr, xl, var, expr)

/* Translate one instance. `rootcur` is the root's register-to-value table (NULL in the first walk); every instance keeps its own (I->cur), and an exit anywhere
   materialises the whole chain of frames (rg_xit). */
/* file-scope helpers of the mechanisms added in the R3 batch; one fragment per mechanism (each owned by one author) */
#include "region_fn_bit.inc"
#include "region_fn_native.inc"
#include "region_fn_var.inc"
static int rg_walk_inst(RCtx *c, int ii, char (*rootcur)[40], const Q2Func *root) {
  RInst *I = c->in[ii];
  const Q2Func *fn = &I->q;
  FILE *o = c->o;
  int is_root = ii == 0, b, k, i;
  char xl[32];
  I->pend_end = 0;
  I->npend = 0; I->lastcall = I->nextcall = -1;
  if (c->emit) {
    /* the frame state of this walk: the value each register holds now (the root's table is the caller's) */
    if (is_root) I->cur = rootcur;
    else if (I->cur == NULL) I->cur = calloc(256, 40);
    else memset(I->cur, 0, 256 * 40);
    if (I->cur == NULL) abort();
  }
  if (I->mname == NULL) { I->mname = calloc((size_t)(fn->nvalues > 0 ? fn->nvalues : 1), 40); I->mpc = calloc((size_t)(fn->nvalues > 0 ? fn->nvalues : 1), sizeof(int)); }
  if (c->emit) {
    fprintf(o, "        /* inlined: %s%s */\n", I->g->name[0] ? I->g->name : "<anon>", "");
    if (!is_root) fprintf(o, "/*@RGI %d@*/\n", I->id);   /* the values of this instance an exit can read are set here (filled in at the end) */
    if (rg_rty_typed(I)) rg_res_decl(o, I);
    else fprintf(o, "        JSValue %sres = JS_UNDEFINED; (void)%sres;\n", I->pfx, I->pfx);
  }
  for (b = 0; b < fn->nblocks; b++) {
    const Q2Block *B = &fn->blocks[b];
    int skip_call = -1;
    if (fn->rpo_pos != NULL && fn->rpo_pos[b] < 0) continue;
    /* a loop inside an inlined function is a loop of the region like the root's: the blocks are labelled per instance, the values are locals of the function, and
       its back edges poll (below) with the instance's frame state, the chain of frames materialised at the poll exit */
    if (c->emit) fprintf(o, "        %sB%d: (void)0;\n", I->pfx, b);
    if (b >= 128) return rg_fail(c, "too many blocks");
    I->curloop = rg_in_loop(fn, b);
    if (I->cur != NULL) {
      int j;
      /* every register live on entry holds the value the block says (a trivial parameter was dropped for the dominating value, which this table may have lost to another path) */
      for (j = 0; j < B->nlive_in; j++)
        if (B->in_reg[j] >= 0 && B->in_reg[j] < 256 && (int32_t)B->in_val[j] >= 0 && (int32_t)B->in_val[j] < fn->nvalues) snprintf(I->cur[B->in_reg[j]], 40, "%sv%u", I->pfx, (unsigned)B->in_val[j]);
      for (j = 0; j < B->nparams; j++)
        if (B->param_reg[j] >= 0 && B->param_reg[j] < 256) snprintf(I->cur[B->param_reg[j]], 40, "%sv%u", I->pfx, (unsigned)B->param_val[j]);
    }
    for (k = 0; k < B->ninsns; k++) {
      int idx = B->first_insn + k;
      const Q2Insn *in = &fn->insns[idx];
      int dv = in->ndef >= 1 ? (int)in->def[0] : -1, dv2 = in->ndef >= 2 ? (int)in->def[1] : -1;
      int u0 = in->nuse >= 1 ? (int)in->use[0] : -1, u1 = in->nuse >= 2 ? (int)in->use[1] : -1;
      int xpc, np_ = 0, pj_;
      I->curidx = idx;
      I->lastcall = I->nextcall;
      if (in->rop == ROP_call_method || in->rop == ROP_call || in->rop == ROP_construct || in->rop == ROP_apply_arguments) I->nextcall = idx;
      for (pj_ = 0; pj_ < I->npend; ) { if (I->pend[pj_].end <= idx) { memmove(&I->pend[pj_], &I->pend[pj_ + 1], (size_t)(I->npend - pj_ - 1) * sizeof(I->pend[0])); I->npend--; } else pj_++; }
      for (pj_ = 0; pj_ < I->npend; pj_++) if (I->pend[pj_].end > idx + 1) np_++;
      xpc = in->pc;
      if (!rg_pstack()) xpc = idx + 1 < I->pend_end ? I->pend_pc : in->pc;
      else if (np_ == 1) {
        /* one pair pending and the arguments so far made no call: the exit resumes at its read (the interpreter makes the callee again) */
        for (pj_ = 0; pj_ < I->npend; pj_++) if (I->pend[pj_].end > idx + 1 && I->lastcall < I->pend[pj_].ridx) xpc = I->pend[pj_].pc;
      }
      int pair_read = 0, ov_active = 0;
      char (*ovsave)[40] = NULL;
      char tmp_[256];
      int ldmask_ = 0;
      if (idx == skip_call) continue;
      if (rg_trust_on() && I->fresh && I->cons > 0 && !rg_cold(I, in)) {
        /* the object being constructed is seen by anything but the constructor's own field reads and stores (and a super-constructor `X.call(this, ...)`,
           which builds the same object) */
        int seen_ = 0, kk_;
        for (kk_ = 0; kk_ < in->nuse && !seen_; kk_++) {
          if (!rg_is_this(fn, in->use[kk_])) continue;
          switch (in->rop) {
            case ROP_put_field: case ROP_put_field_xfer:
            case ROP_get_field: case ROP_get_field_own: case ROP_get_field2: case ROP_get_field2_own:
            case ROP_get_field_chain: case ROP_get_field_chain_own:
              if (kk_ != 0) seen_ = 1;
              break;
            case ROP_move: case ROP_move_s: case ROP_move_xfer: break;
            case ROP_put_local_s0: case ROP_apply_arguments: if (!I->wrap) seen_ = 1; break;   /* a forwarding constructor: the copy of `this` and the forward are its pattern */
            default: seen_ = 1;
          }
        }
        for (kk_ = 0; kk_ < in->win_n && in->win_val != NULL && !seen_; kk_++) {
          if (!rg_is_this(fn, in->win_val[kk_])) continue;
          if (kk_ == 0 && in->rop == ROP_call_method && in->nuse >= 2 && rg_is_thru_recv(fn, rg_base(fn, in->use[1]))) continue;
          if (I->wrap && in->rop == ROP_apply_arguments) continue;   /* the forward is a call of this.m on the object being built: handled by rg_call */
          seen_ = 1;
        }
        if (seen_ && !rg_cons_seen(c, I, in->pc, o)) return 0;
        /* a read of the object's own field name before the constructor's store (walk order): without an own property the read sees the prototype's default. A direct object is made with
           the defaults already in its slots (below), so it reads right; any other object has no slot yet, and the construct is refused at the end of its walk. */
        if (!seen_ && !c->emit && (in->rop == ROP_get_field || in->rop == ROP_get_field_own || in->rop == ROP_get_field2 || in->rop == ROP_get_field2_own || in->rop == ROP_get_field_chain || in->rop == ROP_get_field_chain_own) && in->nuse >= 1 && rg_is_this(fn, in->use[0])) {
          RCons *K_ = &c->cons[I->cons - 1];
          char rn_[40];
          if (rg_atom(in, rn_, sizeof(rn_))) {
            int ni_;
            for (ni_ = 0; ni_ < K_->nn && strcmp(K_->names[ni_], rn_) != 0; ni_++) {}
            if (ni_ == K_->nn || !(K_->seen & (1u << ni_))) {
              int q_;
              for (q_ = 0; q_ < K_->nrbs && strcmp(K_->rbs[q_], rn_) != 0; q_++) {}
              if (q_ == K_->nrbs && K_->nrbs < 8) snprintf(K_->rbs[K_->nrbs++], 40, "%s", rn_);
              else if (q_ == K_->nrbs) return rg_fail(c, "pc %d: too many reads of the constructed object before their stores", in->pc);
            }
          }
        }
      }
      /* the exit label of this instruction */
      if (c->emit) {
        char rn[40];
        int xi;
        if (I->cur != NULL && in->rop == ROP_construct && in->narg >= 3 && in->nuse >= 2 && in->win_val != NULL) {
          /* an exit AT a root construct resumes at the construct in the generic body: the registers it reads (the constructor twice, the arguments in the window)
             are written too, for this instruction and for the constructor's body expanded under it; the table is put back after the instruction */
          int kw, bv_ = rg_base(fn, in->use[0]);
          ovsave = malloc(256 * 40);
          if (ovsave == NULL) abort();
          memcpy(ovsave, I->cur, 256 * 40);
          ov_active = 1;
          if (in->arg[1].val / 16 >= 0 && in->arg[1].val / 16 < 256) snprintf(I->cur[in->arg[1].val / 16], 40, "%sv%d", I->pfx, bv_);
          if (in->arg[2].val / 16 >= 0 && in->arg[2].val / 16 < 256) snprintf(I->cur[in->arg[2].val / 16], 40, "%sv%d", I->pfx, bv_);
          for (kw = 0; kw < in->win_n; kw++)
            if (in->win_reg + kw >= 0 && in->win_reg + kw < 256) snprintf(I->cur[in->win_reg + kw], 40, "%sv%u", I->pfx, (unsigned)in->win_val[kw]);
        }
        xi = rg_xit(c, ii, xpc);
        e_rop_name((uint16_t)in->rop, rn, sizeof(rn));
        rg_xlabel(c, xi, in->pc, rn, xl, sizeof(xl));
      } else {
        snprintf(xl, sizeof(xl), "rx_x");
      }
      /* F64: a typed producer, or a numeric consumer of typed operands, is emitted by region_fn_var.inc; a typed load goes through the old case into a scratch JSValue */
      if (rg_ty_op(c, I, in, dv, dv2, u0, u1, xl)) goto rg_op_done;
      ldmask_ = c->emit ? rg_ty_wrap_begin(c, I, in, o) : 0;
      switch (in->rop) {
        case ROP_load_undefined: if (c->emit) fprintf(o, "        %sv%d = JS_UNDEFINED;\n", I->pfx, dv); break;
        case ROP_load_null: if (c->emit) fprintf(o, "        %sv%d = JS_NULL;\n", I->pfx, dv); break;
        case ROP_load_true: if (c->emit) fprintf(o, "        %sv%d = JS_TRUE;\n", I->pfx, dv); break;
        case ROP_load_false: if (c->emit) fprintf(o, "        %sv%d = JS_FALSE;\n", I->pfx, dv); break;
        case ROP_load_i8: case ROP_load_i8_s: case ROP_load_i32:
          if (in->narg < 2 || dv < 0) return rg_fail(c, "pc %d: a constant", in->pc);
          if (c->emit) fprintf(o, "        %sv%d = JS_MKVAL(JS_TAG_INT, %d);\n", I->pfx, dv, (int)in->arg[1].val);
          break;
#include "region_ops_const.inc"
        case ROP_put_local_s0:
          if (!I->wrap) return rg_fail(c, "pc %d: put_local_s0 is not in the region subset (and the profile executed it)", in->pc);
          break;   /* a wrapper's two slots: nothing is made here, an exit writes them from its resume pc */
        case ROP_get_local_field_0:
          if (!I->wrap || dv < 0) return rg_fail(c, "pc %d: get_local_field_0 is not in the region subset", in->pc);
          if (c->emit) fprintf(o, "        %sv%d = JS_UNDEFINED;   /* the method is read at the apply (dispatch) */\n", I->pfx, dv);
          break;
        case ROP_apply_arguments: {
          Q2Insn syn;
          char rv_[48];
          int r_;
          if (!I->wrap || in->win_n != 3 || in->win_val == NULL) return rg_fail(c, "pc %d: apply_arguments is not in the region subset", in->pc);
          syn = *in;
          syn.rop = ROP_call_method; syn.nuse = 2; syn.use[0] = in->win_val[0]; syn.use[1] = in->win_val[2];
          syn.win_n = 0; syn.win_val = NULL;
          snprintf(rv_, sizeof(rv_), "%sv%d", I->pfx, (int)in->def[0]);
          g_rg_fwd = 1;
          r_ = rg_call(c, ii, I->wr_method, &syn, rootcur, root, I->wr_read_pc, rv_);
          g_rg_fwd = 0;
          if (!r_) return 0;
          break;
        }
        case ROP_load_this:
          if (dv < 0) return rg_fail(c, "pc %d: load_this", in->pc);
          if (c->emit) fprintf(o, "        %sv%d = %s;\n", I->pfx, dv, I->self);
          if (c->emit && (is_root || rg_inv_has(c, I->self))) { snprintf(tmp_, sizeof(tmp_), "%sv%d", I->pfx, dv); rg_inv_add(c, tmp_); rg_orig_set(c, tmp_, rg_orig(c, I->self)); }
          break;
        case ROP_get_arg: case ROP_get_arg2: {
          int s0 = in->narg > 1 ? (int)in->arg[1].val : 0;
          if (dv < 0) return rg_fail(c, "pc %d: get_arg", in->pc);
          if (c->emit) {
            if (is_root) {
              fprintf(o, "        %sv%d = %d < f_argc ? f_argv[%d] : JS_UNDEFINED;\n", I->pfx, dv, s0, s0);
              /* an argument the root never replaces: the same value on every pass */
              if (getenv("QJS_AOT_REGION_NOARGINV") == NULL) { snprintf(tmp_, sizeof(tmp_), "%sv%d", I->pfx, dv); if (rg_frame_owned(fn, tmp_)) rg_inv_add(c, tmp_); }
            } else fprintf(o, "        %sv%d = %s;\n", I->pfx, dv, s0 < I->nargs ? I->arg[s0] : "JS_UNDEFINED");
            if (in->rop == ROP_get_arg2 && dv2 >= 0 && in->narg > 3) {
              int s1 = (int)in->arg[3].val;
              if (is_root) fprintf(o, "        %sv%d = %d < f_argc ? f_argv[%d] : JS_UNDEFINED;\n", I->pfx, dv2, s1, s1);
              else fprintf(o, "        %sv%d = %s;\n", I->pfx, dv2, s1 < I->nargs ? I->arg[s1] : "JS_UNDEFINED");
            }
          }
          break;
        }
        case ROP_move: case ROP_move_s: case ROP_move_xfer:
          if (dv < 0 || u0 < 0) return rg_fail(c, "pc %d: move", in->pc);
          if (c->emit) fprintf(o, "        %sv%d = %sv%d;\n", I->pfx, dv, I->pfx, u0);
          if (c->emit) { char s_[24]; snprintf(s_, sizeof(s_), "%sv%d", I->pfx, u0); if (rg_inv_has(c, s_)) { snprintf(tmp_, sizeof(tmp_), "%sv%d", I->pfx, dv); rg_inv_add(c, tmp_); rg_orig_set(c, tmp_, rg_orig(c, s_)); } }
          break;
        case ROP_get_this_field: case ROP_get_this_field2: {
          char nm[40];
          int fs = -1; (void)fs;
          if (dv < 0 || dv2 < 0 || !rg_atom(in, nm, sizeof(nm))) return rg_fail(c, "pc %d: get_this_field", in->pc);
          /* after a store a read may not exit (the exit would redo the store); a read of `this` in an instance a dispatch made has its position from the shape the
             dispatch chose (no lookup, nothing that can fail), and a trusted read is a constant offset */
          {
            /* `this.m()`: the read is a method read and a call later in the block uses it: the dispatch is made at the call */
            int call_idx = rg_find_call(fn, B, k, dv);
            if (call_idx >= 0) {
              snprintf(I->mname[dv], 40, "%s", nm);
              I->mpc[dv] = in->pc;
              I->pend_end = call_idx + 1; I->pend_pc = I->mpc[dv];
              if (!rg_pend_push(I, fn, idx, dv, call_idx, nm, in->pc, dv2, I->self)) return rg_fail(c, "pc %d: too many pending method reads", in->pc);
              if (c->emit) fprintf(o, "        %sv%d = %s;\n", I->pfx, dv2, I->self);
              pair_read = 1;
              if (I->cur != NULL) {
                int tr = fn->values[dv2].reg;
                if (tr >= 0 && tr < 256) snprintf(I->cur[tr], 40, "%sv%d", I->pfx, dv2);
              }
              break;
            }
          }
          if (!c->emit) {
            if (!rg_trust_field(I, in, 0, tmp_, sizeof(tmp_)) && !rg_note_field(c, I, dv2, nm, 0, 0, 1)) return rg_fail(c, "pc %d: .%s requirements", in->pc, nm);
          } else {
            fs = 0;
            {
              int gq = I->owner_g, qq = gq >= 0 ? rg_qidx(&c->gd[gq], I->owner_t, nm) : -1;
              if (qq < 0) { gq = rg_guard(c, I->id, dv2, -1, 0, NULL); qq = gq >= 0 ? rg_qidx(&c->gd[gq], 0, nm) : -1; }
              if (qq < 0 && !rg_trust_field(I, in, 0, tmp_, sizeof(tmp_))) return rg_fail(c, "pc %d: .%s has no verified position", in->pc, nm);
              char to_[64];
              int tr_ = rg_trust_field(I, in, 0, to_, sizeof(to_));
              if (I->owner_g < 0) { fprintf(o, "        %sv%d = %s;\n", I->pfx, dv2, I->self); if (!tr_) rg_lookup(c, gq, I->self, xl, "        "); }
              else fprintf(o, "        %sv%d = %s;\n", I->pfx, dv2, I->self);
              if (tr_) { rg_verify(c, I, in, 0, I->self, nm, to_); rg_inv_read(c, I, o, dv, I->self, to_, nm); }
              else fprintf(o, "        %sv%d = *q2_slotp(%s, rg_off_%s_%d[gix_%d][%d]);\n", I->pfx, dv, I->self, c->hex, gq, gq, qq);
              if (rg_inv_has(c, I->self)) { snprintf(tmp_, sizeof(tmp_), "%sv%d", I->pfx, dv2); rg_inv_add(c, tmp_); rg_orig_set(c, tmp_, rg_orig(c, I->self)); }
            }
          }
          break;
        }
        case ROP_get_field_chain: case ROP_get_field_chain_own: {
          /* d0 = recv.a1 (a field), d1 = d0.a2 (a method read that the call right after uses) */
          char n1[40], n2[40];
          int call_idx = -1, j, glob_ = 0;
          if (dv < 0 || dv2 < 0 || u0 < 0 || !rg_atom(in, n1, sizeof(n1))) return rg_fail(c, "pc %d: get_field_chain", in->pc);
          {
            int a, seen = 0;
            n2[0] = 0;
            for (a = 0; a < in->narg; a++) if (in->arg[a].kind == 'a' && seen++ == 1) qjs_aot_bundle_atom_name((uint32_t)in->arg[a].val, n2, sizeof(n2));
            if (!n2[0]) return rg_fail(c, "pc %d: get_field_chain second name", in->pc);
          }
          if (rg_trust_on()) {
            char pn_[4][40];
            int pnn_ = 0, q_;
            if (rg_cpath(fn, dv, pn_, &pnn_)) {
              q_ = rg_cp_add(c, pn_, pnn_);
              if (q_ < 0) return rg_fail(c, "pc %d: too many constant paths", in->pc);
              if (c->emit) { char cpt_[64]; fprintf(o, "        %sv%d = %s;\n", I->pfx, dv, rg_cp_text(c, q_, cpt_, sizeof(cpt_)));  snprintf(tmp_, sizeof(tmp_), "%sv%d", I->pfx, dv); rg_inv_add(c, tmp_); }
              glob_ = 1;
            }
          }
          if (!glob_) {
            /* `G.F.m(...)`: the first read is a global constant (a function the region reads from the facts' table), as in the plain get_field case */
            const Q2Insn *rd_ = (int32_t)u0 >= 0 && (int32_t)u0 < fn->nvalues && fn->values[u0].insn >= 0 ? &fn->insns[fn->values[u0].insn] : NULL;
            if (rd_ != NULL && rd_->rop == ROP_get_global && rg_trust_on() && !rg_is_thru_recv(fn, rg_base(fn, (Q2ValueId)dv))) {
              char gn_[40];
              int gl_, found_ = -1;
              if (!rg_atom(rd_, gn_, sizeof(gn_))) return rg_fail(c, "pc %d: global name", in->pc);
              for (gl_ = 0; gl_ < c->ngl; gl_++) if (strcmp(c->gl[gl_].g, gn_) == 0 && strcmp(c->gl[gl_].f, n1) == 0) found_ = gl_;
              if (found_ < 0) {
                if (c->emit) return rg_fail(c, "pc %d: a global the first pass did not see", in->pc);
                if (c->ngl >= RG_MAXK) return rg_fail(c, "too many global constants");
                snprintf(c->gl[c->ngl].g, 40, "%s", gn_);
                snprintf(c->gl[c->ngl].f, 40, "%s", n1);
                found_ = c->ngl++;
              }
              if (c->emit) fprintf(o, "        %sv%d = rgc_%d;\n", I->pfx, dv, found_);
              glob_ = 1;
            }
          }
          if (glob_) {
          } else if (!c->emit) {
            if (!rg_trust_field_n(I, in, 0, n1, tmp_, sizeof(tmp_)) && !rg_note_field(c, I, u0, n1, 0, 0, rg_is_this(fn, u0))) return rg_fail(c, "pc %d: .%s requirements", in->pc, n1);
          } else {
            int is_this = rg_is_this(fn, u0);
            int gq = is_this && I->owner_g >= 0 ? I->owner_g : rg_guard(c, I->id, u0, -1, 0, NULL);
            int qq = gq >= 0 ? rg_qidx(&c->gd[gq], is_this && I->owner_g >= 0 ? I->owner_t : 0, n1) : -1;
            char val[40];
            if (qq < 0 && !rg_trust_field_n(I, in, 0, n1, tmp_, sizeof(tmp_))) return rg_fail(c, "pc %d: .%s has no verified position", in->pc, n1);
            snprintf(val, sizeof(val), "%sv%d", I->pfx, u0);
            {
              char to_[64];
              int tr_ = rg_trust_field_n(I, in, 0, n1, to_, sizeof(to_));
              if (!(is_this && I->owner_g >= 0)) fprintf(o, "        {\n"), (tr_ ? (void)0 : rg_lookup(c, gq, val, xl, "        "));
              if (tr_) { rg_verify(c, I, in, 0, val, n1, to_); rg_inv_read(c, I, o, dv, val, to_, n1); }
              else fprintf(o, "        %sv%d = *q2_slotp(%s, rg_off_%s_%d[gix_%d][%d]);\n", I->pfx, dv, val, c->hex, gq, gq, qq);
            }
            if (!(is_this && I->owner_g >= 0)) fprintf(o, "        }\n");
          }
          call_idx = rg_find_call(fn, B, k, dv2);
          /* when a call follows, an exit before it resumes at this read, which needs its operand register as it was (the result may share the register) */
          if (I->cur != NULL && call_idx < 0) {
            int reg = fn->values[dv].reg;
            if (reg >= 0 && reg < 256) snprintf(I->cur[reg], 40, "%sv%u", I->pfx, (unsigned)dv);
          }
          (void)j;
          if (call_idx < 0) {
            /* a chain of two field reads: the second is an ordinary read on the first's result */
            char pc2_[4][40];
            int pcn2_ = 0;
            if (rg_trust_on() && rg_cpath(fn, dv2, pc2_, &pcn2_)) {
              int q2_ = rg_cp_add(c, pc2_, pcn2_);
              if (q2_ < 0) return rg_fail(c, "pc %d: too many constant paths", in->pc);
              if (c->emit) { char cpt_[64]; fprintf(o, "        %sv%d = %s;\n", I->pfx, dv2, rg_cp_text(c, q2_, cpt_, sizeof(cpt_)));  snprintf(tmp_, sizeof(tmp_), "%sv%d", I->pfx, dv2); rg_inv_add(c, tmp_); }
            } else if (!c->emit) {
              if (!rg_trust_field_n(I, in, 0, n2, tmp_, sizeof(tmp_)) && !rg_note_field(c, I, dv, n2, 0, 0, 0)) return rg_fail(c, "pc %d: .%s requirements", in->pc, n2);
            } else {
              int gq = rg_guard(c, I->id, dv, -1, 0, NULL);
              int qq = gq >= 0 ? rg_qidx(&c->gd[gq], 0, n2) : -1;
              char val[40];
              char t2_[64];
              int tr2_ = rg_trust_field_n(I, in, 0, n2, t2_, sizeof(t2_));
              if (qq < 0 && !tr2_) return rg_fail(c, "pc %d: .%s has no verified position", in->pc, n2);
              snprintf(val, sizeof(val), "%sv%d", I->pfx, dv);
              fprintf(o, "        {\n");
              if (tr2_) { rg_verify(c, I, in, 0, val, n2, t2_); rg_inv_read(c, I, o, dv2, val, t2_, n2); fprintf(o, "        }\n"); }
              else { rg_lookup(c, gq, val, xl, "        ");
              fprintf(o, "        %sv%d = *q2_slotp(%s, rg_off_%s_%d[gix_%d][%d]);\n        }\n", I->pfx, dv2, val, c->hex, gq, gq, qq); }
            }
            if (I->cur != NULL) {
              int reg = fn->values[dv2].reg;
              if (reg >= 0 && reg < 256) snprintf(I->cur[reg], 40, "%sv%u", I->pfx, (unsigned)dv2);
            }
            pair_read = 1;
            break;
          }
          snprintf(I->mname[dv2], 40, "%s", n2);
          I->mpc[dv2] = in->pc;
          I->pend_end = call_idx + 1; I->pend_pc = I->mpc[dv2];
          { char rt_[40]; snprintf(rt_, sizeof(rt_), "%sv%d", I->pfx, dv); if (!rg_pend_push(I, fn, idx, dv2, call_idx, n2, in->pc, dv, rt_)) return rg_fail(c, "pc %d: too many pending method reads", in->pc); }
          pair_read = 1;
          break;
        }
        case ROP_get_field: case ROP_get_field_own: case ROP_get_field2: case ROP_get_field2_own: {
          char nm[40];
          int call_idx = -1, j;
          if (dv < 0 || u0 < 0 || !rg_atom(in, nm, sizeof(nm))) return rg_fail(c, "pc %d: get_field", in->pc);
          if (I->wrap && in->rop == ROP_get_field2) { if (c->emit) fprintf(o, "        %sv%d = JS_UNDEFINED;   /* the wrapper's `.apply` read: the forward proves it is the builtin */\n", I->pfx, dv); break; }
          /* a method read followed, in this block, by the call that is its only use: the pair is a dispatch */
          call_idx = rg_find_call(fn, B, k, dv);
          (void)j;
          if (call_idx >= 0) {
            snprintf(I->mname[dv], 40, "%s", nm);
            I->mpc[dv] = in->pc;
            I->pend_end = call_idx + 1; I->pend_pc = I->mpc[dv];
            { char rt_[40]; snprintf(rt_, sizeof(rt_), "%sv%d", I->pfx, u0); if (!rg_pend_push(I, fn, idx, dv, call_idx, nm, in->pc, -1, rt_)) return rg_fail(c, "pc %d: too many pending method reads", in->pc); }
            pair_read = 1;
            break;
          }
          if (rg_trust_on()) {
            /* TRUST: `G.f...` of a global written once: a constant bound at init, with a fact on every link (G.F constants, X of `X.call`, static holders) */
            char pn_[4][40];
            int pnn_ = 0, q_;
            if (rg_cpath(fn, dv, pn_, &pnn_)) {
              q_ = rg_cp_add(c, pn_, pnn_);
              if (q_ < 0) return rg_fail(c, "pc %d: too many constant paths", in->pc);
              if (c->emit) { char cpt_[64]; fprintf(o, "        %sv%d = %s;\n", I->pfx, dv, rg_cp_text(c, q_, cpt_, sizeof(cpt_)));  snprintf(tmp_, sizeof(tmp_), "%sv%d", I->pfx, dv); rg_inv_add(c, tmp_); }
              break;
            }
          }
          /* a global constant: `G.F` where the receiver is a global read */
          {
            const Q2Insn *rd = (int32_t)u0 >= 0 && (int32_t)u0 < fn->nvalues && fn->values[u0].insn >= 0 ? &fn->insns[fn->values[u0].insn] : NULL;
            if (rd != NULL && rd->rop == ROP_get_global && !rg_is_thru_recv(fn, rg_base(fn, (Q2ValueId)dv))) {
              char gn[40];
              int gl, found = -1;
              if (!rg_atom(rd, gn, sizeof(gn))) return rg_fail(c, "pc %d: global name", in->pc);
              for (gl = 0; gl < c->ngl; gl++) if (strcmp(c->gl[gl].g, gn) == 0 && strcmp(c->gl[gl].f, nm) == 0) found = gl;
              if (found < 0) {
                if (c->emit) return rg_fail(c, "pc %d: a global the first pass did not see", in->pc);
                if (c->ngl >= RG_MAXK) return rg_fail(c, "too many global constants");
                snprintf(c->gl[c->ngl].g, 40, "%s", gn);
                snprintf(c->gl[c->ngl].f, 40, "%s", nm);
                found = c->ngl++;
              }
              if (c->emit) fprintf(o, "        %sv%d = rgc_%d;\n", I->pfx, dv, found);
              break;
            }
          }
          if (rg_trust_on() && !rg_trust_field(I, in, 0, tmp_, sizeof(tmp_))) {
            /* TRUST: not one slot for every receiver: a compare with the class shapes picks the class's slot */
            char val_[40], dst_[40];
            snprintf(val_, sizeof(val_), "%sv%d", I->pfx, u0);
            snprintf(dst_, sizeof(dst_), "%sv%d", I->pfx, dv);
            if (rg_poly_field(c, I, in, nm, val_, dst_, xl)) break;
          }
          if (!c->emit) {
            if (!rg_trust_field(I, in, 0, tmp_, sizeof(tmp_)) && !rg_note_field(c, I, u0, nm, 0, 0, rg_is_this(fn, u0))) return rg_fail(c, "pc %d: .%s requirements", in->pc, nm);
          } else {
            int is_this = rg_is_this(fn, u0);
            int gq = is_this && I->owner_g >= 0 ? I->owner_g : rg_guard(c, I->id, u0, -1, 0, NULL);
            int qq = gq >= 0 ? rg_qidx(&c->gd[gq], is_this && I->owner_g >= 0 ? I->owner_t : 0, nm) : -1;
            char val[40];
            if (qq < 0 && !rg_trust_field(I, in, 0, tmp_, sizeof(tmp_))) return rg_fail(c, "pc %d: .%s has no verified position", in->pc, nm);
            snprintf(val, sizeof(val), "%sv%d", I->pfx, u0);
            {
              char to_[64];
              int tr_ = rg_trust_field(I, in, 0, to_, sizeof(to_));
              if (!(is_this && I->owner_g >= 0)) fprintf(o, "        {\n"), (tr_ ? (void)0 : rg_lookup(c, gq, val, xl, "        "));
              if (tr_) { rg_verify(c, I, in, 0, val, nm, to_); rg_inv_read(c, I, o, dv, val, to_, nm); }
              else fprintf(o, "        %sv%d = *q2_slotp(%s, rg_off_%s_%d[gix_%d][%d]);\n", I->pfx, dv, val, c->hex, gq, gq, qq);
            }
            if (!(is_this && I->owner_g >= 0)) fprintf(o, "        }\n");
          }
          break;
        }
        case ROP_put_field: case ROP_put_field_xfer: {
          char nm[40];
          if (u0 < 0 || u1 < 0 || !rg_atom(in, nm, sizeof(nm))) return rg_fail(c, "pc %d: put_field", in->pc);
          if (I->fresh && I->cons > 0 && rg_is_this(fn, u0)) {
            /* a field of the object this region is constructing: nothing else can see it, so the store is no effect (an exit discards the object), and it is made
               by the engine's own define on the fresh object (the prototype chain was stated to hold no such property) */
            RCons *K = &c->cons[I->cons - 1];
            int ni;
            for (ni = 0; ni < K->nn && strcmp(K->names[ni], nm) != 0; ni++) {}
            if (ni == K->nn) {
              if (c->emit) return rg_fail(c, "pc %d: .%s was not seen by the first pass", in->pc, nm);
              if (K->nn >= 16 || strlen(nm) >= sizeof(K->names[0])) return rg_fail(c, "pc %d: too many fields in a constructed object", in->pc);
              snprintf(K->names[K->nn++], sizeof(K->names[0]), "%s", nm);
            }
            if (K->esc) return rg_fail(c, "pc %d: .%s is stored by the constructor after the object was seen", in->pc, nm);
            K->seen |= 1u << ni;
            if (c->emit && K->direct)
              /* the object was made in its final shape: the field's slot is its place in the list, and it held undefined */
              fprintf(o, "        { JSValue nv_ = %sv%d, *sp_ = q2_slotp(%sv%d, %du * QJS_AOT_SIZEOF_JSPROPERTY), old_ = *sp_; q2_dup_p(nv_); *sp_ = nv_; if (JS_VALUE_HAS_REF_COUNT(old_)) JS_FreeValue(ctx, old_); }\n", I->pfx, u1, I->pfx, u0, ni);
            else if (c->emit)
              fprintf(o, "        { JSValue nv_ = %sv%d; q2_dup_p(nv_);\n          if (__builtin_expect(JS_DefinePropertyValue(ctx, %sv%d, rgc_at_%s_%d[%d], nv_, JS_PROP_C_W_E) <= 0, 0)) { JS_FreeValue(ctx, JS_GetException(ctx)); goto %s; } }\n",
                      I->pfx, u1, I->pfx, u0, c->hex, I->cons - 1, ni, xl);
            break;
          }
          if (!rg_cold(I, in)) rg_mut_add(c, nm);
          if (!c->emit) {
            if (rg_borrow_live(c, ii, in)) rg_note_park(c, ii);
            if (!rg_trust_field(I, in, 1, tmp_, sizeof(tmp_)) && !rg_note_field(c, I, u0, nm, 0, 1, rg_is_this(fn, u0))) return rg_fail(c, "pc %d: .%s requirements", in->pc, nm);
          } else {
            int is_this = rg_is_this(fn, u0);
            int gq = is_this && I->owner_g >= 0 ? I->owner_g : rg_guard(c, I->id, u0, -1, 0, NULL);
            int qq = gq >= 0 ? rg_qidx(&c->gd[gq], is_this && I->owner_g >= 0 ? I->owner_t : 0, nm) : -1;
            char val[40], off[96];
            if (qq < 0 && !rg_trust_field(I, in, 1, tmp_, sizeof(tmp_))) return rg_fail(c, "pc %d: .%s has no verified position", in->pc, nm);
            snprintf(val, sizeof(val), "%sv%d", I->pfx, u0);
            fprintf(o, "        {\n");
            if (rg_trust_field(I, in, 1, off, sizeof(off))) rg_verify(c, I, in, 1, val, nm, off);
            else {
              if (!(is_this && I->owner_g >= 0)) rg_lookup(c, gq, val, xl, "        ");
              snprintf(off, sizeof(off), "rg_off_%s_%d[gix_%d][%d]", c->hex, gq, gq, qq);
            }
            if (rg_trust_field(I, in, 1, tmp_, sizeof(tmp_)) && g_rg_plain && (rg_trust_on() || getenv("QJS_AOT_REGION_LEAN") != NULL) && (!rg_store_narrow(I, u1, g_rg_cls) || rg_f64_knob("QJS_AOT_REGION_F64NOCLAIM") || getenv("QJS_AOT_REGION_F64NARROWEXIT") != NULL)) {
              /* a slot that holds only numbers/booleans/undefined of an instance of a proven class: a plain store */
              if (c->emit && getenv("QJS_AOT_REGION_F64DBG")) { char an_[48] = "?"; rg_atom(in, an_, sizeof(an_)); fprintf(o, "        /* store pc %d in %s line %d .%s cls %x ty %d */\n", in->pc, I->g->name, I->g->line, an_, g_rg_cls, rg_tyof(I, u1)); }
              if (rg_store_narrow(I, u1, g_rg_cls)) {
                /* a double into a field the program only ever stores ints in (it may be claimed an int, which this store does not check): an int that fits, else the store leaves */
                if (rg_f64_knob("QJS_AOT_REGION_F64NOCLAIM"))
                  /* the vkit knock-out: when the run sets the twin the double is stored as it is, double-tagged (the layout claims an int there) */
                  fprintf(o, "        if (" RG_KN("NOCLAIM") ") *q2_slotp(%s, %s) = __JS_NewFloat64(%sd%d); else {\n          if (__builtin_expect(!qjs_aot_fitsi(%sd%d), 0)) goto %s;\n          *q2_slotp(%s, %s) = JS_MKVAL(JS_TAG_INT, (int32_t)%sd%d); }\n        }\n",
                          val, off, I->pfx, u1, I->pfx, u1, xl, val, off, I->pfx, u1);
                else
                  fprintf(o, "        if (__builtin_expect(!qjs_aot_fitsi(%sd%d), 0)) goto %s;\n        *q2_slotp(%s, %s) = JS_MKVAL(JS_TAG_INT, (int32_t)%sd%d);\n        }\n", I->pfx, u1, xl, val, off, I->pfx, u1);
              } else {
                char vs_[96];
                rg_store_val(I, u1, vs_, sizeof(vs_));
                fprintf(o, "        *q2_slotp(%s, %s) = %s;\n        }\n", val, off, vs_);
              }
            } else {
              char vs_[96];
              rg_store_val(I, u1, vs_, sizeof(vs_));
              /* a double into a field the program may claim an int: the normalised number (an int when it fits), and the claim check below sends a real double to the engine */
              if (rg_store_narrow(I, u1, g_rg_cls)) snprintf(vs_, sizeof(vs_), "%sv%d", I->pfx, u1);
              if (rg_tyof(I, u1) != RT_ANY) { fprintf(o, "        JSValue sv_ = %s;\n", vs_); snprintf(vs_, sizeof(vs_), "sv_"); }   /* the boxed form of a typed value, once */
            /* the checks first, then the store: the engine's own store takes a prototype object (the watchpoints) and a bad layout claim */
            {
              char xp[32], xc[32];
              int xi = rg_xit(c, ii, xpc);
              rg_xlabel(c, xi, in->pc, "store-proto", xp, sizeof(xp));
              rg_xlabel(c, xi, in->pc, "store-claim", xc, sizeof(xc));
              fprintf(o, "        if (__builtin_expect(q2_is_proto(%s), 0)) goto %s;\n", val, xp);
              /* a claim the stored value would contradict sends the store to the engine, whose hook breaks the layout; once the layout IS broken nothing trusts the
                 claim any more and the store is made here */
              if (getenv("QJS_AOT_FASTSTATS") != NULL)
                fprintf(o, "        if (Q2_CLAIM_BAD(%s, %s, %sv%d)) { static int once_; if (!once_++) { const unsigned char *sh_ = *(const unsigned char *const *)((const unsigned char *)JS_VALUE_GET_PTR(%s) + QJS_AOT_OFF_JSOBJECT_SHAPE); unsigned id_ = *(const uint16_t *)(sh_ + QJS_AOT_OFF_SHAPE_LAYOUT_ID); fprintf(stderr, \"region claim: layout %%u slot %%u claim %%u tag %%d broken %%llx\\n\", id_, (unsigned)(%s / QJS_AOT_SIZEOF_JSPROPERTY), id_ && id_ <= (unsigned)Q2_LAYOUT_COUNT ? (unsigned)q2_layout_claim[id_ - 1u][(%s / QJS_AOT_SIZEOF_JSPROPERTY) & 31u] : 0u, (int)JS_VALUE_GET_TAG(%sv%d), (unsigned long long)Q2_LAYOUT_BROKEN(ctx)); } }\n", val, off, I->pfx, u1, val, off, off, I->pfx, u1);
              fprintf(o, "        if (__builtin_expect(Q2_CLAIM_BAD(%s, %s, %s) && !(Q2_LAYOUT_BROKEN(ctx) & (1ull << (*(const uint16_t *)(*(const unsigned char *const *)((const unsigned char *)JS_VALUE_GET_PTR(%s) + QJS_AOT_OFF_JSOBJECT_SHAPE) + QJS_AOT_OFF_SHAPE_LAYOUT_ID) - 1u))), 0)) goto %s;\n", val, off, vs_, val, xc);
            }
            fprintf(o, "        { JSValue *sp_ = q2_slotp(%s, %s); JSValue old_ = *sp_; q2_dup_p(%s); *sp_ = %s;\n", val, off, vs_, vs_);
            rg_store_release(c, ii, in);
            fprintf(o, "        } }\n");
            }
          }
          break;
        }
        case ROP_put_element: case ROP_put_element_xfer: {
          if (!c->emit && !rg_cold(I, in)) { c->arr_mut = 1; rg_amut_note(c, I, u0); }
          /* `a[i] = v` on a fast array with an int index: an in-bounds store into an existing element, or an append at index == count (the engine's growth helper when
             there is no room); everything else (hole, not a plain fast array, index out of range, a non-int index) leaves before any effect. The value stored is duplicated;
             the replaced value is parked in the site's slot (rn_K) until the site runs again or the region ends, so a borrow of it stays good for the rest of the iteration. */
          int u2 = in->nuse >= 3 ? (int)in->use[2] : -1, ki;
          if (u0 < 0 || u1 < 0 || u2 < 0) return rg_fail(c, "pc %d: put_element", in->pc);
          ki = rg_cons(c, I->id, in->pc);
          if (ki < 0) return rg_fail(c, "pc %d: too many element stores in one region", in->pc);
          c->cons[ki].kind = 2;
          if (!c->emit && rg_borrow_live(c, ii, in)) rg_note_park(c, ii);
          if (c->emit) {
            char vs_[96];
            rg_store_val(I, u2, vs_, sizeof(vs_));   /* a typed element is boxed as the interpreter would store it */
            fprintf(o, "        { JSValue old_ = JS_UNDEFINED; if (__builtin_expect(!rg_put_elem(ctx, %sv%d, %sv%d, %s, &old_), 0)) goto %s;\n",
                    I->pfx, u0, I->pfx, u1, vs_, xl);
            if (rg_borrow_live(c, ii, in)) rg_store_release(c, ii, in);
            else fprintf(o, "          if (JS_VALUE_HAS_REF_COUNT(old_)) { JS_FreeValue(ctx, rn_%d); rn_%d = old_; }\n", ki, ki);
            fprintf(o, "        }\n");
          }
          break;
        }
        case ROP_get_length:
          if (dv < 0 || u0 < 0) return rg_fail(c, "pc %d: get_length", in->pc);
          /* a dense fast array: its element count is its length, read straight from the object */
          {
            char b_[24];
            snprintf(b_, sizeof(b_), "%sv%d", I->pfx, u0);
            if (rg_hoist_on(c) && (I->curloop || I->ploop) && (rg_hoist_nomut() || !c->arr_mut) && rg_inv_has(c, b_) && rg_orig(c, b_) != NULL && c->nhk < 250) {
              const char *ob = rg_orig(c, b_);
              char key[120];
              int k_;
              snprintf(key, sizeof(key), "L|%s", ob);
              if (rg_hk_unsafe(c, key, &k_, c->arr_mut) == 0)
                rs_add(&c->hoist, "    if (__builtin_expect(JS_VALUE_GET_TAG(%s) != JS_TAG_OBJECT || *(const uint16_t *)((const unsigned char *)JS_VALUE_GET_PTR(%s) + QJS_AOT_OFF_JSOBJECT_CLASS_ID) != QJS_AOT_CLASS_ARRAY, 0)) return aot_g_%s(ctx, f, fr, entry_pc);\n"
                                   "    rgm_%d = *q2_slotp(%s, 0); if (__builtin_expect(JS_VALUE_GET_TAG(rgm_%d) != JS_TAG_INT, 0)) return aot_g_%s(ctx, f, fr, entry_pc);\n    rgmv_%d = 1;\n", ob, ob, c->hex, k_, ob, k_, c->hex, k_);
              fprintf(o, "        %sv%d = rgm_%d;\n", I->pfx, dv, k_);
              snprintf(b_, sizeof(b_), "%sv%d", I->pfx, dv);
              rg_inv_add(c, b_);
              snprintf(key, sizeof(key), "rgm_%d", k_);
              rg_orig_set(c, b_, key);
              break;
            }
            if (rg_inv_on(c) && (I->curloop || I->ploop) && !c->arr_mut && rg_inv_has(c, b_)) {
              /* the length of an array the region never changes, of the same array on every pass: read once */
              int k_ = c->nmemo++;
              fprintf(o, "        if (__builtin_expect(!rgmv_%d, 0)) { const unsigned char *ap_ = (const unsigned char *)JS_VALUE_GET_PTR(%s);\n"
                         "          if (__builtin_expect(JS_VALUE_GET_TAG(%s) != JS_TAG_OBJECT || *(const uint16_t *)(ap_ + QJS_AOT_OFF_JSOBJECT_CLASS_ID) != QJS_AOT_CLASS_ARRAY, 0)) goto %s;\n"
                         "          rgm_%d = *q2_slotp(%s, 0); if (__builtin_expect(JS_VALUE_GET_TAG(rgm_%d) != JS_TAG_INT, 0)) goto %s; rgmv_%d = 1; }\n        %sv%d = rgm_%d;\n",
                      k_, b_, b_, xl, k_, b_, k_, xl, k_, I->pfx, dv, k_);
              snprintf(b_, sizeof(b_), "%sv%d", I->pfx, dv);
              rg_inv_add(c, b_);
              break;
            }
          }
          if (c->emit && rg_trust_on() && getenv("QJS_AOT_REGION_LENCOUNT") == NULL) {
            /* TRUST: an array's `length` is its first property, whatever its storage (a fast array's element count is not its length when it ends
               in holes): an int there, anything else leaves */
            fprintf(o, "        { if (__builtin_expect(JS_VALUE_GET_TAG(%sv%d) != JS_TAG_OBJECT || *(const uint16_t *)((const unsigned char *)JS_VALUE_GET_PTR(%sv%d) + QJS_AOT_OFF_JSOBJECT_CLASS_ID) != QJS_AOT_CLASS_ARRAY, 0)) goto %s;\n"
                       "          %sv%d = *q2_slotp(%sv%d, 0); if (__builtin_expect(JS_VALUE_GET_TAG(%sv%d) != JS_TAG_INT, 0)) goto %s; }\n",
                    I->pfx, u0, I->pfx, u0, xl, I->pfx, dv, I->pfx, u0, I->pfx, dv, xl);
            break;
          }
          if (c->emit) fprintf(o, "        { const unsigned char *ap_ = (const unsigned char *)JS_VALUE_GET_PTR(%sv%d);\n"
                                  "          if (__builtin_expect(JS_VALUE_GET_TAG(%sv%d) == JS_TAG_OBJECT && *(const uint16_t *)(ap_ + QJS_AOT_OFF_JSOBJECT_CLASS_ID) == QJS_AOT_CLASS_ARRAY && (ap_[QJS_AOT_OFF_JSOBJECT_FAST_ARRAY_BYTE] & QJS_AOT_JSOBJECT_FAST_ARRAY_MASK) && !QJS_AOT_ARR_ODD(ap_), 1))\n"
                                  "            %sv%d = JS_MKVAL(JS_TAG_INT, (int32_t)*(const uint32_t *)(ap_ + QJS_AOT_OFF_JSOBJECT_ARRAY_COUNT));\n"
                                  "          else %s }\n",
                               I->pfx, u0, I->pfx, u0, I->pfx, dv,
                               /* TRUST: anything but a plain array leaves (a helper call that rejoined would stop the C compiler from reusing loads across it) */
                               rg_trust_on() ? (snprintf(tmp_, sizeof(tmp_), "goto %s;", xl), tmp_) : (snprintf(tmp_, sizeof(tmp_), "{ double len_; if (__builtin_expect(!q2_array_length(ctx, %sv%d, &len_, NULL, NULL), 0)) goto %s; %sv%d = JS_MKVAL(JS_TAG_INT, (int32_t)len_); }", I->pfx, u0, xl, I->pfx, dv), tmp_));
          break;
        case ROP_get_element:
          if (dv < 0 || u0 < 0 || u1 < 0) return rg_fail(c, "pc %d: get_element", in->pc);
          {
            char dn_[32];
            snprintf(dn_, sizeof(dn_), "%sv%d", I->pfx, dv);
            if (rg_elem_cp(c, I, u0, u1, dv, dn_, xl)) break;   /* a constant-path array in a loop: checked once at the entry (region_fn_var.inc) */
          }
          {
            char b_[24];
            snprintf(b_, sizeof(b_), "%sv%d", I->pfx, u0);
            if (rg_hoist_on(c) && (I->curloop || I->ploop) && (rg_hoist_nomut() || !c->arr_mut) && rg_inv_has(c, b_) && rg_orig(c, b_) != NULL && c->nhk < 250) {
              const char *ob = rg_orig(c, b_);
              char key[120];
              int k_;
              snprintf(key, sizeof(key), "A|%s", ob);
              if (rg_hk_unsafe(c, key, &k_, c->arr_mut) == 0)
                rs_add(&c->hoist, "    { const unsigned char *ap_ = (const unsigned char *)JS_VALUE_GET_PTR(%s);\n"
                                  "      if (__builtin_expect(JS_VALUE_GET_TAG(%s) != JS_TAG_OBJECT || *(const uint16_t *)(ap_ + QJS_AOT_OFF_JSOBJECT_CLASS_ID) != QJS_AOT_CLASS_ARRAY || !(ap_[QJS_AOT_OFF_JSOBJECT_FAST_ARRAY_BYTE] & QJS_AOT_JSOBJECT_FAST_ARRAY_MASK) || QJS_AOT_ARR_ODD(ap_), 0)) return aot_g_%s(ctx, f, fr, entry_pc);\n"
                                  "      rgma_%d = *(JSValue *const *)(ap_ + QJS_AOT_OFF_JSOBJECT_ARRAY_VALUES); rgmc_%d = *(const uint32_t *)(ap_ + QJS_AOT_OFF_JSOBJECT_ARRAY_COUNT); rgmv_%d = 1; }\n", ob, ob, c->hex, k_, k_, k_);
              fprintf(o, "        { JSValue e_; if (__builtin_expect(JS_VALUE_GET_TAG(%sv%d) != JS_TAG_INT || (uint32_t)JS_VALUE_GET_INT(%sv%d) >= rgmc_%d, 0)) goto %s;\n"
                         "          e_ = rgma_%d[(uint32_t)JS_VALUE_GET_INT(%sv%d)];\n"
                         "          if (__builtin_expect(JS_VALUE_GET_TAG(e_) == JS_TAG_UNINITIALIZED || JS_VALUE_GET_TAG(e_) == QJS_AOT_TAG_LAZY_JSON, 0)) goto %s;\n"
                         "          %sv%d = e_; }\n", I->pfx, u1, I->pfx, u1, k_, xl, k_, I->pfx, u1, xl, I->pfx, dv);
              break;
            }
            if (rg_inv_on(c) && (I->curloop || I->ploop) && !c->arr_mut && rg_inv_has(c, b_)) {
              /* an element of an array the region never changes, the same array on every pass: its storage and count read once */
              int k_ = c->nmemo++;
              fprintf(o, "        if (__builtin_expect(!rgmv_%d, 0)) { const unsigned char *ap_ = (const unsigned char *)JS_VALUE_GET_PTR(%s);\n"
                         "          if (__builtin_expect(JS_VALUE_GET_TAG(%s) != JS_TAG_OBJECT || *(const uint16_t *)(ap_ + QJS_AOT_OFF_JSOBJECT_CLASS_ID) != QJS_AOT_CLASS_ARRAY || !(ap_[QJS_AOT_OFF_JSOBJECT_FAST_ARRAY_BYTE] & QJS_AOT_JSOBJECT_FAST_ARRAY_MASK) || QJS_AOT_ARR_ODD(ap_), 0)) goto %s;\n"
                         "          rgma_%d = *(JSValue *const *)(ap_ + QJS_AOT_OFF_JSOBJECT_ARRAY_VALUES); rgmc_%d = *(const uint32_t *)(ap_ + QJS_AOT_OFF_JSOBJECT_ARRAY_COUNT); rgmv_%d = 1; }\n"
                         "        { JSValue e_; if (__builtin_expect(JS_VALUE_GET_TAG(%sv%d) != JS_TAG_INT || (uint32_t)JS_VALUE_GET_INT(%sv%d) >= rgmc_%d, 0)) goto %s;\n"
                         "          e_ = rgma_%d[(uint32_t)JS_VALUE_GET_INT(%sv%d)];\n"
                         "          if (__builtin_expect(JS_VALUE_GET_TAG(e_) == JS_TAG_UNINITIALIZED || JS_VALUE_GET_TAG(e_) == QJS_AOT_TAG_LAZY_JSON, 0)) goto %s;\n"
                         "          %sv%d = e_; }\n",
                      k_, b_, b_, xl, k_, k_, k_, I->pfx, u1, I->pfx, u1, k_, xl, k_, I->pfx, u1, xl, I->pfx, dv);
              break;
            }
          }
          /* a dense fast array: one bounds check against the object's count, then the element
             borrowed in place; a hole, lazy JSON, a packed or typed array takes the helper */
          if (c->emit) fprintf(o, "        { JSValue e_; const unsigned char *ap_ = (const unsigned char *)JS_VALUE_GET_PTR(%sv%d);\n"
                                  "          if (__builtin_expect(JS_VALUE_GET_TAG(%sv%d) == JS_TAG_OBJECT && JS_VALUE_GET_TAG(%sv%d) == JS_TAG_INT && *(const uint16_t *)(ap_ + QJS_AOT_OFF_JSOBJECT_CLASS_ID) == QJS_AOT_CLASS_ARRAY && (ap_[QJS_AOT_OFF_JSOBJECT_FAST_ARRAY_BYTE] & QJS_AOT_JSOBJECT_FAST_ARRAY_MASK) && !QJS_AOT_ARR_ODD(ap_) && (uint32_t)JS_VALUE_GET_INT(%sv%d) < *(const uint32_t *)(ap_ + QJS_AOT_OFF_JSOBJECT_ARRAY_COUNT), 1)) {\n"
                                  "            e_ = (*(JSValue *const *)(ap_ + QJS_AOT_OFF_JSOBJECT_ARRAY_VALUES))[(uint32_t)JS_VALUE_GET_INT(%sv%d)];\n"
                                  "            if (__builtin_expect(JS_VALUE_GET_TAG(e_) == JS_TAG_UNINITIALIZED || JS_VALUE_GET_TAG(e_) == QJS_AOT_TAG_LAZY_JSON, 0)) goto %s;\n"
                                  "          } else if (__builtin_expect(!q2_elem_hit_lz(%sv%d, %sv%d, &e_, 1), 0)) goto %s;\n"
                                  "          %sv%d = e_; }\n",
                               I->pfx, u0, I->pfx, u0, I->pfx, u1, I->pfx, u1, I->pfx, u1, xl, I->pfx, u0, I->pfx, u1, xl, I->pfx, dv);
          break;
#include "region_ops_arith.inc"
#include "region_ops_call.inc"
        case ROP_get_global: {
          /* the receiver of a `.F` read, which the get_field case folds; or the callee of a construct, whose function the region learned at entry */
          int j2, need_val = 0, usepc = -1;
          char usern[48] = "";
          /* the region never loads a global's value: it is read as the receiver of a `.F` field (folded), or it is the constructor of a construct. Any other use needs the value, which is
             only possible where the profile never went (an exit); elsewhere the region is refused */
          for (j2 = 0; dv >= 0 && j2 < fn->ninsns; j2++) {
            const Q2Insn *cj = &fn->insns[j2];
            int uk;
            if (cj->rop == ROP_move || cj->rop == ROP_move_s || cj->rop == ROP_move_xfer) continue;
            for (uk = 0; uk < cj->nuse; uk++) {
              if (rg_base(fn, cj->use[uk]) != dv) continue;
              switch (cj->rop) {
                case ROP_construct: case ROP_get_field: case ROP_get_field_own: case ROP_get_field2: case ROP_get_field2_own:
                case ROP_get_field_chain: case ROP_get_field_chain_own: break;
                case ROP_call_method: if (!rg_trust_on() && uk == 1 && rg_base(fn, cj->use[0]) != dv) break; /* the receiver of a method call: the callee must not read `this` (checked where it is expanded); TRUST loads it */
                  need_val = 1; e_rop_name((uint16_t)cj->rop, usern, sizeof(usern)); usepc = cj->pc; break;
                default: need_val = 1; e_rop_name((uint16_t)cj->rop, usern, sizeof(usern)); usepc = cj->pc;
              }
            }
            for (uk = 0; uk < cj->win_n && cj->win_val != NULL; uk++) if (rg_base(fn, cj->win_val[uk]) == dv) { need_val = 1; snprintf(usern, sizeof(usern), "an argument of pc %d", cj->pc); usepc = cj->pc; }
          }
          /* the value passed on a block edge (a loop-carried or joined variable: `var p = head`) is a use too: the parameter it becomes is read by whatever follows */
          for (j2 = 0; dv >= 0 && !need_val && getenv("QJS_AOT_REGION_NOEDGEUSE") == NULL && j2 < fn->nblocks; j2++) {   /* (the env var brings the old scan back, for the test of this case) */
            const Q2Block *B2 = &fn->blocks[j2];
            int s2, k2;
            if (fn->rpo_pos != NULL && fn->rpo_pos[j2] < 0) continue;
            for (s2 = 0; s2 < B2->nsucc; s2++)
              for (k2 = 0; k2 < B2->nedge_args[s2]; k2++)
                if (rg_base(fn, B2->edge_arg[s2][k2]) == dv) { need_val = 1; snprintf(usern, sizeof(usern), "a value passed on an edge of block %d", j2); usepc = in->pc; }
          }
          if (need_val && rg_trust_on()) {
            /* TRUST: the value is read from the binding every time (a global such as `planner` changes between runs; nothing is assumed about it beyond what the
               dispatch on it checks): the binding's own data value, borrowed, or an exit when it is not one */
            char gn_[40];
            int q_;
            if (!rg_atom(in, gn_, sizeof(gn_))) return rg_fail(c, "pc %d: global name", in->pc);
            {
              /* a global the bundle writes at most once (a declaration, or the engine's binding): its value is the constant the init bound, which a
                 fact on the binding keeps true while the word holds */
              char pg_[4][40];
              int pn_ = 0, qc_;
              if (getenv("QJS_AOT_REGION_NOGCONST") == NULL && rg_cpath(fn, dv, pg_, &pn_) && pn_ == 1 && (qc_ = rg_cp_add(c, pg_, 1)) >= 0) {
                char cpt_[64];
                if (c->emit) { fprintf(o, "        %sv%d = %s;\n", I->pfx, dv, rg_cp_text(c, qc_, cpt_, sizeof(cpt_))); snprintf(tmp_, sizeof(tmp_), "%sv%d", I->pfx, dv); rg_inv_add(c, tmp_); }   /* a constant: the same on every pass */
                need_val = 0;
                break;
              }
            }
            q_ = rg_gv_add(c, gn_);
            if (q_ < 0) return rg_fail(c, "pc %d: too many globals read as values", in->pc);
            if (c->emit) fprintf(o, "        if (__builtin_expect(!qjs_aot_global_get(ctx, rgga_%s[%d], &%sv%d), 0)) goto %s;\n", c->hex, q_, I->pfx, dv, xl);
            need_val = 0;
          }
          if (need_val) {
            if (rg_cold(I, in)) {
              if (c->emit) fprintf(o, "        goto %s; /* get_global: never executed in the profile */\n", xl);
              break;
            }
            return rg_fail(c, "pc %d: a global's value is used as a value by %s at pc %d (the region reads globals only as `G.F` or as a constructor; the profile executed this load)", in->pc, usern, usepc);
          }
          for (j2 = 0; dv >= 0 && j2 < fn->ninsns; j2++) {
            const Q2Insn *cj = &fn->insns[j2];
            if (cj->rop == ROP_construct && cj->nuse >= 1 && rg_base(fn, cj->use[0]) == dv && !rg_cold(I, cj)) {
              int kj = rg_cons(c, I->id, cj->pc);
              if (kj < 0) return rg_fail(c, "pc %d: too many constructs", cj->pc);
              if (c->emit && c->cons[kj].kind != 2) fprintf(o, "        %sv%d = rgc_f_%s_%d;\n", I->pfx, dv, c->hex, kj);   /* kind 2: a native construct (region_fn_native.inc) reads the global itself */
              break;
            }
          }
          /* the receiver of a global constant `G.F`: the object G names, from the facts' table (an exit that resumes at the read, or after it, needs the register) */
          if (dv >= 0 && rg_trust_on()) {
            char gn_[40];
            int gl_, uses_ = 0, ui_;
            /* the global is the holder of a function a call reads as `G.F.call(...)` (or of constants G.F): its object comes from the facts' table, a field is then an ordinary read */
            for (ui_ = 0; ui_ < fn->ninsns && !uses_; ui_++) {
              const Q2Insn *cu = &fn->insns[ui_];
              if ((cu->rop == ROP_get_field || cu->rop == ROP_get_field_own || cu->rop == ROP_get_field_chain || cu->rop == ROP_get_field_chain_own) && cu->nuse >= 1 && cu->ndef >= 1 &&
                  rg_base(fn, cu->use[0]) == dv && rg_is_thru_recv(fn, rg_base(fn, cu->def[0]))) uses_ = 1;
            }
            if (uses_ && rg_atom(in, gn_, sizeof(gn_)) && qjs_aot_bundle_global_writes(gn_) == 1) {
              /* TRUST: a global written once is a constant path of one link */
              char pg_[4][40];
              int qg_;
              snprintf(pg_[0], 40, "%s", gn_);
              qg_ = rg_cp_add(c, pg_, 1);
              if (qg_ < 0) return rg_fail(c, "pc %d: too many constant paths", in->pc);
              if (c->emit) { char cpt_[64]; fprintf(o, "        %sv%d = %s;\n", I->pfx, dv, rg_cp_text(c, qg_, cpt_, sizeof(cpt_)));  snprintf(tmp_, sizeof(tmp_), "%sv%d", I->pfx, dv); rg_inv_add(c, tmp_); }
            } else if (uses_ && rg_atom(in, gn_, sizeof(gn_))) {
              for (gl_ = 0; gl_ < c->ngl; gl_++) if (strcmp(c->gl[gl_].g, gn_) == 0 && c->gl[gl_].f[0] == 0) break;
              if (gl_ == c->ngl) {
                if (c->emit) return rg_fail(c, "pc %d: a global the first pass did not see", in->pc);
                if (c->ngl >= RG_MAXK) return rg_fail(c, "too many global constants");
                snprintf(c->gl[c->ngl].g, 40, "%s", gn_);
                c->gl[c->ngl].f[0] = 0;
                c->ngl++;
              }
              if (c->emit) fprintf(o, "        %sv%d = rgk_ok_%s ? rgkh_%s[%d] : JS_UNDEFINED;\n", I->pfx, dv, c->hex, c->hex, gl_);
            }
          }
          break;
        }
        case ROP_construct: {
          /* `new F(args)` for a global F: the object is made by the engine's allocation (no code runs), owned by the region, and F's body is expanded in place on it */
          const Q2Insn *cd = NULL;
          const JSAotGenFunc *tg1[2];
          char gn[48], rel_[96];
          int ki, nt1, bv, held_;
          RCons *K;
          RInst *J;
          if (rg_cold(I, in)) {
            if (c->emit) fprintf(o, "        goto %s; /* construct: never executed in the profile */\n", xl);
            break;
          }
          if (dv < 0 || in->nuse < 2) return rg_fail(c, "pc %d: a construct of a shape the region does not know", in->pc);
          if (!rg_trust_on()) return rg_fail(c, "pc %d: construct needs the facts word (QJS_AOT_REGION_TRUST=0 turned TRUST off)", in->pc);
          /* in a loop the site's object is replaced (the previous one released) when it runs again: safe only if nothing borrowed from it can still be live then */
          /* in a loop the site's slot is overwritten when it runs again; an object that can still be live then (carried over a back edge) is parked in the
             region's owned list instead of released, so it stays alive to the region's end */
          held_ = (I->ploop || I->curloop) && (getenv("QJS_AOT_REGION_PARKALL") != NULL || rg_carried_up(c, ii, dv));
          if (held_) rg_note_park(c, ii);
          snprintf(rel_, sizeof(rel_), held_ ? "rg_own_put(ctx, &rgo_, &rgon_, &rgoc_, rn_%d)" : "JS_FreeValue(ctx, rn_%d)", rg_cons(c, I->id, in->pc));
          bv = rg_base(fn, in->use[0]);
          if (bv != rg_base(fn, in->use[1])) return rg_fail(c, "pc %d: new.target is not the constructor", in->pc);
          if (bv >= 0 && bv < fn->nvalues && fn->values[bv].insn >= 0) cd = &fn->insns[fn->values[bv].insn];
          ki = rg_cons(c, I->id, in->pc);
          if (ki < 0) return rg_fail(c, "pc %d: too many constructs", in->pc);
          K = &c->cons[ki];
          K->npath = 0;
          if (cd != NULL && cd->rop == ROP_get_global && rg_atom(cd, gn, sizeof(gn))) {
            K->npath = 1;
            snprintf(K->path[0], 40, "%s", gn);
          } else if (cd != NULL && getenv("QJS_AOT_REGION_NOCTORPATH") == NULL && rg_trust_on() && rg_cpath(fn, bv, K->path, &K->npath) && K->npath >= 2) {
            /* TRUST: the constructor is a constant path (`new NS.Vec(..)`): the class is the function OBJECT the path holds when the region learns, every link watched */
            snprintf(gn, sizeof(gn), "%s", K->path[K->npath - 1]);
          } else return rg_fail(c, "pc %d: the constructor is not a global or a constant path", in->pc);
          snprintf(K->gname, sizeof(K->gname), "%s", gn);
          if (strcmp(gn, "Array") == 0 && K->npath == 1) {
            if (in->win_n != 0) return rg_fail(c, "pc %d: new Array(args)", in->pc);
            K->kind = 1;
            /* a collection's backing array: its first elements in the object itself (most hold a few) */
            if (c->emit && K->stack) {
              /* this Array lives in the region's frame, inside its object's pair (never reallocated: a push past its room is the region's exit) */
              fprintf(o, "        { JSValue t_ = qjs_aot_stack_array_init(ctx, skb_%d, sizeof skb_%d, %d);\n          if (__builtin_expect(JS_VALUE_GET_TAG(t_) == JS_TAG_UNINITIALIZED, 0)) goto %s;\n          sk_%d = 1; rn_%d = t_; %sv%d = t_; }\n",
                      ki, ki, getenv("QJS_AOT_REGION_SKCAP") ? atoi(getenv("QJS_AOT_REGION_SKCAP")) : 16, xl, ki, ki, I->pfx, dv);
              break;
            }
            if (c->emit) fprintf(o, "        { JSValue t_ = %s; if (__builtin_expect(JS_IsException(t_), 0)) { JS_FreeValue(ctx, JS_GetException(ctx)); goto %s; } %s; rn_%d = t_; %sv%d = t_; }\n",
                                 (snprintf(tmp_, sizeof(tmp_), "qjs_aot_new_array_inline(ctx, %d)", getenv("QJS_AOT_REGION_ARRINL") ? atoi(getenv("QJS_AOT_REGION_ARRINL")) : 4), getenv("QJS_AOT_REGION_NOARRINL") ? "JS_NewArray(ctx)" : tmp_), xl, rel_, ki, I->pfx, dv);
            break;
          }
          nt1 = qjs_aot_bundle_targets(I->g, in->pc, tg1, 2);
          if (getenv("QJS_AOT_REGION_WHY")) fprintf(stderr, "why: construct pc %d path %d targets %d\n", in->pc, K->npath, nt1);
          if (nt1 != 1) { if (!rg_native_construct(c, ii, in, gn, dv, xl)) return 0; break; }   /* M5: a generic construct out of line (region_fn_native.inc) */
          K->kind = 0;
          K->tg = tg1[0];
          {
            int ci, kk;
            ci = c->nin++;
            J = rg_newinst(c, ci);
            if (!c->emit || J->g == NULL) {
              memset(J, 0, sizeof(*J));
              if (q2_build(tg1[0], &J->q) < 0 || !J->q.ok) { q2_free(&J->q); J->g = NULL; return rg_fail(c, "pc %d: the constructor's IR does not build", in->pc); }
              J->g = tg1[0];
            }
            if (in->win_n > 8) return rg_fail(c, "pc %d: a construct with more than 8 arguments", in->pc);
            { RWrap w_; J->wrap = 0;
              if (J->g->light_var_count > 0 && rg_trust_on() && rg_wrapper(&J->q, J->g, &w_)) { J->wrap = 1; J->wr_uninit_pc = w_.uninit_pc; J->wr_put_pc = w_.put_pc; J->wr_read_pc = w_.read_pc; snprintf(J->wr_method, sizeof(J->wr_method), "%s", w_.method); }
              else if (J->g->light_var_count > 0 || J->g->ctl_count != 0 || J->g->func_kind != 0) return rg_fail(c, "pc %d: the constructor has a variable area or control records", in->pc); }
            if ((int32_t)dv < 0 || (int32_t)dv >= fn->nvalues || fn->values[dv].reg < 0) return rg_fail(c, "pc %d: the construct's result has no register", in->pc);
            if (!rg_ctor_noreturn(&J->q)) return rg_fail(c, "pc %d: the constructor of %s returns a value", in->pc, gn);
            K->seen = 0;
            K->esc = 0;
            K->nrbs = 0;
            J->id = ci;
            J->cls1 = 0;
            J->depth = I->depth + 1;
            snprintf(J->pfx, sizeof(J->pfx), "r%d_", ci);
            snprintf(J->self, sizeof(J->self), "rn_%d", ki);
            J->nargs = in->win_n > 8 ? 8 : in->win_n;
            for (kk = 0; kk < J->nargs; kk++) snprintf(J->arg[kk], sizeof(J->arg[0]), "%sv%u", I->pfx, (unsigned)in->win_val[kk]);
            J->owner_g = -1;
            J->owner_t = 0;
            J->par = ii;
            J->par_call_pc = in->pc;
            J->par_read_pc = xpc;
            J->par_pc_after = in->next;
            J->par_dst = fn->values[dv].reg;
            J->par_dv = dv;
            J->par_kind = 1;
            J->par_cons = ki;
            snprintf(J->co, sizeof(J->co), "(JSObject *)JS_VALUE_GET_PTR(rgc_f_%s_%d)", c->hex, ki);
            J->fresh = 1;
            J->cons = ki + 1;
            J->ploop = 0;
            if (J->depth > 14) return rg_fail(c, "inlining deeper than 14 (the exit chain holds 16 frames)");
            if (!c->emit) K->site = rg_trust_on() ? qjs_aot_bundle_alloc_site(I->g, in->pc) : -1;
            if (J->wrap) {
              /* the forwarded call dispatches on the class of the object being built: the construct's own allocation site */
              if (K->site <= 0) return rg_fail(c, "pc %d: the forwarding constructor's object has no single allocation site", in->pc);
              J->cls1 = K->site + 1;
            }
            if (c->emit && held_) fprintf(o, "        if (__builtin_expect(rgon_ >= RG_OWNCAP, 0)) goto %s;   /* the owned list is full: leave before the site, the generic body goes on */\n", xl);
            if (c->emit && K->stack) {
              /* the object of a stack pair: the previous incarnation (dead: nothing carries it) is released, the block is made again in the class's final shape */
              RStr rt_ = {0};
              rg_sk_release_text(c, ki, &rt_);
              if (K->knock) fprintf(o, "        if (!getenv(\"QJS_AOT_REGION_SRKNOCK\")) goto %s;   /* the knock-out build: only the knocked run makes it a stack object */\n", xl);
              fprintf(o, "        { JSValue t_; %s\n          t_ = qjs_aot_stack_object_init(skb_%d, sizeof skb_%d, rgc_s_%s_%d);\n          if (__builtin_expect(JS_VALUE_GET_TAG(t_) == JS_TAG_UNINITIALIZED, 0)) goto %s;\n          sk_%d = 1; rn_%d = t_; }\n",
                      rt_.p ? rt_.p : "", ki, ki, c->hex, ki, xl, ki, ki);
              free(rt_.p);
            } else
            if (c->emit && K->direct)
              /* TRUST, one class shape: the object is made in its final shape (every field undefined) and the constructor's stores fill the slots */
              fprintf(o, "        { JSValue t_ = qjs_aot_new_from_shape(ctx, rgc_s_%s_%d);\n"
                         "          if (__builtin_expect(JS_IsException(t_), 0)) { JS_FreeValue(ctx, JS_GetException(ctx)); goto %s; }\n          %s; rn_%d = t_; }\n", c->hex, ki, xl, rel_, ki);
            else if (c->emit)
              fprintf(o, "        { JSValue t_; int rc_ = qjs_aot_construct_this(ctx, (JSObject *)JS_VALUE_GET_PTR(rgc_f_%s_%d), &t_);\n"
                         "          if (__builtin_expect(rc_ <= 0, 0)) { if (rc_ < 0) JS_FreeValue(ctx, JS_GetException(ctx)); goto %s; }\n          %s; rn_%d = t_; }\n", c->hex, ki, xl, rel_, ki);
            if (c->emit && K->direct && K->pre && getenv("QJS_AOT_REGION_PDNOPREFILL") == NULL)
              for (int jp_ = 0; jp_ < K->nn; jp_++)
                if (K->pre & (1u << jp_)) fprintf(o, "        *q2_slotp(rn_%d, %du * QJS_AOT_SIZEOF_JSPROPERTY) = rgc_dv_%s_%d[%d];   /* the prototype's default, until the constructor's store */\n", ki, jp_, c->hex, ki, jp_);
            if (!rg_walk_inst(c, ci, rootcur, root)) return 0;
            if (!c->emit) {
              /* the stores the walk met are the class's field list, in order, and nothing else: the object can be made in its final shape */
              char fn_[40];
              int jn;
              K->direct = K->site > 0 && getenv("QJS_AOT_REGION_NODIRECT") == NULL;
              for (jn = 0; K->direct && jn < K->nn; jn++) if (!qjs_aot_bundle_site_field_name(K->site, jn, fn_, sizeof(fn_)) || strcmp(fn_, K->names[jn]) != 0) K->direct = 0;
              if (K->direct && qjs_aot_bundle_site_field_name(K->site, K->nn, fn_, sizeof(fn_))) K->direct = 0;
              if (K->esc && !K->direct) return rg_fail(c, "pc %d: the object of new %s is seen while it is built and is not made in its final shape", in->pc, gn);
              /* a field the prototype chain holds a default for: a direct object is made with the default in its slot (the learn binds the value, a fact on the property keeps it), so a read before the
                 store gives the default and the store replaces it; the engine's define over a default is the same as over nothing */
              K->pre = 0;
              if (K->direct && K->site > 0)
                for (jn = 0; jn < K->nn; jn++)
                  if (qjs_aot_bundle_site_proto_field(K->site, K->names[jn])) K->pre |= 1u << jn;
              if (!K->direct)
                for (jn = 0; jn < K->nrbs; jn++) {
                  int ni2;
                  for (ni2 = 0; ni2 < K->nn && strcmp(K->names[ni2], K->rbs[jn]) != 0; ni2++) {}
                  if (ni2 < K->nn) return rg_fail(c, "pc %d: new %s: .%s is read before its store and the object is not made in its final shape", in->pc, gn, K->rbs[jn]);
                }
            }
            if (c->emit && K->direct && !K->esc && !K->stack) rg_emit_filled(c, o, ki);   /* a stack object is checked against the claims when (if) it migrates */
            if (c->emit) fprintf(o, "        %sv%d = rn_%d;\n", I->pfx, dv, ki);
          }
          break;
        }
        case ROP_put_global: {
          /* TRUST: a store to a global binding by the engine's own store, for a writable data property (anything else exits before it). The binding's old
             value is held to the region's end (the region may borrow from it), and the word is tested after: the binding may be one a fact watches */
          char gn_[40], xo[32];
          int q_;
          if (!rg_trust_on() || rg_cold(I, in)) {
            if (rg_cold(I, in)) { if (c->emit) fprintf(o, "        goto %s; /* put_global: never executed in the profile */\n", xl); break; }
            return rg_fail(c, "pc %d: put_global is not in the region subset (and the profile executed it)", in->pc);
          }
          if (u0 < 0 || !rg_atom(in, gn_, sizeof(gn_))) return rg_fail(c, "pc %d: put_global operands", in->pc);
          if (!c->emit) c->opaque = 1;
          q_ = rg_gv_add(c, gn_);
          if (q_ < 0) return rg_fail(c, "pc %d: too many globals", in->pc);
          rg_note_park(c, ii);
          if (c->emit) {
            rg_xlabel(c, rg_xit(c, ii, in->next), in->pc, "put-global-facts", xo, sizeof(xo));
            fprintf(o, "        { JSValue o_; int r_ = qjs_aot_global_put(ctx, rgga_%s[%d], %sv%d, %d, &o_);\n", c->hex, q_, I->pfx, u0, in->narg >= 3 ? (int)in->arg[2].val : 0);
            fprintf(o, "          if (__builtin_expect(r_ < 0, 0)) { RG_FREE_OWNED(); return JS_EXCEPTION; }\n");
            fprintf(o, "          if (__builtin_expect(r_ == 0, 0)) goto %s;\n", xl);
            fprintf(o, "          rg_own_put(ctx, &rgo_, &rgon_, &rgoc_, o_);\n");
            fprintf(o, "          if (__builtin_expect(!QJS_AOT_FACTS_OK(&rg_fw_%s, 1) || rgon_ >= RG_OWNCAP, 0)) goto %s; }\n", c->hex, xo);
          }
          {
            /* the IR gives the store new versions of the registers it keeps (a store to a binding changes none of them): each is the register's value before it */
            int d_;
            for (d_ = 0; d_ < in->nredef; d_++) {
              int reg_ = in->redef_reg[d_];
              if (!c->emit) continue;
              if (I->cur == NULL || reg_ < 0 || reg_ >= 256 || !I->cur[reg_][0]) return rg_fail(c, "pc %d: put_global redefines a register the region has no value for", in->pc);
              if (c->emit) fprintf(o, "        %sv%u = %s;\n", I->pfx, (unsigned)in->redef_val[d_], I->cur[reg_]);
            }
            for (d_ = 0; d_ < in->nredef && I->cur != NULL; d_++)
              if (in->redef_reg[d_] >= 0 && in->redef_reg[d_] < 256) snprintf(I->cur[in->redef_reg[d_]], 40, "%sv%u", I->pfx, (unsigned)in->redef_val[d_]);
          }
          break;
        }
        case ROP_return:
          if (u0 < 0) return rg_fail(c, "pc %d: return", in->pc);
          if (c->emit) {
            if (is_root) { fprintf(o, "        { JSValue rv_ = %sv%d; q2_dup_p(rv_);\n", I->pfx, u0); rg_free_owned(c, o, "          "); fprintf(o, "          return rv_; } /* every value the region holds is borrowed: the caller gets its own reference */\n"); }
            else if (rg_rty_typed(I)) rg_res_set(o, I, u0);
            else fprintf(o, "        %sres = %sv%d; goto %sdone;\n", I->pfx, I->pfx, u0, I->pfx);
          }
          break;
        case ROP_return_undefined:
          if (c->emit) {
            if (is_root) { fprintf(o, "        {\n"); rg_free_owned(c, o, "          "); fprintf(o, "          return JS_UNDEFINED; }\n"); }
            else fprintf(o, "        %sres = JS_UNDEFINED; goto %sdone;\n", I->pfx, I->pfx);
          }
          break;
#include "region_ops_bit.inc"
#include "region_ops_var.inc"
#include "region_ops_rec.inc"
        default: {
          /* branches */
          if (in->op == Q2_OP_BRANCH || in->op == Q2_OP_BRANCH_CMP) {
            if (k != B->ninsns - 1) return rg_fail(c, "pc %d: a branch inside a block", in->pc);
            if (in->rop == ROP_add_imm_self_branch || in->rop == ROP_add_imm_self_branch_s) return rg_fail(c, "pc %d: add_imm_self_branch is not in the region subset", in->pc);
            if (in->rop == ROP_inc_branch_lt || in->rop == ROP_inc_branch_lt_s) pair_read = 1; /* its result is made by the terminator; an exit before it sees the old counter */
            break;
          }
          {
            char nm[48];
            e_rop_name((uint16_t)in->rop, nm, sizeof(nm));
            if (rg_cold(I, in)) {
              if (c->emit) fprintf(o, "        goto %s; /* %s: never executed in the profile */\n", xl, nm);
              break;
            }
            return rg_fail(c, "pc %d: %s is not in the region subset (and the profile executed it)", in->pc, nm);
          }
        }
      }
      if (ldmask_) rg_ty_wrap_end(c, I, in, o, ldmask_, xl);
    rg_op_done:
      if (ov_active) { memcpy(I->cur, ovsave, 256 * 40); free(ovsave); }
      /* the current values of this function's registers (the frame state an exit writes) */
      if (I->cur != NULL) {
        int d;
        for (d = 0; d < in->ndef && d < 2 && !pair_read; d++) {
          int reg = fn->values[in->def[d]].reg;
          if (reg >= 0 && reg < 256)
            snprintf(I->cur[reg], 40, "%sv%u", I->pfx, (unsigned)in->def[d]);
        }
      }
    }
    /* the block's terminator */
    if (B->nsucc == 0) continue;
    {
      const Q2Insn *last = B->ninsns > 0 ? &fn->insns[B->first_insn + B->ninsns - 1] : NULL;
      int s, j;
      char edge[2][1536];
      for (s = 0; s < B->nsucc; s++) {
        size_t eo = 0;
        const Q2Block *T = &fn->blocks[B->succ_block[s]];
        edge[s][0] = 0;
        rg_edge_text(I, B, s, T, edge[s], sizeof(edge[s]));   /* the parameter takes the argument in its own type (region_fn_var.inc) */
        eo = strlen(edge[s]);
        if (getenv("QJS_AOT_REGION_WHY") && rg_trust_on() && fn->rpo_pos != NULL) fprintf(stderr, "compact: root %.8s inst %d block %d -> %d %s ok=%d\n", c->hex, ii, b, B->succ_block[s], fn->rpo_pos[B->succ_block[s]] <= fn->rpo_pos[b] ? "BACK" : "fwd", rg_compact_ok(fn, B->succ_block[s]));
        if (rg_trust_on() && ii < 256 && c->park[ii] && rg_compact_ok(fn, B->succ_block[s])) {
          int id = rg_cmk_id(c, ii, B->succ_block[s]);
          if (id >= 0 && fn->rpo_pos != NULL && fn->rpo_pos[B->succ_block[s]] <= fn->rpo_pos[b]) {
            /* a back edge: free what the loop parked and its parameters no longer hold */
            eo += (size_t)snprintf(edge[s] + eo, sizeof(edge[s]) - eo, "if (__builtin_expect(rgon_ - rgmk_[%d] >= %d, 0)) { JSValue kp_[%d]; ", id, 64 + T->nparams, T->nparams > 0 ? T->nparams : 1);
            for (j = 0; j < T->nparams; j++) eo += (size_t)snprintf(edge[s] + eo, sizeof(edge[s]) - eo, "kp_[%d] = %sv%u; ", j, I->pfx, (unsigned)T->param_val[j]);
            eo += (size_t)snprintf(edge[s] + eo, sizeof(edge[s]) - eo, "%srg_own_compact(ctx, rgo_, &rgon_, rgmk_[%d], kp_, %d); } ", getenv("QJS_AOT_REGION_KNOCKCOMPACT") != NULL ? "if (!getenv(\"QJS_AOT_REGION_NOCOMPACT\")) " : "", id, T->nparams);
          } else if (id >= 0) {
            eo += (size_t)snprintf(edge[s] + eo, sizeof(edge[s]) - eo, "rgmk_[%d] = rgon_; ", id);
          }
        }
      }
      if (!c->emit) continue;
      if (last != NULL && last->op == Q2_OP_BRANCH_CMP && B->nsucc == 2) {
        const char *cmp = rg_subop(last->sub);
        int a = (int)last->use[0], bb = (int)last->use[1];
        char xl2[32];
        rg_xlabel(c, rg_xit(c, ii, last->pc), last->pc, I->pfx, xl2, sizeof(xl2));
        if (cmp == NULL || last->nuse < 2) return rg_fail(c, "pc %d: a compare-branch of kind %d", last->pc, last->sub);
        if (last->rop == ROP_inc_branch_lt || last->rop == ROP_inc_branch_lt_s) {
          /* `d = d + 1` in place, then compare d with the bound: the exits above leave with d as it was (the generic body does the increment), so the table still has the old d */
          if (last->ndef < 1) return rg_fail(c, "pc %d: inc_branch_lt without a result", last->pc);
          if (rg_ty_num(I, (int)last->def[0])) {
            ROpd ao_, do_;
            char dn_[48];
            rg_opnd(I, a, &ao_);
            snprintf(dn_, sizeof(dn_), rg_tyof(I, (int)last->def[0]) == RT_I32 ? "%si%d" : "%sd%d", I->pfx, (int)last->def[0]);
            rg_emit_un(o, 'i', rg_tyof(I, (int)last->def[0]), dn_, &ao_, xl2);
            (void)do_;
          } else
          fprintf(o, "        { int32_t r_; if (__builtin_expect(JS_VALUE_GET_TAG(%sv%d) != JS_TAG_INT || __builtin_add_overflow(JS_VALUE_GET_INT(%sv%d), 1, &r_), 0)) goto %s; %sv%d = JS_MKVAL(JS_TAG_INT, r_); }\n",
                  I->pfx, a, I->pfx, a, xl2, I->pfx, (int)last->def[0]);
          a = (int)last->def[0];
        }
        if (rg_ty_num(I, a) && rg_ty_num(I, bb)) {
          /* two numbers of known representation: ints as ints, else doubles (NaN compares false), no tag test */
          ROpd ao_, bo_;
          char cb_[160];
          rg_opnd(I, a, &ao_); rg_opnd(I, bb, &bo_);
          rg_cmp_expr(&ao_, &bo_, cmp, cb_, sizeof(cb_));
          fprintf(o, "        if (%s %s) { %s goto %sB%d; } else { %s goto %sB%d; }\n", cb_, (last->sub & 0x40) ? "== 0" : "!= 0", edge[0], I->pfx, B->succ_block[0], edge[1], I->pfx, B->succ_block[1]);
        } else
        if (cmp[1] == '=' && (cmp[0] == '=' || cmp[0] == '!')) {
          /* an equality: two ints compare as ints; two objects are equal when they are the same object (no code runs); a strict equality of an object with anything else is decided by the tags */
          int strict_ = ((last->sub & 0x3f) == REG_BIN_STRICT_EQ || (last->sub & 0x3f) == REG_BIN_STRICT_NEQ);
          fprintf(o, "        { int t_; if (JS_VALUE_GET_TAG(%sv%d) == JS_TAG_INT && JS_VALUE_GET_TAG(%sv%d) == JS_TAG_INT) t_ = JS_VALUE_GET_INT(%sv%d) %s JS_VALUE_GET_INT(%sv%d);\n", I->pfx, a, I->pfx, bb, I->pfx, a, cmp, I->pfx, bb);
          fprintf(o, "          else if (JS_VALUE_GET_TAG(%sv%d) == JS_TAG_OBJECT && JS_VALUE_GET_TAG(%sv%d) == JS_TAG_OBJECT) t_ = JS_VALUE_GET_PTR(%sv%d) %s JS_VALUE_GET_PTR(%sv%d);\n", I->pfx, a, I->pfx, bb, I->pfx, a, cmp, I->pfx, bb);
          /* the other mixes of int, boolean, null, undefined and object that no code decides (an object against a number would run valueOf: it leaves) */
          fprintf(o, "          else if (rg_eqv(%sv%d, %sv%d, %d, &t_)) t_ = %st_;\n", I->pfx, a, I->pfx, bb, strict_, cmp[0] == '!' ? "!" : "");
          fprintf(o, "          else goto %s;\n", xl2);
          fprintf(o, "          if (t_ %s) { %s goto %sB%d; } else { %s goto %sB%d; } }\n", (last->sub & 0x40) ? "== 0" : "!= 0", edge[0], I->pfx, B->succ_block[0], edge[1], I->pfx, B->succ_block[1]);
        } else {
        /* two ints compare as ints, any other two numbers as doubles (NaN compares false, as the engine's), anything else leaves */
        fprintf(o, "        { int c_; if (__builtin_expect(JS_VALUE_GET_TAG(%sv%d) == JS_TAG_INT && JS_VALUE_GET_TAG(%sv%d) == JS_TAG_INT, 1)) c_ = JS_VALUE_GET_INT(%sv%d) %s JS_VALUE_GET_INT(%sv%d);\n", I->pfx, a, I->pfx, bb, I->pfx, a, cmp, I->pfx, bb);
        fprintf(o, "          else { double x_, y_; if (JS_VALUE_GET_TAG(%sv%d) == JS_TAG_INT) x_ = JS_VALUE_GET_INT(%sv%d); else if (JS_VALUE_GET_TAG(%sv%d) == JS_TAG_FLOAT64) x_ = JS_VALUE_GET_FLOAT64(%sv%d); else goto %s;\n", I->pfx, a, I->pfx, a, I->pfx, a, I->pfx, a, xl2);
        fprintf(o, "            if (JS_VALUE_GET_TAG(%sv%d) == JS_TAG_INT) y_ = JS_VALUE_GET_INT(%sv%d); else if (JS_VALUE_GET_TAG(%sv%d) == JS_TAG_FLOAT64) y_ = JS_VALUE_GET_FLOAT64(%sv%d); else goto %s;\n", I->pfx, bb, I->pfx, bb, I->pfx, bb, I->pfx, bb, xl2);
        fprintf(o, "            c_ = x_ %s y_; }\n", cmp);
        fprintf(o, "          if (c_ %s) { %s goto %sB%d; } else { %s goto %sB%d; } }\n",
                (last->sub & 0x40) ? "== 0" : "!= 0", edge[0], I->pfx, B->succ_block[0], edge[1], I->pfx, B->succ_block[1]);
        }
      } else if (last != NULL && last->op == Q2_OP_BRANCH && B->nsucc == 2 && (last->rop == ROP_branch_null || last->rop == ROP_branch_not_null ||
                                                                               last->rop == ROP_branch_nullish || last->rop == ROP_branch_not_nullish)) {
        /* a branch on null (the tag), or on null-or-undefined (an object is never nullish unless it is an HTMLDDA object, which leaves) */
        int a = (int)last->use[0], neg = last->rop == ROP_branch_not_null || last->rop == ROP_branch_not_nullish;
        char xl2[32];
        rg_xlabel(c, rg_xit(c, ii, last->pc), last->pc, I->pfx, xl2, sizeof(xl2));
        if (last->nuse < 1) return rg_fail(c, "pc %d: a null branch without an operand", last->pc);
        if (last->rop == ROP_branch_null || last->rop == ROP_branch_not_null)
          fprintf(o, "        { int nv_ = JS_VALUE_GET_TAG(%sv%d) == JS_TAG_NULL;\n", I->pfx, a);
        else
          fprintf(o, "        { int tg_ = JS_VALUE_GET_TAG(%sv%d), nv_ = tg_ == JS_TAG_NULL || tg_ == JS_TAG_UNDEFINED;\n          if (__builtin_expect(tg_ == JS_TAG_OBJECT && Q2_HTMLDDA(ctx), 0)) goto %s;\n", I->pfx, a, xl2);
        fprintf(o, "          if (nv_ %s) { %s goto %sB%d; } else { %s goto %sB%d; } }\n", neg ? "== 0" : "!= 0", edge[0], I->pfx, B->succ_block[0], edge[1], I->pfx, B->succ_block[1]);
      } else if (last != NULL && last->op == Q2_OP_BRANCH && B->nsucc == 2 && last->rop != ROP_branch_if && last->rop != ROP_branch_if_not) {
        return rg_fail(c, "pc %d: a branch kind the region does not know (rop %d)", last->pc, (int)last->rop);
      } else if (last != NULL && last->op == Q2_OP_BRANCH && B->nsucc == 2) {
        int a = (int)last->use[0];
        char xl2[32];
        rg_xlabel(c, rg_xit(c, ii, last->pc), last->pc, I->pfx, xl2, sizeof(xl2));
        if (rg_ty_num(I, a)) {
          /* a number of known representation: its truthiness (a double is false for 0, -0 and NaN) */
          ROpd ao_;
          char xs_[64];
          rg_opnd(I, a, &ao_);
          if (ao_.ty == RT_I32) snprintf(xs_, sizeof(xs_), "(%s != 0)", ao_.s); else snprintf(xs_, sizeof(xs_), "(%s != 0 && %s == %s)", ao_.s, ao_.s, ao_.s);
          fprintf(o, "        if (%s %s) { %s goto %sB%d; } else { %s goto %sB%d; }\n", xs_, last->rop == ROP_branch_if ? "!= 0" : "== 0", edge[0], I->pfx, B->succ_block[0], edge[1], I->pfx, B->succ_block[1]);
        } else {
        /* a branch on a value: JS truthiness for a boolean or an int (its payload), an object (true), null and undefined (false); a double, string or
           anything else leaves */
        fprintf(o, "        { int tv_; switch (JS_VALUE_GET_TAG(%sv%d)) { case JS_TAG_BOOL: case JS_TAG_INT: tv_ = JS_VALUE_GET_INT(%sv%d) != 0; break;\n", I->pfx, a, I->pfx, a);
        fprintf(o, "            case JS_TAG_OBJECT: if (__builtin_expect(Q2_HTMLDDA(ctx), 0)) goto %s; tv_ = 1; break; case JS_TAG_NULL: case JS_TAG_UNDEFINED: tv_ = 0; break;\n            case JS_TAG_FLOAT64: { double d_ = JS_VALUE_GET_FLOAT64(%sv%d); tv_ = d_ != 0 && d_ == d_; break; } default: goto %s; }\n", xl2, I->pfx, a, xl2);
        fprintf(o, "          if (tv_ %s) { %s goto %sB%d; } else { %s goto %sB%d; } }\n",
                last->rop == ROP_branch_if ? "!= 0" : "== 0", edge[0], I->pfx, B->succ_block[0], edge[1], I->pfx, B->succ_block[1]);
        }
      } else if (B->nsucc == 1) {
        /* a back edge polls (the root's and an inlined function's): a handler may run anything, so a poll that ran one leaves the region */
        if (fn->rpo_pos != NULL && fn->rpo_pos[B->succ_block[0]] <= fn->rpo_pos[b]) {
          int xi = rg_xit(c, ii, fn->blocks[B->succ_block[0]].start_pc);
          if (rg_trust_on() && getenv("QJS_AOT_REGION_POLLEXIT") == NULL)
            /* TRUST: the poll is served here and the loop goes on (a long-running region left at its first poll would run the rest generic); an
               interrupt leaves with its exception */
            fprintf(o, "        %s\n        if (__builtin_expect(Q2_POLL_HIT(ctx), 0)) { ++*(int *)((unsigned char *)ctx + QJS_AOT_OFF_CTX_INTERRUPT_COUNTER); if (__builtin_expect(qjs_aot_poll_fl(ctx) < 0, 0)) { RG_FREE_OWNED(); return JS_EXCEPTION; } }\n", edge[0]);
          else
          fprintf(o, "        %s\n        if (__builtin_expect(Q2_POLL_HIT(ctx), 0)) { ++*(int *)((unsigned char *)ctx + QJS_AOT_OFF_CTX_INTERRUPT_COUNTER); goto rp_%d; }\n", edge[0], xi);
        } else {
          fprintf(o, "        %s\n", edge[0]);
        }
        fprintf(o, "        goto %sB%d;\n", I->pfx, B->succ_block[0]);
      } else {
        return rg_fail(c, "block %d: a terminator the region does not know", b);
      }
    }
  }
  if (c->emit && !is_root) fprintf(o, "        %sdone: (void)0;\n", I->pfx);
  return 1;
}

/* TRUST (AOT-REDESIGN.md, the class table; reviewer item 6): everything the region assumes is bound ONCE, by rg_init_HEX at the first entry, and stated as facts
   of the region's word; the fast path then tests the word only. Bound here, in order: the globals read as values (their atoms), the constant paths G.f... (a fact
   on every link), the classes (constructor, prototype, class shape, layout), the method of every dispatch arm (found from the class's prototype or the constant
   receiver, with a fact on every object of the walk), the function X of every `X.call` and the builtin `call` it reaches, the push/pop builtins, the
   constructors of the region's constructs (and their class shapes), and the G.F constants. Nothing is learned from a receiver at run time; a region whose init
   fails runs its generic body. */
static void rg_emit_init(RCtx *c) {
  FILE *o = c->o;
  const char *h = c->hex;
  int fs = getenv("QJS_AOT_FASTSTATS") != NULL, k, q, gi, t, j, bi, any_intr = 0, any_pop = 0;
#define RG_FAIL_(msg) fprintf(o, "  { %s goto out; }\n", fs ? "fprintf(stderr, \"region init " msg "\\n\");" : "")
  for (gi = 0; gi < c->ngd; gi++) {
    RGuard *G = &c->gd[gi];
    if (G->intr) { if (strcmp(G->method, "pop") == 0) any_pop = 1; else any_intr = 1; continue; }
    if (G->thru) fprintf(o, "static JSObject *rg_tf_%s_%d[%d];\n", h, gi, RG_MAXT);
    else if (G->dispatch) fprintf(o, "static JSObject *rg_fv_%s_%d[%d]; static const void *rk_sa_%s_%d[%d]; static uint8_t rg_kt_%s_%d;\n", h, gi, RG_MAXT, h, gi, RG_MAXT, h, gi);
  }
  if (any_intr) fprintf(o, "static JSValue rg_pf_%s;\n", h);
  if (any_pop) fprintf(o, "static JSValue rg_pp_%s;\n", h);
  fprintf(o, "static void *rk_s_%s[%d]; static JSObject *rk_p_%s[%d];\n", h, c->ncls > 0 ? c->ncls : 1, h, c->ncls > 0 ? c->ncls : 1);
  {
    int any_ms = 0, kq, sq;
    for (kq = 0; kq < c->ncls; kq++) if (c->cls[kq].nsh > 1) any_ms = 1;
    if (any_ms) {
      /* the shape table of the multi-shape classes: rk_sx_[k][s] the held shape s of class k (0 = rk_s_[k]), rk_ns_[k] their count (0 = a single-shape class), rk_tp_[k][s] the shape one field
         shorter that an add of the atom rk_ta_[k][s] turns into s (-1: none observed), so a store that adds a field to an object of shape p is the constant transition to the s with rk_tp_[k][s] == p
         and that atom */
      fprintf(o, "static void *rk_sx_%s[%d][%d]; static uint32_t rk_ta_%s[%d][%d];\n", h, c->ncls, QJS_AOT_MAXSHAPES, h, c->ncls, QJS_AOT_MAXSHAPES);
      fprintf(o, "static const uint8_t rk_ns_%s[%d] = {", h, c->ncls);
      for (kq = 0; kq < c->ncls; kq++) fprintf(o, "%s%d", kq ? "," : "", c->cls[kq].nsh > 1 ? c->cls[kq].nsh : 0);
      fprintf(o, "};\nstatic const int8_t rk_tp_%s[%d][%d] = {", h, c->ncls, QJS_AOT_MAXSHAPES);
      for (kq = 0; kq < c->ncls; kq++) {
        fprintf(o, "%s{", kq ? "," : "");
        for (sq = 0; sq < QJS_AOT_MAXSHAPES; sq++) fprintf(o, "%s%d", sq ? "," : "", c->cls[kq].nsh > 1 && sq < c->cls[kq].nsh ? c->cls[kq].ms.parent[sq] : -1);
        fprintf(o, "}");
      }
      fprintf(o, "};\n");
    }
  }
  fprintf(o, "static uint32_t rgga_%s[%d]; static JSValue rgp_%s[%d]; static JSObject *rgp_o_%s[%d];\n", h, c->ngv > 0 ? c->ngv : 1, h, c->ncp > 0 ? c->ncp : 1, h, c->ncp > 0 ? c->ncp : 1);
  for (q = 0; q < c->ncons; q++) if (c->cons[q].kind == 0 && c->cons[q].direct) fprintf(o, "static void *rgc_s_%s_%d;\n", h, q);
  fprintf(o, "static uint8_t rg_ini_%s, rg_itry_%s;\n", h, h);
  /* init failures and the entries the fallback (the speculative body, aot_fs_) served instead of the region; QJS_AOT_STATS=1 prints them at exit */
  fprintf(o, "static uint32_t rg_ifail_%s, rg_byp_%s;\nstatic JSValue aot_fs_%s(JSContext *, JSRegRun *, JSValue *, uint32_t);\n", h, h, h);
  fprintf(o, "static void __attribute__((destructor)) rg_ifail_print_%s(void) { if (getenv(\"QJS_AOT_STATS\") != NULL && (rg_ifail_%s || rg_byp_%s)) fprintf(stderr, \"aot region %s init-failures=%%u tries=%%u initialised=%%u bypassed-entries=%%u\\n\", rg_ifail_%s, (unsigned)rg_itry_%s, (unsigned)rg_ini_%s, rg_byp_%s); }\n", h, h, h, h, h, h, h, h);
  /* an own plain data property, no code run */
  fprintf(o, "static int rg_dat_%s(JSContext *ctx, JSValue obj, const char *name, JSValue *out)\n{\n  JSPropertyDescriptor d; JSAtom a; int r;\n", h);
  fprintf(o, "  if (!JS_IsObject(obj)) return 0;\n  a = JS_NewAtom(ctx, name); r = JS_GetOwnProperty(ctx, &d, obj, a); JS_FreeAtom(ctx, a);\n  if (r <= 0) return 0;\n");
  fprintf(o, "  JS_FreeValue(ctx, d.getter); JS_FreeValue(ctx, d.setter);\n  if (d.flags & JS_PROP_GETSET) { JS_FreeValue(ctx, d.value); return 0; }\n  *out = d.value;\n  return 1;\n}\n");
  /* a fact that obj.name stays val */
  fprintf(o, "static int rg_kx_%s(JSContext *ctx, JSValue obj, const char *name, JSValue val)\n{\n  JSAtom a = JS_NewAtom(ctx, name); JSAotFact *k_ = qjs_aot_fact_new(ctx, obj, JS_UNDEFINED, 0, &a, 0);\n", h);
  /* a fact that expects the value of a global binding must hear about direct writes of that binding (compiled and interpreted global stores): the atom goes on the watch list */
  fprintf(o, "  int ok = k_ != NULL && qjs_aot_fact_expect(ctx, k_, a, val) == 0 && qjs_aot_fact_validate(ctx, k_) == 1%s;\n  if (ok) qjs_aot_fact_depend64(k_, &rg_fw_%s, 1);\n  JS_FreeAtom(ctx, a);\n  return ok;\n}\n",
          getenv("QJS_AOT_REGION_NOGWATCH") != NULL ? "" :
          getenv("QJS_AOT_REGION_KNOCKGW") != NULL ? " && (getenv(\"QJS_AOT_REGION_NOGWATCH\") != NULL || qjs_aot_global_watch(ctx, obj, (int)a) || getenv(\"QJS_AOT_REGION_GWFULLOPEN\") != NULL)"   /* the knock-out build: the run skips the watch, or goes on unwatched when the list is full */
                                                    : " && qjs_aot_global_watch(ctx, obj, (int)a)", h);
  /* the method `name` found from `start` (start itself first, then its prototype chain): a plain data property whose function has the digest; every object of the
     walk without it gets an ABSENT fact and its prototype link a PROTO fact, the holder a METHOD fact. 1 with the function (owned) */
  fprintf(o, "static int rg_ml_%s(JSContext *ctx, JSValue start, const char *name, const uint8_t *dg, JSValue *out)\n{\n", h);
  fprintf(o, "  JSValue cur = JS_DupValue(ctx, start), nx; JSAtom a = JS_NewAtom(ctx, name); int i, ok = 0;\n");
  fprintf(o, "  for (i = 0; i < 8 && JS_IsObject(cur); i++) {\n    JSPropertyDescriptor d; int r = JS_GetOwnProperty(ctx, &d, cur, a);\n");
  fprintf(o, "    if (r < 0) { JS_FreeValue(ctx, JS_GetException(ctx)); break; }\n");
  fprintf(o, "    if (r > 0) { uint8_t g_[16]; JSAotFact *m_; JS_FreeValue(ctx, d.getter); JS_FreeValue(ctx, d.setter);\n");
  fprintf(o, "      if ((d.flags & JS_PROP_GETSET) || !qjs_aot_function_digest(d.value, g_) || memcmp(g_, dg, 16) != 0) { JS_FreeValue(ctx, d.value); break; }\n");
  fprintf(o, "      m_ = qjs_aot_fact_method(ctx, cur, a, d.value, NULL); if (m_ == NULL) { JS_FreeValue(ctx, d.value); break; }\n");
  fprintf(o, "      qjs_aot_fact_depend64(m_, &rg_fw_%s, 1); *out = d.value; ok = 1; break; }\n", h);
  fprintf(o, "    nx = JS_GetPrototype(ctx, cur);\n    { JSAotFact *g_ = qjs_aot_fact_new(ctx, cur, JS_UNDEFINED, 0, NULL, 0), *p_;\n");
  fprintf(o, "      if (g_ == NULL || qjs_aot_fact_absent(ctx, g_, a) != 0 || qjs_aot_fact_validate(ctx, g_) != 1 || (p_ = qjs_aot_fact_proto_is(ctx, cur, nx)) == NULL) { JS_FreeValue(ctx, nx); break; }\n");
  fprintf(o, "      qjs_aot_fact_depend64(g_, &rg_fw_%s, 1); qjs_aot_fact_depend64(p_, &rg_fw_%s, 1); }\n", h, h);
  fprintf(o, "    JS_FreeValue(ctx, cur); cur = nx;\n  }\n  JS_FreeValue(ctx, cur); JS_FreeAtom(ctx, a);\n  return ok;\n}\n");

  fprintf(o, "static int rg_init_%s(JSContext *ctx)\n{\n  JSValue gv = JS_GetGlobalObject(ctx), F = JS_UNDEFINED, P = JS_UNDEFINED, fv = JS_UNDEFINED, cur = JS_UNDEFINED, nx = JS_UNDEFINED;\n  int ok = 0;\n  (void)F; (void)P; (void)fv; (void)cur; (void)nx;\n"
             "  /* the unit's teardown releases what an init holds (class shapes, functions) for the runtime it has recorded: a region may be the first holder */\n"
             "  if (q2_cc_rt == NULL) q2_cc_rt = JS_GetRuntime(ctx);\n", h);
  /* globals read as values */
  for (q = 0; q < c->ngv; q++) {
    fprintf(o, "  rgga_%s[%d] = (uint32_t)JS_NewAtom(ctx, ", h, q); rg_cstr(o, c->gv[q]); fprintf(o, ");\n");
  }
  /* constant paths */
  for (q = 0; q < c->ncp; q++) {
    fprintf(o, "  /* %s", c->cp[q][0]); for (j = 1; j < c->cpn[q]; j++) fprintf(o, ".%s", c->cp[q][j]); fprintf(o, " */\n");
    fprintf(o, "  if (!rg_dat_%s(ctx, gv, ", h); rg_cstr(o, c->cp[q][0]); fprintf(o, ", &cur) || !rg_kx_%s(ctx, gv, ", h); rg_cstr(o, c->cp[q][0]); fprintf(o, ", cur))"); RG_FAIL_("path head");
    for (j = 1; j < c->cpn[q]; j++) {
      fprintf(o, "  nx = JS_UNDEFINED; if (!rg_dat_%s(ctx, cur, ", h); rg_cstr(o, c->cp[q][j]); fprintf(o, ", &nx) || !rg_kx_%s(ctx, cur, ", h); rg_cstr(o, c->cp[q][j]); fprintf(o, ", nx))"); RG_FAIL_("path link");
      fprintf(o, "  JS_FreeValue(ctx, cur); cur = nx; nx = JS_UNDEFINED;\n");
    }
    fprintf(o, "  if (JS_IsObject(cur)) { if ((rgp_o_%s[%d] = q2_fh_hold(ctx, cur, &rgp_o_%s[%d])) == NULL)", h, q, h, q); RG_FAIL_("path hold");
    fprintf(o, "    rgp_%s[%d] = JS_MKPTR(JS_TAG_OBJECT, rgp_o_%s[%d]); } else rgp_%s[%d] = cur;\n  JS_FreeValue(ctx, cur); cur = JS_UNDEFINED;\n", h, q, h, q, h, q);
    if (c->cpi[q]) { fprintf(o, "  if (" RG_KN("MATHBUILTIN") " == NULL && !q2_is_intr(rgp_%s[%d], %d, %d))", h, q, c->cpi[q] - 1, c->cpm[q]); RG_FAIL_("path is not the builtin"); }
    if (c->cpk[q]) { fprintf(o, "  if (JS_VALUE_GET_TAG(rgp_%s[%d]) != JS_TAG_INT || JS_VALUE_GET_INT(rgp_%s[%d]) != %d)", h, q, h, q, (int)c->cpv[q]); RG_FAIL_("path constant"); }
  }
  /* classes */
  for (k = 0; k < c->ncls; k++) {
    const RCls *K = &c->cls[k];
    fprintf(o, "  /* class %s (%d fields) */\n", K->ctor, K->nn);
    {
      /* the class constructor is a global (`Vec`) or a constant path (`NS.Vec`): every link a data property under a watched fact */
      char pt_[48], *seg_[4], *d_;
      int ns_ = 0, js_;
      snprintf(pt_, sizeof(pt_), "%s", K->ctor);
      seg_[ns_++] = pt_;
      while (ns_ < 4 && (d_ = strchr(seg_[ns_ - 1], '.')) != NULL) { *d_ = 0; seg_[ns_++] = d_ + 1; }
      for (js_ = 0; js_ < ns_; js_++) {
        const char *hold_ = js_ == 0 ? "gv" : "cur", *dst_ = js_ == ns_ - 1 ? "F" : "nx";
        fprintf(o, "  if (!rg_dat_%s(ctx, %s, ", h, hold_); rg_cstr(o, seg_[js_]); fprintf(o, ", &%s) || JS_VALUE_GET_TAG(%s) != JS_TAG_OBJECT || !rg_kx_%s(ctx, %s, ", dst_, dst_, h, hold_); rg_cstr(o, seg_[js_]); fprintf(o, ", %s))", dst_); RG_FAIL_("class constructor");
        if (js_ < ns_ - 1) fprintf(o, "  JS_FreeValue(ctx, cur); cur = nx; nx = JS_UNDEFINED;\n");
      }
      if (ns_ > 1) fprintf(o, "  JS_FreeValue(ctx, cur); cur = JS_UNDEFINED;\n");
    }
    fprintf(o, "  qjs_aot_ctor_prepare(ctx, F);\n");
    fprintf(o, "  if (!rg_dat_%s(ctx, F, \"prototype\", &P) || JS_VALUE_GET_TAG(P) != JS_TAG_OBJECT || !rg_kx_%s(ctx, F, \"prototype\", P))", h, h); RG_FAIL_("class prototype");
    fprintf(o, "  { uint32_t at_[%d]; int i_; void *sh_;\n", K->nn > 0 ? K->nn : 1);
    for (j = 0; j < K->nn; j++) { fprintf(o, "    at_[%d] = (uint32_t)JS_NewAtom(ctx, ", j); rg_cstr(o, K->names[j]); fprintf(o, ");\n"); }
    fprintf(o, "    sh_ = qjs_aot_class_shape(ctx, P, at_, %d);\n    for (i_ = 0; i_ < %d; i_++) JS_FreeAtom(ctx, (JSAtom)at_[i_]);\n", K->nn, K->nn);
    fprintf(o, "    if (sh_ == NULL || q2_phn >= Q2_PH_MAX)"); RG_FAIL_("class shape");
    fprintf(o, "    q2_ph[q2_phn++] = sh_; rk_s_%s[%d] = sh_; }\n", h, k);
    if (getenv("QJS_AOT_REGION_SHAPECHECK") != NULL && K->nn == 0) {}
    if (getenv("QJS_AOT_REGION_SHAPECHECK") != NULL) {
      /* a debug probe (runs the constructor): an instance made by the engine must have the class shape */
      fprintf(o, "  { JSValue i_ = JS_CallConstructor(ctx, F, 0, NULL); if (JS_IsException(i_)) JS_FreeValue(ctx, JS_GetException(ctx)); else { const void *is_ = *(const void *const *)((const char *)JS_VALUE_GET_PTR(i_) + QJS_AOT_OFF_JSOBJECT_SHAPE);\n");
      fprintf(o, "    fprintf(stderr, \"shapecheck %s %%s: instance %%p class %%p props %%d/%%d\\n\", is_ == rk_s_%s[%d] ? \"SAME\" : \"DIFFERENT\", is_, rk_s_%s[%d], *(const int *)((const unsigned char *)is_ + QJS_AOT_OFF_JSSHAPE_PROP_COUNT), *(const int *)((const unsigned char *)rk_s_%s[%d] + QJS_AOT_OFF_JSSHAPE_PROP_COUNT)); JS_FreeValue(ctx, i_); } }\n", K->ctor, h, k, h, k, h, k);
    }
    if (K->need_layout) {
      /* fields of loaded instances are read at fixed slots with no check: an instance that leaves the class shape must break the word */
      fprintf(o, "  { unsigned id_ = *(const uint16_t *)((const unsigned char *)rk_s_%s[%d] + QJS_AOT_OFF_SHAPE_LAYOUT_ID);\n", h, k);
      fprintf(o, "    if (id_ == 0 || id_ > 63 || qjs_aot_layout_depend(ctx, id_, &rg_fw_%s, 1) != 1) {", h);
      if (fs) { fprintf(o, " fprintf(stderr, \"region init: class %%s has no layout for its fixed-slot reads (id %%u, layouts %%u)\\n\", "); rg_cstr(o, K->ctor); fprintf(o, ", id_, (unsigned)Q2_LAYOUT_COUNT);"); }
      fprintf(o, " goto out; } }\n");
      if (fs) fprintf(o, "  fprintf(stderr, \"region %s class %s layout %%u\\n\", (unsigned)*(const uint16_t *)((const unsigned char *)rk_s_%s[%d] + QJS_AOT_OFF_SHAPE_LAYOUT_ID));\n", h, K->ctor, h, k);
    }
    if (K->nsh > 1) {
      int sq;
      fprintf(o, "  /* the other shapes of class %s: each built from the prototype and its ordered fields, held; a transition (rk_tp_) is the shape one field shorter plus the atom rk_ta_ */\n", K->ctor);
      fprintf(o, "  rk_sx_%s[%d][0] = rk_s_%s[%d];\n", h, k, h, k);
      for (sq = 1; sq < K->nsh; sq++) {
        fprintf(o, "  { uint32_t at_[%d]; int i_; void *sh_;\n", K->ms.nn[sq] > 0 ? K->ms.nn[sq] : 1);
        for (j = 0; j < K->ms.nn[sq]; j++) { fprintf(o, "    at_[%d] = (uint32_t)JS_NewAtom(ctx, ", j); rg_cstr(o, K->ms.names[sq][j]); fprintf(o, ");\n"); }
        fprintf(o, "    sh_ = qjs_aot_class_shape(ctx, P, at_, %d);\n", K->ms.nn[sq]);
        if (K->ms.parent[sq] >= 0) fprintf(o, "    rk_ta_%s[%d][%d] = at_[%d];\n    for (i_ = 0; i_ < %d; i_++) if (i_ != %d) JS_FreeAtom(ctx, (JSAtom)at_[i_]);\n", h, k, sq, K->ms.nn[sq] - 1, K->ms.nn[sq], K->ms.nn[sq] - 1);
        else fprintf(o, "    for (i_ = 0; i_ < %d; i_++) JS_FreeAtom(ctx, (JSAtom)at_[i_]);\n", K->ms.nn[sq]);
        fprintf(o, "    if (sh_ == NULL || q2_phn >= Q2_PH_MAX)"); RG_FAIL_("class shape");
        fprintf(o, "    q2_ph[q2_phn++] = sh_; rk_sx_%s[%d][%d] = sh_; }\n", h, k, sq);
      }
    }
    fprintf(o, "  rk_p_%s[%d] = (JSObject *)JS_VALUE_GET_PTR(P);\n  JS_FreeValue(ctx, F); JS_FreeValue(ctx, P); F = P = JS_UNDEFINED;\n", h, k);
  }
  /* dispatch arms */
  for (gi = 0; gi < c->ngd; gi++) {
    RGuard *G = &c->gd[gi];
    if (G->intr || !G->dispatch) continue;
    if (G->thru) {
      /* X.call(...): X is the constant path's function, its `call` is the builtin through X's prototype chain, X has no own `call` */
      fprintf(o, "  { uint8_t dg_[16]; JSValue x_ = rgp_%s[%d], pr_; JSAtom ca_; JSAotFact *g_, *p_;\n", h, G->tcp);
      fprintf(o, "    if (!qjs_aot_function_digest(x_, dg_) || memcmp(dg_, (const uint8_t[16]){");
      for (bi = 0; bi < 16; bi++) fprintf(o, "%s0x%02x", bi ? "," : "", G->tg[0]->digest[bi]);
      fprintf(o, "}, 16) != 0)"); RG_FAIL_("call target");
      fprintf(o, "    pr_ = JS_GetPrototype(ctx, x_); fv = JS_UNDEFINED;\n    if (!rg_dat_%s(ctx, pr_, \"call\", &fv) || !q2_is_fn_call(fv)) { JS_FreeValue(ctx, pr_); JS_FreeValue(ctx, fv); fv = JS_UNDEFINED; %s goto out; }\n", h,
              fs ? "fprintf(stderr, \"region init: call builtin\\n\");" : "");
      fprintf(o, "    ca_ = JS_NewAtom(ctx, \"call\");\n");
      fprintf(o, "    g_ = qjs_aot_fact_new(ctx, x_, JS_UNDEFINED, 0, NULL, 0); p_ = qjs_aot_fact_proto_is(ctx, x_, pr_);\n");
      fprintf(o, "    { JSAotFact *m_ = qjs_aot_fact_method(ctx, pr_, ca_, fv, NULL); int ok_ = g_ != NULL && p_ != NULL && m_ != NULL && qjs_aot_fact_absent(ctx, g_, ca_) == 0 && qjs_aot_fact_validate(ctx, g_) == 1;\n");
      fprintf(o, "      if (ok_) { qjs_aot_fact_depend64(g_, &rg_fw_%s, 1); qjs_aot_fact_depend64(p_, &rg_fw_%s, 1); qjs_aot_fact_depend64(m_, &rg_fw_%s, 1); }\n", h, h, h);
      fprintf(o, "      JS_FreeAtom(ctx, ca_); JS_FreeValue(ctx, pr_); JS_FreeValue(ctx, fv); fv = JS_UNDEFINED;\n      if (!ok_)"); RG_FAIL_("call facts");
      fprintf(o, "    }\n    if ((rg_tf_%s_%d[0] = q2_fh_hold(ctx, x_, &rg_tf_%s_%d[0])) == NULL)", h, gi, h, gi); RG_FAIL_("call hold");
      fprintf(o, "  }\n");
      continue;
    }
    for (t = 0; t < G->ntg; t++) {
      int kk = G->kcls[t];
      fprintf(o, "  /* .%s arm %d: %s */\n", G->method, t, G->crecv ? "a constant receiver" : c->cls[kk].ctor);
      if (!G->crecv) for (j = 0; j < c->cls[kk].nn; j++) if (strcmp(c->cls[kk].names[j], G->method) == 0) { fprintf(o, "  /* the method is shadowed by a field */\n"); RG_FAIL_("shadowed method"); }
      /* the arm is bound when the method found from the class's prototype is this arm's candidate (an arm of another candidate stays unbound) */
      fprintf(o, "  if (rg_ml_%s(ctx, %s", h, G->crecv ? "rgp_" : "JS_MKPTR(JS_TAG_OBJECT, rk_p_");
      if (G->crecv) fprintf(o, "%s[%d]", h, G->tcp); else fprintf(o, "%s[%d])", h, kk);
      fprintf(o, ", "); rg_cstr(o, G->method); fprintf(o, ", (const uint8_t[16]){");
      for (bi = 0; bi < 16; bi++) fprintf(o, "%s0x%02x", bi ? "," : "", G->tg[t]->digest[bi]);
      fprintf(o, "}, &fv)) {\n");
      fprintf(o, "    rg_fv_%s_%d[%d] = q2_fh_hold(ctx, fv, &rg_fv_%s_%d[%d]); JS_FreeValue(ctx, fv); fv = JS_UNDEFINED;\n    if (rg_fv_%s_%d[%d] == NULL)", h, gi, t, h, gi, t, h, gi, t); RG_FAIL_("method hold");
      if (!G->crecv) fprintf(o, "    rk_sa_%s_%d[%d] = rk_s_%s[%d]; rg_kt_%s_%d = %d;\n", h, gi, t, h, kk, h, gi, t);
      fprintf(o, "  }\n");
      if (G->crecv) fprintf(o, "  if (rg_fv_%s_%d[%d] == NULL)", h, gi, t), RG_FAIL_("method");
      if (G->fwd && !G->crecv) {
        /* the forward `m.apply(this, arguments)` calls m directly only if the `apply` it reads from m is the builtin: m has no own `apply`, its prototype is Function.prototype, whose `apply`
           is the engine's. All three are facts of the region's word, so a change of any of them leaves the region (and invalidates it). Bound arms only. */
        fprintf(o, "  if (rg_fv_%s_%d[%d] != NULL) { JSValue x_ = JS_MKPTR(JS_TAG_OBJECT, rg_fv_%s_%d[%d]), pr_; JSAtom ca_; JSAotFact *g_, *p_;\n", h, gi, t, h, gi, t);
        fprintf(o, "    pr_ = JS_GetPrototype(ctx, x_); fv = JS_UNDEFINED;\n    if (!rg_dat_%s(ctx, pr_, \"apply\", &fv) || !q2_is_fn_apply(fv)) { JS_FreeValue(ctx, pr_); JS_FreeValue(ctx, fv); fv = JS_UNDEFINED; %s goto out; }\n", h,
                fs ? "fprintf(stderr, \"region init: apply builtin\\n\");" : "");
        fprintf(o, "    ca_ = JS_NewAtom(ctx, \"apply\");\n");
        fprintf(o, "    g_ = qjs_aot_fact_new(ctx, x_, JS_UNDEFINED, 0, NULL, 0); p_ = qjs_aot_fact_proto_is(ctx, x_, pr_);\n");
        fprintf(o, "    { JSAotFact *m_ = qjs_aot_fact_method(ctx, pr_, ca_, fv, NULL); int ok_ = g_ != NULL && p_ != NULL && m_ != NULL && qjs_aot_fact_absent(ctx, g_, ca_) == 0 && qjs_aot_fact_validate(ctx, g_) == 1;\n");
        fprintf(o, "      if (ok_) { qjs_aot_fact_depend64(g_, &rg_fw_%s, 1); qjs_aot_fact_depend64(p_, &rg_fw_%s, 1); qjs_aot_fact_depend64(m_, &rg_fw_%s, 1); }\n", h, h, h);
        fprintf(o, "      JS_FreeAtom(ctx, ca_); JS_FreeValue(ctx, pr_); JS_FreeValue(ctx, fv); fv = JS_UNDEFINED;\n      if (!ok_)"); RG_FAIL_("apply facts");
        fprintf(o, "    }\n  }\n");
      }
    }
    /* every class of the call has its method bound by one of its arms */
    for (t = 0; t < G->ntg && !G->crecv; t++) {
      int t2, first = 1;
      for (t2 = 0; t2 < t; t2++) if (G->kcls[t2] == G->kcls[t]) break;
      if (t2 < t) continue;
      fprintf(o, "  if (");
      for (t2 = t; t2 < G->ntg; t2++) if (G->kcls[t2] == G->kcls[t]) { fprintf(o, "%srg_fv_%s_%d[%d] == NULL", first ? "" : " && ", h, gi, t2); first = 0; }
      fprintf(o, ")"); RG_FAIL_("method");
    }
  }
  /* the push/pop builtins: own data properties of the array prototype, the facts on the chain from a fresh array */
  if (any_intr || any_pop) {
    int w;
    for (w = 0; w < 2; w++) {
      if ((w == 0 && !any_intr) || (w == 1 && !any_pop)) continue;
      fprintf(o, "  { JSValue ar_ = JS_NewArray(ctx), pr_ = JS_GetPrototype(ctx, ar_); fv = JS_UNDEFINED;\n");
      fprintf(o, "    int ok_ = JS_IsObject(pr_) && rg_dat_%s(ctx, pr_, \"%s\", &fv) && q2_builtin_is(fv, &%s, %d) && rg_fx_%s(ctx, ar_, \"%s\", fv);\n", h, w ? "pop" : "push", w ? "e_arrpop_fn" : "e_arrpush_fn", w, h, w ? "pop" : "push");
      fprintf(o, "    if (ok_) rg_p%c_%s = fv; JS_FreeValue(ctx, fv); fv = JS_UNDEFINED; JS_FreeValue(ctx, pr_); JS_FreeValue(ctx, ar_);\n    if (!ok_)", w ? 'p' : 'f', h); RG_FAIL_("array builtin");
      fprintf(o, "  }\n");
    }
  }
  /* constructs */
  for (q = 0; q < c->ncons; q++) {
    const RCons *K = &c->cons[q];
    if (K->kind == 2) continue;
    fprintf(o, "  if (!rgc_ok_%s_%d && !rg_clearn_%s_%d(ctx))", h, q, h, q); RG_FAIL_("construct");
    if (K->kind == 0 && K->direct) {
      /* the region makes these objects itself: the engine's first-construct work (the unit's layout chain) is done here */
      fprintf(o, "  qjs_aot_ctor_prepare(ctx, rgc_f_%s_%d);\n", h, q);
      fprintf(o, "  P = JS_UNDEFINED; if (!rg_dat_%s(ctx, rgc_f_%s_%d, \"prototype\", &P) || JS_VALUE_GET_TAG(P) != JS_TAG_OBJECT)", h, h, q); RG_FAIL_("construct prototype");
      fprintf(o, "  { uint32_t at_[%d]; int i_;\n", K->nn > 0 ? K->nn : 1);
      for (j = 0; j < K->nn; j++) { fprintf(o, "    at_[%d] = (uint32_t)JS_NewAtom(ctx, ", j); rg_cstr(o, K->names[j]); fprintf(o, ");\n"); }
      fprintf(o, "    rgc_s_%s_%d = qjs_aot_class_shape(ctx, P, at_, %d);\n    for (i_ = 0; i_ < %d; i_++) JS_FreeAtom(ctx, (JSAtom)at_[i_]); }\n", h, q, K->nn, K->nn);
      fprintf(o, "  JS_FreeValue(ctx, P); P = JS_UNDEFINED;\n  if (rgc_s_%s_%d == NULL || q2_phn >= Q2_PH_MAX)", h, q); RG_FAIL_("construct shape");
      fprintf(o, "  q2_ph[q2_phn++] = rgc_s_%s_%d;\n", h, q);
      if (getenv("QJS_AOT_REGION_SHAPECHECK") != NULL) {
        fprintf(o, "  { JSValue i_ = JS_CallConstructor(ctx, rgc_f_%s_%d, 0, NULL); if (JS_IsException(i_)) JS_FreeValue(ctx, JS_GetException(ctx)); else { const void *is_ = *(const void *const *)((const char *)JS_VALUE_GET_PTR(i_) + QJS_AOT_OFF_JSOBJECT_SHAPE);\n", h, q);
        fprintf(o, "    fprintf(stderr, \"shapecheck construct %s %%s: instance %%p class %%p\\n\", is_ == rgc_s_%s_%d ? \"SAME\" : \"DIFFERENT\", is_, rgc_s_%s_%d); JS_FreeValue(ctx, i_); } }\n", K->gname, h, q, h, q);
      }
    }
  }
  if (c->ngl > 0) { fprintf(o, "  if (!rgk_ok_%s) rg_kfacts_%s(ctx);\n  if (!rgk_ok_%s)", h, h, h); RG_FAIL_("global constants"); }
  fprintf(o, "  if (!qjs_aot_facts_commit(ctx, &rg_fw_%s, 1))", h); RG_FAIL_("commit");
  fprintf(o, "  ok = 1;\n out:\n  JS_FreeValue(ctx, F); JS_FreeValue(ctx, P); JS_FreeValue(ctx, fv); JS_FreeValue(ctx, cur); JS_FreeValue(ctx, nx); JS_FreeValue(ctx, gv);\n");
  fprintf(o, "  rg_ini_%s = (uint8_t)ok; rg_armed_%s = (uint8_t)ok;\n  return ok;\n}\n", h, h);
#undef RG_FAIL_
}

static void rg_emit_learners(RCtx *c) {
  if (getenv("QJS_AOT_REGION_VERIFY") != NULL) {
    fprintf(c->o, "static void rg_vf_%s(JSContext *ctx, JSValue v, const char *name, uint32_t off, int w, int pc, const char *fn)\n{\n", c->hex);
    fprintf(c->o, "  static int n_; const void *sh; uint32_t o2; JSAtom a; int ok;\n  if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT) { if (n_++ < 20) fprintf(stderr, \"verify: %%s pc %%d .%%s: receiver tag %%d\\n\", fn, pc, name, (int)JS_VALUE_GET_TAG(v)); return; }\n");
    fprintf(c->o, "  sh = *(const void *const *)((const char *)JS_VALUE_GET_PTR(v) + QJS_AOT_OFF_JSOBJECT_SHAPE);\n  a = JS_NewAtom(ctx, name);\n  ok = (w ? qjs_aot_shape_wslot : qjs_aot_shape_rslot)(sh, a, &o2); JS_FreeAtom(ctx, a);\n");
    fprintf(c->o, "  if ((!ok || o2 != off) && n_++ < 20) fprintf(stderr, \"verify: %%s pc %%d .%%s: expected off %%u, shape has %%s %%u\\n\", fn, pc, name, off, ok ? \"off\" : \"none\", o2);\n}\n");
  }

  if (rg_trust_on()) {
    /* the facts word of this region: bit 0 = every class layout and every method the dispatches resolved is as learned (patch 0432) */
    fprintf(c->o, "static uint64_t rg_fw_%s; static uint8_t rg_armed_%s;\n", c->hex, c->hex);
    /* one fact per (holder, name, kind): a re-learn of another shape of the same class chain adds none, so the word's dependents (and every commit's scan of them) stay few */
    fprintf(c->o, "static struct { void *o; uint32_t a; void *x; int k; } rg_fxc_%s[192]; static int rg_fxn_%s;\n", c->hex, c->hex);
    fprintf(c->o, "static int rg_fxh_%s(void *o, uint32_t a, void *x, int k) { int i; for (i = 0; i < rg_fxn_%s; i++) if (rg_fxc_%s[i].o == o && rg_fxc_%s[i].a == a && rg_fxc_%s[i].x == x && rg_fxc_%s[i].k == k) return 1; return 0; }\n", c->hex, c->hex, c->hex, c->hex, c->hex, c->hex);
    fprintf(c->o, "static void rg_fxa_%s(void *o, uint32_t a, void *x, int k) { if (rg_fxn_%s < 192) { rg_fxc_%s[rg_fxn_%s].o = o; rg_fxc_%s[rg_fxn_%s].a = a; rg_fxc_%s[rg_fxn_%s].x = x; rg_fxc_%s[rg_fxn_%s++].k = k; } }\n", c->hex, c->hex, c->hex, c->hex, c->hex, c->hex, c->hex, c->hex, c->hex, c->hex);
    fprintf(c->o, "static int rg_fx_%s(JSContext *ctx, JSValue recv, const char *mname, JSValue fv)\n{\n", c->hex);
    fprintf(c->o, "  JSValue cur = JS_GetPrototype(ctx, recv);\n  JSAtom a = JS_NewAtom(ctx, mname);\n  int ok = 0, i;\n");
    fprintf(c->o, "  for (i = 0; i < 8 && JS_IsObject(cur); i++) {\n    JSPropertyDescriptor d; int r = JS_GetOwnProperty(ctx, &d, cur, a);\n");
    fprintf(c->o, "    if (r > 0) { JSAotFact *g; JS_FreeValue(ctx, d.value); JS_FreeValue(ctx, d.getter); JS_FreeValue(ctx, d.setter);\n");
    fprintf(c->o, "      if (rg_fxh_%s(JS_VALUE_GET_PTR(cur), (uint32_t)a, JS_VALUE_GET_PTR(fv), 0)) { ok = 1; break; }\n", c->hex);
    fprintf(c->o, "      g = qjs_aot_fact_method(ctx, cur, a, fv, NULL); if (g != NULL) { qjs_aot_fact_depend64(g, &rg_fw_%s, 1); rg_fxa_%s(JS_VALUE_GET_PTR(cur), (uint32_t)a, JS_VALUE_GET_PTR(fv), 0); ok = 1; } else if (getenv(\"QJS_AOT_FASTSTATS\")) fprintf(stderr, \"fx: fact_method NULL %%s at depth %%d\\n\", mname, i); break; }\n", c->hex, c->hex);
    fprintf(c->o, "    if (r < 0) { if (getenv(\"QJS_AOT_FASTSTATS\")) fprintf(stderr, \"fx: getownprop error %%s depth %%d\\n\", mname, i); break; }\n");
    fprintf(c->o, "    if (rg_fxh_%s(JS_VALUE_GET_PTR(cur), (uint32_t)a, NULL, 1)) { JSValue nx = JS_GetPrototype(ctx, cur); JS_FreeValue(ctx, cur); cur = nx; continue; }\n", c->hex);
    fprintf(c->o, "    { JSValue nx = JS_GetPrototype(ctx, cur); JSAotFact *g = qjs_aot_fact_new(ctx, cur, JS_UNDEFINED, 0, NULL, 0);\n");
    fprintf(c->o, "      if (g == NULL || qjs_aot_fact_absent(ctx, g, a) != 0 || qjs_aot_fact_validate(ctx, g) != 1) { if (getenv(\"QJS_AOT_FASTSTATS\")) fprintf(stderr, \"fx: absent fact failed %%s depth %%d g=%%d state=%%d\\n\", mname, i, g != NULL, g ? qjs_aot_fact_state(g) : -9); JS_FreeValue(ctx, nx); break; }\n");
    fprintf(c->o, "      qjs_aot_fact_depend64(g, &rg_fw_%s, 1);\n", c->hex);
    fprintf(c->o, "      { JSAotFact *p = qjs_aot_fact_proto_is(ctx, cur, nx); if (p == NULL) { JS_FreeValue(ctx, nx); break; } qjs_aot_fact_depend64(p, &rg_fw_%s, 1); }\n", c->hex);
    fprintf(c->o, "      rg_fxa_%s(JS_VALUE_GET_PTR(cur), (uint32_t)a, NULL, 1);\n", c->hex);
    fprintf(c->o, "      JS_FreeValue(ctx, cur); cur = nx; }\n  }\n");
    fprintf(c->o, "  JS_FreeValue(ctx, cur); JS_FreeAtom(ctx, a);\n  return ok;\n}\n");
  }

  if (rg_trust_on() && c->ngl > 0) {
    int q;
    fprintf(c->o, "static JSValue rgk_%s[%d]; static JSObject *rgk_ho_%s[%d]; static JSValue rgkh_%s[%d]; static JSObject *rgkh_o_%s[%d]; static uint8_t rgk_ok_%s, rgk_try_%s;\n", c->hex, c->ngl, c->hex, c->ngl, c->hex, c->ngl, c->hex, c->ngl, c->hex, c->hex);
    fprintf(c->o, "static void rg_kfacts_%s(JSContext *ctx)\n{\n  JSValue gv_ = JS_GetGlobalObject(ctx), dv_, fv_; JSAtom ga_, fa_; JSAotFact *k_; int ok_ = 1; JSValue val_[%d], dvh_[%d];\n  if (rgk_ok_%s) return;\n  rgk_try_%s++;\n", c->hex, c->ngl, c->ngl, c->hex, c->hex);
    for (q = 0; q < c->ngl; q++) {
      fprintf(c->o, "  dv_ = JS_GetPropertyStr(ctx, gv_, "); rg_cstr(c->o, c->gl[q].g);
      if (c->gl[q].f[0]) { fprintf(c->o, "); fv_ = JS_IsObject(dv_) ? JS_GetPropertyStr(ctx, dv_, "); rg_cstr(c->o, c->gl[q].f); fprintf(c->o, ") : JS_UNDEFINED;\n"); }
      else fprintf(c->o, "); fv_ = JS_UNDEFINED;\n");
      fprintf(c->o, "  val_[%d] = fv_;\n  if (ok_ && JS_IsObject(dv_) && %s) {\n", q, c->gl[q].f[0] ? "JS_VALUE_GET_TAG(fv_) == JS_TAG_INT" : "1");
      fprintf(c->o, "    ga_ = JS_NewAtom(ctx, "); rg_cstr(c->o, c->gl[q].g); fprintf(c->o, "); fa_ = JS_NewAtom(ctx, "); rg_cstr(c->o, c->gl[q].f); fprintf(c->o, ");\n");
      fprintf(c->o, "    k_ = qjs_aot_fact_new(ctx, gv_, JS_UNDEFINED, 0, &ga_, 0); if (k_ != NULL && qjs_aot_fact_expect(ctx, k_, ga_, dv_) == 0 && qjs_aot_fact_validate(ctx, k_) == 1) qjs_aot_fact_depend64(k_, &rg_fw_%s, 1); else ok_ = 0;\n", c->hex);
      if (c->gl[q].f[0]) fprintf(c->o, "    k_ = qjs_aot_fact_new(ctx, dv_, JS_UNDEFINED, 0, &fa_, 0); if (k_ != NULL && qjs_aot_fact_expect(ctx, k_, fa_, fv_) == 0 && qjs_aot_fact_validate(ctx, k_) == 1) qjs_aot_fact_depend64(k_, &rg_fw_%s, 1); else ok_ = 0;\n", c->hex);
      fprintf(c->o, "    JS_FreeAtom(ctx, ga_); JS_FreeAtom(ctx, fa_);\n  } else ok_ = 0;\n  dvh_[%d] = dv_;\n", q);
    }
    fprintf(c->o, "  JS_FreeValue(ctx, gv_);\n  if (ok_ && qjs_aot_facts_commit(ctx, &rg_fw_%s, 1)) { ", c->hex);
    for (q = 0; q < c->ngl; q++) {
      fprintf(c->o, "rgk_%s[%d] = val_[%d]; ", c->hex, q, q);
      /* (the function and the object it is read from are borrowed from the facts, which hold them while the word is valid; the function an exit builds a frame
         from is the one a thru-call held at learn time, rg_tf. Holding these two as well from here made the runtime's teardown assertion fail: not understood) */
    }
    /* the objects the globals name (the register of the global read is part of an exit's frame state) are held too */
    for (q = 0; q < c->ngl; q++) fprintf(c->o, "rgkh_%s[%d] = dvh_[%d]; ", c->hex, q, q);
    fprintf(c->o, "if (ok_) { rgk_ok_%s = 1; rg_armed_%s = 1; } }\n", c->hex, c->hex);
    /* the values read are owned: kept only when they were bound (ints, no reference), released otherwise */
    for (q = 0; q < c->ngl; q++) fprintf(c->o, "  if (!rgk_ok_%s) JS_FreeValue(ctx, val_[%d]);\n", c->hex, q);
    for (q = 0; q < c->ngl; q++) fprintf(c->o, "  JS_FreeValue(ctx, dvh_[%d]);\n", q);
    fprintf(c->o, "}\n");
  }
  FILE *o = c->o;
  int gi, t, k;
  if (c->ncons > 0) {
    int ki;
    fprintf(o, "static int rg_data_%s(JSContext *ctx, JSValue obj, const char *name, JSValue *out)\n{\n  JSPropertyDescriptor d; JSAtom a; int r;\n", c->hex);
    fprintf(o, "  if (!JS_IsObject(obj)) return 0;\n  a = JS_NewAtom(ctx, name); r = JS_GetOwnProperty(ctx, &d, obj, a); JS_FreeAtom(ctx, a);\n  if (r <= 0) return 0;\n");
    fprintf(o, "  JS_FreeValue(ctx, d.getter); JS_FreeValue(ctx, d.setter);\n  if (d.flags & JS_PROP_GETSET) { JS_FreeValue(ctx, d.value); return 0; }\n  *out = d.value;\n  return 1;\n}\n");
    for (ki = 0; ki < c->ncons; ki++) {
      const RCons *K = &c->cons[ki];
      const char *h = c->hex;
      if (K->kind == 2) continue;
      int fs_ = getenv("QJS_AOT_FASTSTATS") != NULL, ni;
      fprintf(o, "static JSValue rgc_f_%s_%d; static JSAtom rgc_at_%s_%d[16]; static uint8_t rgc_ok_%s_%d, rgc_try_%s_%d;\n", h, ki, h, ki, h, ki, h, ki);
      fprintf(o, "static JSValue rgc_dv_%s_%d[16];   /* the prototype defaults a direct object starts with (primitives: no reference is held) */\n", h, ki);
      fprintf(o, "static int rg_clearn_%s_%d(JSContext *ctx)\n{\n", h, ki);
      fprintf(o, "  JSValue gv = JS_UNDEFINED, fv = JS_UNDEFINED, pv = JS_UNDEFINED, cur = JS_UNDEFINED, nx, hd[4] = { JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED }; JSAtom ga, pa; JSAotFact *k_; int ok = 0, i, depth;\n");
      fprintf(o, "  if (rgc_try_%s_%d >= 4) return 0;\n  rgc_try_%s_%d++;\n  gv = JS_GetGlobalObject(ctx);\n", h, ki, h, ki);
      fprintf(o, "  { int z_; for (z_ = 0; z_ < 16; z_++) rgc_dv_%s_%d[z_] = JS_UNDEFINED; }\n  uint32_t dvs_ = 0; (void)dvs_;\n", h, ki);
      {
        int np_ = K->npath > 0 ? K->npath : 1, li_;
        fprintf(o, "  hd[0] = gv;\n");
        for (li_ = 0; li_ < np_; li_++) {
          const char *dv_ = li_ == np_ - 1 ? "fv" : "nx";
          fprintf(o, "  if (!rg_data_%s(ctx, hd[%d], ", h, li_); rg_cstr(o, K->npath > 0 ? K->path[li_] : K->gname); fprintf(o, ", &%s) || JS_VALUE_GET_TAG(%s) != JS_TAG_OBJECT) goto out;\n", dv_, dv_);
          if (li_ < np_ - 1) fprintf(o, "  hd[%d] = nx;\n", li_ + 1);
        }
      }
      if (K->kind == 1) {
        /* the builtin Array: a C function constructor that is the `constructor` of the prototype every array has */
        fprintf(o, "  if (*(const uint16_t *)((const unsigned char *)JS_VALUE_GET_PTR(fv) + QJS_AOT_OFF_JSOBJECT_CLASS_ID) != QJS_AOT_CLASS_C_FUNCTION || !JS_IsConstructor(ctx, fv)) goto out;\n");
        fprintf(o, "  { JSValue ap_ = JS_NewArray(ctx), pp_ = JS_GetPrototype(ctx, ap_), cc_ = JS_UNDEFINED; int same_ = 0;\n");
        fprintf(o, "    if (JS_IsObject(pp_) && rg_data_%s(ctx, pp_, \"constructor\", &cc_)) same_ = JS_VALUE_GET_TAG(cc_) == JS_TAG_OBJECT && JS_VALUE_GET_PTR(cc_) == JS_VALUE_GET_PTR(fv);\n", h);
        fprintf(o, "    JS_FreeValue(ctx, cc_); JS_FreeValue(ctx, pp_); JS_FreeValue(ctx, ap_); if (!same_) goto out; }\n");
      } else {
        int bi;
        fprintf(o, "  { uint8_t dg_[16]; if (!qjs_aot_function_digest(fv, dg_) || ");
        for (bi = 0; bi < 16; bi++) fprintf(o, "%sdg_[%d] != 0x%02x", bi ? " || " : "", bi, K->tg->digest[bi]);
        fprintf(o, ") goto out; }\n");
        fprintf(o, "  if (!rg_data_%s(ctx, fv, \"prototype\", &pv) || !JS_IsObject(pv)) goto out;\n", h);
      }
      /* the global binding is the function */
      {
        int np_ = K->npath > 0 ? K->npath : 1, li_;
        for (li_ = 0; li_ < np_; li_++) {
          fprintf(o, "  ga = JS_NewAtom(ctx, "); rg_cstr(o, K->npath > 0 ? K->path[li_] : K->gname); fprintf(o, ");\n");
          fprintf(o, "  k_ = qjs_aot_fact_new(ctx, hd[%d], JS_UNDEFINED, 0, &ga, 0); i = k_ != NULL && qjs_aot_fact_expect(ctx, k_, ga, %s) == 0 && qjs_aot_fact_validate(ctx, k_) == 1;\n", li_, li_ == np_ - 1 ? "fv" : li_ == 0 ? "hd[1]" : li_ == 1 ? "hd[2]" : "hd[3]");
          fprintf(o, "  JS_FreeAtom(ctx, ga); if (!i) { %s goto out; } qjs_aot_fact_depend64(k_, &rg_fw_%s, 1);\n", fs_ ? "fprintf(stderr, \"region learn: path link fact\\n\");" : "", h);
        }
      }
      if (K->kind == 0) {
        /* F.prototype is the object learned, and the whole chain from it holds none of the fields the constructor adds */
        fprintf(o, "  pa = JS_NewAtom(ctx, \"prototype\");\n");
        fprintf(o, "  k_ = qjs_aot_fact_new(ctx, fv, JS_UNDEFINED, 0, &pa, 0); i = k_ != NULL && qjs_aot_fact_expect(ctx, k_, pa, pv) == 0 && qjs_aot_fact_validate(ctx, k_) == 1;\n");
        fprintf(o, "  JS_FreeAtom(ctx, pa); if (!i) { %s goto out; } qjs_aot_fact_depend64(k_, &rg_fw_%s, 1);\n", fs_ ? "fprintf(stderr, \"region learn: prototype fact\\n\");" : "", h);
        for (ni = 0; ni < K->nn; ni++) {
          fprintf(o, "  if (!rgc_at_%s_%d[%d]) { rgc_at_%s_%d[%d] = JS_NewAtom(ctx, ", h, ki, ni, h, ki, ni); rg_cstr(o, K->names[ni]); fprintf(o, "); }\n");
        }
        fprintf(o, "  cur = JS_DupValue(ctx, pv);\n  for (depth = 0; depth < 8 && JS_IsObject(cur); depth++) {\n");
        fprintf(o, "    k_ = qjs_aot_fact_new(ctx, cur, JS_UNDEFINED, 0, NULL, 0);\n    if (k_ == NULL) goto out;\n");
        for (ni = 0; ni < K->nn; ni++) {
          fprintf(o, "    { JSPropertyDescriptor d_; int r_ = JS_GetOwnProperty(ctx, &d_, cur, rgc_at_%s_%d[%d]);\n", h, ki, ni);
          /* a field the prototype chain holds as a WRITABLE DATA property (a default such as `isHit: false` in the prototype literal): the constructor's `this.f = v` defines an own property over it
             exactly as it would with nothing there, so it is allowed under a fact that the property keeps this value (a change, a redefinition or a delete breaks the word). Anything else on the chain
             (an accessor, a read-only property) refuses. Reads of the field before its store are refused by the walk (it would see the default, the region's slot starts undefined). */
          fprintf(o, "      if (r_ < 0) { JS_FreeValue(ctx, JS_GetException(ctx)); goto out; }\n");
          /* a default the generator did not provide a slot value for (a direct object) is refused; a primitive only (nothing to hold) */
          fprintf(o, "      if (r_ > 0) { int okp_ = !(d_.flags & JS_PROP_GETSET) && (d_.flags & JS_PROP_WRITABLE)%s;\n", !K->direct ? "" : (K->pre & (1u << ni)) ? " && !JS_VALUE_HAS_REF_COUNT(d_.value)" : " && 0");
          fprintf(o, "        if (okp_) { JSAotFact *e_ = qjs_aot_fact_new(ctx, cur, JS_UNDEFINED, 0, &rgc_at_%s_%d[%d], 0); okp_ = e_ != NULL && qjs_aot_fact_expect(ctx, e_, rgc_at_%s_%d[%d], d_.value) == 0 && qjs_aot_fact_validate(ctx, e_) == 1; if (okp_%s) qjs_aot_fact_depend64(e_, &rg_fw_%s, 1); }\n", h, ki, ni, h, ki, ni, getenv("QJS_AOT_REGION_PDNOFACT") ? " && 0" : "", h);
          if (K->direct && (K->pre & (1u << ni))) fprintf(o, "        if (okp_ && !(dvs_ & %uu)) { dvs_ |= %uu; rgc_dv_%s_%d[%d] = d_.value; }\n", 1u << ni, 1u << ni, h, ki, ni);
          fprintf(o, "        JS_FreeValue(ctx, d_.value); JS_FreeValue(ctx, d_.getter); JS_FreeValue(ctx, d_.setter);\n");
          fprintf(o, "        if (!okp_) { %s goto out; } }\n", fs_ ? "fprintf(stderr, \"region learn: a field on the prototype chain is not a plain default\\n\");" : "");
          fprintf(o, "      else if (qjs_aot_fact_absent(ctx, k_, rgc_at_%s_%d[%d]) != 0) goto out; }\n", h, ki, ni);
        }
        fprintf(o, "    if (qjs_aot_fact_validate(ctx, k_) != 1) goto out;\n    qjs_aot_fact_depend64(k_, &rg_fw_%s, 1);\n", h);
        fprintf(o, "    nx = JS_GetPrototype(ctx, cur);\n    { JSAotFact *p_ = qjs_aot_fact_proto_is(ctx, cur, nx); if (p_ == NULL) { JS_FreeValue(ctx, nx); goto out; } qjs_aot_fact_depend64(p_, &rg_fw_%s, 1); }\n", h);
        fprintf(o, "    JS_FreeValue(ctx, cur); cur = nx;\n  }\n  if (JS_IsObject(cur)) goto out;\n");
      }
      fprintf(o, "  if (!qjs_aot_facts_commit(ctx, &rg_fw_%s, 1)) { %s goto out; }\n", h, fs_ ? "fprintf(stderr, \"region learn: construct facts refused\\n\");" : "");
      /* the region holds no reference to the function: the global binding owns it, and the fact above breaks before that changes */
      fprintf(o, "  rgc_f_%s_%d = fv; rg_armed_%s = 1; rgc_ok_%s_%d = 1; ok = 1;\nout:\n", h, ki, h, h, ki);
      fprintf(o, "  JS_FreeValue(ctx, cur); JS_FreeValue(ctx, pv); JS_FreeValue(ctx, fv); JS_FreeValue(ctx, hd[1]); JS_FreeValue(ctx, hd[2]); JS_FreeValue(ctx, hd[3]); JS_FreeValue(ctx, gv);\n  (void)nx; (void)ga; (void)pa;\n  return ok;\n}\n");
    }
  }
  if (rg_trust_on()) { rg_emit_init(c); return; }
  for (gi = 0; gi < c->ngd; gi++) {
    RGuard *G = &c->gd[gi];
    char hx[8][33];
    fprintf(o, "static const void *rg_sh_%s_%d[4]; static uint8_t rg_tg_%s_%d[4]; static uint8_t rg_n_%s_%d, rg_bad_%s_%d; static uint32_t rg_off_%s_%d[4][%d];\n", c->hex, gi, c->hex, gi, c->hex, gi, c->hex, gi, c->hex, gi, RG_MAXQ * 2);
    if (G->dispatch) for (t = 0; t < G->ntg; t++) rg_hexof(G->tg[t], hx[t]);
    fprintf(o, "static uint8_t rg_sk_%s_%d;\n", c->hex, gi);
    if (G->dispatch && !G->intr) fprintf(o, "static JSObject *rg_fv_%s_%d[%d];\n", c->hex, gi, RG_MAXT);
    if (G->thru) {
      /* `X.call(thisArg, ...)`: the value X is compared with the function object learned for each target. The learner takes X once: its digest is a target's, the `call`
         found through X's prototype chain is the engine's Function.prototype.call, and the chain's facts (and X having no own `call`, X's prototype link) are stated in
         the word. X is held by the unit (q2_fh_hold, released at runtime teardown): an exit builds the callee's frame from it, possibly after the facts broke. */
      int fs_ = getenv("QJS_AOT_FASTSTATS") != NULL, bi;
      fprintf(o, "static JSObject *rg_tf_%s_%d[%d];\n", c->hex, gi, RG_MAXT);
      fprintf(o, "static int rg_learn_%s_%d(JSContext *ctx, JSValue x)\n{\n  uint8_t dg_[16]; int t = -1, ok; JSValue fv = JS_UNDEFINED, pr; JSAtom a; JSAotFact *g_, *p_;\n", c->hex, gi);
      fprintf(o, "  if (JS_VALUE_GET_TAG(x) != JS_TAG_OBJECT || rg_bad_%s_%d >= 8 || !qjs_aot_function_digest(x, dg_)) return -1;\n", c->hex, gi);
      for (t = 0; t < G->ntg; t++) {
        fprintf(o, "  %sif (", t ? "else " : "");
        for (bi = 0; bi < 16; bi++) fprintf(o, "%sdg_[%d] == 0x%02x", bi ? " && " : "", bi, G->tg[t]->digest[bi]);
        fprintf(o, ") t = %d;\n", t);
      }
      fprintf(o, "  if (t < 0 || rg_tf_%s_%d[t] != NULL || q2_phn >= Q2_PH_MAX) { rg_bad_%s_%d++; %s return -1; }\n", c->hex, gi, c->hex, gi, fs_ ? "fprintf(stderr, \"region learn: call target\\n\");" : "");
      /* (Function.prototype is a function object, which the shape walk does not accept: the engine's own property lookup, no code runs) */
      fprintf(o, "  a = JS_NewAtom(ctx, \"call\"); ok = 0; pr = JS_GetPrototype(ctx, x);\n");
      fprintf(o, "  if (JS_IsObject(pr)) { JSPropertyDescriptor d_; if (JS_GetOwnProperty(ctx, &d_, pr, a) > 0) { JS_FreeValue(ctx, d_.getter); JS_FreeValue(ctx, d_.setter); fv = d_.value; ok = !(d_.flags & JS_PROP_GETSET) && q2_is_fn_call(fv); } }\n  JS_FreeValue(ctx, pr);\n");
      fprintf(o, "  if (ok) { ok = rg_fx_%s(ctx, x, \"call\", fv);\n", c->hex);
      fprintf(o, "    if (ok) { g_ = qjs_aot_fact_new(ctx, x, JS_UNDEFINED, 0, NULL, 0); if (g_ != NULL && qjs_aot_fact_absent(ctx, g_, a) == 0 && qjs_aot_fact_validate(ctx, g_) == 1) qjs_aot_fact_depend64(g_, &rg_fw_%s, 1); else ok = 0; }\n", c->hex);
      fprintf(o, "    if (ok) { pr = JS_GetPrototype(ctx, x); p_ = qjs_aot_fact_proto_is(ctx, x, pr); JS_FreeValue(ctx, pr); if (p_ != NULL) qjs_aot_fact_depend64(p_, &rg_fw_%s, 1); else ok = 0; } }\n", c->hex);
      fprintf(o, "  JS_FreeAtom(ctx, a); JS_FreeValue(ctx, fv);\n");
      fprintf(o, "  if (!ok || !qjs_aot_facts_commit(ctx, &rg_fw_%s, 1) || (rg_tf_%s_%d[t] = q2_fh_hold(ctx, x, &rg_tf_%s_%d[t])) == NULL) { rg_bad_%s_%d++; %s return -1; }\n", c->hex, c->hex, gi, c->hex, gi, c->hex, gi, fs_ ? "fprintf(stderr, \"region learn: call facts\\n\");" : "");
      fprintf(o, "  rg_armed_%s = 1;\n  return t;\n}\n", c->hex);
      continue;
    }
    if (G->intr) {
      /* the push or pop builtin on an array: the method found through the array's prototype must be the builtin, and the chain that leads to it is stated as facts */
      int fs_ = getenv("QJS_AOT_FASTSTATS") != NULL, pop_ = strcmp(G->method, "pop") == 0;
      fprintf(o, "static JSValue rg_pf_%s, rg_pp_%s;\n", c->hex, c->hex);
      fprintf(o, "static int rg_learn_%s_%d(JSContext *ctx, JSValue recv)\n{\n  JSAtom a; JSValue fv = JS_UNDEFINED, pr; JSPropertyDescriptor d; int ok = 0;\n", c->hex, gi);
      fprintf(o, "  if (JS_VALUE_GET_TAG(recv) != JS_TAG_OBJECT || rg_bad_%s_%d >= 8) return -1;\n", c->hex, gi);
      /* the method is an own data property of the array's prototype (found without running any code) */
      fprintf(o, "  pr = JS_GetPrototype(ctx, recv); a = JS_NewAtom(ctx, \"%s\");\n", G->method);
      fprintf(o, "  if (JS_IsObject(pr) && JS_GetOwnProperty(ctx, &d, pr, a) > 0) { JS_FreeValue(ctx, d.getter); JS_FreeValue(ctx, d.setter); if (d.flags & JS_PROP_GETSET) JS_FreeValue(ctx, d.value); else { fv = d.value; ok = 1; } }\n");
      fprintf(o, "  JS_FreeAtom(ctx, a); JS_FreeValue(ctx, pr);\n");
      fprintf(o, "  if (!ok || !q2_builtin_is(fv, &%s, %d)) { JS_FreeValue(ctx, fv); rg_bad_%s_%d++; %s return -1; }\n", pop_ ? "e_arrpop_fn" : "e_arrpush_fn", pop_, c->hex, gi, fs_ ? "fprintf(stderr, \"region learn: the array method is not the builtin\\n\");" : "");
      fprintf(o, "  if (!rg_fx_%s(ctx, recv, \"%s\", fv) || !qjs_aot_facts_commit(ctx, &rg_fw_%s, 1)) { JS_FreeValue(ctx, fv); rg_bad_%s_%d++; %s return -1; }\n", c->hex, G->method, c->hex, c->hex, gi, fs_ ? "fprintf(stderr, \"region learn: array method facts refused\\n\");" : "");
      /* the helper compares the function it is handed with the engine's. The region holds no reference: Array.prototype owns the function and the fact
         above breaks, synchronously, before the property changes, and no entry reaches a push after that */
      fprintf(o, "  rg_p%c_%s = fv; JS_FreeValue(ctx, fv);\n  rg_armed_%s = 1;\n  return 0;\n}\n", pop_ ? 'p' : 'f', c->hex, c->hex);
      continue;
    }
    fprintf(o, "static int rg_learn_%s_%d(JSContext *ctx, JSValue recv)\n{\n", c->hex, gi);
    fprintf(o, "  const void *sh; uint32_t off; JSAtom a; int t = 0, ok = 1; JSValue fv = JS_UNDEFINED; (void)fv;\n");
    fprintf(o, "  if (JS_VALUE_GET_TAG(recv) != JS_TAG_OBJECT || rg_n_%s_%d >= 4 || rg_bad_%s_%d >= 8) { %s return -1; }\n", c->hex, gi, c->hex, gi, "");
    fprintf(o, "  sh = *(const void *const *)((const char *)JS_VALUE_GET_PTR(recv) + QJS_AOT_OFF_JSOBJECT_SHAPE);\n");
    if (G->dispatch) {
      fprintf(o, "  { a = JS_NewAtom(ctx, ");
      rg_cstr(o, G->method);
      fprintf(o, ");\n    ok = qjs_aot_shape_method(sh, a, &fv); JS_FreeAtom(ctx, a);\n    if (!ok) { rg_bad_%s_%d++; %s return -1; }\n", c->hex, gi, getenv("QJS_AOT_FASTSTATS") != NULL ? "fprintf(stderr, \"region learn: method lookup failed\\n\");" : "");
      if (G->cls_mode) {
        /* the receiver's class: its prototype is the one the class's constructor (a global) has; the method found must be that class's target */
        fprintf(o, "    { JSValue gv_ = JS_GetGlobalObject(ctx), pr_ = JS_GetPrototype(ctx, recv), c_, p_; uint8_t dg_[16]; t = -1;\n");
        for (t = 0; t < G->ntg; t++) {
          fprintf(o, "      c_ = JS_GetPropertyStr(ctx, gv_, ");
          rg_cstr(o, G->cls_ctor[t]);
          fprintf(o, "); p_ = JS_IsObject(c_) ? JS_GetPropertyStr(ctx, c_, \"prototype\") : JS_UNDEFINED;\n");
          fprintf(o, "      if (t < 0 && JS_IsObject(p_) && JS_IsObject(pr_) && JS_VALUE_GET_PTR(p_) == JS_VALUE_GET_PTR(pr_)) t = %d;\n", t);
          fprintf(o, "      JS_FreeValue(ctx, p_); JS_FreeValue(ctx, c_);\n");
        }
        fprintf(o, "      JS_FreeValue(ctx, gv_); JS_FreeValue(ctx, pr_);\n      if (t >= 0 && !qjs_aot_function_digest(fv, dg_)) t = -1;\n");
        fprintf(o, "      if (t >= 0) { int same_ = 0;\n");
        for (t = 0; t < G->ntg; t++) {
          int bi;
          fprintf(o, "        if (t == %d) same_ = ", t);
          for (bi = 0; bi < 16; bi++) fprintf(o, "%sdg_[%d] == 0x%02x", bi ? " && " : "", bi, G->tg[t]->digest[bi]);
          fprintf(o, ";\n");
        }
        fprintf(o, "        if (!same_) t = -1; } }\n");
      } else {
      fprintf(o, "    { uint8_t dg_[16]; t = -1;\n      if (qjs_aot_function_digest(fv, dg_)) {\n");
      for (t = 0; t < G->ntg; t++) {
        int bi;
        fprintf(o, "        %sif (", t ? "else " : "");
        for (bi = 0; bi < 16; bi++) fprintf(o, "%sdg_[%d] == 0x%02x", bi ? " && " : "", bi, G->tg[t]->digest[bi]);
        fprintf(o, ") t = %d;\n", t);
      }
      fprintf(o, "      } }\n");
      }
      fprintf(o, "    if (t < 0) { rg_bad_%s_%d++; %s return -1; } }\n", c->hex, gi, getenv("QJS_AOT_FASTSTATS") != NULL ? "fprintf(stderr, \"region learn: the method found is none of the targets\\n\");" : "");
    }
    /* The function object of the dispatch target, held at learn time: an exit inside the inlined callee builds the callee's frame from it, and by then the receiver
       may have another shape (the callee added a field), the method may have been replaced (the fact broke, the word is invalid and this very exit is the reaction,
       with the old function possibly freed by the replacing store). So the object is a counted reference of the unit (released at runtime teardown with the shape
       holds, q2_ph_release), kept per guard and target, first learned: callees with a variable area are refused, so the function object only identifies the frame. */
    if (G->dispatch) fprintf(o, "  if (rg_fv_%s_%d[t] == NULL) rg_fv_%s_%d[t] = q2_fh_hold(ctx, fv, &rg_fv_%s_%d[t]);\n", c->hex, gi, c->hex, gi, c->hex, gi);
    fprintf(o, "  switch (t) {\n");
    for (t = 0; t < (G->dispatch ? G->ntg : 1); t++) {
      int qn = 0;
      fprintf(o, "  case %d:\n", t);
      for (k = 0; k < G->ncommon; k++, qn++) {
        fprintf(o, "    a = JS_NewAtom(ctx, ");
        rg_cstr(o, G->common[k].name);
        fprintf(o, "); { int ok0_ = ok; ok = ok && %s(sh, a, &off); if (ok0_ && !ok && getenv(\"QJS_AOT_FASTSTATS\")) { const char *n_ = JS_AtomToCString(ctx, a); fprintf(stderr, \"region learn: field %%s not found (shape props %%d)\\n\", n_, (int)*(const int *)((const unsigned char *)sh + QJS_AOT_OFF_JSSHAPE_PROP_COUNT)); } } JS_FreeAtom(ctx, a); rg_off_%s_%d[rg_n_%s_%d][%d] = off;\n", G->common[k].write ? "qjs_aot_shape_wslot" : "qjs_aot_shape_rslot", c->hex, gi, c->hex, gi, qn);
      }
      for (k = 0; k < G->nreq[t]; k++, qn++) {
        fprintf(o, "    a = JS_NewAtom(ctx, ");
        rg_cstr(o, G->req[t][k].name);
        fprintf(o, "); { int ok0_ = ok; ok = ok && %s(sh, a, &off); if (ok0_ && !ok && getenv(\"QJS_AOT_FASTSTATS\")) { const char *n_ = JS_AtomToCString(ctx, a); fprintf(stderr, \"region learn: field %%s not found (shape props %%d)\\n\", n_, (int)*(const int *)((const unsigned char *)sh + QJS_AOT_OFF_JSSHAPE_PROP_COUNT)); } } JS_FreeAtom(ctx, a); rg_off_%s_%d[rg_n_%s_%d][%d] = off;\n", G->req[t][k].write ? "qjs_aot_shape_wslot" : "qjs_aot_shape_rslot", c->hex, gi, c->hex, gi, qn);
      }
      fprintf(o, "    break;\n");
    }
    fprintf(o, "  default: ok = 0;\n  }\n");
    fprintf(o, "  if (!ok || q2_phn >= Q2_PH_MAX) { rg_bad_%s_%d++; %s return -1; }\n", c->hex, gi, getenv("QJS_AOT_FASTSTATS") != NULL ? "fprintf(stderr, \"region learn: a field is not where required (t=%d ok=%d phn=%d)\\n\", t, ok, q2_phn);" : "");
    if (rg_trust_on()) {
      int fs = getenv("QJS_AOT_FASTSTATS") != NULL;
      fprintf(o, "  { unsigned id_ = *(const uint16_t *)((const unsigned char *)sh + QJS_AOT_OFF_SHAPE_LAYOUT_ID);\n");
      if (getenv("QJS_AOT_REGION_UNSOUND_NOLAYOUT") != NULL) fprintf(o, "    if (id_ == 0) goto nolayout_;\n");
      fprintf(o, "    if (id_ == 0 || id_ > 63) { rg_bad_%s_%d++; %s return -1; }\n", c->hex, gi, fs ? "fprintf(stderr, \"region learn: layout id %u (layouts %u, broken %llx, props %u/%u)\\n\", id_, (unsigned)Q2_LAYOUT_COUNT, (unsigned long long)Q2_LAYOUT_BROKEN(ctx), (unsigned)*(const uint16_t *)((const unsigned char *)sh + QJS_AOT_OFF_SHAPE_LAYOUT_ID), (unsigned)*(const int *)((const unsigned char *)sh + QJS_AOT_OFF_JSSHAPE_PROP_COUNT));" : "");
      fprintf(o, "    if (qjs_aot_layout_depend(ctx, id_, &rg_fw_%s, 1) != 1) { rg_bad_%s_%d++; %s return -1; }\n", c->hex, c->hex, gi, fs ? "fprintf(stderr, \"region learn: layout depend\\n\");" : "");
      if (getenv("QJS_AOT_REGION_UNSOUND_NOLAYOUT") != NULL) fprintf(o, "    nolayout_:;\n");
      if (G->dispatch) { fprintf(o, "    if (!rg_fx_%s(ctx, recv, ", c->hex); rg_cstr(o, G->method); fprintf(o, ", fv)) { rg_bad_%s_%d++; %s return -1; }\n", c->hex, gi, fs ? "fprintf(stderr, \"region learn: method chain\\n\");" : ""); if (fs) fprintf(o, "    /* guard %d: %s */\n", gi, G->method); }
      fprintf(o, "    if (!qjs_aot_facts_commit(ctx, &rg_fw_%s, 1)) { rg_bad_%s_%d++; %s return -1; }\n    rg_armed_%s = 1; }\n", c->hex, c->hex, gi, fs ? "fprintf(stderr, \"region learn: commit refused\\n\");" : "", c->hex);
    }
    fprintf(o, "  q2_ph[q2_phn++] = qjs_aot_shape_hold(sh);\n  rg_sh_%s_%d[rg_n_%s_%d] = sh; rg_tg_%s_%d[rg_n_%s_%d] = (uint8_t)t; return rg_n_%s_%d++;\n}\n", c->hex, gi, c->hex, gi, c->hex, gi, c->hex, gi, c->hex, gi);
    if (G->dispatch) {
      /* the function object a receiver dispatches to, found again from its shape when an exit has to build the callee's frame (cold; the receiver has passed the guard, so the method is there) */
      { char pre_[160] = "";
        if (getenv("QJS_AOT_REGION_REFIND") == NULL) snprintf(pre_, sizeof(pre_), "if (rg_fv_%s_%d[t] != NULL) return rg_fv_%s_%d[t];\n  ", c->hex, gi, c->hex, gi);
        fprintf(o, "static JSObject *rg_fo_%s_%d(JSContext *ctx, JSValue recv, int t)\n{\n  JSValue fv = JS_UNDEFINED; JSAtom a;\n  (void)t; %s", c->hex, gi, pre_);
        fprintf(o, "a = JS_NewAtom(ctx, "); }
      rg_cstr(o, G->method);
      fprintf(o, ");\n  int ok = JS_VALUE_GET_TAG(recv) == JS_TAG_OBJECT && qjs_aot_shape_method(*(const void *const *)((const char *)JS_VALUE_GET_PTR(recv) + QJS_AOT_OFF_JSOBJECT_SHAPE), a, &fv);\n");
      fprintf(o, "  JS_FreeAtom(ctx, a);\n  if (!ok || JS_VALUE_GET_TAG(fv) != JS_TAG_OBJECT) { fprintf(stderr, \"region %s: no method at an exit\\n\"); abort(); }\n  return (JSObject *)JS_VALUE_GET_PTR(fv);\n}\n", c->hex);
    }
  }
}

/* The dry pass (before the unit's layouts are decided): the first walk of every region, recording the constructors of the classes whose layout a
   region's facts word would depend on. Region mode makes contracted layouts only for those (a layout costs the per-site backend checks). */
static int g_rg_dry;
static char g_rg_need[256][48];
static char g_rg_needdg[64][33];   /* constructors named by a path (`NS.P`): their layouts are wanted by the constructor function's digest */
static int g_rg_nneeddg;
static int g_rg_nneed;
/* ---- stack objects: the escape analysis ------------------------------------------------------------------------------------------------------------
   A construct of the region is made in the region's C frame (not the heap) when its object never leaves the region and is not live when its site runs again.
   The object is followed through the instance tree (moves, the receivers and arguments of inlined calls, field reads and writes); the one thing it may hold
   that is also followed is the Array its constructor stores in a field (`this.elms = new Array()`), used only by push, pop, length and element reads.
   Anything else (stored as a value, compared, returned, an argument or receiver of an out-of-line or unresolved call, an operation the analysis does not know)
   is an escape and the object stays on the heap. */
static int g_sr_line;
static int sr_no(int line, int n) { if (!g_sr_line) g_sr_line = line; (void)n; return 0; }
typedef struct { RCtx *c; char arrname[40]; int arrinst, arrpc, arrfound; int fail, budget; int vis[256][3]; int nvis; } RSR;
static int sr_obj(RSR *s, int n, int v, int kind);
static int sr_has_use(const Q2Insn *in, int v, int *pos) {
  int j;
  for (j = 0; j < in->nuse; j++) if ((int)in->use[j] == v) { *pos = j; return 1; }
  for (j = 0; j < in->win_n && in->win_val != NULL; j++) if ((int)in->win_val[j] == v) { *pos = 100 + j; return 1; }
  return 0;
}
/* the uses of the receiver (`this`) of instance j */
static int sr_recv(RSR *s, int j, int kind) {
  RInst *A = s->c->in[j];
  int k;
  for (k = 0; k < A->q.ninsns; k++) {
    const Q2Insn *in = &A->q.insns[k];
    char nm[40];
    if (in->rop == ROP_load_this) { if (in->ndef < 1 || !sr_obj(s, j, (int)in->def[0], kind)) return sr_no(__LINE__, 0); }
    else if (in->rop == ROP_get_this_field || in->rop == ROP_get_this_field2) {
      if (!rg_atom(in, nm, sizeof(nm))) return sr_no(__LINE__, 0);
      if (in->rop == ROP_get_this_field2 && in->ndef >= 2 && !sr_obj(s, j, (int)in->def[1], kind)) return sr_no(__LINE__, 0);
      if (kind == 0 && s->arrfound && strcmp(nm, s->arrname) == 0) { if (in->ndef < 1 || !sr_obj(s, j, (int)in->def[0], 1)) return sr_no(__LINE__, 0); }
    }
  }
  return 1;
}
static int sr_arg(RSR *s, int j, int idx, int kind) {
  RInst *A = s->c->in[j];
  int k;
  for (k = 0; k < A->q.ninsns; k++) {
    const Q2Insn *in = &A->q.insns[k];
    if (in->rop != ROP_get_arg && in->rop != ROP_get_arg2) continue;
    if (in->narg >= 2 && (int)in->arg[1].val == idx && in->ndef >= 1 && !sr_obj(s, j, (int)in->def[0], kind)) return sr_no(__LINE__, 0);
    if (in->rop == ROP_get_arg2 && in->narg >= 4 && (int)in->arg[3].val == idx && in->ndef >= 2 && !sr_obj(s, j, (int)in->def[1], kind)) return sr_no(__LINE__, 0);
  }
  return 1;
}
static int sr_obj(RSR *s, int n, int v, int kind) {
  RCtx *c = s->c;
  RInst *A = c->in[n];
  int k;
  if (--s->budget < 0) return sr_no(__LINE__, 0);
  { int q; for (q = 0; q < s->nvis; q++) if (s->vis[q][0] == n && s->vis[q][1] == v && s->vis[q][2] == kind) return 1; if (s->nvis >= 256) return sr_no(__LINE__, 0); s->vis[s->nvis][0] = n; s->vis[s->nvis][1] = v; s->vis[s->nvis][2] = kind; s->nvis++; }   /* a value met again (a loop variable) is being checked already */
  if (kind == 1) { if (c->nskal < 64) { int q; for (q = 0; q < c->nskal && !(c->skal[q].inst == n && c->skal[q].v == v); q++) {} if (q == c->nskal) { c->skal[q].inst = n; c->skal[q].v = v; c->nskal++; } } else return sr_no(__LINE__, 0); }
  {
    /* the value passed along a block edge (a loop variable, a join) is the same object in the target block's parameter */
    const Q2Func *fn = &A->q;
    int b, sidx, j;
    for (b = 0; b < fn->nblocks; b++) {
      const Q2Block *B = &fn->blocks[b];
      if (fn->rpo_pos != NULL && fn->rpo_pos[b] < 0) continue;
      for (sidx = 0; sidx < B->nsucc; sidx++) {
        const Q2Block *T = &fn->blocks[B->succ_block[sidx]];
        for (j = 0; j < B->nedge_args[sidx] && j < T->nparams; j++)
          if ((int)B->edge_arg[sidx][j] == v && !sr_obj(s, n, (int)T->param_val[j], kind)) return sr_no(__LINE__, 0);
      }
    }
  }
  for (k = 0; k < A->q.ninsns; k++) {
    const Q2Insn *in = &A->q.insns[k];
    int pos;
    char nm[40];
    if (!sr_has_use(in, v, &pos)) continue;
    switch (in->rop) {
    case ROP_move: case ROP_move_s: case ROP_move_xfer:
      if (in->ndef < 1 || !sr_obj(s, n, (int)in->def[0], kind)) return sr_no(__LINE__, 0);
      break;
    case ROP_get_field: case ROP_get_field_own:
      if (pos != 0 || !rg_atom(in, nm, sizeof(nm))) return sr_no(__LINE__, 0);
      if (kind == 0 && s->arrfound && strcmp(nm, s->arrname) == 0) { if (in->ndef < 1 || !sr_obj(s, n, (int)in->def[0], 1)) return sr_no(__LINE__, 0); }
      break;
    case ROP_get_field2: case ROP_get_field2_own:
      if (pos != 0 || !rg_atom(in, nm, sizeof(nm)) || (kind == 0 && s->arrfound && strcmp(nm, s->arrname) == 0)) return sr_no(__LINE__, 0);
      if (in->ndef >= 2 && !sr_obj(s, n, (int)in->def[1], kind)) return sr_no(__LINE__, 0);
      break;
    case ROP_put_field: case ROP_put_field_xfer:
      if (!rg_atom(in, nm, sizeof(nm))) return sr_no(__LINE__, 0);
      if (pos == 0) {
        /* a store into the object: the Array its constructor makes is the one value that is followed */
        if (kind != 0 || in->nuse < 2) return sr_no(__LINE__, 0);
        {
          int vd = A->q.values[in->use[1]].insn;
          const Q2Insn *d = vd >= 0 && vd < A->q.ninsns ? &A->q.insns[vd] : NULL;
          if (d != NULL && d->rop == ROP_construct) {
            int ki = -1, q;
            for (q = 0; q < c->ncons; q++) if (c->cons[q].inst == A->id && c->cons[q].pc == d->pc) ki = q;
            if (ki >= 0 && c->cons[ki].kind == 1) {
              if (s->arrfound && (strcmp(s->arrname, nm) != 0 || s->arrinst != n || s->arrpc != d->pc)) return sr_no(__LINE__, 0);
              snprintf(s->arrname, sizeof(s->arrname), "%s", nm);
              s->arrinst = n; s->arrpc = d->pc; s->arrfound = 1;
            }
          }
        }
      } else if (pos == 1 && kind == 1 && s->arrfound && n == s->arrinst && strcmp(nm, s->arrname) == 0 && rg_is_this(&A->q, in->use[0])) {
        /* the constructor's own store of the Array into the object */
      } else return sr_no(__LINE__, 0);
      break;
    case ROP_get_length:
      if (kind != 1 || pos != 0) return sr_no(__LINE__, 0);
      break;
    case ROP_get_field_chain: case ROP_get_field_chain_own: {
      /* `this.elms.length`: the field that holds the Array, then its length */
      char n0[40] = "", n1[40] = "", n2[40] = "";
      if (pos != 0 || kind != 0 || !s->arrfound) return sr_no(__LINE__, 0);
      rg_atom_n(in, 0, n0, sizeof(n0)); rg_atom_n(in, 1, n1, sizeof(n1));
      if (rg_atom_n(in, 2, n2, sizeof(n2)) || strcmp(n0, s->arrname) != 0) return sr_no(__LINE__, 0);
      /* [arr field, X]: X read off the Array (its length, or the method a call goes through, whose receiver half is the Array) */
      /* def[0] is the first read's value (the Array), def[1] the second read's (the method called) */
      if (in->ndef >= 2) { if (!sr_obj(s, n, (int)in->def[0], 1)) return sr_no(__LINE__, 0); }
      else if (strcmp(n1, "length") != 0) return sr_no(__LINE__, 0);
      break;
    }
    case ROP_get_element:
      if (kind != 1 || pos != 0) return sr_no(__LINE__, 0);
      break;
    case ROP_put_local_s0:
      /* a forwarding constructor's copy of `this` into its variable slot 0: the slot is read back only by the wrapper's own method read, and an exit
         writes the slot through SKFIX (the migrated heap object) */
      if (!A->wrap || pos != 0 || getenv("QJS_AOT_REGION_NOWRAPSLOT") != NULL) { if (getenv("QJS_AOT_REGION_WHY") && !g_sr_line) { fprintf(stderr, "stackobj:   used by put_local_s0 at pc %d of instance %d (%s), kind %d pos %d\n", in->pc, n, A->g->name[0] ? A->g->name : "<anon>", kind, pos); } return sr_no(__LINE__, 0); }
      if (kind != 0) return sr_no(__LINE__, 0);
      break;
    case ROP_apply_arguments: {
      /* the wrapper's forward `this.m.apply(this, arguments)`: the object is the receiver (window slot 2) of the call the instance tree holds at this pc */
      int ch2[RG_MAXT], nch2 = 0, kk2, q2;
      if (!A->wrap || pos != 102 || kind != 0 || getenv("QJS_AOT_REGION_NOWRAPSLOT") != NULL) return sr_no(__LINE__, 0);
      for (kk2 = 1; kk2 < c->nin_total && nch2 < RG_MAXT; kk2++)
        if (c->in[kk2] != NULL && c->in[kk2]->par == n && c->in[kk2]->par_call_pc == in->pc && c->in[kk2]->par_kind != 1) ch2[nch2++] = kk2;
      if (nch2 == 0) return sr_no(__LINE__, 0);
      for (q2 = 0; q2 < nch2; q2++) {
        if (c->in[ch2[q2]]->ool) return sr_no(__LINE__, 0);
        if (!sr_recv(s, ch2[q2], kind)) return sr_no(__LINE__, 0);
      }
      break;
    }
    case ROP_call_method: case ROP_call: {
      int ch[RG_MAXT], nch = 0, kk, q;
      if (pos == 0) { if (getenv("QJS_AOT_REGION_WHY") && !g_sr_line) fprintf(stderr, "stackobj:   the value is a callee at pc %d of instance %d (%s) kind %d nuse %d use0=%d use1=%d v=%d\n", in->pc, n, A->g->name[0] ? A->g->name : "<anon>", kind, in->nuse, (int)in->use[0], in->nuse > 1 ? (int)in->use[1] : -1, v); return sr_no(__LINE__, 0); }
      for (kk = 1; kk < c->nin_total && nch < RG_MAXT; kk++)
        if (c->in[kk] != NULL && c->in[kk]->par == n && c->in[kk]->par_call_pc == in->pc && c->in[kk]->par_kind != 1) ch[nch++] = kk;
      if (nch == 0) {
        /* the builtin push / pop on the Array */
        int callee = in->nuse >= 1 ? rg_base(&A->q, in->use[0]) : -1;
        if (kind != 1 || pos != 1 || callee < 0 || callee >= A->q.nvalues || A->mname == NULL) return sr_no(__LINE__, 0);
        if (strcmp(A->mname[callee], "push") != 0 && strcmp(A->mname[callee], "pop") != 0) return sr_no(__LINE__, 0);
        break;
      }
      for (q = 0; q < nch; q++) {
        if (c->in[ch[q]]->ool) return sr_no(__LINE__, 0);
        if (pos == 1) { if (!sr_recv(s, ch[q], kind)) return sr_no(__LINE__, 0); }
        else if (!sr_arg(s, ch[q], pos - 100, kind)) return sr_no(__LINE__, 0);
      }
      break;
    }
    default:
      if (getenv("QJS_AOT_REGION_WHY") && !g_sr_line) { char rn[40]; e_rop_name(in->rop, rn, sizeof(rn)); fprintf(stderr, "stackobj:   used by %s at pc %d of instance %d (%s), kind %d pos %d\n", rn, in->pc, n, A->g->name[0] ? A->g->name : "<anon>", kind, pos); }
      return sr_no(__LINE__, 0);
    }
  }
  return 1;
}
/* The constructs that become stack objects: cons records of kind 0 (direct, complete) and the Array they hold. */
static void rg_sr_decide(RCtx *c) {
  int ki;
  if (!rg_trust_on() || getenv("QJS_AOT_REGION_NOSTACKOBJ") != NULL) return;
  for (ki = 0; ki < c->ncons; ki++) {
    RCons *K = &c->cons[ki];
    RSR s;
    RInst *I;
    int ii, ctor = -1, q, dv = -1, di, ok;
    if (getenv("QJS_AOT_REGION_WHY") && K->kind == 0) fprintf(stderr, "stackobj: root %.8s cons %d (inst %d pc %d) %s direct=%d esc=%d\n", c->hex, ki, K->inst, K->pc, K->gname, K->direct, K->esc);
    if (K->kind != 0 || !K->direct || K->esc || K->stack || c->nstack >= 1) continue;   /* one stack pair per region: the exit stubs share one pair of migration slots */
    ii = K->inst;
    if (ii < 0 || ii >= c->nin_total || c->in[ii] == NULL) continue;
    I = c->in[ii];
    di = rg_insn_at(&I->q, K->pc);
    if (di < 0 || I->q.insns[di].ndef < 1) continue;
    dv = (int)I->q.insns[di].def[0];
    for (q = 1; q < c->nin_total; q++) if (c->in[q] != NULL && c->in[q]->par == ii && c->in[q]->par_kind == 1 && c->in[q]->par_cons == ki) ctor = q;
    if (ctor < 0) continue;
    if ((I->ploop || I->curloop) && (getenv("QJS_AOT_REGION_PARKALL") != NULL || rg_carried_up(c, ii, dv))) { if (getenv("QJS_AOT_REGION_WHY")) fprintf(stderr, "stackobj: root %.8s cons %d carried over a loop edge\n", c->hex, ki); continue; }
    memset(&s, 0, sizeof(s));
    s.c = c;
    s.budget = 400;
    g_sr_line = 0;
    {
      int nskal0 = c->nskal;
      ok = sr_recv(&s, ctor, 0) && sr_obj(&s, ii, dv, 0);
      /* the Array: every use of the construct's own value in the constructor is the store and the pushes/pops that follow, all checked through the field reads above */
      if (ok && s.arrfound) {
        int ai = -1, ad, av;
        for (q = 0; q < c->ncons; q++) if (c->cons[q].inst == s.arrinst && c->cons[q].pc == s.arrpc) ai = q;
        ad = ai >= 0 ? rg_insn_at(&c->in[s.arrinst]->q, s.arrpc) : -1;
        if (ai < 0 || ad < 0 || c->in[s.arrinst]->q.insns[ad].ndef < 1) ok = 0;
        else {
          av = (int)c->in[s.arrinst]->q.insns[ad].def[0];
          ok = sr_obj(&s, s.arrinst, av, 1) && !(((c->in[s.arrinst]->ploop || c->in[s.arrinst]->curloop)) && rg_carried_up(c, s.arrinst, av));
          if (ok && c->cons[ai].stack == 0) { K->sk_pair = ai; c->cons[ai].sk_pair = ki; c->cons[ai].stack = 1; }
        }
      } else K->sk_pair = -1;
      if (!ok && getenv("QJS_AOT_REGION_WHY")) fprintf(stderr, "stackobj: root %.8s cons %d (inst %d pc %d) escapes (region.c:%d)\n", c->hex, ki, K->inst, K->pc, g_sr_line);
      if (!ok && getenv("QJS_AOT_REGION_SRNOESC") != NULL) {
        /* the vkit knock-out build: an object the analysis refuses is made a stack object anyway, but only when the run sets QJS_AOT_REGION_SRKNOCK (otherwise the construct leaves
           for the generic body), so the case's ordinary run is right and the knocked run shows what the escape rules protect */
        int ai = -1;
        for (q = 0; q < c->ncons; q++) if (s.arrfound && c->cons[q].inst == s.arrinst && c->cons[q].pc == s.arrpc) ai = q;
        K->sk_pair = ai;
        K->knock = 1;
        if (ai >= 0) { c->cons[ai].sk_pair = ki; c->cons[ai].stack = 1; c->cons[ai].knock = 1; }
        ok = 1;
      }
      if (!ok) { c->nskal = nskal0; if (K->sk_pair >= 0 && c->cons[K->sk_pair].stack) c->cons[K->sk_pair].stack = 0; K->sk_pair = -1; continue; }
    }
    K->stack = 1;
    c->nstack++;
  }
  if (getenv("QJS_AOT_REGION_WHY")) for (ki = 0; ki < c->ncons; ki++) if (c->cons[ki].stack) fprintf(stderr, "stackobj: root %.8s cons %d (inst %d pc %d) kind %d%s\n", c->hex, ki, c->cons[ki].inst, c->cons[ki].pc, c->cons[ki].kind, c->cons[ki].kind == 0 && c->cons[ki].sk_pair >= 0 ? " with its Array" : "");
}

int region_emit(FILE *f, const Q2Func *root, const char hex[33], int fi, char *why, size_t whyn);
void region_dry_collect(const Q2Func *root, const char hex[33], int fi) {
  char why[220];
  g_rg_dry = 1;
  region_emit(NULL, root, hex, fi, why, sizeof(why));
  g_rg_dry = 0;
}
int region_needs_ctor(const char *name) {
  int i;
  for (i = 0; i < g_rg_nneed; i++) if (strcmp(g_rg_need[i], name) == 0) return 1;
  return 0;
}
int region_dry_count(void) { return g_rg_nneed + g_rg_nneeddg; }
int region_needs_digest(const char *hex) {
  int i;
  for (i = 0; i < g_rg_nneeddg; i++) if (strcmp(g_rg_needdg[i], hex) == 0) return 1;
  return 0;
}

int region_layout_for(char (*names)[40], int nn);
int region_class_ambiguous(char (*names)[40], int nn);
static int region_emit1(FILE *f, const Q2Func *root, const char hex[33], int fi, char *why, size_t whyn, int loopinl) {
  RCtx *c;
  char (*rootcur)[40];
  int i, ok = 0, g;
  char *buf = NULL;
  size_t blen = 0;
  FILE *o;
  {
    /* bisection knobs: regions whose digest starts with ..._SKIP are refused, and with ..._ONLY set only those regions are made */
    const char *sk_ = getenv("QJS_AOT_REGION_SKIP"), *on_ = getenv("QJS_AOT_REGION_ONLY");
    if ((sk_ != NULL && *sk_ && strncmp(hex, sk_, strlen(sk_)) == 0) || (on_ != NULL && *on_ && strncmp(hex, on_, strlen(on_)) != 0)) {
      snprintf(why, whyn, "skipped by QJS_AOT_REGION_SKIP/ONLY");
      return 0;
    }
  }
  if (qjsaot_region_mode() == 0) { snprintf(why, whyn, "off (QJS_AOT_REGION=0)"); return 0; }
  g_meas_s = g_meas_d = g_meas_np = g_meas_nn = 0; g_meas_kind[0] = g_meas_kind[1] = g_meas_kind[2] = 0;
  c = calloc(1, sizeof(*c));
  g_rgm_c = c;
  rootcur = calloc(256, 40);
  if (c == NULL || rootcur == NULL) { snprintf(why, whyn, "no memory"); free(c); free(rootcur); return 0; }
  c->hex = hex;
  c->fi = fi;
  c->loopinl = loopinl;
  if (!root->ok || root->g->func_kind != 0 || root->g->ctl_count != 0) { snprintf(why, whyn, "not a plain function"); goto done; }
  {
    int has_loop = 0, b;
    for (b = 0; b < root->nblocks; b++) {
      int s;
      if (root->rpo_pos != NULL && root->rpo_pos[b] < 0) continue;
      for (s = 0; s < root->blocks[b].nsucc; s++)
        if (root->rpo_pos != NULL && root->rpo_pos[root->blocks[b].succ_block[s]] <= root->rpo_pos[b]) has_loop = 1;
    }
    if (!has_loop && !sel_is_root(fi)) { snprintf(why, whyn, "no loop and not a selected root"); goto done; }
    if (rg_trust_on() && getenv("QJS_AOT_REGION_VIRT") == NULL) {
      /* the ordinary body replaces some object of this function by its fields (a virtual object): a region would allocate it */
      int k_;
      for (k_ = 0; k_ < root->ninsns; k_++) if (root->insns[k_].vk != 0) { snprintf(why, whyn, "the ordinary body keeps an object virtual"); goto done; }
    }
    if (!has_loop && rg_trust_on()) {
      /* a recursion with no loop of its own: a region adds nothing to the ordinary compiled body (no loop to keep in registers), and its calls
         cost a C frame per level */
      int k_;
      for (k_ = 0; k_ < root->ninsns; k_++) {
        const Q2Insn *ci_ = &root->insns[k_];
        const JSAotGenFunc *tg_[4];
        int nt_, t_;
        if (ci_->rop != ROP_call && ci_->rop != ROP_call_method) continue;
        nt_ = qjs_aot_bundle_targets(root->g, ci_->pc, tg_, 4);
        for (t_ = 0; t_ < nt_; t_++) if (tg_[t_] == root->g) { snprintf(why, whyn, "a recursion with no loop (the ordinary body serves it)"); goto done; }
      }
    }
  }
  g_rgc = c;
  /* the root is instance 0; it is the caller's Q2Func (not owned) */
  rg_newinst(c, 0);
  c->in[0]->q = *root;
  c->in[0]->g = root->g;
  snprintf(c->in[0]->pfx, sizeof(c->in[0]->pfx), "r0_");
  c->in[0]->id = 0;
  c->in[0]->owner_g = -1;
  snprintf(c->in[0]->self, sizeof(c->in[0]->self), "f_this");
  c->nin = 1;
  c->emit = 0;
  if (!rg_walk_inst(c, 0, NULL, root)) { snprintf(why, whyn, "%s", c->why[0] ? c->why : "the region does not compile"); goto cleanup; }
  if (rg_trust_on()) {
    /* profitability: one C function holding thousands of inlined values compiles to spill-bound code (and minutes of compile); until a region can call a
       hot callee out of line, such a tree stays with the regions of its parts */
    int vt = 0, ia;
    for (ia = 0; ia < c->nin; ia++) if (!c->in[ia]->ool) vt += c->in[ia]->q.nvalues;
    if (vt > (getenv("QJS_AOT_REGION_MAXV") ? atoi(getenv("QJS_AOT_REGION_MAXV")) : 2500)) { snprintf(why, whyn, "too large: %d values in %d inlined instances (profitability)", vt, c->nin); goto cleanup; }
  }
  if (rg_trust_on() && getenv("QJS_AOT_REGION_NUMERIC") == NULL && !rg_any_construct(c) && !rg_any_ool(c) &&
      ((c->ncls == 0 && (c->nin == 1 || getenv("QJS_AOT_REGION_NOINLGATE") == NULL) && !rg_any_dispatch(c)) || (getenv("QJS_AOT_REGION_DBLGATE") != NULL && c->ngd == 0 && rg_stores_double(c)))) {   /* rule (b) is off by default (f64 S2): QJS_AOT_REGION_DBLGATE=1 brings it back */
    /* nothing a region does better than the ordinary body: no object is made, nothing is called out of line, and either nothing is inlined and no class is bound (a loop of
       numbers and arrays), or no call is resolved by class or method (no guard at all: plain calls of constant functions,
       field reads and writes of one known class, element reads, numbers), nothing is called out of line, and the work stores doubles. The ordinary body is typed and unboxed
       (a double stays in a register); the region would box every double it stores. */
    snprintf(why, whyn, "nothing for a region (monomorphic field, element and double work only: the ordinary body is typed and unboxed)%s", c->nin > 1 && c->ncls == 0 ? " [TEMPORARY gate until the unboxed-double work: numeric work after inlining; QJS_AOT_REGION_NOINLGATE=1 lifts it]" : "");
    goto cleanup;
  }
  if (g_rg_dry) {
    int k_, j_;
    for (k_ = 0; k_ < c->ncls; k_++) {
      if (!c->cls[k_].need_layout) continue;
      if (c->cls[k_].nn > 0 && region_class_ambiguous(c->cls[k_].names, c->cls[k_].nn)) continue;   /* no layout is asked for a class the region may not use */
      for (j_ = 0; j_ < g_rg_nneed && strcmp(g_rg_need[j_], c->cls[k_].ctor) != 0; j_++) {}
      if (j_ == g_rg_nneed && g_rg_nneed < 256) snprintf(g_rg_need[g_rg_nneed++], 48, "%s", c->cls[k_].ctor);
      if (strchr(c->cls[k_].ctor, '.') != NULL) {
        int sj_, dj_;
        for (sj_ = 0; sj_ < c->nsc; sj_++) if (c->sc_site[sj_] == c->cls[k_].site && c->sc_dg[sj_][0]) {
          for (dj_ = 0; dj_ < g_rg_nneeddg && strcmp(g_rg_needdg[dj_], c->sc_dg[sj_]) != 0; dj_++) {}
          if (dj_ == g_rg_nneeddg && g_rg_nneeddg < 64) snprintf(g_rg_needdg[g_rg_nneeddg++], 33, "%s", c->sc_dg[sj_]);
        }
      }
    }
    snprintf(why, whyn, "dry pass");
    goto cleanup;
  }
  {
    /* a class whose fixed-slot reads the word depends on needs a contracted layout in the unit; without one the init fails at run time and the root falls to the generic body (slower than no region) */
    int k_;
    for (k_ = 0; k_ < c->ncls; k_++) {
      int ok_ = c->cls[k_].nn > 0 ? region_layout_for(c->cls[k_].names, c->cls[k_].nn) : 1;
      if (getenv("QJS_AOT_REGION_WHY")) fprintf(stderr, "why: layout check root %.8s class %s need=%d nn=%d -> %d\n", hex, c->cls[k_].ctor, c->cls[k_].need_layout, c->cls[k_].nn, ok_);
      if (c->cls[k_].need_layout && !ok_ && getenv("QJS_AOT_REGION_NOLAYOUTCHECK") == NULL) {
        snprintf(why, whyn, "class %s has no usable contracted layout in the unit (its init would fail at run time and the generic body would run for good)", c->cls[k_].ctor);
        goto cleanup;
      }
    }
  }
  /* the second pass: the instances are created again in the same order */
  {
    c->nin_total = c->nin;
    c->nin = 1;
    rg_sr_decide(c);
    rg_ty_decide(c);
    if (c->nnative > 0 && getenv("QJS_AOT_REGION_NATGATE") != NULL) {
      int ia, va, nf = 0;
      for (ia = 0; ia < c->nin_total; ia++) if (c->in[ia] != NULL && c->in[ia]->ty != NULL) for (va = 0; va < c->in[ia]->q.nvalues; va++) if (c->in[ia]->ty[va] == RT_F64) nf++;
      if (nf > 0) { snprintf(why, whyn, "a typed (double) region calls %d native function(s) out of line [gate on by QJS_AOT_REGION_NATGATE=1: the temporary gate of f64 S2, off since Math inlining]", c->nnative); goto cleanup; }
    }
  }
  o = open_memstream(&buf, &blen);
  if (o == NULL) { snprintf(why, whyn, "no memory"); goto cleanup; }
  c->o = o;
  c->emit = 1;
  if (getenv("QJS_AOT_FASTSTATS") != NULL) {
    fprintf(o, "static unsigned long rgx_%s[%d], rgw_%s[4097];\n", hex, RG_MAXX + 1, hex);
    
    fprintf(o, "static int rgxpc_%s[%d];\n", hex, RG_MAXX + 1);
  }
  {
    /* the constant paths whose value the bundle shows (an int stored once): the code uses the int, the init checks the bound value is it */
    int q_;
    for (q_ = 0; q_ < c->ncp; q_++)
      c->cpk[q_] = c->cpn[q_] == 2 && getenv("QJS_AOT_REGION_NOCPK") == NULL && qjs_aot_bundle_const_int(c->cp[q_][0], c->cp[q_][1], &c->cpv[q_]);
    if (getenv("QJS_AOT_REGION_NOCPK") == NULL) rg_gfold(c);   /* F64: a global the profile saw hold one int is a literal too */
  }
  rg_emit_learners(c);
  if (c->nin_total > 1) {
    /* what an exit inside inlined code needs: the callees' generic bodies, their frame sizes, and the function that builds the chain of frames and runs it */
    int ia, ib;
    for (ia = 1; ia < c->nin_total; ia++) {
      char hx[33], hy[33];
      int dup = 0;
      rg_hexof(c->in[ia]->g, hx);
      for (ib = 1; ib < ia && !dup; ib++) { rg_hexof(c->in[ib]->g, hy); dup = strcmp(hx, hy) == 0; }
      if (dup) continue;
      fprintf(o, "%sJSValue aot_g_%s(JSContext *, JSRegRun *, JSValue *, uint32_t);\n#ifndef Q2_FR_%s\n#error \"region %s: the inlined function %s is not in the unit\"\n#endif\n", e_linkage(), hx, hx, hex, hx);
    }
    fprintf(o, "static int rg_pollp_%s;\n", hex);
    if (getenv("QJS_AOT_REGION_WRAPEXIT_BUILD") != NULL) fprintf(o, "static int rg_wrapexit_%s(void) { static int n = -1, k; if (n < 0) { const char *e = getenv(\"QJS_AOT_REGION_WRAPEXIT\"); n = e ? atoi(e) : 0; } return n > 0 && ++k %% n == 0; }\n", hex);
    fprintf(o, "static int rg_nof_%s(void) { static int v = -1; if (v < 0) v = getenv(\"QJS_AOT_REGION_NOFRAMES\") != NULL; return v; }\n", hex);
    fprintf(o, "#ifndef RGFC_T\n#define RGFC_T 1\n/* one frame of an exit's chain: the generic body to continue in, where, the register that takes the next frame's result, the frame size, and the registers handed over */\n");
    fprintf(o, "typedef struct RgFC { JSValue (*g)(JSContext *, JSRegRun *, JSValue *, uint32_t); uint32_t pc, cap_pc; uint16_t dst, slots, nlv, nargs; uint8_t ctor, pic, nvar, v0, v1; const uint16_t *offs; } RgFC;\n#endif\n");
    fprintf(o, "static JSValue rg_chain_%s(JSContext *ctx, JSRegRun *parent, const RgFC *fc, JSObject *const *cos, const JSValue *dv, int n)\n{\n", hex);
    fprintf(o, "  const RgFC *c = fc;\n  JSObject *co = cos[0];\n  uint8_t *cb = *(uint8_t **)((uint8_t *)co + QJS_AOT_OFF_OBJECT_FUNC_BYTECODE);\n");
    fprintf(o, "  JSContext *realm = *(JSContext **)(cb + QJS_AOT_OFF_FNBC_REALM);\n  uint8_t *rt = *(uint8_t **)((uint8_t *)ctx + QJS_AOT_OFF_CTX_RT);\n");
    fprintf(o, "  JSValue mem[c->slots] __attribute__((aligned(16)));\n  uint8_t *nf = (uint8_t *)mem;\n  JSValue *vb = mem + QJS_AOT_LIGHT_PREFIX_SLOTS;\n  JSValue *regs = vb + c->nvar;\n");
    fprintf(o, "  const JSValue *lv = dv + 1 + c->nargs;\n  JSRegRun *prev;\n  JSValue ret;\n  int k;\n");
    fprintf(o, "  if (c->pic && *(void **)(cb + QJS_AOT_OFF_FNBC_REGVM_PIC) == NULL) { if (qjs_aot_pic_ensure(ctx, co) < 0) return JS_EXCEPTION; }\n");
    fprintf(o, "  *(JSRegRun **)(nf + QJS_AOT_OFF_REGRUN_CALLER) = parent;\n  *(void **)(nf + QJS_AOT_OFF_REGRUN_RET_IP) = NULL;\n  *(void **)(nf + QJS_AOT_OFF_REGRUN_B) = cb;\n");
    fprintf(o, "  *(void **)(nf + QJS_AOT_OFF_REGRUN_PIC) = *(void **)(cb + QJS_AOT_OFF_FNBC_REGVM_PIC);\n  *(JSValue **)(nf + QJS_AOT_OFF_REGRUN_FR) = regs;\n");
    fprintf(o, "  *(void **)(nf + QJS_AOT_OFF_REGRUN_VAR_REFS) = *(void **)((uint8_t *)co + QJS_AOT_OFF_OBJECT_FUNC_VAR_REFS);\n  *(JSValue **)(nf + QJS_AOT_OFF_REGRUN_VAR_BUF) = c->nvar ? vb : NULL;\n");
    fprintf(o, "  *(JSValue **)(nf + QJS_AOT_OFF_REGRUN_ARG_BUF) = (JSValue *)(dv + 1);\n");
    fprintf(o, "  *(uint64_t *)(nf + QJS_AOT_OFF_REGRUN_HDR_PACK) = ((uint64_t)(uint16_t)c->nargs << 16) | ((uint64_t)(uint32_t)(QJS_AOT_REGVM_FLAG_LIGHT | QJS_AOT_REGVM_FLAG_C_ENTRY | QJS_AOT_REGVM_FLAG_STACK | (c->nvar ? 0 : QJS_AOT_REGVM_FLAG_TRIVIAL_RET) | (realm == ctx ? QJS_AOT_REGVM_FLAG_SAME_REALM : 0)) << 32);\n");
    fprintf(o, "  *(JSObject **)(nf + QJS_AOT_OFF_REGRUN_FUNC) = co;\n  *(void **)(nf + QJS_AOT_OFF_REGRUN_CAP_IP) = NULL;\n  *(void **)(nf + QJS_AOT_OFF_REGRUN_SF) = NULL;\n  *(JSValue *)(nf + QJS_AOT_OFF_REGRUN_THIS_OBJ) = dv[0];\n");
    fprintf(o, "  for (k = 0; k < (int)c->slots - QJS_AOT_LIGHT_PREFIX_SLOTS; k++) __builtin_memcpy(&vb[k], &e_undefined_value, sizeof(JSValue));\n");
    fprintf(o, "  if (c->v0) vb[0] = JS_DupValue(ctx, dv[0]);\n  if (c->v1) vb[1] = JS_UNINITIALIZED;\n");
    fprintf(o, "  for (k = 0; k < c->nlv; k++) AOT_SET_B(ctx, regs, c->offs[k], lv[k]);\n");
    fprintf(o, "  prev = *(JSRegRun **)(rt + QJS_AOT_OFF_RT_REGVM_CUR);\n  *(JSRegRun **)(rt + QJS_AOT_OFF_RT_REGVM_CUR) = (JSRegRun *)nf;\n");
    fprintf(o, "  if (n > 1) {\n    JSValue r;\n    *(const uint8_t **)(nf + QJS_AOT_OFF_REGRUN_CAP_IP) = *(const uint8_t *const *)(cb + QJS_AOT_OFF_FNBC_REGVM_CODE) + c->cap_pc;\n");
    fprintf(o, "    r = rg_chain_%s(ctx, (JSRegRun *)nf, fc + 1, cos + 1, dv + 1 + c->nargs + c->nlv, n - 1);\n", hex);
    fprintf(o, "    if (__builtin_expect(JS_IsException(r), 0)) { *(JSRegRun **)(rt + QJS_AOT_OFF_RT_REGVM_CUR) = prev; if (c->nvar) qjs_aot_light_release(realm, (JSRegRun *)nf); return r; }\n");
    fprintf(o, "    if (fc[1].ctor) { JS_FreeValue(ctx, r); AOT_SET_B(ctx, regs, c->dst, dv[1 + c->nargs + c->nlv]); }\n");
    fprintf(o, "    else if (JS_VALUE_HAS_REF_COUNT(r)) qjs_aot_set_reg_p(ctx, regs, c->dst, r);\n    else AOT_SET_B(ctx, regs, c->dst, r);\n  }\n");
    fprintf(o, "  if (n == 1 && __builtin_expect(rg_pollp_%s, 0)) { rg_pollp_%s = 0; if (qjs_aot_poll_fl(ctx) < 0) { *(JSRegRun **)(rt + QJS_AOT_OFF_RT_REGVM_CUR) = prev; if (c->nvar) qjs_aot_light_release(realm, (JSRegRun *)nf); return JS_EXCEPTION; } }\n  ret = c->g(realm, (JSRegRun *)nf, regs, c->pc);\n  *(JSRegRun **)(rt + QJS_AOT_OFF_RT_REGVM_CUR) = prev;\n  if (c->nvar) qjs_aot_light_release(realm, (JSRegRun *)nf);\n  return ret;\n}\n", hex, hex);
  }
  if (getenv("QJS_AOT_FASTSTATS") != NULL) {
    int gg;
    fprintf(o, "static void __attribute__((destructor)) rg_stat_print_%s(void) { int i; for (i = 0; i < %d; i++) if (rgx_%s[i]) fprintf(stderr, \"region %s exit %%d at pc %%d: %%lu\\n\", i, rgxpc_%s[i], rgx_%s[i]);\n", hex, RG_MAXX, hex, hex, hex, hex);
    fprintf(o, "  fprintf(stderr, \"region %s entered %%lu\\n\", rgx_%s[%d]);\n", hex, hex, RG_MAXX);
    for (gg = 0; gg < c->ngd && !rg_trust_on(); gg++) fprintf(o, "  fprintf(stderr, \"region %s guard %d learned=%%d bad=%%d\\n\", rg_n_%s_%d, rg_bad_%s_%d);\n", hex, gg, hex, gg, hex, gg);
    fprintf(o, "}\n");
  }
  fprintf(o, "#ifndef RG_OWN_T\n#define RG_OWN_T 1\n/* the references an out-of-line call makes the region hold until it returns or leaves */\n");
  /* the list starts in the region's stack buffer (a negative capacity) and moves to the heap when it fills */
  fprintf(o, "static __attribute__((noinline)) void rg_own_grow(JSValue **p, int *cap) { int oc = *cap < 0 ? -*cap : *cap, nc = oc * 2; JSValue *q = *cap < 0 ? malloc((size_t)nc * sizeof(JSValue)) : realloc(*p, (size_t)nc * sizeof(JSValue)); if (q == NULL) abort(); if (*cap < 0) memcpy(q, *p, (size_t)oc * sizeof(JSValue)); *p = q; *cap = nc; }\n");
  fprintf(o, "static inline __attribute__((always_inline)) JSValue rg_numnorm(JSValue v)\n{\n  if (JS_VALUE_GET_TAG(v) == JS_TAG_FLOAT64) { double d = JS_VALUE_GET_FLOAT64(v); int32_t i = (int32_t)d; if (d == (double)i && !(i == 0 && __builtin_signbit(d))) return JS_MKVAL(JS_TAG_INT, i); }\n  return v;\n}\n");
  fprintf(o, "static inline void rg_own_put(JSContext *ctx, JSValue **p, int *n, int *cap, JSValue v) { (void)ctx; if (!JS_VALUE_HAS_REF_COUNT(v)) return; if (*n >= (*cap < 0 ? -*cap : *cap)) rg_own_grow(p, cap); (*p)[(*n)++] = v; }\n");
  fprintf(o, "static inline void rg_own_dup(JSContext *ctx, JSValue **p, int *n, int *cap, JSValue v) { if (!JS_VALUE_HAS_REF_COUNT(v)) return; q2_dup_p(v); rg_own_put(ctx, p, n, cap, v); }\n");
  if (getenv("QJS_AOT_REGION_OWNSTAT") != NULL) fprintf(o, "static unsigned long q2_os_[4096]; __attribute__((destructor)) static void q2_os_dump(void) { int i; for (i = 0; i < 4096; i++) if (q2_os_[i]) fprintf(stderr, \"OWNRUN site %%d: %%lu\\n\", i, q2_os_[i]); }\n");
  fprintf(o, "static inline void rg_own_compact(JSContext *ctx, JSValue *p, int *n, int mark, const JSValue *keep, int nk) { int i, w = mark; for (i = mark; i < *n; i++) { int k, kept = 0; for (k = 0; k < nk; k++) if (JS_VALUE_GET_TAG(keep[k]) == JS_VALUE_GET_TAG(p[i]) && JS_VALUE_GET_PTR(keep[k]) == JS_VALUE_GET_PTR(p[i])) { kept = 1; break; } if (kept) p[w++] = p[i]; else JS_FreeValue(ctx, p[i]); } *n = w; }\n");
  fprintf(o, "static inline void rg_own_free(JSContext *ctx, JSValue *p, int n, int heap) { int i; for (i = 0; i < n; i++) JS_FreeValue(ctx, p[i]); if (heap) free(p); }\n#endif\n");
  fprintf(o, "#ifndef RG_EQV_T\n#define RG_EQV_T 1\n/* a == b (strict: ===) when the tags decide it with no code run: 1 with the answer, 0 to leave */\n");
  fprintf(o, "static inline __attribute__((always_inline)) int rg_eqv(JSValue a, JSValue b, int strict, int *out)\n{\n  int ta = JS_VALUE_GET_TAG(a), tb = JS_VALUE_GET_TAG(b);\n");
  fprintf(o, "  int na = ta == JS_TAG_NULL || ta == JS_TAG_UNDEFINED, nb = tb == JS_TAG_NULL || tb == JS_TAG_UNDEFINED;\n");
  fprintf(o, "  if ((ta == JS_TAG_INT || ta == JS_TAG_FLOAT64) && (tb == JS_TAG_INT || tb == JS_TAG_FLOAT64)) { double x_ = ta == JS_TAG_INT ? (double)JS_VALUE_GET_INT(a) : JS_VALUE_GET_FLOAT64(a), y_ = tb == JS_TAG_INT ? (double)JS_VALUE_GET_INT(b) : JS_VALUE_GET_FLOAT64(b); *out = x_ == y_; return 1; }\n");
  fprintf(o, "  if (ta == tb) { if (ta == JS_TAG_INT || ta == JS_TAG_BOOL) { *out = JS_VALUE_GET_INT(a) == JS_VALUE_GET_INT(b); return 1; } if (ta == JS_TAG_OBJECT) { *out = JS_VALUE_GET_PTR(a) == JS_VALUE_GET_PTR(b); return 1; } if (na) { *out = 1; return 1; } return 0; }\n");
  fprintf(o, "  if (na && nb) { *out = !strict; return 1; }\n");
  fprintf(o, "  if (na || nb) { if ((na ? tb : ta) == JS_TAG_INT || (na ? tb : ta) == JS_TAG_BOOL || (na ? tb : ta) == JS_TAG_OBJECT) { *out = 0; return 1; } return 0; }\n");
  fprintf(o, "  if ((ta == JS_TAG_INT && tb == JS_TAG_BOOL) || (ta == JS_TAG_BOOL && tb == JS_TAG_INT)) { *out = strict ? 0 : JS_VALUE_GET_INT(a) == JS_VALUE_GET_INT(b); return 1; }\n");
  fprintf(o, "  if (strict && (ta == JS_TAG_OBJECT || tb == JS_TAG_OBJECT) && (ta == JS_TAG_INT || ta == JS_TAG_BOOL || tb == JS_TAG_INT || tb == JS_TAG_BOOL || ta == JS_TAG_OBJECT || tb == JS_TAG_OBJECT)) { *out = 0; return 1; }\n");
  fprintf(o, "  return 0;\n}\n#endif\n");
  fprintf(o, "#ifndef RG_PUTEL_T\n#define RG_PUTEL_T 1\n/* a[i] = v on a plain fast array: 1 when stored (*old is the replaced counted value, or undefined), 0 when nothing was done */\n");
  fprintf(o, "static inline __attribute__((always_inline)) int rg_put_elem(JSContext *ctx, JSValue arr, JSValue k, JSValue v, JSValue *old)\n{\n");
  fprintf(o, "  unsigned char *p; uint32_t i, count; JSValue *slot, *lenp;\n");
  fprintf(o, "  if (JS_VALUE_GET_TAG(arr) != JS_TAG_OBJECT || JS_VALUE_GET_TAG(k) != JS_TAG_INT) return 0;\n");
  fprintf(o, "  p = (unsigned char *)JS_VALUE_GET_PTR(arr);\n");
  fprintf(o, "  if (*(const uint16_t *)(p + QJS_AOT_OFF_JSOBJECT_CLASS_ID) != QJS_AOT_CLASS_ARRAY || !(p[QJS_AOT_OFF_JSOBJECT_FAST_ARRAY_BYTE] & QJS_AOT_JSOBJECT_FAST_ARRAY_MASK) || QJS_AOT_ARR_ODD(p) || JS_VALUE_GET_TAG(v) == QJS_AOT_TAG_LAZY_JSON) return 0;\n");
  fprintf(o, "  i = (uint32_t)JS_VALUE_GET_INT(k);\n  count = *(const uint32_t *)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_COUNT);\n");
  fprintf(o, "  if (__builtin_expect(i < count, 1)) {\n    slot = &(*(JSValue **)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_VALUES))[i];\n");
  fprintf(o, "    if (JS_VALUE_GET_TAG(*slot) == JS_TAG_UNINITIALIZED) return 0;\n    *old = *slot; q2_dup_p(v); *slot = v; return 1;\n  }\n");
  fprintf(o, "  if (i != count || !q2_array_plain(ctx, p, &count, &lenp) || count >= 0x7fffffffu) return 0;\n");
  fprintf(o, "  *old = JS_UNDEFINED;\n");
  fprintf(o, "  if (count + 1 > *(const uint32_t *)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_SIZE)) { JSValue o_; return qjs_aot_array_push_slow(ctx, arr, v, &o_); }\n");
  fprintf(o, "  q2_dup_p(v);\n  (*(JSValue **)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_VALUES))[count] = v;\n  *(uint32_t *)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_COUNT) = count + 1;\n");
  fprintf(o, "  *lenp = JS_MKVAL(JS_TAG_INT, (int32_t)(count + 1));\n  return 1;\n}\n#endif\n");
  /* function header, entry gate (as the speculative body's) */
  fprintf(o, "E_TEXT %sJSValue aot_f_%s(JSContext *ctx, JSRegRun *f, JSValue *fr, uint32_t entry_pc)\n{\n", e_linkage(), hex);
  fprintf(o, "    if (__builtin_expect(e_gate[%d] != 0 || entry_pc != 0, 0)) return aot_g_%s(ctx, f, fr, entry_pc);\n", fi, hex);
  fprintf(o, "    _Static_assert(%d < QJS_AOT_NFUNCS, \"function index\");\n    E_SEAL(ctx, f);\n", fi);
  fprintf(o, "    {\n    JSValue *f_argv = *(JSValue **)((uint8_t *)f + QJS_AOT_OFF_REGRUN_ARG_BUF);\n");
  fprintf(o, "    int f_argc = (int)((*(uint64_t *)((uint8_t *)f + QJS_AOT_OFF_REGRUN_HDR_PACK) >> 16) & 0xffffu);\n");
  fprintf(o, "    JSValue f_this = *(JSValue *)((uint8_t *)f + QJS_AOT_OFF_REGRUN_THIS_OBJ);\n");
  fprintf(o, "    (void)f_argv; (void)f_argc;\n");
  {
    /* a root that reads `this` takes it as an object (a plain call's `this` is coerced by the generic body); one that never reads it does not care */
    int ti_, uses_this_ = 0;
    for (ti_ = 0; ti_ < root->ninsns && !uses_this_; ti_++)
      if (root->insns[ti_].rop == ROP_load_this || root->insns[ti_].rop == ROP_get_this_field || root->insns[ti_].rop == ROP_get_this_field2) uses_this_ = 1;
    fprintf(o, "    if (__builtin_expect(E_LAZY_SEEN(ctx), 0)%s) return aot_g_%s(ctx, f, fr, entry_pc);\n", uses_this_ ? " || JS_VALUE_GET_TAG(f_this) != JS_TAG_OBJECT" : "", hex);
  }
  fprintf(o, "    E_ENTER_NOTE(%d);\n", fi);
  if (getenv("QJS_AOT_FASTSTATS") != NULL) fprintf(o, "    rgx_%s[%d]++;\n", hex, RG_MAXX);
  if (rg_trust_on()) {
    /* TRUST: the region's facts are bound once (rg_init, a few tries if a global is not defined yet); after that the word is the only test */
    fprintf(o, "    if (__builtin_expect(!rg_ini_%s, 0)) { if (rg_itry_%s >= 4) { rg_byp_%s++; return aot_fs_%s(ctx, f, fr, entry_pc); } rg_itry_%s++; if (!rg_init_%s(ctx)) { rg_ifail_%s++; rg_byp_%s++; return aot_fs_%s(ctx, f, fr, entry_pc); } }\n", hex, hex, hex, hex, hex, hex, hex, hex, hex);
    fprintf(o, "    if (__builtin_expect(!QJS_AOT_FACTS_OK(&rg_fw_%s, 1), 0)) { rg_byp_%s++; return aot_fs_%s(ctx, f, fr, entry_pc); }\n", hex, hex, hex);
    if (c->in[0]->kcls > 0)
      fprintf(o, "    if (__builtin_expect(JS_VALUE_GET_TAG(f_this) != JS_TAG_OBJECT || *(const void *const *)((const char *)JS_VALUE_GET_PTR(f_this) + QJS_AOT_OFF_JSOBJECT_SHAPE) != rk_s_%s[%d], 0)) return aot_g_%s(ctx, f, fr, entry_pc);\n", hex, c->in[0]->kcls - 1, hex);
    for (g = 0; g < c->ngl; g++) fprintf(o, "    JSValue rgc_%d = rgk_%s[%d];\n", g, hex, g);
    fprintf(o, "/*@RGM@*/\n");
  } else {
  /* the learned tables are good for one prototype epoch */
  fprintf(o, "    { static uint32_t rg_ep = ~0u; uint32_t ep_ = *(const uint32_t *)(*(const uint8_t *const *)((const uint8_t *)ctx + QJS_AOT_OFF_CTX_RT) + QJS_AOT_OFF_RT_PIC_EPOCH);\n");
  /* a vkit knob: the learned tables survive prototype epoch moves, so only the facts word can catch a change to what they learned */
  /* TRUST: a learned shape is held (immutable) and what depends on the prototypes is the facts word's, so an epoch move forgets nothing; resetting re-learned and re-held shapes until the hold table was full */
  if (getenv("QJS_AOT_REGION_NOEPOCH") != NULL || rg_trust_on()) fprintf(o, "      if (rg_ep == ~0u) rg_ep = ep_;\n");
  fprintf(o, "      if (__builtin_expect(ep_ != rg_ep, 0)) { rg_ep = ep_;");
  for (g = 0; g < c->ngd; g++) fprintf(o, " rg_n_%s_%d = 0; rg_bad_%s_%d = 0; memset(rg_sh_%s_%d, 0, sizeof(rg_sh_%s_%d));", hex, g, hex, g, hex, g, hex, g);
  fprintf(o, " } }\n");
  if (rg_trust_on()) fprintf(o, "    if (rg_armed_%s && __builtin_expect(!QJS_AOT_FACTS_OK(&rg_fw_%s, 1), 0)) return aot_g_%s(ctx, f, fr, entry_pc);\n", hex, hex, hex);
  /* the functions the region's constructs name, learned (with their facts) before anything is borrowed */
  for (g = 0; g < c->ncons; g++) if (c->cons[g].kind != 2) fprintf(o, "    if (__builtin_expect(!rgc_ok_%s_%d, 0) && !rg_clearn_%s_%d(ctx)) return aot_g_%s(ctx, f, fr, entry_pc);\n", hex, g, hex, g, hex);
  /* the globals the region reads as constants */
  for (g = 0; g < c->ngl; g++) fprintf(o, "    JSValue rgc_%d;\n", g);
  if (c->ngl > 0 && rg_trust_on()) {
    /* armed: the constants were read once and each is held by facts of the word (the global's binding, the holder's property), so no lookup per entry */
    fprintf(o, "    if (rg_armed_%s && rgk_ok_%s) {", hex, hex);
    for (g = 0; g < c->ngl; g++) fprintf(o, " rgc_%d = rgk_%s[%d];", g, hex, g);
    fprintf(o, " } else {\n");
  }
  for (g = 0; g < c->ngl; g++) {
    if (!c->gl[g].f[0]) {
      /* a function constant is only used from the facts' table: learn it (once, a few tries) or run the generic body */
      fprintf(o, "    { if (!rgk_ok_%s && rgk_try_%s < 4) rg_kfacts_%s(ctx); if (!rgk_ok_%s) return aot_g_%s(ctx, f, fr, entry_pc); rgc_%d = rgk_%s[%d]; }\n", hex, hex, hex, hex, hex, g, hex, g);
      continue;
    }
    fprintf(o, "    { JSValue gv_ = JS_GetGlobalObject(ctx), dv_, fv_;\n      dv_ = JS_GetPropertyStr(ctx, gv_, ");
    rg_cstr(o, c->gl[g].g);
    fprintf(o, "); fv_ = JS_IsObject(dv_) ? JS_GetPropertyStr(ctx, dv_, ");
    rg_cstr(o, c->gl[g].f);
    fprintf(o, ") : JS_UNDEFINED;\n      JS_FreeValue(ctx, gv_); JS_FreeValue(ctx, dv_);\n      if (JS_VALUE_GET_TAG(fv_) != JS_TAG_INT) { JS_FreeValue(ctx, fv_); return aot_g_%s(ctx, f, fr, entry_pc); }\n      rgc_%d = fv_; }\n", hex, g);
  }
  if (c->ngl > 0 && rg_trust_on()) fprintf(o, "    if (rg_armed_%s && rgk_try_%s < 4) rg_kfacts_%s(ctx);\n    }\n", hex, hex, hex);
  }
  for (g = 0; g < c->ncons; g++) fprintf(o, "    JSValue rn_%d = JS_UNDEFINED;\n", g);
  fprintf(o, "    JSValue sk_ho = JS_UNDEFINED, sk_ha = JS_UNDEFINED; (void)sk_ho; (void)sk_ha;\n");
  for (g = 0; g < c->ncons; g++)
    if (c->cons[g].stack) {
      /* a block of the frame: [JSMallocBlockHeader][body]; the object sits 8 bytes in. Its references are counted ones held by native code (the collector's trial decrement leaves them positive: roots). */
      fprintf(o, "    _Alignas(16) uint64_t skb_%d[80]; int sk_%d = 0; JSValue rnh_%d = JS_UNDEFINED; (void)skb_%d;\n#define SKV_%d JS_MKPTR(JS_TAG_OBJECT, (void *)((uint8_t *)skb_%d + 8))\n", g, g, g, g, g, g);
    }
  {
    /* SKFIX(x): x, or the heap object a migration made when x is a stack object (only meaningful after rg_sk_migrate) */
    int any_ = 0;
    /* variadic: the argument may be a compound literal (JS_UNDEFINED) whose commas the preprocessor would split */
    fprintf(o, "#define SKFIX(...) ({ JSValue skx_ = (__VA_ARGS__); (");
    for (g = 0; g < c->ncons; g++)
      if (c->cons[g].stack) {
        fprintf(o, "(JS_VALUE_GET_TAG(skx_) == JS_TAG_OBJECT && JS_VALUE_GET_PTR(skx_) == JS_VALUE_GET_PTR(SKV_%d)) ? %s : ", g, c->cons[g].kind == 0 ? "sk_ho" : "sk_ha");
        any_ = 1;
      }
    fprintf(o, "skx_); })\n");
    (void)any_;
    fprintf(o, "#define RG_PM(R_, I_) ({ JSValue pmr_ = JS_GetPropertyStr(ctx, SKFIX(R_), rg_pmn_[I_]); JS_FreeValue(ctx, pmr_); pmr_; })\n");
  }
  fprintf(o, "    JSValue rgo_b_[64]; JSValue *rgo_ = rgo_b_; int rgon_ = 0, rgoc_ = -64; (void)rgon_; (void)rgoc_;\n");
  fprintf(o, "    int rgmk_[48] = {0}; (void)rgmk_;\n");
  /* the owned list is bounded: past this many entries a site that parked one leaves the region through its ordinary exit (the frame state takes what it needs, the list is freed, the
     generic body goes on). QJS_AOT_REGION_OWNCAP=n sets the bound; QJS_AOT_REGION_NOOWNCAP removes it (the test knock-out) */
  if (getenv("QJS_AOT_REGION_KNOCKOWNCAP") != NULL) fprintf(o, "#define RG_OWNCAP (getenv(\"QJS_AOT_REGION_NOOWNCAP\") || getenv(\"QJS_AOT_REGION_NOCOMPACT\") ? 0x7fffffff : 65536)   /* the knock-out build: the run lifts the bound */\n");
  else fprintf(o, "#define RG_OWNCAP %d\n", getenv("QJS_AOT_REGION_NOOWNCAP") != NULL ? 0x7fffffff : getenv("QJS_AOT_REGION_OWNCAP") ? atoi(getenv("QJS_AOT_REGION_OWNCAP")) : 65536);
  fprintf(o, "#define RG_FREE_OWNED() do {");
  for (g = 0; g < c->ncons; g++) {
    if (c->cons[g].stack) {
      if (c->cons[g].kind == 0) { RStr t = {0}; rg_sk_release_text(c, g, &t); fprintf(o, " %s", t.p ? t.p : ""); free(t.p); }
      fprintf(o, " JS_FreeValue(ctx, rnh_%d);", g);
    } else fprintf(o, " JS_FreeValue(ctx, rn_%d);", g);
  }
  fprintf(o, " if (rgon_) { rg_own_free(ctx, rgo_, rgon_, rgoc_ > 0); rgo_ = rgo_b_; rgon_ = 0; rgoc_ = -64; }");
  fprintf(o, " } while (0)\n");
  /* the values of every instance are locals of the function: an exit stub builds the inlined frames from them */
  for (i = 0; i < c->nin_total; i++) {
    int v;
    /* declared only: a value is defined before every use on the paths of the region; the ones an exit stub can read (its frame state may name a value of
       another branch) are set to undefined where their instance begins (root: here), not all of them at every entry */
    for (v = 0; v < c->in[i]->q.nvalues; v++) rg_ty_decl(c, o, c->in[i], v);   /* a typed value: its local and the macro of its boxed view */
    if (i == 0) fprintf(o, "/*@RGI 0@*/\n");
  }
  c->nin = 1;
  if (!rg_walk_inst(c, 0, rootcur, root)) { snprintf(why, whyn, "%s", c->why[0] ? c->why : "the region does not compile (second pass)"); fclose(o); free(buf); goto cleanup; }
  if (getenv("QJS_AOT_FASTSTATS") != NULL) {
    int si;
    for (si = 0; si < c->nsite; si++)
      fprintf(o, "    rgs_%d:; rgw_%s[%d]++; goto rx_%d;\n", si, hex, si, c->site[si].xi);
  }
  /* exit stubs and poll exits (only the ones some jump names) */
  if (c->bad) { snprintf(why, whyn, "too many or too deep exit states"); fclose(o); free(buf); goto cleanup; }
  if (c->npmn > 0) {
    int pi_;
    fprintf(o, "    static const char *const rg_pmn_[] = {");
    for (pi_ = 0; pi_ < c->npmn; pi_++) fprintf(o, "%s\"%s\"", pi_ ? ", " : "", c->pmn[pi_]);
    fprintf(o, "};\n");
  }
  {
    uint8_t *used = calloc((size_t)(c->nxit > 0 ? c->nxit : 1), 1);
    size_t q;
    if (used == NULL) abort();
    fflush(o);
    for (q = 0; q + 8 < blen; q++) {
      if (buf[q] == 'g' && memcmp(buf + q, "goto r", 6) == 0 && (buf[q + 6] == 'x' || buf[q + 6] == 'p') && buf[q + 7] == '_') {
        size_t z = q + 8;
        int num = 0;
        while (z < blen && buf[z] >= '0' && buf[z] <= '9') num = num * 10 + (buf[z++] - '0');
        if (z < blen && buf[z] == ';' && num < c->nxit) used[num] = 1;
      }
    }
    for (i = 0; i < c->nxit; i++) {
      RXit *x = &c->xit[i];
      if (!used[i]) continue;
      fprintf(o, "    rx_%d:;\n    E_EXIT_NOTE_PC(%d, %d, \"%s\");\n", i, fi, x->pc, hex);
      if (getenv("QJS_AOT_FASTSTATS") != NULL) fprintf(o, "    rgx_%s[%d]++; rgxpc_%s[%d] = %d;\n", hex, i, hex, i, x->pc);
      fputs(x->root, o);
      fputs(x->rest, o);
      fprintf(o, "    RG_FREE_OWNED();\n    E_RET_G(%s, %d);\n", hex, x->rpc);
      if (x->d > 0) {
        /* a poll in an inlined loop: the frames are materialised as at any exit and the innermost one polls (rg_chain) before it goes on in its generic body */
        fprintf(o, "    rp_%d:; rg_pollp_%s = 1; goto rx_%d;\n", i, hex, i);
      } else {
        fprintf(o, "    rp_%d:;\n", i);
        fputs(x->root, o);
        fprintf(o, "    RG_FREE_OWNED();\n    if (qjs_aot_poll_fl(ctx) < 0) return JS_EXCEPTION;\n    E_RET_G(%s, %d);\n", hex, x->rpc);
      }
    }
    free(used);
  }
  fprintf(o, "#undef RG_FREE_OWNED\n");
  fprintf(o, "    }\n}\n\n");
  rg_ty_undefs(c, o);   /* the boxed-view macros of typed values end with the function */
  if (getenv("QJS_AOT_FASTSTATS") != NULL) {
    int si;
    fprintf(o, "static void __attribute__((destructor)) rg_site_print_%s(void) {\n", hex);
    for (si = 0; si < c->nsite; si++) fprintf(o, "  if (rgw_%s[%d]) fprintf(stderr, \"region %s exit source %s: %%lu\\n\", rgw_%s[%d]);\n", hex, si, hex, c->site[si].what, hex, si);
    fprintf(o, "}\n");
  }
  fclose(o);
  if (rg_trust_on() && (memmem(buf, blen, "rg_learn_", 9) != NULL || memmem(buf, blen, "rg_off_", 7) != NULL || memmem(buf, blen, "rg_sh_", 6) != NULL ||
                        memmem(buf, blen, "rg_tg_", 6) != NULL)) {
    /* the rule of TRUST (reviewer item 7): no compiled path learns at run time */
    snprintf(why, whyn, "the region would learn at run time (a learner or a learned table was emitted)");
    free(buf);
    goto cleanup;
  }
  {
    /* the values an exit stub names: set at their instance's start */
    unsigned char **mk = calloc((size_t)c->nin_total, sizeof(*mk));
    size_t q2, z;
    int xi2;
    if (mk == NULL) abort();
    for (i = 0; i < c->nin_total; i++) { mk[i] = calloc((size_t)(c->in[i]->q.nvalues > 0 ? c->in[i]->q.nvalues : 1), 1); if (mk[i] == NULL) abort(); }
    for (xi2 = 0; xi2 <= c->nxit; xi2++) {
      const char *ss[2] = { xi2 < c->nxit ? c->xit[xi2].root : c->ools.p, xi2 < c->nxit ? c->xit[xi2].rest : NULL };
      int si2;
      for (si2 = 0; si2 < 2; si2++) {
        const char *t = ss[si2];
        if (t == NULL) continue;
        for (; *t; t++) {
          int a_, b_;
          const char *u;
          if (*t != 'r' || (t > ss[si2] && ((t[-1] >= 'a' && t[-1] <= 'z') || (t[-1] >= '0' && t[-1] <= '9') || t[-1] == '_'))) continue;
          u = t + 1; a_ = 0;
          if (*u < '0' || *u > '9') continue;
          while (*u >= '0' && *u <= '9') a_ = a_ * 10 + (*u++ - '0');
          if (u[0] != '_' || u[1] != 'v') continue;
          u += 2; b_ = 0;
          if (*u < '0' || *u > '9') continue;
          while (*u >= '0' && *u <= '9') b_ = b_ * 10 + (*u++ - '0');
          for (i = 0; i < c->nin_total; i++) {
            int pi_ = -1;
            if (sscanf(c->in[i]->pfx, "r%d_", &pi_) == 1 && pi_ == a_) { if (b_ < c->in[i]->q.nvalues) mk[i][b_] = 1; break; }
          }
        }
      }
    }
    {
      char *nb = NULL;
      size_t nn = 0;
      FILE *o2 = open_memstream(&nb, &nn);
      if (o2 == NULL) abort();
      for (q2 = 0; q2 < blen; q2++) {
        if (buf[q2] == '/' && q2 + 8 < blen && memcmp(buf + q2, "/*@RGI ", 7) == 0) {
          int id_ = 0, ii2 = -1;
          z = q2 + 7;
          while (z < blen && buf[z] >= '0' && buf[z] <= '9') id_ = id_ * 10 + (buf[z++] - '0');
          for (i = 0; i < c->nin_total; i++) if (c->in[i]->id == id_) { ii2 = i; break; }
          if (ii2 >= 0) { int v2; for (v2 = 0; v2 < c->in[ii2]->q.nvalues; v2++) if (mk[ii2][v2]) rg_ty_init(o2, c->in[ii2], v2); }
          while (z < blen && buf[z] != '\n') z++;
          q2 = z;
          continue;
        }
        fputc(buf[q2], o2);
      }
      fclose(o2);
      free(buf);
      buf = nb;
      blen = nn;
    }
    for (i = 0; i < c->nin_total; i++) free(mk[i]);
    free(mk);
  }
  {
    /* the owned list's stack buffer: one entry per site that can hold a reference when the root has no loop (a recursion's every level pays for it),
       a few otherwise (the list moves to the heap when it fills) */
    int nsite_ = 0, cap_;
    size_t q3;
    char cs_[16];
    for (q3 = 0; q3 + 12 < blen; q3++) if (memcmp(buf + q3, "rg_own_dup(ctx", 14) == 0 || memcmp(buf + q3, "rg_own_put(ctx", 14) == 0) nsite_++;
    cap_ = rg_has_loop(root) ? 16 : (nsite_ > 0 ? nsite_ : 1);
    if (cap_ > 64) cap_ = 64;
    snprintf(cs_, sizeof(cs_), "%d", cap_);
    for (q3 = 0; q3 + 10 < blen; q3++) {
      if (memcmp(buf + q3, "rgo_b_[64]", 10) == 0 || memcmp(buf + q3, "rgoc_ = -64", 11) == 0) {
        size_t at_ = memcmp(buf + q3, "rgo_b_[64]", 10) == 0 ? q3 + 7 : q3 + 9;
        /* rewrite the two digits in place, padded with spaces (the buffer keeps its length) */
        char pad_[3];
        snprintf(pad_, sizeof(pad_), "%-2s", cs_);
        if (cap_ < 10) { buf[at_] = cs_[0]; buf[at_ + 1] = ' '; } else { buf[at_] = cs_[0]; buf[at_ + 1] = cs_[1]; }
        (void)pad_;
      }
    }
  }
  {
    /* the memo slots of invariant reads, declared where the marker stands */
    char *mk_ = blen ? memmem(buf, blen, "/*@RGM@*/", 9) : NULL;
    if (mk_ != NULL) {
      size_t pre_ = (size_t)(mk_ - buf);
      int k_;
      fwrite(buf, 1, pre_, f);
      for (k_ = 0; k_ < c->ncp; k_++) if (!c->cpk[k_] && rg_f64_on() && getenv("QJS_AOT_REGION_NOCPLOCAL") == NULL) fprintf(f, "    JSValue rgq_%d = rgp_%s[%d]; (void)rgq_%d;\n", k_, hex, k_, k_);
      for (k_ = 0; k_ < c->nmemo; k_++)
        fprintf(f, "    JSValue rgm_%d; JSValue *rgma_%d; uint32_t rgmc_%d; uint8_t rgmv_%d = 0; (void)rgm_%d; (void)rgma_%d; (void)rgmc_%d;\n", k_, k_, k_, k_, k_, k_, k_);
      if (c->hoist.p) fputs(c->hoist.p, f);
      fwrite(mk_ + 9, 1, blen - pre_ - 9, f);
    } else fwrite(buf, 1, blen, f);
  }
  free(buf);
  {
    int ia;
    for (ia = 1; ia < c->nin_total; ia++) { char hy[33]; rg_hexof(c->in[ia]->g, hy); qjs_aot_region_dep(hex, hy); }
    /* QJS_AOT_REGION_CENSUS: which functions an emitted region holds (the census script reads the generator's stderr) */
    if (f != NULL && getenv("QJS_AOT_REGION_CENSUS") != NULL) {
      fprintf(stderr, "census: region %s inlines", hex);
      for (ia = 1; ia < c->nin_total; ia++) { char hy[33]; rg_hexof(c->in[ia]->g, hy); fprintf(stderr, " %s", hy); }
      fprintf(stderr, "\n");
    }
  }
  ok = 1;
  goto done;
cleanup:
  if (g_meas_on > 0 && !c->emit) rg_measure_report(hex, root->g);
  /* free the callee IRs built for the walks */
  for (i = 1; i < c->nincap; i++) if (c->in[i] != NULL && c->in[i]->g != NULL) q2_free(&c->in[i]->q);
done:
  if (ok) for (i = 1; i < c->nincap; i++) if (c->in[i] != NULL && c->in[i]->g != NULL) q2_free(&c->in[i]->q);
  for (i = 0; i < c->nincap; i++) if (c->in[i] != NULL) { free(c->in[i]->mname); free(c->in[i]->mpc); free(c->in[i]->ty); free(c->in[i]->rw); if (i > 0) free(c->in[i]->cur); free(c->in[i]); }
  for (i = 0; i < c->nxit; i++) { free(c->xit[i].root); free(c->xit[i].rest); }
  free(c->in);
  free(c->xit);
  free(c->ools.p); free(c->hoist.p); free(c->orig);
  free(c->invn);
  g_rgc = NULL;
  free(c);
  free(rootcur);
  return ok;
}

/* A region with the small loop callees expanded in place, else the same region as before (they out of line): a root that compiled without the first still does. */
int region_emit(FILE *f, const Q2Func *root, const char hex[33], int fi, char *why, size_t whyn) {
  int r;
  if (getenv("QJS_AOT_REGION_OOLLOOP") == NULL && rg_trust_on()) {
    r = region_emit1(f, root, hex, fi, why, whyn, 1);
    if (r) return 1;
    if (getenv("QJS_AOT_REGION_WHY")) fprintf(stderr, "loopinl: root %.8s [%s@%d] refused with loops inlined (%s); retrying out of line\n", hex, root->g->name[0] ? root->g->name : "<anon>", root->g->line, why);
  }
  return region_emit1(f, root, hex, fi, why, whyn, 0);
}
