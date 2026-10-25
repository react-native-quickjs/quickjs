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
int qjs_aot_prof_site_slots(const uint8_t dg[16], int site, int nshapes, int *slots,
                            int cap);
const void *qjs_aot_prof_shape(int id, int *proto, char *digest, int *nfields,
                               const char ***fields, unsigned **flags);
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

/* every fact about one value is dropped (the region's locals now describe another kind of array) */
static void fs_drop_val(FactSet *s, int val) {
  int i, w = 0;
  for (i = 0; i < s->nshape; i++) {
    if (s->shape_val[i] == val) continue;
    s->shape_val[w] = s->shape_val[i];
    s->shape_grp[w] = s->shape_grp[i];
    w++;
  }
  s->nshape = w;
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

static const Q2Func *g_facts_fn;
int qjs_aot_pool_num(const struct JSAotGenFunc *g, uint32_t idx, double *out);
static int insn_kind(const Q2Insn *in) {
  const ERole *role = e_roles[in->rop];
  /* A constant-pool operand that is a number: the speculative body lowers the op for numbers
     and leaves for anything else, like the other arithmetic with an immediate. */
  if (in->rop == ROP_binary_cpool && g_facts_fn != NULL && in->narg > 4 && in->arg[3].val == REG_CONST_CPOOL) {
    double c;
    if (qjs_aot_pool_num(q2_insn_g(g_facts_fn, in), (uint32_t)in->arg[4].val, &c)) return E_SK_PURE;
  }
  /* the guard of a copied callee leaves for the call or falls into the copy */
  if (in->inlg || in->vk == Q2_VK_DEF) return E_SK_PURE;
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
      sites[n].own = e_site_own(fn, ref[r].site);
      sites[n].insn = i;
      sites[n].group = E_GROUP_NONE;
      sites[n].recv = ref[r].recv < 0 ? Q2_NO_VALUE : (Q2ValueId)ref[r].recv;
      sites[n].poly = 0;
      sites[n].proto = 0;
      {
        Access stw;
        sites[n].write = insn_store(in, &stw) && stw.kind == E_K_FIELD;
        sites[n].hot = !in->own && qjs_aot_prof_pc_count(e_dg(fn, in), in->opc) > 0;
      }
      n++;
    }
  }
  for (k = 0; k < n; k++) {
    int ns = 0, g = E_GROUP_NONE, j, osite;
    uint64_t shapes[4];
    const Q2Insn *in = &fn->insns[sites[k].insn];
    const uint8_t *dg = e_site_dg(fn, sites[k].site, &osite);
    if (!qjs_aot_prof_site(dg, osite, &ns, shapes)) continue;
    if (ns >= 2 && ns <= 4 && qjs_aot_prof_site_own(dg, osite) &&
        !qjs_aot_prof_unexecuted(dg, in->opc)) {
      sites[k].poly = 1; /* served by the polymorphic cache, not grouped */
      continue;
    }
    if (ns != 1) continue; /* never filled, or too many shapes: no group */
    /* Only an own readable or writable slot has a fixed offset to group on. */
    if (qjs_aot_prof_site_proto(dg, osite) && !sites[k].write &&
        getenv("QJS_AOT_NOPROTOGROUP") == NULL) {
      sites[k].proto = 1;
    } else if (qjs_aot_prof_site_proto2(dg, osite) && !sites[k].write &&
               getenv("QJS_AOT_NOPROTOGROUP") == NULL) {
      sites[k].proto = 2;
    } else if (!qjs_aot_prof_site_own(dg, osite)) {
      continue;
    }
    if (qjs_aot_prof_unexecuted(dg, in->opc)) continue;
    for (j = 0; j < ng; j++) {
      if (gshape[j] == (int64_t)shapes[0] && groups[j].own == sites[k].own) {
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
      groups[ng].own = sites[k].own;
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

/* The typed-array class an element site was seen with: 1 + the class's index among the typed arrays
   (Uint8Clamped first), or 0 for a dense array or a site no region can serve (two classes, a half-float
   or a BigInt array). The element sites of the same array must agree: the region keeps one class. */
int e_ta_class(const Q2Func *fn, const Q2Insn *in) {
  int i;
  if (in->rop != ROP_get_element && in->rop != ROP_put_element && in->rop != ROP_put_element_xfer) return 0;
  if (fn->prof == NULL) return 0;
  for (i = 0; i < fn->nprof; i++) {
    unsigned m, k;
    if (fn->prof[i].pc != in->pc) continue;
    if (!(fn->prof[i].elem & Q2_ELEM_TA)) return 0;
    m = fn->prof[i].ta;
    if (m == 0 || (m & (m - 1)) != 0) return 0;
    k = (unsigned)__builtin_ctz(m);
    /* Uint8Clamped, Int8, Uint8, Int16, Uint16, Int32, Uint32, Float32, Float64 */
    if (k <= 6 || k == 10 || k == 11) return (int)k + 1;
    return 0;
  }
  return 0;
}

/* The packed storage kind an element site was seen with, as a group code: E_GRP_PK_I32 or
   E_GRP_PK_F64 when the site saw exactly one packed kind and no hole, object, mixed or typed-array
   element, else 0 (today's code). The profile's `pk=` records the kind of the array the site
   accessed, so a site that saw a generic array too is not packed. */
int e_pk_grp(const Q2Func *fn, const Q2Insn *in, int *kindp) {
  int i;
  if (in->rop != ROP_get_element && in->rop != ROP_put_element && in->rop != ROP_put_element_xfer) return 0;
  if (fn->prof == NULL) return 0;
  for (i = 0; i < fn->nprof; i++) {
    unsigned p;
    if (fn->prof[i].pc != in->pc) continue;
    if (fn->prof[i].elem & (Q2_ELEM_HOLEY | Q2_ELEM_TA | Q2_ELEM_OBJECT | Q2_ELEM_MIXED)) return 0;
    p = fn->prof[i].pk;
    if (p == Q2_PK_I32) { if (kindp) *kindp = 1; return E_GRP_PK_I32; }
    if (p == Q2_PK_F64) { if (kindp) *kindp = 2; return E_GRP_PK_F64; }
    return 0;
  }
  return 0;
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
    /* An array whose storage kind was recorded: it is a dense region only when the site never saw
       packed storage (the dense entry check refuses a non-zero kind byte). A site that saw both
       packed kinds saw the storage change, so no single entry check holds for the whole loop and
       it is no region either; a site that saw a packed kind and the generic one keeps the dense
       route, which refuses a packed array at run time and takes the single-access helper's path. */
    if ((fn->prof[i].pk & (Q2_PK_I32 | Q2_PK_F64)) != 0 && e_pk_grp(fn, in, NULL) == 0) {
      unsigned packed = fn->prof[i].pk & (Q2_PK_I32 | Q2_PK_F64);
      if (packed == (Q2_PK_I32 | Q2_PK_F64)) return 0;
    }
    /* A typed array of one class: reads and stores are numbers by construction. */
    if ((fn->prof[i].elem & Q2_ELEM_TA) != 0)
      return e_ta_class(fn, in) != 0 && (fn->prof[i].elem & (Q2_ELEM_HOLEY | Q2_ELEM_OBJECT | Q2_ELEM_MIXED)) == 0;
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
static int e_facts_virtual_planning;
static int global_exit_ok(const Q2Func *fn, const Q2Insn *in) {
  int ns = 0;
  uint64_t shapes[4];
  if (in->rop != ROP_get_global || in->narg < 4 || in->arg[3].kind != 's' ||
      (unsigned)in->arg[3].val == REG_IC_NONE ||
      getenv("QJS_AOT_NOGLOBALEXIT") != NULL)
    return 0;
  /* Filled at exactly one generation of the global object in the profiled run: the cell holds
     unless the global object changes, which that run did not see. */
  {
    int osite;
    const uint8_t *dg = e_site_dg(fn, (int)in->arg[3].val, &osite);
    if (!qjs_aot_prof_site(dg, osite, &ns, shapes)) return 0;
  }
  /* A site that ran many times and was filled only a couple of times saw the global object
     change at startup, not in its steady state. */
  if (ns == 1 && qjs_aot_prof_pc_count(e_dg(fn, in), in->opc) == 0 && fn->nvweb == 0 && !e_facts_virtual_planning)
    return 0; /* (the profile has no sample here; with unmade objects a cold read leaves the same way) */
  return ns == 1 || (ns <= 3 && qjs_aot_prof_pc_count(e_dg(fn, in), in->opc) >= 16384);
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
      /* the fact names the region's class (0: a dense array, 1..12: a typed array's class,
         101/102: a packed array of int32/float64 elements): the locals hold one array kind at a
         time, so a second access to the same receiver in the same kind needs no entry */
      int cls = e_ta_class(fn, in);
      if (cls == 0) {
        int pk = 0;
        if (e_pk_grp(fn, in, &pk) != 0) cls = 100 + pk;
      }
      if (fs_has_shape(s, E_ARRV(rv), cls)) {
        f->arrskip[insn] = 1;
      } else {
        fs_drop_val(s, E_ARRV(rv));
        if (fs_add_shape(s, E_ARRV(rv), cls) < 0) return -1;
      }
      f->arr_used[rv] = 1;
      if (cls >= 101) f->arr_pk[rv] = 1;   /* a packed region: it has its own storage locals */
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
    /* A store into a typed array converts the value (wraps, clamps, rounds to a float): the element
       does not hold the stored value, so the location is not made current by it. */
    if (!(st.kind == E_K_ELEM && e_ta_class(fn, in)) && fs_set_avail(s, &st.key, st.val) < 0) return -1;
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
          int agrp;
          if (!grouped_exit || !elem_region_ok(fn, in)) continue;
          v = in->use[0];
          if (v >= (Q2ValueId)fn->nvalues) continue;
          db = fn->values[v].block;
          if (db < 0 || in_loop(loop, db)) continue;
          /* -2: a dense array; -100 - c: a typed array of class c (1..12); -121/-122: a packed
             array of int32/float64 elements. A site whose profile saw packed storage never gets -2:
             the dense entry check refuses any array with a non-zero kind byte, so the loop would
             leave on its first iteration. elem_region_ok already refused a site that saw both
             kinds, so reaching here with a packed site means exactly one kind. */
          agrp = e_ta_class(fn, in) ? -100 - e_ta_class(fn, in) : -2;
          if (e_pk_grp(fn, in, NULL) != 0) agrp = e_pk_grp(fn, in, NULL);
          for (q = 0; q < out->nhoist; q++)
            if (out->hoist_hdr[q] == h && out->hoist_val[q] == (int)v && out->hoist_grp[q] == agrp) dup = 1;
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
          out->hoist_grp[out->nhoist] = agrp;
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
  g_facts_fn = fn;
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
  out->arr_pk = calloc((size_t)(fn->nvalues > 0 ? fn->nvalues : 1), 1);
  out->miss_exits = calloc((size_t)(fn->ninsns > 0 ? fn->ninsns : 1), 1);
  out->blocks = calloc((size_t)(fn->nblocks > 0 ? fn->nblocks : 1),
                       sizeof(*out->blocks));
  outs = calloc((size_t)(fn->nblocks > 0 ? fn->nblocks : 1), sizeof(*outs));
  if (out->fwd == NULL || out->access == NULL || out->skip == NULL || out->cellskip == NULL || out->elemreg == NULL ||
      out->arrskip == NULL || out->arr_used == NULL || out->arr_pk == NULL || out->purecall == NULL ||
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
          /* The loop can also be entered at its header with the registers taken from the
             frame (a re-entry after an exit), which skips the preheader: a global read is known on
             that path only if its value is live in a register there. */
          for (j = 0; j < outs[pre].navail; j++) {
            int li, live = 0;
            for (li = 0; li < blk->nlive_in; li++)
              if (blk->in_val[li] == outs[pre].avail[j].val) live = 1;
            if (outs[pre].avail[j].key.kind != E_K_GLOBAL) live = 1;
            if (live && fs_set_avail(&cur, &outs[pre].avail[j].key, outs[pre].avail[j].val) < 0)
              goto done;
          }
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
          } else if (out->hoist_grp[k] <= -101 && out->hoist_grp[k] >= -112) {
            if (fs_add_shape(&cur, E_ARRV(out->hoist_val[k]), -100 - out->hoist_grp[k]) < 0) goto done;
          } else if (out->hoist_grp[k] == E_GRP_PK_I32 || out->hoist_grp[k] == E_GRP_PK_F64) {
            /* a packed region: the same fact the dense entry seeds, with the kind's class code
               (100 + kind), so a later access in the loop finds the region and skips its entry */
            if (fs_add_shape(&cur, E_ARRV(out->hoist_val[k]),
                             100 + (out->hoist_grp[k] == E_GRP_PK_I32 ? 1 : 2)) < 0) goto done;
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
  free(f->arr_pk);
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

/* ---- copied callees ------------------------------------------------------
   A call the profile saw reach one compiled function, at a pc that ran, whose callee is a small
   function of plain arithmetic (no field, no call, no allocation): its instructions are copied
   into the caller after a guard on the callee value (see q2_build_inl). */
const JSAotGenFunc *qjs_aot_unit_func(const char hex[33]);
int qjs_aot_unit_has(const char hex[33]);
int qjs_aot_unit_argc(const char hex[33]);
int qjs_aot_light_need(const char hex[33]);

int e_inline_allow_construct = 1;
typedef struct CtorSum {
  int nfields, atom[8], aoff[8], argidx[8], site[8], nparams;
} CtorSum;
static int ctor_candidate(const Q2Func *fn, const Q2Insn *ci, const JSAotGenFunc **cg_out, CtorSum *sum);

static const char *e_inl_why;
/* the operations the copies lower beyond what q2_lowered lists: the bitwise group, value comparisons, a chained read and a constant-pool operand */
static int inl_extra_rop(uint16_t rop) {
  switch (rop) {
    case ROP_bitand:
    case ROP_bitor:
    case ROP_bitxor:
    case ROP_shl:
    case ROP_sar:
    case ROP_shr:
    case ROP_lt:
    case ROP_lte:
    case ROP_gt:
    case ROP_gte:
    case ROP_binary_cpool:
    case ROP_add_imm_self_s:
    case ROP_sub_imm_self_s:
    case ROP_div:
    case ROP_mod:
    case ROP_not:
    case ROP_to_numeric:
    case ROP_post_inc:
    case ROP_post_dec:
    case ROP_eq:
    case ROP_neq:
    case ROP_strict_eq:
    case ROP_strict_neq:
    case ROP_unary:
      return 1;
    default:
      return 0;
  }
}
static int inl_callee_ok(const JSAotGenFunc *cg, int has_recv) {
  Q2Func cf;
  int i, ok = 1, has_back_edge = 0;
  e_inl_why = "ok";
  if (cg->func_kind != 0 || cg->ctl_count != 0 || cg->light_var_count != 0 || cg->arg_count > 6 ||
      cg->len > 400) {
    e_inl_why = "kind/ctl/vars/args/len";
    return 0;
  }
  if (q2_build(cg, &cf) < 0) {
    q2_free(&cf);
    return 0;
  }
  if (cf.ninsns > 70) { ok = 0; e_inl_why = "more than 70 instructions"; }
  for (i = 0; ok && i < cf.ninsns; i++) {
    const Q2Insn *bi = &cf.insns[i];
    int bt;
    for (bt = 0; bt < bi->narg; bt++)
      if (bi->arg[bt].kind == 't' && bi->next + (int32_t)bi->arg[bt].val <= bi->pc) has_back_edge = 1;
  }
  for (i = 0; ok && i < cf.ninsns; i++) {
    const Q2Insn *in = &cf.insns[i];
    /* A copy may build an object of a plain constructor: the copy is kept only if that object is
       never made (see e_plan_virtual), so its constructor must be one that can be. */
    if (in->rop == ROP_get_global) {
      int saved = e_facts_virtual_planning, gok;
      e_facts_virtual_planning = 1;
      gok = global_exit_ok(&cf, in);
      e_facts_virtual_planning = saved;
      if (!e_inline_allow_construct || !gok) ok = 0;
      continue;
    }
    if (in->rop == ROP_construct) {
      const JSAotGenFunc *cg2;
      CtorSum sm;
      if (!e_inline_allow_construct || !ctor_candidate(&cf, in, &cg2, &sm)) ok = 0;
      continue;
    }
    switch (in->op) {
      case Q2_OP_CONST:
      case Q2_OP_MOVE:
      case Q2_OP_BINARY:
      case Q2_OP_COMPARE:
      case Q2_OP_UNARY:
      case Q2_OP_BRANCH:
      case Q2_OP_BRANCH_CMP:
      case Q2_OP_RETURN:
      case Q2_OP_RETURN_UNDEF:
      case Q2_OP_GET_ARG:
      case Q2_OP_GET_ARG2:
      case Q2_OP_PUT_ARG:
        break;
      case Q2_OP_LOAD_THIS:
        if (!has_recv) ok = 0;
        break;
      case Q2_OP_GET_FIELD:
        /* a read of a field of a register, or of `this` when the call has a receiver */
        if (in->rop == ROP_get_this_field || in->rop == ROP_get_this_field2) {
          if (!has_recv) ok = 0;
        } else if (in->rop != ROP_get_field && in->rop != ROP_get_field2 &&
                   in->rop != ROP_get_field_own && in->rop != ROP_get_field2_own &&
                   in->rop != ROP_get_field_chain && in->rop != ROP_get_field_chain_own) {
          ok = 0;
        }
        break;
      case Q2_OP_PUT_FIELD:
        /* A store into a field a copy may make: undone if the copy has to leave after it, so the
           callee must not loop (a store would then be undone twice over). */
        if (in->rop != ROP_put_field || has_back_edge) ok = 0;
        break;
      default:
        ok = 0;
    }
    if (!q2_lowered(in->rop) && !inl_extra_rop(in->rop)) ok = 0;
    if (!ok && strcmp(e_inl_why, "ok") == 0) {
      static char why[80];
      char nm[48];
      e_rop_name(in->rop, nm, sizeof(nm));
      snprintf(why, sizeof(why), "instruction %s", nm);
      e_inl_why = why;
    }
  }
  q2_free(&cf);
  return ok;
}

/* Whether a construct in a function could be lowered in a non-inlined body too: the
   same test the virtual-object plan uses (`ctor_candidate`: a profiled constructor that
   is compiled into this unit, with a summary the plan understands), and only when the
   copy is allowed to build an object at all. A copied body then emits the construct
   itself, so accepting it here does not widen what qualifies -- it is the same
   condition, consulted earlier. */
int e_inl_construct_allowed(const Q2Func *fn, const Q2Insn *ci) {
  char hex[4][33];
  int kind[4], cnt = 0, k, nb = 0;
  unsigned long long hits[4];

  if (!e_inline_allow_construct) return 0;
  if (ci->rop != ROP_construct || ci->ndef < 1 || ci->narg < 5) return 0;
  /* The test the non-inlined construct path itself applies: a profiled callee that
     is compiled into this unit, has a native entry, and is given every argument it
     declares. A copy that lowers a construct therefore lowers exactly what that path
     would have lowered -- the condition is the same one, consulted earlier. */
  if (!qjs_aot_prof_callees(e_dg(fn, ci), ci->onext, &cnt, hex, kind, hits)) return 0;
  for (k = 0; k < cnt && k < 4; k++) {
    if (kind[k] != 1 || !qjs_aot_unit_has(hex[k])) continue;
    if (qjs_aot_light_need(hex[k]) <= 0) continue;
    if (ci->win_n < qjs_aot_unit_argc(hex[k])) continue;
    nb++;
  }
  return nb > 0;
}

/* The most program bytes (about five per instruction) of callees copied into one function: 3000
   is about 600 instructions. A call over the budget stays a call. */
#define E_INLINE_COPY_BYTES 3000

int e_plan_inline(const Q2Func *fn, Q2InlReq **out) {
  int i, n = 0, copied = 0;
  *out = NULL;
  if (getenv("QJS_AOT_NOIRINLINE") != NULL) return 0;
  for (i = 0; i < fn->ninsns && n < 6; i++) {
    const Q2Insn *in = &fn->insns[i];
    char hex[4][33];
    int kind[4], cnt = 0, k;
    unsigned long long hits[4];
    const JSAotGenFunc *cg;
    Q2InlReq *q;
    if ((in->rop != ROP_call && in->rop != ROP_call_method) || in->xpc >= 0 || in->own || in->inlg) continue;
    if (getenv("QJS_AOT_DEBUG_INL")) fprintf(stderr, "inl %s@%d: win_n=%d nredef=%d ndef=%d count=%llu\n", fn->g->name, in->pc, in->win_n, in->nredef, in->ndef, qjs_aot_prof_pc_count(fn->g->digest, in->ppc));
    if (in->win_n > 6 || in->nredef != 0 || in->ndef != 1) continue;
    if (qjs_aot_prof_pc_count(fn->g->digest, in->ppc) == 0) continue;
    if (!qjs_aot_prof_callees(fn->g->digest, in->next, &cnt, hex, kind, hits) || cnt != 1 || kind[0] != 1) {
      if (getenv("QJS_AOT_DEBUG_INL")) fprintf(stderr, "  callees cnt=%d\n", cnt);
      continue;
    }
    if (!qjs_aot_unit_has(hex[0])) { if (getenv("QJS_AOT_DEBUG_INL")) fprintf(stderr, "  not in unit\n"); continue; }
    cg = qjs_aot_unit_func(hex[0]);
    if (cg == NULL || cg == fn->g || memcmp(cg->digest, fn->g->digest, 16) == 0) continue;
    if (getenv("QJS_AOT_INLDEDUP") != NULL) {
      for (k = 0; k < n; k++)
        if ((*out)[k].g == cg) break;
      if (k < n) continue;
    }
    if (!inl_callee_ok(cg, in->rop == ROP_call_method)) { if (getenv("QJS_AOT_DEBUG_INL")) fprintf(stderr, "  callee %s not ok: %s\n", cg->name, e_inl_why); continue; }
    if (copied + cg->len > E_INLINE_COPY_BYTES) {
      char fh[33];
      int di;
      for (di = 0; di < 16; di++) snprintf(fh + 2 * di, 3, "%02x", fn->g->digest[di]);
      e_report_copycap(fh, hex[0], copied, cg->len, E_INLINE_COPY_BYTES);
      continue;
    }
    copied += cg->len;
    q = realloc(*out, sizeof(**out) * (size_t)(n + 1));
    if (q == NULL) return n;
    *out = q;
    q[n].g = cg;
    q[n].call_pc = in->pc;
    n++;
  }
  return n;
}

/* ---- objects that never escape ---------------------------------------------
   `new C(a, b)` where C only stores its parameters into fields of `this` makes an object that is its
   arguments: when every use of it is a read of one of those fields, a copy, or a join with another such
   object, the object is never made. See q2_apply_rewrites. */
static int ctor_summary(const JSAotGenFunc *cg, CtorSum *out) {
  Q2Func cf;
  int i, ok = 1, kind[512], nreg;
  memset(out, 0, sizeof(*out));
  if (cg->func_kind != 0 || cg->ctl_count != 0 || cg->light_var_count != 0 || cg->reg_count > 500) return 0;
  if (q2_build(cg, &cf) < 0) {
    q2_free(&cf);
    return 0;
  }
  nreg = cg->reg_count;
  for (i = 0; i < 512; i++) kind[i] = 0;
  out->nparams = cg->arg_count;
  if (cf.nblocks != 1) ok = 0;
  for (i = 0; ok && i < cf.ninsns; i++) {
    const Q2Insn *in = &cf.insns[i];
    int d, s;
    switch (in->rop) {
      case ROP_get_arg:
        d = (int)(in->arg[0].val >> 4);
        if (d < nreg) kind[d] = 2 + (int)in->arg[1].val; else ok = 0;
        break;
      case ROP_get_arg2:
        d = (int)(in->arg[0].val >> 4);
        if (d < nreg) kind[d] = 2 + (int)in->arg[1].val; else ok = 0;
        d = (int)(in->arg[2].val >> 4);
        if (d < nreg) kind[d] = 2 + (int)in->arg[3].val; else ok = 0;
        break;
      case ROP_load_this:
        d = (int)(in->arg[0].val >> 4);
        if (d < nreg) kind[d] = 1; else ok = 0;
        break;
      case ROP_move:
      case ROP_move_s:
      case ROP_move_xfer:
        d = (int)(in->arg[0].val >> 4);
        s = (int)(in->arg[1].val >> 4);
        if (d < nreg && s < nreg) kind[d] = kind[s]; else ok = 0;
        break;
      case ROP_put_field: {
        int recv = (int)(in->arg[0].val >> 4), val = (int)(in->arg[3].val >> 4), j;
        if (recv >= nreg || val >= nreg || kind[recv] != 1 || kind[val] < 2 || out->nfields >= 8) {
          ok = 0;
          break;
        }
        for (j = 0; j < out->nfields; j++)
          if (out->atom[j] == (int)in->arg[1].val) ok = 0;
        {
          int sa, st = -1;
          for (sa = 0; sa < in->narg; sa++)
            if (in->arg[sa].kind == 's') st = (int)in->arg[sa].val;
          if (st < 0 || st > 65535) {
            ok = 0;
            break;
          }
          out->site[out->nfields] = st;
        }
        if (in->arg[1].kind != 'a' || in->arg[1].off < 0) {
          ok = 0;
          break;
        }
        out->atom[out->nfields] = (int)in->arg[1].val;
        out->aoff[out->nfields] = in->arg[1].off;
        out->argidx[out->nfields] = kind[val] - 2;
        out->nfields++;
        break;
      }
      case ROP_return_undefined:
        break;
      default:
        ok = 0;
    }
  }
  q2_free(&cf);
  return ok && out->nfields > 0;
}

/* A construct of a plain constructor the profile saw with one target: the target and what it does. */
static int ctor_candidate(const Q2Func *fn, const Q2Insn *ci, const JSAotGenFunc **cg_out, CtorSum *sum) {
  char hex[4][33];
  int kind[4], cnt = 0;
  unsigned long long hits[4];
  const JSAotGenFunc *cg;
  if (ci->rop != ROP_construct || ci->ndef != 1 || ci->nuse < 2 || ci->win_n > 8) return 0;
  if (ci->use[0] != ci->use[1]) return 0; /* new.target is the constructor */
  if (!qjs_aot_prof_callees(e_dg(fn, ci), ci->onext, &cnt, hex, kind, hits) || cnt != 1 || kind[0] != 1) return 0;
  if (!qjs_aot_unit_has(hex[0])) return 0;
  cg = qjs_aot_unit_func(hex[0]);
  if (cg == NULL || !ctor_summary(cg, sum) || sum->nparams > 8) return 0;
  *cg_out = cg;
  return 1;
}

typedef struct UseRef {
  int insn;  /* the instruction, or -1 for a block-edge use */
  int slot;  /* operand slot / edge index */
  int param; /* for an edge use, the receiving parameter's value */
} UseRef;

static int uf_find(int *p, int x) {
  while (p[x] != x) {
    p[x] = p[p[x]];
    x = p[x];
  }
  return x;
}

static int rw_push(Q2Rewrite **rw, int *nrw, int *caprw, int kind, int pc, int web, int arg) {
  if (*nrw == *caprw) {
    int nc = *caprw ? *caprw * 2 : 16;
    Q2Rewrite *q = realloc(*rw, sizeof(**rw) * (size_t)nc);
    if (q == NULL) return -1;
    *rw = q;
    *caprw = nc;
  }
  (*rw)[*nrw].kind = kind;
  (*rw)[*nrw].pc = pc;
  (*rw)[*nrw].web = web;
  (*rw)[*nrw].arg = arg;
  (*nrw)++;
  return 0;
}

int e_plan_virtual(const Q2Func *fn, Q2VWeb **webs_out, int *nwebs_out, Q2Rewrite **rw_out, int *nrw_out) {
  EFacts facts;
  int nv = fn->nvalues, i, k, c, j, nweb = 0, nrw = 0, caprw = 0, ret = 0, dbg, pass, changed;
  uint8_t *isreal = NULL;
  UseRef **uses = NULL;
  int *nuses = NULL, *capuses = NULL, *uf = NULL, *candof = NULL, *memlist = NULL, *claimed_root = NULL;
  Q2VWeb *webs = NULL;
  Q2Rewrite *rw = NULL;
  CtorSum *sums = NULL;
  const JSAotGenFunc **cgs = NULL;
  int ncand = 0;
  *webs_out = NULL;
  *nwebs_out = 0;
  *rw_out = NULL;
  *nrw_out = 0;
  if (getenv("QJS_AOT_NOVIRT") != NULL || nv <= 0) return 0;
  dbg = getenv("QJS_AOT_DEBUG_VIRT") != NULL;
  memset(&facts, 0, sizeof(facts));
  {
    int saved = e_facts_pure_ok;
    e_facts_pure_ok = 0;
    e_facts_virtual_planning = 1;
    if (e_facts_run(fn, &facts, 1) < 0) {
      e_facts_pure_ok = saved;
      e_facts_virtual_planning = 0;
      return 0;
    }
    e_facts_pure_ok = saved;
  }
  uses = calloc((size_t)nv, sizeof(*uses));
  nuses = calloc((size_t)nv, sizeof(int));
  capuses = calloc((size_t)nv, sizeof(int));
  uf = malloc(sizeof(int) * (size_t)nv);
  candof = malloc(sizeof(int) * (size_t)nv);
  memlist = malloc(sizeof(int) * (size_t)nv);
  claimed_root = calloc((size_t)nv, sizeof(int));
  sums = calloc((size_t)fn->ninsns + 1, sizeof(*sums));
  cgs = calloc((size_t)fn->ninsns + 1, sizeof(*cgs));
  isreal = calloc((size_t)fn->ninsns + 1, 1);
  if (!uses || !nuses || !capuses || !uf || !candof || !memlist || !claimed_root || !sums || !cgs || !isreal) goto done;
  for (i = 0; i < nv; i++) {
    uf[i] = i;
    candof[i] = -1;
  }
#define ADD_USE(val_, ins_, slot_, par_)                                                       \
  do {                                                                                          \
    Q2ValueId vv_ = (val_);                                                                     \
    if (vv_ < (Q2ValueId)nv) {                                                                  \
      if (nuses[vv_] == capuses[vv_]) {                                                         \
        int nc_ = capuses[vv_] ? capuses[vv_] * 2 : 4;                                          \
        UseRef *q_ = realloc(uses[vv_], sizeof(UseRef) * (size_t)nc_);                          \
        if (q_ == NULL) goto done;                                                              \
        uses[vv_] = q_;                                                                         \
        capuses[vv_] = nc_;                                                                     \
      }                                                                                         \
      uses[vv_][nuses[vv_]].insn = (ins_);                                                      \
      uses[vv_][nuses[vv_]].slot = (slot_);                                                     \
      uses[vv_][nuses[vv_]].param = (par_);                                                     \
      nuses[vv_]++;                                                                             \
    }                                                                                           \
  } while (0)
  for (i = 0; i < fn->ninsns; i++) {
    const Q2Insn *in = &fn->insns[i];
    for (k = 0; k < in->nuse; k++) ADD_USE(in->use[k], i, k, -1);
    for (k = 0; k < in->win_n; k++) ADD_USE(in->win_val[k], i, 100 + k, -1);
    for (k = 0; k < in->nredef; k++) ADD_USE(in->redef_val[k], i, 200 + k, -1);
    if ((in->rop == ROP_move || in->rop == ROP_move_s || in->rop == ROP_move_xfer) && in->ndef == 1 && in->nuse >= 1 &&
        in->vk == 0) {
      int a = uf_find(uf, (int)in->use[0]), b = uf_find(uf, (int)in->def[0]);
      if (a != b) uf[a] = b;
    }
  }
  for (i = 0; i < fn->nblocks; i++) {
    const Q2Block *b = &fn->blocks[i];
    int n;
    for (n = 0; n < b->nsucc; n++) {
      Q2Block *sb = b->succ_block[n] >= 0 && b->succ_block[n] < fn->nblocks ? &fn->blocks[b->succ_block[n]] : NULL;
      for (k = 0; k < b->nedge_args[n]; k++) {
        Q2ValueId av = b->edge_arg[n][k];
        if (av == Q2_NO_VALUE) continue;
        ADD_USE(av, -1, n, sb != NULL && k < sb->nparams ? (int)sb->param_val[k] : -1);
        if (sb != NULL && k < sb->nparams) {
          int a = uf_find(uf, (int)av), bb = uf_find(uf, (int)sb->param_val[k]);
          if (a != bb) uf[a] = bb;
        }
      }
    }
  }
  /* the constructs that could be virtual */
  for (c = 0; c < fn->ninsns; c++) {
    const Q2Insn *ci = &fn->insns[c];
    const JSAotGenFunc *cg;
    if (ci->rop != ROP_construct || ci->ndef != 1) continue;
    if (!ctor_candidate(fn, ci, &cg, &sums[ncand])) {
      if (dbg) fprintf(stderr, "virt: construct at pc %d own %d is not a candidate\n", (int)ci->pc, (int)ci->own);
      continue;
    }
    if (dbg) fprintf(stderr, "virt: candidate construct at pc %d own %d\n", (int)ci->pc, (int)ci->own);
    cgs[ncand] = cg;
    candof[ci->def[0]] = ncand;
    ncand++;
  }
  /* A web is judged with the constructs of the other candidate webs taken as not allocating; a
     candidate whose web fails is real from then on, and the webs are judged again until none changes. */
  for (pass = 0; pass < 8; pass++) {
  changed = 0;
  nweb = 0;
  nrw = 0;
  free(webs);
  webs = NULL;
  memset(claimed_root, 0, sizeof(int) * (size_t)nv);
  for (c = 0; c < fn->ninsns; c++) {
    const Q2Insn *ci = &fn->insns[c];
    int root, nmem = 0, ok = 1, first_rw = nrw, nc2 = 0, cands[16], cc0 = -1, m;
    if (ci->rop != ROP_construct || ci->ndef != 1 || candof[ci->def[0]] < 0) continue;
    root = uf_find(uf, (int)ci->def[0]);
    if (claimed_root[root]) continue;
    claimed_root[root] = 1;
    if (isreal[candof[ci->def[0]]]) continue;
    for (m = 0; m < nv; m++)
      if (uf_find(uf, m) == root) memlist[nmem++] = m;
    /* what makes each member: a candidate construct of the one constructor, a copy, or a join */
    for (j = 0; ok && j < nmem; j++) {
      const Q2Value *mv = &fn->values[memlist[j]];
      if (mv->insn < 0) continue; /* a block parameter */
      {
        const Q2Insn *di = &fn->insns[mv->insn];
        if ((di->rop == ROP_move || di->rop == ROP_move_s || di->rop == ROP_move_xfer) && di->vk == 0) continue;
        if (di->rop == ROP_construct && candof[memlist[j]] >= 0) {
          if (cc0 < 0) cc0 = candof[memlist[j]];
          if (cgs[candof[memlist[j]]] != cgs[cc0] || nc2 >= 16) ok = 0;
          else cands[nc2++] = mv->insn;
        } else {
          ok = 0;
          if (dbg) {
            char nm[64];
            e_rop_name(di->rop, nm, sizeof(nm));
            fprintf(stderr, "virt:   member %d defined by %s at pc %d\n", memlist[j], nm, (int)di->pc);
          }
        }
      }
    }
    if (dbg) fprintf(stderr, "virt: web at pc %d: members=%d constructs=%d ok=%d\n", (int)ci->pc, nmem, nc2, ok);
    /* what each member is used for */
    for (j = 0; ok && j < nmem; j++) {
      int x = memlist[j];
      for (k = 0; k < nuses[x] && ok; k++) {
        const UseRef *u = &uses[x][k];
        const Q2Insn *ui;
        if (u->insn < 0) continue; /* an edge into a member join: in the web */
        ui = &fn->insns[u->insn];
        if (ui->rop == ROP_move_xfer) continue; /* (a copy that empties its source) */
        if (ui->vk != 0) {
          ok = 0;
        } else if ((ui->rop == ROP_move || ui->rop == ROP_move_s) && ui->ndef == 1 && u->slot == 0) {
          continue;
        } else if (ui->inlg) {
          continue; /* the guard of a copied callee: the copy reads the object as the receiver or an argument */
        } else if ((ui->rop == ROP_get_field || ui->rop == ROP_get_field_own || ui->rop == ROP_get_field2 ||
                    ui->rop == ROP_get_field2_own) &&
                   ui->nuse >= 1 && u->slot == 0 && ui->ndef == 1 && ui->narg >= 3) {
          int found = -1, site = -1, sa;
          const CtorSum *sm = &sums[cc0];
          for (sa = 0; sa < sm->nfields; sa++)
            if (sm->atom[sa] == (int)ui->arg[2].val) found = sa;
          for (sa = 0; sa < ui->narg; sa++)
            if (ui->arg[sa].kind == 's') site = (int)ui->arg[sa].val;
          if (found >= 0) {
            if (rw_push(&rw, &nrw, &caprw, Q2_RW_GET, ui->pc, nweb, sm->argidx[found]) < 0) goto done;
          } else if (site >= 0 && e_facts_site_group(&facts, site) >= 0 && e_facts_site_proto(&facts, site) == 1) {
            if (rw_push(&rw, &nrw, &caprw, Q2_RW_PROTO, ui->pc, nweb, 0) < 0) goto done;
          } else {
            ok = 0;
          }
        } else {
          ok = 0;
        }
        if (!ok && dbg) {
          char nm[64];
          e_rop_name(ui->rop, nm, sizeof(nm));
          fprintf(stderr, "virt:   use of value %d (reg %d) by %s at pc %d own %d slot %d\n", x, fn->values[x].reg, nm, (int)ui->pc, (int)ui->own, u->slot);
        }
      }
    }
    if (dbg) fprintf(stderr, "virt: web at pc %d: uses ok=%d\n", (int)ci->pc, ok);
    /* no safepoint while a member is live: a register of a member is not live across one */
    for (j = 0; ok && j < nmem; j++) {
      int r = fn->values[memlist[j]].reg;
      if (r < 0 || r >= fn->nregs) continue;
      for (i = 0; ok && i < fn->ninsns; i++) {
        const uint64_t *lo = q2_live_out(fn, i);
        const Q2Insn *si = &fn->insns[i];
        if (!((lo[r >> 6] >> (r & 63)) & 1) || si->inlg || si->rop == ROP_return || si->rop == ROP_return_undefined)
          continue;
        if (si->rop == ROP_construct && candof[si->def[0]] >= 0 && !isreal[candof[si->def[0]]]) continue;
        if (is_safepoint(fn, &facts, si, 1)) {
          if (dbg) {
            char nm[64];
            e_rop_name(si->rop, nm, sizeof(nm));
            fprintf(stderr, "virt: web at pc %d: safepoint %s at pc %d own %d with reg %d live\n", (int)ci->pc, nm, (int)si->pc, (int)si->own, r);
          }
          ok = 0;
        }
      }
    }
    /* the objects of the web share its argument registers: at a construct no other object of the
       web may still be needed */
    for (j = 0; ok && j < nc2; j++) {
      const Q2Insn *cj = &fn->insns[cands[j]];
      int defreg = fn->values[cj->def[0]].reg;
      int endi = cands[j];
      const uint64_t *lo;
      uint8_t over[512];
      memset(over, 0, sizeof(over));
      if (cj->own) {
        /* In a copied callee the registers the call reads stay live to the end of the copy (an exit
           inside it leaves for the call), so the object is judged where the copy ends; nothing after
           the construct in the copy may then leave. */
        int q3;
        for (q3 = cands[j] + 1; q3 < fn->ninsns && fn->insns[q3].own == cj->own; q3++) {
          const Q2Insn *ti = &fn->insns[q3];
          if (ti->rop != ROP_move && ti->rop != ROP_move_s && ti->rop != ROP_load_undefined && ti->rop != ROP_branch) {
            if (dbg) fprintf(stderr, "virt: web at pc %d: the copy leaves after the construct at pc %d\n", (int)ci->pc, (int)cj->pc);
            ok = 0;
          }
          for (m = 0; m < ti->ndef; m++) {
            int dr = fn->values[ti->def[m]].reg;
            if (dr >= 0 && dr < 512) over[dr] = 1;
          }
          endi = q3;
        }
      }
      lo = q2_live_out(fn, endi);
      for (m = 0; ok && m < nmem; m++) {
        int r = fn->values[memlist[m]].reg;
        if (memlist[m] == (int)cj->def[0] || r < 0 || r >= fn->nregs || r == defreg) continue;
        if (r < 512 && over[r]) continue;
        if ((lo[r >> 6] >> (r & 63)) & 1) {
          if (dbg) fprintf(stderr, "virt: web at pc %d: another object (value %d reg %d, defreg %d) is live after the construct at pc %d (end insn pc %d)\n", (int)ci->pc, memlist[m], r, defreg, (int)cj->pc, (int)fn->insns[endi].pc);
          ok = 0;
        }
      }
    }
    if (ok) {
      int regs = 0, w;
      for (w = 0; w < nweb; w++) regs += webs[w].nargs + 1;
      if (nweb >= 8 || regs + sums[cc0].nparams + 1 > 20) ok = 0;
    }
    if (!ok) {
      nrw = first_rw;
      for (m = 0; m < nv; m++)
        if (candof[m] >= 0 && uf_find(uf, m) == root && !isreal[candof[m]]) {
          isreal[candof[m]] = 1;
          changed = 1;
        }
      continue;
    }
    {
      Q2VWeb *q = realloc(webs, sizeof(*q) * (size_t)(nweb + 1));
      const CtorSum *sm = &sums[cc0];
      if (q == NULL) goto done;
      webs = q;
      memset(&webs[nweb], 0, sizeof(webs[nweb]));
      webs[nweb].ctor = cgs[cc0];
      webs[nweb].nargs = sm->nparams;
      for (j = 0; j < sm->nfields; j++) {
        webs[nweb].atom[j] = sm->atom[j];
        webs[nweb].aoff[j] = sm->aoff[j];
        webs[nweb].argidx[j] = sm->argidx[j];
        webs[nweb].site[j] = sm->site[j];
      }
      webs[nweb].nfields = sm->nfields;
      for (j = 0; j < nc2; j++)
        if (rw_push(&rw, &nrw, &caprw, Q2_RW_CONSTRUCT, fn->insns[cands[j]].pc, nweb, 0) < 0) goto done;
      nweb++;
    }
  }
  if (!changed) break;
  }
  *webs_out = webs;
  *nwebs_out = nweb;
  *rw_out = rw;
  *nrw_out = nrw;
  ret = nweb;
  webs = NULL;
  rw = NULL;
done:
  free(webs);
  free(rw);
  if (uses != NULL)
    for (i = 0; i < nv; i++) free(uses[i]);
  free(uses);
  free(nuses);
  free(capuses);
  free(uf);
  free(candof);
  free(memlist);
  free(claimed_root);
  free(sums);
  free(cgs);
  free(isreal);
  e_facts_virtual_planning = 0;
  e_facts_free(&facts);
  return ret;
}
