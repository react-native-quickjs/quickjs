/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * The IR verifier: the checks the design puts in the compiler because a
 * speculative compiler that gets them wrong corrupts quietly.
 *
 *   - SSA dominance: every use is dominated by its definition (a value used
 *     in a block is either defined there before the use, or is a parameter of
 *     that block);
 *   - block parameters match predecessors: every edge passes exactly one
 *     value per successor parameter, for the same register, defined in the
 *     edge's source block;
 *   - effects: a pure instruction is not a fallback (a fallback may do
 *     anything, so it must declare Heap/Control);
 *   - representations: every value's representation is one the IR knows.
 *
 * The frame-state check lands with frame states (2.10).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir.h"
#include "quickjs-regvm.h"

#define Q2_VERIFY_FAIL(...)                  \
  do {                                       \
    snprintf(v.msg, v.msg_len, __VA_ARGS__); \
    free(v.dom);                             \
    return -1;                               \
  } while (0)

typedef struct Q2Verify {
  char *msg;
  size_t msg_len;
  const Q2Func *f;
  uint64_t *dom; /* nblocks * bwords */
} Q2Verify;

static int q2_vbits_words(int nblocks) {
  return (nblocks + 63) / 64;
}

/* Reverse postorder over the block order (blocks are ordered by pc, which is
   already a topological order for everything except back edges). */
static int q2_dominators(Q2Verify *v) {
  const Q2Func *f = v->f;
  int nb = f->nblocks, words = q2_vbits_words(nb);
  uint64_t *tmp = malloc(sizeof(uint64_t) * (size_t)words);
  int i, changed = 1, pass = 0;

  if (!tmp) return -1;
  v->dom = malloc(sizeof(uint64_t) * (size_t)words * (size_t)nb);
  if (!v->dom) {
    free(tmp);
    return -1;
  }
  memset(v->dom, 0xff, (size_t)words * (size_t)nb * 8);
  memset(v->dom, 0, (size_t)words * 8); /* block 0 dominates only itself */
  v->dom[0] |= 1ull;
  while (changed && pass++ < nb + 4) {
    changed = 0;
    for (i = 1; i < nb; i++) {
      uint64_t *d = v->dom + (size_t)i * words;
      int b, first = 1, w;
      for (w = 0; w < words; w++) tmp[w] = ~0ull;
      for (b = 0; b < nb; b++) {
        int s, has_edge = 0;
        if (b == i) continue;
        for (s = 0; s < f->blocks[b].nsucc; s++)
          if (f->blocks[b].succ_block[s] == i) has_edge = 1;
        if (!has_edge) continue;
        if (first) {
          memcpy(tmp, v->dom + (size_t)b * words, (size_t)words * 8);
          first = 0;
        } else {
          for (w = 0; w < words; w++) tmp[w] &= v->dom[(size_t)b * words + w];
        }
      }
      if (first) { /* unreachable block: only itself */
        for (w = 0; w < words; w++) tmp[w] = 0;
      }
      tmp[i >> 6] |= 1ull << (i & 63);
      {
        int same = 1;
        for (w = 0; w < words; w++)
          if (tmp[w] != d[w]) same = 0;
        if (!same) {
          memcpy(d, tmp, (size_t)words * 8);
          changed = 1;
        }
      }
    }
  }
  free(tmp);
  return 0;
}

static int q2_block_dominates(const Q2Verify *v, int dom_block, int of_block) {
  int words = q2_vbits_words(v->f->nblocks);
  return (int)((v->dom[(size_t)of_block * words + (dom_block >> 6)] >>
                (dom_block & 63)) &
               1);
}

/* The value a block's parameter index stands for, or -1. */
static int q2_param_index(const Q2Block *b, Q2ValueId val) {
  int i;
  for (i = 0; i < b->nparams; i++)
    if (b->param_val[i] == val) return i;
  return -1;
}

/* A typed binary's operands must all be typed: a tagged input would need a
   guard this step does not emit, so the lowering falls back instead. The fused
   family has two halves with different operand sets, so check each half. */
static char q2_dbg_why[128];
static int q2_in_types_ok(const Q2Func *f, const Q2Insn *in) {
  int d, k;
  int fused =
      (in->rop == ROP_mul_add || in->rop == ROP_mul_sub ||
       in->rop == ROP_add_mul || in->rop == ROP_sub_mul);
  for (d = 0; d < in->ndef; d++) {
    if (f->values[in->def[d]].rep == Q2_R_TAGGED) continue;
    snprintf(
        q2_dbg_why, sizeof(q2_dbg_why), "def v%u is %s", in->def[d],
        q2_rep_name(f->values[in->def[d]].rep));
    if (fused) {
      if (d == 0) {
        if (in->nuse < 2) return 0;
        if (f->values[in->use[0]].rep == Q2_R_TAGGED) return 0;
        if (f->values[in->use[1]].rep == Q2_R_TAGGED) return 0;
      } else {
        if (f->values[in->def[0]].rep == Q2_R_TAGGED) return 0;
        if (in->nuse < 3) return 0;
        if (f->values[in->use[2]].rep == Q2_R_TAGGED) return 0;
      }
    } else {
      for (k = 0; k < in->nuse; k++)
        if (f->values[in->use[k]].rep == Q2_R_TAGGED) return 0;
    }
  }
  return 1;
}

