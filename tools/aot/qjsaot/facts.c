/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * The facts pass and the monomorphic site groups.
 *
 * The speculative body gets its speed from doing every check before any effect.
 * Two kinds of check repeat constantly and can be proved unnecessary:
 *
 *   SHAPE(v, G)   v is an object whose shape is the one group G's sites were
 *                 profiled with, established by an earlier guarded access;
 *   AVAIL(key)=w  the memory location `key` (an argument slot, a local, a
 *                 closure cell, a named field, an element, a global, `this`)
 *                 still holds the SSA value w, so a second read of that
 *                 location is a copy of w: no load, no shape test, no tag
 *                 guard.
 *
 * One forward pass over the blocks in reverse postorder computes them. The
 * in-set of a block is the intersection of its predecessors' out-sets, so a
 * fact survives only if every path in established it; a predecessor the pass has
 * not reached yet (a back edge) counts as the empty set, which is why a loop
 * header starts with nothing (loop-invariant facts are hoisting's job, not this
 * pass's).
 *
 * The result is per instruction: `fwd[insn]` is the value an instruction copies,
 * `access[insn]` says it is a guarded access of a group's shape, `skip[insn]`
 * says that test is already implied by a SHAPE fact, and `miss_exits[insn]` says
 * its miss must leave the speculative body. Nothing is emitted here; the
 * emitter reads these and `--types` prints them.
 *
 * ---- what counts as a safepoint, and why ----
 *
 * A safepoint drops every SHAPE fact and applies the runs_js kills. Applied literally to `runs_js` in the role table that would make a
 * property read kill the load fact it just established, so no field read could
 * ever be forwarded and the point of the pass (one load per location) would be lost.
 * The role table's `runs_js` describes what the *interpreter handler* may do,
 * including its slow path; the pass has to model the code the speculative body
 * will actually contain:
 *
 *   - A load or store of a local, an argument, a closure variable or `this`, a
 *     constant and a move cannot run JS (a TDZ read throws and leaves), so it
 *     is no safepoint.
 *   - A grouped field access (every site in a group) has an inline
 *     fast path that runs no JS: a tag and shape test and a slot load or store.
 *     Its miss is an exit to the generic body, which does not
 *     come back, so on the path that continues no JS ran and the facts hold.
 *     The pass marks it `miss_exits`, and the emitter must emit its miss as an
 *     exit. A miss that continued instead would rejoin with facts a getter or
 *     setter may have broken, and a forwarded value cannot be repaired on the
 *     cold arm alone, because the code after the join is shared.
 *   - Everything else that is not pure -- an ungrouped field access, a global
 *     or element access (whose misses continue), calls, op calls,
 *     fallbacks, allocation -- is a safepoint and kills everything except
 *     `this`, which is fixed for the activation.
 *
 * The `access`/`skip`/`miss_exits` marks are what makes this checkable: the gate
 * counts `skip` and `fwd` (the design's "shape checks skipped" and "loads
 * forwarded"), so a change that turns every access into a safepoint again shows
 * up as zero, not as a silent loss of speed.
 *
 * Everything else is deliberately conservative where the bytecode does not tell
 * us enough: a fact dropped needlessly costs a reload, a fact kept wrongly is a
 * miscompile.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "analysis.h"
#include "emit.h"
#include "quickjs-regvm.h"

/* The profile's site and coverage records, from qjsaot.c. */
int qjs_aot_prof_site(
    const uint8_t dg[16], int site, int *nshapes, uint64_t shapes[4]);
int qjs_aot_prof_site_own(const uint8_t dg[16], int site);
int qjs_aot_prof_site_proto(const uint8_t dg[16], int site);
int qjs_aot_prof_site_proto2(const uint8_t dg[16], int site);
int qjs_aot_prof_unexecuted(const uint8_t dg[16], int pc);
unsigned long long qjs_aot_prof_pc_count(const uint8_t dg[16], int pc);

/* A cell fact's value in the shape-fact list: no receiver value is this large. */
#define E_CELLV(site) (0x40000000 + (site))
/* The shape fact of `this` of the activation. */
#define E_THISV 0x10000000
/* An array fact's value in the same list: `rv` is a dense array whose base pointer and count
   are in locals. Group 0. */
#define E_ARRV(rv) (0x20000000 + (rv))

/* ---- the fact set -------------------------------------------------------- */

/* A set of facts, as two flat lists. Both are tiny (a handful of entries per
   block in real code), so linear search beats a hash table and keeps the
   intersection simple. */
typedef struct FactSet {
  int nshape, capshape;
  int *shape_val; /* the receiver value */
  int *shape_grp; /* the group */
  int navail, capavail;
  EAvail *avail;
} FactSet;

static void fs_clear(FactSet *s) {
  s->nshape = 0;
  s->navail = 0;
}

static void fs_free(FactSet *s) {
  free(s->shape_val);
  free(s->shape_grp);
  free(s->avail);
  memset(s, 0, sizeof(*s));
}

static int fs_has_shape(const FactSet *s, int val, int grp) {
  int i;
  for (i = 0; i < s->nshape; i++) {
    if (s->shape_val[i] == val && s->shape_grp[i] == grp) return 1;
  }
  return 0;
}

static int fs_add_shape(FactSet *s, int val, int grp) {
  if (val < 0 || grp < 0) return 0;
  if (fs_has_shape(s, val, grp)) return 0;
  if (s->nshape >= s->capshape) {
    /* The two arrays share one capacity, so both are grown to the same new one
       before either is written. Growing them one at a time would leave the
       second array null, because the first call raises the capacity the second
       one then tests the length against. */
    int cap = s->capshape > 0 ? s->capshape * 2 : 8;
    int *v = realloc(s->shape_val, sizeof(*v) * (size_t)cap);
    int *g;
    if (v == NULL) return -1;
    s->shape_val = v;
    g = realloc(s->shape_grp, sizeof(*g) * (size_t)cap);
    if (g == NULL) return -1;
    s->shape_grp = g;
    s->capshape = cap;
  }
  s->shape_val[s->nshape] = val;
  s->shape_grp[s->nshape] = grp;
  s->nshape++;
  return 0;
}

static int key_eq(const EKey *a, const EKey *b) {
  return a->kind == b->kind && a->a == b->a && a->b == b->b;
}

static Q2ValueId fs_get_avail(const FactSet *s, const EKey *k) {
  int i;
  for (i = 0; i < s->navail; i++) {
    if (key_eq(&s->avail[i].key, k)) return s->avail[i].val;
  }
  return Q2_NO_VALUE;
}

/* Add or replace. One fact per location: a location holds one value. */
static int fs_set_avail(FactSet *s, const EKey *k, Q2ValueId v) {
  int i;
  if (v == Q2_NO_VALUE) return 0;
  for (i = 0; i < s->navail; i++) {
    if (key_eq(&s->avail[i].key, k)) {
      s->avail[i].val = v;
      return 0;
    }
  }
  if (s->navail >= s->capavail) {
    int cap = s->capavail > 0 ? s->capavail * 2 : 8;
    EAvail *q = realloc(s->avail, sizeof(*q) * (size_t)cap);
    if (q == NULL) return -1;
    s->avail = q;
    s->capavail = cap;
  }
  s->avail[s->navail].key = *k;
  s->avail[s->navail].val = v;
  s->navail++;
  return 0;
}

/* Drop every AVAIL fact the predicate accepts. */
typedef int (*KeyPred)(void *arg, const EKey *k);

static void fs_kill(FactSet *s, KeyPred pred, void *arg) {
  int i, w = 0;
  for (i = 0; i < s->navail; i++) {
    if (pred(arg, &s->avail[i].key)) continue;
    s->avail[w++] = s->avail[i];
  }
  s->navail = w;
}

/* in = a ∩ b. A SHAPE fact survives only if both have it; an AVAIL fact only if
   both hold it with the same value. */
static int fs_intersect(FactSet *dst, const FactSet *a, const FactSet *b) {
  int i;
  fs_clear(dst);
  for (i = 0; i < a->nshape; i++) {
    if (fs_has_shape(b, a->shape_val[i], a->shape_grp[i]) &&
        fs_add_shape(dst, a->shape_val[i], a->shape_grp[i]) < 0)
      return -1;
  }
  for (i = 0; i < a->navail; i++) {
    if (fs_get_avail(b, &a->avail[i].key) == a->avail[i].val &&
        fs_set_avail(dst, &a->avail[i].key, a->avail[i].val) < 0)
      return -1;
  }
  return 0;
}

/* ---- kill predicates ----------------------------------------------------- */

static int kp_arg_slot(void *arg, const EKey *k) {
  return k->kind == E_K_ARG && k->a == *(const int64_t *)arg;
}

static int kp_local_slot(void *arg, const EKey *k) {
  return k->kind == E_K_LOCAL && k->a == *(const int64_t *)arg;
}

static int kp_varref_slot(void *arg, const EKey *k) {
  return k->kind == E_K_VARREF && k->a == *(const int64_t *)arg;
}

static int kp_global_atom(void *arg, const EKey *k) {
  return k->kind == E_K_GLOBAL && k->a == *(const int64_t *)arg;
}

/* Two different atoms never alias, so a store kills only its own atom -- on any
   receiver, because two receivers may be the same object. */
static int kp_field_atom(void *arg, const EKey *k) {
  return k->kind == E_K_FIELD && k->b == *(const int64_t *)arg;
}

static int kp_elem(void *arg, const EKey *k) {
  (void)arg;
  return k->kind == E_K_ELEM;
}

/* A safepoint: everything reachable from the heap may have changed, and a
   refill may have freed a shape and handed its address to another one. `this`
   is fixed for the activation and survives. */
static int kp_safepoint(void *arg, const EKey *k) {
  (void)arg;
  return k->kind != E_K_THIS;
}

/* ---- the operands of a property access ---------------------------------- */

/* A register operand's value, or Q2_NO_VALUE when it is a destination or not a
   register. */
static Q2ValueId regval(const Q2Insn *in, int idx) {
  if (idx < 0 || idx >= in->narg) return Q2_NO_VALUE;
  if (in->arg[idx].kind != 'r') return Q2_NO_VALUE;
  return in->argval[idx];
}

/* The nearest operand of the given kind at or before `idx`, or -1. */
static int arg_nearest(const Q2Insn *in, int idx, char kind) {
  int i;
  if (idx >= in->narg) idx = in->narg - 1;
  for (i = idx; i >= 0; i--) {
    if (in->arg[i].kind == kind) return i;
  }
  return -1;
}

/* One property access: the PIC site it uses and the location it reads or
   writes. */
typedef struct SiteRef {
  int site;     /* the PIC index, or REG_IC_NONE */
  int64_t recv; /* a value id, E_RECV_THIS or E_RECV_LOCAL(slot) */
  int64_t atom;
  int is_store;
} SiteRef;

/* The receiver key of a field access whose site is operand `si`.
   `def_recv` is the value a chain's second link reads from. */
static int64_t site_recv(const Q2Insn *in, int si, Q2ValueId def_recv) {
  int ai, ri;

  /* "rras" and "rasr": the register operand just before the atom. */
  ai = arg_nearest(in, si - 1, 'a');
  if (ai >= 0) {
    ri = arg_nearest(in, ai - 1, 'r');
    if (ri >= 0) {
      Q2ValueId v = regval(in, ri);
      if (v != Q2_NO_VALUE) return (int64_t)v;
      /* The operand is written by this instruction. `get_this_field` writes
         `this` into it, so the receiver is `this`; a chain's second link reads
         its receiver from the first link's result. */
      if (in->rop == ROP_get_this_field || in->rop == ROP_get_this_field2)
        return E_RECV_THIS;
      if (def_recv != Q2_NO_VALUE) return (int64_t)def_recv;
      return -1;
    }
    (void)0;
  }
  /* "ruas": the receiver is a local slot, not a register. */
  if (in->rop == ROP_get_local_field) {
    int u = arg_nearest(in, si - 1, 'u');
    if (u >= 0) return E_RECV_LOCAL(in->arg[u].val);
  }
  if (in->rop >= ROP_get_local_field_0 && in->rop <= ROP_get_local_field_3)
    return E_RECV_LOCAL((int64_t)(in->rop - ROP_get_local_field_0));
  if (in->rop == ROP_get_this_field || in->rop == ROP_get_this_field2)
    return E_RECV_THIS;
  return -1;
}

/* Collect the property accesses of one instruction (a chain has two). Returns
   the count, 0 to 2. */
/* A receiver the access emitters can test: an SSA value, or `this` of a get_this_field. */
static int recv_groupable(int64_t recv) { return recv >= 0 || recv == E_RECV_THIS; }

static int site_refs(const Q2Insn *in, SiteRef out[2]) {
  const ERole *role = e_roles[in->rop];
  int n = 0, i;

  if (role == NULL || role->mem != E_MEM_FIELD) return 0;
  for (i = 0; i < in->narg && n < 2; i++) {
    Q2ValueId def_recv = Q2_NO_VALUE;
    int ai;
    if (in->arg[i].kind != 's') continue;
    out[n].site = (int)in->arg[i].val;
    out[n].atom = 0;
    ai = arg_nearest(in, i - 1, 'a');
    if (ai >= 0) out[n].atom = in->arg[ai].val;
    if (n == 1) {
      /* The value the first link wrote, which is this link's receiver. */
      if (in->ndef >= 1) def_recv = in->def[0];
    }
    out[n].recv = site_recv(in, i, def_recv);
    out[n].is_store =
        in->op == Q2_OP_PUT_FIELD || in->rop == ROP_put_field ||
        in->rop == ROP_put_field_xfer;
    n++;
  }
  return n;
}

/* ---- the safepoint / cold-arm classification ---------------------------- */

/* The kind of an instruction for the pass.
 *
 * E_SK_PURE     no safepoint: it cannot run JS or allocate.
 * E_SK_NOJS     a memory access that cannot run JS: no safepoint.
 * E_SK_FIELD    a field access: no safepoint when every site it uses is
 *                grouped (its miss then exits), a safepoint otherwise.
 * E_SK_SAFE     a safepoint.
 */
#define E_SK_PURE 0
#define E_SK_NOJS 1
#define E_SK_FIELD 2
#define E_SK_SAFE 3

static int insn_kind(const Q2Insn *in) {
  const ERole *role = e_roles[in->rop];
  if (role == NULL) return E_SK_SAFE;
  if (role->pure) return E_SK_PURE;
  switch (in->op) {
    case Q2_OP_GET_FIELD:
    case Q2_OP_PUT_FIELD:
      return E_SK_FIELD;
    case Q2_OP_GET_LOCAL:
    case Q2_OP_PUT_LOCAL:
    case Q2_OP_GET_ARG:
    case Q2_OP_GET_ARG2:
    case Q2_OP_PUT_ARG:
    case Q2_OP_GET_VAR_REF:
    case Q2_OP_LOAD_THIS:
    case Q2_OP_CONST:
    case Q2_OP_MOVE:
      /* Each only reads or writes a frame slot, an argument or a closure
         cell, and cannot run JS. A TDZ read throws and leaves the activation,
         so no fact is needed past it. */
      return E_SK_NOJS;
    default:
      return E_SK_SAFE;
  }
}

/* ---- loads and stores the pass tracks ----------------------------------- */

typedef struct Access {
  int kind;      /* E_K_*, or 0 when the instruction is neither */
  EKey key;     /* the location */
  Q2ValueId val; /* the value read (load) or written (store) */
  int is_store;
  int fwd_ok;    /* the whole instruction is this one location's load */
} Access;

static int insn_load(const Q2Insn *in, Access *a) {
  memset(a, 0, sizeof(*a));
  switch (in->rop) {
    case ROP_get_arg:
      a->kind = E_K_ARG;
      a->key.kind = E_K_ARG;
      a->key.a = in->narg > 1 ? in->arg[1].val : -1;
      break;
    case ROP_get_arg2:
      /* Two slots, two destinations: the pass tracks neither (one fact per
         instruction would have to pick one). */
      return 0;
    case ROP_get_local:
      a->kind = E_K_LOCAL;
      a->key.kind = E_K_LOCAL;
      a->key.a = in->narg > 1 ? in->arg[1].val : -1;
      break;
    case ROP_get_local_s0:
    case ROP_get_local_s1:
    case ROP_get_local_s2:
    case ROP_get_local_s3:
      a->kind = E_K_LOCAL;
      a->key.kind = E_K_LOCAL;
      a->key.a = in->rop - ROP_get_local_s0;
      break;
    case ROP_get_var_ref:
      a->kind = E_K_VARREF;
      a->key.kind = E_K_VARREF;
      a->key.a = in->narg > 1 ? in->arg[1].val : -1;
      break;
    case ROP_get_global: {
      int ai = arg_nearest(in, in->narg - 1, 'a');
      a->kind = E_K_GLOBAL;
      a->key.kind = E_K_GLOBAL;
      a->key.a = ai >= 0 ? in->arg[ai].val : -1;
      break;
    }
    case ROP_load_this:
      a->kind = E_K_THIS;
      a->key.kind = E_K_THIS;
      break;
    case ROP_get_element: {
      Q2ValueId obj = regval(in, 1), idx = regval(in, 2);
      if (obj == Q2_NO_VALUE || idx == Q2_NO_VALUE) return 0;
      a->kind = E_K_ELEM;
      a->key.kind = E_K_ELEM;
      a->key.a = (int64_t)obj;
      a->key.b = (int64_t)idx;
      break;
    }
    case ROP_get_length: {
      Q2ValueId obj = regval(in, 1);
      if (obj == Q2_NO_VALUE) return 0;
      a->kind = E_K_ELEM;
      a->key.kind = E_K_ELEM;
      a->key.a = (int64_t)obj;
      a->key.b = E_ELEM_LEN;
      break;
    }
    default: {
      /* A property read: one location, keyed by (receiver, atom). */
      SiteRef ref[2];
      int nr = site_refs(in, ref);
      if (nr != 1) return 0; /* a chain: two locations, no single move */
      if (ref[0].site < 0 || (unsigned)ref[0].site == REG_IC_NONE) return 0;
      if (ref[0].recv < 0 || ref[0].atom == 0) return 0;
      a->kind = E_K_FIELD;
      a->key.kind = E_K_FIELD;
      a->key.a = ref[0].recv;
      a->key.b = ref[0].atom;
      break;
    }
  }
  if (in->ndef < 1) return 0;
  a->val = in->def[0];
  a->fwd_ok = 1;
  return 1;
}

static int insn_store(const Q2Insn *in, Access *a) {
  memset(a, 0, sizeof(*a));
  a->is_store = 1;
  switch (in->rop) {
    case ROP_put_arg:
    case ROP_put_arg_xfer:
      a->kind = E_K_ARG;
      a->key.kind = E_K_ARG;
      a->key.a = in->narg > 0 ? in->arg[0].val : -1;
      a->val = regval(in, 1);
      break;
    case ROP_put_local:
    case ROP_put_local_xfer:
      a->kind = E_K_LOCAL;
      a->key.kind = E_K_LOCAL;
      a->key.a = in->narg > 0 ? in->arg[0].val : -1;
      a->val = regval(in, 1);
      break;
    case ROP_put_local_s0:
    case ROP_put_local_s1:
    case ROP_put_local_s2:
    case ROP_put_local_s3:
      a->kind = E_K_LOCAL;
      a->key.kind = E_K_LOCAL;
      a->key.a = in->rop - ROP_put_local_s0;
      a->val = regval(in, 0);
      break;
    case ROP_put_var_ref:
      a->kind = E_K_VARREF;
      a->key.kind = E_K_VARREF;
      a->key.a = in->narg > 0 ? in->arg[0].val : -1;
      a->val = regval(in, 1);
      break;
    case ROP_put_global:
      a->kind = E_K_GLOBAL;
      a->key.kind = E_K_GLOBAL;
      a->key.a = in->narg > 0 ? in->arg[0].val : -1;
      a->val = regval(in, 1);
      break;
    case ROP_put_element:
    case ROP_put_element_xfer: {
      Q2ValueId obj = regval(in, 0), idx = regval(in, 1);
      if (obj == Q2_NO_VALUE || idx == Q2_NO_VALUE) return 0;
      a->kind = E_K_ELEM;
      a->key.kind = E_K_ELEM;
      a->key.a = (int64_t)obj;
      a->key.b = (int64_t)idx;
      a->val = regval(in, 2);
      break;
    }
    case ROP_put_field:
    case ROP_put_field_xfer: {
      SiteRef ref[2];
      int nr = site_refs(in, ref);
      int vi = arg_nearest(in, in->narg - 1, 'r');
      if (nr != 1) return 0;
      if (ref[0].recv < 0 || ref[0].atom == 0) return 0;
      a->kind = E_K_FIELD;
      a->key.kind = E_K_FIELD;
      a->key.a = ref[0].recv;
      a->key.b = ref[0].atom;
      a->val = vi >= 0 ? regval(in, vi) : Q2_NO_VALUE;
      break;
    }
    default:
      return 0;
  }
  return 1;
}

/* ---- site groups -------------------------------------------- */

static int build_groups(const Q2Func *fn, EFacts *f) {
  int i, k, n = 0, cap = 0, ng = 0;
  ESiteInfo *sites = NULL;
  EGroup *groups = NULL;
  int64_t *gshape = NULL;

  for (i = 0; i < fn->ninsns; i++) {
    const Q2Insn *in = &fn->insns[i];
    SiteRef ref[2];
    int nr = site_refs(in, ref), r;
    for (r = 0; r < nr; r++) {
      int m, dup = 0;
      if (ref[r].site < 0 || (unsigned)ref[r].site == REG_IC_NONE) continue;
      for (m = 0; m < n; m++) {
        if (sites[m].site == ref[r].site) {
          dup = 1;
          break;
        }
      }
      if (dup) continue; /* one entry per PIC cell */
      if (n == cap) {
        int nc = cap > 0 ? cap * 2 : 16;
        ESiteInfo *q = realloc(sites, sizeof(*q) * (size_t)nc);
        if (q == NULL) goto oom;
        sites = q;
        cap = nc;
      }
      sites[n].site = ref[r].site;
      sites[n].insn = i;
      sites[n].group = E_GROUP_NONE;
      sites[n].recv = ref[r].recv < 0 ? Q2_NO_VALUE : (Q2ValueId)ref[r].recv;
      sites[n].poly = 0;
      sites[n].proto = 0;
      {
        Access stw;
        sites[n].write = insn_store(in, &stw) && stw.kind == E_K_FIELD;
        sites[n].hot = qjs_aot_prof_pc_count(fn->g->digest, in->ppc) > 0;
      }
      n++;
    }
  }
  for (k = 0; k < n; k++) {
    int ns = 0, g = E_GROUP_NONE, j;
    uint64_t shapes[4];
    const Q2Insn *in = &fn->insns[sites[k].insn];
    if (!qjs_aot_prof_site(fn->g->digest, sites[k].site, &ns, shapes)) continue;
    if (ns >= 2 && ns <= 4 && qjs_aot_prof_site_own(fn->g->digest, sites[k].site) &&
        !qjs_aot_prof_unexecuted(fn->g->digest, in->ppc)) {
      sites[k].poly = 1; /* served by the polymorphic cache, not grouped */
      continue;
    }
    if (ns != 1) continue; /* never filled, or too many shapes: no group */
    /* Only an own readable or writable slot has a fixed offset to group on. */
    if (qjs_aot_prof_site_proto(fn->g->digest, sites[k].site) && !sites[k].write &&
        getenv("QJS_AOT_NOPROTOGROUP") == NULL) {
      sites[k].proto = 1;
    } else if (qjs_aot_prof_site_proto2(fn->g->digest, sites[k].site) && !sites[k].write &&
               getenv("QJS_AOT_NOPROTOGROUP") == NULL) {
      sites[k].proto = 2;
    } else if (!qjs_aot_prof_site_own(fn->g->digest, sites[k].site)) {
      continue;
    }
    if (qjs_aot_prof_unexecuted(fn->g->digest, in->ppc)) continue;
    for (j = 0; j < ng; j++) {
      if (gshape[j] == (int64_t)shapes[0]) {
        g = j;
        break;
      }
    }
    if (g == E_GROUP_NONE) {
      EGroup *q = realloc(groups, sizeof(*q) * (size_t)(ng + 1));
      int64_t *s2;
      if (q == NULL) goto oom;
      groups = q;
      s2 = realloc(gshape, sizeof(*s2) * (size_t)(ng + 1));
      if (s2 == NULL) goto oom;
      gshape = s2;
      gshape[ng] = (int64_t)shapes[0];
      groups[ng].shape = (int64_t)shapes[0];
      groups[ng].first_site = sites[k].site;
      groups[ng].nsites = 0;
      groups[ng].sites = NULL;
      g = ng++;
    }
    {
      EGroup *gr = &groups[g];
      int *q = realloc(gr->sites, sizeof(*q) * (size_t)(gr->nsites + 1));
      if (q == NULL) goto oom;
      gr->sites = q;
      gr->sites[gr->nsites++] = sites[k].site;
      /* The entry check reads the lowest site's cell, so the order the
         instructions happen to be in cannot change what is emitted. */
      if (sites[k].site < gr->first_site) gr->first_site = sites[k].site;
    }
    sites[k].group = g;
  }
  f->sites = sites;
  f->nsites = n;
  f->groups = groups;
  f->ngroups = ng;
  free(gshape);
  return 0;
oom:
  free(sites);
  if (groups != NULL) {
    for (k = 0; k < ng; k++) free(groups[k].sites);
    free(groups);
  }
  free(gshape);
  return -1;
}

/* ---- the transfer ------------------------------------------------------- */

/* Apply one instruction's transfer to `s`. */
/* What a store may have changed: the facts it kills (not the location it defines). */
static void apply_store_kills(FactSet *s, const Access *stp) {
  const Access st = *stp;
  switch (st.kind) {
    case E_K_ARG: {
      int64_t slot = st.key.a;
      fs_kill(s, kp_arg_slot, &slot);
      break;
    }
    case E_K_LOCAL: {
      int64_t slot = st.key.a;
      fs_kill(s, kp_local_slot, &slot);
      break;
    }
    case E_K_VARREF: {
      int64_t slot = st.key.a;
      fs_kill(s, kp_varref_slot, &slot);
      break;
    }
    case E_K_GLOBAL: {
      int64_t atom = st.key.a;
      fs_kill(s, kp_global_atom, &atom);
      break;
    }
    case E_K_FIELD: {
      /* Different atoms never alias; the same atom on another receiver may be
         the same object, so it kills them all. */
      int64_t atom = st.key.b;
      fs_kill(s, kp_field_atom, &atom);
      break;
    }
    case E_K_ELEM: {
      /* Any element store may move, extend or reshape the store. */
      fs_kill(s, kp_elem, NULL);
      break;
    }
    default:
      break;
  }
}

/* A numeric element access that runs on a checked dense-array region: the profile saw only
   numbers at the pc (no hole, no out-of-range read, no growing store, no typed array), so its
   miss is an exit and it is no safepoint. */
int e_facts_pure_ok = 0;
int (*e_facts_pure_call)(const Q2Func *fn, const Q2Insn *in) = NULL;

static int pure_call_ok(const Q2Func *fn, const Q2Insn *in) {
  return e_facts_pure_ok && e_facts_pure_call != NULL &&
         (in->rop == ROP_call || in->rop == ROP_call_method) && e_facts_pure_call(fn, in);
}

static int elem_region_ok(const Q2Func *fn, const Q2Insn *in) {
  int i;
  if (in->rop != ROP_get_element && in->rop != ROP_put_element &&
      in->rop != ROP_put_element_xfer)
    return 0;
  if (in->nuse < (in->rop == ROP_get_element ? 2 : 3)) return 0;
  if (fn->prof == NULL) return 0;
  for (i = 0; i < fn->nprof; i++) {
    if (fn->prof[i].pc != in->pc) continue;
    /* No miss and no typed array was seen. A read may yield any element (a number or an
       object); a store is a region access only for numbers, since the inline store replaces
       an element without releasing a reference. */
    if (fn->prof[i].elem == 0 ||
        (fn->prof[i].elem & (Q2_ELEM_HOLEY | Q2_ELEM_TA)) != 0)
      return 0;
    if (in->rop == ROP_get_element) return 1;
    return (fn->prof[i].elem & (Q2_ELEM_INT | Q2_ELEM_FLOAT | Q2_ELEM_BOOL)) != 0 &&
           (fn->prof[i].elem & (Q2_ELEM_OBJECT | Q2_ELEM_MIXED)) == 0;
  }
  return 0;
}

/* A global read the profile executed: the cache cell either hits or the body leaves at the
   read (the generic body does the lookup, which can throw or run a getter), so it is no
   safepoint. */
static int global_exit_ok(const Q2Func *fn, const Q2Insn *in) {
  int ns = 0;
  uint64_t shapes[4];
  if (in->rop != ROP_get_global || in->narg < 4 || in->arg[3].kind != 's' ||
      (unsigned)in->arg[3].val == REG_IC_NONE ||
      qjs_aot_prof_pc_count(fn->g->digest, in->ppc) == 0 || getenv("QJS_AOT_NOGLOBALEXIT") != NULL)
    return 0;
  /* Filled at exactly one generation of the global object in the profiled run: the cell holds
     unless the global object changes, which that run did not see. */
  if (!qjs_aot_prof_site(fn->g->digest, (int)in->arg[3].val, &ns, shapes)) return 0;
  /* A site that ran many times and was filled only a couple of times saw the global object
     change at startup, not in its steady state. */
  return ns == 1 || (ns <= 3 && qjs_aot_prof_pc_count(fn->g->digest, in->ppc) >= 16384);
}

/* The receiver of a region access, through the forwarded copies recorded so far. */
static int region_recv(const Q2Func *fn, const EFacts *f, const Q2Insn *in) {
  int rv = (int)in->use[0];
  while (rv >= 0 && rv < fn->nvalues && fn->values[rv].insn >= 0 &&
         f->fwd[fn->values[rv].insn] != Q2_NO_VALUE)
    rv = (int)f->fwd[fn->values[rv].insn];
  return rv;
}

/* Whether the instruction is a safepoint, ignoring forwarding (a forwarded load is
   never one): the classification `transfer` applies, for the loop eligibility test. */
static int is_safepoint(const Q2Func *fn, EFacts *f, const Q2Insn *in, int grouped_exit) {
  int kind = insn_kind(in);
  if (grouped_exit && elem_region_ok(fn, in)) return 0;
  if (grouped_exit && global_exit_ok(fn, in)) return 0;
  if (grouped_exit && pure_call_ok(fn, in)) return 0;
  if (kind == E_SK_FIELD) {
    SiteRef ref[2];
    int nr = site_refs(in, ref), r, grouped = nr > 0;
    if (!grouped_exit) grouped = 0;
    for (r = 0; r < nr; r++)
      if (ref[r].site < 0 || !recv_groupable(ref[r].recv) ||
          (e_facts_site_group(f, ref[r].site) < 0 &&
           !(e_facts_poly_on && nr == 1 && ref[r].recv >= 0 && e_facts_poly_op(in) &&
             e_facts_site_poly(f, ref[r].site))))
        grouped = 0;
    kind = grouped ? E_SK_NOJS : E_SK_SAFE;
  }
  if (kind == E_SK_SAFE && grouped_exit && in->op == Q2_OP_CALL &&
      e_intrinsic_lowerable(fn, in))
    kind = E_SK_NOJS;
  return kind == E_SK_SAFE;
}

static int transfer(const Q2Func *fn, EFacts *f, int insn, FactSet *s,
                    int grouped_exit) {
  const Q2Insn *in = &fn->insns[insn];
  /* The fused local read-modify-write ops update a local slot in memory. */
  if ((in->rop == ROP_local_rmw_branch || in->rop == ROP_local_rmw_imm) && in->narg > 1) {
    Access lk;
    memset(&lk, 0, sizeof(lk));
    lk.kind = E_K_LOCAL;
    lk.key.kind = E_K_LOCAL;
    lk.key.a = (int64_t)in->arg[1].val;
    apply_store_kills(s, &lk);
  }
  Access ld, st;
  int kind = insn_kind(in);
  int have_ld = insn_load(in, &ld);
  int forwarded = 0;

  /* 1. A load of a location that still holds a value is a copy of it: no
     access, no shape check, no tag guard. */
  if (have_ld) {
    Q2ValueId w = fs_get_avail(s, &ld.key);
    if (w != Q2_NO_VALUE && w != ld.val) {
      f->fwd[insn] = w;
      f->nfwd++;
      forwarded = 1;
    }
  }
  if (forwarded) {
    /* The copy still has to be current in its register: the emitter stores a
       tagged value there. Nothing ran, so the instruction is no safepoint
       and no fact changes. */
    return 0;
  }

  /* A numeric element access on a dense array: its miss exits, so it is no safepoint; the
     receiver is a checked array afterwards. */
  if (grouped_exit && elem_region_ok(fn, in)) {
    int rv = region_recv(fn, f, in);
    kind = E_SK_NOJS;
    f->elemreg[insn] = 1;
    f->miss_exits[insn] = 1;
    if (rv >= 0 && rv < fn->nvalues) {
      if (fs_has_shape(s, E_ARRV(rv), 0)) f->arrskip[insn] = 1;
      if (fs_add_shape(s, E_ARRV(rv), 0) < 0) return -1;
      f->arr_used[rv] = 1;
    }
  }

  if (grouped_exit && global_exit_ok(fn, in)) {
    kind = E_SK_NOJS;
    f->miss_exits[insn] = 1;
  }

  /* A call whose callees all have safepoint-free inline bodies: the body runs in place or
     the call exits, so no JS ran. What the callee may have stored dies with the loaded
     values; the shape, cache-cell and array facts stay. */
  if (grouped_exit && pure_call_ok(fn, in)) {
    kind = E_SK_NOJS;
    f->purecall[insn] = 1;
    f->miss_exits[insn] = 1;
    fs_kill(s, kp_safepoint, NULL);
  }

  /* A field access is no safepoint only when every site it uses is grouped:
     only then is its miss an exit. */
  if (kind == E_SK_FIELD) {
    SiteRef ref[2];
    int nr = site_refs(in, ref), r, grouped = nr > 0;
    /* Without `grouped_exit` a grouped access is a safepoint: its miss
       continues into the generic lowering instead of exiting, so whatever it
       ran may have broken every fact. The continuing-miss mode of the speculative body is this one. */
    if (!grouped_exit) grouped = 0;
    for (r = 0; r < nr; r++)
      if (ref[r].site < 0 || !recv_groupable(ref[r].recv) ||
          (e_facts_site_group(f, ref[r].site) < 0 &&
           !(e_facts_poly_on && nr == 1 && ref[r].recv >= 0 && e_facts_poly_op(in) &&
             e_facts_site_poly(f, ref[r].site))))
        grouped = 0;
    kind = grouped ? E_SK_NOJS : E_SK_SAFE;
    f->miss_exits[insn] = (uint8_t)grouped;
  }

  /* An intrinsic call (a builtin on numbers) cannot run JS: with the exit
     fallback it is no safepoint, so the facts survive it. Without
     `grouped_exit` its miss would continue into the real call. */
  if (kind == E_SK_SAFE && grouped_exit && in->op == Q2_OP_CALL &&
      e_intrinsic_lowerable(fn, in)) {
    kind = E_SK_NOJS;
    f->miss_exits[insn] = 1;
  }

  /* 2. A guarded access of a receiver whose shape is known skips its tag and
     shape test; either way the shape is known afterwards. */
  if (kind != E_SK_SAFE) {
    SiteRef ref[2];
    int nr = site_refs(in, ref), r;
    for (r = 0; r < nr; r++) {
      int g = ref[r].site >= 0 ? e_facts_site_group(f, ref[r].site) : -1;
      int rv;
      if (g < 0 || !recv_groupable(ref[r].recv)) continue;
      f->access[insn] = 1;
      /* A receiver that is a forwarded copy of another value is that value. */
      rv = ref[r].recv == E_RECV_THIS ? E_THISV : (int)ref[r].recv;
      while (rv >= 0 && rv < E_THISV && rv < fn->nvalues && fn->values[rv].insn >= 0 &&
             f->fwd[fn->values[rv].insn] != Q2_NO_VALUE)
        rv = (int)f->fwd[fn->values[rv].insn];
      if (fs_has_shape(s, rv, g)) {
        f->skip[insn] = 1;
        f->nskip++;
      }
      if (fs_add_shape(s, rv, g) < 0) return -1;
      /* The site's own cell: a cell fact is held with the shape facts under a value no
         receiver has, and dies with them at a safepoint. */
      if (fs_has_shape(s, E_CELLV(ref[r].site), g)) f->cellskip[insn] |= (uint8_t)(1u << r);
      if (fs_add_shape(s, E_CELLV(ref[r].site), g) < 0) return -1;
    }
  }

  /* 3. A load makes its result current for the location. */
  if (have_ld) {
    if (fs_set_avail(s, &ld.key, ld.val) < 0) return -1;
  }

  /* 4. A store kills what it may have changed, then defines its own
     location. */
  if (insn_store(in, &st)) {
    /* The load and store helpers are disjoint by opcode, so a store is never
       also the load handled above. */
    apply_store_kills(s, &st);
    if (fs_set_avail(s, &st.key, st.val) < 0) return -1;
  }

  /* 5. A safepoint loses every shape and every location, except
     `this`. It applies after the instruction's own load or store, so an
     ungrouped read does not leave a fact behind either: its miss may have run
     a getter. */
  if (kind == E_SK_SAFE) {
    if (getenv("QJS_AOT_DEBUG_SAFEPOINTS")) {
      char nm[64];
      e_rop_name(in->rop, nm, sizeof(nm));
      fprintf(stderr, "safepoint %s %s@%d\n", fn->g->name, nm, in->pc);
    }
    fs_kill(s, kp_safepoint, NULL);
    s->nshape = 0;
  }
  (void)fn;
  return 0;
}

/* ---- loop hoisting of shape checks ------------------------------
   A natural loop with one back edge target (its header), a unique outside
   predecessor P that has no other successor, and no safepoint in its body can have
   the shape checks of its grouped accesses done once, at the end of P: nothing in
   the body can change a shape, so the fact holds throughout. A hoisted check that
   fails exits at the header's pc with the header's live-in set (the loop has not
   run, so nothing needs undoing). The accesses in the loop then skip their receiver
   test. Only receivers defined outside the loop qualify. Loads are not
   hoisted: a hoisted load needs a new value in the IR (deferred). */
static int in_loop(const uint8_t *loop, int b) { return b >= 0 && loop[b]; }

static int plan_hoists(const Q2Func *fn, EFacts *out, int grouped_exit) {
  int b, bi, k, q, p;
  uint8_t *loop = calloc((size_t)(fn->nblocks > 0 ? fn->nblocks : 1), 1);
  int *stack = malloc(sizeof(int) * (size_t)(fn->nblocks > 0 ? fn->nblocks : 1));
  if (loop == NULL || stack == NULL) {
    free(loop);
    free(stack);
    return -1;
  }
  if (getenv("QJS_AOT_NOHOIST") != NULL) goto done;
  for (b = 0; b < fn->nblocks; b++) {
    const Q2Block *lt = &fn->blocks[b];
    int si;
    for (si = 0; si < lt->nsucc && si < 2; si++) {
      int h = lt->succ_block[si], sp = 0, pre = -1, npre = 0, safe = 1;
      if (h < 0 || h >= fn->nblocks || !q2_is_back_edge(fn, b, h)) continue;
      /* a loop whose back edge starts in a resumed part has no preheader on the path that enters it */
      if (lt->resume) continue;
      /* the natural loop of the back edge b -> h */
      memset(loop, 0, (size_t)fn->nblocks);
      loop[h] = 1;
      if (!loop[b]) {
        loop[b] = 1;
        stack[sp++] = b;
      }
      while (sp > 0) {
        int x = stack[--sp];
        for (p = 0; p < fn->blocks[x].npreds; p++) {
          int pb = fn->blocks[x].preds[p];
          if (pb < 0 || loop[pb] || fn->rpo_pos[pb] < 0) continue;
          loop[pb] = 1;
          stack[sp++] = pb;
        }
      }
      /* the unique outside predecessor */
      for (p = 0; p < fn->blocks[h].npreds; p++) {
        int pb = fn->blocks[h].preds[p];
        if (pb < 0 || fn->rpo_pos[pb] < 0 || loop[pb]) continue;
        pre = pb;
        npre++;
      }
      /* The check is emitted on the edge into the header, so the preheader may
         have another successor (a rotated loop is entered by a branch). */
      if (npre != 1) continue;
      /* no safepoint in the body */
      for (bi = 0; bi < fn->nblocks && safe; bi++) {
        if (!loop[bi]) continue;
        for (k = 0; k < fn->blocks[bi].ninsns; k++)
          /* Negative control: "unsafe" hoists past a safepoint. */
          if (is_safepoint(fn, out, &fn->insns[fn->blocks[bi].first_insn + k], grouped_exit) &&
              getenv("QJS_AOT_HOIST_CONTROL_SAFE") == NULL) {
            safe = 0;
            break;
          }
      }
      if (!safe) continue;
      /* record the loop for the facts that survive it */
      if (out->nloops == out->loop_cap) {
        int nc = out->loop_cap ? out->loop_cap * 2 : 8;
        int *a1 = realloc(out->loop_hdr, sizeof(int) * (size_t)nc);
        int *a2 = realloc(out->loop_pre, sizeof(int) * (size_t)nc);
        uint8_t **a3 = realloc(out->loop_mem, sizeof(*a3) * (size_t)nc);
        if (!a1 || !a2 || !a3) {
          free(loop);
          free(stack);
          return -1;
        }
        out->loop_hdr = a1;
        out->loop_pre = a2;
        out->loop_mem = a3;
        out->loop_cap = nc;
      }
      out->loop_hdr[out->nloops] = h;
      out->loop_pre[out->nloops] = pre;
      out->loop_mem[out->nloops] = malloc((size_t)fn->nblocks);
      if (out->loop_mem[out->nloops] == NULL) {
        free(loop);
        free(stack);
        return -1;
      }
      memcpy(out->loop_mem[out->nloops], loop, (size_t)fn->nblocks);
      out->nloops++;
      /* the cache cells of the grouped sites in the loop: checked once on the edge into the
         loop (and at an OSR entry), so no access in it tests its cell (group code
         -1000 - group, value = site) */
      for (bi = 0; bi < fn->nblocks; bi++) {
        if (!loop[bi]) continue;
        for (k = 0; k < fn->blocks[bi].ninsns; k++) {
          const Q2Insn *in = &fn->insns[fn->blocks[bi].first_insn + k];
          SiteRef ref[2];
          int nr = site_refs(in, ref), r;
          for (r = 0; r < nr; r++) {
            int g, dup = 0, z, hot = 0;
            if (ref[r].site < 0 || ref[r].recv < 0) continue;
            g = e_facts_site_group(out, ref[r].site);
            if (g < 0 || e_facts_site_proto(out, ref[r].site)) continue;
            for (z = 0; z < out->nsites; z++)
              if (out->sites[z].site == ref[r].site) hot = out->sites[z].hot;
            if (!hot) continue;
            for (q = 0; q < out->nhoist; q++)
              if (out->hoist_hdr[q] == h && out->hoist_val[q] == ref[r].site &&
                  out->hoist_grp[q] == -1000 - g)
                dup = 1;
            if (dup) continue;
            if (out->nhoist == out->hoist_cap) {
              int nc = out->hoist_cap ? out->hoist_cap * 2 : 8;
              int *a1 = realloc(out->hoist_hdr, sizeof(int) * (size_t)nc);
              int *a2 = realloc(out->hoist_pre, sizeof(int) * (size_t)nc);
              int *a3 = realloc(out->hoist_val, sizeof(int) * (size_t)nc);
              int *a4 = realloc(out->hoist_grp, sizeof(int) * (size_t)nc);
              if (!a1 || !a2 || !a3 || !a4) {
                free(loop);
                free(stack);
                return -1;
              }
              out->hoist_hdr = a1;
              out->hoist_pre = a2;
              out->hoist_val = a3;
              out->hoist_grp = a4;
              out->hoist_cap = nc;
            }
            out->hoist_hdr[out->nhoist] = h;
            out->hoist_pre[out->nhoist] = pre;
            out->hoist_val[out->nhoist] = ref[r].site;
            out->hoist_grp[out->nhoist] = -1000 - g;
            out->nhoist++;
          }
        }
      }
      /* the receivers of numeric element accesses that are defined outside the loop: the array
         check and the loads of its base pointer and count are hoisted (group -2) */
      for (bi = 0; bi < fn->nblocks; bi++) {
        if (!loop[bi]) continue;
        for (k = 0; k < fn->blocks[bi].ninsns; k++) {
          const Q2Insn *in = &fn->insns[fn->blocks[bi].first_insn + k];
          Q2ValueId v;
          int db, dup = 0;
          if (!grouped_exit || !elem_region_ok(fn, in)) continue;
          v = in->use[0];
          if (v >= (Q2ValueId)fn->nvalues) continue;
          db = fn->values[v].block;
          if (db < 0 || in_loop(loop, db)) continue;
          for (q = 0; q < out->nhoist; q++)
            if (out->hoist_hdr[q] == h && out->hoist_val[q] == (int)v && out->hoist_grp[q] == -2) dup = 1;
          if (dup) continue;
          if (out->nhoist == out->hoist_cap) {
            int nc = out->hoist_cap ? out->hoist_cap * 2 : 8;
            int *a1 = realloc(out->hoist_hdr, sizeof(int) * (size_t)nc);
            int *a2 = realloc(out->hoist_pre, sizeof(int) * (size_t)nc);
            int *a3 = realloc(out->hoist_val, sizeof(int) * (size_t)nc);
            int *a4 = realloc(out->hoist_grp, sizeof(int) * (size_t)nc);
            if (!a1 || !a2 || !a3 || !a4) {
              free(loop);
              free(stack);
              return -1;
            }
            out->hoist_hdr = a1;
            out->hoist_pre = a2;
            out->hoist_val = a3;
            out->hoist_grp = a4;
            out->hoist_cap = nc;
          }
          out->hoist_hdr[out->nhoist] = h;
          out->hoist_pre[out->nhoist] = pre;
          out->hoist_val[out->nhoist] = (int)v;
          out->hoist_grp[out->nhoist] = -2;
          out->nhoist++;
          out->arr_used[v] = 1;
        }
      }
      /* the receivers of grouped accesses that are defined outside the loop */
      for (bi = 0; bi < fn->nblocks; bi++) {
        if (!loop[bi]) continue;
        for (k = 0; k < fn->blocks[bi].ninsns; k++) {
          const Q2Insn *in = &fn->insns[fn->blocks[bi].first_insn + k];
          SiteRef ref[2];
          int nr = site_refs(in, ref), r;
          if (insn_kind(in) != E_SK_FIELD) continue;
          for (r = 0; r < nr; r++) {
            int g, dup = 0, db;
            Q2ValueId v;
            if (ref[r].site < 0 || !recv_groupable(ref[r].recv)) continue;
            g = e_facts_site_group(out, ref[r].site);
            if (g < 0) continue;
            if (ref[r].recv == E_RECV_THIS) {
              v = E_THISV; /* `this` is fixed for the activation, so always outside the loop */
            } else {
              v = (Q2ValueId)ref[r].recv;
              if (v >= (Q2ValueId)fn->nvalues) continue;
              db = fn->values[v].block;
              if (db < 0 || in_loop(loop, db)) continue;
            }
            for (q = 0; q < out->nhoist; q++)
              if (out->hoist_hdr[q] == h && out->hoist_val[q] == v && out->hoist_grp[q] == g) dup = 1;
            if (dup) continue;
            if (out->nhoist == out->hoist_cap) {
              int nc = out->hoist_cap ? out->hoist_cap * 2 : 8;
              int *a1 = realloc(out->hoist_hdr, sizeof(int) * (size_t)nc);
              int *a2 = realloc(out->hoist_pre, sizeof(int) * (size_t)nc);
              int *a3 = realloc(out->hoist_val, sizeof(int) * (size_t)nc);
              int *a4 = realloc(out->hoist_grp, sizeof(int) * (size_t)nc);
              if (!a1 || !a2 || !a3 || !a4) {
                free(loop);
                free(stack);
                return -1;
              }
              out->hoist_hdr = a1;
              out->hoist_pre = a2;
              out->hoist_val = a3;
              out->hoist_grp = a4;
              out->hoist_cap = nc;
            }
            out->hoist_hdr[out->nhoist] = h;
            out->hoist_pre[out->nhoist] = pre;
            out->hoist_val[out->nhoist] = (int)v;
            out->hoist_grp[out->nhoist] = g;
            out->nhoist++;
          }
        }
      }
    }
  }
done:
  free(loop);
  free(stack);
  return 0;
}

int e_facts_run(const Q2Func *fn, EFacts *out, int grouped_exit) {
  FactSet cur;
  FactSet *outs = NULL;
  int b, bi, i, k, rc = -1;

  memset(out, 0, sizeof(*out));
  memset(&cur, 0, sizeof(cur));
  out->ninsns = fn->ninsns;
  out->nblocks = fn->nblocks;
  out->fwd =
      malloc(sizeof(*out->fwd) * (size_t)(fn->ninsns > 0 ? fn->ninsns : 1));
  out->access = calloc((size_t)(fn->ninsns > 0 ? fn->ninsns : 1), 1);
  out->skip = calloc((size_t)(fn->ninsns > 0 ? fn->ninsns : 1), 1);
  out->cellskip = calloc((size_t)(fn->ninsns > 0 ? fn->ninsns : 1), 1);
  out->elemreg = calloc((size_t)(fn->ninsns > 0 ? fn->ninsns : 1), 1);
  out->arrskip = calloc((size_t)(fn->ninsns > 0 ? fn->ninsns : 1), 1);
  out->purecall = calloc((size_t)(fn->ninsns > 0 ? fn->ninsns : 1), 1);
  out->narr_vals = fn->nvalues;
  out->arr_used = calloc((size_t)(fn->nvalues > 0 ? fn->nvalues : 1), 1);
  out->miss_exits = calloc((size_t)(fn->ninsns > 0 ? fn->ninsns : 1), 1);
  out->blocks = calloc((size_t)(fn->nblocks > 0 ? fn->nblocks : 1),
                       sizeof(*out->blocks));
  outs = calloc((size_t)(fn->nblocks > 0 ? fn->nblocks : 1), sizeof(*outs));
  if (out->fwd == NULL || out->access == NULL || out->skip == NULL || out->cellskip == NULL || out->elemreg == NULL ||
      out->arrskip == NULL || out->arr_used == NULL || out->purecall == NULL ||
      out->miss_exits == NULL || out->blocks == NULL || outs == NULL)
    goto done;
  for (i = 0; i < fn->ninsns; i++) out->fwd[i] = Q2_NO_VALUE;
  if (build_groups(fn, out) < 0) goto done;
  if (plan_hoists(fn, out, grouped_exit) < 0) goto done;

  /* The blocks are visited in DFS reverse postorder, so every forward
     predecessor has been visited first and only a back edge can reach an
     unvisited block. The pass is a SINGLE SWEEP, not a fixpoint: a block with a
     back-edge predecessor starts from "nothing known" rather than waiting for
     the back edge's out-set, which is exactly the in-set the emitter can
     establish on entry (the back edge carries nothing the first time round).
     Loop-invariant facts are hoisting's job, not this pass's. */
  for (bi = 0; bi < fn->nrpo; bi++) {
    int b = fn->rpo[bi];
    const Q2Block *blk = &fn->blocks[b];
    int p, first = 1, back_edge = 0;

    if (b == 0 || blk->npreds <= 0) {
      fs_clear(&cur);
      /* The prelude validated every group site's cell (strictly) before the first
         instruction, and before every resume entry. */
      for (k = 0; k < out->nsites; k++)
        if (out->sites[k].group >= 0 && out->sites[k].hot &&
            fs_add_shape(&cur, E_CELLV(out->sites[k].site), out->sites[k].group) < 0)
          goto done;
    } else {
      for (p = 0; p < blk->npreds; p++) {
        if (blk->preds[p] < 0) continue;
        if (fn->rpo_pos[blk->preds[p]] < 0) continue; /* dead: not an edge */
        if (q2_is_back_edge(fn, blk->preds[p], b)) {
          back_edge = 1;
          break;
        }
      }
      if (back_edge) {
        /* A loop header starts empty: the back edge carries no fact the first
           time round, so none may be assumed -- except the shape facts of a loop
           that has hoisted checks: its body has no safepoint, so what held at the
           end of the preheader (and the hoisted checks) holds throughout. */
        fs_clear(&cur);
        /* An eligible loop (no safepoint) keeps what held at the end of its
           preheader, except what a store in the loop may kill. */
        for (k = 0; k < out->nloops; k++) {
          int pre = out->loop_pre[k], j, bb, ii;
          if (out->loop_hdr[k] != b) continue;
          for (j = 0; j < outs[pre].navail; j++)
            if (fs_set_avail(&cur, &outs[pre].avail[j].key, outs[pre].avail[j].val) < 0)
              goto done;
          for (bb = 0; bb < fn->nblocks; bb++) {
            if (!out->loop_mem[k][bb]) continue;
            for (ii = 0; ii < fn->blocks[bb].ninsns; ii++) {
              Access st2;
              /* Negative control: "nokill" carries facts past a store that kills them. */
              if (insn_store(&fn->insns[fn->blocks[bb].first_insn + ii], &st2) &&
                  getenv("QJS_AOT_HOIST_CONTROL") == NULL)
                apply_store_kills(&cur, &st2);
            }
          }
          break;
        }
        for (k = 0; k < out->nhoist; k++) {
          int pre = out->hoist_pre[k], j;
          if (out->hoist_hdr[k] != b) continue;
          for (j = 0; j < outs[pre].nshape; j++)
            if (fs_add_shape(&cur, outs[pre].shape_val[j], outs[pre].shape_grp[j]) < 0)
              goto done;
          if (out->hoist_grp[k] <= -1000) {
            if (fs_add_shape(&cur, E_CELLV(out->hoist_val[k]), -1000 - out->hoist_grp[k]) < 0)
              goto done;
          } else if (out->hoist_grp[k] == -2) {
            if (fs_add_shape(&cur, E_ARRV(out->hoist_val[k]), 0) < 0) goto done;
          } else if (fs_add_shape(&cur, out->hoist_val[k], out->hoist_grp[k]) < 0) {
            goto done;
          }
        }
      } else {
        /* `cur` still holds the previous block's out-set; the merge below
           starts from the first predecessor's, not from that. Without this a
           block inherited the facts of its neighbour in reverse postorder --
           invisible while only the types read the facts, a wrong forwarded load
           once the emitter did. */
        fs_clear(&cur);
        for (p = 0; p < blk->npreds; p++) {
          const FactSet *o;
          /* A dead predecessor never executes, so it must not constrain the
             merge: its out-set was never computed and intersecting an empty set
             would wipe every fact in this live block. A negative pred is not
             produced today; skipped for the same reason as in the back-edge
             scan above, so neither loop can index outs[-1]. */
          if (blk->preds[p] < 0) continue;
          if (fn->rpo_pos[blk->preds[p]] < 0) continue;
          o = &outs[blk->preds[p]];
          if (first) {
            for (k = 0; k < o->nshape; k++)
              if (fs_add_shape(&cur, o->shape_val[k], o->shape_grp[k]) < 0)
                goto done;
            for (k = 0; k < o->navail; k++)
              if (fs_set_avail(&cur, &o->avail[k].key, o->avail[k].val) < 0)
                goto done;
            first = 0;
          } else {
            FactSet tmp;
            memset(&tmp, 0, sizeof(tmp));
            if (fs_intersect(&tmp, &cur, o) < 0) {
              fs_free(&tmp);
              goto done;
            }
            fs_free(&cur);
            cur = tmp;
          }
        }
      }
    }

    /* Keep the in-set: `--types` prints it per block. */
    if (cur.nshape > 0) {
      out->blocks[b].nshape = cur.nshape;
      out->blocks[b].shape_val = malloc(sizeof(int) * (size_t)cur.nshape);
      out->blocks[b].shape_grp = malloc(sizeof(int) * (size_t)cur.nshape);
      if (out->blocks[b].shape_val == NULL ||
          out->blocks[b].shape_grp == NULL)
        goto done;
      memcpy(
          out->blocks[b].shape_val, cur.shape_val,
          sizeof(int) * (size_t)cur.nshape);
      memcpy(
          out->blocks[b].shape_grp, cur.shape_grp,
          sizeof(int) * (size_t)cur.nshape);
    }
    if (cur.navail > 0) {
      out->blocks[b].navail = cur.navail;
      out->blocks[b].avail = malloc(sizeof(EAvail) * (size_t)cur.navail);
      if (out->blocks[b].avail == NULL) goto done;
      memcpy(
          out->blocks[b].avail, cur.avail,
          sizeof(EAvail) * (size_t)cur.navail);
    }

    for (k = 0; k < blk->ninsns; k++) {
      if (transfer(fn, out, blk->first_insn + k, &cur, grouped_exit) < 0)
        goto done;
    }

    for (k = 0; k < cur.nshape; k++)
      if (fs_add_shape(&outs[b], cur.shape_val[k], cur.shape_grp[k]) < 0)
        goto done;
    for (k = 0; k < cur.navail; k++)
      if (fs_set_avail(&outs[b], &cur.avail[k].key, cur.avail[k].val) < 0)
        goto done;
  }
  rc = 0;
done:
  if (outs != NULL) {
    for (b = 0; b < fn->nblocks; b++) fs_free(&outs[b]);
    free(outs);
  }
  fs_free(&cur);
  if (rc < 0) e_facts_free(out);
  return rc;
}

void e_facts_free(EFacts *f) {
  int i;
  if (f == NULL) return;
  for (i = 0; i < f->nloops; i++) free(f->loop_mem[i]);
  free(f->loop_mem);
  free(f->loop_hdr);
  free(f->loop_pre);
  free(f->hoist_hdr);
  free(f->hoist_pre);
  free(f->hoist_val);
  free(f->hoist_grp);
  free(f->fwd);
  free(f->access);
  free(f->skip);
  free(f->cellskip);
  free(f->elemreg);
  free(f->arrskip);
  free(f->purecall);
  free(f->arr_used);
  free(f->miss_exits);
  if (f->groups != NULL) {
    for (i = 0; i < f->ngroups; i++) free(f->groups[i].sites);
    free(f->groups);
  }
  free(f->sites);
  if (f->blocks != NULL) {
    for (i = 0; i < f->nblocks; i++) {
      free(f->blocks[i].shape_val);
      free(f->blocks[i].shape_grp);
      free(f->blocks[i].avail);
    }
    free(f->blocks);
  }
  memset(f, 0, sizeof(*f));
}

int e_facts_poly_on;

int e_facts_site_poly(const EFacts *f, int site) {
  int i;
  if (f == NULL || f->sites == NULL) return 0;
  for (i = 0; i < f->nsites; i++)
    if (f->sites[i].site == site) return f->sites[i].poly;
  return 0;
}

int e_facts_poly_op(const Q2Insn *in) {
  return in->rop == ROP_get_field || in->rop == ROP_get_field_own ||
         in->rop == ROP_get_field2 || in->rop == ROP_get_field2_own ||
         in->rop == ROP_put_field;
}

int e_facts_site_group(const EFacts *f, int site) {
  int i;
  if (f == NULL || f->sites == NULL) return E_GROUP_NONE;
  for (i = 0; i < f->nsites; i++) {
    if (f->sites[i].site == site) return f->sites[i].group;
  }
  return E_GROUP_NONE;
}

int e_facts_safepoint_free(const Q2Func *fn) {
  EFacts f;
  int i, free_ = 1, saved = e_facts_pure_ok;
  e_facts_pure_ok = 0; /* a call inside the body is a safepoint */
  if (e_facts_run(fn, &f, 1) < 0) {
    e_facts_pure_ok = saved;
    return 0;
  }
  for (i = 0; i < fn->ninsns && free_; i++)
    if (fn->insns[i].rop != ROP_return && fn->insns[i].rop != ROP_return_undefined &&
        is_safepoint(fn, &f, &fn->insns[i], 1)) {
      if (getenv("QJS_AOT_DEBUG_PURE")) {
        char nm[64];
        e_rop_name(fn->insns[i].rop, nm, sizeof(nm));
        fprintf(stderr, "pure: safepoint %s@%d\n", nm, fn->insns[i].pc);
      }
      free_ = 0;
    }
  e_facts_free(&f);
  e_facts_pure_ok = saved;
  return free_;
}

int e_facts_site_proto(const EFacts *f, int site) {
  int i;
  if (f == NULL || f->sites == NULL) return 0;
  for (i = 0; i < f->nsites; i++)
    if (f->sites[i].site == site) return f->sites[i].proto;
  return 0;
}

/* ---- loop-invariant global reads ----------------------------------------
   A loop with no safepoint cannot change a global binding, so a read of it inside the loop
   returns what a read before the loop does. The read is copied to the block that enters the
   outermost such loop and the original becomes a copy of it; the copy's misses leave before
   the loop starts. The facts then see a receiver defined outside the loop, and the array and
   shape checks of its accesses can be done once. Returns the number of reads to hoist, filling
   `*out` (malloc'd). */
int e_plan_licm(const Q2Func *fn, Q2Extra **out) {
  EFacts facts;
  int nl = 0, capl = 0, nex = 0, i, b, p, k;
  struct Lp {
    uint8_t *mem;
    int pre, hdr, size;
  } *loops = NULL;
  uint8_t *loop = NULL;
  int *stack = NULL;
  int saved_pure = e_facts_pure_ok;
  *out = NULL;
  if (getenv("QJS_AOT_NOLICM") != NULL || fn->nblocks <= 0) return 0;
  e_facts_pure_ok = 0;
  memset(&facts, 0, sizeof(facts));
  if (e_facts_run(fn, &facts, 1) < 0) {
    e_facts_pure_ok = saved_pure;
    return 0;
  }
  e_facts_pure_ok = saved_pure;
  loop = calloc((size_t)fn->nblocks, 1);
  stack = malloc(sizeof(int) * (size_t)fn->nblocks);
  if (loop == NULL || stack == NULL) goto done;
  for (b = 0; b < fn->nblocks; b++) {
    const Q2Block *lt = &fn->blocks[b];
    int si;
    for (si = 0; si < lt->nsucc && si < 2; si++) {
      int h = lt->succ_block[si], sp = 0, pre = -1, npre = 0, safe = 1, bi, sz = 0;
      if (h < 0 || h >= fn->nblocks || !q2_is_back_edge(fn, b, h) || lt->resume) continue;
      memset(loop, 0, (size_t)fn->nblocks);
      loop[h] = 1;
      if (!loop[b]) {
        loop[b] = 1;
        stack[sp++] = b;
      }
      while (sp > 0) {
        int x = stack[--sp];
        for (p = 0; p < fn->blocks[x].npreds; p++) {
          int pb = fn->blocks[x].preds[p];
          if (pb < 0 || loop[pb] || fn->rpo_pos[pb] < 0) continue;
          loop[pb] = 1;
          stack[sp++] = pb;
        }
      }
      for (p = 0; p < fn->blocks[h].npreds; p++) {
        int pb = fn->blocks[h].preds[p];
        if (pb < 0 || fn->rpo_pos[pb] < 0 || loop[pb]) continue;
        pre = pb;
        npre++;
      }
      if (npre != 1 || fn->blocks[pre].resume) continue;
      for (bi = 0; bi < fn->nblocks && safe; bi++) {
        if (!loop[bi]) continue;
        sz++;
        for (k = 0; k < fn->blocks[bi].ninsns; k++)
          if (is_safepoint(fn, &facts, &fn->insns[fn->blocks[bi].first_insn + k], 1)) {
            safe = 0;
            if (getenv("QJS_AOT_DEBUG_LICM") != NULL) {
              char nm[64];
              e_rop_name(fn->insns[fn->blocks[bi].first_insn + k].rop, nm, sizeof(nm));
              fprintf(stderr, "licm %s: loop at block %d has a safepoint %s@%d\n", fn->g->name, h, nm,
                      fn->insns[fn->blocks[bi].first_insn + k].pc);
            }
            break;
          }
      }
      if (!safe) continue;
      if (nl == capl) {
        int nc = capl ? capl * 2 : 8;
        struct Lp *q = realloc(loops, sizeof(*q) * (size_t)nc);
        if (q == NULL) goto done;
        loops = q;
        capl = nc;
      }
      loops[nl].mem = malloc((size_t)fn->nblocks);
      if (loops[nl].mem == NULL) goto done;
      memcpy(loops[nl].mem, loop, (size_t)fn->nblocks);
      loops[nl].pre = pre;
      loops[nl].hdr = h;
      loops[nl].size = sz;
      nl++;
    }
  }
  for (i = 0; i < fn->ninsns && nex < 8; i++) {
    const Q2Insn *in = &fn->insns[i];
    int best = -1, blk = -1, j;
    if (in->rop != ROP_get_global || in->xpc >= 0 || in->ndef != 1) continue;
    for (b = 0; b < fn->nblocks; b++)
      if (i >= fn->blocks[b].first_insn && i < fn->blocks[b].first_insn + fn->blocks[b].ninsns) blk = b;
    if (blk < 0) continue;
    for (j = 0; j < nl; j++)
      if (loops[j].mem[blk] && (best < 0 || loops[j].size > loops[best].size)) best = j;
    if (best < 0) continue;
    {
      const Q2Block *pb = &fn->blocks[loops[best].pre];
      const Q2Block *hb = &fn->blocks[loops[best].hdr];
      const Q2Insn *t;
      Q2Extra *q;
      if (pb->ninsns <= 0) continue;
      t = &fn->insns[pb->first_insn + pb->ninsns - 1];
      q = realloc(*out, sizeof(**out) * (size_t)(nex + 1));
      if (q == NULL) goto done;
      *out = q;
      q[nex].src_pc = in->pc;
      q[nex].newreg = fn->nregs + nex;
      if (t->op == Q2_OP_BRANCH || t->op == Q2_OP_BRANCH_CMP) {
        q[nex].anchor_pc = t->pc;
        q[nex].fallthrough = 0;
      } else if (pb->nsucc == 1 && pb->succ_block[0] == loops[best].hdr && hb->start_pc == pb->end_pc &&
                 t->next == pb->end_pc && t->op != Q2_OP_RETURN && t->op != Q2_OP_RETURN_UNDEF) {
        q[nex].anchor_pc = hb->start_pc;
        q[nex].fallthrough = 1;
      } else {
        continue;
      }
      nex++;
    }
  }
done:
  if (loops != NULL)
    for (i = 0; i < nl; i++) free(loops[i].mem);
  free(loops);
  free(loop);
  free(stack);
  e_facts_free(&facts);
  return nex;
}
