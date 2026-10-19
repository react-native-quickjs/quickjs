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
 * emitter reads these and `--emit2-types` prints them.
 *
 * ---- what counts as a safepoint, and why ----
 *
 * D7 step 5 drops every SHAPE fact and applies D6's runs_js kills at a
 * safepoint. Applied literally to `runs_js` in the role table that would make a
 * property read kill the load fact it just established, so no field read could
 * ever be forwarded and D7's whole point (one load per location) would be lost.
 * The role table's `runs_js` describes what the *interpreter handler* may do,
 * including its slow path; the pass has to model the code the speculative body
 * will actually contain:
 *
 *   - A load or store of a local, an argument, a closure variable or `this`, a
 *     constant and a move cannot run JS (a TDZ read throws and leaves), so it
 *     is no safepoint.
 *   - A grouped field access (every site in a group, D9 step 1) has an inline
 *     fast path that runs no JS: a tag and shape test and a slot load or store.
 *     Its miss is an exit to the generic body (D9 steps 3-4), which does not
 *     come back, so on the path that continues no JS ran and the facts hold.
 *     The pass marks it `miss_exits`, and the emitter must emit its miss as an
 *     exit. A miss that continued instead would rejoin with facts a getter or
 *     setter may have broken, and a forwarded value cannot be repaired on the
 *     cold arm alone, because the code after the join is shared.
 *   - Everything else that is not pure -- an ungrouped field access, a global
 *     or element access (whose misses continue, D9), calls, op calls,
 *     fallbacks, allocation -- is a safepoint (S4) and kills everything except
 *     `this`, which is fixed for the activation (D6).
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
#include <stdlib.h>
#include <string.h>

#include "e2_analysis.h"
#include "e2.h"
#include "quickjs-regvm.h"

/* The profile's site and coverage records, from qjsaot.c. */
int qjs_aot_prof_site(
    const uint8_t dg[16], int site, int *nshapes, uint64_t shapes[4]);
int qjs_aot_prof_unexecuted(const uint8_t dg[16], int pc);

/* ---- the fact set -------------------------------------------------------- */

/* A set of facts, as two flat lists. Both are tiny (a handful of entries per
   block in real code), so linear search beats a hash table and keeps the
   intersection simple. */