int q2_verify(const Q2Func *f, char *msg, size_t msg_len) {
  Q2Verify v;
  int b, i, k;

  memset(&v, 0, sizeof(v));
  v.msg = msg;
  v.msg_len = msg_len;
  v.f = f;
  msg[0] = 0;

  if (!f->ok) Q2_VERIFY_FAIL("build failed: %s", f->why ? f->why : "?");
  if (q2_dominators(&v)) {
    v.dom = NULL;
    snprintf(msg, msg_len, "out of memory");
    return -1;
  }

  for (b = 0; b < f->nblocks; b++) {
    const Q2Block *blk = &f->blocks[b];
    for (i = 0; i < blk->ninsns; i++) {
      const Q2Insn *in = &f->insns[blk->first_insn + i];
      const Q2RopInfo *ri = q2_rop_info(in->rop);

      /* A natively lowered operation must be classified, and a classified one
         must not need the step for its operands. */
      if (q2_lowered(in->rop) && in->op == Q2_OP_FALLBACK)
        Q2_VERIFY_FAIL(
            "block %d pc %d: rop %u is lowered but classified as a fallback", b,
            in->pc, in->rop);
      if (in->op == Q2_OP_FALLBACK) {
        /* A fallback with known roles runs through the step for its effects;
           one with no roles at all must still declare what it may touch. */
        if (ri->ndef == 0 && ri->nuse == 0 && ri->reads == 0 && ri->writes == 0)
          Q2_VERIFY_FAIL(
              "block %d pc %d: a fallback declares no effects", b, in->pc);
      } else if (in->op != ri->op) {
        Q2_VERIFY_FAIL(
            "block %d pc %d: operation %s does not match the table's %s", b,
            in->pc, q2_op_name(in->op), q2_op_name(ri->op));
      }

      /* The role table must agree with the decoded operands. After the build,
         `def[]`/`use[]` hold value ids, so the check is by register: every
         register operand of a classified operation is named (a def, a use or
         the call window base). A hand-written table that drifts here used to
         hide as a silent frame read in the lowering. */
      if (in->op != Q2_OP_FALLBACK) {
        int m;
        for (m = 0; m < in->narg; m++) {
          int named = 0, r;
          if (in->arg[m].kind != 'r') continue;
          r = (int)in->arg[m].val >> 4;
          if (in->win_n > 0 && r >= in->win_reg && r < in->win_reg + in->win_n)
            named = 1;
          /* the window base register is the frame address the callee reads
             from, even when the argument count is zero */
          if (ri->window >= 1 && m == ri->window - 1) named = 1;
          for (k = 0; !named && k < in->ndef; k++) {
            Q2ValueId val = in->def[k];
            if (val >= (Q2ValueId)f->nvalues)
              Q2_VERIFY_FAIL("block %d pc %d: def out of range", b, in->pc);
            if (f->values[val].reg == r) named = 1;
          }
          for (k = 0; !named && k < in->nuse; k++) {
            Q2ValueId val = in->use[k];
            if (val >= (Q2ValueId)f->nvalues)
              Q2_VERIFY_FAIL("block %d pc %d: use out of range", b, in->pc);
            if (f->values[val].reg == r) named = 1;
          }
          if (!named)
            Q2_VERIFY_FAIL(
                "block %d pc %d rop=%u: register operand %d (r%d) is not a "
                "def, "
                "a use or the window",
                b, in->pc, in->rop, m, r);
        }
      }
      if (ri->window >= 0) {
        if (ri->window >= in->narg || ri->window < 1 ||
            in->arg[ri->window].kind == 'r' ||
            in->arg[ri->window - 1].kind != 'r')
          Q2_VERIFY_FAIL(
              "block %d pc %d: window operand %d is not a count after a "
              "register",
              b, in->pc, ri->window);
      }

      for (k = 0; k < in->ndef; k++) {
        Q2ValueId val = in->def[k];
        if (val >= (Q2ValueId)f->nvalues)
          Q2_VERIFY_FAIL("block %d pc %d: definition out of range", b, in->pc);
        if (f->values[val].block != b ||
            f->values[val].insn != blk->first_insn + i)
          Q2_VERIFY_FAIL(
              "block %d pc %d: definition not owned here", b, in->pc);
        if (f->values[val].rep >= Q2_R_COUNT)
          Q2_VERIFY_FAIL("block %d pc %d: bad representation", b, in->pc);
        /* A speculated load must have a record: speculation without a profile
           is exactly what this step forbids. */
        if (f->values[val].spec && !f->values[val].has_prof)
          Q2_VERIFY_FAIL(
              "block %d pc %d: a speculated load has no profile record", b,
              in->pc);
        /* A typed value must be producible by its instruction. */
        if (f->values[val].rep != Q2_R_TAGGED) {
          int ok = 0;
          if (in->op == Q2_OP_CONST || in->op == Q2_OP_MOVE ||
              in->op == Q2_OP_BINARY || in->op == Q2_OP_COMPARE ||
              in->op == Q2_OP_GET_FIELD || in->op == Q2_OP_GET_ELEMENT ||
              in->op == Q2_OP_GET_VAR_REF || in->op == Q2_OP_GET_ARG ||
              in->op == Q2_OP_GET_ARG2 || in->op == Q2_OP_GET_LOCAL ||
              (in->op == Q2_OP_BRANCH_CMP &&
               (in->rop == ROP_inc_branch_lt ||
                in->rop == ROP_inc_branch_lt_s)) ||
              (in->op == Q2_OP_BRANCH &&
               (in->rop == ROP_add_imm_self_branch ||
                in->rop == ROP_add_imm_self_branch_s)) ||
              q2_def_from_record(in))
            ok = 1;
          if (!ok)
            Q2_VERIFY_FAIL(
                "block %d pc %d: a typed result from an untyped operation", b,
                in->pc);
        }
      }
      /* A natively typed operation may not consume a tagged operand. */
      if (in->op == Q2_OP_BINARY && !q2_in_types_ok(f, in))
        Q2_VERIFY_FAIL(
            "block %d pc %d: typed binary with a tagged input (%s)", b, in->pc,
            q2_dbg_why);
      for (k = 0; k < in->nuse; k++) {
        Q2ValueId val = in->use[k];
        const Q2Value *pv;
        if (val >= (Q2ValueId)f->nvalues)
          Q2_VERIFY_FAIL("block %d pc %d: use out of range", b, in->pc);
        pv = &f->values[val];
        /* A use is dominated by its definition: either a parameter of this
           block, or a definition in a block that dominates this one. */
        if (pv->block == b && q2_param_index(blk, val) >= 0) continue;
        if (!q2_block_dominates(&v, pv->block, b))
          Q2_VERIFY_FAIL(
              "block %d pc %d: use of a value from block %d, which does not "
              "dominate",
              b, in->pc, pv->block);
      }
    }
    /* edges */
    for (i = 0; i < blk->nsucc; i++) {
      int sb = blk->succ_block[i];
      const Q2Block *s;
      if (sb >= f->nblocks) continue; /* the exit */
      s = &f->blocks[sb];
      if (blk->nedge_args[i] != s->nparams)
        Q2_VERIFY_FAIL(
            "edge %d->%d: %d arguments for %d parameters", b, sb,
            blk->nedge_args[i], s->nparams);
      for (k = 0; k < s->nparams; k++) {
        Q2ValueId val = blk->edge_arg[i][k];
        if (val >= (Q2ValueId)f->nvalues)
          Q2_VERIFY_FAIL("edge %d->%d: argument out of range", b, sb);
        if (f->values[val].reg != s->param_reg[k])
          Q2_VERIFY_FAIL(
              "edge %d->%d: argument %d is register %d, parameter wants %d", b,
              sb, k, f->values[val].reg, s->param_reg[k]);
        /* The representation must convert: I32 -> F64 and typed -> tagged (by
           boxing) are fine; tagged -> typed is not. */
        if (f->values[s->param_val[k]].rep != Q2_R_TAGGED &&
            f->values[val].rep == Q2_R_TAGGED)
          Q2_VERIFY_FAIL(
              "edge %d->%d: argument %d is tagged, parameter wants %s", b, sb,
              k, q2_rep_name(f->values[s->param_val[k]].rep));
        if (f->values[val].rep != Q2_R_TAGGED) {
          int pr = f->values[s->param_val[k]].rep;
          if (pr == Q2_R_TAGGED) {
            /* boxing is fine */
          } else if (pr == Q2_R_I32 && f->values[val].rep != Q2_R_I32) {
            Q2_VERIFY_FAIL(
                "edge %d->%d: argument %d is %s, parameter wants i32", b, sb, k,
                q2_rep_name(f->values[val].rep));
          } else if (pr == Q2_R_F64 && f->values[val].rep == Q2_R_BOOL) {
            Q2_VERIFY_FAIL(
                "edge %d->%d: argument %d is bool, parameter wants f64", b, sb,
                k);
          } else if (pr == Q2_R_BOOL && f->values[val].rep != Q2_R_BOOL) {
            Q2_VERIFY_FAIL("edge %d->%d: argument %d is not bool", b, sb, k);
          }
        }
        /* The argument is either defined in the source block, one of its
           parameters, or a value whose definition dominates the source (which
           is what trivial-parameter removal produces). */
        if (f->values[val].block != b && q2_param_index(blk, val) < 0 &&
            !q2_block_dominates(&v, f->values[val].block, b))
          Q2_VERIFY_FAIL(
              "edge %d->%d: argument %d does not dominate the source block", b,
              sb, k);
      }
    }
  }
  free(v.dom);
  return 0;
}
