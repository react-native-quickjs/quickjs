#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emit.h"
#include "ir.h"
#include "quickjs-aot-gen.h"
#include "quickjs-aot-rt.h"
#include "quickjs-regvm.h"
#include "quickjs.h"

/* ---- source scan ---------------------------------------------------------
   Every function of the bundle that has a register program is walked, and every store of a
   named field is listed by field name with the static class of the stored value: a literal
   int, a double, a bool, null, undefined, the result of an arithmetic operation, a comparison,
   the value of a global that is only ever given one class, or unknown. A name is by design a
   superset of the stores into one slot of a layout (any object with that field name), so a
   verdict drawn from it only errs toward refusing.

   A store whose receiver is `this` belongs to the class its function is a method or constructor
   of, when the scan can see that: a function made by a closure that is only given to a global
   function declaration, a class body, or `X.prototype.m = function`. Those stores count only
   against the layout of their own class. Every other function is unattributed, and its `this`
   stores count against every layout with that field name. When `this` leaves a function as a
   value (a call argument, a return, a store) or the bundle has a derived class, a store may
   build an instance of another class, and nothing is excluded.

   Computed-key stores that are not an int constant, writes of private or super fields, `with`,
   `eval`, `delete`, prototype changes, functions without a register program and closures the
   scan cannot follow to a function are escapes: the source cannot account for every store, so
   a promise rests on the profile and the engine hook (the verdict is consistent, not proven). */
enum { SC_INT = 1, SC_DBL = 2, SC_NUM = 4, SC_BOOL = 8, SC_NULL = 16, SC_UNDEF = 32, SC_UNK = 64, SC_INTOV = 128 };
#define SC_NUMERIC (SC_INT | SC_DBL | SC_NUM | SC_INTOV)
#define SCAN_NAMES 512
#define SCAN_GLOBALS 512
#define SCAN_THIS 8192
typedef struct ScanName { char name[48]; unsigned mask, prev; int nstores; } ScanName;
typedef struct ScanThis { int name; unsigned cls; int fn; int param; int isthis; } ScanThis;
typedef struct ScanGlobal { char name[48]; unsigned mask; int nstores; int fn; } ScanGlobal;
typedef struct ScanFn {
  const JSAotGenFunc *g;
  Q2Func q;
  int ok;
  int owner;      /* index of the function whose class this is a method or constructor of, or -1 */
  int claimed;    /* a use of its closure was attributed */
  int escaped;    /* some use of its closure was not understood */
  int closed;     /* every reference to it is a call the scan resolves: its parameters have the argument classes */
  char mname[48]; /* the property name it is a method under, or "" */
  unsigned pcls[16];
} ScanFn;

static ScanName g_scan_names[SCAN_NAMES];
static ScanGlobal g_scan_globals[SCAN_GLOBALS];
static ScanThis g_scan_this[SCAN_THIS];
static ScanFn *g_sfn;
static int g_scan_nnames, g_scan_nglobals, g_scan_nthis, g_scan_done, g_scan_escapes, g_scan_opaque, g_scan_funcs;
static int g_scan_params_ready, g_scan_fixing;
static int g_scan_derived, g_scan_unresolved, g_scan_leaks, g_scan_attributed;
static JSContext *g_unit_ctx;
static const JSAotGenFunc *g_unit_funcs;
static int g_unit_nfuncs;

static int scan_atom(const Q2Insn *in, int *pos, char *out, size_t cap) {
  int a;
  for (a = 0; a < in->narg && in->arg[a].kind != 'a'; a++) {}
  if (a >= in->narg) return 0;
  {
    size_t len;
    const char *nm = JS_AtomToCStringLen(g_unit_ctx, &len, (JSAtom)in->arg[a].val);
    if (nm == NULL) return 0;
    snprintf(out, cap, "%s", nm);
    JS_FreeCString(g_unit_ctx, nm);
  }
  if (pos) *pos = a;
  return 1;
}

