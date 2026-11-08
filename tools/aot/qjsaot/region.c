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

unsigned long long qjs_aot_prof_site_calls(const uint8_t dg[16], int next_pc);
unsigned long long qjs_aot_prof_entries(const uint8_t dg[16]);
int qjs_aot_regs_of_gen(const JSAotGenFunc *g);
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
  int bsnap[128], bpe[128];   /* per block: the stretch's stub and whether its last op ended it, when the block was left */
  int snap, prev_eff;     /* the exit stub of the current stretch (the state at its start), and whether the previous op ended it */
  int eff, ieff;          /* census: the current effect point of this instance, and whether an effect op has run in it since its start */
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
  uint64_t key;           /* identifies the instance across the walks of one region (its parent's key, the call pc, the arm): the line cap names the instances it cuts by it */
  int ool;                /* TRUST: the arm is called out of line (a callee with a loop of its own runs its own compiled body), not expanded */
  int wrap;               /* a forwarding constructor `function () { this.m.apply(this, arguments); }`: its variable area is the two slots the pattern uses (see rg_wrapper) */
  int wr_uninit_pc, wr_put_pc, wr_read_pc;
  char wr_method[40];   /* the pcs of its local_uninit, put_local_s0 and the read of `this.m` */
  uint8_t rty;            /* F64: the type of this instance's result (RT_*; RT_ANY or 0 when it is a JSValue `res`) */
  int *idom, curb;        /* F21: the immediate dominators of the blocks (computed on first use), and the block being walked */
  uint8_t *ty, *rw;       /* F64 (region_fn_var.inc): the type of each value (RT_*), and whether a double was made by + - * neg inc dec (stored as the engine stores it, not normalised) */
} RInst;

typedef struct RGlob { char g[40], f[40]; int obj; } RGlob;   /* obj: the value is a function (a thru-call's X), held by the unit; else an int */

/* A construct (`new F(args)` of a global F) in the region: the object is owned by the region (`rn_K`) until it returns or leaves. */
#define RG_MAXC 256
typedef struct RCons { int inst, pc, kind; char gname[48]; char path[4][40]; int npath; const JSAotGenFunc *tg; int nn; char names[16][40]; int site, direct, fwd; unsigned seen; int esc; int stack, sk_pair, knock, ss, late, nmat, mat[4][5]; unsigned pre; int nrbs; char rbs[8][40]; int chi, par, nkvis, kvis[48][2]; char cfield[40]; int nst, stover; short st[64][4]; unsigned fref; int vok, vstore, nvthis, virt; unsigned vcov; short vthis[32][2]; unsigned char vcls[16], vty[16]; int nls, ls[4][4]; int accum, acn, acv[16], accroom, matacc[4], rslot; } RCons;   /* stack: lives in the region's C frame (kind 0 object, kind 1 array); sk_pair: the cons index of its array / its object, else -1; chi: the cons index + 1 of the stack object its field `cfield` holds (a container), par: the cons index + 1 of the container (a child);
   kvis: the container's aliases (instance, value) the analysis followed, for the reads of `cfield` that would hand out the child */

/* TRUST: a class of the region (AOT-REDESIGN.md, the class table): the allocation site, its constructor's global, and its fields in add order. At the region's first
   entry the init binds it once: the constructor (a fact on the global binding), its prototype (a fact on F.prototype), the class shape built from the prototype and
   the field list (qjs_aot_class_shape, held), and the shape's layout (a dependency of the word: an instance leaving the shape breaks it). Dispatch is a compare of
   the receiver's shape with these, field reads and writes are at the slots of the list; nothing is learned at run time. */
#define RG_MAXCL 24
#define RG_MAXSTK 40
static const Q2Func *g_matdef_fn[16];   /* the results of the moves that make a heap object of a stack object (an origin the region made, for the owned list's compaction) */
static int g_matdef_v[16], g_nmatdef;
/* nsh > 1: a class whose instances gain fields late (QJS_AOT_REGION_MSHAPE=1, opt-in): `ms` is its table of up to QJS_AOT_MAXSHAPES profile-observed shapes (fast.h QjsAotClassShapes); shape 0
   is the construction walk's final shape (`nn`/`names` above, rk_s_), the others extend it. nsh is 0 for every other class, whose emission is unchanged. */
typedef struct RCls { int site; char ctor[48]; int nn; char names[32][40]; int need_layout; int nsh; QjsAotClassShapes ms; } RCls;
/* The inline slots a constructed object is given: its construction-walk fields and the fields its class gains after (the profile's late adds), 0 when there are none. A size hint for the allocation. */
static unsigned rg_cons_cap(const RCons *K) {
  char late[24][40];
  int nl;
  if (getenv("QJS_AOT_REGION_NOCAP") != NULL) return 0;
  nl = K->site > 0 ? qjs_aot_bundle_class_late_names(K->site, late, 24) : qjs_aot_bundle_late_count_by_fields((char (*)[40])K->names, K->nn);
  return nl > 0 ? (unsigned)(K->nn + nl) : 0u;
}
#define RG_MAXGV 8
#define RG_MAXCP 64
/* The inline slots a stack object's block is given: the property-array size the engine's shape reaches for the construction walk's fields and for the fields the class gains later (it doubles from 2, so the room is
   the power of two that holds them; 0 when the engine's inline capacity cannot name it). */
static unsigned rg_stack_room(const RCons *K) {
  unsigned cap = K->late ? rg_cons_cap(K) : 0u, room = 2;
  while (room < (unsigned)K->nn) room *= 2;
  while (room < cap) room *= 2;
  if (K->accroom > (int)room) room = (unsigned)K->accroom;   /* the accumulator holds any of the objects that are assigned into it */
  return room <= 16 ? room : 0;
}

/* Whether the stack object of construct K is made by copying an image of the block (an object of one class shape with a known room) rather than by the engine's init call. */
static int rg_img_ok(const RCons *K) {
  static int v = -1;
  if (v < 0) v = getenv("QJS_AOT_REGION_NOIMG") == NULL;
  return v && K->stack && K->kind == 0 && K->direct && !K->rslot && rg_stack_room(K) > 0;
}

/* An exit stub: the root's frame state (`root`, AOT_SET_B lines), and for an exit inside inlined code the chain of inlined frames to materialise (`rest`). */
typedef struct RXit { int pc, d, rpc, cep, cii; unsigned long long h; char *root, *rest; char *t3, *t3pre; int nofpc; } RXit;   /* t3: the stub's frame-state code as one call to the region's shared builder with a per-site descriptor (tier 3) */
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
  struct { int xi, ep, d, outerok; } *cen; int ncen, cencap, effn; int *effcnt, effcap;   /* census of exits against effect points */
  int nxit, xitcap, bad;
  int xelig;              /* the walk is at an op whose guards may resume at the stretch start instead of the op */
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
  int t3used;   /* some stub calls the tier-3 builder: emit it */
  RStr ools;              /* TRUST: the frame states the out-of-line calls hold (their values must be set where their instances begin) */
  int nsc; int sc_site[96]; char sc_text[96][48]; char sc_dg[96][33];   /* TRUST: the constructor path (`NS.P`) of an allocation site whose constructor is not a global, from the construct that makes it */
  RCls cls[RG_MAXCL];     /* TRUST: the class table */
  int ncls;
  char gv[RG_MAXGV][40];  /* TRUST: globals read as values (their atoms are bound at init) */
  int ngv;
  char cp[RG_MAXCP][6][40];  /* TRUST: constant paths G.f1.f2.f3 (a global and own data properties), bound at init with a fact on every link */
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
  int nss, skstride;                            /* the stack slots (objects and their Arrays), and the bytes of one slot's block */
  int loopinl;            /* TRUST: a callee with a loop of its own is expanded in place when it is small and the call has one target (else it is called out of line); the retry without it is region_emit's */
  char hk[256][120]; int hkm[256], nhk;
  char (*invn)[24]; int ninv, invcap; int nmemo;   /* TRUST: a path whose value the bundle shows is one int constant (the code uses it, the init checks it) */
  /* T13, the per-site profile-set dispatch: the shapes (class table index, ordered field list) the dispatches test against, built at init (rk_dx_); the shape-test CSE: the variable (rgd_N) that
     holds the matched shape of a value, valid while dsx_ep is the epoch it was made in (an op that may add or delete a field, and every block start, moves the epoch on) */
  struct RDsh { int k, nn; char names[32][40]; } dsh[48]; int ndsh;
  unsigned dsx_ep; struct { int inst, base; unsigned ep; int var; } dsx[96]; int ndsx; int ndx;
  unsigned lent;   /* the parameters of the root that it only reads, so a caller may lend its stack objects to them (rg_lent_mask) */
  /* F21: the root arguments whose value a trusted fixed-slot access depends on, tested once at the entry (an object of one of the classes the accesses assume), and the accesses
     refused trust because their receiver is neither made by the region nor tied to such an argument (the dominance check, QJS_AOT_REGION_DOMCHECK) */
  unsigned ra_mask, ra_cls[8]; int ra_any[8]; int ra_flag, ra_site;
  struct { int inst, blk; char val[40]; } ck[96]; int nck;   /* receivers already tested, and the block of the test: an access dominated by it tests nothing */
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
static int rg_cls_multi(RCls *K) {
  /* a class that gains fields late: the profile's observed shapes (E1/E2); the base alone, or an ambiguous or over-full table, is not a multi-shape class */
  char hex[33], late[16][40];
  int nl, t;
  if (!qjs_aot_mshape_on() || !qjs_aot_bundle_site_ctor_digest(K->site, hex) || (nl = qjs_aot_bundle_class_late_names(K->site, late, 16)) <= 0) return -1;
  qjs_aot_prof_class_shapes(hex, K->names, K->nn, late, nl, &K->ms);
  if (getenv("QJS_AOT_REGION_WHY")) fprintf(stderr, "why: class %s: profile shapes %d status %d\n", K->ctor, K->ms.n, K->ms.status);
  if (K->ms.status != QJS_AOT_MS_OK || K->ms.n < 2) return -1;
  K->nsh = K->ms.n;
  for (t = 1; t < K->nsh; t++) rg_clreg_add(K->ctor, K->ms.names[t], K->ms.nn[t]);
  return 0;
}
static int rg_cls_add(RCtx *c, int site, const char *ctor, int nfields, int multi) {
  int k = rg_cls_find(c, ctor), j;
  RCls *K;
  if (k >= 0) {
    /* a class first bound for the per-site dispatch only (multi 2) that a later use needs the shape table of (multi 1) */
    if (multi == 1 && !c->emit && c->cls[k].nsh == 0 && c->cls[k].ms.n == 0 && c->cls[k].site > 0) { if (rg_cls_multi(&c->cls[k]) == 0) rg_clreg_add(c->cls[k].ctor, c->cls[k].names, c->cls[k].nn); }
    return k;
  }
  if (c->emit || ctor == NULL || !ctor[0] || strlen(ctor) >= sizeof(c->cls[0].ctor) || c->ncls >= RG_MAXCL || nfields < 0 || nfields > 32) return -1;
  K = &c->cls[c->ncls];
  memset(K, 0, sizeof(*K));
  K->site = site;
  snprintf(K->ctor, sizeof(K->ctor), "%s", ctor);
  for (j = 0; j < nfields; j++) if (!qjs_aot_bundle_site_field_name(site, j, K->names[j], sizeof(K->names[0]))) return -1;
  K->nn = nfields;
  if (multi == 1 && rg_cls_multi(K) < 0) return -1;
  if (multi != 2) rg_clreg_add(K->ctor, K->names, K->nn);   /* multi 2: a class bound for the per-site shape dispatch only; no layout row is asked for */
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
static void rg_lent_assume(const char *callee, const char *caller, unsigned m);
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
/* The emitted-line cap per compiled function (QJS_AOT_FN_LINE_CAP=N, off when unset): a region whose inlined instances are estimated to emit more than N C
   lines has the coldest (then the largest) instances cut: a cut instance is called out of line, as the callee's own compiled body (or the interpreter when it
   has none). The estimate is QJS_AOT_FN_LINES_PER_VALUE (default 12) lines per value of an instance. The cut set is found after the first walk, and the region
   is walked again with it (region_emit retries). */
static uint64_t g_cut[1024];
static int g_ncut, g_cut_retry;
static int rg_cut_has(uint64_t k) { int i; for (i = 0; i < g_ncut; i++) if (g_cut[i] == k) return 1; return 0; }
static int rg_line_cap(void) { const char *e = getenv("QJS_AOT_FN_LINE_CAP"); return e != NULL ? atoi(e) : 0; }
static int rg_lines_per_value(void) { const char *e = getenv("QJS_AOT_FN_LINES_PER_VALUE"); return e != NULL && atoi(e) > 0 ? atoi(e) : 12; }

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

static int rg_insn_at(const Q2Func *fn, int pc);
/* Whether value v of fn is used by instruction k or after it (the instruction a generic body resumes at runs first). */
static int rg_blk_uses(const Q2Func *fn, const Q2Block *B, int from, Q2ValueId v) {
  int j, u, si;
  for (j = from; j < B->first_insn + B->ninsns; j++) {
    const Q2Insn *in = &fn->insns[j];
    for (u = 0; u < in->nuse; u++) if (in->use[u] == v) return 1;
    for (u = 0; u < in->win_n && in->win_val != NULL; u++) if (in->win_val[u] == v) return 1;
  }
  for (si = 0; si < B->nsucc; si++)
    for (j = 0; j < B->nedge_args[si]; j++) if (B->edge_arg[si][j] == v) return 1;
  return 0;
}
/* Whether value v (held in register `reg`) is still needed when the code resumes at instruction index `from` of its block: it is read in the rest of the block, or on an edge out of
   it, or in ANY block reachable from the block's successors (a value is defined once, so a read down any path is a read of this value; a block only lists the values it reads itself, so
   a value passed through blocks that do not read it, a loop bound read by the loop header, is found by walking on). A block that defines v ends the walk on its path: v is made again
   there before any read. A register live into a reached block (its in_reg) counts too, since the later blocks name a flowing value by their own parameter. A target that leaves the
   function keeps everything. */
static int g_lw_param_kill;   /* set: a block parameter is defined afresh at the entry of its block (the walk does not follow it round a loop to its own block) */
static int rg_live_walk(const Q2Func *fn, int from, Q2ValueId v, int reg) {
  int b, j, u, si, nst = 0, defblk = -1, found = 0;
  const Q2Block *B = NULL;
  int *stack;
  unsigned char *seen;
  for (b = 0; b < fn->nblocks; b++) if (from >= fn->blocks[b].first_insn && from < fn->blocks[b].first_insn + fn->blocks[b].ninsns + 1) { B = &fn->blocks[b]; break; }
  if (B == NULL) return 1;
  for (j = from; j < B->first_insn + B->ninsns; j++) {
    const Q2Insn *in = &fn->insns[j];
    for (u = 0; u < in->nuse; u++) if (in->use[u] == v) return 1;
    for (u = 0; u < in->win_n && in->win_val != NULL; u++) if (in->win_val[u] == v) return 1;
  }
  if ((int32_t)v >= 0 && (int32_t)v < fn->nvalues && fn->values[v].insn >= 0) {
    int di = fn->values[v].insn;
    for (b = 0; b < fn->nblocks; b++) if (di >= fn->blocks[b].first_insn && di < fn->blocks[b].first_insn + fn->blocks[b].ninsns) { defblk = b; break; }
  } else if (g_lw_param_kill && (int32_t)v >= 0 && (int32_t)v < fn->nvalues && fn->values[v].insn < 0 && fn->values[v].param_of >= 0) defblk = fn->values[v].block;
  stack = malloc((size_t)(fn->nblocks + 1) * sizeof(int) * 2);
  seen = calloc((size_t)fn->nblocks + 1, 1);
  if (stack == NULL || seen == NULL) abort();
  for (si = 0; si < B->nsucc; si++) {
    for (j = 0; j < B->nedge_args[si]; j++) if (B->edge_arg[si][j] == v) { found = 1; goto done; }
    if (B->succ_block[si] < 0 || B->succ_block[si] >= fn->nblocks) { found = 1; goto done; }
    stack[nst++] = B->succ_block[si];
  }
  while (nst > 0) {
    const Q2Block *T = &fn->blocks[stack[--nst]];
    int tb = (int)(T - fn->blocks);
    if (seen[tb]) continue;
    seen[tb] = 1;
    if (g_lw_param_kill && tb == defblk) continue;
    for (j = 0; j < T->nlive_in; j++) if (T->in_val[j] == v || T->in_reg[j] == reg) { found = 1; goto done; }
    if (tb == defblk) continue;
    for (j = T->first_insn; j < T->first_insn + T->ninsns; j++) {
      const Q2Insn *in = &fn->insns[j];
      for (u = 0; u < in->nuse; u++) if (in->use[u] == v) { found = 1; goto done; }
      for (u = 0; u < in->win_n && in->win_val != NULL; u++) if (in->win_val[u] == v) { found = 1; goto done; }
    }
    for (si = 0; si < T->nsucc; si++) {
      for (j = 0; j < T->nedge_args[si]; j++) if (T->edge_arg[si][j] == v) { found = 1; goto done; }
      if (T->succ_block[si] < 0 || T->succ_block[si] >= fn->nblocks) { found = 1; goto done; }
      if (nst < fn->nblocks * 2) stack[nst++] = T->succ_block[si];
    }
  }
done:
  free(stack);
  free(seen);
  return found;
}
static int rg_live_from(const Q2Func *fn, int k, Q2ValueId v, int reg) { return rg_live_walk(fn, k, v, reg); }
/* The registers of instance X an exit that resumes X's generic body at `pc` must write: those whose current value is still used from that instruction on. A register whose current value is
   not a plain SSA name of X (a pending-pair text, a constant) is always written. QJS_AOT_REGION_NOLIVEEXIT=1 writes every register as before. */
static void rg_exit_keep(const RInst *X, int pc, uint8_t keep[256]) {
  int r, idx = -1;
  size_t pl = strlen(X->pfx);
  memset(keep, 1, 256);
  if (getenv("QJS_AOT_REGION_NOLIVEEXIT") != NULL || X->g == NULL) return;
  idx = rg_insn_at(&X->q, pc);
  if (idx < 0) return;
  for (r = 0; r < 256; r++) {
    const char *t = X->cur[r];
    char *e;
    long v;
    if (!t[0]) { keep[r] = 0; continue; }
    if (strncmp(t, X->pfx, pl) != 0 || t[pl] != 'v') continue;
    v = strtol(t + pl + 1, &e, 10);
    if (*e != 0 || e == t + pl + 1 || v < 0 || v >= X->q.nvalues) continue;
    keep[r] = (uint8_t)rg_live_from(&X->q, idx, (Q2ValueId)v, r);
  }
}

static void rg_cen_rec(RCtx *c, int xi, int ep, int d, int ok) {
  if (getenv("QJS_AOT_REGION_EPCENSUS") == NULL) return;
  if (c->ncen >= c->cencap) { c->cencap = c->cencap ? c->cencap * 2 : 4096; c->cen = realloc(c->cen, (size_t)c->cencap * sizeof(c->cen[0])); if (c->cen == NULL) abort(); }
  c->cen[c->ncen].xi = xi; c->cen[c->ncen].ep = ep; c->cen[c->ncen].d = d; c->cen[c->ncen].outerok = ok; c->ncen++;
}

/* census: a new effect point of instance I at pc, with the number of distinct values an exit resuming there would name */
static void rg_cen_point(RCtx *c, RInst *I, int pc) {
  uint8_t keep[256];
  int r, q, n = 0;
  if (getenv("QJS_AOT_REGION_EPCENSUS") == NULL || I->cur == NULL) return;
  rg_exit_keep(I, pc, keep);
  for (r = 0; r < 256; r++) {
    if (!I->cur[r][0] || !keep[r]) continue;
    for (q = 0; q < r; q++) if (keep[q] && strcmp(I->cur[q], I->cur[r]) == 0) break;
    if (q == r) n++;
  }
  c->effn++;
  if (c->effn >= c->effcap) { c->effcap = c->effcap ? c->effcap * 2 : 4096; c->effcnt = realloc(c->effcnt, (size_t)c->effcap * sizeof(int)); if (c->effcnt == NULL) abort(); }
  c->effcnt[c->effn] = n;
  I->eff = c->effn;
}

/* The accumulator the root returns, when the root may fill a shell its caller gives (QJS_AOT_REGION_RETSLOT=1): its construct, else -1. */
static int rg_retslot_on(void) { static int v = -1; if (v < 0) v = getenv("QJS_AOT_REGION_RETSLOT") != NULL && getenv("QJS_AOT_REGION_NORETSLOT") == NULL; return v; }
static int rg_acc_cons(const RCtx *c) {
  int k;
  for (k = 0; k < c->ncons; k++) if (c->cons[k].stack && c->cons[k].accum) return k;
  return -1;
}
static int rg_has_retslot(const RCtx *c) { return rg_retslot_on() && rg_acc_cons(c) >= 0; }
static int rg_skf(const RCtx *c) { return c->nstack > 0 || c->lent != 0; }
/* Tier 3 (the default; QJS_AOT_REGION_NOT3=1 turns it off): an exit stub's frame state (the stores of the root frame, then the chain of inlined frames) is not code at the stub. The stub names the live values in
   an array and describes the rest in static data (offsets, the frame table), and one shared builder per region (rg_t3b_HEX) stores, builds the chain and runs it. It returns 0 (go
   on: free the owned list, return to the generic body), 1 (the no-frames knob: return at the call's read pc) or 2 (an exception). */
typedef struct RT3 { RStr vals, cos, offs, fcs; int nv, nc, nroot, noffs, on, bad, nst; } RT3;
static int rg_t3_on(void) { static int v = -1; if (v < 0) v = getenv("QJS_AOT_REGION_NOT3") == NULL; return v; }
static int rg_t3_min(void) { static int v = -1; if (v < 0) v = getenv("QJS_AOT_REGION_T3_MIN") ? atoi(getenv("QJS_AOT_REGION_T3_MIN")) : 6; return v; }
static void t3_free(RT3 *t) { free(t->vals.p); free(t->cos.p); free(t->offs.p); free(t->fcs.p); }
static int rg_xit_x(RCtx *c, int ii, int pc) {
  int chain[16], d = 0, k, i, j, r, rpc, total = 0, off, nlv;
  RInst *I0 = c->in[0], *C1 = NULL;
  RStr rt = {0}, rs = {0};
  RT3 t3;
  int foffs_[16];
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
  memset(&t3, 0, sizeof t3);
  t3.on = rg_t3_on() && e_nogeneric() && getenv("QJS_AOT_REGION_NOWRAPSKFIX") == NULL && getenv("QJS_AOT_REGION_NOSKFIXCTOR") == NULL && getenv("QJS_AOT_FASTSTATS") == NULL;
  if (c->lent != 0) rs_add(&rt, "    (void)RG_BORP();\n");   /* a block lent to this function moves to the heap before any name below is read */
  if (c->nstack > 0) {
    /* a stack object alive at this exit moves to the heap first (its references move: qjs_aot_stack_migrate); sk_ho / sk_ha are the heap object and Array, and every
       name below is read through SKFIX, which maps a stack block to them. The region then owns the heap objects in rnh_ (freed with the rest at the stub's end);
       AOT_SET_B takes the frame's own reference to each name, as for any other object. */
    int k0;
    /* a virtual object's fields are written into its block before anything reads the block */
    if (getenv("QJS_AOT_REGION_VIRTKNOCK") == NULL)
      for (k0 = 0; k0 < c->ncons; k0++) if (c->cons[k0].virt) {
        int j0;
        if (getenv("QJS_AOT_REGION_VFCHECK") != NULL) rs_add(&rt, "    if (sk_%d) rg_vmat_%s_%d(ctx, skb_%d, vs_%d", k0, c->hex, k0, k0, k0);
        else rs_add(&rt, "    if (vk_%d) { sk_%d = 1; vk_%d = 0; rg_vmat_%s_%d(ctx, skb_%d, vs_%d", k0, k0, k0, c->hex, k0, k0, k0);
        for (j0 = 0; j0 < c->cons[k0].nn; j0++) rs_add(&rt, ", vf_%d_%d", k0, j0);
        rs_add(&rt, getenv("QJS_AOT_REGION_VFCHECK") != NULL ? ");\n" : "); }\n");
      }
    /* the vkit knock-out build (QJS_AOT_REGION_NOMIGRATE at generation): the stub migrates unless the run sets QJS_AOT_REGION_MIGKNOCK, which leaves the stack object un-migrated */
    if (getenv("QJS_AOT_REGION_NOMIGRATE") != NULL) rs_add(&rt, "    if (!getenv(\"QJS_AOT_REGION_MIGKNOCK\")) {\n");
    rs_add(&rt, "    if (!rg_skm_%s(ctx, &sks_b[0][0], sks_on, sks_h)) { RG_FREE_OWNED(); return JS_EXCEPTION; }\n", c->hex);
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
        if (e->end <= X->curidx + (getenv("QJS_AOT_REGION_NOCALLPAIR") != NULL ? 1 : 0)) continue;   /* an exit at the call itself resumes there: its callee and receiver are still needed */
        if (innermost && e->pc == pc) continue;
        if (npm + 2 > 24) { c->bad = 1; break; }
        if (e->creg >= 0) {
          pmsave[npm].I = X; pmsave[npm].reg = e->creg; memcpy(pmsave[npm].old, X->cur[e->creg], 40); npm++;
          { int pi_; for (pi_ = 0; pi_ < c->npmn && strcmp(c->pmn[pi_], e->name) != 0; pi_++) {}
            if (pi_ == c->npmn) { if (pi_ >= 64) { e_fatal("more than 64 distinct pending-pair method names in one region", NULL); pi_ = 0; } snprintf(c->pmn[pi_], 40, "%s", e->name); c->npmn++; }
            if (snprintf(X->cur[e->creg], 40, "RG_PM(%s,%d)", e->recv, pi_) >= 40) { fprintf(stderr, "qjsaot: FATAL pending-pair callee text overflows the 40-char slot: RG_PM(%s,%d)\n", e->recv, pi_); e_fatal("pending-pair callee text overflows its slot", NULL); } }
        }
        if (e->treg >= 0 && e->this_t[0]) {
          pmsave[npm].I = X; pmsave[npm].reg = e->treg; memcpy(pmsave[npm].old, X->cur[e->treg], 40); npm++;
          snprintf(X->cur[e->treg], 40, "%s", e->this_t);
        }
      }
    }
  }
  size_t t3pre = rt.n;   /* the stub's prelude (lent block, stack migration, virtual objects) stays at the stub; the stores and the chain become the descriptor */
  uint8_t keep0[256], keepj[16][256];
  int cen_ok, cen_ep;
  rg_exit_keep(I0, rpc, keep0);
  for (j = 0; j < d; j++) rg_exit_keep(c->in[chain[j]], j + 1 < d ? c->in[chain[j + 1]]->par_pc_after : pc, keepj[j]);
  for (r = 0; r < 256; r++) if (I0->cur[r][0] && keep0[r]) {
    rs_add(&rt, rg_skf(c) ? "    AOT_SET_B(ctx, fr, %d, SKFIX(%s));\n" : "    AOT_SET_B(ctx, fr, %d, %s);\n", r * 16, I0->cur[r]);
    if (t3.on) { rs_add(&t3.vals, rg_skf(c) ? "%sSKFIX(%s)" : "%s%s", t3.nv ? ", " : "", I0->cur[r]); rs_add(&t3.offs, "%s%d", t3.noffs ? ", " : "", r * 16); t3.nv++; t3.noffs++; t3.nroot++; t3.nst++; }
  }
  if (d > 0) {
    for (j = 0; j < d; j++) {
      RInst *J = c->in[chain[j]];
      nlv = 0;
      for (r = 0; r < 256; r++) if (J->cur[r][0] && keepj[j][r]) nlv++;
      total += 1 + J->nargs + nlv;
    }
    /* a vkit knob: leave at the root's call as the replaying design did (no frames), so the effects the callee had made are made twice */
    rs_add(&rs, "    if (__builtin_expect(rg_nof_%s(), 0)) { RG_FREE_OWNED(); E_RET_G(%s, %d); }\n", c->hex, c->hex, C1->par_read_pc);
    rs_add(&rs, "    {\n      JSValue rr_, dv_[%d]; JSObject *cos_[%d];\n", total, d);
    if (C1->par_kind == 1) rs_add(&rs, rg_skf(c) ? "      AOT_SET_B(ctx, fr, %d, SKFIX(rn_%d));\n" : "      AOT_SET_B(ctx, fr, %d, rn_%d);\n", C1->par_dst * 16, C1->par_cons);
    rs_add(&rs, "      *(const uint8_t **)((uint8_t *)f + QJS_AOT_OFF_REGRUN_CAP_IP) = *(const uint8_t *const *)(*(uint8_t **)((uint8_t *)f + QJS_AOT_OFF_REGRUN_B) + QJS_AOT_OFF_FNBC_REGVM_CODE) + %d;\n", C1->par_call_pc);
    off = 0;
    if (t3.on && C1->par_kind == 1) { char rn_[24]; snprintf(rn_, sizeof rn_, "rn_%d", C1->par_cons); rs_add(&t3.vals, rg_skf(c) ? "%sSKFIX(%s)" : "%s%s", t3.nv ? ", " : "", rn_); t3.nv++; }
    for (j = 0; j < d; j++) {
      RInst *J = c->in[chain[j]];
      int n = 0;
      foffs_[j] = t3.noffs;
      rs_add(&rs, rg_skf(c) && !(J->wrap && getenv("QJS_AOT_REGION_NOWRAPSKFIX") != NULL) ? "      cos_[%d] = %s;\n      dv_[%d] = SKFIX(%s);\n" : "      cos_[%d] = %s;\n      dv_[%d] = %s;\n", j, J->co, off, J->self);   /* NOWRAPSKFIX: a vkit knock-out (a wrapper's slot 0 would hold the stale stack pointer) */
      if (t3.on) { rs_add(&t3.cos, "%s%s", t3.nc ? ", " : "", J->co); t3.nc++; rs_add(&t3.vals, rg_skf(c) ? "%sSKFIX(%s)" : "%s%s", t3.nv ? ", " : "", J->self); t3.nv++; }
      for (i = 0; i < J->nargs; i++) { rs_add(&rs, rg_skf(c) ? "      dv_[%d] = SKFIX(%s);\n" : "      dv_[%d] = %s;\n", off + 1 + i, J->arg[i]); if (t3.on) { rs_add(&t3.vals, rg_skf(c) ? ", SKFIX(%s)" : ", %s", J->arg[i]); t3.nv++; } }
      nlv = 0;
      for (r = 0; r < 256; r++) if (J->cur[r][0] && keepj[j][r]) { rs_add(&rs, rg_skf(c) ? "      dv_[%d] = SKFIX(%s);\n" : "      dv_[%d] = %s;\n", off + 1 + J->nargs + nlv, J->cur[r]); if (t3.on) { rs_add(&t3.vals, rg_skf(c) ? ", SKFIX(%s)" : ", %s", J->cur[r]); t3.nv++; } nlv++; }
      rs_add(&rs, "      static const uint16_t of_%d[] = {", j);
      for (r = 0; r < 256; r++) if (J->cur[r][0] && keepj[j][r]) { rs_add(&rs, "%s%d", n++ ? ", " : " ", r * 16); if (t3.on) { rs_add(&t3.offs, "%s%d", t3.noffs ? ", " : "", r * 16); t3.noffs++; } }
      if (n == 0) { rs_add(&rs, " 0"); if (t3.on) { rs_add(&t3.offs, "%s0", t3.noffs ? ", " : ""); t3.noffs++; } }
      rs_add(&rs, " };\n");
      off += 1 + J->nargs + nlv;
    }
    rs_add(&rs, "      static const RgFC fc_[%d] = {\n", d);
    for (j = 0; j < d; j++) {
      RInst *J = c->in[chain[j]], *N = j + 1 < d ? c->in[chain[j + 1]] : NULL;
      char hx[33];
      rg_hexof(J->g, hx);
      nlv = 0;
      for (r = 0; r < 256; r++) if (J->cur[r][0] && keepj[j][r]) nlv++;
      {
        /* a forwarding constructor's frame has its two-slot variable area: slot 0 holds `this` once the put_local_s0 has run, slot 1 is uninitialised once the local_uninit has: by the pc the generic body
           resumes at (static, not a run-time flag). QJS_AOT_REGION_NOWRAPVARS (a vkit knock-out build): the area is not built. */
        int rj_ = N ? N->par_pc_after : pc, nv_ = J->wrap && getenv("QJS_AOT_REGION_NOWRAPVARS") == NULL ? J->g->var_count : 0;
        if (e_nogeneric()) {
          /* the interpreter continues the level: a callee need not be in the unit, its frame size is its program's register count */
          rs_add(&rs, "        { rg_cont_%s, %du, %du, %d, QJS_AOT_LIGHT_PREFIX_SLOTS + %d + %d, %d, %d, %d, %d, %d, %d, %d, of_%d },\n", c->hex,
                 rj_, N ? N->par_call_pc : 0, N ? N->par_dst * 16 : 0xffff, nv_, qjs_aot_regs_of_gen(J->g), nlv, J->nargs, J->par_kind == 1, J->g->pic_count > 0,
                 nv_, nv_ && rj_ > J->wr_put_pc, nv_ && rj_ > J->wr_uninit_pc, j);
          if (t3.on) rs_add(&t3.fcs, "        { rg_cont_%s, %du, %du, %d, QJS_AOT_LIGHT_PREFIX_SLOTS + %d + %d, %d, %d, %d, %d, %d, %d, %d, of_ + %d },\n", c->hex,
                 rj_, N ? N->par_call_pc : 0, N ? N->par_dst * 16 : 0xffff, nv_, qjs_aot_regs_of_gen(J->g), nlv, J->nargs, J->par_kind == 1, J->g->pic_count > 0,
                 nv_, nv_ && rj_ > J->wr_put_pc, nv_ && rj_ > J->wr_uninit_pc, foffs_[j]);
        } else
        rs_add(&rs, "        { aot_g_%s, %du, %du, %d, QJS_AOT_LIGHT_PREFIX_SLOTS + %d + Q2_FR_%s, %d, %d, %d, %d, %d, %d, %d, of_%d },\n", hx,
               rj_, N ? N->par_call_pc : 0, N ? N->par_dst * 16 : 0xffff, nv_, hx, nlv, J->nargs, J->par_kind == 1, J->g->pic_count > 0,
               nv_, nv_ && rj_ > J->wr_put_pc, nv_ && rj_ > J->wr_uninit_pc, j);
      }
    }
    rs_add(&rs, "      };\n      rr_ = rg_chain_%s(ctx, f, fc_, cos_, dv_, %d);\n", c->hex, d);
    rs_add(&rs, "      if (__builtin_expect(JS_IsException(rr_), 0)) { RG_FREE_OWNED(); return JS_EXCEPTION; }\n");
    if (C1->par_kind == 1) rs_add(&rs, rg_skf(c) && getenv("QJS_AOT_REGION_NOSKFIXCTOR") == NULL ? "      JS_FreeValue(ctx, rr_); AOT_SET_B(ctx, fr, %d, SKFIX(rn_%d));\n" : "      JS_FreeValue(ctx, rr_); AOT_SET_B(ctx, fr, %d, rn_%d);\n", C1->par_dst * 16, C1->par_cons);   /* a stack object was migrated above: the frame gets the heap object, not the stale stack pointer */
    else rs_add(&rs, "      if (JS_VALUE_HAS_REF_COUNT(rr_)) qjs_aot_set_reg_p(ctx, fr, %d, rr_); else AOT_SET_B(ctx, fr, %d, rr_);\n", C1->par_dst * 16, C1->par_dst * 16);
    rs_add(&rs, "    }\n");
  }
  while (npm > 0) { npm--; memcpy(pmsave[npm].I->cur[pmsave[npm].reg], pmsave[npm].old, 40); }
  /* the stub of a pc is shared by the exits whose frame state is the same (several IR instructions can come from one bytecode op) */
  for (cp = rt.p ? rt.p : ""; *cp; cp++) h = (h ^ (unsigned char)*cp) * 1099511628211ull;
  for (cp = rs.p ? rs.p : ""; *cp; cp++) h = (h ^ (unsigned char)*cp) * 1099511628211ull;
  cen_ok = 1;
  if (d > 0) for (j = 0; j < d; j++) if (c->in[chain[j]]->ieff) cen_ok = 0;
  cen_ep = d > 0 && cen_ok ? I0->eff : c->in[ii]->eff;
  h = (h ^ (unsigned)rpc) * 1099511628211ull;
  for (i = 0; i < c->nxit; i++) {
    x = &c->xit[i];
    if (getenv("QJS_AOT_REGION_STRETCHBOUND") != NULL && x->cep == cen_ep && x->cii == ii && x->d == d && cen_ep != 0) {
      /* a measurement of the generated unit's size only: the exits of a stretch share the first one's stub (not a runnable region) */
      free(rt.p);
      free(rs.p);
      return i;
    }
    if (x->h != h || x->rpc != rpc || x->d != d) continue;
    if (strcmp(x->root, rt.p ? rt.p : "") != 0 || strcmp(x->rest, rs.p ? rs.p : "") != 0) continue;
    free(rt.p);
    free(rs.p);
    t3_free(&t3);
    rg_cen_rec(c, i, cen_ep, d, cen_ok);
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
  x->cep = cen_ep;
  x->cii = ii;
  x->h = h;
  x->root = rt.p ? rt.p : strdup("");
  x->rest = rs.p ? rs.p : strdup("");
  x->t3 = NULL; x->t3pre = NULL; x->nofpc = d ? C1->par_read_pc : 0;
  if (t3.on && !t3.bad && (d > 0 || t3.nst >= rg_t3_min())) {
    RStr o = {0};
    x->t3pre = rt.p != NULL && t3pre > 0 ? strndup(rt.p, t3pre) : NULL;
    rs_add(&o, "    { static const uint16_t of_[] = { %s };\n", t3.offs.p ? t3.offs.p : "0");
    if (d > 0) rs_add(&o, "      static const RgFC fc_[%d] = {\n%s      };\n", d, t3.fcs.p ? t3.fcs.p : "");
    rs_add(&o, "      static const RgX3 x_ = { %d, %d, %d, %d, %du, %d, %s, of_ };\n", t3.nroot, d, d && C1->par_kind == 1, d ? C1->par_dst * 16 : 0, d ? C1->par_call_pc : 0, total, d ? "fc_" : "NULL");
    rs_add(&o, "      const JSValue v_[] = { %s };\n", t3.vals.p ? t3.vals.p : "JS_UNDEFINED");
    if (d > 0) rs_add(&o, "      JSObject *const co_[] = { %s };\n", t3.cos.p ? t3.cos.p : "NULL");
    rs_add(&o, "      int s_ = rg_t3b_%s(ctx, f, fr, &x_, v_, %s);\n", c->hex, d > 0 ? "co_" : "NULL");
    rs_add(&o, "      if (__builtin_expect(s_ != 0, 0)) { RG_FREE_OWNED(); if (s_ == 1) E_RET_G(%s, %d); return JS_EXCEPTION; } }\n", c->hex, x->nofpc);
    x->t3 = o.p;
    c->t3used = 1;
  }
  t3_free(&t3);
  rg_cen_rec(c, c->nxit, cen_ep, d, cen_ok);
  return c->nxit++;
}

