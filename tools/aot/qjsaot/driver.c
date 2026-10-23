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
#include "prof.h"

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

/* Whether a compiled caller may call this unit function on the C stack: a light
   function, or one the engine does not call light (it reads closure variables) but
   that a frameless body can serve: a plain function with no control records and no
   variable area. The final answer is the `Q2_NATIVE_` macro, defined only when a
   native entry was emitted. */
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
      return g->func_kind == 0 && g->ctl_count == 0 && g->light_var_count == 0;
    }
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
static uint8_t *g_inl_nx; /* the inline body exits through aot_nx_ */

/* Which functions this run's emitter gave a native entry. A call site may
   only name an `aot_n_` that some body in this unit defines, and only a
   light-eligible function gets one. Keyed by unit index. */
static uint8_t *g_e_native;

/* The emitter's hook, so the unit builder knows which bodies it produced. */
void qjs_aot_note_native_entry(int index) {
  if (g_e_native && index >= 0) g_e_native[index] = 1;
}

/* The unit index of the function being emitted, for the hook above. */
int qjs_aot_cur_func_idx(void) {
  return g_cur_func_idx;
}

int qjs_aot_has_native_entry(int index) {
  return g_e_native && index >= 0 ? g_e_native[index] : 0;
}

/* Whether the unit function with this digest has an inline form a caller can run
   in place of a call. */
int qjs_aot_inline_has(const char hex[33]) {
  uint8_t dg[16];
  int i;

  if (!g_unit_funcs || !g_inl_eb || hex_to_bytes16(hex, dg) < 0) return 0;
  for (i = 0; i < g_unit_nfuncs; i++)
    if (memcmp(g_unit_funcs[i].digest, dg, 16) == 0)
      return g_unit_funcs[i].eligible == AOT_GEN_ELIGIBLE && g_inl_eb[i].buf;
  return 0;
}