static ScanName *scan_name(const char *s) {
  int i;
  for (i = 0; i < g_scan_nnames; i++)
    if (strcmp(g_scan_names[i].name, s) == 0) return &g_scan_names[i];
  if (g_scan_nnames >= SCAN_NAMES) return NULL;
  memset(&g_scan_names[g_scan_nnames], 0, sizeof(g_scan_names[0]));
  snprintf(g_scan_names[g_scan_nnames].name, sizeof(g_scan_names[0].name), "%s", s);
  return &g_scan_names[g_scan_nnames++];
}

static ScanGlobal *scan_global(const char *s, int create) {
  int i;
  for (i = 0; i < g_scan_nglobals; i++)
    if (strcmp(g_scan_globals[i].name, s) == 0) return &g_scan_globals[i];
  if (!create || g_scan_nglobals >= SCAN_GLOBALS) return NULL;
  memset(&g_scan_globals[g_scan_nglobals], 0, sizeof(g_scan_globals[0]));
  snprintf(g_scan_globals[g_scan_nglobals].name, sizeof(g_scan_globals[0].name), "%s", s);
  g_scan_globals[g_scan_nglobals].fn = -1;
  return &g_scan_globals[g_scan_nglobals++];
}

/* The instruction that defines value v, looking through moves. */
static const Q2Insn *scan_def(const Q2Func *fn, Q2ValueId v) {
  int hop;
  for (hop = 0; hop < 8; hop++) {
    const Q2Insn *d;
    if (v < 0 || v >= fn->nvalues || fn->values[v].insn < 0 || fn->values[v].insn >= fn->ninsns) return NULL;
    d = &fn->insns[fn->values[v].insn];
    if (d->op == Q2_OP_MOVE && d->nuse == 1) { v = d->use[0]; continue; }
    return d;
  }
  return NULL;
}

static int g_scan_depth;
static int scan_is_read_rop(int rop);
static unsigned scan_class(int fi, Q2ValueId v) {
  const Q2Func *fn = &g_sfn[fi].q;
  const Q2Value *val;
  if (v < 0 || v >= fn->nvalues) return SC_UNK;
  val = &fn->values[v];
  if (val->is_const) {
    switch (val->con_kind) {
      case Q2_CON_INT: return SC_INT;
      case Q2_CON_DOUBLE: return SC_DBL;
      case Q2_CON_TRUE:
      case Q2_CON_FALSE: return SC_BOOL;
      case Q2_CON_NULL: return SC_NULL;
      case Q2_CON_UNDEF: return SC_UNDEF;
    }
    return SC_UNK;
  }
  {
    const Q2Insn *d = scan_def(fn, v);
    if (d == NULL) return SC_UNK;
    switch (d->rop) {
      case ROP_get_arg:
      case ROP_get_arg2: {
        int slot = d->rop == ROP_get_arg || d->def[0] == v ? (int)d->arg[1].val : (int)d->arg[3].val;
        if (!g_scan_params_ready || !g_sfn[fi].closed || slot < 0 || slot >= 16) return SC_UNK;
        return g_sfn[fi].pcls[slot] != 0 || g_scan_fixing ? g_sfn[fi].pcls[slot] : SC_UNK;
      }
      case ROP_load_undefined: return SC_UNDEF;
      case ROP_load_null: return SC_NULL;
      case ROP_load_true:
      case ROP_load_false: return SC_BOOL;
      case ROP_load_i8:
      case ROP_load_i8_s:
      case ROP_load_i32: return SC_INT;
      case ROP_get_global: {
        char nm[48];
        const ScanGlobal *gl;
        if (scan_atom(d, NULL, nm, sizeof(nm)) && (gl = scan_global(nm, 0)) != NULL && gl->nstores > 0 && gl->fn < 0 &&
            (gl->mask & SC_UNK) == 0)
          return gl->mask;
        return SC_UNK;
      }
    }
    if (d->op == Q2_OP_COMPARE) return SC_BOOL;
    if (d->op == Q2_OP_BINARY && d->sub >= 0 && d->nuse >= 2 && g_scan_depth < 6) {
      unsigned x, y;
      g_scan_depth++;
      x = scan_class(fi, d->use[0]);
      y = scan_class(fi, d->use[1]);
      g_scan_depth--;
      if (x != 0 && y != 0 && (x & ~SC_NUMERIC) == 0 && (y & ~SC_NUMERIC) == 0 && d->sub != REG_BIN_POW) {
        /* int arithmetic that may leave the int range gives a double: a store of it into an int slot
           takes the typed store's exit and the engine's hook breaks the layout, so it is an int-or-overflow */
        int ints = (x & ~(SC_INT | SC_INTOV)) == 0 && (y & ~(SC_INT | SC_INTOV)) == 0;
        switch (d->sub) {
          case REG_BIN_ADD: case REG_BIN_SUB: case REG_BIN_MUL: case REG_BIN_MOD:
            return ints ? SC_INTOV : SC_NUM;
          case REG_BIN_AND: case REG_BIN_OR: case REG_BIN_XOR: case REG_BIN_SHL: case REG_BIN_SAR:
            return SC_INTOV;
          default:
            return SC_NUM;
        }
      }
      if (x == 0 || y == 0) return g_scan_fixing ? 0 : SC_UNK;
      return SC_UNK;
    }
    if (scan_is_read_rop(d->rop) && d->ndef == 1) {
      char nm[48];
      int ni;
      if (scan_atom(d, NULL, nm, sizeof(nm)))
        for (ni = 0; ni < g_scan_nnames; ni++)
          if (strcmp(g_scan_names[ni].name, nm) == 0)
            return g_scan_names[ni].prev != 0 ? g_scan_names[ni].prev : (g_scan_fixing ? 0 : SC_UNK);
      return SC_UNK;
    }
  }
  return SC_UNK;
}

