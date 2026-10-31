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

/* ---- the proof backend, R2 (AOT-REDESIGN.md sections 1, 3, 5, 8) --------------------------------------
   `aot_p_HEX` is a fast body for a LEAF function: no call, no loop, one chain of blocks. It is entered from the function's native entry
   before that builds a frame, and it either returns the function's result or `E_PEXIT`, having done nothing observable: the caller
   then runs the body it always ran. So an exit needs no frame state: every exit-capable test comes before the first store, which this
   file checks while it emits (a leaf that would exit after a store is not compiled).

   What the bundle analysis contributes: the property position of every field the function reads or writes (qjs_aot_bundle_field), the same in every
   receiver it knows, so the access is `prop[slot]` at a constant offset. The analysis is a hypothesis about the receivers; the body verifies
   it once per receiver shape (`pl_` below: qjs_aot_shape_slot says the property lives at that offset, as a data property, and as a writable one for a
   store) and keeps the verified shapes (at most four per receiver) until the observed-shape epoch moves. A receiver of any other shape
   is verified when first seen and exits when it does not fit.

   Reference counts follow the borrow rule in its leaf form: a loaded value is borrowed (no call can cut its holder); a returned value is
   duplicated; a store duplicates the new value and releases the old one after the last use of every borrowed value, at the return.

   Values are all JSValue locals `t<N>`; an int is taken out of one with a tag test. */

#define FAST_MAXG 6
#define FAST_MAXF 6

typedef struct FGuard {
  int val;       /* the SSA value of the receiver */
  int hoist;     /* checked at the top: the receiver is `this` or an argument */
  int nf;
  char name[FAST_MAXF][48];
  int slot[FAST_MAXF];
  int write[FAST_MAXF];
} FGuard;

typedef struct FCtx {
  FILE *o;
  const Q2Func *fn;
  const char *hex;
  FGuard g[FAST_MAXG];
  int ng;
  int nstores;
  int store_seen;
  char why[160];
} FCtx;

#define FAST_MAXFN 4096
static char g_fast_has[FAST_MAXFN][33];
static int g_fast_n;
static int g_fast_ok, g_fast_no, g_fast_prelude;

int fast_has(const char hex[33]) {
  int i;
  for (i = 0; i < g_fast_n; i++)
    if (strcmp(g_fast_has[i], hex) == 0) return 1;
  return 0;
}

static int fx_fail(FCtx *c, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(c->why, sizeof(c->why), fmt, ap);
  va_end(ap);
  return 0;
}

static int fx_guard_of(FCtx *c, int val, int hoist) {
  int i;
  for (i = 0; i < c->ng; i++)
    if (c->g[i].val == val) return i;
  if (c->ng >= FAST_MAXG) return -1;
  memset(&c->g[c->ng], 0, sizeof(FGuard));
  c->g[c->ng].val = val;
  c->g[c->ng].hoist = hoist;
  return c->ng++;
}

static int fx_guard_field(FCtx *c, int gi, const char *name, int slot, int write) {
  FGuard *g = &c->g[gi];
  int i;
  for (i = 0; i < g->nf; i++)
    if (strcmp(g->name[i], name) == 0) {
      if (g->slot[i] != slot) return 0;
      g->write[i] |= write;
      return 1;
    }
  if (g->nf >= FAST_MAXF || strlen(name) >= sizeof(g->name[0])) return 0;
  snprintf(g->name[g->nf], sizeof(g->name[0]), "%s", name);
  g->slot[g->nf] = slot;
  g->write[g->nf] = write;
  g->nf++;
  return 1;
}

static void fx_cstr(FILE *o, const char *s) {
  fputc('"', o);
  for (; *s; s++) {
    if (*s == '"' || *s == '\\') fputc('\\', o);
    fputc(*s, o);
  }
  fputc('"', o);
}

/* The operand's field name and atom text. */
static int fx_atom(const Q2Insn *in, char *out, size_t cap) {
  int a;
  for (a = 0; a < in->narg && in->arg[a].kind != 'a'; a++) {}
  if (a >= in->narg) return 0;
  return qjs_aot_bundle_atom_name((uint32_t)in->arg[a].val, out, cap);
}