int qjs_aot_inline_uses_nx(const char hex[33]) {
  uint8_t dg[16];
  int i;

  if (!g_unit_funcs || !g_inl_nx || hex_to_bytes16(hex, dg) < 0) return 0;
  for (i = 0; i < g_unit_nfuncs; i++)
    if (memcmp(g_unit_funcs[i].digest, dg, 16) == 0) return g_inl_nx[i];
  return 0;
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
    } else if (strcmp(argv[i], "--nocall") == 0) {
      e_set_nocall(1);
    } else if (strcmp(argv[i], "--poison") == 0) {
      e_set_poison(1);
    } else if (strcmp(argv[i], "--split") == 0 && i + 1 < argc) {
      split_dir = argv[++i];
    } else if (strcmp(argv[i], "--report") == 0 && i + 1 < argc) {
      g_report_path = argv[++i];
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
    g_e_native = calloc((size_t)(nfuncs > 0 ? nfuncs : 1), 1);
    /* The bodies are emitted into memory buffers before the output file is
     opened, so the linkage decision has to be made before that: a split unit
     gives every compiled function hidden external linkage so the parts can
     call each other. */
  if (split_dir != NULL) e_set_split(1);
  e_roles_check();
    e_report_open(g_report_path);
    /* The inline forms first: a caller asks whether its callee has one while its own
       body is emitted. */
    g_inl_eb = calloc((size_t)(nfuncs > 0 ? nfuncs : 1), sizeof(*g_inl_eb));
    g_inl_nx = calloc((size_t)(nfuncs > 0 ? nfuncs : 1), 1);
    for (i = 0; g_inl_eb && i < nfuncs; i++) {
      char hex[33], why[160] = "";
      FILE *mf;
      int ok;

      if (funcs[i].eligible != AOT_GEN_ELIGIBLE) continue;
      if (i + 1 < nfuncs && memcmp(funcs[i].digest, funcs[i + 1].digest, 16) == 0)
        continue;
      hex16(funcs[i].digest, hex);
      g_cur_func_idx = i;
      mf = open_memstream(&g_inl_eb[i].buf, &g_inl_eb[i].len);
      if (!mf) continue;
      ok = e_emit_inline_func(mf, &funcs[i], hex, why, sizeof(why));
      fclose(mf);
      e_report_inline(hex, ok, why);
      if (ok && g_inl_nx) g_inl_nx[i] = (uint8_t)e_inline_used_nx();
      if (!ok) {
        free(g_inl_eb[i].buf);
        g_inl_eb[i].buf = NULL;
        g_inl_eb[i].len = 0;
      }
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
      ninsn = e_emit_body(mf, &funcs[i], hex);
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
          "/* The poll's decrement as one read-modify-write with the flags kept: `sub $1, mem; jle`. */\n"
          "#define Q2_POLL_HIT(c) ({ int hit_; __asm__ volatile(\"subl $1, %%0\" : \"+m\"(*(int *)((unsigned char *)(c) + QJS_AOT_OFF_CTX_INTERRUPT_COUNTER)), \"=@ccle\"(hit_)); hit_; })\n"
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
          "#define QJS_AOT_OFF_CTX_GV_CELLS %uu\n"
          "#define QJS_AOT_SIZEOF_GV_CELL %uu\n"
          "#define QJS_AOT_GV_CELL_SETS %uu\n"
          "#define QJS_AOT_GV_CELL_WAYS %uu\n"
          "#define QJS_AOT_OFF_GVC_ATOM %uu\n"
          "#define QJS_AOT_OFF_GVC_GEN %uu\n"
          "#define QJS_AOT_OFF_GVC_SLOT %uu\n"
          "#define QJS_AOT_OFF_GVC_WR %uu\n\n"
          "#define QJS_AOT_OFF_REGRUN_RET_IP %uu\n"
          "#define QJS_AOT_OFF_REGRUN_FR %uu\n"
          "#define QJS_AOT_OFF_REGRUN_VAR_REFS %uu\n"
          "#define QJS_AOT_OFF_REGRUN_HDR_PACK %uu\n"
          "#define QJS_AOT_OFF_REGRUN_CAP_IP %uu\n"
          "#define QJS_AOT_OFF_FNBC_REALM %uu\n"
          "#define QJS_AOT_OFF_RT_REGVM_CUR %uu\n"
          "#define QJS_AOT_OFF_RT_STACK_LIMIT %uu\n"
          "#define QJS_AOT_OFF_RT_FL_TOP %uu\n"
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
          "#define QJS_AOT_OFF_RT_LAZY_JSON %uu\n"
          "#define QJS_AOT_OFF_JSOBJECT_FLAGS16 %uu\n"
          "#define QJS_AOT_JSOBJECT_EXTENSIBLE_MASK %uu\n"
          "#define QJS_AOT_JSOBJECT_NOADD_MASK %uu\n"
          "#define QJS_AOT_JSOBJECT_INLINE_CAP_MASK %uu\n"
          "#define QJS_AOT_JSOBJECT_INLINE_CAP_SHIFT %uu\n"
          "#define QJS_AOT_OFF_ROPE_LEFT %uu\n"
          "#define QJS_AOT_OFF_ROPE_RIGHT %uu\n"
          "#define QJS_AOT_OFF_CTX_TA_LEN_EPOCH %uu\n"
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
          lay.class_array, lay.class_uint8c_array, lay.class_int8_array,
          lay.class_uint8_array, lay.class_int16_array, lay.class_uint16_array,
          lay.class_int32_array, lay.class_uint32_array, lay.class_float32_array,
          lay.class_float64_array, lay.string_len_mask, lay.off_ctx_class_proto,
          lay.class_string, lay.string_wide_mask, lay.string_kind_mask, lay.shape_prop_tmask, lay.off_rt_onechar,
          lay.off_fnbc_aot_poly, lay.off_fnbc_aot_poly_sites,
          lay.aot_poly_ways, lay.off_object_holey_byte, lay.object_holey_mask,
          lay.reg_pic_proto2, lay.off_object_htmldda_byte, lay.object_htmldda_mask,
          lay.off_ctx_gv_cells, lay.sizeof_gv_cell, lay.gv_cell_sets, lay.gv_cell_ways,
          lay.off_gvc_atom, lay.off_gvc_gen, lay.off_gvc_slot, lay.off_gvc_wr,
          lay.off_regrun_ret_ip, lay.off_regrun_fr, lay.off_regrun_var_refs,
          lay.off_regrun_hdr_pack, lay.off_regrun_cap_ip, lay.off_fnbc_realm,
          lay.off_rt_regvm_cur, lay.off_rt_stack_limit, lay.off_rt_fl_top,
          lay.off_object_func_var_refs, lay.light_prefix_slots,
          lay.regvm_flag_light, lay.regvm_flag_c_entry, lay.regvm_flag_stack,
          lay.regvm_flag_trivial_ret, lay.regvm_flag_same_realm,
          lay.off_rc_back, lay.off_varref_pvalue, lay.class_c_function,
          lay.off_cfunc_c_function, lay.off_cfunc_magic, lay.off_regpic_misses,
          lay.reg_pic_add_w, lay.off_rt_ic_epoch, lay.off_rt_lazy_json, lay.off_object_flags16,
          lay.object_extensible_mask, lay.object_noadd_mask,
          lay.object_inline_cap_mask, lay.object_inline_cap_shift, lay.off_rope_left,
          lay.off_rope_right, lay.off_ctx_ta_len_epoch);
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
        "#define E_TEXT __attribute__((section(\"aottext\")))\n"
        "extern char __start_aottext[] __attribute__((weak));\n"
        "extern char __stop_aottext[] __attribute__((weak));\n");
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
        "typedef struct E_XNode { const char *hex, *kind; int pc; uint32_t cnt; struct E_XNode *next; } E_XNode;\n"
        "#ifdef QJS_AOT_SPLIT_UNIT\n"
        "extern uint64_t e_ent[QJS_AOT_NFUNCS];\n"
        "extern uint32_t e_exi[QJS_AOT_NFUNCS], e_ref[QJS_AOT_NFUNCS];\n"
        "extern uint8_t e_off[QJS_AOT_NFUNCS];\n"
        "extern uint32_t e_han[QJS_AOT_NFUNCS];\n"
        "extern uint8_t e_nfl[QJS_AOT_NFUNCS], e_dis[QJS_AOT_NFUNCS];\n"
        "extern uint8_t e_osr_ok[QJS_AOT_NFUNCS], e_osr_n[QJS_AOT_NFUNCS];\n"
        "extern uint64_t e_inl[QJS_AOT_NFUNCS];\n"
        "extern uint32_t e_inx[QJS_AOT_NFUNCS];\n"
        "extern uint8_t e_ioff[QJS_AOT_NFUNCS];\n"
        "extern E_XNode *e_xhead;\n"
        "#else\n"
        "static E_XNode *e_xhead;\n"
        "static uint64_t e_ent[QJS_AOT_NFUNCS];\n"
        "static uint32_t e_exi[QJS_AOT_NFUNCS], e_ref[QJS_AOT_NFUNCS];\n"
        "static uint8_t e_off[QJS_AOT_NFUNCS];\n"
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
        "  for (i = 0; i < QJS_AOT_NFUNCS; i++) {\n"
        "    if (e_ent[i] == 0 && e_ref[i] == 0) continue;\n"
        "    fprintf(stderr, \"unit-stats fn=%%s enters=%%llu exits=%%llu handoffs=%%llu refuses=%%llu off=%%u idx=%%d frameless=%%llu inlined=%%llu\\n\",\n"
        "            qjs_aot_e_names[i] ? qjs_aot_e_names[i] : \"?\",\n"
        "            (unsigned long long)e_ent[i], (unsigned long long)e_exi[i],\n"
        "            (unsigned long long)e_han[i], (unsigned long long)e_ref[i],\n"
        "            (unsigned)e_off[i], i, e_nfl[i] ? (unsigned long long)e_ent[i] : 0ull,\n            (unsigned long long)e_inl[i]);\n"
        "  }\n"
        "  { const E_XNode *n_; for (n_ = e_xhead; n_ != NULL; n_ = n_->next)\n"
        "      fprintf(stderr, \"unit-exit-site fn=%%s kind=%%s pc=%%d count=%%u\\n\", n_->hex, n_->kind, n_->pc, n_->cnt); }\n"
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
        "#define E_ENTER_NOTE(fi) (e_ent[fi]++)\n"
        "#define E_INL_NOTE(fi) (e_inl[fi]++)\n"
        /* Loop re-entry: the generic body, running a loop the speculative body left,
           hands the call back to it at the loop header. Skipped once when the generic
           body was entered at that very header (an exit there), and limited in count so
           a loop the speculative body keeps leaving does not nest without end. */
        "#define E_OSR_HOOK(fi_, h_, fn_) do { static uint8_t e_oc_; static int e_ic_; \\\n"
        "    if (osr_from == (h_)) osr_from = ~0u; \\\n"
        "    else if (__builtin_expect(e_osr_ok[fi_] && e_osr_n[fi_] < 200 && \\\n"
        "             !(e_off[fi_] | e_dis[fi_]), 0)) { \\\n"
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
        "e_exit_log(const char *hex, const char *kind, int pc, int tag, int *seen)\n"
        "{\n"
        "  if (e_exit_log_v < 0) {\n"
        "    const char *e_ = getenv(\"QJS_AOT_EXIT_LOG\");\n"
        "    e_exit_log_v = e_ != NULL ? (e_[0] == '2' ? 2 : e_[0] == '1') : 0;\n"
        "  }\n"
        "  if (!e_exit_log_v) return;\n"
        "  *seen = e_exit_log_v != 2;\n"
        "  fprintf(stderr, \"unit-exit fn=%%s kind=%%s pc=%%d tag=%%d\\n\", hex, kind,\n"
        "          pc, tag);\n"
        "}\n"
        "static inline void e_xnode_reg(E_XNode *n) { n->next = e_xhead; e_xhead = n; }\n"
        "#define E_EXIT_LOG(hex, kind, pc, tag) do { static E_XNode e_xn_ = {(hex), (kind), (pc), 0, NULL}; \\\n"
        "    int e_xl_ = 0; \\\n"
        "    if (__builtin_expect(++e_xn_.cnt == 1, 0)) e_xnode_reg(&e_xn_); \\\n"
        "    if (__builtin_expect(e_exit_log_v != 0, 0) && (e_exit_log_v == 2 || e_xn_.cnt == 1)) \\\n"
        "      e_exit_log((hex), (kind), (pc), (int)(tag), &e_xl_); } while (0)\n"
        "#define E_EXIT_NOTE(fi) do { if (++e_exi[fi] > 256 && \\\n"
        "    (uint64_t)e_exi[fi] * 16 > e_ent[fi]) e_off[fi] = 1; } while (0)\n"
        "/* A HAND-OFF is not a deopt: it is the exit at an instruction the spec\n"
        "   body does not lower. Counting them toward the switch-off would turn off\n"
        "   every spec body after 257 calls and every measurement would time the\n"
        "   generic body. They are counted, never acted on. */\n"
        "#define E_HANDOFF_NOTE(fi) (e_han[fi]++)\n"
        "/* Entry refused in more than half the calls: nothing to speculate on. */\n"
        "#define E_REFUSE_NOTE(fi) do { if (++e_ref[fi] > 256 && \\\n"
        "    (uint64_t)e_ref[fi] * 2 > e_ent[fi]) e_off[fi] = 1; } while (0)\n"
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
        "#define E_PUT_N 3\n"
        "static unsigned long long e_put_why[E_PUT_N];\n"
        "static void e_put_why_note(int why) { e_put_why[why]++; }\n"
        "static const char *const e_put_why_names[E_PUT_N] = {\n"
        "  \"shape\", \"kind-not-own-w\", \"receiver-prototype\"\n"
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
        "#define E_NO_SHAPE ((const void *)1)\n"
        /* An integral double boxes as an int, as the engine does. The conversion is the
           hardware's (out of range gives INT_MIN, which never compares equal to a double that
           does not fit), so the test is a convert, a convert back and a compare. */
        "#if defined(__SSE2__)\n"
        "#include <emmintrin.h>\n"
        "#define E_BOX_F64(d) ({ double d_ = (d); int32_t i_ = _mm_cvttsd_si32(_mm_set_sd(d_)); "
        "__builtin_expect((double)i_ == d_ && (i_ != 0 || !signbit(d_)), 0) ? "
        "JS_MKVAL(JS_TAG_INT, i_) : __JS_NewFloat64(d_); })\n"
        "#else\n"
        "#define E_BOX_F64(d) ({ double d_ = (d); "
        "qjs_aot_box_num(d_, qjs_aot_fitsi(d_)); })\n"
        "#endif\n"
        /* The `undefined` word a native entry fills its frame with, as one
           static constant so the fill is a copy of a known word. */
        "static const JSValue e_undefined_value = { { .int32 = 0 }, "
        "JS_TAG_UNDEFINED };\n"
        "static inline int q2_spec_fns_out(int fi);\n"
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
        "    e_dis[i] = (!q2_spec_on || q2_no_spec || q2_spec_fns_out(i));\n"
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
        "static inline const void *q2_cell_shape(const void *pic, int k)\n"
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
        /* A fast array's element read: class, bounds, hole and lazy-JSON
           checks, then the element itself, borrowed. No reference is taken,
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
        "  *eq = __builtin_memcmp(pa + QJS_AOT_SIZEOF_JSSTRING, pb + QJS_AOT_SIZEOF_JSSTRING, n) == 0;\n"
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
        "  if (p[QJS_AOT_OFF_JSOBJECT_HOLEY_BYTE] & "
        "QJS_AOT_JSOBJECT_HOLEY_MASK) return 0;\n"
        "  if (i >= *(const uint32_t *)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_COUNT)) "
        "return 0;\n"
        "  v = (*(JSValue *const *)(p + "
        "QJS_AOT_OFF_JSOBJECT_ARRAY_VALUES))[i];\n"
        "  if (JS_VALUE_GET_TAG(v) == JS_TAG_UNINITIALIZED) return 0;\n"
        "  if (lz && JS_VALUE_GET_TAG(v) == QJS_AOT_TAG_LAZY_JSON) return 0;\n"
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
        "    return qjs_aot_typed_put(arr, i, v);\n"
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
        /* A captured variable, read through the function object's cell vector:
           what qjs_aot_get_var_ref does from a frame, for a body that has none. */
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
        "static inline __attribute__((always_inline)) int "
        "q2_own_read_hit_lz(const void *pic, void *const *holders,\n"
        "                       JSValue recv, int site, int nsites,\n"
        "                       JSValue *out, int lz)\n"
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
        "    if (*(const uint16_t *)(e + QJS_AOT_OFF_JSREGPIC_KIND) !=\n"
        "        QJS_AOT_REG_PIC_PROTO) return 0;\n"
        "    holder = *(const unsigned char *const *)(*(const unsigned char "
        "*const *)(\n"
        "                 obj + QJS_AOT_OFF_JSOBJECT_SHAPE) + "
        "QJS_AOT_OFF_JSSHAPE_PROTO);\n"
        "    if (holder == NULL ||\n"
        "        *(const void *const *)(holder + QJS_AOT_OFF_JSOBJECT_SHAPE) "
        "!=\n"
        "            holders[(size_t)(e - (const unsigned char *)pic) / "
        "QJS_AOT_SIZEOF_JSREGPIC]) {\n"
        "      return 0;\n"
        "    }\n"
        "    obj = holder;\n"
        "  }\n"
        "  prop = *(const unsigned char *const *)(obj + "
        "QJS_AOT_OFF_JSOBJECT_PROP);\n"
        "  slot_off = *(const uint32_t *)(e + QJS_AOT_OFF_JSREGPIC_SLOT_OFF);\n"
        "  v = *(const JSValue *)(prop + slot_off);\n"
        "  if (lz && JS_VALUE_GET_TAG(v) == QJS_AOT_TAG_LAZY_JSON) {\n"
        "    return 0;\n"
        "  }\n"
        "#if !QJS_DRC\n"
        "  if (JS_VALUE_GET_TAG(v) < 0) {\n"
        "    return 0; /* registers own: the slow path takes the reference */\n"
        "  }\n"
        "#endif\n"
        "  *out = v;\n"
        "  return 1;\n"
        "}\n"
        /* The lazy-JSON marker test is on unless the body in progress assumes no lazy
           document exists (Q2_LZ is redefined around such a body). */
        "#define Q2_LZ 1\n"
        "#define q2_elem_hit(a, k, o) q2_elem_hit_lz(a, k, o, Q2_LZ)\n"
        "#define q2_own_read_hit(...) q2_own_read_hit_lz(__VA_ARGS__, Q2_LZ)\n"
        "#define E_LAZY_SEEN(ctx_) (*((uint8_t *)*(uint8_t **)((uint8_t *)(ctx_) + "
        "QJS_AOT_OFF_CTX_RT) + QJS_AOT_OFF_RT_LAZY_JSON))\n\n");
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
        "      q2_dup(v);\n"
        "      *p = v;\n"
        "      if (JS_VALUE_HAS_REF_COUNT(old)) JS_FreeValue(ctx, old);\n"
        "      return 1;\n"
        "    }\n"
        "  }\n"
        "  return 0;\n"
        "}\n"
);
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
        "  if (p[QJS_AOT_OFF_JSOBJECT_HOLEY_BYTE] & "
        "QJS_AOT_JSOBJECT_HOLEY_MASK) return 0;\n"
        "  if (i >= *(const uint32_t *)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_COUNT)) "
        "return 0;\n"
        "  slot = &(*(JSValue **)(p + QJS_AOT_OFF_JSOBJECT_ARRAY_VALUES))[i];\n"
        "  if (JS_VALUE_GET_TAG(*slot) == JS_TAG_UNINITIALIZED) return 0;\n"
        "  old = *slot;\n"
        "  q2_dup(v);\n"
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
    fprintf(
        f,
        "static inline __attribute__((always_inline)) int "
        "q2_put_add(JSContext *ctx, const void *pic, void *const *holders,\n"
        "                                  uint32_t ns, JSValue recv, int site, JSValue v)\n"
        "{\n"
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
        "  if (*(const uint16_t *)(e + QJS_AOT_OFF_JSREGPIC_MISSES) !=\n"
        "      (uint16_t)*(const uint32_t *)(*(const unsigned char *const *)((const unsigned char *)ctx +\n"
        "          QJS_AOT_OFF_CTX_RT) + QJS_AOT_OFF_RT_IC_EPOCH)) return 0;\n"
        "  nsh = (unsigned char *)holders[ns + (uint32_t)site];\n"
        "  npsz = *(const int *)(nsh + QJS_AOT_OFF_JSSHAPE_PROP_SIZE);\n"
        "  if (npsz != *(const int *)(osh + QJS_AOT_OFF_JSSHAPE_PROP_SIZE) &&\n"
        "      (unsigned)npsz > ((fl & QJS_AOT_JSOBJECT_INLINE_CAP_MASK) >> QJS_AOT_JSOBJECT_INLINE_CAP_SHIFT)) return 0;\n"
        "  if (*(const int *)(osh - QJS_AOT_RC_BACK) <= 1) return 0;\n"
        "  slot = (JSValue *)(*(unsigned char *const *)(obj + QJS_AOT_OFF_JSOBJECT_PROP) +\n"
        "      *(const uint32_t *)(e + QJS_AOT_OFF_JSREGPIC_SLOT_OFF));\n"
        "  ++*(int *)(nsh - QJS_AOT_RC_BACK);\n"
        "  *(unsigned char **)(obj + QJS_AOT_OFF_JSOBJECT_SHAPE) = nsh;\n"
        "  --*(int *)(osh - QJS_AOT_RC_BACK);\n"
        "  q2_dup(v);\n"
        "  *slot = v;\n"
        "  return 1;\n"
        "}\n");
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
        "    case QJS_AOT_REG_BIN_STRICT_EQ: *out = JS_MKVAL(JS_TAG_BOOL, d1 "
        "== d2); return 1;\n"
        "    case QJS_AOT_REG_BIN_STRICT_NEQ: *out = JS_MKVAL(JS_TAG_BOOL, d1 "
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
        "    case QJS_AOT_REG_BIN_STRICT_EQ: *out = JS_MKVAL(JS_TAG_BOOL, d1 == d2); return 1;\n"
        "    case QJS_AOT_REG_BIN_STRICT_NEQ: *out = JS_MKVAL(JS_TAG_BOOL, d1 != d2); return 1;\n"
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
        "      if (__builtin_expect(v1 >= 0 && v2 > 0, 1)) { *out = JS_MKVAL(JS_TAG_INT, v1 % v2); return 1; }\n"
        "      return 0;\n"
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
      /* `Q2_SEL_` says the function's *body* is in this unit. A call site needs
         more: the native *entry*, which exists only for a light-eligible
         function, so the two must be distinguished or a call site names an
         `aot_n_` nothing defines. */
      if (qjs_aot_has_native_entry(i))
        fprintf(
            f,
            "#define Q2_NATIVE_%s 1\n"
            "%sJSValue aot_n_%s(JSContext *, JSRegRun *, JSObject *, "
            "JSValue, JSValue *, int);\n",
            hex,
            e_split_on() ? "__attribute__((visibility(\"hidden\"))) " : "static ",
            hex);
    }
    fprintf(f, "\n");
    /* The inline forms of the selected functions that have a native entry, before
       any caller. */
    for (i = 0; g_inl_eb && i < nfuncs; i++) {
      char hex[33];
      if (!sel[i] || !eb[i].buf || !g_inl_eb[i].buf ||
          !qjs_aot_has_native_entry(i))
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
            "#define E_TEXT __attribute__((section(\"aottext\")))\n"
            "extern char __start_aottext[] __attribute__((weak));\n"
            "extern char __stop_aottext[] __attribute__((weak));\n");
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
      /* The switch-off counters are declared in the shared header, so they are defined
         exactly once, here. */
      fprintf(
          f,
          "uint64_t e_ent[QJS_AOT_NFUNCS];\n"
          "uint32_t e_exi[QJS_AOT_NFUNCS], e_ref[QJS_AOT_NFUNCS];\n"
          "uint8_t e_off[QJS_AOT_NFUNCS];\n"
          "uint32_t e_han[QJS_AOT_NFUNCS];\n"
          "uint8_t e_nfl[QJS_AOT_NFUNCS], e_dis[QJS_AOT_NFUNCS];\n"
          "uint8_t e_osr_ok[QJS_AOT_NFUNCS], e_osr_n[QJS_AOT_NFUNCS];\n"
          "E_XNode *e_xhead;\n"
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
          "  for (i = 0; i < QJS_AOT_NFUNCS; i++) {\n"
          "    if (e_ent[i] == 0 && e_ref[i] == 0) continue;\n"
          "    fprintf(stderr, \"unit-stats fn=%%s enters=%%llu exits=%%llu handoffs=%%llu refuses=%%llu off=%%u idx=%%d frameless=%%llu inlined=%%llu\\n\",\n"
          "            qjs_aot_e_names[i] ? qjs_aot_e_names[i] : \"?\",\n"
          "            (unsigned long long)e_ent[i], (unsigned long long)e_exi[i],\n"
          "            (unsigned long long)e_han[i], (unsigned long long)e_ref[i],\n"
          "            (unsigned)e_off[i], i, e_nfl[i] ? (unsigned long long)e_ent[i] : 0ull,\n            (unsigned long long)e_inl[i]);\n"
          "  }\n"
          "  { const E_XNode *n_; for (n_ = e_xhead; n_ != NULL; n_ = n_->next)\n"
        "      fprintf(stderr, \"unit-exit-site fn=%%s kind=%%s pc=%%d count=%%u\\n\", n_->hex, n_->kind, n_->pc, n_->cnt); }\n"
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
        if (qjs_aot_has_native_entry(i) && getenv("QJS_AOT_NONFN") == NULL)
          fprintf(f, "}, aot_f_%s, aot_n_%s, 0u, 0x%08xu },\n", hex, hex, funcs[i].code_crc);
        else
          fprintf(f, "}, aot_f_%s, NULL, 0u, 0x%08xu },\n", hex, funcs[i].code_crc);
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
    e_handoff_report(stderr);
    /* Functions the speculative body refused as irreducible. */
    if (e_irreducible)
      fprintf(
          stderr, "qjsaot: irreducible (refused for the speculative body): %d\n",
          e_irreducible);

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
