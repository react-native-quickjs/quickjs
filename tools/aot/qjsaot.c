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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs-aot-gen.h"
#include "quickjs-aot-rt.h"
#include "quickjs-regvm.h"
#include "quickjs.h"

/* Mutation-testing hooks. Each selects a deliberately wrong
 * lowering so the guard it bypasses can be shown to be load-bearing. Set with
 * `--mut <name>`; NULL means a correct build. */
static const char *g_mut = NULL;
static int mut_is(const char *s) {
  return g_mut != NULL && strcmp(g_mut, s) == 0;
}

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

#include "qjsaot2/e2.h"
#include "qjsaot2/prof.h"

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
   lives in qjsaot2/prof.h so the IR reads the same struct. */

/* An `S` record: the shapes one property site's inline cache saw. Shape values
   are addresses from the profiling process; they only say "the same shape as
   another site in this profile", never a runtime address. */
#define PROF_MAX_SHAPES 4
typedef struct {
  int site;
  int nshapes;
  uint64_t shape[PROF_MAX_SHAPES];
  /* Which cache kinds the site's fills installed, one bit per REG_PIC_* value
     (only meaningful when the header said `sitekinds=1`). */
  unsigned kinds;
} ProfSite;

/* A `U` record: a pc range the profiler never saw executed, inclusive. */
typedef struct {
  int lo, hi;
} ProfUnused;

typedef struct {
  uint8_t digest[16];
  unsigned long long ops;
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
  ProfUnused *unused;
  int nunused, capunused;
  /* The `T` records: sampler hits by pc, kept as the per-instruction execution
     count the op-call weight sums over. Sorted by pc after parsing. */
  ProfCount *counts;
  int ncounts, capcounts;
} ProfEntry;

/* The parsed profile (for call-site specialisation) and the current function's
   entry, set by emit_func; `g_site_next` hands out one AOT site id per emitted
   call site (the engine's per-runtime binding array is indexed by it). */
static const ProfEntry *g_prof = NULL;
static int g_nprof = 0;
/* Whether the profile's header said `thr=1`, i.e. it came from the threaded
   build whose `R` records are complete. See parse_profile. */
static int g_prof_thr = 0;
/* Whether the header said `callres=1`: the profile records call results on
   every instrumented return path. Without it no `K` record is believed. */
static int g_prof_callres = 0;
/* Whether the header said `sitekinds=1`: the `S` records carry the cache kinds,
   so a site can be told apart as an own slot, a prototype read or an add. */
static int g_prof_sitekinds = 0;
static const ProfEntry *g_cur_prof = NULL;
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
/* The index of the function `emit_func` is currently emitting, and the running
   count of table entries before it. */
static int g_cur_func_idx = 0;
static int g_emit_func_base = 0;
/* The unit's functions and selection, so the emitter can tell whether a
   profiled callee is emitted in this same translation unit. */
static const JSAotGenFunc *g_unit_funcs = NULL;
static int g_unit_nfuncs = 0;
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

/* --emit2: build the unit with the E2 emitter. --emit2-only HEX restricts it to
   one function so the two emitters can be compared function by function, and
   --emit2-report FILE writes one line per function saying which emitter
   produced its body. */
static const char *g_emit2_report = NULL;

/* --emit2-nocall: emit the generic body with no native call sites, so the
   call-cost benchmark can separate the entry's own cost from the call site's.
   A/B switch only; the shipped path always lowers call sites. */
static int e2_nocall;

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

/* The deopt site of the instruction `emit_func` is currently emitting, or -1.
   Under a deopt-everywhere sweep every instruction gets one, so a forced
   failure has somewhere to land and the fallback reuses the same site. */
static int g_cur_deopt_site = -1;

