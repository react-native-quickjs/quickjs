/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * qjsaot -- the AOT unit generator.
 *
 *   qjsaot --out aot_unit.c [--all] [--budget N] [--profile app.qjsprof] bundle.js ...
 *
 * The generator links a QJS_REGVM engine built with -DQJS_AOT_GEN. It compiles
 * each bundle exactly as the app will (same flags, so the same register
 * bytecode and therefore the same digest), enumerates every eligible function,
 * and emits a translation unit of step-only bodies plus the digest-sorted
 * binding table. Hot opcodes get inline templates (Layer A) and typed loop
 * regions (Layer B); every other instruction calls qjs_aot_step.
 *
 * `--all` emits every eligible function; `--profile`/`--budget` select a
 * subset (see the selection section below).
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs-aot-gen.h"
#include "quickjs-aot-rt.h"
#include "quickjs-regvm.h"
#include "quickjs.h"
#include "libregexp.h"
#include "regexp.h"

static char *read_file(const char *path, long *len_out) {
  FILE *f = fopen(path, "rb");
  long len;
  char *buf;
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  len = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (len < 0) {
    fclose(f);
    return NULL;
  }
  buf = malloc((size_t)len + 1);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  if (len > 0 && fread(buf, 1, (size_t)len, f) != (size_t)len) {
    free(buf);
    fclose(f);
    return NULL;
  }
  buf[len] = '\0';
  fclose(f);
  *len_out = len;
  return buf;
}

static int digest_cmp(const void *a, const void *b) {
  const JSAotGenFunc *x = a, *y = b;
  return memcmp(x->digest, y->digest, 16);
}

static void hex16(const uint8_t *d, char out[33]) {
  static const char hx[] = "0123456789abcdef";
  int i;
  for (i = 0; i < 16; i++) {
    out[i * 2] = hx[d[i] >> 4];
    out[i * 2 + 1] = hx[d[i] & 15];
  }
  out[32] = '\0';
}

/* --- refusal census ------------------------------------------------------ */

static const char *reason_name(int r) {
  switch (r) {
    case AOT_GEN_ELIGIBLE:
      return "eligible";
    case AOT_GEN_REF_LAZY:
      return "lazy";
    case AOT_GEN_REF_REGVM:
      return "regvm_refused";
    case AOT_GEN_REF_EMPTY:
      return "empty";
    case AOT_GEN_REF_TOO_BIG:
      return "too_big";
    default:
      return "?";
  }
}

static int hexval(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int hex_to_bytes16(const char *s, uint8_t out[16]) {
  int i;
  for (i = 0; i < 16; i++) {
    int hi = hexval((unsigned char)s[i * 2]);
    int lo = hexval((unsigned char)s[i * 2 + 1]);
    if (hi < 0 || lo < 0) return -1;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return 0;
}

/* Type classes the profiler records per register (mirrors quickjs.c's
   AOT_TYPE_*). A register is a numeric-region candidate only when every bit it
   ever carried is inside the arithmetic set; the emitted code still guards. */
#define PT_FLOAT 0x01u
#define PT_INT 0x02u
#define PT_OBJECT 0x04u
#define PT_STRING 0x08u
#define PT_BOOL 0x10u
#define PT_UNDEF 0x20u
#define PT_BIGINT 0x40u
#define PT_OTHER 0x80u
#define PT_ARITH (PT_FLOAT | PT_INT)

#include "emit.h"
#include "fast.h"
#include "prof.h"
#include "select.h"
int qjs_aot_scan_mode(void);
int qjs_aot_recscan_mode(void);
int qjs_aot_rat_mode(void);

typedef struct {
  int pc;
  long long iters;
} ProfLoop;
typedef struct {
  int reg;
  unsigned types;
} ProfReg;
typedef struct {
  int pc;
  unsigned long long hits; /* the dominant callee's hit count */
  int kind;                /* 1 bytecode, 2 C, 3 other */
  int cproto, magic;       /* kind 2 */
  int argc_min, argc_max;
  char digest[33]; /* kind 1: the callee digest hex */
} ProfCall;

/* Every callee a call site saw, for polymorphic dispatch. The dominant callee
   above is kept as it was so existing callers do not change. */
/* A compiled function's body above this many bytes gets no specialised copy:
   the code is a cost of its own (I-cache, and a second body doubles it), and
   SVC, the largest function box2d has, is 423 KB. */
#define QJS_AOT_BODY_BUDGET (1024u * 1024u)

#define PROF_MAX_CALLEES 4
typedef struct {
  int kind;
  int cproto;
  char digest[33];
  unsigned long long hits;
} ProfCallee;
typedef struct {
  int pc;
  int n;
  ProfCallee c[PROF_MAX_CALLEES];
} ProfCallSite;

/* A `P` record: the type set of every register the instruction at `pc` writes,
   the same one record shared by the two destinations of a fused op. Its shape
   lives in qjsaot/prof.h so the IR reads the same struct. */

/* An `S` record: the shapes one property site's inline cache saw. Shape values
   are addresses from the profiling process; they only say "the same shape as
   another site in this profile", never a runtime address. */
static int prof_a_adds_into_own(const char *rec, int nown);
#define PROF_MAX_SHAPES 4
typedef struct {
  int site;
  int nshapes;
  uint64_t shape[PROF_MAX_SHAPES];
  /* The slot each shape put this site's atom in, or -1 when the profile did not
     say. Two shapes with the same slot share the field's offset, which is what
     lets a contracted site read one constant slot; different slots need a switch
     on the layout. */
  int slot[PROF_MAX_SHAPES];
  /* Which cache kinds the site's fills installed, one bit per REG_PIC_* value
     (only meaningful when the header said `sitekinds=1`). */
  unsigned kinds;
  /* The regvm offset of the op that first filled the site, or -1. */
  int pc;
  /* the fills the engine counted at this site, and the hits of each recorded shape: count > sum(hits) means fills were NOT recorded (a fifth shape, or an unhashed one),
     so the site's shape set is SATURATED and says nothing about the shapes it did not keep */
  unsigned count;
  unsigned hits[PROF_MAX_SHAPES];
  /* The engine's `A` record of a store site: how often the store overwrote an own slot, added a property, or took neither path (a setter, a
     proxy, an exotic object, a dead cache), counted at every execution and not only at cache fills. 0 when the profile has none. */
  int ahas;
  unsigned long long aown, aadd, aoth;
  /* The engine's `R` record: the executions through a read site and how many of them did not see its commonest receiver shape (0/0 without a record). */
  unsigned long long rtotal;
  int nr;
  uint64_t rshape[8];
  unsigned long long rcount[8];
} ProfSite;

/* A `W` record: the shape of the objects the instruction at `pc` defined, as a
   shape table index, or -1 when it never defined an object; `multi` when it
   defined objects of more than one shape. */
typedef struct {
  int pc;
  int ord; /* which register of the op, in the order it first wrote them */
  int shape;
  int multi;
  unsigned types; /* the PT_* set of the register, 0 when the record carries none */
} ProfRes;

/* A `Q` record: the shape table. One entry per distinct shape any field site
   observed, with the prototype it belongs to and its ordered property list.
   `proto` is 1 for fn:<digest> (the constructor the unit binds by), 2 for
   Object.prototype, 3 for Array.prototype, 4 for a null prototype, and 0 for
   anything else, which cannot be contracted. */
#define PROF_MAX_FIELDS 32
typedef struct {
  int id;
  int proto;              /* see above */
  char digest[33];        /* proto == 1 */
  int isproto;            /* some object of this shape is a prototype (patch 0435): never a layout */
  unsigned long long pk;  /* the prototype's stable key (patch 0449): equal across processes for the same prototype by construction; 0 = none recorded */
  int via_scan;           /* the constructor was found by scanning for the function whose prototype this is (patch 0433): a class with no own `constructor` */
  int nfields;
  int nprops;             /* the shape's property count, deleted slots included */
  char *field[PROF_MAX_FIELDS];  /* the atom, by name */
  unsigned flags[PROF_MAX_FIELDS];
} ProfShape;

/* The shape table: the `Q` records describe shapes, which belong to no one
   function, so they are collected once for the whole profile rather than per
   function entry. */
static ProfShape *g_shapes;
static int g_nshapes, g_capshapes;

/* A `V` record: what the live objects of a shape held in one slot when the profile was written
   (the PT_* set, the table indexes of up to four shapes of the object values, and whether
   there were others). */
#define PROF_CONT_SHAPES 4
typedef struct {
  int shape, slot;
  unsigned types;
  int nvs;
  int vs[PROF_CONT_SHAPES];
  int vm;
} ProfCont;
static ProfCont *g_cont;
static int g_ncont, g_capcont;

/* A `U` record: a pc range the profiler never saw executed, inclusive. */
typedef struct {
  int lo, hi;
} ProfUnused;

typedef struct {
  uint8_t digest[16];
  unsigned long long ops;
  unsigned long long entries; /* calls of the function in the profile run (exact) */
  ProfLoop *loops;
  int nloops, caploops;
  ProfReg *regs;
  int nregs, capregs;
  ProfCall *calls;
  int ncalls, capcalls;
  ProfCallSite *callsites;
  int ncallsites, capcallsites;
  ProfPc *pcs;
  int npcs, cappcs;
  /* The `K` records: what each call site returned, keyed by the pc just past
     the call. Only complete records survive loading (see parse_profile). */
  ProfPc *cres;
  int ncres, capcres;
  ProfSite *sites;
  int nsites, capsites;
  ProfRes *res;
  int nres, capres;
  ProfUnused *unused;
  int nunused, capunused;
  /* The `T` records: sampler hits by pc, kept as the per-instruction execution
     count the op-call weight sums over. Sorted by pc after parsing. */
  ProfCount *counts;
  int ncounts, capcounts;
  /* The `N` records: runs of instructions (start pcs lo..hi) that each executed `n` times. Summed over the profiles merged. */
  struct ProfExecRun { int lo, hi; unsigned long long n; } *exec;
  int nexec, capexec;
  /* How many `F` records the profile holds for this digest: more than one means two live bytecodes of the same code (a twin), of which the
     engine binds one to the compiled entry. */
  int nrec;
  /* The `A` records of store sites that have no `S` record and no overwrite shapes to give: the executions by kind, read by the store
     predicates only (a site with an `S` record carries them in its ProfSite). */
  struct ProfAOnly { int site; unsigned long long own, add, oth; } *aonly;
  int naonly, capaonly;
} ProfEntry;

/* The parsed profile (for call-site specialisation) and the current function's
   entry, set by emit_func; `g_site_next` hands out one AOT site id per emitted
   call site (the engine's per-runtime binding array is indexed by it). */
static const ProfEntry *g_prof = NULL;

/* How many contracted layouts the generator decided on. Set once, before the
   unit's helpers are emitted, so the entry check and the layout table agree. */
static int g_nlayouts;
/* Defined in ops.c: how many call sites hold a callee in the unit's table. */
extern int g_cc_count;
static int g_nprof = 0;

/* The profile's regexp section (`X <matcher cycles> <attempts on 16-bit subjects> <hex bytecode>`): the compiled bytecode of every pattern that ran, with the cycles its matcher took. */
typedef struct { unsigned long long cyc, wide; uint8_t *bc; size_t len; } ProfRx;
static ProfRx *g_prx;
static int g_nprx, g_cprx;
/* Whether the profile's header said `thr=1`, i.e. it came from the threaded
   build whose `R` records are complete. See parse_profile. */
static int g_prof_thr = 0;
/* Whether the header said `callres=1`: the profile records call results on
   every instrumented return path. Without it no `K` record is believed. */
static int g_prof_callres = 0;
/* Whether the header said `sitekinds=1`: the `S` records carry the cache kinds,
   so a site can be told apart as an own slot, a prototype read or an add. */
static int g_prof_sitekinds = 0;
/* The `G` records: the stores each global binding saw in the profiled run (count and the last value). `gwrites=1` in the header says the
   profile carries them, so a global with no record was never stored to; a profile without the flag says nothing. */
typedef struct ProfGlobal {
  char *name;
  unsigned writes;
} ProfGlobal;
static ProfGlobal *g_pglob;
static int g_npglob, g_cappglob;
static int g_prof_gwrites = 0;

/* How many stores the profiled run made to the global `name`; -1 when the profile does not say. */
int qjs_aot_prof_global_writes(const char *name) {
  int i;
  if (!g_prof_gwrites) return -1;
  for (i = 0; i < g_npglob; i++)
    if (strcmp(g_pglob[i].name, name) == 0) return (int)(g_pglob[i].writes > 0x7fffffffu ? 0x7fffffff : g_pglob[i].writes);
  return 0;
}
/* Call-binding site ids (the engine's per-runtime binding array is indexed by
   one). A deopt site has its own id space, handed out by `g_deopt_site_next`,
   because a deopt site needs a failure counter and a function to disable while
   a binding site needs neither. */
static int g_site_next = 0;
static int g_deopt_site_next = 0;
/* Deopt site -> index of its function in the emitted table, so the engine can
   disable a function whose guards keep failing. Grown with the site count. */
static int *g_deopt_site_func = NULL;
static int g_deopt_site_func_cap = 0;
/* The index of the function being emitted. */
static int g_cur_func_idx = 0;
/* The unit's functions and selection, so the emitter can tell whether a
   profiled callee is emitted in this same translation unit. */
static const JSAotGenFunc *g_unit_funcs = NULL;
static int g_unit_nfuncs = 0;
/* region roots and the functions they expanded (region.c reports each when its region compiles) */
static struct { uint8_t root[16], callee[16]; } *g_rgdep;
static int g_rgdep_n, g_rgdep_cap;
void qjs_aot_region_dep(const char root_hex[33], const char callee_hex[33]);
/* Deopt-everywhere: every operation the unit does not lower leaves compiled
   code at its own instruction. Sweeping every entry exercises every frame state
   against the interpreter. */
static int g_deopt_everywhere = 0;
/* Debug: before every leave, assert each live value's boxed form equals its
   frame slot, so a missing write-back or a stale local is caught at the site
   rather than as a wrong result. */
static int g_check_locals = 0;

static long long count_insns(const uint8_t *code, int len);

/* --dump-ir: print the register program of every unique eligible function,
   one line per instruction, so a change to the lowering or the generator's
   reading of it shows up as a golden-file diff. The new generator replaces
   this with its SSA dump. */
static int g_dump_ir = 0;
static const char *g_ir_path = NULL;

/* --report FILE writes one line per function, and a summary. */
static const char *g_report_path = NULL;
static const char *g_facts_path = NULL;

static void dump_ir_func(FILE *f, const JSAotGenFunc *g) {
  char hex[33];
  int off = 0, n = 0;
  JSAotGenInsn in;

  hex16(g->digest, hex);
  fprintf(
      f, "F %s name=%s insns=%lld\n", hex, g->name[0] ? g->name : "<anon>",
      count_insns(g->code, g->len));
  while (off < g->len &&
         js_aot_gen_decode_insn(g->code, g->len, off, &in) == 0) {
    int k;
    fprintf(f, "  %4d pc=%-4d op=%d sz=%d", n, off, in.op, in.size);
    for (k = 0; k < in.nargs; k++)
      fprintf(f, " %c%d", in.arg[k].kind, (int)in.arg[k].val);
    fputc('\n', f);
    off += in.size;
    n++;
  }
}

static void dump_ir(const JSAotGenFunc *funcs, int nfuncs) {
  FILE *f = stdout;
  int i;

  if (g_ir_path) {
    f = fopen(g_ir_path, "w");
    if (!f) {
      fprintf(stderr, "qjsaot: cannot write %s\n", g_ir_path);
      return;
    }
  }
  for (i = 0; i < nfuncs; i++) {
    if (funcs[i].eligible != AOT_GEN_ELIGIBLE) continue;
    if (i + 1 < nfuncs && memcmp(funcs[i].digest, funcs[i + 1].digest, 16) == 0)
      continue;
    dump_ir_func(f, &funcs[i]);
  }
  if (g_ir_path) fclose(f);
}

/* Record that deopt site `sid` belongs to the function being emitted. */
static void deopt_site_note(int sid) {
  if (sid >= g_deopt_site_func_cap) {
    int nc = g_deopt_site_func_cap ? g_deopt_site_func_cap : 64;
    int *np;
    while (nc <= sid) nc *= 2;
    np = realloc(g_deopt_site_func, sizeof(*np) * (size_t)nc);
    if (!np) return;
    g_deopt_site_func = np;
    g_deopt_site_func_cap = nc;
  }
  g_deopt_site_func[sid] = g_cur_func_idx;
}

static unsigned prof_type_bit(const char *name) {
  if (!strcmp(name, "float")) return PT_FLOAT;
  if (!strcmp(name, "int")) return PT_INT;
  if (!strcmp(name, "object")) return PT_OBJECT;
  if (!strcmp(name, "string")) return PT_STRING;
  if (!strcmp(name, "bool")) return PT_BOOL;
  if (!strcmp(name, "undef")) return PT_UNDEF;
  if (!strcmp(name, "bigint")) return PT_BIGINT;
  if (!strcmp(name, "other")) return PT_OTHER;
  return PT_OTHER;
}

/* `<min>..<max>` as the profile writes it. A scanf of the two numbers reads `2.` and stops at `..2`
   when the bounds are integers, so the separator is found first. */
static void prof_parse_range(const char *p, double *lo, double *hi) {
  const char *dd = strstr(p, "..");
  *lo = strtod(p, NULL);
  if (dd != NULL) *hi = strtod(dd + 2, NULL);
}

static unsigned prof_parse_types(const char *s) {
  unsigned t = 0;
  char buf[64];
  int j = 0;
  /* The token runs to whitespace or end of line; anything past it (`num=`,
     `src=`) is a different field and must not be read as a type name. */
  for (;; s++) {
    if (*s == '|' || *s == '\0' || *s == '\n' || *s == ' ' || *s == '\t') {
      if (j) {
        buf[j] = '\0';
        t |= prof_type_bit(buf);
        j = 0;
      }
      if (*s == '\0' || *s == '\n' || *s == ' ' || *s == '\t') break;
    } else if (j < (int)sizeof(buf) - 1) {
      buf[j++] = *s;
    }
  }
  return t;
}

/* Parse-time index of the per-function pc-keyed records, so loading a profile
   of a function with very many sites stays linear. Keyed by (function index,
   record kind, pc); the value is the record's position in its array. */
typedef struct { int ent, kind, pc, idx; } PidxSlot;
static PidxSlot *g_pidx;
static size_t g_pidx_cap, g_pidx_n;
static size_t pidx_hash(int ent, int kind, int pc) {
  uint64_t h = ((uint64_t)(uint32_t)ent << 35) ^ ((uint64_t)(uint32_t)kind << 32) ^ (uint32_t)pc;
  h *= 0x9E3779B97F4A7C15ull;
  return (size_t)(h >> 20);
}
static int pidx_find(int ent, int kind, int pc) {
  size_t m, i;
  if (!g_pidx_cap) return -1;
  m = g_pidx_cap - 1;
  for (i = pidx_hash(ent, kind, pc) & m; g_pidx[i].idx >= 0; i = (i + 1) & m)
    if (g_pidx[i].ent == ent && g_pidx[i].kind == kind && g_pidx[i].pc == pc) return g_pidx[i].idx;
  return -1;
}
static int pidx_put(int ent, int kind, int pc, int idx) {
  size_t m, i;
  if ((g_pidx_n + 1) * 2 > g_pidx_cap) {
    size_t nc = g_pidx_cap ? g_pidx_cap * 2 : 4096, j;
    PidxSlot *nt = malloc(sizeof(*nt) * nc), *old = g_pidx;
    if (!nt) return -1;
    for (j = 0; j < nc; j++) nt[j].idx = -1;
    for (j = 0; j < g_pidx_cap; j++)
      if (old[j].idx >= 0) {
        size_t k = pidx_hash(old[j].ent, old[j].kind, old[j].pc) & (nc - 1);
        while (nt[k].idx >= 0) k = (k + 1) & (nc - 1);
        nt[k] = old[j];
      }
    free(old);
    g_pidx = nt;
    g_pidx_cap = nc;
  }
  m = g_pidx_cap - 1;
  for (i = pidx_hash(ent, kind, pc) & m; g_pidx[i].idx >= 0; i = (i + 1) & m) {}
  g_pidx[i].ent = ent; g_pidx[i].kind = kind; g_pidx[i].pc = pc; g_pidx[i].idx = idx;
  g_pidx_n++;
  return 0;
}
static void pidx_free(void) { free(g_pidx); g_pidx = NULL; g_pidx_cap = g_pidx_n = 0; }

static ProfEntry *prof_get(
    ProfEntry **parr, int *pn, int *pcap, const uint8_t dg[16]) {
  ProfEntry *arr = *parr;
  int i;
  for (i = 0; i < *pn; i++)
    if (memcmp(arr[i].digest, dg, 16) == 0) return &arr[i];
  if (*pn == *pcap) {
    ProfEntry *na;
    *pcap = *pcap ? *pcap * 2 : 64;
    na = realloc(arr, sizeof(*arr) * (size_t)*pcap);
    if (!na) {
      free(arr);
      *parr = NULL;
      return NULL;
    }
    arr = na;
    *parr = arr;
  }
  memset(&arr[*pn], 0, sizeof(arr[*pn]));
  memcpy(arr[*pn].digest, dg, 16);
  (*pn)++;
  return &arr[*pn - 1];
}

/* Parse a .qjsprof file: `F` (function + ops), `L` (loop pc + trip count) and
   `R` (register type set) lines, summed/merged per digest. The header carries
   the engine identity; a profile from another engine or build configuration is
   refused rather than silently matching nothing. Returns a malloc'd array (may
   be empty) or NULL. */
static int cmp_profpc(const void *a, const void *b) {
  const ProfPc *x = a, *y = b;
  return (x->pc > y->pc) - (x->pc < y->pc);
}

static int cmp_profcount(const void *a, const void *b) {
  const ProfCount *x = a, *y = b;
  return (x->pc > y->pc) - (x->pc < y->pc);
}

static ProfEntry *parse_profile(const char *path, int *pn) {
  FILE *f = fopen(path, "r");
  ProfEntry *arr = NULL;
  int n = 0, cap = 0, i;
  char line[8192];
  *pn = 0;
  if (!f) return NULL;
  if (fgets(line, sizeof(line), f)) {
    if (strncmp(line, "qjsprof ", 8) == 0) {
      const char *p = strstr(line, "abi=");
      const char *fp = strstr(line, "fmt=");
      if (p && strtoul(p + 4, NULL, 10) != (unsigned long)QJS_AOT_ABI) {
        fprintf(
            stderr,
            "qjsaot: %s: ABI mismatch (profile %lu, engine %lu); "
            "collect the profile with this engine build\n",
            path, strtoul(p + 4, NULL, 10), (unsigned long)QJS_AOT_ABI);
        fclose(f);
        return NULL;
      }
      if (fp &&
          strtoul(fp + 4, NULL, 10) != (unsigned long)REGVM_FORMAT_VERSION) {
        fprintf(
            stderr,
            "qjsaot: %s: register-format mismatch (profile %lu, "
            "engine %u); collect the profile with this engine build\n",
            path, strtoul(fp + 4, NULL, 10), (unsigned)REGVM_FORMAT_VERSION);
        fclose(f);
        return NULL;
      }
      /* `thr=1` means the profile came from the threaded (feature-table)
         build, the only one whose `R` records are complete: they are written
         by the threaded `regvm_t_get_arg2`/`SET_REG_B` handlers. A monolith
         build writes no `R` record for a value, so a record it does emit would
         describe only the *other* values a reused register held and would type
         a result from the wrong history. So the records are used only when the
         header says `thr=1`; otherwise every `R` line is ignored. */
      {
        const char *tp = strstr(line, "thr=");
        g_prof_thr = tp && strtoul(tp + 4, NULL, 10) != 0;
      }
      {
        const char *cp = strstr(line, "callres=");
        g_prof_callres = cp && strtoul(cp + 8, NULL, 10) != 0;
      }
      {
        const char *kp = strstr(line, "sitekinds=");
        g_prof_sitekinds = kp && strtoul(kp + 10, NULL, 10) != 0;
      }
      {
        const char *gp = strstr(line, "gwrites=");
        g_prof_gwrites = gp && strtoul(gp + 8, NULL, 10) != 0;
      }
    }
  }
  while (fgets(line, sizeof(line), f)) {
    uint8_t dg[16];
    ProfEntry *e;
    /* The `Q` records are the shape table: they name a shape by a decimal table
       index, not by a function digest, so they are read before the
       per-function filter below -- which needs a hex digest and would drop
       them. The table belongs to no one function, so it is collected globally. */
    if (line[0] == 'G' && line[1] == ' ') {
      /* G <name> writes=<n> types=.. val=..: summed over the profiles merged. */
      const char *wp = strstr(line + 2, " writes=");
      if (wp != NULL && wp > line + 2) {
        size_t nl = (size_t)(wp - (line + 2));
        unsigned w = (unsigned)strtoul(wp + 8, NULL, 10);
        int k;
        for (k = 0; k < g_npglob; k++)
          if (strlen(g_pglob[k].name) == nl && strncmp(g_pglob[k].name, line + 2, nl) == 0) break;
        if (k == g_npglob) {
          if (g_npglob == g_cappglob) {
            ProfGlobal *ng;
            g_cappglob = g_cappglob ? g_cappglob * 2 : 64;
            ng = realloc(g_pglob, sizeof(*ng) * (size_t)g_cappglob);
            if (!ng) return NULL;
            g_pglob = ng;
          }
          g_pglob[k].name = malloc(nl + 1);
          if (!g_pglob[k].name) return NULL;
          memcpy(g_pglob[k].name, line + 2, nl);
          g_pglob[k].name[nl] = '\0';
          g_pglob[k].writes = 0;
          g_npglob++;
        }
        g_pglob[k].writes += w;
      }
      continue;
    }
    if (line[0] == 'V' && line[1] == ' ') {
      /* V <shape> <slot> types=<n> vs=<a,b,..|-1> vm=<0|1> */
      ProfCont c;
      const char *ty = strstr(line + 2, "types="), *vs = strstr(line + 2, "vs="),
                 *vm = strstr(line + 2, "vm=");
      int k;
      memset(&c, 0, sizeof(c));
      if (sscanf(line + 2, "%d %d", &c.shape, &c.slot) == 2 && ty && vs && vm) {
        c.types = (unsigned)strtoul(ty + 6, NULL, 10);
        c.vm = (int)strtol(vm + 3, NULL, 10) != 0;
        for (vs += 3; c.nvs < PROF_CONT_SHAPES;) {
          char *end;
          long v = strtol(vs, &end, 10);
          if (end == vs) break;
          if (v >= 0) c.vs[c.nvs++] = (int)v;
          if (*end != ',') break;
          vs = end + 1;
        }
        for (k = 0; k < g_ncont; k++)
          if (g_cont[k].shape == c.shape && g_cont[k].slot == c.slot) break;
        if (k == g_ncont) {
          if (g_ncont == g_capcont) {
            ProfCont *nc;
            g_capcont = g_capcont ? g_capcont * 2 : 64;
            nc = realloc(g_cont, sizeof(*nc) * (size_t)g_capcont);
            if (!nc) return NULL;
            g_cont = nc;
          }
          g_cont[g_ncont++] = c;
        }
      }
      continue;
    }
    if (line[0] == 'Q' && line[1] == ' ') {
      char *pp = strstr(line + 2, "proto=");
      if (pp != NULL) {
        ProfShape *sh;
        int id = (int)strtol(line + 2, NULL, 10);
        const char *pr = pp + 6;
        int k;
        if (g_nshapes == g_capshapes) {
          ProfShape *ns;
          g_capshapes = g_capshapes ? g_capshapes * 2 : 64;
          ns = realloc(g_shapes, sizeof(*ns) * (size_t)g_capshapes);
          if (!ns) return NULL;
          g_shapes = ns;
        }
        sh = &g_shapes[g_nshapes++];
        memset(sh, 0, sizeof(*sh));
        sh->id = id;
        {
          const char *kp = strstr(line + 2, " pk=");
          const char *pp3 = strstr(line + 2, " props=");
          if (kp != NULL && (pp3 == NULL || kp < pp3)) sh->pk = strtoull(kp + 4, NULL, 16);
        }
        {
          const char *np = strstr(line + 2, " n=");
          sh->nprops = np != NULL ? (int)strtol(np + 3, NULL, 10) : -1;
        }
        if (strncmp(pr, "fn:", 3) == 0) {
          sh->proto = 1;
          for (k = 0; k < 32 && pr[3 + k] != '\0' && pr[3 + k] != ' '; k++)
            sh->digest[k] = pr[3 + k];
          sh->digest[k] = '\0';
          sh->via_scan = strncmp(pr + 3 + k, " via=scan", 9) == 0;
        } else if (strncmp(pr, "object", 6) == 0) sh->proto = 2;
        else if (strncmp(pr, "array", 5) == 0) sh->proto = 3;
        else if (strncmp(pr, "null", 4) == 0) sh->proto = 4;
        else sh->proto = 0;
        {
          const char *ip = strstr(line + 2, " isproto=1");
          const char *pp2 = strstr(line + 2, " props=");
          sh->isproto = ip != NULL && (pp2 == NULL || ip < pp2);
        }
        {
          char *pf = strstr(line + 2, " props=");
          if (pf != NULL) {
            char *q = pf + 7;
            while (*q != '\0' && *q != '\n' && sh->nfields < PROF_MAX_FIELDS) {
              char *colon = strchr(q, ':');
              char *comma;
              if (colon == NULL) break;
              comma = strchr(colon, ',');
              *colon = '\0';
              sh->field[sh->nfields] = strdup(q);
              sh->flags[sh->nfields] = (unsigned)strtoul(colon + 1, NULL, 10);
              sh->nfields++;
              if (comma == NULL) break;
              q = comma + 1;
            }
          }
        }
      }
      continue;
    }
    if (line[0] == 'X' && line[1] == ' ') {
      char *q;
      unsigned long long cyc = strtoull(line + 2, &q, 10), wide = strtoull(q, &q, 10);
      size_t hl = 0, k;
      uint8_t *bc;
      while (*q == ' ') q++;
      while (isxdigit((unsigned char)q[hl])) hl++;
      if (hl < 16 || (hl & 1)) continue;
      bc = malloc(hl / 2);
      if (!bc) continue;
      for (k = 0; k < hl / 2; k++) { unsigned v; sscanf(q + 2 * k, "%2x", &v); bc[k] = (uint8_t)v; }
      if (g_nprx == g_cprx) {
        g_cprx = g_cprx ? g_cprx * 2 : 64;
        g_prx = realloc(g_prx, sizeof(ProfRx) * (size_t)g_cprx);
      }
      g_prx[g_nprx].cyc = cyc; g_prx[g_nprx].wide = wide; g_prx[g_nprx].bc = bc; g_prx[g_nprx].len = hl / 2; g_nprx++;
      continue;
    }
    if ((line[0] != 'F' && line[0] != 'L' && line[0] != 'R' && line[0] != 'C' &&
         line[0] != 'P' && line[0] != 'S' && line[0] != 'U' && line[0] != 'T' &&
         line[0] != 'K' && line[0] != 'W' && line[0] != 'N' && line[0] != 'A') ||
        line[1] != ' ')
      continue;
    if (hex_to_bytes16(line + 2, dg) < 0) continue;
    e = prof_get(&arr, &n, &cap, dg);
    if (!e) return NULL;
    if (line[0] == 'F') {
      char *p = strstr(line + 2, "ops=");
      char *pe = strstr(line + 2, "entries=");
      if (p) e->ops += strtoull(p + 4, NULL, 10);
      if (pe) e->entries += strtoull(pe + 8, NULL, 10);
      e->nrec++;
    } else if (line[0] == 'L') {
      char *p = strstr(line + 2, "pc=");
      if (p) {
        long long it = 0;
        char *q = strstr(p, "iters=");
        if (q) it = strtoll(q + 6, NULL, 10);
        if (e->nloops == e->caploops) {
          ProfLoop *nl;
          e->caploops = e->caploops ? e->caploops * 2 : 8;
          nl = realloc(e->loops, sizeof(*nl) * (size_t)e->caploops);
          if (!nl) return NULL;
          e->loops = nl;
        }
        e->loops[e->nloops].pc = (int)strtol(p + 3, NULL, 10);
        e->loops[e->nloops].iters = it;
        e->nloops++;
      }
    } else if (line[0] == 'C') {
      /* C <hex> pc= count= argc=min..max callee=... hits= ; keep the dominant
         callee per pc (a site is usually monomorphic). */
      char *p = strstr(line + 2, "pc=");
      char *h = strstr(line + 2, "hits=");
      char *a = strstr(line + 2, "argc=");
      char *cl = strstr(line + 2, "callee=");
      if (p && h && cl) {
        int pc = (int)strtol(p + 3, NULL, 10);
        unsigned long long hits = strtoull(h + 5, NULL, 10);
        int k, amin = 0, amax = 0, kind = 3, cproto = -1, magic = 0;
        char cd[33] = "";
        if (a) sscanf(a + 5, "%d..%d", &amin, &amax);
        if (!strncmp(cl + 7, "bc:", 3)) {
          kind = 1;
          if (hex_to_bytes16(cl + 10, dg) == 0) {
            int z;
            for (z = 0; z < 16; z++) sprintf(cd + z * 2, "%02x", dg[z]);
            cd[32] = 0;
          }
        } else if (!strncmp(cl + 7, "thru:", 5)) {
          /* `X.call(...)`: X is the callee of kind 4 (see the profile writer) */
          kind = 4;
          if (hex_to_bytes16(cl + 12, dg) == 0) {
            int z;
            for (z = 0; z < 16; z++) sprintf(cd + z * 2, "%02x", dg[z]);
            cd[32] = 0;
          }
        } else if (!strncmp(cl + 7, "c:", 2)) {
          char *mg = strstr(cl, ":magic=");
          char *cp = strstr(cl, ":cproto=");
          kind = 2;
          if (mg) magic = (int)strtol(mg + 7, NULL, 10);
          if (cp) cproto = (int)strtol(cp + 8, NULL, 10);
          /* Name a C callee by its symbol and magic ("js_math_floor:0"): the
             pointer differs from run to run, the symbol does not. It is held in
             the digest slot, which a C callee has no other use for, so two
             different builtins at one site stay two callees. */
          {
            const char *e = strstr(cl + 9, ":fn=");
            size_t n = e ? (size_t)(e - (cl + 9)) : 0;
            if (n > 0 && n < 24) {
              char nm[32];
              memcpy(nm, cl + 9, n);
              nm[n] = 0;
              snprintf(cd, sizeof(cd), "%s:%d", nm, magic);
            }
          }
        }
        k = pidx_find((int)(e - arr), 1, pc);
        if (k < 0) k = e->ncalls;
        if (k == e->ncalls) {
          if (pidx_put((int)(e - arr), 1, pc, k) < 0) return NULL;
          if (e->ncalls == e->capcalls) {
            ProfCall *nc;
            e->capcalls = e->capcalls ? e->capcalls * 2 : 16;
            nc = realloc(e->calls, sizeof(*nc) * (size_t)e->capcalls);
            if (!nc) return NULL;
            e->calls = nc;
          }
          e->ncalls++;
          memset(&e->calls[k], 0, sizeof(e->calls[k]));
          e->calls[k].pc = pc;
        }
        if (hits >= e->calls[k].hits && kind != 4) {
          e->calls[k].hits = hits;
          e->calls[k].kind = kind;
          e->calls[k].cproto = cproto;
          e->calls[k].magic = magic;
          e->calls[k].argc_min = amin;
          e->calls[k].argc_max = amax;
          snprintf(e->calls[k].digest, sizeof(e->calls[k].digest), "%s", cd);
        }
        /* And the whole callee set of this site, for polymorphic dispatch. */
        {
          ProfCallSite *cs = NULL;
          int j;
          j = pidx_find((int)(e - arr), 2, pc);
          if (j >= 0) cs = &e->callsites[j];
          if (!cs) {
            if (pidx_put((int)(e - arr), 2, pc, e->ncallsites) < 0) return NULL;
            if (e->ncallsites == e->capcallsites) {
              ProfCallSite *ncs;
              e->capcallsites = e->capcallsites ? e->capcallsites * 2 : 16;
              ncs =
                  realloc(e->callsites, sizeof(*ncs) * (size_t)e->capcallsites);
              if (!ncs) return NULL;
              e->callsites = ncs;
            }
            cs = &e->callsites[e->ncallsites++];
            memset(cs, 0, sizeof(*cs));
            cs->pc = pc;
          }
          /* The same callee can appear on more than one line (one per proto
             or per argc range): keep the highest hit count. */
          for (j = 0; j < cs->n; j++) {
            if (cs->c[j].kind != kind) continue;
            if (strcmp(cs->c[j].digest, cd) != 0) continue;
            if (hits > cs->c[j].hits) cs->c[j].hits = hits;
            break;
          }
          if (j == cs->n && cs->n < PROF_MAX_CALLEES) {
            cs->c[cs->n].kind = kind;
            cs->c[cs->n].cproto = cproto;
            snprintf(
                cs->c[cs->n].digest, sizeof(cs->c[cs->n].digest), "%s", cd);
            cs->c[cs->n].hits = hits;
            cs->n++;
          } else if (j == cs->n) {
            /* Full: replace the least-hit entry when this one is hotter. */
            int lo = 0;
            for (j = 1; j < cs->n; j++)
              if (cs->c[j].hits < cs->c[lo].hits) lo = j;
            if (hits > cs->c[lo].hits) {
              cs->c[lo].kind = kind;
              cs->c[lo].cproto = cproto;
              snprintf(cs->c[lo].digest, sizeof(cs->c[lo].digest), "%s", cd);
              cs->c[lo].hits = hits;
            }
          }
        }
      }
    } else if (line[0] == 'P') {
      /* P <hex> pc=<n> types=<a|b> [num=<min>..<max> frac=<0|1>
         negzero=<0|1>] [elem=..|props=..] src=..; merge by pc. */
      char *p = strstr(line + 2, "pc=");
      char *q = strstr(line + 2, "types=");
      if (p && q) {
        int pc = (int)strtol(p + 3, NULL, 10);
        unsigned t = prof_parse_types(q + 6);
        const char *nm = strstr(line + 2, "num=");
        const char *fm = strstr(line + 2, "frac=");
        const char *nz = strstr(line + 2, "negzero=");
        const char *el = strstr(line + 2, "elem=");
        const char *tc = strstr(line + 2, "tacls=");
        const char *pk = strstr(line + 2, " pk=");
        int k;
        double nmin = 0, nmax = 0;
        int has_num = 0, frac = 0, negzero = 0, elem = 0, tacls = 0, pkinds = 0;
        if (nm) {
          has_num = 1;
          prof_parse_range(nm + 4, &nmin, &nmax);
        }
        if (fm) frac = (int)strtol(fm + 5, NULL, 10) != 0;
        if (nz) negzero = (int)strtol(nz + 8, NULL, 10) != 0;
        if (el) elem = (uint8_t)strtoul(el + 5, NULL, 10);
        if (tc) tacls = (int)strtoul(tc + 6, NULL, 10);
        if (pk) pkinds = (int)strtoul(pk + 4, NULL, 10);
        k = pidx_find((int)(e - arr), 3, pc);
        if (k < 0) k = e->npcs;
        if (k == e->npcs) {
          if (pidx_put((int)(e - arr), 3, pc, k) < 0) return NULL;
          if (e->npcs == e->cappcs) {
            ProfPc *np;
            e->cappcs = e->cappcs ? e->cappcs * 2 : 16;
            np = realloc(e->pcs, sizeof(*np) * (size_t)e->cappcs);
            if (!np) return NULL;
            e->pcs = np;
          }
          e->npcs++;
          memset(&e->pcs[k], 0, sizeof(e->pcs[k]));
          e->pcs[k].pc = pc;
        }
        e->pcs[k].types |= t;
        e->pcs[k].frac |= (uint8_t)frac;
        e->pcs[k].negzero |= (uint8_t)negzero;
        e->pcs[k].elem |= (uint8_t)elem;
        e->pcs[k].ta |= (uint16_t)tacls;
        e->pcs[k].pk |= (uint8_t)pkinds;
        if (has_num) {
          if (!e->pcs[k].has_num) {
            e->pcs[k].num_min = nmin;
            e->pcs[k].num_max = nmax;
          } else {
            if (nmin < e->pcs[k].num_min) e->pcs[k].num_min = nmin;
            if (nmax > e->pcs[k].num_max) e->pcs[k].num_max = nmax;
          }
          e->pcs[k].has_num = 1;
        }
      }
    } else if (line[0] == 'K') {
      /* K <hex> pc=<n> types=<a|b> [num=<min>..<max> frac=.. negzero=..]
         calls=<n> results=<n>; merge by pc. */
      char *p = strstr(line + 2, "pc=");
      char *q = strstr(line + 2, "types=");
      char *cl = strstr(line + 2, "calls=");
      char *rs = strstr(line + 2, "results=");
      if (p && q && cl && rs) {
        int pc = (int)strtol(p + 3, NULL, 10);
        const char *nm = strstr(line + 2, "num=");
        const char *fm = strstr(line + 2, "frac=");
        const char *nz = strstr(line + 2, "negzero=");
        double nmin = 0, nmax = 0;
        int k;
        if (nm) prof_parse_range(nm + 4, &nmin, &nmax);
        k = pidx_find((int)(e - arr), 4, pc);
        if (k < 0) k = e->ncres;
        if (k == e->ncres) {
          if (pidx_put((int)(e - arr), 4, pc, k) < 0) return NULL;
          if (e->ncres == e->capcres) {
            ProfPc *np;
            e->capcres = e->capcres ? e->capcres * 2 : 16;
            np = realloc(e->cres, sizeof(*np) * (size_t)e->capcres);
            if (!np) return NULL;
            e->cres = np;
          }
          e->ncres++;
          memset(&e->cres[k], 0, sizeof(e->cres[k]));
          e->cres[k].pc = pc;
        }
        e->cres[k].types |= prof_parse_types(q + 6);
        e->cres[k].calls += (unsigned)strtoul(cl + 6, NULL, 10);
        e->cres[k].results += (unsigned)strtoul(rs + 8, NULL, 10);
        if (fm) e->cres[k].frac |= (strtol(fm + 5, NULL, 10) != 0);
        if (nz) e->cres[k].negzero |= (strtol(nz + 8, NULL, 10) != 0);
        if (nm) {
          if (!e->cres[k].has_num) {
            e->cres[k].num_min = nmin;
            e->cres[k].num_max = nmax;
          } else {
            if (nmin < e->cres[k].num_min) e->cres[k].num_min = nmin;
            if (nmax > e->cres[k].num_max) e->cres[k].num_max = nmax;
          }
          e->cres[k].has_num = 1;
        }
      }
    } else if (line[0] == 'R' && strstr(line + 2, " total=") != NULL) { /* a read site's receiver census; the register type sets are the `else` below */
      const char *p = strstr(line + 2, "site="), *t = strstr(line + 2, " total=");
      if (p && t) {
        int site = (int)strtol(p + 5, NULL, 10), j = pidx_find((int)(e - arr), 5, site);
        if (j >= 0) {
          ProfSite *ps = &e->sites[j];
          unsigned long long total = strtoull(t + 7, NULL, 10);
          const char *q = line + 2;
          while ((q = strstr(q, " r")) != NULL) {
            const char *c;
            q += 2;
            if (*q < '0' || *q > '9') continue;
            c = strchr(q, ':');
            if (c != NULL && ps->nr < 8) {
              const char *eq = strchr(q, '=');
              if (eq != NULL && eq < c) {
                ps->rshape[ps->nr] = strtoull(eq + 1, NULL, 10);
                ps->rcount[ps->nr++] = strtoull(c + 1, NULL, 10);
              }
            }
          }
          ps->rtotal += total;
        }
      }
    } else if (line[0] == 'S') {
      /* S <hex> site=<k> count=<n> src=.. shapes=<m> shape0=0x.. hits=..
         [shape1=..]. The shapes are process addresses; only equality across
         sites of the same profile is meaningful. */
      char *p = strstr(line + 2, "site=");
      char *q = strstr(line + 2, "shapes=");
      if (p && q) {
        int site = (int)strtol(p + 5, NULL, 10);
        int nshapes = (int)strtol(q + 7, NULL, 10);
        ProfSite *ps = NULL;
        int j, k;
        if (nshapes > PROF_MAX_SHAPES) nshapes = PROF_MAX_SHAPES;
        if (nshapes < 0) nshapes = 0;
        j = pidx_find((int)(e - arr), 5, site);
        if (j >= 0) ps = &e->sites[j];
        if (!ps) {
          if (pidx_put((int)(e - arr), 5, site, e->nsites) < 0) return NULL;
          if (e->nsites == e->capsites) {
            ProfSite *ns;
            e->capsites = e->capsites ? e->capsites * 2 : 16;
            ns = realloc(e->sites, sizeof(*ns) * (size_t)e->capsites);
            if (!ns) return NULL;
            e->sites = ns;
          }
          ps = &e->sites[e->nsites++];
          memset(ps, 0, sizeof(*ps));
          ps->site = site;
          ps->pc = -1;
        }
        {
          const char *pp = strstr(line + 2, " pc=");
          if (pp) ps->pc = (int)strtol(pp + 4, NULL, 10);
        }
        {
          const char *kd = strstr(line + 2, "kinds=");
          if (kd) ps->kinds |= (unsigned)strtoul(kd + 6, NULL, 10);
        }
        {
          const char *cd = strstr(line + 2, " count=");
          if (cd) ps->count += (unsigned)strtoul(cd + 7, NULL, 10);
        }
        for (k = 0; k < nshapes; k++) {
          char key[24];
          char *s;
          uint64_t sh;
          unsigned hv;
          snprintf(key, sizeof(key), "shape%d=", k);
          s = strstr(line + 2, key);
          if (!s) continue;
          /* Decimal: the dump prints a TABLE INDEX, not an address. It was base
             16 while the record held an address, and reading an index as hex
             silently mapped every site to the wrong shape. */
          sh = strtoull(s + strlen(key), NULL, 10);
          {
            const char *hp = strstr(s, " hits=");
            hv = hp != NULL ? (unsigned)strtoul(hp + 6, NULL, 10) : 0;
          }
          for (j = 0; j < ps->nshapes; j++)
            if (ps->shape[j] == sh) break;
          if (j < ps->nshapes) { ps->hits[j] += hv; continue; }
          if (ps->nshapes < PROF_MAX_SHAPES) {
            char skey[24];
            char *ss;
            ps->hits[ps->nshapes] = hv;
            ps->slot[ps->nshapes] = -1;
            snprintf(skey, sizeof(skey), "slot%d=", k);
            ss = strstr(line + 2, skey);
            if (ss) ps->slot[ps->nshapes] = (int)strtol(ss + strlen(skey), NULL, 10);
            ps->shape[ps->nshapes++] = sh;
          }
        }
      }
    } else if (line[0] == 'A') {
      /* A <hex> site=<k> pc=<pc> own=<N> add=<N> other=<N> ...: the executions of a store site by kind (see ProfSite). Repeated records of one
         site (two bytecodes with one digest) add up. */
      char *p = strstr(line + 2, "site=");
      char *po = strstr(line + 2, " own=");
      char *pa = strstr(line + 2, " add=");
      char *pt = strstr(line + 2, " other=");
      if (p && po && pa && pt && getenv("QJS_AOT_NOAREC") == NULL) {
        int site = (int)strtol(p + 5, NULL, 10), j;
        ProfSite *ps = NULL;
        j = pidx_find((int)(e - arr), 5, site);
        if (j >= 0) ps = &e->sites[j];
        if (!ps) {
          const char *nn0 = strstr(line + 2, " nown=");
          int has_own = nn0 != NULL && (int)strtol(nn0 + 6, NULL, 10) > 0 && strtoull(pa + 5, NULL, 10) <= 16 && strtoull(pa + 5, NULL, 10) * 1024 <= strtoull(po + 5, NULL, 10) &&
                        strtoull(pt + 7, NULL, 10) == 0;
          if (!has_own && getenv("QJS_AOT_AASIDE") != NULL) {
            int ai;
            for (ai = 0; ai < e->naonly; ai++)
              if (e->aonly[ai].site == site) break;
            if (ai == e->naonly) {
              if (e->naonly == e->capaonly) {
                struct ProfAOnly *na;
                e->capaonly = e->capaonly ? e->capaonly * 2 : 8;
                na = realloc(e->aonly, sizeof(*na) * (size_t)e->capaonly);
                if (!na) return NULL;
                e->aonly = na;
              }
              memset(&e->aonly[e->naonly], 0, sizeof(e->aonly[0]));
              e->aonly[e->naonly++].site = site;
            }
            e->aonly[ai].own += strtoull(po + 5, NULL, 10);
            e->aonly[ai].add += strtoull(pa + 5, NULL, 10);
            e->aonly[ai].oth += strtoull(pt + 7, NULL, 10);
            continue;
          }
          if (pidx_put((int)(e - arr), 5, site, e->nsites) < 0) return NULL;
          if (e->nsites == e->capsites) {
            ProfSite *ns;
            e->capsites = e->capsites ? e->capsites * 2 : 16;
            ns = realloc(e->sites, sizeof(*ns) * (size_t)e->capsites);
            if (!ns) return NULL;
            e->sites = ns;
          }
          ps = &e->sites[e->nsites++];
          memset(ps, 0, sizeof(*ps));
          ps->site = site;
          ps->pc = -1;
        }
        {
          const char *pp = strstr(line + 2, " pc=");
          if (pp && ps->pc < 0) ps->pc = (int)strtol(pp + 4, NULL, 10);
        }
        /* The own-slot overwrites by receiver shape (`o<i>=<shape>:<slot>:<count>`) are what the site did on every execution; the S record
           holds only the fills the cache took, one sample for a site that then hit. The first A record of a site replaces the S shapes
           with them (a later one, of a twin bytecode, adds). */
        {
          const char *nn = strstr(line + 2, " nown=");
          int nown = nn != NULL ? (int)strtol(nn + 6, NULL, 10) : 0, oi;
          if (strtoull(pt + 7, NULL, 10) != 0 || ((strtoull(pa + 5, NULL, 10) > 16 || strtoull(pa + 5, NULL, 10) * 1024 > strtoull(po + 5, NULL, 10)) && !prof_a_adds_into_own(line + 2, nown))) nown = 0;   /* a site that also adds or goes elsewhere keeps its record */
          {
            const char *rr = strstr(line + 2, " rest=");
            if (rr == NULL || strtoull(rr + 6, NULL, 10) != 0) nown = 0;   /* shapes the record did not keep: it proves nothing about them */
          }
          if (getenv("QJS_AOT_NOAOWN") != NULL) nown = 0;
          if (nown > 0 && !ps->ahas) {
            ps->nshapes = 0;
            ps->count = 0;
            memset(ps->hits, 0, sizeof(ps->hits));
          }
          for (oi = 0; oi < nown && oi < PROF_MAX_SHAPES; oi++) {
            char key[8];
            const char *ok;
            unsigned long long osh = 0, on = 0;
            int osl = -1, jj;
            snprintf(key, sizeof(key), " o%d=", oi);
            ok = strstr(line + 2, key);
            if (ok == NULL || sscanf(ok + strlen(key), "%llu:%d:%llu", &osh, &osl, &on) != 3) continue;
            /* a shape under a thousandth of the site's overwrites is left out when another shape carries them: a receiver of that shape takes the
               store's miss route (the transition store or the exit) instead of the site becoming a many-shape one */
            if (oi > 0 && on * 1000 < strtoull(po + 5, NULL, 10) && ps->nshapes >= 1 && getenv("QJS_AOT_NORARESHAPE") == NULL) continue;
            ps->count += (unsigned)on;
            ps->kinds |= 1u << 3;
            for (jj = 0; jj < ps->nshapes; jj++)
              if (ps->shape[jj] == osh) break;
            if (jj < ps->nshapes) { ps->hits[jj] += (unsigned)on; continue; }
            if (ps->nshapes < PROF_MAX_SHAPES) {
              ps->shape[ps->nshapes] = osh;
              ps->slot[ps->nshapes] = osl;
              ps->hits[ps->nshapes++] = (unsigned)on;
            }
          }
        }
        /* A site the record says overwrote on the shapes just taken and also added by transition: the add fill (bit 4) is the transition store's, so the
           site reads as an own-slot site for the grouping and the layouts alike. */
        if ((ps->kinds & (1u << 4)) != 0 && (ps->kinds & (1u << 3)) != 0 && ps->nshapes > 0 && strstr(line + 2, " nxf=") != NULL && strtoull(pa + 5, NULL, 10) * 8 <= strtoull(po + 5, NULL, 10) && getenv("QJS_AOT_NOADDWOWN") == NULL)
          ps->kinds &= ~(1u << 4);
        ps->ahas = 1;
        ps->aown += strtoull(po + 5, NULL, 10);
        ps->aadd += strtoull(pa + 5, NULL, 10);
        ps->aoth += strtoull(pt + 7, NULL, 10);
      }
    } else if (line[0] == 'W') {
      /* W <hex> pc=<n> rshape=<id> rmulti=<0|1>: merged by pc; two records
         that disagree on the shape count as several shapes. */
      char *p = strstr(line + 2, "pc=");
      char *q = strstr(line + 2, "rshape=");
      char *m = strstr(line + 2, "rmulti=");
      if (p && q && m) {
        int pc = (int)strtol(p + 3, NULL, 10);
        int sh = (int)strtol(q + 7, NULL, 10);
        int multi = (int)strtol(m + 7, NULL, 10) != 0;
        const char *ty = strstr(line + 2, "types=");
        unsigned types = ty != NULL ? (unsigned)strtoul(ty + 6, NULL, 10) : 0;
        int k;
        const char *od = strstr(line + 2, "ord=");
        int ord = od != NULL ? (int)strtol(od + 4, NULL, 10) : 0;
        for (k = 0; k < e->nres; k++)
          if (e->res[k].pc == pc && e->res[k].ord == ord) break;
        if (k == e->nres) {
          if (e->nres == e->capres) {
            ProfRes *nr;
            e->capres = e->capres ? e->capres * 2 : 16;
            nr = realloc(e->res, sizeof(*nr) * (size_t)e->capres);
            if (!nr) return NULL;
            e->res = nr;
          }
          e->nres++;
          e->res[k].pc = pc;
          e->res[k].ord = ord;
          e->res[k].shape = sh;
          e->res[k].multi = multi;
          e->res[k].types = types;
        } else {
          e->res[k].types |= types;
          if (e->res[k].shape != sh) {
            if (e->res[k].shape < 0) e->res[k].shape = sh;
            else if (sh >= 0) e->res[k].multi = 1;
          }
          if (multi) e->res[k].multi = 1;
        }
      }
    } else if (line[0] == 'N') {
      /* N <hex> pc=<lo>..<hi> n=<executions> */
      char *p = strstr(line + 2, "pc=");
      char *q = strstr(line + 2, " n=");
      if (p && q) {
        char *end;
        int lo = (int)strtol(p + 3, &end, 10), hi = lo;
        if (end[0] == '.' && end[1] == '.') hi = (int)strtol(end + 2, NULL, 10);
        if (e->nexec == e->capexec) {
          struct ProfExecRun *nr;
          e->capexec = e->capexec ? e->capexec * 2 : 16;
          nr = realloc(e->exec, sizeof(*nr) * (size_t)e->capexec);
          if (!nr) return NULL;
          e->exec = nr;
        }
        e->exec[e->nexec].lo = lo;
        e->exec[e->nexec].hi = hi;
        e->exec[e->nexec].n = strtoull(q + 3, NULL, 10);
        e->nexec++;
      }
    } else if (line[0] == 'T') {
      /* T <hex> pc=<n> samples=<s>: the sampler charged `s` ticks to the
         instruction at `n`. The profiler takes one sample per 1024 dispatches,
         so `s * 1024` estimates that pc's execution count, which is what the
         op-call weight needs; the scaling happens in the accessor so a merged
         profile sums ticks before it scales. Kept apart from the type records
         because a hot pc can have a sample record and no types. */
      char *p = strstr(line + 2, "pc=");
      char *smp = strstr(line + 2, "samples=");
      if (p && smp) {
        int pc = (int)strtol(p + 3, NULL, 10);
        unsigned long long n = strtoull(smp + 8, NULL, 10);
        int k;
        for (k = 0; k < e->ncounts; k++)
          if (e->counts[k].pc == pc) break;
        if (k == e->ncounts) {
          if (e->ncounts == e->capcounts) {
            ProfCount *nc;
            e->capcounts = e->capcounts ? e->capcounts * 2 : 16;
            nc = realloc(e->counts, sizeof(*nc) * (size_t)e->capcounts);
            if (!nc) return NULL;
            e->counts = nc;
          }
          e->ncounts++;
          e->counts[k].pc = pc;
          e->counts[k].count = 0;
        }
        e->counts[k].count += n;
      }
    } else if (line[0] == 'U') {
      /* U <hex> pc=<a>..<b>: a range the profiler never executed. */
      char *p = strstr(line + 2, "pc=");
      if (p) {
        int lo = 0, hi = 0;
        if (sscanf(p + 3, "%d..%d", &lo, &hi) == 2) {
          if (e->nunused == e->capunused) {
            ProfUnused *nu;
            e->capunused = e->capunused ? e->capunused * 2 : 8;
            nu = realloc(e->unused, sizeof(*nu) * (size_t)e->capunused);
            if (!nu) return NULL;
            e->unused = nu;
          }
          e->unused[e->nunused].lo = lo;
          e->unused[e->nunused].hi = hi;
          e->nunused++;
        }
      }
    } else { /* R */
      char *p = strstr(line + 2, "reg=");
      char *q = strstr(line + 2, "types=");
      if (p && q) {
        int reg = (int)strtol(p + 4, NULL, 10);
        unsigned t = prof_parse_types(q + 6);
        int k;
        for (k = 0; k < e->nregs; k++)
          if (e->regs[k].reg == reg) {
            e->regs[k].types |= t;
            break;
          }
        if (k == e->nregs) {
          if (e->nregs == e->capregs) {
            ProfReg *nr;
            e->capregs = e->capregs ? e->capregs * 2 : 16;
            nr = realloc(e->regs, sizeof(*nr) * (size_t)e->capregs);
            if (!nr) return NULL;
            e->regs = nr;
          }
          e->regs[e->nregs].reg = reg;
          e->regs[e->nregs].types = t;
          e->nregs++;
        }
      }
    }
  }
  fclose(f);
  pidx_free();
  for (i = 0; i < n; i++) {
    if (arr[i].npcs > 1)
      qsort(arr[i].pcs, (size_t)arr[i].npcs, sizeof(arr[i].pcs[0]), cmp_profpc);
    if (arr[i].ncounts > 1)
      qsort(
          arr[i].counts, (size_t)arr[i].ncounts, sizeof(arr[i].counts[0]),
          cmp_profcount);
    /* A call site's result record is believed only when the header says the
       profile records them and every call that entered the site returned a
       recorded value. A call that threw, or one that returned through a path
       the profiler does not see, leaves `results` short, and the site stays
       untyped. */
    {
      int k, m = 0;
      for (k = 0; k < arr[i].ncres; k++) {
        const ProfPc *c = &arr[i].cres[k];
        if (g_prof_callres && c->calls > 0 && c->results == c->calls)
          arr[i].cres[m++] = *c;
      }
      arr[i].ncres = m;
      if (m > 1)
        qsort(arr[i].cres, (size_t)m, sizeof(arr[i].cres[0]), cmp_profpc);
    }
  }
  *pn = n;
  return arr;
}

static const ProfEntry *prof_entry(
    const ProfEntry *arr, int n, const uint8_t dg[16]) {
  int i;
  for (i = 0; i < n; i++)
    if (memcmp(arr[i].digest, dg, 16) == 0) return &arr[i];
  return NULL;
}

/* The dominant profiled callee at `pc`, or NULL. */
static const ProfCall *prof_call_at(const ProfEntry *e, int pc) {
  int i;
  if (!e) return NULL;
  for (i = 0; i < e->ncalls; i++)
    if (e->calls[i].pc == pc) return &e->calls[i];
  return NULL;
}

static unsigned long long prof_lookup(
    const ProfEntry *arr, int n, const uint8_t dg[16]) {
  int i;
  for (i = 0; i < n; i++)
    if (memcmp(arr[i].digest, dg, 16) == 0) return arr[i].ops;
  return 0;
}

/* Count register instructions in an eligible program. */
static long long count_insns(const uint8_t *code, int len) {
  long long n = 0;
  int off = 0;
  while (off < len) {
    int op, sz = js_aot_gen_insn_size(code, len, off, &op);
    if (sz < 0) break;
    n++;
    off += sz;
  }
  return n;
}

static void print_census(
    const JSAotGenFunc *funcs, int nfuncs, const ProfEntry *prof, int nprof) {
  long long cnt[AOT_GEN_REF_REASON_COUNT];
  long long insns[AOT_GEN_REF_REASON_COUNT];
  long long stack[AOT_GEN_REF_REASON_COUNT];
  long long pops[AOT_GEN_REF_REASON_COUNT];
  long long tot_insns = 0, tot_stack = 0, tot_pops = 0;
  long long ref_pops = 0, ref_funcs = 0;
  int i, r;

  memset(cnt, 0, sizeof(cnt));
  memset(insns, 0, sizeof(insns));
  memset(stack, 0, sizeof(stack));
  memset(pops, 0, sizeof(pops));
  for (i = 0; i < nfuncs; i++) {
    r = funcs[i].eligible;
    if (r < 0 || r >= AOT_GEN_REF_REASON_COUNT) r = AOT_GEN_REF_REGVM;
    cnt[r]++;
    if (r == AOT_GEN_ELIGIBLE)
      insns[r] += count_insns(funcs[i].code, funcs[i].len);
    else
      stack[r] += funcs[i].stack_len;
    if (nprof > 0) {
      unsigned long long o = prof_lookup(prof, nprof, funcs[i].digest);
      pops[r] += (long long)o;
      tot_pops += (long long)o;
    }
  }
  for (r = 0; r < AOT_GEN_REF_REASON_COUNT; r++) {
    tot_insns += insns[r];
    tot_stack += stack[r];
    if (r != AOT_GEN_ELIGIBLE) {
      ref_pops += pops[r];
      ref_funcs += cnt[r];
    }
  }

  /* A profile entry whose digest is not in the unit is the sharpest measure of
     refused work: it ran on the register VM but the generator did not emit a
     body for it. (Functions the register backend itself refused never reach the
     profile, so they cannot be weighted here.) */
  {
    int j;
    for (j = 0; j < nprof; j++) {
      int k, hit = 0;
      for (k = 0; k < nfuncs; k++)
        if (funcs[k].eligible == AOT_GEN_ELIGIBLE &&
            memcmp(funcs[k].digest, prof[j].digest, 16) == 0) {
          hit = 1;
          break;
        }
      if (!hit) {
        ref_pops += (long long)prof[j].ops;
        tot_pops += (long long)prof[j].ops;
        fprintf(
            stderr,
            "qjsaot: profiled digest %02x%02x%02x%02x... has no unit body "
            "(%llu ops)\n",
            prof[j].digest[0], prof[j].digest[1], prof[j].digest[2],
            prof[j].digest[3], prof[j].ops);
      }
    }
  }

  fprintf(stderr, "qjsaot: refusal census\n");
  fprintf(
      stderr, "  %-14s %8s %12s %12s %12s\n", "reason", "funcs", "reginsns",
      "stackB", "profops");
  for (r = 0; r < AOT_GEN_REF_REASON_COUNT; r++) {
    if (cnt[r] == 0) continue;
    fprintf(
        stderr, "  %-14s %8lld %12lld %12lld %12lld\n", reason_name(r), cnt[r],
        insns[r], stack[r], pops[r]);
  }
  fprintf(
      stderr, "  %-14s %8d %12lld %12lld %12lld\n", "total", nfuncs, tot_insns,
      tot_stack, tot_pops);

  fprintf(
      stderr, "qjsaot: refused functions %lld/%d (%.2f%%)", ref_funcs, nfuncs,
      nfuncs ? 100.0 * (double)ref_funcs / nfuncs : 0.0);
  if (nprof > 0)
    fprintf(
        stderr, ", refused profiled ops %lld/%lld (%.3f%%)", ref_pops, tot_pops,
        tot_pops ? 100.0 * (double)ref_pops / (double)tot_pops : 0.0);
  fprintf(stderr, "\n");
}

/* The deopt-everywhere sweep and its site ids, shared with the emitter: under
   the sweep every instruction is a guard site, and the ids have to come from
   the driver's single id space. */
/* The AOT proto-fold site ids are a second id space, handed out per emitted
   `get_field2`; the emitter needs it for the fold it emits too. */
int qjs_aot_site_new(void) {
  return g_site_next++;
}

/* A deopt site for a real guard: allocated in every build, so a site id never
   depends on whether the sweep is on. */
int qjs_aot_site_alloc(void) {
  int sid = g_deopt_site_next++;
  deopt_site_note(sid);
  return sid;
}

int qjs_aot_sweep_on(void) {
  return g_deopt_everywhere;
}

int qjs_aot_check_locals(void) {
  return g_check_locals;
}

/* The `P` records of the function with this digest, sorted by pc, or NULL.
   `*n` receives the count. The `qjsaot` IR reads a definition's profile from
   the record at its defining instruction's pc. */
const ProfPc *qjs_aot_prof_pcs(const uint8_t dg[16], int *n) {
  const ProfEntry *e;
  *n = 0;
  if (!g_prof) return NULL;
  e = prof_entry(g_prof, g_nprof, dg);
  if (!e) return NULL;
  *n = e->npcs;
  return e->pcs;
}

/* The function's complete call-result (`K`) records, sorted by pc, or NULL. The
   pc is the one just past the call, so it is the call instruction's `next`. */
const ProfPc *qjs_aot_prof_cres(const uint8_t dg[16], int *n) {
  const ProfEntry *e;
  *n = 0;
  if (!g_prof) return NULL;
  e = prof_entry(g_prof, g_nprof, dg);
  if (!e) return NULL;
  *n = e->ncres;
  return e->cres;
}

/* The function's `R` records: one merged type set per register, over the whole
   function. A register the profiler never saw has no entry, so `*n` is the
   number of registers that *have* a record -- a caller must not treat the
   absence of a register as a record with no types. */
const ProfReg *qjs_aot_prof_regs(const uint8_t dg[16], int *n) {
  const ProfEntry *e;
  *n = 0;
  /* Only the threaded build records `R` completely; on any other profile the
     records would describe a register's *other* values and mis-type a reused
     destination, so none are offered. */
  if (!g_prof || !g_prof_thr) return NULL;
  e = prof_entry(g_prof, g_nprof, dg);
  if (!e) return NULL;
  *n = e->nregs;
  return e->regs;
}

/* The instruction at `pc` of the function with this digest, as an estimated
   execution count, or 0 when the profile has no sample there. The profiler
   charges one tick per 1024 dispatches, so the count is the sample count
   scaled back up; it is an estimate and is reported as one. */
unsigned long long qjs_aot_prof_pc_count(const uint8_t dg[16], int pc);
unsigned long long qjs_aot_prof_entries(const uint8_t dg[16]);
unsigned long long qjs_aot_prof_entries(const uint8_t dg[16]) {
  const ProfEntry *e = g_prof ? prof_entry(g_prof, g_nprof, dg) : NULL;
  return e ? e->entries : 0;
}

/* The calls a call site made in the profile run (exact: the sum over its callees), 0 for a site that never ran. */
unsigned long long qjs_aot_prof_site_calls(const uint8_t dg[16], int next_pc) {
  const ProfEntry *e = g_prof ? prof_entry(g_prof, g_nprof, dg) : NULL;
  unsigned long long t = 0;
  int i, k;
  if (!e) return 0;
  for (i = 0; i < e->ncallsites; i++) {
    if (e->callsites[i].pc != next_pc) continue;
    for (k = 0; k < e->callsites[i].n && k < PROF_MAX_CALLEES; k++) t += e->callsites[i].c[k].hits;
  }
  return t;
}

/* How many times the instruction at `pc` ran in the profiled run, or -1 when the profile has no execution counts for the function (an older profile). */
long long qjs_aot_prof_pc_exec(const uint8_t dg[16], int pc) {
  const ProfEntry *e = g_prof ? prof_entry(g_prof, g_nprof, dg) : NULL;
  long long t = 0;
  int i;
  if (!e || e->nexec == 0) return -1;
  for (i = 0; i < e->nexec; i++)
    if (pc >= e->exec[i].lo && pc <= e->exec[i].hi) t += (long long)e->exec[i].n;
  return t;
}

/* A store site whose cache never serves it: the code around it ran (the profile counts its executions) and the site was never filled (the store
   ran a setter, or landed in a place the cache does not hold), or it was filled again and again with different shapes (the cache goes dead after
   8 of them). An inline or frameless body leaves the compiled code at such a store on every execution; the body that contains it is left to the
   framed form, where the miss continues. QJS_AOT_NOSTOREUNCACHED restores the old reading, for the negative control only. */
int qjs_aot_prof_store_uncached(const uint8_t dg[16], int site, int pc) {
  const ProfEntry *e = g_prof ? prof_entry(g_prof, g_nprof, dg) : NULL;
  int i;
  if (!e || !g_prof_sitekinds || getenv("QJS_AOT_NOSTOREUNCACHED") != NULL) return 0;
  for (i = 0; i < e->nsites; i++) {
    const ProfSite *ps = &e->sites[i];
    if (ps->site != site) continue;
    if (ps->ahas) {
      unsigned long long tot = ps->aown + ps->aadd + ps->aoth;
      return tot >= 16 && ps->aoth * 8 >= tot;
    }
    return ps->count >= 8 && ps->nshapes >= 2;
  }
  for (i = 0; i < e->naonly; i++)
    if (e->aonly[i].site == site) {
      unsigned long long tot = e->aonly[i].own + e->aonly[i].add + e->aonly[i].oth;
      return tot >= 16 && e->aonly[i].oth * 8 >= tot;
    }
  return qjs_aot_prof_pc_exec(dg, pc) >= 16;
}

/* Whether the instruction at `pc` is hot: it ran, and at least 2% as often as the function's most executed instruction (a branch taken in a
   percent or two of the iterations is cold, however many times it ran in absolute terms; a loop body is hot whatever the function's entries).
   A profile without execution counts falls back to the sampler. */
int qjs_aot_prof_pc_hot(const uint8_t dg[16], int pc) {
  const ProfEntry *e = g_prof ? prof_entry(g_prof, g_nprof, dg) : NULL;
  long long n = qjs_aot_prof_pc_exec(dg, pc), mx = 0;
  int i;
  if (n < 0 || e == NULL) return qjs_aot_prof_pc_count(dg, pc) > 0;
  if (n == 0) return 0;
  for (i = 0; i < e->nexec; i++)
    if ((long long)e->exec[i].n > mx) mx = (long long)e->exec[i].n;
  return n * 50 >= mx;
}

unsigned long long qjs_aot_prof_pc_count(const uint8_t dg[16], int pc) {
  const ProfEntry *e = g_prof ? prof_entry(g_prof, g_nprof, dg) : NULL;
  int lo, hi;

  if (!e || e->ncounts == 0) return 0;
  lo = 0;
  hi = e->ncounts - 1;
  while (lo <= hi) {
    int mid = lo + (hi - lo) / 2;
    if (e->counts[mid].pc == pc) return e->counts[mid].count * 1024ull;
    if (e->counts[mid].pc < pc)
      lo = mid + 1;
    else
      hi = mid - 1;
  }
  return 0;
}

/* The dominant profiled call at `pc` of the function with this digest. Returns
   1 and fills `*kind` (1 for a bytecode callee, 2 for a C function, 3 for
   anything else), `*cproto` (-1 unless a C callee) and `hex` (the bytecode
   callee's digest hex, 33 bytes including the terminator), or 0 when the
   function or pc has no call record. Exposed for the SSA emitter's call
   lowering. */
int qjs_aot_prof_call(
    const uint8_t dg[16], int pc, int *kind, int *cproto, char hex[33]) {
  const ProfEntry *e = g_prof ? prof_entry(g_prof, g_nprof, dg) : NULL;
  const ProfCall *c = prof_call_at(e, pc);
  if (!c) return 0;
  *kind = c->kind;
  *cproto = c->cproto;
  if (hex) {
    memcpy(hex, c->digest, 33);
    hex[32] = 0;
  }
  return 1;
}

/* A site that also adds (`nxf` transitions in its record): its overwrite shapes are what the site's stores end up on, and a receiver of another shape is the
   transition store's (see qjs_aot_store_xfer). */
static int prof_a_adds_into_own(const char *rec, int nown) {
  const char *nx = strstr(rec, " nxf=");
  int nxf = nx != NULL ? (int)strtol(nx + 5, NULL, 10) : 0;
  const char *po = strstr(rec, " own="), *pa = strstr(rec, " add=");
  (void)nown;
  /* the adds are at most an eighth of the overwrites: more than that is a site that builds objects, not one that overwrites them */
  if (po == NULL || pa == NULL || strtoull(pa + 5, NULL, 10) * 8 > strtoull(po + 5, NULL, 10)) return 0;
  return nxf > 0 && nxf <= PROF_MAX_SHAPES && getenv("QJS_AOT_NOAXFER") == NULL;
}
/* A store site the profile cannot tell an overwrite from an add. The engine records a store only when its cache misses, so a site that ran many
   times carries the one sample of its first miss, and an add records no shape: an own-slot record with a single fill, in a function entered
   many times, whose stores continue a fill of one shape (slot 0, or every lower slot stored earlier), is what an initialiser looks like and what a `Set(x, y)` looks like. The
   receiver may be a fresh object, so the site proves no receiver shape; it takes the cached store, which hits an existing field and adds a new
   one (running the layout claim check). Opt-in (QJS_AOT_STOREAMBIG=1): the stores of such a site then take the cached add/overwrite path, which refuses objects of a contracted layout (q2_put_field_hit declines an object in a layout, q2_put_add one whose old shape is a layout's), so on a layout class the inline bodies leave at every store (richards 318M -> 1026M instructions). Measured on box2d it removes the interpreter share but cannot be on by default. */
static int prof_store_continues_fill(const ProfEntry *e, int i) {
  const ProfSite *ps = &e->sites[i];
  int j, s;
  if (ps->kinds != (1u << 3) || ps->nshapes != 1 || ps->count != 1 || ps->slot[0] < 0 || ps->slot[0] > 16 || e->entries < 16) return 0;
  /* the slots below this one were each stored earlier in the function (on the same shape): the store continues a fill */
  for (s = 0; s < ps->slot[0]; s++) {
    for (j = 0; j < e->nsites; j++) {
      const ProfSite *q = &e->sites[j];
      if (j != i && q->kinds == (1u << 3) && q->nshapes == 1 && q->count == 1 && q->shape[0] == ps->shape[0] && q->slot[0] == s && q->pc >= 0 && q->pc < ps->pc) break;
    }
    if (j == e->nsites) return 0;
  }
  return 1;
}
static int prof_store_ambiguous(const ProfEntry *e, int i) {
  return getenv("QJS_AOT_STOREAMBIG") != NULL && prof_store_continues_fill(e, i);
}
/* A store site the generator emits as a transition store (QJS_AOT_ADDXFER=1): the group's overwrite when the receiver has the group's shape, else the cached
   add replay. Today decided from the one-sample record (see prof_store_continues_fill); with the engine's add/overwrite counts it is `add > 0`. */
int qjs_aot_store_xfer(const uint8_t dg[16], int site) {
  const ProfEntry *e = g_prof && getenv("QJS_AOT_NOADDXFER") == NULL ? prof_entry(g_prof, g_nprof, dg) : NULL;
  int i;
  if (!e || !g_prof_sitekinds) return 0;
  for (i = 0; i < e->nsites; i++) {
    if (e->sites[i].site != site) continue;
    if (e->sites[i].ahas) return e->sites[i].aadd > 0 && (e->sites[i].aadd * 8 >= e->sites[i].aown + e->sites[i].aoth || (e->sites[i].aadd >= 16 && e->sites[i].aadd * 128 >= e->sites[i].aown));
    return getenv("QJS_AOT_ADDXFER") != NULL && prof_store_continues_fill(e, i);
  }
  for (i = 0; i < e->naonly; i++)
    if (e->aonly[i].site == site) return e->aonly[i].add > 0 && (e->aonly[i].add * 8 >= e->aonly[i].own + e->aonly[i].oth || (e->aonly[i].add >= 16 && e->aonly[i].add * 128 >= e->aonly[i].own));
  return 0;
}

/* The shapes one property site saw, from its `S` record. `site` is the site
   index the emitter uses (the k-th property access in source order). Returns 1
   and fills `nshapes`/`shapes` (at most PROF_MAX_SHAPES entries) when there is
   a record, 0 otherwise. The shape values are profile-process addresses: they
   compare only against other shapes of the same profile. */
int qjs_aot_prof_site(
    const uint8_t dg[16], int site, int *nshapes, uint64_t shapes[4]) {
  const ProfEntry *e = g_prof ? prof_entry(g_prof, g_nprof, dg) : NULL;
  int i, k;
  if (!e) return 0;
  for (i = 0; i < e->nsites; i++) {
    if (e->sites[i].site != site) continue;
    *nshapes = e->sites[i].nshapes;
    /* A site that counted more fills than its recorded shapes account for saw shapes it did not keep: report it as seeing more than it lists
       (5), so no caller reads a saturated site as one that saw its kept shapes only. */
    {
      unsigned sum = 0;
      for (k = 0; k < e->sites[i].nshapes && k < PROF_MAX_SHAPES; k++) sum += e->sites[i].hits[k];
      if (e->sites[i].nshapes >= 1 && e->sites[i].count != sum && getenv("QJS_AOT_NOSAT") == NULL) *nshapes = e->sites[i].nshapes + 1;
      if (e->sites[i].nshapes >= 1 && prof_store_ambiguous(e, i)) *nshapes = e->sites[i].nshapes + 1;
    }
    for (k = 0; k < PROF_MAX_SHAPES; k++) shapes[k] = e->sites[i].shape[k];
    return 1;
  }
  return 0;
}

/* E0 (QJS_AOT_REGION_MEASURE): print the property sites of one function whose profile saw shapes: the count (4 = the profile's per-site cap, so "4 or more") and each shape's ordered fields. */
static const ProfShape *prof_shape_by_id(int id);
void qjs_aot_prof_site_dump(const uint8_t dg[16], const char *tag) {
  const ProfEntry *e = g_prof ? prof_entry(g_prof, g_nprof, dg) : NULL;
  int i, k, f;
  if (!e) return;
  for (i = 0; i < e->nsites; i++) {
    const ProfSite *ps = &e->sites[i];
    if (ps->nshapes < 2) continue;
    fprintf(stderr, "measure: %s site %d saw %d shapes:", tag, ps->site, ps->nshapes);
    for (k = 0; k < ps->nshapes; k++) {
      const ProfShape *sh = prof_shape_by_id((int)ps->shape[k]);
      fprintf(stderr, " [");
      if (sh) for (f = 0; f < sh->nfields; f++) fprintf(stderr, "%s%s", f ? "," : "", sh->field[f] ? sh->field[f] : "?");
      fprintf(stderr, "]");
    }
    fprintf(stderr, "\n");
  }
}

int qjs_aot_prof_access_shapes(const uint8_t dg[16], int pc, int which, const char *name, int write, QjsAotSiteShapes *out) {
  const ProfEntry *e = g_prof ? prof_entry(g_prof, g_nprof, dg) : NULL;
  int i, k, f, n = 0, sel[8], j;
  unsigned sum = 0;
  const ProfSite *ps;
  int why = getenv("QJS_AOT_REGION_WHY") != NULL;
  memset(out, 0, sizeof(*out));
  if (!e || g_shapes == NULL) return 0;
  /* the sites whose first fill was at this pc, in site order (the k-th property access in source order) */
  for (i = 0; i < e->nsites && n < 8; i++) if (e->sites[i].pc == pc && e->sites[i].nshapes > 0) sel[n++] = i;
  if (which < 0 || which >= n) { if (why) { fprintf(stderr, "why: site pc %d .%s: profile has %d record(s) there, wanted #%d; sites:", pc, name, n, which); for (i = 0; i < e->nsites; i++) fprintf(stderr, " %d(%d)", e->sites[i].pc, e->sites[i].nshapes); fprintf(stderr, "\n"); } return 0; }
  ps = &e->sites[sel[which]];
  for (k = 0; k < ps->nshapes; k++) sum += ps->hits[k];
  out->saturated = ps->count != sum;
  /* hottest first (stable on the profile's own order) */
  for (k = 0; k < ps->nshapes; k++) {
    const ProfShape *sh = prof_shape_by_id((int)ps->shape[k]);
    int idx = -1, d = out->n, t;
    if (sh == NULL || (sh->proto != 1 && sh->proto != 2) || sh->isproto || sh->nprops != sh->nfields || sh->nfields > 32 || sh->nfields < 1) { if (why) fprintf(stderr, "why: site pc %d .%s: shape %d unusable (proto %d isproto %d nprops %d nfields %d)\n", pc, name, (int)ps->shape[k], sh ? sh->proto : -1, sh ? sh->isproto : -1, sh ? sh->nprops : -1, sh ? sh->nfields : -1); return 0; }
    for (f = 0; f < sh->nfields; f++) {
      if (sh->field[f] == NULL || strlen(sh->field[f]) >= 40 || sh->flags[f] != 7) { if (why) fprintf(stderr, "why: site pc %d .%s: shape %d has a non-plain field\n", pc, name, (int)ps->shape[k]); return 0; }
      if (idx < 0 && strcmp(sh->field[f], name) == 0) idx = f;
    }
    if (idx < 0 && (write || sh->proto != 1)) { if (why) fprintf(stderr, "why: site pc %d .%s: shape %d lacks the field\n", pc, name, (int)ps->shape[k]); return 0; }
    /* a read the object's prototype answers (the field is a default there): slot -1 */
    if (!write && ps->slot[k] >= 0 && ps->slot[k] != idx) { if (why) fprintf(stderr, "why: site pc %d .%s: profile slot %d != field index %d (site mapping)\n", pc, name, ps->slot[k], idx); return 0; }
    for (t = 0; t < d; t++) {
      if (out->nf[t] != sh->nfields) continue;
      for (f = 0; f < sh->nfields && strcmp(out->f[t][f], sh->field[f]) == 0; f++) {}
      if (f == sh->nfields && out->pk[t] == sh->pk && out->proto[t] == sh->proto) break;
    }
    if (t < d) { out->hits[t] += ps->hits[k]; continue; }
    out->pk[d] = sh->pk;
    out->proto[d] = sh->proto;
    snprintf(out->digest[d], 33, "%s", sh->proto == 1 ? sh->digest : "");
    out->nf[d] = sh->nfields;
    for (f = 0; f < sh->nfields; f++) snprintf(out->f[d][f], 40, "%s", sh->field[f]);
    out->slot[d] = idx;
    out->hits[d] = ps->hits[k];
    out->n++;
  }
  /* hottest first: a stable insertion sort on hits */
  for (i = 1; i < out->n; i++) {
    for (j = i; j > 0 && out->hits[j] > out->hits[j - 1]; j--) {
      unsigned long long pk_ = out->pk[j]; unsigned h_ = out->hits[j]; int nf_ = out->nf[j], sl_ = out->slot[j], pr_ = out->proto[j];
      char dg_[33], f_[32][40];
      memcpy(dg_, out->digest[j], 33); memcpy(f_, out->f[j], sizeof(f_));
      out->proto[j] = out->proto[j - 1]; out->proto[j - 1] = pr_; out->pk[j] = out->pk[j - 1]; out->hits[j] = out->hits[j - 1]; out->nf[j] = out->nf[j - 1]; out->slot[j] = out->slot[j - 1];
      memcpy(out->digest[j], out->digest[j - 1], 33); memcpy(out->f[j], out->f[j - 1], sizeof(f_));
      out->pk[j - 1] = pk_; out->hits[j - 1] = h_; out->nf[j - 1] = nf_; out->slot[j - 1] = sl_;
      memcpy(out->digest[j - 1], dg_, 33); memcpy(out->f[j - 1], f_, sizeof(f_));
    }
  }
  return out->n > 0;
}

/* The slot each observed shape put this site's atom in, -1 when the profile did
   not record one. Two shapes with the same slot give a contracted site one
   constant offset, which is what lets a polymorphic site read a single slot. */
/* The shape table entry for a table index, or NULL. The fields are the shape's
   ordered property names and flags; `proto` says whether the unit can find the
   prototype at run time (1 = a constructor digest, 2 = Object.prototype). */
/* The table entry for a shape id, or NULL. */
static const ProfShape *prof_shape_by_id(int id) {
  int i;
  if (g_shapes == NULL) return NULL;
  for (i = 0; i < g_nshapes; i++)
    if (g_shapes[i].id == id) return &g_shapes[i];
  return NULL;
}

/* ---- contracted layouts ------------------------------------------------
   A candidate layout is one shape of the shape table that a monomorphic own-slot
   site saw, provided the unit can find its prototype at run time: a constructor
   digest (`fn:<digest>`) or Object.prototype. `array`, `null` and `other:` are
   refused with a reason, because the chain could not be rebuilt against the same
   prototype.

   A layout whose field list is a strict PREFIX of another candidate with the same
   prototype is refused too: an instance of the longer layout passes through the
   shorter one's final shape while it is being constructed, so it would break the
   shorter contract on arrival and never use it. Literals `{x,y}` and `{x,y,z}`
   are the common case. */
#define E_LAYOUT_MAX 63
int qjs_aot_layout_claims_any(void);
int qjs_aot_layout_claim_of(int layout, int slot);
typedef struct ELayoutInfo {
  int shape_id;
  int proto;              /* 1 = fn:<digest>, 2 = Object.prototype */
  char digest[33];
  int nfields;
} ELayoutInfo;

/* Gather the candidate layouts from the profile's shape table, dropping the ones
   the unit could not rebuild and the ones a longer layout shadows. Returns the
   number kept, in `out` (at most E_LAYOUT_MAX), and reports the refusals. */
void region_dry_collect(const Q2Func *root, const char hex[33], int fi);
int region_needs_ctor(const char *name);
unsigned region_lent_real(const char *hex);
int region_retslot_real(const char *hex);
int region_lent_verify(void);
int region_needs_digest(const char *hex);
int region_class_path(char (*names)[40], int nn, char *out, size_t cap);
static int g_rg_dry_done;
/* Region mode's extra layouts (classes seen at polymorphic sites, constructors found by a scan): with the dry pass done, only for a class a region
   binds (its constructor's digest is that of a function named as a region's class constructor). */
static int e_hex_cmp(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }
static int e_region_wants_layout(const ProfShape *sh) {
  /* with the dry pass done the set of constructor digests a region binds is fixed: built once, sorted, searched */
  static char (*need)[33];
  static int nneed, built, off = -1;
  if (off < 0) off = getenv("QJS_AOT_REGION_NOLAYOUTX") != NULL;
  if (qjsaot_region_mode() == 0 || off) return 0;
  if (!g_rg_dry_done) return 1;
  if (sh->proto != 1 || g_unit_funcs == NULL) return 0;
  if (region_needs_digest(sh->digest)) return 1;
  if (!built) {
    int i, h;
    need = malloc((size_t)(g_unit_nfuncs + 1) * sizeof(*need));
    for (i = 0; i < g_unit_nfuncs; i++) {
      const JSAotGenFunc *g = &g_unit_funcs[i];
      if (!g->name[0] || !region_needs_ctor(g->name)) continue;
      for (h = 0; h < 16; h++) snprintf(need[nneed] + 2 * h, 3, "%02x", g->digest[h]);
      nneed++;
    }
    qsort(need, (size_t)nneed, sizeof(*need), e_hex_cmp);
    built = 1;
  }
  return nneed > 0 && bsearch(sh->digest, need, (size_t)nneed, sizeof(*need), e_hex_cmp) != NULL;
}

static int e_layouts_collect_all(ELayoutInfo *out, int cap);
/* The candidates are gathered and the prefix rule applied over ALL of them, and only then cut to `cap`: stopping the scan at the cap
   kept the shorter of two shapes whose longer one came later in the table, so the generator's layout table and the layouts the run
   builds for a class could disagree about which shape of the class is the layout's. */
int e_layouts_collect(ELayoutInfo *out, int cap) {
  ELayoutInfo *all;
  int n;
  if (g_prof == NULL || g_shapes == NULL) return 0;
  if (getenv("QJS_AOT_LAYOUT_OLDCAP") != NULL) return e_layouts_collect_all(out, cap);   /* the scan that stopped at the cap: a negative control */
  all = calloc((size_t)g_nshapes + 1, sizeof(*all));
  if (all == NULL) return 0;
  n = e_layouts_collect_all(all, g_nshapes);
  if (n > cap) {
    /* More layouts than the unit can number: keep the ones the profile's time goes through (the ops of the functions with a site on the shape),
       in their original order so the numbering stays stable. */
    double *w = calloc((size_t)n, sizeof(*w));
    char *keep = calloc((size_t)n, 1);
    int i2, k2, f2, kept = 0;
    if (w != NULL && keep != NULL) {
      for (i2 = 0; i2 < n; i2++)
        for (f2 = 0; f2 < g_nprof; f2++) {
          const ProfEntry *pe = &g_prof[f2];
          for (k2 = 0; k2 < pe->nsites; k2++)
            if (pe->sites[k2].nshapes == 1 && pe->sites[k2].shape[0] == (uint64_t)all[i2].shape_id) w[i2] += (double)pe->ops + 1.0;
        }
      while (kept < cap) {
        int best = -1;
        for (i2 = 0; i2 < n; i2++)
          if (!keep[i2] && (best < 0 || w[i2] > w[best])) best = i2;
        keep[best] = 1;
        kept++;
      }
      kept = 0;
      for (i2 = 0; i2 < n; i2++)
        if (keep[i2]) all[kept++] = all[i2];
    }
    free(w);
    free(keep);
    if (getenv("QJS_AOT_LAYOUTCENSUS") != NULL)
      fprintf(stderr, "layout census: %d layouts survive the prefix rule, the %d most used are kept\n", n, cap);
    n = cap;
  }
  memcpy(out, all, sizeof(*out) * (size_t)n);
  free(all);
  return n;
}

static int e_layouts_collect_all(ELayoutInfo *out, int cap) {
  int i, j, n = 0, nproto = 0, nprefix = 0, nempty = 0;
  if (g_prof == NULL || g_shapes == NULL) return 0;
  for (i = 0; i < g_nshapes && n < cap; i++) {
    const ProfShape *sh = &g_shapes[i];
    int used = 0, f, k;
    /* Only a shape some monomorphic own-slot site actually reads. The sites are
       per function, so every entry of the profile is searched, not just the
       first. */
    for (f = 0; f < g_nprof && !used; f++) {
      const ProfEntry *pe = &g_prof[f];
      if (pe->sites == NULL) continue;
      for (k = 0; k < pe->nsites; k++) {
        const ProfSite *ps = &pe->sites[k];
        if (ps->nshapes == 1 && ps->shape[0] == (uint64_t)sh->id) {
          used = 1;
          break;
        }
        /* a class the region compiler dispatches on is seen by polymorphic sites too: its layout is what the facts word depends on */
        if (getenv("QJS_AOT_LAYOUT_ALL") != NULL || e_region_wants_layout(sh)) {
          int q;
          for (q = 0; q < ps->nshapes && q < PROF_MAX_SHAPES; q++)
            if (ps->shape[q] == (uint64_t)sh->id) used = 1;
          if (used) break;
        }
      }
    }
    if (getenv("QJS_AOT_LAYOUT_WHY") != NULL && atoi(getenv("QJS_AOT_LAYOUT_WHY")) == sh->id)
      fprintf(stderr, "layout why: shape %d used=%d proto=%d isproto=%d via_scan=%d nfields=%d\n", sh->id, used, sh->proto, sh->isproto, sh->via_scan, sh->nfields);
    if (!used) continue;
    if (sh->proto != 1 && sh->proto != 2) {
      nproto++;
      continue;
    }
    /* Such a layout serves the region compiler (the facts word depends on it); the per-site backend pays for a contracted layout in claim and store
       checks, so without the region compiler these classes keep the shape-guarded code they had (AOT-REDESIGN.md, MSG 80) */
    if (sh->isproto) {
      nproto++;
      continue;
    }
    if (sh->via_scan && !e_region_wants_layout(sh) && getenv("QJS_AOT_PLAIN_VIA_SCAN") == NULL) {
      nproto++;
      continue;
    }
    if (sh->nfields <= 0) {
      nempty++;
      continue;
    }
    if (sh->nfields > PROF_MAX_FIELDS) continue;
    /* A shape whose property list the profile could not hold whole (more than PROF_MAX_FIELDS properties) or that has a deleted slot is no layout: its
       list would build a different shape than the objects have, and every site on it is refused by qjs_aot_layout_site. */
    if (sh->nprops >= 0 && sh->nprops != sh->nfields && getenv("QJS_AOT_LAYOUT_TRUNC") == NULL) continue;
    out[n].shape_id = sh->id;
    out[n].proto = sh->proto;
    memcpy(out[n].digest, sh->digest, sizeof(out[n].digest));
    out[n].nfields = sh->nfields;
    n++;
  }
  /* The prefix refusal: a candidate whose field list is a strict prefix of
     another candidate's, with the same prototype, cannot be contracted -- an
     instance of the longer layout passes through the shorter one's final shape
     while it is constructed, so it would break the shorter contract at once.
     Marked first, then compacted, so one candidate's removal cannot shift
     another out from under the scan. */
  {
    int *drop = calloc((size_t)(n > 0 ? n : 1), sizeof(*drop));
    int kept = 0;
    if (drop == NULL) return n;
    for (i = 0; i < n; i++) {
      for (j = 0; j < n; j++) {
        const ProfShape *a, *b;
        int f, same = 1;
        if (i == j) continue;
        a = prof_shape_by_id(out[i].shape_id);
        b = prof_shape_by_id(out[j].shape_id);
        if (a == NULL || b == NULL) continue;
        if (a->proto != b->proto) continue;
        /* the same prototype: for a constructor-named one, the same constructor */
        if (a->proto == 1 && strcmp(a->digest, b->digest) != 0) continue;
        if (b->nfields <= a->nfields) continue;
        for (f = 0; f < a->nfields; f++)
          if (a->field[f] == NULL || b->field[f] == NULL ||
              strcmp(a->field[f], b->field[f]) != 0) {
            same = 0;
            break;
          }
        if (same) {
          drop[i] = 1;
          break;
        }
      }
    }
    for (i = 0; i < n; i++) {
      if (drop[i]) {
        nprefix++;
        continue;
      }
      out[kept++] = out[i];
    }
    n = kept;
    free(drop);
  }
  if (getenv("QJS_AOT_LAYOUT_SKIP") != NULL) {
    /* a bisecting knob: drop the layouts whose profile shape ids are listed (comma separated) */
    const char *sk = getenv("QJS_AOT_LAYOUT_SKIP");
    int kept2 = 0;
    for (i = 0; i < n; i++) {
      char key[16];
      const char *q;
      snprintf(key, sizeof(key), ",%d,", out[i].shape_id);
      {
        char hay[512];
        snprintf(hay, sizeof(hay), ",%s,", sk);
        q = strstr(hay, key);
      }
      if (q == NULL) out[kept2++] = out[i];
    }
    n = kept2;
  }
  if (getenv("QJS_AOT_LAYOUTCENSUS") != NULL) {
    for (i = 0; i < n; i++) fprintf(stderr, "layout kept: shape %d nfields %d\n", out[i].shape_id, out[i].nfields);
  }
  if (getenv("QJS_AOT_LAYOUTCENSUS") != NULL)
    fprintf(stderr, "layout census: kept=%d proto-refused=%d prefix-refused=%d empty=%d\n",
            n, nproto, nprefix, nempty);
  return n;
}

/* The contracted layout a property site reads, and the constant slot its field
   sits at in that layout. Only a site that saw exactly one shape and filled it as
   an own data read or write qualifies, and only when the shape's property list has no
   deleted slot, so the profiled slot is also the field's index in the layout the
   unit builds. The layout number is the id the unit stamps (the position in the
   candidate list plus one). */
static int g_slot_refused;
int qjs_aot_layout_slot_refusals(void) { return g_slot_refused; }
/* Whether the unit will carry a contracted layout for a class with these ordered fields (the region's init fails at run time for a class it has no layout for, and the whole run then pays the
   generic body): checked at generation. Any kept layout with a function prototype and exactly this field list. */
/* Whether the profiled shapes with exactly this ordered field list belong to a digest shared by another class (see region_layout_for): independent of which layouts the unit ends up carrying, so the dry
   pass can use it to avoid asking for layouts a region would never be allowed to use. */
int region_class_ambiguous(char (*names)[40], int nn) {
  int i, m, f;
  if (getenv("QJS_AOT_REGION_NOAMBIG") != NULL) return 0;
  /* Rows of one digest are built per class, against the closure the class's constant path names; what a row cannot tell apart is two profiled shapes of the same digest with the SAME ordered fields
     (two prototypes, one field list). Without a path (the region compiler knows no single ctor for the list) the digest's classes are ambiguous too. */
  {
    char p_[64];
    int has_path = region_class_path(names, nn, p_, sizeof(p_));
    int shared = 0;
    for (i = 0; i < g_nshapes; i++) {
      const ProfShape *sh = &g_shapes[i];
      if (sh->proto != 1 || sh->nfields != nn) continue;
      for (f = 0; f < nn && sh->field[f] != NULL && strcmp(sh->field[f], names[f]) == 0; f++) {}
      if (f != nn) continue;
      for (m = 0; m < g_nshapes; m++) {
        const ProfShape *o = &g_shapes[m];
        int k, same = 1;
        if (o->proto != 1 || o->nfields <= 0 || strcmp(o->digest, sh->digest) != 0 || o->id == sh->id) continue;
        {
          int n2 = o->nfields < nn ? o->nfields : nn;
          for (k = 0; k < n2; k++) if (o->field[k] == NULL || strcmp(o->field[k], names[k]) != 0) same = 0;
          if (!same) shared = 1;   /* a prefix or extension is the same class at another stage */
        }
      }
    }
    if (shared && !has_path) return 1;
  }
  return 0;
}

/* The profile's shapes of one class (see QjsAotClassShapes in fast.h). Candidates: shapes of the constructor's digest that no object uses as a prototype, with no deleted slot,
   whose first `nbase` fields are the base list and whose further fields are all among the names added late. Keyed by (prototype key, ordered field list). Shapes of more than one
   prototype key are ambiguous (the init cannot tell which prototype F.prototype will be): the table keeps only the base and says so. */
static int ms_cmp(const void *a, const void *b) {
  const ProfShape *x = *(const ProfShape *const *)a, *y = *(const ProfShape *const *)b;
  int k;
  if (x->nfields != y->nfields) return x->nfields < y->nfields ? -1 : 1;
  for (k = 0; k < x->nfields; k++) {
    int c = strcmp(x->field[k], y->field[k]);
    if (c != 0) return c;
  }
  return 0;
}
int qjs_aot_mshape_on(void) {
  const char *e = getenv("QJS_AOT_REGION_MSHAPE");
  return e == NULL || (e[0] != '\0' && strcmp(e, "0") != 0);
}
int qjs_aot_prof_class_shapes(const char *hex, char (*base)[40], int nbase, char (*late)[40], int nlate, QjsAotClassShapes *out) {
  const ProfShape *cand[64];
  int nc = 0, i, k, j, nuniq = 0, seen_digest = 0;
  unsigned long long pk = 0;
  memset(out, 0, sizeof(*out));
  out->n = 1;
  out->nn[0] = nbase;
  for (k = 0; k < nbase; k++) snprintf(out->names[0][k], 40, "%s", base[k]);
  out->parent[0] = -1;
  if (g_shapes == NULL || hex == NULL) { out->status = QJS_AOT_MS_NOPROF; return 0; }
  for (i = 0; i < g_nshapes; i++) {
    const ProfShape *sh = &g_shapes[i];
    int ok = 1, dup = 0;
    if (sh->proto != 1 || strcmp(sh->digest, hex) != 0 || sh->isproto) continue;
    seen_digest = 1;
    if (sh->nfields < nbase || sh->nprops != sh->nfields) continue;
    for (k = 0; k < sh->nfields && ok; k++) {
      if (sh->field[k] == NULL) { ok = 0; break; }
      if (k < nbase) { if (strcmp(sh->field[k], base[k]) != 0) ok = 0; }
      else {
        for (j = 0; j < nlate && strcmp(late[j], sh->field[k]) != 0; j++) {}
        if (j == nlate) ok = 0;
      }
    }
    if (!ok) continue;
    /* the base-length shape carries no class information (every class of the digest with this first field passes through it): only a shape that EXTENDS the base, by names this
       class is known to add late, says which prototype is meant */
    if (sh->nfields == nbase) continue;
    if (nuniq == 0) pk = sh->pk;
    else if (sh->pk != pk && getenv("QJS_AOT_REGION_MSHAPE_NOPK") == NULL) { out->status = QJS_AOT_MS_AMBIG; out->pk = pk; return 0; }
    nuniq++;
    for (j = 0; j < nc && !dup; j++) if (ms_cmp(&cand[j], &sh) == 0) dup = 1;
    if (dup) continue;
    if (nc >= 64) { out->status = QJS_AOT_MS_CAP; return 0; }
    cand[nc++] = sh;
  }
  out->pk = pk;
  if (!seen_digest) { out->status = QJS_AOT_MS_NOPROF; return 0; }
  qsort(cand, (size_t)nc, sizeof(cand[0]), ms_cmp);
  {
    const char *ce = getenv("QJS_AOT_REGION_MSHAPE_CAP");
    int cap = ce != NULL ? atoi(ce) : 16;
    if (cap < 2) cap = 16;
    if (cap > QJS_AOT_MAXSHAPES) cap = QJS_AOT_MAXSHAPES;
    if (nc > cap - 1) { out->status = QJS_AOT_MS_CAP; out->n = 1; return 0; }
  }
  for (i = 0; i < nc; i++) {
    const ProfShape *sh = cand[i];
    int t = out->n++;
    out->nn[t] = sh->nfields;
    for (k = 0; k < sh->nfields; k++) snprintf(out->names[t][k], 40, "%s", sh->field[k]);
    out->parent[t] = -1;
    for (j = 0; j < t; j++) {
      if (out->nn[j] != sh->nfields - 1) continue;
      for (k = 0; k < out->nn[j] && strcmp(out->names[j][k], sh->field[k]) == 0; k++) {}
      if (k == out->nn[j]) { out->parent[t] = j; snprintf(out->add[t], 40, "%s", sh->field[sh->nfields - 1]); break; }
    }
  }
  return out->n;
}
int region_layout_for(char (*names)[40], int nn) {
  static ELayoutInfo tab[E_LAYOUT_MAX];
  static int ntab = -1;
  int l, f;
  if (ntab < 0) ntab = e_layouts_collect(tab, E_LAYOUT_MAX);
  if (region_class_ambiguous(names, nn)) return 0;
  for (l = 0; l < ntab; l++) {
    const ProfShape *sh;
    if (tab[l].proto != 1 || tab[l].nfields != nn) continue;
    sh = prof_shape_by_id(tab[l].shape_id);
    if (sh == NULL) continue;
    for (f = 0; f < nn && sh->field[f] != NULL && strcmp(sh->field[f], names[f]) == 0; f++) {}
    if (f == nn) return 1;
  }
  return 0;
}
/* The id of the contracted layout whose objects the constructor `hex` builds (the layout's prototype is that function's), or 0 if there is none or
   several. */
int qjs_aot_layout_of_ctor(const char hex[33]) {
  static ELayoutInfo tab[E_LAYOUT_MAX];
  static int ntab = -1;
  int l, id = 0;
  if (ntab < 0) ntab = e_layouts_collect(tab, E_LAYOUT_MAX);
  for (l = 0; l < ntab; l++)
    if (tab[l].proto == 1 && strcmp(tab[l].digest, hex) == 0) {
      if (id != 0) return 0;
      id = l + 1;
    }
  return id;
}
int qjs_aot_layout_site(const uint8_t dg[16], int site, int *layout, int *slot) {
  static ELayoutInfo tab[E_LAYOUT_MAX];
  static int ntab = -1;
  const ProfEntry *e;
  int i, l;
  if (ntab < 0) ntab = e_layouts_collect(tab, E_LAYOUT_MAX);
  e = g_prof != NULL ? prof_entry(g_prof, g_nprof, dg) : NULL;
  if (e == NULL || !g_prof_sitekinds) return 0;
  for (i = 0; i < e->nsites; i++) {
    const ProfSite *ps = &e->sites[i];
    if (ps->site != site) continue;
    if (getenv("QJS_AOT_LAYOUT_WHY") != NULL && ps->nshapes >= 1 && (int)ps->shape[0] == atoi(getenv("QJS_AOT_LAYOUT_WHY")))
      fprintf(stderr, "layout site: site %d nshapes %d slot0 %d kinds %u count %u hits0 %u\n", site, ps->nshapes, ps->slot[0], ps->kinds, ps->count, ps->hits[0]);
    if (prof_store_ambiguous(e, (int)(ps - e->sites))) return 0;
    if (ps->nshapes != 1 || ps->slot[0] < 0 ||
        (ps->kinds != (1u << 1) && ps->kinds != (1u << 3)))
      return 0;
    for (l = 0; l < ntab; l++) {
      const ProfShape *sh;
      if (tab[l].shape_id != (int)ps->shape[0]) continue;
      sh = prof_shape_by_id(tab[l].shape_id);
      /* Two sources must agree: the slot the site's fill recorded and the property
         list of the shape table. A shape with a deleted slot, or a slot past the
         list, refuses the site. */
      if (sh == NULL || sh->nprops != sh->nfields || ps->slot[0] >= sh->nfields) {
        g_slot_refused++;
        return 0;
      }
      *layout = l + 1;
      *slot = ps->slot[0];
      return 1;
    }
    return 0;
  }
  return 0;
}

/* ---- slot claims -------------------------------------------------------
   What a contracted layout promises about the value in one of its slots, the last
   one included: 0 nothing, QJS_AOT_CLAIM_INT "an int", QJS_AOT_CLAIM_NOREF "a
   value that is not reference counted", QJS_AOT_CLAIM_OBJ(l) "an object of layout
   l, or null or undefined", QJS_AOT_CLAIM_OBJ_NN(l) "an object of layout l". The
   promise is derived from the heap walk and the recorded stores of the slot and
   from every read site of it: they must agree, and a read the profile saw
   produce anything else leaves the slot without one. The engine enforces it at
   every store and add (a store that breaks it sets the layout's broken bit), so
   a store the profile never saw only turns the dependent bodies to their
   generic form.

   The promises are emitted only while every property-value store in the engine
   is either hooked or listed with its reason (the audit run by
   `.local/specs/audit-slot-stores.sh`). */
#define QJS_AOT_SLOT_STORES_AUDITED 1
void qjs_aot_scan_init(JSContext *ctx, const JSAotGenFunc *funcs, int n);
int qjs_aot_scan_verdict(const char *name, int claim, const uint8_t own[16], const uint8_t (*ctors)[16], int nctors);
void qjs_aot_layterm_counts(int *dropped, int *kept);
static unsigned char g_claim_tab[E_LAYOUT_MAX][PROF_MAX_FIELDS];
static int g_claim_done, g_claim_any;

static void claims_derive(void) {
  static ELayoutInfo tab[E_LAYOUT_MAX];
  static uint8_t ctors[E_LAYOUT_MAX][16];
  int ntab, l, f;
  if (g_claim_done) return;
  g_claim_done = 1;
  memset(g_claim_tab, 0, sizeof(g_claim_tab));
  if (!QJS_AOT_SLOT_STORES_AUDITED || getenv("QJS_AOT_NOCLAIMS") != NULL) return;
  if (g_prof == NULL || !g_prof_sitekinds) return;
  ntab = e_layouts_collect(tab, E_LAYOUT_MAX);
  for (l = 0; l < ntab; l++)
    if (tab[l].proto == 1) hex_to_bytes16(tab[l].digest, ctors[l]); else memset(ctors[l], 0, 16);
  for (l = 0; l < ntab; l++) {
    const uint8_t *own = ctors[l];
    const ProfShape *sh = prof_shape_by_id(tab[l].shape_id);
    /* Only a constructor's layout: it is installed when the function object and its fresh prototype
       exist, so no instance can exist yet. An object-literal layout is built at the first bind. */
    if (sh == NULL || sh->nprops != sh->nfields || tab[l].proto != 1) continue;
    for (f = 0; f < sh->nfields; f++) {
      int claim = 0, k, j;
      unsigned ldty = 0;
      const ProfCont *ct = NULL;
      for (k = 0; k < g_ncont; k++)
        if (g_cont[k].shape == sh->id && g_cont[k].slot == f) { ct = &g_cont[k]; break; }
      /* What the loads of this slot saw, by pc: the union of the types the profile recorded at
         the defining instruction of every read site of the slot. */
      for (j = 0; j < g_nprof; j++) {
        const ProfEntry *e = &g_prof[j];
        for (k = 0; k < e->nsites; k++) {
          const ProfSite *ps = &e->sites[k];
          int m;
          if (ps->nshapes < 1 || ps->kinds != (1u << 1)) continue;
          for (m = 0; m < ps->nshapes; m++)
            if ((int)ps->shape[m] == sh->id && ps->slot[m] == f) {
              int q;
              for (q = 0; q < e->npcs; q++)
                if (e->pcs[q].pc == ps->pc) ldty |= e->pcs[q].types;
            }
        }
      }
      if (ct != NULL) {
        unsigned ty = ct->types;
        /* The heap walk and every load's record must agree. A fused load (a field read together with
           its receiver) shares one record between its two destinations, so an object in a record
           of a read of a number slot may be the receiver: that bit is not held against a number. */
        unsigned lnum = ldty & ~PT_OBJECT;
        if (ty == PT_INT && (lnum & ~PT_INT) == 0) {
          claim = QJS_AOT_CLAIM_INT;
        } else if ((ty & ~(PT_INT | PT_FLOAT | PT_BOOL | PT_UNDEF)) == 0 && (ty & (PT_INT | PT_FLOAT | PT_BOOL)) &&
                   (lnum & ~(PT_INT | PT_FLOAT | PT_BOOL | PT_UNDEF)) == 0) {
          claim = QJS_AOT_CLAIM_NOREF;
        }
        /* An object promise (QJS_AOT_CLAIM_OBJ / OBJ_NN) is not derived: measured on richards, the shape
           test it removes costs less than the store-side check it adds (+2.1M). */
      }
      {
        const char *nm = sh->field[f];
        int pclaim = claim, vd = 0;
        if (nm != NULL) {
          if (claim != 0) {
            vd = qjs_aot_scan_verdict(nm, claim, own, ctors, ntab);
            if (vd < 0) claim = 0;
          } else if (ct == NULL) {
            vd = qjs_aot_scan_verdict(nm, QJS_AOT_CLAIM_INT, own, ctors, ntab);
            if (vd == 1) claim = QJS_AOT_CLAIM_INT;
            else if ((vd = qjs_aot_scan_verdict(nm, QJS_AOT_CLAIM_NOREF, own, ctors, ntab)) == 1) claim = QJS_AOT_CLAIM_NOREF;
            else vd = 0;
          }
          if (getenv("QJS_AOT_SCANREPORT"))
            fprintf(stderr, "scan layout=%d slot=%d name=%s profile=%d verdict=%d final=%d%s heap-types=0x%x load-types=0x%x\n", l + 1, f, nm, pclaim, vd,
                    claim, pclaim != 0 && claim == 0 ? " CONTRADICTED-profile-promise" : "", ct != NULL ? ct->types : 0u, ldty);
        }
      }
      if (claim > 0) {
        g_claim_tab[l][f] = (unsigned char)claim;
        g_claim_any = 1;
      }
    }
  }
}

/* The promise for slot `slot` of contracted layout `layout` (1-based). */
int qjs_aot_layout_claim_of(int layout, int slot) {
  claims_derive();
  if (layout < 1 || layout > E_LAYOUT_MAX || slot < 0 || slot >= PROF_MAX_FIELDS) return 0;
  return g_claim_tab[layout - 1][slot];
}

int qjs_aot_layout_claims_any(void) {
  claims_derive();
  return g_claim_any;
}

/* Whether the run gives the profile shape `shape_id` a layout: it is a kept layout's shape, or it extends one with the same prototype (an
   instance of the layout that was given more fields after its construction). A group whose entry refuses "a shape that is a layout's"
   and whose own profile shape answers yes here refuses every entry: the generator's two views of the class disagree. */
/* Whether the profile shape `shape_id` IS a kept layout's shape (exactly). */
int qjs_aot_shape_in_layout_table(int shape_id) {
  static ELayoutInfo tab[E_LAYOUT_MAX];
  static int ntab = -1;
  int l;
  if (ntab < 0) ntab = e_layouts_collect(tab, E_LAYOUT_MAX);
  for (l = 0; l < ntab; l++)
    if (tab[l].shape_id == shape_id) return 1;
  return 0;
}

int qjs_aot_shape_in_layout_family(int shape_id) {
  static ELayoutInfo tab[E_LAYOUT_MAX];
  static int ntab = -1;
  const ProfShape *b = prof_shape_by_id(shape_id);
  int l, f;
  if (ntab < 0) ntab = e_layouts_collect(tab, E_LAYOUT_MAX);
  if (b == NULL) return 0;
  for (l = 0; l < ntab; l++) {
    const ProfShape *a = prof_shape_by_id(tab[l].shape_id);
    if (a == NULL || a->proto != b->proto || b->nfields < a->nfields) continue;
    if (a->proto == 1 && strcmp(a->digest, b->digest) != 0) continue;
    for (f = 0; f < a->nfields; f++)
      if (a->field[f] == NULL || b->field[f] == NULL || strcmp(a->field[f], b->field[f]) != 0) break;
    if (f == a->nfields) return 1;
  }
  return 0;
}

/* The layout census, for the report: the candidates the profile supports. */
int qjs_aot_layout_census(void) {
  ELayoutInfo tmp[E_LAYOUT_MAX];
  return e_layouts_collect(tmp, E_LAYOUT_MAX);
}

/* The same candidates, handed to the emitter. Two entry points rather than one so
   the census can report without the emitter having to run. */
int qjs_aot_layout_export(ELayoutInfo *out, int cap) {
  return e_layouts_collect(out, cap);
}

const void *qjs_aot_prof_shape(int id, int *proto, char *digest, int *nfields,
                               const char ***fields, unsigned **flags) {
  int i;
  if (g_shapes == NULL) return NULL;
  for (i = 0; i < g_nshapes; i++) {
    const ProfShape *sh = &g_shapes[i];
    if (sh->id != id) continue;
    if (proto) *proto = sh->proto;
    if (digest) {
      int k;
      for (k = 0; k < 32 && sh->digest[k]; k++) digest[k] = sh->digest[k];
      digest[k] = 0;
    }
    if (nfields) *nfields = sh->nfields;
    if (fields) *fields = (const char **)sh->field;
    if (flags) *flags = (unsigned *)sh->flags;
    return sh;
  }
  return NULL;
}

int qjs_aot_prof_site_slots(const uint8_t dg[16], int site, int nshapes, int *slots,
                            int cap) {
  const ProfEntry *e = g_prof ? prof_entry(g_prof, g_nprof, dg) : NULL;
  int i, k;
  if (!e) return 0;
  for (i = 0; i < e->nsites; i++) {
    if (e->sites[i].site != site) continue;
    if (e->sites[i].nshapes != nshapes) return 0;
    for (k = 0; k < nshapes && k < cap; k++) slots[k] = e->sites[i].slot[k];
    return 1;
  }
  return 0;
}

/* Whether a property site can be grouped: every fill installed an own readable
   slot or an own writable one, nothing else. A prototype read or an add-
   transition store can never hit an own slot at a fixed offset, so a group made
   of one would exit (or refuse) forever. Without `sitekinds` the profile cannot
   say, and no site qualifies. */
int qjs_aot_prof_site_own(const uint8_t dg[16], int site) {
  const ProfEntry *e = g_prof ? prof_entry(g_prof, g_nprof, dg) : NULL;
  int i;
  if (!e || !g_prof_sitekinds) return 0;
  for (i = 0; i < e->nsites; i++) {
    unsigned k;
    if (e->sites[i].site != site) continue;
    k = e->sites[i].kinds;
    /* bits: 1 = REG_PIC_OWN, 3 = REG_PIC_OWN_W; anything else disqualifies */
    return k != 0 && (k & ~((1u << 1) | (1u << 3))) == 0;
  }
  return 0;
}

/* Whether the profile has no record of the site at all: the code around it never ran, so a read
   there cannot be a hit and the body leaves at it. */
int qjs_aot_prof_site_cold(const uint8_t dg[16], int site) {
  const ProfEntry *e = g_prof ? prof_entry(g_prof, g_nprof, dg) : NULL;
  int i;
  if (!e || !g_prof_sitekinds) return 0;
  for (i = 0; i < e->nsites; i++)
    if (e->sites[i].site == site) return 0;
  return 1;
}

/* Whether every fill of a read site was a read from a holder two prototypes up. */
int qjs_aot_prof_site_proto2(const uint8_t dg[16], int site) {
  const ProfEntry *e = g_prof ? prof_entry(g_prof, g_nprof, dg) : NULL;
  int i;
  if (!e || !g_prof_sitekinds) return 0;
  for (i = 0; i < e->nsites; i++) {
    if (e->sites[i].site != site) continue;
    return e->sites[i].kinds == (1u << 5);
  }
  return 0;
}

/* Whether a read site's receivers were, for at least 5% of its executions, of a shape the site's cache never recorded: what the body guards on would not
   cover them. 0 when the profile has no receiver census for the site. */
int qjs_aot_prof_site_multirecv(const uint8_t dg[16], int site) {
  const ProfEntry *e = g_prof ? prof_entry(g_prof, g_nprof, dg) : NULL;
  int i, k, j;
  if (!e) return 0;
  for (i = 0; i < e->nsites; i++) {
    const ProfSite *ps = &e->sites[i];
    unsigned long long cov = 0;
    if (ps->site != site) continue;
    if (ps->rtotal == 0) return 0;
    for (k = 0; k < ps->nr; k++)
      for (j = 0; j < ps->nshapes; j++)
        if (ps->shape[j] == ps->rshape[k]) { cov += ps->rcount[k]; break; }
    return (ps->rtotal - (cov < ps->rtotal ? cov : ps->rtotal)) * 20 > ps->rtotal;
  }
  return 0;
}

/* Whether every fill of a read site was a read from the direct prototype. */
int qjs_aot_prof_site_proto(const uint8_t dg[16], int site) {
  const ProfEntry *e = g_prof ? prof_entry(g_prof, g_nprof, dg) : NULL;
  int i;
  if (!e || !g_prof_sitekinds) return 0;
  for (i = 0; i < e->nsites; i++) {
    if (e->sites[i].site != site) continue;
    return e->sites[i].kinds == (1u << 2);
  }
  return 0;
}

/* A read site whose recorded shapes (two or more, or a saturated set) all put the atom at one slot, and whose fills installed own slots:
   the slot, for a read by position. 0 when the profile says otherwise or says nothing. QJS_AOT_NO_ATOMSLOT=1 turns it off. */
int qjs_aot_prof_site_atom_slot(const uint8_t dg[16], int site, int *slot) {
  const ProfEntry *e = g_prof ? prof_entry(g_prof, g_nprof, dg) : NULL;
  int i, k;
  if (!e || !g_prof_sitekinds || getenv("QJS_AOT_NO_ATOMSLOT") != NULL) return 0;
  for (i = 0; i < e->nsites; i++) {
    const ProfSite *s = &e->sites[i];
    unsigned sum = 0;
    if (s->site != site) continue;
    if ((s->kinds & (1u << 1)) == 0 || s->nshapes < 1 || s->slot[0] < 0) return 0;
    for (k = 0; k < s->nshapes; k++) {
      if (s->slot[k] != s->slot[0]) return 0;
      sum += s->hits[k];
    }
    /* one shape that covers the site's fills is the cache's own case */
    if (s->nshapes < 2 && s->count <= sum) return 0;
    *slot = s->slot[0];
    return 1;
  }
  return 0;
}

/* Whether every fill of a read site installed an own slot or a prototype slot, the
   two kinds the inline read hit (q2_own_read_hit) serves: a prototype method read
   hits through the cell's holder and shape check. */
int qjs_aot_prof_site_read_hits(const uint8_t dg[16], int site) {
  const ProfEntry *e = g_prof ? prof_entry(g_prof, g_nprof, dg) : NULL;
  int i;
  if (!e || !g_prof_sitekinds) return 0;
  for (i = 0; i < e->nsites; i++) {
    unsigned k;
    if (e->sites[i].site != site) continue;
    k = e->sites[i].kinds;
    return k != 0 && (k & ~((1u << 1) | (1u << 2))) == 0;
  }
  return 0;
}

/* Whether the profiler never executed the instruction at `pc` (a `U` record).
   Such a block is emitted as an exit in a specialised body. */
/* Copies of a callee's never-run branches (cold copies) are OFF by default: their resume exits lose registers the generic continuation reads (an open old-backend defect, see
   vcases/o-cc-1level.js and o-coldcopy.js), which runs wrong or aborts the generator. QJS_AOT_COLDCOPY=1 turns them back on, for the investigation. */
int qjs_aot_coldcopy_off(void) {
  static int off = -1;
  if (off < 0) off = !(getenv("QJS_AOT_COLDCOPY") != NULL && getenv("QJS_AOT_NOCOLDCOPY") == NULL);
  return off;
}
int qjs_aot_prof_unexecuted(const uint8_t dg[16], int pc) {
  const ProfEntry *e = g_prof ? prof_entry(g_prof, g_nprof, dg) : NULL;
  int i;
  if (!e) return 0;
  for (i = 0; i < e->nunused; i++)
    if (pc >= e->unused[i].lo && pc <= e->unused[i].hi) return 1;
  return 0;
}

/* Every callee one call site saw, up to PROF_MAX_CALLEES, for polymorphic call
   dispatch. Returns the count (0 when the function or pc has no call record)
   and fills the parallel arrays; `hex[k]` is 33 bytes and is empty for a C or
   unknown callee. `hits[k]` is that callee's hit count at the site. */
int qjs_aot_prof_callees(
    const uint8_t dg[16], int pc, int *n, char hex[4][33], int kind[4],
    unsigned long long hits[4]) {
  const ProfEntry *e = g_prof ? prof_entry(g_prof, g_nprof, dg) : NULL;
  int i, k;
  *n = 0;
  if (!e) return 0;
  for (i = 0; i < e->ncallsites; i++) {
    if (e->callsites[i].pc != pc) continue;
    *n = e->callsites[i].n;
    for (k = 0; k < *n && k < PROF_MAX_CALLEES; k++) {
      kind[k] = e->callsites[i].c[k].kind;
      hits[k] = e->callsites[i].c[k].hits;
      if (hex) {
        memcpy(hex[k], e->callsites[i].c[k].digest, 33);
        hex[k][32] = 0;
      }
    }
    /* Hottest first: a site tests its candidates in this order. */
    for (k = 1; k < *n && k < PROF_MAX_CALLEES; k++) {
      int j = k;
      while (j > 0 && hits[j] > hits[j - 1]) {
        unsigned long long th = hits[j];
        int tk = kind[j];
        char tx[33];
        hits[j] = hits[j - 1];
        hits[j - 1] = th;
        kind[j] = kind[j - 1];
        kind[j - 1] = tk;
        if (hex) {
          memcpy(tx, hex[j], 33);
          memcpy(hex[j], hex[j - 1], 33);
          memcpy(hex[j - 1], tx, 33);
        }
        j--;
      }
    }
    return *n;
  }
  return 0;
}

/* Load a profile for the verify/dump/stats modes (the unit-builder mode loads
   its own from its own option parsing). */
int qjs_aot_prof_load(const char *path) {
  ProfEntry *prof = parse_profile(path, &g_nprof);
  if (!prof) return -1;
  g_prof = prof;
  return 0;
}

/* Whether the unit emits a body for this digest. The emitter asks before
   lowering a call to it as a direct call: only then does the callee have an
   `aot_f_` this translation unit can name. */
/* The two kinds a constant-pool entry can fold as. JS_TAG_INT is 0 in the
   engine, so these are their own values rather than the tag values. */
enum { QJS_AOT_POOL_INT = 1, QJS_AOT_POOL_DOUBLE = 2 };

int qjs_aot_pool_num(const JSAotGenFunc *g, uint32_t idx, double *out) {
  if (!g || idx >= (uint32_t)g->cpool_count) return 0;
  if (!g->cpool_numeric || !g->cpool_numeric[idx]) return 0;
  *out = g->cpool_num[idx];
  return 1;
}

/* The text of constant-pool entry `idx` when it is a short plain string, else NULL. "" is "unknown" in the engine's table (a number, a function and
   the empty string all read ""), so an empty answer is NULL too: a caller never mistakes a non-string entry for a string. */
const char *qjs_aot_pool_str(const JSAotGenFunc *g, uint32_t idx) {
  if (!g || !g->cpool_str || idx >= (uint32_t)g->cpool_count) return NULL;
  return g->cpool_str[idx][0] != 0 ? g->cpool_str[idx] : NULL;
}

/* The entry's tag when it is a foldable number (JS_TAG_INT or
   JS_TAG_FLOAT64), else 0. The generator needs it to pick an int literal or a
   double literal; `qjs_aot_pool_num` alone cannot say, because it returns the
   value already widened to double. */
int qjs_aot_pool_tag(const JSAotGenFunc *g, uint32_t idx) {
  if (!g || idx >= (uint32_t)g->cpool_count) return 0;
  if (!g->cpool_numeric) return 0;
  /* JS_TAG_INT is 0, which would be indistinguishable from "not numeric", so
     the two foldable kinds are reported as their own small codes. */
  if (g->cpool_numeric[idx] == (uint8_t)JS_TAG_INT) return QJS_AOT_POOL_INT;
  if (g->cpool_numeric[idx] == (uint8_t)JS_TAG_FLOAT64)
    return QJS_AOT_POOL_DOUBLE;
  return 0;
}

/* The light-frame size of the unit function with this digest, when a compiled
   caller can enter it directly: 0 when it cannot. */
int qjs_aot_light_need(const char hex[33]) {
  uint8_t dg[16];
  int i;

  if (!g_unit_funcs || hex_to_bytes16(hex, dg) < 0) return 0;
  for (i = 0; i < g_unit_nfuncs; i++)
    if (memcmp(g_unit_funcs[i].digest, dg, 16) == 0)
      return g_unit_funcs[i].eligible == AOT_GEN_ELIGIBLE &&
                     g_unit_funcs[i].light_ok
                 ? g_unit_funcs[i].light_need
                 : 0;
  return 0;
}

/* Whether a compiled caller may call this unit function on the C stack: a light
   function, or one the engine does not call light (it reads closure variables) but
   that a frameless body can serve: a plain function with no control records and no
   variable area. The final answer is the `Q2_NATIVE_` macro, defined only when a
   native entry was emitted. */
int e_vargs_capable(const JSAotGenFunc *g);
int qjs_aot_native_capable(const char hex[33]) {
  uint8_t dg[16];
  int i;

  if (!g_unit_funcs || hex_to_bytes16(hex, dg) < 0) return 0;
  for (i = 0; i < g_unit_nfuncs; i++)
    if (memcmp(g_unit_funcs[i].digest, dg, 16) == 0) {
      const JSAotGenFunc *g = &g_unit_funcs[i];
      if (g->eligible != AOT_GEN_ELIGIBLE) return 0;
      if (getenv("QJS_AOT_WHYNAT") != NULL)
        fprintf(stderr, "native? %s elig=%d light_ok=%d need=%d kind=%d ctl=%d lvc=%d\n", hex,
                (int)g->eligible, (int)g->light_ok, (int)g->light_need, (int)g->func_kind,
                (int)g->ctl_count, (int)g->light_var_count);
      if (g->light_ok) return g->light_need > 0 && g->light_need <= E_NATIVE_FRAME_MAX;
      return g->func_kind == 0 && g->ctl_count == 0 && (g->light_var_count == 0 || e_vargs_capable(g));
    }
  return 0;
}

/* The register-program length in bytes of the unit function with this digest, or -1 when it is not in the unit (diagnostics). */
int qjs_aot_unit_len(const char hex[33]) {
  uint8_t dg[16];
  int i;
  if (!g_unit_funcs || hex_to_bytes16(hex, dg) < 0) return -1;
  for (i = 0; i < g_unit_nfuncs; i++)
    if (memcmp(g_unit_funcs[i].digest, dg, 16) == 0) return g_unit_funcs[i].len;
  return -1;
}

/* The declared argument count of the unit function with this digest, or -1 when
   it is not in the unit. A direct call must supply at least this many: the
   callee's frame publishes `argc` and its body reads its parameters from the
   argument window, so a short window would read a register the caller never
   wrote. */
int qjs_aot_unit_argc(const char hex[33]) {
  uint8_t dg[16];
  int i;

  if (!g_unit_funcs || hex_to_bytes16(hex, dg) < 0) return -1;
  for (i = 0; i < g_unit_nfuncs; i++)
    if (memcmp(g_unit_funcs[i].digest, dg, 16) == 0)
      return g_unit_funcs[i].eligible == AOT_GEN_ELIGIBLE
                 ? g_unit_funcs[i].arg_count
                 : -1;
  return -1;
}

/* Whether the unit function with this digest is a constructor wrapper: `function () { this.X.apply(this, arguments); }`,
   recognised by its register program, not by names. The program is compared instruction by instruction with the one
   the compiler makes for it, operands included except the atoms and the cache sites. On a match `*init_off` and
   `*apply_off` are the byte offsets, in that function's own program, of the atom operands of the two reads (`this.X` and
   `.apply`): atoms are runtime values, so the generated code hands the offsets to the engine, which loads the atoms
   from the callee's live code, requires the second to be `apply` and reads the first.
   Two programs are known. The register allocator keeps `this` in a register and the two reads are one `get_field_chain`
   (both atoms in one instruction); the older form stores `this` into local slot 0 and reads `X` from the slot. */
_Static_assert(ROP_local_uninit == 22 && ROP_load_this == 9 && ROP_get_local_field_0 == 145 && ROP_get_field2 == 39 &&
                   ROP_move_s == 217 && ROP_move_xfer == 141 && ROP_apply_arguments == 79 && ROP_return_undefined == 68,
               "the wrapper reference program names its opcodes by number");
int qjs_aot_wrapper_shape(const char hex[33], int *init_off, int *apply_off) {
  /* op, then the register operands in order (byte offsets) or -1 for an operand that is not compared;
     ia / aa: the index of the instruction holding the `X` atom and the `apply` atom, and which atom operand of it (0 first) */
  typedef struct { int op; int n; int64_t v[4]; } Step;
  static const struct { int len; Step ref[10]; int ia, ian, aa, aan; } prog[2] = {
      {8,
       {
           {ROP_local_uninit, 1, {1}},          /* the `arguments` slot */
           {ROP_load_this, 1, {0}},             /* r0 = this */
           {ROP_get_field_chain, 4, {16, 0, 32, 16}}, /* r1 = r0.X (atom, site not compared); r2 = r1.apply (atom, site) */
           {ROP_move_s, 2, {48, 16}},           /* r3 = r1 */
           {ROP_move_xfer, 2, {64, 32}},        /* r4 = r2 */
           {ROP_move_s, 2, {80, 0}},            /* r5 = this */
           {ROP_apply_arguments, 3, {48, 16, 1}}, /* window r3, dest r1, arguments slot 1 */
           {ROP_return_undefined, 0, {0}},
       },
       2, 0, 2, 1},
      {10,
       {
           {ROP_local_uninit, 1, {1}},          /* the `this` slot */
           {ROP_load_this, 1, {0}},
           {153, 1, {0}},                       /* store r0 into the slot */
           {ROP_get_local_field_0, 1, {16}},    /* r1 = slot0.X (atom, site not compared) */
           {ROP_get_field2, 2, {32, 16}},       /* r2 = r1.apply (atom, site not compared) */
           {ROP_move_s, 2, {48, 16}},
           {ROP_move_xfer, 2, {64, 32}},
           {ROP_move_s, 2, {80, 0}},
           {ROP_apply_arguments, 3, {48, 16, 1}},
           {ROP_return_undefined, 0, {0}},
       },
       3, 0, 4, 0},
  };
  uint8_t dg[16];
  int i, pi;

  if (!g_unit_funcs || hex_to_bytes16(hex, dg) < 0) return 0;
  for (i = 0; i < g_unit_nfuncs; i++)
    if (memcmp(g_unit_funcs[i].digest, dg, 16) == 0) {
      const JSAotGenFunc *g = &g_unit_funcs[i];
      if (g->eligible != AOT_GEN_ELIGIBLE || g->arg_count != 0 || g->len > 128) return 0;
      for (pi = 0; pi < 2; pi++) {
        int off = 0, n = 0, a, ia = -1, aa = -1, ok = 1;
        JSAotGenInsn in;
        while (off < g->len && ok) {
          int ri = 0, na = 0;
          if (n >= prog[pi].len || js_aot_gen_decode_insn(g->code, g->len, off, &in) < 0 ||
              (in.op != prog[pi].ref[n].op &&
               !(prog[pi].ref[n].op == ROP_get_field_chain && in.op == ROP_get_field_chain_own))) { ok = 0; break; }
          for (a = 0; a < in.nargs; a++) {
            if (in.arg[a].kind == 'a') {
              if (n == prog[pi].ia && na == prog[pi].ian) ia = in.arg[a].off;
              if (n == prog[pi].aa && na == prog[pi].aan) aa = in.arg[a].off;
              na++;
              continue;
            }
            if (in.arg[a].kind == 's') continue;
            if (ri >= prog[pi].ref[n].n || in.arg[a].val != prog[pi].ref[n].v[ri]) { ok = 0; break; }
            ri++;
          }
          if (ok && ri != prog[pi].ref[n].n) ok = 0;
          off += in.size;
          n++;
        }
        if (!ok || n != prog[pi].len || ia < 0 || aa < 0) continue;
        *init_off = ia;
        *apply_off = aa;
        return 1;
      }
      return 0;
    }
  return 0;
}

/* Whether the profiled run held two or more live bytecodes of this code. A call to it can reach the one the unit binds or its twin, which runs in the
   interpreter: a body must not be copied into a caller, nor a method cell pinned to it. */
int qjs_aot_prof_has_twin_hex(const char hex[33]) {
  uint8_t dg[16];
  const ProfEntry *e;
  if (getenv("QJS_AOT_TWINS_OK") != NULL || hex_to_bytes16(hex, dg) < 0) return 0;
  e = g_prof ? prof_entry(g_prof, g_nprof, dg) : NULL;
  return e != NULL && e->nrec > 1;
}

int qjs_aot_unit_has(const char hex[33]) {
  uint8_t dg[16];
  int i;

  if (!g_unit_funcs || hex_to_bytes16(hex, dg) < 0) return 0;
  for (i = 0; i < g_unit_nfuncs; i++)
    if (memcmp(g_unit_funcs[i].digest, dg, 16) == 0)
      return g_unit_funcs[i].eligible == AOT_GEN_ELIGIBLE;
  return 0;
}

/* --- selection ------------------------------------------------------------
   The generator measures each function's emitted body once, then chooses a
   subset: rank by profiled ops, take the prefix covering HOT_COVER of the
   profiled ops, drop startup-only entries below MIN_ENTRY_OPS, close over
   compiled callees, and finally drop the lowest ops/byte until the
   estimated body bytes fit the budget. `--all` bypasses selection. */

typedef struct {
  char *buf; /* memstream body, or NULL when the function failed */
  size_t len;
} EmitBuf;

/* The inline forms (`ei_HEX`) of the unit's small leaves, by unit index. */
static EmitBuf *g_inl_eb;
/* The born-dead nursery: QJS_AOT_NURSERY=0 turns the young-cell forms off in a unit; the default is on. Young cells are made only by a fused
   construct site (its `static Q2Fz fz_` cache), so a unit whose emitted bodies have none never sees a young cell and carries the ordinary
   counted forms: every store and release would otherwise pay the young-cell test for nothing. The decision is taken when the unit's prologue
   is written, once every body is out, and checked again after the bodies are written. */
static int g_nursery_decided = -1;
static int e_bodies_have_fused_site(const EmitBuf *eb, const EmitBuf *inl, const char *sel, int n) {
  int i;
  for (i = 0; i < n; i++) {
    if (!sel[i]) continue;
    if (eb[i].buf && eb[i].len && memmem(eb[i].buf, eb[i].len, "static Q2Fz fz_", 15) != NULL) return 1;
    if (inl && inl[i].buf && inl[i].len && memmem(inl[i].buf, inl[i].len, "static Q2Fz fz_", 15) != NULL) return 1;
  }
  return 0;
}
static void e_nursery_decide(const EmitBuf *eb, const EmitBuf *inl, const char *sel, int n) {
  const char *e = getenv("QJS_AOT_NURSERY");
  g_nursery_decided = e != NULL && *e == '1' ? 1 : (e == NULL || *e != '0') && e_bodies_have_fused_site(eb, inl, sel, n);
}
static int e_nursery(void) { return g_nursery_decided > 0; }
static uint8_t *g_inl_nx; /* the inline body exits through aot_nx_ */
static uint8_t *g_inl_leaf; /* the inline body makes no call and no construct */
static uint8_t *g_inl_tl;  /* the layout its inline form tests `this` for at entry */
int e_inline_thislay(void);
static int *g_inl_rz;       /* the slots its inline form takes in the shared root array */
int e_inline_rz(void);
static uint8_t *g_inl_try;  /* 1: the inline form was emitted in some sweep, 2: it was refused */
static uint8_t *g_inl_lvl;  /* the sweep that committed the inline form */

/* Which functions this run's emitter gave a native entry. A call site may
   only name an `aot_n_` that some body in this unit defines, and only a
   light-eligible function gets one. Keyed by unit index. */
static uint8_t *g_e_native;

/* The register file each body was compiled for, keyed by unit index: the
   widest register the emitted code can touch. It is published in the unit's
   `qjs_aot_table`, because the engine scans and sizes a light frame from it --
   the program's own `regvm_reg_count` does not cover the registers allocated
   above it (a web's argument window, a hoisted load). */
static int *g_e_frameregs;

/* The emitter's hook, so the unit builder knows which bodies it produced. */
void qjs_aot_note_native_entry(int index) {
  if (g_e_native && index >= 0) g_e_native[index] = 1;
}

/* A compiled caller that leaves a copy of this function in the middle asks for the function's resume entry
   (aot_rx_HEX, emitted with the function's body). Returns 0 when the function is not in the unit. */
static uint8_t *g_rx_need;
void qjs_aot_region_dep(const char root_hex[33], const char callee_hex[33]) {
  uint8_t a[16], b[16];
  int i;
  if (hex_to_bytes16(root_hex, a) < 0 || hex_to_bytes16(callee_hex, b) < 0) return;
  for (i = 0; i < g_rgdep_n; i++)
    if (memcmp(g_rgdep[i].root, a, 16) == 0 && memcmp(g_rgdep[i].callee, b, 16) == 0) return;
  if (g_rgdep_n >= g_rgdep_cap) {
    g_rgdep_cap = g_rgdep_cap ? g_rgdep_cap * 2 : 64;
    g_rgdep = realloc(g_rgdep, (size_t)g_rgdep_cap * sizeof(*g_rgdep));
    if (g_rgdep == NULL) abort();
  }
  memcpy(g_rgdep[g_rgdep_n].root, a, 16);
  memcpy(g_rgdep[g_rgdep_n].callee, b, 16);
  g_rgdep_n++;
}

int qjs_aot_rx_request(const char hex[33]) {
  uint8_t dg[16];
  int i;

  if (!g_unit_funcs || hex_to_bytes16(hex, dg) < 0) return 0;
  if (g_rx_need == NULL) g_rx_need = calloc((size_t)(g_unit_nfuncs > 0 ? g_unit_nfuncs : 1), 1);
  if (g_rx_need == NULL) return 0;
  /* every function of this digest: byte-identical functions share one, and the one the unit emits need not be the first listed (QJS_AOT_DUPRX_BUG=1 restores the old first-only mark, for the vkit knock) */
  {
    int found = 0;
    for (i = 0; i < g_unit_nfuncs; i++)
      if (memcmp(g_unit_funcs[i].digest, dg, 16) == 0) {
        g_rx_need[i] = 1;
        found = 1;
        if (getenv("QJS_AOT_DUPRX_BUG") != NULL) return 1;
      }
    return found;
  }
}

void qjs_aot_note_frame_regs(int index, int nregs) {
  if (g_e_frameregs == NULL || index < 0 || nregs <= 0) return;
  if (nregs > g_e_frameregs[index]) g_e_frameregs[index] = nregs;
}

int qjs_aot_frame_regs(int index) {
  if (index < 0) return 0;
  /* without a generic body the interpreter finishes the activation in this frame: it needs every register of the function */
  if (e_nogeneric() && index < g_unit_nfuncs) {
    int n = g_unit_funcs[index].reg_count;
    if (g_e_frameregs != NULL && g_e_frameregs[index] > n) n = g_e_frameregs[index];
    return n;
  }
  if (g_e_frameregs != NULL && g_e_frameregs[index] > 0) return g_e_frameregs[index];
  return index < g_unit_nfuncs ? g_unit_funcs[index].reg_count : 0;
}

/* The registers a frame of this function needs when the interpreter finishes it: the program's own count, or the wider count the unit compiled it with. */
int qjs_aot_regs_of_gen(const JSAotGenFunc *g) {
  int i, n = g->reg_count;
  for (i = 0; i < g_unit_nfuncs; i++)
    if (memcmp(g_unit_funcs[i].digest, g->digest, 16) == 0) {
      int w = qjs_aot_frame_regs(i);
      if (w > n) n = w;
      break;
    }
  return n;
}

/* The unit index of the function being emitted, for the hook above. */
int qjs_aot_cur_func_idx(void) {
  return g_cur_func_idx;
}

int qjs_aot_has_native_entry(int index) {
  return g_e_native && index >= 0 ? g_e_native[index] : 0;
}

/* The inline forms are built for the last of the entries that share a digest, so the lookups
   name that entry. */
static int inl_last_index(const uint8_t dg[16]) {
  int i, r = -1;

  for (i = 0; i < g_unit_nfuncs; i++)
    if (memcmp(g_unit_funcs[i].digest, dg, 16) == 0) r = i;
  return r;
}

/* Whether the unit function with this digest has an inline form a caller can run
   in place of a call. */
int qjs_aot_inline_has(const char hex[33]) {
  uint8_t dg[16];
  int i;

  if (!g_unit_funcs || !g_inl_eb || hex_to_bytes16(hex, dg) < 0) return 0;
  i = inl_last_index(dg);
  return i >= 0 && g_unit_funcs[i].eligible == AOT_GEN_ELIGIBLE && g_inl_eb[i].buf;
}

/* The program of the unit function with this digest, or NULL. */
const JSAotGenFunc *qjs_aot_unit_func(const char hex[33]) {
  uint8_t dg[16];
  int i;

  if (!g_unit_funcs || hex_to_bytes16(hex, dg) < 0) return NULL;
  for (i = 0; i < g_unit_nfuncs; i++)
    if (memcmp(g_unit_funcs[i].digest, dg, 16) == 0 && g_unit_funcs[i].eligible == AOT_GEN_ELIGIBLE)
      return &g_unit_funcs[i];
  return NULL;
}

/* The slots the inline form of the unit function with this digest takes in the shared root
   array of the frame that runs it. */
int qjs_aot_inline_rz(const char hex[33]) {
  uint8_t dg[16];
  int i;

  if (!g_unit_funcs || !g_inl_rz || hex_to_bytes16(hex, dg) < 0) return 0;
  i = inl_last_index(dg);
  return i >= 0 ? g_inl_rz[i] : 0;
}

/* The layout the inline form of the unit function with this digest tests `this` for at its
   entry, or 0. */
int qjs_aot_inline_thislay(const char hex[33]) {
  uint8_t dg[16];
  int i;

  if (!g_unit_funcs || !g_inl_tl || hex_to_bytes16(hex, dg) < 0) return 0;
  i = inl_last_index(dg);
  return i >= 0 ? g_inl_tl[i] : 0;
}

/* Whether the inline form of the unit function with this digest has no safepoint. */
int qjs_aot_inline_leaf(const char hex[33]) {
  uint8_t dg[16];
  int i;

  if (!g_unit_funcs || !g_inl_leaf || hex_to_bytes16(hex, dg) < 0) return 0;
  i = inl_last_index(dg);
  return i >= 0 ? g_inl_leaf[i] : 0;
}

/* Whether the unit function with this digest may still get an inline form: not committed
   yet and not refused. A body that calls such a function waits for it. */
int qjs_aot_inline_pending(const char hex[33]) {
  uint8_t dg[16];
  int i;

  if (!g_unit_funcs || !g_inl_eb || !g_inl_try || hex_to_bytes16(hex, dg) < 0) return 0;
  i = inl_last_index(dg);
  return i >= 0 && g_unit_funcs[i].eligible == AOT_GEN_ELIGIBLE && !g_inl_eb[i].buf &&
         g_inl_try[i] != 2;
}

int qjs_aot_inline_uses_nx(const char hex[33]) {
  uint8_t dg[16];
  int i;

  if (!g_unit_funcs || !g_inl_nx || hex_to_bytes16(hex, dg) < 0) return 0;
  i = inl_last_index(dg);
  return i >= 0 ? g_inl_nx[i] : 0;
}

/* A function's share of the profile's executed ops, in percent: the report's
   `weight`. Without a profile every function is 0 %, which is honest — nothing
   was measured, so nothing is known to matter. */
double qjs_aot_prof_weight(const uint8_t dg[16]) {
  int i;
  unsigned long long mine = 0, all = 0;

  if (!g_prof || g_nprof == 0) return 0.0;
  for (i = 0; i < g_nprof; i++) {
    all += g_prof[i].ops;
    if (memcmp(g_prof[i].digest, dg, 16) == 0) mine = g_prof[i].ops;
  }
  return all ? 100.0 * (double)mine / (double)all : 0.0;
}

static unsigned long long prof_ops_of(
    const ProfEntry *prof, int nprof, const uint8_t dg[16]) {
  int i;
  for (i = 0; i < nprof; i++)
    if (memcmp(prof[i].digest, dg, 16) == 0) return prof[i].ops;
  return 0;
}

static const ProfEntry *prof_entry_of(
    const ProfEntry *prof, int nprof, const uint8_t dg[16]) {
  int i;
  for (i = 0; i < nprof; i++)
    if (memcmp(prof[i].digest, dg, 16) == 0) return &prof[i];
  return NULL;
}

/* The unique-body representative of a digest (the last index of its sorted
   run, matching the emit loop's dedupe), or -1. funcs is digest-sorted. */
static int find_unit(
    const JSAotGenFunc *funcs, int nfuncs, const uint8_t dg[16]) {
  int i, found = -1;
  for (i = 0; i < nfuncs; i++) {
    int c = memcmp(funcs[i].digest, dg, 16);
    if (c == 0) {
      if (funcs[i].eligible == AOT_GEN_ELIGIBLE) found = i;
    } else if (c > 0) {
      break;
    }
  }
  return found;
}

/* `--self-weights FILE`: lines `<digest hex> <n>`, the samples a function spent in the interpreter's own code (tools/aot/selfweights.py). When
   given, the hot-cover ranking and its cumulative target use these instead of the profile's op counts, so a function that spends its time in
   native builtins is not picked for the work it does not do itself. */
static const char *g_self_weights;
static unsigned long long self_weight_of(const char *path, const uint8_t dg[16]) {
  static struct { uint8_t dg[16]; unsigned long long n; } *tab;
  static int ntab, loaded;
  int i;
  if (!loaded) {
    FILE *f = fopen(path, "r");
    char line[128];
    int cap = 0;
    loaded = 1;
    if (!f) { fprintf(stderr, "qjsaot: cannot read %s\n", path); exit(2); }
    while (fgets(line, sizeof line, f)) {
      char hx[40];
      unsigned long long n;
      uint8_t d[16];
      if (sscanf(line, "%32s %llu", hx, &n) != 2 || hex_to_bytes16(hx, d) < 0) continue;
      if (ntab == cap) { cap = cap ? cap * 2 : 256; tab = realloc(tab, (size_t)cap * sizeof(*tab)); }
      memcpy(tab[ntab].dg, d, 16);
      tab[ntab].n = n;
      ntab++;
    }
    fclose(f);
  }
  for (i = 0; i < ntab; i++)
    if (memcmp(tab[i].dg, dg, 16) == 0) return tab[i].n;
  return 0;
}

/* Compare for a descending-ops sort of unit indices. */
static const unsigned long long *g_sort_ops;
static int unit_ops_cmp(const void *a, const void *b) {
  int ia = *(const int *)a, ib = *(const int *)b;
  if (g_sort_ops[ia] != g_sort_ops[ib])
    return g_sort_ops[ia] < g_sort_ops[ib] ? 1 : -1;
  return ia < ib ? -1 : (ia > ib ? 1 : 0);
}

/* The statistics a unit prints at exit, shared by the single unit and the split unit's runtime part (both put it inside a format
   string, hence the doubled percent signs). The per-function lines, the exit sites the unit's guards took (kind, pc, the op at the
   pc) and the exit tally; the gate report names the op and the reason of each exit that switched a function off, and lists the
   interpreter activations of functions the unit holds a body for. */
#define E_UNIT_STATS_TAIL \
  "  for (i = 0; i < QJS_AOT_NFUNCS; i++) {\n" \
  "    if (e_ent[i] == 0 && e_ref[i] == 0 && e_exi[i] == 0 && e_han[i] == 0 && e_inl[i] == 0) continue;\n" \
  "    fprintf(stderr, \"unit-stats fn=%%s enters=%%llu exits=%%llu handoffs=%%llu refuses=%%llu off=%%u idx=%%d frameless=%%llu inlined=%%llu\\n\",\n" \
  "            qjs_aot_e_names[i] ? qjs_aot_e_names[i] : \"?\",\n" \
  "            (unsigned long long)e_ent[i], (unsigned long long)e_exi[i],\n" \
  "            (unsigned long long)e_han[i], (unsigned long long)e_ref[i],\n" \
  "            (unsigned)e_off[i], i, e_nfl[i] ? (unsigned long long)e_ent[i] : 0ull,\n            (unsigned long long)e_inl[i]);\n" \
  "  }\n" \
  "  fprintf(stderr, \"unit-stats entries-counted=%%d\\n\", E_ENT_COUNTED);\n" \
  "  { const E_XNode *n_; for (n_ = e_xhead; n_ != NULL; n_ = n_->next)\n" \
  "      fprintf(stderr, \"unit-exit-site fn=%%s kind=%%s pc=%%d count=%%u op=%%s shapes=%%u\\n\", n_->hex, n_->kind, n_->pc, n_->cnt, n_->op ? n_->op : \"?\", n_->nsh); }\n" \
  "  { int k_; for (k_ = 0; k_ < E_XT_N; k_++) if (e_xt[k_].cnt)\n" \
  "      fprintf(stderr, \"unit-xt fi=%%d pc=%%d count=%%u hex=%%s op=%%s reason=%%s\\n\", e_xt[k_].fi, e_xt[k_].pc, e_xt[k_].cnt, e_xt[k_].hex ? e_xt[k_].hex : \"?\", e_xt[k_].op ? e_xt[k_].op : \"?\", e_xt[k_].kind ? e_xt[k_].kind : \"?\"); }\n"
#define E_GATE_REPORT_BODY \
  "  int i, k;\n" \
  "  if (getenv(\"QJS_AOT_STATS\") == NULL) return;\n" \
  "  for (i = 0; i < QJS_AOT_NFUNCS; i++) {\n" \
  "    const E_XT *b_ = NULL;\n" \
  "    if (!e_gwhy[i]) continue;\n" \
  "    for (k = 0; k < E_XT_N; k++) if (e_xt[k].cnt && e_xt[k].fi == i && (b_ == NULL || e_xt[k].cnt > b_->cnt)) b_ = &e_xt[k];\n" \
  "    fprintf(stderr, \"aot gated fn=%%s idx=%%d enters=%%llu inl-enters=%%llu entries-counted=%%d exits=%%u refuses=%%u exit-op=%%s exit-pc=%%d reason=%%s count=%%u hex=%%s\\n\", qjs_aot_e_names[i] ? qjs_aot_e_names[i] : \"?\", i,\n" \
  "            (unsigned long long)e_ent[i], (unsigned long long)e_inl[i], E_ENT_COUNTED, e_exi[i], e_ref[i],\n" \
  "            b_ && b_->op ? b_->op : \"?\", b_ ? b_->pc : -1, b_ && b_->kind ? b_->kind : \"?\", b_ ? b_->cnt : 0u, b_ && b_->hex ? b_->hex : \"?\");\n" \
  "    for (k = 0; k < E_XT_N; k++) if (e_xt[k].cnt && e_xt[k].fi == i)\n" \
  "      fprintf(stderr, \"aot gated-exit fn=%%s idx=%%d pc=%%d op=%%s reason=%%s count=%%u\\n\", qjs_aot_e_names[i] ? qjs_aot_e_names[i] : \"?\", i, e_xt[k].pc, e_xt[k].op ? e_xt[k].op : \"?\", e_xt[k].kind ? e_xt[k].kind : \"?\", e_xt[k].cnt);\n" \
  "  }\n" \
  "  fprintf(stderr, \"aot gated=%%u\\n\", e_ngated);\n" \
  "  { unsigned nic_ = 0; const uint64_t *ic_ = &qjs_aot_icount_table != NULL ? qjs_aot_icount_table(&nic_) : NULL;\n" \
  "    if (ic_ != NULL) for (k = 0; (unsigned)k < nic_; k++) if (ic_[k] != 0 || ic_[nic_ + (unsigned)k] != 0) {\n" \
  "      i = qjs_aot_tab_fi[k];\n" \
  "      if (i < 0 || i >= QJS_AOT_NFUNCS) continue;\n" \
  "      fprintf(stderr, \"aot interp fn=%%s idx=%%d tab=%%d activations=%%llu handed-back=%%llu compiled-enters=%%llu gated=%%d exits=%%u refuses=%%u\\n\", qjs_aot_e_names[i] ? qjs_aot_e_names[i] : \"?\", i, k,\n" \
  "              (unsigned long long)ic_[k], (unsigned long long)ic_[nic_ + (unsigned)k], (unsigned long long)(e_ent[i] + e_inl[i]), (int)e_gate[i], e_exi[i], e_ref[i]); } }\n"

void qjsaot_number_guards(const char *path);

/* The matchers of the hot patterns: the profile's patterns by matcher cycles, down to the cover of the hot-cover fraction of their total, those the
   translator accepts, as <unit>-rx.c next to the unit (compiled and linked with it by the build wrappers; the engine finds a matcher by the exact
   bytecode of the pattern, so a pattern that is not here, or whose bytecode differs, runs in the interpreter). A raw-atom pattern has its own
   engine path and no matcher. */
static int rx_by_cycles(const void *a, const void *b) {
  const ProfRx *x = a, *y = b;
  return x->cyc < y->cyc ? 1 : x->cyc > y->cyc ? -1 : 0;
}
static void e_emit_regexps(const char *out, double cover) {
  size_t n = strlen(out);
  char *path = malloc(n + 8);
  FILE *f;
  unsigned long long total = 0, acc = 0;
  int i, k, nemit = 0;
  char (*names)[24] = NULL;
  const char *e = getenv("QJS_AOT_REGEXP");
  if (path == NULL) return;
  strcpy(path, out);
  if (n > 2 && strcmp(path + n - 2, ".c") == 0) path[n - 2] = 0;
  strcat(path, "-rx.c");
  remove(path);
  if (g_nprx == 0 || (e != NULL && e[0] == '0') || !(cover > 0.0)) { free(path); return; }
  qsort(g_prx, (size_t)g_nprx, sizeof(ProfRx), rx_by_cycles);
  for (i = 0; i < g_nprx; i++) total += g_prx[i].cyc;
  if (total == 0) { free(path); return; }
  f = fopen(path, "w");
  if (f == NULL) { free(path); return; }
  fprintf(f, "/* Generated by qjsaot: matchers of the profile's hot regexps (see regexp.c). Do not edit. */\n#include <stdint.h>\n#include \"libregexp.h\"\n");
  names = calloc((size_t)g_nprx, sizeof(*names));
  for (i = 0; i < g_nprx && names != NULL; i++) {
    char nm[24];
    int has16 = 0;
    FILE *t;
    char buf[65536];
    size_t r;
    if ((double)acc >= cover * (double)total) break;
    acc += g_prx[i].cyc;
    if (g_prx[i].len < 8 || (lre_get_flags(g_prx[i].bc) & LRE_FLAG_ATOM)) continue;
    snprintf(nm, sizeof(nm), "p%d", nemit);
    t = tmpfile();
    if (t == NULL) break;
    if (!rx_emit(t, nm, g_prx[i].bc, g_prx[i].len, g_prx[i].wide != 0, &has16)) { fclose(t); continue; }
    rewind(t);
    while ((r = fread(buf, 1, sizeof(buf), t)) > 0) fwrite(buf, 1, r, f);
    fclose(t);
    fprintf(f, "static const uint8_t bc_%s[%zu] = {", nm, g_prx[i].len);
    for (k = 0; k < (int)g_prx[i].len; k++) fprintf(f, "%s%u", k ? "," : "", g_prx[i].bc[k]);
    fprintf(f, "};\n");
    snprintf(names[nemit], sizeof(names[0]), "%s%s", nm, has16 ? "+" : "");
    nemit++;
  }
  if (nemit > 0) {
    fprintf(f, "static const LreAotEntry rx_tab[] = {\n");
    for (i = 0; i < nemit; i++) {
      char nm[24];
      int h16 = strchr(names[i], '+') != NULL;
      snprintf(nm, sizeof(nm), "%s", names[i]);
      if (h16) *strchr(nm, '+') = 0;
      fprintf(f, "  {bc_%s, (uint32_t)sizeof(bc_%s), rxf_%s_8, %s%s%s},\n", nm, nm, nm, h16 ? "rxf_" : "NULL", h16 ? nm : "", h16 ? "_16" : "");
    }
    fprintf(f, "};\nstatic void __attribute__((constructor, unused)) rx_register(void) { lre_aot_register(rx_tab, %d); }\n", nemit);
  }
  fclose(f);
  free(names);
  if (nemit == 0) remove(path);
  else fprintf(stderr, "qjsaot: %d regexp matchers (%.1f%% of the profile's matcher cycles) -> %s\n", nemit, 100.0 * (double)acc / (double)total, path);
  free(path);
}

int qjsaot_driver_main(int argc, char **argv) {
  JSRuntime *rt;
  JSContext *ctx;
  JSAotGenFunc *funcs = NULL;
  int nfuncs = 0;
  const char *out = "aot_unit.c";
  /* --split DIR: write one .c per compiled function plus a shared header
     and a registration file, so a change to one function recompiles only that
     file. */
  const char *split_dir = NULL;
  const char *profile = NULL;
  long budget = -1; /* -1 = unlimited */
  double hot_cover = 0.95;
  long min_ops = 1000;
  int all = 0;
  int i, fails = 0;
  JSValue *keep = NULL;
  int nkeep = 0;
  FILE *f;
  long long total_insns = 0;
  int emitted = 0;
  char **inputs = NULL;
  int ninputs = 0;
  const char *aot_layout_header = "quickjs-aot-layout.h";

  inputs = malloc(sizeof(*inputs) * (size_t)argc);
  if (!inputs) return 2;
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
      out = argv[++i];
      if (getenv("QJS_AOT_RAT_FILE") == NULL) {
        /* the side file of the return-address table (QJS_AOT_RAT) sits next to the unit, and starts empty */
        size_t ol = strlen(out);
        char *sf = malloc(ol + 5);
        if (sf != NULL) {
          snprintf(sf, ol + 5, "%s.rat", out);
          remove(sf);
          setenv("QJS_AOT_RAT_FILE", sf, 1);
        }
      }
    } else if (strcmp(argv[i], "--all") == 0) {
      all = 1;
    } else if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc) {
      profile = argv[++i];
    } else if (strcmp(argv[i], "--budget") == 0 && i + 1 < argc) {
      budget = strtol(argv[++i], NULL, 10);
    } else if (strcmp(argv[i], "--hot-cover") == 0 && i + 1 < argc) {
      hot_cover = strtod(argv[++i], NULL);
    } else if (strcmp(argv[i], "--self-weights") == 0 && i + 1 < argc) {
      g_self_weights = argv[++i];
    } else if (strcmp(argv[i], "--min-ops") == 0 && i + 1 < argc) {
      min_ops = strtol(argv[++i], NULL, 10);
    } else if (strcmp(argv[i], "--layout-header") == 0 && i + 1 < argc) {
      aot_layout_header = argv[++i];
    } else if (strcmp(argv[i], "--deopt-everywhere") == 0) {
      g_deopt_everywhere = 1;
    } else if (strcmp(argv[i], "--check-locals") == 0) {
      g_check_locals = 1;
    } else if (strcmp(argv[i], "--nocall") == 0) {
      e_set_nocall(1);
    } else if (strcmp(argv[i], "--poison") == 0) {
      e_set_poison(1);
    } else if (strcmp(argv[i], "--split") == 0 && i + 1 < argc) {
      split_dir = argv[++i];
    } else if (strcmp(argv[i], "--report") == 0 && i + 1 < argc) {
      g_report_path = argv[++i];
    } else if (strcmp(argv[i], "--facts") == 0 && i + 1 < argc) {
      g_facts_path = argv[++i];
    } else if (strcmp(argv[i], "--dump-ir") == 0) {
      g_dump_ir = 1;
    } else if (strcmp(argv[i], "--dump-ir-out") == 0 && i + 1 < argc) {
      g_dump_ir = 1;
      g_ir_path = argv[++i];
    } else if (strcmp(argv[i], "--types") == 0) {
      /* Analysis flags the wrapper consumed; tolerated here so that generating
         and dumping types in one run works. */
    } else if (strcmp(argv[i], "--types-out") == 0 && i + 1 < argc) {
      i++;
    } else if (strcmp(argv[i], "--verify-ir") == 0 ||
               strcmp(argv[i], "--stats") == 0) {
      /* ditto */
    } else if (argv[i][0] == '-') {
      fprintf(stderr, "qjsaot: unknown option %s\n", argv[i]);
      free(inputs);
      return 2;
    } else {
      inputs[ninputs++] = argv[i];
    }
  }
  if (ninputs == 0) {
    fprintf(
        stderr,
        "usage: qjsaot --out aot_unit.c [--all] [--budget N] "
        "[--hot-cover F] [--min-ops N] [--profile app.qjsprof] "
        "bundle.js ...\n");
    free(inputs);
    return 2;
  }
  {
    const char *e;
    if ((e = getenv("QJS_AOT_HOT_COVER")) != NULL) hot_cover = strtod(e, NULL);
    if ((e = getenv("QJS_AOT_MIN_OPS")) != NULL) min_ops = strtol(e, NULL, 10);
    if ((e = getenv("QJS_AOT_BUDGET")) != NULL) budget = strtol(e, NULL, 10);
  }
  if (profile && !all && !(hot_cover > 0.0))
    fprintf(stderr, "qjsaot: --profile selection is disabled; using --all\n");

  rt = JS_NewRuntime();
  ctx = JS_NewContext(rt);

  /* The compiled scripts are kept alive until after js_aot_gen_collect: the
     collector enumerates the runtime's function list, and a collection in the
     middle of a multi-file build would drop the earlier inputs' functions
     from the unit. */
  keep = malloc(sizeof(*keep) * (size_t)(ninputs > 0 ? ninputs : 1));
  nkeep = 0;
  if (!keep) {
    free(inputs);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 2;
  }

  for (i = 0; i < ninputs; i++) {
    long len = 0;
    char *src = read_file(inputs[i], &len);
    JSValue v;
    if (!src) {
      fprintf(stderr, "qjsaot: cannot read %s\n", inputs[i]);
      fails++;
      continue;
    }
    v = JS_Eval(
        ctx, src, (size_t)len, inputs[i],
        JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(v)) {
      JS_FreeValue(ctx, JS_GetException(ctx));
      v = JS_Eval(
          ctx, src, (size_t)len, inputs[i],
          JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    }
    if (JS_IsException(v)) {
      JSValue e = JS_GetException(ctx);
      const char *s = JS_ToCString(ctx, e);
      fprintf(stderr, "qjsaot: %s: %s\n", inputs[i], s ? s : "(unprintable)");
      JS_FreeCString(ctx, s);
      JS_FreeValue(ctx, e);
      fails++;
    }
    keep[nkeep++] = v;
    free(src);
  }
  free(inputs);

  nfuncs = js_aot_gen_collect(rt, &funcs);
  for (i = 0; i < nkeep; i++) JS_FreeValue(ctx, keep[i]);
  free(keep);
  qsort(funcs, (size_t)nfuncs, sizeof(*funcs), digest_cmp);

  if (g_dump_ir) {
    dump_ir(funcs, nfuncs);
    js_aot_gen_free(rt, funcs, nfuncs);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
  }

  {
    ProfEntry *prof = NULL;
    int nprof = 0;
    if (profile) {
      prof = parse_profile(profile, &nprof);
      if (!prof) {
        fprintf(stderr, "qjsaot: cannot use profile %s\n", profile);
        return 1;
      }
    }
    g_prof = prof;
    g_nprof = nprof;
    /* Decide the contracted layouts here, before any helper or body is emitted,
       so the entry check and the layout table come from one decision. */
    if (qjsaot_region_mode() == 2 && getenv("QJS_AOT_REGION_NOLAYOUTDRY") == NULL) {
      /* the classes the regions would bind, so that region mode makes layouts for those only (a dry first walk of every region) */
      int di_;
      g_unit_funcs = funcs;
      g_unit_nfuncs = nfuncs;
      qjs_aot_scan_init(ctx, funcs, nfuncs);
      qjs_aot_bundle_init(ctx, funcs, nfuncs);
      sel_prepare(funcs, nfuncs, NULL);
      for (di_ = 0; di_ < nfuncs; di_++) {
        Q2Func dfn_;
        char dhex_[33];
        int h_;
        if (!funcs[di_].light_ok) continue;
        if (q2_build(&funcs[di_], &dfn_) < 0 || q2_infer(&dfn_) < 0) { q2_free(&dfn_); continue; }
        for (h_ = 0; h_ < 16; h_++) snprintf(dhex_ + 2 * h_, 3, "%02x", funcs[di_].digest[h_]);
        region_dry_collect(&dfn_, dhex_, di_);
        q2_free(&dfn_);
      }
      g_rg_dry_done = 1;
    }
    {
      ELayoutInfo *pre = calloc(E_LAYOUT_MAX, sizeof(*pre));
      g_nlayouts = pre != NULL ? e_layouts_collect(pre, E_LAYOUT_MAX) : 0;
      free(pre);
    }
    if (getenv("QJS_AOT_LAYOUTCENSUS") != NULL) {
      ELayoutInfo *cand = calloc(E_LAYOUT_MAX, sizeof(*cand));
      int nc = cand != NULL ? e_layouts_collect(cand, E_LAYOUT_MAX) : 0;
      int ci;
      fprintf(stderr, "qjsaot: layout candidates=%d\n", nc);
      for (ci = 0; ci < nc; ci++)
        fprintf(stderr, "qjsaot:   layout shape=%d proto=%s%s fields=%d\n",
                cand[ci].shape_id, cand[ci].proto == 1 ? "fn:" : "",
                cand[ci].proto == 1 ? cand[ci].digest : "object",
                cand[ci].nfields);
      free(cand);
    }
    print_census(funcs, nfuncs, prof, nprof);
  }

  /* --- emit every unique eligible body once into a memory buffer --- */
  {
    EmitBuf *eb = calloc((size_t)nfuncs, sizeof(*eb));
    int *uniq = malloc(sizeof(int) * (size_t)(nfuncs > 0 ? nfuncs : 1));
    int nuniq = 0;
    char *sel = calloc((size_t)(nfuncs > 0 ? nfuncs : 1), 1);
    unsigned long long total_ops = 0, cov_ops = 0;
    long est_bytes = 0, actual_bytes = 0;

    if (!eb || !uniq || !sel) {
      free(eb);
      free(uniq);
      free(sel);
      js_aot_gen_free(rt, funcs, nfuncs);
      JS_FreeContext(ctx);
      JS_FreeRuntime(rt);
      return 2;
    }

    g_unit_funcs = funcs;
    qjs_aot_scan_init(ctx, funcs, nfuncs);
    qjs_aot_bundle_init(ctx, funcs, nfuncs);
    if (g_facts_path != NULL) {
      if (qjs_aot_bundle_report(g_facts_path) != 0) fprintf(stderr, "qjsaot: cannot write the facts report %s\n", g_facts_path);
    }
    g_unit_nfuncs = nfuncs;
    g_e_native = calloc((size_t)(nfuncs > 0 ? nfuncs : 1), 1);
    g_e_frameregs = calloc((size_t)(nfuncs > 0 ? nfuncs : 1), sizeof(int));
    /* The bodies are emitted into memory buffers before the output file is
     opened, so the linkage decision has to be made before that: a split unit
     gives every compiled function hidden external linkage so the parts can
     call each other. */
  if (split_dir != NULL) e_set_split(1);
  e_roles_check();
    e_report_open(g_report_path);
    sel_prepare(funcs, nfuncs, e_report_file());
    /* The inline forms first: a caller asks whether its callee has one while its own
       body is emitted. */
    g_inl_eb = calloc((size_t)(nfuncs > 0 ? nfuncs : 1), sizeof(*g_inl_eb));
    g_inl_nx = calloc((size_t)(nfuncs > 0 ? nfuncs : 1), 1);
    g_inl_leaf = calloc((size_t)(nfuncs > 0 ? nfuncs : 1), 1);
    g_inl_rz = calloc((size_t)(nfuncs > 0 ? nfuncs : 1), sizeof(int));
    g_inl_tl = calloc((size_t)(nfuncs > 0 ? nfuncs : 1), 1);
    /* The inline forms in sweeps. A body is committed once every callee it could take inline
       has been (or cannot be), so a committed body only names callees committed before it:
       there are no cycles, and the depth is the number of sweeps. The last sweep commits what
       is left. */
    g_inl_try = calloc((size_t)(nfuncs > 0 ? nfuncs : 1), 1);
    g_inl_lvl = calloc((size_t)(nfuncs > 0 ? nfuncs : 1), 1);
    {
      char (*whys)[160] = calloc((size_t)(nfuncs > 0 ? nfuncs : 1), 160);
      enum { E_INL_SWEEPS = 5 };
      for (int sweep = 0; g_inl_eb && whys && g_inl_try && sweep < E_INL_SWEEPS; sweep++)
        for (i = 0; i < nfuncs; i++) {
          char hex[33];
          char *tb = NULL;
          size_t tl = 0;
          FILE *mf;
          int ok;

          if (funcs[i].eligible != AOT_GEN_ELIGIBLE) continue;
          if (i + 1 < nfuncs && memcmp(funcs[i].digest, funcs[i + 1].digest, 16) == 0) continue;
          if (g_inl_eb[i].buf) continue;
          hex16(funcs[i].digest, hex);
          g_cur_func_idx = i;
          mf = open_memstream(&tb, &tl);
          if (!mf) continue;
          whys[i][0] = 0;
          e_fatal_reset(); e_fatal_set_hex(hex);
          ok = e_emit_inline_func(mf, &funcs[i], hex, whys[i], 160);
          if (e_fatal_taken()) ok = 0;   /* refused by e_fatal */
          fclose(mf);
          g_inl_try[i] = ok ? 1 : 2;
          if (ok && (sweep == E_INL_SWEEPS - 1 || !e_inline_pending())) {
            g_inl_eb[i].buf = tb;
            g_inl_eb[i].len = tl;
            g_inl_lvl[i] = (uint8_t)sweep;
            g_inl_nx[i] = (uint8_t)e_inline_used_nx();
            g_inl_leaf[i] = (uint8_t)e_inline_leaf();
            g_inl_rz[i] = e_inline_rz();
            g_inl_tl[i] = (uint8_t)e_inline_thislay();
          } else {
            free(tb);
          }
        }
      for (i = 0; whys && i < nfuncs; i++) {
        char hex[33];
        if (funcs[i].eligible != AOT_GEN_ELIGIBLE) continue;
        if (i + 1 < nfuncs && memcmp(funcs[i].digest, funcs[i + 1].digest, 16) == 0) continue;
        hex16(funcs[i].digest, hex);
        e_report_inline(hex, g_inl_eb[i].buf != NULL, whys[i]);
      }
      free(whys);
    }
    for (i = 0; i < nfuncs; i++) {
      char hex[33];
      int ninsn;
      FILE *mf;

      if (funcs[i].eligible != AOT_GEN_ELIGIBLE)
        continue; /* refused: no body to emit */
      if (i + 1 < nfuncs &&
          memcmp(funcs[i].digest, funcs[i + 1].digest, 16) == 0)
        continue; /* duplicate bytecode: one body serves both */
      hex16(funcs[i].digest, hex);
      g_cur_func_idx = i;
      mf = open_memstream(&eb[i].buf, &eb[i].len);
      if (!mf) {
        fails++;
        continue;
      }
      e_fatal_reset(); e_fatal_set_hex(hex);
      ninsn = e_emit_body(mf, &funcs[i], hex);
      fclose(mf);
      /* QJS_AOT_FATAL_FN=<name or digest prefix>: refuse that function as an emitter's e_fatal does (a test of the refusal path) */
      {
        const char *ff = getenv("QJS_AOT_FATAL_FN");
        if (ff != NULL && ff[0] != 0 && (strcmp(funcs[i].name, ff) == 0 || strncmp(hex, ff, strlen(ff)) == 0))
          e_fatal("forced by QJS_AOT_FATAL_FN", hex);
      }
      if (e_fatal_taken() || (e_body_stub_taken() && getenv("QJS_AOT_KEEP_STUBS") == NULL)) {   /* refused by e_fatal, or only a forward to the interpreter's deopt: no body, the function runs in the engine */
        free(eb[i].buf);
        eb[i].buf = NULL;
        eb[i].len = 0;
        continue;
      }
      if (ninsn < 0) {
        fprintf(
            stderr, "qjsaot: malformed program in %s (%s)\n",
            funcs[i].name[0] ? funcs[i].name : "?", funcs[i].file);
        free(eb[i].buf);
        eb[i].buf = NULL;
        eb[i].len = 0;
        fails++;
        continue;
      }
      total_insns += ninsn;
      uniq[nuniq++] = i;
    }
    e_report_close();

    /* --- selection --- */
    if (all || !profile || g_nprof == 0) {
      int k;
      for (k = 0; k < nuniq; k++) {
        sel[uniq[k]] = 1;
        est_bytes += (long)eb[uniq[k]].len;
      }
    } else {
      unsigned long long *ops =
          calloc((size_t)(nfuncs > 0 ? nfuncs : 1), sizeof(*ops));
      int k;
      if (!ops) {
        free(eb);
        free(uniq);
        free(sel);
        js_aot_gen_free(rt, funcs, nfuncs);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 2;
      }
      unsigned long long *wt = calloc((size_t)(nfuncs > 0 ? nfuncs : 1), sizeof(*wt));
      unsigned long long total_wt = 0;
      for (k = 0; k < nuniq; k++) {
        ops[uniq[k]] = prof_ops_of(g_prof, g_nprof, funcs[uniq[k]].digest);
        total_ops += ops[uniq[k]];
        wt[uniq[k]] = g_self_weights ? self_weight_of(g_self_weights, funcs[uniq[k]].digest) : ops[uniq[k]];
        total_wt += wt[uniq[k]];
      }
      if (total_ops == 0) {
        for (k = 0; k < nuniq; k++) {
          sel[uniq[k]] = 1;
          est_bytes += (long)eb[uniq[k]].len;
        }
      } else {
        int *order = malloc(sizeof(int) * (size_t)nuniq);
        unsigned long long cum = 0;
        unsigned long long target =
            (unsigned long long)(hot_cover * (double)total_wt);
        if (order) {
          for (k = 0; k < nuniq; k++) order[k] = uniq[k];
          /* QJS_AOT_SEL_DENSITY=1: rank by weight per byte of generated C (the cover is still counted in weight), so a large function with a thin
             share is the first to stay interpreted */
          if (getenv("QJS_AOT_SEL_DENSITY") != NULL) {
            unsigned long long *dw = calloc((size_t)(nfuncs > 0 ? nfuncs : 1), sizeof(*dw));
            if (dw) {
              for (k = 0; k < nuniq; k++) dw[uniq[k]] = wt[uniq[k]] * 1000000ull / (eb[uniq[k]].len ? (unsigned long long)eb[uniq[k]].len : 1ull);
              g_sort_ops = dw;
              qsort(order, (size_t)nuniq, sizeof(int), unit_ops_cmp);
              free(dw);
            } else { g_sort_ops = wt; qsort(order, (size_t)nuniq, sizeof(int), unit_ops_cmp); }
          } else {
            g_sort_ops = wt;
            qsort(order, (size_t)nuniq, sizeof(int), unit_ops_cmp);
          }
          for (k = 0; k < nuniq; k++) {
            if (cum >= target) break;
            if (wt[order[k]] == 0) break;
            sel[order[k]] = 1;
            cum += wt[order[k]];
          }
          free(order);
        }
        /* drop startup-only noise */
        for (k = 0; k < nuniq; k++)
          if (sel[uniq[k]] && ops[uniq[k]] < (unsigned long long)min_ops)
            sel[uniq[k]] = 0;
        /* close over compiled callees */
        {
          int changed = 1;
          /* census / what-if knobs: QJS_AOT_SEL_LOG=1 prints why each function is selected; the closure follows only the call sites that ran at least
             QJS_AOT_CLOSE_MIN_CALLS times (default 1000; 0 follows every call site, the old behaviour; a callee below the minimum stays interpreted); QJS_AOT_CLOSE_MIN_PCT=P only those whose calls are at least P percent of the caller's profiled ops;
             QJS_AOT_NO_RGDEP_CLOSURE=1 drops the region-dependency closure */
          unsigned long long close_min = getenv("QJS_AOT_CLOSE_MIN_CALLS") ? strtoull(getenv("QJS_AOT_CLOSE_MIN_CALLS"), NULL, 10) : 1000;
          double close_pct = getenv("QJS_AOT_CLOSE_MIN_PCT") ? strtod(getenv("QJS_AOT_CLOSE_MIN_PCT"), NULL) : 0.0;
          int no_rgdep = getenv("QJS_AOT_NO_RGDEP_CLOSURE") != NULL;
          unsigned long long edge_min = getenv("QJS_AOT_EDGE_MIN") ? strtoull(getenv("QJS_AOT_EDGE_MIN"), NULL, 10) : 0;
          int sel_log = getenv("QJS_AOT_SEL_LOG") != NULL;
          char (*why)[160] = sel_log ? calloc((size_t)(nfuncs > 0 ? nfuncs : 1), sizeof(*why)) : NULL;
          if (sel_log && why != NULL)
            for (k = 0; k < nuniq; k++) if (sel[uniq[k]]) snprintf(why[uniq[k]], sizeof(why[0]), "hot");
          while (changed) {
            changed = 0;
            for (k = 0; k < nuniq; k++) {
              const ProfEntry *pe;
              int c;
              if (!sel[uniq[k]]) continue;
              pe = prof_entry_of(g_prof, g_nprof, funcs[uniq[k]].digest);
              if (!pe) continue;
              for (c = 0; c < pe->ncalls; c++) {
                uint8_t dg[16];
                int ci;
                if (pe->calls[c].kind != 1 || !pe->calls[c].digest[0]) continue;
                if (hex_to_bytes16(pe->calls[c].digest, dg) < 0) continue;
                ci = find_unit(funcs, nfuncs, dg);
                if (ci >= 0 && !sel[ci]) {
                  if (pe->calls[c].hits < close_min) continue;
                  if (close_pct > 0.0 && (double)pe->calls[c].hits * 100.0 < close_pct * (double)ops[uniq[k]]) continue;
                  sel[ci] = 1;
                  changed = 1;
                  if (why != NULL) {
                    char ch[33];
                    hex16(funcs[uniq[k]].digest, ch);
                    snprintf(why[ci], sizeof(why[0]), "closure via=%s pc=%d hits=%llu", ch, pe->calls[c].pc, pe->calls[c].hits);
                  }
                }
              }
            }
            /* QJS_AOT_EDGE_MIN=N: a function that calls a selected function at least N times from one site is selected too, so the hot call does not
               cross from the interpreter into compiled code on every execution (edges to natives are not counted: only bytecode callees are `kind 1`) */
            if (edge_min > 0) {
              for (k = 0; k < nuniq; k++) {
                const ProfEntry *pe;
                int c;
                if (sel[uniq[k]] || !eb[uniq[k]].buf) continue;
                pe = prof_entry_of(g_prof, g_nprof, funcs[uniq[k]].digest);
                if (!pe) continue;
                for (c = 0; c < pe->ncalls; c++) {
                  uint8_t dg[16];
                  int ci;
                  if (pe->calls[c].kind != 1 || !pe->calls[c].digest[0] || pe->calls[c].hits < edge_min) continue;
                  if (hex_to_bytes16(pe->calls[c].digest, dg) < 0) continue;
                  ci = find_unit(funcs, nfuncs, dg);
                  if (ci >= 0 && sel[ci]) {
                    sel[uniq[k]] = 1;
                    changed = 1;
                    if (why != NULL) {
                      char ch[33];
                      hex16(funcs[ci].digest, ch);
                      snprintf(why[uniq[k]], sizeof(why[0]), "edge to=%s pc=%d hits=%llu", ch, pe->calls[c].pc, pe->calls[c].hits);
                    }
                    break;
                  }
                }
              }
            }
            /* a region's exits continue in the generic bodies of the functions it expanded: they must be in the unit when the root is */
            for (k = 0; k < g_rgdep_n && !no_rgdep; k++) {
              int rj = find_unit(funcs, nfuncs, g_rgdep[k].root), cj = find_unit(funcs, nfuncs, g_rgdep[k].callee);
              if (rj >= 0 && cj >= 0 && sel[rj] && !sel[cj] && eb[cj].buf) {
                sel[cj] = 1;
                changed = 1;
                if (why != NULL) {
                  char ch[33];
                  hex16(funcs[rj].digest, ch);
                  snprintf(why[cj], sizeof(why[0]), "rgdep via=%s", ch);
                }
              }
            }
          }
          if (why != NULL) {
            for (k = 0; k < nuniq; k++) {
              char ch[33];
              int i2 = uniq[k];
              if (!sel[i2]) continue;
              hex16(funcs[i2].digest, ch);
              fprintf(stderr, "sel fn=%s name=%s line=%d ops=%llu entries=%llu reason=%s\n", ch, funcs[i2].name[0] ? funcs[i2].name : "<anon>", funcs[i2].line, ops[i2],
                      qjs_aot_prof_entries(funcs[i2].digest), why[i2][0] ? why[i2] : "?");
            }
            free(why);
          }
        }
        /* budget: drop the lowest ops/byte until it fits */
        for (k = 0; k < nuniq; k++)
          if (sel[uniq[k]]) est_bytes += (long)eb[uniq[k]].len;
        if (budget > 0) {
          while (est_bytes > budget) {
            int best = -1, m;
            double best_ratio = 0;
            for (m = 0; m < nuniq; m++) {
              int idx = uniq[m];
              double ratio;
              if (!sel[idx]) continue;
              ratio =
                  eb[idx].len ? (double)ops[idx] / (double)eb[idx].len : 0.0;
              if (best < 0 || ratio < best_ratio ||
                  (ratio == best_ratio && eb[idx].len > eb[best].len)) {
                best = idx;
                best_ratio = ratio;
              }
            }
            if (best < 0) break;
            sel[best] = 0;
            est_bytes -= (long)eb[best].len;
          }
        }
        /* bisect aid: QJS_AOT_SEL_RANGE=lo:hi keeps only the selected functions whose ordinal (in selection order) is in [lo,hi);
           the dependency pass below then drops what cannot stay. QJS_AOT_SEL_RANGE=count prints the selected total. */
        {
          const char *rg = getenv("QJS_AOT_SEL_RANGE");
          if (rg != NULL) {
            long lo = 0, hi = 0, ord = 0, total = 0;
            for (k = 0; k < nuniq; k++) total += sel[uniq[k]] ? 1 : 0;
            if (strchr(rg, ':') != NULL) {
              for (k = 0; k < nuniq; k++) {
                const char *t = rg;
                int keep = 0;
                if (!sel[uniq[k]]) continue;
                while (t != NULL && *t != '\0') {
                  if (sscanf(t, "%ld:%ld", &lo, &hi) == 2 && ord >= lo && ord < hi) keep = 1;
                  t = strchr(t, ',');
                  if (t != NULL) t++;
                }
                if (!keep) sel[uniq[k]] = 0;
                ord++;
              }
            }
            fprintf(stderr, "qjsaot: QJS_AOT_SEL_RANGE: %ld functions selected before the range\n", total);
          }
        }
        /* A function with no body (refused by e_fatal at emission, or never emitted) cannot be in the table: the closure rule above selects
           callees by digest without asking, and the table, the forward declarations and the trampolines name only what has a body. */
        for (i = 0; i < nfuncs; i++)
          if (sel[i] && !eb[i].buf) sel[i] = 0;
        /* a body whose exits name another function's resume or generic body (a region's expanded callees, a resumable copy's callee) cannot stay when that function is not in the unit
           (not selected, no body, dropped by the budget): drop the body, which then runs through the engine */
        {
          int changed2 = 1;
          while (changed2) {
            changed2 = 0;
            for (k = 0; k < g_rgdep_n && getenv("QJS_AOT_NORXDEP") == NULL; k++) {
              int rj = find_unit(funcs, nfuncs, g_rgdep[k].root), cj = find_unit(funcs, nfuncs, g_rgdep[k].callee);
              if (rj >= 0 && cj >= 0 && sel[rj] && (!sel[cj] || !eb[cj].buf)) { sel[rj] = 0; changed2 = 1; }
            }
          }
        }
      }
      /* QJS_AOT_SEL_MODEL=1: what the selection costs by the profile, on stderr: the ops of the functions left to the interpreter, and the calls that cross
         between compiled and interpreted code (an edge with exactly one end selected; hits are the profile's call counts) */
      if (getenv("QJS_AOT_SEL_MODEL") != NULL) {
        unsigned long long dropped_ops = 0, all_ops = 0, cross_in = 0, cross_out = 0, all_calls = 0;
        for (k = 0; k < nuniq; k++) {
          const ProfEntry *pe = prof_entry_of(g_prof, g_nprof, funcs[uniq[k]].digest);
          int c;
          all_ops += ops[uniq[k]];
          if (!sel[uniq[k]]) dropped_ops += ops[uniq[k]];
          if (!pe) continue;
          for (c = 0; c < pe->ncalls; c++) {
            uint8_t dg[16];
            int ci;
            if (pe->calls[c].kind != 1 || !pe->calls[c].digest[0] || hex_to_bytes16(pe->calls[c].digest, dg) < 0) continue;
            ci = find_unit(funcs, nfuncs, dg);
            if (ci < 0) continue;
            all_calls += pe->calls[c].hits;
            if (sel[uniq[k]] && !sel[ci]) cross_out += pe->calls[c].hits;
            else if (!sel[uniq[k]] && sel[ci]) cross_in += pe->calls[c].hits;
          }
        }
        fprintf(stderr, "model: dropped_ops=%llu all_ops=%llu cross_out=%llu cross_in=%llu all_calls=%llu\n", dropped_ops, all_ops, cross_out, cross_in, all_calls);
      }
      free(ops);
    }
    for (i = 0; i < nfuncs; i++)
      if (sel[i]) cov_ops += prof_ops_of(g_prof, g_nprof, funcs[i].digest);

    /* The layout header the unit includes: the engine's offsets, the hash, and
       the unit's own `qjs_aot_layout` value. A unit compiled against a
       different engine's layouts has a different hash, which the engine
       refuses at bind (aot_unit_present). The static asserts that pin each
       offset against a mirror struct are generated alongside the code that
       reads a field; the hash already makes a drift unsound-free, because an
       old unit stops binding. */
    {
      JSAotGenLayout lay;
      FILE *lf;

      js_aot_gen_layout(&lay);
      lf = fopen(aot_layout_header, "w");
      if (!lf) {
        fprintf(stderr, "qjsaot: cannot write %s\n", aot_layout_header);
        for (i = 0; i < nfuncs; i++) free(eb[i].buf);
        free(eb);
        free(uniq);
        free(sel);
        js_aot_gen_free(rt, funcs, nfuncs);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 2;
      }
      /* the form of the build (stack maps) travels with the unit through its layout header, so no compile flag has to agree with the generator */
      if (qjs_aot_scan_mode()) fprintf(lf, "#ifndef QJS_AOT_STACKSCAN\n#define QJS_AOT_STACKSCAN 1\n#endif\n");
      if (qjs_aot_recscan_mode()) fprintf(lf, "#ifndef QJS_AOT_RECSCAN\n#define QJS_AOT_RECSCAN 1\n#endif\n");
      if (qjs_aot_rat_mode()) fprintf(lf, "#ifndef QJS_AOT_RAT\n#define QJS_AOT_RAT 1\n#endif\n");
      /* E_NONE: the calling convention of the unit's own entries (aot_n_), where a compiled caller calls a compiled callee: preserve_none
         (clang 19) saves no callee-saved registers in the callee. A compiler without the attribute ignores it for the declarations and the
         definitions alike. The engine and the entry tables reach the entries through aot_e_ shims in the C convention.
         QJS_AOT_NOPNONE=1 removes it. */
      if (getenv("QJS_AOT_NOPNONE") == NULL)
        fprintf(
            lf,
            "#if defined(__has_attribute)\n#if __has_attribute(preserve_none)\n#define E_NONE __attribute__((preserve_none))\n#endif\n#endif\n");
      fprintf(lf, "#ifndef E_NONE\n#define E_NONE\n#endif\n");
      /* Direct field addressing tests the object's header word at every access; measured on earleyboyer that test costs more than the property-array
         load it replaces (88.5G against 93.1G instructions with it off), and without a proof that the slots cannot move there is nothing to
         take it away. It is off unless QJS_AOT_HDRDIRECT=1; the accesses of contracted layouts use q2_slotd (no test) instead. */
      if (getenv("QJS_AOT_HDRDIRECT") == NULL) fprintf(lf, "#define QJS_AOT_NODIRECT 1\n");
      if (qjs_aot_scan_mode())
        fprintf(
            lf,
            "extern void qjs_aot_set_form(int);\n"
            "static void __attribute__((constructor, unused)) qjs_aot_form_reg(void) { qjs_aot_set_form(%d); }\n",
            (qjs_aot_rat_mode() ? 7 : qjs_aot_recscan_mode() ? 3 : 1));
      fprintf(
          lf,
          "/* Generated by qjsaot from the engine's struct layouts. Do not "
          "edit. */\n"
          "#ifndef QUICKJS_AOT_LAYOUT_H\n"
          "#define QUICKJS_AOT_LAYOUT_H\n\n"
          "#define QJS_AOT_LAYOUT_HASH %uu\n\n"
          "#define QJS_AOT_SIZEOF_JSOBJECT %uu\n"
          "#define QJS_AOT_SIZEOF_JSSHAPE %uu\n"
          "#define QJS_AOT_SIZEOF_JSPROPERTY %uu\n"
          "#define QJS_AOT_SIZEOF_JSSTRING %uu\n"
          "#define QJS_AOT_SIZEOF_JSVALUE %uu\n\n"
          "#define QJS_AOT_OFF_JSOBJECT_CLASS_ID %uu\n"
          "#define QJS_AOT_OFF_JSOBJECT_SHAPE %uu\n"
          "#define QJS_AOT_OFF_JSOBJECT_PROP %uu\n"
          "#define QJS_AOT_OFF_JSOBJECT_INLINE_CAP_BYTE %uu\n"
          "#define QJS_AOT_OFF_JSSHAPE_PROP_SIZE %uu\n"
          "#define QJS_AOT_OFF_JSSHAPE_PROP_COUNT %uu\n"
          "#define QJS_AOT_OFF_JSSHAPE_PROTO %uu\n"
          "#define QJS_AOT_OFF_JSSHAPE_HASH_TABLE %uu\n"
          "#define QJS_AOT_OFF_JSPROPERTY_U_VALUE %uu\n"
          "#define QJS_AOT_OFF_JSSHAPE_PROPERTY_FLAGS_ATOM %uu\n"
          "#define QJS_AOT_OFF_JSSTRING_LEN_BITS %uu\n"
          "#define QJS_AOT_OFF_JSSTRING_STRV %uu\n"
          "#define QJS_AOT_OFF_JSVALUE_TAG %uu\n"
          "#define QJS_AOT_OFF_JSVALUE_U %uu\n\n"
          "#define QJS_AOT_SIZEOF_JSREGPIC %uu\n"
          "#define QJS_AOT_REG_PIC_WAYS %uu\n"
          "#define QJS_AOT_OFF_FNBC_PIC_COUNT %uu\n"
          "#define QJS_AOT_OFF_JSREGPIC_SHAPE %uu\n"
          "#define QJS_AOT_OFF_JSREGPIC_SLOT_OFF %uu\n"
          "#define QJS_AOT_OFF_JSREGPIC_KIND %uu\n"
          "#define QJS_AOT_REG_PIC_OWN %uu\n"
          "#define QJS_AOT_REG_PIC_PROTO %uu\n"
          "#define QJS_AOT_REG_IC_NONE %uu\n"
          "#define QJS_AOT_REG_BIN_ADD %uu\n"
          /* The bitwise and shift sub-operations, printed with the engine's own
             enum values rather than derived by offset from BIN_ADD. The enum is
             contiguous today, but an offset would silently compute the wrong
             operation if anyone reordered it; a define that comes from the enum
             either stays correct or fails to compile if the name disappears. */
          "#define QJS_AOT_REG_BIN_SHL %uu\n"
          "#define QJS_AOT_REG_BIN_SAR %uu\n"
          "#define QJS_AOT_REG_BIN_SHR %uu\n"
          "#define QJS_AOT_REG_BIN_AND %uu\n"
          "#define QJS_AOT_REG_BIN_XOR %uu\n"
          "#define QJS_AOT_REG_BIN_OR %uu\n"
          "#define QJS_AOT_REG_BIN_SUB %uu\n"
          "#define QJS_AOT_REG_BIN_MUL %uu\n"
          "#define QJS_AOT_REG_BIN_DIV %uu\n"
          "#define QJS_AOT_REG_BIN_MOD %uu\n"
          "#define QJS_AOT_REG_BIN_LT %uu\n"
          "#define QJS_AOT_REG_BIN_LTE %uu\n"
          "#define QJS_AOT_REG_BIN_GT %uu\n"
          "#define QJS_AOT_REG_BIN_GTE %uu\n"
          "#define QJS_AOT_REG_BIN_STRICT_EQ %uu\n"
          "#define QJS_AOT_REG_BIN_STRICT_NEQ %uu\n"
          "#define QJS_AOT_REG_BIN_EQ %uu\n"
          "#define QJS_AOT_REG_BIN_NEQ %uu\n"
          "#define QJS_AOT_REG_CMP_INVERT %uu\n"
          "#define QJS_AOT_REG_BIN_IS_UNDEF %uu\n"
          "#define QJS_AOT_REG_BIN_IS_NOT_UNDEF %uu\n"
          "#define QJS_AOT_REG_PIC_OWN_W %uu\n"
          "#define QJS_AOT_OFF_JSOBJECT_FLAGS_BYTE %uu\n"
          "#define QJS_AOT_JSOBJECT_IS_PROTOTYPE_MASK %uu\n"
          "#define QJS_AOT_REG_UN_NEG %uu\n"
          "#define QJS_AOT_REG_UN_INC %uu\n\n"
          "#define QJS_AOT_OFF_REGRUN_B %uu\n"
          "#define QJS_AOT_OFF_REGRUN_PIC %uu\n"
          "#define QJS_AOT_OFF_REGRUN_VAR_BUF %uu\n"
          "#define QJS_AOT_OFF_REGRUN_ARG_BUF %uu\n"
          "#define QJS_AOT_OFF_REGRUN_ARGC %uu\n"
          "#define QJS_AOT_OFF_REGRUN_ARGV %uu\n"
          "#define QJS_AOT_OFF_REGRUN_FUNC %uu\n"
          "#define QJS_AOT_OFF_REGRUN_THIS_OBJ %uu\n"
          "#define QJS_AOT_OFF_REGRUN_FLAGS %uu\n"
          "#define QJS_AOT_OFF_REGRUN_SF %uu\n"
          "#define QJS_AOT_OFF_REGRUN_CALLER %uu\n"
          "#define QJS_AOT_OFF_FNBC_REGVM_CODE %uu\n"
          "#define QJS_AOT_OFF_FNBC_REGVM_PIC_HOLDER %uu\n"
          "#define QJS_AOT_OFF_FNBC_VAR_COUNT %uu\n"
          "#define QJS_AOT_OFF_FNBC_ARG_COUNT %uu\n"
          "#define QJS_AOT_OFF_FNBC_REGVM_REG_COUNT %uu\n"
          "#define QJS_AOT_OFF_FNBC_VAR_REF_COUNT %uu\n"
          "#define QJS_AOT_OFF_RT_GV_CELL_GEN %uu\n"
          "#define QJS_AOT_OFF_CTX_INTERRUPT_COUNTER %uu\n"
          "/* The poll's decrement as one read-modify-write with the flags kept: `sub $1, mem; jle`. */\n"
          "#if defined(__x86_64__)\n"
          "#define Q2_POLL_HIT(c) ({ int hit_; __asm__ volatile(\"subl $1, %%0\" : \"+m\"(*(int *)((unsigned char *)(c) + QJS_AOT_OFF_CTX_INTERRUPT_COUNTER)), \"=@ccle\"(hit_)); hit_; })\n"
          "#else\n"
          "#define Q2_POLL_HIT(c) (--*(int *)((unsigned char *)(c) + QJS_AOT_OFF_CTX_INTERRUPT_COUNTER) <= 0)\n"
          "#endif\n"
          "/* The back-edge poll of a loop body counts in a register and charges the shared counter once per batch: one decrement\n"
          "   and a branch per iteration, no store. A batch that reaches zero charges the counter and, when that runs it out, polls. */\n"
          "#define Q2_POLL_BATCH 1024\n"
          "#define Q2_POLL_HITB(c, q) (__builtin_expect(--(q) == 0, 0) && ((q) = Q2_POLL_BATCH, (*(int *)((unsigned char *)(c) + QJS_AOT_OFF_CTX_INTERRUPT_COUNTER) -= Q2_POLL_BATCH) <= 0))\n"
          "#define QJS_AOT_OFF_CTX_RT %uu\n"
          "#define QJS_AOT_OFF_OBJECT_FUNC_BYTECODE %uu\n"
          "#define QJS_AOT_CLASS_BYTECODE_FUNCTION %uu\n"
          "#define QJS_AOT_OFF_FNBC_AOT_FN %uu\n"
          "#define QJS_AOT_OFF_FNBC_REGVM_PIC %uu\n"
          "#define QJS_AOT_OFF_JSOBJECT_ARRAY_COUNT %uu\n"
          "#define QJS_AOT_OFF_JSOBJECT_ARRAY_VALUES %uu\n"
          "#define QJS_AOT_CLASS_ARRAY %uu\n"
          "#define QJS_AOT_CLASS_UINT8C_ARRAY %uu\n"
          "#define QJS_AOT_CLASS_INT8_ARRAY %uu\n"
          "#define QJS_AOT_CLASS_UINT8_ARRAY %uu\n"
          "#define QJS_AOT_CLASS_INT16_ARRAY %uu\n"
          "#define QJS_AOT_CLASS_UINT16_ARRAY %uu\n"
          "#define QJS_AOT_CLASS_INT32_ARRAY %uu\n"
          "#define QJS_AOT_CLASS_UINT32_ARRAY %uu\n"
          "#define QJS_AOT_CLASS_FLOAT32_ARRAY %uu\n"
          "#define QJS_AOT_CLASS_FLOAT64_ARRAY %uu\n"
          "#define QJS_AOT_STRING_LEN_MASK %uu\n"
          "#define QJS_AOT_OFF_CTX_CLASS_PROTO %uu\n"
          "#define QJS_AOT_CLASS_STRING %uu\n"
          "#define QJS_AOT_STRING_WIDE_MASK %uu\n"
          "#define QJS_AOT_STRING_KIND_MASK %uu\n"
          "#define QJS_AOT_SHAPE_PROP_TMASK %uu\n"
          "#define QJS_AOT_OFF_RT_ONECHAR %uu\n"
          "#define QJS_AOT_OFF_FNBC_AOT_POLY %uu\n"
          "#define QJS_AOT_OFF_FNBC_AOT_POLY_SITES %uu\n"
          "#define QJS_AOT_AOT_POLY_WAYS %uu\n"
          "#define QJS_AOT_OFF_JSOBJECT_HOLEY_BYTE %uu\n"
          "#define QJS_AOT_JSOBJECT_HOLEY_MASK %uu\n"
          "#define QJS_AOT_REG_PIC_PROTO2 %uu\n"
          "#define QJS_AOT_OFF_JSOBJECT_HTMLDDA_BYTE %uu\n"
          "#define QJS_AOT_JSOBJECT_HTMLDDA_MASK %uu\n"
          "#define QJS_AOT_OFF_JSOBJECT_EXT_BYTE %uu\n"
          "#define QJS_AOT_JSOBJECT_EXT_MASK %uu\n"
          "#define QJS_AOT_OFF_JSOBJECT_FAST_ARRAY_BYTE %uu\n"
          "#define QJS_AOT_JSOBJECT_FAST_ARRAY_MASK %uu\n"
          "#define QJS_AOT_OFF_JSOBJECT_ARRAY_SIZE %uu\n"
          "#define QJS_AOT_OFF_CTX_ARRAY_SHAPE %uu\n"
          "#define QJS_AOT_OFF_CTX_STD_ARRAY_PROTO_BYTE %uu\n"
          "#define QJS_AOT_CTX_STD_ARRAY_PROTO_MASK %uu\n"
          "#define QJS_AOT_OFF_CTX_GV_CELLS %uu\n"
          "#define QJS_AOT_SIZEOF_GV_CELL %uu\n"
          "#define QJS_AOT_GV_CELL_SETS %uu\n"
          "#define QJS_AOT_GV_CELL_WAYS %uu\n"
          "#define QJS_AOT_OFF_GVC_ATOM %uu\n"
          "#define QJS_AOT_OFF_GVC_GEN %uu\n"
          "#define QJS_AOT_OFF_GVC_SLOT %uu\n"
          "#define QJS_AOT_OFF_GVC_WR %uu\n"
          "#define QJS_AOT_OFF_RT_PIC_EPOCH %uu\n"
          "#define QJS_AOT_OFF_SHAPE_LAYOUT_ID %uu\n"
          "#define QJS_AOT_OFF_RT_LAYOUT_BROKEN %uu\n"
          "#define QJS_AOT_OFF_RT_LAYOUT_SHAPE %uu\n"
          "#define QJS_AOT_OFF_RT_LAYOUT_COUNT %uu\n"
          "#define QJS_AOT_OFF_RT_HTMLDDA %uu\n\n"
          "#define QJS_AOT_OFF_REGRUN_RET_IP %uu\n"
          "#define QJS_AOT_OFF_REGRUN_FR %uu\n"
          "#define QJS_AOT_OFF_REGRUN_VAR_REFS %uu\n"
          "#define QJS_AOT_OFF_REGRUN_HDR_PACK %uu\n"
          "#define QJS_AOT_OFF_REGRUN_CAP_IP %uu\n"
          "#define QJS_AOT_OFF_FNBC_REALM %uu\n"
          "#define QJS_AOT_OFF_RT_REGVM_CUR %uu\n"
          "#define QJS_AOT_OFF_RT_STACK_LIMIT %uu\n"
          "#define QJS_AOT_OFF_RT_FL_TOP %uu\n"
          "#define QJS_AOT_OFF_RT_ZPARK %uu\n"
          "#define QJS_AOT_OFF_RT_ZPARK_LEN %uu\n"
          "#define QJS_AOT_OFF_RT_ZPARK_CAP %uu\n"
          "#define QJS_AOT_OFF_RT_ZCT_NOBJ %uu\n"
          "#define QJS_AOT_OFF_RT_DRC_PEND %uu\n"
          "#define QJS_AOT_OFF_RT_DRC_PEND_I %uu\n"
          "#define QJS_AOT_OFF_RT_DRC_IN_RECONCILE %uu\n"
          "#define QJS_AOT_OFF_RT_GC_PHASE %uu\n"
          "#define QJS_AOT_ZCT_BACK %uu\n"
          "#define QJS_AOT_ZCT_MASK %uu\n"
          "#define QJS_AOT_CLASS_OBJECT %uu\n"
          "#define QJS_AOT_OFF_RT_BLKCACHE %uu\n"
          "#define QJS_AOT_OFF_RT_BLKCACHE_N %uu\n"
          "#define QJS_AOT_OFF_RT_MALLOC_COUNT %uu\n"
          "#define QJS_AOT_OFF_RT_MALLOC_SIZE %uu\n"
          "#define QJS_AOT_OFF_RT_MALLOC_LIMIT %uu\n"
          "#define QJS_AOT_OFF_RT_GC_OBJ_LIST %uu\n"
          "#define QJS_AOT_OFF_FNBC_OBJLIT_TAB %uu\n"
          "#define QJS_AOT_SIZEOF_OBJLIT_TPL %uu\n"
          "#define QJS_AOT_OFF_TPL_SHAPE %uu\n"
          "#define QJS_AOT_OFF_TPL_PROP_COUNT %uu\n"
          "#define QJS_AOT_OFF_OBJECT_U %uu\n"
          "#define QJS_AOT_PLAIN_GCBYTE %uu\n"
          "#define QJS_AOT_OBJ_INLINE_PROPS %uu\n"
          "#define QJS_AOT_OFF_FNBC_CTOR_FINAL_SHAPE %uu\n"
          "#define QJS_AOT_OFF_FNBC_CTOR_FN_SHAPE %uu\n"
          "#define QJS_AOT_OFF_FNBC_CTOR_PROTO_SLOT %uu\n"
          "#define QJS_AOT_OFF_FNBC_CTOR_FUSED_OK %uu\n"
          "#define QJS_AOT_OFF_FNBC_INST_INLINE_CAP %uu\n"
          "#define QJS_AOT_OBJECT_CTOR_MASK %uu\n"
          "#define QJS_AOT_PLAIN_BSI { %uu, %uu, %uu, %uu, %uu }\n"
          "#define QJS_AOT_PLAIN_ACCT { %uu, %uu, %uu, %uu, %uu }\n"
          "#define QJS_AOT_PLAIN_HDR { 0x%08x%08xull, 0x%08x%08xull, 0x%08x%08xull, 0x%08x%08xull, 0x%08x%08xull }\n"
          "#define QJS_AOT_OFF_OBJECT_FUNC_VAR_REFS %uu\n"
          "#define QJS_AOT_LIGHT_PREFIX_SLOTS %uu\n"
          "#define QJS_AOT_REGVM_FLAG_LIGHT %uu\n"
          "#define QJS_AOT_REGVM_FLAG_C_ENTRY %uu\n"
          "#define QJS_AOT_REGVM_FLAG_STACK %uu\n"
          "#define QJS_AOT_REGVM_FLAG_TRIVIAL_RET %uu\n"
          "#define QJS_AOT_REGVM_FLAG_SAME_REALM %uu\n"
          "#define QJS_AOT_RC_BACK %uu\n"
          "#define QJS_AOT_OFF_VARREF_PVALUE %uu\n"
          "#define QJS_AOT_CLASS_C_FUNCTION %uu\n"
          "#define QJS_AOT_OFF_CFUNC_C_FUNCTION %uu\n"
          "#define QJS_AOT_OFF_CFUNC_MAGIC %uu\n"
          "#define QJS_AOT_OFF_JSREGPIC_MISSES %uu\n"
          "#define QJS_AOT_REG_PIC_ADD_W %uu\n"
          "#define QJS_AOT_OFF_RT_IC_EPOCH %uu\n"
          "#define QJS_AOT_OFF_JSOBJECT_FLAGS16 %uu\n"
          "#define QJS_AOT_JSOBJECT_EXTENSIBLE_MASK %uu\n"
          "#define QJS_AOT_JSOBJECT_NOADD_MASK %uu\n"
          "#define QJS_AOT_JSOBJECT_INLINE_CAP_MASK %uu\n"
          "#define QJS_AOT_JSOBJECT_INLINE_CAP_SHIFT %uu\n"
          "#define QJS_AOT_OFF_ROPE_LEFT %uu\n"
          "#define QJS_AOT_OFF_ROPE_RIGHT %uu\n"
          "#define QJS_AOT_OFF_CTX_TA_LEN_EPOCH %uu\n"
          "#define QJS_AOT_CLASS_ARRAY_ITERATOR %uu\n"
          "#define QJS_AOT_OFF_ARRAY_ITER_OBJ %uu\n"
          "#define QJS_AOT_OFF_ARRAY_ITER_KIND %uu\n"
          "#define QJS_AOT_OFF_ARRAY_ITER_IDX %uu\n"
          "#define QJS_AOT_ARRAY_ITER_KIND_VALUE %uu\n"
          "#define QJS_AOT_OFF_CFUNC_CPROTO %uu\n"
          "#define QJS_AOT_CFUNC_CPROTO_ITERATOR_NEXT %uu\n"
          "#define QJS_AOT_CFUNC_CPROTO_GENERIC_MAGIC %uu\n"
          "_Static_assert(QJS_AOT_RC_BACK >= 4 && QJS_AOT_RC_BACK <= 64, "
          "\"rc offset\");\n\n"
          "#endif /* QUICKJS_AOT_LAYOUT_H */\n",
          lay.hash, lay.sizeof_JSObject, lay.sizeof_JSShape,
          lay.sizeof_JSProperty, lay.sizeof_JSString, lay.sizeof_JSValue,
          lay.off_object_class_id, lay.off_object_shape, lay.off_object_prop,
          lay.off_object_prop_inline_cap_byte, lay.off_shape_prop_size,
          lay.off_shape_prop_count, lay.off_shape_proto,
          lay.off_shape_hash_table, lay.off_property_u_value,
          lay.off_shape_property_flags_atom, lay.off_string_len_bits,
          lay.off_string_strv, lay.off_jsvalue_tag, lay.off_jsvalue_u,
          lay.sizeof_JSRegPIC, lay.reg_pic_ways, lay.off_fnbc_pic_count,
          lay.off_regpic_shape, lay.off_regpic_slot_off, lay.off_regpic_kind,
          lay.reg_pic_own, lay.reg_pic_proto, lay.reg_ic_none,
          lay.reg_bin_add, (unsigned)REG_BIN_SHL,
          (unsigned)REG_BIN_SAR, (unsigned)REG_BIN_SHR, (unsigned)REG_BIN_AND,
          (unsigned)REG_BIN_XOR, (unsigned)REG_BIN_OR, lay.reg_bin_sub,
          lay.reg_bin_mul, lay.reg_bin_div, lay.reg_bin_mod, lay.reg_bin_lt,
          lay.reg_bin_lte, lay.reg_bin_gt, lay.reg_bin_gte,
          lay.reg_bin_strict_eq, lay.reg_bin_strict_neq, lay.reg_bin_eq,
          lay.reg_bin_neq, lay.reg_cmp_invert, lay.reg_bin_is_undef,
          lay.reg_bin_is_not_undef, lay.reg_pic_own_w,
          lay.off_object_flags_byte, lay.object_is_prototype_mask,
          lay.reg_un_neg, lay.reg_un_inc, lay.off_regrun_b, lay.off_regrun_pic,
          lay.off_regrun_var_buf, lay.off_regrun_arg_buf, lay.off_regrun_argc, lay.off_regrun_argv, lay.off_regrun_func,
          lay.off_regrun_this_obj, lay.off_regrun_flags, lay.off_regrun_sf,
          lay.off_regrun_caller, lay.off_fnbc_regvm_code,
          lay.off_fnbc_regvm_pic_holder, lay.off_fnbc_var_count,
          lay.off_fnbc_arg_count, lay.off_fnbc_regvm_reg_count,
          lay.off_fnbc_var_ref_count, lay.off_rt_gv_cell_gen,
          lay.off_ctx_interrupt_counter, lay.off_ctx_rt,
          lay.off_object_func_bytecode, lay.class_bytecode_function,
          lay.off_fnbc_aot_fn, lay.off_fnbc_regvm_pic,
          lay.off_object_array_count, lay.off_object_array_values,
          lay.class_array, lay.class_uint8c_array, lay.class_int8_array,
          lay.class_uint8_array, lay.class_int16_array, lay.class_uint16_array,
          lay.class_int32_array, lay.class_uint32_array, lay.class_float32_array,
          lay.class_float64_array, lay.string_len_mask, lay.off_ctx_class_proto,
          lay.class_string, lay.string_wide_mask, lay.string_kind_mask, lay.shape_prop_tmask, lay.off_rt_onechar,
          lay.off_fnbc_aot_poly, lay.off_fnbc_aot_poly_sites,
          lay.aot_poly_ways, lay.off_object_holey_byte, lay.object_holey_mask,
          lay.reg_pic_proto2, lay.off_object_htmldda_byte, lay.object_htmldda_mask,
          lay.off_object_ext_byte, lay.object_ext_mask, lay.off_object_fast_array_byte,
          lay.object_fast_array_mask, lay.off_array_size, lay.off_ctx_array_shape,
          lay.off_ctx_std_array_proto_byte, lay.ctx_std_array_proto_mask,
          lay.off_ctx_gv_cells, lay.sizeof_gv_cell, lay.gv_cell_sets, lay.gv_cell_ways,
          lay.off_gvc_atom, lay.off_gvc_gen, lay.off_gvc_slot, lay.off_gvc_wr,
          lay.off_rt_pic_epoch, lay.off_shape_layout_id, lay.off_rt_layout_broken,
          lay.off_rt_layout_shape, lay.off_rt_layout_count, lay.off_rt_htmldda,
          lay.off_regrun_ret_ip, lay.off_regrun_fr, lay.off_regrun_var_refs,
          lay.off_regrun_hdr_pack, lay.off_regrun_cap_ip, lay.off_fnbc_realm,
          lay.off_rt_regvm_cur, lay.off_rt_stack_limit, lay.off_rt_fl_top,
          lay.off_rt_zpark, lay.off_rt_zpark_len, lay.off_rt_zpark_cap, lay.off_rt_zct_nobj, lay.off_rt_drc_pend, lay.off_rt_drc_pend_i,
          lay.off_rt_drc_in_reconcile, lay.off_rt_gc_phase, lay.off_zct_back, lay.zct_mask, lay.class_object,
          lay.off_rt_blkcache, lay.off_rt_blkcache_n, lay.off_rt_malloc_count, lay.off_rt_malloc_size, lay.off_rt_malloc_limit,
          lay.off_rt_gc_obj_list, lay.off_fnbc_objlit_tab, lay.sizeof_objlit_tpl, lay.off_tpl_shape, lay.off_tpl_prop_count,
          lay.off_object_u, lay.plain_gcbyte, lay.js_obj_inline_props,
          lay.off_fnbc_ctor_final_shape, lay.off_fnbc_ctor_fn_shape, lay.off_fnbc_ctor_proto_slot, lay.off_fnbc_ctor_fused_ok, lay.off_fnbc_inst_inline_cap, lay.object_ctor_mask,
          lay.plain_bsi[0], lay.plain_bsi[1], lay.plain_bsi[2], lay.plain_bsi[3], lay.plain_bsi[4],
          lay.plain_acct[0], lay.plain_acct[1], lay.plain_acct[2], lay.plain_acct[3], lay.plain_acct[4],
          lay.plain_hdr_hi[0], lay.plain_hdr_lo[0], lay.plain_hdr_hi[1], lay.plain_hdr_lo[1], lay.plain_hdr_hi[2], lay.plain_hdr_lo[2],
          lay.plain_hdr_hi[3], lay.plain_hdr_lo[3], lay.plain_hdr_hi[4], lay.plain_hdr_lo[4],
          lay.off_object_func_var_refs, lay.light_prefix_slots,
          lay.regvm_flag_light, lay.regvm_flag_c_entry, lay.regvm_flag_stack,
          lay.regvm_flag_trivial_ret, lay.regvm_flag_same_realm,
          lay.off_rc_back, lay.off_varref_pvalue, lay.class_c_function,
          lay.off_cfunc_c_function, lay.off_cfunc_magic, lay.off_regpic_misses,
          lay.reg_pic_add_w, lay.off_rt_ic_epoch, lay.off_object_flags16,
          lay.object_extensible_mask, lay.object_noadd_mask,
          lay.object_inline_cap_mask, lay.object_inline_cap_shift, lay.off_rope_left,
          lay.off_rope_right, lay.off_ctx_ta_len_epoch, lay.class_array_iterator,
          lay.off_array_iter_obj, lay.off_array_iter_kind, lay.off_array_iter_idx,
          lay.array_iter_kind_value, lay.off_cfunc_cproto, lay.cfunc_cproto_iterator_next,
          lay.cfunc_cproto_generic_magic);
      fprintf(lf, "#define QJS_AOT_OFF_SHAPE_YSITE %uu\n", lay.off_shape_ysite);
      /* Arrays that may hold int32 or double elements: every inline access of an array's elements
         refuses one whose kind byte is not zero. A holey array is the case the element path meets
         most often, so its byte is tested first and the kind byte only when that byte is clear:
         ORing the two loads together costs a third instruction on every element and decides no
         test the two tests do not already decide. */
      fprintf(lf,
          "#define QJS_AOT_PACKED_ARRAYS %uu\n"
          "#define QJS_AOT_OFF_JSOBJECT_ARR_KIND %uu\n"
          "#define QJS_AOT_OFF_RT_SHAPE_FREE_EPOCH %uu\n"
          "#define QJS_AOT_OFF_FNBC_REGVM_LEN %uu\n"
          "#define QJS_AOT_OFF_FNBC_AOT_IDENT %uu\n"
          "#define QJS_AOT_ARR_HOLES(p_) ((p_)[QJS_AOT_OFF_JSOBJECT_HOLEY_BYTE] & QJS_AOT_JSOBJECT_HOLEY_MASK)\n"
          "#if QJS_AOT_PACKED_ARRAYS\n"
          "#define QJS_AOT_ARR_ODD(p_) (QJS_AOT_ARR_HOLES(p_) || (p_)[QJS_AOT_OFF_JSOBJECT_ARR_KIND] != 0)\n"
          "#define QJS_AOT_ARR_KIND_ODD(p_) ((p_)[QJS_AOT_OFF_JSOBJECT_ARR_KIND] != 0)\n"
          "#else\n"
          "#define QJS_AOT_ARR_ODD(p_) QJS_AOT_ARR_HOLES(p_)\n"
          "#define QJS_AOT_ARR_KIND_ODD(p_) 0\n"
          "#endif\n",
          lay.packed_arrays, lay.off_object_arr_kind, lay.off_rt_shape_free_epoch, lay.off_fnbc_regvm_len, lay.off_fnbc_aot_ident);
      fprintf(lf, "#define QJS_AOT_CLASS_PROXY %uu\n", lay.class_proxy);
      fclose(lf);
    }

    if (split_dir != NULL) {
      char hpath[4096];
      snprintf(hpath, sizeof(hpath), "%s/u.h", split_dir);
      f = fopen(hpath, "w");
      if (!f) {
        fprintf(stderr, "qjsaot: cannot write %s\n", hpath);
        return 2;
      }
      fprintf(
          stderr,
          "qjsaot: split unit: %s (header, per-function .c and reg.c)\n",
          split_dir);
    } else {
      f = fopen(out, "w");
    }
    if (!f) {
      fprintf(stderr, "qjsaot: cannot write %s\n", out);
      for (i = 0; i < nfuncs; i++) free(eb[i].buf);
      free(eb);
      free(uniq);
      free(sel);
      js_aot_gen_free(rt, funcs, nfuncs);
      JS_FreeContext(ctx);
      JS_FreeRuntime(rt);
      return 2;
    }

    fprintf(
        f,
        "/* Generated by qjsaot from the register-VM bytecode. Do not edit. "
        "*/\n");
    fprintf(f, "#include \"quickjs-aot-rt.h\"\n");
    /* In a split unit every part is compiled with the split directory on the
       include path, so the layout header is named bare there. */
    if (split_dir != NULL) {
      const char *base = strrchr(aot_layout_header, '/');
      fprintf(f, "#include \"%s\"\n", base ? base + 1 : aot_layout_header);
    } else {
      fprintf(f, "#include \"%s\"\n", aot_layout_header);
    }
    /* Every compiled body goes in its own section, so the sampler can tell a
       sample in compiled code from one in the engine. The symbols are declared
       weak, so a unit without the section still links. */
    fprintf(
        f,
        "#if defined(__APPLE__)\n"
        "#define E_TEXT __attribute__((section(\"__TEXT,aottext,regular,pure_instructions\")))\n"
        "extern char __start_aottext[] __asm(\"section$start$__TEXT$aottext\");\n"
        "extern char __stop_aottext[] __asm(\"section$end$__TEXT$aottext\");\n"
        "#else\n"
        "#ifndef E_TEXT\n#define E_TEXT __attribute__((section(\"aottext\")))\n#endif\n"
        "extern char __start_aottext[] __attribute__((weak));\n"
        "extern char __stop_aottext[] __attribute__((weak));\n"
        "#endif\n");
    /* Unconditional: the A/B switch reads the environment, and a unit that is
       compiled without the debug checks still has to do that. */
    fprintf(f, "#include <stdlib.h>\n");
    if (g_check_locals) fprintf(f, "#include <string.h>\n#include <stdio.h>\n");
    fprintf(f, "\n");
    /* The unit's function count, so the switch-off counter arrays are sized exactly
       rather than to a guessed bound. In a split unit this text is the shared
       header, and the arrays are declared there and defined once in reg.c. */
    fprintf(f, "#define QJS_AOT_NFUNCS %uu\n",
            nfuncs > 0 ? (unsigned)nfuncs : 1u);
    {
      int mi, mr = 1;
      for (mi = 0; mi < nfuncs; mi++)
        if (funcs[mi].reg_count > mr) mr = funcs[mi].reg_count;
      fprintf(f, "#define QJS_AOT_MAXREGS %du\n", mr);
    }
    /* The function names, indexed the same way as the switch-off counter arrays, so the
       teardown line can print one row per function and a test can assert on one
       by name. */
    {
      int ni;
      if (split_dir != NULL) {
        fprintf(f, "extern const char *const qjs_aot_e_names[];\n");
      } else {
        fprintf(f, "static const char *const qjs_aot_e_names[] = {\n");
        for (ni = 0; ni < nfuncs; ni++)
          fprintf(f, "  \"%s\",\n", funcs[ni].name[0] ? funcs[ni].name : "?");
        if (nfuncs == 0) fprintf(f, "  \"?\",\n");
        fprintf(f, "};\n");
      }
    }
    if (split_dir != NULL) fprintf(f, "#define QJS_AOT_SPLIT_UNIT 1\n");
    fprintf(f, "\n");
    /* The frame reads a compiled entry performs: the same fields the engine's
       accessors return, read inline from the layout header's offsets, so the
       prologue does not pay a call across the unit boundary for each. */
    fprintf(
        f,
        "static inline void *q2_b(JSRegRun *f)\n"
        "{\n"
        "  return *(void **)((unsigned char *)f + QJS_AOT_OFF_REGRUN_B);\n"
        "}\n\n"
        /* The number of PIC sites in the running function: the stride between a
           site's caching ways. Read from the bytecode rather than baked into
           the unit, so the two can never disagree. */
        "static inline int q2_pic_sites(JSRegRun *f)\n"
        "{\n"
        "  return (int)*(const uint16_t *)((const unsigned char *)q2_b(f) +\n"
        "                                  QJS_AOT_OFF_FNBC_PIC_COUNT);\n"
        "}\n\n"
        "static inline uint8_t *q2_code(JSRegRun *f)\n"
        "{\n"
        "  return *(uint8_t **)((unsigned char *)q2_b(f) +\n"
        "                       QJS_AOT_OFF_FNBC_REGVM_CODE);\n"
        "}\n\n"
        /* The A/B switch, and the enable switch for the specialised body. The
           specialised body is the default; QJS_AOT_SPEC=0 runs every function
           through the generic one for comparison. Its polymorphic compare
           chain is disabled (see q2_poly_site): with it, DeltaBlue's compiled
           body reported a constraint cycle the interpreter does not. */
        "static int q2_no_spec, q2_spec_on;\n"
        /* Unit counters. `e_g8_hits` counts a dispatch to a
           pc that is not an instruction start; it must stay 0. */
        "static unsigned long e_g8_hits;\n"
        /* The switch-off counters, exactly one entry per function in the
           table. A function whose speculative body keeps exiting, or keeps
           refusing at entry, is switched off for good and its calls go straight
           to the generic body.

           In a split unit this text goes into the shared header, which every
           part includes, so the arrays must be declared here and defined once
           (in reg.c) rather than being `static` -- otherwise each part gets its
           own copy and its own counters. */

        /* One node per exit stub, linked the first time the stub is taken, with the
           number of times it was: the unit's statistics list the stubs that fire. */
        "typedef struct E_XNode { const char *hex, *kind; int pc; uint32_t cnt; struct E_XNode *next; const char *op; uint32_t nsh; } E_XNode;\n"
        "typedef struct E_NoteD { const char *hex, *op, *why; int fi, pc; } E_NoteD;\n"
        "typedef struct E_WhyD { const char *hex, *op; int pc, last, n; } E_WhyD;\n"
        /* Exit tally for the profitability gate's report: how often each (function, source pc) exit was taken, kept for every exit
           (not only under a stats flag) so that a function the gate switched off can name the exit that did it. */
        "#define E_XT_N 1024\n"
        "typedef struct { const char *hex; int fi, pc; uint32_t cnt; const char *op, *kind; } E_XT;\n"
        /* Whether the entry counters are live in this build (the production unit build drops them: QJS_AOT_NOENTNOTE). */
        "#ifdef QJS_AOT_NOENTNOTE\n#define E_ENT_COUNTED 0\n#else\n#define E_ENT_COUNTED 1\n#endif\n"
        "extern const uint64_t *qjs_aot_icount_table(unsigned *n) __attribute__((weak));\n"
        "extern const int32_t qjs_aot_tab_fi[];\n"
        "#ifdef QJS_AOT_SPLIT_UNIT\n"
        "extern uint64_t e_ent[QJS_AOT_NFUNCS];\n"
        "extern uint32_t e_exi[QJS_AOT_NFUNCS], e_ref[QJS_AOT_NFUNCS], e_refs[QJS_AOT_NFUNCS];\n"
        "extern uint8_t e_off[QJS_AOT_NFUNCS], e_gate[QJS_AOT_NFUNCS];\n"
        "extern uint32_t e_han[QJS_AOT_NFUNCS];\n"
        "extern uint8_t e_nfl[QJS_AOT_NFUNCS], e_dis[QJS_AOT_NFUNCS];\n"
        "extern uint8_t e_osr_ok[QJS_AOT_NFUNCS], e_osr_n[QJS_AOT_NFUNCS];\n"
        "extern uint64_t e_inl[QJS_AOT_NFUNCS];\n"
        "extern uint32_t e_inx[QJS_AOT_NFUNCS];\n"
        "extern uint8_t e_ioff[QJS_AOT_NFUNCS];\n"
        "extern E_XNode *e_xhead;\n"
        "extern E_XT e_xt[E_XT_N];\n"
        "extern uint32_t e_ngated;\n"
        "extern uint8_t e_gwhy[QJS_AOT_NFUNCS];\n"
        "#else\n"
        "static E_XNode *e_xhead;\n"
        "static E_XT e_xt[E_XT_N];\n"
        "static uint32_t e_ngated;\n"
        "static uint8_t e_gwhy[QJS_AOT_NFUNCS];\n"
        "static uint64_t e_ent[QJS_AOT_NFUNCS];\n"
        "static uint32_t e_exi[QJS_AOT_NFUNCS], e_ref[QJS_AOT_NFUNCS], e_refs[QJS_AOT_NFUNCS];\n"
        "static uint8_t e_off[QJS_AOT_NFUNCS], e_gate[QJS_AOT_NFUNCS];\n"
        "static uint32_t e_han[QJS_AOT_NFUNCS];\n"
        "static uint8_t e_nfl[QJS_AOT_NFUNCS], e_dis[QJS_AOT_NFUNCS];\n"
        "static uint8_t e_osr_ok[QJS_AOT_NFUNCS], e_osr_n[QJS_AOT_NFUNCS];\n"
        "static uint64_t e_inl[QJS_AOT_NFUNCS];\n"
        "static uint32_t e_inx[QJS_AOT_NFUNCS];\n"
        "static uint8_t e_ioff[QJS_AOT_NFUNCS];\n"
        "/* How many times the speculative bodies have been entered. A case that\n"
        "   means to exercise a spec path asserts this is non-zero: a body that is\n"
        "   never entered passes every check for the wrong reason, which is how\n"
        "   the entry-refusal bug survived a whole step. */\n"
        "unsigned long long qjs_aot_e_enters(void)\n"
        "{\n"
        "  unsigned long long t = 0;\n"
        "  int i;\n"
        "  for (i = 0; i < QJS_AOT_NFUNCS; i++) t += e_ent[i];\n"
        "  return t;\n"
        "}\n"
        "/* With QJS_AOT_UNIT_STATS=1 in the environment, print the unit counters at\n"
        "   teardown so a test can prove the speculative body actually ran. Without\n"
        "   it a case that means to exercise a spec path passes even when the body\n"
        "   is never entered -- which is exactly how the entry-refusal bug survived\n"
        "   a whole step. */\n"
        "#ifdef QJS_AOT_SPLIT_UNIT\n"
        "void e_stats_dump(void);\n"
        "#else\n"
        "static void __attribute__((destructor)) e_stats_dump(void)\n"
        "{\n"
        "  unsigned long long en = 0, ex = 0, rf = 0, off = 0, hn = 0, fl = 0, il = 0;\n"
        "  const char *e = getenv(\"QJS_AOT_UNIT_STATS\");\n"
        "  int i;\n"
        "  if (e == NULL || e[0] != '1') return;\n"
        "  for (i = 0; i < QJS_AOT_NFUNCS; i++) {\n"
        "    en += e_ent[i]; ex += e_exi[i]; rf += e_ref[i]; off += e_off[i];\n"
        "    hn += e_han[i]; il += e_inl[i]; if (e_nfl[i]) fl += e_ent[i];\n"
        "  }\n"
        "  fprintf(stderr, \"unit-stats enters=%%llu exits=%%llu handoffs=%%llu refuses=%%llu off=%%llu frameless=%%llu inlined=%%llu\\n\",\n"
        "          en, ex, hn, rf, off, fl, il);\n"
        /* Per function too, so the single unit answers the same question the split
           unit's reg.c does. A hand-off is CORRECT, merely slower, so a fix that
           only makes things faster can ONLY be tested by counting one function. */
        E_UNIT_STATS_TAIL
        "}\n"
        "static void __attribute__((destructor)) e_gate_report(void)\n"
        "{\n"
        E_GATE_REPORT_BODY
        "}\n"
        "#endif\n"
        "#endif\n"
        /* What a frameless body hands its exit function: where to resume, which
           registers to restore from `lv` (at the frame offsets in `offs`), the
           arguments' count, and for QJS_AOT_EXIT_LOG the guard that failed. One
           pointer, so the exit is a call with no more arguments than the entry. */
        "typedef struct ENx {\n"
        "  int pc, tag;\n"
        "  const char *kind;\n"
        "  const JSValue *lv;\n"
        "  const uint16_t *offs;\n"
        "  int n, argc;\n"
        "  const JSAotFlRec *rec;\n"
        "} ENx;\n"
        /* The exit record of the stub that is leaving. A stub writes it here and
           passes the address, so the hot body keeps no ENx of its own; the exit
           function reads it before running anything else. */
        "#ifdef QJS_AOT_SPLIT_UNIT\n"
        "extern _Thread_local ENx e_xs;\n"
        "extern _Thread_local JSValue e_xl[QJS_AOT_MAXREGS];\n"
        "#else\n"
        "static _Thread_local ENx e_xs;\n"
        "static _Thread_local JSValue e_xl[QJS_AOT_MAXREGS];\n"
        "#endif\n"
        /* A stub of a frameless or inlined body: the live values to the record's list in one statement, then the exit function. */
        "#define E_XLV(...) do { const JSValue xv_[] = { __VA_ARGS__ }; unsigned i_; \\\n"
        "    for (i_ = 0; i_ < sizeof(xv_) / sizeof(xv_[0]); i_++) e_xl[i_] = xv_[i_]; } while (0)\n"
        "#define E_FLX_RET(hex_, pc_, tag_, kind_, lv_, offs_, n_) \\\n"
        "    return aot_nx_##hex_(ctx_in, caller_f, co, this_obj, argv, \\\n"
        "                         (e_xs = (ENx){ pc_, tag_, kind_, lv_, offs_, n_, argc, rec_ }, &e_xs))\n"
        "#define E_FLX_OUT(hex_, pc_, tag_, kind_, lv_, offs_, n_) do { \\\n"
        "    *out = aot_nx_##hex_(ctx_in, caller_f, co, this_obj, argv, \\\n"
        "                         (e_xs = (ENx){ pc_, tag_, kind_, lv_, offs_, n_, argc, rec_ }, &e_xs)); return 1; } while (0)\n"
        "#ifdef QJS_AOT_NOENTNOTE\n#define E_ENTER_NOTE(fi) ((void)0)\n#else\n#define E_ENTER_NOTE(fi) (e_ent[fi]++)\n#endif\n"
        "#ifdef QJS_AOT_NOENTNOTE\n#define E_INL_NOTE(fi) ((void)0)\n#else\n#define E_INL_NOTE(fi) (e_inl[fi]++)\n#endif\n"
        /* Loop re-entry: the generic body, running a loop the speculative body left,
           hands the call back to it at the loop header. Skipped once when the generic
           body was entered at that very header (an exit there), and limited in count so
           a loop the speculative body keeps leaving does not nest without end. */
        "#define E_OSR_HOOK(fi_, h_, fn_) do { static uint8_t e_oc_; static int e_ic_; \\\n"
        "    if (osr_from == (h_)) osr_from = ~0u; \\\n"
        "    else if (__builtin_expect(e_osr_ok[fi_] && e_osr_n[fi_] < 200 && \\\n"
        "             !e_gate[fi_], 0)) { \\\n"
        /* The back-edge poll counter counts down once per loop iteration in either body, so
           its fall since the last re-entry is how long that stay lasted. When the speculative
           body called this one it takes the call back by returning to it (no nesting), and a
           re-entry followed by a long run is not charged: only loops that leave again at once
           use up the allowance. A nested call keeps the plain count. */
        "      int ic_ = *(int *)((unsigned char *)ctx + QJS_AOT_OFF_CTX_INTERRUPT_COUNTER); \\\n"
        "      if (from_spec_ && e_oc_ != 0 && e_ic_ - ic_ >= 64 && e_ic_ - ic_ < 8192) { \\\n"
        "        e_oc_--; if (e_exi[fi_] != 0) e_exi[fi_]--; } \\\n"
        "      if (e_oc_ < 64) { e_oc_++; e_ic_ = ic_; \\\n"
        "        if (from_spec_) return JS_MKVAL(JS_TAG_UNINITIALIZED, (h_)); \\\n"
        "        return fn_(ctx, f, fr, (h_)); } } } while (0)\n"
        /* A speculative body leaves for the generic body through this; a loop hook there
           answers with the re-entry marker, which the entry function (aot_f_) turns into a call of
           the body at the loop entry. */
        "#define E_RET_G(h_, pc_) do { e_gfs_##h_ = 1; return aot_g_##h_(ctx, f, fr, (pc_)); } while (0)\n"
        /* An inline body that cannot run (its nested callee is another function than
           the profiled one, say) leaves with nothing done and the native call takes
           over; that is not a failed speculation, so it does not count toward
           switching the function off, but a body that does it on more than 1/16 of
           its attempts stops being tried. */
        "#define E_INL_EXIT_NOTE(fi) do { if (++e_inx[fi] > 256 && \\\n"
        "    (uint64_t)e_inx[fi] * 16 > e_inl[fi] + e_inx[fi]) e_ioff[fi] = 1; } while (0)\n"
        /* A frameless function marks itself at load (e_nfl), so its entries
           are counted by the one counter every spec body bumps. */
        "/* More than 256 exits and in more than 1/16 of the calls: the guards do\n"
        "   not match the data this function sees. */\n"
        /* QJS_AOT_EXIT_LOG=1: the first time each guard stub is taken, one
           line naming it and the tag its guard saw (-100 where there is no
           value: an overflow exit, or a stub whose guard named none). Read
           once; one load and a branch when off. The work is out of line: at
           -O0 every inline copy took its own stack slots, and the stubs of a big
           function grew its frame enough to overflow earleyboyer's recursion. */
        "static int e_exit_log_v = -1;\n"
        /* preserve_most: the call preserves every register, so a body that
           reaches it in an exit does not give up its caller-saved registers (and
           push seven callee-saved ones in its prologue) for a log nobody asked for. */
        "static void __attribute__((noinline, cold, unused, preserve_most))\n"
        "e_exit_log(const char *hex, const char *kind, int pc, int tag, int *seen, const char *op, unsigned nsh)\n"
        "{\n"
        "  if (e_exit_log_v < 0) {\n"
        "    const char *e_ = getenv(\"QJS_AOT_EXIT_LOG\");\n"
        "    e_exit_log_v = e_ != NULL ? (e_[0] == '2' ? 2 : e_[0] == '1') : 0;\n"
        "  }\n"
        "  if (!e_exit_log_v) return;\n"
        "  *seen = e_exit_log_v != 2;\n"
        "  fprintf(stderr, \"unit-exit fn=%%s kind=%%s pc=%%d tag=%%d op=%%s shapes=%%u\\n\", hex, kind,\n"
        "          pc, tag, op ? op : \"?\", nsh);\n"
        "}\n"
        "static inline void e_xnode_reg(E_XNode *n) { n->next = e_xhead; e_xhead = n; }\n"
        /* The bookkeeping of the other exits (notes, refusals, reasons) is out of line, one call per stub. The exit log counter stays inline: outlined, crypto lost 8%. */
        /* E_LOGO_COLD (large units, set by the profile-guided build): the stub is one call to a cold helper that takes the descriptor. */
        "#ifdef E_LOGO_COLD\n"
        "static void __attribute__((noinline, cold, unused, preserve_most)) e_logo_c(E_XNode *n, int tag)\n"
        "{\n"
        "  int xl = 0;\n"
        "  if (++n->cnt == 1) e_xnode_reg(n);\n"
        "  if (e_exit_log_v != 0 && (e_exit_log_v == 2 || n->cnt == 1)) e_exit_log(n->hex, n->kind, n->pc, tag, &xl, n->op, 0);\n"
        "}\n"
        "#define E_EXIT_LOGO(hex, kind, pc, tag, op) do { static E_XNode e_xn_ = {(hex), (kind), (pc), 0, NULL, (op), 0}; e_logo_c(&e_xn_, (int)(tag)); } while (0)\n"
        "#else\n"
        "#define E_EXIT_LOGO(hex, kind, pc, tag, op) do { static E_XNode e_xn_ = {(hex), (kind), (pc), 0, NULL, (op), 0}; \\\n"
        "    int e_xl_ = 0; \\\n"
        "    if (__builtin_expect(++e_xn_.cnt == 1, 0)) e_xnode_reg(&e_xn_); \\\n"
        "    if (__builtin_expect(e_exit_log_v != 0, 0) && (e_exit_log_v == 2 || e_xn_.cnt == 1)) \\\n"
        "      e_exit_log((hex), (kind), (pc), (int)(tag), &e_xl_, (op), 0); } while (0)\n"
        "#endif\n"
        "#define E_EXIT_LOG(hex, kind, pc, tag) E_EXIT_LOGO(hex, kind, pc, tag, NULL)\n"
        /* An entry refusal at a property site: the node keeps the largest number of distinct shapes the site's cache ways held
           (read at the first 64 refusals and then at each power of two, so a hot refusal does not pay for it). */
        "static int __attribute__((noinline, cold, unused, preserve_most)) e_refuse_pre(E_XNode *n)\n"
        "{\n"
        "  if (++n->cnt == 1) e_xnode_reg(n);\n"
        "  return n->cnt <= 64 || (n->cnt & (n->cnt - 1)) == 0 || e_exit_log_v != 0;\n"
        "}\n"
        "static void __attribute__((noinline, cold, unused, preserve_most)) e_refuse_post(E_XNode *n, unsigned nsh)\n"
        "{\n"
        "  int l_ = 0;\n"
        "  if (nsh > n->nsh) n->nsh = nsh;\n"
        "  if (e_exit_log_v != 0 && (e_exit_log_v == 2 || n->cnt == 1)) e_exit_log(n->hex, \"refuse-site\", n->pc, -2, &l_, n->op, nsh);\n"
        "}\n"
        "#define E_REFUSE_LOG(hex, pc, op, nsh_expr) do { static E_XNode e_xn_ = {(hex), \"refuse-site\", (pc), 0, NULL, (op), 0}; \\\n"
        "    if (e_refuse_pre(&e_xn_)) e_refuse_post(&e_xn_, (nsh_expr)); } while (0)\n"
        /* An exit taken because some cache entry changed since the body validated its cells is not a
           failed speculation: the generic body fills what changed and the next entry passes. It does
           not count toward the switch-off. */
        "static int e_ep_exit;\n"
        /* QJS_AOT_POSTCALLCOUNT: the number of post-call epoch checks executed, printed at exit */
        "#ifdef QJS_AOT_POSTCALLCOUNT\n"
        "static unsigned long long e_pcn;\n"
        "__attribute__((destructor)) static void e_pcn_report(void) { fprintf(stderr, \"aot postcall-checks %%llu\\n\", e_pcn); }\n"
        "#define E_PC_TICK() (e_pcn++)\n"
        "#else\n"
        "#define E_PC_TICK() ((void)0)\n"
        "#endif\n"
        /* the profitability gate's exit threshold (256): QJS_AOT_GATE_MIN=n lowers it for the F43 stress lane (gate early, many functions gated mid-run) */
        "static inline unsigned e_gate_div(void) { static int v = -1; if (v < 0) { const char *e_ = getenv(\"QJS_AOT_GATE_DIV\"); v = e_ ? atoi(e_) : 16; } return (unsigned)v; }\n"
        "static inline unsigned e_gate_min(void) { static int v = -1; if (v < 0) { const char *e_ = getenv(\"QJS_AOT_GATE_MIN\"); v = e_ ? atoi(e_) : 256; } return (unsigned)v; }\n"
        /* The profitability gate (a safety net, never a fix): a function that exits more than 256 times and in more than 1 of 16
           of its entries is switched off (it runs its generic body from then on). A region's exit stubs name their pc and hex
           (E_EXIT_NOTE_PC); the switch-off is counted (e_ngated, printed as gated=K with QJS_AOT_STATS=1) and reported with the
           exit taken most, so a gated region is never silent. */
        "static inline void e_xt_note(const char *hex, int fi, int pc, const char *op, const char *kind)\n"
        "{\n"
        "  unsigned h_ = ((unsigned)fi * 31u + (unsigned)pc) %% E_XT_N, k_;\n"
        "  for (k_ = 0; k_ < E_XT_N; k_++, h_ = (h_ + 1) %% E_XT_N) {\n"
        "    E_XT *t_ = &e_xt[h_];\n"
        "    if (t_->cnt == 0) { t_->hex = hex; t_->fi = fi; t_->pc = pc; t_->op = op; t_->kind = kind; t_->cnt = 1; return; }\n"
        "    if (t_->fi == fi && t_->pc == pc && (t_->kind == kind || (t_->kind != NULL && kind != NULL && __builtin_strcmp(t_->kind, kind) == 0))) { t_->cnt++; return; }\n"
        "  }\n"
        "}\n"
        "extern uint64_t q2_kw[];\n"
        "static inline void e_gate_note(int fi)\n"
        "{\n"
        "  q2_kw[fi] = 0;\n"
        "  if (!e_gate[fi]) { e_gwhy[fi] = 1; e_ngated++; }\n"
        "  e_off[fi] = e_gate[fi] = 1;\n"
        "}\n"
        /* The entries a function's exits are measured against: its own entries and, where the build counts them (not the production
           unit build, see QJS_AOT_NOENTNOTE), the entries of its inlined copies. */
        "#define E_ENTS(fi) (e_ent[fi] + e_inl[fi])\n"
        /* A plain cold call, not preserve_most: the exit stubs are many and inside hot loops' functions, and preserve_most there changed
           the register allocation of the loops themselves (kit am3: two more moves per iteration, +4.3% cycles) for no size gain. */
        "static void __attribute__((noinline, cold, unused)) e_exit_note_c(const E_NoteD *d)\n"
        "{\n"
        "  int fi = d->fi;\n"
        "  if (e_ep_exit) { e_ep_exit = 0; return; }\n"
        "  e_xt_note(d->hex, fi, d->pc, d->op, d->why);\n"
        "  if (++e_exi[fi] > e_gate_min() && (uint64_t)e_exi[fi] * e_gate_div() > E_ENTS(fi)) e_gate_note(fi);\n"
        "}\n"
        "#define E_EXIT_NOTE_AT(fi, pc_, hex_, op_, why_) do { static const E_NoteD e_nd_ = {(hex_), (op_), (why_), (fi), (pc_)}; e_exit_note_c(&e_nd_); } while (0)\n"
        "#define E_EXIT_NOTE_PC(fi, pc_, hex_) E_EXIT_NOTE_AT(fi, pc_, hex_, \"region\", \"region-exit\")\n"
        "/* A HAND-OFF is not a deopt: it is the exit at an instruction the spec\n"
        "   body does not lower. Counting them toward the switch-off would turn off\n"
        "   every spec body after 257 calls and every measurement would time the\n"
        "   generic body. They are counted, never acted on. */\n"
        "#define E_HANDOFF_NOTE(fi) (e_ep_exit = 0, e_han[fi]++)\n"
        /* The same at an instruction the body does not lower, with a counter of its own per site (one increment): the unit statistics
           list the sites that were taken and the op at each. */
        "#define E_HANDOFF_NOTE_AT(fi, hex_, pc_, op_) do { static E_XNode e_hn_ = {(hex_), \"handoff\", (pc_), 0, NULL, (op_), 0}; \\\n"
        "    if (__builtin_expect(++e_hn_.cnt == 1, 0)) e_xnode_reg(&e_hn_); E_HANDOFF_NOTE(fi); } while (0)\n"
        "/* Entry refused in more than half the calls: nothing to speculate on. */\n"
        /* A refusal because a hot site's cell is still EMPTY: the site has not run yet in this process (a rare branch), so the
           generic body that runs instead fills it and the next entry passes. It does not count toward the switch-off at 256,
           which was reached by such functions before the branch ever ran; a much larger limit still ends a cell that is never
           filled. */
        "#ifdef E_LOGO_COLD\n"
        "static void __attribute__((noinline, cold, unused, preserve_most)) e_refuse_soft_c(int fi)\n"
        "{ if (e_osr_n[fi] < 200) e_osr_n[fi] += 32; if (++e_refs[fi] > 65536 && (uint64_t)e_refs[fi] * 2 > E_ENTS(fi)) e_gate_note(fi); }\n"
        "static void __attribute__((noinline, cold, unused, preserve_most)) e_refuse_note_c(int fi)\n"
        "{ if (e_osr_n[fi] < 200) e_osr_n[fi] += 32; if (++e_ref[fi] > 256 && (uint64_t)e_ref[fi] * 2 > E_ENTS(fi)) e_gate_note(fi); }\n"
        "#define E_REFUSE_SOFT(fi) e_refuse_soft_c(fi)\n"
        "#define E_REFUSE_NOTE(fi) e_refuse_note_c(fi)\n"
        "#else\n"
        "#define E_REFUSE_SOFT(fi) do { if (e_osr_n[fi] < 200) e_osr_n[fi] += 32; if (++e_refs[fi] > 65536 && \\\n"
        "    (uint64_t)e_refs[fi] * 2 > E_ENTS(fi)) e_gate_note(fi); } while (0)\n"
        "#define E_REFUSE_NOTE(fi) do { if (e_osr_n[fi] < 200) e_osr_n[fi] += 32; if (++e_ref[fi] > 256 && \\\n"
        "    (uint64_t)e_ref[fi] * 2 > E_ENTS(fi)) e_gate_note(fi); } while (0)\n"
        "#endif\n"
        "#ifdef QJS_AOT_PICSOFT\n#define E_REFUSE_NOTE_PIC(fi) E_REFUSE_SOFT(fi)\n#else\n#define E_REFUSE_NOTE_PIC(fi) E_REFUSE_NOTE(fi)\n#endif\n"
        /* The dynamic register check, compiled into the unit only with -DE_POISON.
           It compares every register an op call was not allowed to write
           against a copy taken before the call, by tag and then by kind, so a
           handler that writes a register it did not declare is caught at the
           site rather than as a wrong answer later. */
        "#if defined(E_POISON) && E_POISON\n"
        "#include <stdio.h>\n"
        "#include <stdlib.h>\n"
        "/* Equal as values: the tag decides how the payload is compared, so "
        "a\n"
        "   NaN payload or a pointer is never memcmp'd (invariant 12). */\n"
        "static int e_same_value(JSValue a, JSValue b)\n"
        "{\n"
        "  int ta = JS_VALUE_GET_TAG(a), tb = JS_VALUE_GET_TAG(b);\n"
        "  if (ta != tb) return 0;\n"
        "  switch (ta) {\n"
        "  case JS_TAG_INT: return JS_VALUE_GET_INT(a) == "
        "JS_VALUE_GET_INT(b);\n"
        "  case JS_TAG_BOOL: return JS_VALUE_GET_BOOL(a) == "
        "JS_VALUE_GET_BOOL(b);\n"
        "  case JS_TAG_FLOAT64: {\n"
        "    double x = JS_VALUE_GET_FLOAT64(a), y = JS_VALUE_GET_FLOAT64(b);\n"
        "    return x == y && !(x != x) && !(y != y) ? 1 : (x != x && y != "
        "y);\n"
        "  }\n"
        "  case JS_TAG_STRING: case JS_TAG_STRING_ROPE: case JS_TAG_OBJECT:\n"
        "    return JS_VALUE_GET_PTR(a) == JS_VALUE_GET_PTR(b);\n"
        "  default:\n"
        "    /* null, undefined, uninitialized and the internal tags carry no\n"
        "       payload; the tag comparison was the whole test. */\n"
        "    return 1;\n"
        "  }\n"
        "}\n"
        "#define E_SENTINEL JS_MKVAL(JS_TAG_INT, 0x5e5e5e5e)\n"
        "static long e_poison_seen;\n"
        "static void e_poison_abort(const char *op, int pc, int reg,\n"
        "                            JSValue got)\n"
        "{\n"
        "  if (e_poison_seen++ < 200) {\n"
        "    int tg = JS_VALUE_GET_TAG(got);\n"
        "    fprintf(stderr, \"poison: %%s pc %%d wrote r%%d (now tag %%d "
        "int %%d)\\n\",\n"
        "            op, pc, reg, tg, tg == JS_TAG_INT ? JS_VALUE_GET_INT(got) "
        ": -1);\n"
        "  }\n"

        "#ifdef E_POISON_QUIET\n"
        "  /* Report and keep going, so one run lists every disagreement. */\n"
        "  (void)op; (void)pc; (void)reg;\n"
        "#else\n"
        "  abort();\n"
        "#endif\n"
        "}\n"
        /* The two halves of the check. `pm[r]` is '1' for a register the op
           neither reads nor writes and that is dead after it; `wm[r]` is '1'
           for a register it may write. Only uncounted values are replaced: a
           register can be the only root of a counted one. The counters are
           printed at exit so a check that poisons nothing is visible. */
        "static unsigned long e_poison_calls, e_poison_regs;\n"
        "static __attribute__((noinline)) void e_poison_pre(JSValue *fr, int "
        "n,\n"
        "    JSValue *copy, uint8_t *pz, const char *pm)\n"
        "{\n"
        "  e_poison_calls++;\n"
        "  for (int r = 0; r < n; r++) {\n"
        "    copy[r] = AOT_REG(fr, r * 16);\n"
        "    pz[r] = 0;\n"
        "    if (pm[r] == '1' && !JS_VALUE_HAS_REF_COUNT(copy[r])) {\n"
        "      AOT_REG(fr, r * 16) = E_SENTINEL;\n"
        "      pz[r] = 1;\n"
        "      e_poison_regs++;\n"
        "    }\n"
        "  }\n"
        "}\n"
        "static __attribute__((noinline)) void e_poison_post(JSValue *fr, int "
        "n,\n"
        "    const JSValue *copy, const uint8_t *pz, const char *wm,\n"
        "    const char *op, int pc)\n"
        "{\n"
        "  for (int r = 0; r < n; r++) {\n"
        "    if (wm[r] == '1') continue;\n"
        "    if (!e_same_value(AOT_REG(fr, r * 16), pz[r] ? E_SENTINEL : "
        "copy[r])) {\n"
        "      e_poison_abort(op, pc, r, AOT_REG(fr, r * 16));\n"
        "    } else if (pz[r]) {\n"
        "      AOT_REG(fr, r * 16) = copy[r];\n"
        "    }\n"
        "  }\n"
        "}\n"
        "static void __attribute__((destructor)) e_poison_stats(void)\n"
        "{\n"
        "  fprintf(stderr, \"poison-stats: calls=%%lu poisoned=%%lu\\n\",\n"
        "          e_poison_calls, e_poison_regs);\n"
        "}\n"
        "#endif\n"
        /* Per-site hit/miss counters (compiled with -DE_COUNT=1). One pair per
           inline fast path, so a lowering that never hits is visible as data
           rather than inferred from a benchmark. A site is named by its
           function's digest, the instruction's pc and the op, which is what the
           generator's op-call weight lines use, so the two can be read side by
           side. Disabled by default: the counters are a diagnostic, not a gate,
           and the lookup is linear because a function has only a few sites. */
        "#if defined(E_COUNT) && E_COUNT\n"
        "#include <stdio.h>\n"
        "#include <string.h>\n"
        "typedef struct { const char *hex; unsigned pc; uint16_t rop;\n"
        "                 unsigned long long hit, miss; } ESiteCtr;\n"
        "static ESiteCtr e_sites[8192];\n"
        "static unsigned e_nsites;\n"
        "static const char *const e_rop_names[] = {\n");
    {
      int r;
      for (r = 0; r < ROP_COUNT; r++) {
        char nm[64];
        e_rop_name((uint16_t)r, nm, sizeof(nm));
        fprintf(f, "  \"%s\",%s", nm, (r % 8 == 7) ? "\n" : " ");
      }
    }
    fprintf(
        f,
        "\n};\n"
        "static void e_count_note(const char *hex, unsigned pc, unsigned "
        "rop,\n"
        "                          int hit)\n"
        "{\n"
        "  unsigned i;\n"
        "  for (i = 0; i < e_nsites; i++)\n"
        "    if (e_sites[i].pc == pc && e_sites[i].rop == rop &&\n"
        "        e_sites[i].hex == hex) {\n"
        "      if (hit) e_sites[i].hit++; else e_sites[i].miss++;\n"
        "      return;\n"
        "    }\n"
        "  if (e_nsites >= sizeof(e_sites) / sizeof(e_sites[0])) return;\n"
        "  i = e_nsites++;\n"
        "  e_sites[i].hex = hex; e_sites[i].pc = pc;\n"
        "  e_sites[i].rop = (uint16_t)rop;\n"
        "  e_sites[i].hit = hit ? 1 : 0; e_sites[i].miss = hit ? 0 : 1;\n"
        "}\n"
        "static void __attribute__((destructor)) e_count_dump(void)\n"
        "{\n"
        "  unsigned i;\n"
        "  for (i = 0; i < e_nsites; i++)\n"
        "    fprintf(stderr, \"site %%s pc %%u %%s hit=%%llu "
        "miss=%%llu\\n\",\n"
        "            e_sites[i].hex, e_sites[i].pc,\n"
        "            e_rop_names[e_sites[i].rop],\n"
        "            e_sites[i].hit, e_sites[i].miss);\n"
        "}\n"
        "#define E_CTR(hex, pc, rop, hit) e_count_note((hex), (pc), (rop), "
        "(hit))\n"
        /* Why q2_put_field_hit declined, so a store site that never hits can
           be explained rather than guessed at. */
        "#define E_PUT_SHAPE   0\n"
        "#define E_PUT_KIND    1\n"
        "#define E_PUT_PROTO   2\n"
        "#define E_PUT_LAYOUT  3\n"
        "#define E_PUT_N 4\n"
        "static unsigned long long e_put_why[E_PUT_N];\n"
        "static void e_put_why_note(int why) { e_put_why[why]++; }\n"
        "static const char *const e_put_why_names[E_PUT_N] = {\n"
        "  \"shape\", \"kind-not-own-w\", \"receiver-prototype\", \"layout-object\"\n"
        "};\n"
        "static void __attribute__((destructor)) e_put_why_dump(void)\n"
        "{\n"
        "  int i; unsigned long long tot = 0;\n"
        "  for (i = 0; i < E_PUT_N; i++) tot += e_put_why[i];\n"
        "  if (!tot) return;\n"
        "  for (i = 0; i < E_PUT_N; i++) {\n"
        "    if (e_put_why[i]) {\n"
        "      fprintf(stderr, \"putfield-why %%s %%llu (%%llu%%%%)\\n\",\n"
        "              e_put_why_names[i], e_put_why[i],\n"
        "              100ull * e_put_why[i] / tot);\n"
        "    }\n"
        "  }\n"
        "}\n"
        "#define E_PUTWHY(w) e_put_why_note(w)\n"
        "#else\n"
        "#define E_CTR(hex, pc, rop, hit) ((void)0)\n"
        "#define E_PUTWHY(w) ((void)0)\n"
        "#endif\n"
        /* The frame seal. The engine's frame area has a top marker, and a
           compiled function may be running on a frame above it; a native call
           made from there lets the callee's own calls into the engine allocate
           a frame at that marker, writing over the caller's registers. Sealing
           publishes this activation's frame so the marker sits above it.
           A frame on the C stack needs no seal: the marker already accounts for
           it. Once per activation, not per `goto dispatch`. */
        "#ifndef E_SEAL\n"
        "#define E_SEAL(ctx, f) do { if (!(*(uint16_t *)((uint8_t *)(f) + "
        "QJS_AOT_OFF_REGRUN_FLAGS) & QJS_AOT_REGVM_FLAG_STACK)) "
        "qjs_aot_seal(ctx, f); } while (0)\n"
        "#endif\n"
        /* The stack margin a native entry keeps free for the throw it may have
           to perform. 4 KB crashed before; 64 KB is what the call-path
           experiments used and what the design states. */
        "#ifndef E_STACK_MARGIN\n"
        "#define E_STACK_MARGIN (64 * 1024)\n"
        "#endif\n"
        /* A double local stored where another function or entry can see it is
           boxed as the interpreter's arithmetic would box it: an integral
           value that fits int32 (and is not -0) is an INT, so a guard typed
           from the profile does not see FLOAT64 for the same number. */
        /* The shape of a monomorphic group whose cells were all empty at entry:
           no object has it, so every access of the group exits. */
        /* A boxed int or bool is built in one vector register and stored as one 16-byte
           word. JS_MKVAL's own 4-byte payload store, and two 8-byte stores, are both
           followed in a frame slot by a copy that loads all 16 bytes at once, and such a
           load cannot be forwarded from the smaller stores. */
        "#if !defined(JS_NAN_BOXING) && defined(__SSE2__)\n"
        "#include <emmintrin.h>\n"
        "static inline JSValue q2_mkval(int32_t tag, int32_t val)\n"
        "{\n"
        "  __m128i x = _mm_set_epi64x((long long)tag, (long long)(uint32_t)val);\n"
        "  JSValue r;\n"
        "  __builtin_memcpy(&r, &x, sizeof r);\n"
        "  return r;\n"
        "}\n"
        "#undef JS_MKVAL\n"
        "#define JS_MKVAL(tag, val) q2_mkval((tag), (val))\n"
        "#endif\n"
        /* `this` is an object in nearly every call. The out-of-line helper writes the
           result as two words, which the caller then copies as one 16-byte load: that
           load cannot be forwarded from them and stalls. Reading the slot here avoids
           the round trip; any other `this` takes the helper. */
        "static inline int q2_load_this(JSContext *ctx, JSRegRun *f, JSValue *out)\n"
        "{\n"
        "  JSValue t = *(JSValue *)((uint8_t *)f + QJS_AOT_OFF_REGRUN_THIS_OBJ);\n"
        "  if (__builtin_expect(JS_VALUE_GET_TAG(t) == JS_TAG_OBJECT, 1)) { *out = t; return 0; }\n"
        "  return (qjs_aot_load_this)(ctx, f, out);\n"
        "}\n"
        "#define qjs_aot_load_this(ctx, f, out) q2_load_this(ctx, f, out)\n"
        /* The caller's position at a native call, for a backtrace: the callee's frame
           carries no return address. */
        "#define E_CAP(f, pc) (*(const uint8_t **)((uint8_t *)(f) + QJS_AOT_OFF_REGRUN_CAP_IP) = ecb + (pc))\n"
        "#define E_FL_TOP(ctx_) (*(JSAotFlRec **)(*(uint8_t **)((uint8_t *)(ctx_) + "
        "QJS_AOT_OFF_CTX_RT) + QJS_AOT_OFF_RT_FL_TOP))\n"
        "#ifdef QJS_AOT_RAT\n"
        "#define E_FL_PREV(c_) ((JSAotFlRec *)0)\n"
        "#define q2_fl_push(r_) ((void)(r_))\n"
        "#define E_FL_PUSH(c_, r_) ((void)0)\n"
        "#define E_FL_POP(c_, r_) ((void)0)\n"
        "#elif defined(QJS_AOT_RECSCAN)\n"
        "extern const char qjs_aot_rec_magic[];\nextern uint64_t qjs_aot_seq;\n"
        "#define E_FL_PREV(c_) ((JSAotFlRec *)qjs_aot_rec_magic)\n"
        "static inline __attribute__((always_inline)) void q2_fl_push(JSAotFlRec *r_)\n{ *(JSAotFlRec *volatile *)&r_->prev = (JSAotFlRec *)qjs_aot_rec_magic; r_->roots = (JSValue *)(uintptr_t)++qjs_aot_seq; __asm__ volatile(\"\" :: \"r\"(r_) : \"memory\"); }\n"
        "#define E_FL_PUSH(c_, r_) q2_fl_push(&(r_))\n"
        "#define E_FL_POP(c_, r_) (*(JSAotFlRec *volatile *)&(r_).prev = NULL)\n"
        "#else\n"
        "#define E_FL_PREV(c_) E_FL_TOP(c_)\n"
        "#define E_FL_PUSH(c_, r_) (E_FL_TOP(c_) = &(r_))\n"
        "#define E_FL_POP(c_, r_) (E_FL_TOP(c_) = (r_).prev)\n"
        "#endif\n"
        "static inline __attribute__((always_inline)) uintptr_t q2_sp_(void)\n"
        "{ uintptr_t s_; __asm__ volatile(\"mov %%%%rsp, %0\" : \"=r\"(s_)); return s_; }\n"
        "#define E_RLINK(rec_) ((void)0)\n"
        "#ifdef QJS_AOT_RANGE_CHECK\n"
        /* The check build keeps every slot outside the record's range poisoned (all of them at entry, a slot as it leaves the
           range) and traps when a site claims a slot it has not written: a slot that left the range and re-entered unwritten
           would make the walk pin a pointer to an object that may have been freed meanwhile. */
        "#define Q2_POISON_P ((void *)16)\n"
        "#define E_POISON_FROM(a_, n_) do { long pi_; for (pi_ = (a_); pi_ < (long)(n_); pi_++) ra_[pi_] = JS_MKPTR(JS_TAG_OBJECT, Q2_POISON_P); } while (0)\n"
        "#define E_RNG(rp_, abs_) do { JSAotFlRec *rq_ = (rp_); long a_ = (abs_), o_ = (long)rq_->nroots, b_ = (long)(ra_ - rq_->roots), i_; \\\n"
        "    for (i_ = (o_ > b_ ? o_ : b_); i_ < a_; i_++) if (JS_VALUE_GET_TAG(rq_->roots[i_]) == JS_TAG_OBJECT && JS_VALUE_GET_PTR(rq_->roots[i_]) == Q2_POISON_P) __builtin_trap(); \\\n"
        "    for (i_ = a_; i_ < o_; i_++) rq_->roots[i_] = JS_MKPTR(JS_TAG_OBJECT, Q2_POISON_P); \\\n"
        "    rq_->nroots = (int)a_; } while (0)\n"
        "#define E_RANGE(rec_, n_) E_RNG(rec_, n_)\n"
        "#define E_RCHK(rec_, k_) do { if ((long)(k_) >= (long)(rec_)->nroots) __builtin_trap(); } while (0)\n"
        "#else\n"
        "#define E_POISON_FROM(a_, n_) ((void)0)\n"
        "#define E_RNG(rp_, abs_) ((void)((rp_)->nroots = (abs_)))\n"
        "#define E_RANGE(rec_, n_) ((void)((rec_)->nroots = (n_)))\n"
        "#define E_RCHK(rec_, k_) ((void)0)\n"
        "#endif\n"
        /* Stack-scan build: the collector finds the roots on the native stack, so a site publishes no range. */
        "#ifdef QJS_AOT_STACKSCAN\n"
        "#undef E_RNG\n#undef E_RANGE\n"
        "#define E_RNG(rp_, abs_) ((void)0)\n"
        "#define E_RANGE(rec_, n_) ((void)0)\n"
        "#endif\n"
        "#define E_RLINK_REAL(rec_) do { JSAotFlRec *rl_ = (rec_); if (rl_ != NULL && !rl_->linked) { \\\n"
        "    rl_->prev = E_FL_TOP(ctx_in); rl_->func = co; rl_->pc = 0; rl_->roots = ra_; rl_->nroots = RT_; \\\n"
        "    E_FL_TOP(ctx_in) = rl_; rl_->linked = 1; } } while (0)\n"
        "#define E_RLINK_NR(rec_) do { JSAotFlRec *rl_ = (rec_); if (rl_ != NULL && !rl_->linked) { \\\n"
        "    rl_->prev = E_FL_TOP(ctx_in); rl_->func = co; rl_->pc = 0; rl_->roots = NULL; rl_->nroots = 0; \\\n"
        "    E_FL_TOP(ctx_in) = rl_; rl_->linked = 1; } } while (0)\n"
        "#ifdef QJS_AOT_STACKSCAN\n"
        "#undef E_RLINK_REAL\n#define E_RLINK_REAL(rec_) E_RLINK_NR(rec_)\n"
        "#endif\n"
        "#define E_NO_SHAPE ((const void *)1)\n"
        /* A double boxes as a float64, whatever its value: the engine's own double arithmetic does
           the same (an integral float64 is an ordinary value everywhere), and a consumer that wants
           an int takes an integral float64 through Q2_I32_OK. */
        "#define E_BOX_F64(d) __JS_NewFloat64(d)\n"
        "#define Q2_DKEY(d_) ({ double dk_ = (d_); int32_t ik_ = (dk_ >= 0.0 && dk_ <= 2147483647.0) ? (int32_t)dk_ : -1; (ik_ >= 0 && (double)ik_ == dk_ && !(dk_ == 0.0 && signbit(dk_))) ? JS_MKVAL(JS_TAG_INT, ik_) : __JS_NewFloat64(dk_); })\n"
        /* An int32 from a boxed value: an int is the one-compare fast arm; an integral float64
           (not -0, in range) converts; anything else is the caller's exit. */
        "#if defined(__SSE2__)\n"
        "#include <emmintrin.h>\n"
        "#define Q2_CVTT_I32(d) _mm_cvttsd_si32(_mm_set_sd(d))\n"
        "#else\n"
        /* arm64 fcvtzs saturates; the range test keeps the C conversion defined. */
        "#define Q2_CVTT_I32(d) ((d) > -2147483649.0 && (d) < 2147483648.0 ? (int32_t)(d) : INT32_MIN)\n"
        "#endif\n"
        "static inline __attribute__((always_inline)) int q2_to_i32_float(JSValue v, int32_t *out)\n"
        "{\n"
        "  double d;\n"
        "  int32_t i;\n"
        "  if (JS_VALUE_GET_TAG(v) != JS_TAG_FLOAT64) return 0;\n"
        "  d = JS_VALUE_GET_FLOAT64(v);\n"
        "  i = Q2_CVTT_I32(d);\n"
        "  if (!((double)i == d && (i != 0 || !signbit(d)))) return 0;\n"
        "  *out = i;\n"
        "  return 1;\n"
        "}\n"
        "#define Q2_I32_OK(v_, out_) (__builtin_expect(JS_VALUE_GET_TAG(v_) == JS_TAG_INT, 1) ? "
        "((out_) = JS_VALUE_GET_INT(v_), 1) : q2_to_i32_float((v_), &(out_)))\n"
        /* The `undefined` word a native entry fills its frame with, as one
           static constant so the fill is a copy of a known word. */
        "static const JSValue e_undefined_value = { { .int32 = 0 }, "
        "JS_TAG_UNDEFINED };\n"
        "static inline int q2_spec_fns_out(int fi);\n"
        "static inline int q2_spec_set_out(int fi);\n"
        "static void __attribute__((constructor)) q2_no_spec_init(void)\n"
        "{\n"
        "  const char *e = getenv(\"QJS_AOT_NO_SPEC\");\n"
        "  const char *s = getenv(\"QJS_AOT_SPEC\");\n"
        "  int i;\n"
        "  q2_no_spec = (e != NULL && e[0] != '0');\n"
        "  q2_spec_on = (s == NULL || s[0] != '0');\n"
        /* The switches folded per function, so a body that must be as short as
           a call can afford (a frameless one) tests one byte, not four globals. */
        "  for (i = 0; i < QJS_AOT_NFUNCS; i++)\n"
        "    e_gate[i] = e_dis[i] = (!q2_spec_on || q2_no_spec || q2_spec_fns_out(i) || q2_spec_set_out(i));\n"
        "}\n\n"
        /* The builtins a unit lowers inline: the engine's own
           function pointers, filled once, and the number view the inline C
           works on (k: 0 not a number, 1 int, 2 double, 3 undefined). */
        "typedef struct { int k; int i; double d; } Q2Num;\n"
        "static void *e_intr_fn[QJS_AOT_INTR_COUNT];\n"
        "static void __attribute__((constructor)) q2_intr_init(void)\n"
        "{\n"
        "  int i;\n"
        "  for (i = 0; i < QJS_AOT_INTR_COUNT; i++)\n"
        "    e_intr_fn[i] = qjs_aot_intrinsic_fn(i);\n"
        "}\n"
        "static inline __attribute__((always_inline)) Q2Num q2_numv(JSValue v)\n"
        "{\n"
        "  Q2Num n;\n"
        "  uint32_t t = JS_VALUE_GET_TAG(v);\n"
        "  n.k = 0; n.i = 0; n.d = 0;\n"
        "  if (t == JS_TAG_UNDEFINED) n.k = 3;\n"
        "  if (t == JS_TAG_INT) {\n"
        "    n.k = 1; n.i = JS_VALUE_GET_INT(v); n.d = (double)n.i;\n"
        "  } else if (t == JS_TAG_FLOAT64) {\n"
        "    n.k = 2; n.d = JS_VALUE_GET_FLOAT64(v);\n"
        "  }\n"
        "  return n;\n"
        "}\n"
        /* The value is the engine's builtin: a C function whose entry is the
           one the engine registered (and, for min/max, whose magic selects
           the operation). A replaced `Math.floor` fails this. */
        "static inline __attribute__((always_inline)) int q2_is_intr(JSValue c, "
        "int id, int magic)\n"
        "{\n"
        "  const unsigned char *p;\n"
        "  if (JS_VALUE_GET_TAG(c) != JS_TAG_OBJECT) return 0;\n"
        "  p = (const unsigned char *)JS_VALUE_GET_PTR(c);\n"
        "  if (*(const uint16_t *)(p + QJS_AOT_OFF_JSOBJECT_CLASS_ID) !=\n"
        "      QJS_AOT_CLASS_C_FUNCTION)\n"
        "    return 0;\n"
        "  if (*(void *const *)(p + QJS_AOT_OFF_CFUNC_C_FUNCTION) !=\n"
        "      e_intr_fn[id])\n"
        "    return 0;\n"
        "  return magic < 0 || *(const int16_t *)(p + QJS_AOT_OFF_CFUNC_MAGIC) == "
        "magic;\n"
        "}\n"
        /* js_math_round, verbatim: not C round() (it differs at -0.5 and the
           half-way cases the engine rounds up). */
        "static inline double q2_math_round(double a)\n"
        "{\n"
        "  union { double d; uint64_t u64; } u;\n"
        "  uint64_t frac_mask, one;\n"
        "  unsigned int e, s;\n"
        "  u.d = a;\n"
        "  e = (u.u64 >> 52) & 0x7ff;\n"
        "  if (e < 1023) {\n"
        "    if (e == (1023 - 1) && u.u64 != 0xbfe0000000000000) {\n"
        "      u.u64 = (u.u64 & ((uint64_t)1 << 63)) | ((uint64_t)1023 << 52);\n"
        "    } else {\n"
        "      u.u64 &= (uint64_t)1 << 63;\n"
        "    }\n"
        "  } else if (e < (1023 + 52)) {\n"
        "    s = u.u64 >> 63;\n"
        "    one = (uint64_t)1 << (52 - (e - 1023));\n"
        "    frac_mask = one - 1;\n"
        "    u.u64 += (one >> 1) - s;\n"
        "    u.u64 &= ~frac_mask;\n"
        "  }\n"
        "  return u.d;\n"
        "}\n"
        /* The inline arm of an intrinsic: 1 and a boxed result for numeric
           arguments of the shapes the engine handles the same way, 0 for
           anything else (the caller exits before the call). */
        /* A character of a flat string, read in place: `charCodeAt` (NaN past the end) and
           `codePointAt` (undefined past the end; a surrogate in a wide string is the
           builtin's). A rope, a slice or an indirect string, a non-integer index: 0. */
        /* A string for character access: a flat string as is, a rope whose linearised
           form the engine has memoised (flat on the left, empty on the right) as that flat
           string. A rope not yet linearised is the engine's, which memoises it. */
        "static inline __attribute__((always_inline)) int q2_flat_str(JSValue *v)\n"
        "{\n"
        "  const unsigned char *p;\n"
        "  JSValue l, r;\n"
        "  if (__builtin_expect(JS_VALUE_GET_TAG(*v) == JS_TAG_STRING, 1)) return 1;\n"
        "  if (JS_VALUE_GET_TAG(*v) != JS_TAG_STRING_ROPE) return 0;\n"
        "  p = (const unsigned char *)JS_VALUE_GET_PTR(*v);\n"
        "  l = *(const JSValue *)(p + QJS_AOT_OFF_ROPE_LEFT);\n"
        "  r = *(const JSValue *)(p + QJS_AOT_OFF_ROPE_RIGHT);\n"
        "  if (JS_VALUE_GET_TAG(l) != JS_TAG_STRING || JS_VALUE_GET_TAG(r) != JS_TAG_STRING) return 0;\n"
        "  if ((*(const uint32_t *)JS_VALUE_GET_PTR(r) & QJS_AOT_STRING_LEN_MASK) != 0) return 0;\n"
        "  *v = l;\n"
        "  return 1;\n"
        "}\n"
        "static inline __attribute__((always_inline)) int q2_intr_str(JSContext *ctx, int id, "
        "JSValue s, Q2Num a, JSValue *out)\n"
        "{\n"
        "  const unsigned char *p;\n"
        "  uint32_t w0, w1, c;\n"
        "  if (!q2_flat_str(&s) || a.k != 1) return 0;\n"
        "  p = (const unsigned char *)JS_VALUE_GET_PTR(s);\n"
        "  w0 = *(const uint32_t *)p;\n"
        "  w1 = *(const uint32_t *)(p + 4);\n"
        "  if (w1 & QJS_AOT_STRING_KIND_MASK) return 0;\n"
        "  if ((uint32_t)a.i >= (w0 & QJS_AOT_STRING_LEN_MASK)) {\n"
        "    if (id == QJS_AOT_INTR_STR_CHARAT) return 0;\n"
        "    *out = id == QJS_AOT_INTR_STR_CHARCODEAT ? JS_NAN : JS_UNDEFINED;\n"
        "    return 1;\n"
        "  }\n"
        "  if (w0 & QJS_AOT_STRING_WIDE_MASK) {\n"
        "    c = ((const uint16_t *)(p + QJS_AOT_SIZEOF_JSSTRING))[a.i];\n"
        "    if (id == QJS_AOT_INTR_STR_CODEPOINTAT && c >= 0xD800u && c <= 0xDFFFu)\n"
        "      return 0;\n"
        "  } else {\n"
        "    c = ((const uint8_t *)(p + QJS_AOT_SIZEOF_JSSTRING))[a.i];\n"
        "  }\n"
        "  if (id == QJS_AOT_INTR_STR_CHARAT) {\n"
        "    /* The runtime's one-character string, borrowed (the table holds it). */\n"
        "    void *const *tab = (void *const *)(*(const unsigned char *const *)((const unsigned char *)ctx +\n"
        "                                       QJS_AOT_OFF_CTX_RT) + QJS_AOT_OFF_RT_ONECHAR);\n"
        "    if (c >= 256u || tab[c] == NULL) return 0;\n"
        "    *out = JS_MKPTR(JS_TAG_STRING, tab[c]);\n"
        "    return 1;\n"
        "  }\n"
        "  *out = JS_MKVAL(JS_TAG_INT, (int32_t)c);\n"
        "  return 1;\n"
        "}\n"
        "static inline __attribute__((always_inline)) int q2_intr(int id, "
        "Q2Num a, Q2Num b, JSValue *out)\n"
        "{\n"
        "  double r;\n"
        "  if (a.k != 1 && a.k != 2) return 0;\n"
        "  switch (id) {\n"
        "  case QJS_AOT_INTR_SQRT: r = sqrt(a.d); break;\n"
        "  case QJS_AOT_INTR_SIN: r = sin(a.d); break;\n"
        "  case QJS_AOT_INTR_COS: r = cos(a.d); break;\n"
        "  case QJS_AOT_INTR_ABS: r = fabs(a.d); break;\n"
        "  case QJS_AOT_INTR_FLOOR:\n"
        "    if (a.k == 1) { *out = JS_MKVAL(JS_TAG_INT, a.i); return 1; }\n"
        "    r = floor(a.d); break;\n"
        "  case QJS_AOT_INTR_CEIL:\n"
        "    if (a.k == 1) { *out = JS_MKVAL(JS_TAG_INT, a.i); return 1; }\n"
        "    r = ceil(a.d); break;\n"
        "  case QJS_AOT_INTR_ROUND:\n"
        "    if (a.k == 1) { *out = JS_MKVAL(JS_TAG_INT, a.i); return 1; }\n"
        "    r = q2_math_round(a.d); break;\n"
        "  case QJS_AOT_INTR_MIN:\n"
        "  case QJS_AOT_INTR_MAX: {\n"
        "    int mx = id == QJS_AOT_INTR_MAX;\n"
        "    if (b.k != 1 && b.k != 2) return 0;\n"
        "    if (a.k == 1 && b.k == 1) {\n"
        "      int x = mx ? (a.i > b.i ? a.i : b.i) : (a.i < b.i ? a.i : b.i);\n"
        "      *out = JS_MKVAL(JS_TAG_INT, x);\n"
        "      return 1;\n"
        "    }\n"
        "    if (a.d != a.d) r = a.d;\n"
        "    else if (b.d != b.d) r = b.d;\n"
        "    else if (a.d == 0 && b.d == 0) {\n"
        "      union { double d; uint64_t u; } x, y;\n"
        "      x.d = a.d; y.d = b.d;\n"
        "      if (mx) x.u &= y.u; else x.u |= y.u;\n"
        "      r = x.d;\n"
        "    } else if (mx) r = a.d < b.d ? b.d : a.d;\n"
        "    else r = a.d < b.d ? a.d : b.d;\n"
        "    break;\n"
        "  }\n"
        "  case QJS_AOT_INTR_PARSEINT:\n"
        /* radix absent, 0 or 10; an int is its own parse, an integral double
           below 2^53 prints as that integer (and -0 prints as 0). */
        "    if (b.k != 3 && !(b.k == 1 && (b.i == 0 || b.i == 10))) return 0;\n"
        "    if (a.k == 1) { *out = JS_MKVAL(JS_TAG_INT, a.i); return 1; }\n"
        "    if (!(fabs(a.d) < 9007199254740992.0) || a.d != floor(a.d)) return 0;\n"
        "    r = a.d + 0.0; break;\n"
        "  default: return 0;\n"
        "  }\n"
        "  *out = E_BOX_F64(r);\n"
        "  return 1;\n"
        "}\n\n"
        /* A site's polymorphic cache: four (shape, offset, kind) entries the
           compiled body fills on demand; NULL until the function has used the
           site. */
        "static inline const void *q2_poly(JSRegRun *f, int site)\n"
        "{\n"
        "  /* q2_b(f) is already the bytecode pointer; dereferencing it again "
        "reads\n"
        "     the struct's GC header as an address. */\n"
        "  const unsigned char *b = (const unsigned char *)q2_b(f);\n"
        "  const void *p = *(const void *const *)(b + "
        "QJS_AOT_OFF_FNBC_AOT_POLY);\n"
        "  if (p == NULL) return NULL;\n"
        "  if ((uint32_t)site >= *(const uint32_t *)(b + "
        "QJS_AOT_OFF_FNBC_AOT_POLY_SITES)) return NULL;\n"
        "  return (const unsigned char *)p + (size_t)site * "
        "QJS_AOT_AOT_POLY_WAYS * 16;\n"
        "}\n\n"
        "static inline const void *q2_poly_shape(const void *e, int k)\n"
        "{\n"
        "  return *(const void *const *)((const unsigned char *)e + (size_t)k "
        "* 16);\n"
        "}\n\n"
        "static inline int q2_poly_kind(const void *e, int k)\n"
        "{\n"
        "  return *(const uint16_t *)((const unsigned char *)e + (size_t)k * "
        "16 + 12);\n"
        "}\n\n"
        "static inline uint32_t q2_poly_off(const void *e, int k)\n"
        "{\n"
        "  return *(const uint32_t *)((const unsigned char *)e + (size_t)k * "
        "16 + 8);\n"
        "}\n\n"
        /* A site's cache cell, read at the specialised body's entry: the
           receiver shape the profile established and the slot offset that came
           with it. NULL/0 unless the cell is an own (read) or own-writable
           (write) hit, because only those mean "this shape, this offset". */
        "#ifdef E_LOGO_COLD\n"
        "static __attribute__((noinline, cold)) const void *q2_cell_shape(const void *pic, int k)\n"
        "#else\n"
        "static inline const void *q2_cell_shape(const void *pic, int k)\n"
        "#endif\n"
        "{\n"
        "  const unsigned char *e;\n"
        "  uint16_t kind;\n"
        "  if (pic == NULL || (unsigned)k == QJS_AOT_REG_IC_NONE) return "
        "NULL;\n"
        "  e = (const unsigned char *)pic + (size_t)k * "
        "QJS_AOT_SIZEOF_JSREGPIC;\n"
        "  kind = *(const uint16_t *)(e + QJS_AOT_OFF_JSREGPIC_KIND);\n"
        "  if (kind != QJS_AOT_REG_PIC_OWN && kind != QJS_AOT_REG_PIC_OWN_W &&\n"
        "      kind != QJS_AOT_REG_PIC_PROTO && kind != QJS_AOT_REG_PIC_PROTO2) "
        "return NULL;\n"
        "  return *(const void *const *)e;\n"
        "}\n\n"
        /* The shape word of a cell the entry validated: a group's accesses compare it with the
           group's shape, and a shape names the property's place, so the kind needs no re-test. */
        "static inline const void *q2_cell_raw(const void *pic, int k)\n"
        "{\n"
        "  return *(const void *const *)((const unsigned char *)pic + (size_t)(unsigned)k * "
        "QJS_AOT_SIZEOF_JSREGPIC);\n"
        "}\n\n"
        "#ifdef E_LOGO_COLD\n"
        "static __attribute__((noinline, cold)) const void *q2_cell_wshape(const void *pic, int k)\n"
        "#else\n"
        "static inline const void *q2_cell_wshape(const void *pic, int k)\n"
        "#endif\n"
        "{\n"
        "  const unsigned char *e;\n"
        "  if (pic == NULL || (unsigned)k == QJS_AOT_REG_IC_NONE) return "
        "NULL;\n"
        "  e = (const unsigned char *)pic + (size_t)k * "
        "QJS_AOT_SIZEOF_JSREGPIC;\n"
        "  if (*(const uint16_t *)(e + QJS_AOT_OFF_JSREGPIC_KIND) !=\n"
        "      QJS_AOT_REG_PIC_OWN_W)\n"
        "    return NULL;\n"
        "  return *(const void *const *)e;\n"
        "}\n\n"
        "static inline uint32_t q2_cell_off(const void *pic, int k)\n"
        "{\n"
        "  const unsigned char *e;\n"
        "  if (pic == NULL || (unsigned)k == QJS_AOT_REG_IC_NONE) return 0;\n"
        "  e = (const unsigned char *)pic + (size_t)k * "
        "QJS_AOT_SIZEOF_JSREGPIC;\n"
        "  return *(const uint32_t *)(e + QJS_AOT_OFF_JSREGPIC_SLOT_OFF);\n"
        "}\n\n"
        "static inline const void *q2_shape_of(JSValue v)\n"
        "{\n"
        "  if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT) return NULL;\n"
        "  return *(const void *const *)((const unsigned char "
        "*)JS_VALUE_GET_PTR(v) +\n"
        "                                QJS_AOT_OFF_JSOBJECT_SHAPE);\n"
        "}\n\n"
        "static inline __attribute__((always_inline)) int q2_shape_ne(JSValue v, const void *sh)\n"
        "{\n"
        "  return JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT ||\n"
        "         *(const void *const *)((const unsigned char *)JS_VALUE_GET_PTR(v) +\n"
        "                                QJS_AOT_OFF_JSOBJECT_SHAPE) != sh;\n"
        "}\n\n"
        "/* `a instanceof b` where b is a plain function: the proto-chain walk in place. The site's cache `ic` holds the shape of\n"
        "   such a function (qjs_aot_instof_fill), so a function of that shape has its prototype in slot 2 and no @@hasInstance. */\n"
        "static inline __attribute__((always_inline)) int q2_instanceof(JSContext *ctx, JSValue a, JSValue b, void **ic)\n"
        "{\n"
        "  if (__builtin_expect(JS_VALUE_GET_TAG(b) == JS_TAG_OBJECT, 1)) {\n"
        "    const unsigned char *ob = (const unsigned char *)JS_VALUE_GET_PTR(b);\n"
        "    if (__builtin_expect(*(const void *const *)(ob + QJS_AOT_OFF_JSOBJECT_SHAPE) == ic[0] &&\n"
        "                         *(const uint16_t *)(ob + QJS_AOT_OFF_JSOBJECT_CLASS_ID) == QJS_AOT_CLASS_BYTECODE_FUNCTION, 1)) {\n"
        "      JSValue pv = *(const JSValue *)(*(const unsigned char *const *)(ob + QJS_AOT_OFF_JSOBJECT_PROP) + 2 * QJS_AOT_SIZEOF_JSPROPERTY);\n"
        "      if (__builtin_expect(JS_VALUE_GET_TAG(pv) == JS_TAG_OBJECT, 1)) {\n"
        "        const unsigned char *p, *proto;\n"
        "        if (JS_VALUE_GET_TAG(a) != JS_TAG_OBJECT) return 0;\n"
        "        proto = (const unsigned char *)JS_VALUE_GET_PTR(pv);\n"
        "        p = (const unsigned char *)JS_VALUE_GET_PTR(a);\n"
        "        for (;;) {\n"
        "          const unsigned char *p1 = *(const unsigned char *const *)(*(const unsigned char *const *)(p + QJS_AOT_OFF_JSOBJECT_SHAPE) + QJS_AOT_OFF_JSSHAPE_PROTO);\n"
        "          if (p1 == proto) return 1;\n"
        "          if (!p1) return *(const uint16_t *)(p + QJS_AOT_OFF_JSOBJECT_CLASS_ID) == QJS_AOT_CLASS_PROXY ? qjs_aot_instanceof(ctx, a, b) : 0;\n"
        "          p = p1;\n"
        "        }\n"
        "      }\n"
        "    } else if ((uintptr_t)ic[1] < 8) {\n"
        "      /* learn the function's shape (a few misses at most) */\n"
        "      ic[1] = (void *)((uintptr_t)ic[1] + 1);\n"
        "      qjs_aot_instof_fill(ctx, b, ic);\n"
        "    }\n"
        "  }\n"
        "  return qjs_aot_instanceof(ctx, a, b);\n"
        "}\n\n"
        "#ifndef QJS_AOT_NODIRECT\n"
        "_Static_assert(QJS_AOT_JSOBJECT_INLINE_CAP_MASK == 0xF800u && QJS_AOT_CLASS_OBJECT == 1u && QJS_AOT_OFF_JSOBJECT_CLASS_ID == QJS_AOT_OFF_JSOBJECT_FLAGS16 + 2u, \"direct field addressing: the header word layout changed\");\n"
        "#endif\n"
        "static inline __attribute__((always_inline)) JSValue *q2_slotd(JSValue v, uint32_t off)\n"
        "{ return (JSValue *)((const unsigned char *)JS_VALUE_GET_PTR(v) + QJS_AOT_OFF_OBJECT_U + off); }\n"
        "static inline __attribute__((always_inline)) const unsigned char *q2_propd(JSValue v)\n"
        "{ return (const unsigned char *)JS_VALUE_GET_PTR(v) + QJS_AOT_OFF_OBJECT_U; }\n"
        "static inline JSValue *q2_slotp(JSValue v, uint32_t off)\n"
        "{\n"
        "  const unsigned char *p = (const unsigned char "
        "*)JS_VALUE_GET_PTR(v);\n"
        /* Direct field addressing: a plain object whose properties live in its own block (class OBJECT with a non-zero inline capacity) has them at
           a fixed offset from the object, so the slot address does not depend on the load of p->prop. The word at p+16 holds the flag bits, the
           5-bit capacity (bits 11..15) and the class id (bits 16..31); capacity 1..31 with class 1 is the range 0x10800..0x1F800 of the word with
           its low 11 bits cleared. Any other object takes the pointer. */
        "#ifndef QJS_AOT_NODIRECT\n"
        "  {\n"
        "    uint32_t w_ = *(const uint32_t *)(p + QJS_AOT_OFF_JSOBJECT_FLAGS16);\n"
        "    if (__builtin_expect(((w_ & 0xFFFFF800u) - 0x10800u) <= 0xF000u, 1))\n"
        "      return (JSValue *)(p + QJS_AOT_OFF_OBJECT_U + off);\n"
        "  }\n"
        "#endif\n"
        "  const unsigned char *prop =\n"
        "      *(const unsigned char *const *)(p + "
        "QJS_AOT_OFF_JSOBJECT_PROP);\n"
        "  return (JSValue *)(prop + off);\n"
        "}\n\n"
        /* A prototype read: the holder is the receiver's shape's prototype. The test is that
           the holder's own shape is still the one the cell was filled with. */
        "static inline JSValue *q2_hslotp(JSValue v, uint32_t off)\n"
        "{\n"
        "  const unsigned char *h = *(const unsigned char *const *)(\n"
        "      (const unsigned char *)q2_shape_of(v) + QJS_AOT_OFF_JSSHAPE_PROTO);\n"
        "  const unsigned char *prop =\n"
        "      *(const unsigned char *const *)(h + QJS_AOT_OFF_JSOBJECT_PROP);\n"
        "  return (JSValue *)(prop + off);\n"
        "}\n"
        "static inline JSValue *q2_hslotp_sh(const void *sh, uint32_t off)\n"
        "{\n"
        "  const unsigned char *h = *(const unsigned char *const *)(\n"
        "      (const unsigned char *)sh + QJS_AOT_OFF_JSSHAPE_PROTO);\n"
        "  const unsigned char *prop =\n"
        "      *(const unsigned char *const *)(h + QJS_AOT_OFF_JSOBJECT_PROP);\n"
        "  return (JSValue *)(prop + off);\n"
        "}\n"
        /* A holder two prototypes up: the cache entry sits in way 1 or above at index `ci`, its
           holder shape in holders[ci] and the middle object's shape in the second half. */
        "static inline const void *q2_cell2_shape(const void *pic, int site, int nsites)\n"
        "{\n"
        "  unsigned k;\n"
        "  if (pic == NULL) return NULL;\n"
        "  for (k = 1; k < QJS_AOT_REG_PIC_WAYS; k++) {\n"
        "    const unsigned char *e = (const unsigned char *)pic +\n"
        "        ((size_t)k * (size_t)nsites + (size_t)site) * QJS_AOT_SIZEOF_JSREGPIC;\n"
        "    if (*(const uint16_t *)(e + QJS_AOT_OFF_JSREGPIC_KIND) == QJS_AOT_REG_PIC_PROTO2)\n"
        "      return *(const void *const *)e;\n"
        "  }\n"
        "  return NULL;\n"
        "}\n"
        "static inline int q2_cell2_index(const void *pic, int site, int nsites, const void *sh)\n"
        "{\n"
        "  unsigned k;\n"
        "  if (pic == NULL) return (int)QJS_AOT_REG_IC_NONE;\n"
        "  for (k = 1; k < QJS_AOT_REG_PIC_WAYS; k++) {\n"
        "    const unsigned char *e = (const unsigned char *)pic +\n"
        "        ((size_t)k * (size_t)nsites + (size_t)site) * QJS_AOT_SIZEOF_JSREGPIC;\n"
        "    if (*(const uint16_t *)(e + QJS_AOT_OFF_JSREGPIC_KIND) == QJS_AOT_REG_PIC_PROTO2 &&\n"
        "        *(const void *const *)e == sh)\n"
        "      return (int)(k * (unsigned)nsites + (unsigned)site);\n"
        "  }\n"
        "  return (int)QJS_AOT_REG_IC_NONE;\n"
        "}\n"
        /* The number of distinct shapes the ways of a site's cache hold (an entry refusal's report). */
        "static __attribute__((noinline, cold, unused)) unsigned e_pic_nshapes(const void *pic, int site, int nsites)\n"
        "{\n"
        "  const void *seen[QJS_AOT_REG_PIC_WAYS];\n"
        "  unsigned n = 0, k, j;\n"
        "  if (pic == NULL) return 0;\n"
        "  for (k = 0; k < QJS_AOT_REG_PIC_WAYS; k++) {\n"
        "    const unsigned char *e = (const unsigned char *)pic +\n"
        "        ((size_t)k * (size_t)nsites + (size_t)site) * QJS_AOT_SIZEOF_JSREGPIC;\n"
        "    uint16_t kind = *(const uint16_t *)(e + QJS_AOT_OFF_JSREGPIC_KIND);\n"
        "    const void *sh;\n"
        "    if (kind != QJS_AOT_REG_PIC_OWN && kind != QJS_AOT_REG_PIC_OWN_W && kind != QJS_AOT_REG_PIC_PROTO && kind != QJS_AOT_REG_PIC_PROTO2) continue;\n"
        "    sh = *(const void *const *)e;\n"
        "    for (j = 0; j < n && seen[j] != sh; j++) {}\n"
        "    if (j == n) seen[n++] = sh;\n"
        "  }\n"
        "  return n;\n"
        "}\n"
        /* The first time an exit stub at a property site is taken (QJS_AOT_STATS=1): what the site's cache ways held against the shape the
           body assumed, so a guard that fails on every call can be told from a stale or polymorphic cache. */
        "static int e_why_v = -1;\n"
        /* The number of the guard that sent the activation to its exit stub (E_GOTO sets it; the generator numbers them and lists them in <unit>.guards). */
        "#ifdef QJS_AOT_SPLIT_UNIT\nextern int e_xw;\n#else\nstatic int e_xw;\n#endif\n"
        "#define E_GOTO(k_, pc_, ...) do { e_xw = (0 + __VA_ARGS__ +0); goto k_##_##pc_; } while (0)\n"
        "static __attribute__((noinline, cold, unused)) void e_why_pic(const char *hex, int pc, const char *op, const void *pic, int site, int nsites, const void *assumed, int gl)\n"
        "{\n"
        "  unsigned k, held = 0;\n"
        "  if (e_why_v < 0) e_why_v = getenv(\"QJS_AOT_STATS\") != NULL;\n"
        "  if (!e_why_v) return;\n"
        "  if (pic == NULL) { fprintf(stderr, \"unit-why fn=%%s pc=%%d op=%%s guard=%%d\\n\", hex, pc, op, gl); return; }\n"
        "  fprintf(stderr, \"unit-why fn=%%s pc=%%d op=%%s guard=%%d site=%%d assumed=%%p shapes=%%u\", hex, pc, op, gl, site, assumed, e_pic_nshapes(pic, site, nsites));\n"
        "  for (k = 0; k < QJS_AOT_REG_PIC_WAYS; k++) {\n"
        "    const unsigned char *e = (const unsigned char *)pic + ((size_t)k * (size_t)nsites + (size_t)site) * QJS_AOT_SIZEOF_JSREGPIC;\n"
        "    const void *sh = *(const void *const *)e;\n"
        "    if (assumed != NULL && sh == assumed) held = 1;\n"
        "    fprintf(stderr, \" way%%u=%%p/kind%%u/off%%u\", k, sh, (unsigned)*(const uint16_t *)(e + QJS_AOT_OFF_JSREGPIC_KIND), (unsigned)*(const uint32_t *)(e + QJS_AOT_OFF_JSREGPIC_SLOT_OFF));\n"
        "  }\n"
        "  fprintf(stderr, \" assumed-held=%%u\\n\", held);\n"
        "}\n"
        "static void __attribute__((noinline, cold, unused, preserve_most)) e_why_c(E_WhyD *d, const void *pic, int site, int ns, const void *assumed)\n"
        "{\n"
        "  if ((e_xw != d->last || d->n == 0) && d->n < 8) { d->last = e_xw; d->n++; e_why_pic(d->hex, d->pc, d->op, pic, site, ns, assumed, e_xw); }\n"
        "  e_xw = 0;\n"
        "}\n"
        "#define E_EXIT_WHY(hex_, pc_, op_, pic_, site_, ns_, assumed_) do { static E_WhyD e_wd_ = {(hex_), (op_), (pc_), 0, 0}; \\\n"
        "    e_why_c(&e_wd_, (pic_), (site_), (ns_), (assumed_)); } while (0)\n"
        "static inline int q2_holder2_bad_sh(const void *sh, void *const *holders, int nsites, int ci)\n"
        "{\n"
        "  const unsigned char *p1, *p2;\n"
        "  if ((unsigned)ci == QJS_AOT_REG_IC_NONE) return 1;\n"
        "  p1 = *(const unsigned char *const *)((const unsigned char *)sh + QJS_AOT_OFF_JSSHAPE_PROTO);\n"
        "  if (p1 == NULL ||\n"
        "      *(const void *const *)(p1 + QJS_AOT_OFF_JSOBJECT_SHAPE) !=\n"
        "          holders[(size_t)QJS_AOT_REG_PIC_WAYS * (size_t)nsites + (size_t)ci])\n"
        "    return 1;\n"
        "  p2 = *(const unsigned char *const *)(*(const unsigned char *const *)(p1 +\n"
        "           QJS_AOT_OFF_JSOBJECT_SHAPE) + QJS_AOT_OFF_JSSHAPE_PROTO);\n"
        "  return p2 == NULL || *(const void *const *)(p2 + QJS_AOT_OFF_JSOBJECT_SHAPE) != holders[ci];\n"
        "}\n"
        "static inline int q2_holder2_bad(JSValue v, void *const *holders, int nsites, int ci)\n"
        "{\n"
        "  return q2_holder2_bad_sh(q2_shape_of(v), holders, nsites, ci);\n"
        "}\n"
        "static inline JSValue *q2_hslot2p(JSValue v, uint32_t off)\n"
        "{\n"
        "  const unsigned char *p1 = *(const unsigned char *const *)(\n"
        "      (const unsigned char *)q2_shape_of(v) + QJS_AOT_OFF_JSSHAPE_PROTO);\n"
        "  const unsigned char *p2 = *(const unsigned char *const *)(*(const unsigned char *const *)(p1 +\n"
        "           QJS_AOT_OFF_JSOBJECT_SHAPE) + QJS_AOT_OFF_JSSHAPE_PROTO);\n"
        "  const unsigned char *prop = *(const unsigned char *const *)(p2 + QJS_AOT_OFF_JSOBJECT_PROP);\n"
        "  return (JSValue *)(prop + off);\n"
        "}\n"
        "static inline int q2_nullish_obj(JSValue v)\n"
        "{\n"
        "  return (((const unsigned char *)JS_VALUE_GET_PTR(v))[QJS_AOT_OFF_JSOBJECT_HTMLDDA_BYTE] &\n"
        "          QJS_AOT_JSOBJECT_HTMLDDA_MASK) != 0;\n"
        "}\n"
        "static inline int q2_holder_bad_sh(const void *sh, void *const *holders, int site)\n"
        "{\n"
        "  const unsigned char *h = *(const unsigned char *const *)(\n"
        "      (const unsigned char *)sh + QJS_AOT_OFF_JSSHAPE_PROTO);\n"
        "  return h == NULL || *(const void *const *)(h + QJS_AOT_OFF_JSOBJECT_SHAPE) != holders[site];\n"
        "}\n"
        "static inline int q2_holder_bad(JSValue v, void *const *holders, int site)\n"
        "{\n"
        "  const unsigned char *h = *(const unsigned char *const *)(\n"
        "      (const unsigned char *)q2_shape_of(v) + QJS_AOT_OFF_JSSHAPE_PROTO);\n"
        "  return h == NULL || *(const void *const *)(h + QJS_AOT_OFF_JSOBJECT_SHAPE) != holders[site];\n"
        "}\n\n"
        /* A fast array's element read: class, bounds and hole checks,
           then the element itself, borrowed. No reference is taken,
           so the caller must consume it before the next safepoint, exactly
           like the cache-hit field read. */
        /* An element of a typed array, in range: the engine keeps the element
           pointer and the count where a dense array keeps its values and count, and
           a detached or shrunken buffer has its count at zero. */
        /* A method on String.prototype read through a string receiver: the prototype's
           shape and the property's offset are kept per site, and the current value
           is read while the shape is unchanged. */
        "typedef struct Q2StrCell { const void *shape; const void *prs; uint32_t off; uint32_t tried; } Q2StrCell;\n"
        "static inline __attribute__((always_inline)) int q2_strproto_hit(JSContext *ctx, "
        "JSValue recv, Q2StrCell *c, uint32_t atom, JSValue *out)\n"
        "{\n"
        "  JSValue pv;\n"
        "  if (JS_VALUE_GET_TAG(recv) != JS_TAG_STRING) return 0;\n"
        "  pv = (*(JSValue *const *)((const unsigned char *)ctx + "
        "QJS_AOT_OFF_CTX_CLASS_PROTO))[QJS_AOT_CLASS_STRING];\n"
        "  if (JS_VALUE_GET_TAG(pv) != JS_TAG_OBJECT) return 0;\n"
        "  if (__builtin_expect(q2_shape_of(pv) != c->shape || c->prs == NULL ||\n"
        "      ((const uint32_t *)c->prs)[1] != atom ||\n"
        "      (((const uint32_t *)c->prs)[0] & QJS_AOT_SHAPE_PROP_TMASK) != 0, 0)) {\n"
        "    if (c->tried > 8 || !qjs_aot_strproto_slot(ctx, atom, &c->shape, &c->prs, &c->off)) {\n"
        "      c->prs = NULL;\n"
        "      c->tried++;\n"
        "      return 0;\n"
        "    }\n"
        "  }\n"
        "  *out = *q2_slotp(pv, c->off);\n"
        "  return 1;\n"
        "}\n"
        "#define E_TA_F64(d_) ({ double e_ = (d_); qjs_aot_box_num(e_, qjs_aot_fitsi(e_)); })\n"
        "static inline __attribute__((always_inline)) int q2_ta_get(const "
        "unsigned char *p, unsigned cid, uint32_t i, JSValue *out)\n"
        "{\n"
        "  const void *base;\n"
        "  if (i >= *(const uint32_t *)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_COUNT)) "
        "return 0;\n"
        "  base = *(const void *const *)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_VALUES);\n"
        "  switch (cid) {\n"
        "  case QJS_AOT_CLASS_UINT8C_ARRAY:\n"
        "  case QJS_AOT_CLASS_UINT8_ARRAY:\n"
        "    *out = JS_MKVAL(JS_TAG_INT, ((const uint8_t *)base)[i]); return 1;\n"
        "  case QJS_AOT_CLASS_INT8_ARRAY:\n"
        "    *out = JS_MKVAL(JS_TAG_INT, ((const int8_t *)base)[i]); return 1;\n"
        "  case QJS_AOT_CLASS_INT16_ARRAY:\n"
        "    *out = JS_MKVAL(JS_TAG_INT, ((const int16_t *)base)[i]); return 1;\n"
        "  case QJS_AOT_CLASS_UINT16_ARRAY:\n"
        "    *out = JS_MKVAL(JS_TAG_INT, ((const uint16_t *)base)[i]); return 1;\n"
        "  case QJS_AOT_CLASS_INT32_ARRAY:\n"
        "    *out = JS_MKVAL(JS_TAG_INT, ((const int32_t *)base)[i]); return 1;\n"
        "  case QJS_AOT_CLASS_UINT32_ARRAY: {\n"
        "    uint32_t x = ((const uint32_t *)base)[i];\n"
        "    *out = x <= 2147483647u ? JS_MKVAL(JS_TAG_INT, (int32_t)x) : "
        "qjs_aot_box_num((double)x, 0);\n"
        "    return 1; }\n"
        "  case QJS_AOT_CLASS_FLOAT32_ARRAY:\n"
        "    *out = E_TA_F64((double)((const float *)base)[i]); return 1;\n"
        "  case QJS_AOT_CLASS_FLOAT64_ARRAY:\n"
        "    *out = E_TA_F64(((const double *)base)[i]); return 1;\n"
        "  default: return 0;\n"
        "  }\n"
        "}\n\n"
        /* `s[i]` on a string: the runtime's one-character string for a character
           below 256, borrowed (the table holds it). A wide string, a surrogate-free
           character past Latin-1, a rope and an index out of range take the engine. */
        /* Equality of two strings, when it can be decided from their bytes: the same
           object, a different length, or two flat strings of one width compared byte
           for byte. A rope or a pair of different widths is the engine's. */
        /* Order of two flat strings, whatever their widths: the first differing code
           unit decides, else the shorter is less. A rope is the engine's. */
        "static inline __attribute__((always_inline)) int q2_str_cmp(JSValue a, JSValue b, int *cmp)\n"
        "{\n"
        "  const unsigned char *pa, *pb, *da, *db;\n"
        "  uint32_t a0, a1, b0, b1, na, nb, n, i;\n"
        "  int wa, wb;\n"
        "  if (JS_VALUE_GET_TAG(a) != JS_TAG_STRING || JS_VALUE_GET_TAG(b) != JS_TAG_STRING) return 0;\n"
        "  pa = (const unsigned char *)JS_VALUE_GET_PTR(a);\n"
        "  pb = (const unsigned char *)JS_VALUE_GET_PTR(b);\n"
        "  a0 = *(const uint32_t *)pa; a1 = *(const uint32_t *)(pa + 4);\n"
        "  b0 = *(const uint32_t *)pb; b1 = *(const uint32_t *)(pb + 4);\n"
        "  if ((a1 | b1) & QJS_AOT_STRING_KIND_MASK) return 0;\n"
        "  na = a0 & QJS_AOT_STRING_LEN_MASK; nb = b0 & QJS_AOT_STRING_LEN_MASK;\n"
        "  wa = (a0 & QJS_AOT_STRING_WIDE_MASK) != 0; wb = (b0 & QJS_AOT_STRING_WIDE_MASK) != 0;\n"
        "  da = pa + QJS_AOT_SIZEOF_JSSTRING; db = pb + QJS_AOT_SIZEOF_JSSTRING;\n"
        "  n = na < nb ? na : nb;\n"
        "  if (!wa && !wb) {\n"
        "    int r = __builtin_memcmp(da, db, n);\n"
        "    if (r != 0) { *cmp = r < 0 ? -1 : 1; return 1; }\n"
        "  } else {\n"
        "    for (i = 0; i < n; i++) {\n"
        "      uint32_t ca = wa ? ((const uint16_t *)da)[i] : da[i];\n"
        "      uint32_t cb = wb ? ((const uint16_t *)db)[i] : db[i];\n"
        "      if (ca != cb) { *cmp = ca < cb ? -1 : 1; return 1; }\n"
        "    }\n"
        "  }\n"
        "  *cmp = na < nb ? -1 : (na > nb ? 1 : 0);\n"
        "  return 1;\n"
        "}\n"
        /* A string against a number: the string is read as a decimal integer when it is
           one to nine digits (what ToNumber gives, exactly), and the numbers compare. `k` is
           0 `<`, 1 `<=`, 2 `>`, 3 `>=`, 4 `==`, 5 `!=`, 6 `===`, 7 `!==`. Strict equality
           of a string and a number is decided by the types alone. Two strings, two numbers
           and every other mix are not decided here. */
        "static inline __attribute__((always_inline)) int q2_mixed_cmp(JSValue a, JSValue b, int k, int *c)\n"
        "{\n"
        "  int ta = JS_VALUE_GET_TAG(a), tb = JS_VALUE_GET_TAG(b);\n"
        "  int na = ta == JS_TAG_INT || ta == JS_TAG_FLOAT64, nb = tb == JS_TAG_INT || tb == JS_TAG_FLOAT64;\n"
        "  double x, y;\n"
        "  JSValue sv;\n"
        "  const unsigned char *p;\n"
        "  uint32_t w0, i, n, v;\n"
        "  if (!((ta == JS_TAG_STRING && nb) || (na && tb == JS_TAG_STRING))) return 0;\n"
        "  if (k >= 6) { *c = k == 7; return 1; }\n"
        "  sv = ta == JS_TAG_STRING ? a : b;\n"
        "  p = (const unsigned char *)JS_VALUE_GET_PTR(sv);\n"
        "  w0 = *(const uint32_t *)p;\n"
        "  if ((*(const uint32_t *)(p + 4) & QJS_AOT_STRING_KIND_MASK) || (w0 & QJS_AOT_STRING_WIDE_MASK)) return 0;\n"
        "  n = w0 & QJS_AOT_STRING_LEN_MASK;\n"
        "  if (n == 0 || n > 9) return 0;\n"
        "  v = 0;\n"
        "  for (i = 0; i < n; i++) {\n"
        "    unsigned d = p[QJS_AOT_SIZEOF_JSSTRING + i] - '0';\n"
        "    if (d > 9) return 0;\n"
        "    v = v * 10 + d;\n"
        "  }\n"
        "  x = ta == JS_TAG_STRING ? (double)v : (ta == JS_TAG_INT ? (double)JS_VALUE_GET_INT(a) : JS_VALUE_GET_FLOAT64(a));\n"
        "  y = tb == JS_TAG_STRING ? (double)v : (tb == JS_TAG_INT ? (double)JS_VALUE_GET_INT(b) : JS_VALUE_GET_FLOAT64(b));\n"
        "  switch (k) {\n"
        "  case 0: *c = x < y; break; case 1: *c = x <= y; break; case 2: *c = x > y; break;\n"
        "  case 3: *c = x >= y; break; case 4: *c = x == y; break; default: *c = x != y; break;\n"
        "  }\n"
        "  return 1;\n"
        "}\n"
        /* Equality of two values that are objects, symbols, booleans, null or undefined
           (k: 4 ==, 5 !=, 6 ===, 7 !==), and of one of them against a string or a
           number under the strict forms. The loose forms decide only the same tag and
           null against undefined; the rest convert and are the engine's. */
        "static inline __attribute__((always_inline)) int q2_ident_cmp(JSValue a, JSValue b, int k, int *c)\n"
        "{\n"
        "  int ta = JS_VALUE_GET_TAG(a), tb = JS_VALUE_GET_TAG(b), eq;\n"
        "  int ia = ta == JS_TAG_OBJECT || ta == JS_TAG_SYMBOL || ta == JS_TAG_BOOL || ta == JS_TAG_NULL || ta == JS_TAG_UNDEFINED;\n"
        "  int ib = tb == JS_TAG_OBJECT || tb == JS_TAG_SYMBOL || tb == JS_TAG_BOOL || tb == JS_TAG_NULL || tb == JS_TAG_UNDEFINED;\n"
        "  if (k < 4) return 0;\n"
        "  if (!ia || !ib) {\n"
        "    int sa = ta == JS_TAG_STRING || ta == JS_TAG_INT || ta == JS_TAG_FLOAT64;\n"
        "    int sb = tb == JS_TAG_STRING || tb == JS_TAG_INT || tb == JS_TAG_FLOAT64;\n"
        "    if (k >= 6 && ((ia && sb) || (ib && sa))) { *c = k == 7; return 1; }\n"
        "    return 0;\n"
        "  }\n"
        "  if (ta == tb)\n"
        "    eq = (ta == JS_TAG_NULL || ta == JS_TAG_UNDEFINED) ? 1 :\n"
        "         ta == JS_TAG_BOOL ? JS_VALUE_GET_BOOL(a) == JS_VALUE_GET_BOOL(b) :\n"
        "         JS_VALUE_GET_PTR(a) == JS_VALUE_GET_PTR(b);\n"
        "  else if (k >= 6) eq = 0;\n"
        "  else if ((ta == JS_TAG_NULL && tb == JS_TAG_UNDEFINED) || (ta == JS_TAG_UNDEFINED && tb == JS_TAG_NULL)) eq = 1;\n"
        /* an object is neither null nor undefined (no HTMLDDA object exists while a body runs: its entry refuses otherwise) */
        "  else if ((ta == JS_TAG_OBJECT && (tb == JS_TAG_NULL || tb == JS_TAG_UNDEFINED)) ||\n"
        "           (tb == JS_TAG_OBJECT && (ta == JS_TAG_NULL || ta == JS_TAG_UNDEFINED))) eq = 0;\n"
        "  else return 0;\n"
        "  *c = (k == 4 || k == 6) ? eq : !eq;\n"
        "  return 1;\n"
        "}\n"
        "static inline __attribute__((always_inline)) int q2_str_eq(JSValue a, JSValue b, int *eq)\n"
        "{\n"
        "  const unsigned char *pa, *pb;\n"
        "  uint32_t a0, a1, b0, b1, n;\n"
        "  if (JS_VALUE_GET_TAG(a) != JS_TAG_STRING || JS_VALUE_GET_TAG(b) != JS_TAG_STRING) return 0;\n"
        "  pa = (const unsigned char *)JS_VALUE_GET_PTR(a);\n"
        "  pb = (const unsigned char *)JS_VALUE_GET_PTR(b);\n"
        "  if (pa == pb) { *eq = 1; return 1; }\n"
        "  a0 = *(const uint32_t *)pa; a1 = *(const uint32_t *)(pa + 4);\n"
        "  b0 = *(const uint32_t *)pb; b1 = *(const uint32_t *)(pb + 4);\n"
        "  if ((a1 | b1) & QJS_AOT_STRING_KIND_MASK) return 0;\n"
        "  n = a0 & QJS_AOT_STRING_LEN_MASK;\n"
        "  if (n != (b0 & QJS_AOT_STRING_LEN_MASK)) { *eq = 0; return 1; }\n"
        "  if ((a0 ^ b0) & QJS_AOT_STRING_WIDE_MASK) return 0;\n"
        "  if (a0 & QJS_AOT_STRING_WIDE_MASK) n *= 2;\n"
        "  pa += QJS_AOT_SIZEOF_JSSTRING; pb += QJS_AOT_SIZEOF_JSSTRING;\n"
        /* Equal-length strings that differ are the common case, and a libc call costs more than the compare for the short ones the programs hold:
           up to 16 bytes are compared with two overlapping loads of the widest size that fits, never reading outside [0, n). */
        "  if (n >= 8 && n <= 16) {\n"
        "    uint64_t x0, x1, y0, y1;\n"
        "    __builtin_memcpy(&x0, pa, 8); __builtin_memcpy(&y0, pb, 8);\n"
        "    __builtin_memcpy(&x1, pa + n - 8, 8); __builtin_memcpy(&y1, pb + n - 8, 8);\n"
        "    *eq = ((x0 ^ y0) | (x1 ^ y1)) == 0;\n"
        "  } else if (n >= 4 && n < 8) {\n"
        "    uint32_t x0, x1, y0, y1;\n"
        "    __builtin_memcpy(&x0, pa, 4); __builtin_memcpy(&y0, pb, 4);\n"
        "    __builtin_memcpy(&x1, pa + n - 4, 4); __builtin_memcpy(&y1, pb + n - 4, 4);\n"
        "    *eq = ((x0 ^ y0) | (x1 ^ y1)) == 0;\n"
        "  } else if (n >= 2 && n < 4) {\n"
        "    uint16_t x0, x1, y0, y1;\n"
        "    __builtin_memcpy(&x0, pa, 2); __builtin_memcpy(&y0, pb, 2);\n"
        "    __builtin_memcpy(&x1, pa + n - 2, 2); __builtin_memcpy(&y1, pb + n - 2, 2);\n"
        "    *eq = ((x0 ^ y0) | (x1 ^ y1)) == 0;\n"
        "  } else if (n == 1) {\n"
        "    *eq = pa[0] == pb[0];\n"
        "  } else {\n"
        "    *eq = n == 0 || __builtin_memcmp(pa, pb, n) == 0;\n"
        "  }\n"
        "  return 1;\n"
        "}\n"
        "static inline __attribute__((always_inline)) int q2_str_elem(JSContext *ctx, "
        "JSValue s, JSValue k, JSValue *out)\n"
        "{\n"
        "  const unsigned char *p;\n"
        "  uint32_t w0, w1, c;\n"
        "  void *const *tab;\n"
        "  if (!q2_flat_str(&s) || JS_VALUE_GET_TAG(k) != JS_TAG_INT) return 0;\n"
        "  p = (const unsigned char *)JS_VALUE_GET_PTR(s);\n"
        "  w0 = *(const uint32_t *)p;\n"
        "  w1 = *(const uint32_t *)(p + 4);\n"
        "  if ((w1 & QJS_AOT_STRING_KIND_MASK) || (w0 & QJS_AOT_STRING_WIDE_MASK)) return 0;\n"
        "  if ((uint32_t)JS_VALUE_GET_INT(k) >= (w0 & QJS_AOT_STRING_LEN_MASK)) return 0;\n"
        "  c = ((const uint8_t *)(p + QJS_AOT_SIZEOF_JSSTRING))[JS_VALUE_GET_INT(k)];\n"
        "  tab = (void *const *)(*(const unsigned char *const *)((const unsigned char *)ctx +\n"
        "        QJS_AOT_OFF_CTX_RT) + QJS_AOT_OFF_RT_ONECHAR);\n"
        "  if (tab[c] == NULL) return 0;\n"
        "  *out = JS_MKPTR(JS_TAG_STRING, tab[c]);\n"
        "  return 1;\n"
        "}\n"
        "static inline __attribute__((always_inline)) int q2_elem_hit_lz(JSValue "
        "arr, JSValue k, JSValue *out, int lz)\n"
        "{\n"
        "  const unsigned char *p;\n"
        "  uint32_t i;\n"
        "  JSValue v;\n"
        "  if (JS_VALUE_GET_TAG(arr) != JS_TAG_OBJECT) return 0;\n"
        "  if (JS_VALUE_GET_TAG(k) != JS_TAG_INT) return 0;\n"
        "  p = (const unsigned char *)JS_VALUE_GET_PTR(arr);\n"
        "  i = (uint32_t)JS_VALUE_GET_INT(k);\n"
        "  if (*(const uint16_t *)(p + QJS_AOT_OFF_JSOBJECT_CLASS_ID) !=\n"
        "      QJS_AOT_CLASS_ARRAY)\n"
        "    return q2_ta_get(p, *(const uint16_t *)(p + "
        "QJS_AOT_OFF_JSOBJECT_CLASS_ID), i, out);\n"
        "  if (QJS_AOT_ARR_ODD(p)) {\n"
        "#if QJS_AOT_PACKED_ARRAYS\n"
        "    /* a packed array holds numbers only: the element in its own kind, no tag test */\n"
        "    unsigned kind = p[QJS_AOT_OFF_JSOBJECT_ARR_KIND];\n"
        "    if (kind != 1 && kind != 2) return 0;\n"
        "    if (i >= *(const uint32_t *)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_COUNT)) return 0;\n"
        "    if (kind == 1)\n"
        "      *out = JS_MKVAL(JS_TAG_INT, ((const int32_t *)*(const void *const *)(p + "
        "QJS_AOT_OFF_JSOBJECT_ARRAY_VALUES))[i]);\n"
        "    else\n"
        "      *out = __JS_NewFloat64(((const double *)*(const void *const *)(p + "
        "QJS_AOT_OFF_JSOBJECT_ARRAY_VALUES))[i]);\n"
        "    return 1;\n"
        "#else\n"
        "    return 0;\n"
        "#endif\n"
        "  }\n"
        "  if (i >= *(const uint32_t *)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_COUNT)) "
        "return 0;\n"
        "  v = (*(JSValue *const *)(p + "
        "QJS_AOT_OFF_JSOBJECT_ARRAY_VALUES))[i];\n"
        "  if (JS_VALUE_GET_TAG(v) == JS_TAG_UNINITIALIZED) return 0;\n"
        "  *out = v;\n"
        "  return 1;\n"
        "}\n\n"
        /* A store into an existing element of a dense array, when neither
           the old nor the new value is counted, so no reference changes hands.
           A hole is refused: writing one creates a property, which a setter
           on a prototype could see. */
        /* A guarded region over one array. The first access validates the
           receiver (its tag, the array class, the hole byte) and keeps the
           element storage and the count in C locals; later accesses in the same
           block reuse them instead of reloading both from the object after every
           store. The compiler cannot prove a store into the elements did not
           change `count` or `values` (a store can grow the array and move the
           storage), so without this each access reloads them and waits on the
           store it just made. */
        "typedef struct Q2ElemRegion {\n"
        "  const unsigned char *vals;\n"
        "  uint32_t count;\n"
        "  uint32_t kind;   /* the array's storage kind when it was opened (0: JSValue elements) */\n"
        "} Q2ElemRegion;\n\n"
        "/* Validate the receiver once. Returns 0 when the fast path does not\n"
        "   apply, and the caller leaves the whole block to the op call. */\n"
        "static inline __attribute__((always_inline)) int q2_elem_open(\n"
        "    JSValue arr, Q2ElemRegion *r)\n"
        "{\n"
        "  const unsigned char *p;\n"
        "  if (JS_VALUE_GET_TAG(arr) != JS_TAG_OBJECT) return 0;\n"
        "  p = (const unsigned char *)JS_VALUE_GET_PTR(arr);\n"
        "  if (*(const uint16_t *)(p + QJS_AOT_OFF_JSOBJECT_CLASS_ID) !=\n"
        "      QJS_AOT_CLASS_ARRAY)\n"
        "    return 0;\n"
        "  if (QJS_AOT_ARR_ODD(p)) return 0;\n"
        "  r->vals = *(const unsigned char *const *)(p + "
        "QJS_AOT_OFF_JSOBJECT_ARRAY_VALUES);\n"
        "  r->count = *(const uint32_t *)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_COUNT);\n"
        "#if QJS_AOT_PACKED_ARRAYS\n"
        "  r->kind = p[QJS_AOT_OFF_JSOBJECT_ARR_KIND];\n"
        "#else\n"
        "  r->kind = 0;\n"
        "#endif\n"
        "  return 1;\n"
        "}\n\n"
        "/* A read through the region: the bounds test uses the stored count, so\n"
        "   no field of the object is read again. */\n"
        "static inline __attribute__((always_inline)) int q2_elem_region_get(\n"
        "    const Q2ElemRegion *r, JSValue k, JSValue *out)\n"
        "{\n"
        "  const JSValue *slot;\n"
        "  JSValue v;\n"
        "  uint32_t i;\n"
        "  if (JS_VALUE_GET_TAG(k) != JS_TAG_INT) return 0;\n"
        "  i = (uint32_t)JS_VALUE_GET_INT(k);\n"
        "  if (i >= r->count) return 0;\n"
        "#if QJS_AOT_PACKED_ARRAYS\n"
        "  if (r->kind == 1) {\n"
        "    *out = JS_MKVAL(JS_TAG_INT, ((const int32_t *)r->vals)[i]);\n"
        "    return 1;\n"
        "  }\n"
        "  if (r->kind == 2) {\n"
        "    *out = __JS_NewFloat64(((const double *)r->vals)[i]);\n"
        "    return 1;\n"
        "  }\n"
        "#endif\n"
        "  slot = (const JSValue *)(r->vals + (size_t)i * 16);\n"
        "  v = *slot;\n"
        "  if (JS_VALUE_GET_TAG(v) != JS_TAG_INT &&\n"
        "      JS_VALUE_GET_TAG(v) != JS_TAG_FLOAT64)\n"
        "    return 0;\n"
        "  *out = v;\n"
        "  return 1;\n"
        "}\n\n"
        "/* A write through the region. The bounds test is against the stored\n"
        "   count, and the slot's current tag is checked as the helper does. */\n"
        "static inline __attribute__((always_inline)) int q2_elem_region_put(\n"
        "    const Q2ElemRegion *r, JSValue k, JSValue v)\n"
        "{\n"
        "  JSValue *slot;\n"
        "  uint32_t i;\n"
        "  if (JS_VALUE_GET_TAG(k) != JS_TAG_INT) return 0;\n"
        "  if (JS_VALUE_GET_TAG(v) < 0) return 0;\n"
        "  i = (uint32_t)JS_VALUE_GET_INT(k);\n"
        "  if (i >= r->count) return 0;\n"
        "#if QJS_AOT_PACKED_ARRAYS\n"
        "  if (r->kind == 1) {\n"
        "    if (JS_VALUE_GET_TAG(v) != JS_TAG_INT) return 0;\n"
        "    ((int32_t *)r->vals)[i] = JS_VALUE_GET_INT(v);\n"
        "    return 1;\n"
        "  }\n"
        "  if (r->kind == 2) {\n"
        "    if (JS_VALUE_GET_TAG(v) == JS_TAG_INT)\n"
        "      ((double *)r->vals)[i] = (double)JS_VALUE_GET_INT(v);\n"
        "    else if (JS_VALUE_GET_TAG(v) == JS_TAG_FLOAT64)\n"
        "      ((double *)r->vals)[i] = JS_VALUE_GET_FLOAT64(v);\n"
        "    else\n"
        "      return 0;\n"
        "    return 1;\n"
        "  }\n"
        "#endif\n"
        "  slot = (JSValue *)(r->vals + (size_t)i * 16);\n"
        "  if (JS_VALUE_GET_TAG(*slot) < 0 ||\n"
        "      JS_VALUE_GET_TAG(*slot) == JS_TAG_UNINITIALIZED)\n"
        "    return 0;\n"
        "  *slot = v;\n"
        "  return 1;\n"
        "}\n\n"
        "static inline __attribute__((always_inline)) int q2_elem_put(JSValue "
        "arr, JSValue k, JSValue v)\n"
        "{\n"
        "  unsigned char *p;\n"
        "  uint32_t i;\n"
        "  JSValue *slot;\n"
        "  if (JS_VALUE_GET_TAG(arr) != JS_TAG_OBJECT) return 0;\n"
        "  if (JS_VALUE_GET_TAG(k) != JS_TAG_INT || JS_VALUE_GET_TAG(v) < 0) "
        "return 0;\n"
        "  p = (unsigned char *)JS_VALUE_GET_PTR(arr);\n"
        "  i = (uint32_t)JS_VALUE_GET_INT(k);\n"
        "  if (*(const uint16_t *)(p + QJS_AOT_OFF_JSOBJECT_CLASS_ID) !=\n"
        "      QJS_AOT_CLASS_ARRAY)\n"
        "    return qjs_aot_typed_put(arr, i, v);\n"
        "  if (QJS_AOT_ARR_ODD(p)) {\n"
        "#if QJS_AOT_PACKED_ARRAYS\n"
        "    /* a packed array takes a value of its own kind: an int32 into either, a double into\n"
        "       a float64 array. Anything else misses, and the op call widens or unpacks it. */\n"
        "    unsigned kind = p[QJS_AOT_OFF_JSOBJECT_ARR_KIND];\n"
        "    if (kind != 1 && kind != 2) return 0;\n"
        "    if (i >= *(const uint32_t *)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_COUNT)) return 0;\n"
        "    if (kind == 1) {\n"
        "      if (JS_VALUE_GET_TAG(v) != JS_TAG_INT) return 0;\n"
        "      ((int32_t *)*(void *const *)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_VALUES))[i] =\n"
        "          JS_VALUE_GET_INT(v);\n"
        "    } else {\n"
        "      if (JS_VALUE_GET_TAG(v) == JS_TAG_INT)\n"
        "        ((double *)*(void *const *)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_VALUES))[i] =\n"
        "            (double)JS_VALUE_GET_INT(v);\n"
        "      else if (JS_VALUE_GET_TAG(v) == JS_TAG_FLOAT64)\n"
        "        ((double *)*(void *const *)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_VALUES))[i] =\n"
        "            JS_VALUE_GET_FLOAT64(v);\n"
        "      else\n"
        "        return 0;\n"
        "    }\n"
        "    return 1;\n"
        "#else\n"
        "    return 0;\n"
        "#endif\n"
        "  }\n"
        "  if (i >= *(const uint32_t *)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_COUNT)) "
        "return 0;\n"
        "  slot = &(*(JSValue **)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_VALUES))[i];\n"
        "  if (JS_VALUE_GET_TAG(*slot) < 0 ||\n"
        "      JS_VALUE_GET_TAG(*slot) == JS_TAG_UNINITIALIZED)\n"
        "    return 0;\n"
        "  *slot = v;\n"
        "  return 1;\n"
        "}\n\n"
        /* A store at index == count into a fast, extensible array of the context's initial array
           shape, with a standard array prototype, when `length` already covers the new element and
           the storage has room: the engine's own fast case of an append, without the length update
           or the growth (those, and every other case, take the engine's path). The value carries no
           reference. */
        "static inline __attribute__((always_inline)) int q2_elem_append(JSContext *ctx, JSValue arr, JSValue k, "
        "JSValue v)\n"
        "{\n"
        "  unsigned char *p;\n"
        "  const unsigned char *c = (const unsigned char *)ctx;\n"
        "  uint32_t i, count;\n"
        "  const JSValue *lenp;\n"
        "  if (JS_VALUE_GET_TAG(arr) != JS_TAG_OBJECT) return 0;\n"
        "  if (JS_VALUE_GET_TAG(k) != JS_TAG_INT || JS_VALUE_GET_TAG(v) < 0) return 0;\n"
        "  p = (unsigned char *)JS_VALUE_GET_PTR(arr);\n"
        "  i = (uint32_t)JS_VALUE_GET_INT(k);\n"
        "  count = *(const uint32_t *)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_COUNT);\n"
        "  if (i != count) return 0;\n"
        "  if (!(p[QJS_AOT_OFF_JSOBJECT_EXT_BYTE] & QJS_AOT_JSOBJECT_EXT_MASK) ||\n"
        "      !(p[QJS_AOT_OFF_JSOBJECT_FAST_ARRAY_BYTE] & QJS_AOT_JSOBJECT_FAST_ARRAY_MASK))\n"
        "    return 0;\n"
        "  if (*(const void *const *)(p + QJS_AOT_OFF_JSOBJECT_SHAPE) !=\n"
        "      *(const void *const *)(c + QJS_AOT_OFF_CTX_ARRAY_SHAPE))\n"
        "    return 0;\n"
        "  if (!(c[QJS_AOT_OFF_CTX_STD_ARRAY_PROTO_BYTE] & QJS_AOT_CTX_STD_ARRAY_PROTO_MASK)) return 0;\n"
        "  lenp = (const JSValue *)(*(const unsigned char *const *)(p + QJS_AOT_OFF_JSOBJECT_PROP) +\n"
        "                           QJS_AOT_OFF_JSPROPERTY_U_VALUE);\n"
        "  if (JS_VALUE_GET_TAG(*lenp) != JS_TAG_INT || (uint32_t)JS_VALUE_GET_INT(*lenp) <= count) return 0;\n"
#if QJS_AOT_PACKED_ARRAYS
        "  if (p[QJS_AOT_OFF_JSOBJECT_ARR_KIND]) {\n"
        "    unsigned kind = p[QJS_AOT_OFF_JSOBJECT_ARR_KIND], tag = (unsigned)JS_VALUE_GET_TAG(v);\n"
        "    size_t esz = kind == 1 ? sizeof(int32_t) : sizeof(double);\n"
        "    if ((kind == 1 && tag != JS_TAG_INT) ||\n"
        "        (kind == 2 && tag != JS_TAG_INT && tag != JS_TAG_FLOAT64))\n"
        "      return 0;\n"
        "    if ((size_t)(count + 1) * esz >\n"
        "        *(const uint32_t *)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_SIZE) * esz)\n"
        "      return 0;\n"
        "    if (kind == 1)\n"
        "      ((int32_t *)*(void *const *)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_VALUES))[count] =\n"
        "          JS_VALUE_GET_INT(v);\n"
        "    else if (tag == JS_TAG_INT)\n"
        "      ((double *)*(void *const *)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_VALUES))[count] =\n"
        "          (double)JS_VALUE_GET_INT(v);\n"
        "    else\n"
        "      ((double *)*(void *const *)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_VALUES))[count] =\n"
        "          JS_VALUE_GET_FLOAT64(v);\n"
        "    *(uint32_t *)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_COUNT) = count + 1;\n"
        "    return 1;\n"
        "  }\n"
#endif
        "  if (count + 1 > *(const uint32_t *)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_SIZE)) return 0;\n"
        "  (*(JSValue **)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_VALUES))[count] = v;\n"
        "  *(uint32_t *)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_COUNT) = count + 1;\n"
        "  return 1;\n"
        "}\n\n"
        /* The step of a for-of over a dense array that reads its elements from an Array Iterator
           whose `next` is the builtin: the element in place (the register borrows it, the array
           owns it) and the iterator's index advanced, as the engine's own step does. Every other
           case, the end of the iteration included, is the engine's. */
        "static const void *e_arrnext_fn;\n"
        "static inline __attribute__((always_inline)) int q2_iter_next_fast(JSValue it, JSValue nx, JSValue *out)\n"
        "{\n"
        "  const unsigned char *pi, *pn, *ap;\n"
        "  JSValue ao, v;\n"
        "  uint32_t idx;\n"
        "  if (JS_VALUE_GET_TAG(it) != JS_TAG_OBJECT || JS_VALUE_GET_TAG(nx) != JS_TAG_OBJECT) return 0;\n"
        "  pi = (const unsigned char *)JS_VALUE_GET_PTR(it);\n"
        "  pn = (const unsigned char *)JS_VALUE_GET_PTR(nx);\n"
        "  if (*(const uint16_t *)(pi + QJS_AOT_OFF_JSOBJECT_CLASS_ID) != QJS_AOT_CLASS_ARRAY_ITERATOR) return 0;\n"
        "  if (*(const uint16_t *)(pn + QJS_AOT_OFF_JSOBJECT_CLASS_ID) != QJS_AOT_CLASS_C_FUNCTION ||\n"
        "      pn[QJS_AOT_OFF_CFUNC_CPROTO] != QJS_AOT_CFUNC_CPROTO_ITERATOR_NEXT)\n"
        "    return 0;\n"
        "  if (__builtin_expect(e_arrnext_fn == NULL, 0)) e_arrnext_fn = qjs_aot_array_iterator_next_fn();\n"
        "  if (*(const void *const *)(pn + QJS_AOT_OFF_CFUNC_C_FUNCTION) != e_arrnext_fn) return 0;\n"
        "  if (*(const uint32_t *)(pi + QJS_AOT_OFF_ARRAY_ITER_KIND) != QJS_AOT_ARRAY_ITER_KIND_VALUE) return 0;\n"
        "  ao = *(const JSValue *)(pi + QJS_AOT_OFF_ARRAY_ITER_OBJ);\n"
        "  if (JS_VALUE_GET_TAG(ao) != JS_TAG_OBJECT) return 0;\n"
        "  ap = (const unsigned char *)JS_VALUE_GET_PTR(ao);\n"
        "  if (*(const uint16_t *)(ap + QJS_AOT_OFF_JSOBJECT_CLASS_ID) != QJS_AOT_CLASS_ARRAY ||\n"
        "      !(ap[QJS_AOT_OFF_JSOBJECT_FAST_ARRAY_BYTE] & QJS_AOT_JSOBJECT_FAST_ARRAY_MASK) ||\n"
        "      QJS_AOT_ARR_HOLES(ap))\n"
        "    return 0;\n"
        "#if QJS_AOT_PACKED_ARRAYS\n"
        "  /* An int32 packed array's element in place: the bounds test, the element, and the\n"
        "     iterator advanced, in one block, so the step of a for-of loop over the commonest\n"
        "     kind of storage has no taken branch in it. Double storage is the op call's. */\n"
        "  if (__builtin_expect(ap[QJS_AOT_OFF_JSOBJECT_ARR_KIND] == 1, 0)) {\n"
        "    idx = *(const uint32_t *)(pi + QJS_AOT_OFF_ARRAY_ITER_IDX);\n"
        "    if (idx >= *(const uint32_t *)(ap + QJS_AOT_OFF_JSOBJECT_ARRAY_COUNT)) return 0;\n"
        "    v = JS_MKVAL(JS_TAG_INT, ((const int32_t *)*(const void *const *)(ap + "
        "QJS_AOT_OFF_JSOBJECT_ARRAY_VALUES))[idx]);\n"
        "    *(uint32_t *)(pi + QJS_AOT_OFF_ARRAY_ITER_IDX) = idx + 1;\n"
        "    *out = v;\n"
        "    return 1;\n"
        "  }\n"
        "#endif\n"
        "  idx = *(const uint32_t *)(pi + QJS_AOT_OFF_ARRAY_ITER_IDX);\n"
        "  if (idx >= *(const uint32_t *)(ap + QJS_AOT_OFF_JSOBJECT_ARRAY_COUNT)) return 0;\n"
        "#if QJS_AOT_PACKED_ARRAYS\n"
        "  /* A double-packed element is a double, not a JSValue. */\n"
        "  if (ap[QJS_AOT_OFF_JSOBJECT_ARR_KIND] >= 2) return 0;\n"
        "#endif\n"
        "  v = (*(const JSValue *const *)(ap + QJS_AOT_OFF_JSOBJECT_ARRAY_VALUES))[idx];\n"
        "  *(uint32_t *)(pi + QJS_AOT_OFF_ARRAY_ITER_IDX) = idx + 1;\n"
        "  *out = v;\n"
        "  return 1;\n"
        "}\n\n"
        /* Array.prototype.push (one argument) and pop on a plain dense array: done in place when the
           callee is the builtin, the array has the initial array shape (Array.prototype, `length` writable
           as its first property) and `length` equals the count. push needs room; pop needs an element
           that is not a hole. Anything else returns 0 and the call is made. */
        "static const void *e_arrpush_fn, *e_arrpop_fn, *e_strreplace_fn;\n"
        "static inline __attribute__((always_inline)) int q2_builtin_is(JSValue callee, const void **cache, int which)\n"
        "{\n"
        "  const unsigned char *p;\n"
        "  if (JS_VALUE_GET_TAG(callee) != JS_TAG_OBJECT) return 0;\n"
        "  p = (const unsigned char *)JS_VALUE_GET_PTR(callee);\n"
        "  if (*(const uint16_t *)(p + QJS_AOT_OFF_JSOBJECT_CLASS_ID) != QJS_AOT_CLASS_C_FUNCTION ||\n"
        "      p[QJS_AOT_OFF_CFUNC_CPROTO] != QJS_AOT_CFUNC_CPROTO_GENERIC_MAGIC ||\n"
        "      *(const int16_t *)(p + QJS_AOT_OFF_CFUNC_MAGIC) != 0)\n"
        "    return 0;\n"
        "  if (__builtin_expect(*cache == NULL, 0)) *cache = qjs_aot_array_builtin_fn(which);\n"
        "  return *(const void *const *)(p + QJS_AOT_OFF_CFUNC_C_FUNCTION) == *cache;\n"
        "}\n"
        /* Function.prototype.apply: the builtin's function object (class C function, generic-magic entry with magic 0, the engine's own pointer) */
        "static const void *e_fnapply_fn;\n"
        "static inline __attribute__((always_inline)) int q2_is_fn_apply(JSValue callee)\n"
        "{\n"
        "  const unsigned char *p;\n"
        "  if (JS_VALUE_GET_TAG(callee) != JS_TAG_OBJECT) return 0;\n"
        "  p = (const unsigned char *)JS_VALUE_GET_PTR(callee);\n"
        "  if (*(const uint16_t *)(p + QJS_AOT_OFF_JSOBJECT_CLASS_ID) != QJS_AOT_CLASS_C_FUNCTION ||\n"
        "      p[QJS_AOT_OFF_CFUNC_CPROTO] != QJS_AOT_CFUNC_CPROTO_GENERIC_MAGIC ||\n"
        "      *(const int16_t *)(p + QJS_AOT_OFF_CFUNC_MAGIC) != 0)\n"
        "    return 0;\n"
        "  if (__builtin_expect(e_fnapply_fn == NULL, 0)) e_fnapply_fn = qjs_aot_intrinsic_fn(QJS_AOT_INTR_X_FUNC_APPLY);\n"
        "  return *(const void *const *)(p + QJS_AOT_OFF_CFUNC_C_FUNCTION) == e_fnapply_fn;\n"
        "}\n"
        "static const void *e_fncall_fn;\n"
        "static inline __attribute__((always_inline)) int q2_is_fn_call(JSValue callee)\n"
        "{\n"
        "  const unsigned char *p;\n"
        "  if (JS_VALUE_GET_TAG(callee) != JS_TAG_OBJECT) return 0;\n"
        "  p = (const unsigned char *)JS_VALUE_GET_PTR(callee);\n"
        "  if (*(const uint16_t *)(p + QJS_AOT_OFF_JSOBJECT_CLASS_ID) != QJS_AOT_CLASS_C_FUNCTION) return 0;\n"
        "  if (__builtin_expect(e_fncall_fn == NULL, 0)) e_fncall_fn = qjs_aot_function_call_fn();\n"
        "  return *(const void *const *)(p + QJS_AOT_OFF_CFUNC_C_FUNCTION) == e_fncall_fn;\n"
        "}\n"
        "static inline __attribute__((always_inline)) int q2_array_plain(JSContext *ctx, const unsigned char *p, uint32_t *count, JSValue **lenp)\n"
        "{\n"
        "  const unsigned char *c = (const unsigned char *)ctx;\n"
        "  if (!(p[QJS_AOT_OFF_JSOBJECT_EXT_BYTE] & QJS_AOT_JSOBJECT_EXT_MASK) ||\n"
        "      !(p[QJS_AOT_OFF_JSOBJECT_FAST_ARRAY_BYTE] & QJS_AOT_JSOBJECT_FAST_ARRAY_MASK))\n"
        "    return 0;\n"
        "  if (*(const void *const *)(p + QJS_AOT_OFF_JSOBJECT_SHAPE) != *(const void *const *)(c + QJS_AOT_OFF_CTX_ARRAY_SHAPE))\n"
        "    return 0;\n"
        "  if (!(c[QJS_AOT_OFF_CTX_STD_ARRAY_PROTO_BYTE] & QJS_AOT_CTX_STD_ARRAY_PROTO_MASK)) return 0;\n"
        "  *count = *(const uint32_t *)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_COUNT);\n"
        "  *lenp = (JSValue *)(*(const unsigned char *const *)(p + QJS_AOT_OFF_JSOBJECT_PROP) + QJS_AOT_OFF_JSPROPERTY_U_VALUE);\n"
        "  return JS_VALUE_GET_TAG(**lenp) == JS_TAG_INT && (uint32_t)JS_VALUE_GET_INT(**lenp) == *count;\n"
        "}\n"
        "static inline __attribute__((always_inline)) void q2_dup_y_(JSValue v);\n"
        "static inline __attribute__((always_inline)) int q2_array_push1(JSContext *ctx, JSValue callee, JSValue arr, JSValue v, JSValue *out)\n"
        "{\n"
        "  unsigned char *p;\n"
        "  uint32_t count;\n"
        "  JSValue *lenp;\n"
        "  if (JS_VALUE_GET_TAG(arr) != JS_TAG_OBJECT || !q2_builtin_is(callee, &e_arrpush_fn, 0)) return 0;\n"
        "  p = (unsigned char *)JS_VALUE_GET_PTR(arr);\n"
        "  if (!q2_array_plain(ctx, p, &count, &lenp)) return 0;\n"
        "#if QJS_AOT_PACKED_ARRAYS\n"
        "  if (p[QJS_AOT_OFF_JSOBJECT_ARR_KIND]) {\n"
        "    /* a packed array appends a value of its own kind, with the capacity in the kind's\n"
        "       element size. The append cannot widen, so no element is left behind. */\n"
        "    unsigned kind = p[QJS_AOT_OFF_JSOBJECT_ARR_KIND], tag = (unsigned)JS_VALUE_GET_TAG(v);\n"
        "    size_t esz = kind == 1 ? sizeof(int32_t) : sizeof(double);\n"
        "    if ((kind == 1 && tag != JS_TAG_INT) ||\n"
        "        (kind == 2 && tag != JS_TAG_INT && tag != JS_TAG_FLOAT64) || count >= 0x7fffffffu)\n"
        "      return 0;\n"
        "    if ((size_t)(count + 1) * esz >\n"
        "        *(const uint32_t *)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_SIZE) * esz)\n"
        "      return qjs_aot_array_push_slow(ctx, arr, v, out);\n"
        "    if (kind == 1)\n"
        "      ((int32_t *)*(void *const *)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_VALUES))[count] =\n"
        "          JS_VALUE_GET_INT(v);\n"
        "    else if (tag == JS_TAG_INT)\n"
        "      ((double *)*(void *const *)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_VALUES))[count] =\n"
        "          (double)JS_VALUE_GET_INT(v);\n"
        "    else\n"
        "      ((double *)*(void *const *)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_VALUES))[count] =\n"
        "          JS_VALUE_GET_FLOAT64(v);\n"
        "    *(uint32_t *)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_COUNT) = count + 1;\n"
        "    *lenp = JS_MKVAL(JS_TAG_INT, (int32_t)(count + 1));\n"
        "    *out = *lenp;\n"
        "    return 1;\n"
        "  }\n"
        "#endif\n"
        "  if (count >= 0x7fffffffu) return 0;\n"
        "  if (count + 1 > *(const uint32_t *)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_SIZE)) return qjs_aot_array_push_slow(ctx, arr, v, out);\n"
        "  q2_dup_y_(v);\n"
        "  (*(JSValue **)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_VALUES))[count] = v;\n"
        "  *(uint32_t *)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_COUNT) = count + 1;\n"
        "  *lenp = JS_MKVAL(JS_TAG_INT, (int32_t)(count + 1));\n"
        "  *out = *lenp;\n"
        "  return 1;\n"
        "}\n"
        "static inline __attribute__((always_inline)) int q2_replace_len(JSContext *ctx, JSValue callee, JSValue str, JSValue rx, JSValue rep, JSValue *out)\n"
        "{\n"
        "  int32_t n;\n"
        "  if (!q2_builtin_is(callee, &e_strreplace_fn, 2) || !qjs_aot_replace_len(ctx, str, rx, rep, &n)) return 0;\n"
        "  *out = JS_MKVAL(JS_TAG_INT, n);\n"
        "  return 1;\n"
        "}\n"
        "static inline __attribute__((always_inline)) int q2_array_pop0(JSContext *ctx, JSValue callee, JSValue arr, JSValue *out)\n"
        "{\n"
        "  unsigned char *p;\n"
        "  uint32_t count;\n"
        "  JSValue *lenp, v;\n"
        "  if (JS_VALUE_GET_TAG(arr) != JS_TAG_OBJECT || !q2_builtin_is(callee, &e_arrpop_fn, 1)) return 0;\n"
        "  p = (unsigned char *)JS_VALUE_GET_PTR(arr);\n"
        "  if (!q2_array_plain(ctx, p, &count, &lenp) || count == 0) return 0;\n"
        "#if QJS_AOT_PACKED_ARRAYS\n"
        "  if (p[QJS_AOT_OFF_JSOBJECT_ARR_KIND]) {\n"
        "    unsigned kind = p[QJS_AOT_OFF_JSOBJECT_ARR_KIND];\n"
        "    if (kind == 1)\n"
        "      v = JS_MKVAL(JS_TAG_INT, ((int32_t *)*(void *const *)(p + "
        "QJS_AOT_OFF_JSOBJECT_ARRAY_VALUES))[count - 1]);\n"
        "    else\n"
        "      v = __JS_NewFloat64(((double *)*(void *const *)(p + "
        "QJS_AOT_OFF_JSOBJECT_ARRAY_VALUES))[count - 1]);\n"
        "    *(uint32_t *)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_COUNT) = count - 1;\n"
        "    *lenp = JS_MKVAL(JS_TAG_INT, (int32_t)(count - 1));\n"
        "    *out = v;\n"
        "    return 1;\n"
        "  }\n"
        "#endif\n"
        "  if (QJS_AOT_ARR_ODD(p)) return 0;\n"
        "  v = (*(JSValue **)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_VALUES))[count - 1];\n"
        "  if (JS_VALUE_GET_TAG(v) == JS_TAG_UNINITIALIZED) return 0;\n"
        "  *(uint32_t *)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_COUNT) = count - 1;\n"
        "  *lenp = JS_MKVAL(JS_TAG_INT, (int32_t)(count - 1));\n"
        "  *out = v;\n"
        "  return 1;\n"
        "}\n\n"
        "static inline const void *q2_pic(JSRegRun *f)\n"
        "{\n"
        "  return *(const void **)((unsigned char *)f +\n"
        "                          QJS_AOT_OFF_REGRUN_PIC);\n"
        "}\n\n"
        "static inline JSValue *q2_var_buf(JSRegRun *f)\n"
        "{\n"
        "  return *(JSValue **)((unsigned char *)f +\n"
        "                       QJS_AOT_OFF_REGRUN_VAR_BUF);\n"
        "}\n\n"
        "static inline JSValue *q2_arg_buf(JSRegRun *f)\n"
        "{\n"
        "  return *(JSValue **)((unsigned char *)f +\n"
        "                       QJS_AOT_OFF_REGRUN_ARG_BUF);\n"
        "}\n\n"
        /* A captured variable, read through THE FUNCTION that owns the cell
           vector: what qjs_aot_get_var_ref does from a frame, for a body that has
           none.

           THE CELLS BELONG TO THE FUNCTION, NOT TO THE ACTIVATION. `co` is right
           for a frameless body's own entry, where it names the function being
           entered. A COPY is not: its instructions run inside its CALLER's body,
           where `co` (frameless) and `f` (framed) both name the CALLER. A caller
           that captures nothing has a NULL `var_refs` -- the NULL dereference that
           segfaulted -- and one that does capture has ITS OWN slots, so a copied
           cell read would read or write another function's variable. A copy
           therefore names the function object its guard has already proved
           identity on, `nco_<n>`, and keeps the CALLEE's slot number. */
        "static inline __attribute__((always_inline)) JSValue *"
        "q2_vref_ptr(JSObject *co, int slot)\n"
        "{\n"
        "  void *const *cells = *(void *const **)((unsigned char *)co +\n"
        "                           QJS_AOT_OFF_OBJECT_FUNC_VAR_REFS);\n"
        "  return *(JSValue **)((unsigned char *)cells[slot] +\n"
        "                       QJS_AOT_OFF_VARREF_PVALUE);\n"
        "}\n\n"
        "static inline __attribute__((always_inline)) JSValue "
        "q2_vref_get(JSObject *co, int slot)\n"
        "{\n"
        "  return *q2_vref_ptr(co, slot);\n"
        "}\n\n"
        "static inline void *const *q2_pic_holder(JSRegRun *f)\n"
        "{\n"
        "  return *(void *const **)((unsigned char *)q2_b(f) +\n"
        "                           QJS_AOT_OFF_FNBC_REGVM_PIC_HOLDER);\n"
        "}\n\n"
        /* The global-read site cache hit: the site names the global's property
           slot and the generation it was filled at, and a hit borrows the
           slot's value. Runs no user code and allocates nothing. */
        /* A dense array's `length`, read the way the engine stores it: for a
           non-holey fast array the element count *is* the length, and prop[0]
           holds the same number. A holey array (or any array with a gap between
           its count and its length) does not satisfy that, so the test is
           structural -- class, fast array, not holey -- and the value comes from
           the length property itself, which is prop[0] for every array. */
        "static inline __attribute__((always_inline)) int "
        "q2_array_length(JSContext *ctx, JSValue v, double *out, const void **tash, int *taok)\n"
        "{\n"
        "  const unsigned char *p, *prop;\n"
        "  JSValue len;\n"
        "  if (JS_VALUE_GET_TAG(v) == JS_TAG_STRING) {\n"
        "    *out = (double)(*(const uint32_t *)JS_VALUE_GET_PTR(v) &\n"
        "                    QJS_AOT_STRING_LEN_MASK);\n"
        "    return 1;\n"
        "  }\n"
        "  if (JS_VALUE_GET_TAG(v) == JS_TAG_STRING_ROPE) {\n"
        "    *out = (double)*(const uint32_t *)JS_VALUE_GET_PTR(v);\n"
        "    return 1;\n"
        "  }\n"
        "  if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT) return 0;\n"
        "  p = (const unsigned char *)JS_VALUE_GET_PTR(v);\n"
        "  if (*(const uint16_t *)(p + QJS_AOT_OFF_JSOBJECT_CLASS_ID) !=\n"
        "      QJS_AOT_CLASS_ARRAY) {\n"
        "    /* a typed array's length is the standard accessor: the instance has no own\n"
        "       property, its prototype is the realm's class prototype, and the engine has\n"
        "       validated the accessor at the current watchpoint epoch */\n"
        "    unsigned cid = *(const uint16_t *)(p + QJS_AOT_OFF_JSOBJECT_CLASS_ID);\n"
        "    const unsigned char *sh;\n"
        "    JSValue pv;\n"
        "    if (cid < QJS_AOT_CLASS_UINT8C_ARRAY || cid > QJS_AOT_CLASS_FLOAT64_ARRAY) return 0;\n"
        "    sh = *(const unsigned char *const *)(p + QJS_AOT_OFF_JSOBJECT_SHAPE);\n"
        "    /* the caller's validated shape: nothing that can run JS or reshape an object has\n"
        "       run since it was checked (the flag is cleared at every such instruction) */\n"
        "    if (taok != NULL && *taok && sh == (const unsigned char *)*tash) {\n"
        "      *out = (double)*(const uint32_t *)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_COUNT);\n"
        "      return 1;\n"
        "    }\n"
        "    if (*(const int *)(sh + QJS_AOT_OFF_JSSHAPE_PROP_COUNT) != 0) return 0;\n"
        "    pv = (*(JSValue *const *)((const unsigned char *)ctx + QJS_AOT_OFF_CTX_CLASS_PROTO))[cid];\n"
        "    if (JS_VALUE_GET_TAG(pv) != JS_TAG_OBJECT ||\n"
        "        *(const void *const *)(sh + QJS_AOT_OFF_JSSHAPE_PROTO) != JS_VALUE_GET_PTR(pv)) return 0;\n"
        "    if (__builtin_expect(*(const uint32_t *)((const unsigned char *)ctx + QJS_AOT_OFF_CTX_TA_LEN_EPOCH) !=\n"
        "        *(const uint32_t *)(*(const unsigned char *const *)((const unsigned char *)ctx +\n"
        "            QJS_AOT_OFF_CTX_RT) + QJS_AOT_OFF_RT_IC_EPOCH), 0) &&\n"
        "        !qjs_aot_ta_len_valid(ctx)) return 0;\n"
        "    if (taok != NULL) { *tash = sh; *taok = 1; }\n"
        "    *out = (double)*(const uint32_t *)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_COUNT);\n"
        "    return 1;\n"
        "  }\n"
        "  prop = *(const unsigned char *const *)(p + "
        "QJS_AOT_OFF_JSOBJECT_PROP);\n"
        "  len = *(const JSValue *)(prop + QJS_AOT_OFF_JSPROPERTY_U_VALUE);\n"
        "  if (JS_VALUE_GET_TAG(len) != JS_TAG_INT || JS_VALUE_GET_INT(len) < "
        "0) return 0;\n"
        "  *out = (double)JS_VALUE_GET_INT(len);\n"
        "  return 1;\n"
        "}\n\n"
        "static inline __attribute__((always_inline)) int "
        "q2_global_hit(JSContext *ctx, const void *pic,\n"
        "                                int site, JSValue *out)\n"
        "{\n"
        "  const unsigned char *e;\n"
        "  const unsigned char *slot;\n"
        "  uint32_t gen;\n"
        "\n"
        "  if (pic == NULL) {\n"
        "    return 0;\n"
        "  }\n"
        "  e = (const unsigned char *)pic +\n"
        "      (size_t)site * QJS_AOT_SIZEOF_JSREGPIC;\n"
        "  gen = *(const uint32_t *)(*(const unsigned char *const *)(\n"
        "            (const unsigned char *)ctx + QJS_AOT_OFF_CTX_RT) +\n"
        "        QJS_AOT_OFF_RT_GV_CELL_GEN);\n"
        "  if (*(const uint32_t *)(e + QJS_AOT_OFF_JSREGPIC_SLOT_OFF) != gen) "
        "{\n"
        "    return 0;\n"
        "  }\n"
        "  slot = *(const unsigned char *const *)e;\n"
        "  *out = *(const JSValue *)(slot + QJS_AOT_OFF_JSPROPERTY_U_VALUE);\n"
        "  return 1;\n"
        "}\n\n"); /* The inline own-read hit: the engine's regvm_own_read_hit predicate
       against the layout header's offsets, so a compiled field read is a couple
       of loads rather than a call across the unit boundary. A miss returns 0 and
       the caller takes the cache-filling slow path and continues. */
    /* QJS_AOT_FORCE_MISS: forces every spec-body inline cache hit to fail, so
       the miss path (the op-call wrapper) runs on the EXISTING tests. A unit
       built with -DQJS_AOT_FORCE_MISS_ENV reads the variable once at load; any
       other unit has no test at all, because a load and a branch at every
       cache site cost instructions on every hot path. */
    fprintf(
        f,
        "#ifdef QJS_AOT_FORCE_MISS_ENV\n"
        "static int q2_force_miss_cache = -1;\n"
        "#define Q2_FORCE_MISS (q2_force_miss_cache > 0)\n"
        "static void __attribute__((constructor, unused)) q2_force_miss_init(void)\n"
        "{\n"
        "  const char *v = getenv(\"QJS_AOT_FORCE_MISS\");\n"
        "  q2_force_miss_cache = v != NULL && v[0] != 0 && v[0] != '0';\n"
        "}\n"
        "#else\n"
        "#define Q2_FORCE_MISS 0\n"
        "#endif\n"
        /* QJS_AOT_SPEC_FNS="<lo>-<hi>" allows the spec body only for function
           indices in that range, so a miscompile can be pinned to ONE function by
           halving the range. A run-time test, so a stale unit cannot fake it. */
        "static int q2_spec_fns_lo = -2, q2_spec_fns_hi = -2;\n"
        "static inline __attribute__((always_inline)) int q2_spec_fns_out(int fi)\n"
        "{\n"
        "  if (q2_spec_fns_lo == -2) {\n"
        "    const char *v = getenv(\"QJS_AOT_SPEC_FNS\");\n"
        "    q2_spec_fns_lo = -1; q2_spec_fns_hi = -1;\n"
        "    if (v != NULL && v[0] != 0) {\n"
        "      int lo = -1, hi = -1;\n"
        "      if (sscanf(v, \"%%d-%%d\", &lo, &hi) == 2) {\n"
        "        q2_spec_fns_lo = lo; q2_spec_fns_hi = hi;\n"
        "        if (getenv(\"QJS_AOT_SPEC_FNS_VERBOSE\"))\n"
        "          fprintf(stderr, \"spec-fns %%d..%%d\\n\", lo, hi);\n"
        "      }\n"
        "    }\n"
        "  }\n"
        "  if (q2_spec_fns_lo < 0) return 0;\n"
        "  return fi < q2_spec_fns_lo || fi > q2_spec_fns_hi;\n"
        "}\n\n"
        /* QJS_AOT_SPEC_SET="3,17,40": the spec body only for those function indices (a set, for
           pinning down a failure that needs more than one function) */
        "static inline __attribute__((always_inline)) int q2_spec_set_out(int fi)\n"
        "{\n"
        "  static const char *v; static int init;\n"
        "  char tok[16];\n"
        "  const char *p;\n"
        "  if (!init) { v = getenv(\"QJS_AOT_SPEC_SET\"); init = 1; }\n"
        "  if (v == NULL) return 0;\n"
        "  snprintf(tok, sizeof(tok), \",%%d,\", fi);\n"
        "  { char buf[512]; snprintf(buf, sizeof(buf), \",%%s,\", v); p = strstr(buf, tok); return p == NULL; }\n"
        "}\n\n");
    /* The sweep's coverage record: which guard sites a run reached. Only a sweep
       unit calls it. Each translation unit keeps its own bitmap and appends the
       ids it saw to $QJS_AOT_SWEEP_OUT at exit, so a split unit needs no
       shared array and the run's own output is untouched. */
    if (g_deopt_everywhere)
      fprintf(
          f,
          "#define E_SWEEP_MAX (1u << 20)\n"
          "static uint8_t e_sw_seen[E_SWEEP_MAX / 8];\n"
          "static inline int e_sweep_hit(JSContext *ctx, uint32_t sid)\n"
          "{\n"
          "  if (sid < E_SWEEP_MAX) e_sw_seen[sid >> 3] |= (uint8_t)(1u << "
          "(sid & 7));\n"
          /* QJS_AOT_SWEEP_SET="<n>:<id>,<id>,...": fail each listed site on its <n>-th execution (a debugging aid
             for finding which combination of exits differs; the unit consults it before the engine's single-site sweep). */
          "  { static int set_ = -1; static uint32_t n_, ids_[64], cnt_[64]; static int nid_;\n"
          "    if (__builtin_expect(set_ < 0, 0)) {\n"
          "      const char *e_ = getenv(\"QJS_AOT_SWEEP_SET\"); set_ = 0;\n"
          "      if (e_ != NULL) { char *q_; n_ = (uint32_t)strtoul(e_, &q_, 10); while (*q_ == ':' || *q_ == ',') { ids_[nid_++] = (uint32_t)strtoul(q_ + 1, &q_, 10); if (nid_ >= 64) break; } set_ = 1; }\n"
          "    }\n"
          "    if (set_ == 1) { int k_; for (k_ = 0; k_ < nid_; k_++) if (ids_[k_] == sid) return ++cnt_[k_] == n_; return 0; } }\n"
          "  return qjs_aot_should_deopt(ctx, sid);\n"
          "}\n"
          "static void __attribute__((destructor, unused)) "
          "e_sweep_dump(void)\n"
          "{\n"
          "  const char *p = getenv(\"QJS_AOT_SWEEP_OUT\");\n"
          "  FILE *o;\n"
          "  uint32_t i;\n"
          "  if (p == NULL || p[0] == 0) return;\n"
          "  o = fopen(p, \"a\");\n"
          "  if (o == NULL) return;\n"
          "  for (i = 0; i < E_SWEEP_MAX; i++)\n"
          "    if (e_sw_seen[i >> 3] & (1u << (i & 7))) fprintf(o, "
          "\"%%u\\n\", i);\n"
          "  fclose(o);\n"
          "}\n\n");
    fprintf(
        f,
        "/* The grandparent object a two-level prototype entry names, or NULL when\n"
        "   the chain no longer matches what the entry recorded. The entry sits in\n"
        "   way 1 or above; holders[] carries the upper object's shape at\n"
        "   ways*nsites and the grandparent's at its own index. Cold, so it stays\n"
        "   out of line and every read site keeps its small body. */\n"
        "static __attribute__((noinline)) const unsigned char *q2_holder2_of(\n"
        "    const unsigned char *obj, void *const *holders, int nsites,\n"
        "    const unsigned char *e, const void *pic)\n"
        "{\n"
        "  const unsigned char *p1, *p2;\n"
        "  size_t ci = (size_t)(e - (const unsigned char *)pic) / QJS_AOT_SIZEOF_JSREGPIC;\n"
        "  p1 = *(const unsigned char *const *)(*(const unsigned char *const *)(\n"
        "           obj + QJS_AOT_OFF_JSOBJECT_SHAPE) + QJS_AOT_OFF_JSSHAPE_PROTO);\n"
        "  if (p1 == NULL) return NULL;\n"
        "  if (*(const void *const *)(p1 + QJS_AOT_OFF_JSOBJECT_SHAPE) !=\n"
        "      holders[(size_t)QJS_AOT_REG_PIC_WAYS * (size_t)nsites + ci]) return NULL;\n"
        "  p2 = *(const unsigned char *const *)(*(const unsigned char *const *)(p1 +\n"
        "           QJS_AOT_OFF_JSOBJECT_SHAPE) + QJS_AOT_OFF_JSSHAPE_PROTO);\n"
        "  if (p2 == NULL ||\n"
        "      *(const void *const *)(p2 + QJS_AOT_OFF_JSOBJECT_SHAPE) != holders[ci])\n"
        "    return NULL;\n"
        "  return p2;\n"
        "}\n"
        "#if defined(E_LOGO_COLD) && defined(E_ORH_SPLIT)\n"
        "#define Q2_ORH_NAME q2_own_read_hit_full\n"
        "#define Q2_ORH_ATTR __attribute__((noinline))\n"
        "#else\n"
        "#define Q2_ORH_NAME q2_own_read_hit_k\n"
        "#define Q2_ORH_ATTR __attribute__((always_inline))\n"
        "#endif\n"
        "static inline Q2_ORH_ATTR int "
        "Q2_ORH_NAME(const void *pic, void *const *holders,\n"
        "                       JSValue recv, int site, int nsites,\n"
        "                       JSValue *out, int lz, int *proto)\n"
        "{\n"
        "  const unsigned char *obj, *e, *holder, *prop;\n"
        "  uint32_t slot_off;\n"
        "  JSValue v;\n"
        "\n"
        "  *proto = 0;\n"
        "  if (pic == NULL || (unsigned)site == QJS_AOT_REG_IC_NONE) {\n"
        "    return 0;\n"
        "  }\n"
        "  if (JS_VALUE_GET_TAG(recv) != JS_TAG_OBJECT) {\n"
        "    return 0;\n"
        "  }\n"
        "  obj = (const unsigned char *)JS_VALUE_GET_PTR(recv);\n"
        "  e = (const unsigned char *)pic + (size_t)site * "
        "QJS_AOT_SIZEOF_JSREGPIC;\n"
        "  /* A polymorphic site puts its later shapes in way 1 and up, not at\n"
        "     `site`, so probing only the primary misses them forever. Walk the\n"
        "     ways exactly as regvm_get_field_ic does. */\n"
        "  {\n"
        "    const unsigned char *w = e;\n"
        "    unsigned k;\n"
        "    for (k = 0; k < QJS_AOT_REG_PIC_WAYS; k++) {\n"
        "      if (*(const void *const *)w ==\n"
        "          *(const void *const *)(obj + QJS_AOT_OFF_JSOBJECT_SHAPE))\n"
        "        break;\n"
        "      w += (size_t)nsites * QJS_AOT_SIZEOF_JSREGPIC;\n"
        "    }\n"
        "    if (k == QJS_AOT_REG_PIC_WAYS) return 0;\n"
        "    e = w;\n"
        "  }\n"
        "  if (*(const uint16_t *)(e + QJS_AOT_OFF_JSREGPIC_KIND) !=\n"
        "      QJS_AOT_REG_PIC_OWN) {\n"
        "    unsigned kind = *(const uint16_t *)(e + QJS_AOT_OFF_JSREGPIC_KIND);\n"
        "    if (kind == QJS_AOT_REG_PIC_PROTO) {\n"
        "      holder = *(const unsigned char *const *)(*(const unsigned char "
        "*const *)(\n"
        "                   obj + QJS_AOT_OFF_JSOBJECT_SHAPE) + "
        "QJS_AOT_OFF_JSSHAPE_PROTO);\n"
        "      if (holder == NULL ||\n"
        "          *(const void *const *)(holder + QJS_AOT_OFF_JSOBJECT_SHAPE) "
        "!=\n"
        "              holders[(size_t)(e - (const unsigned char *)pic) / "
        "QJS_AOT_SIZEOF_JSREGPIC]) {\n"
        "        return 0;\n"
        "      }\n"
        "      obj = holder;\n"
        "      *proto = 1;\n"
        "    } else if (kind == QJS_AOT_REG_PIC_PROTO2) {\n"
        "      /* Two prototypes up. Out of line: this path is rare next to an own\n"
        "         or one-level prototype read, and the caller is always_inline, so\n"
        "         keeping it here would grow every read site in the unit. */\n"
        "      holder = q2_holder2_of(obj, holders, nsites, e, pic);\n"
        "      if (holder == NULL) return 0;\n"
        "      obj = holder;\n"
        "      *proto = 1;\n"
        "    } else {\n"
        "      return 0;\n"
        "    }\n"
        "  }\n"
        "  prop = *(const unsigned char *const *)(obj + "
        "QJS_AOT_OFF_JSOBJECT_PROP);\n"
        "  slot_off = *(const uint32_t *)(e + QJS_AOT_OFF_JSREGPIC_SLOT_OFF);\n"
        "  v = *(const JSValue *)(prop + slot_off);\n"
        "#if !QJS_DRC\n"
        "  if (JS_VALUE_GET_TAG(v) < 0) {\n"
        "    return 0; /* registers own: the slow path takes the reference */\n"
        "  }\n"
        "#endif\n"
        "  *out = v;\n"
        "  return 1;\n"
        "}\n"
        "#if defined(E_LOGO_COLD) && defined(E_ORH_SPLIT)\n"
        "static inline __attribute__((always_inline)) int "
        "q2_own_read_hit_k(const void *pic, void *const *holders,\n"
        "                       JSValue recv, int site, int nsites,\n"
        "                       JSValue *out, int lz, int *proto)\n"
        "{\n"
        "  if (pic != NULL && (unsigned)site != QJS_AOT_REG_IC_NONE && JS_VALUE_GET_TAG(recv) == JS_TAG_OBJECT) {\n"
        "    const unsigned char *obj = (const unsigned char *)JS_VALUE_GET_PTR(recv);\n"
        "    const unsigned char *e = (const unsigned char *)pic + (size_t)site * QJS_AOT_SIZEOF_JSREGPIC;\n"
        "    if (*(const void *const *)e == *(const void *const *)(obj + QJS_AOT_OFF_JSOBJECT_SHAPE) &&\n"
        "        *(const uint16_t *)(e + QJS_AOT_OFF_JSREGPIC_KIND) == QJS_AOT_REG_PIC_OWN) {\n"
        "      JSValue v = *(const JSValue *)(*(const unsigned char *const *)(obj + QJS_AOT_OFF_JSOBJECT_PROP) +\n"
        "                                     *(const uint32_t *)(e + QJS_AOT_OFF_JSREGPIC_SLOT_OFF));\n"
        "      if (1\n"
        "#if !QJS_DRC\n"
        "          && JS_VALUE_GET_TAG(v) >= 0\n"
        "#endif\n"
        "          ) {\n"
        "        *proto = 0; *out = v; return 1;\n"
        "      }\n"
        "    }\n"
        "  }\n"
        "  return q2_own_read_hit_full(pic, holders, recv, site, nsites, out, lz, proto);\n"
        "}\n"
        "#endif\n"
        "static inline __attribute__((always_inline)) int "
        "q2_own_read_hit_lz(const void *pic, void *const *holders,\n"
        "                       JSValue recv, int site, int nsites,\n"
        "                       JSValue *out, int lz)\n"
        "{\n"
        "  int proto;\n"
        "  return q2_own_read_hit_k(pic, holders, recv, site, nsites, out, lz, &proto);\n"
        "}\n"
        /* A prototype method read at a site whose receivers have several shapes: the
           (shape, method) pairs the site has read are kept per site while the runtime's
           cache epoch stays what it was when they were read, so the next read of a known
           shape is a short compare chain, not a walk of the site's ways. A shape not in the
           table reads through the ways and is added. */
        "static inline __attribute__((always_inline)) const void *q2_pe_of(JSValue fnv)\n"
        "{\n"
        "  const unsigned char *o, *b;\n"
        "  if (JS_VALUE_GET_TAG(fnv) != JS_TAG_OBJECT) return NULL;\n"
        "  o = (const unsigned char *)JS_VALUE_GET_PTR(fnv);\n"
        "  if (*(const uint16_t *)(o + QJS_AOT_OFF_JSOBJECT_CLASS_ID) != QJS_AOT_CLASS_BYTECODE_FUNCTION) return NULL;\n"
        "  b = *(const unsigned char *const *)(o + QJS_AOT_OFF_OBJECT_FUNC_BYTECODE);\n"
        "  return *(const void *const *)(b + QJS_AOT_OFF_FNBC_AOT_FN);\n"
        "}\n"
        "typedef struct Q2PolyC { const void *pic; uint32_t ep; uint32_t n;\n"
        "                          const void *sh[4]; JSValue v[4]; const void *e[4]; } Q2PolyC;\n"
        "static inline __attribute__((always_inline)) int "
        "q2_poly_read_lz(const void *pic, void *const *holders,\n"
        "                       JSValue recv, int site, int nsites,\n"
        "                       JSValue *out, int lz, Q2PolyC *c, const uint32_t *epp)\n"
        "{\n"
        "  const void *s;\n"
        "  int proto;\n"
        "  uint32_t ep;\n"
        "  if (JS_VALUE_GET_TAG(recv) != JS_TAG_OBJECT) return 0;\n"
        "  s = *(const void *const *)((const unsigned char *)JS_VALUE_GET_PTR(recv) +\n"
        "                             QJS_AOT_OFF_JSOBJECT_SHAPE);\n"
        "  ep = *epp;\n"
        "  if (__builtin_expect(c->ep == ep && c->pic == pic, 1)) {\n"
        "    if (s == c->sh[0]) { *out = c->v[0]; return 1; }\n"
        "    if (s == c->sh[1]) { *out = c->v[1]; return 1; }\n"
        "    if (s == c->sh[2]) { *out = c->v[2]; return 1; }\n"
        "    if (s == c->sh[3]) { *out = c->v[3]; return 1; }\n"
        "  }\n"
        "  if (!q2_own_read_hit_k(pic, holders, recv, site, nsites, out, lz, &proto)) return 0;\n"
        "  if (proto && *epp == ep) {\n"
        "    if (c->ep != ep || c->pic != pic) {\n"
        "      c->pic = pic; c->ep = ep; c->n = 0;\n"
        "      c->sh[0] = c->sh[1] = c->sh[2] = c->sh[3] = NULL;\n"
        "    }\n"
        "    if (c->n < 4) { c->sh[c->n] = s; c->v[c->n] = *out; c->e[c->n] = q2_pe_of(*out); c->n++; }\n"
        "  }\n"
        "  return 1;\n"
        "}\n"
        /* The same read in an inline body that validated its caches at entry: its epoch alone says the
           table was filled under the epoch the body runs at, as for the statics it reads elsewhere. The
           entry the function's bytecode is bound to is final once the bytecode is, so the table keeps it
           per way and a call site tests it by comparison alone. */
        "static inline __attribute__((always_inline)) int "
        "q2_poly_read_lze(const void *pic, void *const *holders,\n"
        "                       JSValue recv, int site, int nsites,\n"
        "                       JSValue *out, int lz, Q2PolyC *c, const uint32_t *epp, uint32_t qep, const void **ent)\n"
        "{\n"
        "  const void *s;\n"
        "  if (JS_VALUE_GET_TAG(recv) != JS_TAG_OBJECT) return 0;\n"
        "  s = *(const void *const *)((const unsigned char *)JS_VALUE_GET_PTR(recv) +\n"
        "                             QJS_AOT_OFF_JSOBJECT_SHAPE);\n"
        "  if (__builtin_expect(c->ep == qep, 1)) {\n"
        "    if (s == c->sh[0]) { *out = c->v[0]; *ent = c->e[0]; return 1; }\n"
        "    if (s == c->sh[1]) { *out = c->v[1]; *ent = c->e[1]; return 1; }\n"
        "    if (s == c->sh[2]) { *out = c->v[2]; *ent = c->e[2]; return 1; }\n"
        "    if (s == c->sh[3]) { *out = c->v[3]; *ent = c->e[3]; return 1; }\n"
        "  }\n"
        "  if (!q2_poly_read_lz(pic, holders, recv, site, nsites, out, lz, c, epp)) return 0;\n"
        "  *ent = q2_pe_of(*out);\n"
        "  return 1;\n"
        "}\n"
        "/* undefined into n slots, one 16-byte store each */\n"
        "typedef long long q2_v2_t __attribute__((vector_size(16), aligned(8), may_alias));\n"
        "#define Q2_FILL_UNDEF(p_, n_) do { const q2_v2_t u_ = { 0, (long long)JS_TAG_UNDEFINED }; int k_; \\\n"
        "    for (k_ = 0; k_ < (n_); k_++) *(q2_v2_t *)&(p_)[k_] = u_; } while (0)\n"
        "static inline __attribute__((always_inline)) void q2_fillu_(JSValue *p, int n) {\n"
        "    const q2_v2_t u_ = { 0, (long long)JS_TAG_UNDEFINED }; int k_;\n"
        "    for (k_ = 0; k_ < n; k_++) *(q2_v2_t *)&p[k_] = u_; }\n"
        "#define E_RTOF(ctx_) (*(uint8_t *const *)((const uint8_t *)(ctx_) + QJS_AOT_OFF_CTX_RT))\n"
        "#define Q2_LAYOUT_BROKEN(ctx_) (*(const uint64_t *)(E_RTOF(ctx_) + QJS_AOT_OFF_RT_LAYOUT_BROKEN))\n"
        "#define Q2_HTMLDDA(ctx_) (*(const uint8_t *)(E_RTOF(ctx_) + QJS_AOT_OFF_RT_HTMLDDA))\n"
        "#define Q2_LZ 1\n"
        "#define q2_elem_hit(a, k, o) q2_elem_hit_lz(a, k, o, Q2_LZ)\n"
        "#define q2_own_read_hit(...) q2_own_read_hit_lz(__VA_ARGS__, Q2_LZ)\n"
        /* some cache entry has changed since this body's entry validated its cache cells */
        "#ifdef Q2_FORCE_EPOCH\n"
        "#define Q2_EPOCH_MOVED(ctx_) ((void)q2_ep, 1)\n"
        "#else\n"
        "#define Q2_EPOCH_MOVED(ctx_) (*(const uint32_t *)(q2_rt + QJS_AOT_OFF_RT_PIC_EPOCH) != q2_ep ? (e_ep_exit = 1, 1) : 0)\n"
        "#endif\n\n");
    /* A reference taken inline: the count is an int QJS_AOT_RC_BACK bytes before
       the object (the layout publishes the offset), and this is js_dup()'s body,
       so a counted store or a return costs no call into the engine. Release
       stays JS_FreeValue, whose zero path parks the value. */
    /* The born-dead nursery (QJS_AOT_NURSERY, on by default): a plain object made by compiled code comes from the runtime's young region with a negative
       count; a store into a young cell neither takes nor drops a count, and a counted reference to one is noted as a root. With the nursery off the
       macros below collapse to the ordinary counted forms. */
    e_nursery_decide(eb, g_inl_eb, sel, nfuncs);
    if (e_nursery())
      fputs(
          "extern unsigned long long qjs_y_ep48;\n"
          "static __attribute__((noinline, cold)) void q2_note_young_(JSValue v) { /* a counted reference to a young cell (its count is negative): record it as a root while the count is still near the bias */ "
          " int *rc_ = (int *)((uint8_t *)JS_VALUE_GET_PTR(v) - QJS_AOT_RC_BACK); if ((unsigned)*rc_ - 0x80000000u <= 2u) { "
          "extern JSObject **y_roots; extern uint32_t y_roots_n, y_roots_cap; extern void qjs_y_note_slow(void *); "
          "if (__builtin_expect(y_roots_n >= y_roots_cap, 0)) qjs_y_note_slow(JS_VALUE_GET_PTR(v)); else y_roots[y_roots_n++] = (JSObject *)JS_VALUE_GET_PTR(v); } }\n"
          "static inline __attribute__((always_inline)) void q2_inc_(JSValue v) { int *rc_ = (int *)((uint8_t *)JS_VALUE_GET_PTR(v) - QJS_AOT_RC_BACK);\n"
          " if (__builtin_expect(++*rc_ < 0, 0)) q2_note_young_(v);\n"
          " }\n"
          "static inline __attribute__((always_inline)) void q2_dupn(JSValue v) { if (JS_VALUE_HAS_REF_COUNT(v)) q2_inc_(v); }\n"
          "#define Q2_INC(v_) q2_inc_(v_)\n"
          "typedef struct { unsigned char *bump, *end; unsigned long long hdrw; unsigned stride, ready; unsigned char pad_[32]; } QjsYCls; extern QjsYCls qjs_ycls[16]; "
          "int qjs_y_refill(JSContext *, unsigned, unsigned, unsigned long long); void qjs_y_trip(JSContext *); void qjs_y_adopt(JSContext *, JSValue); void qjs_y_mut(void); void qjs_y_promote_v(JSContext *, JSValue);\n"
          "typedef struct { unsigned char pre, tick; unsigned short pad_; unsigned made, surv, am_, as_, sp_[3]; } QjsYSite; extern QjsYSite qjs_ysite[4096]; unsigned qjs_y_site_assign(void *);\n"
          "extern JSObject **qjs_y_log; extern uint32_t qjs_y_n, qjs_y_cap, qjs_y_lim; void qjs_y_grow(void);\n"
          "#define Q2_YREG(p_) (((uintptr_t)(p_) >> 30) == (0x400000000000ull >> 30))\n"
          "static inline __attribute__((always_inline)) int q2_yis_(const unsigned char *p) { return Q2_YREG(p) && *(const int *)(p - QJS_AOT_RC_BACK) < 0; }\n"
          "#define Q2_YMP(pp_) do { const unsigned char *c_ = (const unsigned char *)(pp_) - QJS_AOT_OFF_OBJECT_U; "
          "if (*(const unsigned char *const *)(c_ + QJS_AOT_OFF_JSOBJECT_PROP) == (const unsigned char *)(pp_) && q2_yis_(c_)) qjs_y_mut(); } while (0)\n"
          "#define Q2_YVAL(v_) (JS_VALUE_GET_TAG(v_) == JS_TAG_OBJECT && q2_yis_((const unsigned char *)JS_VALUE_GET_PTR(v_)))\n"
          "#define Q2_YCP(pp_) q2_yis_((const unsigned char *)(pp_) - QJS_AOT_OFF_OBJECT_U)\n"
          "#define Q2_YCR(recv_) q2_yis_((const unsigned char *)JS_VALUE_GET_PTR(recv_))\n"
          "#define Q2_YMR(recv_) do { const unsigned char *c_ = (const unsigned char *)JS_VALUE_GET_PTR(recv_); "
          "if (JS_VALUE_GET_TAG(recv_) == JS_TAG_OBJECT && q2_yis_(c_)) qjs_y_mut(); } while (0)\n"
          "#define Q2_YPRO(ctx_, recv_) do { if (__builtin_expect(q2_yis_((const unsigned char *)JS_VALUE_GET_PTR(recv_)), 0)) qjs_y_promote_v(ctx_, recv_); } while (0)\n"
          "#define Q2_SHAPE_UNCOUNTED(p_) q2_yis_((const unsigned char *)(p_))\n"
          "#define Q2YD_P(pp_) int Q2Y_ = __builtin_expect(Q2_YCP(pp_), 0)\n"
          "#define Q2YD_R(recv_) int Q2Y_ = __builtin_expect(JS_VALUE_GET_TAG(recv_) == JS_TAG_OBJECT && Q2_YCR(recv_), 0)\n"
          "#define Q2Y_DUP(v_) do { if (!Q2Y_) q2_dupn(v_); } while (0)\n"
          "#define Q2Y_DUPNL(v_) do { if (!Q2Y_) q2_dupn(v_); } while (0)\n"
          "#define Q2Y_REL(...) do { if (!Q2Y_) { __VA_ARGS__; } } while (0)\n"
          "#define Q2Y_OLD(old_) (Q2Y_ ? JS_UNDEFINED : (old_))\n"
          "#define Q2Y_YOUNG(rc_) ((void)0)\n"
          "#define Q2_FREE_JCC \"je\"\n"
          "#define Q2_FREE_LIVE(n_) ((n_) != 0)\n"
          "#define Q2Y_OR(rc_) || (unsigned)(rc_) > 0x80000001u\n\n",
          f);
    else
      fputs(
          "extern void qjs_aot_set_form(int);\n"
          "static void __attribute__((constructor, unused)) qjs_aot_noyoung_reg(void) { qjs_aot_set_form(8); }\n"
          "#define q2_dupn(v_) q2_dup(v_)\n"
          "#define Q2_INC(v_) (++*(int *)((uint8_t *)JS_VALUE_GET_PTR(v_) - QJS_AOT_RC_BACK))\n"
          "#define Q2_YPRO(ctx_, recv_) ((void)0)\n"
          "#define Q2_SHAPE_UNCOUNTED(p_) 0\n"
          "#define Q2YD_P(pp_) ((void)0)\n"
          "#define Q2YD_R(recv_) ((void)0)\n"
          "#define Q2Y_ 0\n"
          "#define Q2Y_DUP(v_) q2_dup(v_)\n"
          "#define Q2Y_DUPNL(v_) q2_dup_nl(v_)\n"
          "#define Q2Y_REL(...) __VA_ARGS__\n"
          "#define Q2Y_OLD(old_) (old_)\n"
          "#define Q2Y_YOUNG(rc_) ((void)0)\n"
          "#define Q2_FREE_JCC \"jle\"\n"
          "#define Q2_FREE_LIVE(n_) ((n_) > 0)\n"
          "#define Q2Y_OR(rc_)\n\n",
          f);
    fprintf(
        f,
        "static inline __attribute__((always_inline)) void q2_dup(JSValue v)\n"
        "{\n"
        "  if (JS_VALUE_HAS_REF_COUNT(v))\n"
        "    Q2_INC(v);\n"
        "}\n"
        "static inline __attribute__((always_inline)) void q2_dup_y_(JSValue v) { q2_dupn(v); }\n\n");
    /* Releasing a reference: a count that stays above zero is a decrement. Only the
       last reference needs the engine. */
    fprintf(
        f,
        "static inline __attribute__((always_inline)) void q2_free_obj(JSContext *ctx, JSValue v)\n"
        "{\n"
        "  int *rc = (int *)((uint8_t *)JS_VALUE_GET_PTR(v) - QJS_AOT_RC_BACK);\n"
        "#if defined(__x86_64__)\n"
        "  __asm__ goto(\"subl $1, %0\\n\\t\" Q2_FREE_JCC \" %l1\" : : \"m\"(*rc) : \"cc\", \"memory\" : zero);\n"
        "  return;\n"
        "zero:\n"
        "  Q2Y_YOUNG(rc);\n"
        "#else\n"
        "  if (__builtin_expect(Q2_FREE_LIVE(--*rc), 1)) return;\n"
        "  Q2Y_YOUNG(rc);\n"
        "#endif\n"
        "  ++*rc;\n"
        "  (qjs_aot_drc_zero)(ctx, v);\n"
        "}\n"
        "static inline __attribute__((always_inline)) void q2_free(JSContext *ctx, JSValue v)\n"
        "{\n"
        "  if (JS_VALUE_HAS_REF_COUNT(v)) q2_free_obj(ctx, v);\n"
        "}\n"
        "#define JS_FreeValue(c_, v_) q2_free(c_, v_)\n\n");
    /* Adopting an owned value into a frame slot: its count drops by the one the slot's
       register does not hold. Only a count that would reach zero (the zero-count table
       takes the value) needs the engine. */
    fprintf(
        f,
        "#if defined(E_LOGO_COLD) && !defined(E_KEEP_SETREG)\n"
        "static __attribute__((noinline)) void q2_set_reg_last(JSContext *ctx, JSValue *fr, int off, JSValue v)\n"
        "{\n"
        "  int *rc = (int *)((uint8_t *)JS_VALUE_GET_PTR(v) - QJS_AOT_RC_BACK); (void)rc;\n"
        "%1$s"
        "  (qjs_aot_set_reg_p)(ctx, fr, off, v);\n"
        "}\n"
        "#endif\n"
        "static inline __attribute__((always_inline)) void q2_set_reg_p(JSContext *ctx, "
        "JSValue *fr, int off, JSValue v)\n"
        "{\n"
        "  if (!JS_VALUE_HAS_REF_COUNT(v)) {\n"
        "    *(JSValue *)((uint8_t *)fr + off) = v;\n"
        "    return;\n"
        "  }\n"
        "  {\n"
        "    int *rc = (int *)((uint8_t *)JS_VALUE_GET_PTR(v) - QJS_AOT_RC_BACK);\n"
        "    if (__builtin_expect(*rc > 1, 1) Q2Y_OR(*rc)) {\n"
        "      *rc -= 1;\n"
        "      *(JSValue *)((uint8_t *)fr + off) = v;\n"
        "      return;\n"
        "    }\n"
        "#if defined(E_LOGO_COLD) && !defined(E_KEEP_SETREG)\n"
        "    q2_set_reg_last(ctx, fr, off, v);\n"
        "    return;\n"
        "#else\n"
        "%1$s"
        "#endif\n"
        "  }\n"
        "  (qjs_aot_set_reg_p)(ctx, fr, off, v);\n"
        "}\n"
        "#define qjs_aot_set_reg_p(c_, f_, o_, v_) q2_set_reg_p(c_, f_, o_, v_)\n\n",
        getenv("QJS_AOT_NOINLPARK") != NULL ? "" :
        "    if (JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT) {\n"
        "#ifndef Q2_NOPEND\n"
        "      /* The last reference is kept by the runtime's one-object hold instead of being parked: the hold's previous object gives its\n"
        "         reference back (a count of zero parks it). A loop that stores each object into the next one it makes never parks. */\n"
        "      {\n"
        "        unsigned char *rtp_ = *(unsigned char **)((unsigned char *)ctx + QJS_AOT_OFF_CTX_RT);\n"
        "        if (__builtin_expect(*(rtp_ + QJS_AOT_OFF_RT_DRC_IN_RECONCILE) == 0 && *(rtp_ + QJS_AOT_OFF_RT_GC_PHASE) == 0, 1)) {\n"
        "          unsigned char **pend_ = (unsigned char **)(rtp_ + QJS_AOT_OFF_RT_DRC_PEND);\n"
        "          unsigned char *old_ = pend_[1];\n"
        "          pend_[1] = pend_[0];\n"
        "          pend_[0] = (unsigned char *)JS_VALUE_GET_PTR(v);\n"
        "          ++*(uint32_t *)(rtp_ + QJS_AOT_OFF_RT_ZCT_NOBJ); /* counts toward the next reconcile as a park would: it keeps the working set small */\n"
        "          *(JSValue *)((uint8_t *)fr + off) = v;\n"
        "#ifdef Q2_PEND_NOREL\n          if (0) {\n#else\n          if (old_ != 0) {\n#endif\n"
        "            int *orc_ = (int *)(old_ - QJS_AOT_RC_BACK);\n"
        "            if (__builtin_expect(*orc_ > 1, 1)) *orc_ -= 1;\n"
        "            else (qjs_aot_drc_zero)(ctx, JS_MKPTR(JS_TAG_OBJECT, old_));\n"
        "          }\n"
        "          return;\n"
        "        }\n"
        "      }\n"
        "#endif\n"
        "      /* The count would reach zero and the value is a plain object: park it on the side array here, as js_drc_zero does,\n"
        "         unless a reconcile or a collection is running or the array is full (the engine then). */\n"
        "      uint8_t *p_ = (uint8_t *)JS_VALUE_GET_PTR(v);\n"
        "      uint8_t *rt_ = *(uint8_t **)((uint8_t *)ctx + QJS_AOT_OFF_CTX_RT);\n"
        "      if (__builtin_expect(*(const uint16_t *)(p_ + QJS_AOT_OFF_JSOBJECT_CLASS_ID) == QJS_AOT_CLASS_OBJECT &&\n"
        "                           *(rt_ + QJS_AOT_OFF_RT_DRC_IN_RECONCILE) == 0 && *(rt_ + QJS_AOT_OFF_RT_GC_PHASE) == 0, 1)) {\n"
        "        uint8_t *zb_ = p_ - QJS_AOT_ZCT_BACK;\n"
        "        if (*zb_ & QJS_AOT_ZCT_MASK) {\n"
        "          *rc = 0;\n"
        "          *(JSValue *)((uint8_t *)fr + off) = v;\n"
        "          return;\n"
        "        }\n"
        "        {\n"
        "          uint32_t n_ = *(uint32_t *)(rt_ + QJS_AOT_OFF_RT_ZPARK_LEN);\n"
        "          if (__builtin_expect(n_ < *(uint32_t *)(rt_ + QJS_AOT_OFF_RT_ZPARK_CAP), 1)) {\n"
        "            (*(void ***)(rt_ + QJS_AOT_OFF_RT_ZPARK))[n_] = p_;\n"
        "            *(uint32_t *)(rt_ + QJS_AOT_OFF_RT_ZPARK_LEN) = n_ + 1;\n"
        "            ++*(uint32_t *)(rt_ + QJS_AOT_OFF_RT_ZCT_NOBJ);\n"
        "            *zb_ |= QJS_AOT_ZCT_MASK;\n"
        "            *rc = 0;\n"
        "            *(JSValue *)((uint8_t *)fr + off) = v;\n"
        "            return;\n"
        "          }\n"
        "        }\n"
        "      }\n"
        "    }\n");
    /* Allocation of a plain object from the runtime's block cache, in compiled code: what js_new_plain_object does on its cached-block path
       (the accounting, the block header byte, the object's first words, the shape reference, the GC list link). NULL when the cache has no
       block of the class, the size is not a plain-object class or the memory limit is near: the caller then takes the engine's helper. */
    fprintf(
        f,
        "extern void qjs_aot_dbg_note(int k);\n"
        "#ifdef Q2_OA_DEBUG\n#define q2_oa_note(k) qjs_aot_dbg_note(k)\n#else\n#define q2_oa_note(k) ((void)0)\n#endif\n"
        "static const unsigned q2_plain_bsi[5] = QJS_AOT_PLAIN_BSI;\n"
        "static const unsigned q2_plain_acct[5] = QJS_AOT_PLAIN_ACCT;\n"
        "static const unsigned long long q2_plain_hdr[5] = QJS_AOT_PLAIN_HDR;\n"
        "static inline __attribute__((always_inline)) unsigned char *q2_alloc_plain(JSContext *ctx, const void *sh, unsigned cap)\n"
        "{\n"
        "#ifdef Q2_STRESS\n  (void)ctx; (void)sh; (void)cap; return 0;\n#endif\n"
        "  unsigned char *rt = *(unsigned char **)((unsigned char *)ctx + QJS_AOT_OFF_CTX_RT);\n"
        "  unsigned bsi = q2_plain_bsi[cap];\n"
        "  unsigned char *p;\n"
        "  unsigned long *msz = (unsigned long *)(rt + QJS_AOT_OFF_RT_MALLOC_SIZE);\n"
        "  if (__builtin_expect(bsi == 0xffu, 0)) { q2_oa_note(0); return 0; }\n"
        "  p = ((unsigned char **)(rt + QJS_AOT_OFF_RT_BLKCACHE))[bsi];\n"
        "  if (__builtin_expect(p == 0 || *msz + q2_plain_acct[cap] > *(unsigned long *)(rt + QJS_AOT_OFF_RT_MALLOC_LIMIT) - 1, 0)) { q2_oa_note(p == 0 ? 1 : 2); return 0; }\n"
        "  {\n"
        "    unsigned char *nx = *(unsigned char **)p;\n"
        "    unsigned char **head = (unsigned char **)(rt + QJS_AOT_OFF_RT_GC_OBJ_LIST);\n"
        "    unsigned char *tail = head[0];\n"
        "    ((unsigned char **)(rt + QJS_AOT_OFF_RT_BLKCACHE))[bsi] = nx;\n"
        "    --((unsigned *)(rt + QJS_AOT_OFF_RT_BLKCACHE_N))[bsi];\n"
        "    if (nx) __builtin_prefetch(nx - QJS_AOT_RC_BACK, 1, 3);\n"
        "    ++*(unsigned long *)(rt + QJS_AOT_OFF_RT_MALLOC_COUNT);\n"
        "    *msz += q2_plain_acct[cap];\n"
        "    *(p - QJS_AOT_ZCT_BACK) = (unsigned char)QJS_AOT_PLAIN_GCBYTE;\n"
        "    *(int *)(p - QJS_AOT_RC_BACK) = 1;\n"
        "    *(unsigned long long *)(p + QJS_AOT_OFF_JSOBJECT_FLAGS_BYTE) = q2_plain_hdr[cap];\n"
        "    *(const void **)(p + QJS_AOT_OFF_JSOBJECT_SHAPE) = sh;\n"
        "    *(unsigned char **)(p + QJS_AOT_OFF_JSOBJECT_PROP) = p + QJS_AOT_OFF_OBJECT_U;\n"
        "    *(void **)(p + QJS_AOT_OFF_JSOBJECT_PROP + 8) = 0;\n"
        "    ++*(int *)((const unsigned char *)sh - QJS_AOT_RC_BACK);\n"
        "#ifndef QJS_EXP_NOLIST\n"
        "    ((unsigned char **)p)[1] = (unsigned char *)head;\n"
        "    ((unsigned char **)p)[0] = tail;\n"
        "    ((unsigned char **)tail)[1] = p;\n"
        "    head[0] = p;\n"
        "#else\n    (void)head; (void)tail;\n#endif\n"
        "  }\n"
        "  return p;\n"
        "}\n"
        "static inline __attribute__((always_inline)) int q2_obj_template(JSContext *ctx, struct JSFunctionBytecode *cb, int idx, JSValue *out)\n"
        "{\n"
        "  const unsigned char *tpl = *(const unsigned char *const *)((const unsigned char *)cb + QJS_AOT_OFF_FNBC_OBJLIT_TAB) +\n"
        "                             (unsigned long)idx * QJS_AOT_SIZEOF_OBJLIT_TPL;\n"
        "  const unsigned char *sh = *(const unsigned char *const *)(tpl + QJS_AOT_OFF_TPL_SHAPE);\n"
        "  if (__builtin_expect(sh != 0, 1)) {\n"
        "    unsigned cap = (unsigned)*(const int *)(sh + QJS_AOT_OFF_JSSHAPE_PROP_SIZE);\n"
        "    if (__builtin_expect(cap - 1u < QJS_AOT_OBJ_INLINE_PROPS &&\n"
        "          *(const void *const *)(sh + QJS_AOT_OFF_JSSHAPE_PROTO) ==\n"
        "          *(const void *const *)(*(const unsigned char *const *)((const unsigned char *)ctx + QJS_AOT_OFF_CTX_CLASS_PROTO) + QJS_AOT_CLASS_OBJECT * 16), 1)) {\n"
        "      unsigned char *p = q2_alloc_plain(ctx, sh, cap);\n"
        "      if (__builtin_expect(p != 0, 1)) {\n"
        "        unsigned n = *(const unsigned short *)(tpl + QJS_AOT_OFF_TPL_PROP_COUNT), i;\n"
        "        JSValue *pr = (JSValue *)(p + QJS_AOT_OFF_OBJECT_U);\n"
        "        for (i = 0; i < n; i++) pr[i] = JS_UNDEFINED;\n"
        "        *out = JS_MKPTR(JS_TAG_OBJECT, p);\n        q2_oa_note(4);\n"
        "        return 0;\n"
        "      }\n"
        "    }\n"
        "  }\n"
        "  q2_oa_note(3);\n  return qjs_aot_object_template_b(ctx, cb, idx, out);\n"
        "}\n"
        "static inline __attribute__((always_inline)) JSValue q2_new_from_shape_cap(JSContext *ctx, void *shape, unsigned cap)\n"
        "{\n"
        "  const unsigned char *sh = (const unsigned char *)shape;\n"
        "  unsigned n = (unsigned)*(const int *)(sh + QJS_AOT_OFF_JSSHAPE_PROP_COUNT), size = (unsigned)*(const int *)(sh + QJS_AOT_OFF_JSSHAPE_PROP_SIZE);\n"
        "  if (__builtin_expect(cap <= n, 1)) {\n"
        "    unsigned room = (size > n && size <= QJS_AOT_OBJ_INLINE_PROPS) ? size : n;\n"
        "    if (__builtin_expect(room - 1u < QJS_AOT_OBJ_INLINE_PROPS, 1)) {\n"
        "      unsigned char *p = q2_alloc_plain(ctx, sh, room);\n"
        "      if (__builtin_expect(p != 0, 1)) {\n"
        "        JSValue *pr = (JSValue *)(p + QJS_AOT_OFF_OBJECT_U);\n"
        "        unsigned i;\n"
        "        for (i = 0; i < room; i++) pr[i] = JS_UNDEFINED;\n"
        "        return JS_MKPTR(JS_TAG_OBJECT, p);\n"
        "      }\n"
        "    }\n"
        "  }\n"
        "  return qjs_aot_new_from_shape_cap(ctx, shape, cap);\n"
        "}\n"
        "");
    /* The cache-block (old-form) constructor fill, for builds with the nursery: the nursery's refusal (paused window, class not ready, region exhausted) goes
       through it inline instead of the engine's out-of-line constructor. Counted references: a moved argument keeps its count, a copied one gains one; either may be a nursery cell, which
       q2_dupn records as a root (a plain q2_dup would leave the old object holding a cell the next young collection treats as dead). */
    if (e_nursery())
        fputs(
        "static __attribute__((noinline)) JSValue q2_fill_old(JSContext *ctx, const unsigned char *sh, unsigned cap, JSValue *argv, int na,\n"
        "                                                             const uint32_t *fm, uint32_t steal)\n"
        "{\n"
        "  unsigned char *p = q2_alloc_plain(ctx, sh, cap);\n"
        "  if (__builtin_expect(p != 0, 1)) {\n"
        "    unsigned nf = fm[0], i;\n"
        "    JSValue *pr = (JSValue *)(p + QJS_AOT_OFF_OBJECT_U);\n"
        "    for (i = 0; i < nf; i++) {\n"
        "      unsigned ai = fm[2 + 2 * i];\n"
        "      if (ai >= (unsigned)na) pr[i] = JS_UNDEFINED;\n"
        "      else if (ai < 32 && ((steal >> ai) & 1)) { JSValue v = argv[ai]; pr[i] = v; steal &= ~(1u << ai); q2_dupn(v); q2_free(ctx, v); }\n"
        "      else { JSValue v = argv[ai]; q2_dupn(v); pr[i] = v; }\n"
        "    }\n"
        "    return JS_MKPTR(JS_TAG_OBJECT, p);\n"
        "  }\n"
        "  return JS_MKVAL(JS_TAG_UNINITIALIZED, 0);\n"
        "}\n", f);
    fputs(e_nursery() ?
        "static inline __attribute__((always_inline)) JSValue q2_fill_new(JSContext *ctx, const unsigned char *sh, unsigned cap, JSValue *argv, int na,\n"
        "                                                             const uint32_t *fm, uint32_t steal)\n"
        "{\n"
        "  if (__builtin_expect(cap - 1u < QJS_AOT_OBJ_INLINE_PROPS, 1)) {\n"
        "    unsigned bsi_;\n"
        "    QjsYCls *yk_ = &qjs_ycls[cap];\n"
        "    unsigned si_ = *(const unsigned short *)(sh + QJS_AOT_OFF_SHAPE_YSITE);\n"
        "    QjsYSite *ys_;\n"
        "    if (__builtin_expect(si_ == 0, 0)) si_ = qjs_y_site_assign((void *)sh);\n"
        "    ys_ = &qjs_ysite[si_];\n"
        "    if (__builtin_expect(ys_->pre, 0) && (++ys_->tick & 63)) goto y_old;\n"
        "    if (__builtin_expect((uintptr_t)yk_->bump + yk_->stride > (uintptr_t)yk_->end, 0)) {\n"
        "      bsi_ = q2_plain_bsi[cap]; if (!qjs_y_refill(ctx, cap, bsi_, QJS_AOT_PLAIN_GCBYTE)) goto y_old;\n"
        "    }\n"
        "    {\n"
        "      unsigned char *p = yk_->bump + 8;\n"
        "      unsigned nf = fm[0], i;\n"
        "      JSValue *pr = (JSValue *)(p + QJS_AOT_OFF_OBJECT_U);\n"
        "      yk_->bump += yk_->stride;\n"
        "      *(unsigned long long *)(p - 8) = yk_->hdrw;\n"
        "      ys_->made++;\n"
        "      *(unsigned *)p = si_;\n"
        "      *(unsigned long long *)(p + QJS_AOT_OFF_JSOBJECT_FLAGS_BYTE) = q2_plain_hdr[cap] | ((0x80ull << 40) | qjs_y_ep48);\n"
        "      *(const void **)(p + QJS_AOT_OFF_JSOBJECT_SHAPE) = sh;\n"
        "      *(unsigned char **)(p + QJS_AOT_OFF_JSOBJECT_PROP) = p + QJS_AOT_OFF_OBJECT_U;\n"
        "      *(void **)(p + QJS_AOT_OFF_JSOBJECT_PROP + 8) = 0;\n"
        "      for (i = 0; i < nf; i++) {\n"
        "        unsigned ai = fm[2 + 2 * i];\n"
        "        if (ai >= (unsigned)na) pr[i] = JS_UNDEFINED;\n"
        "        else if (ai < 32 && ((steal >> ai) & 1)) { JSValue v = argv[ai]; pr[i] = v; steal &= ~(1u << ai); q2_free(ctx, v); }\n"
        "        else { pr[i] = argv[ai]; }\n"
        "      }\n"
        "      return JS_MKPTR(JS_TAG_OBJECT, p);\n"
        "    }\n"
        "    y_old: return q2_fill_old(ctx, sh, cap, argv, na, fm, steal);\n"
        "  }\n"
        "  return JS_MKVAL(JS_TAG_UNINITIALIZED, 0);\n"
        "}\n"
        :
        "static inline __attribute__((always_inline)) JSValue q2_fill_new(JSContext *ctx, const unsigned char *sh, unsigned cap, JSValue *argv, int na,\n"
        "                                                             const uint32_t *fm, uint32_t steal)\n"
        "{\n"
        "  if (__builtin_expect(cap - 1u < QJS_AOT_OBJ_INLINE_PROPS, 1)) {\n"
        "    unsigned char *p = q2_alloc_plain(ctx, sh, cap);\n"
        "    if (__builtin_expect(p != 0, 1)) {\n"
        "      unsigned nf = fm[0], i;\n"
        "      JSValue *pr = (JSValue *)(p + QJS_AOT_OFF_OBJECT_U);\n"
        "      for (i = 0; i < nf; i++) {\n"
        "        unsigned ai = fm[2 + 2 * i];\n"
        "        if (ai >= (unsigned)na) pr[i] = JS_UNDEFINED;\n"
        "        else if (ai < 32 && ((steal >> ai) & 1)) { pr[i] = argv[ai]; steal &= ~(1u << ai); }\n"
        "        else { JSValue v = argv[ai]; q2_dupn(v); pr[i] = v; }\n"
        "      }\n"
        "      q2_oa_note(4);\n"
        "      return JS_MKPTR(JS_TAG_OBJECT, p);\n"
        "    }\n"
        "  }\n"
        "  return JS_MKVAL(JS_TAG_UNINITIALIZED, 0);\n"
        "}\n"
        , f);
    fprintf(
        f,
        /* A construct site that has proved its constructor once: the engine's checks (finished shape learned, the function object's shape, its prototype
           value against the finished shape's prototype) are replaced by a fact on the constructor listing its `prototype` property, which moves
           z->ok to 0 when the property is stored to, deleted or redefined. The finished shape is write-once in the function bytecode; the bytecode
           and the shape live as long as the fact's reference to the constructor. The instance capacity is still read, as it can grow. */
        "typedef struct { uint64_t ok; const unsigned char *ctor, *cb, *sh; unsigned cap0, tries; uint32_t ep; } Q2Fz;\n"
        "__attribute__((noinline, cold)) static void q2_fz_establish(JSContext *ctx, Q2Fz *z, JSObject *ctor)\n"
        "{\n"
        "  const unsigned char *cb = *(const unsigned char *const *)((const unsigned char *)ctor + QJS_AOT_OFF_OBJECT_FUNC_BYTECODE);\n"
        "  const unsigned char *sh;\n"
        "  JSValue pv, cv = JS_MKPTR(JS_TAG_OBJECT, ctor);\n"
        "  JSAtom pa;\n"
        "  JSAotFact *g;\n"
        "  unsigned cap;\n"
        "  if (z->tries >= 4) return;\n"
        "  z->tries++;\n"
        "  if (!(*(cb + QJS_AOT_OFF_FNBC_CTOR_FUSED_OK) != 0 &&\n"
        "        (*(const unsigned short *)((const unsigned char *)ctor + QJS_AOT_OFF_JSOBJECT_FLAGS_BYTE) & QJS_AOT_OBJECT_CTOR_MASK) != 0 &&\n"
        "        *(const void *const *)((const unsigned char *)ctor + QJS_AOT_OFF_JSOBJECT_SHAPE) == *(const void *const *)(cb + QJS_AOT_OFF_FNBC_CTOR_FN_SHAPE)))\n"
        "    return;\n"
        "  pv = ((const JSValue *)*(const unsigned char *const *)((const unsigned char *)ctor + QJS_AOT_OFF_JSOBJECT_PROP))\n"
        "           [*(const unsigned short *)(cb + QJS_AOT_OFF_FNBC_CTOR_PROTO_SLOT)];\n"
        "  sh = *(const unsigned char *const *)(cb + QJS_AOT_OFF_FNBC_CTOR_FINAL_SHAPE);\n"
        "  if (sh == NULL || JS_VALUE_GET_TAG(pv) != JS_TAG_OBJECT || JS_VALUE_GET_PTR(pv) != *(void *const *)(sh + QJS_AOT_OFF_JSSHAPE_PROTO)) return;\n"
        "  cap = (unsigned)*(const int *)(sh + QJS_AOT_OFF_JSSHAPE_PROP_SIZE);\n"
        "  if (cap > QJS_AOT_OBJ_INLINE_PROPS) cap = 0;\n"
        "  pa = JS_NewAtom(ctx, \"prototype\");\n"
        "  g = qjs_aot_fact_new(ctx, cv, JS_UNDEFINED, 0, &pa, 1);\n"
        "  JS_FreeAtom(ctx, pa);\n"
        "  if (g == NULL) return;\n"
        "  if (qjs_aot_fact_validate(ctx, g) != 1) { qjs_aot_fact_break(ctx, g); return; }\n"
        "  z->ctor = (const unsigned char *)ctor; z->cb = cb; z->sh = sh; z->cap0 = cap;\n"
        "  qjs_aot_fact_depend64(g, &z->ok, 1);\n"
        "  if (!qjs_aot_facts_commit(ctx, &z->ok, 1)) qjs_aot_fact_break(ctx, g);\n"
        "}\n"
        /* The inline build is taken only while the prototype cache epoch is the one the engine last validated the
           constructor's field stores against (no setter or read-only property on the chain for any field name);
           otherwise the engine answers, validating again, and the epoch it saw is remembered. */
        "#define Q2_FZ_EP(ctx_) (*(const uint32_t *)(*(const unsigned char *const *)((const unsigned char *)(ctx_) + QJS_AOT_OFF_CTX_RT) + QJS_AOT_OFF_RT_PIC_EPOCH))\n"
        "static inline __attribute__((always_inline)) JSValue q2_new_fz_try(JSContext *ctx, Q2Fz *z, JSObject *ctor, JSValue *argv, int na,\n"
        "                                                      const uint32_t *fm, uint32_t steal)\n"
        "{\n"
        "  JSValue o;\n"
        "  uint32_t ep0;\n"
        "  if (__builtin_expect(z->ok != 0 && (const unsigned char *)ctor == z->ctor && z->ep == Q2_FZ_EP(ctx), 1)) {\n"
        "    unsigned cap = z->cap0, req = *(z->cb + QJS_AOT_OFF_FNBC_INST_INLINE_CAP);\n"
        "    if (req > cap) cap = req;\n"
        "    o = q2_fill_new(ctx, z->sh, cap, argv, na, fm, steal);\n"
        "    if (__builtin_expect(JS_VALUE_GET_TAG(o) != JS_TAG_UNINITIALIZED, 1)) return o;\n"
        "  }\n"
        "  ep0 = Q2_FZ_EP(ctx);\n"
        "  o = qjs_aot_new_fused(ctx, ctor, argv, na, fm, steal);\n"
        "  if (JS_VALUE_GET_TAG(o) != JS_TAG_UNINITIALIZED) {\n"
        "    if (!z->ok && z->tries < 4) q2_fz_establish(ctx, z, ctor);\n"
        "    if (z->ok) z->ep = ep0;\n"
        "  }\n"
        "  return o;\n"
        "}\n"
        "static inline __attribute__((always_inline)) JSValue q2_new_fz(JSContext *ctx, Q2Fz *z, JSObject *ctor, JSValue *argv, int na,\n"
        "                                                      const uint32_t *fm, uint32_t steal)\n"
        "{\n"
        "  return q2_new_fz_try(ctx, z, ctor, argv, na, fm, steal);\n"
        "}\n\n");

    /* A grouped field store, after the caller has checked the shape: the same
       store the engine's fast path makes, minus the cache lookup (the offset
       comes from the group's cell). The new value is duplicated before the old
       one is released, so storing a slot's own value cannot free it. A
       prototype receiver goes to the engine, which invalidates the prototype
       epoch, so the caller exits on it first. */
    if (qjs_aot_layout_claims_any())
      fprintf(
          f,
          "static inline __attribute__((always_inline)) int q2_claim_bad(JSValue recv, uint32_t off,\n"
          "                                                             JSValue v);\n"
          "#define Q2_CLAIM_BAD(recv_, off_, v_) q2_claim_bad(recv_, off_, v_)\n"
          "#define Q2_ENTERS_LAYOUT(shape_) (*(const uint16_t *)((shape_) + QJS_AOT_OFF_SHAPE_LAYOUT_ID) != 0)\n"
          "#define Q2_OBJ_IN_LAYOUT(obj_) Q2_ENTERS_LAYOUT(*(const unsigned char *const *)((obj_) + QJS_AOT_OFF_JSOBJECT_SHAPE))\n");
    else
      fprintf(
          f,
          "#define Q2_CLAIM_BAD(recv_, off_, v_) 0\n"
          "#define Q2_ENTERS_LAYOUT(shape_) 0\n"
          "#define Q2_OBJ_IN_LAYOUT(obj_) 0\n");
    fprintf(
        f,
        "static inline __attribute__((always_inline)) int q2_is_proto(JSValue "
        "v)\n"
        "{\n"
        "  return (((const unsigned char *)JS_VALUE_GET_PTR(v))"
        "[QJS_AOT_OFF_JSOBJECT_FLAGS_BYTE] &\n"
        "          QJS_AOT_JSOBJECT_IS_PROTOTYPE_MASK) != 0;\n"
        "}\n"
        "static inline __attribute__((always_inline)) const unsigned char *q2_propp(JSValue v)\n"
        "{\n"
        "  const unsigned char *p_ = (const unsigned char *)JS_VALUE_GET_PTR(v);\n"
        "#ifndef QJS_AOT_NODIRECT\n"
        "  if (__builtin_expect(((*(const uint32_t *)(p_ + QJS_AOT_OFF_JSOBJECT_FLAGS16) & 0xFFFFF800u) - 0x10800u) <= 0xF000u, 1))\n"
        "    return p_ + QJS_AOT_OFF_OBJECT_U;\n"
        "#endif\n"
        "  return *(const unsigned char *const *)(p_ + QJS_AOT_OFF_JSOBJECT_PROP);\n"
        "}\n"
        "#define q2_slotpp(pp_, off_) ((JSValue *)((pp_) + (off_)))\n"
        "static inline __attribute__((always_inline)) void q2_put_slotpp(JSContext *ctx,\n"
        "    const unsigned char *pp, uint32_t off, JSValue v)\n"
        "{ Q2YD_P(pp);\n"
        "  JSValue *slot = q2_slotpp(pp, off);\n"
        "  JSValue old = *slot;\n"
        "  Q2Y_DUP(v);\n"
        "  *slot = v;\n"
        "  if (JS_VALUE_HAS_REF_COUNT(old)) Q2Y_REL(JS_FreeValue(ctx, old));\n"
        "}\n"
        /* The store of a slot whose old value an earlier load of the region holds: the old value
           is released without reading the slot again, and without a tag test when it is known to
           be an object. */
        "static inline __attribute__((always_inline)) void q2_put_slotpp_k(JSContext *ctx,\n"
        "    const unsigned char *pp, uint32_t off, JSValue v, JSValue old)\n"
        "{ Q2YD_P(pp);\n"
        "  Q2Y_DUP(v);\n"
        "  *q2_slotpp(pp, off) = v;\n"
        "  Q2Y_REL(JS_FreeValue(ctx, old));\n"
        "}\n"
        "static inline __attribute__((always_inline)) void q2_put_slotpp_ko(JSContext *ctx,\n"
        "    const unsigned char *pp, uint32_t off, JSValue v, JSValue old)\n"
        "{ Q2YD_P(pp);\n"
        "  Q2Y_DUP(v);\n"
        "  *q2_slotpp(pp, off) = v;\n"
        "  Q2Y_REL(q2_free_obj(ctx, old));\n"
        "}\n"
        "typedef struct { JSContext *ctx; JSValue v; } Q2Own;\n"
        "static inline __attribute__((always_inline)) void q2_own_drop(Q2Own *o)\n"
        "{\n"
        "  JSValue v_ = o->v;\n"
        "  if (JS_VALUE_GET_TAG(v_) == JS_TAG_OBJECT) {\n"
        "#ifdef QJS_OWN_LEAK\n"
        "    (void)v_;\n"
        "#elif defined(QJS_OWN_PLAIN)\n"
        "    JS_FreeValue(o->ctx, v_);\n"
        "#else\n"
        "    q2_free_obj(o->ctx, v_);\n"
        "#endif\n"
        "  }\n"
        "}\n"
        "static inline __attribute__((always_inline)) void q2_put_slot_own(\n"
        "    JSValue recv, uint32_t off, JSValue v, Q2Own *o, JSValue old)\n"
        "{ Q2YD_R(recv);\n"
        "  Q2Y_DUP(v);\n"
        "  *q2_slotp(recv, off) = v;\n"
        "  o->v = Q2Y_OLD(old);\n"
        "}\n"
        "static inline __attribute__((always_inline)) void q2_put_slotpp_own(\n"
        "    const unsigned char *pp, uint32_t off, JSValue v, Q2Own *o, JSValue old)\n"
        "{ Q2YD_P(pp);\n"
        "  Q2Y_DUP(v);\n"
        "  *q2_slotpp(pp, off) = v;\n"
        "  o->v = Q2Y_OLD(old);\n"
        "}\n"
        "static inline __attribute__((always_inline)) void "
        "q2_put_slot(JSContext *ctx, JSValue recv,\n"
        "                                                   uint32_t off, "
        "JSValue v)\n"
        "{ Q2YD_R(recv);\n"
        "  JSValue *slot = q2_slotp(recv, off);\n"
        "  JSValue old = *slot;\n"
        "  Q2Y_DUP(v);\n"
        "  *slot = v;\n"
        "  if (JS_VALUE_HAS_REF_COUNT(old)) Q2Y_REL(JS_FreeValue(ctx, old));\n"
        "}\n\n"
        /* A store to a global the read cache holds and the cell says is plainly writable. A miss
           (no cell, another generation, a read-only or accessor property) takes the engine's
           own store. */
        "static inline int q2_put_global_hit(JSContext *ctx, uint32_t atom, JSValue v)\n"
        "{\n"
        "  const unsigned char *rt = *(const unsigned char *const *)((const unsigned char *)ctx +\n"
        "                                                            QJS_AOT_OFF_CTX_RT);\n"
        "  uint32_t gen = *(const uint32_t *)(rt + QJS_AOT_OFF_RT_GV_CELL_GEN);\n"
        "  const unsigned char *set = (const unsigned char *)ctx + QJS_AOT_OFF_CTX_GV_CELLS +\n"
        "      (size_t)(atom & (QJS_AOT_GV_CELL_SETS - 1)) * QJS_AOT_GV_CELL_WAYS * QJS_AOT_SIZEOF_GV_CELL;\n"
        "  unsigned w;\n"
        "  for (w = 0; w < QJS_AOT_GV_CELL_WAYS; w++) {\n"
        "    const unsigned char *c = set + (size_t)w * QJS_AOT_SIZEOF_GV_CELL;\n"
        "    if (*(const uint32_t *)(c + QJS_AOT_OFF_GVC_ATOM) == atom &&\n"
        "        *(const uint32_t *)(c + QJS_AOT_OFF_GVC_GEN) == gen) {\n"
        "      JSValue *p, old;\n"
        "      if (!*(const uint32_t *)(c + QJS_AOT_OFF_GVC_WR)) return 0;\n"
        "      p = (JSValue *)(*(const unsigned char *const *)(c + QJS_AOT_OFF_GVC_SLOT) +\n"
        "                      QJS_AOT_OFF_JSPROPERTY_U_VALUE);\n"
        "      old = *p;\n"
        "      q2_dupn(v);\n"
        "      *p = v;\n"
        "      if (JS_VALUE_HAS_REF_COUNT(old)) JS_FreeValue(ctx, old);\n"
        "      return 1;\n"
        "    }\n"
        "  }\n"
        "  return 0;\n"
        "}\n"
);
    /* The same helpers with "counted" as one unsigned compare of the tag. A body in the `_nl`
       form renames the helpers to these around itself (e_emit_spec_body_pass). */
    fprintf(
        f,
        "#define Q2_HRCN(v_) ((unsigned)JS_VALUE_GET_TAG(v_) >= (unsigned)JS_TAG_FIRST)\n"
        "#define Q2_HRC(v_) (Q2_LZ ? JS_VALUE_HAS_REF_COUNT(v_) : Q2_HRCN(v_))\n"
        "static inline __attribute__((always_inline)) void q2_dup_nl(JSValue v)\n"
        "{\n"
        "  if (Q2_HRCN(v))\n"
        "    Q2_INC(v);\n"
        "}\n"
        "static inline __attribute__((always_inline)) void q2_free_nl(JSContext *ctx, JSValue v)\n"
        "{\n"
        "  if (Q2_HRCN(v)) q2_free_obj(ctx, v);\n"
        "}\n"
        "static inline __attribute__((always_inline)) void q2_put_slotpp_nl(JSContext *ctx,\n"
        "    const unsigned char *pp, uint32_t off, JSValue v)\n"
        "{ Q2YD_P(pp);\n"
        "  JSValue *slot = q2_slotpp(pp, off);\n"
        "  JSValue old = *slot;\n"
        "  Q2Y_DUPNL(v);\n"
        "  *slot = v;\n"
        "  if (Q2_HRCN(old)) Q2Y_REL(q2_free_obj(ctx, old));\n"
        "}\n"
        "static inline __attribute__((always_inline)) void q2_put_slotpp_k_nl(JSContext *ctx,\n"
        "    const unsigned char *pp, uint32_t off, JSValue v, JSValue old)\n"
        "{ Q2YD_P(pp);\n"
        "  Q2Y_DUPNL(v);\n"
        "  *q2_slotpp(pp, off) = v;\n"
        "  Q2Y_REL(q2_free_nl(ctx, old));\n"
        "}\n"
        "static inline __attribute__((always_inline)) void q2_put_slotpp_ko_nl(JSContext *ctx,\n"
        "    const unsigned char *pp, uint32_t off, JSValue v, JSValue old)\n"
        "{ Q2YD_P(pp);\n"
        "  Q2Y_DUPNL(v);\n"
        "  *q2_slotpp(pp, off) = v;\n"
        "  Q2Y_REL(q2_free_obj(ctx, old));\n"
        "}\n"
        "static inline __attribute__((always_inline)) void q2_put_slot_own_nl(\n"
        "    JSValue recv, uint32_t off, JSValue v, Q2Own *o, JSValue old)\n"
        "{ Q2YD_R(recv);\n"
        "  Q2Y_DUPNL(v);\n"
        "  *q2_slotp(recv, off) = v;\n"
        "  o->v = Q2Y_OLD(old);\n"
        "}\n"
        "static inline __attribute__((always_inline)) void q2_put_slotpp_own_nl(\n"
        "    const unsigned char *pp, uint32_t off, JSValue v, Q2Own *o, JSValue old)\n"
        "{ Q2YD_P(pp);\n"
        "  Q2Y_DUPNL(v);\n"
        "  *q2_slotpp(pp, off) = v;\n"
        "  o->v = Q2Y_OLD(old);\n"
        "}\n"
        "static inline __attribute__((always_inline)) void q2_put_slot_nl(JSContext *ctx, JSValue recv,\n"
        "                                                   uint32_t off, JSValue v)\n"
        "{ Q2YD_R(recv);\n"
        "  JSValue *slot = q2_slotp(recv, off);\n"
        "  JSValue old = *slot;\n"
        "  Q2Y_DUPNL(v);\n"
        "  *slot = v;\n"
        "  if (Q2_HRCN(old)) Q2Y_REL(q2_free_obj(ctx, old));\n"
        "}\n");
    fprintf(
        f,
        "static inline __attribute__((always_inline)) int q2_elem_put_rc("
        "JSContext *ctx, JSValue arr, JSValue k, JSValue v)\n"
        "{\n"
        "  unsigned char *p;\n"
        "  uint32_t i;\n"
        "  JSValue *slot, old;\n"
        "  if (JS_VALUE_GET_TAG(arr) != JS_TAG_OBJECT) return 0;\n"
        "  if (JS_VALUE_GET_TAG(k) != JS_TAG_INT) return 0;\n"
        "  p = (unsigned char *)JS_VALUE_GET_PTR(arr);\n"
        "  i = (uint32_t)JS_VALUE_GET_INT(k);\n"
        "  if (*(const uint16_t *)(p + QJS_AOT_OFF_JSOBJECT_CLASS_ID) !=\n"
        "      QJS_AOT_CLASS_ARRAY)\n"
        "    return 0;\n"
        "  if (QJS_AOT_ARR_ODD(p)) return 0;\n"
        "  if (i >= *(const uint32_t *)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_COUNT)) "
        "return 0;\n"
        "  slot = &(*(JSValue **)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_VALUES))[i];\n"
        "  if (JS_VALUE_GET_TAG(*slot) == JS_TAG_UNINITIALIZED) return 0;\n"
        "  old = *slot;\n"
        "  q2_dupn(v);\n"
        "  *slot = v;\n"
        "  if (JS_VALUE_HAS_REF_COUNT(old)) JS_FreeValue(ctx, old);\n"
        "  return 1;\n"
        "}\n\n");
    /* The inline own-slot store hit: the interpreter's REG_PIC_OWN_W test on
       an object that is not a prototype, and the store the engine's own fast
       path makes (`*slot = js_dup(v); JS_FreeValue(ctx, old)`). The new value
       is duplicated before the old one is released, so storing a slot's own
       value cannot free it. A prototype receiver (whose store invalidates the
       prototype epoch) or a miss returns 0 and the caller takes the engine's
       store. */
    /* The cached add-transition store, replayed inline: the cell holds the shape the
       object has now and the shape it gets (the holder), the new slot's offset, and
       the epoch stamp that says no prototype interceptor has appeared. Taken only when
       the new shape fits the slots the object has (no growth), the object is extensible
       and not exotic or watched; the old shape is dropped by count and only while another
       reference holds it, so nothing is freed here. */
    /* An add that has just made an object take a contracted layout's shape: the engine's claim check over the filled object (a violated promise breaks the layout,
       as in the engine's own add). QJS_AOT_ADDCLAIMKNOCK drops it, for the negative control only. */
    if (getenv("QJS_AOT_ADDCLAIMKNOCK") != NULL)
      fprintf(f, "#define Q2_ADD_FILLED(ctx_, obj_, nsh_) ((void)0)\n");
    else
      fprintf(f, "#define Q2_ADD_FILLED(ctx_, obj_, nsh_) do { if (__builtin_expect(Q2_ENTERS_LAYOUT((const unsigned char *)(nsh_)), 0)) qjs_aot_object_filled(ctx_, obj_); } while (0)\n");
    fprintf(
        f,
        "static inline __attribute__((always_inline)) int "
        "q2_put_add(JSContext *ctx, const void *pic, void *const *holders,\n"
        "                                  uint32_t ns, JSValue recv, int site, JSValue v)\n"
        "{ Q2YD_R(recv);\n"
        "  unsigned char *obj; const unsigned char *e; unsigned char *osh, *nsh;\n"
        "  uint16_t fl; int npsz; JSValue *slot;\n"
        "  if (pic == NULL || (unsigned)site == QJS_AOT_REG_IC_NONE) return 0;\n"
        "  if (JS_VALUE_GET_TAG(recv) != JS_TAG_OBJECT) return 0;\n"
        "  obj = (unsigned char *)JS_VALUE_GET_PTR(recv);\n"
        "  e = (const unsigned char *)pic + (size_t)(ns + (uint32_t)site) * QJS_AOT_SIZEOF_JSREGPIC;\n"
        "  osh = *(unsigned char *const *)(obj + QJS_AOT_OFF_JSOBJECT_SHAPE);\n"
        "  if (*(unsigned char *const *)e != osh) return 0;\n"
        "  if (*(const uint16_t *)(e + QJS_AOT_OFF_JSREGPIC_KIND) != QJS_AOT_REG_PIC_ADD_W) return 0;\n"
        "  fl = *(const uint16_t *)(obj + QJS_AOT_OFF_JSOBJECT_FLAGS16);\n"
        "  if ((fl & (QJS_AOT_JSOBJECT_EXTENSIBLE_MASK | QJS_AOT_JSOBJECT_NOADD_MASK)) !=\n"
        "      QJS_AOT_JSOBJECT_EXTENSIBLE_MASK) return 0;\n"
        /* an addition to a prototype is the engine's to make: it moves the cache epoch */
        "  if (obj[QJS_AOT_OFF_JSOBJECT_FLAGS_BYTE] & QJS_AOT_JSOBJECT_IS_PROTOTYPE_MASK) return 0;\n"
        "  if (*(const uint16_t *)(e + QJS_AOT_OFF_JSREGPIC_MISSES) !=\n"
        "      (uint16_t)*(const uint32_t *)(*(const unsigned char *const *)((const unsigned char *)ctx +\n"
        "          QJS_AOT_OFF_CTX_RT) + QJS_AOT_OFF_RT_IC_EPOCH)) return 0;\n"
        "  nsh = (unsigned char *)holders[ns + (uint32_t)site];\n"
        "  if (Q2_ENTERS_LAYOUT(osh)) return 0;\n"
        "  npsz = *(const int *)(nsh + QJS_AOT_OFF_JSSHAPE_PROP_SIZE);\n"
        "  if (npsz != *(const int *)(osh + QJS_AOT_OFF_JSSHAPE_PROP_SIZE) &&\n"
        "      (unsigned)npsz > ((fl & QJS_AOT_JSOBJECT_INLINE_CAP_MASK) >> QJS_AOT_JSOBJECT_INLINE_CAP_SHIFT)) return 0;\n"
        /* a nursery cell holds no count on its shape (promotion takes it), so the swap moves none */
        "  Q2Y_REL(if (*(const int *)(osh - QJS_AOT_RC_BACK) <= 1) return 0);\n"
        "  slot = (JSValue *)(*(unsigned char *const *)(obj + QJS_AOT_OFF_JSOBJECT_PROP) +\n"
        "      *(const uint32_t *)(e + QJS_AOT_OFF_JSREGPIC_SLOT_OFF));\n"
        "  Q2Y_REL(++*(int *)(nsh - QJS_AOT_RC_BACK));\n"
        "  *(unsigned char **)(obj + QJS_AOT_OFF_JSOBJECT_SHAPE) = nsh;\n"
        "  Q2Y_REL(--*(int *)(osh - QJS_AOT_RC_BACK));\n"
        "  Q2Y_DUP(v);\n"
        "  *slot = v;\n"
        /* an add that lands in a contracted layout is checked against the slot claims, as the engine's add does */
        "  Q2_ADD_FILLED(ctx, recv, nsh);\n"
        "  return 1;\n"
        "}\n");
    fprintf(
        f,
        "static inline __attribute__((always_inline)) int "
        "q2_put_field_hit(JSContext *ctx, const void *pic,\n"
        "                                   JSValue recv, int site, JSValue "
        "v)\n"
        "{ Q2YD_R(recv);\n"
        "  const unsigned char *obj, *e;\n"
        "  JSValue *slot;\n"
        "\n"
        "  if (pic == NULL || (unsigned)site == QJS_AOT_REG_IC_NONE) {\n"
        "    return 0;\n"
        "  }\n"
        "  if (JS_VALUE_GET_TAG(recv) != JS_TAG_OBJECT) {\n"
        "    return 0;\n"
        "  }\n"
        "  obj = (const unsigned char *)JS_VALUE_GET_PTR(recv);\n"
        "  e = (const unsigned char *)pic + (size_t)site * "
        "QJS_AOT_SIZEOF_JSREGPIC;\n"
        "  if (*(const void *const *)e !=\n"
        "          *(const void *const *)(obj + QJS_AOT_OFF_JSOBJECT_SHAPE)) "
        "{\n"
        "    E_PUTWHY(E_PUT_SHAPE);\n"
        "    return 0;\n"
        "  }\n"
        "  if (*(const uint16_t *)(e + QJS_AOT_OFF_JSREGPIC_KIND) !=\n"
        "          QJS_AOT_REG_PIC_OWN_W) {\n"
        "    E_PUTWHY(E_PUT_KIND);\n"
        "    return 0;\n"
        "  }\n"
        "  if ((obj[QJS_AOT_OFF_JSOBJECT_FLAGS_BYTE] &\n"
        "       QJS_AOT_JSOBJECT_IS_PROTOTYPE_MASK) != 0) {\n"
        "    E_PUTWHY(E_PUT_PROTO);\n"
        "    return 0;\n"
        "  }\n"
        "  if (Q2_OBJ_IN_LAYOUT(obj)) {\n"
        "    E_PUTWHY(E_PUT_LAYOUT);\n"
        "    return 0;\n"
        "  }\n"
        "  slot = (JSValue *)(*(unsigned char *const *)(obj + "
        "QJS_AOT_OFF_JSOBJECT_PROP) +\n"
        "                     *(const uint32_t *)(e + "
        "QJS_AOT_OFF_JSREGPIC_SLOT_OFF));\n"
        "  {\n"
        "    JSValue old = *slot;\n"
        "    Q2Y_DUP(v);\n"
        "    *slot = v;\n"
        "    if (JS_VALUE_HAS_REF_COUNT(old)) {\n"
        "      Q2Y_REL(JS_FreeValue(ctx, old));\n"
        "    }\n"
        "  }\n"
        "  return 1;\n"
        "}\n\n");
    /* The fast bodies' (fast.c) shared pieces: the exit value, the reference duplicate, and the shapes the bodies hold. */
    fprintf(
        f,
        "#define E_PEXIT JS_MKVAL(JS_TAG_UNINITIALIZED, 0)\n"
        "#define Q2_PH_MAX 8192\n"
        "#ifdef QJS_AOT_SPLIT_UNIT\n"
        "extern void *q2_ph[Q2_PH_MAX]; extern int q2_phn;\n"
        "#else\n"
        "static void *q2_ph[Q2_PH_MAX]; static int q2_phn;\n"
        "#endif\n"
        "static JSValue *q2_rs_pending;   /* the block of the caller's frame an `aot_nr_` entry hands the region it enters; the region takes it as its first statement */\n"
        "static inline __attribute__((always_inline)) void q2_dup_p(JSValue v) { q2_dupn(v); }\n"
        "#define Q2_FH_MAX 1024\n"
        "#ifdef QJS_AOT_SPLIT_UNIT\n"
        "extern JSRuntime *q2_cc_rt;\n"
        "extern JSObject **q2_fh_slot[Q2_FH_MAX]; extern int q2_fhn;\n"
        "#else\n"
        "static JSRuntime *q2_cc_rt;\n"
        "static JSObject **q2_fh_slot[Q2_FH_MAX]; static int q2_fhn;\n"
        "#endif\n"
        "/* a counted reference to a function object a region dispatched to (an exit builds the callee's frame from it, possibly after the fact that kept it alive broke); *slot is cleared when it is dropped at teardown */\n"
        "/* a slot holds at most one reference: a second hold (an init tried again) releases the first and keeps the slot's one registration */\n"
        "static JSObject *q2_fh_hold(JSContext *ctx, JSValue fv, JSObject **slot) { if (JS_VALUE_GET_TAG(fv) != JS_TAG_OBJECT) return NULL; if (*slot != NULL) { JSObject *o_ = *slot; *slot = NULL; JS_FreeValue(ctx, JS_MKPTR(JS_TAG_OBJECT, o_)); } else { int i_; for (i_ = 0; i_ < q2_fhn && q2_fh_slot[i_] != slot; i_++) {} if (i_ == q2_fhn) { if (q2_fhn >= Q2_FH_MAX) return NULL; q2_fh_slot[q2_fhn++] = slot; } } if (q2_cc_rt == NULL) q2_cc_rt = JS_GetRuntime(ctx); return (JSObject *)JS_VALUE_GET_PTR(JS_DupValue(ctx, fv)); }\n"
        "static void q2_ph_release(JSRuntime *rt) { while (q2_fhn > 0) { JSObject **s_ = q2_fh_slot[--q2_fhn]; if (*s_ != NULL) JS_FreeValueRT(rt, JS_MKPTR(JS_TAG_OBJECT, *s_)); *s_ = NULL; } while (q2_phn > 0) qjs_aot_shape_release(rt, q2_ph[--q2_phn]); }\n\n");
    /* Whether an argument-window call can be a plain C call into a compiled
       callee: the value is a bytecode function whose bound entry is `entry`,
       which only that function's digest produces. The same test `q2_entry_is`
       makes; named separately because a call also reads the function object's
       pointer out of the value, and doing both here keeps the call site one
       line. */
    fprintf(
        f,
        "static inline __attribute__((always_inline)) int q2_call_entry("
        "JSValue fnv, const void *entry)\n"
        "{\n"
        "  const unsigned char *o, *b;\n"
        "\n"
        "  if (JS_VALUE_GET_TAG(fnv) != JS_TAG_OBJECT) return 0;\n"
        "  o = (const unsigned char *)JS_VALUE_GET_PTR(fnv);\n"
        "  if (*(const uint16_t *)(o + QJS_AOT_OFF_JSOBJECT_CLASS_ID) !=\n"
        "      QJS_AOT_CLASS_BYTECODE_FUNCTION)\n"
        "    return 0;\n"
        "  b = *(const unsigned char *const *)(o + "
        "QJS_AOT_OFF_OBJECT_FUNC_BYTECODE);\n"
        "  return *(const void *const *)(b + QJS_AOT_OFF_FNBC_AOT_FN) == "
        "entry;\n"
        "}\n\n");
    /* The unit entry of a bytecode function's CODE, whether or not this bytecode runs it compiled: a gated function and a second
       bytecode of the same code carry it with no bound entry. NULL for any other value. */
    fprintf(
        f,
        "static inline __attribute__((always_inline)) const void *q2_ident_of(JSValue fnv)\n"
        "{\n"
        "  const unsigned char *o, *b;\n"
        "\n"
        "  if (JS_VALUE_GET_TAG(fnv) != JS_TAG_OBJECT) return NULL;\n"
        "  o = (const unsigned char *)JS_VALUE_GET_PTR(fnv);\n"
        "  if (*(const uint16_t *)(o + QJS_AOT_OFF_JSOBJECT_CLASS_ID) != QJS_AOT_CLASS_BYTECODE_FUNCTION) return NULL;\n"
        "  b = *(const unsigned char *const *)(o + QJS_AOT_OFF_OBJECT_FUNC_BYTECODE);\n"
        "  return *(const void *const *)(b + QJS_AOT_OFF_FNBC_AOT_IDENT);\n"
        "}\n\n");
    /* The bound entry of a bytecode function, or NULL for any other value. */
    fprintf(
        f,
        "static inline __attribute__((always_inline)) const void *q2_entry_of("
        "JSValue fnv)\n"
        "{\n"
        "  const unsigned char *o, *b;\n"
        "\n"
        "  if (JS_VALUE_GET_TAG(fnv) != JS_TAG_OBJECT) return NULL;\n"
        "  o = (const unsigned char *)JS_VALUE_GET_PTR(fnv);\n"
        "  if (*(const uint16_t *)(o + QJS_AOT_OFF_JSOBJECT_CLASS_ID) !=\n"
        "      QJS_AOT_CLASS_BYTECODE_FUNCTION)\n"
        "    return NULL;\n"
        "  b = *(const unsigned char *const *)(o + "
        "QJS_AOT_OFF_OBJECT_FUNC_BYTECODE);\n"
        "  return *(const void *const *)(b + QJS_AOT_OFF_FNBC_AOT_FN);\n"
        "}\n\n");
    /* The same test with the function object a call site first accepted held in
       `q2_cc[ix]`: the unit holds a counted reference on it, so its address cannot be
       reused by another object while the site compares against it, and a match is one
       compare where the full test is a tag test, a class test and two dependent loads.
       A function object's bytecode never changes. Any other object runs the full test. */
    fprintf(
        f,
        "extern void *q2_cc[];\n"
        "void q2_cc_fill(JSContext *ctx, uint32_t ix, JSValue fnv);\n"
        /* constant globals (see e_kslot_of in ops.c) */
        "extern JSValue q2_kv[];\n"
        "extern uint64_t q2_kw[];\n"
        "JSValue q2_kentry(JSContext *ctx_in, int fi, JSValue (*fast)(JSContext *, JSRegRun *, JSObject *, JSValue, JSValue *, int),\n"
        "                  JSRegRun *caller_f, JSObject *co, JSValue this_obj, JSValue *argv, int argc, int needpic);\n"
        "#define q2_call_entry_k(ctx_, v_, entry_, ix_) 1\n"
        /* Whether `fnv` is a bytecode function whose register program is exactly `code[0..n)`: a function that has the same body as a compiled one but
           no bound entry of its own (the engine binds an entry to one bytecode; a twin such as an identical empty method stays interpreted). Only
           for bodies that read nothing from their own cache or constants, which the generator checks. */
        "static inline __attribute__((always_inline)) int q2_code_is(JSValue fnv, const unsigned char *code, int n)\n"
        "{\n"
        "  const unsigned char *o, *b;\n"
        "  if (JS_VALUE_GET_TAG(fnv) != JS_TAG_OBJECT) return 0;\n"
        "  o = (const unsigned char *)JS_VALUE_GET_PTR(fnv);\n"
        "  if (*(const uint16_t *)(o + QJS_AOT_OFF_JSOBJECT_CLASS_ID) != QJS_AOT_CLASS_BYTECODE_FUNCTION) return 0;\n"
        "  b = *(const unsigned char *const *)(o + QJS_AOT_OFF_OBJECT_FUNC_BYTECODE);\n"
        "  return *(const int *)(b + QJS_AOT_OFF_FNBC_REGVM_LEN) == n &&\n"
        "         __builtin_memcmp(*(const unsigned char *const *)(b + QJS_AOT_OFF_FNBC_REGVM_CODE), code, (size_t)n) == 0;\n"
        "}\n"
        "static inline __attribute__((always_inline)) int q2_call_entry_u(\n"
        "JSContext *ctx, JSValue fnv, const void *entry, uint32_t ix)\n"
        "{\n"
        "  (void)ctx; (void)ix;\n"
        "  return q2_call_entry(fnv, entry);\n"
        "}\n"
        "static inline __attribute__((always_inline)) int q2_call_entry_c(\n"
        "JSContext *ctx, JSValue fnv, const void *entry, uint32_t ix)\n"
        "{\n"
        "  if (JS_VALUE_GET_TAG(fnv) != JS_TAG_OBJECT) return 0;\n"
        "  if (__builtin_expect(JS_VALUE_GET_PTR(fnv) == q2_cc[ix], 1)) return 1;\n"
        "  if (!q2_call_entry(fnv, entry)) return 0;\n"
        "  if (q2_cc[ix] == NULL) q2_cc_fill(ctx, ix, fnv);\n"
        "  return 1;\n"
        "}\n\n");
    /* Prototype-method tables (see e_pm_collect in ops.c). A table holds, for one read site, the
       receiver shapes its cache cells name and the method value each of them reaches through the
       prototype, resolved once per cache epoch at the body's entry.
       INVARIANT: a key is a shape taken from a NON-EMPTY cache cell at fill time, never from a live
       receiver. Cells hold counted references on their shapes, and clearing or refilling a cell
       moves aot_pic_epoch, so a key stays a live shape while the epoch is unchanged. A value is
       valid while the epoch is unchanged too: every write that can change what a prototype holds
       moves it. A way that is not a plain one-level prototype data read whose holder still has the
       shape the cell was filled with is left out (its key stays NULL, which no object's shape
       equals), so a receiver of that shape takes the ordinary read. */
    /* A learned method table: for a method read no cache cell serves (the function is more than one prototype up), the shapes receivers
       of the site have had and the method each reaches, found by qjs_aot_shape_method. An entry is a function of the shape and of what the
       prototypes hold, so it is kept while neither aot_pic_epoch (every write that can change a prototype) nor shape_free_epoch (a freed shape's
       address can be reused) has moved; both are read on every lookup, and a lookup that finds either moved starts the table again. */
    fprintf(
        f,
        "typedef struct { uint32_t ep, sfe, n; const void *k[4]; JSValue v[4]; const void *e[4]; } Q2Lmt;\n"
        "static __attribute__((noinline)) int q2_lmt_fill(Q2Lmt *t, const void *sh, uint32_t atom, JSValue *out, const void **ent)\n"
        "{\n"
        "  JSValue v;\n"
        "  uint32_t s;\n"
        "  if (!qjs_aot_shape_method(sh, atom, &v)) {\n"
        "#ifdef Q2_MXDEBUG\n"
        "    static int n_; if (n_++ < 5) fprintf(stderr, \"lmt fill failed: atom=%%u shape=%%p\\n\", atom, sh);\n"
        "#endif\n"
        "    return 0;\n"
        "  }\n"
        "  s = t->n++ & 3;\n"
        "  t->k[s] = sh; t->v[s] = v; t->e[s] = q2_entry_of(v);\n"
        "  *out = v; *ent = t->e[s];\n"
        "  return 1;\n"
        "}\n"
        "static inline __attribute__((always_inline)) int q2_lmt_hit(JSContext *ctx, Q2Lmt *t, JSValue recv, uint32_t atom, JSValue *out, const void **ent)\n"
        "{\n"
        "  const unsigned char *rt = *(const unsigned char *const *)((const unsigned char *)ctx + QJS_AOT_OFF_CTX_RT);\n"
        "  uint32_t ep = *(const uint32_t *)(rt + QJS_AOT_OFF_RT_PIC_EPOCH), sfe = *(const uint32_t *)(rt + QJS_AOT_OFF_RT_SHAPE_FREE_EPOCH + 4), k; /* shape_obs_epoch */\n"
        "  const void *sh;\n"
        "  if (JS_VALUE_GET_TAG(recv) != JS_TAG_OBJECT) return 0;\n"
        "  sh = *(const void *const *)((const unsigned char *)JS_VALUE_GET_PTR(recv) + QJS_AOT_OFF_JSOBJECT_SHAPE);\n"
        "  if (__builtin_expect(t->ep != ep || t->sfe != sfe, 0)) {\n"
        "    t->ep = ep; t->sfe = sfe; t->n = 0;\n"
        "    for (k = 0; k < 4; k++) t->k[k] = NULL;\n"
        "  }\n"
        "  for (k = 0; k < 4; k++)\n"
        "    if (sh == t->k[k]) { *out = t->v[k]; *ent = t->e[k]; return 1; }\n"
        "  return q2_lmt_fill(t, sh, atom, out, ent);\n"
        "}\n");
    /* The same for a field a copy reads off receivers of several shapes: shape -> slot offset of an own data property (a function of the shape
       alone, so only a shape freed or edited retires an entry: shape_obs_epoch). */
    fprintf(
        f,
        "typedef struct { uint32_t oe, n; const void *k[4]; uint32_t o[4]; } Q2Slt;\n"
        "static __attribute__((noinline)) int q2_slt_fill(JSContext *ctx, Q2Slt *t, const void *sh, uint32_t atom, uint32_t *off)\n"
        "{\n"
        "  uint32_t s;\n"
        "  if (!qjs_aot_shape_slot(sh, atom, off)) {\n"
        "#ifdef Q2_MXDEBUG\n"
        "    { static int n_; if (n_++ < 6) { const char *an_ = JS_AtomToCString(ctx, (JSAtom)atom); fprintf(stderr, \"slt miss atom=%%s shape=%%p\\n\", an_ ? an_ : \"?\", sh); } }\n"
        "#endif\n"
        "    (void)ctx;\n"
        "    return 0;\n"
        "  }\n"
        "  s = t->n++ & 3;\n"
        "  t->k[s] = sh; t->o[s] = *off;\n"
        "  return 1;\n"
        "}\n"
        "static inline __attribute__((always_inline)) int q2_slt_off(JSContext *ctx, Q2Slt *t, JSValue recv, uint32_t atom, uint32_t *off)\n"
        "{\n"
        "  const unsigned char *rt = *(const unsigned char *const *)((const unsigned char *)ctx + QJS_AOT_OFF_CTX_RT);\n"
        "  uint32_t oe = *(const uint32_t *)(rt + QJS_AOT_OFF_RT_SHAPE_FREE_EPOCH + 4), k;\n"
        "  const void *sh;\n"
        "  if (JS_VALUE_GET_TAG(recv) != JS_TAG_OBJECT) return 0;\n"
        "  sh = *(const void *const *)((const unsigned char *)JS_VALUE_GET_PTR(recv) + QJS_AOT_OFF_JSOBJECT_SHAPE);\n"
        "  if (__builtin_expect(t->oe != oe, 0)) {\n"
        "    t->oe = oe; t->n = 0;\n"
        "    for (k = 0; k < 4; k++) t->k[k] = NULL;\n"
        "  }\n"
        "  for (k = 0; k < 4; k++)\n"
        "    if (sh == t->k[k]) { *off = t->o[k]; return 1; }\n"
        "  return q2_slt_fill(ctx, t, sh, atom, off);\n"
        "}\n");
    fprintf(
        f,
        "static inline __attribute__((always_inline)) void q2_pm_fill(const void *pic, "
        "void *const *holders, uint32_t nsites, int site, const void **ks, JSValue *vs, const void **es)\n"
        "{\n"
        "  unsigned k;\n"
        "  for (k = 0; k < QJS_AOT_REG_PIC_WAYS; k++) {\n"
        "    const unsigned char *e = (const unsigned char *)pic +\n"
        "        ((size_t)site + (size_t)k * nsites) * QJS_AOT_SIZEOF_JSREGPIC;\n"
        "    const unsigned char *sh, *holder, *prop;\n"
        "    JSValue v;\n"
        "    ks[k] = NULL; es[k] = NULL; vs[k] = JS_UNDEFINED;\n"
        "#if QJS_DRC\n"
        "    sh = *(const unsigned char *const *)e;\n"
        "    if (sh == NULL || *(const uint16_t *)(e + QJS_AOT_OFF_JSREGPIC_KIND) != QJS_AOT_REG_PIC_PROTO)\n"
        "      continue;\n"
        "    holder = *(const unsigned char *const *)(sh + QJS_AOT_OFF_JSSHAPE_PROTO);\n"
        "    if (holder == NULL || *(const void *const *)(holder + QJS_AOT_OFF_JSOBJECT_SHAPE) !=\n"
        "        holders[(size_t)site + (size_t)k * nsites])\n"
        "      continue;\n"
        "    prop = *(const unsigned char *const *)(holder + QJS_AOT_OFF_JSOBJECT_PROP);\n"
        "    v = *(const JSValue *)(prop + *(const uint32_t *)(e + QJS_AOT_OFF_JSREGPIC_SLOT_OFF));\n"
        "    ks[k] = sh; vs[k] = v; es[k] = q2_entry_of(v);\n"
        "#else\n"
        "    (void)e; (void)sh; (void)holder; (void)prop; (void)v; (void)holders;\n"
        "#endif\n"
        "  }\n"
        "}\n"
        /* Debug counters, compiled in only under -DQ2_PMSTATS=1 (aotcase.sh builds a second binary with it):
           reads that reached a table and reads it served, per table, printed at exit when QJS_AOT_PMSTATS is set. */
        "#if defined(Q2_PMSTATS) && Q2_PMSTATS\n"
        "static unsigned long q2_pm_calls[16], q2_pm_hits[16];\n"
        "__attribute__((destructor)) static void q2_pm_report(void)\n"
        "{\n"
        "  int i;\n"
        "  if (getenv(\"QJS_AOT_PMSTATS\") == NULL) return;\n"
        "  for (i = 0; i < 16; i++)\n"
        "    if (q2_pm_calls[i] != 0) fprintf(stderr, \"pmstats table=%%d calls=%%lu hits=%%lu\\n\", i, q2_pm_calls[i], q2_pm_hits[i]);\n"
        "}\n"
        "#define Q2_PM_COUNT(c_, j_) (q2_pm_##c_[(j_) & 15]++)\n"
        "static unsigned long q2_wc_calls[256], q2_wc_hits[256];\n"
        "__attribute__((destructor)) static void q2_wc_report(void)\n"
        "{\n"
        "  int i;\n"
        "  if (getenv(\"QJS_AOT_PMSTATS\") == NULL) return;\n"
        "  for (i = 0; i < 256; i++)\n"
        "    if (q2_wc_calls[i] != 0) fprintf(stderr, \"wcstats site=%%d calls=%%lu hits=%%lu\\n\", i, q2_wc_calls[i], q2_wc_hits[i]);\n"
        "}\n"
        "#define Q2_WC_COUNT(c_, j_) (q2_wc_##c_[(j_) & 255]++)\n"
        "#else\n"
        "#define Q2_PM_COUNT(c_, j_) ((void)(j_))\n"
        "#define Q2_WC_COUNT(c_, j_) ((void)(j_))\n"
        "#endif\n"
        "static inline __attribute__((always_inline)) int q2_pm_hit(JSValue recv, const void *const *ks, "
        "const JSValue *vs, const void *const *es, JSValue *out, const void **ent, int j)\n"
        "{\n"
        "  const void *sh;\n"
        "  unsigned k;\n"
        "  Q2_PM_COUNT(calls, j);\n"
        "  if (JS_VALUE_GET_TAG(recv) != JS_TAG_OBJECT) return 0;\n"
        "  sh = *(const void *const *)((const unsigned char *)JS_VALUE_GET_PTR(recv) + QJS_AOT_OFF_JSOBJECT_SHAPE);\n"
        "  for (k = 0; k < QJS_AOT_REG_PIC_WAYS; k++)\n"
        "    if (sh == ks[k]) { Q2_PM_COUNT(hits, j); *out = vs[k]; *ent = es[k]; return 1; }\n"
        "  return 0;\n"
        "}\n\n");
    /* Whether the callee is the function whose bound entry is `entry`. Only
       that function's digest produces that entry, so this is an exact identity
       test, cheap enough for a dispatch chain. */
    fprintf(
        f,
        "static inline __attribute__((always_inline)) int q2_entry_is(JSValue "
        "fnv, const void *entry)\n"
        "{\n"
        "  const unsigned char *o, *b;\n"
        "  if (JS_VALUE_GET_TAG(fnv) != JS_TAG_OBJECT) return 0;\n"
        "  o = (const unsigned char *)JS_VALUE_GET_PTR(fnv);\n"
        "  if (*(const uint16_t *)(o + QJS_AOT_OFF_JSOBJECT_CLASS_ID) !=\n"
        "      QJS_AOT_CLASS_BYTECODE_FUNCTION)\n"
        "    return 0;\n"
        "  b = *(const unsigned char *const *)(o + "
        "QJS_AOT_OFF_OBJECT_FUNC_BYTECODE);\n"
        "  return *(const void *const *)(b + QJS_AOT_OFF_FNBC_AOT_FN) == "
        "entry;\n"
        "}\n\n");
    /* ---- the contracted-layout check ---------------------------------------
       A value entering compiled code is checked ONCE, here, never at each field
       access. `q2_layout_of` answers which contracted layout a value's shape is,
       or 0 when it is not one: one load of the shape's id, one test of that
       layout's bit in the broken word. A body that needs a layout calls this at
       entry and hands over to its generic body when the answer is wrong, so a
       violation costs the check and can never give a wrong result. */
    fprintf(f, "#define Q2_LAYOUT_COUNT %du\n", g_nlayouts);
    if (g_nlayouts > 0)
      fprintf(
          f,
          "/* The contracted layout a value's shape ends, or 0 for anything else:\n"
          "   not an object, not a contracted shape, or a layout already broken. */\n"
          "static inline __attribute__((always_inline)) uint32_t q2_layout_of(\n"
          "    JSRuntime *rt, JSValue v)\n"
          "{\n"
          "  const unsigned char *o, *sh;\n"
          "  uint32_t id;\n"
          "  if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT) return 0u;\n"
          "  o = (const unsigned char *)JS_VALUE_GET_PTR(v);\n"
          "  sh = *(const unsigned char *const *)(o + QJS_AOT_OFF_JSOBJECT_SHAPE);\n"
          "  id = (uint32_t)*(const uint16_t *)(sh + QJS_AOT_OFF_SHAPE_LAYOUT_ID);\n"
          "  if (id == 0u || id > (uint32_t)Q2_LAYOUT_COUNT) return 0u;\n"
          "  if ((*(const uint64_t *)((const unsigned char *)rt +\n"
          "                           QJS_AOT_OFF_RT_LAYOUT_BROKEN) >> (id - 1u)) & 1u)\n"
          "    return 0u;\n"
          "  return id;\n"
          "}\n"
          "/* The shapes the unit's built layouts end, defined where the layouts are\n"
          "   owned. A value whose shape IS one of them is in that layout: the unit\n"
          "   holds a reference, so the shape is never changed in place, and leaving\n"
          "   the layout changes the value\'s shape pointer. */\n"
          "extern void *q2_layout_shape[];\n"
          "/* A field of a value in layout `k` (1-based) at constant slot `slot`: one\n"
          "   compare against the layout\'s shape and one load. Anything else is a miss. */\n"
          "static inline __attribute__((always_inline)) int q2_cslot_hit_lz(\n"
          "    JSValue recv, uint32_t k, uint32_t slot, JSValue *out, int lz)\n"
          "{\n"
          "  const unsigned char *obj, *prop;\n"
          "  JSValue v;\n"
          "  if (JS_VALUE_GET_TAG(recv) != JS_TAG_OBJECT) return 0;\n"
          "  obj = (const unsigned char *)JS_VALUE_GET_PTR(recv);\n"
          "  if (*(void *const *)(obj + QJS_AOT_OFF_JSOBJECT_SHAPE) !=\n"
          "      q2_layout_shape[k - 1u])\n"
          "    return 0;\n"
          "  prop = *(const unsigned char *const *)(obj + QJS_AOT_OFF_JSOBJECT_PROP);\n"
          "  v = *(const JSValue *)(prop + (size_t)slot * QJS_AOT_SIZEOF_JSPROPERTY);\n"
          "#if !QJS_DRC\n"
          "  if (JS_VALUE_GET_TAG(v) < 0) return 0;\n"
          "#endif\n"
          "  *out = v;\n"
          "  return 1;\n"
          "}\n\n");
    if (qjs_aot_layout_claims_any())
      fprintf(
          f,
          "/* What each built layout promises about its slots (0 nothing, 1..63 an object of\n"
          "   that layout or null or undefined, 0x80 not reference counted). The engine\n"
          "   enforces them at its stores; a store made here checks its own. */\n"
          "#define Q2_CLAIMS 1\n"
          "extern const uint8_t q2_layout_claim[][32];\n"
          "static inline __attribute__((always_inline)) int q2_claim_val_ok(unsigned c, JSValue v)\n"
          "{\n"
          "  int t;\n"
          "  if (c == QJS_AOT_CLAIM_INT) return JS_VALUE_GET_TAG(v) == JS_TAG_INT;\n"
          "  if (c & QJS_AOT_CLAIM_NOREF) return !JS_VALUE_HAS_REF_COUNT(v);\n"
          "  t = JS_VALUE_GET_TAG(v);\n"
          "  if (!(c & 0x40u) && (t == JS_TAG_NULL || t == JS_TAG_UNDEFINED)) return 1;\n"
          "  return t == JS_TAG_OBJECT &&\n"
          "         *(void *const *)((const unsigned char *)JS_VALUE_GET_PTR(v) +\n"
          "                          QJS_AOT_OFF_JSOBJECT_SHAPE) == q2_layout_shape[(c & 0x3fu) - 1u];\n"
          "}\n"
          "/* Whether storing `v` at byte offset `off` of the property array of `recv` (an object)\n"
          "   would break a promise of the layout its shape ends. */\n"
          "static inline __attribute__((always_inline)) int q2_claim_bad(JSValue recv, uint32_t off,\n"
          "                                                             JSValue v)\n"
          "{\n"
          "  const unsigned char *sh = *(const unsigned char *const *)((const unsigned char *)\n"
          "      JS_VALUE_GET_PTR(recv) + QJS_AOT_OFF_JSOBJECT_SHAPE);\n"
          "  uint32_t id = (uint32_t)*(const uint16_t *)(sh + QJS_AOT_OFF_SHAPE_LAYOUT_ID), slot, c;\n"
          "  if (id == 0u || id > (uint32_t)Q2_LAYOUT_COUNT) return 0;\n"
          "  slot = off / QJS_AOT_SIZEOF_JSPROPERTY;\n"
          "  if (slot >= 32u) return 0;\n"
          "  c = q2_layout_claim[id - 1u][slot];\n"
          "  return c != 0u && !q2_claim_val_ok(c, v);\n"
          "}\n\n");
    /* The inline unary fast path for neg and inc: an int whose result stays an
       int, or a float64. Anything else (-0, an overflow, a coercion) returns 0
       and the caller takes the engine's unary. */
    fprintf(
        f,
        "static inline __attribute__((always_inline)) int q2_unary_fast(int "
        "sub, JSValue a, JSValue *out)\n"
        "{\n"
        "  if (JS_VALUE_GET_TAG(a) == JS_TAG_INT) {\n"
        "    int32_t i = JS_VALUE_GET_INT(a);\n"
        /* The edges are the interpreter's: -0 is a double, and the magnitude of
           INT32_MIN (and one past INT32_MAX / below INT32_MIN) is not an int. */
        "    if (sub == QJS_AOT_REG_UN_NEG) {\n"
        "      if (i == 0) { *out = __JS_NewFloat64(-0.0); return 1; }\n"
        "      if (i == INT32_MIN) { *out = __JS_NewFloat64(2147483648.0); "
        "return 1; }\n"
        "      *out = JS_MKVAL(JS_TAG_INT, -i);\n"
        "      return 1;\n"
        "    }\n"
        "    if (sub == QJS_AOT_REG_UN_INC) {\n"
        "      if (i == INT32_MAX) { *out = __JS_NewFloat64(2147483648.0); "
        "return 1; }\n"
        "      *out = JS_MKVAL(JS_TAG_INT, i + 1);\n"
        "      return 1;\n"
        "    }\n"
        /* QJS_AOT_REG_UN_DEC is INC + 1: the engine's enum is NEG, PLUS, NOT,
           LNOT, TYPEOF, ... INC, DEC, and the layout header carries NEG and INC
           because they are the two the old templates needed. */
        "    if (sub == QJS_AOT_REG_UN_INC + 1) {\n"
        "      if (i == INT32_MIN) { *out = __JS_NewFloat64(-2147483649.0); "
        "return 1; }\n"
        "      *out = JS_MKVAL(JS_TAG_INT, i - 1);\n"
        "      return 1;\n"
        "    }\n"
        "    return 0;\n"
        "  }\n"
        "  if (JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(a))) {\n"
        "    double d = JS_VALUE_GET_FLOAT64(a);\n"
        "    if (sub == QJS_AOT_REG_UN_NEG) { *out = __JS_NewFloat64(-d); "
        "return 1; }\n"
        "    if (sub == QJS_AOT_REG_UN_INC) { *out = __JS_NewFloat64(d + 1); "
        "return 1; }\n"
        "    if (sub == QJS_AOT_REG_UN_INC + 1) { *out = __JS_NewFloat64(d - "
        "1); return 1; }\n"
        "  }\n"
        "  return 0;\n"
        "}\n\n");
    /* The inline tagged-arithmetic fast path: both operands int, or both
       float64, for the sub-operations the profile shows. Anything else (a
       coercion, a bitwise/shift or a loose compare) returns 0 and the caller
       takes the engine's regvm_num_fast. `sub` is a constant at every call, so
       the switches fold. */
    fprintf(
        f,
        "static inline __attribute__((always_inline)) int q2_num_fast(int sub, "
        "JSValue a, JSValue b, JSValue *out)\n"
        "{\n"
        "  if (JS_VALUE_IS_BOTH_FLOAT(a, b)) {\n"
        "    double d1 = JS_VALUE_GET_FLOAT64(a), d2 = "
        "JS_VALUE_GET_FLOAT64(b);\n"
        "    switch (sub) {\n"
        "    case QJS_AOT_REG_BIN_ADD: *out = __JS_NewFloat64(d1 + d2); return "
        "1;\n"
        "    case QJS_AOT_REG_BIN_SUB: *out = __JS_NewFloat64(d1 - d2); return "
        "1;\n"
        "    case QJS_AOT_REG_BIN_MUL: *out = __JS_NewFloat64(d1 * d2); return "
        "1;\n"
        "    case QJS_AOT_REG_BIN_DIV: *out = __JS_NewFloat64(d1 / d2); return "
        "1;\n"
        "    case QJS_AOT_REG_BIN_LT: *out = JS_MKVAL(JS_TAG_BOOL, d1 < d2); "
        "return 1;\n"
        "    case QJS_AOT_REG_BIN_LTE: *out = JS_MKVAL(JS_TAG_BOOL, d1 <= d2); "
        "return 1;\n"
        "    case QJS_AOT_REG_BIN_GT: *out = JS_MKVAL(JS_TAG_BOOL, d1 > d2); "
        "return 1;\n"
        "    case QJS_AOT_REG_BIN_GTE: *out = JS_MKVAL(JS_TAG_BOOL, d1 >= d2); "
        "return 1;\n"
        "    case QJS_AOT_REG_BIN_EQ: case QJS_AOT_REG_BIN_STRICT_EQ: *out = JS_MKVAL(JS_TAG_BOOL, d1 "
        "== d2); return 1;\n"
        "    case QJS_AOT_REG_BIN_NEQ: case QJS_AOT_REG_BIN_STRICT_NEQ: *out = JS_MKVAL(JS_TAG_BOOL, d1 "
        "!= d2); return 1;\n"
        "    default: return 0;\n"
        "    }\n"
        "  }\n"
        "  if ((JS_VALUE_GET_TAG(a) == JS_TAG_FLOAT64 && JS_VALUE_GET_TAG(b) == JS_TAG_INT) ||\n"
        "      (JS_VALUE_GET_TAG(a) == JS_TAG_INT && JS_VALUE_GET_TAG(b) == JS_TAG_FLOAT64)) {\n"
        "    double d1 = JS_VALUE_GET_TAG(a) == JS_TAG_INT ? (double)JS_VALUE_GET_INT(a) : JS_VALUE_GET_FLOAT64(a);\n"
        "    double d2 = JS_VALUE_GET_TAG(b) == JS_TAG_INT ? (double)JS_VALUE_GET_INT(b) : JS_VALUE_GET_FLOAT64(b);\n"
        "    switch (sub) {\n"
        "    case QJS_AOT_REG_BIN_ADD: *out = __JS_NewFloat64(d1 + d2); return 1;\n"
        "    case QJS_AOT_REG_BIN_SUB: *out = __JS_NewFloat64(d1 - d2); return 1;\n"
        "    case QJS_AOT_REG_BIN_MUL: *out = __JS_NewFloat64(d1 * d2); return 1;\n"
        "    case QJS_AOT_REG_BIN_DIV: *out = __JS_NewFloat64(d1 / d2); return 1;\n"
        "    case QJS_AOT_REG_BIN_LT: *out = JS_MKVAL(JS_TAG_BOOL, d1 < d2); return 1;\n"
        "    case QJS_AOT_REG_BIN_LTE: *out = JS_MKVAL(JS_TAG_BOOL, d1 <= d2); return 1;\n"
        "    case QJS_AOT_REG_BIN_GT: *out = JS_MKVAL(JS_TAG_BOOL, d1 > d2); return 1;\n"
        "    case QJS_AOT_REG_BIN_GTE: *out = JS_MKVAL(JS_TAG_BOOL, d1 >= d2); return 1;\n"
        "    case QJS_AOT_REG_BIN_EQ: case QJS_AOT_REG_BIN_STRICT_EQ: *out = JS_MKVAL(JS_TAG_BOOL, d1 == d2); return 1;\n"
        "    case QJS_AOT_REG_BIN_NEQ: case QJS_AOT_REG_BIN_STRICT_NEQ: *out = JS_MKVAL(JS_TAG_BOOL, d1 != d2); return 1;\n"
        "    default: return 0;\n"
        "    }\n"
        "  }\n"
        "  if (JS_VALUE_IS_BOTH_INT(a, b)) {\n"
        "    int32_t v1 = JS_VALUE_GET_INT(a), v2 = JS_VALUE_GET_INT(b);\n"
        "    int64_t r;\n"
        "    switch (sub) {\n"
        "    case QJS_AOT_REG_BIN_ADD: r = (int64_t)v1 + v2; break;\n"
        "    case QJS_AOT_REG_BIN_SUB: r = (int64_t)v1 - v2; break;\n"
        "    case QJS_AOT_REG_BIN_MUL:\n"
        "      r = (int64_t)v1 * v2;\n"
        "      if (__builtin_expect((int32_t)r != r, 0)) { *out = "
        "__JS_NewFloat64((double)r); return 1; }\n"
        "      if (__builtin_expect(r == 0 && (v1 | v2) < 0, 0)) { *out = "
        "__JS_NewFloat64(-0.0); return 1; }\n"
        "      *out = JS_MKVAL(JS_TAG_INT, (int32_t)r); return 1;\n"
        "    case QJS_AOT_REG_BIN_DIV: *out = E_TA_F64((double)v1 / (double)v2); return 1;\n"
        "    case QJS_AOT_REG_BIN_MOD:\n"
        "      if (__builtin_expect(v1 >= 0 && v2 > 0, 1)) { *out = JS_MKVAL(JS_TAG_INT, v1 %% v2); return 1; }\n"
        "      return 0;\n"
        "    case QJS_AOT_REG_BIN_LT: *out = JS_MKVAL(JS_TAG_BOOL, v1 < v2); "
        "return 1;\n"
        "    case QJS_AOT_REG_BIN_LTE: *out = JS_MKVAL(JS_TAG_BOOL, v1 <= v2); "
        "return 1;\n"
        "    case QJS_AOT_REG_BIN_GT: *out = JS_MKVAL(JS_TAG_BOOL, v1 > v2); "
        "return 1;\n"
        "    case QJS_AOT_REG_BIN_GTE: *out = JS_MKVAL(JS_TAG_BOOL, v1 >= v2); "
        "return 1;\n"
        "    case QJS_AOT_REG_BIN_EQ: case QJS_AOT_REG_BIN_STRICT_EQ: *out = JS_MKVAL(JS_TAG_BOOL, v1 "
        "== v2); return 1;\n"
        "    case QJS_AOT_REG_BIN_NEQ: case QJS_AOT_REG_BIN_STRICT_NEQ: *out = JS_MKVAL(JS_TAG_BOOL, v1 "
        "!= v2); return 1;\n"
        /* The bitwise and shift sub-operations, handled inline. Without these
           cases they fall through to `qjs_aot_num_fast`, which is an
           out-of-line call: the unit is a separate translation unit, so the
           engine side's `always_inline` does not reach it. */
        "    case QJS_AOT_REG_BIN_SHL: /* SHL */\n"
        "      *out = JS_MKVAL(JS_TAG_INT, (int32_t)((uint32_t)v1 << (v2 & "
        "31))); return 1;\n"
        "    case QJS_AOT_REG_BIN_SAR: /* SAR */\n"
        "      *out = JS_MKVAL(JS_TAG_INT, v1 >> (v2 & 31)); return 1;\n"
        "    case QJS_AOT_REG_BIN_SHR: /* SHR: above INT32_MAX it is a "
        "double */\n"
        "      { uint32_t u = (uint32_t)v1 >> (v2 & 31);\n"
        "        *out = u <= (uint32_t)INT32_MAX\n"
        "                   ? JS_MKVAL(JS_TAG_INT, (int32_t)u)\n"
        "                   : __JS_NewFloat64((double)u);\n"
        "        return 1; }\n"
        "    case QJS_AOT_REG_BIN_AND: /* AND */\n"
        "      *out = JS_MKVAL(JS_TAG_INT, v1 & v2); return 1;\n"
        "    case QJS_AOT_REG_BIN_XOR: /* XOR */\n"
        "      *out = JS_MKVAL(JS_TAG_INT, v1 ^ v2); return 1;\n"
        "    case QJS_AOT_REG_BIN_OR: /* OR */\n"
        "      *out = JS_MKVAL(JS_TAG_INT, v1 | v2); return 1;\n"
        "    default: return 0;\n"
        "    }\n"
        "    if (__builtin_expect(r < INT32_MIN || r > INT32_MAX, 0)) {\n"
        "      *out = __JS_NewFloat64((double)r);\n"
        "    } else {\n"
        "      *out = JS_MKVAL(JS_TAG_INT, (int32_t)r);\n"
        "    }\n"
        "    return 1;\n"
        "  }\n"
        "  return 0;\n"
        "}\n\n"
        "static inline __attribute__((always_inline)) int q2_arith(int sub, "
        "JSValue a, JSValue b, JSValue *out)\n"
        "{\n"
        "  return q2_num_fast(sub, a, b, out) || qjs_aot_num_fast(sub, a, b, "
        "out);\n"
        "}\n\n"
        /* ToInt32 for a double: JS's exact conversion, and total (no exit).
           Strictly inside (-2147483649.0, 2147483648.0) C's truncation toward
           zero IS ToInt32 -- fractions included, and -0.x becomes 0. NaN fails
           both compares and falls to the modulo, which yields 0. Outside the
           range the value is reduced modulo 2^32 with the sign applied, which is
           what the spec says and what the interpreter computes. */
        "static inline __attribute__((always_inline)) int32_t q2_toi32(double d)\n"
        "{\n"
        "  if (d > -2147483649.0 && d < 2147483648.0) return (int32_t)d;\n"
        "  if (d > -9.0e18 && d < 9.0e18) return (int32_t)(uint32_t)(int64_t)d;\n"
        "  {\n"
        "    double m;\n"
        "    if (isnan(d) || isinf(d)) return 0;\n"
        "    /* Reduce the MAGNITUDE, then apply the sign: ToInt32 is the\n"
        "       mathematical modulo, so -2147483649 becomes +2147483647.\n"
        "       fmod alone loses that sign. */\n"
        "    m = fmod(d < 0 ? -d : d, 4294967296.0);\n"
        "    if (m < 0) m += 4294967296.0;\n"
        "    if (d < 0) m = 4294967296.0 - m;\n"
        "    if (m >= 4294967296.0) m -= 4294967296.0;\n"
        "    return (int32_t)(uint32_t)m;\n"
        "  }\n"
        "}\n"
        "static inline __attribute__((always_inline)) int q2_cmp_int(int sub, "
        "int32_t a, int32_t b)\n"
        "{\n"
        "  switch (sub) {\n"
        "  case QJS_AOT_REG_BIN_LT: return a < b;\n"
        "  case QJS_AOT_REG_BIN_LTE: return a <= b;\n"
        "  case QJS_AOT_REG_BIN_GT: return a > b;\n"
        "  case QJS_AOT_REG_BIN_GTE: return a >= b;\n"
        "  case QJS_AOT_REG_BIN_EQ: case QJS_AOT_REG_BIN_STRICT_EQ: return a "
        "== b;\n"
        "  case QJS_AOT_REG_BIN_NEQ: case QJS_AOT_REG_BIN_STRICT_NEQ: return a "
        "!= b;\n"
        "  default: return -1;\n"
        "  }\n"
        "}\n\n"
        "static inline __attribute__((always_inline)) int q2_cmp_double(int "
        "sub, double a, double b)\n"
        "{\n"
        "  switch (sub) {\n"
        "  case QJS_AOT_REG_BIN_LT: return a < b;\n"
        "  case QJS_AOT_REG_BIN_LTE: return a <= b;\n"
        "  case QJS_AOT_REG_BIN_GT: return a > b;\n"
        "  case QJS_AOT_REG_BIN_GTE: return a >= b;\n"
        "  case QJS_AOT_REG_BIN_EQ: case QJS_AOT_REG_BIN_STRICT_EQ: return a "
        "== b;\n"
        "  case QJS_AOT_REG_BIN_NEQ: case QJS_AOT_REG_BIN_STRICT_NEQ: return a "
        "!= b;\n"
        "  default: return -1;\n"
        "  }\n"
        "}\n\n");

    /* Every body is out: a region that lent a stack object to a callee must find the callee's emitted region taking it (a mismatch is the generator's to report, never an unsound unit). */
    if (region_lent_verify() > 0) e_fatal("a region lent a stack object to a callee whose emitted region does not take it", NULL);
    /* Forward declarations, so a compiled body can name a callee that is
       emitted later in this file. A call site names the callee's `aot_f_` for
       its identity check and its `aot_n_` for the call, and either may be
       emitted after the caller, so both are declared here for every selected
       function rather than only the ones the caller happens to precede.

       `aot_n_` exists only for a function a compiled caller can enter on the
       C stack (`light_ok`); a call to anything else takes the generic path.
       Declaring the prototype unconditionally for a light function is what
       lets a call site in an earlier body name it. */
    for (i = 0; i < nfuncs; i++) {
      char hex[33];
      if (!sel[i] || !eb[i].buf) continue;
      hex16(funcs[i].digest, hex);
      fprintf(
          f,
          "%sJSValue aot_f_%s(JSContext *, JSRegRun *, JSValue *, "
          "uint32_t);\n#define Q2_SEL_%s 1\n",
          e_split_on() ? "__attribute__((visibility(\"hidden\"))) " : "static ",
          hex, hex);
      /* the register file a frame of this function is built with (a region's exit builds inlined callees' frames; the engine scans them over this many) */
      fprintf(f, "#define Q2_FR_%s %d\n", hex, qjs_aot_frame_regs(i) > funcs[i].reg_count ? qjs_aot_frame_regs(i) : funcs[i].reg_count);
      /* `Q2_SEL_` says the function's *body* is in this unit. A call site needs
         more: the native *entry*, which exists only for a light-eligible
         function, so the two must be distinguished or a call site names an
         `aot_n_` nothing defines. */
      if (g_rx_need != NULL && g_rx_need[i])
        fprintf(
            f,
            "%sJSValue aot_rx_%s(JSContext *, JSRegRun *, JSObject *, JSValue, JSValue *, const ENx *);\n",
            e_split_on() ? "__attribute__((visibility(\"hidden\"))) " : "static ", hex);
      if (qjs_aot_has_native_entry(i))
        fprintf(
            f,
            "#define Q2_NATIVE_%s 1\n"
            "%sE_NONE JSValue aot_n_%s(JSContext *, JSRegRun *, JSObject *, "
            "JSValue, JSValue *, int);\n"
            "%sJSValue aot_e_%s(JSContext *, JSRegRun *, JSObject *, "
            "JSValue, JSValue *, int);\n",
            hex,
            e_split_on() ? "__attribute__((visibility(\"hidden\"))) " : "static ",
            hex,
            e_split_on() ? "__attribute__((visibility(\"hidden\"))) " : "static ",
            hex);
      {
        /* the parameters this function's region takes as stack blocks lent by its callers */
        unsigned lm_ = region_lent_real(hex);
        if (lm_ != 0) fprintf(f, "#define RG_LENT_%s %uu\n", hex, lm_);
        if (region_retslot_real(hex))
          fprintf(f, "#define RG_RETSLOT_%s 1\n%sJSValue aot_nr_%s(JSContext *, JSRegRun *, JSObject *, JSValue, JSValue *, int, JSValue *);\n", hex, e_split_on() ? "__attribute__((visibility(\"hidden\"))) " : "static ", hex);
      }
    }
    fprintf(f, "\n");
    /* The inline forms of the selected functions that have a native entry, before
       any caller. */
    for (int sweep = 0; g_inl_eb && sweep < 5; sweep++)
    for (i = 0; g_inl_eb && i < nfuncs; i++) {
      char hex[33];
      if (!sel[i] || !eb[i].buf || !g_inl_eb[i].buf ||
          !qjs_aot_has_native_entry(i) || g_inl_lvl[i] != sweep)
        continue;
      hex16(funcs[i].digest, hex);
      fprintf(f, "#define Q2_INLINE_%s 1\n", hex);
      if (g_inl_nx && g_inl_nx[i])
        fprintf(
            f,
            "%sJSValue aot_nx_%s(JSContext *, JSRegRun *, JSObject *, JSValue, "
            "JSValue *, const ENx *);\n",
            e_split_on() ? "__attribute__((visibility(\"hidden\"))) " : "static ",
            hex);
      fwrite(g_inl_eb[i].buf, 1, g_inl_eb[i].len, f);
    }
    for (i = 0; i < nfuncs; i++) {
      if (!sel[i] || !eb[i].buf) continue;
      if (split_dir != NULL) {
        /* One file per compiled function. The name is the digest, so it is
           stable across runs and an unchanged function produces a byte
           identical file -- which is what lets the build skip it. */
        char hex[33], path[4096], inc[64];
        FILE *pf;
        hex16(funcs[i].digest, hex);
        snprintf(path, sizeof(path), "%s/f_%s.c", split_dir, hex);
        pf = fopen(path, "w");
        if (!pf) {
          fprintf(stderr, "qjsaot: cannot write %s\n", path);
          fclose(f);
          return 2;
        }
        snprintf(inc, sizeof(inc), "\"u.h\"");
        fprintf(pf, "#include \"quickjs-aot-rt.h\"\n#include %s\n", inc);
        fprintf(
            pf,
            "#if defined(__APPLE__)\n"
            "#define E_TEXT __attribute__((section(\"__TEXT,aottext,regular,pure_instructions\")))\n"
            "extern char __start_aottext[] __asm(\"section$start$__TEXT$aottext\");\n"
            "extern char __stop_aottext[] __asm(\"section$end$__TEXT$aottext\");\n"
            "#else\n"
            "#ifndef E_TEXT\n#define E_TEXT __attribute__((section(\"aottext\")))\n#endif\n"
            "extern char __start_aottext[] __attribute__((weak));\n"
            "extern char __stop_aottext[] __attribute__((weak));\n"
            "#endif\n");
        /* The pure leaf helpers are `static inline` and are written once, to
           the shared header above; repeating them in each part would be a
           redefinition. Only the function's own bodies go here. */
        fwrite(eb[i].buf, 1, eb[i].len, pf);
        if (g_rx_need != NULL && g_rx_need[i]) e_emit_resume_fn(pf, &funcs[i], qjs_aot_frame_regs(i), hex);
        fclose(pf);
      } else {
        fwrite(eb[i].buf, 1, eb[i].len, f);
        if (g_rx_need != NULL && g_rx_need[i]) {
          char hexr[33];
          hex16(funcs[i].digest, hexr);
          e_emit_resume_fn(f, &funcs[i], qjs_aot_frame_regs(i), hexr);
        }
      }
      actual_bytes += (long)eb[i].len;
      emitted++;
    }

    if (split_dir != NULL) {
      /* The registration table is one small file of its own, so it can be
         recompiled without touching any function. */
      char rpath[4096];
      fclose(f);
      snprintf(rpath, sizeof(rpath), "%s/reg.c", split_dir);
      f = fopen(rpath, "w");
      if (!f) {
        fprintf(stderr, "qjsaot: cannot write %s\n", rpath);
        return 2;
      }
      fprintf(f, "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n"
                 "#include \"quickjs-aot-rt.h\"\n#include \"u.h\"\n");
      /* The switch-off counters are declared in the shared header, so they are defined
         exactly once, here. */
      fprintf(
          f,
          "uint64_t e_ent[QJS_AOT_NFUNCS];\n"
          "uint32_t e_exi[QJS_AOT_NFUNCS], e_ref[QJS_AOT_NFUNCS], e_refs[QJS_AOT_NFUNCS];\n"
          "uint8_t e_off[QJS_AOT_NFUNCS], e_gate[QJS_AOT_NFUNCS];\n"
          "uint32_t e_han[QJS_AOT_NFUNCS];\n"
          "uint8_t e_nfl[QJS_AOT_NFUNCS], e_dis[QJS_AOT_NFUNCS];\n"
          "uint8_t e_osr_ok[QJS_AOT_NFUNCS], e_osr_n[QJS_AOT_NFUNCS];\n"
          "E_XNode *e_xhead;\n"
          "E_XT e_xt[E_XT_N];\n"
          "uint32_t e_ngated;\n"
          "int e_xw;\n"
          "uint8_t e_gwhy[QJS_AOT_NFUNCS];\n"
          "uint64_t e_inl[QJS_AOT_NFUNCS];\n"
          "uint32_t e_inx[QJS_AOT_NFUNCS];\n"
          "uint8_t e_ioff[QJS_AOT_NFUNCS];\n"
          "_Thread_local ENx e_xs;\n"
          "_Thread_local JSValue e_xl[QJS_AOT_MAXREGS];\n"
          "const char *const qjs_aot_e_names[] = {\n"
          );
      {
        int ni;
        for (ni = 0; ni < nfuncs; ni++)
          fprintf(f, "  \"%s\",\n", funcs[ni].name[0] ? funcs[ni].name : "?");
        if (nfuncs == 0) fprintf(f, "  \"?\",\n");
      }
      fprintf(f,
          "};\n"
          /* Defined here, once, so the destructor is linked: a definition in a
             header included by every part is dropped as unused in each. */
          "/* One line per function, so a case can assert on ONE function's\n"
          "   hand-offs. The whole-script total cannot express remote\"a\"\n"
          "   counts, which is what a faster-not-just-correct fix needs. */\n"
          "void __attribute__((destructor)) e_stats_dump(void)\n"
          "{\n"
          "  unsigned long long en = 0, ex = 0, rf = 0, off = 0, hn = 0, fl = 0, il = 0;\n"
          "  const char *e = getenv(\"QJS_AOT_UNIT_STATS\");\n"
          "  int i;\n"
          "  if (e == NULL || e[0] != '1') return;\n"
          "  for (i = 0; i < QJS_AOT_NFUNCS; i++) {\n"
          "    en += e_ent[i]; ex += e_exi[i]; rf += e_ref[i]; off += e_off[i];\n"
          "    hn += e_han[i]; il += e_inl[i]; if (e_nfl[i]) fl += e_ent[i];\n"
          "  }\n"
          "  fprintf(stderr, \"unit-stats enters=%%llu exits=%%llu handoffs=%%llu refuses=%%llu off=%%llu frameless=%%llu inlined=%%llu\\n\",\n"
          "          en, ex, hn, rf, off, fl, il);\n"
          E_UNIT_STATS_TAIL
          "}\n"
          "void __attribute__((destructor)) e_gate_report(void)\n"
          "{\n"
          E_GATE_REPORT_BODY
          "}\n"
          );
    }
    fprintf(f, "const JSAotEntry qjs_aot_table[] = {\n");
    {
      int *tbl_idx = malloc(sizeof(int) * (size_t)(nfuncs > 0 ? nfuncs : 1));
      int ti = 0;
      for (i = 0; i < nfuncs; i++) tbl_idx[i] = -1;
      for (i = 0; i < nfuncs; i++) {
        char hex[33];
        if (!sel[i]) continue;
        tbl_idx[i] = ti++;
        hex16(funcs[i].digest, hex);
        fprintf(f, "    { {");
        {
          int k;
          for (k = 0; k < 16; k++)
            fprintf(f, "%s0x%02x", k ? ", " : "", funcs[i].digest[k]);
        }
        /* The register file the body was compiled for. The engine scans a light
           frame over this many JSValues and sizes the frame for it, because the
           program's own `regvm_reg_count` does not cover the registers the
           generator allocates above it. Clamped to at least the program's count,
           so an unbound or never-widened function reports its own. */
        {
          int fr = qjs_aot_frame_regs(i);
          if (fr < funcs[i].reg_count) fr = funcs[i].reg_count;
          if (fr < 0) fr = 0;
          if (qjs_aot_has_native_entry(i) && getenv("QJS_AOT_NONFN") == NULL)
            fprintf(f, "}, aot_f_%s, aot_e_%s, 0u, 0x%08xu, %du },\n", hex, hex,
                    funcs[i].code_crc, (unsigned)fr);
          else
            fprintf(f, "}, aot_f_%s, NULL, 0u, 0x%08xu, %du },\n", hex,
                    funcs[i].code_crc, (unsigned)fr);
        }
      }
      fprintf(f, "};\n");
      /* Table index -> the function index the statistics arrays use (the table holds only the selected functions). */
      fprintf(f, "const int32_t qjs_aot_tab_fi[] = {");
      {
        int any = 0;
        for (i = 0; i < nfuncs; i++)
          if (sel[i]) fprintf(f, "%s%d", any++ ? ", " : " ", i);
        if (!any) fprintf(f, " -1");
      }
      fprintf(f, " };\n");
      /* Deopt site -> its function's index in the emitted table. A site whose
         function was not selected cannot run, so it maps to no function. */
      fprintf(f, "const uint32_t qjs_aot_deopt_site_func[] = {");
      if (g_deopt_site_next == 0) {
        fprintf(f, " 0u };\n");
      } else {
        for (i = 0; i < g_deopt_site_next; i++) {
          int fi = g_deopt_site_func ? g_deopt_site_func[i] : -1;
          int t = (fi >= 0 && fi < nfuncs) ? tbl_idx[fi] : -1;
          fprintf(
              f, "%s%du", i ? ", " : " ", t < 0 ? 0xffffffffu : (unsigned)t);
        }
        fprintf(f, " };\n");
      }
      free(tbl_idx);
    }
    fprintf(
        f,
        "const uint32_t qjs_aot_count = "
        "sizeof(qjs_aot_table) / sizeof(qjs_aot_table[0]);\n");
    /* The bundle this unit was generated for: a hash of the digests of its functions. The engine records it in the runtime that binds the
       unit, which owns it until it is freed (a unit serves one runtime at a time). */
    {
      uint64_t bh = 1469598103934665603ULL;
      int bi, bb;
      for (bi = 0; bi < nfuncs; bi++)
        for (bb = 0; bb < 16; bb++) {
          bh ^= funcs[bi].digest[bb];
          bh *= 1099511628211ULL;
        }
      fprintf(f, "const uint64_t qjs_aot_bundle_tag = 0x%016llxULL;\n", (unsigned long long)bh);
    }
    /* The unit's teardown, called once at the start of JS_FreeRuntime. The site
       caches and the contracted layout shapes hold counted references that must be
       dropped while the runtime is still alive; the final collection follows, so a
       shape still held here would keep its whole object graph reachable. The
       runtime that filled the caches is recorded on the first fill, so a second
       runtime only ever runs the full checks and never frees against the first. */
    /* The function objects the call sites hold (see q2_call_entry_c): the first runtime
       to fill one owns them all, and its teardown drops each reference. */
    /* The constant globals' slots, the per-function fact words and the cold entry that validates them (see e_kslot_of in ops.c). */
    {
      extern int qjs_aot_k_count(void);
      extern const char *qjs_aot_k_name(int k);
      extern const char *qjs_aot_k_hex(int k);
      extern int qjs_aot_k_used(int fi, int k);
      extern int qjs_aot_k_called(int k);
      extern int qjs_aot_k_layout(int k);
      int nk = qjs_aot_k_count(), k, fi_;
      {
        int any_layout = 0;
        for (k = 0; k < nk; k++)
          if (qjs_aot_k_layout(k) > 0) any_layout = 1;
        fprintf(f, "#define Q2_KLAYOUTS %d\n", any_layout);
        if (any_layout) fprintf(f, "extern void *q2_layout_shape[];\n");
      }
      fprintf(f, "JSValue q2_kv[%d];\nuint64_t q2_kw[QJS_AOT_NFUNCS];\n", nk + 1);
      fprintf(f, "static JSRuntime *q2_k_rt;\nstatic uint16_t q2_ktry[QJS_AOT_NFUNCS];\nunsigned q2_k_gated;\n");
      for (k = 0; k < nk; k++)
        if (qjs_aot_k_hex(k) != NULL)
          fprintf(f, "#ifdef Q2_NATIVE_%s\n__attribute__((visibility(\"hidden\"))) JSValue aot_f_%s(JSContext *, JSRegRun *, JSValue *, uint32_t);\n#endif\n", qjs_aot_k_hex(k), qjs_aot_k_hex(k));
      fprintf(f, "static const struct { const char *name; const void *entry; int layout; } q2_kd[%d] = {\n", nk + 1);
      for (k = 0; k < nk; k++) {
        const char *hx = qjs_aot_k_hex(k);
        if (hx != NULL)
          fprintf(f, "  { \"%s\",\n#ifdef Q2_NATIVE_%s\n (const void *)aot_f_%s\n#else\n NULL\n#endif\n , %d },\n", qjs_aot_k_name(k), hx, hx, qjs_aot_k_layout(k));
        else
          fprintf(f, "  { %s, NULL, 0 },\n", hx != NULL ? "\"?\"" : "NULL");
      }
      fprintf(f, "  { NULL, NULL, 0 } };\n");
      fprintf(f, "static const uint64_t q2_kmask[QJS_AOT_NFUNCS] = {\n");
      for (fi_ = 0; fi_ < nfuncs; fi_++) {
        unsigned long long m = 0;
        for (k = 0; k < nk && k < 64; k++)
          if (qjs_aot_k_used(fi_, k)) m |= 1ull << k;
        fprintf(f, "%s0x%llxull", fi_ ? (fi_ % 8 ? ", " : ",\n") : "", m);
      }
      fprintf(f, " };\n");
      fprintf(
          f,
          /* Validate the constants function `fi` reads: each global holds the unit slot's object (the first function to run fills it), a function whose
             bound entry is the one the call sites assume; a fact on the global object per global moves the function's word to 0 when the property is
             stored to, deleted or redefined. A slot is never replaced: a global that holds another object later leaves its readers on the generic body. */
          "static int q2_kres(JSContext *ctx, int fi)\n"
          "{\n"
          "  uint64_t m = q2_kmask[fi];\n"
          "  JSRuntime *rt = JS_GetRuntime(ctx);\n"
          "  JSValue gv;\n"
          "  int k, ok = 1;\n"
          "  if (q2_k_rt != NULL && q2_k_rt != rt) return 0;\n"
          "  gv = JS_GetGlobalObject(ctx);\n"
          "  for (k = 0; ok && k < 64; k++) {\n"
          "    JSValue v;\n"
          "    JSAtom a;\n"
          "    JSAotFact *g;\n"
          "    if (!((m >> k) & 1)) continue;\n"
          "    v = JS_GetPropertyStr(ctx, gv, q2_kd[k].name);\n"
          "    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT || !JS_IsFunction(ctx, v) || (q2_kd[k].entry != NULL && !q2_call_entry(v, q2_kd[k].entry))) {\n"
          "      ok = 0; if (getenv(\"QJS_AOT_KTRACE\")) fprintf(stderr, \"kres: fn %%d global %%s: not a function with the expected entry (tag %%d fn %%d)\\n\", fi, q2_kd[k].name, (int)JS_VALUE_GET_TAG(v), JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT ? JS_IsFunction(ctx, v) : -1);\n"
          "    } else if (JS_VALUE_GET_TAG(q2_kv[k]) == JS_TAG_OBJECT) {\n"
          "      ok = JS_VALUE_GET_PTR(q2_kv[k]) == JS_VALUE_GET_PTR(v);\n"
          "    } else {\n"
          "      q2_k_rt = rt;\n"
          "      q2_kv[k] = JS_DupValue(ctx, v);\n"
          "    }\n"
          "#if Q2_KLAYOUTS\n"
          "    if (ok && q2_kd[k].layout > 0) {\n"
          /* `x instanceof C` answered by the shape of x. The layout's prototype must be the live C.prototype. C must keep exactly its own properties
             length, name and prototype (so it has no own @@hasInstance; Function.prototype's is not writable or configurable) with the values read
             here, and its [[prototype]] must stay Function.prototype: a closed fact on C lists the three values and a prototype fact covers the rest. */
          "      JSValue pr = JS_GetPropertyStr(ctx, v, \"prototype\"), ln = JS_GetPropertyStr(ctx, v, \"length\"), nm = JS_GetPropertyStr(ctx, v, \"name\");\n"
          "      JSValue fp = JS_GetPrototype(ctx, v);\n"
          "      const unsigned char *sh = (const unsigned char *)q2_layout_shape[q2_kd[k].layout - 1];\n"
          "      JSAotFact *gp, *gq;\n"
          "      JSAtom pa = JS_NewAtom(ctx, \"prototype\"), la = JS_NewAtom(ctx, \"length\"), na = JS_NewAtom(ctx, \"name\");\n"
          "      ok = sh != NULL && JS_VALUE_GET_TAG(pr) == JS_TAG_OBJECT && JS_VALUE_GET_TAG(fp) == JS_TAG_OBJECT &&\n"
          "           *(const void *const *)(sh + QJS_AOT_OFF_JSSHAPE_PROTO) == JS_VALUE_GET_PTR(pr);\n"
          "      if (ok) {\n"
          "        gp = qjs_aot_fact_new(ctx, v, JS_UNDEFINED, QJS_AOT_FACT_CLOSED, NULL, 0);\n"
          "        gq = qjs_aot_fact_proto_is(ctx, v, fp);\n"
          "        ok = gp != NULL && gq != NULL && qjs_aot_fact_expect(ctx, gp, pa, pr) == 0 && qjs_aot_fact_expect(ctx, gp, la, ln) == 0 &&\n"
          "             qjs_aot_fact_expect(ctx, gp, na, nm) == 0 && qjs_aot_fact_validate(ctx, gp) == 1;\n"
          "        if (ok) { qjs_aot_fact_depend64(gp, &q2_kw[fi], 1); qjs_aot_fact_depend64(gq, &q2_kw[fi], 1); }\n"
          "      }\n"
          "      if (!ok && getenv(\"QJS_AOT_KTRACE\")) fprintf(stderr, \"kres: fn %%d %%s: instanceof facts refused\\n\", fi, q2_kd[k].name);\n"
          "      JS_FreeAtom(ctx, pa); JS_FreeAtom(ctx, la); JS_FreeAtom(ctx, na);\n"
          "      JS_FreeValue(ctx, pr); JS_FreeValue(ctx, ln); JS_FreeValue(ctx, nm);\n"
          "      JS_FreeValue(ctx, fp);\n"
          "    }\n"
          "#endif\n"
          "    if (ok) {\n"
          "      a = JS_NewAtom(ctx, q2_kd[k].name);\n"
          "      g = qjs_aot_fact_new(ctx, gv, JS_UNDEFINED, 0, &a, 0);\n"
          "      if (g != NULL && qjs_aot_fact_expect(ctx, g, a, q2_kv[k]) == 0 && qjs_aot_fact_validate(ctx, g) == 1) qjs_aot_fact_depend64(g, &q2_kw[fi], 1);\n"
          "      else ok = 0;\n"
          "      JS_FreeAtom(ctx, a);\n"
          "    }\n"
          "    JS_FreeValue(ctx, v);\n"
          "  }\n"
          "  JS_FreeValue(ctx, gv);\n"
          "  { int c_ = ok && qjs_aot_facts_commit(ctx, &q2_kw[fi], 1); if (!c_ && getenv(\"QJS_AOT_KTRACE\")) fprintf(stderr, \"kres: fn %%d refused (ok %%d, mask %%llx)\\n\", fi, ok, (unsigned long long)m); return c_; }\n"
          "}\n"
          /* The cold path of an entry whose fact word is clear: validate and run the body, or run the generic body. A function that fails 256 times is
             switched off for good, which is counted (q2_k_gated). */
          "JSValue q2_kentry(JSContext *ctx_in, int fi, JSValue (*fast)(JSContext *, JSRegRun *, JSObject *, JSValue, JSValue *, int),\n"
          "                  JSRegRun *caller_f, JSObject *co, JSValue this_obj, JSValue *argv, int argc, int needpic)\n"
          "{\n"
          "  if (e_gate[fi]) { if (getenv(\"QJS_AOT_KTRACE\")) fprintf(stderr, \"kentry: fn %%d gated: e_gate (why %%d)\\n\", fi, (int)e_gwhy[fi]); return qjs_aot_gated_call(ctx_in, co, this_obj, argv, argc); }\n"
          "  if (needpic && *(void **)(*(uint8_t **)((uint8_t *)co + QJS_AOT_OFF_OBJECT_FUNC_BYTECODE) + QJS_AOT_OFF_FNBC_REGVM_PIC) == NULL)\n"
          "    return qjs_aot_call_interp(ctx_in, co, this_obj, argv, argc);\n"
          "  if (q2_ktry[fi] < 256) {\n"
          "    q2_ktry[fi]++;\n"
          "    if (q2_kres(ctx_in, fi)) return fast(ctx_in, caller_f, co, this_obj, argv, argc);\n"
          "    if (q2_ktry[fi] == 256) { q2_k_gated++; e_gate_note(fi); }\n"
          "  }\n"
          "  return qjs_aot_gated_call(ctx_in, co, this_obj, argv, argc);\n"
          "}\n"
          "static void q2_k_release(JSRuntime *rt)\n"
          "{\n"
          "  int k;\n"
          "  if (q2_k_rt != rt) return;\n"
          "  for (k = 0; k < %d; k++)\n"
          "    if (JS_VALUE_GET_TAG(q2_kv[k]) == JS_TAG_OBJECT) { JS_FreeValueRT(rt, q2_kv[k]); q2_kv[k] = JS_UNDEFINED; }\n"
          "  q2_k_rt = NULL;\n"
          "}\n",
          nk);
    }
    fprintf(
        f,
        "void *q2_cc[%d];\n"
        "#ifdef QJS_AOT_SPLIT_UNIT\n"
        "JSRuntime *q2_cc_rt; void *q2_ph[Q2_PH_MAX]; int q2_phn; JSObject **q2_fh_slot[Q2_FH_MAX]; int q2_fhn;\n"
        "#else\n"
        "static JSRuntime *q2_cc_rt;\n"
        "#endif\n"
        "void q2_cc_fill(JSContext *ctx, uint32_t ix, JSValue fnv)\n"
        "{\n"
        "  JSRuntime *rt = JS_GetRuntime(ctx);\n"
        "  if (q2_cc_rt == NULL) q2_cc_rt = rt;\n"
        "  if (q2_cc_rt != rt || q2_cc[ix] != NULL) return;\n"
        "  q2_cc[ix] = JS_VALUE_GET_PTR(JS_DupValue(ctx, fnv));\n"
        "}\n"
        "static void q2_cc_release(JSRuntime *rt)\n"
        "{\n"
        "  uint32_t i;\n"
        "  q2_k_release(rt);\n"
        "  if (q2_cc_rt != rt) return;\n"
        "  for (i = 0; i < %du; i++)\n"
        "    if (q2_cc[i] != NULL) {\n"
        "      JS_FreeValueRT(rt, JS_MKPTR(JS_TAG_OBJECT, q2_cc[i]));\n"
        "      q2_cc[i] = NULL;\n"
        "    }\n"
        "%s"
        "  q2_cc_rt = NULL;\n"
        "}\n",
        g_cc_count + 1, g_cc_count + 1, "  q2_ph_release(rt);\n");
    /* The contracted layouts. This file owns them: the table of rows (prototype
       kind, constructor digest, ordered field names), the shapes the built chains
       end, the runtime that holds those references, and both runtime-wide entry
       points. `u.h` carries only what a body needs at compile time. A layout is
       built once, outside any compiled frame, and released once at teardown. */
    {
      extern int qjs_aot_layout_export(ELayoutInfo *out, int cap);
      extern const void *qjs_aot_prof_shape(int id, int *proto, char *digest,
                                            int *nfields, const char ***fields,
                                            unsigned **flags);
      ELayoutInfo *cand = calloc(E_LAYOUT_MAX, sizeof(*cand));
      int nc = cand != NULL ? qjs_aot_layout_export(cand, E_LAYOUT_MAX) : 0;
      int li;
      g_nlayouts = nc;
      if (nc > 0) {
        fprintf(f, "#define Q2_MAXF %d\n", PROF_MAX_FIELDS);
        fprintf(f,
                "void *q2_layout_shape[Q2_LAYOUT_COUNT + 1u];\n"
                "static void *q2_layout_chain[Q2_LAYOUT_COUNT][Q2_MAXF + 1];\n"
                "static uint8_t q2_layout_state[Q2_LAYOUT_COUNT];\n"
                "static JSRuntime *q2_owner_rt;\n");
        fprintf(f, "static const char *const q2_layout_fields[Q2_LAYOUT_COUNT][Q2_MAXF] = {\n");
        for (li = 0; li < nc; li++) {
          const char **fld = NULL;
          unsigned *flg = NULL;
          int nf = 0, fi;
          qjs_aot_prof_shape(cand[li].shape_id, NULL, NULL, &nf, &fld, &flg);
          fprintf(f, "  {");
          for (fi = 0; fi < nf; fi++)
            fprintf(f, "%s\"%s\"", fi ? "," : "", fld[fi] != NULL ? fld[fi] : "");
          fprintf(f, "},\n");
        }
        fprintf(f, "};\n");
        if (qjs_aot_layout_claims_any()) {
          fprintf(f, "const uint8_t q2_layout_claim[Q2_LAYOUT_COUNT][32] = {\n");
          for (li = 0; li < nc; li++) {
            int fi;
            fprintf(f, "  {");
            for (fi = 0; fi < PROF_MAX_FIELDS; fi++)
              fprintf(f, "%s%d", fi ? "," : "", qjs_aot_layout_claim_of(li + 1, fi));
            fprintf(f, "},\n");
          }
          fprintf(f, "};\n");
        }
        if (qjs_aot_layout_claims_any())
          fprintf(f,
                  "static void q2_install_claims(JSContext *ctx, uint32_t i)\n"
                  "{\n"
                  "  uint32_t k;\n"
                  "  for (k = 0; k < Q2_MAXF; k++)\n"
                  "    if (q2_layout_claim[i][k] != 0)\n"
                  "      qjs_aot_layout_claim(ctx, i + 1u, k, q2_layout_claim[i][k]);\n"
                  "}\n");
        else
          fprintf(f, "static void q2_install_claims(JSContext *ctx, uint32_t i) { (void)ctx; (void)i; }\n");
        fprintf(f, "static const uint32_t q2_layout_nfields[Q2_LAYOUT_COUNT] = {");
        for (li = 0; li < nc; li++)
          fprintf(f, "%s%du", li ? "," : "", (unsigned)cand[li].nfields);
        fprintf(f, "};\n");
        fprintf(f, "static const int q2_layout_proto[Q2_LAYOUT_COUNT] = {");
        for (li = 0; li < nc; li++)
          fprintf(f, "%s%d", li ? "," : "", cand[li].proto);
        fprintf(f, "};\n");
        fprintf(f, "static const uint8_t q2_layout_digest[Q2_LAYOUT_COUNT][16] = {\n");
        for (li = 0; li < nc; li++) {
          int bi;
          fprintf(f, "  {");
          for (bi = 0; bi < 16; bi++) {
            unsigned v = 0;
            if (cand[li].proto == 1) sscanf(cand[li].digest + bi * 2, "%2x", &v);
            fprintf(f, "%s0x%02x", bi ? "," : "", v);
          }
          fprintf(f, "},\n");
        }
        fprintf(f, "};\n");
        {
          /* The constant path (`Flog.RayTracer.Camera`, or a global's name) of the class a layout row belongs to, when the region compiler knows exactly one class with that ordered field list: two
             classes made by one function share a digest, and the row must be built against the prototype of THEIR closure, found by this path. "" = the legacy rule (the closure that constructs first). */
          fprintf(f, "static const char *const q2_layout_path[Q2_LAYOUT_COUNT] = {\n");
          for (li = 0; li < nc; li++) {
            const char **fld = NULL;
            unsigned *flg = NULL;
            int nf = 0, fi2;
            char names2[32][40], path2[64];
            qjs_aot_prof_shape(cand[li].shape_id, NULL, NULL, &nf, &fld, &flg);
            path2[0] = 0;
            int shared_ = 0, lj;
            for (lj = 0; lj < nc; lj++) if (lj != li && cand[lj].proto == 1 && strcmp(cand[lj].digest, cand[li].digest) == 0) shared_ = 1;
            if (shared_ && cand[li].proto == 1 && nf > 0 && nf <= 32 && getenv("QJS_AOT_NOLAYOUTPATH") == NULL) {   /* a digest with one row keeps the legacy rule exactly */
              for (fi2 = 0; fi2 < nf; fi2++) snprintf(names2[fi2], 40, "%s", fld[fi2] != NULL ? fld[fi2] : "");
              region_class_path(names2, nf, path2, sizeof(path2));
            }
            fprintf(f, "  \"%s\",\n", path2);
          }
          fprintf(f, "};\n");
        }
        fprintf(
            f,
            "/* The first runtime to build claims the layouts, and records itself before the\n"
            "   first reference is taken; another runtime only ever runs the full checks. */\n"
            "static int q2_claim(JSRuntime *rt)\n"
            "{\n"
            "  if (q2_owner_rt == NULL) {\n"
            "    q2_owner_rt = rt;\n"
            "    *(void ***)((unsigned char *)rt + QJS_AOT_OFF_RT_LAYOUT_SHAPE) =\n"
            "        q2_layout_shape;\n"
            "    *(uint32_t *)((unsigned char *)rt + QJS_AOT_OFF_RT_LAYOUT_COUNT) =\n"
            "        Q2_LAYOUT_COUNT;\n"
            "  }\n"
            "  return q2_owner_rt == rt;\n"
            "}\n"
            "/* Lay out layout `i` against `proto` (borrowed). A failure is final: nothing is\n"
            "   stamped, so the entry check hands every such value to the generic body. */\n"
            "static void q2_build_one(JSContext *ctx, uint32_t i, JSValueConst proto)\n"
            "{\n"
            "  JSAtom atoms[Q2_MAXF];\n"
            "  uint32_t k, nf = q2_layout_nfields[i], made = 0, n = 0;\n"
            "  for (k = 0; k < nf; k++) {\n"
            "    atoms[k] = JS_NewAtom(ctx, q2_layout_fields[i][k]);\n"
            "    if (atoms[k] == 0) break;\n"
            "    made++;\n"
            "  }\n"
            "  if (made == nf)\n"
            "    n = qjs_aot_layout_build(ctx, proto, atoms, nf, q2_layout_chain[i],\n"
            "                             Q2_MAXF + 1, i + 1u);\n"
            "  for (k = 0; k < made; k++) JS_FreeAtom(ctx, atoms[k]);\n"
            "  if (n == nf + 1u) {\n"
            "    q2_layout_shape[i] = q2_layout_chain[i][nf];\n"
            "    q2_layout_state[i] = 1;\n"
            "    q2_install_claims(ctx, i);\n"
            "  } else {\n"
            "    q2_layout_state[i] = 2;\n"
            "  }\n"
            "}\n"
            "/* Object-literal layouts, built at the first bind. */\n"
            "int qjs_aot_layouts_build(JSContext *ctx)\n"
            "{\n"
            "  uint32_t i;\n"
            "  if (!q2_claim(JS_GetRuntime(ctx))) return 0;\n"
            "  for (i = 0; i < Q2_LAYOUT_COUNT; i++)\n"
            "    if (q2_layout_proto[i] == 2 && q2_layout_state[i] == 0)\n"
            "      q2_build_one(ctx, i, qjs_aot_object_prototype(ctx));\n"
            "  return 0;\n"
            "}\n"
            "/* Whether a constructor digest still has a layout to build. */\n"
            "int qjs_aot_layout_ctor(const uint8_t digest[16])\n"
            "{\n"
            "  uint32_t i;\n"
            "  for (i = 0; i < Q2_LAYOUT_COUNT; i++)\n"
            "    if (q2_layout_proto[i] == 1 && q2_layout_state[i] == 0 &&\n"
            "        memcmp(q2_layout_digest[i], digest, 16) == 0)\n"
            "      return 1;\n"
            "  return 0;\n"
            "}\n"
            "/* A constructor function object is about to make its first instance (the engine calls\n"
            "   this from the construct path, after any `F.prototype = X`, and once per bytecode). The\n"
            "   chain is built against the `.prototype` it has now, and that property is watched: any\n"
            "   later change of it breaks the layout. A second function object of the same digest\n"
            "   finds the layout built and keeps the first chain. */\n"
            "/* The value of the dotted path from the global object through own data properties only (a getter, an accessor or a missing link: undefined). Owned. */\n"
            "static JSValue q2_resolve_path(JSContext *ctx, const char *path)\n"
            "{\n"
            "  JSValue cur = JS_GetGlobalObject(ctx);\n"
            "  char buf[64], *seg, *next;\n"
            "  snprintf(buf, sizeof(buf), \"%%s\", path);\n"
            "  for (seg = buf; seg != NULL && *seg; seg = next) {\n"
            "    JSPropertyDescriptor d; JSAtom a; int r;\n"
            "    next = strchr(seg, '.');\n"
            "    if (next) *next++ = 0;\n"
            "    if (!JS_IsObject(cur)) { JS_FreeValue(ctx, cur); return JS_UNDEFINED; }\n"
            "    a = JS_NewAtom(ctx, seg);\n"
            "    r = JS_GetOwnProperty(ctx, &d, cur, a);\n"
            "    JS_FreeAtom(ctx, a);\n"
            "    JS_FreeValue(ctx, cur);\n"
            "    if (r <= 0) return JS_UNDEFINED;\n"
            "    JS_FreeValue(ctx, d.getter); JS_FreeValue(ctx, d.setter);\n"
            "    if (d.flags & JS_PROP_GETSET) { JS_FreeValue(ctx, d.value); return JS_UNDEFINED; }\n"
            "    cur = d.value;\n"
            "  }\n"
            "  return cur;\n"
            "}\n"
            "void qjs_aot_layout_fn_created(JSContext *ctx, JSValue fn, const uint8_t digest[16])\n"
            "{\n"
            "  uint32_t i;\n"
            "  if (!q2_claim(JS_GetRuntime(ctx))) return;\n"
            "  for (i = 0; i < Q2_LAYOUT_COUNT; i++) {\n"
            "    JSValue proto, rfn = JS_UNDEFINED;\n"
            "    JSValue bfn = fn;\n"
            "    if (q2_layout_proto[i] != 1 || q2_layout_state[i] != 0 ||\n"
            "        memcmp(q2_layout_digest[i], digest, 16) != 0)\n"
            "      continue;\n"
            "    if (q2_layout_path[i][0]) {\n"
            "      /* a class of a shared digest: its row is built against the closure its path names (own data properties only) */\n"
            "      uint8_t dg2[16];\n"
            "      rfn = q2_resolve_path(ctx, q2_layout_path[i]);\n"
            "      if (!JS_IsObject(rfn) || !qjs_aot_function_digest(rfn, dg2) || memcmp(dg2, digest, 16) != 0) { JS_FreeValue(ctx, rfn); continue; }\n"
            "      bfn = rfn;\n"
            "    }\n"
            "    if (getenv(\"QJS_AOT_STATS\")) fprintf(stderr, \"aot layout row %%u path '%%s' built against %%s\\n\", i, q2_layout_path[i], bfn.u.ptr == fn.u.ptr ? \"the constructing closure\" : \"the path's closure\");\n"
            "    proto = qjs_aot_function_prototype(ctx, bfn);\n"
            "    if (JS_IsUndefined(proto)) {\n"
            "      q2_layout_state[i] = 2;\n"
            "      JS_FreeValue(ctx, rfn);\n"
            "      continue;\n"
            "    }\n"
            "    q2_build_one(ctx, i, proto);\n"
            "    JS_FreeValue(ctx, proto);\n"
            "    if (q2_layout_state[i] == 1) qjs_aot_layout_watch_proto(ctx, bfn, i + 1u);\n"
            "    JS_FreeValue(ctx, rfn);\n"
            "  }\n"
            "}\n"
            "void qjs_aot_release(JSRuntime *rt)\n"
            "{\n"
            "  uint32_t i, k, built = 0, refused = 0, pending = 0;\n"
            "  q2_cc_release(rt);\n"
            "  if (q2_owner_rt != rt) return;\n"
            "  for (i = 0; i < Q2_LAYOUT_COUNT; i++) {\n"
            "    built += q2_layout_state[i] == 1;\n"
            "    refused += q2_layout_state[i] == 2;\n"
            "    pending += q2_layout_state[i] == 0;\n"
            "    if (q2_layout_state[i] == 1)\n"
            "      for (k = 0; k <= q2_layout_nfields[i]; k++) {\n"
            "        qjs_aot_layout_free(rt, q2_layout_chain[i][k]);\n"
            "        q2_layout_chain[i][k] = NULL;\n"
            "      }\n"
            "    q2_layout_shape[i] = NULL;\n"
            "    q2_layout_state[i] = 0;\n"
            "  }\n"
            "  if (getenv(\"QJS_AOT_STATS\"))\n"
            "    fprintf(stderr, \"aot layouts: built=%%u refused=%%u unbuilt=%%u broken=0x%%llx\\n\", built,\n"
            "            refused, pending, (unsigned long long)*(const uint64_t *)((unsigned char *)rt + QJS_AOT_OFF_RT_LAYOUT_BROKEN));\n"
            "  *(void ***)((unsigned char *)rt + QJS_AOT_OFF_RT_LAYOUT_SHAPE) = NULL;\n"
            "  *(uint32_t *)((unsigned char *)rt + QJS_AOT_OFF_RT_LAYOUT_COUNT) = 0;\n"
            "  q2_owner_rt = NULL;\n"
            "}\n");
      } else {
        fprintf(f, "void qjs_aot_release(JSRuntime *rt) { q2_cc_release(rt); }\n");
      }
      free(cand);
    }
    fprintf(f, "const uint32_t qjs_aot_abi = QJS_AOT_ABI;\n");
    fprintf(f, "const uint32_t qjs_aot_layout = QJS_AOT_LAYOUT_HASH;\n");
    fprintf(f, "const uint32_t qjs_aot_cell_count = 0u;\n");
    fprintf(f, "const uint32_t qjs_aot_site_count = %du;\n", g_deopt_site_next);
    fclose(f);
    qjsaot_number_guards(out);
    e_emit_regexps(out, hot_cover);

    if (getenv("QJS_AOT_LAYOUTCENSUS") != NULL)
      fprintf(stderr, "layout census: site-slot-refused=%d\n", g_slot_refused);
    {
      int dropped, kept;
      qjs_aot_layterm_counts(&dropped, &kept);
      fprintf(stderr, "qjsaot: layout-bit tests: %d dropped (an epoch test covers them), %d kept\n", dropped, kept);
    }
    fprintf(
        stderr,
        "qjsaot: %d functions seen, %d emitted, %lld instructions, "
        "coverage %.1f%% (%llu/%llu profiled ops), est %ld B, "
        "actual %ld B -> %s\n",
        nfuncs, emitted, total_insns,
        total_ops ? 100.0 * (double)cov_ops / (double)total_ops : 100.0,
        cov_ops, total_ops, est_bytes, actual_bytes, out);

    /* The bound fraction: of the profile's function records, how many the
       built bundle still has by digest. A changed bundle or a foreign profile
       drops its functions out silently, and this is the number that shows it. */
    if (g_nprof > 0) {
      int present = 0, hi;
      unsigned long long present_ops = 0, all_ops = 0;
      for (hi = 0; hi < g_nprof; hi++) {
        int ui = find_unit(funcs, nfuncs, g_prof[hi].digest);
        all_ops += g_prof[hi].ops;
        if (ui >= 0) {
          present++;
          present_ops += g_prof[hi].ops;
        }
      }
      fprintf(
          stderr,
          "qjsaot: bound %d/%d profile records (%.1f%% of profiled ops)\n",
          present, g_nprof,
          all_ops ? 100.0 * (double)present_ops / (double)all_ops : 100.0);
    }
    /* Which rop each spec block gave up at, weighted by the profile: the next
       thing to lower, chosen by data. */
    e_handoff_report(stderr);
    /* Functions the speculative body refused as irreducible. */
    if (e_irreducible)
      fprintf(
          stderr, "qjsaot: irreducible (refused for the speculative body): %d\n",
          e_irreducible);

    /* A construct site written after the prologue chose the ordinary forms would allocate young cells the unit cannot handle. */
    if (g_nursery_decided == 0 && getenv("QJS_AOT_NURSERY") == NULL && e_bodies_have_fused_site(eb, g_inl_eb, sel, nfuncs)) {
      fprintf(stderr, "qjsaot: a construct site was emitted after the nursery decision\n");
      return 1;
    }
    for (i = 0; i < nfuncs; i++) free(eb[i].buf);
    free(eb);
    free(uniq);
    free(sel);
  }

  js_aot_gen_free(rt, funcs, nfuncs);
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
  return fails == 0 ? 0 : 1;
}