/* Which values are `this` or an argument (the receivers whose shape test can come first). */
static int fx_is_entry_value(const Q2Func *fn, Q2ValueId v) {
  int di;
  const Q2Insn *d;
  if ((int32_t)v < 0 || (int32_t)v >= fn->nvalues) return 0;
  di = fn->values[v].insn;
  if (di < 0 || di >= fn->ninsns) return 0;
  d = &fn->insns[di];
  switch (d->rop) {
    case ROP_load_this:
    case ROP_get_arg:
    case ROP_get_arg2:
      return 1;
    case ROP_get_this_field:
    case ROP_get_this_field2:
      return d->ndef >= 2 && d->def[1] == v; /* the copy of `this` */
    default:
      return 0;
  }
}

/* The guard on a value and the field the instruction touches through it. */
static int fx_access(FCtx *c, const Q2Insn *in, Q2ValueId recv, int write) {
  char nm[48];
  int slot, is_add = 0, gi;
  unsigned cls = 0;
  if (!fx_atom(in, nm, sizeof(nm))) return fx_fail(c, "pc %d: the field name is unreadable", in->pc);
  if (!qjs_aot_bundle_field(c->fn->g, in->pc, write, &slot, &cls, &is_add))
    return fx_fail(c, "pc %d: .%s has no proven position in its receivers", in->pc, nm);
  if (is_add) return fx_fail(c, "pc %d: .%s is added by this function (a constructor)", in->pc, nm);
  gi = fx_guard_of(c, (int)recv, fx_is_entry_value(c->fn, recv));
  if (gi < 0) return fx_fail(c, "pc %d: too many receivers", in->pc);
  if (!fx_guard_field(c, gi, nm, slot, write)) return fx_fail(c, "pc %d: .%s at two positions", in->pc, nm);
  return 1;
}

static void fx_emit_guard_check(FCtx *c, int gi, const char *indent) {
  FILE *o = c->o;
  fprintf(o, "%s{ const void *sh_ = JS_VALUE_GET_TAG(t%d) == JS_TAG_OBJECT ? *(const void *const *)((const char *)JS_VALUE_GET_PTR(t%d) + QJS_AOT_OFF_JSOBJECT_SHAPE) : NULL;\n",
          indent, c->g[gi].val, c->g[gi].val);
  fprintf(o, "%s  if (__builtin_expect(!(sh_ != NULL && ((pg_%s_%d_n > 0 && pg_%s_%d[0] == sh_) || (pg_%s_%d_n > 1 && pg_%s_%d[1] == sh_) ||\n", indent, c->hex, gi, c->hex, gi, c->hex, gi, c->hex, gi);
  fprintf(o, "%s       (pg_%s_%d_n > 2 && pg_%s_%d[2] == sh_) || (pg_%s_%d_n > 3 && pg_%s_%d[3] == sh_))), 0)) {\n", indent, c->hex, gi, c->hex, gi, c->hex, gi, c->hex, gi);
  fprintf(o, "%s    if (!pl_%s_%d(ctx, t%d)) goto pexit;\n%s  }\n%s}\n", indent, c->hex, gi, c->g[gi].val, indent, indent);
  {
    int k, writes = 0;
    for (k = 0; k < c->g[gi].nf; k++) writes |= c->g[gi].write[k];
    /* a store the engine must make itself: the object is a prototype (the watchpoints see it there) */
    if (writes) fprintf(o, "%sif (__builtin_expect(q2_is_proto(t%d), 0)) goto pexit;\n", indent, c->g[gi].val);
  }
}