/* The argument slot a value is read from, or -1. */
static int scan_param_of(int fi, Q2ValueId v) {
  const Q2Insn *d = scan_def(&g_sfn[fi].q, v);
  if (d == NULL) return -1;
  if (d->rop == ROP_get_arg) return (int)d->arg[1].val;
  if (d->rop == ROP_get_arg2) return d->def[0] == v ? (int)d->arg[1].val : (int)d->arg[3].val;
  return -1;
}

/* The function a make_closure value creates, or -1. */
static int scan_closure_fn(const ScanFn *sf, const Q2Insn *d) {
  int a, i;
  if (d == NULL || d->rop != ROP_make_closure) return -1;
  for (a = 0; a < d->narg && d->arg[a].kind != 'c'; a++) {}
  if (a >= d->narg || sf->g->cpool_fn == NULL || d->arg[a].val < 0 || d->arg[a].val >= sf->g->cpool_count) return -1;
  {
    static const uint8_t zero[16];
    const uint8_t *dg = sf->g->cpool_fn[d->arg[a].val];
    if (memcmp(dg, zero, 16) == 0) return -1;
    for (i = 0; i < g_unit_nfuncs; i++)
      if (g_sfn[i].g != NULL && memcmp(g_sfn[i].g->digest, dg, 16) == 0) return i;
  }
  return -1;
}

/* Whether a use of value v at instruction `in` is one the scan attributes; closure values reach
   their uses through moves, so the uses of the whole chain are looked at. */