typedef struct FactSet {
  int nshape, capshape;
  int *shape_val; /* the receiver value */
  int *shape_grp; /* the group */
  int navail, capavail;
  E2Avail *avail;
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

static int key_eq(const E2Key *a, const E2Key *b) {
  return a->kind == b->kind && a->a == b->a && a->b == b->b;
}

static Q2ValueId fs_get_avail(const FactSet *s, const E2Key *k) {
  int i;
  for (i = 0; i < s->navail; i++) {
    if (key_eq(&s->avail[i].key, k)) return s->avail[i].val;
  }
  return Q2_NO_VALUE;
}

/* Add or replace. One fact per location: a location holds one value. */
static int fs_set_avail(FactSet *s, const E2Key *k, Q2ValueId v) {
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
    E2Avail *q = realloc(s->avail, sizeof(*q) * (size_t)cap);
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
typedef int (*KeyPred)(void *arg, const E2Key *k);

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

static int kp_arg_slot(void *arg, const E2Key *k) {
  return k->kind == E2_K_ARG && k->a == *(const int64_t *)arg;
}

static int kp_local_slot(void *arg, const E2Key *k) {
  return k->kind == E2_K_LOCAL && k->a == *(const int64_t *)arg;
}

static int kp_varref_slot(void *arg, const E2Key *k) {
  return k->kind == E2_K_VARREF && k->a == *(const int64_t *)arg;
}

static int kp_global_atom(void *arg, const E2Key *k) {
  return k->kind == E2_K_GLOBAL && k->a == *(const int64_t *)arg;
}

/* Two different atoms never alias, so a store kills only its own atom -- on any
   receiver, because two receivers may be the same object (D6). */
static int kp_field_atom(void *arg, const E2Key *k) {
  return k->kind == E2_K_FIELD && k->b == *(const int64_t *)arg;
}

static int kp_elem(void *arg, const E2Key *k) {
  (void)arg;
  return k->kind == E2_K_ELEM;
}

/* A safepoint: everything reachable from the heap may have changed, and a
   refill may have freed a shape and handed its address to another one. `this`
   is fixed for the activation and survives (D6). */
static int kp_safepoint(void *arg, const E2Key *k) {
  (void)arg;
  return k->kind != E2_K_THIS;
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

/* One property access: the PIC site it uses and the D6 location it reads or
   writes. */
typedef struct SiteRef {
  int site;     /* the PIC index, or REG_IC_NONE */
  int64_t recv; /* a value id, E2_RECV_THIS or E2_RECV_LOCAL(slot) */
  int64_t atom;
  int is_store;
} SiteRef;

/* The D6 receiver key of a field access whose site is operand `si`.
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
        return E2_RECV_THIS;
      if (def_recv != Q2_NO_VALUE) return (int64_t)def_recv;
      return -1;
    }
    (void)0;
  }
  /* "ruas": the receiver is a local slot, not a register. */
  if (in->rop == ROP_get_local_field) {
    int u = arg_nearest(in, si - 1, 'u');
    if (u >= 0) return E2_RECV_LOCAL(in->arg[u].val);
  }
  if (in->rop >= ROP_get_local_field_0 && in->rop <= ROP_get_local_field_3)
    return E2_RECV_LOCAL((int64_t)(in->rop - ROP_get_local_field_0));
  if (in->rop == ROP_get_this_field || in->rop == ROP_get_this_field2)
    return E2_RECV_THIS;
  return -1;
}

/* Collect the property accesses of one instruction (a chain has two). Returns
   the count, 0 to 2. */
static int site_refs(const Q2Insn *in, SiteRef out[2]) {
  const E2Role *role = e2_roles[in->rop];
  int n = 0, i;

  if (role == NULL || role->mem != E2_MEM_FIELD) return 0;
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
 * E2_SK_PURE     no safepoint: it cannot run JS or allocate.
 * E2_SK_NOJS     a memory access that cannot run JS: no safepoint.
 * E2_SK_FIELD    a field access: no safepoint when every site it uses is
 *                grouped (its miss then exits), a safepoint otherwise.
 * E2_SK_SAFE     a safepoint.
 */
#define E2_SK_PURE 0
#define E2_SK_NOJS 1
#define E2_SK_FIELD 2
#define E2_SK_SAFE 3

static int insn_kind(const Q2Insn *in) {
  const E2Role *role = e2_roles[in->rop];
  if (role == NULL) return E2_SK_SAFE;
  if (role->pure) return E2_SK_PURE;
  switch (in->op) {
    case Q2_OP_GET_FIELD:
    case Q2_OP_PUT_FIELD:
      return E2_SK_FIELD;
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
      return E2_SK_NOJS;
    default:
      return E2_SK_SAFE;
  }
}

/* ---- loads and stores the pass tracks ----------------------------------- */

typedef struct Access {
  int kind;      /* E2_K_*, or 0 when the instruction is neither */
  E2Key key;     /* the location */
  Q2ValueId val; /* the value read (load) or written (store) */
  int is_store;
  int fwd_ok;    /* the whole instruction is this one location's load */
} Access;

static int insn_load(const Q2Insn *in, Access *a) {
  memset(a, 0, sizeof(*a));
  switch (in->rop) {
    case ROP_get_arg:
      a->kind = E2_K_ARG;
      a->key.kind = E2_K_ARG;
      a->key.a = in->narg > 1 ? in->arg[1].val : -1;
      break;
    case ROP_get_arg2:
      /* Two slots, two destinations: the pass tracks neither (one fact per
         instruction would have to pick one). */
      return 0;
    case ROP_get_local:
      a->kind = E2_K_LOCAL;
      a->key.kind = E2_K_LOCAL;
      a->key.a = in->narg > 1 ? in->arg[1].val : -1;
      break;
    case ROP_get_local_s0:
    case ROP_get_local_s1:
    case ROP_get_local_s2:
    case ROP_get_local_s3:
      a->kind = E2_K_LOCAL;
      a->key.kind = E2_K_LOCAL;
      a->key.a = in->rop - ROP_get_local_s0;
      break;
    case ROP_get_var_ref:
      a->kind = E2_K_VARREF;
      a->key.kind = E2_K_VARREF;
      a->key.a = in->narg > 1 ? in->arg[1].val : -1;
      break;
    case ROP_get_global: {
      int ai = arg_nearest(in, in->narg - 1, 'a');
      a->kind = E2_K_GLOBAL;
      a->key.kind = E2_K_GLOBAL;
      a->key.a = ai >= 0 ? in->arg[ai].val : -1;
      break;
    }
    case ROP_load_this:
      a->kind = E2_K_THIS;
      a->key.kind = E2_K_THIS;
      break;
    case ROP_get_element: {
      Q2ValueId obj = regval(in, 1), idx = regval(in, 2);
      if (obj == Q2_NO_VALUE || idx == Q2_NO_VALUE) return 0;
      a->kind = E2_K_ELEM;
      a->key.kind = E2_K_ELEM;
      a->key.a = (int64_t)obj;
      a->key.b = (int64_t)idx;
      break;
    }
    case ROP_get_length: {
      Q2ValueId obj = regval(in, 1);
      if (obj == Q2_NO_VALUE) return 0;
      a->kind = E2_K_ELEM;
      a->key.kind = E2_K_ELEM;
      a->key.a = (int64_t)obj;
      a->key.b = E2_ELEM_LEN;
      break;
    }
    default: {
      /* A property read: one location, keyed by (receiver, atom). */
      SiteRef ref[2];
      int nr = site_refs(in, ref);
      if (nr != 1) return 0; /* a chain: two locations, no single move */
      if (ref[0].site < 0 || (unsigned)ref[0].site == REG_IC_NONE) return 0;
      if (ref[0].recv < 0 || ref[0].atom == 0) return 0;
      a->kind = E2_K_FIELD;
      a->key.kind = E2_K_FIELD;
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
      a->kind = E2_K_ARG;
      a->key.kind = E2_K_ARG;
      a->key.a = in->narg > 0 ? in->arg[0].val : -1;
      a->val = regval(in, 1);
      break;
    case ROP_put_local:
    case ROP_put_local_xfer:
      a->kind = E2_K_LOCAL;
      a->key.kind = E2_K_LOCAL;
      a->key.a = in->narg > 0 ? in->arg[0].val : -1;
      a->val = regval(in, 1);
      break;
    case ROP_put_local_s0:
    case ROP_put_local_s1:
    case ROP_put_local_s2:
    case ROP_put_local_s3:
      a->kind = E2_K_LOCAL;
      a->key.kind = E2_K_LOCAL;
      a->key.a = in->rop - ROP_put_local_s0;
      a->val = regval(in, 0);
      break;
    case ROP_put_var_ref:
      a->kind = E2_K_VARREF;
      a->key.kind = E2_K_VARREF;
      a->key.a = in->narg > 0 ? in->arg[0].val : -1;
      a->val = regval(in, 1);
      break;
    case ROP_put_global:
      a->kind = E2_K_GLOBAL;
      a->key.kind = E2_K_GLOBAL;
      a->key.a = in->narg > 0 ? in->arg[0].val : -1;
      a->val = regval(in, 1);
      break;
    case ROP_put_element:
    case ROP_put_element_xfer: {
      Q2ValueId obj = regval(in, 0), idx = regval(in, 1);
      if (obj == Q2_NO_VALUE || idx == Q2_NO_VALUE) return 0;
      a->kind = E2_K_ELEM;
      a->key.kind = E2_K_ELEM;
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
      a->kind = E2_K_FIELD;
      a->key.kind = E2_K_FIELD;
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

/* ---- site groups (D9 step 1) -------------------------------------------- */

static int build_groups(const Q2Func *fn, E2Facts *f) {
  int i, k, n = 0, cap = 0, ng = 0;
  E2SiteInfo *sites = NULL;
  E2Group *groups = NULL;
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
        E2SiteInfo *q = realloc(sites, sizeof(*q) * (size_t)nc);
        if (q == NULL) goto oom;
        sites = q;
        cap = nc;
      }
      sites[n].site = ref[r].site;
      sites[n].insn = i;
      sites[n].group = E2_GROUP_NONE;
      sites[n].recv = ref[r].recv < 0 ? Q2_NO_VALUE : (Q2ValueId)ref[r].recv;
      n++;
    }
  }
  for (k = 0; k < n; k++) {
    int ns = 0, g = E2_GROUP_NONE, j;
    uint64_t shapes[4];
    const Q2Insn *in = &fn->insns[sites[k].insn];
    if (!qjs_aot_prof_site(fn->g->digest, sites[k].site, &ns, shapes)) continue;
    if (ns != 1) continue; /* polymorphic or never filled: no group */
    if (qjs_aot_prof_unexecuted(fn->g->digest, in->pc)) continue;
    for (j = 0; j < ng; j++) {
      if (gshape[j] == (int64_t)shapes[0]) {
        g = j;
        break;
      }
    }
    if (g == E2_GROUP_NONE) {
      E2Group *q = realloc(groups, sizeof(*q) * (size_t)(ng + 1));
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
      E2Group *gr = &groups[g];
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
static int transfer(const Q2Func *fn, E2Facts *f, int insn, FactSet *s) {
  const Q2Insn *in = &fn->insns[insn];
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
       tagged value there (S2). Nothing ran, so the instruction is no safepoint
       and no fact changes. */
    return 0;
  }

  /* A field access is no safepoint only when every site it uses is grouped:
     only then is its miss an exit. */
  if (kind == E2_SK_FIELD) {
    SiteRef ref[2];
    int nr = site_refs(in, ref), r, grouped = nr > 0;
    for (r = 0; r < nr; r++)
      if (ref[r].site < 0 || ref[r].recv < 0 ||
          e2_facts_site_group(f, ref[r].site) < 0)
        grouped = 0;
    kind = grouped ? E2_SK_NOJS : E2_SK_SAFE;
    f->miss_exits[insn] = (uint8_t)grouped;
  }

  /* 2. A guarded access of a receiver whose shape is known skips its tag and
     shape test; either way the shape is known afterwards. */
  if (kind != E2_SK_SAFE) {
    SiteRef ref[2];
    int nr = site_refs(in, ref), r;
    for (r = 0; r < nr; r++) {
      int g = ref[r].site >= 0 ? e2_facts_site_group(f, ref[r].site) : -1;
      if (g < 0 || ref[r].recv < 0) continue;
      f->access[insn] = 1;
      if (fs_has_shape(s, (int)ref[r].recv, g)) {
        f->skip[insn] = 1;
        f->nskip++;
      }
      if (fs_add_shape(s, (int)ref[r].recv, g) < 0) return -1;
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
    switch (st.kind) {
      case E2_K_ARG: {
        int64_t slot = st.key.a;
        fs_kill(s, kp_arg_slot, &slot);
        break;
      }
      case E2_K_LOCAL: {
        int64_t slot = st.key.a;
        fs_kill(s, kp_local_slot, &slot);
        break;
      }
      case E2_K_VARREF: {
        int64_t slot = st.key.a;
        fs_kill(s, kp_varref_slot, &slot);
        break;
      }
      case E2_K_GLOBAL: {
        int64_t atom = st.key.a;
        fs_kill(s, kp_global_atom, &atom);
        break;
      }
      case E2_K_FIELD: {
        /* Different atoms never alias; the same atom on another receiver may be
           the same object, so it kills them all (D6). */
        int64_t atom = st.key.b;
        fs_kill(s, kp_field_atom, &atom);
        break;
      }
      case E2_K_ELEM: {
        /* Any element store may move, extend or reshape the store. */
        fs_kill(s, kp_elem, NULL);
        break;
      }
      default:
        break;
    }
    if (fs_set_avail(s, &st.key, st.val) < 0) return -1;
  }

  /* 5. A safepoint loses every shape and every location (D7 step 5), except
     `this`. It applies after the instruction's own load or store, so an
     ungrouped read does not leave a fact behind either: its miss may have run
     a getter. */
  if (kind == E2_SK_SAFE) {
    fs_kill(s, kp_safepoint, NULL);
    s->nshape = 0;
  }
  (void)fn;
  return 0;
}

int e2_facts_run(const Q2Func *fn, E2Facts *out) {
  FactSet cur;
  FactSet *outs = NULL;
  int b, i, k, rc = -1;

  memset(out, 0, sizeof(*out));
  memset(&cur, 0, sizeof(cur));
  out->ninsns = fn->ninsns;
  out->nblocks = fn->nblocks;
  out->fwd =
      malloc(sizeof(*out->fwd) * (size_t)(fn->ninsns > 0 ? fn->ninsns : 1));
  out->access = calloc((size_t)(fn->ninsns > 0 ? fn->ninsns : 1), 1);
  out->skip = calloc((size_t)(fn->ninsns > 0 ? fn->ninsns : 1), 1);
  out->miss_exits = calloc((size_t)(fn->ninsns > 0 ? fn->ninsns : 1), 1);
  out->blocks = calloc((size_t)(fn->nblocks > 0 ? fn->nblocks : 1),
                       sizeof(*out->blocks));
  outs = calloc((size_t)(fn->nblocks > 0 ? fn->nblocks : 1), sizeof(*outs));
  if (out->fwd == NULL || out->access == NULL || out->skip == NULL ||
      out->miss_exits == NULL || out->blocks == NULL || outs == NULL)
    goto done;
  for (i = 0; i < fn->ninsns; i++) out->fwd[i] = Q2_NO_VALUE;
  if (build_groups(fn, out) < 0) goto done;

  /* The blocks are in pc order, which is a reverse postorder: a block starts at
     every branch target and after every terminator, so only a back edge points
     at a lower index. A forward edge into a block the pass has not visited yet
     therefore cannot happen, and any unvisited predecessor is a back edge. */
  for (b = 0; b < fn->nblocks; b++) {
    const Q2Block *blk = &fn->blocks[b];
    int p, first = 1, back_edge = 0;

    if (b == 0 || blk->npreds <= 0) {
      fs_clear(&cur);
    } else {
      for (p = 0; p < blk->npreds; p++) {
        if (blk->preds[p] < 0 || blk->preds[p] >= b) {
          back_edge = 1;
          break;
        }
      }
      if (back_edge) {
        /* A loop header starts empty: the back edge carries no fact the first
           time round, so none may be assumed. */
        fs_clear(&cur);
      } else {
        for (p = 0; p < blk->npreds; p++) {
          const FactSet *o = &outs[blk->preds[p]];
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

    /* Keep the in-set: `--emit2-types` prints it per block. */
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
      out->blocks[b].avail = malloc(sizeof(E2Avail) * (size_t)cur.navail);
      if (out->blocks[b].avail == NULL) goto done;
      memcpy(
          out->blocks[b].avail, cur.avail,
          sizeof(E2Avail) * (size_t)cur.navail);
    }

    for (k = 0; k < blk->ninsns; k++) {
      if (transfer(fn, out, blk->first_insn + k, &cur) < 0) goto done;
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
  if (rc < 0) e2_facts_free(out);
  return rc;
}

void e2_facts_free(E2Facts *f) {
  int i;
  if (f == NULL) return;
  free(f->fwd);
  free(f->access);
  free(f->skip);
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

int e2_facts_site_group(const E2Facts *f, int site) {
  int i;
  if (f == NULL || f->sites == NULL) return E2_GROUP_NONE;
  for (i = 0; i < f->nsites; i++) {
    if (f->sites[i].site == site) return f->sites[i].group;
  }
  return E2_GROUP_NONE;
}
