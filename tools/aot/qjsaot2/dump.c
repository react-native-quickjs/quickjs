/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * The SSA dump behind `qjsaot2 --dump-ir`. It is the golden file for the
 * graph build: a change to the lowering, to the decoder or to the builder
 * shows up as a diff, and the layout is stable (pc order, one line per
 * instruction, values named by index).
 */
#include <stdio.h>

#include "ir.h"

static void dump_value(FILE *f, const Q2Func *fn, Q2ValueId v) {
  if (v == Q2_NO_VALUE) {
    fputs("dead", f);
    return;
  }
  fprintf(
      f, "v%u(r%d:%s%s%s%s)", v, fn->values[v].reg,
      q2_rep_name(fn->values[v].rep), fn->values[v].spec ? ",spec" : "",
      fn->values[v].synced ? ",sync" : "", fn->values[v].ovf ? ",ovf" : "");
}

void q2_dump(FILE *f, const Q2Func *fn) {
  int b, i, k;

  fprintf(
      f, "F %s blocks=%d insns=%d values=%d regs=%d ctl_max=%d\n",
      fn->g->name[0] ? fn->g->name : "<anon>", fn->nblocks, fn->ninsns,
      fn->nvalues, fn->nregs, fn->max_ctl);
  fprintf(f, "  digest=");
  for (i = 0; i < 16; i++) fprintf(f, "%02x", fn->g->digest[i]);
  fputc('\n', f);
  for (b = 0; b < fn->nblocks; b++) {
    const Q2Block *blk = &fn->blocks[b];
    fprintf(
        f, "B%d pc=%d..%d ctl=%d params=", b, blk->start_pc, blk->end_pc,
        blk->ctl_in);
    for (k = 0; k < blk->nparams; k++) {
      fprintf(
          f, "%sv%u=r%d", k ? "," : "", blk->param_val[k], blk->param_reg[k]);
    }
    if (blk->nparams == 0) fputc('-', f);
    fprintf(f, " succ=");
    for (k = 0; k < blk->nsucc; k++) {
      int j;
      fprintf(f, "%s%d(", k ? "," : "", blk->succ_pc[k]);
      for (j = 0; j < blk->nedge_args[k]; j++)
        fprintf(f, "%sv%u", j ? "," : "", blk->edge_arg[k][j]);
      fputc(')', f);
    }
    if (blk->nsucc == 0) fputc('-', f);
    fputc('\n', f);
    for (i = 0; i < blk->ninsns; i++) {
      const Q2Insn *in = &fn->insns[blk->first_insn + i];
      fprintf(
          f, "  %5d %-16s rop=%-4u def=", in->pc, q2_op_name(in->op), in->rop);
        /* The SUB field: a branch's compare kind, an op's sub-operation. Without
           it a `branch_cmp`'s sense is invisible, which cost a wrong theory. */
        if (in->narg > 0 && in->arg[0].kind == 'b')
          fprintf(f, "sub=%d ", (int)in->arg[0].val);
      if (in->ndef == 0) fputc('-', f);
      for (k = 0; k < in->ndef; k++)
        fprintf(
            f, "%s%u(%s)", k ? "," : "", in->def[k],
            q2_rep_name(fn->values[in->def[k]].rep));
      fputs(" use=", f);
      if (in->nuse == 0) fputc('-', f);
      for (k = 0; k < in->nuse; k++) {
        fprintf(f, "%s", k ? "," : "");
        dump_value(f, fn, in->use[k]);
      }
      fputc('\n', f);
    }
  }
}