static void scan_closure_uses(int fi, const Q2Insn *mk, int child) {
  ScanFn *sf = &g_sfn[fi];
  const Q2Func *fn = &sf->q;
  int k, n = 0;
  Q2ValueId chain[8];
  if (mk->ndef < 1) { g_sfn[child].escaped = 1; return; }
  chain[n++] = mk->def[0];
  for (k = 0; k < fn->ninsns; k++) {
    const Q2Insn *in = &fn->insns[k];
    int u, in_chain;
    for (u = 0; u < in->nuse; u++) {
      int c;
      in_chain = 0;
      for (c = 0; c < n; c++) if (in->use[u] == chain[c]) in_chain = 1;
      if (!in_chain) continue;
      if (in->op == Q2_OP_MOVE && in->nuse == 1 && in->ndef == 1) {
        if (n < 8) chain[n++] = in->def[0]; else g_sfn[child].escaped = 1;
        continue;
      }
      switch (in->rop) {
        case ROP_define_global_func:
          if (u == 0) { g_sfn[child].claimed = 1; g_sfn[child].owner = child; break; }
          g_sfn[child].escaped = 1;
          break;
        case ROP_define_class:
          if (u == 0) { g_sfn[child].claimed = 1; g_sfn[child].owner = child; if (in->arg[2].val != 0) g_scan_derived++; break; }
          g_sfn[child].escaped = 1;
          break;
        case ROP_define_method:
        case ROP_put_field:
          /* the owner is settled in the second walk, which knows the classes */
          break;
        case ROP_put_global:
          g_sfn[child].claimed = 1;
          break;
        default:
          g_sfn[child].escaped = 1;
          break;
      }
    }
  }
}

static void scan_attribute_methods(int fi) {
  ScanFn *sf = &g_sfn[fi];
  const Q2Func *fn = &sf->q;
  int k, cur_class = -1;
  for (k = 0; k < fn->ninsns; k++) {
    const Q2Insn *in = &fn->insns[k];
    if (in->rop == ROP_define_class && in->nuse >= 1) {
      cur_class = scan_closure_fn(sf, scan_def(fn, in->use[0]));
    } else if (in->rop == ROP_define_method && in->nuse >= 2) {
      int ch = scan_closure_fn(sf, scan_def(fn, in->use[1]));
      if (ch >= 0) {
        if (cur_class >= 0 && !g_sfn[ch].escaped) {
          g_sfn[ch].owner = cur_class; g_sfn[ch].claimed = 1; g_scan_attributed++;
          if (!scan_atom(in, NULL, g_sfn[ch].mname, sizeof(g_sfn[ch].mname))) g_sfn[ch].mname[0] = 0;
        }
        else g_sfn[ch].escaped = 1;
      }
    } else if (in->rop == ROP_put_field && in->nuse >= 2) {
      int ch = scan_closure_fn(sf, scan_def(fn, in->use[1]));
      if (ch >= 0) {
        /* X.prototype.m = function: the receiver is a read of `prototype` from a global X */
        const Q2Insn *r = scan_def(fn, in->use[0]);
        char pn[48], gn[48];
        int own = -1;
        if (r != NULL && (r->rop == ROP_get_field || r->rop == ROP_get_field_own) && r->nuse >= 1 &&
            scan_atom(r, NULL, pn, sizeof(pn)) && strcmp(pn, "prototype") == 0) {
          const Q2Insn *gdef = scan_def(fn, r->use[0]);
          if (gdef != NULL && gdef->rop == ROP_get_global && scan_atom(gdef, NULL, gn, sizeof(gn))) {
            const ScanGlobal *gl = scan_global(gn, 0);
            if (gl != NULL && gl->fn >= 0) own = gl->fn;
          }
        }
        if (own >= 0 && !g_sfn[ch].escaped) {
          g_sfn[ch].owner = own; g_sfn[ch].claimed = 1; g_scan_attributed++;
          if (!scan_atom(in, NULL, g_sfn[ch].mname, sizeof(g_sfn[ch].mname))) g_sfn[ch].mname[0] = 0;
        }
        else g_sfn[ch].escaped = 1;
      }
    }
  }
}


/* Whether every use of the value `v` of function `fi` (and of its copies) is the callee of a call. */
static int scan_only_callee(int fi, Q2ValueId v) {
  const Q2Func *fn = &g_sfn[fi].q;
  Q2ValueId chain[8];
  int n = 1, k;
  chain[0] = v;
  for (k = 0; k < fn->ninsns; k++) {
    const Q2Insn *in = &fn->insns[k];
    int u, c;
    for (u = 0; u < in->nuse; u++) {
      for (c = 0; c < n && in->use[u] != chain[c]; c++) {}
      if (c == n) continue;
      if (in->op == Q2_OP_MOVE && in->nuse == 1 && in->ndef == 1) {
        if (n >= 8) return 0;
        chain[n++] = in->def[0];
      } else if (u == 0 && (in->rop == ROP_call || in->rop == ROP_call_method || in->rop == ROP_construct)) {
        continue;
      } else if (u == 1 && in->rop == ROP_construct) {
        continue;
      } else {
        return 0;
      }
    }
  }
  return 1;
}