/* The statics and the learner of every guard. */
static void fx_emit_guard_decls(FCtx *c) {
  FILE *o = c->o;
  int gi, k;
  for (gi = 0; gi < c->ng; gi++) {
    FGuard *g = &c->g[gi];
    fprintf(o, "static const void *pg_%s_%d[4]; static uint8_t pg_%s_%d_n, pg_%s_%d_bad;\n", c->hex, gi, c->hex, gi, c->hex, gi);
    fprintf(o, "static int pl_%s_%d(JSContext *ctx, JSValue v)\n{\n", c->hex, gi);
    fprintf(o, "  const void *sh;\n  uint32_t off;\n  JSAtom a;\n  int ok = 1;\n");
    fprintf(o, "  if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT || pg_%s_%d_n >= 4 || pg_%s_%d_bad >= 8) return 0;\n", c->hex, gi, c->hex, gi);
    fprintf(o, "  sh = *(const void *const *)((const char *)JS_VALUE_GET_PTR(v) + QJS_AOT_OFF_JSOBJECT_SHAPE);\n");
    for (k = 0; k < g->nf; k++) {
      fprintf(o, "  a = JS_NewAtom(ctx, ");
      fx_cstr(o, g->name[k]);
      fprintf(o, ");\n  ok = ok && %s(sh, a, &off) && off == %du * QJS_AOT_SIZEOF_JSPROPERTY;\n  JS_FreeAtom(ctx, a);\n",
              g->write[k] ? "qjs_aot_shape_wslot" : "qjs_aot_shape_rslot", g->slot[k]);
    }
    if (getenv("QJS_AOT_FASTSTATS") != NULL)
      fprintf(o, "  if (!ok) { pg_%s_%d_bad++; pst_%s[2]++; return 0; }\n", c->hex, gi, c->hex);
    else
      fprintf(o, "  if (!ok) { pg_%s_%d_bad++; return 0; }\n", c->hex, gi);
    if (getenv("QJS_AOT_FASTSTATS") != NULL) fprintf(o, "  pst_%s[3]++;\n", c->hex);
    /* the verified shape is held: it cannot be freed (its address reused) or edited in place, so the set needs no epoch */
    fprintf(o, "  if (q2_phn >= Q2_PH_MAX) return 0;\n  q2_ph[q2_phn++] = qjs_aot_shape_hold(sh);\n");
    /* The first sight of a shape runs the body the function always had: its cache cells (which the callers' copies of this function
       validate) are filled by it, and the shape is used by this body from the next call. */
    fprintf(o, "  pg_%s_%d[pg_%s_%d_n++] = sh;\n  return 0;\n}\n", c->hex, gi, c->hex, gi);
  }
}