/* A fresh deopt site for the current function, or -1 when not sweeping. */
static int deopt_site_new(void) {
  int sid;
  if (!g_deopt_everywhere) return -1;
  sid = g_deopt_site_next++;
  deopt_site_note(sid);
  return sid;
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
    }
  }
  while (fgets(line, sizeof(line), f)) {
    uint8_t dg[16];
    ProfEntry *e;
    if ((line[0] != 'F' && line[0] != 'L' && line[0] != 'R' && line[0] != 'C' &&
         line[0] != 'P' && line[0] != 'S' && line[0] != 'U' && line[0] != 'T' &&
         line[0] != 'K') ||
        line[1] != ' ')
      continue;
    if (hex_to_bytes16(line + 2, dg) < 0) continue;
    e = prof_get(&arr, &n, &cap, dg);
    if (!e) return NULL;
    if (line[0] == 'F') {
      char *p = strstr(line + 2, "ops=");
      if (p) e->ops += strtoull(p + 4, NULL, 10);
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
        } else if (!strncmp(cl + 7, "c:", 2)) {
          char *mg = strstr(cl, ":magic=");
          char *cp = strstr(cl, ":cproto=");
          kind = 2;
          if (mg) magic = (int)strtol(mg + 7, NULL, 10);
          if (cp) cproto = (int)strtol(cp + 8, NULL, 10);
        }
        for (k = 0; k < e->ncalls; k++)
          if (e->calls[k].pc == pc) break;
        if (k == e->ncalls) {
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
        if (hits >= e->calls[k].hits) {
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
          for (j = 0; j < e->ncallsites; j++)
            if (e->callsites[j].pc == pc) {
              cs = &e->callsites[j];
              break;
            }
          if (!cs) {
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
        int k;
        double nmin = 0, nmax = 0;
        int has_num = 0, frac = 0, negzero = 0, elem = 0;
        if (nm) {
          has_num = 1;
          sscanf(nm + 4, "%lf..%lf", &nmin, &nmax);
        }
        if (fm) frac = (int)strtol(fm + 5, NULL, 10) != 0;
        if (nz) negzero = (int)strtol(nz + 8, NULL, 10) != 0;
        if (el) elem = (uint8_t)strtoul(el + 5, NULL, 10);
        for (k = 0; k < e->npcs; k++)
          if (e->pcs[k].pc == pc) break;
        if (k == e->npcs) {
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
        if (nm) sscanf(nm + 4, "%lf..%lf", &nmin, &nmax);
        for (k = 0; k < e->ncres; k++)
          if (e->cres[k].pc == pc) break;
        if (k == e->ncres) {
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
        for (j = 0; j < e->nsites; j++)
          if (e->sites[j].site == site) {
            ps = &e->sites[j];
            break;
          }
        if (!ps) {
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
        }
        {
          const char *kd = strstr(line + 2, "kinds=");
          if (kd) ps->kinds |= (unsigned)strtoul(kd + 6, NULL, 10);
        }
        for (k = 0; k < nshapes; k++) {
          char key[24];
          char *s;
          uint64_t sh;
          snprintf(key, sizeof(key), "shape%d=", k);
          s = strstr(line + 2, key);
          if (!s) continue;
          sh = strtoull(s + strlen(key), NULL, 16);
          for (j = 0; j < ps->nshapes; j++)
            if (ps->shape[j] == sh) break;
          if (j < ps->nshapes) continue;
          if (ps->nshapes < PROF_MAX_SHAPES) ps->shape[ps->nshapes++] = sh;
        }
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

/* A branch to itself or to an earlier instruction is a loop back edge: poll
   the interrupt handler first, the way every interpreter back edge does (a
   self-branch counts there too), so a compiled loop is stoppable. */
static void emit_branch(FILE *f, int off, int tgt) {
  if (tgt <= off) {
    fprintf(
        f,
        "    if (__builtin_expect(qjs_aot_poll(ctx) < 0, 0)) { pc = %d; goto "
        "exception; }\n",
        off);
  }
  fprintf(f, "    goto pc_%d;\n", tgt);
}

/* The slow path for one instruction. Under a deopt-everywhere sweep a guard
   miss leaves compiled code here (`guard_miss`), before the instruction has
   had any effect, so the interpreter re-executes it; an operation the unit
   does not lower still runs through the shared step routine, which handles
   every type. */
static void emit_fallback(
    FILE *f, int off, int next, int last, int guard_miss) {
  if (g_deopt_everywhere && guard_miss) {
    int sid = g_cur_deopt_site >= 0 ? g_cur_deopt_site : deopt_site_new();
    (void)next;
    (void)last;
    fprintf(f, "    return qjs_aot_deopt(ctx, f, fr, %d, %d);\n", off, sid);
    return;
  }
  fprintf(f, "    ip = qjs_aot_step(code + %d, fr, f, ctx);\n", off);
  fprintf(
      f,
      "    if (__builtin_expect(ip == NULL, 0)) { pc = %d; goto exception; }\n",
      off);
  fprintf(f, "    if (ip == QJS_AOT_COLD_RET) return qjs_aot_ret(f);\n");
  fprintf(
      f,
      "    if (__builtin_expect(ip == QJS_AOT_SUSPEND, 0)) return "
      "qjs_aot_ret(f);\n");
  if (!last)
    fprintf(
        f, "    if (__builtin_expect(ip == code + %d, 1)) goto pc_%d;\n", next,
        next);
  fprintf(f, "    pc = (uint32_t)(ip - code);\n    goto dispatch_ip;\n");
}

/* The REG_BIN_* sub-op of a per-sense short branch, or -1 for any other op. */
static int branch_subop(int op) {
  switch (op) {
    case ROP_branch_lt_s:
      return REG_BIN_LT;
    case ROP_branch_nlt_s:
      return REG_BIN_LT | REG_CMP_INVERT;
    case ROP_branch_lte_s:
      return REG_BIN_LTE;
    case ROP_branch_nlte_s:
      return REG_BIN_LTE | REG_CMP_INVERT;
    case ROP_branch_gt_s:
      return REG_BIN_GT;
    case ROP_branch_ngt_s:
      return REG_BIN_GT | REG_CMP_INVERT;
    case ROP_branch_gte_s:
      return REG_BIN_GTE;
    case ROP_branch_ngte_s:
      return REG_BIN_GTE | REG_CMP_INVERT;
    case ROP_branch_eq_s:
      return REG_BIN_EQ;
    case ROP_branch_neq_s:
      return REG_BIN_NEQ;
    default:
      return -1;
  }
}

/* The REG_BIN_* sub-op of a "rrr" binary op (excluding add/sub/mul, which have
   their own immediate-friendly template), or -1 for any other op. */
static int binop_subop(int op) {
  switch (op) {
    case ROP_div:
      return REG_BIN_DIV;
    case ROP_mod:
      return REG_BIN_MOD;
    case ROP_pow:
      return REG_BIN_POW;
    case ROP_bitand:
      return REG_BIN_AND;
    case ROP_bitor:
      return REG_BIN_OR;
    case ROP_bitxor:
      return REG_BIN_XOR;
    case ROP_shl:
      return REG_BIN_SHL;
    case ROP_sar:
      return REG_BIN_SAR;
    case ROP_shr:
      return REG_BIN_SHR;
    case ROP_lt:
      return REG_BIN_LT;
    case ROP_lte:
      return REG_BIN_LTE;
    case ROP_gt:
      return REG_BIN_GT;
    case ROP_gte:
      return REG_BIN_GTE;
    case ROP_eq:
      return REG_BIN_EQ;
    case ROP_neq:
      return REG_BIN_NEQ;
    case ROP_strict_eq:
      return REG_BIN_STRICT_EQ;
    case ROP_strict_neq:
      return REG_BIN_STRICT_NEQ;
    case ROP_instanceof:
      return REG_BIN_INSTANCEOF;
    case ROP_in:
      return REG_BIN_IN;
    default:
      return -1;
  }
}

/* The REG_UN_* sub-op of a "rr" unary op, or -1 for any other op. */
static int unop_subop(int op) {
  switch (op) {
    case ROP_neg:
      return REG_UN_NEG;
    case ROP_inc:
      return REG_UN_INC;
    case ROP_dec:
      return REG_UN_DEC;
    case ROP_not:
      return REG_UN_NOT;
    case ROP_to_numeric:
      return REG_UN_TO_NUMERIC;
    default:
      return -1;
  }
}

/*
 * Emit the Layer A fast path for one instruction. Returns 1 when it emitted
 * the whole `pc_N:` block (including the step fallback where the fast path can
 * miss), 0 to let the caller emit the plain step body. Every fast path mirrors
 * the named interpreter handler exactly; a miss falls through to the step.
 */
static int emit_template(
    FILE *f, const JSAotGenInsn *in, int off, int next, int last) {
  int d, a, b, site, sub;

  switch (in->op) {
    /* get_local / get_local_s0..s3: SET_REG_B(d, var_buf[slot]). */
    case ROP_get_local:
      d = (int)in->arg[0].val;
      fprintf(
          f, "    AOT_SET_B(ctx, fr, %d, vb[%d]);\n    goto pc_%d;\n", d,
          (int)in->arg[1].val, next);
      return 1;
    case ROP_get_local_s0:
    case ROP_get_local_s1:
    case ROP_get_local_s2:
    case ROP_get_local_s3:
      d = (int)in->arg[0].val;
      fprintf(
          f, "    AOT_SET_B(ctx, fr, %d, vb[%d]);\n    goto pc_%d;\n", d,
          in->op - ROP_get_local_s0, next);
      return 1;

    /* add/sub/mul: REGVM_BIN_OP over regvm_num_fast, step on a miss. */
    case ROP_add:
    case ROP_sub:
    case ROP_mul:
      sub = in->op == ROP_add   ? REG_BIN_ADD
            : in->op == ROP_sub ? REG_BIN_SUB
                                : REG_BIN_MUL;
      d = (int)in->arg[0].val;
      a = (int)in->arg[1].val;
      b = (int)in->arg[2].val;
      fprintf(f, "    { JSValue _o;\n");
      fprintf(
          f,
          "      if (qjs_aot_num_fast(%d, AOT_REG(fr, %d), AOT_REG(fr, %d), "
          "&_o)) {\n",
          sub, a, b);
      fprintf(
          f, "          AOT_SET_B(ctx, fr, %d, _o); goto pc_%d; } }\n", d,
          next);
      emit_fallback(f, off, next, last, 1);
      return 1;

    /* get_field / get_field_own / get_field2 / get_field2_own: own_read_hit.
     get_field2 is the "load a method for call_method" form, so it also tries
     the folded-prototype read first. */
    case ROP_get_field:
    case ROP_get_field_own:
    case ROP_get_field2:
    case ROP_get_field2_own:
      d = (int)in->arg[0].val;
      a = (int)in->arg[1].val;
      site = (int)in->arg[3].val;
      /* No site (a baseline-tier function has no PIC table): the step does the
         generic read, as the interpreter handler does. */
      if ((unsigned)site == REG_IC_NONE) return 0;
      if (in->op == ROP_get_field2 || in->op == ROP_get_field2_own) {
        int sid = g_site_next++;
        fprintf(f, "    { JSValue _o;\n");
        fprintf(
            f,
            "      if (qjs_aot_proto_fold(ctx, f, pic, %d, AOT_REG(fr, %d), "
            "%d, &_o)) {\n",
            sid, a, site);
        fprintf(
            f,
            "          qjs_aot_set_reg_p(ctx, fr, %d, _o); goto pc_%d; } }\n",
            d, next);
      }
      fprintf(f, "    { JSValue _o;\n");
      fprintf(
          f,
          "      if (qjs_aot_own_read_hit(f, pic, AOT_REG(fr, %d), %d, &_o)) "
          "{\n",
          a, site);
      fprintf(
          f, "          AOT_SET_B(ctx, fr, %d, _o); goto pc_%d; } }\n", d,
          next);
      emit_fallback(f, off, next, last, 1);
      return 1;

    /* get_this_field / get_this_field2: the fused `this` load and the read
       that consumes it, exactly as the interpreter's fused handler: the load
       writes the receiver register and the read then tests the PIC. The
       load's register write is repeated by the step when the read misses, so
       it has no lasting effect. */
    case ROP_get_this_field:
    case ROP_get_this_field2: {
      int rr = (int)in->arg[1].val;
      d = (int)in->arg[0].val;
      site = (int)in->arg[3].val;
      if ((unsigned)site == REG_IC_NONE) return 0;
      fprintf(
          f, "    { JSValue _o; int _r = qjs_aot_load_this(ctx, f, &_o);\n");
      fprintf(f, "      if (_r < 0) goto exception;\n");
      fprintf(f, "      if (_r) qjs_aot_set_reg_p(ctx, fr, %d, _o);\n", rr);
      fprintf(f, "      else AOT_SET_B(ctx, fr, %d, _o);\n", rr);
      if (in->op == ROP_get_this_field2) {
        int sid = g_site_next++;
        fprintf(
            f,
            "      if (qjs_aot_proto_fold(ctx, f, pic, %d, AOT_REG(fr, %d), "
            "%d, &_o)) {\n",
            sid, rr, site);
        fprintf(
            f, "          qjs_aot_set_reg_p(ctx, fr, %d, _o); goto pc_%d; }\n",
            d, next);
      }
      fprintf(
          f,
          "      if (qjs_aot_own_read_hit(f, pic, AOT_REG(fr, %d), %d, &_o)) "
          "{\n",
          rr, site);
      fprintf(
          f, "          AOT_SET_B(ctx, fr, %d, _o); goto pc_%d; } }\n", d,
          next);
      emit_fallback(f, off, next, last, 1);
      return 1;
    }

    /* call / call_method: the specialised C-call path when the profile shows
     a C callee at this pc; the direct compiled path when it shows a bytecode
     callee; otherwise (and on a miss) the step engine. */
    case ROP_call:
    case ROP_call_method: {
      const ProfCall *pcl = prof_call_at(g_cur_prof, next);
      if (pcl && pcl->kind == 2) {
        int is_m = in->op == ROP_call_method;
        int sid = g_site_next++;
        int dc = (int)in->arg[0].val;
        int cc = (int)in->arg[1].val;
        int rc = is_m ? (int)in->arg[2].val : -1;
        int abc = (int)in->arg[is_m ? 3 : 2].val;
        int nac = (int)in->arg[is_m ? 4 : 3].val;
        fprintf(f, "    { JSValue _o;\n");
        fprintf(
            f,
            "      int _h = qjs_aot_call_c(ctx, %d, %d, %d, AOT_REG(fr, %d), ",
            sid, pcl->cproto, nac, cc);
        if (is_m)
          fprintf(f, "AOT_REG(fr, %d), ", rc);
        else
          fprintf(f, "JS_UNDEFINED, ");
        fprintf(f, "&AOT_REG(fr, %d), &_o);\n", abc);
        fprintf(
            f,
            "      if (_h > 0) { qjs_aot_set_reg_p(ctx, fr, %d, _o); goto "
            "pc_%d; }\n",
            dc, next);
        fprintf(f, "      if (_h < 0) { pc = %d; goto exception; } }\n", off);
      } else if (pcl && pcl->kind == 1) {
        int is_m = in->op == ROP_call_method;
        int dc = (int)in->arg[0].val;
        int cc = (int)in->arg[1].val;
        int rc = is_m ? (int)in->arg[2].val : -1;
        int abc = (int)in->arg[is_m ? 3 : 2].val;
        int nac = (int)in->arg[is_m ? 4 : 3].val;
        fprintf(f, "    { JSValue _o;\n");
        fprintf(
            f, "      int _h = qjs_aot_call_b(ctx, %d, AOT_REG(fr, %d), ", nac,
            cc);
        if (is_m)
          fprintf(f, "AOT_REG(fr, %d), ", rc);
        else
          fprintf(f, "JS_UNDEFINED, ");
        fprintf(f, "&AOT_REG(fr, %d), &_o);\n", abc);
        fprintf(
            f,
            "      if (_h > 0) { qjs_aot_set_reg_p(ctx, fr, %d, _o); goto "
            "pc_%d; }\n",
            dc, next);
        fprintf(f, "      if (_h < 0) { pc = %d; goto exception; } }\n", off);
      }
      emit_fallback(f, off, next, last, 1);
      return 1;
    }

    /* move/move_s: SET_REG_B(d, regs[s]) unless d == s (self-move is a no-op). */
    case ROP_move:
    case ROP_move_s:
      d = (int)in->arg[0].val;
      a = (int)in->arg[1].val;
      if (d == a)
        fprintf(f, "    goto pc_%d;\n", next);
      else
        fprintf(
            f,
            "    AOT_SET_B(ctx, fr, %d, AOT_REG(fr, %d));\n    goto pc_%d;\n",
            d, a, next);
      return 1;

    /* load_i8 / load_i8_s / load_i32: SET_REG_P(d, constant int). */
    case ROP_load_i8:
    case ROP_load_i8_s:
      d = (int)in->arg[0].val;
      fprintf(
          f,
          "    qjs_aot_set_reg_p(ctx, fr, %d, JS_MKVAL(JS_TAG_INT, %d));\n"
          "    goto pc_%d;\n",
          d, (int)(int8_t)in->arg[1].val, next);
      return 1;
    case ROP_load_i32:
      d = (int)in->arg[0].val;
      fprintf(
          f,
          "    qjs_aot_set_reg_p(ctx, fr, %d, JS_MKVAL(JS_TAG_INT, "
          "(int32_t)%d));\n"
          "    goto pc_%d;\n",
          d, (int32_t)in->arg[1].val, next);
      return 1;

    /* get_arg: SET_REG_B(d, arg_buf[slot]). */
    case ROP_get_arg:
      d = (int)in->arg[0].val;
      fprintf(
          f, "    AOT_SET_B(ctx, fr, %d, ab[%d]);\n    goto pc_%d;\n", d,
          (int)in->arg[1].val, next);
      return 1;

    /* get_arg2: two argument slots into their registers. */
    case ROP_get_arg2:
      d = (int)in->arg[0].val;
      a = (int)in->arg[1].val;
      b = (int)in->arg[2].val;
      site = (int)in->arg[3].val;
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, ab[%d]);\n", d, a);
      fprintf(f, "    AOT_SET_B(ctx, fr, %d, ab[%d]);\n", b, site);
      fprintf(f, "    goto pc_%d;\n", next);
      return 1;

    /* put_local / put_local_s0..s3: set_value(ctx, &var_buf[slot], dup(regs[s])). */
    case ROP_put_local:
      fprintf(
          f,
          "    qjs_aot_put_local(ctx, vb, %d, AOT_REG(fr, %d));\n    goto "
          "pc_%d;\n",
          (int)in->arg[0].val, (int)in->arg[1].val, next);
      return 1;
    case ROP_put_local_s0:
    case ROP_put_local_s1:
    case ROP_put_local_s2:
    case ROP_put_local_s3:
      fprintf(
          f,
          "    qjs_aot_put_local(ctx, vb, %d, AOT_REG(fr, %d));\n    goto "
          "pc_%d;\n",
          in->op - ROP_put_local_s0, (int)in->arg[0].val, next);
      return 1;

    /* get_global: the handler's per-instruction site cache, then the cell
     cache, then JS_GetGlobalVar. The atom is runtime-remapped, so re-read it
     from the live code rather than baking the generator's atom id. A site hit
     borrows the slot's value; the fallbacks return an owned one. */
    case ROP_get_global:
      d = (int)in->arg[0].val;
      site = (int)in->arg[3].val;
      /* No site (a baseline-tier function has no PIC table): the step does the
         generic read, as the interpreter handler does. */
      if ((unsigned)site == REG_IC_NONE) return 0;
      fprintf(f, "    { JSValue _o; int _r =\n");
      fprintf(
          f,
          "        qjs_aot_get_global_site(ctx, f, %d, "
          "(int)qjs_aot_ld_u32(code + %d), %d, &_o);\n",
          site, in->arg[1].off, (int)in->arg[2].val);
      fprintf(f, "      if (_r < 0) goto exception;\n");
      fprintf(f, "      if (_r) qjs_aot_set_reg_p(ctx, fr, %d, _o);\n", d);
      fprintf(f, "      else AOT_SET_B(ctx, fr, %d, _o);\n", d);
      fprintf(f, "      goto pc_%d; }\n", next);
      return 1;

    /* put_field: the REG_PIC_OWN_W direct-slot store, else the step. */
    case ROP_put_field:
      a = (int)in->arg[0].val;
      site = (int)in->arg[2].val;
      b = (int)in->arg[3].val;
      if ((unsigned)site == REG_IC_NONE) return 0;
      fprintf(
          f,
          "    if (qjs_aot_put_field_own(ctx, f, AOT_REG(fr, %d), %d, "
          "AOT_REG(fr, %d)))\n"
          "        goto pc_%d;\n",
          a, site, b, next);
      emit_fallback(f, off, next, last, 1);
      return 1;

    /* get_element: the plain-array fast path, else the step. */
    case ROP_get_element:
      d = (int)in->arg[0].val;
      a = (int)in->arg[1].val;
      b = (int)in->arg[2].val;
      fprintf(f, "    { JSValue _o;\n");
      fprintf(
          f,
          "      if (qjs_aot_get_element(AOT_REG(fr, %d), AOT_REG(fr, %d), "
          "&_o)) {\n",
          a, b);
      fprintf(
          f, "          qjs_aot_set_reg_p(ctx, fr, %d, _o); goto pc_%d; } }\n",
          d, next);
      emit_fallback(f, off, next, last, 1);
      return 1;

    /* array_const: a fresh array from the function's constant-pool template. */
    case ROP_array_const:
      d = (int)in->arg[0].val;
      fprintf(f, "    { JSValue _o;\n");
      fprintf(
          f,
          "      if (qjs_aot_array_const(ctx, f, %du, &_o) < 0) goto "
          "exception;\n",
          (uint32_t)in->arg[1].val);
      fprintf(f, "      qjs_aot_set_reg_p(ctx, fr, %d, _o);\n", d);
      fprintf(f, "      goto pc_%d; }\n", next);
      return 1;

    /* Fused multiply-add family: mul then add/sub, exactly as the handler. */
    case ROP_mul_add:
    case ROP_mul_sub:
    case ROP_add_mul:
    case ROP_sub_mul: {
      int dm = (int)in->arg[0].val, x = (int)in->arg[1].val,
          y = (int)in->arg[2].val;
      int da = (int)in->arg[3].val, c = (int)in->arg[4].val;
      int mul_first = (in->op == ROP_mul_add || in->op == ROP_mul_sub);
      int sub2 = (in->op == ROP_mul_add || in->op == ROP_add_mul) ? REG_BIN_ADD
                                                                  : REG_BIN_SUB;
      int lhs2 = mul_first ? dm : c;
      int rhs2 = mul_first ? c : dm;
      fprintf(f, "    { JSValue _t, _r;\n");
      fprintf(
          f,
          "      if (!qjs_aot_num_fast(%d, AOT_REG(fr, %d), AOT_REG(fr, %d), "
          "&_t) &&\n",
          REG_BIN_MUL, x, y);
      fprintf(
          f,
          "          qjs_aot_binary(ctx, %d, AOT_REG(fr, %d), AOT_REG(fr, %d), "
          "&_t))\n",
          REG_BIN_MUL, x, y);
      fprintf(f, "          goto exception;\n");
      fprintf(f, "      qjs_aot_set_reg_p(ctx, fr, %d, _t);\n", dm);
      fprintf(
          f,
          "      if (!qjs_aot_num_fast(%d, AOT_REG(fr, %d), AOT_REG(fr, %d), "
          "&_r) &&\n",
          sub2, lhs2, rhs2);
      fprintf(
          f,
          "          qjs_aot_binary(ctx, %d, AOT_REG(fr, %d), AOT_REG(fr, %d), "
          "&_r))\n",
          sub2, lhs2, rhs2);
      fprintf(f, "          goto exception;\n");
      fprintf(
          f, "      qjs_aot_set_reg_p(ctx, fr, %d, _r); goto pc_%d; }\n", da,
          next);
      return 1;
    }

    /* get_field_chain / get_field_chain_own: two own_read_hit fast paths. Both
     hits are tested before either register is written, so a miss leaves the
     registers untouched and the step fallback starts from a clean state. */
    case ROP_get_field_chain:
    case ROP_get_field_chain_own: {
      int d1 = (int)in->arg[0].val, r1 = (int)in->arg[1].val;
      int s1 = (int)in->arg[3].val, d2 = (int)in->arg[4].val;
      int s2 = (int)in->arg[7].val;
      if (s1 == REG_IC_NONE || s2 == REG_IC_NONE) return 0;
      fprintf(f, "    { JSValue _v1, _v2;\n");
      fprintf(
          f,
          "      if (qjs_aot_own_read_hit(f, pic, AOT_REG(fr, %d), %d, &_v1) "
          "&&\n",
          r1, s1);
      fprintf(
          f, "          qjs_aot_own_read_hit(f, pic, _v1, %d, &_v2)) {\n", s2);
      fprintf(f, "          AOT_SET_B(ctx, fr, %d, _v1);\n", d1);
      fprintf(
          f, "          AOT_SET_B(ctx, fr, %d, _v2); goto pc_%d; } }\n", d2,
          next);
      emit_fallback(f, off, next, last, 1);
      return 1;
    }

    /* branch_cmp_reg_reg / _s: the int/float compare, else the step. */
    case ROP_branch_cmp_reg_reg:
    case ROP_branch_cmp_reg_reg_s: {
      int s = (int)in->arg[0].val;
      int la = (int)in->arg[1].val, rb = (int)in->arg[2].val;
      int tgt = next + (int)in->arg[3].val;
      fprintf(f, "    { int _t;\n");
      fprintf(
          f,
          "      if (qjs_aot_branch_cmp(%d, AOT_REG(fr, %d), AOT_REG(fr, %d), "
          "&_t)) {\n",
          s, la, rb);
      fprintf(
          f, "          if (_t) goto pc_%d; else goto pc_%d; } }\n", tgt, next);
      emit_fallback(f, off, next, last, 1);
      return 1;
    }

    /* get_var_ref: SET_REG_B(d, *var_refs[slot]->pvalue). */
    case ROP_get_var_ref:
      d = (int)in->arg[0].val;
      fprintf(
          f,
          "    AOT_SET_B(ctx, fr, %d, qjs_aot_get_var_ref(f, %d));\n    goto "
          "pc_%d;\n",
          d, (int)in->arg[1].val, next);
      return 1;

    /* load_this: SET_REG_B for a borrowed this, SET_REG_P when boxed. */
    case ROP_load_this:
      d = (int)in->arg[0].val;
      fprintf(
          f, "    { JSValue _o; int _r = qjs_aot_load_this(ctx, f, &_o);\n");
      fprintf(f, "      if (_r < 0) goto exception;\n");
      fprintf(f, "      if (_r) qjs_aot_set_reg_p(ctx, fr, %d, _o);\n", d);
      fprintf(f, "      else AOT_SET_B(ctx, fr, %d, _o);\n", d);
      fprintf(f, "      goto pc_%d; }\n", next);
      return 1;

    /* move_xfer: move d<-s then clear s (self-move is a no-op). */
    case ROP_move_xfer:
      d = (int)in->arg[0].val;
      a = (int)in->arg[1].val;
      if (d == a)
        fprintf(f, "    goto pc_%d;\n", next);
      else
        fprintf(
            f, "    qjs_aot_move_xfer(ctx, fr, %d, %d);\n    goto pc_%d;\n", d,
            a, next);
      return 1;

    /* post_inc / post_dec: the converted old value into the first register
       and the new value into the second, exactly as the interpreter's fused
       pair (to_numeric, then inc/dec, both with the ordinary slow paths). */
    case ROP_post_inc:
    case ROP_post_dec: {
      int sub2 = in->op == ROP_post_inc ? REG_UN_INC : REG_UN_DEC;
      int t = (int)in->arg[0].val, s = (int)in->arg[1].val,
          h = (int)in->arg[2].val;
      fprintf(f, "    { JSValue _o;\n");
      fprintf(
          f,
          "      if (!qjs_aot_unary_num_fast(%d, AOT_REG(fr, %d), &_o) &&\n"
          "          qjs_aot_unary(ctx, %d, AOT_REG(fr, %d), &_o))\n"
          "          goto exception;\n",
          REG_UN_TO_NUMERIC, s, REG_UN_TO_NUMERIC, s);
      fprintf(f, "      qjs_aot_set_reg_p(ctx, fr, %d, _o);\n", t);
      fprintf(
          f,
          "      if (!qjs_aot_unary_num_fast(%d, AOT_REG(fr, %d), &_o) &&\n"
          "          qjs_aot_unary(ctx, %d, AOT_REG(fr, %d), &_o))\n"
          "          goto exception;\n",
          sub2, t, sub2, t);
      fprintf(
          f, "      qjs_aot_set_reg_p(ctx, fr, %d, _o); goto pc_%d; }\n", h,
          next);
      return 1;
    }

    /* load_undefined / load_null / load_true / load_false: SET_REG_P(d, const). */
    case ROP_load_undefined:
    case ROP_load_null:
    case ROP_load_true:
    case ROP_load_false:
      d = (int)in->arg[0].val;
      fprintf(
          f, "    qjs_aot_set_reg_p(ctx, fr, %d, %s);\n    goto pc_%d;\n", d,
          in->op == ROP_load_undefined ? "JS_UNDEFINED"
          : in->op == ROP_load_null    ? "JS_NULL"
          : in->op == ROP_load_true    ? "JS_TRUE"
                                       : "JS_FALSE",
          next);
      return 1;

    /* binary_imm / binary_imm_s: regvm_num_fast with an int immediate. */
    case ROP_binary_imm:
    case ROP_binary_imm_s: {
      int32_t imm = (int32_t)in->arg[3].val;
      d = (int)in->arg[0].val;
      sub = (int)in->arg[1].val;
      a = (int)in->arg[2].val;
      fprintf(f, "    { JSValue _o;\n");
      fprintf(
          f,
          "      if (!qjs_aot_num_fast(%d, AOT_REG(fr, %d), "
          "JS_MKVAL(JS_TAG_INT, %d), &_o) &&\n",
          sub, a, (int)imm);
      fprintf(
          f,
          "          qjs_aot_binary(ctx, %d, AOT_REG(fr, %d), "
          "JS_MKVAL(JS_TAG_INT, %d), &_o))\n",
          sub, a, (int)imm);
      fprintf(f, "          goto exception;\n");
      fprintf(
          f, "      qjs_aot_set_reg_p(ctx, fr, %d, _o); goto pc_%d; }\n", d,
          next);
      return 1;
    }

    /* branch / branch_s: always taken. */
    case ROP_branch:
    case ROP_branch_s:
      emit_branch(f, off, next + (int)in->arg[0].val);
      return 1;

    /* branch_if / branch_if_not: regvm_to_bool, else the step. */
    case ROP_branch_if:
    case ROP_branch_if_not: {
      int want_true = (in->op == ROP_branch_if);
      int tgt = next + (int)in->arg[1].val;
      a = (int)in->arg[0].val;
      fprintf(
          f, "    if ((qjs_aot_to_bool(ctx, AOT_REG(fr, %d)) != 0) == %d) {\n",
          a, want_true);
      emit_branch(f, off, tgt);
      fprintf(f, "    } else goto pc_%d;\n", next);
      return 1;
    }

    /* branch_null / branch_not_null: one tag test, no fallback. */
    case ROP_branch_null:
    case ROP_branch_not_null: {
      int want_null = (in->op == ROP_branch_null);
      int tgt = next + (int)in->arg[1].val;
      a = (int)in->arg[0].val;
      fprintf(
          f,
          "    if ((JS_VALUE_GET_TAG(AOT_REG(fr, %d)) == JS_TAG_NULL) == %d) "
          "{\n",
          a, want_null);
      emit_branch(f, off, tgt);
      fprintf(f, "    } else goto pc_%d;\n", next);
      return 1;
    }

    /* branch_nullish / branch_not_nullish: null and undefined by tag; the
       IsHTMLDDA case asks the engine. No fallback. */
    case ROP_branch_nullish:
    case ROP_branch_not_nullish: {
      int want_nullish = (in->op == ROP_branch_nullish);
      int tgt = next + (int)in->arg[1].val;
      a = (int)in->arg[0].val;
      fprintf(
          f,
          "    { JSValue _v = AOT_REG(fr, %d); uint32_t _t = "
          "JS_VALUE_GET_TAG(_v);\n",
          a);
      fprintf(
          f,
          "      int _n = (_t == JS_TAG_NULL || _t == JS_TAG_UNDEFINED) ||\n"
          "                (_t == JS_TAG_OBJECT && qjs_aot_nullish(_v));\n");
      fprintf(f, "      if (_n == %d) {\n", want_nullish);
      emit_branch(f, off, tgt);
      fprintf(f, "      } else goto pc_%d; }\n", next);
      return 1;
    }

    default:
      sub = branch_subop(in->op);
      if (sub >= 0) {
        /* The per-sense short branches: the int/float compare, else step. */
        a = (int)in->arg[0].val;
        b = (int)in->arg[1].val;
        int tgt = next + (int)in->arg[2].val;
        fprintf(f, "    { int _t;\n");
        fprintf(
            f,
            "      if (qjs_aot_branch_cmp(%d, AOT_REG(fr, %d), AOT_REG(fr, "
            "%d), &_t)) {\n",
            sub, a, b);
        fprintf(f, "          if (_t) {\n");
        emit_branch(f, off, tgt);
        fprintf(f, "          } else goto pc_%d; } }\n", next);
        emit_fallback(f, off, next, last, 1);
        return 1;
      }
      sub = binop_subop(in->op);
      if (sub >= 0) {
        /* The remaining "rrr" binary ops: REGVM_BIN_OP. */
        d = (int)in->arg[0].val;
        a = (int)in->arg[1].val;
        b = (int)in->arg[2].val;
        fprintf(f, "    { JSValue _o;\n");
        fprintf(
            f,
            "      if (!qjs_aot_num_fast(%d, AOT_REG(fr, %d), AOT_REG(fr, %d), "
            "&_o) &&\n",
            sub, a, b);
        fprintf(
            f,
            "          qjs_aot_binary(ctx, %d, AOT_REG(fr, %d), AOT_REG(fr, "
            "%d), &_o))\n",
            sub, a, b);
        fprintf(f, "          goto exception;\n");
        fprintf(
            f, "      qjs_aot_set_reg_p(ctx, fr, %d, _o); goto pc_%d; }\n", d,
            next);
        return 1;
      }
      sub = unop_subop(in->op);
      if (sub >= 0) {
        /* The unary ops: REGVM_UN_OP. */
        d = (int)in->arg[0].val;
        a = (int)in->arg[1].val;
        fprintf(f, "    { JSValue _o;\n");
        fprintf(
            f,
            "      if (!qjs_aot_unary_num_fast(%d, AOT_REG(fr, %d), &_o) &&\n",
            sub, a);
        fprintf(
            f, "          qjs_aot_unary(ctx, %d, AOT_REG(fr, %d), &_o))\n", sub,
            a);
        fprintf(f, "          goto exception;\n");
        fprintf(
            f, "      qjs_aot_set_reg_p(ctx, fr, %d, _o); goto pc_%d; }\n", d,
            next);
        return 1;
      }
      return 0;
  }
}

/* ========================= Layer B typed regions =========================
 *
 * A "region" is a single-entry back-edge loop whose body only reads numbers
 * and dense array elements. The loop-carried var slots and the numeric
 * registers are kept unboxed in C locals (a double plus an is-int flag); the
 * loop bound and the array bases are guarded once at entry. Any value that is
 * not provably numeric, and any op outside the whitelist, deopts: the
 * unboxed values are boxed back into the register file / var buffer and
 * control jumps to the Layer A label for the same pc, which re-executes the
 * instruction with full interpreter semantics.
 *
 * The region does no allocation and makes no calls, so it can never trigger a
 * GC while a value is held only as a double; that is what makes the unboxed
 * representation sound without a safepoint.
 */

#include <stdarg.h>

#define RG_NONE 0
#define RG_NUM 1
#define RG_OBJ 2
#define RG_BOX 3
#define RG_MIX 4

typedef struct {
  const JSAotGenFunc *g;
  const JSAotGenInsn *ins;
  const int *offs, *nxts;
  int h, back, n; /* instruction indices: head .. back-edge */
  int head_pc, exit_pc;
  int rn;                           /* number of register slots (off>>4 + 1) */
  int sn;                           /* number of var slots */
  unsigned char *role_r, *role_s;   /* how each value is read */
  unsigned char *prod_r, *prod_s;   /* how each value is written */
  unsigned char *live_r, *live_s;   /* read before first write in the region */
  unsigned char *read_r, *read_s;   /* read anywhere in the region */
  unsigned char *unb_r, *unb_s;     /* currently unboxed during emission */
  unsigned char *entry_r, *entry_s; /* unboxed set at the loop head */
  unsigned char *elem_r;            /* register used as an element base */
  unsigned char *elem_iv_r;         /* element base read with the IV as key */
  int iv_reg;                 /* induction-variable register (-1 = none) */
  int bound_isreg, bound_off; /* loop-test bound operand */
  int pic_count;
  int *fguard_recv; /* PIC site -> receiver register (-1 = none) */
  int *vref_slot;   /* register -> captured-var slot (-1 = none) */
  FILE *f;
  int dry;
} RgCtx;

static int rg_ri(int off) {
  return off >> 4;
}

static void rg_out(RgCtx *c, const char *fmt, ...) {
  va_list ap;
  if (c->dry) return;
  va_start(ap, fmt);
  vfprintf(c->f, fmt, ap);
  va_end(ap);
}

/* Record how a value is read / written; a disagreement is RG_MIX (reject). */
static void rg_role(unsigned char *role, int idx, int r) {
  if (idx < 0) return;
  if (role[idx] == RG_NONE)
    role[idx] = (unsigned char)r;
  else if (role[idx] != r)
    role[idx] = RG_MIX;
}

/* A numeric value must be unboxed at region entry when it is read before its
   first write (a loop-carried input) OR when it is written in-region but never
   read there (a "sink" — a live-out variable such as `for (...) s = expr`). A
   sink has no in-region reader to keep its local initialised, so it must be
   seeded from the entry value and boxed back at the exit; otherwise a
   zero-iteration exit would drop the last write (and a >=1-iteration exit would
   leave the variable stale). */
static int rg_entry_unbox(RgCtx *c, int i, int isreg) {
  unsigned char *role = isreg ? c->role_r : c->role_s;
  unsigned char *prod = isreg ? c->prod_r : c->prod_s;
  unsigned char *live = isreg ? c->live_r : c->live_s;
  unsigned char *rd = isreg ? c->read_r : c->read_s;
  if (role[i] != RG_NUM) return 0;
  return live[i] || (prod[i] != RG_NONE && !rd[i]);
}

/* Allocate the analysis arrays for a candidate loop. */
static RgCtx *rg_new(
    const JSAotGenFunc *g, const JSAotGenInsn *ins, const int *offs,
    const int *nxts, int n, int h, int back, int exit_pc) {
  RgCtx *c = (RgCtx *)calloc(1, sizeof(*c));
  int i, maxoff = 0;
  if (!c) return NULL;
  c->g = g;
  c->ins = ins;
  c->offs = offs;
  c->nxts = nxts;
  c->h = h;
  c->back = back;
  c->n = n;
  c->head_pc = offs[h];
  c->exit_pc = exit_pc;
  for (i = h; i <= back; i++) {
    int k;
    for (k = 0; k < c->ins[i].nargs; k++)
      if (c->ins[i].arg[k].kind == 'r' && (int)c->ins[i].arg[k].val > maxoff)
        maxoff = (int)c->ins[i].arg[k].val;
  }
  c->rn = maxoff / 16 + 1;
  c->sn = g->var_count > 0 ? g->var_count : 1;
  c->role_r = (unsigned char *)calloc((size_t)c->rn, 1);
  c->role_s = (unsigned char *)calloc((size_t)c->sn, 1);
  c->prod_r = (unsigned char *)calloc((size_t)c->rn, 1);
  c->prod_s = (unsigned char *)calloc((size_t)c->sn, 1);
  c->live_r = (unsigned char *)calloc((size_t)c->rn, 1);
  c->live_s = (unsigned char *)calloc((size_t)c->sn, 1);
  c->read_r = (unsigned char *)calloc((size_t)c->rn, 1);
  c->read_s = (unsigned char *)calloc((size_t)c->sn, 1);
  c->unb_r = (unsigned char *)calloc((size_t)c->rn, 1);
  c->unb_s = (unsigned char *)calloc((size_t)c->sn, 1);
  c->entry_r = (unsigned char *)calloc((size_t)c->rn, 1);
  c->entry_s = (unsigned char *)calloc((size_t)c->sn, 1);
  c->elem_r = (unsigned char *)calloc((size_t)c->rn, 1);
  c->elem_iv_r = (unsigned char *)calloc((size_t)c->rn, 1);
  c->iv_reg = -1;
  c->bound_isreg = -1;
  c->bound_off = 0;
  c->pic_count = g->pic_count;
  c->fguard_recv = (int *)malloc(
      sizeof(int) * (size_t)(c->pic_count > 0 ? c->pic_count : 1));
  if (c->fguard_recv) {
    int j;
    for (j = 0; j < c->pic_count; j++) c->fguard_recv[j] = -1;
  }
  c->vref_slot = (int *)malloc(sizeof(int) * (size_t)c->rn);
  if (c->vref_slot) {
    int j;
    for (j = 0; j < c->rn; j++) c->vref_slot[j] = -1;
  }
  if (!c->role_r || !c->role_s || !c->prod_r || !c->prod_s || !c->live_r ||
      !c->live_s || !c->read_r || !c->read_s || !c->unb_r || !c->unb_s ||
      !c->entry_r || !c->entry_s || !c->elem_r || !c->elem_iv_r ||
      !c->fguard_recv || !c->vref_slot) {
    free(c->role_r);
    free(c->role_s);
    free(c->prod_r);
    free(c->prod_s);
    free(c->live_r);
    free(c->live_s);
    free(c->read_r);
    free(c->read_s);
    free(c->unb_r);
    free(c->unb_s);
    free(c->entry_r);
    free(c->entry_s);
    free(c->elem_r);
    free(c->elem_iv_r);
    free(c->fguard_recv);
    free(c->vref_slot);
    free(c);
    return NULL;
  }
  return c;
}

static void rg_free(RgCtx *c) {
  if (!c) return;
  free(c->role_r);
  free(c->role_s);
  free(c->prod_r);
  free(c->prod_s);
  free(c->live_r);
  free(c->live_s);
  free(c->read_r);
  free(c->read_s);
  free(c->unb_r);
  free(c->unb_s);
  free(c->entry_r);
  free(c->entry_s);
  free(c->elem_r);
  free(c->elem_iv_r);
  free(c->fguard_recv);
  free(c->vref_slot);
  free(c);
}

/* Classify a value read by an op. */
static void rg_read(RgCtx *c, int isreg, int off, int role) {
  int idx = isreg ? rg_ri(off) : off;
  unsigned char *role_a = isreg ? c->role_r : c->role_s;
  unsigned char *live_a = isreg ? c->live_r : c->live_s;
  unsigned char *read_a = isreg ? c->read_r : c->read_s;
  unsigned char *prod_a = isreg ? c->prod_r : c->prod_s;
  if (idx < 0 || (isreg ? idx >= c->rn : idx >= c->sn)) return;
  read_a[idx] = 1;
  rg_role(role_a, idx, role);
  if (prod_a[idx] == RG_NONE)
    live_a[idx] = 1; /* read before any write in the region */
}

/* Mark a value produced by an op. */
static void rg_write(RgCtx *c, int isreg, int off, int prod) {
  int idx = isreg ? rg_ri(off) : off;
  unsigned char *prod_a = isreg ? c->prod_r : c->prod_s;
  if (idx < 0 || (isreg ? idx >= c->rn : idx >= c->sn)) return;
  /* A register written inside the region must carry a role even when it is
     never read in-region (a write-only / live-out temporary): otherwise it is
     neither declared nor boxed, yet rg_numop still emits `u_/k_` for it. */
  rg_role(isreg ? c->role_r : c->role_s, idx, prod);
  if (prod_a[idx] == RG_NONE)
    prod_a[idx] = (unsigned char)prod;
  else if (prod_a[idx] != prod)
    prod_a[idx] = RG_MIX;
}

/* Compute roles / producers / liveness for a candidate region. Returns 0 if
   the region contains an op outside the whitelist. */
static int rg_scan(RgCtx *c) {
  int i;
  for (i = c->h; i <= c->back; i++) {
    const JSAotGenInsn *in = &c->ins[i];
    switch (in->op) {
      case ROP_local_check:
      case ROP_local_check_init:
        break;

      case ROP_get_local:
        rg_read(
            c, 0, (int)in->arg[1].val, RG_NUM); /* provisional: fixed below */
        rg_write(c, 1, (int)in->arg[0].val, RG_NUM);
        break;
      case ROP_get_local_s0:
      case ROP_get_local_s1:
      case ROP_get_local_s2:
      case ROP_get_local_s3:
        rg_read(c, 0, in->op - ROP_get_local_s0, RG_NUM);
        rg_write(c, 1, (int)in->arg[0].val, RG_NUM);
        break;

      case ROP_put_local:
        rg_read(c, 1, (int)in->arg[1].val, RG_NUM);
        rg_write(c, 0, (int)in->arg[0].val, RG_NUM);
        break;
      case ROP_put_local_s0:
      case ROP_put_local_s1:
      case ROP_put_local_s2:
      case ROP_put_local_s3:
        rg_read(c, 1, (int)in->arg[0].val, RG_NUM);
        rg_write(c, 0, in->op - ROP_put_local_s0, RG_NUM);
        break;
      case ROP_put_local_xfer:
        rg_read(c, 1, (int)in->arg[1].val, RG_NUM);
        rg_write(c, 0, (int)in->arg[0].val, RG_NUM);
        break;
      case ROP_put_local_xfer_s0:
      case ROP_put_local_xfer_s1:
      case ROP_put_local_xfer_s2:
      case ROP_put_local_xfer_s3:
        rg_read(c, 1, (int)in->arg[0].val, RG_NUM);
        rg_write(c, 0, in->op - ROP_put_local_xfer_s0, RG_NUM);
        break;

      case ROP_get_element:
        rg_read(c, 1, (int)in->arg[1].val, RG_OBJ);
        rg_read(c, 1, (int)in->arg[2].val, RG_NUM);
        rg_write(c, 1, (int)in->arg[0].val, RG_NUM);
        {
          int bi = rg_ri((int)in->arg[1].val);
          if (bi >= 0 && bi < c->rn) c->elem_r[bi] = 1;
        }
        break;

      case ROP_get_field:
      case ROP_get_field2: {
        int d = (int)in->arg[0].val, r = (int)in->arg[1].val;
        int site = (int)in->arg[3].val;
        rg_read(c, 1, r, RG_OBJ);
        rg_write(c, 1, d, RG_NUM);
        if (site >= 0 && site < c->pic_count) {
          if (c->fguard_recv[site] < 0)
            c->fguard_recv[site] = r;
          else if (c->fguard_recv[site] != r)
            return 0; /* same site, different receiver: cannot hoist */
        } else {
          return 0; /* no PIC site: cannot hoist the guard */
        }
        break;
      }

      case ROP_get_length:
        rg_read(c, 1, (int)in->arg[1].val, RG_OBJ);
        rg_write(c, 1, (int)in->arg[0].val, RG_NUM);
        break;

      case ROP_get_var_ref:
        rg_write(c, 1, (int)in->arg[0].val, RG_NUM);
        break;

      case ROP_add_mul:
      case ROP_mul_add:
      case ROP_mul_sub:
      case ROP_sub_mul:
        rg_read(c, 1, (int)in->arg[1].val, RG_NUM);
        rg_read(c, 1, (int)in->arg[2].val, RG_NUM);
        rg_read(c, 1, (int)in->arg[4].val, RG_NUM);
        rg_write(c, 1, (int)in->arg[0].val, RG_NUM); /* dm */
        rg_write(c, 1, (int)in->arg[3].val, RG_NUM); /* da */
        break;

      case ROP_eq:
      case ROP_lt:
      case ROP_lte:
      case ROP_gt:
      case ROP_gte:
      case ROP_neq:
      case ROP_strict_eq:
      case ROP_strict_neq:
        rg_read(c, 1, (int)in->arg[1].val, RG_NUM);
        rg_read(c, 1, (int)in->arg[2].val, RG_NUM);
        rg_write(c, 1, (int)in->arg[0].val, RG_BOX);
        break;

      case ROP_mul:
      case ROP_add:
      case ROP_sub:
        rg_read(c, 1, (int)in->arg[1].val, RG_NUM);
        rg_read(c, 1, (int)in->arg[2].val, RG_NUM);
        rg_write(c, 1, (int)in->arg[0].val, RG_NUM);
        break;
      case ROP_binary_loc_reg:
        rg_read(c, 0, (int)in->arg[2].val, RG_NUM);
        rg_read(c, 1, (int)in->arg[3].val, RG_NUM);
        rg_write(c, 1, (int)in->arg[0].val, RG_NUM);
        break;
      case ROP_binary_loc_loc:
        rg_read(c, 0, (int)in->arg[2].val, RG_NUM);
        rg_read(c, 0, (int)in->arg[3].val, RG_NUM);
        rg_write(c, 1, (int)in->arg[0].val, RG_NUM);
        break;
      case ROP_binary_imm:
      case ROP_binary_imm_s:
        rg_read(c, 1, (int)in->arg[2].val, RG_NUM);
        rg_write(c, 1, (int)in->arg[0].val, RG_NUM);
        break;
      case ROP_add_imm_s:
      case ROP_sub_imm_s:
        rg_read(c, 1, (int)in->arg[1].val, RG_NUM);
        rg_write(c, 1, (int)in->arg[0].val, RG_NUM);
        break;
      case ROP_add_imm_self_s:
      case ROP_sub_imm_self_s:
        rg_read(c, 1, (int)in->arg[0].val, RG_NUM);
        rg_write(c, 1, (int)in->arg[0].val, RG_NUM);
        break;
      case ROP_add_imm_self_branch:
      case ROP_add_imm_self_branch_s:
        rg_read(c, 1, (int)in->arg[0].val, RG_NUM);
        rg_write(c, 1, (int)in->arg[0].val, RG_NUM);
        break;

      case ROP_neg:
      case ROP_inc:
      case ROP_dec:
      case ROP_not:
      case ROP_to_numeric:
        rg_read(c, 1, (int)in->arg[1].val, RG_NUM);
        rg_write(c, 1, (int)in->arg[0].val, RG_NUM);
        break;

      case ROP_load_i8:
      case ROP_load_i8_s:
      case ROP_load_i32:
        rg_write(c, 1, (int)in->arg[0].val, RG_NUM);
        break;

      case ROP_move:
      case ROP_move_s:
        rg_read(c, 1, (int)in->arg[1].val, RG_NUM);
        rg_write(c, 1, (int)in->arg[0].val, RG_NUM);
        break;
      case ROP_move_xfer:
        rg_read(c, 1, (int)in->arg[1].val, RG_NUM);
        rg_write(c, 1, (int)in->arg[0].val, RG_NUM);
        break;

      case ROP_branch_cmp_loc_loc:
        rg_read(c, 0, (int)in->arg[1].val, RG_NUM);
        rg_read(c, 0, (int)in->arg[2].val, RG_NUM);
        break;
      case ROP_branch_cmp_loc_reg:
        rg_read(c, 0, (int)in->arg[1].val, RG_NUM);
        rg_read(c, 1, (int)in->arg[2].val, RG_NUM);
        break;
      case ROP_branch_cmp_reg_loc:
        rg_read(c, 1, (int)in->arg[1].val, RG_NUM);
        rg_read(c, 0, (int)in->arg[2].val, RG_NUM);
        break;
      case ROP_branch_cmp_reg_reg:
      case ROP_branch_cmp_reg_reg_s:
        rg_read(c, 1, (int)in->arg[1].val, RG_NUM);
        rg_read(c, 1, (int)in->arg[2].val, RG_NUM);
        break;

      case ROP_branch_lt_s:
      case ROP_branch_nlt_s:
      case ROP_branch_lte_s:
      case ROP_branch_nlte_s:
      case ROP_branch_gt_s:
      case ROP_branch_ngt_s:
      case ROP_branch_gte_s:
      case ROP_branch_ngte_s:
      case ROP_branch_eq_s:
      case ROP_branch_neq_s:
        rg_read(c, 1, (int)in->arg[0].val, RG_NUM);
        rg_read(c, 1, (int)in->arg[1].val, RG_NUM);
        break;

      case ROP_div:
        rg_read(c, 1, (int)in->arg[1].val, RG_NUM);
        rg_read(c, 1, (int)in->arg[2].val, RG_NUM);
        rg_write(c, 1, (int)in->arg[0].val, RG_NUM);
        break;

      case ROP_load_false:
      case ROP_load_true:
      case ROP_load_null:
        rg_write(c, 1, (int)in->arg[0].val, RG_BOX);
        break;

      case ROP_branch_if:
      case ROP_branch_if_not:
        rg_read(c, 1, (int)in->arg[0].val, RG_NUM);
        break;

      case ROP_branch:
      case ROP_branch_s:
        break;

      default:
        if (getenv("QJSAOT_REGION_DEBUG"))
          fprintf(stderr, "    op %d not whitelisted\n", in->op);
        return 0;
    }
  }
  return 1;
}

/* Recognise the canonical `for (i = 0; i < n; i++) a[i]` induction
   variable and its loop bound. Sets c->iv_reg and c->bound_off, and marks the
   element bases read with the IV as key (so their index checks can be elided).
   Only the register-IV shape is handled; anything else leaves iv_reg = -1. */
static void rg_find_iv(RgCtx *c) {
  int t;
  c->iv_reg = -1;
  c->bound_isreg = -1;
  for (t = c->h; t <= c->h + 1 && t <= c->back; t++) {
    const JSAotGenInsn *in = &c->ins[t];
    int idx_off = 0, bd_off = 0, tgt = -1, ok = 0;
    switch (in->op) {
      case ROP_branch_nlt_s:
        idx_off = (int)in->arg[0].val;
        bd_off = (int)in->arg[1].val;
        tgt = c->nxts[t] + (int)in->arg[2].val;
        ok = 1;
        break;
      case ROP_branch_cmp_reg_reg:
      case ROP_branch_cmp_reg_reg_s: {
        int sub = (int)in->arg[0].val;
        if (sub == REG_BIN_GTE || sub == (REG_BIN_LT | REG_CMP_INVERT)) {
          idx_off = (int)in->arg[1].val;
          bd_off = (int)in->arg[2].val;
          tgt = c->nxts[t] + (int)in->arg[3].val;
          ok = 1;
        }
        break;
      }
      default:
        break;
    }
    if (!ok || tgt != c->exit_pc) continue;
    {
      int iv = rg_ri(idx_off), bi = rg_ri(bd_off), k, writes = 0,
          last_write = -1;
      if (iv < 0 || iv >= c->rn || bi < 0 || bi >= c->rn) continue;
      if (c->role_r[iv] != RG_NUM || !c->live_r[iv]) continue;
      if (c->role_r[bi] != RG_NUM || c->prod_r[bi] != RG_NONE)
        continue; /* the bound must be loop-invariant */
      /* The IV is written only by self +/- 1, and every element read keyed by
         it happens before that write (so the read never sees the +1 value). */
      for (k = c->h; k <= c->back; k++) {
        const JSAotGenInsn *q = &c->ins[k];
        int d = (int)q->arg[0].val;
        if ((q->op == ROP_inc || q->op == ROP_dec) && d == idx_off &&
            (int)q->arg[1].val == idx_off) {
          writes++;
          last_write = k;
        } else if (
            (q->op == ROP_add_imm_self_s || q->op == ROP_sub_imm_self_s ||
             q->op == ROP_add_imm_self_branch ||
             q->op == ROP_add_imm_self_branch_s) &&
            d == idx_off && (int)(int8_t)q->arg[1].val == 1) {
          writes++;
          last_write = k;
        }
      }
      if (writes == 0) continue;
      for (k = c->h; k <= c->back; k++) {
        const JSAotGenInsn *q = &c->ins[k];
        if (q->op == ROP_get_element && (int)q->arg[2].val == idx_off &&
            k > last_write)
          return; /* an element read sees the incremented value: unsafe */
      }
      /* mark element bases read with the IV as key */
      for (k = c->h; k <= c->back; k++) {
        const JSAotGenInsn *q = &c->ins[k];
        if (q->op == ROP_get_element && (int)q->arg[2].val == idx_off) {
          int b = rg_ri((int)q->arg[1].val);
          if (b >= 0 && b < c->rn) c->elem_iv_r[b] = 1;
        }
      }
      c->iv_reg = iv;
      c->bound_isreg = 1;
      c->bound_off = bd_off;
      return;
    }
  }
}

/* Emit the box-back of every currently-unboxed value. */
static void rg_box_all(RgCtx *c) {
  int i;
  if (mut_is("no_box_on_exit")) return;
  for (i = 0; i < c->rn; i++)
    if (c->role_r[i] == RG_NUM && c->unb_r[i]) {
      if (i == c->iv_reg && !mut_is("iv_no_box"))
        rg_out(
            c,
            "        qjs_aot_set_reg_p(ctx, fr, %d, "
            "qjs_aot_box_num(u_%d, qjs_aot_fitsi(u_%d)));\n",
            i * 16, i * 16, i * 16);
      else
        rg_out(
            c,
            "        qjs_aot_set_reg_p(ctx, fr, %d, qjs_aot_box_num(u_%d, "
            "k_%d));\n",
            i * 16, i * 16, i * 16);
    }
  for (i = 0; i < c->sn; i++)
    if (c->role_s[i] == RG_NUM && c->unb_s[i])
      rg_out(
          c,
          "        qjs_aot_put_local(ctx, vb, %d, qjs_aot_box_num(us_%d, "
          "ks_%d));\n",
          i, i, i);
}

/* A deopt: box everything, then jump to the Layer A label for pc. The
   per-region disable counter is bumped so a loop that keeps failing its entry
   guard stops paying for it. */
static void rg_deopt(RgCtx *c, int pc, int is_head) {
  rg_box_all(c);
  rg_out(c, "        rg_dis_%d++;\n", c->head_pc);
  if (is_head)
    rg_out(c, "        goto pc_%d_la;\n", pc);
  else
    rg_out(c, "        goto pc_%d;\n", pc);
}

typedef struct {
  int isreg;
  int off;
} RgVal;

static RgVal rg_reg(int off) {
  RgVal v;
  v.isreg = 1;
  v.off = off;
  return v;
}
static RgVal rg_slot(int s) {
  RgVal v;
  v.isreg = 0;
  v.off = s;
  return v;
}

/* Emit a deopt when a value is not currently unboxed as a number. */
static void rg_ensure_num(RgCtx *c, int isreg, int off, int pc) {
  int idx = isreg ? rg_ri(off) : off;
  unsigned char *unb = isreg ? c->unb_r : c->unb_s;
  unsigned char *role = isreg ? c->role_r : c->role_s;
  int bad;
  if (idx < 0 || (isreg ? idx >= c->rn : idx >= c->sn))
    bad = 1;
  else
    bad = (role[idx] != RG_NUM || !unb[idx]);
  if (bad) rg_deopt(c, pc, pc == c->head_pc);
}

static void rg_num_expr(RgVal v, char *buf, size_t n) {
  if (v.isreg)
    snprintf(buf, n, "u_%d", v.off);
  else
    snprintf(buf, n, "us_%d", v.off);
}
/* The induction variable keeps k=1 with no per-iteration FITSI, so its
   tag is stale past INT32_MAX; every consumer re-derives the exact isint. */
static int rg_is_iv(RgCtx *c, RgVal v) {
  return v.isreg && c->iv_reg >= 0 && rg_ri(v.off) == c->iv_reg;
}
static void rg_kint_expr(RgCtx *c, RgVal v, char *buf, size_t n) {
  if (rg_is_iv(c, v) && !mut_is("iv_no_box")) {
    snprintf(buf, n, "qjs_aot_fitsi(u_%d)", v.off);
  } else if (v.isreg) {
    snprintf(buf, n, "k_%d", v.off);
  } else {
    snprintf(buf, n, "ks_%d", v.off);
  }
}
static void rg_box_expr(RgCtx *c, RgVal v, char *buf, size_t n) {
  char k[32];
  if (v.isreg) {
    rg_kint_expr(c, v, k, sizeof k);
    snprintf(buf, n, "qjs_aot_box_num(u_%d,%s)", v.off, k);
  } else {
    rg_kint_expr(c, v, k, sizeof k);
    snprintf(buf, n, "qjs_aot_box_num(us_%d,%s)", v.off, k);
  }
}

/* A JSValue expression for a value of any role: an unboxed numeric is boxed,
   a boxed/object value already lives in fr[] / vb[]. */
static void rg_val_box_expr(RgCtx *c, int isreg, int off, char *buf, size_t n) {
  int idx = isreg ? rg_ri(off) : off;
  unsigned char *role = isreg ? c->role_r : c->role_s;
  int lim = isreg ? c->rn : c->sn;
  if (idx >= 0 && idx < lim && role[idx] == RG_NUM)
    rg_box_expr(c, isreg ? rg_reg(off) : rg_slot(off), buf, n);
  else if (isreg)
    snprintf(buf, n, "AOT_REG(fr,%d)", off);
  else
    snprintf(buf, n, "vb[%d]", off);
}

/* Emit a numeric binary op into an unboxed destination expression. */
static void rg_numop(
    RgCtx *c, int sub, const char *dst, const char *dka, const char *ea,
    const char *ka, const char *eb, const char *kb, int pc) {
  if (sub == REG_BIN_ADD || sub == REG_BIN_SUB || sub == REG_BIN_MUL) {
    const char *cs =
        sub == REG_BIN_ADD ? "+" : (sub == REG_BIN_SUB ? "-" : "*");
    if (mut_is("fitsi_loose"))
      rg_out(
          c, "        { double _d = %s %s %s; %s = _d; %s = %s && %s; }\n", ea,
          cs, eb, dst, dka, ka, kb);
    else
      rg_out(
          c,
          "        { double _d = %s %s %s; %s = _d; %s = %s && %s && "
          "qjs_aot_fitsi(_d); }\n",
          ea, cs, eb, dst, dka, ka, kb);
  } else if (sub == REG_BIN_DIV) {
    /* E1 div: the result is an int only when it is exactly an int32. */
    if (mut_is("div_int"))
      rg_out(
          c, "        { double _d = %s / %s; %s = _d; %s = 1; }\n", ea, eb, dst,
          dka);
    else
      rg_out(
          c,
          "        { double _d = %s / %s; %s = _d; %s = qjs_aot_fitsi(_d); }\n",
          ea, eb, dst, dka);
  } else {
    rg_out(c, "        { JSValue _o;\n");
    rg_out(
        c,
        "          if (!qjs_aot_num_fast(%d, qjs_aot_box_num(%s,%s), "
        "qjs_aot_box_num(%s,%s), &_o)) {\n",
        sub, ea, ka, eb, kb);
    rg_deopt(c, pc, pc == c->head_pc);
    rg_out(c, "          }\n");
    rg_out(c, "          qjs_aot_to_num(_o, &%s, &%s);\n", dst, dka);
    rg_out(c, "        }\n");
  }
}

/* A boxed-result binary op (comparisons): the destination register holds the
   boxed boolean/result the interpreter would produce. */
static void rg_boxop(RgCtx *c, int sub, int d, RgVal a, RgVal b, int pc) {
  char ea[64], eb[64];
  rg_ensure_num(c, a.isreg, a.off, pc);
  rg_ensure_num(c, b.isreg, b.off, pc);
  rg_box_expr(c, a, ea, sizeof ea);
  rg_box_expr(c, b, eb, sizeof eb);
  rg_out(c, "        { JSValue _r;\n");
  rg_out(
      c, "          if (qjs_aot_binary(ctx, %d, %s, %s, &_r)) {\n", sub, ea,
      eb);
  rg_deopt(c, pc, pc == c->head_pc);
  rg_out(c, "          }\n");
  rg_out(c, "          qjs_aot_set_reg_p(ctx, fr, %d, _r); }\n", d);
  c->unb_r[rg_ri(d)] = 0;
}

/* E1 binary op d = a OP b (d is always a register). */
static void rg_bin2(RgCtx *c, int sub, int d, RgVal a, RgVal b, int pc) {
  char ea[40], eb[40], ka[40], kb[40], dst[40], dka[40];
  rg_ensure_num(c, a.isreg, a.off, pc);
  rg_ensure_num(c, b.isreg, b.off, pc);
  rg_num_expr(a, ea, sizeof ea);
  rg_num_expr(b, eb, sizeof eb);
  rg_kint_expr(c, a, ka, sizeof ka);
  rg_kint_expr(c, b, kb, sizeof kb);
  snprintf(dst, sizeof dst, "u_%d", d);
  snprintf(dka, sizeof dka, "k_%d", d);
  rg_numop(c, sub, dst, dka, ea, ka, eb, kb, pc);
  c->unb_r[rg_ri(d)] = 1;
}

/* E1 binary op with an int immediate: d = a OP imm. */
static void rg_bin_imm(RgCtx *c, int sub, int d, RgVal a, int64_t imm, int pc) {
  char ea[40], ka[40], dst[40], dka[40], ei[32];
  rg_ensure_num(c, a.isreg, a.off, pc);
  rg_num_expr(a, ea, sizeof ea);
  rg_kint_expr(c, a, ka, sizeof ka);
  snprintf(dst, sizeof dst, "u_%d", d);
  snprintf(dka, sizeof dka, "k_%d", d);
  snprintf(ei, sizeof ei, "%.17g", (double)imm);
  rg_numop(c, sub, dst, dka, ea, ka, ei, "1", pc);
  c->unb_r[rg_ri(d)] = 1;
}

/* E1 unary op d = OP a. */
static void rg_un(RgCtx *c, int op, int d, RgVal a, int pc) {
  char ea[40], ka[40];
  rg_ensure_num(c, a.isreg, a.off, pc);
  rg_num_expr(a, ea, sizeof ea);
  rg_kint_expr(c, a, ka, sizeof ka);
  if (op == ROP_to_numeric) {
    rg_out(c, "        u_%d = %s; k_%d = %s;\n", d, ea, d, ka);
  } else if (op == ROP_neg || op == ROP_inc || op == ROP_dec) {
    const char *cs = op == ROP_neg ? "-" : (op == ROP_inc ? "+" : "-");
    int is_iv = (op == ROP_inc || op == ROP_dec) && c->iv_reg >= 0 &&
                rg_ri(d) == c->iv_reg && a.isreg && rg_ri(a.off) == c->iv_reg;
    if (op == ROP_neg)
      rg_out(
          c,
          "        { double _d = -%s; u_%d = _d; k_%d = %s && "
          "qjs_aot_fitsi(_d); }\n",
          ea, d, d, ka);
    else if (is_iv)
      /* The IV is kept as an exact double (an int64-range integer) with no
         per-iteration FITSI; its tag stays 1 and may be stale past 2^31. Every
         box site re-applies the exact interpreter rule (rg_box_expr/rg_kint_expr
         and rg_box_all), so the observable tag is still INT iff (int32_t)d == d. */
      rg_out(
          c, "        { double _d = %s %s 1.0; u_%d = _d; k_%d = 1; }\n", ea,
          cs, d, d);
    else
      rg_out(
          c,
          "        { double _d = %s %s 1.0; u_%d = _d; k_%d = %s && "
          "qjs_aot_fitsi(_d); }\n",
          ea, cs, d, d, ka);
  } else {
    rg_out(c, "        { JSValue _o;\n");
    rg_out(
        c,
        "          if (!qjs_aot_unary_num_fast(%d, qjs_aot_box_num(%s,%s), "
        "&_o)) {\n",
        REG_UN_NOT, ea, ka);
    rg_out(
        c,
        "            if (qjs_aot_unary(ctx, %d, qjs_aot_box_num(%s,%s), &_o)) "
        "{\n",
        REG_UN_NOT, ea, ka);
    rg_deopt(c, pc, pc == c->head_pc);
    rg_out(c, "            }\n          }\n");
    rg_out(c, "          qjs_aot_to_num(_o, &u_%d, &k_%d);\n", d, d);
    rg_out(c, "        }\n");
  }
  c->unb_r[rg_ri(d)] = 1;
}

/* Dense-array element read, all unboxed. */
static void rg_elem(RgCtx *c, int d, int base, RgVal key, int pc) {
  int bk = rg_ri(base);
  char ek[40], kk[40];
  rg_ensure_num(c, key.isreg, key.off, pc);
  if (bk < 0 || bk >= c->rn || c->role_r[bk] != RG_OBJ)
    rg_deopt(c, pc, pc == c->head_pc);
  rg_num_expr(key, ek, sizeof ek);
  rg_kint_expr(c, key, kk, sizeof kk);
  {
    /* An IV-keyed read is covered by the hoisted `bound <= len` guard. */
    int iv_key = c->iv_reg >= 0 && key.isreg && rg_ri(key.off) == c->iv_reg;
    if (!mut_is("skip_bounds") && !iv_key) {
      rg_out(
          c, "        if (!(%s && %s >= 0.0 && %s < (double)len_%d)) {\n", kk,
          ek, ek, base);
      rg_deopt(c, pc, pc == c->head_pc);
      rg_out(c, "        }\n");
    }
  }
  rg_out(
      c,
      "        if (!qjs_aot_elem_num(data_%d, (uint32_t)%s, &u_%d, &k_%d)) {\n",
      base, ek, d, d);
  rg_deopt(c, pc, pc == c->head_pc);
  rg_out(c, "        }\n");
  c->unb_r[rg_ri(d)] = 1;
}

/* Emit `goto` for a branch target: loop head, region exit, or leave the
   region (boxing first). The loop head is a back edge, so it polls the
   interrupt handler first, the way an interpreter back edge does. */
static void rg_goto_target(RgCtx *c, int tgt) {
  if (tgt == c->head_pc) {
    rg_out(
        c,
        "        if (__builtin_expect(qjs_aot_poll(ctx) < 0, 0)) { pc = "
        "%d; goto exception; }\n",
        c->head_pc);
    rg_out(c, "        goto rg_%d_loop;\n", c->head_pc);
  } else if (tgt == c->exit_pc) {
    rg_out(c, "        goto rg_%d_exit;\n", c->head_pc);
  } else {
    rg_box_all(c);
    rg_out(c, "        goto pc_%d;\n", tgt);
  }
}

/* Conditional numeric compare branch. */
static void rg_branch_cmp(
    RgCtx *c, int sub, RgVal a, RgVal b, int tgt, int pc) {
  char ea[40], eb[40];
  int invert = (sub & REG_CMP_INVERT) != 0;
  int base = sub & ~REG_CMP_INVERT;
  const char *cop = NULL;
  rg_ensure_num(c, a.isreg, a.off, pc);
  rg_ensure_num(c, b.isreg, b.off, pc);
  rg_num_expr(a, ea, sizeof ea);
  rg_num_expr(b, eb, sizeof eb);
  switch (base) {
    case REG_BIN_LT:
      cop = "<";
      break;
    case REG_BIN_LTE:
      cop = "<=";
      break;
    case REG_BIN_GT:
      cop = ">";
      break;
    case REG_BIN_GTE:
      cop = ">=";
      break;
    case REG_BIN_EQ:
    case REG_BIN_STRICT_EQ:
      cop = "==";
      break;
    case REG_BIN_NEQ:
    case REG_BIN_STRICT_NEQ:
      cop = "!=";
      break;
    default:
      break;
  }
  if (cop == NULL) { /* not a plain numeric compare: fall back to the helper */
    char ea2[40], eb2[40];
    rg_box_expr(c, a, ea2, sizeof ea2);
    rg_box_expr(c, b, eb2, sizeof eb2);
    rg_out(c, "        { int _t;\n");
    rg_out(
        c, "          if (!qjs_aot_branch_cmp(%d, %s, %s, &_t)) {\n", sub, ea2,
        eb2);
    rg_deopt(c, pc, pc == c->head_pc);
    rg_out(c, "          }\n");
  } else {
    /* Both operands are unboxed numbers (the entry to_num guards prove it),
       so a direct double compare has exactly JS semantics, including NaN and
       -0. */
    rg_out(c, "        { int _t = (%s %s %s);\n", ea, cop, eb);
    if (invert) rg_out(c, "          _t = !_t;\n");
  }
  if (tgt == c->head_pc) {
    rg_out(c, "          if (_t) goto rg_%d_loop;\n", c->head_pc);
  } else if (tgt == c->exit_pc) {
    rg_out(c, "          if (_t) goto rg_%d_exit;\n", c->head_pc);
  } else {
    rg_out(c, "          if (_t) {\n");
    rg_box_all(c);
    rg_out(c, "            goto pc_%d;\n          }\n", tgt);
  }
  rg_out(c, "        }\n");
}

/* Conditional boolean branch. */
static void rg_branch_bool(RgCtx *c, int op, int a, int tgt, int pc) {
  int want = (op == ROP_branch_if);
  char ev[64];
  rg_val_box_expr(c, 1, a, ev, sizeof ev);
  if (tgt == c->head_pc) {
    rg_out(
        c,
        "        if ((qjs_aot_to_bool(ctx, %s) != 0) == %d) goto rg_%d_loop;\n",
        ev, want, c->head_pc);
  } else if (tgt == c->exit_pc) {
    rg_out(
        c,
        "        if ((qjs_aot_to_bool(ctx, %s) != 0) == %d) goto rg_%d_exit;\n",
        ev, want, c->head_pc);
  } else {
    rg_out(
        c, "        if ((qjs_aot_to_bool(ctx, %s) != 0) == %d) {\n", ev, want);
    rg_box_all(c);
    rg_out(c, "          goto pc_%d;\n        }\n", tgt);
  }
}

/* Emit the unboxed loop body. Assumes rg_scan accepted the region. */
static void rg_emit_body(RgCtx *c) {
  int i;
  for (i = c->h; i <= c->back; i++) {
    const JSAotGenInsn *in = &c->ins[i];
    int pc = c->offs[i], next = c->nxts[i];
    switch (in->op) {
      case ROP_local_check:
      case ROP_local_check_init:
        break;

      case ROP_get_local: {
        int d = (int)in->arg[0].val, s = (int)in->arg[1].val;
        if (s >= 0 && s < c->sn && c->role_s[s] == RG_NUM) {
          rg_out(c, "        u_%d = us_%d; k_%d = ks_%d;\n", d, s, d, s);
          c->unb_r[rg_ri(d)] = 1;
        } else {
          rg_out(c, "        AOT_SET_B(ctx, fr, %d, vb[%d]);\n", d, s);
          c->unb_r[rg_ri(d)] = 0;
        }
        break;
      }
      case ROP_get_local_s0:
      case ROP_get_local_s1:
      case ROP_get_local_s2:
      case ROP_get_local_s3: {
        int d = (int)in->arg[0].val, s = in->op - ROP_get_local_s0;
        if (s < c->sn && c->role_s[s] == RG_NUM) {
          rg_out(c, "        u_%d = us_%d; k_%d = ks_%d;\n", d, s, d, s);
          c->unb_r[rg_ri(d)] = 1;
        } else {
          rg_out(c, "        AOT_SET_B(ctx, fr, %d, vb[%d]);\n", d, s);
          c->unb_r[rg_ri(d)] = 0;
        }
        break;
      }

      case ROP_put_local: {
        int s = (int)in->arg[0].val, r = (int)in->arg[1].val;
        if (s >= 0 && s < c->sn && c->role_s[s] == RG_NUM) {
          rg_ensure_num(c, 1, r, pc);
          rg_out(c, "        us_%d = u_%d; ks_%d = k_%d;\n", s, r, s, r);
          c->unb_s[s] = 1;
        } else {
          rg_deopt(c, pc, pc == c->head_pc);
        }
        break;
      }
      case ROP_put_local_s0:
      case ROP_put_local_s1:
      case ROP_put_local_s2:
      case ROP_put_local_s3: {
        int s = in->op - ROP_put_local_s0, r = (int)in->arg[0].val;
        if (c->role_s[s] == RG_NUM) {
          rg_ensure_num(c, 1, r, pc);
          rg_out(c, "        us_%d = u_%d; ks_%d = k_%d;\n", s, r, s, r);
          c->unb_s[s] = 1;
        } else {
          rg_deopt(c, pc, pc == c->head_pc);
        }
        break;
      }
      case ROP_put_local_xfer:
      case ROP_put_local_xfer_s0:
      case ROP_put_local_xfer_s1:
      case ROP_put_local_xfer_s2:
      case ROP_put_local_xfer_s3: {
        int s = (in->op == ROP_put_local_xfer) ? (int)in->arg[0].val
                                               : in->op - ROP_put_local_xfer_s0;
        int r = (int)in->arg[in->op == ROP_put_local_xfer ? 1 : 0].val;
        if (s >= 0 && s < c->sn && c->role_s[s] == RG_NUM) {
          rg_ensure_num(c, 1, r, pc);
          rg_out(c, "        us_%d = u_%d; ks_%d = k_%d;\n", s, r, s, r);
          c->unb_s[s] = 1;
          c->unb_r[rg_ri(r)] = 0; /* the xfer empties the register */
        } else {
          rg_deopt(c, pc, pc == c->head_pc);
        }
        break;
      }

      case ROP_get_element:
        rg_elem(
            c, (int)in->arg[0].val, (int)in->arg[1].val,
            rg_reg((int)in->arg[2].val), pc);
        break;

      case ROP_get_field:
      case ROP_get_field2: {
        int d = (int)in->arg[0].val, r = (int)in->arg[1].val;
        int site = (int)in->arg[3].val;
        rg_out(
            c,
            "        { JSValue _v = qjs_aot_field_read(AOT_REG(fr,%d), "
            "_fso_%d);\n",
            r, site);
        rg_out(c, "          if (!qjs_aot_to_num(_v, &u_%d, &k_%d)) {\n", d, d);
        rg_deopt(c, pc, pc == c->head_pc);
        rg_out(c, "          } }\n");
        c->unb_r[rg_ri(d)] = 1;
        break;
      }

      case ROP_get_length: {
        int d = (int)in->arg[0].val, r = (int)in->arg[1].val;
        rg_out(
            c,
            "        if (!qjs_aot_get_length_num(AOT_REG(fr,%d), &u_%d, "
            "&k_%d)) {\n",
            r, d, d);
        rg_deopt(c, pc, pc == c->head_pc);
        rg_out(c, "        }\n");
        c->unb_r[rg_ri(d)] = 1;
        break;
      }
      case ROP_get_var_ref: {
        int d = (int)in->arg[0].val, slot = (int)in->arg[1].val;
        rg_out(
            c, "        { JSValue _v = *qjs_aot_var_ref_ptr(f, %d);\n", slot);
        rg_out(c, "          if (!qjs_aot_to_num(_v, &u_%d, &k_%d)) {\n", d, d);
        rg_deopt(c, pc, pc == c->head_pc);
        rg_out(c, "          } }\n");
        c->unb_r[rg_ri(d)] = 1;
        break;
      }

      case ROP_add_mul:
      case ROP_mul_add:
      case ROP_mul_sub:
      case ROP_sub_mul: {
        int dm = (int)in->arg[0].val, x = (int)in->arg[1].val,
            y = (int)in->arg[2].val;
        int da = (int)in->arg[3].val, cc = (int)in->arg[4].val;
        int isadd = (in->op == ROP_mul_add || in->op == ROP_add_mul);
        int mul_first = (in->op == ROP_mul_add || in->op == ROP_mul_sub);
        int bsub = isadd ? REG_BIN_ADD : REG_BIN_SUB;
        rg_bin2(c, REG_BIN_MUL, dm, rg_reg(x), rg_reg(y), pc);
        if (mul_first)
          rg_bin2(c, bsub, da, rg_reg(dm), rg_reg(cc), pc);
        else
          rg_bin2(c, bsub, da, rg_reg(cc), rg_reg(dm), pc);
        break;
      }

      case ROP_eq:
      case ROP_lt:
      case ROP_lte:
      case ROP_gt:
      case ROP_gte:
      case ROP_neq:
      case ROP_strict_eq:
      case ROP_strict_neq:
        rg_boxop(
            c, binop_subop(in->op), (int)in->arg[0].val,
            rg_reg((int)in->arg[1].val), rg_reg((int)in->arg[2].val), pc);
        break;

      case ROP_mul:
      case ROP_add:
      case ROP_sub: {
        int sub = in->op == ROP_mul   ? REG_BIN_MUL
                  : in->op == ROP_add ? REG_BIN_ADD
                                      : REG_BIN_SUB;
        rg_bin2(
            c, sub, (int)in->arg[0].val, rg_reg((int)in->arg[1].val),
            rg_reg((int)in->arg[2].val), pc);
        break;
      }
      case ROP_binary_loc_reg:
        rg_bin2(
            c, (int)in->arg[1].val, (int)in->arg[0].val,
            rg_slot((int)in->arg[2].val), rg_reg((int)in->arg[3].val), pc);
        break;
      case ROP_binary_loc_loc:
        rg_bin2(
            c, (int)in->arg[1].val, (int)in->arg[0].val,
            rg_slot((int)in->arg[2].val), rg_slot((int)in->arg[3].val), pc);
        break;
      case ROP_binary_imm:
      case ROP_binary_imm_s:
        rg_bin_imm(
            c, (int)in->arg[1].val, (int)in->arg[0].val,
            rg_reg((int)in->arg[2].val), (int32_t)in->arg[3].val, pc);
        break;
      case ROP_add_imm_s:
      case ROP_sub_imm_s:
        rg_bin_imm(
            c, in->op == ROP_add_imm_s ? REG_BIN_ADD : REG_BIN_SUB,
            (int)in->arg[0].val, rg_reg((int)in->arg[1].val),
            (int32_t)in->arg[2].val, pc);
        break;
      case ROP_add_imm_self_s:
      case ROP_sub_imm_self_s: {
        int d = (int)in->arg[0].val;
        rg_bin_imm(
            c, in->op == ROP_add_imm_self_s ? REG_BIN_ADD : REG_BIN_SUB, d,
            rg_reg(d), (int32_t)in->arg[1].val, pc);
        break;
      }
      case ROP_add_imm_self_branch:
      case ROP_add_imm_self_branch_s: {
        int d = (int)in->arg[0].val;
        int tgt = next + (int)in->arg[2].val;
        rg_bin_imm(c, REG_BIN_ADD, d, rg_reg(d), (int32_t)in->arg[1].val, pc);
        rg_goto_target(c, tgt);
        break;
      }
      case ROP_neg:
      case ROP_inc:
      case ROP_dec:
      case ROP_not:
      case ROP_to_numeric:
        rg_un(c, in->op, (int)in->arg[0].val, rg_reg((int)in->arg[1].val), pc);
        break;

      case ROP_load_i8:
      case ROP_load_i8_s: {
        int d = (int)in->arg[0].val;
        rg_out(
            c, "        u_%d = (double)(int)%d; k_%d = 1;\n", d,
            (int)(int8_t)in->arg[1].val, d);
        c->unb_r[rg_ri(d)] = 1;
        break;
      }
      case ROP_load_i32: {
        int d = (int)in->arg[0].val;
        rg_out(
            c, "        u_%d = (double)(int32_t)%d; k_%d = 1;\n", d,
            (int32_t)in->arg[1].val, d);
        c->unb_r[rg_ri(d)] = 1;
        break;
      }

      case ROP_move:
      case ROP_move_s:
      case ROP_move_xfer: {
        int d = (int)in->arg[0].val, a = (int)in->arg[1].val;
        rg_ensure_num(c, 1, a, pc);
        rg_out(c, "        u_%d = u_%d; k_%d = k_%d;\n", d, a, d, a);
        c->unb_r[rg_ri(d)] = 1;
        if (in->op == ROP_move_xfer) c->unb_r[rg_ri(a)] = 0;
        break;
      }

      case ROP_branch_cmp_loc_loc:
        rg_branch_cmp(
            c, (int)in->arg[0].val, rg_slot((int)in->arg[1].val),
            rg_slot((int)in->arg[2].val), next + (int)in->arg[3].val, pc);
        break;
      case ROP_branch_cmp_loc_reg:
        rg_branch_cmp(
            c, (int)in->arg[0].val, rg_slot((int)in->arg[1].val),
            rg_reg((int)in->arg[2].val), next + (int)in->arg[3].val, pc);
        break;
      case ROP_branch_cmp_reg_loc:
        rg_branch_cmp(
            c, (int)in->arg[0].val, rg_reg((int)in->arg[1].val),
            rg_slot((int)in->arg[2].val), next + (int)in->arg[3].val, pc);
        break;
      case ROP_branch_cmp_reg_reg:
      case ROP_branch_cmp_reg_reg_s:
        rg_branch_cmp(
            c, (int)in->arg[0].val, rg_reg((int)in->arg[1].val),
            rg_reg((int)in->arg[2].val), next + (int)in->arg[3].val, pc);
        break;

      case ROP_branch_lt_s:
      case ROP_branch_nlt_s:
      case ROP_branch_lte_s:
      case ROP_branch_nlte_s:
      case ROP_branch_gt_s:
      case ROP_branch_ngt_s:
      case ROP_branch_gte_s:
      case ROP_branch_ngte_s:
      case ROP_branch_eq_s:
      case ROP_branch_neq_s:
        rg_branch_cmp(
            c, branch_subop(in->op), rg_reg((int)in->arg[0].val),
            rg_reg((int)in->arg[1].val), next + (int)in->arg[2].val, pc);
        break;

      case ROP_div:
        rg_bin2(
            c, REG_BIN_DIV, (int)in->arg[0].val, rg_reg((int)in->arg[1].val),
            rg_reg((int)in->arg[2].val), pc);
        break;

      case ROP_load_false:
        rg_out(
            c, "        AOT_SET_B(ctx, fr, %d, JS_FALSE);\n",
            (int)in->arg[0].val);
        c->unb_r[rg_ri((int)in->arg[0].val)] = 0;
        break;
      case ROP_load_true:
        rg_out(
            c, "        AOT_SET_B(ctx, fr, %d, JS_TRUE);\n",
            (int)in->arg[0].val);
        c->unb_r[rg_ri((int)in->arg[0].val)] = 0;
        break;
      case ROP_load_null:
        rg_out(
            c, "        AOT_SET_B(ctx, fr, %d, JS_NULL);\n",
            (int)in->arg[0].val);
        c->unb_r[rg_ri((int)in->arg[0].val)] = 0;
        break;

      case ROP_branch_if:
      case ROP_branch_if_not:
        rg_branch_bool(
            c, in->op, (int)in->arg[0].val, next + (int)in->arg[1].val, pc);
        break;

      case ROP_branch:
      case ROP_branch_s:
        rg_goto_target(c, next + (int)in->arg[0].val);
        break;

      default:
        break;
    }
  }
}

/* If `in` is a branch, return 1 and set *tgt to its target pc. */
static int insn_branch_target(const JSAotGenInsn *in, int next, int *tgt) {
  int op = in->op;
  if (op == ROP_branch || op == ROP_branch_s) {
    *tgt = next + (int)in->arg[0].val;
    return 1;
  }
  if (op == ROP_branch_if || op == ROP_branch_if_not) {
    *tgt = next + (int)in->arg[1].val;
    return 1;
  }
  if (op == ROP_branch_cmp_loc_loc || op == ROP_branch_cmp_loc_reg ||
      op == ROP_branch_cmp_reg_loc || op == ROP_branch_cmp_reg_reg ||
      op == ROP_branch_cmp_reg_reg_s) {
    *tgt = next + (int)in->arg[3].val;
    return 1;
  }
  if (op == ROP_branch_lt_s || op == ROP_branch_nlt_s ||
      op == ROP_branch_lte_s || op == ROP_branch_nlte_s ||
      op == ROP_branch_gt_s || op == ROP_branch_ngt_s ||
      op == ROP_branch_gte_s || op == ROP_branch_ngte_s ||
      op == ROP_branch_eq_s || op == ROP_branch_neq_s) {
    *tgt = next + (int)in->arg[2].val;
    return 1;
  }
  if (op == ROP_add_imm_self_branch || op == ROP_add_imm_self_branch_s) {
    *tgt = next + (int)in->arg[2].val;
    return 1;
  }
  if (op == ROP_local_rmw_branch) {
    *tgt = next + (int)in->arg[3].val;
    return 1;
  }
  return 0;
}

/* Try to build a typed region for the loop [h, back]. Returns 1 and emits it
   when eligible. */
static int emit_region(
    FILE *f, const JSAotGenFunc *g, const JSAotGenInsn *ins, const int *offs,
    const int *nxts, int n, int h, int back, int exit_pc) {
  RgCtx *c = rg_new(g, ins, offs, nxts, n, h, back, exit_pc);
  int i, ok = 1, region_end = nxts[back];
  const char *rej = NULL;
  if (!c) return 0;
  if (exit_pc != region_end) ok = 0, rej = "exit != back-fallthrough";
  /* v1 handles only a top-tested loop with an unconditional back-edge. */
  if (ok && ins[back].op != ROP_branch && ins[back].op != ROP_branch_s)
    ok = 0, rej = "back-edge not unconditional";
  if (ok) {
    int btgt = nxts[back] + (int)ins[back].arg[0].val;
    if (btgt != c->head_pc) ok = 0, rej = "back-edge target != head";
  }
  if (ok && !rg_scan(c)) ok = 0, rej = "op outside whitelist";
  if (ok) {
    for (i = 0; i < c->rn; i++)
      if (c->role_r[i] == RG_MIX || c->prod_r[i] == RG_MIX)
        ok = 0, rej = "register role/producer mix";
    for (i = 0; i < c->sn; i++)
      if (c->role_s[i] == RG_MIX || c->prod_s[i] == RG_MIX)
        ok = 0, rej = "slot role/producer mix";
  }
  /* A numeric value must not be produced by a boxed op. */
  if (ok)
    for (i = 0; i < c->rn; i++)
      if (c->role_r[i] == RG_NUM && c->prod_r[i] == RG_BOX)
        ok = 0, rej = "num register produced by boxed op";
  if (ok)
    for (i = 0; i < c->sn; i++)
      if (c->role_s[i] == RG_NUM && c->prod_s[i] == RG_BOX)
        ok = 0, rej = "num slot produced by boxed op";
  /* Recognise the induction variable / loop bound (best effort). */
  if (ok) rg_find_iv(c);
  /* Field-guard receivers must be loop-invariant so the guard can be hoisted
     out of the loop. */
  if (ok) {
    for (i = 0; i < c->pic_count; i++) {
      int r = c->fguard_recv[i], ri;
      if (r < 0) continue;
      ri = rg_ri(r);
      if (ri < 0 || ri >= c->rn || c->role_r[ri] != RG_OBJ || !c->live_r[ri] ||
          c->prod_r[ri] != RG_NONE)
        ok = 0, rej = "field receiver not loop-invariant";
    }
  }
  /* Element bases are captured once at region entry (data_/len_), so they
     must be loop-invariant too. */
  if (ok) {
    for (i = 0; i < c->rn; i++) {
      if (!c->elem_r[i]) continue;
      if (c->role_r[i] != RG_OBJ || !c->live_r[i] || c->prod_r[i] != RG_NONE)
        ok = 0, rej = "element base not loop-invariant";
    }
  }
  /* Every branch must stay within the region, target the loop head (the
     back-edge), or leave to the single exit. Anything else is rejected. */
  if (ok) {
    for (i = h; i <= back && ok; i++) {
      int tgt;
      if (!insn_branch_target(&ins[i], nxts[i], &tgt)) continue;
      if (tgt == c->head_pc || tgt == c->exit_pc) continue;
      ok = 0, rej = "branch to a non-exit target";
    }
  }
  if (!ok) {
    if (getenv("QJSAOT_REGION_DEBUG"))
      fprintf(
          stderr, "region [%d,%d] head=%d reject: %s\n", h, back, c->head_pc,
          rej ? rej : "?");
    rg_free(c);
    return 0;
  }

  c->f = f;
  c->dry = 0;
  rg_out(c, "        {\n");
  rg_out(c, "        static unsigned int rg_dis_%d = 0;\n", c->head_pc);
  rg_out(
      c, "        if (rg_dis_%d > 64u) goto pc_%d_la;\n", c->head_pc,
      c->head_pc);
  for (i = 0; i < c->rn; i++)
    if (c->role_r[i] == RG_NUM)
      rg_out(c, "        double u_%d; int k_%d;\n", i * 16, i * 16);
  for (i = 0; i < c->rn; i++)
    if (c->role_r[i] == RG_OBJ && c->elem_r[i])
      rg_out(c, "        JSValue *data_%d; uint32_t len_%d;\n", i * 16, i * 16);
  for (i = 0; i < c->sn; i++)
    if (c->role_s[i] == RG_NUM)
      rg_out(c, "        double us_%d; int ks_%d;\n", i, i);
  for (i = 0; i < c->rn; i++) {
    if (rg_entry_unbox(c, i, 1)) {
      rg_out(
          c,
          "        if (!qjs_aot_to_num(AOT_REG(fr,%d), &u_%d, &k_%d)) goto "
          "pc_%d_la;\n",
          i * 16, i * 16, i * 16, c->head_pc);
      c->unb_r[i] = 1;
    } else if (c->live_r[i] && c->role_r[i] == RG_OBJ && c->elem_r[i]) {
      rg_out(
          c,
          "        if (!qjs_aot_dense(AOT_REG(fr,%d), &data_%d, &len_%d)) goto "
          "pc_%d_la;\n",
          i * 16, i * 16, i * 16, c->head_pc);
    }
  }
  for (i = 0; i < c->sn; i++) {
    if (rg_entry_unbox(c, i, 0)) {
      rg_out(
          c,
          "        if (!qjs_aot_to_num(vb[%d], &us_%d, &ks_%d)) goto "
          "pc_%d_la;\n",
          i, i, i, c->head_pc);
      c->unb_s[i] = 1;
    }
  }
  /* The induction variable starts >= 0 and stays below the (invariant)
     loop bound; if the bound is within every element base's length, the
     per-iteration index checks for IV-keyed reads can be elided. */
  if (c->iv_reg >= 0) {
    int iv = c->iv_reg;
    rg_out(
        c, "        if (!(k_%d && u_%d >= 0.0)) goto pc_%d_la;\n", iv * 16,
        iv * 16, c->head_pc);
    if (!mut_is("bounds_elide_len_write")) {
      for (i = 0; i < c->rn; i++)
        if (c->elem_iv_r[i])
          rg_out(
              c,
              "        if (!(k_%d && u_%d <= (double)len_%d)) goto pc_%d_la;\n",
              c->bound_off, c->bound_off, i * 16, c->head_pc);
    }
  }
  for (i = 0; i < c->pic_count; i++) {
    int r = c->fguard_recv[i];
    if (r < 0) continue;
    rg_out(c, "        int _fso_%d;\n", i);
    if (mut_is("hoist_ignore_store")) {
      /* Mutant: trust the generation-time slot offset and skip the shape
         guard, so a mid-loop shape change reads a stale slot. */
      rg_out(c, "        _fso_%d = 0;\n", i);
    } else {
      rg_out(
          c,
          "        if (!qjs_aot_field_guard(pic, AOT_REG(fr,%d), %d, "
          "&_fso_%d)) goto pc_%d_la;\n",
          r, i, i, c->head_pc);
    }
  }
  memcpy(c->entry_r, c->unb_r, (size_t)c->rn);
  memcpy(c->entry_s, c->unb_s, (size_t)c->sn);
  rg_out(c, "rg_%d_loop:\n", c->head_pc);
  rg_emit_body(c);
  /* The exit is reached from the top of the loop, where only the loop-carried
     (entry) values are definitely unboxed. Restore that set before boxing so
     that temporaries defined later in the body are never read uninitialised. */
  memcpy(c->unb_r, c->entry_r, (size_t)c->rn);
  memcpy(c->unb_s, c->entry_s, (size_t)c->sn);
  rg_out(c, "rg_%d_exit:\n", c->head_pc);
  rg_box_all(c);
  if (exit_pc >= g->len)
    rg_out(c, "        goto pc_end;\n");
  else
    rg_out(c, "        goto pc_%d;\n", exit_pc);
  rg_out(c, "        }\n");
  rg_free(c);
  return 1;
}

/* Emit one function's body (templates, step for the rest). Returns the
   instruction count, or -1 on a malformed program. */
/* The body emitter, overridable so qjsaot2 can drive this same unit builder
   with its own lowering. NULL means the template emitter below. */
int (*qjs_aot_emit_body)(FILE *f, const JSAotGenFunc *g, char hex[33]);

/* A compiled function's code is a cost of its own (I-cache, and a second body
   doubles it). A function over this many instructions keeps the generic body
   only, which is what box2d's SVC-sized functions would get if the threshold
   were ever reached: SVC is 5,000-odd instructions and 423 KB. */
int qjs_aot_body_over_budget(int ninsns) {
  const char *e = getenv("QJS_AOT_BODY_BUDGET");
  int budget = e ? atoi(e) : 6000;
  return ninsns > budget;
}
/* Whether a function is large enough that a second body is not worth it. The
   lowering calls this before emitting; the count is the function's instruction
   count, which tracks its code size closely and needs no measurement. */
int qjs_aot_body_over_budget(int ninsns);
/* The leaf emitter: a frameless body for a function a compiled caller can run
   in place of a call. Returns 1 with the body written, 0 when the function is
   not such a leaf. NULL means no leaves. */
int (*qjs_aot_emit_pure)(FILE *f, const JSAotGenFunc *g, char hex[33]);

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
   `*n` receives the count. The `qjsaot2` IR reads a definition's profile from
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
    for (k = 0; k < PROF_MAX_SHAPES; k++) shapes[k] = e->sites[i].shape[k];
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

/* Whether the profiler never executed the instruction at `pc` (a `U` record).
   Such a block is emitted as an exit in a specialised body. */
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

int qjs_aot_unit_has(const char hex[33]) {
  uint8_t dg[16];
  int i;

  if (!g_unit_funcs || hex_to_bytes16(hex, dg) < 0) return 0;
  for (i = 0; i < g_unit_nfuncs; i++)
    if (memcmp(g_unit_funcs[i].digest, dg, 16) == 0)
      return g_unit_funcs[i].eligible == AOT_GEN_ELIGIBLE;
  return 0;
}

static int emit_func(FILE *f, const JSAotGenFunc *g, char hex[33]) {
  int off = 0, n = 0, i;
  int *offs = NULL;
  int *nxts = NULL;
  JSAotGenInsn *ins = NULL;
  int cap = 0;

  g_cur_prof = prof_entry(g_prof, g_nprof, g->digest);

  while (off < g->len) {
    JSAotGenInsn cur;
    if (js_aot_gen_decode_insn(g->code, g->len, off, &cur) < 0) {
      free(offs);
      free(nxts);
      free(ins);
      return -1;
    }
    if (n == cap) {
      int *no, *nn;
      JSAotGenInsn *ni;
      cap = cap ? cap * 2 : 64;
      no = realloc(offs, sizeof(*offs) * (size_t)cap);
      nn = realloc(nxts, sizeof(*nxts) * (size_t)cap);
      ni = realloc(ins, sizeof(*ins) * (size_t)cap);
      if (!no || !nn || !ni) {
        free(no ? no : offs);
        free(nn ? nn : nxts);
        free(ni ? ni : ins);
        return -1;
      }
      offs = no;
      nxts = nn;
      ins = ni;
    }
    offs[n] = off;
    nxts[n] = off + cur.size;
    ins[n] = cur;
    n++;
    off += cur.size;
  }

  fprintf(
      f,
      "%sJSValue aot_f_%s(JSContext *ctx, JSRegRun *f,\n"
      "                                 JSValue *fr, uint32_t entry_pc)\n{\n",
      e2_split_on() ? "__attribute__((visibility(\"hidden\"))) " : "static ",
      hex);
  fprintf(f, "    uint8_t *code = qjs_aot_code(f);\n");
  fprintf(f, "    uint8_t *ip;\n    uint32_t pc = entry_pc;\n");
  fprintf(f, "    JSValue *vb = qjs_aot_var_buf(f);\n");
  fprintf(f, "    JSValue *ab = qjs_aot_arg_buf(f);\n");
  fprintf(f, "    const void *pic = qjs_aot_pic(f);\n");
  fprintf(f, "    (void)code; (void)vb; (void)ab; (void)pic;\n");
  fprintf(f, "    goto dispatch_ip;\n");
  /* Detect back-edges: a branch whose target is an earlier instruction start.
     region_back[head_idx] holds the back-edge instruction index. */
  {
    int *rb = calloc((size_t)n, sizeof(int));
    int k;
    if (rb) {
      for (k = 0; k < n; k++) rb[k] = -1;
      for (k = 0; k < n; k++) {
        int tgt, j;
        if (!insn_branch_target(&ins[k], nxts[k], &tgt)) continue;
        if (tgt >= offs[k]) continue;
        for (j = 0; j < n; j++)
          if (offs[j] == tgt) break;
        if (j < n && rb[j] < 0) rb[j] = k;
      }
    }
    for (i = 0; i < n; i++) {
      int last = nxts[i] >= g->len;
      int is_region = 0;
      fprintf(f, "pc_%d:\n", offs[i]);
      /* Under a deopt-everywhere sweep every instruction is a guard site: a
         forced failure leaves compiled code here, before the instruction has
         had any effect, and the interpreter re-executes it. */
      g_cur_deopt_site = deopt_site_new();
      if (g_cur_deopt_site >= 0)
        fprintf(
            f,
            "    if (qjs_aot_should_deopt(ctx, %d)) return qjs_aot_deopt(ctx, "
            "f, fr, %d, %d);\n",
            g_cur_deopt_site, offs[i], g_cur_deopt_site);
      if (rb && rb[i] >= 0 && !g_deopt_everywhere) {
        is_region =
            emit_region(f, g, ins, offs, nxts, n, i, rb[i], nxts[rb[i]]);
        if (is_region) fprintf(f, "pc_%d_la:\n", offs[i]);
      }
      if (!emit_template(f, &ins[i], offs[i], nxts[i], last))
        emit_fallback(f, offs[i], nxts[i], last, 0);
      g_cur_deopt_site = -1;
    }
    free(rb);
  }
  fprintf(f, "pc_end:\n    return qjs_aot_ret(f);\n");
  fprintf(f, "dispatch_ip:\n    switch (pc) {\n");
  for (i = 0; i < n; i++)
    fprintf(f, "    case %d: goto pc_%d;\n", offs[i], offs[i]);
  fprintf(f, "    case %d: goto pc_end;\n", g->len);
  fprintf(f, "    default: __builtin_unreachable();\n    }\n");
  fprintf(f, "exception: {\n");
  fprintf(f, "        uint32_t rp;\n");
  fprintf(f, "        JSValue ex = qjs_aot_unwind(ctx, f, fr, pc, &rp);\n");
  fprintf(
      f, "        if (!JS_IsException(ex)) { pc = rp; goto dispatch_ip; }\n");
  fprintf(f, "        return JS_EXCEPTION;\n    }\n}\n\n");

  free(offs);
  free(nxts);
  return n;
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

/* Per unit function: the leaf body, when it has one. */
static EmitBuf *g_pure_eb;

/* Which functions this run's E2 emitter gave a native entry. A call site may
   only name an `aot_n_` that some body in this unit defines, and a unit built
   with `--emit2-only` emits the *other* functions with the old emitter, which
   emits no native entry. Keyed by unit index. */
static uint8_t *g_e2_native;

/* The E2 emitter's hook, so the unit builder knows which bodies it produced. */
void qjs_aot_note_native_entry(int index) {
  if (g_e2_native && index >= 0) g_e2_native[index] = 1;
}

/* The unit index of the function being emitted, for the hook above. */
int qjs_aot_cur_func_idx(void) {
  return g_cur_func_idx;
}

int qjs_aot_has_native_entry(int index) {
  return g_e2_native && index >= 0 ? g_e2_native[index] : 0;
}

/* The argument count of the unit function with this digest when it has a leaf
   body, else -1. */
int qjs_aot_leaf_argc(const char hex[33]) {
  uint8_t dg[16];
  int i;

  if (!g_unit_funcs || !g_pure_eb || hex_to_bytes16(hex, dg) < 0) return -1;
  for (i = 0; i < g_unit_nfuncs; i++)
    if (memcmp(g_unit_funcs[i].digest, dg, 16) == 0)
      return g_unit_funcs[i].eligible == AOT_GEN_ELIGIBLE && g_pure_eb[i].buf
                 ? g_unit_funcs[i].arg_count
                 : -1;
  return -1;
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

/* Compare for a descending-ops sort of unit indices. */
static const unsigned long long *g_sort_ops;
static int unit_ops_cmp(const void *a, const void *b) {
  int ia = *(const int *)a, ib = *(const int *)b;
  if (g_sort_ops[ia] != g_sort_ops[ib])
    return g_sort_ops[ia] < g_sort_ops[ib] ? 1 : -1;
  return ia < ib ? -1 : (ia > ib ? 1 : 0);
}

int qjsaot_driver_main(int argc, char **argv) {
  JSRuntime *rt;
  JSContext *ctx;
  JSAotGenFunc *funcs = NULL;
  int nfuncs = 0;
  const char *out = "aot_unit.c";
  /* --emit2-split DIR: write one .c per compiled function plus a shared header
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
    } else if (strcmp(argv[i], "--all") == 0) {
      all = 1;
    } else if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc) {
      profile = argv[++i];
    } else if (strcmp(argv[i], "--budget") == 0 && i + 1 < argc) {
      budget = strtol(argv[++i], NULL, 10);
    } else if (strcmp(argv[i], "--hot-cover") == 0 && i + 1 < argc) {
      hot_cover = strtod(argv[++i], NULL);
    } else if (strcmp(argv[i], "--min-ops") == 0 && i + 1 < argc) {
      min_ops = strtol(argv[++i], NULL, 10);
    } else if (strcmp(argv[i], "--layout-header") == 0 && i + 1 < argc) {
      aot_layout_header = argv[++i];
    } else if (strcmp(argv[i], "--deopt-everywhere") == 0) {
      g_deopt_everywhere = 1;
    } else if (strcmp(argv[i], "--check-locals") == 0) {
      g_check_locals = 1;
    } else if (strcmp(argv[i], "--emit2-nocall") == 0) {
      e2_on = 1;
      e2_set_nocall(1);
    } else if (strcmp(argv[i], "--emit2-poison") == 0) {
      e2_on = 1;
      e2_set_poison(1);
    } else if (strcmp(argv[i], "--emit2") == 0) {
      e2_on = 1;
    } else if (strcmp(argv[i], "--emit2-split") == 0 && i + 1 < argc) {
      split_dir = argv[++i];
      e2_on = 1;
    } else if (strcmp(argv[i], "--emit2-report") == 0 && i + 1 < argc) {
      g_emit2_report = argv[++i];
    } else if (strcmp(argv[i], "--emit2-only") == 0 && i + 1 < argc) {
      const char *h = argv[++i];
      e2_on = 1;
      if (strlen(h) != 32 || strspn(h, "0123456789abcdef") != 32) {
        fprintf(stderr, "qjsaot: --emit2-only wants a 32-digit hex digest\n");
        free(inputs);
        return 2;
      }
      memcpy(e2_only_hex, h, 32);
      e2_only_hex[32] = 0;
    } else if (strcmp(argv[i], "--dump-ir") == 0) {
      g_dump_ir = 1;
    } else if (strcmp(argv[i], "--dump-ir-out") == 0 && i + 1 < argc) {
      g_dump_ir = 1;
      g_ir_path = argv[++i];
    } else if (strcmp(argv[i], "--mut") == 0 && i + 1 < argc) {
      g_mut = argv[++i];
    } else if (strcmp(argv[i], "--emit2-types") == 0) {
      /* Analysis flags the wrapper consumed; tolerated here so that generating
         and dumping types in one run works. */
    } else if (strcmp(argv[i], "--emit2-types-out") == 0 && i + 1 < argc) {
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
    g_unit_nfuncs = nfuncs;
    /* The leaves first: a caller asks whether its callee has one while its own
       body is emitted. */
    g_pure_eb = calloc((size_t)(nfuncs > 0 ? nfuncs : 1), sizeof(*g_pure_eb));
    g_e2_native = calloc((size_t)(nfuncs > 0 ? nfuncs : 1), 1);
    for (i = 0; qjs_aot_emit_pure && g_pure_eb && i < nfuncs; i++) {
      char hex[33];
      FILE *mf;
      int r;

      if (funcs[i].eligible != AOT_GEN_ELIGIBLE) continue;
      hex16(funcs[i].digest, hex);
      g_cur_func_idx = i;
      mf = open_memstream(&g_pure_eb[i].buf, &g_pure_eb[i].len);
      if (!mf) continue;
      r = qjs_aot_emit_pure(mf, &funcs[i], hex);
      fclose(mf);
      if (r != 1) {
        free(g_pure_eb[i].buf);
        g_pure_eb[i].buf = NULL;
        g_pure_eb[i].len = 0;
      }
    }
    /* The bodies are emitted into memory buffers before the output file is
     opened, so the linkage decision has to be made before that: a split unit
     gives every compiled function hidden external linkage so the parts can
     call each other. */
  if (split_dir != NULL) e2_set_split(1);
  if (qjs_aot_emit2_on()) e2_roles_check();
    e2_report_open(g_emit2_report);
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
      ninsn = (qjs_aot_emit_body ? qjs_aot_emit_body : emit_func)(
          mf, &funcs[i], hex);
      fclose(mf);
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
    e2_report_close();

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
      for (k = 0; k < nuniq; k++) {
        ops[uniq[k]] = prof_ops_of(g_prof, g_nprof, funcs[uniq[k]].digest);
        total_ops += ops[uniq[k]];
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
            (unsigned long long)(hot_cover * (double)total_ops);
        if (order) {
          for (k = 0; k < nuniq; k++) order[k] = uniq[k];
          g_sort_ops = ops;
          qsort(order, (size_t)nuniq, sizeof(int), unit_ops_cmp);
          for (k = 0; k < nuniq; k++) {
            if (cum >= target) break;
            sel[order[k]] = 1;
            cum += ops[order[k]];
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
                  sel[ci] = 1;
                  changed = 1;
                }
              }
            }
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
      }
      free(ops);
    }
    for (i = 0; i < nfuncs; i++)
      if (sel[i]) cov_ops += prof_ops_of(g_prof, g_nprof, funcs[i].digest);

    /* The layout header the unit includes: the engine's offsets, the hash, and
       the unit's own `qjs_aot_layout` value. A unit compiled against a
       different engine's layouts has a different hash, which the engine
       refuses at bind (aot_unit_present). The static asserts that pin each
       offset against a mirror struct land with the first generated code that
       reads a field (stage 2.8); the hash already makes a drift unsound-free,
       because an old unit stops binding. */
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
          "#define QJS_AOT_TAG_LAZY_JSON %uu\n"
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
          "#define QJS_AOT_OFF_CTX_RT %uu\n"
          "#define QJS_AOT_OFF_OBJECT_FUNC_BYTECODE %uu\n"
          "#define QJS_AOT_CLASS_BYTECODE_FUNCTION %uu\n"
          "#define QJS_AOT_OFF_FNBC_AOT_FN %uu\n"
          "#define QJS_AOT_OFF_FNBC_REGVM_PIC %uu\n"
          "#define QJS_AOT_OFF_JSOBJECT_ARRAY_COUNT %uu\n"
          "#define QJS_AOT_OFF_JSOBJECT_ARRAY_VALUES %uu\n"
          "#define QJS_AOT_CLASS_ARRAY %uu\n"
          "#define QJS_AOT_OFF_FNBC_AOT_POLY %uu\n"
          "#define QJS_AOT_OFF_FNBC_AOT_POLY_SITES %uu\n"
          "#define QJS_AOT_AOT_POLY_WAYS %uu\n"
          "#define QJS_AOT_OFF_JSOBJECT_HOLEY_BYTE %uu\n"
          "#define QJS_AOT_JSOBJECT_HOLEY_MASK %uu\n\n"
          "#define QJS_AOT_OFF_REGRUN_RET_IP %uu\n"
          "#define QJS_AOT_OFF_REGRUN_FR %uu\n"
          "#define QJS_AOT_OFF_REGRUN_VAR_REFS %uu\n"
          "#define QJS_AOT_OFF_REGRUN_HDR_PACK %uu\n"
          "#define QJS_AOT_OFF_REGRUN_CAP_IP %uu\n"
          "#define QJS_AOT_OFF_FNBC_REALM %uu\n"
          "#define QJS_AOT_OFF_RT_REGVM_CUR %uu\n"
          "#define QJS_AOT_OFF_RT_STACK_LIMIT %uu\n"
          "#define QJS_AOT_OFF_OBJECT_FUNC_VAR_REFS %uu\n"
          "#define QJS_AOT_LIGHT_PREFIX_SLOTS %uu\n"
          "#define QJS_AOT_REGVM_FLAG_LIGHT %uu\n"
          "#define QJS_AOT_REGVM_FLAG_C_ENTRY %uu\n"
          "#define QJS_AOT_REGVM_FLAG_STACK %uu\n"
          "#define QJS_AOT_REGVM_FLAG_TRIVIAL_RET %uu\n"
          "#define QJS_AOT_REGVM_FLAG_SAME_REALM %uu\n"
          "#define QJS_AOT_RC_BACK %uu\n"
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
          lay.tag_lazy_json, lay.reg_bin_add, (unsigned)REG_BIN_SHL,
          (unsigned)REG_BIN_SAR, (unsigned)REG_BIN_SHR, (unsigned)REG_BIN_AND,
          (unsigned)REG_BIN_XOR, (unsigned)REG_BIN_OR, lay.reg_bin_sub,
          lay.reg_bin_mul, lay.reg_bin_div, lay.reg_bin_mod, lay.reg_bin_lt,
          lay.reg_bin_lte, lay.reg_bin_gt, lay.reg_bin_gte,
          lay.reg_bin_strict_eq, lay.reg_bin_strict_neq, lay.reg_bin_eq,
          lay.reg_bin_neq, lay.reg_cmp_invert, lay.reg_bin_is_undef,
          lay.reg_bin_is_not_undef, lay.reg_pic_own_w,
          lay.off_object_flags_byte, lay.object_is_prototype_mask,
          lay.reg_un_neg, lay.reg_un_inc, lay.off_regrun_b, lay.off_regrun_pic,
          lay.off_regrun_var_buf, lay.off_regrun_arg_buf, lay.off_regrun_func,
          lay.off_regrun_this_obj, lay.off_regrun_flags, lay.off_regrun_sf,
          lay.off_regrun_caller, lay.off_fnbc_regvm_code,
          lay.off_fnbc_regvm_pic_holder, lay.off_fnbc_var_count,
          lay.off_fnbc_arg_count, lay.off_fnbc_regvm_reg_count,
          lay.off_fnbc_var_ref_count, lay.off_rt_gv_cell_gen,
          lay.off_ctx_interrupt_counter, lay.off_ctx_rt,
          lay.off_object_func_bytecode, lay.class_bytecode_function,
          lay.off_fnbc_aot_fn, lay.off_fnbc_regvm_pic,
          lay.off_object_array_count, lay.off_object_array_values,
          lay.class_array, lay.off_fnbc_aot_poly, lay.off_fnbc_aot_poly_sites,
          lay.aot_poly_ways, lay.off_object_holey_byte, lay.object_holey_mask,
          lay.off_regrun_ret_ip, lay.off_regrun_fr, lay.off_regrun_var_refs,
          lay.off_regrun_hdr_pack, lay.off_regrun_cap_ip, lay.off_fnbc_realm,
          lay.off_rt_regvm_cur, lay.off_rt_stack_limit,
          lay.off_object_func_var_refs, lay.light_prefix_slots,
          lay.regvm_flag_light, lay.regvm_flag_c_entry, lay.regvm_flag_stack,
          lay.regvm_flag_trivial_ret, lay.regvm_flag_same_realm,
          lay.off_rc_back);
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
        "#define E2_TEXT __attribute__((section(\"e2text\")))\n"
        "extern char __start_e2text[] __attribute__((weak));\n"
        "extern char __stop_e2text[] __attribute__((weak));\n");
    /* Unconditional: the A/B switch reads the environment, and a unit that is
       compiled without the debug checks still has to do that. */
    fprintf(f, "#include <stdlib.h>\n");
    if (g_check_locals) fprintf(f, "#include <string.h>\n#include <stdio.h>\n");
    fprintf(f, "\n");
    /* The unit's function count, so the D4 counter arrays are sized exactly
       rather than to a guessed bound. In a split unit this text is the shared
       header, and the arrays are declared there and defined once in reg.c. */
    fprintf(f, "#define QJS_AOT_E2_NFUNCS %uu\n",
            nfuncs > 0 ? (unsigned)nfuncs : 1u);
    /* The function names, indexed the same way as the D4 counter arrays, so the
       teardown line can print one row per function and a test can assert on one
       by name. */
    {
      int ni;
      if (split_dir != NULL) {
        fprintf(f, "extern const char *const qjs_aot_e2_names[];\n");
      } else {
        fprintf(f, "static const char *const qjs_aot_e2_names[] = {\n");
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
        /* The A/B switch: with a specialised body in place, setting this
           environment variable runs every function through the generic one. */
        /* An exit burst this long means the specialised body is mispredicting
           more than it wins, so it hands the function back to the generic
           body. */
        "#define Q2_SPEC_FAIL_MAX 64\n"
        /* The A/B switch, and the enable switch for the specialised body. The
           specialised body is the default; QJS_AOT_SPEC=0 runs every function
           through the generic one for comparison. Its polymorphic compare
           chain is disabled (see q2_poly_site): with it, DeltaBlue's compiled
           body reported a constraint cycle the interpreter does not. */
        "static int q2_no_spec, q2_spec_on;\n"
        /* E2 unit counters (D14 part 2). `e2_g8_hits` counts a dispatch to a
           pc that is not an instruction start; it must stay 0. */
        "static unsigned long e2_g8_hits;\n"
        /* The D4 switch-off counters, exactly one entry per function in the
           table. A function whose speculative body keeps exiting, or keeps
           refusing at entry, is switched off for good and its calls go straight
           to the generic body.

           In a split unit this text goes into the shared header, which every
           part includes, so the arrays must be declared here and defined once
           (in reg.c) rather than being `static` -- otherwise each part gets its
           own copy and its own counters. */

        "#ifdef QJS_AOT_SPLIT_UNIT\n"
        "extern uint64_t e2_ent[QJS_AOT_E2_NFUNCS];\n"
        "extern uint32_t e2_exi[QJS_AOT_E2_NFUNCS], e2_ref[QJS_AOT_E2_NFUNCS];\n"
        "extern uint8_t e2_off[QJS_AOT_E2_NFUNCS];\n"
        "extern uint32_t e2_han[QJS_AOT_E2_NFUNCS];\n"
        "#else\n"
        "static uint64_t e2_ent[QJS_AOT_E2_NFUNCS];\n"
        "static uint32_t e2_exi[QJS_AOT_E2_NFUNCS], e2_ref[QJS_AOT_E2_NFUNCS];\n"
        "static uint8_t e2_off[QJS_AOT_E2_NFUNCS];\n"
        "static uint32_t e2_han[QJS_AOT_E2_NFUNCS];\n"
        "/* How many times the speculative bodies have been entered. A case that\n"
        "   means to exercise a spec path asserts this is non-zero: a body that is\n"
        "   never entered passes every check for the wrong reason, which is how\n"
        "   the entry-refusal bug survived a whole step. */\n"
        "unsigned long long qjs_aot_e2_enters(void)\n"
        "{\n"
        "  unsigned long long t = 0;\n"
        "  int i;\n"
        "  for (i = 0; i < QJS_AOT_E2_NFUNCS; i++) t += e2_ent[i];\n"
        "  return t;\n"
        "}\n"
        "/* With QJS_AOT_E2_STATS=1 in the environment, print the E2 counters at\n"
        "   teardown so a test can prove the speculative body actually ran. Without\n"
        "   it a case that means to exercise a spec path passes even when the body\n"
        "   is never entered -- which is exactly how the entry-refusal bug survived\n"
        "   a whole step. */\n"
        "#ifdef QJS_AOT_SPLIT_UNIT\n"
        "void e2_stats_dump(void);\n"
        "#else\n"
        "static void __attribute__((destructor)) e2_stats_dump(void)\n"
        "{\n"
        "  unsigned long long en = 0, ex = 0, rf = 0, off = 0, hn = 0;\n"
        "  const char *e = getenv(\"QJS_AOT_E2_STATS\");\n"
        "  int i;\n"
        "  if (e == NULL || e[0] != '1') return;\n"
        "  for (i = 0; i < QJS_AOT_E2_NFUNCS; i++) {\n"
        "    en += e2_ent[i]; ex += e2_exi[i]; rf += e2_ref[i]; off += e2_off[i];\n"
        "    hn += e2_han[i];\n"
        "  }\n"
        "  fprintf(stderr, \"e2-stats enters=%%llu exits=%%llu handoffs=%%llu refuses=%%llu off=%%llu\\n\",\n"
        "          en, ex, hn, rf, off);\n"
        /* Per function too, so the single unit answers the same question the split
           unit's reg.c does. A hand-off is CORRECT, merely slower, so a fix that
           only makes things faster can ONLY be tested by counting one function. */
        "  for (i = 0; i < QJS_AOT_E2_NFUNCS; i++) {\n"
        "    if (e2_ent[i] == 0 && e2_ref[i] == 0) continue;\n"
        "    fprintf(stderr, \"e2-stats fn=%%s enters=%%llu exits=%%llu handoffs=%%llu refuses=%%llu off=%%u idx=%%d\\n\",\n"
        "            qjs_aot_e2_names[i] ? qjs_aot_e2_names[i] : \"?\",\n"
        "            (unsigned long long)e2_ent[i], (unsigned long long)e2_exi[i],\n"
        "            (unsigned long long)e2_han[i], (unsigned long long)e2_ref[i],\n"
        "            (unsigned)e2_off[i], i);\n"
        "  }\n"
        "}\n"
        "#endif\n"
        "#endif\n"
        "#define E2_ENTER_NOTE(fi) (e2_ent[fi]++)\n"
        "/* More than 256 exits and in more than 1/16 of the calls: the guards do\n"
        "   not match the data this function sees. */\n"
        /* QJS_AOT_E2_EXIT_LOG=1: the first time each guard stub is taken, one
           line naming it and the tag its guard saw (-100 where there is no
           value: an overflow exit, or a stub whose guard named none). Read
           once; one load and a branch when off. The work is out of line: at
           -O0 every inline copy took its own stack slots, and the stubs of a big
           function grew its frame enough to overflow earleyboyer's recursion. */
        "static int e2_exit_log_v = -1;\n"
        "static void __attribute__((noinline, cold, unused))\n"
        "e2_exit_log(const char *hex, const char *kind, int pc, int tag, int *seen)\n"
        "{\n"
        "  if (e2_exit_log_v < 0) {\n"
        "    const char *e_ = getenv(\"QJS_AOT_E2_EXIT_LOG\");\n"
        "    e2_exit_log_v = e_ != NULL && e_[0] == '1';\n"
        "  }\n"
        "  if (!e2_exit_log_v) return;\n"
        "  *seen = 1;\n"
        "  fprintf(stderr, \"e2-exit fn=%%s kind=%%s pc=%%d tag=%%d\\n\", hex, kind,\n"
        "          pc, tag);\n"
        "}\n"
        "#define E2_EXIT_LOG(hex, kind, pc, tag) do { static int e2_xl_; \\\n"
        "    if (__builtin_expect(e2_exit_log_v != 0, 0) && !e2_xl_) \\\n"
        "      e2_exit_log((hex), (kind), (pc), (int)(tag), &e2_xl_); } while (0)\n"
        "#define E2_EXIT_NOTE(fi) do { if (++e2_exi[fi] > 256 && \\\n"
        "    (uint64_t)e2_exi[fi] * 16 > e2_ent[fi]) e2_off[fi] = 1; } while (0)\n"
        "/* A HAND-OFF is not a deopt: it is the exit at an instruction the spec\n"
        "   body does not lower yet (or at a terminator 2b has not reached). While\n"
        "   E8 is incomplete almost every function has one, so feeding those to\n"
        "   D4 switched every spec body off after 257 calls and every measurement\n"
        "   timed the generic body. They are counted, never acted on. */\n"
        "#define E2_HANDOFF_NOTE(fi) (e2_han[fi]++)\n"
        "/* Entry refused in more than half the calls: nothing to speculate on. */\n"
        "#define E2_REFUSE_NOTE(fi) do { if (++e2_ref[fi] > 256 && \\\n"
        "    (uint64_t)e2_ref[fi] * 2 > e2_ent[fi]) e2_off[fi] = 1; } while (0)\n"
        /* The D12 dynamic check, compiled into the unit only with -DE2_POISON.
           It compares every register an op call was not allowed to write
           against a copy taken before the call, by tag and then by kind, so a
           handler that writes a register it did not declare is caught at the
           site rather than as a wrong answer later. */
        "#if defined(E2_POISON) && E2_POISON\n"
        "#include <stdio.h>\n"
        "#include <stdlib.h>\n"
        "/* Equal as values: the tag decides how the payload is compared, so "
        "a\n"
        "   NaN payload or a pointer is never memcmp'd (invariant 12). */\n"
        "static int e2_same_value(JSValue a, JSValue b)\n"
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
        "#define E2_SENTINEL JS_MKVAL(JS_TAG_INT, 0x5e5e5e5e)\n"
        "static long e2_poison_seen;\n"
        "static void e2_poison_abort(const char *op, int pc, int reg,\n"
        "                            JSValue got)\n"
        "{\n"
        "  if (e2_poison_seen++ < 200) {\n"
        "    int tg = JS_VALUE_GET_TAG(got);\n"
        "    fprintf(stderr, \"e2 poison: %%s pc %%d wrote r%%d (now tag %%d "
        "int %%d)\\n\",\n"
        "            op, pc, reg, tg, tg == JS_TAG_INT ? JS_VALUE_GET_INT(got) "
        ": -1);\n"
        "  }\n"

        "#ifdef E2_POISON_QUIET\n"
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
        "static unsigned long e2_poison_calls, e2_poison_regs;\n"
        "static __attribute__((noinline)) void e2_poison_pre(JSValue *fr, int "
        "n,\n"
        "    JSValue *copy, uint8_t *pz, const char *pm)\n"
        "{\n"
        "  e2_poison_calls++;\n"
        "  for (int r = 0; r < n; r++) {\n"
        "    copy[r] = AOT_REG(fr, r * 16);\n"
        "    pz[r] = 0;\n"
        "    if (pm[r] == '1' && !JS_VALUE_HAS_REF_COUNT(copy[r])) {\n"
        "      AOT_REG(fr, r * 16) = E2_SENTINEL;\n"
        "      pz[r] = 1;\n"
        "      e2_poison_regs++;\n"
        "    }\n"
        "  }\n"
        "}\n"
        "static __attribute__((noinline)) void e2_poison_post(JSValue *fr, int "
        "n,\n"
        "    const JSValue *copy, const uint8_t *pz, const char *wm,\n"
        "    const char *op, int pc)\n"
        "{\n"
        "  for (int r = 0; r < n; r++) {\n"
        "    if (wm[r] == '1') continue;\n"
        "    if (!e2_same_value(AOT_REG(fr, r * 16), pz[r] ? E2_SENTINEL : "
        "copy[r])) {\n"
        "      e2_poison_abort(op, pc, r, AOT_REG(fr, r * 16));\n"
        "    } else if (pz[r]) {\n"
        "      AOT_REG(fr, r * 16) = copy[r];\n"
        "    }\n"
        "  }\n"
        "}\n"
        "static void __attribute__((destructor)) e2_poison_stats(void)\n"
        "{\n"
        "  fprintf(stderr, \"e2 poison-stats: calls=%%lu poisoned=%%lu\\n\",\n"
        "          e2_poison_calls, e2_poison_regs);\n"
        "}\n"
        "#endif\n"
        /* Per-site hit/miss counters (compiled with -DE2_COUNT=1). One pair per
           inline fast path, so a lowering that never hits is visible as data
           rather than inferred from a benchmark. A site is named by its
           function's digest, the instruction's pc and the op, which is what the
           generator's op-call weight lines use, so the two can be read side by
           side. Disabled by default: the counters are a diagnostic, not a gate,
           and the lookup is linear because a function has only a few sites. */
        "#if defined(E2_COUNT) && E2_COUNT\n"
        "#include <stdio.h>\n"
        "#include <string.h>\n"
        "typedef struct { const char *hex; unsigned pc; uint16_t rop;\n"
        "                 unsigned long long hit, miss; } E2SiteCtr;\n"
        "static E2SiteCtr e2_sites[8192];\n"
        "static unsigned e2_nsites;\n"
        "static const char *const e2_rop_names[] = {\n");
    {
      int r;
      for (r = 0; r < ROP_COUNT; r++) {
        char nm[64];
        e2_rop_name((uint16_t)r, nm, sizeof(nm));
        fprintf(f, "  \"%s\",%s", nm, (r % 8 == 7) ? "\n" : " ");
      }
    }
    fprintf(
        f,
        "\n};\n"
        "static void e2_count_note(const char *hex, unsigned pc, unsigned "
        "rop,\n"
        "                          int hit)\n"
        "{\n"
        "  unsigned i;\n"
        "  for (i = 0; i < e2_nsites; i++)\n"
        "    if (e2_sites[i].pc == pc && e2_sites[i].rop == rop &&\n"
        "        e2_sites[i].hex == hex) {\n"
        "      if (hit) e2_sites[i].hit++; else e2_sites[i].miss++;\n"
        "      return;\n"
        "    }\n"
        "  if (e2_nsites >= sizeof(e2_sites) / sizeof(e2_sites[0])) return;\n"
        "  i = e2_nsites++;\n"
        "  e2_sites[i].hex = hex; e2_sites[i].pc = pc;\n"
        "  e2_sites[i].rop = (uint16_t)rop;\n"
        "  e2_sites[i].hit = hit ? 1 : 0; e2_sites[i].miss = hit ? 0 : 1;\n"
        "}\n"
        "static void __attribute__((destructor)) e2_count_dump(void)\n"
        "{\n"
        "  unsigned i;\n"
        "  for (i = 0; i < e2_nsites; i++)\n"
        "    fprintf(stderr, \"e2 site %%s pc %%u %%s hit=%%llu "
        "miss=%%llu\\n\",\n"
        "            e2_sites[i].hex, e2_sites[i].pc,\n"
        "            e2_rop_names[e2_sites[i].rop],\n"
        "            e2_sites[i].hit, e2_sites[i].miss);\n"
        "}\n"
        "#define E2_CTR(hex, pc, rop, hit) e2_count_note((hex), (pc), (rop), "
        "(hit))\n"
        /* Why q2_put_field_hit declined, so a store site that never hits can
           be explained rather than guessed at. */
        "#define E2_PUT_SHAPE   0\n"
        "#define E2_PUT_KIND    1\n"
        "#define E2_PUT_PROTO   2\n"
        "#define E2_PUT_N 3\n"
        "static unsigned long long e2_put_why[E2_PUT_N];\n"
        "static void e2_put_why_note(int why) { e2_put_why[why]++; }\n"
        "static const char *const e2_put_why_names[E2_PUT_N] = {\n"
        "  \"shape\", \"kind-not-own-w\", \"receiver-prototype\"\n"
        "};\n"
        "static void __attribute__((destructor)) e2_put_why_dump(void)\n"
        "{\n"
        "  int i; unsigned long long tot = 0;\n"
        "  for (i = 0; i < E2_PUT_N; i++) tot += e2_put_why[i];\n"
        "  if (!tot) return;\n"
        "  for (i = 0; i < E2_PUT_N; i++) {\n"
        "    if (e2_put_why[i]) {\n"
        "      fprintf(stderr, \"e2 putfield-why %%s %%llu (%%llu%%%%)\\n\",\n"
        "              e2_put_why_names[i], e2_put_why[i],\n"
        "              100ull * e2_put_why[i] / tot);\n"
        "    }\n"
        "  }\n"
        "}\n"
        "#define E2_PUTWHY(w) e2_put_why_note(w)\n"
        "#else\n"
        "#define E2_CTR(hex, pc, rop, hit) ((void)0)\n"
        "#define E2_PUTWHY(w) ((void)0)\n"
        "#endif\n"
        /* D10's frame seal. The engine's frame area has a top marker, and a
           compiled function may be running on a frame above it; a native call
           made from there lets the callee's own calls into the engine allocate
           a frame at that marker, writing over the caller's registers. Sealing
           publishes this activation's frame so the marker sits above it.
           A frame on the C stack needs no seal: the marker already accounts for
           it. Once per activation, not per `goto dispatch`. */
        "#ifndef E2_SEAL\n"
        "#define E2_SEAL(ctx, f) do { if (!(*(uint16_t *)((uint8_t *)(f) + "
        "QJS_AOT_OFF_REGRUN_FLAGS) & QJS_AOT_REGVM_FLAG_STACK)) "
        "qjs_aot_seal(ctx, f); } while (0)\n"
        "#endif\n"
        /* The stack margin a native entry keeps free for the throw it may have
           to perform. 4 KB crashed before; 64 KB is what the call-path
           experiments used and what the design states. */
        "#ifndef E2_STACK_MARGIN\n"
        "#define E2_STACK_MARGIN (64 * 1024)\n"
        "#endif\n"
        /* A double local stored where another function or entry can see it is
           boxed as the interpreter's arithmetic would box it: an integral
           value that fits int32 (and is not -0) is an INT, so a guard typed
           from the profile does not see FLOAT64 for the same number. */
        /* The shape of a monomorphic group whose cells were all empty at entry:
           no object has it, so every access of the group exits. */
        "#define E2_NO_SHAPE ((const void *)1)\n"
        "#define E2_BOX_F64(d) ({ double d_ = (d); "
        "qjs_aot_box_num(d_, qjs_aot_fitsi(d_)); })\n"
        /* The `undefined` word a native entry fills its frame with, as one
           static constant so the fill is a copy of a known word. */
        "static const JSValue e2_undefined_value = { { .int32 = 0 }, "
        "JS_TAG_UNDEFINED };\n"
        "static void __attribute__((constructor)) q2_no_spec_init(void)\n"
        "{\n"
        "  const char *e = getenv(\"QJS_AOT_NO_SPEC\");\n"
        "  const char *s = getenv(\"QJS_AOT_SPEC\");\n"
        "  q2_no_spec = (e != NULL && e[0] != '0');\n"
        "  q2_spec_on = (s == NULL || s[0] != '0');\n"
        "}\n\n"
        /* A numeric element read: the profile saw only ints (or only doubles)
           at this pc, so a class check, a bounds check and the load are enough;
           a hole cannot be there or the profile would say so. */
        "static inline __attribute__((always_inline)) int q2_elem_num(JSValue "
        "arr, JSValue k, JSValue *out)\n"
        "{\n"
        "  const unsigned char *p;\n"
        "  uint32_t i;\n"
        "  if (JS_VALUE_GET_TAG(arr) != JS_TAG_OBJECT) return 0;\n"
        "  if (JS_VALUE_GET_TAG(k) != JS_TAG_INT) return 0;\n"
        "  p = (const unsigned char *)JS_VALUE_GET_PTR(arr);\n"
        "  i = (uint32_t)JS_VALUE_GET_INT(k);\n"
        "  if (*(const uint16_t *)(p + QJS_AOT_OFF_JSOBJECT_CLASS_ID) !=\n"
        "      QJS_AOT_CLASS_ARRAY)\n"
        "    return 0;\n"
        "  if (p[QJS_AOT_OFF_JSOBJECT_HOLEY_BYTE] & "
        "QJS_AOT_JSOBJECT_HOLEY_MASK) return 0;\n"
        "  if (i >= *(const uint32_t *)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_COUNT)) "
        "return 0;\n"
        "  *out = (*(JSValue *const *)(p + "
        "QJS_AOT_OFF_JSOBJECT_ARRAY_VALUES))[i];\n"
        "  if (JS_VALUE_GET_TAG(*out) != JS_TAG_INT &&\n"
        "      JS_VALUE_GET_TAG(*out) != JS_TAG_FLOAT64)\n"
        "    return 0;\n"
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
        "static inline uint32_t q2_poly_off(const void *e, int k)\n"
        "{\n"
        "  return *(const uint32_t *)((const unsigned char *)e + (size_t)k * "
        "16 + 8);\n"
        "}\n\n"
        /* A site's cache cell, read at the specialised body's entry: the
           receiver shape the profile established and the slot offset that came
           with it. NULL/0 unless the cell is an own (read) or own-writable
           (write) hit, because only those mean "this shape, this offset". */
        "static inline const void *q2_cell_shape(const void *pic, int k)\n"
        "{\n"
        "  const unsigned char *e;\n"
        "  uint16_t kind;\n"
        "  if (pic == NULL || (unsigned)k == QJS_AOT_REG_IC_NONE) return "
        "NULL;\n"
        "  e = (const unsigned char *)pic + (size_t)k * "
        "QJS_AOT_SIZEOF_JSREGPIC;\n"
        "  kind = *(const uint16_t *)(e + QJS_AOT_OFF_JSREGPIC_KIND);\n"
        "  if (kind != QJS_AOT_REG_PIC_OWN && kind != QJS_AOT_REG_PIC_OWN_W) "
        "return NULL;\n"
        "  return *(const void *const *)e;\n"
        "}\n\n"
        "static inline int q2_cell_kind(const void *pic, int k)\n"
        "{\n"
        "  if (pic == NULL || (unsigned)k == QJS_AOT_REG_IC_NONE) return -1;\n"
        "  return *(const uint16_t *)((const unsigned char *)pic + (size_t)k "
        "* QJS_AOT_SIZEOF_JSREGPIC + QJS_AOT_OFF_JSREGPIC_KIND);\n"
        "}\n\n"
        "static inline const void *q2_cell_wshape(const void *pic, int k)\n"
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
        "static inline JSValue *q2_slotp(JSValue v, uint32_t off)\n"
        "{\n"
        "  const unsigned char *p = (const unsigned char "
        "*)JS_VALUE_GET_PTR(v);\n"
        "  const unsigned char *prop =\n"
        "      *(const unsigned char *const *)(p + "
        "QJS_AOT_OFF_JSOBJECT_PROP);\n"
        "  return (JSValue *)(prop + off);\n"
        "}\n\n"
        /* A fast array's element read: class, bounds, hole and lazy-JSON
           checks, then the element itself, borrowed. No reference is taken,
           so the caller must consume it before the next safepoint, exactly
           like the cache-hit field read. */
        "static inline __attribute__((always_inline)) int q2_elem_hit(JSValue "
        "arr, JSValue k, JSValue *out)\n"
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
        "    return 0;\n"
        "  if (p[QJS_AOT_OFF_JSOBJECT_HOLEY_BYTE] & "
        "QJS_AOT_JSOBJECT_HOLEY_MASK) return 0;\n"
        "  if (i >= *(const uint32_t *)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_COUNT)) "
        "return 0;\n"
        "  v = (*(JSValue *const *)(p + "
        "QJS_AOT_OFF_JSOBJECT_ARRAY_VALUES))[i];\n"
        "  if (JS_VALUE_GET_TAG(v) == JS_TAG_UNINITIALIZED) return 0;\n"
        "  if (JS_VALUE_GET_TAG(v) == QJS_AOT_TAG_LAZY_JSON) return 0;\n"
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
        "  if (p[QJS_AOT_OFF_JSOBJECT_HOLEY_BYTE] & "
        "QJS_AOT_JSOBJECT_HOLEY_MASK) return 0;\n"
        "  r->vals = *(const unsigned char *const *)(p + "
        "QJS_AOT_OFF_JSOBJECT_ARRAY_VALUES);\n"
        "  r->count = *(const uint32_t *)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_COUNT);\n"
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
        "    return 0;\n"
        "  if (p[QJS_AOT_OFF_JSOBJECT_HOLEY_BYTE] & "
        "QJS_AOT_JSOBJECT_HOLEY_MASK) return 0;\n"
        "  if (i >= *(const uint32_t *)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_COUNT)) "
        "return 0;\n"
        "  slot = &(*(JSValue **)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_VALUES))[i];\n"
        "  if (JS_VALUE_GET_TAG(*slot) < 0 ||\n"
        "      JS_VALUE_GET_TAG(*slot) == JS_TAG_UNINITIALIZED)\n"
        "    return 0;\n"
        "  *slot = v;\n"
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
        "q2_array_length(JSValue v, double *out)\n"
        "{\n"
        "  const unsigned char *p, *prop;\n"
        "  JSValue len;\n"
        "  if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT) return 0;\n"
        "  p = (const unsigned char *)JS_VALUE_GET_PTR(v);\n"
        "  if (*(const uint16_t *)(p + QJS_AOT_OFF_JSOBJECT_CLASS_ID) !=\n"
        "      QJS_AOT_CLASS_ARRAY)\n"
        "    return 0;\n"
        "  if (p[QJS_AOT_OFF_JSOBJECT_HOLEY_BYTE] & "
        "QJS_AOT_JSOBJECT_HOLEY_MASK) return 0;\n"
        "  prop = *(const unsigned char *const *)(p + "
        "QJS_AOT_OFF_JSOBJECT_PROP);\n"
        "  len = *(const JSValue *)(prop + QJS_AOT_OFF_JSPROPERTY_U_VALUE);\n"
        "  if (JS_VALUE_GET_TAG(len) != JS_TAG_INT || JS_VALUE_GET_INT(len) < "
        "0) return 0;\n"
        "  /* A non-holey array's count is its length; a gap would make them "
        "differ,\n"
        "     and `holey` is set for that case, so the count must match. */\n"
        "  if ((uint32_t)JS_VALUE_GET_INT(len) !=\n"
        "      *(const uint32_t *)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_COUNT))\n"
        "    return 0;\n"
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
    /* QJS_AOT_E2_FORCE_MISS: forces every spec-body inline cache hit to fail, so
       the miss path (the op-call wrapper) runs on the EXISTING tests. The test is
       a static load, so it costs nothing when unset, and the environment is read
       once per process on the first spec-body entry. */
    fprintf(
        f,
        "static int q2_force_miss_cache = -1;\n"
        "static inline __attribute__((always_inline)) int q2_env1(const char *n)\n"
        "{\n"
        "  const char *v = getenv(n);\n"
        "  return v != NULL && v[0] == '1';\n"
        "}\n"
        "#define q2_force_miss q2_force_miss_cache\n"
        "#define Q2_FORCE_MISS (q2_force_miss > 0)\n"
        /* QJS_AOT_E2_SPEC_FNS="<lo>-<hi>" allows the spec body only for function
           indices in that range, so a miscompile can be pinned to ONE function by
           halving the range. A run-time test, so a stale unit cannot fake it. */
        "static int q2_spec_fns_lo = -2, q2_spec_fns_hi = -2;\n"
        "static inline __attribute__((always_inline)) int q2_spec_fns_out(int fi)\n"
        "{\n"
        "  if (q2_spec_fns_lo == -2) {\n"
        "    const char *v = getenv(\"QJS_AOT_E2_SPEC_FNS\");\n"
        "    q2_spec_fns_lo = -1; q2_spec_fns_hi = -1;\n"
        "    if (v != NULL && v[0] != 0) {\n"
        "      int lo = -1, hi = -1;\n"
        "      if (sscanf(v, \"%%d-%%d\", &lo, &hi) == 2) {\n"
        "        q2_spec_fns_lo = lo; q2_spec_fns_hi = hi;\n"
        "        if (getenv(\"QJS_AOT_E2_SPEC_FNS_VERBOSE\"))\n"
        "          fprintf(stderr, \"e2 spec-fns %%d..%%d\\n\", lo, hi);\n"
        "      }\n"
        "    }\n"
        "  }\n"
        "  if (q2_spec_fns_lo < 0) return 0;\n"
        "  return fi < q2_spec_fns_lo || fi > q2_spec_fns_hi;\n"
        "}\n\n");
    /* The sweep's coverage record: which guard sites a run reached. Only a sweep
       unit calls it. Each translation unit keeps its own bitmap and appends the
       ids it saw to $QJS_AOT_E2_SWEEP_OUT at exit, so a split unit needs no
       shared array and the run's own output is untouched. */
    if (g_deopt_everywhere)
      fprintf(
          f,
          "#define E2_SWEEP_MAX (1u << 20)\n"
          "static uint8_t e2_sw_seen[E2_SWEEP_MAX / 8];\n"
          "static inline int e2_sweep_hit(JSContext *ctx, uint32_t sid)\n"
          "{\n"
          "  if (sid < E2_SWEEP_MAX) e2_sw_seen[sid >> 3] |= (uint8_t)(1u << "
          "(sid & 7));\n"
          "  return qjs_aot_should_deopt(ctx, sid);\n"
          "}\n"
          "static void __attribute__((destructor, unused)) "
          "e2_sweep_dump(void)\n"
          "{\n"
          "  const char *p = getenv(\"QJS_AOT_E2_SWEEP_OUT\");\n"
          "  FILE *o;\n"
          "  uint32_t i;\n"
          "  if (p == NULL || p[0] == 0) return;\n"
          "  o = fopen(p, \"a\");\n"
          "  if (o == NULL) return;\n"
          "  for (i = 0; i < E2_SWEEP_MAX; i++)\n"
          "    if (e2_sw_seen[i >> 3] & (1u << (i & 7))) fprintf(o, "
          "\"%%u\\n\", i);\n"
          "  fclose(o);\n"
          "}\n\n");
    fprintf(
        f,
        "static inline __attribute__((always_inline)) int "
        "q2_own_read_hit(const void *pic, void *const *holders,\n"
        "                       JSValue recv, int site, int nsites,\n"
        "                       JSValue *out)\n"
        "{\n"
        "  const unsigned char *obj, *e, *holder, *prop;\n"
        "  uint32_t slot_off;\n"
        "  JSValue v;\n"
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
        "    holder = *(const unsigned char *const *)(*(const unsigned char "
        "*const *)(\n"
        "                 obj + QJS_AOT_OFF_JSOBJECT_SHAPE) + "
        "QJS_AOT_OFF_JSSHAPE_PROTO);\n"
        "    if (holder == NULL ||\n"
        "        *(const void *const *)(holder + QJS_AOT_OFF_JSOBJECT_SHAPE) "
        "!=\n"
        "            holders[site]) {\n"
        "      return 0;\n"
        "    }\n"
        "    obj = holder;\n"
        "  }\n"
        "  prop = *(const unsigned char *const *)(obj + "
        "QJS_AOT_OFF_JSOBJECT_PROP);\n"
        "  slot_off = *(const uint32_t *)(e + QJS_AOT_OFF_JSREGPIC_SLOT_OFF);\n"
        "  v = *(const JSValue *)(prop + slot_off);\n"
        "  if (JS_VALUE_GET_TAG(v) == QJS_AOT_TAG_LAZY_JSON) {\n"
        "    return 0;\n"
        "  }\n"
        "#if !QJS_DRC\n"
        "  if (JS_VALUE_GET_TAG(v) < 0) {\n"
        "    return 0; /* registers own: the slow path takes the reference */\n"
        "  }\n"
        "#endif\n"
        "  *out = v;\n"
        "  return 1;\n"
        "}\n\n");
    /* A reference taken inline: the count is an int QJS_AOT_RC_BACK bytes before
       the object (the layout publishes the offset), and this is js_dup()'s body,
       so a counted store or a return costs no call into the engine. Release
       stays JS_FreeValue, whose zero path parks the value. */
    fprintf(
        f,
        "static inline __attribute__((always_inline)) void q2_dup(JSValue v)\n"
        "{\n"
        "  if (JS_VALUE_HAS_REF_COUNT(v))\n"
        "    ++*(int *)((uint8_t *)JS_VALUE_GET_PTR(v) - QJS_AOT_RC_BACK);\n"
        "}\n\n");
    /* A grouped field store, after the caller has checked the shape: the same
       store the engine's fast path makes, minus the cache lookup (the offset
       comes from the group's cell). The new value is duplicated before the old
       one is released, so storing a slot's own value cannot free it. A
       prototype receiver goes to the engine, which invalidates the prototype
       epoch, so the caller exits on it first. */
    fprintf(
        f,
        "static inline __attribute__((always_inline)) int q2_is_proto(JSValue "
        "v)\n"
        "{\n"
        "  return (((const unsigned char *)JS_VALUE_GET_PTR(v))"
        "[QJS_AOT_OFF_JSOBJECT_FLAGS_BYTE] &\n"
        "          QJS_AOT_JSOBJECT_IS_PROTOTYPE_MASK) != 0;\n"
        "}\n"
        "static inline __attribute__((always_inline)) void "
        "q2_put_slot(JSContext *ctx, JSValue recv,\n"
        "                                                   uint32_t off, "
        "JSValue v)\n"
        "{\n"
        "  JSValue *slot = q2_slotp(recv, off);\n"
        "  JSValue old = *slot;\n"
        "  q2_dup(v);\n"
        "  *slot = v;\n"
        "  if (JS_VALUE_HAS_REF_COUNT(old)) JS_FreeValue(ctx, old);\n"
        "}\n\n");
    /* The inline own-slot store hit: the interpreter's REG_PIC_OWN_W test on
       an object that is not a prototype, and the store the engine's own fast
       path makes (`*slot = js_dup(v); JS_FreeValue(ctx, old)`). The new value
       is duplicated before the old one is released, so storing a slot's own
       value cannot free it. A prototype receiver (whose store invalidates the
       prototype epoch) or a miss returns 0 and the caller takes the engine's
       store. */
    fprintf(
        f,
        "static inline __attribute__((always_inline)) int "
        "q2_put_field_hit(JSContext *ctx, const void *pic,\n"
        "                                   JSValue recv, int site, JSValue "
        "v)\n"
        "{\n"
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
        "    E2_PUTWHY(E2_PUT_SHAPE);\n"
        "    return 0;\n"
        "  }\n"
        "  if (*(const uint16_t *)(e + QJS_AOT_OFF_JSREGPIC_KIND) !=\n"
        "          QJS_AOT_REG_PIC_OWN_W) {\n"
        "    E2_PUTWHY(E2_PUT_KIND);\n"
        "    return 0;\n"
        "  }\n"
        "  if ((obj[QJS_AOT_OFF_JSOBJECT_FLAGS_BYTE] &\n"
        "       QJS_AOT_JSOBJECT_IS_PROTOTYPE_MASK) != 0) {\n"
        "    E2_PUTWHY(E2_PUT_PROTO);\n"
        "    return 0;\n"
        "  }\n"
        "  slot = (JSValue *)(*(unsigned char *const *)(obj + "
        "QJS_AOT_OFF_JSOBJECT_PROP) +\n"
        "                     *(const uint32_t *)(e + "
        "QJS_AOT_OFF_JSREGPIC_SLOT_OFF));\n"
        "  {\n"
        "    JSValue old = *slot;\n"
        "    q2_dup(v);\n"
        "    *slot = v;\n"
        "    if (JS_VALUE_HAS_REF_COUNT(old)) {\n"
        "      JS_FreeValue(ctx, old);\n"
        "    }\n"
        "  }\n"
        "  return 1;\n"
        "}\n\n");
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
    /* A direct call to a leaf compiled into this unit: the callee must be the
       bytecode function whose bound entry is `entry`, which only a function
       with that digest can be. Its caches are the leaf's. */
    fprintf(
        f,
        "static inline __attribute__((always_inline)) int "
        "q2_leaf_callee(JSValue fnv, const void *entry,\n"
        "                                 const void **pic, void *const "
        "**holders)\n"
        "{\n"
        "  const unsigned char *o, *b;\n"
        "\n"
        "  if (JS_VALUE_GET_TAG(fnv) != JS_TAG_OBJECT) {\n"
        "    return 0;\n"
        "  }\n"
        "  o = (const unsigned char *)JS_VALUE_GET_PTR(fnv);\n"
        "  if (*(const uint16_t *)(o + QJS_AOT_OFF_JSOBJECT_CLASS_ID) !=\n"
        "      QJS_AOT_CLASS_BYTECODE_FUNCTION) {\n"
        "    return 0;\n"
        "  }\n"
        "  b = *(const unsigned char *const *)(o + "
        "QJS_AOT_OFF_OBJECT_FUNC_BYTECODE);\n"
        "  if (*(const void *const *)(b + QJS_AOT_OFF_FNBC_AOT_FN) != entry) "
        "{\n"
        "    return 0;\n"
        "  }\n"
        "  *pic = *(const void *const *)(b + QJS_AOT_OFF_FNBC_REGVM_PIC);\n"
        "  *holders = *(void *const *const *)(b + "
        "QJS_AOT_OFF_FNBC_REGVM_PIC_HOLDER);\n"
        "  return 1;\n"
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
        "    if (sub == QJS_AOT_REG_UN_NEG) {\n"
        "      if (i == 0 || i == INT32_MIN) return 0;\n"
        "      *out = JS_MKVAL(JS_TAG_INT, -i);\n"
        "      return 1;\n"
        "    }\n"
        "    if (sub == QJS_AOT_REG_UN_INC) {\n"
        "      if (i == INT32_MAX) return 0;\n"
        "      *out = JS_MKVAL(JS_TAG_INT, i + 1);\n"
        "      return 1;\n"
        "    }\n"
        /* QJS_AOT_REG_UN_DEC is INC + 1: the engine's enum is NEG, PLUS, NOT,
           LNOT, TYPEOF, ... INC, DEC, and the layout header carries NEG and INC
           because they are the two the old templates needed. */
        "    if (sub == QJS_AOT_REG_UN_INC + 1) {\n"
        "      if (i == INT32_MIN) return 0;\n"
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
        "    case QJS_AOT_REG_BIN_STRICT_EQ: *out = JS_MKVAL(JS_TAG_BOOL, d1 "
        "== d2); return 1;\n"
        "    case QJS_AOT_REG_BIN_STRICT_NEQ: *out = JS_MKVAL(JS_TAG_BOOL, d1 "
        "!= d2); return 1;\n"
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
        "    case QJS_AOT_REG_BIN_LT: *out = JS_MKVAL(JS_TAG_BOOL, v1 < v2); "
        "return 1;\n"
        "    case QJS_AOT_REG_BIN_LTE: *out = JS_MKVAL(JS_TAG_BOOL, v1 <= v2); "
        "return 1;\n"
        "    case QJS_AOT_REG_BIN_GT: *out = JS_MKVAL(JS_TAG_BOOL, v1 > v2); "
        "return 1;\n"
        "    case QJS_AOT_REG_BIN_GTE: *out = JS_MKVAL(JS_TAG_BOOL, v1 >= v2); "
        "return 1;\n"
        "    case QJS_AOT_REG_BIN_STRICT_EQ: *out = JS_MKVAL(JS_TAG_BOOL, v1 "
        "== v2); return 1;\n"
        "    case QJS_AOT_REG_BIN_STRICT_NEQ: *out = JS_MKVAL(JS_TAG_BOOL, v1 "
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
        "}\n\n"
        "static inline __attribute__((always_inline)) int q2_branch_cmp(int "
        "sub, JSValue a, JSValue b, int *take)\n"
        "{\n"
        "  int r;\n"
        "  if (__builtin_expect((unsigned)(sub - QJS_AOT_REG_BIN_IS_UNDEF) < "
        "2, 0)) {\n"
        "    *take = (JS_VALUE_GET_TAG(a) == JS_TAG_UNDEFINED) ==\n"
        "            (sub == QJS_AOT_REG_BIN_IS_UNDEF);\n"
        "    return 1;\n"
        "  }\n"
        "  if (JS_VALUE_IS_BOTH_INT(a, b) &&\n"
        "      (r = q2_cmp_int(sub & ~QJS_AOT_REG_CMP_INVERT,\n"
        "                      JS_VALUE_GET_INT(a), JS_VALUE_GET_INT(b))) >= "
        "0) {\n"
        "    *take = r ^ ((sub & QJS_AOT_REG_CMP_INVERT) != 0);\n"
        "    return 1;\n"
        "  }\n"
        "  if (JS_VALUE_GET_TAG(a) == JS_TAG_FLOAT64 &&\n"
        "      JS_VALUE_GET_TAG(b) == JS_TAG_FLOAT64 &&\n"
        "      (r = q2_cmp_double(sub & ~QJS_AOT_REG_CMP_INVERT,\n"
        "                         JS_VALUE_GET_FLOAT64(a),\n"
        "                         JS_VALUE_GET_FLOAT64(b))) >= 0) {\n"
        "    *take = r ^ ((sub & QJS_AOT_REG_CMP_INVERT) != 0);\n"
        "    return 1;\n"
        "  }\n"
        "  return 0;\n"
        "}\n\n");

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
          e2_split_on() ? "__attribute__((visibility(\"hidden\"))) " : "static ",
          hex, hex);
      /* `Q2_SEL_` says the function's *body* is in this unit. A call site needs
         more: the native *entry*, which exists only for a body the E2 emitter
         produced for a light-eligible function. A unit built with
         `--emit2-only` emits every other function with the old emitter, which
         emits no entry, so the two must be distinguished or a call site names
         an `aot_n_` nothing defines. */
      if (qjs_aot_has_native_entry(i))
        fprintf(
            f,
            "#define Q2_NATIVE_%s 1\n"
            "%sJSValue aot_n_%s(JSContext *, JSRegRun *, JSObject *, "
            "JSValue, JSValue *, int);\n",
            hex,
            e2_split_on() ? "__attribute__((visibility(\"hidden\"))) " : "static ",
            hex);
    }
    fprintf(f, "\n");
    /* The leaves of the selected functions, before any caller. */
    for (i = 0; g_pure_eb && i < nfuncs; i++) {
      if (!sel[i] || !eb[i].buf || !g_pure_eb[i].buf) continue;
      fwrite(g_pure_eb[i].buf, 1, g_pure_eb[i].len, f);
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
            "#define E2_TEXT __attribute__((section(\"e2text\")))\n"
            "extern char __start_e2text[] __attribute__((weak));\n"
            "extern char __stop_e2text[] __attribute__((weak));\n");
        /* The pure leaf helpers are `static inline` and are written once, to
           the shared header above; repeating them in each part would be a
           redefinition. Only the function's own bodies go here. */
        fwrite(eb[i].buf, 1, eb[i].len, pf);
        fclose(pf);
      } else {
        fwrite(eb[i].buf, 1, eb[i].len, f);
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
      fprintf(f, "#include \"quickjs-aot-rt.h\"\n#include \"u.h\"\n");
      /* The D4 counters are declared in the shared header, so they are defined
         exactly once, here. */
      fprintf(
          f,
          "uint64_t e2_ent[QJS_AOT_E2_NFUNCS];\n"
          "uint32_t e2_exi[QJS_AOT_E2_NFUNCS], e2_ref[QJS_AOT_E2_NFUNCS];\n"
          "uint8_t e2_off[QJS_AOT_E2_NFUNCS];\n"
          "uint32_t e2_han[QJS_AOT_E2_NFUNCS];\n"
          "const char *const qjs_aot_e2_names[] = {\n"
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
          "void __attribute__((destructor)) e2_stats_dump(void)\n"
          "{\n"
          "  unsigned long long en = 0, ex = 0, rf = 0, off = 0, hn = 0;\n"
          "  const char *e = getenv(\"QJS_AOT_E2_STATS\");\n"
          "  int i;\n"
          "  if (e == NULL || e[0] != '1') return;\n"
          "  for (i = 0; i < QJS_AOT_E2_NFUNCS; i++) {\n"
          "    en += e2_ent[i]; ex += e2_exi[i]; rf += e2_ref[i]; off += e2_off[i];\n"
          "    hn += e2_han[i];\n"
          "  }\n"
          "  fprintf(stderr, \"e2-stats enters=%%llu exits=%%llu handoffs=%%llu refuses=%%llu off=%%llu\\n\",\n"
          "          en, ex, hn, rf, off);\n"
          "  for (i = 0; i < QJS_AOT_E2_NFUNCS; i++) {\n"
          "    if (e2_ent[i] == 0 && e2_ref[i] == 0) continue;\n"
          "    fprintf(stderr, \"e2-stats fn=%%s enters=%%llu exits=%%llu handoffs=%%llu refuses=%%llu off=%%u idx=%%d\\n\",\n"
          "            qjs_aot_e2_names[i] ? qjs_aot_e2_names[i] : \"?\",\n"
          "            (unsigned long long)e2_ent[i], (unsigned long long)e2_exi[i],\n"
          "            (unsigned long long)e2_han[i], (unsigned long long)e2_ref[i],\n"
          "            (unsigned)e2_off[i], i);\n"
          "  }\n"
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
        fprintf(f, "}, aot_f_%s, 0u, 0x%08xu },\n", hex, funcs[i].code_crc);
      }
      fprintf(f, "};\n");
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
    fprintf(f, "const uint32_t qjs_aot_abi = QJS_AOT_ABI;\n");
    fprintf(f, "const uint32_t qjs_aot_layout = QJS_AOT_LAYOUT_HASH;\n");
    fprintf(f, "const uint32_t qjs_aot_cell_count = 0u;\n");
    fprintf(f, "const uint32_t qjs_aot_site_count = %du;\n", g_deopt_site_next);
    fclose(f);

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
    e2_handoff_report(stderr);
    /* Functions the speculative body refused as irreducible. */
    if (e2_irreducible)
      fprintf(
          stderr, "qjsaot: irreducible (refused for the speculative body): %d\n",
          e2_irreducible);

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

#ifndef QJS_AOT_NO_MAIN
int main(int argc, char **argv) {
  return qjsaot_driver_main(argc, argv);
}
#endif