static int scan_is_read(const Q2Insn *in) { return scan_is_read_rop(in->rop); }

static int scan_is_read_rop(int rop) {
  switch (rop) {
    case ROP_get_field:
    case ROP_get_field_own:
    case ROP_get_field2:
    case ROP_get_field2_own:
    case ROP_get_this_field:
    case ROP_get_this_field2:
    case ROP_get_field_chain:
    case ROP_get_field_chain_own:
    case ROP_get_local_field:
    case ROP_get_local_field_0:
    case ROP_get_local_field_1:
    case ROP_get_local_field_2:
    case ROP_get_local_field_3:
      return 1;
  }
  return 0;
}

/* Whether every read of property `name` in the bundle is a call of it. */
static int scan_name_only_called(const char *name) {
  int i, k;
  for (i = 0; i < g_unit_nfuncs; i++) {
    if (!g_sfn[i].ok) continue;
    for (k = 0; k < g_sfn[i].q.ninsns; k++) {
      const Q2Insn *in = &g_sfn[i].q.insns[k];
      char nm[48];
      if (!scan_is_read(in) || !scan_atom(in, NULL, nm, sizeof(nm)) || strcmp(nm, name) != 0) continue;
      if (in->ndef != 1 || !scan_only_callee(i, in->def[0])) return 0;
    }
  }
  return 1;
}

static int scan_global_only_called(const char *name) {
  int i, k;
  for (i = 0; i < g_unit_nfuncs; i++) {
    if (!g_sfn[i].ok) continue;
    for (k = 0; k < g_sfn[i].q.ninsns; k++) {
      const Q2Insn *in = &g_sfn[i].q.insns[k];
      char nm[48];
      if (in->rop != ROP_get_global || !scan_atom(in, NULL, nm, sizeof(nm)) || strcmp(nm, name) != 0) continue;
      if (in->ndef != 1 || !scan_only_callee(i, in->def[0])) return 0;
    }
  }
  return 1;
}

static void scan_params(void) {
  int i, k, round;
  for (i = 0; i < g_unit_nfuncs; i++) {
    ScanFn *sf = &g_sfn[i];
    int gi;
    if (!sf->ok || !sf->claimed || sf->escaped || g_scan_derived > 0 || g_scan_leaks > 0 || sf->g->arg_count > 16) continue;
    for (gi = 0; gi < g_scan_nglobals; gi++)
      if (g_scan_globals[gi].fn == i) { sf->closed = scan_global_only_called(g_scan_globals[gi].name); break; }
    if (gi == g_scan_nglobals && sf->mname[0]) sf->closed = scan_name_only_called(sf->mname);
  }
  g_scan_fixing = 1;
  for (round = 0; round < 8; round++) {
    int changed = 0;
    for (i = 0; i < g_unit_nfuncs; i++) {
      if (!g_sfn[i].ok) continue;
      for (k = 0; k < g_sfn[i].q.ninsns; k++) {
        const Q2Insn *in = &g_sfn[i].q.insns[k];
        const Q2Insn *cd;
        int tg[64], ntg = 0, j, argc, a;
        char nm[48];
        if ((in->rop != ROP_call && in->rop != ROP_call_method && in->rop != ROP_construct) || in->nuse < 1) continue;
        cd = scan_def(&g_sfn[i].q, in->use[0]);
        if (cd == NULL) continue;
        if (cd->rop == ROP_get_global && scan_atom(cd, NULL, nm, sizeof(nm))) {
          const ScanGlobal *gl = scan_global(nm, 0);
          if (gl != NULL && gl->fn >= 0 && g_sfn[gl->fn].closed) tg[ntg++] = gl->fn;
        } else if (scan_is_read(cd) && scan_atom(cd, NULL, nm, sizeof(nm))) {
          for (j = 0; j < g_unit_nfuncs && ntg < 64; j++)
            if (g_sfn[j].closed && strcmp(g_sfn[j].mname, nm) == 0) tg[ntg++] = j;
        }
        argc = in->win_n;
        for (j = 0; j < ntg; j++)
          for (a = 0; a < 16; a++) {
            unsigned cls = a < argc && in->win_val != NULL ? scan_class(i, in->win_val[a]) : a < argc ? SC_UNK : SC_UNDEF;
            if ((g_sfn[tg[j]].pcls[a] | cls) != g_sfn[tg[j]].pcls[a]) { g_sfn[tg[j]].pcls[a] |= cls; changed = 1; }
          }
      }
    }
    if (!changed) break;
  }
  g_scan_fixing = 0;
  g_scan_params_ready = 1;
}