static const char *fx_cmp(int rop) {
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

/* An int operand: a test and the payload. */
static void fx_int(FCtx *c, int v, const char *name, int *seq) {
  fprintf(c->o, "    int32_t %s%d; if (__builtin_expect(JS_VALUE_GET_TAG(t%d) != JS_TAG_INT, 0)) goto pexit; %s%d = JS_VALUE_GET_INT(t%d);\n", name, *seq, v, name, *seq, v);
}

static const char *fx_slot_expr(int slot, char *buf, size_t cap) {
  snprintf(buf, cap, "%du * QJS_AOT_SIZEOF_JSPROPERTY", slot);
  return buf;
}

/* Walk the function once: decide, collect the guards, and (when emit) write the body. */
static int fx_walk(FCtx *c, int emit) {
  const Q2Func *fn = c->fn;
  FILE *o = c->o;
  int ii, nstore = 0, seq = 0;
  char sbuf[48];
  c->store_seen = 0;
  for (ii = 0; ii < fn->ninsns; ii++) {
    const Q2Insn *in = &fn->insns[ii];
    int dv = in->ndef >= 1 ? (int)in->def[0] : -1, dv2 = in->ndef >= 2 ? (int)in->def[1] : -1;
    int u0 = in->nuse >= 1 ? (int)in->use[0] : -1, u1 = in->nuse >= 2 ? (int)in->use[1] : -1;
    int exits = 0, nstore_before = nstore;
    switch (in->rop) {
      case ROP_load_undefined:
        if (emit) fprintf(o, "    t%d = JS_UNDEFINED;\n", dv);
        break;
      case ROP_load_null:
        if (emit) fprintf(o, "    t%d = JS_NULL;\n", dv);
        break;
      case ROP_load_true:
        if (emit) fprintf(o, "    t%d = JS_TRUE;\n", dv);
        break;
      case ROP_load_false:
        if (emit) fprintf(o, "    t%d = JS_FALSE;\n", dv);
        break;
      case ROP_load_i8:
      case ROP_load_i8_s:
      case ROP_load_i32:
        if (in->narg < 2 || dv < 0) return fx_fail(c, "pc %d: a constant of unknown shape", in->pc);
        if (emit) fprintf(o, "    t%d = JS_MKVAL(JS_TAG_INT, %d);\n", dv, (int)in->arg[1].val);
        break;
      case ROP_load_this:
        if (dv < 0) return fx_fail(c, "pc %d: load_this without a destination", in->pc);
        if (emit) fprintf(o, "    t%d = this_obj;\n", dv);
        break;
      case ROP_get_arg:
        if (dv < 0 || in->narg < 2) return fx_fail(c, "pc %d: get_arg", in->pc);
        if (emit) fprintf(o, "    t%d = %d < argc ? argv[%d] : JS_UNDEFINED;\n", dv, (int)in->arg[1].val, (int)in->arg[1].val);
        break;
      case ROP_get_arg2:
        if (dv < 0 || dv2 < 0 || in->narg < 4) return fx_fail(c, "pc %d: get_arg2", in->pc);
        if (emit)
          fprintf(o, "    t%d = %d < argc ? argv[%d] : JS_UNDEFINED; t%d = %d < argc ? argv[%d] : JS_UNDEFINED;\n", dv,
                  (int)in->arg[1].val, (int)in->arg[1].val, dv2, (int)in->arg[3].val, (int)in->arg[3].val);
        break;
      case ROP_move:
      case ROP_move_s:
      case ROP_move_xfer:
        if (dv < 0 || u0 < 0) return fx_fail(c, "pc %d: move", in->pc);
        if (emit) fprintf(o, "    t%d = t%d;\n", dv, u0);
        break;
      case ROP_get_this_field:
      case ROP_get_this_field2: {
        /* def[0] is the value, def[1] the copy of `this` the handler leaves in the register */
        char nm[48];
        int gi;
        if (dv < 0 || dv2 < 0) return fx_fail(c, "pc %d: get_this_field", in->pc);
        if (!fx_atom(in, nm, sizeof(nm))) return fx_fail(c, "pc %d: the field name is unreadable", in->pc);
        if (!emit) {
          if (!fx_access(c, in, dv2, 0)) return 0;
        }
        gi = fx_guard_of(c, dv2, 1);
        if (emit) {
          int k, fs = -1;
          for (k = 0; k < c->g[gi].nf; k++) if (strcmp(c->g[gi].name[k], nm) == 0) fs = c->g[gi].slot[k];
          fprintf(o, "    t%d = this_obj; t%d = *q2_slotp(t%d, %s);\n", dv2, dv, dv2, fx_slot_expr(fs, sbuf, sizeof(sbuf)));
        }
        exits = 0; /* the guard on `this` came first */
        break;
      }
      case ROP_get_field:
      case ROP_get_field_own:
      case ROP_get_field2:
      case ROP_get_field2_own: {
        char nm[48];
        int gi, k, fs = -1;
        if (dv < 0 || u0 < 0) return fx_fail(c, "pc %d: get_field", in->pc);
        if (!fx_atom(in, nm, sizeof(nm))) return fx_fail(c, "pc %d: the field name is unreadable", in->pc);
        if (!emit && !fx_access(c, in, u0, 0)) return 0;
        gi = fx_guard_of(c, u0, fx_is_entry_value(fn, u0));
        if (emit) {
          for (k = 0; k < c->g[gi].nf; k++) if (strcmp(c->g[gi].name[k], nm) == 0) fs = c->g[gi].slot[k];
          if (!c->g[gi].hoist) fx_emit_guard_check(c, gi, "    ");
          fprintf(o, "    t%d = *q2_slotp(t%d, %s);\n", dv, u0, fx_slot_expr(fs, sbuf, sizeof(sbuf)));
        }
        exits = !c->g[gi].hoist;
        break;
      }
      case ROP_put_field:
      case ROP_put_field_xfer: {
        char nm[48];
        int gi, k, fs = -1;
        if (u0 < 0 || u1 < 0) return fx_fail(c, "pc %d: put_field", in->pc);
        if (!fx_atom(in, nm, sizeof(nm))) return fx_fail(c, "pc %d: the field name is unreadable", in->pc);
        if (!emit && !fx_access(c, in, u0, 1)) return 0;
        gi = fx_guard_of(c, u0, fx_is_entry_value(fn, u0));
        if (emit) {
          for (k = 0; k < c->g[gi].nf; k++) if (strcmp(c->g[gi].name[k], nm) == 0) fs = c->g[gi].slot[k];
          if (!c->g[gi].hoist) fx_emit_guard_check(c, gi, "    ");
          /* the engine's own store takes an object that is a prototype (the watchpoints; tested with the hoisted guard), and a layout's claim on the slot */
          if (!c->g[gi].hoist) fprintf(o, "    if (__builtin_expect(q2_is_proto(t%d), 0)) goto pexit;\n", u0);
          fprintf(o, "    if (__builtin_expect(Q2_CLAIM_BAD(t%d, %s, t%d), 0)) goto pexit;\n", u0, fx_slot_expr(fs, sbuf, sizeof(sbuf)), u1);
          fprintf(o, "    { JSValue *sp_ = q2_slotp(t%d, %s); old%d = *sp_; q2_dup_p(t%d); *sp_ = t%d; }\n", u0,
                  fx_slot_expr(fs, sbuf, sizeof(sbuf)), nstore, u1, u1);
        }
        exits = !c->g[gi].hoist || qjs_aot_layout_claims_any();
        nstore++;
        c->store_seen = 1;
        break;
      }
      case ROP_get_length:
        if (dv < 0 || u0 < 0) return fx_fail(c, "pc %d: get_length", in->pc);
        if (emit)
          fprintf(o, "    { double len_; if (__builtin_expect(!q2_array_length(ctx, t%d, &len_, NULL, NULL), 0)) goto pexit; t%d = JS_MKVAL(JS_TAG_INT, (int32_t)len_); }\n", u0, dv);
        exits = 1;
        break;
      case ROP_get_element:
        if (dv < 0 || u0 < 0 || u1 < 0) return fx_fail(c, "pc %d: get_element", in->pc);
        if (emit)
          fprintf(o, "    { JSValue e_; if (__builtin_expect(!q2_elem_hit_lz(t%d, t%d, &e_, 1), 0)) goto pexit; t%d = e_; }\n", u0, u1, dv);
        exits = 1;
        break;
      case ROP_lt: case ROP_lte: case ROP_gt: case ROP_gte:
      case ROP_eq: case ROP_neq: case ROP_strict_eq: case ROP_strict_neq:
        if (dv < 0 || u0 < 0 || u1 < 0) return fx_fail(c, "pc %d: compare", in->pc);
        if (emit) {
          fprintf(o, "    {\n");
          fx_int(c, u0, "x", &seq);
          fx_int(c, u1, "y", &seq);
          fprintf(o, "    t%d = x%d %s y%d ? JS_TRUE : JS_FALSE; }\n", dv, seq, fx_cmp(in->rop), seq);
          seq++;
        }
        exits = 1;
        break;
      case ROP_add: case ROP_sub: case ROP_mul:
        if (dv < 0 || u0 < 0 || u1 < 0) return fx_fail(c, "pc %d: arithmetic", in->pc);
        if (emit) {
          fprintf(o, "    {\n");
          fx_int(c, u0, "x", &seq);
          fx_int(c, u1, "y", &seq);
          if (in->rop == ROP_mul)
            fprintf(o, "    int32_t r%d; if (__builtin_expect(__builtin_mul_overflow(x%d, y%d, &r%d) || (r%d == 0 && (x%d < 0 || y%d < 0)), 0)) goto pexit;\n", seq, seq, seq, seq, seq, seq, seq);
          else
            fprintf(o, "    int32_t r%d; if (__builtin_expect(__builtin_%s_overflow(x%d, y%d, &r%d), 0)) goto pexit;\n", seq, in->rop == ROP_add ? "add" : "sub", seq, seq, seq);
          fprintf(o, "    t%d = JS_MKVAL(JS_TAG_INT, r%d); }\n", dv, seq);
          seq++;
        }
        exits = 1;
        break;
      case ROP_inc: case ROP_dec:
        if (dv < 0 || u0 < 0) return fx_fail(c, "pc %d: inc/dec", in->pc);
        if (emit) {
          fprintf(o, "    {\n");
          fx_int(c, u0, "x", &seq);
          fprintf(o, "    int32_t r%d; if (__builtin_expect(__builtin_%s_overflow(x%d, 1, &r%d), 0)) goto pexit;\n", seq, in->rop == ROP_inc ? "add" : "sub", seq, seq);
          fprintf(o, "    t%d = JS_MKVAL(JS_TAG_INT, r%d); }\n", dv, seq);
          seq++;
        }
        exits = 1;
        break;
      case ROP_return:
        if (u0 < 0) return fx_fail(c, "pc %d: return", in->pc);
        if (emit) {
          int k;
          fprintf(o, "    { JSValue ret_ = t%d; q2_dup_p(ret_);\n", u0);
          for (k = 0; k < nstore; k++) fprintf(o, "      if (JS_VALUE_HAS_REF_COUNT(old%d)) JS_FreeValue(ctx, old%d);\n", k, k);
          fprintf(o, "      return ret_; }\n");
        }
        break;
      case ROP_return_undefined:
        if (emit) {
          int k;
          for (k = 0; k < nstore; k++) fprintf(o, "    if (JS_VALUE_HAS_REF_COUNT(old%d)) JS_FreeValue(ctx, old%d);\n", k, k);
          fprintf(o, "    return JS_UNDEFINED;\n");
        }
        break;
      default: {
        char nm[48];
        e_rop_name((uint16_t)in->rop, nm, sizeof(nm));
        return fx_fail(c, "pc %d: %s is not in the leaf subset", in->pc, nm);
      }
    }
    /* an exit after a store would need the frame the exit does not have (the store itself counts: it is checked before it is made) */
    if (exits && c->store_seen && nstore_before > 0) return fx_fail(c, "pc %d: can exit after a store", in->pc);
  }
  c->nstores = nstore;
  return 1;
}

int fast_emit(FILE *f, const Q2Func *fn, const char hex[33], char *why, size_t whyn) {
  FCtx c;
  int b, i, nv = fn->nvalues;
  char *buf = NULL;
  size_t blen = 0;
  FILE *o;
  /* Off by default (reviewer MSG 73): a leaf call is already as cheap as the fast body, and the old backend's copies read the leaf's own cache cells,
     which the keep-warm exits exist to protect. QJS_AOT_FAST=1 turns it on. */
  if (getenv("QJS_AOT_FAST") == NULL || getenv("QJS_AOT_NOFAST") != NULL) { snprintf(why, whyn, "off (QJS_AOT_FAST=1 turns it on)"); return 0; }
  memset(&c, 0, sizeof(c));
  c.fn = fn;
  c.hex = hex;
  if (!fn->ok || fn->g->func_kind != 0 || fn->g->ctl_count != 0 || fn->g->arg_count > 8) {
    snprintf(why, whyn, "not a plain function");
    g_fast_no++;
    return 0;
  }
  for (b = 0; b < fn->nblocks; b++) {
    const Q2Block *B = &fn->blocks[b];
    if (B->nparams != 0 || B->nsucc > 1 || (B->nsucc == 1 && B->succ_block[0] != b + 1)) {
      snprintf(why, whyn, "control flow beyond a chain of blocks");
      g_fast_no++;
      return 0;
    }
  }
  if (!fx_walk(&c, 0)) {
    snprintf(why, whyn, "%s", c.why);
    g_fast_no++;
    return 0;
  }
  if (c.ng == 0 && c.nstores == 0) {
    /* a function with no field access gains nothing over the entry it already has */
  }
  o = open_memstream(&buf, &blen);
  if (o == NULL) { snprintf(why, whyn, "no memory"); return 0; }
  c.o = o;
  if (getenv("QJS_AOT_FASTSTATS") != NULL) {
    fprintf(o, "static unsigned long pst_%s[4];\n", hex);
    fprintf(o, "static void __attribute__((destructor)) pstat_%s(void) { if (pst_%s[0]) fprintf(stderr, \"fast %%s calls=%%lu exits=%%lu learn_bad=%%lu learned=%%lu\\n\", \"%s\", pst_%s[0], pst_%s[1], pst_%s[2], pst_%s[3]); }\n", hex, hex, hex, hex, hex, hex, hex);
  }
  fx_emit_guard_decls(&c);
  fprintf(o, "/* aot_p_%s: the fast body (R2): %d receiver guard(s), %d store(s) */\n", hex, c.ng, c.nstores);
  fprintf(o, "static inline __attribute__((always_inline)) JSValue aot_p_%s(JSContext *ctx, JSValue this_obj, JSValue *argv, int argc)\n{\n", hex);
  fprintf(o, "    (void)this_obj; (void)argv; (void)argc;\n");
  /* every 1024th call takes the old body, which keeps its cache cells warm for the callers' copies */
  fprintf(o, "    { static unsigned pc_; if (__builtin_expect((++pc_ & 1023u) == 0, 0)) goto pexit; }\n");
  if (getenv("QJS_AOT_FASTSTATS") != NULL) fprintf(o, "    pst_%s[0]++;\n", hex);
  for (i = 0; i < nv; i++) fprintf(o, "    JSValue t%d = JS_UNDEFINED;\n", i);
  for (i = 0; i < c.nstores; i++) fprintf(o, "    JSValue old%d = JS_UNDEFINED;\n", i);
  /* the one fact: no lazy JSON document exists in this runtime */
  fprintf(o, "    if (__builtin_expect(E_LAZY_SEEN(ctx), 0)) goto pexit;\n");
  /* the receivers of `this` and the arguments are loaded first, then checked, before anything else can leave */
  for (i = 0; i < fn->ninsns; i++) {
    const Q2Insn *in = &fn->insns[i];
    int gi;
    for (gi = 0; gi < c.ng; gi++) (void)gi;
    if (in->rop == ROP_load_this && in->ndef >= 1) fprintf(o, "    t%d = this_obj;\n", (int)in->def[0]);
    if (in->rop == ROP_get_arg && in->ndef >= 1 && in->narg >= 2)
      fprintf(o, "    t%d = %d < argc ? argv[%d] : JS_UNDEFINED;\n", (int)in->def[0], (int)in->arg[1].val, (int)in->arg[1].val);
    if (in->rop == ROP_get_arg2 && in->ndef >= 2 && in->narg >= 4)
      fprintf(o, "    t%d = %d < argc ? argv[%d] : JS_UNDEFINED; t%d = %d < argc ? argv[%d] : JS_UNDEFINED;\n", (int)in->def[0],
              (int)in->arg[1].val, (int)in->arg[1].val, (int)in->def[1], (int)in->arg[3].val, (int)in->arg[3].val);
    if ((in->rop == ROP_get_this_field || in->rop == ROP_get_this_field2) && in->ndef >= 2) fprintf(o, "    t%d = this_obj;\n", (int)in->def[1]);
  }
  for (i = 0; i < c.ng; i++)
    if (c.g[i].hoist) fx_emit_guard_check(&c, i, "    ");
  if (!fx_walk(&c, 1)) {
    fclose(o);
    free(buf);
    snprintf(why, whyn, "%s", c.why);
    g_fast_no++;
    return 0;
  }
  if (getenv("QJS_AOT_FASTSTATS") != NULL)
    fprintf(o, "pexit:\n    pst_%s[1]++;\n    return E_PEXIT;\n}\n\n", hex);
  else
    fprintf(o, "pexit:\n    return E_PEXIT;\n}\n\n");
  fclose(o);
  fwrite(buf, 1, blen, f);
  free(buf);
  if (g_fast_n < FAST_MAXFN) snprintf(g_fast_has[g_fast_n++], 33, "%s", hex);
  g_fast_ok++;
  return 1;
}

void fast_emit_entry_call(FILE *f, const char hex[33]) {
  fprintf(f,
          "    {\n"
          "        JSValue pr_ = aot_p_%s(ctx, this_obj, argv, argc);\n"
          "        if (__builtin_expect(JS_VALUE_GET_TAG(pr_) != JS_TAG_UNINITIALIZED, 1)) return pr_;\n"
          "    }\n",
          hex);
}

void fast_counts(int *ok, int *no) {
  *ok = g_fast_ok;
  *no = g_fast_no;
}

/* The same lines as a string (for a format argument). */
const char *fast_entry_text(const char hex[33]) {
  static char buf[4][400];
  static int rot;
  char *b = buf[rot++ & 3];
  snprintf(b, 400,
           "    {\n"
           "        JSValue pr_ = aot_p_%s(ctx, this_obj, argv, argc);\n"
           "        if (__builtin_expect(JS_VALUE_GET_TAG(pr_) != JS_TAG_UNINITIALIZED, 1)) return pr_;\n"
           "    }\n",
           hex);
  return b;
}

int fast_any(void) { return g_fast_ok > 0; }