/* The stretch rule: a guard that fails before the op it belongs to has taken any effect resumes at the start of its stretch, the state taken there, so the stretch's guards share one stub
   and no value defined inside the stretch is named by an exit; the generic body re-executes the pure ops in between. A stretch starts at a block and after every op that is not
   shape-neutral (a store, an allocation, a throw, a call made out of line, an inlined call whose body had one of these). QJS_AOT_REGION_NOSTRETCH=1 turns it off; a guard after an effect op
   (the facts test after a call) names its own pc through rg_xit_x. QJS_AOT_REGION_STRETCHKNOCK=1 (a knock-out build for the test) takes calls made out of line for pure. */
static int rg_stretch_on(void) {
  static int off = -1;
  if (off < 0) off = getenv("QJS_AOT_REGION_NOSTRETCH") != NULL;
  return !off;
}
static int rg_stretch_knock(void) {
  static int k = -1;
  if (k < 0) k = getenv("QJS_AOT_REGION_STRETCHKNOCK") != NULL;
  return k;
}
static void rg_mark_eff(RCtx *c, int ii) {
  if (rg_stretch_knock()) return;
  c->in[ii]->prev_eff = 1;
  c->in[ii]->ieff = 1;
}
static int rg_xit(RCtx *c, int ii, int pc) {
  if (c->xelig && rg_stretch_on() && c->in[ii]->snap >= 0) return c->in[ii]->snap;
  return rg_xit_x(c, ii, pc);
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

/* The C test "the shape SH is arm t's": the class's base shape, or (a class that gains fields late) any shape of its table. NULL arm shapes (unbound arms) match nothing. */
static void rg_arm_eq(RCtx *c, int gi, int t, const char *sh, char *buf, size_t cap) {
  const RGuard *G = &c->gd[gi];
  int kk = G->kcls[t];
  if (!G->crecv && kk >= 0 && kk < c->ncls && c->cls[kk].nsh > 1)
    snprintf(buf, cap, "(%s == rk_sa_%s_%d[%d] || (rk_sa_%s_%d[%d] != NULL && rg_inx_%s(rk_sx_%s[%d], %d, %s)))", sh, c->hex, gi, t, c->hex, gi, t, c->hex, c->hex, kk, c->cls[kk].nsh, sh);
  else
    snprintf(buf, cap, "%s == rk_sa_%s_%d[%d]", sh, c->hex, gi, t);
}
typedef struct RWalk {
  char (*cur)[40];        /* the root's current value per register (as C text), NULL inside an inlined instance */
  int root_pc;            /* the root pc exits resume at (the call pair being expanded), -1 at root level */
} RWalk;

static int rg_walk_inst_(RCtx *c, int ii, char (*rootcur)[40], const Q2Func *root);
/* Walk an instance; the effects its body took end the caller's stretch (unless the body took none), and the caller's guard rule is back as it was. */
static int rg_walk_inst(RCtx *c, int ii, char (*rootcur)[40], const Q2Func *root) {
  int saved = c->xelig, ok = rg_walk_inst_(c, ii, rootcur, root);
  c->xelig = saved;
  if (ii > 0 && c->in[ii]->ieff && c->in[ii]->par >= 0 && c->emit && !rg_stretch_knock()) { c->in[c->in[ii]->par]->prev_eff = 1; c->in[c->in[ii]->par]->ieff = 1; }
  return ok;
}
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

/* F21: where a receiver of a trusted fixed-slot access comes from. A trusted access reads a header and a slot with no tag or shape test, so the receiver must be an object of the
   class the access assumes. 1: it is (made by this region, a field the claims cover, the dispatch's `this`, or a root `this`, which the entry takes as an object). 2: it is the root
   argument *arg, which the entry tests once. 0: nothing proves it (a call result, an element, a merge of values, an argument of a caller that does not prove it). */
static char g_ro_why[40];
/* the receiver tests are off for the vkit knock-outs: QJS_AOT_REGION_NOF21GUARD=1, and QJS_AOT_FACTS_OFF=layout (the tests guard the same assumption as the layout fact, an instance of the class shape) */
static int rg_f21_off(void) { const char *fo = getenv("QJS_AOT_FACTS_OFF"); return getenv("QJS_AOT_REGION_NOF21GUARD") != NULL || getenv("QJS_AOT_REGION_NOLAYOUTDEP") != NULL || (fo != NULL && strstr(fo, "layout") != NULL); }
static int g_rg_chk;          /* the access just trusted has a receiver nothing proves: the emitter tests it first (rg_recv_guard) */
static unsigned g_rg_chk_km;  /* ... against the shapes of these classes (a bit per class-table index), any object when 0 */
static int rg_recv_origin(RCtx *c, const RInst *I, Q2ValueId v, unsigned *amask, int depth) {
  int guard, di, s0;
  const Q2Insn *d = NULL;
  if (depth > 16) { snprintf(g_ro_why, sizeof(g_ro_why), "too deep"); return 0; }
  for (guard = 0; guard < 64; guard++) {
    if ((int32_t)v < 0 || (int32_t)v >= I->q.nvalues) return 0;
    di = I->q.values[v].insn;
    if (di < 0 || di >= I->q.ninsns) break;
    d = &I->q.insns[di];
    if ((d->rop == ROP_move || d->rop == ROP_move_s || d->rop == ROP_move_xfer) && d->nuse >= 1) { v = d->use[0]; continue; }
    break;
  }
  if (di < 0 || di >= I->q.ninsns) { snprintf(g_ro_why, sizeof(g_ro_why), "block parameter"); return 0; }
  switch (d->rop) {
    case ROP_load_this: return 1;
    case ROP_get_this_field: case ROP_get_this_field2:
      if (d->ndef >= 2 && d->def[1] == v) return 1;   /* the copy of `this` */
      snprintf(g_ro_why, sizeof(g_ro_why), "field value");
      return 0;
    case ROP_construct: case ROP_new_object: case ROP_object_template: case ROP_array_from: return 1;
    case ROP_get_field2: case ROP_get_field2_own:
      if (d->ndef >= 2 && d->def[1] == v && d->nuse >= 1) return rg_recv_origin(c, I, d->use[0], amask, depth + 1);
      snprintf(g_ro_why, sizeof(g_ro_why), "field value");
      return 0;
    case ROP_get_field: case ROP_get_field_own: case ROP_get_field_chain: case ROP_get_field_chain_own:
      snprintf(g_ro_why, sizeof(g_ro_why), "field value");
      return 0;
    case ROP_get_element: snprintf(g_ro_why, sizeof(g_ro_why), "array element"); return 0;
    case ROP_get_global: snprintf(g_ro_why, sizeof(g_ro_why), "global value"); return 0;
    case ROP_call: case ROP_call_method: snprintf(g_ro_why, sizeof(g_ro_why), "call result"); return 0;
    case ROP_get_arg: case ROP_get_arg2: {
      char *e;
      long ci, vid;
      s0 = d->narg > 1 ? (int)d->arg[1].val : 0;
      if (d->rop == ROP_get_arg2 && d->ndef >= 2 && d->def[1] == v && d->narg > 3) s0 = (int)d->arg[3].val;
      if (s0 < 0 || s0 > 7) return 0;
      if (I->id == 0) {
        int k;
        for (k = 0; k < I->q.ninsns; k++) {
          int rp = I->q.insns[k].rop;
          if (rp == ROP_put_arg || rp == ROP_put_arg_xfer || rp == ROP_apply_arguments) { snprintf(g_ro_why, sizeof(g_ro_why), "assigned argument"); return 0; }   /* the entry value is not the value at the access */
        }
        *amask |= 1u << s0;
        return 2;
      }
      if (s0 >= I->nargs || I->arg[s0][0] != 'r') { snprintf(g_ro_why, sizeof(g_ro_why), "missing argument"); return 0; }
      ci = strtol(I->arg[s0] + 1, &e, 10);
      if (e == I->arg[s0] + 1 || e[0] != '_' || e[1] != 'v') return 0;
      vid = strtol(e + 2, &e, 10);
      if (*e != 0 || ci < 0 || ci >= c->nin || c->in[ci] == I) return 0;
      return rg_recv_origin(c, c->in[ci], (Q2ValueId)vid, amask, depth + 1);
    }
    default: e_rop_name(d->rop, g_ro_why, sizeof(g_ro_why)); return 0;
  }
}
static int rg_trust_field_n(const RInst *I, const Q2Insn *in, int write, const char *name, char *buf, size_t cap) {
  int slot, add = 0;
  unsigned cls = 0;
  int org = 1;
  unsigned oam = 0, km = 0;
  g_rg_chk = 0;
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
  if (g_rgc != NULL && !rg_f21_off() && in->nuse >= 1) {
    /* the access reads a header and a slot with no test: the receiver has to be proven an object of the assumed class (see rg_recv_origin) */
    Q2ValueId rv = in->use[0];
    int chain2 = 0;
    char n2_[48], n1_[48];
    if ((in->rop == ROP_get_field_chain || in->rop == ROP_get_field_chain_own) && name != NULL && rg_atom_n(in, 1, n2_, sizeof(n2_)) && strcmp(n2_, name) == 0 && rg_atom_n(in, 0, n1_, sizeof(n1_)) && strcmp(n1_, name) != 0) chain2 = 1;
    if (chain2) org = 0;   /* the receiver of the second read is the first read's value */
    else { g_ro_why[0] = 0; org = rg_recv_origin(g_rgc, I, rv, &oam, 0); }
    if (org == 0) {
      /* nothing proves the receiver: the access stays trusted (a fixed slot) behind a test of the receiver made where the access is emitted */
      if (!g_rgc->emit) {
        g_rgc->ra_flag++;
        if (getenv("QJS_AOT_REGION_DOMCHECK") != NULL) fprintf(stderr, "domcheck: %s pc %d .%s: the receiver (%s) is not proven an object of the assumed class; it is tested at the access\n", I->g->name ? I->g->name : "?", in->pc, name ? name : "?", g_ro_why[0] ? g_ro_why : "field of unproven kind");
      }
      g_rg_chk = 1;
    }
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
      if (!rg_cls_ok(g_rgc, &cl[u]) && g_rgc->cls[k].nsh <= 1) return 0;   /* bound for the per-site dispatch only (T13): its fields have no fixed slot */
      if (g_rgc->cls[k].nsh > 1) {
        /* a fixed-slot access to an object of a class with several shapes needs a shape test (the class has no layout the word could depend on): the shape join is the integrator's (E3) */
        if (getenv("QJS_AOT_REGION_MSHAPE_INITONLY") == NULL) { if (getenv("QJS_AOT_REGION_WHY")) fprintf(stderr, "why: %s pc %d .%s: class %s has %d shapes; a fixed-slot access needs the shape join\n", I->g->name, in->pc, name ? name : "?", cl[u].label, g_rgc->cls[k].nsh); return 0; }
        continue;   /* test knob: bind the tables, access the base fields unguarded (UNSOUND for a base field that is deleted or reordered) */
      }
      g_rgc->cls[k].need_layout = 1;
      km |= k < 32 ? 1u << k : 0;
    }
  }
  if (g_rg_chk) g_rg_chk_km = km;
  if (org == 2 && oam != 0) {
    /* the receiver comes from root arguments (directly, or through merges, expanded calls and inlined parameters): the entry tests each one (an object, of one of the classes assumed here) */
    int oa_;
    for (oa_ = 0; oa_ < 8; oa_++) if ((oam >> oa_) & 1u) {
      g_rgc->ra_mask |= 1u << oa_;
      if (km != 0) g_rgc->ra_cls[oa_] |= km; else g_rgc->ra_any[oa_] = 1;
    }
  }
  snprintf(buf, cap, "%du * QJS_AOT_SIZEOF_JSPROPERTY", slot);
  g_rg_plain = qjs_aot_bundle_cls_plain(cls);
  g_rg_cls = cls;
  return 1;
}
static int rg_trust_field(const RInst *I, const Q2Insn *in, int write, char *buf, size_t cap) { return rg_trust_field_n(I, in, write, NULL, buf, cap); }
/* F21: the test of a receiver nothing proves, emitted before the first trusted access on it in a stretch: an object, and of one of the classes the access assumes. */
static int rg_dominates(RInst *I, int a, int b) {
  Q2Func *fn = &I->q;
  int i, ch;
  if (a == b) return 1;
  if (a < 0 || b < 0 || a >= fn->nblocks || b >= fn->nblocks || fn->rpo == NULL || fn->rpo_pos == NULL) return 0;
  if (I->idom == NULL) {
    I->idom = malloc((size_t)fn->nblocks * sizeof(int));
    if (I->idom == NULL) abort();
    for (i = 0; i < fn->nblocks; i++) I->idom[i] = -1;
    if (fn->nrpo > 0) I->idom[fn->rpo[0]] = fn->rpo[0];
    do {
      ch = 0;
      for (i = 1; i < fn->nrpo; i++) {
        int bb = fn->rpo[i], nd = -1, k;
        if (fn->blocks[bb].resume) continue;
        for (k = 0; k < fn->blocks[bb].npreds; k++) {
          int pr = fn->blocks[bb].preds[k], x, y;
          if (pr < 0 || pr >= fn->nblocks || fn->rpo_pos[pr] < 0 || I->idom[pr] < 0) continue;
          if (nd < 0) { nd = pr; continue; }
          x = pr; y = nd;
          while (x != y) {
            while (fn->rpo_pos[x] > fn->rpo_pos[y]) { x = I->idom[x]; if (x < 0) break; }
            if (x < 0) break;
            while (fn->rpo_pos[y] > fn->rpo_pos[x]) { y = I->idom[y]; if (y < 0) break; }
            if (y < 0) { x = -1; break; }
          }
          nd = x;
          if (nd < 0) break;
        }
        if (nd >= 0 && I->idom[bb] != nd) { I->idom[bb] = nd; ch = 1; }
      }
    } while (ch);
  }
  for (i = 0; b != a && i < fn->nblocks; i++) { b = I->idom[b]; if (b < 0 || I->idom[b] == b) return b == a; }
  return b == a;
}
static void rg_recv_guard(RCtx *c, RInst *I, const char *val, const char *xl) {
  int i, k, first = 1;
  if (!g_rg_chk || !c->emit) return;
  for (i = 0; i < c->nck; i++) if (c->ck[i].inst == I->id && strcmp(c->ck[i].val, val) == 0 && rg_dominates(I, c->ck[i].blk, I->curb)) return;
  fprintf(c->o, "        if (__builtin_expect(JS_VALUE_GET_TAG(%s) != JS_TAG_OBJECT", val);
  if (g_rg_chk_km != 0) {
    fprintf(c->o, " || (");
    for (k = 0; k < 32; k++) if ((g_rg_chk_km >> k) & 1u) { fprintf(c->o, "%s*(const void *const *)((const char *)JS_VALUE_GET_PTR(%s) + QJS_AOT_OFF_JSOBJECT_SHAPE) != rk_s_%s[%d]", first ? "" : " && ", val, c->hex, k); first = 0; }
    fprintf(c->o, ")");
  }
  fprintf(c->o, ", 0)) goto %s;\n", xl);
  i = c->nck < 96 ? c->nck++ : 0;
  c->ck[i].inst = I->id; c->ck[i].blk = I->curb; snprintf(c->ck[i].val, sizeof(c->ck[i].val), "%s", val);
}
static void rg_verify(RCtx *c, RInst *I, const Q2Insn *in, int write, const char *val, const char *nm, const char *off, const char *xl) {
  rg_recv_guard(c, I, val, xl);
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

/* ---- T13: the per-site profile-set dispatch -----------------------------------------------------------------------------------------------------------
   A field access whose receivers are objects of one class that gains fields late (several shapes, so no fixed slot) is a compare of the receiver's shape with the shapes the PROFILE SAW
   AT THAT SITE, hottest first, each arm reading at that shape's own slot; any other shape leaves (before any effect). The shapes are rebuilt at init from the class's prototype and the
   ordered field lists (qjs_aot_class_shape, held), so soundness is the shape's: an object whose shape IS the held shape has exactly those plain data properties in that order. A site the
   profile saturated (it keeps four shapes per site and counted more fills) or whose shapes do not all hold the field is refused (this access only). One test per object per straight-line
   stretch: the matched shape index is kept in rgd_N, reused until an op that may add or delete a field (or a block start) ends the stretch.
   QJS_AOT_REGION_NODISP=1 turns it off. */
typedef struct RDisp { int n, sh[4], slot[4]; unsigned cls; int dcp; } RDisp;
static int g_disp_quiet;
static int rg_disp_on(void) { static int off = -1; if (off < 0) off = getenv("QJS_AOT_REGION_NODISP") != NULL; return !off; }
static int rg_dsh_idx(RCtx *c, int k, const QjsAotSiteShapes *S, int j) {
  int q, f;
  for (q = 0; q < c->ndsh; q++) {
    if (c->dsh[q].k != k || c->dsh[q].nn != S->nf[j]) continue;
    for (f = 0; f < S->nf[j] && strcmp(c->dsh[q].names[f], S->f[j][f]) == 0; f++) {}
    if (f == S->nf[j]) return q;
  }
  if (c->emit || c->ndsh >= 48) return -1;
  c->dsh[c->ndsh].k = k;
  c->dsh[c->ndsh].nn = S->nf[j];
  for (f = 0; f < S->nf[j]; f++) snprintf(c->dsh[c->ndsh].names[f], 40, "%s", S->f[j][f]);
  return c->ndsh++;
}
/* The constant path `Ctor.prototype.name` (the constructor's own path, then "prototype", then the field): bound and watched at init, so a class prototype's default value or
   its plain data property is a fact of the region. -1: the path does not fit. */
static int rg_proto_cp(RCtx *c, const char *ctor, const char *name) {
  char pn[6][40];
  const char *p = ctor;
  int pnn = 0;
  while (*p && pnn < 4) {
    const char *e = strchr(p, '.');
    size_t l = e ? (size_t)(e - p) : strlen(p);
    if (l == 0 || l >= 40) return -1;
    memcpy(pn[pnn], p, l); pn[pnn][l] = 0; pnn++;
    p = e ? e + 1 : p + l;
  }
  if (*p || pnn > 3 || strlen(name) >= 40) return -1;
  snprintf(pn[pnn++], 40, "prototype");
  snprintf(pn[pnn++], 40, "%s", name);
  return rg_cp_add(c, pn, pnn);
}
/* A store to a field the receiver's class gains late (QJS_AOT_REGION_MSHAPE): per shape of the class table, the slot of an existing field (kind 1) or the add of the field,
   the constant transition to the table's child shape (kind 2). Any other shape leaves. */
typedef struct RAdd { int k, n, cp, kind[16], sh[16], slot[16], to[16]; } RAdd;
static int rg_add_plan(RCtx *c, RInst *I, const Q2Insn *in, const char *name, RAdd *a) {
  QjsAotRecvClass cl[32];
  int nr, r, k = -1, t, s, nadd = 0, why = getenv("QJS_AOT_REGION_WHY") != NULL && !g_disp_quiet;
  const RCls *K;
  if (!rg_trust_on() || !qjs_aot_mshape_on() || I->g == NULL || name == NULL || qjsaot_region_mode() != 2) return 0;
  nr = qjs_aot_bundle_receivers_named(I->g, in->pc, 1, name, cl, 32);
  if (nr < 1) return 0;
  for (r = 0; r < nr; r++) {
    const char *ctor = rg_clctor(c, &cl[r]);
    int kk;
    if (!ctor[0] || cl[r].nfields < 0 || cl[r].nfields > 32) return 0;
    kk = rg_cls_add(c, cl[r].site, ctor, cl[r].nfields, 1);
    if (kk < 0 || (k >= 0 && kk != k)) return 0;
    k = kk;
  }
  K = &c->cls[k];
  if (K->nsh < 2) return 0;
  memset(a, 0, sizeof(*a));
  a->k = k;
  for (t = 0; t < K->nsh && a->n < 16; t++) {
    int idx = -1, f;
    for (f = 0; f < K->ms.nn[t]; f++) if (strcmp(K->ms.names[t][f], name) == 0) { idx = f; break; }
    if (idx >= 0) { a->kind[a->n] = 1; a->sh[a->n] = t; a->slot[a->n] = idx; a->to[a->n] = -1; a->n++; continue; }
    for (s = 1; s < K->nsh; s++) if (K->ms.parent[s] == t && strcmp(K->ms.add[s], name) == 0) break;
    if (s == K->nsh) continue;
    a->kind[a->n] = 2; a->sh[a->n] = t; a->slot[a->n] = K->ms.nn[t]; a->to[a->n] = s; a->n++; nadd++;
  }
  if (a->n == 0) return 0;
  a->cp = -1;
  if (nadd > 0) {
    a->cp = rg_proto_cp(c, K->ctor, name);   /* the prototype holds the field as a plain data property: the add shadows it, no setter runs */
    if (a->cp < 0) return 0;
  }
  if (why) fprintf(stderr, "why: %s pc %d .%s: store into class %s: %d shape arms, %d adds\n", I->g->name, in->pc, name, K->ctor, a->n, nadd);
  return 1;
}
static int rg_disp_plan(RCtx *c, RInst *I, const Q2Insn *in, const char *name, int which, int write, RDisp *d) {
  QjsAotRecvClass cl[32];
  QjsAotSiteShapes S;
  struct { int k; char hex[33]; } grp[8];
  int ngrp = 0;
  int nr, j, f, r, why = getenv("QJS_AOT_REGION_WHY") != NULL && !g_disp_quiet;
  int any_multi = 0, has_lit = 0;
  if (!rg_trust_on() || !rg_disp_on() || I->g == NULL || name == NULL || qjsaot_region_mode() != 2) return 0;
  nr = qjs_aot_bundle_receivers_named(I->g, in->pc, write ? 1 : 0, name, cl, 32);
  if (nr < 1) { if (why) fprintf(stderr, "why: %s pc %d .%s: dispatch: no receivers named (%d)\n", I->g->name, in->pc, name, nr); return 0; }
  /* the receiver classes, one per constructor (several allocation sites of one constructor and one construction field list are one class) */
  for (r = 0; r < nr; r++) {
    const char *ctor = rg_clctor(c, &cl[r]);
    int g, same = 0;
    if (!cl[r].one_shape) any_multi = 1;
    if (!ctor[0] && cl[r].label[0] == '{') { has_lit = 1; continue; }   /* an object literal: its shapes have Object.prototype (bound at init) */
    if (!ctor[0] || cl[r].nfields < 0 || cl[r].nfields > 32) { if (why) fprintf(stderr, "why: %s pc %d .%s: dispatch: class %s has no bindable constructor\n", I->g->name, in->pc, name, cl[r].label); return 0; }
    for (g = 0; g < ngrp; g++) {
      if (strcmp(c->cls[grp[g].k].ctor, ctor) != 0) continue;
      same = 1;
      if (cl[r].nfields != c->cls[grp[g].k].nn) return 0;
      for (f = 0; f < cl[r].nfields; f++) {
        char a_[40];
        if (!qjs_aot_bundle_site_field_name(cl[r].site, f, a_, sizeof(a_)) || strcmp(a_, c->cls[grp[g].k].names[f]) != 0) return 0;
      }
    }
    if (same) continue;
    if (ngrp >= 8) return 0;
    if (!qjs_aot_bundle_site_ctor_digest(cl[r].site, grp[ngrp].hex)) { if (why) fprintf(stderr, "why: %s pc %d .%s: dispatch: class %s has no constructor digest\n", I->g->name, in->pc, name, ctor); return 0; }
    grp[ngrp].k = rg_cls_add(c, cl[r].site, ctor, cl[r].nfields, cl[r].one_shape ? 0 : 2);
    if (grp[ngrp].k < 0) { if (why) fprintf(stderr, "why: %s pc %d .%s: dispatch: class %s not bindable\n", I->g->name, in->pc, name, ctor); return 0; }
    ngrp++;
  }
  /* a site whose receivers are single-shape classes only is the class table's (rg_poly_field) unless its profile saw more shapes: dispatch serves it too, with no layout to depend on */
  (void)any_multi;
  if (!qjs_aot_bundle_access_cls(I->g, in->pc, write ? 1 : 0, name, &d->cls)) { if (why) fprintf(stderr, "why: %s pc %d .%s: dispatch: no value class for the access (lost flow)\n", I->g->name, in->pc, name); return 0; }
  if (!qjs_aot_prof_access_shapes(I->g->digest, in->pc, which, name, write, &S)) { if (why) fprintf(stderr, "why: %s pc %d .%s: dispatch: the profile has no shapes for the access\n", I->g->name, in->pc, name); return 0; }
  if (S.saturated) { if (why) fprintf(stderr, "why: %s pc %d .%s: dispatch: the site's profile is saturated (%d shapes kept, more seen); this access is generic\n", I->g->name, in->pc, name, S.n); return 0; }
  for (j = 0; j < S.n; j++) {
    int g, k = -1;
    if (S.proto[j] == 2) {
      if (!has_lit) { if (why) fprintf(stderr, "why: %s pc %d .%s: dispatch: profile shape %d has Object.prototype but no receiver of the analysis is an object literal\n", I->g->name, in->pc, name, j); return 0; }
      d->sh[j] = rg_dsh_idx(c, -1, &S, j);
      if (d->sh[j] < 0) return 0;
      d->slot[j] = S.slot[j];
      continue;
    }
    for (g = 0; g < ngrp && k < 0; g++) {
      const RCls *K = &c->cls[grp[g].k];
      if (strcmp(S.digest[j], grp[g].hex) != 0 || S.nf[j] < K->nn) continue;
      for (f = 0; f < K->nn && strcmp(S.f[j][f], K->names[f]) == 0; f++) {}
      if (f == K->nn) k = grp[g].k;
    }
    if (k < 0) { if (why) fprintf(stderr, "why: %s pc %d .%s: dispatch: profile shape %d is of no receiver class of the analysis\n", I->g->name, in->pc, name, j); return 0; }
    d->sh[j] = rg_dsh_idx(c, k, &S, j);
    if (d->sh[j] < 0) return 0;
    d->slot[j] = S.slot[j];
  }
  d->n = S.n;
  d->dcp = -1;
  for (j = 0; j < S.n; j++) {
    if (d->slot[j] >= 0) continue;
    /* a shape the field is absent from: the read finds the class prototype's value, a constant path Ctor.prototype.name bound and watched at init */
    if (write || ngrp != 1 || has_lit) return 0;
    d->dcp = rg_proto_cp(c, c->cls[grp[0].k].ctor, name);
    if (d->dcp < 0) return 0;
    break;
  }
  if (why) fprintf(stderr, "why: %s pc %d .%s: dispatch on %d profile shape(s)\n", I->g->name, in->pc, name, d->n);
  return 1;
}
/* Whether an op leaves every object's property set as it was (so a matched shape stays matched). */
static int rg_shape_neutral(int rop) {
  switch (rop) {
    case ROP_load_const: case ROP_load_i8: case ROP_load_i32: case ROP_load_undefined: case ROP_load_null: case ROP_load_true: case ROP_load_false: case ROP_load_this:
    case ROP_move: case ROP_move_xfer: case ROP_get_local: case ROP_put_local: case ROP_get_arg: case ROP_put_arg: case ROP_get_global:
    case ROP_unary: case ROP_binary: case ROP_binary_imm: case ROP_to_number: case ROP_compare:
    case ROP_get_field: case ROP_get_field2: case ROP_get_element: case ROP_get_length:
    case ROP_branch: case ROP_branch_if: case ROP_branch_if_not: case ROP_return: case ROP_return_undefined:
    case ROP_get_local_field: case ROP_get_arg_field: case ROP_get_local_s0: case ROP_get_local_s1: case ROP_get_local_s2: case ROP_get_local_s3:
    case ROP_put_local_s0: case ROP_put_local_s1: case ROP_put_local_s2: case ROP_put_local_s3: case ROP_put_local_xfer: case ROP_put_arg_xfer:
    case ROP_binary_loc_reg: case ROP_binary_reg_loc: case ROP_binary_loc_loc: case ROP_local_rmw_imm:
    case ROP_branch_cmp_loc_loc: case ROP_branch_cmp_loc_reg: case ROP_branch_cmp_reg_loc: case ROP_branch_cmp_reg_reg: case ROP_branch_cmp_reg_reg_s: case ROP_binary_imm_s: case ROP_branch_s:
    case ROP_add_imm_s: case ROP_sub_imm_s: case ROP_add_imm_self_s: case ROP_sub_imm_self_s: case ROP_binary_cpool:
      return 1;
    default:
      break;
  }
  return rop == ROP_get_field_chain || rop == ROP_get_field_chain_own || rop == ROP_get_field_own || rop == ROP_get_field2_own || rop == ROP_get_this_field || rop == ROP_get_this_field2;
}
/* Emit statements (in a block of the caller's) that leave the slot offset of the accessed field in `off_` for the object `val`: from the matched shape of an earlier test of the same object
   when the stretch is the same, else by comparing the shape with the site's shapes. Any other object leaves through `xl`. */
static void rg_disp_off(RCtx *c, RInst *I, const char *val, int base, const RDisp *d, const char *xl) {
  FILE *o = c->o;
  int j, e = -1, var;
  for (j = 0; j < c->ndsx; j++) if (c->dsx[j].inst == I->id && c->dsx[j].base == base) { e = j; break; }
  fprintf(o, "        uint32_t off_;\n");
  if (e >= 0 && c->dsx[e].ep == c->dsx_ep) {
    var = c->dsx[e].var;
    fprintf(o, "        ");
    for (j = 0; j < d->n; j++) fprintf(o, d->slot[j] < 0 ? "if (rgd_%d == %d) off_ = 0xffffffffu; else " : "if (rgd_%d == %d) off_ = %du * QJS_AOT_SIZEOF_JSPROPERTY; else ", var, d->sh[j], d->slot[j] < 0 ? 0 : d->slot[j]);
    fprintf(o, "goto %s;\n", xl);
    return;
  }
  if (e >= 0) var = c->dsx[e].var;
  else {
    var = c->ndx++;
    if (c->ndsx < 96) { e = c->ndsx++; c->dsx[e].inst = I->id; c->dsx[e].base = base; c->dsx[e].var = var; }
  }
  if (e >= 0) c->dsx[e].ep = c->dsx_ep;
  fprintf(o, "        const void *sh_;\n        if (__builtin_expect(JS_VALUE_GET_TAG(%s) != JS_TAG_OBJECT, 0)) goto %s;\n", val, xl);
  fprintf(o, "        sh_ = *(const void *const *)((const char *)JS_VALUE_GET_PTR(%s) + QJS_AOT_OFF_JSOBJECT_SHAPE);\n        ", val);
  for (j = 0; j < d->n; j++) {
    if (d->slot[j] < 0) fprintf(o, "if (sh_ == rk_dx_%s[%d]) { off_ = 0xffffffffu; rgd_%d = %d; } else ", c->hex, d->sh[j], var, d->sh[j]);
    else fprintf(o, "if (sh_ == rk_dx_%s[%d]) { off_ = %du * QJS_AOT_SIZEOF_JSPROPERTY; rgd_%d = %d; } else ", c->hex, d->sh[j], d->slot[j], var, d->sh[j]);
  }
  fprintf(o, "goto %s;\n", xl);
}
/* a read of the field at the dispatched slot */
static void rg_disp_read(RCtx *c, RInst *I, const Q2Insn *in, int ii, int xpc, const char *val, int base, const char *dst, const RDisp *d) {
  char xd[32];
  int xi;
  if (!c->emit) return;
  xi = rg_xit(c, ii, xpc);
  rg_xlabel(c, xi, in->pc, "disp-miss", xd, sizeof(xd));
  fprintf(c->o, "        {\n");
  rg_disp_off(c, I, val, base, d, xd);
  if (d->dcp >= 0) { char cpt[64]; fprintf(c->o, "        %s = off_ == 0xffffffffu ? %s : *q2_slotp(%s, off_);\n        }\n", dst, rg_cp_text(c, d->dcp, cpt, sizeof(cpt)), val); return; }
  fprintf(c->o, "        %s = *q2_slotp(%s, off_);\n        }\n", dst, val);
}

/* Emit statements that leave `off_` (the property's slot offset) and `nsh_` (the held shape an add turns the object into, else NULL) for a store into the object `val`. */
static void rg_add_off(RCtx *c, const char *val, const RAdd *a, const char *xl) {
  FILE *o = c->o;
  int j;
  fprintf(o, "        uint32_t off_; const void *nsh_ = NULL, *sh_;\n        if (__builtin_expect(JS_VALUE_GET_TAG(%s) != JS_TAG_OBJECT, 0)) goto %s;\n", val, xl);
  fprintf(o, "        sh_ = *(const void *const *)((const char *)JS_VALUE_GET_PTR(%s) + QJS_AOT_OFF_JSOBJECT_SHAPE);\n        ", val);
  for (j = 0; j < a->n; j++) {
    if (a->kind[j] == 1) fprintf(o, "if (sh_ == rk_sx_%s[%d][%d]) off_ = %du * QJS_AOT_SIZEOF_JSPROPERTY; else ", c->hex, a->k, a->sh[j], a->slot[j]);
    else fprintf(o, "if (sh_ == rk_sx_%s[%d][%d]) { off_ = %du * QJS_AOT_SIZEOF_JSPROPERTY; nsh_ = rk_sx_%s[%d][%d]; } else ", c->hex, a->k, a->sh[j], a->slot[j], c->hex, a->k, a->to[j]);
  }
  fprintf(o, "goto %s;\n", xl);
}
/* The shape change of an add, before the slot is written: the object takes the new shape (a reference to it, the old one dropped) when it may be extended in place. */
static void rg_add_swap(RCtx *c, const char *val, const char *xl) {
  FILE *o = c->o;
  fprintf(o, "        if (nsh_ != NULL) {\n");
  fprintf(o, "          unsigned char *ob_ = (unsigned char *)JS_VALUE_GET_PTR(%s); uint16_t fl_ = *(const uint16_t *)(ob_ + QJS_AOT_OFF_JSOBJECT_FLAGS16); int np_ = *(const int *)((const unsigned char *)nsh_ + QJS_AOT_OFF_JSSHAPE_PROP_SIZE);\n", val);
  fprintf(o, "          if (__builtin_expect((fl_ & (QJS_AOT_JSOBJECT_EXTENSIBLE_MASK | QJS_AOT_JSOBJECT_NOADD_MASK)) != QJS_AOT_JSOBJECT_EXTENSIBLE_MASK, 0)) goto %s;\n", xl);
  fprintf(o, "          if (__builtin_expect(np_ != *(const int *)((const unsigned char *)sh_ + QJS_AOT_OFF_JSSHAPE_PROP_SIZE) && (unsigned)np_ > ((fl_ & QJS_AOT_JSOBJECT_INLINE_CAP_MASK) >> QJS_AOT_JSOBJECT_INLINE_CAP_SHIFT), 0)) goto %s;\n", xl);
  /* an object whose inline room is below its shape's property-array size (made without room for the fields it gains) has no slot for the add even when the size does not change */
  fprintf(o, "          { unsigned ic_ = (fl_ & QJS_AOT_JSOBJECT_INLINE_CAP_MASK) >> QJS_AOT_JSOBJECT_INLINE_CAP_SHIFT; if (__builtin_expect(ic_ != 0 && off_ >= ic_ * QJS_AOT_SIZEOF_JSPROPERTY, 0)) goto %s; }\n", xl);
  fprintf(o, "          ++*(int *)((unsigned char *)nsh_ - QJS_AOT_RC_BACK); *(const void **)(ob_ + QJS_AOT_OFF_JSOBJECT_SHAPE) = nsh_; --*(int *)((unsigned char *)sh_ - QJS_AOT_RC_BACK);\n        }\n");
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

/* Whether value v of fn is used after instruction k (an index): later in its block, on an edge out of it, or in any block reachable from it (rg_live_walk). */
static int rg_live_after(const Q2Func *fn, int k, Q2ValueId v, int reg) { return rg_live_walk(fn, k + 1, v, reg); }
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
/* Whether the block that defines value v of fn lies in a loop of fn. The instance flag `curloop` is set while a block is being emitted, which is after the stack-object decision
   asks: for a construct directly in a loop of the root function it was still 0 there, and an object carried over the back edge was taken for one that is not. */
static int rg_val_in_loop(const Q2Func *fn, int v) {
  if (v < 0 || v >= fn->nvalues) return 1;
  return rg_in_loop(fn, fn->values[v].block);
}
static int rg_carried_up(RCtx *c, int ii, int v) {
  for (;;) {
    RInst *I = c->in[ii];
    if (rg_trust_on()) {
      if ((I->curloop || (getenv("QJS_AOT_KO_CURLOOP") == NULL && rg_val_in_loop(&I->q, v))) && rg_carried(&I->q, (Q2ValueId)v)) return 1;
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
    { int q_; for (q_ = 0; q_ < g_nmatdef; q_++) if (g_matdef_fn[q_] == fn && g_matdef_v[q_] == (int)a) return 1; }   /* a heap object made of a stack object of the region */
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
  if (c->emit) rg_mark_eff(c, ii);
  if (!c->emit) { c->arr_mut = 1; rg_amut_note(c, c->in[ii], (int)call->use[1]); }
  RInst *I = c->in[ii];
  int gi = rg_guard(c, I->id, (int)call->use[1], call->pc, 1, "push");
  FILE *o = c->o;
  if (gi < 0) return rg_fail(c, "too many guards");
  c->gd[gi].intr = 1;
  if (c->emit) {
    char xl2[32];
    rg_xlabel(c, rg_xit_x(c, ii, exit_pc), call->pc, "push", xl2, sizeof(xl2));
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
static const Q2Func *g_mskip_fn[4];   /* where an object is moved to the heap, left out of a carried test: (kind, a, b, value) = (0, block, successor, argument) of an edge or (1, insn, -, result) of a move */
static int g_mskip[4][4], g_nmskip;
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
      if ((d->rop == ROP_move || d->rop == ROP_move_s || d->rop == ROP_move_xfer) && d->nuse >= 1 && d->ndef >= 1 && in[d->use[0]] && !in[d->def[0]]) {
        int q_;
        for (q_ = 0; q_ < g_nmskip && !(g_mskip_fn[q_] == fn && g_mskip[q_][0] == 1 && g_mskip[q_][1] == i); q_++) {}
        if (q_ < g_nmskip) continue;
        in[d->def[0]] = 1; changed = 1;
      }
    }
    for (b = 0; b < fn->nblocks; b++) {
      const Q2Block *B = &fn->blocks[b];
      if (fn->rpo_pos != NULL && fn->rpo_pos[b] < 0) continue;
      for (s = 0; s < B->nsucc; s++) {
        const Q2Block *T = &fn->blocks[B->succ_block[s]];
        for (j = 0; j < B->nedge_args[s] && j < T->nparams; j++) {
          if (!in[B->edge_arg[s][j]]) continue;
          {
            int q_;
            for (q_ = 0; q_ < g_nmskip && !(g_mskip_fn[q_] == fn && g_mskip[q_][0] == 0 && g_mskip[q_][1] == b && g_mskip[q_][2] == s && g_mskip[q_][3] == (int)B->edge_arg[s][j]); q_++) {}
            if (q_ < g_nmskip) continue;
          }
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
  if (c->emit) rg_mark_eff(c, ii);
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
    rg_xlabel(c, rg_xit_x(c, ii, exit_pc), call->pc, "pop", xl2, sizeof(xl2));
    fprintf(o, "    { /* %s.pop() at pc %d */\n", I->pfx, call->pc);
    if (held_) fprintf(o, "      if (__builtin_expect(rgon_ >= RG_OWNCAP, 0)) goto %s;\n", xl2);
    if (!rg_trust_on()) fprintf(o, "      if (__builtin_expect(!rg_sk_%s_%d, 0)) { if (rg_learn_%s_%d(ctx, %sv%u) < 0) goto %s; rg_sk_%s_%d = 1; }\n", c->hex, gi, c->hex, gi, I->pfx, (unsigned)call->use[1], xl2, c->hex, gi);
    fprintf(o, "      { JSValue o_; if (__builtin_expect(!q2_array_pop0(ctx, rg_pp_%s, %sv%u, &o_), 0)) goto %s; %s; rn_%d = o_; %sv%u = o_; }\n    }\n",
            c->hex, I->pfx, (unsigned)call->use[1], xl2, rel_, ki, I->pfx, (unsigned)call->def[0]);
  }
  return 1;
}

/* The C statement that moves the stack object of construct kq to the heap (its heap object lands in sks_m[ss]) and parks the heap objects in the owned list: with the stack child its
   field holds when that is alive (qjs_aot_stack_migrate_child), else alone. A failure leaves with the exception. */
static void rg_mig_stmt(const RCtx *c, int kq, char *buf, size_t cap) {
  const RCons *Kq = &c->cons[kq];
  if (Kq->chi > 0) {
    int cq = Kq->chi - 1;
    snprintf(buf, cap, "{ JSValue ha_; if (sk_%d) { JSValue hc_; if (!qjs_aot_stack_migrate_child(ctx, SKV_%d, SKV_%d, &sks_m[%d], &hc_)) { RG_FREE_OWNED(); return JS_EXCEPTION; } sk_%d = 0; rg_own_put(ctx, &rgo_, &rgon_, &rgoc_, hc_); } "
                   "else if (!qjs_aot_stack_migrate(ctx, SKV_%d, JS_UNDEFINED, &sks_m[%d], &ha_)) { RG_FREE_OWNED(); return JS_EXCEPTION; } sk_%d = 0; rg_own_put(ctx, &rgo_, &rgon_, &rgoc_, sks_m[%d]);",
            cq, kq, cq, Kq->ss, cq, kq, Kq->ss, kq, Kq->ss);
  } else
    snprintf(buf, cap, "{ JSValue ha_; if (!qjs_aot_stack_migrate(ctx, SKV_%d, JS_UNDEFINED, &sks_m[%d], &ha_)) { RG_FREE_OWNED(); return JS_EXCEPTION; } sk_%d = 0; rg_own_put(ctx, &rgo_, &rgon_, &rgoc_, sks_m[%d]);", kq, Kq->ss, kq, Kq->ss);
}
/* The text that releases the stack object of construct k0 and its Array (a no-op when neither is alive). */
static void rg_sk_release_text(RCtx *c, int k0, RStr *out) {
  int k1 = c->cons[k0].sk_pair;
  if (k1 >= 0) rs_add(out, "if (sk_%d || sk_%d) { qjs_aot_stack_release(ctx, sk_%d ? SKV_%d : JS_UNDEFINED, sk_%d ? SKV_%d : JS_UNDEFINED); sk_%d = 0; sk_%d = 0; } ", k0, k1, k0, k0, k1, k1, k0, k1);
  else if (c->cons[k0].fref == 0 && getenv("QJS_AOT_REGION_MASKCHECK") == NULL && getenv("QJS_AOT_REGION_NOINLREL") == NULL) rs_add(out, "if (sk_%d) { rg_skrel0(ctx, SKV_%d); sk_%d = 0; } ", k0, k0, k0);
  else if (c->cons[k0].fref != ~0u && (c->cons[k0].fref >> 8) == 0 && getenv("QJS_AOT_REGION_MASKCHECK") == NULL && getenv("QJS_AOT_REGION_NOINLREL") == NULL) rs_add(out, "if (sk_%d) { rg_skrelm(ctx, SKV_%d, %uu); sk_%d = 0; } ", k0, k0, c->cons[k0].fref, k0);
  else if (c->cons[k0].fref != ~0u) rs_add(out, "if (sk_%d) { qjs_aot_stack_release_m(ctx, SKV_%d, %uu); sk_%d = 0; } ", k0, k0, c->cons[k0].fref, k0);
  else rs_add(out, "if (sk_%d) { qjs_aot_stack_release(ctx, SKV_%d, JS_UNDEFINED); sk_%d = 0; } ", k0, k0, k0);
}
/* The objects the region's constructs made (`rn_K`) belong to the region until it returns or leaves: whoever kept one took a reference of its own. */
static void rg_free_owned(RCtx *c, FILE *o, const char *ind) {
  int k;
  for (k = 0; k < c->ncons; k++) {
    if (!c->cons[k].stack) fprintf(o, "%sJS_FreeValue(ctx, rn_%d);\n", ind, k);
  }
  /* the stack blocks are released as pairs (they are the region's frame); once migrated, the heap objects are the region's */
  if (c->nss > 0) fprintf(o, "%srg_skr_%s(ctx, &sks_b[0][0], sks_on, sks_h);\n", ind, c->hex);
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
/* The text that makes a virtual stack object's block real (its fields are written from the registers that hold them), as an exit does before it moves anything. */
static void rg_vmat_text(RCtx *c, int k0, RStr *out) {
  int j0;
  if (getenv("QJS_AOT_REGION_VFCHECK") != NULL) rs_add(out, "    if (sk_%d) rg_vmat_%s_%d(ctx, skb_%d, vs_%d", k0, c->hex, k0, k0, k0);
  else rs_add(out, "    if (vk_%d) { sk_%d = 1; vk_%d = 0; rg_vmat_%s_%d(ctx, skb_%d, vs_%d", k0, k0, k0, c->hex, k0, k0, k0);
  for (j0 = 0; j0 < c->cons[k0].nn; j0++) rs_add(out, ", vf_%d_%d", k0, j0);
  rs_add(out, getenv("QJS_AOT_REGION_VFCHECK") != NULL ? ");\n" : "); }\n");
}
/* The stack objects lent at the call (instruction `at` of instance ii): their container constructs, and for each the stack child it holds. Before the call a virtual one is made real, the callee
   reads the blocks; after it a block whose shape is NULL was moved to the heap by the callee (the forward word holds the heap object): the region takes the reference into its owned list and
   forgets the block. Returns how many were lent; `pre` and `post` get the text, `need` the lent parameters of the callee the call assumes. */
static int rg_lend_text(RCtx *c, int ii, int at, RStr *pre, RStr *post, unsigned *need) {
  int ki, m, n = 0;
  *need = 0;
  for (ki = 0; ki < c->ncons; ki++) {
    const RCons *K = &c->cons[ki];
    if (!K->stack || K->kind != 0) continue;
    for (m = 0; m < K->nls; m++) {
      int ch = K->chi > 0 ? K->chi - 1 : -1;
      if (K->ls[m][0] != ii || K->ls[m][1] != at) continue;
      if (K->virt || K->sk_pair >= 0) return -1;
      *need |= 1u << K->ls[m][3];
      if (ch >= 0 && c->cons[ch].virt) rg_vmat_text(c, ch, pre);
      if (getenv("QJS_AOT_REGION_BORROWRT") != NULL) {
        /* the run-time BORROWCHECK: a lent block that is still a block after the call (the callee did not move it) holds exactly the owner's reference: nothing kept it, and the child's either */
        rs_add(post, "        if (sk_%d && *(const void *const *)((const char *)JS_VALUE_GET_PTR(SKV_%d) + QJS_AOT_OFF_JSOBJECT_SHAPE) != NULL && *(const int *)((const char *)JS_VALUE_GET_PTR(SKV_%d) - 4) != (1 << 30)) { fprintf(stderr, \"BORROWCHECK: a lent block was kept (refcount %%d)\\n\", *(const int *)((const char *)JS_VALUE_GET_PTR(SKV_%d) - 4) - (1 << 30)); abort(); }\n", ki, ki, ki, ki);
        if (ch >= 0) rs_add(post, "        if (sk_%d && sk_%d && *(const void *const *)((const char *)JS_VALUE_GET_PTR(SKV_%d) + QJS_AOT_OFF_JSOBJECT_SHAPE) != NULL && *(const int *)((const char *)JS_VALUE_GET_PTR(SKV_%d) - 4) != (1 << 30)) { fprintf(stderr, \"BORROWCHECK: a lent child was kept (refcount %%d)\\n\", *(const int *)((const char *)JS_VALUE_GET_PTR(SKV_%d) - 4) - (1 << 30)); abort(); }\n", ki, ch, ch, ch, ch);
      }
      rs_add(post, "        if (__builtin_expect(sk_%d && *(const void *const *)((const char *)JS_VALUE_GET_PTR(SKV_%d) + QJS_AOT_OFF_JSOBJECT_SHAPE) == NULL, 0)) { JSValue lw_ = qjs_aot_stack_forward(SKV_%d);%s rg_own_put(ctx, &rgo_, &rgon_, &rgoc_, lw_); sk_%d = 0;", ki, ki, ki, getenv("QJS_AOT_REGION_BORROWDBG") != NULL ? " fprintf(stderr, \"adopt rc=%d\\n\", *(const int *)((const char *)JS_VALUE_GET_PTR(lw_) - 4));" : "", ki);
      if (ch >= 0) rs_add(post, " if (sk_%d && *(const void *const *)((const char *)JS_VALUE_GET_PTR(SKV_%d) + QJS_AOT_OFF_JSOBJECT_SHAPE) == NULL) { rg_own_put(ctx, &rgo_, &rgon_, &rgoc_, qjs_aot_stack_forward(SKV_%d)); sk_%d = 0; }", ch, ch, ch, ch);   /* (a child block the callee did not move, such as one the container no longer holds, stays a block: the owner releases it) */
      /* what names the block after the call (a copy made later, another call it is lent to, an exit's frame state) names the heap object now */
      { int w_; for (w_ = 0; w_ < K->nkvis; w_++) rs_add(post, " %sv%d = lw_;", c->in[K->kvis[w_][0]]->pfx, K->kvis[w_][1]); }
      rs_add(post, " }\n");
      n++;
    }
  }
  return n;
}
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
      if (sscanf(A->cur[reg], "%11[^v]v%u", px_, &vn_) == 2 && strcmp(px_, A->pfx) == 0 && kidx >= 0 && !rg_live_after(&A->q, kidx, (Q2ValueId)vn_, reg)) continue;
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
      if (sscanf(A->cur[reg], "%11[^v]v%u", px_, &vn_) == 2 && strcmp(px_, A->pfx) == 0 && kidx >= 0 && !rg_live_after(&A->q, kidx, (Q2ValueId)vn_, reg)) continue;
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
    rg_xlabel(c, rg_xit_x(c, ii, in->next), in->pc, "store-cap", xo, sizeof(xo));
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
static int rg_rty_typed(const RInst *I);
static int rg_virt_at(RCtx *c, const RInst *I, int u0, int thisop, const char *nm, int *fio);
static void rg_virt_put(RCtx *c, FILE *o, const RInst *I, int ki, int j, int u1, const char *recv);
static void rg_virt_get(RCtx *c, FILE *o, int ki, int j, const char *dst, const char *recv, int dty);
int g_vty_dv = -1;   /* the value whose typed local a virtual read just wrote: its load wrapper has nothing left to convert */
static void rg_virt_dst(const RInst *I, int dv, char *buf, size_t cap);   /* region_fn_var.inc: an instance whose result is a typed local */
static void rg_res_decl(FILE *o, const RInst *I);
static void rg_res_set(FILE *o, const RInst *I, int u0);
static void rg_res_get(FILE *o, const RInst *P, int dv, const char *resvar, const RInst *J);
static int rg_math_site(RCtx *c, RInst *I, const Q2Insn *call, const char *method, int *id, int *magic);   /* region_fn_native.inc */
static int rg_math_call(RCtx *c, int ii, const char *method, const Q2Insn *call, const char *resvar, int exit_pc, int id, int magic);   /* region_fn_native.inc */
/* QJS_AOT_REGION_NOINL=a-b,c-d (generator, a measurement knob): a callee whose source line lies in a listed range is not expanded; its call is made out of line like any call
   the bundle resolves to nothing. */
static int rg_noinl(const JSAotGenFunc *g) {
  const char *e = getenv("QJS_AOT_REGION_NOINL");
  if (e == NULL || g == NULL) return 0;
  while (*e) {
    char *end;
    long a = strtol(e, &end, 10), b = a;
    if (end == e) break;
    if (*end == '-') { e = end + 1; b = strtol(e, &end, 10); }
    if ((long)g->line >= a && (long)g->line <= b) return 1;
    e = *end == ',' ? end + 1 : end;
    if (*end != ',') break;
  }
  return 0;
}
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
  for (t = 0; t < nt; t++) if (rg_noinl(tg[t])) { nt = 0; break; }
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
      if (!rg_cls_ok(c, &cl[u]) && !(!cl[u].one_shape && rg_clctor(c, &cl[u])[0] && cl[u].nfields >= 0 && cl[u].nfields <= 32 && qjs_aot_mshape_on()))
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
      if (u == n && c->cls[kc[0]].nsh <= 1) { I->kcls = kc[0] + 1; c->cls[kc[0]].need_layout = 1; }
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
    J->key = I->key * 1000003ull + (uint64_t)call->pc * 131u + (uint64_t)t + 1u;
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
    if (J->ool && c->loopinl && nt == 1 && J->q.nvalues <= (getenv("QJS_AOT_REGION_LOOPINL_FN") != NULL && strcmp(J->g->name, getenv("QJS_AOT_REGION_LOOPINL_FN")) == 0 ? 800 : getenv("QJS_AOT_REGION_LOOPINL_MAX") ? atoi(getenv("QJS_AOT_REGION_LOOPINL_MAX")) : 400)) {
      /* a small loop callee with one target, not on the call chain already (a recursion): expanded in place, its loop a loop of the region */
      int a_, rec_ = 0;
      for (a_ = ii; a_ >= 0 && !rec_; a_ = c->in[a_]->par) { if (c->in[a_]->g == J->g) rec_ = 1; if (a_ == 0) break; }
      if (!rec_) J->ool = 0;
      if (getenv("QJS_AOT_REGION_WHY")) fprintf(stderr, "loopinl: %s (%d values) at pc %d: %s\n", J->g->name[0] ? J->g->name : "<anon>", J->q.nvalues, call->pc, rec_ ? "recursive, out of line" : "inlined");
    } else if (J->ool && c->loopinl && getenv("QJS_AOT_REGION_WHY")) fprintf(stderr, "loopinl: %s (%d values) at pc %d: out of line (%s)\n", J->g->name[0] ? J->g->name : "<anon>", J->q.nvalues, call->pc, nt != 1 ? "several targets" : "too large");
    if (rg_trust_on() && g_ncut > 0 && rg_cut_has(J->key)) J->ool = 1;   /* cut by the line cap */
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
          {
            char eq_[900], sx_[160];
            snprintf(sx_, sizeof(sx_), "*(const void *const *)((const char *)JS_VALUE_GET_PTR(%s) + QJS_AOT_OFF_JSOBJECT_SHAPE)", ov);
            rg_arm_eq(c, gi, 0, sx_, eq_, sizeof(eq_));
            rs_add(&c->hoist, "    if (__builtin_expect(JS_VALUE_GET_TAG(%s) != JS_TAG_OBJECT || !(%s), 0)) return aot_g_%s(ctx, f, fr, entry_pc);\n    rgmv_%d = 1;\n", ov, eq_, c->hex, k_);
          }
        c->in[base]->hver = 1;
        fprintf(o, "      switch (0) {\n");
      } else
      if (nt == 1 && !G->known && !thru && rg_inv_on(c) && (I->curloop || I->ploop) && rg_inv_has(c, val)) {
        /* one arm on a receiver that is the same object on every pass (its shape cannot change: the region adds no field to an object it did not make) */
        int k_ = c->nmemo++;
        {
          char eq_[900], sx_[160];
          snprintf(sx_, sizeof(sx_), "*(const void *const *)((const char *)JS_VALUE_GET_PTR(%s) + QJS_AOT_OFF_JSOBJECT_SHAPE)", val);
          rg_arm_eq(c, gi, 0, sx_, eq_, sizeof(eq_));
          fprintf(o, "      if (__builtin_expect(!rgmv_%d, 0)) { if (__builtin_expect(JS_VALUE_GET_TAG(%s) != JS_TAG_OBJECT || !(%s), 0)) goto %s; rgmv_%d = 1; }\n", k_, val, eq_, xl2, k_);
        }
        fprintf(o, "      switch (0) {\n");
      } else if (nt == 1 && !G->known) {
        /* one arm: a compare and an exit (no arm index to select) */
        if (thru) fprintf(o, "      if (__builtin_expect(JS_VALUE_GET_TAG(%s) != JS_TAG_OBJECT || JS_VALUE_GET_PTR(%s) != (const void *)rg_tf_%s_%d[0], 0)) goto %s;\n", val, val, c->hex, gi, xl2);
        else {
          char eq_[900], sx_[160];
          snprintf(sx_, sizeof(sx_), "*(const void *const *)((const char *)JS_VALUE_GET_PTR(%s) + QJS_AOT_OFF_JSOBJECT_SHAPE)", val);
          rg_arm_eq(c, gi, 0, sx_, eq_, sizeof(eq_));
          fprintf(o, "      if (__builtin_expect(JS_VALUE_GET_TAG(%s) != JS_TAG_OBJECT || !(%s), 0)) goto %s;\n", val, eq_, xl2);
        }
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
        for (tt = 0; tt < nt; tt++) { char eq_[900]; rg_arm_eq(c, gi, tt, "sh_", eq_, sizeof(eq_)); fprintf(o, "        %sif (%s) gix_%d = %d;\n", tt ? "else " : "", eq_, gi, tt); }
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
        if (c->emit) rg_mark_eff(c, ii);
        /* an out-of-line call (TRUST): the callee runs its own compiled body. Every value the region's frames hold is a borrow the callee could free, so
           each is kept alive (a reference held to the region's end); the result is owned and held the same way; after the call the word is tested
           (the callee may have broken a fact) and a broken word leaves after the call with the result in place */
        int ka, reg, rk_ = -1;
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
          {
            /* stack objects lent to the callee: made real before the call, adopted back after it (the callee may have moved them to the heap) */
            RStr lpre = {0}, lpost = {0};
            unsigned lneed = 0;
            int nl = c->emit ? rg_lend_text(c, ii, (int)(call - I->q.insns), &lpre, &lpost, &lneed) : 0;
            if (nl < 0) { free(lpre.p); free(lpost.p); return rg_fail(c, "pc %d: a lent object is virtual or paired with an Array", call->pc); }
            if (nl > 0) {
              if (getenv("QJS_AOT_REGION_NODIRECTCALL") != NULL) return rg_fail(c, "pc %d: a lent object needs the direct call", call->pc);
              rg_lent_assume(hx_, c->hex, lneed);
              fprintf(o, "#if !defined(Q2_NATIVE_%s) || !defined(RG_LENT_%s) || (RG_LENT_%s & %uu) != %uu\n#error \"a stack object is lent to a function that does not take it\"\n#endif\n", hx_, hx_, hx_, lneed, lneed);
              if (lpre.p) fputs(lpre.p, o);
            }
            rg_ool_live(c, ii, call, o);
            rg_emit_cap(c, ii, call->pc, o);
            {
              int q_;
              rk_ = -1;
              for (q_ = 0; q_ < c->ncons; q_++) if (c->cons[q_].rslot && c->cons[q_].stack && c->cons[q_].inst == ii && c->cons[q_].pc == call->pc) rk_ = q_;
            }
            if (rk_ >= 0 && getenv("QJS_AOT_REGION_NODIRECTCALL") == NULL) {
              /* the result may come back in a block of this frame: an empty shell goes to the callee's aot_nr_ entry (when it has one); the previous incarnation of the block is released first */
              RStr rt_ = {0};
              rg_sk_release_text(c, rk_, &rt_);
              fprintf(o, "#ifdef Q2_NATIVE_%s\n#ifdef RG_RETSLOT_%s\n        { %s JSValue rsv_ = qjs_aot_stack_shell(skb_%d, sizeof skb_%d, 8u);\n          JSValue r_ = %s ? aot_nr_%s(ctx, f, (JSObject *)(%s), %s, av_, %d, &rsv_) : JS_Call(ctx, JS_MKPTR(JS_TAG_OBJECT, %s), %s, %d, av_);\n#else\n",
                      hx_, hx_, rt_.p ? rt_.p : "", rk_, rk_, "1", hx_, J->co, J->self, J->nargs, J->co, J->self, J->nargs);
              fprintf(o, "        { JSValue r_ = aot_n_%s(ctx, f, (JSObject *)(%s), %s, av_, %d);\n#endif\n#else\n", hx_, J->co, J->self, J->nargs);
              free(rt_.p);
            } else
            fprintf(o, "#ifdef Q2_NATIVE_%s\n        { JSValue r_ = %s ? aot_n_%s(ctx, f, (JSObject *)(%s), %s, av_, %d) : JS_Call(ctx, JS_MKPTR(JS_TAG_OBJECT, %s), %s, %d, av_);\n#else\n",
                    hx_, getenv("QJS_AOT_REGION_NODIRECTCALL") ? "0" : "1", hx_, J->co, J->self, J->nargs, J->co, J->self, J->nargs);
            fprintf(o, "        { JSValue r_ = JS_Call(ctx, JS_MKPTR(JS_TAG_OBJECT, %s), %s, %d, av_);\n#endif\n", J->co, J->self, J->nargs);
            if (nl > 0 && lpost.p) fputs(lpost.p, o);
            free(lpre.p); free(lpost.p);
          }
        }
        fprintf(o, "          if (__builtin_expect(JS_IsException(r_), 0)) { RG_FREE_OWNED(); return JS_EXCEPTION; }\n");
        if (rk_ >= 0) { char hy_[33]; rg_hexof(J->g, hy_); fprintf(o, "#ifdef RG_RETSLOT_%s\n          if (JS_VALUE_GET_TAG(r_) == JS_TAG_OBJECT && JS_VALUE_GET_PTR(r_) == JS_VALUE_GET_PTR(SKV_%d)) sk_%d = 1; else\n#endif\n", hy_, rk_, rk_); }
        fprintf(o, "          rg_own_put(ctx, &rgo_, &rgon_, &rgoc_, r_); %s = r_; }\n", resvar);
        reg = (int32_t)call->def[0] >= 0 && (int32_t)call->def[0] < I->q.nvalues ? I->q.values[call->def[0]].reg : -1;
        if (I->cur != NULL && reg >= 0 && reg < 256) snprintf(I->cur[reg], 40, "%s", resvar);
        rg_xlabel(c, rg_xit_x(c, ii, call->next), call->pc, "ool-facts", xo, sizeof(xo));
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
    if (nt == 1 && c->in[base]->ool && rg_retslot_on() && (int32_t)call->def[0] >= 0 && (int32_t)call->def[0] < I->q.nvalues) {
      /* the result of an out-of-line call: a block of this frame the callee may return its object in (decided like any stack object; the call passes a shell for it) */
      int kr = rg_cons(c, ii, call->pc);
      if (kr >= 0) {
        RCons *R = &c->cons[kr];
        R->kind = 0; R->direct = 1; R->esc = 0; R->rslot = 1; R->nn = 0; R->accroom = 8; R->site = -1;
        snprintf(R->gname, sizeof(R->gname), "<result>");
      }
    }
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
      fprintf(o, "            double d_ = (double)JS_VALUE_GET_INT(%s) / (double)JS_VALUE_GET_INT(%s); int32_t t_ = d_ >= -2147483648.0 && d_ <= 2147483647.0 ? (int32_t)d_ : 0;\n", a, b);
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
static int rg_walk_inst_(RCtx *c, int ii, char (*rootcur)[40], const Q2Func *root) {
  RInst *I = c->in[ii];
  const Q2Func *fn = &I->q;
  FILE *o = c->o;
  int is_root = ii == 0, b, k, i;
  char xl[32];
  I->pend_end = 0;
  I->npend = 0; I->lastcall = I->nextcall = -1;
  I->eff = 0; I->ieff = 0; I->snap = -1; I->prev_eff = 0;
  { int q_; for (q_ = 0; q_ < 128; q_++) { I->bsnap[q_] = -1; I->bpe[q_] = 0; } }
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
    c->dsx_ep++;   /* a block start ends the stretch of a shape test (T13) */
    I->curb = b;
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
    if (c->emit) rg_cen_point(c, I, B->start_pc);
    for (k = 0; k < B->ninsns; k++) {
      int idx = B->first_insn + k;
      const Q2Insn *in = &fn->insns[idx];
      int dv = in->ndef >= 1 ? (int)in->def[0] : -1, dv2 = in->ndef >= 2 ? (int)in->def[1] : -1;
      int u0 = in->nuse >= 1 ? (int)in->use[0] : -1, u1 = in->nuse >= 2 ? (int)in->use[1] : -1;
      int xpc, np_ = 0, pj_;
      char fsmv_[64]; char migs_[1400];
      I->curidx = idx;
      /* T13: an op that may add or delete a field ends the stretch of the shape tests; a store of a field the profile shapes of its site all hold is a slot store and keeps it */
      if (in->rop == ROP_put_field || in->rop == ROP_put_field_xfer) {
        RDisp wd_; char wn_[40]; int keep_ = 0;
        if (!I->fresh && rg_atom(in, wn_, sizeof(wn_))) { g_disp_quiet = 1; keep_ = rg_disp_plan(c, I, in, wn_, 0, 1, &wd_); g_disp_quiet = 0; }
        if (!keep_) c->dsx_ep++;
      } else if (!rg_shape_neutral(in->rop)) c->dsx_ep++;
      if (c->emit && !rg_shape_neutral(in->rop)) {
        const char *ce_ = getenv("QJS_AOT_REGION_EPCENSUS");
        int eff_ = 1;
        if (ce_ != NULL && ce_[0] == '2') {
          /* refined: an inlined call is not itself an effect (its callee's ops are), a store to the object under construction is not */
          if (in->rop == ROP_call || in->rop == ROP_call_method || in->rop == ROP_construct || in->rop == ROP_apply_arguments) eff_ = 0;
          if ((in->rop == ROP_put_field || in->rop == ROP_put_field_xfer) && I->fresh && in->nuse >= 1 && rg_is_this(fn, in->use[0])) eff_ = 0;
        }
        if (eff_) { rg_cen_point(c, I, in->pc); I->ieff = 1; }
      }
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
      if (c->emit && rg_stretch_on() && I->cur != NULL) {
        /* the stretch rule (rg_xit): a snapshot at a block's start and after every op that took an effect; this op's pre-effect guards resume there */
        int eff_ = !rg_shape_neutral(in->rop);
        if (in->rop == ROP_call || in->rop == ROP_call_method) eff_ = 0;   /* an inlined call has the effects of its body (marked when the body is walked); one made out of line marks itself */
        if ((in->rop == ROP_put_field || in->rop == ROP_put_field_xfer) && I->fresh && in->nuse >= 1 && rg_is_this(fn, in->use[0])) eff_ = 0;   /* a store to the object being constructed */
        int newsnap_ = 0;
        if (k == 0) {
          /* a block entered from one earlier block only continues that block's stretch (its guards may resume where the stretch began); a join or a loop head starts one */
          int p_ = B->npreds == 1 && B->preds != NULL ? B->preds[0] : -1;
          if (p_ >= 0 && p_ < 128 && p_ != b && fn->rpo_pos != NULL && fn->rpo_pos[p_] >= 0 && fn->rpo_pos[p_] < fn->rpo_pos[b] && I->bsnap[p_] >= 0) { I->snap = I->bsnap[p_]; I->prev_eff = I->bpe[p_]; }
          else newsnap_ = 1;
        }
        if (I->prev_eff) newsnap_ = 1;
        if (newsnap_) { I->snap = rg_xit_x(c, ii, xpc); }
        I->prev_eff = 0;
        if (eff_) { I->prev_eff = 1; I->ieff = 1; }
        c->xelig = !eff_;
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
          if (c->emit) {
            int vf_ = -1, vk_ = rg_virt_at(c, I, -1, 1, nm, &vf_);
            if (vk_ >= 0) { char dd_[40]; rg_virt_dst(I, dv, dd_, sizeof(dd_)); rg_virt_get(c, o, vk_, vf_, dd_, I->self, rg_tyof(I, dv)); fprintf(o, "        %sv%d = %s;\n", I->pfx, dv2, I->self); break; }
          }
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
              if (tr_) { rg_verify(c, I, in, 0, I->self, nm, to_, xl); rg_inv_read(c, I, o, dv, I->self, to_, nm); }
              else fprintf(o, "        %sv%d = *q2_slotp(%s, rg_off_%s_%d[gix_%d][%d]);\n", I->pfx, dv, I->self, c->hex, gq, gq, qq);
              if (rg_inv_has(c, I->self)) { snprintf(tmp_, sizeof(tmp_), "%sv%d", I->pfx, dv2); rg_inv_add(c, tmp_); rg_orig_set(c, tmp_, rg_orig(c, I->self)); }
            }
          }
          break;
        }
        case ROP_get_field_chain: case ROP_get_field_chain_own: {
          /* d0 = recv.a1 (a field), d1 = d0.a2 (a method read that the call right after uses) */
          char n1[40], n2[40];
          int call_idx = -1, j, glob_ = 0, disp1_ = 0;
          RDisp d1_, d2_;
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
          if (!glob_ && rg_trust_on() && !rg_trust_field_n(I, in, 0, n1, tmp_, sizeof(tmp_))) disp1_ = rg_disp_plan(c, I, in, n1, 0, 0, &d1_);   /* T13 */
          if (glob_) {
          } else if (disp1_) {
            char val_[40], dst_[40];
            snprintf(val_, sizeof(val_), "%sv%d", I->pfx, u0);
            snprintf(dst_, sizeof(dst_), "%sv%d", I->pfx, dv);
            rg_disp_read(c, I, in, ii, xpc, val_, rg_base(fn, (Q2ValueId)u0), dst_, &d1_);
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
              if (tr_) { rg_verify(c, I, in, 0, val, n1, to_, xl); rg_inv_read(c, I, o, dv, val, to_, n1); }
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
            } else if (rg_trust_on() && !rg_trust_field_n(I, in, 0, n2, tmp_, sizeof(tmp_)) && rg_disp_plan(c, I, in, n2, 1, 0, &d2_)) {   /* T13 */
              char val_[40], dst_[40];
              snprintf(val_, sizeof(val_), "%sv%d", I->pfx, dv);
              snprintf(dst_, sizeof(dst_), "%sv%d", I->pfx, dv2);
              rg_disp_read(c, I, in, ii, xpc, val_, rg_base(fn, (Q2ValueId)dv), dst_, &d2_);
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
              if (tr2_) { rg_verify(c, I, in, 0, val, n2, t2_, xl); rg_inv_read(c, I, o, dv2, val, t2_, n2); fprintf(o, "        }\n"); }
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
          if (c->emit && (in->rop == ROP_get_field || in->rop == ROP_get_field_own)) {
            int vf_ = -1, vk_ = rg_virt_at(c, I, u0, 0, nm, &vf_);
            if (vk_ >= 0) { char rv_[40], dd_[40]; snprintf(rv_, sizeof(rv_), "%sv%d", I->pfx, u0); rg_virt_dst(I, dv, dd_, sizeof(dd_)); rg_virt_get(c, o, vk_, vf_, dd_, rv_, rg_tyof(I, dv)); break; }
          }
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
          if (rg_trust_on() && !rg_trust_field(I, in, 0, tmp_, sizeof(tmp_))) {
            /* T13: one class that gains fields late: a compare with the shapes the profile saw at this site */
            RDisp dd_;
            if (rg_disp_plan(c, I, in, nm, 0, 0, &dd_)) {
              char val_[40], dst_[40];
              snprintf(val_, sizeof(val_), "%sv%d", I->pfx, u0);
              snprintf(dst_, sizeof(dst_), "%sv%d", I->pfx, dv);
              rg_disp_read(c, I, in, ii, xpc, val_, rg_base(fn, (Q2ValueId)u0), dst_, &dd_);
              break;
            }
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
              if (tr_) { rg_verify(c, I, in, 0, val, nm, to_, xl); rg_inv_read(c, I, o, dv, val, to_, nm); }
              else fprintf(o, "        %sv%d = *q2_slotp(%s, rg_off_%s_%d[gix_%d][%d]);\n", I->pfx, dv, val, c->hex, gq, gq, qq);
            }
            if (!(is_this && I->owner_g >= 0)) fprintf(o, "        }\n");
          }
          break;
        }
        case ROP_put_field: case ROP_put_field_xfer: {
          char nm[40];
          RDisp wd_;
          RAdd ad_;
          int wdisp_ = 0;
          if (u0 < 0 || u1 < 0 || !rg_atom(in, nm, sizeof(nm))) return rg_fail(c, "pc %d: put_field", in->pc);
          if (I->fresh && I->cons > 0 && rg_is_this(fn, u0)) {
            /* a field of the object this region is constructing: nothing else can see it, so the store is no effect (an exit discards the object), and it is made
               by the engine's own define on the fresh object (the prototype chain was stated to hold no such property) */
            RCons *K = &c->cons[I->cons - 1];
            int ni;
            for (ni = 0; ni < K->nn && strcmp(K->names[ni], nm) != 0; ni++) {}
            if (c->emit && K->virt && ni < K->nn) {
              char rv_[40];
              snprintf(rv_, sizeof(rv_), "%sv%d", I->pfx, u0);
              rg_virt_put(c, o, I, I->cons - 1, ni, u1, rv_);
              break;
            }
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
          if (c->emit) {
            int vf_ = -1, vk_ = rg_virt_at(c, I, u0, 0, nm, &vf_);
            if (vk_ >= 0) { char rv_[40]; snprintf(rv_, sizeof(rv_), "%sv%d", I->pfx, u0); rg_virt_put(c, o, I, vk_, vf_, u1, rv_); break; }
          }
          if (rg_trust_on() && !rg_trust_field(I, in, 1, tmp_, sizeof(tmp_))) wdisp_ = rg_disp_plan(c, I, in, nm, 0, 1, &wd_);
          if (!wdisp_ && rg_trust_on() && !rg_trust_field(I, in, 1, tmp_, sizeof(tmp_)) && rg_add_plan(c, I, in, nm, &ad_)) wdisp_ = 2;
          if (wdisp_) g_rg_cls = wdisp_ == 2 ? (512u | 32u | 64u) : wd_.cls;   /* the kinds the field may hold, for the narrowing of a double store */
          if (!c->emit) {
            if (rg_borrow_live(c, ii, in)) rg_note_park(c, ii);
            if (!wdisp_ && !rg_trust_field(I, in, 1, tmp_, sizeof(tmp_)) && !rg_note_field(c, I, u0, nm, 0, 1, rg_is_this(fn, u0))) return rg_fail(c, "pc %d: .%s requirements", in->pc, nm);
          } else {
            int is_this = rg_is_this(fn, u0);
            int gq = wdisp_ ? -1 : is_this && I->owner_g >= 0 ? I->owner_g : rg_guard(c, I->id, u0, -1, 0, NULL);
            int qq = gq >= 0 ? rg_qidx(&c->gd[gq], is_this && I->owner_g >= 0 ? I->owner_t : 0, nm) : -1;
            char val[40], off[96];
            if (!wdisp_ && qq < 0 && !rg_trust_field(I, in, 1, tmp_, sizeof(tmp_))) return rg_fail(c, "pc %d: .%s has no verified position", in->pc, nm);
            snprintf(val, sizeof(val), "%sv%d", I->pfx, u0);
            fprintf(o, "        {\n");
            if (wdisp_) {
              char xd_[32];
              rg_xlabel(c, rg_xit(c, ii, xpc), in->pc, "disp-miss", xd_, sizeof(xd_));
              if (wdisp_ == 2) rg_add_off(c, val, &ad_, xd_);
              else rg_disp_off(c, I, val, rg_base(fn, (Q2ValueId)u0), &wd_, xd_);
              snprintf(off, sizeof(off), "off_");
            } else if (rg_trust_field(I, in, 1, off, sizeof(off))) rg_verify(c, I, in, 1, val, nm, off, xl);
            else {
              if (!(is_this && I->owner_g >= 0)) rg_lookup(c, gq, val, xl, "        ");
              snprintf(off, sizeof(off), "rg_off_%s_%d[gix_%d][%d]", c->hex, gq, gq, qq);
            }
            if (!wdisp_ && rg_trust_field(I, in, 1, tmp_, sizeof(tmp_)) && g_rg_plain && (rg_trust_on() || getenv("QJS_AOT_REGION_LEAN") != NULL) && (!rg_store_narrow(I, u1, g_rg_cls) || rg_f64_knob("QJS_AOT_REGION_F64NOCLAIM") || getenv("QJS_AOT_REGION_F64NARROWEXIT") != NULL)) {
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
            if (wdisp_ == 2) {
              char xc2[32], xg2[32];
              rg_xlabel(c, rg_xit(c, ii, xpc), in->pc, "store-oldlay", xc2, sizeof(xc2));
              rg_xlabel(c, rg_xit(c, ii, xpc), in->pc, "store-grow", xg2, sizeof(xg2));
              fprintf(o, "        if (nsh_ != NULL && __builtin_expect(Q2_ENTERS_LAYOUT((const unsigned char *)sh_), 0)) goto %s;\n", xc2);
              rg_add_swap(c, val, xg2);
              fprintf(o, "        { JSValue *sp_ = q2_slotp(%s, %s); JSValue old_ = nsh_ != NULL ? JS_UNDEFINED : *sp_; q2_dup_p(%s); *sp_ = %s;\n", val, off, vs_, vs_);
              fprintf(o, "        if (nsh_ != NULL) Q2_ADD_FILLED(ctx, %s, nsh_);\n", val);
            } else
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
          if (c->emit && getenv("QJS_AOT_REGION_LENCOUNT") == NULL) {   /* also without TRUST: the element count is not the length of `new Array(n)` */
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
          if (need_val) {
            /* the value is read from the binding every time (also without TRUST) (a global such as `planner` changes between runs; nothing is assumed about it beyond what the
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
          /* a construct the profile never ran, reading this global, is an exit that spills the callee register: a value the region never made would be handed to the generic body as
             the callee. The load is then the cold point itself (the exit resumes at the load). */
          if (dv >= 0 && rg_cold(I, in) && getenv("QJS_AOT_REGION_NOCOLDGLOBAL") == NULL) {   /* (the env var brings the old behaviour back, for the knock-out of this case) */
            int ncold_ = 0, nhot_ = 0;
            for (j2 = 0; j2 < fn->ninsns; j2++) {
              const Q2Insn *cj = &fn->insns[j2];
              if (cj->rop == ROP_construct && cj->nuse >= 1 && rg_base(fn, cj->use[0]) == dv) { if (rg_cold(I, cj)) ncold_++; else nhot_++; }
            }
            if (ncold_ > 0 && nhot_ == 0) {
              if (c->emit) fprintf(o, "        goto %s; /* get_global: its construct was never executed in the profile */\n", xl);
              break;
            }
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
          if (nt1 == 1 && rg_noinl(tg1[0])) nt1 = 0;
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
            if (!c->emit) K->site = rg_trust_on() ? (qjs_aot_bundle_alloc_site(I->g, in->pc) > 0 || getenv("QJS_AOT_REGION_NOLATESITE") != NULL || getenv("QJS_AOT_REGION_NOCAP") != NULL ? qjs_aot_bundle_alloc_site(I->g, in->pc) : qjs_aot_bundle_alloc_site_late(I->g, in->pc)) : -1;
            if (J->wrap) {
              /* the forwarded call dispatches on the class of the object being built: the construct's own allocation site */
              int ws_ = K->site > 0 ? K->site : (rg_trust_on() ? qjs_aot_bundle_alloc_site_any(I->g, in->pc) : 0);   /* a class that gains fields later has no single shape but its construct site still has its class */
              if (ws_ <= 0) return rg_fail(c, "pc %d: the forwarding constructor's object has no allocation site", in->pc);
              J->cls1 = ws_ + 1;
              if (getenv("QJS_AOT_REGION_FWDONESITE") != NULL) {
                /* vkit knock: the old rule, one forwarded construct site per region (the constructor digest is shared by every class) */
                int k_;
                for (k_ = 0; k_ < c->ncons; k_++) if (k_ != ki && c->cons[k_].fwd) return rg_fail(c, "pc %d: the forwarding constructor already builds an object at another construct site (FWDONESITE)", in->pc);
              }
              K->fwd = 1;
            }
            if (c->emit && held_) fprintf(o, "        if (__builtin_expect(rgon_ >= RG_OWNCAP, 0)) goto %s;   /* the owned list is full: leave before the site, the generic body goes on */\n", xl);
            if (c->emit && getenv("QJS_AOT_FASTSTATS") != NULL) fprintf(o, "        rgn_%s[%d]++;\n", c->hex, ki);
            if (c->emit && K->stack && K->virt && getenv("QJS_AOT_REGION_VFCHECK") == NULL) {
              /* a virtual object has no block until something observes it: its shape word is the one thing the region reads (the guards of the methods it is the receiver of); the fields are locals,
                 and the exits make the block (rg_vmat_) before they migrate it */
              fprintf(o, "        *(void **)((uint8_t *)skb_%d + 8 + QJS_AOT_OFF_JSOBJECT_SHAPE) = rgc_s_%s_%d;\n        vk_%d = 1; vs_%d = 0; rn_%d = SKV_%d;\n", ki, c->hex, ki, ki, ki, ki, ki);
            } else
            if (c->emit && K->stack) {
              /* the object of a stack pair: the previous incarnation (dead: nothing carries it) is released, the block is made again in the class's final shape */
              RStr rt_ = {0};
              rg_sk_release_text(c, ki, &rt_);
              if (K->knock) fprintf(o, "        if (!getenv(\"QJS_AOT_REGION_SRKNOCK\")) goto %s;   /* the knock-out build: only the knocked run makes it a stack object */\n", xl);
              if (rg_img_ok(K)) {
                /* the block is a copy of the image of the class's block (rg_img_build): the shape gains the reference the engine's init would give it */
                fprintf(o, "        { JSValue t_; %s\n          if (__builtin_expect(rg_imgok_%s_%d, 1)) {\n            _Static_assert(RGIMGW_%s_%d * 8 <= sizeof(sks_b[0]), \"the image fits the slot\");\n"
                           "            rg_img_copy(skb_%d, rg_img_%s_%d, RGIMGW_%s_%d);\n            ++*(int *)((uint8_t *)rgc_s_%s_%d - QJS_AOT_RC_BACK);\n            t_ = SKV_%d;\n",
                        rt_.p ? rt_.p : "", c->hex, ki, c->hex, ki, ki, c->hex, ki, c->hex, ki, c->hex, ki, ki);
                if (getenv("QJS_AOT_REGION_IMGCHECK") != NULL)
                  fprintf(o, "            { _Alignas(64) uint64_t chk_[RGIMGW_%s_%d] = {0}; JSValue u_ = qjs_aot_stack_object_init_cap(chk_, sizeof chk_, rgc_s_%s_%d, %uu); unsigned w_;\n"
                             "              for (w_ = 0; w_ < RGIMGW_%s_%d; w_++) {\n                uint64_t a_ = skb_%d[w_], b_ = chk_[w_];\n"
                             "                if (w_ == 1 || w_ == 2) { a_ -= (uintptr_t)skb_%d; b_ -= (uintptr_t)chk_; } else if (w_ == RG_IMGPW) { a_ -= (uintptr_t)skb_%d; b_ -= (uintptr_t)chk_; }\n"
                             "                if (a_ != b_) { fprintf(stderr, \"IMGCHECK cons %d word %%u: image %%llx init %%llx\\n\", w_, (unsigned long long)a_, (unsigned long long)b_); abort(); }\n              }\n"
                             "              qjs_aot_stack_release(ctx, u_, JS_UNDEFINED); }\n",
                          c->hex, ki, c->hex, ki, K->late ? rg_cons_cap(K) : 0u, c->hex, ki, ki, ki, ki, ki);
                fprintf(o, "          } else {\n            t_ = qjs_aot_stack_object_init_cap(skb_%d, sizeof skb_%d, rgc_s_%s_%d, %uu);\n            if (__builtin_expect(JS_VALUE_GET_TAG(t_) == JS_TAG_UNINITIALIZED, 0)) goto %s;\n          }\n          sk_%d = 1; rn_%d = t_; }\n",
                        ki, ki, c->hex, ki, K->late ? rg_cons_cap(K) : 0u, xl, ki, ki);
              } else
              fprintf(o, "        { JSValue t_; %s\n          t_ = qjs_aot_stack_object_init_cap(skb_%d, sizeof skb_%d, rgc_s_%s_%d, %uu);\n          if (__builtin_expect(JS_VALUE_GET_TAG(t_) == JS_TAG_UNINITIALIZED, 0)) goto %s;\n          sk_%d = 1; rn_%d = t_; }\n",
                      rt_.p ? rt_.p : "", ki, ki, c->hex, ki, K->late ? rg_cons_cap(K) : 0u, xl, ki, ki);
              free(rt_.p);
              if (K->virt) fprintf(o, "        vs_%d = 0; vk_%d = 1;\n", ki, ki);
            } else
            if (c->emit && K->direct)
              /* TRUST, one class shape: the object is made in its final shape (every field undefined) and the constructor's stores fill the slots */
            {
              /* the fields the class's objects gain after the construction walk (stores in the code that fills them) get room in the object, so that their adds are made in place:
                 a property array that has to grow is the engine's to grow, and the add leaves the region */
              fprintf(o, "        { JSValue t_ = %s(ctx, rgc_s_%s_%d, %du);\n"
                         "          if (__builtin_expect(JS_IsException(t_), 0)) { JS_FreeValue(ctx, JS_GetException(ctx)); goto %s; }\n          %s; rn_%d = t_; }\n", getenv("QJS_AOT_NOINLALLOC") == NULL ? "q2_new_from_shape_cap" : "qjs_aot_new_from_shape_cap", c->hex, ki, rg_cons_cap(K), xl, rel_, ki);
            }
            else if (c->emit) {
              fprintf(o, "        { JSValue t_; int rc_ = qjs_aot_construct_this_cap(ctx, (JSObject *)JS_VALUE_GET_PTR(rgc_f_%s_%d), &t_, %du);\n"
                         "          if (__builtin_expect(rc_ <= 0, 0)) { if (rc_ < 0) JS_FreeValue(ctx, JS_GetException(ctx)); goto %s; }\n          %s; rn_%d = t_; }\n", c->hex, ki, rg_cons_cap(K), xl, rel_, ki);
            }
            if (c->emit && K->direct && K->pre && getenv("QJS_AOT_REGION_PDNOPREFILL") == NULL && !(K->stack && K->virt && getenv("QJS_AOT_REGION_VFCHECK") == NULL))   /* a virtual object has no block yet: its materialise writes the defaults */
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
          if (rg_cold(I, in)) {
            if (rg_cold(I, in)) { if (c->emit) fprintf(o, "        goto %s; /* put_global: never executed in the profile */\n", xl); break; }
            return rg_fail(c, "pc %d: put_global is not in the region subset (and the profile executed it)", in->pc);
          }
          if (u0 < 0 || !rg_atom(in, gn_, sizeof(gn_))) return rg_fail(c, "pc %d: put_global operands", in->pc);
          if (!c->emit) c->opaque = 1;
          q_ = rg_gv_add(c, gn_);
          if (q_ < 0) return rg_fail(c, "pc %d: too many globals", in->pc);
          rg_note_park(c, ii);
          if (c->emit) {
            rg_xlabel(c, rg_xit_x(c, ii, in->next), in->pc, "put-global-facts", xo, sizeof(xo));
            fprintf(o, "        { JSValue o_; int r_ = qjs_aot_global_put(ctx, rgga_%s[%d], %sv%d, %d, &o_);\n", c->hex, q_, I->pfx, u0, in->narg >= 3 ? (int)in->arg[2].val : 0);
            fprintf(o, "          if (__builtin_expect(r_ < 0, 0)) { RG_FREE_OWNED(); return JS_EXCEPTION; }\n");
            fprintf(o, "          if (__builtin_expect(r_ == 0, 0)) goto %s;\n", xl);
            fprintf(o, "          rg_own_put(ctx, &rgo_, &rgon_, &rgoc_, o_);\n");
            if (rg_trust_on()) fprintf(o, "          if (__builtin_expect(!QJS_AOT_FACTS_OK(&rg_fw_%s, 1) || rgon_ >= RG_OWNCAP, 0)) goto %s; }\n", c->hex, xo);
            else fprintf(o, "          if (__builtin_expect(rgon_ >= RG_OWNCAP, 0)) goto %s; }\n", xl);
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
            if (is_root) {
              int ka;
              for (ka = 0; ka < c->ncons; ka++) if (c->cons[ka].stack && c->cons[ka].accum) {
                /* the accumulator the root returns moves to the heap here */
                char mg_[1400];
                rg_mig_stmt(c, ka, mg_, sizeof(mg_));
                if (rg_retslot_on()) {
                  /* a caller that gave a shell (aot_nr_) gets the object in it: the shell takes the fields, the stack child goes to the heap alone; any refusal moves the object to the heap */
                  const RCons *Ka = &c->cons[ka];
                  fprintf(o, "        if (sk_%d && JS_VALUE_GET_PTR(%sv%d) == JS_VALUE_GET_PTR(SKV_%d)) {\n          if (rs_ != NULL && qjs_aot_slot_fill(ctx, *rs_, SKV_%d, ", ka, I->pfx, u0, ka, ka);
                  if (Ka->chi > 0) fprintf(o, "SKV_%d) > 0) { sk_%d = 0; JSValue rv_ = *rs_;\n", Ka->chi - 1, Ka->chi - 1); else fprintf(o, "JS_UNDEFINED) > 0) { JSValue rv_ = *rs_;\n");
                  rg_free_owned(c, o, "            ");
                  fprintf(o, "            return rv_; }   /* the shell is the caller's block: its reference is not counted */\n          %s\n          %sv%d = sks_m[%d]; } }\n", mg_, I->pfx, u0, c->cons[ka].ss);
                } else
                  fprintf(o, "        if (sk_%d && JS_VALUE_GET_PTR(%sv%d) == JS_VALUE_GET_PTR(SKV_%d)) %s\n          %sv%d = sks_m[%d]; }\n", ka, I->pfx, u0, ka, mg_, I->pfx, u0, c->cons[ka].ss);
              }
            }
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
      if (c->emit) {
        /* a move that makes the heap object of a stack object: it is migrated here and parked in the owned list (which holds the region's reference), and the result is the heap object */
        int kq, mq;
        for (kq = 0; kq < c->ncons; kq++) {
          const RCons *Kq = &c->cons[kq];
          if (!Kq->stack) continue;
          for (mq = 0; mq < Kq->nmat; mq++) {
            if (Kq->mat[mq][0] != ii || Kq->mat[mq][1] != 1 || Kq->mat[mq][2] != idx) continue;
            /* the moved value is this object only when it is the block itself: several objects can reach one move (a value that is one of two constructs) */
            rg_mig_stmt(c, kq, migs_, sizeof(migs_));
            if (Kq->matacc[mq] > 0) {
              /* the accumulator takes the object's fields (its stack child with them): the result is the accumulator; when it can not (it was moved, or the object does not fit) the object goes to the heap as it would otherwise */
              const RCons *K0 = &c->cons[Kq->matacc[mq] - 1];
              char c0_[24], cq_[24];
              int knock_ = getenv("QJS_AOT_REGION_ACCUMKNOCK") != NULL;   /* a knock-out: a refused assignment is taken as done */
              if (K0->chi > 0) snprintf(c0_, sizeof(c0_), "SKV_%d", K0->chi - 1); else snprintf(c0_, sizeof(c0_), "JS_UNDEFINED");
              if (Kq->chi > 0) snprintf(cq_, sizeof(cq_), "SKV_%d", Kq->chi - 1); else snprintf(cq_, sizeof(cq_), "JS_UNDEFINED");
              fprintf(o, "        if (sk_%d && JS_VALUE_GET_PTR(%sv%d) == JS_VALUE_GET_PTR(SKV_%d)) { if (sk_%d && %sqjs_aot_slot_assign(ctx, SKV_%d, %s, SKV_%d, %s)%s) %sv%d = SKV_%d; else %s%s\n          %sv%d = sks_m[%d]; } }\n",
                      kq, I->pfx, u0, kq, (int)(K0 - c->cons), knock_ ? "(" : "", (int)(K0 - c->cons), c0_, kq, cq_, knock_ ? ", 1)" : "", I->pfx, dv, (int)(K0 - c->cons), migs_, getenv("QJS_AOT_FASTSTATS") != NULL ? (snprintf(fsmv_, sizeof(fsmv_), " rgm_%s[%d]++;", c->hex, kq), fsmv_) : "", I->pfx, dv, Kq->ss);
              continue;
            }
            rg_mig_stmt(c, kq, migs_, sizeof(migs_));
            fprintf(o, "        if (sk_%d && JS_VALUE_GET_PTR(%sv%d) == JS_VALUE_GET_PTR(SKV_%d)) %s%s\n          %sv%d = sks_m[%d]; }\n",
                    kq, I->pfx, u0, kq, migs_, getenv("QJS_AOT_FASTSTATS") != NULL ? (snprintf(fsmv_, sizeof(fsmv_), " rgm_%s[%d]++;", c->hex, kq), fsmv_) : "", I->pfx, dv, Kq->ss);
          }
        }
      }
    }
    if (c->emit && b < 128) { I->bsnap[b] = I->snap; I->bpe[b] = I->prev_eff; }
    /* the block's terminator */
    if (B->nsucc == 0) continue;
    {
      const Q2Insn *last = B->ninsns > 0 ? &fn->insns[B->first_insn + B->ninsns - 1] : NULL;
      int s, j;
      char edge[2][3072];
      for (s = 0; s < B->nsucc; s++) {
        size_t eo = 0;
        const Q2Block *T = &fn->blocks[B->succ_block[s]];
        edge[s][0] = 0;
        {
          /* an object that moves to the heap on this edge: migrated here (parked in the owned list, which holds the region's reference), and the parameter takes the heap object */
          char mpre[2600], fsm_[64], migs2_[1400];
          int kq, mq;
          mpre[0] = 0;
          g_edge_ovr_n = 0;
          for (kq = 0; kq < c->ncons && c->emit; kq++) {
            const RCons *Kq = &c->cons[kq];
            if (!Kq->stack) continue;
            for (mq = 0; mq < Kq->nmat; mq++) {
              if (Kq->mat[mq][0] != ii || Kq->mat[mq][1] != 0 || Kq->mat[mq][2] != b || Kq->mat[mq][3] != s || g_edge_ovr_n >= 4) continue;
              if (getenv("QJS_AOT_FASTSTATS") != NULL) snprintf(fsm_, sizeof(fsm_), " rgm_%s[%d]++;", c->hex, kq); else fsm_[0] = 0;
              rg_mig_stmt(c, kq, migs2_, sizeof(migs2_));
              snprintf(mpre + strlen(mpre), sizeof(mpre) - strlen(mpre),
                       "if (sk_%d && JS_VALUE_GET_PTR(%sv%d) == JS_VALUE_GET_PTR(SKV_%d)) %s%s } ",
                       kq, I->pfx, Kq->mat[mq][4], kq, migs2_, fsm_);
              g_edge_ovr_v[g_edge_ovr_n] = Kq->mat[mq][4];
              g_edge_ovr_k[g_edge_ovr_n] = kq;
              g_edge_ovr_s[g_edge_ovr_n] = Kq->ss;
              g_edge_ovr_n++;
            }
          }
          rg_edge_text(I, B, s, T, edge[s], sizeof(edge[s]));   /* the parameter takes the argument in its own type (region_fn_var.inc) */
          g_edge_ovr_n = 0;
          if (mpre[0]) {
            char t_[3072];
            snprintf(t_, sizeof(t_), "%s%s", mpre, edge[s]);
            snprintf(edge[s], sizeof(edge[s]), "%s", t_);
          }
        }
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
        rg_xlabel(c, rg_xit_x(c, ii, last->pc), last->pc, I->pfx, xl2, sizeof(xl2));
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
        rg_xlabel(c, rg_xit_x(c, ii, last->pc), last->pc, I->pfx, xl2, sizeof(xl2));
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
        rg_xlabel(c, rg_xit_x(c, ii, last->pc), last->pc, I->pfx, xl2, sizeof(xl2));
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
          int xi = rg_xit_x(c, ii, fn->blocks[B->succ_block[0]].start_pc);
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
  if (c->ndsh > 0) fprintf(o, "static void *rk_dx_%s[%d];   /* the shapes the per-site dispatches test against (T13) */\n", h, c->ndsh);
  {
    int any_ms = 0, kq, sq;
    for (kq = 0; kq < c->ncls; kq++) if (c->cls[kq].nsh > 1) any_ms = 1;
    if (any_ms) {
      /* the shape table of the multi-shape classes: rk_sx_[k][s] the held shape s of class k (0 = rk_s_[k]), rk_ns_[k] their count (0 = a single-shape class), rk_tp_[k][s] the shape one field
         shorter that an add of the atom rk_ta_[k][s] turns into s (-1: none observed), so a store that adds a field to an object of shape p is the constant transition to the s with rk_tp_[k][s] == p
         and that atom */
      fprintf(o, "static void *rk_sx_%s[%d][%d]; static uint32_t rk_ta_%s[%d][%d];\n", h, c->ncls, QJS_AOT_MAXSHAPES, h, c->ncls, QJS_AOT_MAXSHAPES);
      fprintf(o, "static inline __attribute__((always_inline)) int rg_inx_%s(void *const *tab, int n, const void *sh) { int i; for (i = 1; i < n; i++) if (tab[i] == sh) return 1; return 0; }\n", h);
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
  for (q = 0; q < c->ncons; q++) if (c->cons[q].kind == 0 && c->cons[q].direct && !c->cons[q].rslot) fprintf(o, "static void *rgc_s_%s_%d;\n", h, q);
  for (q = 0; q < c->ncons; q++) if (rg_img_ok(&c->cons[q])) { fprintf(o, "#ifndef RG_IMGW\n#define RG_IMGW(room) ((8 + QJS_AOT_OFF_JSOBJECT_PROP + 16 + (room) * QJS_AOT_SIZEOF_JSPROPERTY) / 8)\n#define RG_IMGPW ((8 + QJS_AOT_OFF_JSOBJECT_PROP) / 8)\n#define RG_IMGPU (8 + QJS_AOT_OFF_JSOBJECT_PROP + 16)\nstatic int rg_img_build(JSContext *, void *, unsigned, uint64_t *, unsigned);\n#endif\n"); break; }
  for (q = 0; q < c->ncons; q++) if (rg_img_ok(&c->cons[q])) fprintf(o, "#define RGIMGW_%s_%d RG_IMGW(%u)\nstatic uint64_t rg_img_%s_%d[RGIMGW_%s_%d]; static uint8_t rg_imgok_%s_%d;\n", h, q, rg_stack_room(&c->cons[q]), h, q, h, q, h, q);
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
    if (K->need_layout) {
      /* a class made last has no later constructor closure to build its row (the unit builds a row when a closure of the same digest is created or constructs): ask for it now,
         before the class shape is made, so the shape carries the layout id */
      fprintf(o, "  { uint8_t ldg_[16]; int qjs_aot_layout_ctor(const uint8_t digest[16]); void qjs_aot_layout_fn_created(JSContext *ctx, JSValue fn, const uint8_t digest[16]);\n");
      fprintf(o, "    if (qjs_aot_function_digest(F, ldg_) && qjs_aot_layout_ctor(ldg_)) qjs_aot_layout_fn_created(ctx, F, ldg_); }\n");
    }
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
    if (K->need_layout && getenv("QJS_AOT_REGION_NOLAYOUTDEP") == NULL) {   /* (the vkit knock-out drops the dependency and the receiver tests together) */
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
    for (q = 0; q < c->ndsh; q++) {
      const struct RDsh *D = &c->dsh[q];
      if (D->k != k) continue;
      fprintf(o, "  /* dispatch shape %d of class %s: its ordered fields, built from the prototype and held */\n  { uint32_t at_[%d]; int i_; void *sh_;\n", q, K->ctor, D->nn > 0 ? D->nn : 1);
      for (j = 0; j < D->nn; j++) { fprintf(o, "    at_[%d] = (uint32_t)JS_NewAtom(ctx, ", j); rg_cstr(o, D->names[j]); fprintf(o, ");\n"); }
      fprintf(o, "    sh_ = qjs_aot_class_shape(ctx, P, at_, %d);\n    for (i_ = 0; i_ < %d; i_++) JS_FreeAtom(ctx, (JSAtom)at_[i_]);\n", D->nn, D->nn);
      fprintf(o, "    if (sh_ == NULL || q2_phn >= Q2_PH_MAX)"); RG_FAIL_("dispatch shape");
      fprintf(o, "    q2_ph[q2_phn++] = sh_; rk_dx_%s[%d] = sh_; }\n", h, q);
    }
    fprintf(o, "  rk_p_%s[%d] = (JSObject *)JS_VALUE_GET_PTR(P);\n  JS_FreeValue(ctx, F); JS_FreeValue(ctx, P); F = P = JS_UNDEFINED;\n", h, k);
  }
  {
    /* the shapes of object literals (T13): built on Object.prototype, the prototype a fresh `{}` has */
    int any_ = 0;
    for (q = 0; q < c->ndsh; q++) if (c->dsh[q].k < 0) any_ = 1;
    if (any_) {
      fprintf(o, "  { JSValue t_ = JS_NewObject(ctx), op_ = JS_IsException(t_) ? JS_UNDEFINED : JS_GetPrototype(ctx, t_); JS_FreeValue(ctx, t_);\n    if (JS_VALUE_GET_TAG(op_) != JS_TAG_OBJECT) { JS_FreeValue(ctx, op_); goto out; }\n");
      for (q = 0; q < c->ndsh; q++) {
        const struct RDsh *D = &c->dsh[q];
        if (D->k >= 0) continue;
        fprintf(o, "    { uint32_t at_[%d]; int i_; void *sh_;\n", D->nn > 0 ? D->nn : 1);
        for (j = 0; j < D->nn; j++) { fprintf(o, "      at_[%d] = (uint32_t)JS_NewAtom(ctx, ", j); rg_cstr(o, D->names[j]); fprintf(o, ");\n"); }
        fprintf(o, "      sh_ = qjs_aot_class_shape(ctx, op_, at_, %d);\n      for (i_ = 0; i_ < %d; i_++) JS_FreeAtom(ctx, (JSAtom)at_[i_]);\n", D->nn, D->nn);
        fprintf(o, "      if (sh_ == NULL || q2_phn >= Q2_PH_MAX) { JS_FreeValue(ctx, op_); %s goto out; }\n", fs ? "fprintf(stderr, \"region init literal shape\\n\");" : "");
        fprintf(o, "      q2_ph[q2_phn++] = sh_; rk_dx_%s[%d] = sh_; }\n", h, q);
      }
      fprintf(o, "    JS_FreeValue(ctx, op_); }\n");
    }
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
    if (K->kind == 2 || K->rslot) continue;
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
      if (rg_img_ok(K)) fprintf(o, "  rg_imgok_%s_%d = (uint8_t)rg_img_build(ctx, rgc_s_%s_%d, %uu, rg_img_%s_%d, RGIMGW_%s_%d);\n", h, q, h, q, K->late ? rg_cons_cap(K) : 0u, h, q, h, q);
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
      if (K->kind == 2 || K->rslot) continue;
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
static int g_sr_at_n, g_sr_at_pc, g_sr_at_pos, g_sr_at_fline;
static int g_sr_wait;   /* set when a construct was refused only because its container is not decided yet */
static char g_sr_at_op[40], g_sr_at_fn[48];   /* the instance, pc and operand position of the use being checked (the first failure is reported with them) */
/* Whether every path from the entry of fn to the end of the function passes through instruction k's block (the block post-dominates the entry): a store there is made whenever the function runs to its end. */
static int sr_on_all_paths(const Q2Func *fn, int k) {
  int b, tb = -1, nb = fn->nblocks, top = 0, ok = 1;
  int *stk, *seen;
  for (b = 0; b < nb; b++) if (k >= fn->blocks[b].first_insn && k < fn->blocks[b].first_insn + fn->blocks[b].ninsns) tb = b;
  if (tb < 0 || nb <= 0) return 0;
  if (tb == 0) return 1;
  stk = malloc(sizeof(int) * (size_t)nb * 2); seen = calloc((size_t)nb, sizeof(int));
  if (stk == NULL || seen == NULL) { free(stk); free(seen); return 0; }
  stk[top++] = 0; seen[0] = 1;
  while (top > 0 && ok) {
    const Q2Block *B = &fn->blocks[stk[--top]];
    int sidx;
    if (B->nsucc == 0) { ok = 0; break; }
    for (sidx = 0; sidx < B->nsucc; sidx++) {
      int t = B->succ_block[sidx];
      if (t < 0 || t >= nb) { ok = 0; break; }
      if (t == tb || seen[t]) continue;
      seen[t] = 1; stk[top++] = t;
    }
  }
  free(stk); free(seen);
  return ok;
}
static void sr_vno_at(void *sp, int line, int n, int pc);
static int sr_no(int line, int n) { if (!g_sr_line) { g_sr_line = line; if (getenv("QJS_AOT_REGION_WHY")) fprintf(stderr, "stackobj:   refused at line %d: instance %d pc %d position %d op %s in %s (js line %d)\n", line, g_sr_at_n, g_sr_at_pc, g_sr_at_pos, g_sr_at_op, g_sr_at_fn[0] ? g_sr_at_fn : "<anon>", g_sr_at_fline); } (void)n; return 0; }
typedef struct { RCtx *c; char arrname[40]; int arrinst, arrpc, arrfound; int fail, budget; int vis[256][3]; int nvis; int cinst, cpc, nmat, mat[4][5], nok, okinst[8]; unsigned long long okcnt[8]; const RCons *K; int latestore; int par, npar; char pfield[40]; int nst, stover; short st[64][4]; int vok, vstore, nvthis; unsigned vcov; short vthis[32]; int nls, ls[4][4]; int matacc[4], retmat; } RSR;
static void sr_vno_at(void *sp, int line, int n, int pc) {
  RSR *s = (RSR *)sp;
  if (s->vok && getenv("QJS_AOT_REGION_WHY")) fprintf(stderr, "virtual:   not virtual at region.c:%d (instance %d pc %d)\n", line, n, pc);
  s->vok = 0;
}
static int sr_obj(RSR *s, int n, int v, int kind);
static unsigned rg_lent_pred_get(const char *hex);
static void rg_lent_assume(const char *callee, const char *caller, unsigned m);
/* A stack object handed to parameter `idx` of an out-of-line callee (instance j): allowed when that callee only reads the parameter (its lent mask) and the call is a direct native call; the site
   (instance, call instruction, value) is recorded, and after the whole object is known no alias of it may be live past the call (the callee may move the block to the heap). */
static int sr_lend_ok(RSR *s, int j, int idx, int n, int kidx, int v) {
  RInst *J = s->c->in[j];
  char hx[33];
  int q;
  if (getenv("QJS_AOT_REGION_NOLEND") != NULL || J->g == NULL || idx < 0 || idx > 7) return 0;
  rg_hexof(J->g, hx);
  if (!((rg_lent_pred_get(hx) >> idx) & 1)) return 0;
  for (q = 0; q < s->nls; q++) if (s->ls[q][0] == n && s->ls[q][1] == kidx && s->ls[q][2] == v) return 1;
  if (s->nls >= 4) return 0;
  s->ls[s->nls][0] = n; s->ls[s->nls][1] = kidx; s->ls[s->nls][2] = v; s->ls[s->nls][3] = idx;
  s->nls++;
  return 1;
}
/* An edge where the object may be moved to the heap instead of being an escape: the block runs rarely against the construct (a quarter or less of its count in the profile). The
   value must also be dead after the edge (checked once the whole object is known). */
unsigned long long qjs_aot_prof_pc_count(const uint8_t dg[16], int pc);
/* a moved object costs more than a heap one: only a place the profile reaches 1 in this many times as often as the object is made */
static unsigned long long sr_mat_ratio(void) { const char *e = getenv("QJS_AOT_REGION_MATRATIO"); return e != NULL ? (unsigned long long)atoi(e) : 8ull; }
static int sr_accum_on(void) { static int v = -1; if (v < 0) v = getenv("QJS_AOT_REGION_ACCUM") != NULL && getenv("QJS_AOT_REGION_NOACCUM") == NULL; return v; }
/* The accumulator a move assigns into: a stack object of the root that the root returns, whose value the move's result joins (it flows over an edge into a parameter the accumulator reaches), with
   none of the accumulator's values used after the move. -1 when there is none. */
static int sr_acc_target(RSR *s, RInst *A, int n, int idx) {
  RCtx *c = s->c;
  const Q2Func *fn = &A->q;
  const Q2Insn *in = &fn->insns[idx];
  int k, b, sidx, j, w;
  if (!sr_accum_on() || n != 0 || in->ndef < 1) return -1;
  for (k = 0; k < c->ncons; k++) {
    const RCons *K = &c->cons[k];
    int found = 0;
    if (!K->stack || !K->accum || K->inst != 0 || K == s->K) continue;
    for (b = 0; b < fn->nblocks && !found; b++) {
      const Q2Block *B = &fn->blocks[b];
      if (fn->rpo_pos != NULL && fn->rpo_pos[b] < 0) continue;
      for (sidx = 0; sidx < B->nsucc && !found; sidx++) {
        const Q2Block *T = &fn->blocks[B->succ_block[sidx]];
        for (j = 0; j < B->nedge_args[sidx] && j < T->nparams && !found; j++)
          if ((int)B->edge_arg[sidx][j] == (int)in->def[0])
            for (w = 0; w < K->acn; w++) if (K->acv[w] == (int)T->param_val[j]) found = 1;
      }
    }
    if (!found) { if (getenv("QJS_AOT_REGION_WHY")) fprintf(stderr, "stackobj:   accumulator cons %d: the move at insn %d (value %d) does not reach its parameters\n", k, idx, (int)in->def[0]); continue; }
    for (w = 0; w < K->acn; w++) {
      int lv_;
      g_lw_param_kill = 1;
      lv_ = rg_live_walk(fn, idx + 1, (Q2ValueId)K->acv[w], -1);
      g_lw_param_kill = 0;
      if (lv_) { if (getenv("QJS_AOT_REGION_WHY")) { int vv_ = K->acv[w], bb_, ii_; fprintf(stderr, "stackobj:   accumulator cons %d: its value %d (block %d insn %d param %d) is used after the move at insn %d\n", k, vv_, fn->values[vv_].block, fn->values[vv_].insn, fn->values[vv_].param_of, idx);
          for (bb_ = 0; bb_ < fn->nblocks; bb_++) { const Q2Block *Bq_ = &fn->blocks[bb_]; int s_, j_; for (ii_ = Bq_->first_insn; ii_ < Bq_->first_insn + Bq_->ninsns; ii_++) { int u_; for (u_ = 0; u_ < fn->insns[ii_].nuse; u_++) if ((int)fn->insns[ii_].use[u_] == vv_) fprintf(stderr, "  use in block %d insn %d\n", bb_, ii_); } for (s_ = 0; s_ < Bq_->nsucc; s_++) for (j_ = 0; j_ < Bq_->nedge_args[s_]; j_++) if ((int)Bq_->edge_arg[s_][j_] == vv_) fprintf(stderr, "  edge arg block %d succ %d -> block %d arg %d\n", bb_, s_, Bq_->succ_block[s_], j_); } } return -1; }
    }
    return k;
  }
  return -1;
}
static int g_sr_nomat;   /* set during a walk that may not use the moves to the heap */
static int sr_mat_edge(RSR *s, RInst *A, int n, int b, int sidx, int v) {
  const Q2Func *fn = &A->q;
  const Q2Block *B = &fn->blocks[b];
  unsigned long long cb, cc;
  int q;
  for (q = 0; q < s->nok && s->okinst[q] != n; q++) {}
  if (q == s->nok) return 0;   /* only where the object is held by this function's values alone: the construct's own instance and the callers it was returned to */
  if (g_sr_nomat || getenv("QJS_AOT_REGION_NOMAT") != NULL || A->g == NULL || B->ninsns <= 0 || s->nmat >= 4 || A->ool) return 0;
  cb = qjs_aot_prof_pc_count(A->g->digest, fn->insns[B->first_insn + B->ninsns - 1].pc);
  cc = s->okcnt[q];   /* how often the object arrives in this function */
  if (cb == 0 || cc == 0 || cb * sr_mat_ratio() > cc) return 0;   /* a place the profile never ran is cold code (an exit), its frequency unknown */
  for (q = 0; q < s->nmat; q++) if (s->mat[q][0] == n && s->mat[q][1] == 0 && s->mat[q][2] == b && s->mat[q][3] == sidx && s->mat[q][4] == v) return 1;
  if (getenv("QJS_AOT_REGION_WHY")) fprintf(stderr, "stackobj:   edge of block %d in instance %d (%s): %llu times against %llu arrivals\n", b, n, A->g != NULL && A->g->name[0] ? A->g->name : "<anon>", cb, cc);
  s->mat[s->nmat][0] = n; s->mat[s->nmat][1] = 0; s->mat[s->nmat][2] = b; s->mat[s->nmat][3] = sidx; s->mat[s->nmat][4] = v;
  s->nmat++;
  return 1;
}
/* The same for a move (`best = info`): the result is the heap object. */
static int sr_mat_move(RSR *s, RInst *A, int n, int idx) {
  const Q2Func *fn = &A->q;
  const Q2Insn *in = &fn->insns[idx];
  unsigned long long cb, cc;
  int q, acc;
  for (q = 0; q < s->nok && s->okinst[q] != n; q++) {}
  if (q == s->nok || in->ndef < 1) return 0;
  if (g_sr_nomat || getenv("QJS_AOT_REGION_NOMAT") != NULL || A->g == NULL || s->nmat >= 4 || A->ool) return 0;
  cb = qjs_aot_prof_pc_count(A->g->digest, in->pc);
  cc = s->okcnt[q];
  acc = sr_acc_target(s, A, n, idx);
  if (cb == 0 || cc == 0 || (acc < 0 && cb * sr_mat_ratio() > cc)) return 0;   /* a place the profile never ran is cold code (an exit), its frequency unknown */
  for (q = 0; q < s->nmat; q++) if (s->mat[q][0] == n && s->mat[q][1] == 1 && s->mat[q][2] == idx) return 1;
  if (getenv("QJS_AOT_REGION_WHY")) fprintf(stderr, "stackobj:   move at instance %d pc %d (%s): %llu times against %llu arrivals\n", n, in->pc, A->g != NULL && A->g->name[0] ? A->g->name : "<anon>", cb, cc);
  s->mat[s->nmat][0] = n; s->mat[s->nmat][1] = 1; s->mat[s->nmat][2] = idx; s->mat[s->nmat][3] = 0; s->mat[s->nmat][4] = (int)in->def[0];
  s->matacc[s->nmat] = acc + 1;
  s->nmat++;
  return 1;
}
static int sr_has_use(const Q2Insn *in, int v, int *pos) {
  int j;
  for (j = 0; j < in->nuse; j++) if ((int)in->use[j] == v) { *pos = j; return 1; }
  for (j = 0; j < in->win_n && in->win_val != NULL; j++) if ((int)in->win_val[j] == v) { *pos = 100 + j; return 1; }
  return 0;
}
/* Whether any alias of the container K0 (the ones its analysis followed) reads the field `nm`: such a read hands out whatever the field holds, and a field that holds a stack child is not a value
   that may flow on. */
static int sr_k_reads(RCtx *c, const RCons *K0, const char *nm) {
  int w;
  if (K0->nkvis <= 0) return 1;
  for (w = 0; w < K0->nkvis; w++) {
    RInst *A = c->in[K0->kvis[w][0]];
    int v = K0->kvis[w][1], k;
    if (A == NULL) return 1;
    for (k = 0; k < A->q.ninsns; k++) {
      const Q2Insn *in = &A->q.insns[k];
      int pos, a;
      char an[40];
      if (!sr_has_use(in, v, &pos)) continue;
      switch (in->rop) {
        case ROP_get_field: case ROP_get_field_own: case ROP_get_field2: case ROP_get_field2_own: case ROP_get_field_chain: case ROP_get_field_chain_own:
          if (pos != 0) break;
          for (a = 0; a < 3; a++) if (rg_atom_n(in, a, an, sizeof(an)) && strcmp(an, nm) == 0) return 1;
          break;
        default: break;
      }
    }
  }
  return 0;
}
/* Whether the object constructed by the instance A (a store of `this` in its constructor walk) may take the construct `s->K`'s place as a stack child of that container's field `nm`. */
/* WHY only: the first condition of the child-store rule that a refused put_field of a constructed object into a field fails */
static void sr_child_why(RSR *s, const RInst *A, const Q2Insn *in, int kind, int pos, const char *nm) {
  RCtx *c = s->c;
  const RCons *K0;
  const char *r = "?";
  int q;
  if (!getenv("QJS_AOT_REGION_WHY")) return;
  if (pos != 1) r = "the object is the stored value's container, not the stored value (position != 1)";
  else if (kind != 0) r = "the stored value is not a kind-0 object";
  else if (!A->fresh) r = "the store is not in a fresh constructor instance";
  else if (A->cons <= 0 || A->cons > c->ncons) r = "no container construct for this instance";
  else if (!rg_is_this(&A->q, in->use[0])) r = "the container is not `this` of the store's function (the store goes through another reference)";
  else if (getenv("QJS_AOT_REGION_NOCHILD") != NULL) r = "NOCHILD";
  else if (s->K == NULL || s->K->kind != 0) r = "the stored construct is not kind 0";
  else {
    K0 = &c->cons[A->cons - 1];
    if (K0 == s->K) r = "the container is the stored construct itself";
    else if (!K0->stack) r = "the container is a heap object (not a stack construct)";
    else if (K0->kind != 0) r = "the container is not kind 0";
    else if (K0->chi != 0) r = "the container is itself a child";
    else if (K0->par != 0) r = "the container has a parent";
    else if (K0->sk_pair >= 0) r = "the container is paired with an Array";
    else if (K0->nkvis <= 0) r = "the container's fields are not visible (late field / not walked in its constructor)";
    else if (s->npar != 0) r = "the stored construct already has a container";
    else { for (q = 0; q < K0->nn && strcmp(K0->names[q], nm) != 0; q++) {}
      if (q == K0->nn) r = "the field is not one of the container's constructor fields (late field)";
      else if (sr_k_reads(c, K0, nm)) r = "the container's field is read where the child could be seen (reads F)"; }
  }
  fprintf(stderr, "stackobj:   child store .%s refused: %s [inst %d fresh=%d cons=%d par=%d par_kind=%d par_call_pc=%d ool=%d; parent fresh=%d cons=%d par=%d par_kind=%d] (store pc %d in %s, %lld executions)\n", nm, r, A->id, A->fresh, A->cons, A->par, A->par_kind, A->par_call_pc, A->ool, A->par >= 0 ? c->in[A->par]->fresh : -1, A->par >= 0 ? c->in[A->par]->cons : -1, A->par >= 0 ? c->in[A->par]->par : -1, A->par >= 0 ? c->in[A->par]->par_kind : -1, in->pc, A->g != NULL ? A->g->name : "?", A->g != NULL ? (long long)qjs_aot_prof_pc_count(A->g->digest, in->pc) : 0LL);
}
static int sr_child_ok(RSR *s, const RInst *A, const char *nm) {
  RCtx *c = s->c;
  const RCons *K0;
  int q;
  if (getenv("QJS_AOT_REGION_NOCHILD") != NULL || s->K == NULL || s->K->kind != 0 || A->cons <= 0 || A->cons > c->ncons) return 0;
  K0 = &c->cons[A->cons - 1];
  if (!K0->stack && K0 > s->K && K0->kind == 0 && K0->direct && !K0->esc && A->fresh) g_sr_wait = 1;
  if (K0 == s->K || !K0->stack || K0->kind != 0 || K0->chi != 0 || K0->par != 0 || K0->sk_pair >= 0 || K0->nkvis <= 0 || s->npar != 0) return 0;
  for (q = 0; q < K0->nn && strcmp(K0->names[q], nm) != 0; q++) {}
  if (q == K0->nn || sr_k_reads(c, K0, nm)) return 0;
  s->par = A->cons;
  s->npar++;
  snprintf(s->pfield, sizeof(s->pfield), "%s", nm);
  return 1;
}
/* the uses of the receiver (`this`) of instance j */
static int sr_recv(RSR *s, int j, int kind) {
  RInst *A = s->c->in[j];
  int k;
  if (kind == 0) { if (s->nvthis < 32) s->vthis[s->nvthis++] = (short)j; else s->vok = 0; }   /* `this` of instance j is the object: the reads of its fields in j are virtual reads */
  for (k = 0; k < A->q.ninsns; k++) {
    const Q2Insn *in = &A->q.insns[k];
    char nm[40];
    if (in->rop == ROP_load_this) { if (in->ndef < 1 || !sr_obj(s, j, (int)in->def[0], kind)) return sr_no(__LINE__, 0); }
    else if (in->rop == ROP_get_this_field || in->rop == ROP_get_this_field2) {
      if (!rg_atom(in, nm, sizeof(nm))) return sr_no(__LINE__, 0);
      if (in->ndef >= 2 && !sr_obj(s, j, (int)in->def[1], kind)) return sr_no(__LINE__, 0);   /* the copy of `this` the read hands on is the object too */
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
          if ((int)B->edge_arg[sidx][j] == v) {
            if (kind == 0 && sr_mat_edge(s, A, n, b, sidx, v)) continue;   /* moved to the heap here: the parameter is the heap object */
            if (kind == 0) sr_vno_at(s, __LINE__, n, -1);   /* a join: the receiver of a field access is not exactly the object */
            if (!sr_obj(s, n, (int)T->param_val[j], kind)) return sr_no(__LINE__, 0);
          }
      }
    }
  }
  for (k = 0; k < A->q.ninsns; k++) {
    const Q2Insn *in = &A->q.insns[k];
    int pos;
    char nm[40];
    if (!sr_has_use(in, v, &pos)) continue;
    g_sr_at_n = n; g_sr_at_pc = in->pc; g_sr_at_pos = pos;
    if (getenv("QJS_AOT_REGION_WHY")) { e_rop_name(in->rop, g_sr_at_op, sizeof(g_sr_at_op)); snprintf(g_sr_at_fn, sizeof(g_sr_at_fn), "%s", A->g != NULL ? A->g->name : ""); g_sr_at_fline = A->g != NULL ? A->g->line : 0; }
    switch (in->rop) {
    case ROP_move: case ROP_move_s: case ROP_move_xfer:
      if (kind == 0 && pos == 0 && sr_mat_move(s, A, n, (int)(in - A->q.insns))) break;   /* moved to the heap here: the result is the heap object */
      if (in->ndef < 1 || !sr_obj(s, n, (int)in->def[0], kind)) return sr_no(__LINE__, 0);
      break;
    case ROP_get_field: case ROP_get_field_own:
      if (pos != 0 || !rg_atom(in, nm, sizeof(nm))) return sr_no(__LINE__, 0);
      if (kind == 0 && s->arrfound && strcmp(nm, s->arrname) == 0) { if (in->ndef < 1 || !sr_obj(s, n, (int)in->def[0], 1)) return sr_no(__LINE__, 0); }
      break;
    case ROP_get_field2: case ROP_get_field2_own:
      if (pos != 0 || !rg_atom(in, nm, sizeof(nm)) || (kind == 0 && s->arrfound && strcmp(nm, s->arrname) == 0)) return sr_no(__LINE__, 0);
      if (kind == 0 && s->K != NULL) { int q; for (q = 0; q < s->K->nn && strcmp(s->K->names[q], nm) != 0; q++) {} if (q < s->K->nn) sr_vno_at(s, __LINE__, n, in->pc); }   /* a field read whose value is called: not a virtual read */
      if (in->ndef >= 2 && !sr_obj(s, n, (int)in->def[1], kind)) return sr_no(__LINE__, 0);
      break;
    case ROP_put_field: case ROP_put_field_xfer:
      if (!rg_atom(in, nm, sizeof(nm))) return sr_no(__LINE__, 0);
      if (pos == 0) {
        /* a store into the object: the Array its constructor makes is the one value that is followed */
        if (kind != 0 || in->nuse < 2) return sr_no(__LINE__, 0);
        if (s->K != NULL) { int q; for (q = 0; q < s->K->nn && strcmp(s->K->names[q], nm) != 0; q++) {} if (q == s->K->nn) s->latestore = 1;   /* a field the construction walk did not add */
          /* the store, to be typed once the value types are decided: the fields that may come to hold a reference decide what the block's release reads */
          { const Q2Func *fq_ = &A->q; int ki_ = (int)(in - fq_->insns), q_, okc_ = rg_is_this(fq_, in->use[0]) && sr_on_all_paths(fq_, ki_);
            const RInst *H_ = A;
            /* the store is made on every path through the constructor: it is in the entry block of the function that makes it, and so is the call that reaches that function from the constructor (the wrapper's forward) */
            while (okc_ && !H_->fresh && H_->par >= 0 && H_->par_kind != 1 && H_->par < c->nin_total) { const RInst *P_ = c->in[H_->par]; okc_ = P_ != NULL && sr_on_all_paths(&P_->q, rg_insn_at(&P_->q, H_->par_call_pc)); H_ = P_; if (H_ == NULL) okc_ = 0; }
            if (okc_ && s->nvthis > 0) for (q_ = 0; q_ < s->K->nn && strcmp(s->K->names[q_], nm) != 0; q_++) {}
            else q_ = s->K->nn;
            if (getenv("QJS_AOT_REGION_WHY")) fprintf(stderr, "virtual:   store .%s in instance %d (%s): okc=%d fresh=%d par_kind=%d H=%d Hfresh=%d nvthis=%d\n", nm, n, A->g != NULL ? A->g->name : "", okc_, A->fresh, A->par_kind, H_->id, H_->fresh, s->nvthis);
            if (okc_ && q_ < s->K->nn && (H_->fresh || (H_->par_kind == 1 && H_->par_cons >= 0))) s->vcov |= 1u << q_; }

          if (s->nst < 64) { s->st[s->nst][0] = (short)n; s->st[s->nst][1] = (short)in->use[1]; s->st[s->nst][2] = (short)(q == s->K->nn ? -1 : q); s->st[s->nst][3] = (short)(in - A->q.insns); s->nst++; } else s->stover = 1; } else s->stover = 1;
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
      } else if (pos == 1 && kind == 0 && A->fresh && A->cons > 0 && rg_is_this(&A->q, in->use[0]) && sr_child_ok(s, A, nm)) {
        /* the constructor's own store of this object into a field of the stack object it constructs: the container holds it, and moves it with itself */
        sr_vno_at(s, __LINE__, n, in->pc);
      } else { sr_child_why(s, A, in, kind, pos, nm); return sr_no(__LINE__, 0); }
      break;
    case ROP_return:
      /* an inlined function's result is its caller's call result: the object goes on in the parent, where every use of it is checked in turn. The root's return, a constructor's, and an out-of-line
         callee's leave the region. */
      if (kind == 0 && pos == 0 && n == 0 && A->par < 0 && !A->ool && s->K != NULL && s->K->inst == 0 && sr_accum_on()) { s->retmat = 1; break; }   /* the root returns the object: it moves to the heap there (the accumulator) */
      if (pos != 0 || n == 0 || A->par < 0 || A->par_kind == 1 || A->par_dv < 0 || A->ool || getenv("QJS_AOT_REGION_NOSRRET") != NULL) return sr_no(__LINE__, 0);
      if (kind == 0 && s->nok < 8) {
        s->okcnt[s->nok] = c->in[A->par]->g != NULL ? qjs_aot_prof_pc_count(c->in[A->par]->g->digest, A->par_call_pc) : 0;
        s->okinst[s->nok++] = A->par;
      }
      if (!sr_obj(s, A->par, A->par_dv, kind)) return sr_no(__LINE__, 0);
      break;
    case ROP_get_length:
      if (kind != 1 || pos != 0) return sr_no(__LINE__, 0);
      break;
    case ROP_get_field_chain: case ROP_get_field_chain_own: {
      /* `this.elms.length`: the field that holds the Array, then its length */
      char n0[40] = "", n1[40] = "", n2[40] = "";
      if (pos == 0 && kind == 0 && !s->arrfound) { sr_vno_at(s, __LINE__, n, in->pc); break; }   /* an object without an Array: the chain's links are reads of its fields, and what they give is another object */
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
      break;   /* the pointer only is copied into the slot: no field is read */
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
    case ROP_construct: {
      /* a stack object given to a constructor as an argument: the argument reaches the constructor's parameter, directly or through the forwarding wrapper's apply (the constructor body is inlined, and what it does with the parameter is checked like any inlined callee's) */
      int jj, kk2, nf = 0;
      if (kind != 0 || pos < 100 || getenv("QJS_AOT_REGION_NOLEND") != NULL) return sr_no(__LINE__, 0);
      for (jj = 1; jj < c->nin_total; jj++) {
        if (c->in[jj] == NULL || c->in[jj]->par != n || c->in[jj]->par_call_pc != in->pc || c->in[jj]->par_kind != 1) continue;
        if (c->in[jj]->ool) return sr_no(__LINE__, 0);
        if (!c->in[jj]->wrap) { if (!sr_arg(s, jj, pos - 100, kind)) return sr_no(__LINE__, 0); nf++; continue; }
        for (kk2 = 1; kk2 < c->nin_total; kk2++) {
          if (c->in[kk2] == NULL || c->in[kk2]->par != jj) continue;
          if (c->in[kk2]->ool) return sr_no(__LINE__, 0);
          if (!sr_arg(s, kk2, pos - 100, kind)) return sr_no(__LINE__, 0);
          nf++;
        }
      }
      if (nf == 0) return sr_no(__LINE__, 0);
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
        if (c->in[ch[q]]->ool) {
          if (kind == 0 && pos >= 100 && sr_lend_ok(s, ch[q], pos - 100, n, (int)(in - A->q.insns), v)) continue;
          return sr_no(__LINE__, 0);
        }
        if (pos == 1) { if (!sr_recv(s, ch[q], kind)) return sr_no(__LINE__, 0); }
        else if (!sr_arg(s, ch[q], pos - 100, kind)) return sr_no(__LINE__, 0);
      }
      break;
    }
    case ROP_branch_null: case ROP_branch_not_null: case ROP_branch_nullish: case ROP_branch_not_nullish: case ROP_branch_if: case ROP_branch_if_not:
      /* a branch on the value's tag or truthiness (an object is true, defined and not null; an HTMLDDA object leaves, and the exit migrates) keeps no reference */
      if (pos == 0 && kind == 0) break;
      return sr_no(__LINE__, 0);
    case ROP_unary: case ROP_add: case ROP_sub: case ROP_mul: case ROP_div: {
      if (in->rop == ROP_unary) {
        /* `!v`, `v === undefined`, `v == null`: a test of the value that keeps no reference (the object is truthy, defined and not null; the region's LNOT leaves for an HTMLDDA object, and the exit migrates) */
        int sub = in->narg > 1 ? (int)in->arg[1].val : -1;
        if (pos == 0 && kind == 0 && (sub == REG_UN_LNOT || sub == REG_UN_IS_UNDEFINED || sub == REG_UN_IS_NULL || sub == REG_UN_IS_UNDEFINED_OR_NULL)) break;
      } else if (kind == 0 && pos <= 1 && rg_cold(A, in)) {
        /* arithmetic the profile never executed (the message of a throw): the region's arithmetic takes numbers and a string with a primitive only, so an object operand leaves before any effect, and the exit migrates the object */
        break;
      }
    }
    /* FALLTHROUGH */
    default:
      if (getenv("QJS_AOT_REGION_WHY") && !g_sr_line) { char rn[40]; e_rop_name(in->rop, rn, sizeof(rn)); fprintf(stderr, "stackobj:   used by %s at pc %d of instance %d (%s), kind %d pos %d\n", rn, in->pc, n, A->g->name[0] ? A->g->name : "<anon>", kind, pos); }
      return sr_no(__LINE__, 0);
    }
  }
  return 1;
}
/* The lent parameters each emitted region really implements (the protocol is in its code), and what the callers' code assumed of their callees; the driver compares them when every body is out. */
static struct { char hex[33]; unsigned m; } g_lent_real[1024];
static int g_nlent_real;
static struct { char callee[33], caller[33]; unsigned m; } g_lent_assume[256];
static int g_nlent_assume;
static char g_rs_real[64][33];
static int g_nrs_real;
/* Whether the region of root `hex` returns its accumulator into a shell the caller gives (an `aot_nr_` entry exists). */
int region_retslot_real(const char *hex) {
  int i;
  for (i = 0; i < g_nrs_real; i++) if (strcmp(g_rs_real[i], hex) == 0) return 1;
  return 0;
}
static void rg_lent_real_set(const char *hex, unsigned m) {
  int i;
  for (i = 0; i < g_nlent_real; i++) if (strcmp(g_lent_real[i].hex, hex) == 0) { g_lent_real[i].m = m; return; }
  if (g_nlent_real < 1024) { snprintf(g_lent_real[g_nlent_real].hex, 33, "%s", hex); g_lent_real[g_nlent_real++].m = m; }
}
unsigned region_lent_real(const char *hex) {
  int i;
  for (i = 0; i < g_nlent_real; i++) if (strcmp(g_lent_real[i].hex, hex) == 0) return g_lent_real[i].m;
  return 0;
}
static void rg_lent_assume(const char *callee, const char *caller, unsigned m) {
  if (g_nlent_assume < 256) { snprintf(g_lent_assume[g_nlent_assume].callee, 33, "%s", callee); snprintf(g_lent_assume[g_nlent_assume].caller, 33, "%s", caller); g_lent_assume[g_nlent_assume++].m = m; }
}
/* 0 when every region that lends a stack object to a callee finds the callee's emitted code taking it (the parameter is lent in the callee's real emission); else the number of mismatches, each named. */
int region_lent_verify(void) {
  int i, bad = 0;
  for (i = 0; i < g_nlent_assume; i++) {
    unsigned have = region_lent_real(g_lent_assume[i].callee);
    if ((have & g_lent_assume[i].m) != g_lent_assume[i].m) {
      fprintf(stderr, "qjsaot: FATAL region %s lends parameters %x to %s, whose emitted region takes only %x (the callee was not emitted as a region, or reads the parameter in a way the lent walk refuses)\n", g_lent_assume[i].caller, g_lent_assume[i].m, g_lent_assume[i].callee, have);
      bad++;
    }
  }
  return bad;
}
/* The lent parameters of every function whose region was walked (the dry pass walks them all, the real emission walks them again): a caller decides from this table what it may lend. */
static struct { char hex[33]; unsigned m; } g_lent_pred[1024];
static int g_nlent_pred;
static void rg_lent_pred_set(const char *hex, unsigned m) {
  int i;
  for (i = 0; i < g_nlent_pred; i++) if (strcmp(g_lent_pred[i].hex, hex) == 0) { g_lent_pred[i].m = m; return; }
  if (g_nlent_pred < 1024) { snprintf(g_lent_pred[g_nlent_pred].hex, 33, "%s", hex); g_lent_pred[g_nlent_pred++].m = m; }
}
static unsigned rg_lent_pred_get(const char *hex) {
  int i;
  for (i = 0; i < g_nlent_pred; i++) if (strcmp(g_lent_pred[i].hex, hex) == 0) return g_lent_pred[i].m;
  return 0;
}
/* LENT parameters. A caller may hand a stack object to this function's parameter k (it stays in the caller's frame) when the function only READS it: the value and every value loaded from
   its fields reach field reads, tests, arithmetic (which leaves the region for an object operand) and arguments of inlined instances, and nothing else (no store of it, no return, no
   construct argument, no call made out of line, no method call on it). The walk is over the instance tree of the root, so it sees the callee bodies the region inlined. */
typedef struct { RCtx *c; int vis[256][3]; int nvis, budget; } RLT;
static int g_lt_line;
static int lt_no(RLT *s, int line) {
  (void)s;
  if (!g_lt_line) { g_lt_line = line; if (getenv("QJS_AOT_REGION_WHY")) fprintf(stderr, "lent:   refused at line %d: instance %d pc %d position %d op %s in %s\n", line, g_sr_at_n, g_sr_at_pc, g_sr_at_pos, g_sr_at_op, g_sr_at_fn[0] ? g_sr_at_fn : "<anon>"); }
  return 0;
}
static int lt_obj(RLT *s, int n, int v, int deriv);
static int lt_recv(RLT *s, int j, int deriv) {
  RInst *A = s->c->in[j];
  int k;
  for (k = 0; k < A->q.ninsns; k++) {
    const Q2Insn *in = &A->q.insns[k];
    if (in->rop == ROP_load_this) { if (in->ndef < 1 || !lt_obj(s, j, (int)in->def[0], deriv)) return lt_no(s, __LINE__); }
    else if (in->rop == ROP_get_this_field || in->rop == ROP_get_this_field2) {
      if (in->ndef >= 1 && !lt_obj(s, j, (int)in->def[0], 1)) return lt_no(s, __LINE__);   /* the field's value: what the object holds in it */
      if (in->ndef >= 2 && !lt_obj(s, j, (int)in->def[1], deriv)) return lt_no(s, __LINE__);   /* the copy of `this` the read hands on */
    }
  }
  return 1;
}
static int lt_arg(RLT *s, int j, int idx, int deriv) {
  RInst *A = s->c->in[j];
  int k;
  for (k = 0; k < A->q.ninsns; k++) {
    const Q2Insn *in = &A->q.insns[k];
    if (in->rop != ROP_get_arg && in->rop != ROP_get_arg2) continue;
    if (in->narg >= 2 && (int)in->arg[1].val == idx && in->ndef >= 1 && !lt_obj(s, j, (int)in->def[0], deriv)) return lt_no(s, __LINE__);
    if (in->rop == ROP_get_arg2 && in->narg >= 4 && (int)in->arg[3].val == idx && in->ndef >= 2 && !lt_obj(s, j, (int)in->def[1], deriv)) return lt_no(s, __LINE__);
  }
  return 1;
}
static int lt_obj(RLT *s, int n, int v, int deriv) {
  RCtx *c = s->c;
  RInst *A = c->in[n];
  int k;
  if (--s->budget < 0) return lt_no(s, __LINE__);
  { int q; for (q = 0; q < s->nvis; q++) if (s->vis[q][0] == n && s->vis[q][1] == v && s->vis[q][2] == deriv) return 1; if (s->nvis >= 256) return lt_no(s, __LINE__); s->vis[s->nvis][0] = n; s->vis[s->nvis][1] = v; s->vis[s->nvis][2] = deriv; s->nvis++; }
  {
    const Q2Func *fn = &A->q;
    int b, sidx, j;
    for (b = 0; b < fn->nblocks; b++) {
      const Q2Block *B = &fn->blocks[b];
      if (fn->rpo_pos != NULL && fn->rpo_pos[b] < 0) continue;
      for (sidx = 0; sidx < B->nsucc; sidx++) {
        const Q2Block *T = &fn->blocks[B->succ_block[sidx]];
        for (j = 0; j < B->nedge_args[sidx] && j < T->nparams; j++)
          if ((int)B->edge_arg[sidx][j] == v && !lt_obj(s, n, (int)T->param_val[j], deriv)) return lt_no(s, __LINE__);
      }
    }
  }
  for (k = 0; k < A->q.ninsns; k++) {
    const Q2Insn *in = &A->q.insns[k];
    int pos;
    if (!sr_has_use(in, v, &pos)) continue;
    g_sr_at_n = n; g_sr_at_pc = in->pc; g_sr_at_pos = pos;
    if (getenv("QJS_AOT_REGION_WHY")) { e_rop_name(in->rop, g_sr_at_op, sizeof(g_sr_at_op)); snprintf(g_sr_at_fn, sizeof(g_sr_at_fn), "%s", A->g != NULL ? A->g->name : ""); }
    switch (in->rop) {
    case ROP_move: case ROP_move_s: case ROP_move_xfer:
      if (in->ndef < 1 || !lt_obj(s, n, (int)in->def[0], deriv)) return lt_no(s, __LINE__);
      break;
    case ROP_get_field: case ROP_get_field_own:
      if (pos != 0) return lt_no(s, __LINE__);
      if (in->ndef >= 1 && !lt_obj(s, n, (int)in->def[0], 1)) return lt_no(s, __LINE__);
      break;
    case ROP_get_field2: case ROP_get_field2_own:
      if (pos != 0) return lt_no(s, __LINE__);
      if (in->ndef >= 2 && !lt_obj(s, n, (int)in->def[1], deriv)) return lt_no(s, __LINE__);
      break;
    case ROP_branch_null: case ROP_branch_not_null: case ROP_branch_nullish: case ROP_branch_not_nullish: case ROP_branch_if: case ROP_branch_if_not:
      if (pos != 0) return lt_no(s, __LINE__);
      break;
    case ROP_unary: {
      int sub = in->narg > 1 ? (int)in->arg[1].val : -1;
      if (pos == 0 && (sub == REG_UN_LNOT || sub == REG_UN_IS_UNDEFINED || sub == REG_UN_IS_NULL || sub == REG_UN_IS_UNDEFINED_OR_NULL)) break;
      if (!deriv) return lt_no(s, __LINE__);
      break;
    }
    case ROP_add: case ROP_sub: case ROP_mul: case ROP_div: case ROP_mod: case ROP_neg: case ROP_inc: case ROP_dec: case ROP_add_imm_s: case ROP_sub_imm_s:
    case ROP_mul_add: case ROP_add_mul: case ROP_mul_sub: case ROP_sub_mul:
    case ROP_branch_cmp_loc_loc: case ROP_branch_cmp_loc_reg: case ROP_branch_cmp_reg_loc: case ROP_branch_cmp_reg_reg: case ROP_branch_cmp_reg_reg_s:
    case ROP_branch_eq_s: case ROP_branch_gt_s: case ROP_branch_gte_s: case ROP_branch_lt_s: case ROP_branch_lte_s: case ROP_branch_neq_s:
    case ROP_branch_ngt_s: case ROP_branch_ngte_s: case ROP_branch_nlt_s: case ROP_branch_nlte_s:
      if (!deriv) return lt_no(s, __LINE__);   /* an operand loaded from a field: a number, or an object the arithmetic or the comparison leaves the region for */
      break;
    case ROP_get_length:
      if (pos != 0) return lt_no(s, __LINE__);
      break;
    case ROP_get_field_chain: case ROP_get_field_chain_own: {
      int d_;
      if (pos != 0) return lt_no(s, __LINE__);
      for (d_ = 0; d_ < in->ndef; d_++) if (!lt_obj(s, n, (int)in->def[d_], 1)) return lt_no(s, __LINE__);
      break;
    }
    case ROP_call_method: case ROP_call: {
      int ch[RG_MAXT], nch = 0, kk, q;
      if (pos == 0) return lt_no(s, __LINE__);
      for (kk = 1; kk < c->nin_total && nch < RG_MAXT; kk++)
        if (c->in[kk] != NULL && c->in[kk]->par == n && c->in[kk]->par_call_pc == in->pc && c->in[kk]->par_kind != 1) ch[nch++] = kk;
      if (nch == 0) return lt_no(s, __LINE__);
      for (q = 0; q < nch; q++) {
        if (c->in[ch[q]]->ool || c->in[ch[q]]->wrap) return lt_no(s, __LINE__);
        if (pos == 1) { if (!lt_recv(s, ch[q], deriv)) return lt_no(s, __LINE__); }
        else if (!lt_arg(s, ch[q], pos - 100, deriv)) return lt_no(s, __LINE__);
      }
      break;
    }
    default:
      return lt_no(s, __LINE__);
    }
  }
  return 1;
}
/* The parameters of the root (bit k: parameter k) that the function only reads. */
static unsigned rg_lent_mask(RCtx *c, const Q2Func *root) {
  unsigned m = 0;
  int k, na = root->g != NULL ? root->g->arg_count : 0, saved = c->nin_total;
  if (getenv("QJS_AOT_REGION_NOLEND") != NULL) return 0;
  if (getenv("QJS_AOT_REGION_LENDONLY") != NULL && strncmp(getenv("QJS_AOT_REGION_LENDONLY"), c->hex, strlen(getenv("QJS_AOT_REGION_LENDONLY"))) != 0) return 0;   /* (a debugging aid: only the root with this hex prefix lends) */
  c->nin_total = c->nin;
  for (k = 0; k < na && k < 8; k++) {
    RLT s;
    int i, used = 0, bad = 0;
    for (i = 0; i < root->ninsns; i++) {
      const Q2Insn *in = &root->insns[i];
      if ((in->rop == ROP_put_arg || in->rop == ROP_put_arg_xfer) && in->narg >= 2 && (int)in->arg[1].val == k) bad = 1;
      if ((in->rop == ROP_get_arg || in->rop == ROP_get_arg2) && ((in->narg >= 2 && (int)in->arg[1].val == k) || (in->rop == ROP_get_arg2 && in->narg >= 4 && (int)in->arg[3].val == k))) used = 1;
    }
    if (bad || !used) continue;
    memset(&s, 0, sizeof(s));
    s.c = c;
    s.budget = 3000;
    g_lt_line = 0;
    if (getenv("QJS_AOT_REGION_BORROWKNOCK") != NULL || lt_arg(&s, 0, k, 0)) m |= 1u << k;   /* (BORROWKNOCK lends every read parameter: rg_borrow_check must then refuse the root) */
    else if (getenv("QJS_AOT_REGION_WHY")) fprintf(stderr, "lent: root %.8s parameter %d is not lent (region.c:%d)\n", c->hex, k, g_lt_line);
  }
  c->nin_total = saved;
  if (m != 0 && getenv("QJS_AOT_REGION_WHY")) fprintf(stderr, "lent: root %.8s parameters %x are lent\n", c->hex, m);
  return m;
}

/* BORROWCHECK. An independent check of the lent mask (a deny-list over a closure of the values derived from a lent parameter, where lt_obj is an allow-list walk): in the instance tree of a lent
   root, no store, definition, construct, return or out-of-line call may take a value derived from a lent parameter (the parameter, a copy of it, anything read from a field of it, and the
   same inside the inlined instances it is passed to). A violation aborts the generator: the caller would hand a stack object to a function that could keep it. QJS_AOT_REGION_BORROWKNOCK
   skips the walk for the mask (every object parameter the function reads is lent) so that this check is what refuses it. */
static int rg_borrow_deny(int rop) {
  switch (rop) {
    case ROP_put_field: case ROP_put_field_xfer: case ROP_put_element: case ROP_put_element_xfer: case ROP_put_global: case ROP_put_var_ref: case ROP_put_ref: case ROP_put_ref_value:
    case ROP_put_super_value: case ROP_put_private: case ROP_define_field: case ROP_define_element: case ROP_define_method: case ROP_define_global: case ROP_define_private:
    case ROP_array_from: case ROP_construct: case ROP_return: case ROP_return_async: case ROP_set_proto: case ROP_set_home_object: case ROP_set_name:
      return 1;
    default: return 0;
  }
}
static int rg_borrow_check(RCtx *c, const Q2Func *root) {
  unsigned char **D;
  int n, pass, changed = 1, bad = 0, ni = c->nin;
  (void)root;
  if (c->lent == 0 || ni <= 0) return 0;
  D = calloc((size_t)ni, sizeof(*D));
  for (n = 0; n < ni; n++) D[n] = calloc((size_t)(c->in[n]->q.nvalues + 1), 1);
  for (pass = 0; pass < 12 && changed; pass++) {
    changed = 0;
    for (n = 0; n < ni; n++) {
      RInst *A = c->in[n];
      const Q2Func *fn = &A->q;
      int k, b, sidx, j;
#define BC_SET(v_) do { int vv_ = (int)(v_); if (vv_ >= 0 && vv_ < fn->nvalues && !D[n][vv_]) { D[n][vv_] = 1; changed = 1; } } while (0)
      for (k = 0; k < fn->ninsns; k++) {
        const Q2Insn *in = &fn->insns[k];
        int pos, d_;
        if (n == 0 && (in->rop == ROP_get_arg || in->rop == ROP_get_arg2)) {
          if (in->narg >= 2 && in->ndef >= 1 && (c->lent >> (int)in->arg[1].val & 1u) && (int)in->arg[1].val < 8) BC_SET(in->def[0]);
          if (in->rop == ROP_get_arg2 && in->narg >= 4 && in->ndef >= 2 && (c->lent >> (int)in->arg[3].val & 1u) && (int)in->arg[3].val < 8) BC_SET(in->def[1]);
        }
        {
          int any = 0;
          for (j = 0; j < in->nuse; j++) if ((int)in->use[j] >= 0 && (int)in->use[j] < fn->nvalues && D[n][in->use[j]]) any = 1;
          for (j = 0; j < in->win_n && in->win_val != NULL; j++) if ((int)in->win_val[j] >= 0 && (int)in->win_val[j] < fn->nvalues && D[n][in->win_val[j]]) any = 1;
          if (!any) continue;
        }
        if (rg_borrow_deny(in->rop)) { bad = 1; fprintf(stderr, "qjsaot: BORROWCHECK: root %.8s: %s at pc %d of instance %d takes a value derived from a lent parameter\n", c->hex, "a store/construct/return", in->pc, n); continue; }
        switch (in->rop) {
          case ROP_move: case ROP_move_s: case ROP_move_xfer: case ROP_get_field: case ROP_get_field_own: case ROP_get_field2: case ROP_get_field2_own:
          case ROP_get_field_chain: case ROP_get_field_chain_own:
            for (d_ = 0; d_ < in->ndef; d_++) BC_SET(in->def[d_]);
            break;
          case ROP_call: case ROP_call_method: {
            int kk;
            if (!sr_has_use(in, -1, &pos)) pos = 0;
            for (kk = 1; kk < c->nin; kk++) {
              RInst *C1 = c->in[kk];
              int q, m2;
              if (C1 == NULL || C1->par != n || C1->par_call_pc != in->pc || C1->par_kind == 1) continue;
              if (C1->ool || C1->wrap) {
                /* a lent site (the caller lends its own block) is a use of the container; here the callee is out of line: a derived value is passed to it only where its own mask lends that position */
                continue;
              }
              for (q = 0; q < C1->q.ninsns; q++) {
                const Q2Insn *g2 = &C1->q.insns[q];
                if (g2->rop == ROP_load_this || g2->rop == ROP_get_this_field || g2->rop == ROP_get_this_field2) {
                  for (m2 = 0; m2 < in->nuse; m2++) if (m2 == 1 && (int)in->use[1] < fn->nvalues && D[n][in->use[1]]) { int dd; for (dd = 0; dd < g2->ndef; dd++) if ((int)g2->def[dd] < C1->q.nvalues && !D[kk][g2->def[dd]]) { D[kk][g2->def[dd]] = 1; changed = 1; } }
                } else if (g2->rop == ROP_get_arg || g2->rop == ROP_get_arg2) {
                  for (m2 = 0; m2 < in->win_n && in->win_val != NULL; m2++)
                    if (in->win_val[m2] >= 0 && (int)in->win_val[m2] < fn->nvalues && D[n][in->win_val[m2]]) {
                      if (g2->narg >= 2 && (int)g2->arg[1].val == m2 && g2->ndef >= 1 && !D[kk][g2->def[0]]) { D[kk][g2->def[0]] = 1; changed = 1; }
                      if (g2->rop == ROP_get_arg2 && g2->narg >= 4 && (int)g2->arg[3].val == m2 && g2->ndef >= 2 && !D[kk][g2->def[1]]) { D[kk][g2->def[1]] = 1; changed = 1; }
                    }
                }
              }
            }
            break;
          }
          default: break;   /* tests, arithmetic, lengths: they leave nothing derived that a store could keep */
        }
      }
      for (b = 0; b < fn->nblocks; b++) {
        const Q2Block *B = &fn->blocks[b];
        if (fn->rpo_pos != NULL && fn->rpo_pos[b] < 0) continue;
        for (sidx = 0; sidx < B->nsucc; sidx++) {
          const Q2Block *T = &fn->blocks[B->succ_block[sidx]];
          for (j = 0; j < B->nedge_args[sidx] && j < T->nparams; j++)
            if ((int)B->edge_arg[sidx][j] >= 0 && (int)B->edge_arg[sidx][j] < fn->nvalues && D[n][B->edge_arg[sidx][j]]) BC_SET(T->param_val[j]);
        }
      }
#undef BC_SET
    }
  }
  for (n = 0; n < ni; n++) free(D[n]);
  free(D);
  return bad;
}

/* Whether the value `val` of instance `inst` is used after the call (instruction kidx of instance n) in the frame of `inst`: the instance itself, or one the call is inlined into. An instance below
   the call's is gone when the call runs. */
static int sr_is_lent_site(RCtx *c, const int (*ls)[4], int nls, const Q2Func *fn, int k) {
  int m;
  for (m = 0; m < nls; m++) if (c->in[ls[m][0]] != NULL && &c->in[ls[m][0]]->q == fn && ls[m][1] == k) return 1;
  return 0;
}
/* Whether value v is read by an instruction that can run at or after instruction `from`, other than as the source of a copy whose destination nothing reads (a variable's register that is written
   again before any read still receives the copy). Followed through copies and block edges. */
static int sr_reach_use(RCtx *c, const int (*ls)[4], int nls, const Q2Func *fn, int from, int v, int depth) {
  int b, k, u, sidx, j, b0 = -1, nb = fn->nblocks, res = 0, defblk = -1;
  unsigned char *rb;
  int *stk, top = 0;
  if (depth > 8 || from < 0 || from > fn->ninsns) return 1;
  for (b = 0; b < nb; b++) if (from >= fn->blocks[b].first_insn && from <= fn->blocks[b].first_insn + fn->blocks[b].ninsns) { b0 = b; break; }
  if (b0 < 0) return 1;
  rb = calloc((size_t)nb, 1);
  stk = malloc(sizeof(int) * (size_t)nb * 4 + 4);
  if (rb == NULL || stk == NULL) { free(rb); free(stk); return 1; }
  /* the block that defines v (as an instruction's result or as a block parameter): a path that enters it defines v again, and what it reaches uses the new value, not this one */
  {
    int dv = (int)v < fn->nvalues ? fn->values[v].insn : -1, p_;
    for (b = 0; b < nb && defblk < 0; b++) {
      const Q2Block *B = &fn->blocks[b];
      if (dv >= 0 && dv >= B->first_insn && dv < B->first_insn + B->ninsns) defblk = b;
      for (p_ = 0; p_ < B->nparams && defblk < 0; p_++) if ((int)B->param_val[p_] == v) defblk = b;
    }
  }
  /* the blocks the start block's end reaches (the start block itself only if a path leads back to it) */
  stk[top++] = b0;
  while (top > 0) {
    const Q2Block *B = &fn->blocks[stk[--top]];
    for (sidx = 0; sidx < B->nsucc; sidx++) { int t = B->succ_block[sidx]; if (t >= 0 && t < nb && !rb[t] && t != defblk) { rb[t] = 1; stk[top++] = t; } }
  }
  for (b = 0; b < nb && !res; b++) {
    const Q2Block *B = &fn->blocks[b];
    int lo = B->first_insn, hi = B->first_insn + B->ninsns;
    if (fn->rpo_pos != NULL && fn->rpo_pos[b] < 0) continue;
    if (b == b0 && !rb[b]) lo = from;
    else if (!rb[b]) continue;
    for (k = lo; k < hi && !res; k++) {
      const Q2Insn *in = &fn->insns[k];
      int used = 0;
      for (u = 0; u < in->nuse; u++) if ((int)in->use[u] == v) used = 1;
      for (u = 0; u < in->win_n && in->win_val != NULL; u++) if ((int)in->win_val[u] == v) used = 1;
      if (!used) continue;
      if ((in->rop == ROP_move || in->rop == ROP_move_s || in->rop == ROP_move_xfer) && in->nuse == 1 && in->ndef >= 1) { if (sr_reach_use(c, ls, nls, fn, k + 1, (int)in->def[0], depth + 1)) res = 1; }
      else if (sr_is_lent_site(c, ls, nls, fn, k)) {}   /* another call the object is lent to: it takes the object (the heap one if an earlier call moved it) */
      else { res = 1; if (getenv("QJS_AOT_REGION_WHY")) { char un_[40]; e_rop_name(in->rop, un_, sizeof(un_)); fprintf(stderr, "stackobj:    (reach) value %d used by %s at pc %d, depth %d (use positions:", v, un_, in->pc, depth); for (u = 0; u < in->nuse; u++) fprintf(stderr, " %d", (int)in->use[u]); fprintf(stderr, " | win:"); for (u = 0; u < in->win_n && in->win_val != NULL; u++) fprintf(stderr, " %d", (int)in->win_val[u]); fprintf(stderr, ")\n"); } }
    }
    /* the edges out of a block that runs at or after `from` */
    if (!res && (b != b0 || rb[b] || 1)) {
      int run = (b == b0) ? 1 : rb[b];
      if (run) for (sidx = 0; sidx < B->nsucc && !res; sidx++) {
        const Q2Block *T = &fn->blocks[B->succ_block[sidx]];
        for (j = 0; j < B->nedge_args[sidx] && j < T->nparams && !res; j++)
          if ((int)B->edge_arg[sidx][j] == v && sr_reach_use(c, ls, nls, fn, T->first_insn, (int)T->param_val[j], depth + 1)) { res = 1; if (getenv("QJS_AOT_REGION_WHY")) fprintf(stderr, "stackobj:    (reach) value %d passed on an edge of block %d to a live parameter, depth %d\n", v, b, depth); }
      }
    }
  }
  free(rb);
  free(stk);
  return res;
}
static int sr_live_past_call(RCtx *c, const int (*ls)[4], int nls, int n, int kidx, int inst, int val) {
  int a, prev = n;
  if (inst == n) return sr_reach_use(c, ls, nls, &c->in[n]->q, kidx + 1, val, 0);
  for (a = c->in[n]->par; a >= 0; prev = a, a = c->in[a]->par) {
    if (a == inst) {
      int pk = rg_insn_at(&c->in[a]->q, c->in[prev]->par_call_pc);
      return pk < 0 ? 1 : sr_reach_use(c, ls, nls, &c->in[a]->q, pk + 1, val, 0);
    }
    if (a == 0) break;
  }
  return 0;
}
/* The constructs that become stack objects: cons records of kind 0 (direct, complete) and the Array they hold. */
static void rg_sr_decide(RCtx *c) {
  int ki, pass2, ndefer = 0;
  unsigned char defer[RG_MAXC] = {0};
  g_nmatdef = 0;
  if (!rg_trust_on() || getenv("QJS_AOT_REGION_NOSTACKOBJ") != NULL) return;
  /* A construct whose only way to be a stack object is as the stack child of a container the loop has not reached yet (a constructor's argument is made before the object it is given to) waits:
     once every construct has had its turn, the waiting ones are decided again, in order. That is the whole of the second turn: nothing else about a refused construct changes between the turns
     (its own walk reads the same instances), so a construct refused for another reason is refused again. */
  for (pass2 = 0; pass2 < 2; pass2++) {
  if (pass2 == 1 && ndefer == 0) break;
  for (ki = 0; ki < c->ncons; ki++) {
    RCons *K = &c->cons[ki];
    RSR s;
    RInst *I;
    int ii, ctor = -1, q, dv = -1, di, ok, tried_nomat = 0;
    if (pass2 == 1 && !defer[ki]) continue;
    g_sr_wait = 0;
    if (getenv("QJS_AOT_REGION_WHY") && K->kind == 0) fprintf(stderr, "stackobj: root %.8s cons %d (inst %d pc %d) %s direct=%d esc=%d\n", c->hex, ki, K->inst, K->pc, K->gname, K->direct, K->esc);
    if (K->kind != 0 || !K->direct || K->esc || K->stack || c->nstack >= RG_MAXSTK) continue;   /* the region's frame holds this many stack objects */
    ii = K->inst;
    if (ii < 0 || ii >= c->nin_total || c->in[ii] == NULL) continue;
    I = c->in[ii];
    di = rg_insn_at(&I->q, K->pc);
    if (di < 0 || I->q.insns[di].ndef < 1) continue;
    dv = (int)I->q.insns[di].def[0];
    for (q = 1; q < c->nin_total; q++) if (c->in[q] != NULL && c->in[q]->par == ii && c->in[q]->par_kind == 1 && c->in[q]->par_cons == ki) ctor = q;
    if (ctor < 0 && !K->rslot) continue;
  sr_again:
    memset(&s, 0, sizeof(s));
    s.c = c;
    s.budget = 400;
    s.cinst = ii;
    s.cpc = K->pc;
    s.K = K;
    s.vok = 1;
    s.vstore = 1;
    s.okcnt[s.nok] = I->g != NULL ? qjs_aot_prof_pc_count(I->g->digest, K->pc) : 0;
    s.okinst[s.nok++] = ii;
    g_sr_line = 0;
    {
      int nskal0 = c->nskal;
      g_sr_nomat = tried_nomat;
      ok = (K->rslot || sr_recv(&s, ctor, 0)) && sr_obj(&s, ii, dv, 0);
      g_sr_nomat = 0;
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
      if (ok && s.nmat > 0) {
        /* the moves to the heap: no alias of the object may be used after an edge that moves it, and an object with its Array stays whole */
        int m, w;
        if (s.arrfound) ok = 0;
        for (m = 0; ok && m < s.nmat; m++) {
          const Q2Func *fq = &c->in[s.mat[m][0]]->q;
          int from = s.mat[m][1] == 1 ? s.mat[m][2] + 1 : fq->blocks[fq->blocks[s.mat[m][2]].succ_block[s.mat[m][3]]].first_insn;
          for (w = 0; ok && w < s.nvis; w++)
            if (s.vis[w][0] == s.mat[m][0] && rg_live_walk(fq, from, (Q2ValueId)s.vis[w][1], -1)) { ok = 0; if (getenv("QJS_AOT_REGION_WHY")) fprintf(stderr, "stackobj: root %.8s cons %d value %d is used after the place that moves it\n", c->hex, ki, s.vis[w][1]); }
          if (!ok && K->rslot && !tried_nomat) { tried_nomat = 1; goto sr_again; }   /* a call result: followed to its uses instead of moved at a rare edge */
        }
      }
      if (ok && s.retmat) {
        /* the root returns this object: it is an accumulator, one stack object that the moves of other objects assign into; it moves to the heap at the return */
        int w, n0_ = 0;
        for (w = 0; w < s.nvis; w++) if (s.vis[w][2] == 0 && s.vis[w][0] == 0) n0_++;
        if (s.nmat > 0 || s.arrfound || s.par > 0 || s.nls > 0 || n0_ > 16) { ok = 0; g_sr_line = -1; }
      }
      if (ok && (I->ploop || I->curloop || (getenv("QJS_AOT_KO_CURLOOP") == NULL && rg_val_in_loop(&I->q, dv)))) {
        /* alive when the construct runs again (carried over a back edge) is the heap's: the edges that move the object to the heap are not such a carry */
        int carried_ = 0, m;
        if (getenv("QJS_AOT_REGION_PARKALL") != NULL) carried_ = 1;
        else {
          g_nmskip = s.nmat;
          for (m = 0; m < s.nmat; m++) { g_mskip_fn[m] = &c->in[s.mat[m][0]]->q; g_mskip[m][0] = s.mat[m][1]; g_mskip[m][1] = s.mat[m][2]; g_mskip[m][2] = s.mat[m][3]; g_mskip[m][3] = s.mat[m][4]; }
          carried_ = rg_carried_up(c, ii, dv);
          g_nmskip = 0;
        }
        if (getenv("QJS_AOT_REGION_WHY")) fprintf(stderr, "stackobj: root %.8s cons %d carry test: nmat %d carried %d\n", c->hex, ki, s.nmat, carried_);
        if (carried_) { ok = 0; if (getenv("QJS_AOT_REGION_WHY")) fprintf(stderr, "stackobj: root %.8s cons %d carried over a loop edge\n", c->hex, ki); g_sr_line = -1; }
      }
      if (ok && s.par > 0 && (s.arrfound || s.nmat > 0)) { ok = 0; g_sr_line = -1; }   /* a stack child is one level: no Array of its own, no move to the heap of its own */
      if (ok && s.nls > 0) {
        /* lent to a callee that may move it to the heap: neither the object nor anything the walk followed may be used after the call */
        int m, w;
        if (s.arrfound || s.nmat > 0 || s.par > 0) ok = 0;
        { int n0_ = 0; for (w = 0; w < s.nvis; w++) if (s.vis[w][2] == 0) n0_++; if (n0_ > 48) ok = 0; }   /* the aliases are named after the call (K->kvis holds 48) */
        for (m = 0; ok && m < s.nls; m++)
          for (w = 0; ok && w < s.nvis; w++)
            if (s.vis[w][2] == 0 && sr_live_past_call(c, s.ls, s.nls, s.ls[m][0], s.ls[m][1], s.vis[w][0], s.vis[w][1])) {
              ok = 0;
              g_sr_line = -1;
              if (getenv("QJS_AOT_REGION_DUMPPC")) {
                int lo_ = 0, hi_ = 0, k_;
                sscanf(getenv("QJS_AOT_REGION_DUMPPC"), "%d-%d", &lo_, &hi_);
                for (k_ = 0; k_ < c->in[s.vis[w][0]]->q.ninsns; k_++) {
                  const Q2Insn *di_ = &c->in[s.vis[w][0]]->q.insns[k_];
                  char dn_[40]; int u_;
                  if (di_->pc < lo_ || di_->pc > hi_) continue;
                  e_rop_name(di_->rop, dn_, sizeof(dn_));
                  fprintf(stderr, "dump: pc %d insn %d %s def:", di_->pc, k_, dn_);
                  for (u_ = 0; u_ < di_->ndef; u_++) fprintf(stderr, " %d", (int)di_->def[u_]);
                  fprintf(stderr, " use:");
                  for (u_ = 0; u_ < di_->nuse; u_++) fprintf(stderr, " %d", (int)di_->use[u_]);
                  fprintf(stderr, " win:");
                  for (u_ = 0; u_ < di_->win_n && di_->win_val != NULL; u_++) fprintf(stderr, " %d", (int)di_->win_val[u_]);
                  fprintf(stderr, "\n");
                }
              }
              if (getenv("QJS_AOT_REGION_WHY")) { int w2_; for (w2_ = 0; w2_ < s.nvis; w2_++) fprintf(stderr, "stackobj:    vis[%d] = (inst %d fn line %d, value %d, kind %d)\n", w2_, s.vis[w2_][0], c->in[s.vis[w2_][0]]->g != NULL ? c->in[s.vis[w2_][0]]->g->line : -1, s.vis[w2_][1], s.vis[w2_][2]); }
              if (getenv("QJS_AOT_REGION_WHY")) { const Q2Func *vq_ = &c->in[s.vis[w][0]]->q; int vi_ = vq_->values[s.vis[w][1]].insn; char rn_[40] = "-"; if (vi_ >= 0 && vi_ < vq_->ninsns) e_rop_name(vq_->insns[vi_].rop, rn_, sizeof(rn_)); fprintf(stderr, "stackobj: root %.8s cons %d is lent at instance %d insn %d (pc %d) but value %d of instance %d (defined by %s at pc %d) is used after the call (vis kind %d)\n", c->hex, ki, s.ls[m][0], s.ls[m][1], c->in[s.ls[m][0]]->q.insns[s.ls[m][1]].pc, s.vis[w][1], s.vis[w][0], rn_, vi_ >= 0 ? vq_->insns[vi_].pc : -1, s.vis[w][2]);
              { int u_, uu_; for (u_ = 0; u_ < vq_->ninsns; u_++) { const Q2Insn *ui_ = &vq_->insns[u_]; for (uu_ = 0; uu_ < ui_->nuse; uu_++) if ((int)ui_->use[uu_] == s.vis[w][1]) { char un_[40]; e_rop_name(ui_->rop, un_, sizeof(un_)); fprintf(stderr, "stackobj:    value %d is used by %s at pc %d (pos %d)\n", s.vis[w][1], un_, ui_->pc, uu_); } } } }
            }
      }
      if (ok && s.par > 0) {
        /* a child of a container that is lent: the callee may move the child with it, so the child must not be used after that call either */
        const RCons *K0 = &c->cons[s.par - 1];
        int m, w;
        for (m = 0; ok && m < K0->nls; m++)
          for (w = 0; ok && w < s.nvis; w++)
            if (s.vis[w][2] == 0 && sr_live_past_call(c, K0->ls, K0->nls, K0->ls[m][0], K0->ls[m][1], s.vis[w][0], s.vis[w][1])) {
              ok = 0;
              g_sr_line = -1;
              if (getenv("QJS_AOT_REGION_WHY")) fprintf(stderr, "stackobj: root %.8s cons %d is a child of cons %d, lent at instance %d insn %d, but its value %d of instance %d is used after the call\n", c->hex, ki, s.par - 1, K0->ls[m][0], K0->ls[m][1], s.vis[w][1], s.vis[w][0]);
            }
      }
      K->late = s.latestore;
      if (ok && s.latestore && rg_stack_room(K) == 0) {
        /* a field the class gains after its construction is added to the object here: the add needs room the engine's inline capacity cannot name (it would leave the region at every object) */
        ok = 0;
        g_sr_line = -1;
        if (getenv("QJS_AOT_REGION_WHY")) fprintf(stderr, "stackobj: root %.8s cons %d has late fields (%u slots wanted, %d in the block)\n", c->hex, ki, rg_cons_cap(K), K->nn);
      }
      if (!ok && g_sr_line >= 0 && getenv("QJS_AOT_REGION_WHY")) fprintf(stderr, "stackobj: root %.8s cons %d (inst %d pc %d) escapes (region.c:%d)\n", c->hex, ki, K->inst, K->pc, g_sr_line);
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
      if (!ok) { c->nskal = nskal0; if (K->sk_pair >= 0 && c->cons[K->sk_pair].stack) c->cons[K->sk_pair].stack = 0; K->sk_pair = -1; if (pass2 == 0 && g_sr_wait && getenv("QJS_AOT_REGION_NOLEND") == NULL) { defer[ki] = 1; ndefer++; } continue; }
    }
    if (s.par > 0) {
      /* a stack child: its container's field holds it (the constructor's store), and the container migrates and releases it */
      RCons *K0 = &c->cons[s.par - 1];
      K0->chi = ki + 1;
      snprintf(K0->cfield, sizeof(K0->cfield), "%s", s.pfield);
      K->par = s.par;
      if (getenv("QJS_AOT_REGION_WHY")) fprintf(stderr, "stackobj: root %.8s cons %d is held by field .%s of cons %d\n", c->hex, ki, s.pfield, s.par - 1);
    }
    {
      /* the aliases of the object that were followed: a later child of it checks that none reads the field it holds */
      int w, n0 = 0;
      for (w = 0; w < s.nvis; w++) if (s.vis[w][2] == 0) n0++;
      if (n0 <= 48) {
        n0 = 0;
        for (w = 0; w < s.nvis; w++) if (s.vis[w][2] == 0) { K->kvis[n0][0] = s.vis[w][0]; K->kvis[n0][1] = s.vis[w][1]; n0++; }
        K->nkvis = n0;
      } else K->nkvis = 0;
    }
    K->stack = 1;
    K->nls = s.nls;
    memcpy(K->ls, s.ls, sizeof(K->ls));
    K->nst = s.nst;
    K->stover = s.stover;
    K->vok = s.vok;
    K->vcov = s.vcov;
    K->nvthis = s.nvthis;
    { int m; for (m = 0; m < s.nvthis; m++) { K->vthis[m][0] = s.vthis[m]; K->vthis[m][1] = 0; } }
    memcpy(K->st, s.st, sizeof(K->st));
    K->nmat = s.nmat;
    memcpy(K->mat, s.mat, sizeof(K->mat));
    memcpy(K->matacc, s.matacc, sizeof(K->matacc));
    if (s.retmat) {
      int w;
      K->accum = 1;
      K->acn = 0;
      for (w = 0; w < s.nvis; w++) if (s.vis[w][2] == 0 && s.vis[w][0] == 0 && K->acn < 16) K->acv[K->acn++] = s.vis[w][1];
      if (getenv("QJS_AOT_REGION_WHY")) fprintf(stderr, "stackobj: root %.8s cons %d is an accumulator (returned by the root, %d values)\n", c->hex, ki, K->acn);
    }
    { int m; for (m = 0; m < s.nmat && g_nmatdef < 16; m++) if (s.mat[m][1] == 1 && !s.matacc[m]) { g_matdef_fn[g_nmatdef] = &c->in[s.mat[m][0]]->q; g_matdef_v[g_nmatdef++] = s.mat[m][4]; } }
    if (s.nmat > 0) rg_note_park(c, ii);
    c->nstack++;
  }
  }
  {
    /* an accumulator's block is as large as the largest object assigned into it */
    int kq, m;
    for (kq = 0; kq < c->ncons; kq++) {
      const RCons *Kq = &c->cons[kq];
      if (!Kq->stack) continue;
      for (m = 0; m < Kq->nmat; m++) if (Kq->matacc[m] > 0) {
        RCons *K0 = &c->cons[Kq->matacc[m] - 1];
        unsigned r = rg_stack_room(Kq);
        if (r > 0 && (int)r > K0->accroom) K0->accroom = (int)r;
      }
    }
  }
  {
    /* the stack slots: one per stack object and per Array of a pair; every block has the same size (the region's frame holds them as one array) */
    int need = 0;
    c->nss = 0;
    for (ki = 0; ki < c->ncons; ki++) {
      RCons *K = &c->cons[ki];
      int b = K->kind == 1 ? 640 : (int)(8 + 48 + (rg_stack_room(K) > 0 ? (int)rg_stack_room(K) : (K->nn > 0 ? K->nn : 1)) * 16);
      if (!K->stack) continue;
      K->ss = c->nss++;
      if (b > need) need = b;
    }
    c->skstride = (need + 15) / 16 * 16;
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
/* The fields of each stack object that can come to hold a counted reference: the release of its block reads only those. A field is left out when every store the analysis met into it
   (the construct's own stores, the late adds, the stores through aliases) has a value of a number type (an int32 or a double the region guards where it is defined). An object with a
   store the analysis did not list (more than 64, or a receiver it could not name), an Array pair, or a pointer to a child keeps the engine's full release.
   QJS_AOT_REGION_NOSKIP=1 keeps every field in the loop. QJS_AOT_REGION_SKIPALL=1 (a vkit knock-out) leaves every field out, which leaks what a field holds. */
static void rg_sk_fref(RCtx *c) {
  int ki;
  static int noskip = -1, skipall = -1;
  if (noskip < 0) { noskip = getenv("QJS_AOT_REGION_NOSKIP") != NULL; skipall = getenv("QJS_AOT_REGION_SKIPALL") != NULL; }
  for (ki = 0; ki < c->ncons; ki++) {
    RCons *K = &c->cons[ki];
    uint32_t mask = 0;
    int q;
    K->fref = ~0u;
    if (!K->stack || K->kind != 0 || K->sk_pair >= 0 || noskip || K->accum || K->rslot) continue;
    if (K->stover || K->nst == 0) { if (!skipall) continue; }
    for (q = 0; q < K->nst && !skipall; q++) {
      int n = K->st[q][0], v = K->st[q][1], fidx = K->st[q][2], ty;
      if (n < 0 || n >= c->nin_total || c->in[n] == NULL) { mask = ~0u; break; }
      ty = rg_tyof(c->in[n], v);
      if (ty != RT_I32 && ty != RT_F64) mask |= fidx < 0 ? (K->nn < 32 && getenv("QJS_AOT_REGION_NOLATEMASK") == NULL ? ~0u << K->nn : ~0u) : 1u << fidx;   /* a late field's slot is not named here: every slot past the construction's own */
    }
    if (K->late && getenv("QJS_AOT_REGION_NOLATEMASK") == NULL) { unsigned room_ = rg_stack_room(K); if (room_ > 0 && room_ < 32) mask &= (1u << room_) - 1u; }   /* an object with late fields holds no more than its block's room: the late region of the mask ends there */
    K->fref = mask;
    if (getenv("QJS_AOT_REGION_WHY")) fprintf(stderr, "stackobj: root %.8s cons %d release reads fields %x of %d%s\n", c->hex, ki, mask, K->nn, K->late ? " + late" : "");
  }
}

/* Virtual stack fields: a stack object whose every field access is one the analysis knows (a read or a write at a named field of an alias that is exactly the object) keeps its
   fields in locals; the block is made as before (its header and shape are real) and the fields are written into it only where it is observed (an exit, before the migration).
   vcls: the boxing the old store gave the field's value: 0 an int (JS_MKVAL INT), 1 a double made by arithmetic (double-tagged), 2 a typed double through the normalised box. A field
   is virtual only if every store to it has the same class, so the block a materialise writes holds what the last store of the old code held. */
static int rg_virt_off(void) { static int v = -1; if (v < 0) v = getenv("QJS_AOT_REGION_NOVIRT") != NULL || getenv("QJS_AOT_REGION_F64NARROWEXIT") != NULL || getenv("QJS_AOT_REGION_F64NOCLAIM") != NULL; return v; }
static void rg_virt_decide(RCtx *c) {
  int ki, nv = 0, nmix = 0, nrej = 0;
  int why = getenv("QJS_AOT_REGION_WHY") != NULL;
  for (ki = 0; ki < c->ncons; ki++) {
    RCons *K = &c->cons[ki];
    unsigned full;
    int q, j, cls[16], ok = 1, fresh_;
    unsigned mixf_ = 0, noint_ = 0, seen_ = 0;
    const char *r = "";
    K->virt = 0;
    if (rg_virt_off() || !K->stack || K->kind != 0) continue;
    full = K->nn >= 16 ? 0xffffu : (1u << K->nn) - 1;
    { static char rb_[160]; rb_[0] = 0;
      if (!K->direct) strcat(rb_, "!direct ");
      if (K->fref != 0) strcat(rb_, "fref ");
      if (K->stover) strcat(rb_, "stover ");
      if (K->nst == 0) strcat(rb_, "nst0 ");
      if (K->late) strcat(rb_, "late ");
      if (K->sk_pair >= 0) strcat(rb_, "pair ");
      if (K->chi) strcat(rb_, "child ");
      if (K->par) strcat(rb_, "isChild ");
      if (K->nmat) strcat(rb_, "mat ");
      if (K->accum) strcat(rb_, "accum ");
      if (K->nrbs) strcat(rb_, "rbs ");
      if (K->knock) strcat(rb_, "knock ");
      if (!K->vok) strcat(rb_, "unclassified-use ");
      if (K->nn <= 0 || K->nn > 16) strcat(rb_, "nn ");
      if (K->nkvis <= 0) strcat(rb_, "nokvis ");
      if (rb_[0]) r = rb_; }
    if (!r[0] && (K->vcov & full) != full) r = "a field the constructor does not store on every path";
    if (r[0]) { if (why) fprintf(stderr, "virtual: root %.8s cons %d stays a block (%s)\n", c->hex, ki, r); nrej++; continue; }
    for (j = 0; j < 16; j++) cls[j] = -1;
    for (q = 0; ok && q < K->nst; q++) {
      int n = K->st[q][0], v = K->st[q][1], fx = K->st[q][2], ix = K->st[q][3], ty, cl;
      RInst *I;
      char tmp_[96];
      if (n < 0 || n >= c->nin_total || c->in[n] == NULL || fx < 0 || ix < 0 || ix >= c->in[n]->q.ninsns) { ok = 0; r = "a store the analysis cannot place"; break; }
      I = c->in[n];
      ty = rg_tyof(I, v);
      if (ty != RT_I32 && ty != RT_F64) { ok = 0; r = "a store of a value that is not a number"; break; }
      /* the constructor's own store of a direct object is by slot index with no claim check, boxed through the normalised form; every other store is the plain one at a fixed slot */
      fresh_ = I->fresh && I->cons > 0 && rg_is_this(&I->q, I->q.insns[ix].use[0]);
      if (!fresh_) {
        int tf_ = rg_trust_field(I, &I->q.insns[ix], 1, tmp_, sizeof(tmp_));
        if (!tf_) { ok = 0; r = "a store with no fixed slot (trust_field)"; break; }
        if (!g_rg_plain && __builtin_popcount(g_rg_cls) < 2) { ok = 0; r = "a store into a field of one kind that may be a reference (a claim may be broken by the store)"; break; }
        if (rg_store_narrow(I, v, g_rg_cls)) { ok = 0; r = "a double store the layout claim narrows to an int"; break; }
        seen_ |= 1u << fx;
        if (!(g_rg_cls & (8u | 16u)) || !(g_rg_cls & (32u | 64u))) noint_ |= 1u << fx;   /* the claim must allow BOTH tags: an int (K_INT|K_INTOV) and a double (K_DBL|K_NUM) */
      }
      cl = ty == RT_I32 ? 0 : (!fresh_ && I->rw != NULL && I->rw[v] ? 1 : 2);
      if (cls[fx] < 0) cls[fx] = cl;
      else if (cls[fx] != cl) {
        /* int and double stores of one number field: a number is the same JS value whichever tag the block holds, so the field is one double local and the block gets the normalised box
           (int when exact, else double). Sound only where every plain store's claim already allows an int in the field (the block may then hold either tag at every store) */
        if (cls[fx] != 3 && getenv("QJS_AOT_REGION_NOMIXBOX") == NULL) { cls[fx] = 3; mixf_ |= 1u << fx; }
        else if (cls[fx] != 3) { ok = 0; r = "stores of different boxing classes"; nmix++; if (why) fprintf(stderr, "virtual:   mixed classes in cons %d field %d: %d then %d (store %d: fresh=%d rw=%d ty=%d)\n", ki, fx, cls[fx], cl, q, fresh_, I->rw != NULL && I->rw[v], ty); break; }
      }
    }
    if (ok) for (j = 0; j < K->nn; j++) if (cls[j] < 0) { ok = 0; r = "a field that is never stored"; break; }
    if (ok && getenv("QJS_AOT_REGION_MIXLOOSE") == NULL && (mixf_ & (noint_ | ~seen_))) { ok = 0; r = "stores of different boxing classes (no plain store whose claim allows both an int and a double)"; nmix++; }
    if (ok) for (j = 0; j < K->nn; j++) if (cls[j] == 3) cls[j] = 2;
    if (!ok) { if (why) fprintf(stderr, "virtual: root %.8s cons %d stays a block (%s)\n", c->hex, ki, r); nrej++; continue; }
    for (j = 0; j < K->nn; j++) { K->vcls[j] = (unsigned char)cls[j]; K->vty[j] = cls[j] == 0 ? RT_I32 : RT_F64; }
    { static int cnt_ = 0; const char *mx_ = getenv("QJS_AOT_REGION_VIRTMAX"), *mn_ = getenv("QJS_AOT_REGION_VIRTMIN"); int me_ = cnt_++; if ((mx_ != NULL && me_ >= atoi(mx_)) || (mn_ != NULL && me_ < atoi(mn_))) continue; }   /* bisection of a fault: only the constructs numbered [MIN, MAX) */
    K->virt = 1;
    nv++;
    if (why) fprintf(stderr, "virtual: root %.8s cons %d (%s, %d fields) is virtual\n", c->hex, ki, K->gname, K->nn);
  }
  if (why || getenv("QJS_AOT_REGION_VIRTSTAT") != NULL) fprintf(stderr, "virtual: root %.8s: %d virtual, %d stay blocks (%d of them for mixed boxing classes)\n", c->hex, nv, nrej, nmix);
}
/* The virtual construct whose field `nm` an access of instance I (the receiver value u0, or the instance's `this` when thisop) reaches, and the field's index. */
static int rg_virt_at(RCtx *c, const RInst *I, int u0, int thisop, const char *nm, int *fio) {
  int ki, w, j;
  for (ki = 0; ki < c->ncons; ki++) {
    const RCons *K = &c->cons[ki];
    int hit = 0;
    if (!K->virt) continue;
    if (thisop) { for (w = 0; w < K->nvthis; w++) if (K->vthis[w][0] == I->id) hit = 1; }
    else for (w = 0; w < K->nkvis; w++) if (K->kvis[w][0] == I->id && K->kvis[w][1] == u0) hit = 1;
    if (!hit) continue;
    for (j = 0; j < K->nn && strcmp(K->names[j], nm) != 0; j++) {}
    if (j == K->nn) return -1;
    *fio = j;
    return ki;
  }
  return -1;
}
/* the boxed form of virtual field j of construct ki, as the old store wrote it */
static void rg_virt_box(const RCons *K, int ki, int j, char *buf, size_t cap) {
  if (K->vcls[j] == 0) snprintf(buf, cap, "JS_MKVAL(JS_TAG_INT, vf_%d_%d)", ki, j);
  else if (K->vcls[j] == 1) snprintf(buf, cap, "__JS_NewFloat64(vf_%d_%d)", ki, j);
  else snprintf(buf, cap, "qjs_aot_box_num(vf_%d_%d, qjs_aot_fitsi(vf_%d_%d))", ki, j, ki, j);
}
/* a store into a virtual field: the local, the mask bit, and under VFCHECK the old store as well */
static void rg_virt_put(RCtx *c, FILE *o, const RInst *I, int ki, int j, int u1, const char *recv) {
  const RCons *K = &c->cons[ki];
  char bx[160];
  if (K->vty[j] == RT_F64 && rg_tyof(I, u1) == RT_I32) fprintf(o, "        vf_%d_%d = (double)%si%d; vs_%d |= %uu;\n", ki, j, I->pfx, u1, ki, 1u << j);
  else fprintf(o, "        vf_%d_%d = %s%c%d; vs_%d |= %uu;\n", ki, j, I->pfx, K->vty[j] == RT_I32 ? 'i' : 'd', u1, ki, 1u << j);
  if (getenv("QJS_AOT_REGION_VFCHECK") != NULL) {
    rg_virt_box(K, ki, j, bx, sizeof(bx));
    fprintf(o, "        if (Q2_CLAIM_BAD(%s, %du * QJS_AOT_SIZEOF_JSPROPERTY, %s) && !(Q2_LAYOUT_BROKEN(ctx) & (1ull << (*(const uint16_t *)(*(const unsigned char *const *)((const unsigned char *)JS_VALUE_GET_PTR(%s) + QJS_AOT_OFF_JSOBJECT_SHAPE) + QJS_AOT_OFF_SHAPE_LAYOUT_ID) - 1u)))) { fprintf(stderr, \"VFCHECK: the old store of cons %d field %d would have failed its layout claim\\n\"); abort(); }\n", recv, j, bx, recv, ki, j);
    fprintf(o, "        *q2_slotp(%s, %du * QJS_AOT_SIZEOF_JSPROPERTY) = %s;   /* VFCHECK: the block the old code would hold */\n", recv, j, bx);
  }
}
/* a read of a virtual field into the JSValue `dst` */
/* where a virtual read's result goes: the typed local when the value is typed, else the boxed value */
static void rg_virt_dst(const RInst *I, int dv, char *buf, size_t cap) {
  int t = getenv("QJS_AOT_REGION_VTYOFF") != NULL ? RT_ANY : rg_tyof(I, dv);
  if (t == RT_I32 || t == RT_F64) g_vty_dv = dv;
  if (t == RT_I32) snprintf(buf, cap, "%si%d", I->pfx, dv);
  else if (t == RT_F64) snprintf(buf, cap, "%sd%d", I->pfx, dv);
  else snprintf(buf, cap, "%sv%d", I->pfx, dv);
}
static void rg_virt_get(RCtx *c, FILE *o, int ki, int j, const char *dst, const char *recv, int dty) {
  if (getenv("QJS_AOT_REGION_VTYOFF") != NULL) dty = RT_ANY;
  const RCons *K = &c->cons[ki];
  char bx[160];
  rg_virt_box(K, ki, j, bx, sizeof(bx));
  if (getenv("QJS_AOT_REGION_VFCHECK") != NULL)
    fprintf(o, "        { JSValue b_ = *q2_slotp(%s, %du * QJS_AOT_SIZEOF_JSPROPERTY), e_ = %s; if (!(vs_%d & %uu) || rg_vdiff(b_, e_)) { fprintf(stderr, \"VFCHECK read cons %d field %d: block %%llx local %%llx stored %%d\\n\", (unsigned long long)*(uint64_t *)&b_, (unsigned long long)*(uint64_t *)&e_, !!(vs_%d & %uu)); abort(); } }\n",
            recv, j, bx, ki, 1u << j, ki, j, ki, 1u << j);
  if (dty == RT_I32 || dty == RT_F64) fprintf(o, "        %s = vf_%d_%d;\n", dst, ki, j);
  else fprintf(o, "        %s = %s;\n", dst, bx);
}

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
  c->lent = rg_lent_mask(c, root);
  rg_lent_pred_set(hex, c->lent);
  if (rg_trust_on()) {
    /* profitability: one C function holding thousands of inlined values compiles to spill-bound code (and minutes of compile); until a region can call a
       hot callee out of line, such a tree stays with the regions of its parts */
    int vt = 0, ia;
    for (ia = 0; ia < c->nin; ia++) if (!c->in[ia]->ool) vt += c->in[ia]->q.nvalues;
    if (rg_line_cap() > 0 && g_ncut < 1000) {
      /* the line cap: estimate = lines-per-value * values of the instances not called out of line; cut the coldest (then the largest) subtrees until it fits */
      int cap = rg_line_cap(), lpv = rg_lines_per_value(), nin = c->nin, cut_any = 0, rounds = 0;
      long est;
      int *sub = calloc((size_t)nin, sizeof(int)), *gone = calloc((size_t)nin, sizeof(int));
      unsigned long long *cnt = calloc((size_t)nin, sizeof(unsigned long long));
      if (sub == NULL || gone == NULL || cnt == NULL) { free(sub); free(gone); free(cnt); snprintf(why, whyn, "no memory"); goto cleanup; }
      for (ia = 0; ia < nin; ia++) {
        const RInst *Ia = c->in[ia];
        cnt[ia] = Ia != NULL && Ia->par >= 0 && c->in[Ia->par] != NULL && c->in[Ia->par]->g != NULL ? qjs_aot_prof_site_calls(c->in[Ia->par]->g->digest, Ia->par_pc_after) : ~0ull;
      }
      for (;;) {
        int best = -1, k, a;
        est = 0;
        for (ia = 0; ia < nin; ia++) { sub[ia] = 0; }
        for (ia = nin - 1; ia >= 0; ia--) {
          const RInst *Ia = c->in[ia];
          int anc_gone = 0;
          if (Ia == NULL) continue;
          for (a = ia; a >= 0 && !anc_gone; a = c->in[a]->par) { if (gone[a]) anc_gone = 1; if (a == 0) break; }
          if (anc_gone || Ia->ool) continue;
          sub[ia] += Ia->q.nvalues;
          if (Ia->par >= 0) sub[Ia->par] += sub[ia];
          est += (long)Ia->q.nvalues * lpv;
        }
        if (est <= cap || rounds++ > 400) break;
        for (k = 1; k < nin; k++) {
          const RInst *Ik = c->in[k];
          int anc_gone = 0;
          if (Ik == NULL || gone[k] || Ik->ool || Ik->fresh || Ik->wrap || Ik->cons > 0 || Ik->par < 0) continue;
          for (a = Ik->par; a >= 0 && !anc_gone; a = c->in[a]->par) { if (gone[a]) anc_gone = 1; if (a == 0) break; }
          if (anc_gone) continue;
          if (best < 0 || cnt[k] < cnt[best] || (cnt[k] == cnt[best] && sub[k] > sub[best])) best = k;
        }
        if (best < 0) break;
        gone[best] = 1;
        if (g_ncut < 1024) { g_cut[g_ncut++] = c->in[best]->key; cut_any = 1; }
      }
      if (cut_any) {
        if (getenv("QJS_AOT_REGION_INSTS") != NULL || getenv("QJS_AOT_FN_LINE_CAP_LOG") != NULL)
          fprintf(stderr, "fn-line-cap root=%s cap=%d est_before=%ld est_after=%ld cut=%d of %d instances\n", hex, cap, (long)vt * lpv, est, g_ncut, nin);
        free(sub); free(gone); free(cnt);
        g_cut_retry = 1;
        snprintf(why, whyn, "line cap: walking again with %d instances out of line", g_ncut);
        goto cleanup;
      }
      free(sub); free(gone); free(cnt);
    }
    if (vt > (getenv("QJS_AOT_REGION_MAXV") ? atoi(getenv("QJS_AOT_REGION_MAXV")) : 2800)) { snprintf(why, whyn, "too large: %d values in %d inlined instances (profitability)", vt, c->nin); goto cleanup; }
  }
  if (rg_trust_on() && getenv("QJS_AOT_REGION_NUMERIC") == NULL && !rg_any_construct(c) && !rg_any_ool(c) &&
      ((c->ncls == 0 && (c->nin == 1 || (getenv("QJS_AOT_REGION_NOINLGATE") == NULL && rg_stores_double(c))) && !rg_any_dispatch(c)) || (getenv("QJS_AOT_REGION_DBLGATE") != NULL && c->ngd == 0 && rg_stores_double(c)))) {   /* rule (b) is off by default (f64 S2): QJS_AOT_REGION_DBLGATE=1 brings it back */
    /* nothing a region does better than the ordinary body: no object is made, nothing is called out of line, and either nothing is inlined and no class is bound (a loop of
       numbers and arrays), or no call is resolved by class or method (no guard at all: plain calls of constant functions,
       field reads and writes of one known class, element reads, numbers), nothing is called out of line, and the work stores doubles. The ordinary body is typed and unboxed
       (a double stays in a register); the region would box every double it stores. */
    snprintf(why, whyn, "nothing for a region (monomorphic field, element and double work only: the ordinary body is typed and unboxed)%s", c->nin > 1 && c->ncls == 0 ? " [inlined numeric work that stores doubles; QJS_AOT_REGION_NOINLGATE=1 lifts it]" : "");
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
    rg_sk_fref(c);
    rg_virt_decide(c);
    if (getenv("QJS_AOT_REGION_NOVIRTTY") == NULL) {
      int kv_, anyv_ = 0;
      for (kv_ = 0; kv_ < c->ncons; kv_++) if (c->cons[kv_].virt) anyv_ = 1;
      if (anyv_) {
        /* the second type round may only refine the typed locals: every construct's storage decision (stack, virtual, escape, pairing, field boxing classes and coverage) must come out unchanged */
        RCons *snap_ = getenv("QJS_AOT_REGION_VIRTCHECK") != NULL ? malloc((size_t)c->ncons * sizeof(RCons)) : NULL;
        if (snap_ != NULL) memcpy(snap_, c->cons, (size_t)c->ncons * sizeof(RCons));
        rg_ty_decide(c);
        if (snap_ != NULL) {
          for (kv_ = 0; kv_ < c->ncons; kv_++) {
            const RCons *a_ = &snap_[kv_], *b_ = &c->cons[kv_];
            if (a_->virt != b_->virt || a_->stack != b_->stack || a_->esc != b_->esc || a_->sk_pair != b_->sk_pair || a_->vok != b_->vok || a_->vcov != b_->vcov || a_->nn != b_->nn || a_->chi != b_->chi || a_->par != b_->par || memcmp(a_->vcls, b_->vcls, sizeof(a_->vcls)) != 0 || memcmp(a_->vty, b_->vty, sizeof(a_->vty)) != 0) {
              fprintf(stderr, "VIRTCHECK: cons %d changed its storage decision in the second type round\n", kv_);
              e_fatal("internal inconsistency (see the message above)", NULL);
            }
          }
          free(snap_);
        }
      }
    }
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
    if (getenv("QJS_AOT_FASTSTATS") != NULL) fprintf(o, "static unsigned long rgn_%s[%d], rgm_%s[%d];\n", hex, RG_MAXC + 1, hex, RG_MAXC + 1);
    
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
  if (getenv("QJS_AOT_REGION_INSTS") != NULL) {
    /* census: the instances of this region (r<id> in the C text), for tools/aot/inline-census.py */
    int ia;
    for (ia = 0; ia < c->nin_total; ia++) {
      char hx[33];
      if (c->in[ia] == NULL) continue;
      rg_hexof(c->in[ia]->g, hx);
      {
        const RInst *Ia = c->in[ia];
        unsigned long long sc = 0;
        if (Ia->par >= 0 && c->in[Ia->par] != NULL) sc = qjs_aot_prof_site_calls(c->in[Ia->par]->g->digest, Ia->par_pc_after);
        else sc = qjs_aot_prof_entries(Ia->g->digest);
        fprintf(stderr, "region-inst root=%s id=%d fn=%s name=%s line=%d par=%d depth=%d call_pc=%d sitecalls=%llu entries=%llu rootent=%llu\n", hex, ia, hx, Ia->g->name[0] ? Ia->g->name : "<anon>", Ia->g->line, Ia->par, Ia->depth, Ia->par_call_pc, sc, qjs_aot_prof_entries(Ia->g->digest), qjs_aot_prof_entries(c->in[0]->g->digest));
      }
    }
  }
  if (c->nin_total > 1) {
    /* what an exit inside inlined code needs: the callees' generic bodies, their frame sizes, and the function that builds the chain of frames and runs it */
    int ia, ib;
    for (ia = 1; ia < c->nin_total; ia++) {
      char hx[33], hy[33];
      int dup = 0;
      rg_hexof(c->in[ia]->g, hx);
      for (ib = 1; ib < ia && !dup; ib++) { rg_hexof(c->in[ib]->g, hy); dup = strcmp(hx, hy) == 0; }
      if (dup) continue;
      if (!e_nogeneric())
        fprintf(o, "%sJSValue aot_g_%s(JSContext *, JSRegRun *, JSValue *, uint32_t);\n#ifndef Q2_FR_%s\n#error \"region %s: the inlined function %s is not in the unit\"\n#endif\n", e_linkage(), hx, hx, hex, hx);
    }
    if (e_nogeneric())
      fprintf(o, "static JSValue rg_cont_%s(JSContext *c_, JSRegRun *f_, JSValue *fr_, uint32_t pc_) { return qjs_aot_deopt(c_, f_, fr_, pc_, QJS_AOT_NO_SITE); }\n", hex);
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
  if (getenv("QJS_AOT_KO_NUMNORM") != NULL)   /* (the knock-out: the unguarded conversion, whose result for a double outside int32 the optimizer may assume away) */
    fprintf(o, "static inline __attribute__((always_inline)) JSValue rg_numnorm(JSValue v)\n{\n  if (JS_VALUE_GET_TAG(v) == JS_TAG_FLOAT64) { double d = JS_VALUE_GET_FLOAT64(v); int32_t i = (int32_t)d; if (d == (double)i && !(i == 0 && __builtin_signbit(d))) return JS_MKVAL(JS_TAG_INT, i); }\n  return v;\n}\n");
  else
  fprintf(o, "static inline __attribute__((always_inline)) JSValue rg_numnorm(JSValue v)\n{\n  if (JS_VALUE_GET_TAG(v) == JS_TAG_FLOAT64) { double d = JS_VALUE_GET_FLOAT64(v); if (d >= -2147483648.0 && d <= 2147483647.0) { int32_t i = (int32_t)d; if (d == (double)i && !(i == 0 && __builtin_signbit(d))) return JS_MKVAL(JS_TAG_INT, i); } }\n  return v;\n}\n");
  fprintf(o, "static inline __attribute__((always_inline)) void rg_own_put(JSContext *ctx, JSValue **p, int *n, int *cap, JSValue v) { (void)ctx; if (!JS_VALUE_HAS_REF_COUNT(v)) return; if (*n >= (*cap < 0 ? -*cap : *cap)) rg_own_grow(p, cap); (*p)[(*n)++] = v; }\n");
  fprintf(o, "static inline __attribute__((always_inline)) void rg_own_dup(JSContext *ctx, JSValue **p, int *n, int *cap, JSValue v) { if (!JS_VALUE_HAS_REF_COUNT(v)) return; q2_dup_p(v); rg_own_put(ctx, p, n, cap, v); }\n");
  if (getenv("QJS_AOT_REGION_OWNSTAT") != NULL) fprintf(o, "static unsigned long q2_os_[4096]; __attribute__((destructor)) static void q2_os_dump(void) { int i; for (i = 0; i < 4096; i++) if (q2_os_[i]) fprintf(stderr, \"OWNRUN site %%d: %%lu\\n\", i, q2_os_[i]); }\n");
  fprintf(o, "static inline void rg_own_compact(JSContext *ctx, JSValue *p, int *n, int mark, const JSValue *keep, int nk) { int i, w = mark; for (i = mark; i < *n; i++) { int k, kept = 0; for (k = 0; k < nk; k++) if (JS_VALUE_GET_TAG(keep[k]) == JS_VALUE_GET_TAG(p[i]) && JS_VALUE_GET_PTR(keep[k]) == JS_VALUE_GET_PTR(p[i])) { kept = 1; break; } if (kept) p[w++] = p[i]; else JS_FreeValue(ctx, p[i]); } *n = w; }\n");
  fprintf(o, "static inline void rg_own_free(JSContext *ctx, JSValue *p, int n, int heap) { int i; for (i = 0; i < n; i++) JS_FreeValue(ctx, p[i]); if (heap) free(p); }\n#endif\n");
  fprintf(o, "#ifndef RG_IMG_T\n#define RG_IMG_T 1\n"
             "/* A stack object's block is made by copying an image: the bytes the engine's own qjs_aot_stack_object_init_cap writes for the class shape, taken once from a call of it. The words\n"
             "   that point into the block itself (the gc list head, twice, and the property array) are written for the block they are copied to. The size of the image comes from the engine's\n"
             "   layout header; an engine whose block is not the shape this assumes builds no image and every object is made by the call. */\n"
             "#ifndef RG_IMGW\n#define RG_IMGW(room) ((8 + QJS_AOT_OFF_JSOBJECT_PROP + 16 + (room) * QJS_AOT_SIZEOF_JSPROPERTY) / 8)\n#define RG_IMGPW ((8 + QJS_AOT_OFF_JSOBJECT_PROP) / 8)\n#define RG_IMGPU (8 + QJS_AOT_OFF_JSOBJECT_PROP + 16)\n#endif\n"
             "static int rg_img_build(JSContext *ctx, void *shape, unsigned cap, uint64_t *img, unsigned nw)\n{\n"
             "  _Alignas(64) uint64_t b_[48] = {0};\n  JSValue t_;\n  unsigned i;\n  uintptr_t lo = (uintptr_t)b_, hi = lo + (uintptr_t)nw * 8;\n"
             "  if (nw > 48 || (QJS_AOT_OFF_JSOBJECT_PROP & 7) != 0 || RG_IMGPW <= 2 || RG_IMGPW >= nw) return 0;\n"
             "  t_ = qjs_aot_stack_object_init_cap(b_, (size_t)nw * 8, shape, cap);\n  if (JS_VALUE_GET_TAG(t_) != JS_TAG_OBJECT) return 0;\n"
             "  for (i = 0; i < nw; i++) {\n    uint64_t w = b_[i];\n    int self = w >= lo && w < hi;\n"
             "    if (i == 1 || i == 2) { if (w != lo + 8) goto bad; }\n    else if (i == RG_IMGPW) { if (w != lo + RG_IMGPU) goto bad; }\n    else if (self) goto bad;\n  }\n"
             "  memcpy(img, b_, (size_t)nw * 8);\n  img[1] = img[2] = img[RG_IMGPW] = 0;\n  qjs_aot_stack_release(ctx, t_, JS_UNDEFINED);\n  return 1;\n bad:\n  qjs_aot_stack_release(ctx, t_, JS_UNDEFINED);\n  return 0;\n}\n"
             "/* The release of a stack object none of whose fields can hold a reference: the shape's reference count goes down exactly as js_free_shape does it when the count stays above zero (the decrement,\n"
             "   no other work); when it would reach zero the engine's release does it, with the count untouched. */\n"
             "static inline __attribute__((always_inline)) void rg_skrel0(JSContext *ctx, JSValue v)\n{\n"
             "  uint8_t *p_ = (uint8_t *)JS_VALUE_GET_PTR(v);\n  void **shp_ = (void **)(p_ + QJS_AOT_OFF_JSOBJECT_SHAPE);\n  int *rc_ = (int *)((uint8_t *)*shp_ - QJS_AOT_RC_BACK);\n"
             "  if (__builtin_expect(*rc_ > 1, 1)) { --*rc_; *shp_ = NULL; } else qjs_aot_stack_release_m(ctx, v, 0u);\n}\n"
             "/* the release of a stack object whose reference-holding fields are the bits of a constant mask (below 8): those fields are tested in line, then the shape reference is dropped */\n"
             "#define RG_SKF_(i_) if (((m) >> (i_)) & 1u && (i_) < n_ && JS_VALUE_HAS_REF_COUNT(f_[i_])) JS_FreeValue(ctx, f_[i_]);\n"
             "static inline __attribute__((always_inline)) void rg_skrelm(JSContext *ctx, JSValue v, uint32_t m)\n{\n"
             "  uint8_t *p_ = (uint8_t *)JS_VALUE_GET_PTR(v);\n  void **shp_ = (void **)(p_ + QJS_AOT_OFF_JSOBJECT_SHAPE);\n"
             "  int n_ = *(const int *)((const uint8_t *)*shp_ + QJS_AOT_OFF_JSSHAPE_PROP_COUNT);\n  JSValue *f_ = q2_slotp(v, 0);\n"
             "  RG_SKF_(0) RG_SKF_(1) RG_SKF_(2) RG_SKF_(3) RG_SKF_(4) RG_SKF_(5) RG_SKF_(6) RG_SKF_(7)\n"
             "  { int *rc_ = (int *)((uint8_t *)*shp_ - QJS_AOT_RC_BACK);\n"
             "    if (__builtin_expect(*rc_ > 1, 1)) { --*rc_; *shp_ = NULL; } else qjs_aot_stack_release_m(ctx, v, 0u); }\n}\n#undef RG_SKF_\n"
             "static inline __attribute__((always_inline)) void rg_img_copy(uint64_t *d, const uint64_t *img, unsigned nw)\n{\n"
             "  memcpy(d, img, (size_t)nw * 8);\n  d[1] = d[2] = (uint64_t)(uintptr_t)(d + 1);\n  d[RG_IMGPW] = (uint64_t)((uintptr_t)d + RG_IMGPU);\n}\n#endif\n");
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
  if (c->nss > 0) {
    /* the exits' work on the stack blocks (migrating the live ones to the heap, releasing them) is two functions per region, not text in every exit stub: the blocks are an array of
       equal blocks (b), their liveness (on) and the heap objects migration made (h); pr[i] is the Array slot paired with object slot i, -1 for none, -2 for an Array slot */
    int g1, w = c->skstride / 8;
    fprintf(o, "static const signed char rg_skp_%s[%d] = {", hex, c->nss);
    for (g1 = 0; g1 < c->nss; g1++) {
      int pr = -2, q;
      for (q = 0; q < c->ncons; q++)
        if (c->cons[q].stack && c->cons[q].ss == g1) pr = c->cons[q].kind == 1 ? -2 : c->cons[q].par > 0 ? -3 : c->cons[q].sk_pair >= 0 ? c->cons[c->cons[q].sk_pair].ss : -1;
      fprintf(o, "%s%d", g1 ? ", " : "", pr);
    }
    fprintf(o, "};\n");
    /* a container's stack child (the slot of the object its field holds) and a child's container: the pair migrates together, and a child whose container is gone moves alone */
    fprintf(o, "static const signed char rg_skc_%s[%d] = {", hex, c->nss);
    for (g1 = 0; g1 < c->nss; g1++) {
      int pc_ = -1, q;
      for (q = 0; q < c->ncons; q++)
        if (c->cons[q].stack && c->cons[q].ss == g1 && c->cons[q].chi > 0) pc_ = c->cons[c->cons[q].chi - 1].ss;
      fprintf(o, "%s%d", g1 ? ", " : "", pc_);
    }
    fprintf(o, "};\nstatic const signed char rg_skq_%s[%d] = {", hex, c->nss);
    for (g1 = 0; g1 < c->nss; g1++) {
      int pc_ = -1, q;
      for (q = 0; q < c->ncons; q++)
        if (c->cons[q].stack && c->cons[q].ss == g1 && c->cons[q].par > 0) pc_ = c->cons[c->cons[q].par - 1].ss;
      fprintf(o, "%s%d", g1 ? ", " : "", pc_);
    }
    fprintf(o, "};\n");
    {
      /* the materialise of each virtual object: its fields into the block, with the boxing the old stores gave them (only the fields stored so far: vs bit j) */
      int kv, any_ = 0;
      for (kv = 0; kv < c->ncons; kv++) if (c->cons[kv].virt) any_ = 1;
      if (any_) {
        fprintf(o, "#ifndef RG_VDIFF\n#define RG_VDIFF\nstatic inline int rg_vdiff(JSValue a, JSValue b) {\n  if (JS_VALUE_GET_TAG(a) != JS_VALUE_GET_TAG(b)) return 1;\n"
                   "  if (JS_VALUE_GET_TAG(a) == JS_TAG_INT) return JS_VALUE_GET_INT(a) != JS_VALUE_GET_INT(b);\n"
                   "  { double x = JS_VALUE_GET_FLOAT64(a), y = JS_VALUE_GET_FLOAT64(b); return memcmp(&x, &y, sizeof x) != 0; }\n}\n#endif\n");
        for (kv = 0; kv < c->ncons; kv++) {
          const RCons *VK = &c->cons[kv];
          int j;
          if (!VK->virt) continue;
          fprintf(o, "static __attribute__((noinline, cold)) void rg_vmat_%s_%d(JSContext *ctx, uint64_t *blk, unsigned vs", hex, kv);
          for (j = 0; j < VK->nn; j++) fprintf(o, ", %s vf%d", VK->vty[j] == RT_I32 ? "int32_t" : "double", j);
          fprintf(o, ") {\n  JSValue ob_ = JS_MKPTR(JS_TAG_OBJECT, (void *)((uint8_t *)blk + 8)); (void)ctx;\n");
          if (getenv("QJS_AOT_REGION_VFCHECK") == NULL) {
            /* the block of a virtual object is made here, when something first observes it: as the construct would have made it (the image copy and the shape's reference, or the engine's init), then the defaults of the prototype */
            if (rg_img_ok(VK))
              fprintf(o, "  if (__builtin_expect(rg_imgok_%s_%d, 1)) { rg_img_copy(blk, rg_img_%s_%d, RGIMGW_%s_%d); ++*(int *)((uint8_t *)rgc_s_%s_%d - QJS_AOT_RC_BACK); }\n  else ", hex, kv, hex, kv, hex, kv, hex, kv);
            fprintf(o, "{ JSValue t_ = qjs_aot_stack_object_init_cap(blk, %du, rgc_s_%s_%d, 0u); if (JS_VALUE_GET_TAG(t_) == JS_TAG_UNINITIALIZED) abort(); }\n", c->skstride, hex, kv);
            for (j = 0; j < VK->nn; j++)
              if (VK->pre & (1u << j)) fprintf(o, "  *q2_slotp(ob_, %uu * QJS_AOT_SIZEOF_JSPROPERTY) = rgc_dv_%s_%d[%d];\n", j, hex, kv, j);
          }
          for (j = 0; j < VK->nn; j++) {
            char bx[160];
            if (VK->vcls[j] == 0) snprintf(bx, sizeof(bx), "JS_MKVAL(JS_TAG_INT, vf%d)", j);
            else if (VK->vcls[j] == 1) snprintf(bx, sizeof(bx), "__JS_NewFloat64(vf%d)", j);
            else snprintf(bx, sizeof(bx), "qjs_aot_box_num(vf%d, qjs_aot_fitsi(vf%d))", j, j);
            if (getenv("QJS_AOT_REGION_VFCHECK") != NULL)
              fprintf(o, "  if (vs & %uu) { JSValue b_ = *q2_slotp(ob_, %uu * QJS_AOT_SIZEOF_JSPROPERTY), e_ = %s; if (rg_vdiff(b_, e_)) { fprintf(stderr, \"VFCHECK materialise cons %d field %d\\n\"); abort(); } }\n", 1u << j, j, bx, kv, j);
            fprintf(o, "  if (vs & %uu) *q2_slotp(ob_, %uu * QJS_AOT_SIZEOF_JSPROPERTY) = %s;\n", 1u << j, j, bx);
          }
          fprintf(o, "}\n");
        }
      }
    }
    fprintf(o, "#define RG_SKV(i_) JS_MKPTR(JS_TAG_OBJECT, (void *)((uint8_t *)(b + (size_t)(i_) * %d) + 8))\n", w);
    fprintf(o, "static __attribute__((noinline, cold)) int rg_skm_%s(JSContext *ctx, uint64_t *b, int *on, JSValue *h) {\n  JSValue ha_ = JS_UNDEFINED; int i;\n  h[%d] = JS_NULL;   /* heap objects may be parked in h: the release looks */\n  for (i = 0; i < %d; i++) {\n    int a = rg_skp_%s[i], ch = rg_skc_%s[i];\n"
               "    if (a == -2 || !(on[i] || (a >= 0 && on[a]))) continue;\n"
               "    if (a == -3 && on[rg_skq_%s[i]]) continue;   /* a child moves with its container */\n"
               "    if (ch >= 0 && on[ch]) { if (!qjs_aot_stack_migrate_child(ctx, on[i] ? RG_SKV(i) : JS_UNDEFINED, RG_SKV(ch), &h[i], &h[ch])) return 0; on[i] = 0; on[ch] = 0; continue; }\n"
               "    if (!qjs_aot_stack_migrate(ctx, on[i] ? RG_SKV(i) : JS_UNDEFINED, a >= 0 && on[a] ? RG_SKV(a) : JS_UNDEFINED, &h[i], a >= 0 ? &h[a] : &ha_)) return 0;\n"
               "    on[i] = 0; if (a >= 0) on[a] = 0;\n  }\n  return 1;\n}\n", hex, c->nss, c->nss, hex, hex, hex);
    /* the release of every live block, one straight-line test per slot (the slot's pair and field mask are known here) */
    fprintf(o, "static void rg_skr_%s(JSContext *ctx, uint64_t *b, int *on, JSValue *h) {\n", hex);
    for (g1 = 0; g1 < c->nss; g1++) {
      int q, kq = -1;
      for (q = 0; q < c->ncons; q++) if (c->cons[q].stack && c->cons[q].ss == g1) kq = q;
      if (kq >= 0 && c->cons[kq].kind != 1) {
        const RCons *Kq = &c->cons[kq];
        if (Kq->sk_pair >= 0) {
          int pj = c->cons[Kq->sk_pair].ss;
          fprintf(o, "  if (on[%d] || on[%d]) { qjs_aot_stack_release(ctx, on[%d] ? RG_SKV(%d) : JS_UNDEFINED, on[%d] ? RG_SKV(%d) : JS_UNDEFINED); on[%d] = 0; on[%d] = 0; }\n", g1, pj, g1, g1, pj, pj, g1, pj);
        } else if (Kq->fref == 0 && getenv("QJS_AOT_REGION_MASKCHECK") == NULL && getenv("QJS_AOT_REGION_NOINLREL") == NULL)
          fprintf(o, "  if (on[%d]) { rg_skrel0(ctx, RG_SKV(%d)); on[%d] = 0; }\n", g1, g1, g1);
        else if (Kq->fref != ~0u && (Kq->fref >> 8) == 0 && getenv("QJS_AOT_REGION_MASKCHECK") == NULL && getenv("QJS_AOT_REGION_NOINLREL") == NULL)
          fprintf(o, "  if (on[%d]) { rg_skrelm(ctx, RG_SKV(%d), %uu); on[%d] = 0; }\n", g1, g1, Kq->fref, g1);
        else if (Kq->fref != ~0u)
          fprintf(o, "  if (on[%d]) { qjs_aot_stack_release_m(ctx, RG_SKV(%d), %uu); on[%d] = 0; }\n", g1, g1, Kq->fref, g1);
        else
          fprintf(o, "  if (on[%d]) { qjs_aot_stack_release(ctx, RG_SKV(%d), JS_UNDEFINED); on[%d] = 0; }\n", g1, g1, g1);
      }
    }
    fprintf(o, "  if (__builtin_expect(JS_VALUE_GET_TAG(h[%d]) != JS_TAG_UNDEFINED, 0)) { int i; for (i = 0; i <= %d; i++) { if (JS_VALUE_HAS_REF_COUNT(h[i])) JS_FreeValue(ctx, h[i]); h[i] = JS_UNDEFINED; } }\n", c->nss, c->nss);
    fprintf(o, "}\n#undef RG_SKV\n");
  }
  if (c->lent != 0) {
    /* parameters a caller may have lent as stack blocks: whatever leaves this body for another body that reads the arguments (the generic or the speculative one, an exit stub) first moves them to
       the heap, and the arguments the callee sees are the heap objects. Every `aot_g_`/`aot_fs_` call in this function goes through the move. */
    int k_;
    fprintf(o, "#define RG_BORP() ({ JSValue *ba_ = *(JSValue **)((uint8_t *)f + QJS_AOT_OFF_REGRUN_ARG_BUF); int bc_ = (int)((*(uint64_t *)((uint8_t *)f + QJS_AOT_OFF_REGRUN_HDR_PACK) >> 16) & 0xffffu); (void)ba_; (void)bc_;");
    for (k_ = 0; k_ < 8; k_++) if ((c->lent >> k_) & 1) fprintf(o, " if (%d < bc_ && __builtin_expect(!qjs_aot_stack_unlend(ctx, &ba_[%d]), 0)) return JS_EXCEPTION;", k_, k_);
    fprintf(o, " 0; })\n");
    fprintf(o, "#define aot_g_%s(a_, b_, c_, d_) (RG_BORP(), (aot_g_%s)(a_, b_, c_, d_))\n#define aot_fs_%s(a_, b_, c_, d_) (RG_BORP(), (aot_fs_%s)(a_, b_, c_, d_))\n", hex, hex, hex, hex);
  }
  fprintf(o, "/*@RGT3@*/\n");
  fprintf(o, "E_TEXT %sJSValue aot_f_%s(JSContext *ctx, JSRegRun *f, JSValue *fr, uint32_t entry_pc)\n{\n", e_linkage(), hex);
  if (rg_has_retslot(c)) fprintf(o, "    JSValue *rs_ = q2_rs_pending; q2_rs_pending = NULL;   /* the shell the entry handed over, if any */\n");
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
    {
      /* F21: the root arguments a trusted fixed-slot access reads through are objects of the classes the accesses assume; anything else runs the generic body */
      int ra_;
      if (getenv("QJS_AOT_REGION_DOMCHECK") != NULL) fprintf(stderr, "domcheck: region %s: %d trusted accesses have a receiver nothing proves (tested at the access), root arguments tested once at the entry: mask %x\n", hex, c->ra_flag, c->ra_mask);
      for (ra_ = 0; ra_ < 8; ra_++) {
        int kq_, first_ = 1;
        if (!((c->ra_mask >> ra_) & 1u)) continue;
        fprintf(o, "    { JSValue ra_ = %d < f_argc ? f_argv[%d] : JS_UNDEFINED;\n      if (__builtin_expect(JS_VALUE_GET_TAG(ra_) != JS_TAG_OBJECT", ra_, ra_);
        if (!c->ra_any[ra_] && c->ra_cls[ra_] != 0) {
          fprintf(o, " || (");
          for (kq_ = 0; kq_ < 32; kq_++) if ((c->ra_cls[ra_] >> kq_) & 1u) { fprintf(o, "%s*(const void *const *)((const char *)JS_VALUE_GET_PTR(ra_) + QJS_AOT_OFF_JSOBJECT_SHAPE) != rk_s_%s[%d]", first_ ? "" : " && ", hex, kq_); first_ = 0; }
          fprintf(o, ")");
        }
        fprintf(o, ", 0)) return aot_g_%s(ctx, f, fr, entry_pc); }\n", hex);
      }
    }
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
  for (g = 0; g < c->ncons; g++) if (c->cons[g].kind != 2 && !c->cons[g].rslot) fprintf(o, "    if (__builtin_expect(!rgc_ok_%s_%d, 0) && !rg_clearn_%s_%d(ctx)) return aot_g_%s(ctx, f, fr, entry_pc);\n", hex, g, hex, g, hex);
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
  for (g = 0; g < c->ncons; g++)
    if (c->cons[g].virt) {
      int j;
      fprintf(o, "    unsigned vs_%d = 0; int vk_%d = 0; (void)vs_%d; (void)vk_%d;\n", g, g, g, g);
      for (j = 0; j < c->cons[g].nn; j++) fprintf(o, "    %s vf_%d_%d = 0; (void)vf_%d_%d;\n", c->cons[g].vty[j] == RT_I32 ? "int32_t" : "double", g, j, g, j);
    }
  if (c->nss > 0) {
    /* the stack blocks of the frame: [JSMallocBlockHeader][body] each, the object 8 bytes in. Their references are counted ones held by native code (the collector's trial decrement
       leaves them positive: roots). sks_on: the block is alive; sks_h: the heap object a migration made of it (the region owns it). */
    fprintf(o, "    _Alignas(16) uint64_t sks_b[%d][%d]; int sks_on[%d] = {0}; JSValue sks_h[%d], sks_m[%d]; JSValue sk_ha = JS_UNDEFINED; (void)sk_ha; (void)sks_b; (void)sks_m;\n    { int i_; for (i_ = 0; i_ <= %d; i_++) { sks_h[i_] = JS_UNDEFINED; } }\n",
            c->nss, c->skstride / 8, c->nss, c->nss + 1, c->nss, c->nss);
    for (g = 0; g < c->ncons; g++)
      if (c->cons[g].stack)
        fprintf(o, "#define skb_%d sks_b[%d]\n#define sk_%d sks_on[%d]\n#define rnh_%d sks_h[%d]\n#define SKV_%d JS_MKPTR(JS_TAG_OBJECT, (void *)((uint8_t *)sks_b[%d] + 8))\n", g, c->cons[g].ss, g, c->cons[g].ss, g, c->cons[g].ss, g, c->cons[g].ss);
    /* SKFIX(x): x, or the heap object a migration made when x is a stack block (only meaningful after the exit's migration) */
    fprintf(o, "#define SKFIX(...) ({ JSValue skx_ = (__VA_ARGS__); (JS_VALUE_GET_TAG(skx_) == JS_TAG_OBJECT && (uintptr_t)JS_VALUE_GET_PTR(skx_) - (uintptr_t)sks_b < sizeof sks_b) ? sks_h[((uintptr_t)JS_VALUE_GET_PTR(skx_) - (uintptr_t)sks_b) / %d] : %s; })\n", c->skstride, c->lent != 0 ? "qjs_aot_stack_forward(skx_)" : "skx_");
  } else if (c->lent != 0) fprintf(o, "#define SKFIX(...) qjs_aot_stack_forward(__VA_ARGS__)\n");
  else fprintf(o, "#define SKFIX(...) (__VA_ARGS__)\n");
  fprintf(o, "#define RG_PM(R_, I_) ({ JSValue pmr_ = JS_GetPropertyStr(ctx, SKFIX(R_), rg_pmn_[I_]); JS_FreeValue(ctx, pmr_); pmr_; })\n");
  fprintf(o, "    JSValue rgo_b_[64]; JSValue *rgo_ = rgo_b_; int rgon_ = 0, rgoc_ = -64; (void)rgon_; (void)rgoc_;\n");
  fprintf(o, "    int rgmk_[48] = {0}; (void)rgmk_;\n    int rg_xi_ = 0; (void)rg_xi_;\n");
  /* the owned list is bounded: past this many entries a site that parked one leaves the region through its ordinary exit (the frame state takes what it needs, the list is freed, the
     generic body goes on). QJS_AOT_REGION_OWNCAP=n sets the bound; QJS_AOT_REGION_NOOWNCAP removes it (the test knock-out) */
  if (getenv("QJS_AOT_REGION_KNOCKOWNCAP") != NULL) fprintf(o, "#define RG_OWNCAP (getenv(\"QJS_AOT_REGION_NOOWNCAP\") || getenv(\"QJS_AOT_REGION_NOCOMPACT\") ? 0x7fffffff : 65536)   /* the knock-out build: the run lifts the bound */\n");
  else fprintf(o, "#define RG_OWNCAP %d\n", getenv("QJS_AOT_REGION_NOOWNCAP") != NULL ? 0x7fffffff : getenv("QJS_AOT_REGION_OWNCAP") ? atoi(getenv("QJS_AOT_REGION_OWNCAP")) : 65536);
  fprintf(o, "#define RG_FREE_OWNED() do {");
  for (g = 0; g < c->ncons; g++) {
    if (!c->cons[g].stack) fprintf(o, " JS_FreeValue(ctx, rn_%d);", g);
  }
  if (c->nss > 0) fprintf(o, " rg_skr_%s(ctx, &sks_b[0][0], sks_on, sks_h);", hex);
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
    if (getenv("QJS_AOT_REGION_EPCENSUS") != NULL) {
      int *seen = calloc((size_t)c->effn + 2, sizeof(int)), *cnt = calloc((size_t)c->effn + 2, sizeof(int));
      int nus = 0, nep = 0, multi = 0, nd = 0, ndok = 0, sumlive = 0, k;
      unsigned char *cu = calloc((size_t)c->nxit + 1, 1);
      if (seen == NULL || cnt == NULL || cu == NULL) abort();
      for (k = 0; k < c->ncen; k++) {
        if (c->cen[k].xi >= c->nxit || !used[c->cen[k].xi]) continue;
        cnt[c->cen[k].ep]++;
      }
      for (k = 0; k < c->ncen; k++) {
        int e_ = c->cen[k].ep, xi_ = c->cen[k].xi;
        if (xi_ >= c->nxit || !used[xi_] || cu[xi_]) continue;
        cu[xi_] = 1; nus++;
        if (!seen[e_]) { seen[e_] = 1; nep++; sumlive += c->effcnt[e_]; }
        if (cnt[e_] >= 2) multi++;
        if (c->cen[k].d > 0) { nd++; if (c->cen[k].outerok) ndok++; }
      }
      fprintf(stderr, "epcensus %s: used stubs %d, distinct effect points %d, stubs in a stretch of 2+ exits %d, summed live-in values %d, stubs in inlined frames %d, of those with no effect in the inner frames %d\n", hex, nus, nep, multi, sumlive, nd, ndok);
      free(seen); free(cnt); free(cu);
    }
    /* tier 3: the prelude of a stub (lent block, virtual objects, stack migration) is the same text for every stub of the region, so it is written once (rx_pre) and the stubs reach it by a jump and
       come back through a switch on the stub's number (rg_xi_): a cold path, the fast path is untouched */
    int pre_shared = 0;
    {
      const char *p0 = NULL;
      int n_ = 0, same = 1;
      for (i = 0; i < c->nxit; i++) {
        RXit *x = &c->xit[i];
        if (!used[i] || x->t3 == NULL || x->t3pre == NULL) continue;
        if (p0 == NULL) p0 = x->t3pre; else if (strcmp(p0, x->t3pre) != 0) same = 0;
        n_++;
      }
      pre_shared = same && n_ >= 2 && getenv("QJS_AOT_REGION_T3PRE") != NULL;   /* opt-in: measured +1.9% Minstr on deltablue/raytrace (the stub-number local and the switch cost the fast path) */
    }
    for (i = 0; i < c->nxit; i++) {
      RXit *x = &c->xit[i];
      if (!used[i]) continue;
      fprintf(o, "    rx_%d:;\n    E_EXIT_NOTE_PC(%d, %d, \"%s\");\n", i, fi, x->pc, hex);
      if (pre_shared && x->t3 != NULL && x->t3pre != NULL) fprintf(o, "    rg_xi_ = %d; goto rx_pre;\n    rxb_%d:;\n", i, i);
      else if (x->t3pre != NULL && x->t3 != NULL) fputs(x->t3pre, o);
      if (getenv("QJS_AOT_FASTSTATS") != NULL) fprintf(o, "    rgx_%s[%d]++; rgxpc_%s[%d] = %d;\n", hex, i, hex, i, x->pc);
      if (x->t3 != NULL) fputs(x->t3, o);
      else {
        fputs(x->root, o);
        fputs(x->rest, o);
      }
      fprintf(o, "    RG_FREE_OWNED();\n    E_RET_G(%s, %d);\n", hex, x->rpc);
      if (x->d > 0) {
        /* a poll in an inlined loop: the frames are materialised as at any exit and the innermost one polls (rg_chain) before it goes on in its generic body */
        fprintf(o, "    rp_%d:; rg_pollp_%s = 1; goto rx_%d;\n", i, hex, i);
      } else {
        fprintf(o, "    rp_%d:;\n", i);
        if (pre_shared && x->t3 != NULL && x->t3pre != NULL) fprintf(o, "    rg_xi_ = %d; goto rx_pre;\n    rpb_%d:;\n", RG_MAXX + i, i);
        else if (x->t3pre != NULL && x->t3 != NULL) fputs(x->t3pre, o);
        if (x->t3 != NULL) fputs(x->t3, o);
        else
        fputs(x->root, o);
        fprintf(o, "    RG_FREE_OWNED();\n    if (qjs_aot_poll_fl(ctx) < 0) return JS_EXCEPTION;\n    E_RET_G(%s, %d);\n", hex, x->rpc);
      }
    }
    if (pre_shared) {
      const char *p0 = NULL;
      for (i = 0; i < c->nxit && p0 == NULL; i++) if (used[i] && c->xit[i].t3 != NULL && c->xit[i].t3pre != NULL) p0 = c->xit[i].t3pre;
      fprintf(o, "    rx_pre:;\n%s    switch (rg_xi_) {\n", p0);
      for (i = 0; i < c->nxit; i++) {
        RXit *x = &c->xit[i];
        if (!used[i] || x->t3 == NULL || x->t3pre == NULL) continue;
        fprintf(o, "    case %d: goto rxb_%d;\n", i, i);
        if (x->d <= 0) fprintf(o, "    case %d: goto rpb_%d;\n", RG_MAXX + i, i);
      }
      fprintf(o, "    default: __builtin_unreachable();\n    }\n");
    }
    free(used);
  }
  fprintf(o, "#undef RG_FREE_OWNED\n");
  fprintf(o, "    }\n}\n\n");
  fprintf(o, "#undef SKFIX\n#undef RG_PM\n");
  if (c->lent != 0) fprintf(o, "#undef RG_BORP\n#undef aot_g_%s\n#undef aot_fs_%s\n", hex, hex);
  for (g = 0; g < c->ncons; g++) if (c->cons[g].stack) fprintf(o, "#undef skb_%d\n#undef sk_%d\n#undef rnh_%d\n#undef SKV_%d\n", g, g, g, g);
  rg_ty_undefs(c, o);   /* the boxed-view macros of typed values end with the function */
  if (getenv("QJS_AOT_FASTSTATS") != NULL) {
    int si;
    fprintf(o, "static void __attribute__((destructor)) rg_site_print_%s(void) {\n", hex);
    for (si = 0; si < c->nsite; si++) fprintf(o, "  if (rgw_%s[%d]) fprintf(stderr, \"region %s exit source %s: %%lu\\n\", rgw_%s[%d]);\n", hex, si, hex, c->site[si].what, hex, si);
    for (si = 0; si < c->ncons; si++) {
      const RCons *K = &c->cons[si];
      fprintf(o, "  if (rgn_%s[%d]) fprintf(stderr, \"region %s alloc cons %d inst %d pc %d kind %d class %s fields %d direct %d stack %d esc %d fwd %d: %%lu\\n\", rgn_%s[%d]);\n",
              hex, si, hex, si, K->inst, K->pc, K->kind, K->gname[0] ? K->gname : (K->npath > 0 ? K->path[K->npath - 1] : "?"), K->nn, K->direct, K->stack, K->esc, K->fwd, hex, si);
      if (K->stack && K->nmat > 0) fprintf(o, "  if (rgm_%s[%d]) fprintf(stderr, \"region %s moved cons %d inst %d pc %d class %s: %%lu\\n\", rgm_%s[%d]);\n", hex, si, hex, si, K->inst, K->pc, K->gname, hex, si);
    }
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
    cap_ = rg_has_loop(root) ? (getenv("QJS_AOT_REGION_OWNBUF") ? atoi(getenv("QJS_AOT_REGION_OWNBUF")) : 64) : (nsite_ > 0 ? nsite_ : 1);
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
    char *m3_ = blen ? memmem(buf, blen, "/*@RGT3@*/\n", 11) : NULL;
    if (m3_ != NULL) {
      size_t pre_ = (size_t)(m3_ - buf);
      RStr b3 = {0};
      char *nb_;
      if (c->t3used) {
        rs_add(&b3, "#ifndef RGX3_T\n#define RGX3_T 1\n/* an exit's frame state, described: root stores, the chain of inlined frames (tier 3) */\n"
                    "typedef struct RgX3 { uint16_t nroot, d, par_kind, dst; uint32_t call_pc; uint16_t total; const void *fc; const uint16_t *offs; } RgX3;\n#endif\n");
        rs_add(&b3, "static __attribute__((noinline, cold)) int rg_t3b_%s(JSContext *ctx, JSRegRun *f, JSValue *fr, const RgX3 *x, const JSValue *v, JSObject *const *cos)\n{\n", hex);
        rs_add(&b3, "  int i;\n  for (i = 0; i < x->nroot; i++) AOT_SET_B(ctx, fr, x->offs[i], v[i]);\n  if (x->d == 0) return 0;\n");
        if (c->nin_total > 1)
          rs_add(&b3, "  if (__builtin_expect(rg_nof_%s(), 0)) return 1;\n"
                      "  if (x->par_kind) AOT_SET_B(ctx, fr, x->dst, v[x->nroot]);\n"
                      "  *(const uint8_t **)((uint8_t *)f + QJS_AOT_OFF_REGRUN_CAP_IP) = *(const uint8_t *const *)(*(uint8_t **)((uint8_t *)f + QJS_AOT_OFF_REGRUN_B) + QJS_AOT_OFF_FNBC_REGVM_CODE) + x->call_pc;\n"
                      "  {\n    JSValue rr_ = rg_chain_%s(ctx, f, (const RgFC *)x->fc, cos, v + x->nroot + (x->par_kind != 0), x->d);\n"
                      "    if (__builtin_expect(JS_IsException(rr_), 0)) return 2;\n"
                      "    if (x->par_kind) { JS_FreeValue(ctx, rr_); AOT_SET_B(ctx, fr, x->dst, v[x->nroot]); }\n"
                      "    else if (JS_VALUE_HAS_REF_COUNT(rr_)) qjs_aot_set_reg_p(ctx, fr, x->dst, rr_);\n"
                      "    else AOT_SET_B(ctx, fr, x->dst, rr_);\n  }\n", hex, hex);
        rs_add(&b3, "  (void)cos;\n  return 0;\n}\n");
      }
      nb_ = malloc(blen - 11 + b3.n + 1);
      if (nb_ == NULL) abort();
      memcpy(nb_, buf, pre_);
      if (b3.n) memcpy(nb_ + pre_, b3.p, b3.n);
      memcpy(nb_ + pre_ + b3.n, m3_ + 11, blen - pre_ - 11);
      free(buf);
      free(b3.p);
      buf = nb_;
      blen = blen - 11 + b3.n;
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
      for (k_ = 0; k_ < c->ndx; k_++) fprintf(f, "    int rgd_%d = -1; (void)rgd_%d;\n", k_, k_);
      if (c->hoist.p) fputs(c->hoist.p, f);
      fwrite(mk_ + 9, 1, blen - pre_ - 9, f);
    } else fwrite(buf, 1, blen, f);
  }
  free(buf);
  {
    int ia;
    if (!e_nogeneric()) for (ia = 1; ia < c->nin_total; ia++) { char hy[33]; rg_hexof(c->in[ia]->g, hy); qjs_aot_region_dep(hex, hy); }
    /* QJS_AOT_REGION_CENSUS: which functions an emitted region holds (the census script reads the generator's stderr) */
    if (f != NULL && getenv("QJS_AOT_REGION_CENSUS") != NULL) {
      fprintf(stderr, "census: region %s inlines", hex);
      for (ia = 1; ia < c->nin_total; ia++) { char hy[33]; rg_hexof(c->in[ia]->g, hy); fprintf(stderr, " %s", hy); }
      fprintf(stderr, "\n");
    }
  }
  if (f != NULL && getenv("QJS_AOT_REGION_NOBORROWCHECK") == NULL && rg_borrow_check(c, root)) { fprintf(stderr, "qjsaot: a lent root stores or keeps a lent value (BORROWCHECK)\n"); e_fatal("a lent root stores or keeps a lent value", NULL); }
  if (f != NULL) rg_lent_real_set(hex, c->lent);
  if (f != NULL && rg_has_retslot(c) && g_nrs_real < 64 && !region_retslot_real(hex)) snprintf(g_rs_real[g_nrs_real++], 33, "%s", hex);
  ok = 1;
  goto done;
cleanup:
  if (g_meas_on > 0 && !c->emit) rg_measure_report(hex, root->g);
  /* free the callee IRs built for the walks */
  for (i = 1; i < c->nincap; i++) if (c->in[i] != NULL && c->in[i]->g != NULL) q2_free(&c->in[i]->q);
done:
  if (ok) for (i = 1; i < c->nincap; i++) if (c->in[i] != NULL && c->in[i]->g != NULL) q2_free(&c->in[i]->q);
  for (i = 0; i < c->nincap; i++) if (c->in[i] != NULL) { free(c->in[i]->mname); free(c->in[i]->mpc); free(c->in[i]->ty); free(c->in[i]->rw); if (i > 0) free(c->in[i]->cur); free(c->in[i]); }
  for (i = 0; i < c->nxit; i++) { free(c->xit[i].root); free(c->xit[i].rest); free(c->xit[i].t3); free(c->xit[i].t3pre); }
  free(c->cen); free(c->effcnt);
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
static int region_emit_capped(FILE *f, const Q2Func *root, const char hex[33], int fi, char *why, size_t whyn);
int region_emit(FILE *f, const Q2Func *root, const char hex[33], int fi, char *why, size_t whyn) {
  int r, k;
  {   /* bisect aid: QJS_AOT_REGION_ONLYHEX="prefix,prefix" keeps only these roots, QJS_AOT_REGION_SKIPHEX removes these */
    const char *on = getenv("QJS_AOT_REGION_ONLYHEX"), *sk = getenv("QJS_AOT_REGION_SKIPHEX");
    if (on != NULL || sk != NULL) {
      const char *q;
      int hit_on = 0, hit_sk = 0;
      for (q = on; q != NULL && *q;) { char t[40]; int n = 0; while (*q && *q != ',' && n < 38) t[n++] = *q++; t[n] = 0; if (*q == ',') q++; if (n > 0 && strncmp(hex, t, (size_t)n) == 0) hit_on = 1; }
      for (q = sk; q != NULL && *q;) { char t[40]; int n = 0; while (*q && *q != ',' && n < 38) t[n++] = *q++; t[n] = 0; if (*q == ',') q++; if (n > 0 && strncmp(hex, t, (size_t)n) == 0) hit_sk = 1; }
      if ((on != NULL && !hit_on) || hit_sk) { snprintf(why, whyn, "excluded by QJS_AOT_REGION_ONLYHEX/SKIPHEX"); return 0; }
    }
  }
  if (rg_line_cap() <= 0) return region_emit_capped(f, root, hex, fi, why, whyn);
  g_ncut = 0;
  for (k = 0; k < 8; k++) {
    g_cut_retry = 0;
    r = region_emit_capped(f, root, hex, fi, why, whyn);
    if (!g_cut_retry) break;
  }
  if (g_cut_retry && !r) {
    /* the cut set kept moving: the region without a cap */
    g_cut_retry = 0;
    g_ncut = 0;
    r = region_emit_capped(f, root, hex, fi, why, whyn);
  }
  g_ncut = 0;
  return r;
}
static int region_emit_capped(FILE *f, const Q2Func *root, const char hex[33], int fi, char *why, size_t whyn) {
  int r;
  if (getenv("QJS_AOT_REGION_OOLLOOP") == NULL && rg_trust_on()) {
    r = region_emit1(f, root, hex, fi, why, whyn, 1);
    if (r) return 1;
    if (g_cut_retry) return 0;   /* the line cap asks for another walk with its cut set */
    if (getenv("QJS_AOT_REGION_WHY")) fprintf(stderr, "loopinl: root %.8s [%s@%d] refused with loops inlined (%s); retrying out of line\n", hex, root->g->name[0] ? root->g->name : "<anon>", root->g->line, why);
  }
  return region_emit1(f, root, hex, fi, why, whyn, 0);
}