static void scan_run(void) {
  int i, pass;
  if (g_scan_done) return;
  g_scan_done = 1;
  if (g_unit_funcs == NULL || g_unit_ctx == NULL) return;
  g_sfn = calloc((size_t)(g_unit_nfuncs > 0 ? g_unit_nfuncs : 1), sizeof(*g_sfn));
  if (g_sfn == NULL) return;
  for (i = 0; i < g_unit_nfuncs; i++) {
    const JSAotGenFunc *g = &g_unit_funcs[i];
    g_sfn[i].owner = -1;
    if (i + 1 < g_unit_nfuncs && memcmp(g->digest, g_unit_funcs[i + 1].digest, 16) == 0) continue;
    if (g->eligible != AOT_GEN_ELIGIBLE) { g_scan_opaque++; continue; }
    if (q2_build(g, &g_sfn[i].q) < 0) { q2_free(&g_sfn[i].q); g_scan_opaque++; continue; }
    g_sfn[i].g = g;
    g_sfn[i].ok = 1;
    g_scan_funcs++;
  }
  /* global functions: name -> function; then the class of what the other globals are given, to a
     fixed point (a global's value may be another global's) */
  for (pass = 0; pass < 4; pass++) {
    for (i = 0; i < g_scan_nglobals; i++)
      if (g_scan_globals[i].fn < 0) { g_scan_globals[i].mask = 0; g_scan_globals[i].nstores = 0; }
    for (i = 0; i < g_unit_nfuncs; i++) {
      const Q2Func *fn;
      int k;
      if (!g_sfn[i].ok) continue;
      fn = &g_sfn[i].q;
      for (k = 0; k < fn->ninsns; k++) {
        const Q2Insn *in = &fn->insns[k];
        char nm[48];
        ScanGlobal *gl;
        if (in->rop == ROP_define_global_func && in->nuse >= 1 && scan_atom(in, NULL, nm, sizeof(nm))) {
          int ch = scan_closure_fn(&g_sfn[i], scan_def(fn, in->use[0]));
          gl = scan_global(nm, 1);
          if (gl != NULL && pass == 0) { gl->nstores++; gl->mask |= SC_UNK; if (ch >= 0) gl->fn = ch; }
        } else if (in->rop == ROP_define_class && in->nuse >= 1 && scan_atom(in, NULL, nm, sizeof(nm))) {
          int ch = scan_closure_fn(&g_sfn[i], scan_def(fn, in->use[0]));
          gl = scan_global(nm, 1);
          if (gl != NULL && pass == 0) { gl->nstores++; gl->mask |= SC_UNK; if (ch >= 0) gl->fn = ch; }
        } else if (in->rop == ROP_put_global && in->nuse >= 1 && scan_atom(in, NULL, nm, sizeof(nm))) {
          gl = scan_global(nm, 1);
          if (gl != NULL && gl->fn < 0) { gl->nstores++; gl->mask |= scan_class(i, in->use[0]); }
        }
      }
    }
  }
  /* closures: what each is given to */
  for (i = 0; i < g_unit_nfuncs; i++) {
    const Q2Func *fn;
    int k;
    if (!g_sfn[i].ok) continue;
    fn = &g_sfn[i].q;
    for (k = 0; k < fn->ninsns; k++) {
      const Q2Insn *in = &fn->insns[k];
      if (in->rop == ROP_make_closure) {
        int ch = scan_closure_fn(&g_sfn[i], in);
        if (ch < 0) { g_scan_unresolved++; continue; }
        scan_closure_uses(i, in, ch);
      }
    }
  }
  for (i = 0; i < g_unit_nfuncs; i++)
    if (g_sfn[i].ok) scan_attribute_methods(i);
  /* a method read as `call`, `apply` or `bind` may run on an instance of another class */
  for (i = 0; i < g_unit_nfuncs; i++) {
    const Q2Func *fn;
    int k;
    if (!g_sfn[i].ok) continue;
    fn = &g_sfn[i].q;
    for (k = 0; k < fn->ninsns; k++) {
      char nm[48];
      if (scan_atom(&fn->insns[k], NULL, nm, sizeof(nm)) &&
          (strcmp(nm, "call") == 0 || strcmp(nm, "apply") == 0 || strcmp(nm, "bind") == 0))
        g_scan_leaks++;
    }
  }
  scan_params();
  /* the stores, to a fixed point: the class of a field read is what the stores of that name give */
  g_scan_fixing = 1;
  for (pass = 0; pass < 8; pass++) {
    int changed = 0;
    if (pass == 7) g_scan_fixing = 0;
    g_scan_nthis = 0;
    g_scan_escapes = 0;
    for (i = 0; i < g_scan_nnames; i++) { g_scan_names[i].mask = 0; g_scan_names[i].nstores = 0; }
  for (i = 0; i < g_unit_nfuncs; i++) {
    const Q2Func *fn;
    int k;
    if (!g_sfn[i].ok) continue;
    fn = &g_sfn[i].q;
    for (k = 0; k < fn->ninsns; k++) {
      const Q2Insn *in = &fn->insns[k];
      switch (in->rop) {
        case ROP_put_field:
        case ROP_put_field_xfer:
        case ROP_define_field: {
          char nm[48];
          ScanName *sn = scan_atom(in, NULL, nm, sizeof(nm)) ? scan_name(nm) : NULL;
          unsigned cls;
          int isthis = 0;
          if (sn == NULL) { g_scan_escapes++; break; }
          cls = in->rop == ROP_define_field || in->narg < 4 || in->arg[3].kind != 'r' ? SC_UNK : scan_class(i, in->argval[3]);
          if (in->arg[0].kind == 'r') {
            const Q2Insn *d = scan_def(fn, in->argval[0]);
            isthis = d != NULL && d->rop == ROP_load_this;
          }
          sn->nstores++;
          if (isthis && g_scan_nthis < SCAN_THIS) {
            ScanThis *st = &g_scan_this[g_scan_nthis++];
            st->name = (int)(sn - g_scan_names);
            st->cls = cls;
            st->fn = i;
          } else {
            sn->mask |= cls;
          }
          break;
        }
        case ROP_put_element:
        case ROP_put_element_xfer: {
          const Q2Value *kv = in->narg > 1 && in->arg[1].kind == 'r' && in->argval[1] >= 0 ? &fn->values[in->argval[1]] : NULL;
          if (kv == NULL || !kv->is_const || kv->con_kind != Q2_CON_INT) g_scan_escapes++;
          break;
        }
        case ROP_define_element:
        case ROP_put_private:
        case ROP_define_private:
        case ROP_put_super_value:
        case ROP_super_put:
        case ROP_put_ref:
        case ROP_put_ref_value:
        case ROP_copy_props:
        case ROP_set_proto:
        case ROP_delete:
        case ROP_eval:
        case ROP_apply_eval:
        case ROP_with_put:
        case ROP_with_delete:
        case ROP_object_fill:
        case ROP_object_template:
          g_scan_escapes++;
          break;
        default: break;
      }
    }
  }

    for (i = 0; i < g_scan_nnames; i++) {
      unsigned all = g_scan_names[i].mask;
      int t;
      for (t = 0; t < g_scan_nthis; t++)
        if (g_scan_this[t].name == i) all |= g_scan_this[t].cls;
      if (all != g_scan_names[i].prev) { g_scan_names[i].prev = all; changed = 1; }
    }
    if (!changed && pass < 7) pass = 6;
  }
  g_scan_fixing = 0;
  if (getenv("QJS_AOT_SCANDEBUG"))
    fprintf(stderr, "scan: funcs=%d opaque=%d escapes=%d unresolved=%d attributed=%d derived=%d reborrow=%d globals=%d\n",
            g_scan_funcs, g_scan_opaque, g_scan_escapes, g_scan_unresolved, g_scan_attributed, g_scan_derived,
            g_scan_leaks, g_scan_nglobals);
}

/* The verdict on a promise for a slot named `name` of the layout built by constructor `own`:
   -1 contradicted (some store visibly puts another type there), 1 proven, 2 consistent, 0 the
   source says nothing. */
int qjs_aot_scan_verdict(const char *name, int claim, const uint8_t own[16], const uint8_t (*ctors)[16], int nctors) {
  const ScanName *sn = NULL;
  int i, ownfn = -1;
  unsigned seen, allowed, definite;
  (void)ctors;
  (void)nctors;
  if (getenv("QJS_AOT_NOSCAN") != NULL) return 0;
  scan_run();
  if (g_sfn == NULL) return 0;
  for (i = 0; i < g_unit_nfuncs; i++)
    if (g_sfn[i].g != NULL && memcmp(g_sfn[i].g->digest, own, 16) == 0) ownfn = i;
  for (i = 0; i < g_scan_nnames; i++)
    if (strcmp(g_scan_names[i].name, name) == 0) sn = &g_scan_names[i];
  if (sn == NULL || sn->nstores == 0) return 0;
  seen = sn->mask;
  for (i = 0; i < g_scan_nthis; i++) {
    int owner;
    if (g_scan_this[i].name != (int)(sn - g_scan_names)) continue;
    owner = g_sfn[g_scan_this[i].fn].owner;
    if (g_sfn[g_scan_this[i].fn].escaped) owner = -1;
    if (owner >= 0 && owner != ownfn && g_scan_derived == 0 && g_scan_leaks == 0) continue;
    seen |= g_scan_this[i].cls;
    if (getenv("QJS_AOT_SCANDEBUG")) fprintf(stderr, "scan   this-store %s in fn %d (%s) cls=%x\n", name, g_scan_this[i].fn, g_sfn[g_scan_this[i].fn].g->name, g_scan_this[i].cls);
  }
  if (getenv("QJS_AOT_SCANDEBUG")) fprintf(stderr, "scan name=%s ownfn=%d seen=%x nonthis=%x esc=%d\n", name, ownfn, seen, sn->mask, g_scan_escapes);
  if (claim == QJS_AOT_CLAIM_INT) {
    allowed = SC_INT | SC_INTOV; definite = SC_DBL | SC_BOOL | SC_NULL | SC_UNDEF;
  } else {
    allowed = SC_INT | SC_INTOV | SC_DBL | SC_NUM | SC_BOOL | SC_NULL | SC_UNDEF; definite = 0;
  }
  if (seen & definite) return -1;
  if ((seen & ~allowed) == 0)
    return g_scan_escapes == 0 && g_scan_opaque == 0 && g_scan_unresolved == 0 ? 1 : 2;
  return 0;
}

void qjs_aot_scan_init(JSContext *ctx, const JSAotGenFunc *funcs, int n) {
  g_unit_ctx = ctx;
  g_unit_funcs = funcs;
  g_unit_nfuncs = n;
}
