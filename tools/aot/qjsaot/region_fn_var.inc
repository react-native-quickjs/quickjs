/* region.c fragment at file scope, before region_emit1: helpers of closure variables (M4, empty) and the unboxed number representation (M3, C2).

   A region value is typed RT_I32 (an int32_t local `rN_iM`), RT_F64 (a double local `rN_dM`) or RT_ANY (a JSValue local `rN_vM`, as before). A typed value keeps its old name as a
   MACRO, the boxed view (`#define rN_vM (...)`): every consumer this file does not convert (calls, exit stubs, generic ops, returns) reads it unchanged and gets the number in
   the normalised form (an int when it fits, -0 and NaN as doubles). A definition that is not converted would assign to a macro and not compile (the safety net), so only the
   whitelisted producers below are ever typed.

   Types come from rg_ty_decide, a monotone fixpoint over all instances (it runs between the two walks, next to rg_sr_decide). Loads are typed from their profile record and
   guarded at the definition (an I32 site that meets anything but an int, an F64 site that meets a non-number, leaves before the instruction); arithmetic follows its operands; a
   phi is the join of its edges (I32 and F64 join to F64, anything with an ANY is ANY). Knobs: QJS_AOT_REGION_NOF64 (everything ANY, today's code), QJS_AOT_REGION_F64DBG (dump). */
enum { RT_BOT = 0, RT_I32 = 1, RT_F64 = 2, RT_ANY = 3 };

static int rg_f64_on(void) {
  static int v = -1;
  if (v < 0) v = rg_trust_on() && getenv("QJS_AOT_REGION_NOF64") == NULL;
  return v;
}
/* a vkit knock-out: the generator names it (the unit then carries the wrong code), the run twin QJS_AOT_REGION_F64KNOCK switches the wrong code on */
static int rg_f64_knob(const char *nm) { return getenv(nm) != NULL; }
#define RG_KN(n) "getenv(\"QJS_AOT_REGION_F64KNOCK_" n "\")"   /* the run twin of a knock-out: QJS_AOT_REGION_F64KNOCK_<n> */

static int rg_tyof(const RInst *I, int v) {
  if (I == NULL || I->ty == NULL || v < 0 || v >= I->q.nvalues) return RT_ANY;
  return I->ty[v];
}
static int rg_ty_num(const RInst *I, int v) { int t = rg_tyof(I, v); return t == RT_I32 || t == RT_F64; }
static int rg_ty_join(int a, int b) {
  if (a == RT_BOT) return b;
  if (b == RT_BOT) return a;
  if (a == b) return a;
  if (a == RT_ANY || b == RT_ANY) return RT_ANY;
  return RT_F64;
}

/* the index of a constant path already in the region's table (rg_cp_add without the add), else -1 */
static int rg_cp_find(const RCtx *c, char (*names)[40], int n) {
  int q, j;
  for (q = 0; q < c->ncp; q++) {
    if (c->cpn[q] != n) continue;
    for (j = 0; j < n && strcmp(c->cp[q][j], names[j]) == 0; j++) {}
    if (j == n) return q;
  }
  return -1;
}

/* the instance and value an argument text (`rN_vM`) names, or -1 */
static int rg_ty_parse(const RCtx *c, const char *t, int *inst, int *val) {
  int n, m;
  char tail;
  if (sscanf(t, "r%d_v%d%c", &n, &m, &tail) != 2 || n < 0 || n >= c->nin_total || c->in[n] == NULL || m < 0 || m >= c->in[n]->q.nvalues) return 0;
  *inst = n; *val = m;
  return 1;
}

/* a vkit test seed (QJS_AOT_REGION_F64SEED=i32|f64): every load whose record has a number in it is typed so, whatever else the record holds (the profile of a vkit case is the case's
   own run, so a site that meets a double or a string later is otherwise typed from it); with i32 every record is also taken as int-only (the sites leave on overflow and -0) */
static int rg_seed(void) {
  static int v = -1;
  if (v < 0) { const char *e = getenv("QJS_AOT_REGION_F64SEED"); v = e == NULL ? 0 : strcmp(e, "i32") == 0 ? 1 : strcmp(e, "f64") == 0 ? 2 : 0; }
  return v;
}
static int rg_rec_int_only(const Q2Value *v) { return v->has_prof && (v->ptypes == Q2_T_INT32 || (rg_seed() == 1 && (v->ptypes & (Q2_T_INT32 | Q2_T_NUM)))); }
static int rg_rec_load_ty(const Q2Value *v) {
  if (!v->has_prof) return RT_ANY;
  if (rg_seed() && (v->ptypes & (Q2_T_INT32 | Q2_T_NUM))) return rg_seed() == 1 ? RT_I32 : RT_F64;
  if (v->ptypes == Q2_T_INT32) return RT_I32;
  if (v->ptypes == Q2_T_NUM || v->ptypes == (Q2_T_INT32 | Q2_T_NUM)) return RT_F64;
  return RT_ANY;
}

/* which defs of an instruction are loads (a JSValue the existing code makes, converted and guarded after it): bit 0 = def 0, bit 1 = def 1; 0 for any other instruction. */
static int rg_ty_loaddefs(const RInst *I, const Q2Insn *in) {
  switch (in->rop) {
    case ROP_get_field: case ROP_get_field_own: case ROP_get_field2: case ROP_get_field2_own: case ROP_get_element: case ROP_get_length: case ROP_get_global: return 1;
    case ROP_get_this_field: case ROP_get_this_field2: return 1;
    case ROP_get_field_chain: case ROP_get_field_chain_own: return 2;
    case ROP_get_arg: return I->id == 0 ? 1 : 0;
    case ROP_get_arg2: return I->id == 0 ? 3 : 0;
    case ROP_call_method: {
      int callee = in->nuse >= 1 ? rg_base(&I->q, (Q2ValueId)in->use[0]) : -1, id_, mg_;
      return callee >= 0 && callee < I->q.nvalues && I->mname != NULL && I->mname[callee][0] && rg_math_site(g_rgm_c, (RInst *)I, in, I->mname[callee], &id_, &mg_) ? 1 : 0;
    }
    default: return 0;
  }
}

/* the immediate of an add_imm / binary_imm form */
static int rg_imm_of(const Q2Insn *in, int32_t *k) {
  int ai, found = 0;
  for (ai = 0; ai < in->narg; ai++) if (in->arg[ai].kind == 'i' || in->arg[ai].kind == 'I') { *k = (int32_t)in->arg[ai].val; found = 1; }
  return found;
}

/* The shape of a typed arithmetic instruction: 0 for any other. kind 1 = a binary of two values (sub in *sub), 2 = a binary with an int immediate, 3 = with a pool constant, 4 = fused
   (mul then add/sub), 5 = unary (neg 'n', inc 'i', dec 'd', identity 'p') */
typedef struct { int kind, sub; int32_t k; double kd; int ktag; char un; } RArith;
static int rg_arith_shape(const RInst *I, const Q2Insn *in, RArith *a) {
  int sub;
  memset(a, 0, sizeof(*a));
  switch (in->rop) {
    case ROP_add: a->kind = 1; a->sub = REG_BIN_ADD; return 1;
    case ROP_sub: a->kind = 1; a->sub = REG_BIN_SUB; return 1;
    case ROP_mul: a->kind = 1; a->sub = REG_BIN_MUL; return 1;
    case ROP_div: a->kind = 1; a->sub = REG_BIN_DIV; return 1;
    case ROP_mul_add: case ROP_mul_sub: case ROP_add_mul: case ROP_sub_mul: a->kind = 4; return in->nuse >= 3;
    case ROP_add_imm_s: case ROP_add_imm_self_s: a->kind = 2; a->sub = REG_BIN_ADD; return rg_imm_of(in, &a->k);
    case ROP_sub_imm_s: case ROP_sub_imm_self_s: a->kind = 2; a->sub = REG_BIN_SUB; return rg_imm_of(in, &a->k);
    case ROP_binary_imm_s: case ROP_binary_imm:
      if (in->narg < 4 || in->arg[1].kind != 'b') return 0;
      sub = (int)in->arg[1].val;
      if (sub != REG_BIN_ADD && sub != REG_BIN_SUB && sub != REG_BIN_MUL && sub != REG_BIN_DIV) return 0;
      a->kind = 2; a->sub = sub;
      if (sub == REG_BIN_MUL || sub == REG_BIN_DIV) { a->k = (int32_t)in->arg[3].val; return 1; }
      return rg_imm_of(in, &a->k);
    case ROP_binary_cpool: {
      double pn = 0;
      uint32_t pidx;
      if (in->narg < 5) return 0;
      sub = (int)in->arg[1].val & 0x3f;
      pidx = (uint32_t)in->arg[4].val;
      if (!(sub == REG_BIN_ADD || sub == REG_BIN_SUB || sub == REG_BIN_MUL || sub == REG_BIN_DIV)) return 0;
      if (in->arg[3].val != REG_CONST_CPOOL || !qjs_aot_pool_num(I->g, pidx, &pn)) return 0;
      a->kind = 3; a->sub = sub; a->kd = pn; a->ktag = qjs_aot_pool_tag(I->g, pidx);
      if (a->ktag == 1) a->k = (int32_t)pn;
      return a->ktag != 0;
    }
    case ROP_neg: a->kind = 5; a->un = 'n'; return 1;
    case ROP_inc: a->kind = 5; a->un = 'i'; return 1;
    case ROP_dec: a->kind = 5; a->un = 'd'; return 1;
    case ROP_to_numeric: a->kind = 5; a->un = 'p'; return 1;
    case ROP_unary:
      sub = in->narg > 1 ? (int)in->arg[1].val : -1;
      a->kind = 5;
      if (sub == REG_UN_NEG) a->un = 'n'; else if (sub == REG_UN_INC) a->un = 'i'; else if (sub == REG_UN_DEC) a->un = 'd';
      else if (sub == REG_UN_PLUS || sub == REG_UN_TO_NUMERIC) a->un = 'p'; else return 0;
      return 1;
    default: return 0;
  }
}

/* the result type of `x sub y` for operand types ta, tb (RT_BOT while an operand is not decided yet); int_only: the result's record is int-only */
static int rg_ty_bin(int sub, int ta, int tb, int int_only) {
  if (ta == RT_ANY || tb == RT_ANY) return RT_ANY;
  if (ta == RT_BOT || tb == RT_BOT) return RT_BOT;
  if (sub == REG_BIN_DIV) return RT_F64;
  if (ta == RT_I32 && tb == RT_I32) return int_only ? RT_I32 : RT_F64;
  return RT_F64;
}
static int rg_ty_un(int un, int ta, int int_only, int has_rec) {
  if (ta == RT_ANY) return RT_ANY;
  if (ta == RT_BOT) return RT_BOT;
  if (un == 'p') return ta;
  if (ta == RT_F64) return RT_F64;
  /* an int: neg gives -0 for 0, inc/dec leave int32 at the ends; a record that saw only ints keeps the int (the site leaves otherwise), else a double */
  if (has_rec) return int_only ? RT_I32 : RT_F64;
  return RT_I32;
}

/* The result of an inlined callee: the join of what its returns return (RT_ANY for a return of nothing, a callee with no return, an out-of-line arm). */
static int rg_inst_rty(const RInst *J) {
  int k, t = RT_BOT, n = 0;
  if (J == NULL || J->ty == NULL || J->ool) return RT_ANY;
  for (k = 0; k < J->q.ninsns; k++) {
    const Q2Insn *in = &J->q.insns[k];
    if (in->rop == ROP_return_undefined) return RT_ANY;
    if (in->rop == ROP_return) { n++; if (in->nuse < 1) return RT_ANY; t = rg_ty_join(t, rg_tyof(J, (int)in->use[0])); }
  }
  return n ? t : RT_ANY;
}
/* The result type of a call (value dv of instance ii) over its expanded arms; RT_ANY with none or when any arm is out of line, a construct or untyped. */
static int rg_arms_ty(const RCtx *c, int ii, int dv) {
  int j, n = 0, t = RT_BOT;
  for (j = 1; j < c->nin_total; j++) {
    const RInst *J = c->in[j];
    if (J == NULL || J->par != ii || J->par_dv != dv) continue;
    if (J->par_kind != 0 || J->ool || J->ty == NULL) return RT_ANY;
    n++;
    t = rg_ty_join(t, rg_inst_rty(J));
  }
  return n ? t : RT_ANY;
}
/* structural: the call has expanded arms, all of them plain calls inlined in the region */
static int rg_arms_ok(const RCtx *c, int ii, int dv) {
  int j, n = 0;
  for (j = 1; j < c->nin_total; j++) {
    const RInst *J = c->in[j];
    if (J == NULL || J->par != ii || J->par_dv != dv) continue;
    if (J->par_kind != 0 || J->ool || J->g == NULL) return 0;
    n++;
  }
  return n > 0;
}
static int rg_is_call_op(const Q2Insn *in) { return in->rop == ROP_call || in->rop == ROP_call_method; }
/* The type of def d of instruction `in` of instance I from the types so far (I->ty): RT_ANY when the producer is not whitelisted. */
static int rg_ty_transfer(RCtx *c, RInst *I, const Q2Insn *in, int d) {
  const Q2Func *fn = &I->q;
  const Q2Value *rec = in->ndef > 0 ? &fn->values[in->def[0]] : NULL;
  RArith ar;
  int u0 = in->nuse >= 1 ? (int)in->use[0] : -1, u1 = in->nuse >= 2 ? (int)in->use[1] : -1, ld = rg_ty_loaddefs(I, in);
  if (ld & (1 << d)) {
    if (in->rop == ROP_get_length) return RT_I32;
    return rg_rec_load_ty(&fn->values[in->def[d]]);
  }
  if (rg_is_call_op(in)) return d == 0 && in->ndef >= 1 ? rg_arms_ty(c, I->id, (int)in->def[0]) : RT_ANY;
  switch (in->rop) {
    case ROP_load_i8: case ROP_load_i8_s: case ROP_load_i32: return d == 0 && in->narg >= 2 ? RT_I32 : RT_ANY;
    case ROP_load_const: {
      double pn = 0;
      if (d != 0 || in->narg < 3 || in->arg[2].kind != 'c' || in->arg[1].val != REG_CONST_CPOOL || !qjs_aot_pool_num(I->g, (uint32_t)in->arg[2].val, &pn)) return RT_ANY;
      return qjs_aot_pool_tag(I->g, (uint32_t)in->arg[2].val) == 1 ? RT_I32 : qjs_aot_pool_tag(I->g, (uint32_t)in->arg[2].val) != 0 ? RT_F64 : RT_ANY;
    }
    case ROP_move: case ROP_move_s: case ROP_move_xfer: return d == 0 && u0 >= 0 ? rg_tyof(I, u0) : RT_ANY;
    case ROP_get_arg: case ROP_get_arg2: {
      /* an inlined callee's argument is the parent's value (its type) */
      int s0, pi, pv;
      if (I->id == 0) return RT_ANY;
      s0 = d == 0 ? (in->narg > 1 ? (int)in->arg[1].val : 0) : (in->narg > 3 ? (int)in->arg[3].val : -1);
      if (s0 < 0 || s0 >= I->nargs || !rg_ty_parse(c, I->arg[s0], &pi, &pv)) return RT_ANY;
      return rg_tyof(c->in[pi], pv);
    }
    default: break;
  }
  if (rg_arith_shape(I, in, &ar)) {
    int int_only = rec != NULL && rg_rec_int_only(rec);
    switch (ar.kind) {
      case 1: if (d != 0 || u0 < 0 || u1 < 0) return RT_ANY; return rg_ty_bin(ar.sub, rg_tyof(I, u0), rg_tyof(I, u1), int_only);
      case 2: if (d != 0 || u0 < 0) return RT_ANY; return rg_ty_bin(ar.sub, rg_tyof(I, u0), RT_I32, int_only);
      case 3: if (d != 0 || u0 < 0) return RT_ANY; return rg_ty_bin(ar.sub, rg_tyof(I, u0), ar.ktag == 1 ? RT_I32 : RT_F64, int_only);
      case 4: {
        /* dm = x * y; da = c +/- dm (add_mul, sub_mul) or dm +/- c (mul_add, mul_sub); one record for both */
        int tm = rg_ty_bin(REG_BIN_MUL, rg_tyof(I, u0), rg_tyof(I, u1), int_only), u2 = (int)in->use[2];
        if (in->ndef < 2) return RT_ANY;
        if (d == 0) return tm;
        return rg_ty_bin(in->rop == ROP_mul_add || in->rop == ROP_add_mul ? REG_BIN_ADD : REG_BIN_SUB, tm, rg_tyof(I, u2), int_only);
      }
      case 5: if (d != 0 || u0 < 0) return RT_ANY; return rg_ty_un(ar.un, rg_tyof(I, u0), int_only, rec != NULL && rec->has_prof);
    }
  }
  if ((in->rop == ROP_inc_branch_lt || in->rop == ROP_inc_branch_lt_s) && d == 0 && in->ndef >= 1 && u0 >= 0) {
    int t = rg_tyof(I, u0);
    return t == RT_ANY ? RT_ANY : t == RT_BOT ? RT_BOT : t;
  }
  return RT_ANY;
}

/* Whether def d of `in` can ever be typed (the producers above); the others are ANY from the start. */
static int rg_ty_cand(const RInst *I, const Q2Insn *in, int d) {
  RArith ar;
  if (rg_ty_loaddefs(I, in) & (1 << d)) return 1;
  if (rg_is_call_op(in)) return d == 0 && in->ndef >= 1 && g_rgm_c != NULL && !getenv("QJS_AOT_REGION_NOF64CALL") && rg_arms_ok(g_rgm_c, I->id, (int)in->def[0]);
  switch (in->rop) {
    case ROP_load_i8: case ROP_load_i8_s: case ROP_load_i32: case ROP_load_const: case ROP_move: case ROP_move_s: case ROP_move_xfer: return d == 0;
    case ROP_get_arg: case ROP_get_arg2: return I->id != 0;
    case ROP_inc_branch_lt: case ROP_inc_branch_lt_s: return d == 0;
    default: break;
  }
  if (rg_arith_shape(I, in, &ar)) return ar.kind == 4 ? d < 2 : d == 0;
  return 0;
}

static void rg_ty_dump(RCtx *c) {
  int i, v;
  for (i = 0; i < c->nin_total; i++) {
    RInst *I = c->in[i];
    int nf = 0, ni = 0, na = 0;
    if (I == NULL || I->ty == NULL) continue;
    for (v = 0; v < I->q.nvalues; v++) { if (I->ty[v] == RT_F64) nf++; else if (I->ty[v] == RT_I32) ni++; else na++; }
    fprintf(stderr, "f64: root %.8s inst %d (%s)%s: %d f64, %d i32, %d any\n", c->hex, i, I->g && I->g->name[0] ? I->g->name : "<anon>", I->ool ? " [ool]" : "", nf, ni, na);
    for (v = 0; v < I->q.nvalues; v++) {
      char nm[48] = "param";
      int di = I->q.values[v].insn;
      if (di >= 0 && di < I->q.ninsns) e_rop_name(I->q.insns[di].rop, nm, sizeof(nm));
      fprintf(stderr, "f64:   r%d_v%d %s %s%s", i, v, I->ty[v] == RT_F64 ? "F64" : I->ty[v] == RT_I32 ? "I32" : "any", nm, I->rw && I->rw[v] ? " raw" : "");
      if (I->ty[v] == RT_ANY && di >= 0 && di < I->q.ninsns) {
        /* why: the operands of an arithmetic result that stayed ANY, by their producers */
        const Q2Insn *in = &I->q.insns[di];
        RArith ar;
        if (rg_arith_shape(I, in, &ar)) {
          int u;
          fprintf(stderr, " {ndef %d nuse %d nredef %d}", in->ndef, in->nuse, in->nredef);
          for (u = 0; u < in->nuse && u < 3; u++) {
            char on[48] = "param";
            int uv = (int)in->use[u], ud = uv >= 0 && uv < I->q.nvalues ? I->q.values[uv].insn : -1;
            if (ud >= 0 && ud < I->q.ninsns) e_rop_name(I->q.insns[ud].rop, on, sizeof(on));
            fprintf(stderr, " [%s:%s", on, rg_tyof(I, uv) == RT_ANY ? "any" : rg_tyof(I, uv) == RT_F64 ? "F64" : "I32");
            if (ud < 0 && uv >= 0 && uv < I->q.nvalues) fprintf(stderr, " v%d blk%d po%d", uv, I->q.values[uv].block, I->q.values[uv].param_of);
            fprintf(stderr, "]");
          }
        }
      }
      if (I->ty[v] == RT_ANY && di < 0) {
        int bb, jj;
        for (bb = 0; bb < I->q.nblocks; bb++) for (jj = 0; jj < I->q.blocks[bb].nparams; jj++) if ((int)I->q.blocks[bb].param_val[jj] == v) fprintf(stderr, " blk%d.p%d(npred %d)", bb, jj, I->q.blocks[bb].npreds);
        /* a block parameter or function argument: the producers of its incoming edge values */
        int b, s, j;
        for (b = 0; b < I->q.nblocks; b++) {
          const Q2Block *B = &I->q.blocks[b];
          for (s = 0; s < B->nsucc; s++) {
            const Q2Block *T = &I->q.blocks[B->succ_block[s]];
            for (j = 0; j < T->nparams && j < B->nedge_args[s]; j++) if ((int)T->param_val[j] == v) {
              char on[48] = "param";
              int av = (int)B->edge_arg[s][j], ad = av >= 0 && av < I->q.nvalues ? I->q.values[av].insn : -1;
              if (ad >= 0 && ad < I->q.ninsns) e_rop_name(I->q.insns[ad].rop, on, sizeof(on));
              fprintf(stderr, " <%s:%s>", on, rg_tyof(I, av) == RT_ANY ? "any" : rg_tyof(I, av) == RT_F64 ? "F64" : "I32");
            }
          }
        }
      }
      fprintf(stderr, "\n");
    }
  }
}

/* The typing pass: fills I->ty / I->rw of every instance that is walked. */
static void rg_ty_decide(RCtx *c) {
  int i, pass, changed;
  if (!rg_f64_on()) return;
  for (i = 0; i < c->nin_total; i++) {
    RInst *I = c->in[i];
    const Q2Func *fn;
    int k, d, b;
    if (I == NULL || I->ool || I->g == NULL) continue;
    fn = &I->q;
    I->ty = malloc((size_t)(fn->nvalues > 0 ? fn->nvalues : 1));
    I->rw = calloc((size_t)(fn->nvalues > 0 ? fn->nvalues : 1), 1);
    if (I->ty == NULL || I->rw == NULL) abort();
    memset(I->ty, RT_ANY, (size_t)(fn->nvalues > 0 ? fn->nvalues : 1));
    for (k = 0; k < fn->ninsns; k++) {
      const Q2Insn *in = &fn->insns[k];
      for (d = 0; d < in->ndef && d < 2; d++) if ((int32_t)in->def[d] >= 0 && (int32_t)in->def[d] < fn->nvalues && rg_ty_cand(I, in, d)) I->ty[in->def[d]] = RT_BOT;
      /* a register a fallback redefines is a plain JSValue */
      for (d = 0; d < in->nredef; d++) if (in->redef_val[d] < (Q2ValueId)fn->nvalues) I->ty[in->redef_val[d]] = RT_ANY;
    }
    /* block parameters: candidates, except the entry block's and a block nothing branches to */
    for (b = 1; b < fn->nblocks; b++) {
      const Q2Block *B = &fn->blocks[b];
      if (fn->rpo_pos != NULL && fn->rpo_pos[b] < 0) continue;
      if (B->npreds == 0) continue;
      for (k = 0; k < B->nparams; k++) if ((int32_t)B->param_val[k] >= 0 && (int32_t)B->param_val[k] < fn->nvalues) I->ty[B->param_val[k]] = RT_BOT;
    }
  }
  for (pass = 0; pass < 64; pass++) {
    changed = 0;
    for (i = 0; i < c->nin_total; i++) {
      RInst *I = c->in[i];
      const Q2Func *fn;
      int k, d, b, s, j;
      if (I == NULL || I->ty == NULL) continue;
      fn = &I->q;
      for (k = 0; k < fn->ninsns; k++) {
        const Q2Insn *in = &fn->insns[k];
        for (d = 0; d < in->ndef && d < 2; d++) {
          int v = (int)in->def[d], nt;
          if (v < 0 || v >= fn->nvalues || I->ty[v] == RT_ANY || !rg_ty_cand(I, in, d)) continue;
          nt = rg_ty_join(I->ty[v], rg_ty_transfer(c, I, in, d));
          if (nt != I->ty[v]) { if (nt == RT_ANY && getenv("QJS_AOT_REGION_F64TRACE")) fprintf(stderr, "f64trace: pass %d r%d_v%d insn %d becomes ANY (was %d)\n", pass, i, v, k, I->ty[v]); I->ty[v] = (uint8_t)nt; changed = 1; }
        }
      }
      for (b = 0; b < fn->nblocks; b++) {
        const Q2Block *B = &fn->blocks[b];
        if (fn->rpo_pos != NULL && fn->rpo_pos[b] < 0) continue;
        for (s = 0; s < B->nsucc; s++) {
          const Q2Block *T = &fn->blocks[B->succ_block[s]];
          for (j = 0; j < T->nparams; j++) {
            int pv = (int)T->param_val[j], nt;
            if (pv < 0 || pv >= fn->nvalues || I->ty[pv] == RT_ANY) continue;
            nt = j < B->nedge_args[s] ? rg_ty_join(I->ty[pv], rg_tyof(I, (int)B->edge_arg[s][j])) : RT_ANY;
            if (nt != I->ty[pv]) { if (nt == RT_ANY && getenv("QJS_AOT_REGION_F64TRACE")) fprintf(stderr, "f64trace: pass %d phi r%d_v%d becomes ANY via edge arg %d (j=%d nedge %d)\n", pass, i, pv, j < B->nedge_args[s] ? (int)B->edge_arg[s][j] : -1, j, B->nedge_args[s]); I->ty[pv] = (uint8_t)nt; changed = 1; }
          }
        }
      }
    }
    if (!changed) break;
  }
  if (pass == 64) {
    /* not settled (it cannot happen: the lattice is finite and the steps ascend); be safe */
    for (i = 0; i < c->nin_total; i++) if (c->in[i] != NULL && c->in[i]->ty != NULL) memset(c->in[i]->ty, RT_ANY, (size_t)(c->in[i]->q.nvalues > 0 ? c->in[i]->q.nvalues : 1));
  }
  /* what is still undecided is unreachable or unconstrained: a plain JSValue */
  for (i = 0; i < c->nin_total; i++) {
    RInst *I = c->in[i];
    int v;
    if (I == NULL || I->ty == NULL) continue;
    for (v = 0; v < I->q.nvalues; v++) if (I->ty[v] == RT_BOT) I->ty[v] = RT_ANY;
  }
  /* an operand an instruction decided to type must be typed itself (a pass that ended on ANY operands): drop a typed result whose operands are not numeric, until stable */
  do {
    changed = 0;
    for (i = 0; i < c->nin_total; i++) {
      RInst *I = c->in[i];
      const Q2Func *fn;
      int k, d, b, s, j;
      if (I == NULL || I->ty == NULL) continue;
      fn = &I->q;
      for (k = 0; k < fn->ninsns; k++) {
        const Q2Insn *in = &fn->insns[k];
        RArith ar;
        /* a typed move / argument / call result whose source ended up untyped (a call: whose arms are) */
        for (d = 0; d < in->ndef && d < 2; d++) {
          int v = (int)in->def[d];
          if (v < 0 || v >= fn->nvalues || I->ty[v] == RT_ANY || !(rg_is_call_op(in) || in->rop == ROP_move || in->rop == ROP_move_s || in->rop == ROP_move_xfer || in->rop == ROP_get_arg || in->rop == ROP_get_arg2)) continue;
          if (rg_ty_transfer(c, I, in, d) == RT_ANY) { I->ty[v] = RT_ANY; changed = 1; }
        }
        if (!rg_arith_shape(I, in, &ar)) continue;
        for (d = 0; d < in->ndef && d < 2; d++) {
          int v = (int)in->def[d], u;
          if (v < 0 || v >= fn->nvalues || I->ty[v] == RT_ANY) continue;
          for (u = 0; u < (ar.kind == 4 ? 3 : ar.kind == 1 ? 2 : 1); u++)
            if (u < in->nuse && !rg_ty_num(I, (int)in->use[u])) { I->ty[v] = RT_ANY; changed = 1; break; }
        }
      }
      for (b = 0; b < fn->nblocks; b++) {
        const Q2Block *B = &fn->blocks[b];
        if (fn->rpo_pos != NULL && fn->rpo_pos[b] < 0) continue;
        for (s = 0; s < B->nsucc; s++) {
          const Q2Block *T = &fn->blocks[B->succ_block[s]];
          for (j = 0; j < T->nparams; j++) {
            int pv = (int)T->param_val[j];
            if (pv < 0 || pv >= fn->nvalues || I->ty[pv] == RT_ANY) continue;
            if (j >= B->nedge_args[s] || !rg_ty_num(I, (int)B->edge_arg[s][j])) { I->ty[pv] = RT_ANY; changed = 1; }
          }
        }
      }
    }
  } while (changed);
  for (i = 0; i < c->nin_total; i++) if (c->in[i] != NULL && c->in[i]->ty != NULL) { int t = i == 0 ? RT_ANY : rg_inst_rty(c->in[i]); c->in[i]->rty = (uint8_t)(t == RT_BOT ? RT_ANY : t); }
  /* the raw flag: the value is the result of a double operation of + - * neg inc dec (the engine does not normalise it) or a move / phi of such values only */
  for (i = 0; i < c->nin_total; i++) {
    RInst *I = c->in[i];
    const Q2Func *fn;
    int k;
    if (I == NULL || I->ty == NULL) continue;
    fn = &I->q;
    for (k = 0; k < fn->ninsns; k++) {
      const Q2Insn *in = &fn->insns[k];
      RArith ar;
      int d;
      if (!rg_arith_shape(I, in, &ar)) continue;
      for (d = 0; d < in->ndef && d < 2; d++) {
        int v = (int)in->def[d], u, anyd = 0;
        if (v < 0 || v >= fn->nvalues || I->ty[v] != RT_F64) continue;
        if (ar.kind != 5 && ar.sub == REG_BIN_DIV) continue;   /* the engine's division normalises */
        if (ar.kind == 3 && ar.ktag != 1) anyd = 1;
        if (ar.kind == 4 && d == 1 && I->ty[in->def[0]] == RT_F64) anyd = 1;
        for (u = 0; u < (ar.kind == 4 ? 3 : ar.kind == 1 ? 2 : 1) && u < in->nuse; u++) if (rg_tyof(I, (int)in->use[u]) == RT_F64) anyd = 1;
        if (ar.kind == 5 && ar.un == 'p') continue;
        if (anyd) I->rw[v] = 1;
      }
    }
  }
  do {
    changed = 0;
    for (i = 0; i < c->nin_total; i++) {
      RInst *I = c->in[i];
      const Q2Func *fn;
      int k, b, s, j;
      if (I == NULL || I->ty == NULL) continue;
      fn = &I->q;
      for (k = 0; k < fn->ninsns; k++) {
        const Q2Insn *in = &fn->insns[k];
        if ((in->rop == ROP_move || in->rop == ROP_move_s || in->rop == ROP_move_xfer) && in->nuse >= 1 && in->ndef >= 1 && I->ty[in->def[0]] == RT_F64) {
          uint8_t nr = I->ty[in->use[0]] == RT_F64 ? I->rw[in->use[0]] : 0;
          if (nr != I->rw[in->def[0]]) { I->rw[in->def[0]] = nr; changed = 1; }
        }
      }
      for (b = 0; b < fn->nblocks; b++) {
        const Q2Block *B = &fn->blocks[b];
        if (fn->rpo_pos != NULL && fn->rpo_pos[b] < 0) continue;
        for (s = 0; s < B->nsucc; s++) {
          const Q2Block *T = &fn->blocks[B->succ_block[s]];
          for (j = 0; j < T->nparams && j < B->nedge_args[s]; j++) {
            int pv = (int)T->param_val[j];
            if (pv < 0 || pv >= fn->nvalues || I->ty[pv] != RT_F64) continue;
            /* a phi is raw only if every edge brings a raw value: start from raw, clear on a non-raw edge */
          }
        }
      }
    }
  } while (changed);
  /* phis: raw iff every incoming is a raw F64 (computed by clearing from an optimistic start) */
  for (i = 0; i < c->nin_total; i++) {
    RInst *I = c->in[i];
    int b, k;
    if (I == NULL || I->ty == NULL) continue;
    for (b = 1; b < I->q.nblocks; b++) {
      const Q2Block *B = &I->q.blocks[b];
      for (k = 0; k < B->nparams; k++) if (I->ty[B->param_val[k]] == RT_F64) I->rw[B->param_val[k]] = 1;
    }
  }
  do {
    changed = 0;
    for (i = 0; i < c->nin_total; i++) {
      RInst *I = c->in[i];
      const Q2Func *fn;
      int k, b, s, j;
      if (I == NULL || I->ty == NULL) continue;
      fn = &I->q;
      for (k = 0; k < fn->ninsns; k++) {
        const Q2Insn *in = &fn->insns[k];
        if ((in->rop == ROP_move || in->rop == ROP_move_s || in->rop == ROP_move_xfer) && in->nuse >= 1 && in->ndef >= 1 && I->ty[in->def[0]] == RT_F64) {
          uint8_t nr = I->ty[in->use[0]] == RT_F64 ? I->rw[in->use[0]] : 0;
          if (nr != I->rw[in->def[0]]) { I->rw[in->def[0]] = nr; changed = 1; }
        }
      }
      for (b = 0; b < fn->nblocks; b++) {
        const Q2Block *B = &fn->blocks[b];
        if (fn->rpo_pos != NULL && fn->rpo_pos[b] < 0) continue;
        for (s = 0; s < B->nsucc; s++) {
          const Q2Block *T = &fn->blocks[B->succ_block[s]];
          for (j = 0; j < T->nparams && j < B->nedge_args[s]; j++) {
            int pv = (int)T->param_val[j], av = (int)B->edge_arg[s][j];
            if (pv < 0 || pv >= fn->nvalues || I->ty[pv] != RT_F64 || !I->rw[pv]) continue;
            if (I->ty[av] != RT_F64 || !I->rw[av]) { I->rw[pv] = 0; changed = 1; }
          }
        }
      }
    }
  } while (changed);
  if (getenv("QJS_AOT_REGION_F64DBG")) rg_ty_dump(c);
}

/* ---- emission ------------------------------------------------------------------------------------------------------------------------------------------------ */

/* the macro of a typed value's boxed view (the normalised number) */
static void rg_ty_macro(FILE *o, const RInst *I, int v) {
  int t = rg_tyof(I, v);
  if (t == RT_F64 && rg_f64_knob("QJS_AOT_REGION_F64BOXINT"))
    /* the vkit knock-out: the boxed view of a double is an int cast when the run sets the twin (no -0, no fraction) */
    fprintf(o, "#define %sv%d (" RG_KN("BOXINT") " ? JS_MKVAL(JS_TAG_INT, (int32_t)%sd%d) : qjs_aot_box_num(%sd%d, qjs_aot_fitsi(%sd%d)))\n", I->pfx, v, I->pfx, v, I->pfx, v, I->pfx, v);
  else if (t == RT_F64) fprintf(o, "#define %sv%d (qjs_aot_box_num(%sd%d, qjs_aot_fitsi(%sd%d)))\n", I->pfx, v, I->pfx, v, I->pfx, v);
  else if (t == RT_I32) fprintf(o, "#define %sv%d (JS_MKVAL(JS_TAG_INT, %si%d))\n", I->pfx, v, I->pfx, v);
}
/* A value declared as the JSValue it is, or as its typed local plus the boxed-view macro. */
static void rg_ty_decl(const RCtx *c, FILE *o, const RInst *I, int v) {
  int t = rg_tyof(I, v);
  (void)c;
  if (t == RT_F64) { fprintf(o, "    double %sd%d;\n", I->pfx, v); rg_ty_macro(o, I, v); }
  else if (t == RT_I32) { fprintf(o, "    int32_t %si%d;\n", I->pfx, v); rg_ty_macro(o, I, v); }
  else fprintf(o, "    JSValue %sv%d;\n", I->pfx, v);
}
static void rg_ty_undefs(const RCtx *c, FILE *o) {
  int i, v;
  for (i = 0; i < c->nin_total; i++) {
    const RInst *I = c->in[i];
    for (v = 0; I != NULL && v < I->q.nvalues; v++) if (rg_tyof(I, v) != RT_ANY) fprintf(o, "#undef %sv%d\n", I->pfx, v);
    if (I != NULL && rg_rty_typed(I)) fprintf(o, "#undef %sres\n", I->pfx);
  }
}
/* the initialisation of a value an exit stub names (`rN_vM = JS_UNDEFINED` for an untyped one) */
static void rg_ty_init(FILE *o, const RInst *I, int v) {
  int t = rg_tyof(I, v);
  if (t == RT_F64) fprintf(o, "        %sd%d = 0.0;\n", I->pfx, v);
  else if (t == RT_I32) fprintf(o, "        %si%d = 0;\n", I->pfx, v);
  else fprintf(o, "        %sv%d = JS_UNDEFINED;\n", I->pfx, v);
}

/* an operand: its type and C expression in that type */
typedef struct { int ty; char s[48]; } ROpd;
static void rg_opnd(const RInst *I, int v, ROpd *od) {
  od->ty = rg_tyof(I, v);
  snprintf(od->s, sizeof(od->s), od->ty == RT_I32 ? "%si%d" : "%sd%d", I->pfx, v);
}
/* the double value of a numeric operand */
static const char *rg_dx(const ROpd *od, char *buf, size_t cap) {
  if (od->ty == RT_I32) snprintf(buf, cap, "(double)%s", od->s); else snprintf(buf, cap, "%s", od->s);
  return buf;
}
static void rg_kopnd(int ktag, int32_t k, double kd, ROpd *od) {
  if (ktag == 1) { od->ty = RT_I32; snprintf(od->s, sizeof(od->s), "%d", (int)k); }
  else {
    od->ty = RT_F64;
    if (kd != kd) snprintf(od->s, sizeof(od->s), "__builtin_nan(\"\")");
    else if (kd == __builtin_inf() || kd == -__builtin_inf()) snprintf(od->s, sizeof(od->s), "(%s__builtin_inf())", kd < 0 ? "-" : "");
    else snprintf(od->s, sizeof(od->s), "%a", kd);
  }
}

static int rg_rty_typed(const RInst *I) { return I->rty == RT_I32 || I->rty == RT_F64; }
/* an instance's result is a typed local `rN_res_d` / `rN_res_i` with the boxed view `rN_res` */
static void rg_res_decl(FILE *o, const RInst *I) {
  if (I->rty == RT_F64) fprintf(o, "        double %sres_d = 0.0; (void)%sres_d;\n#define %sres (qjs_aot_box_num(%sres_d, qjs_aot_fitsi(%sres_d)))\n", I->pfx, I->pfx, I->pfx, I->pfx, I->pfx);
  else fprintf(o, "        int32_t %sres_i = 0; (void)%sres_i;\n#define %sres (JS_MKVAL(JS_TAG_INT, %sres_i))\n", I->pfx, I->pfx, I->pfx, I->pfx);
}
static void rg_res_set(FILE *o, const RInst *I, int u0) {
  ROpd od;
  char b[64];
  rg_opnd(I, u0, &od);
  if (I->rty == RT_F64 && rg_f64_knob("QJS_AOT_REGION_F64CALLKNOCK"))
    /* the vkit knock-out: when the run sets the twin the returned double is truncated to an int on its way to the caller */
    fprintf(o, "        %sres_d = " RG_KN("CALLRES") " ? (double)(int32_t)(%s) : %s; goto %sdone;\n", I->pfx, rg_dx(&od, b, sizeof(b)), rg_dx(&od, b, sizeof(b)), I->pfx);
  else if (I->rty == RT_F64) fprintf(o, "        %sres_d = %s; goto %sdone;\n", I->pfx, rg_dx(&od, b, sizeof(b)), I->pfx);
  else fprintf(o, "        %sres_i = %s; goto %sdone;\n", I->pfx, od.s, I->pfx);
}
/* the parent's call result dv takes the arm's result J: typed when dv is, else the boxed view (resvar, when the caller names it) */
static void rg_res_get(FILE *o, const RInst *P, int dv, const char *resvar, const RInst *J) {
  int t = rg_tyof(P, dv);
  if (t == RT_F64 && J->rty == RT_I32) fprintf(o, "        %sd%d = (double)%sres_i;\n", P->pfx, dv, J->pfx);
  else if (t == RT_F64 && J->rty == RT_F64) fprintf(o, "        %sd%d = %sres_d;\n", P->pfx, dv, J->pfx);
  else if (t == RT_I32 && J->rty == RT_I32) fprintf(o, "        %si%d = %sres_i;\n", P->pfx, dv, J->pfx);
  else if (resvar != NULL) fprintf(o, "        %s = %sres;\n", resvar, J->pfx);
  else fprintf(o, "        %sv%d = %sres;\n", P->pfx, dv, J->pfx);
}

/* dst = a <sub> b, with the result type rty (I32: the site leaves on overflow and on a -0; F64: plain C double) */
static void rg_emit_bin(FILE *o, int sub, int rty, const char *dst, const ROpd *a, const ROpd *b, const char *xl) {
  const char *opc = sub == REG_BIN_ADD ? "+" : sub == REG_BIN_SUB ? "-" : sub == REG_BIN_MUL ? "*" : "/";
  char xs[64], ys[64];
  if (rty == RT_I32) {
    if (sub == REG_BIN_MUL)
      fprintf(o, "        { int64_t r_ = (int64_t)%s * (int64_t)%s; if (__builtin_expect(r_ < INT32_MIN || r_ > INT32_MAX || (r_ == 0 && (%s < 0 || %s < 0)), 0)) goto %s; %s = (int32_t)r_; }\n", a->s, b->s, a->s, b->s, xl, dst);
    else
      fprintf(o, "        { int32_t r_; if (__builtin_expect(__builtin_%s_overflow(%s, %s, &r_), 0)) goto %s; %s = r_; }\n", sub == REG_BIN_ADD ? "add" : "sub", a->s, b->s, xl, dst);
    return;
  }
  fprintf(o, "        %s = %s %s %s;\n", dst, rg_dx(a, xs, sizeof(xs)), opc, rg_dx(b, ys, sizeof(ys)));
}
static void rg_emit_un(FILE *o, int un, int rty, const char *dst, const ROpd *a, const char *xl) {
  char xs[64];
  if (un == 'p') {
    if (a->ty == RT_I32 && rty == RT_F64) fprintf(o, "        %s = (double)%s;\n", dst, a->s); else fprintf(o, "        %s = %s;\n", dst, a->s);
    return;
  }
  if (rty == RT_I32) {
    if (un == 'n') fprintf(o, "        if (__builtin_expect(%s == 0 || %s == INT32_MIN, 0)) goto %s; %s = -%s;\n", a->s, a->s, xl, dst, a->s);
    else fprintf(o, "        { int32_t r_; if (__builtin_expect(__builtin_%s_overflow(%s, 1, &r_), 0)) goto %s; %s = r_; }\n", un == 'i' ? "add" : "sub", a->s, xl, dst);
    return;
  }
  if (un == 'n') fprintf(o, "        %s = -%s;\n", dst, rg_dx(a, xs, sizeof(xs)));
  else fprintf(o, "        %s = %s %c 1;\n", dst, rg_dx(a, xs, sizeof(xs)), un == 'i' ? '+' : '-');
}

/* dst (a typed local of type dt) = the value of operand `a` converted: I32 -> F64 exact, same type a copy */
static void rg_emit_conv(FILE *o, const char *ind, int dt, const char *dst, const ROpd *a) {
  if (dt == RT_F64 && a->ty == RT_I32) {
    if (rg_f64_knob("QJS_AOT_REGION_F64NOPHICONV")) fprintf(o, "%sif (%s) { int64_t b_ = %s; __builtin_memcpy(&%s, &b_, 8); } else %s = (double)%s;\n", ind, RG_KN("NOPHICONV"), a->s, dst, dst, a->s);
    else fprintf(o, "%s%s = (double)%s;\n", ind, dst, a->s);
  } else fprintf(o, "%s%s = %s;\n", ind, dst, a->s);
}

/* the guard and conversion of a loaded JSValue `src` into the typed local of value v */
static void rg_emit_load_conv(FILE *o, const RInst *I, int v, const char *src, const char *xl) {
  int t = rg_tyof(I, v);
  char ib[200], other[120];
  if (t == RT_I32) {
    if (rg_f64_knob("QJS_AOT_REGION_F64NOGUARD")) fprintf(o, "        if (__builtin_expect(JS_VALUE_GET_TAG(%s) != JS_TAG_INT, 0)) { if (!%s) goto %s; }\n        %si%d = JS_VALUE_GET_INT(%s);\n", src, RG_KN("NOGUARD"), xl, I->pfx, v, src);
    else fprintf(o, "        if (__builtin_expect(JS_VALUE_GET_TAG(%s) != JS_TAG_INT, 0)) goto %s;\n        %si%d = JS_VALUE_GET_INT(%s);\n", src, xl, I->pfx, v, src);
    return;
  }
  /* an int tag converts exactly; a double is its payload; anything else leaves (the knock-outs: the int read as a payload, no guard at all) */
  if (rg_f64_knob("QJS_AOT_REGION_F64NOCONV")) snprintf(ib, sizeof(ib), "(%s ? JS_VALUE_GET_FLOAT64(%s) : (double)JS_VALUE_GET_INT(%s))", RG_KN("NOCONV"), src, src);
  else snprintf(ib, sizeof(ib), "(double)JS_VALUE_GET_INT(%s)", src);
  if (rg_f64_knob("QJS_AOT_REGION_F64NOGUARD")) snprintf(other, sizeof(other), "{ if (!%s) goto %s; %sd%d = JS_VALUE_GET_FLOAT64(%s); }", RG_KN("NOGUARD"), xl, I->pfx, v, src);
  else snprintf(other, sizeof(other), "goto %s;", xl);
  fprintf(o, "        { int tg_ = JS_VALUE_GET_TAG(%s); if (tg_ == JS_TAG_FLOAT64) %sd%d = JS_VALUE_GET_FLOAT64(%s); else if (__builtin_expect(tg_ == JS_TAG_INT, 1)) %sd%d = %s; else %s }\n",
          src, I->pfx, v, src, I->pfx, v, ib, other);
}

/* Before an instruction that loads a typed value: the old code is left to write a scratch JSValue (the value's name is redirected to it), converted and guarded after.
   Returns the mask of defs wrapped. */
static int rg_ty_wrap_begin(RCtx *c, RInst *I, const Q2Insn *in, FILE *o) {
  int ld = rg_ty_loaddefs(I, in), d, mask = 0;
  (void)c;
  if (ld == 0 || I->ty == NULL) return 0;
  for (d = 0; d < 2 && d < in->ndef; d++) {
    if (!(ld & (1 << d)) || !rg_ty_num(I, (int)in->def[d])) continue;
    fprintf(o, "#undef %sv%d\n#define %sv%d ldv%d_\n", I->pfx, (int)in->def[d], I->pfx, (int)in->def[d], d);
    mask |= 1 << d;
  }
  if (mask) {
    fprintf(o, "        {\n");
    for (d = 0; d < 2; d++) if (mask & (1 << d)) fprintf(o, "        JSValue ldv%d_ = JS_UNDEFINED;\n", d);
  }
  return mask;
}
static void rg_ty_wrap_end(RCtx *c, RInst *I, const Q2Insn *in, FILE *o, int mask, const char *xl) {
  int d;
  (void)c;
  if (!mask) return;
  for (d = 0; d < 2; d++) if (mask & (1 << d)) { char sn[16]; snprintf(sn, sizeof(sn), "ldv%d_", d); rg_emit_load_conv(o, I, (int)in->def[d], sn, xl); }
  fprintf(o, "        }\n");
  for (d = 0; d < 2; d++) {
    if (!(mask & (1 << d))) continue;
    fprintf(o, "#undef %sv%d\n", I->pfx, (int)in->def[d]);
    rg_ty_macro(o, I, (int)in->def[d]);
  }
}

/* A compare of two numeric operands as a C condition: ints as ints, otherwise doubles (NaN compares false, -0 == 0). */
static void rg_cmp_expr(const ROpd *a, const ROpd *b, const char *cmp, char *buf, size_t cap) {
  char xs[64], ys[64];
  if (a->ty == RT_I32 && b->ty == RT_I32) snprintf(buf, cap, "(%s %s %s)", a->s, cmp, b->s);
  else snprintf(buf, cap, "(%s %s %s)", rg_dx(a, xs, sizeof(xs)), cmp, rg_dx(b, ys, sizeof(ys)));
}

/* The instructions whose typed form is emitted here instead of by the old cases (called before the instruction's switch, both passes): returns 1 when it handled the instruction.
   A typed result is made by a whitelisted producer; a numeric compare of two typed operands needs no tag test. */
static int rg_ty_op(RCtx *c, RInst *I, const Q2Insn *in, int dv, int dv2, int u0, int u1, const char *xl) {
  FILE *o = c->o;
  RArith ar;
  char dn[48], dn2[48];
  if (I->ty == NULL) return 0;
  (void)dv2;
  if (dv >= 0 && rg_ty_num(I, dv)) snprintf(dn, sizeof(dn), rg_tyof(I, dv) == RT_I32 ? "%si%d" : "%sd%d", I->pfx, dv);
  else dn[0] = 0;
  switch (in->rop) {
    case ROP_load_i8: case ROP_load_i8_s: case ROP_load_i32:
      if (!dn[0] || in->narg < 2) return 0;
      if (c->emit) fprintf(o, "        %s = %d;\n", dn, (int)in->arg[1].val);
      return 1;
    case ROP_load_const: {
      double pn = 0;
      if (!dn[0] || in->narg < 3 || in->arg[2].kind != 'c' || in->arg[1].val != REG_CONST_CPOOL || !qjs_aot_pool_num(I->g, (uint32_t)in->arg[2].val, &pn)) return 0;
      if (c->emit) {
        ROpd k;
        rg_kopnd(qjs_aot_pool_tag(I->g, (uint32_t)in->arg[2].val), (int32_t)pn, pn, &k);
        fprintf(o, "        %s = %s;\n", dn, k.s);
      }
      return 1;
    }
    case ROP_move: case ROP_move_s: case ROP_move_xfer: {
      ROpd a;
      if (!dn[0] || u0 < 0) return 0;
      rg_opnd(I, u0, &a);
      if (c->emit) rg_emit_conv(o, "        ", rg_tyof(I, dv), dn, &a);
      return 1;
    }
    case ROP_get_arg: case ROP_get_arg2: {
      int d;
      if (I->id == 0 || (!dn[0] && !(in->rop == ROP_get_arg2 && dv2 >= 0 && rg_ty_num(I, dv2)))) return 0;
      /* an inlined callee's arguments: the parent's typed values, converted to the callee's own type; an untyped one is the old code's */
      for (d = 0; d < 2; d++) {
        int v = d == 0 ? dv : dv2, s0 = d == 0 ? (in->narg > 1 ? (int)in->arg[1].val : 0) : (in->narg > 3 ? (int)in->arg[3].val : -1), pi, pv;
        if (v < 0 || (d == 1 && in->rop != ROP_get_arg2)) continue;
        if (rg_ty_num(I, v)) {
          ROpd a;
          if (s0 < 0 || s0 >= I->nargs || !rg_ty_parse(c, I->arg[s0], &pi, &pv)) return 0;
          rg_opnd(c->in[pi], pv, &a);
          snprintf(dn2, sizeof(dn2), rg_tyof(I, v) == RT_I32 ? "%si%d" : "%sd%d", I->pfx, v);
          if (c->emit) rg_emit_conv(o, "        ", rg_tyof(I, v), dn2, &a);
        } else if (c->emit) {
          if (d == 0 || in->rop == ROP_get_arg2) fprintf(o, "        %sv%d = %s;\n", I->pfx, v, s0 >= 0 && s0 < I->nargs ? I->arg[s0] : "JS_UNDEFINED");
        }
      }
      return 1;
    }
    case ROP_get_global: case ROP_get_field: case ROP_get_field_own: {
      /* a typed constant path in a loop (G.F, or a global written once): its guard and conversion are made once at the region's entry (a value that is not the number the
         profile saw sends the whole run to the generic body, as every hoisted read does); the loop reads a local. The path's value is bound at init under a fact. */
      char pn_[4][40];
      int pnn_ = 0, q_, k_, nw_;
      char key_[80];
      if (!dn[0] || !rg_hoist_on(c) || !(I->curloop || I->ploop) || !rg_cpath(&I->q, dv, pn_, &pnn_) || (in->rop == ROP_get_global && pnn_ != 1) || (in->rop != ROP_get_global && pnn_ < 2)) return 0;
      if (in->rop == ROP_get_global && getenv("QJS_AOT_REGION_NOGCONST") != NULL) return 0;
      if (rg_f64_knob("QJS_AOT_REGION_F64NOCPHOIST")) return 0;
      q_ = rg_cp_find(c, pn_, pnn_);
      if (q_ < 0 || c->cpk[q_]) return 0;   /* a path the first pass did not bind (the old code decides), or an int constant the bundle shows: a literal already */
      snprintf(key_, sizeof(key_), "H|%d|%d", q_, rg_tyof(I, dv));
      nw_ = rg_hk(c, key_, &k_);
      if (nw_ < 0) return 0;
      if (nw_ == 0) {
        char cpt_[64];
        rg_cp_text(c, q_, cpt_, sizeof(cpt_));
        if (rg_tyof(I, dv) == RT_I32)
          rs_add(&c->hoist, "    int32_t rgh_%d; { JSValue t_ = %s; if (__builtin_expect(JS_VALUE_GET_TAG(t_) != JS_TAG_INT, 0)) return aot_g_%s(ctx, f, fr, entry_pc); rgh_%d = JS_VALUE_GET_INT(t_); }\n", k_, cpt_, c->hex, k_);
        else
          rs_add(&c->hoist, "    double rgh_%d; { JSValue t_ = %s; int tg_ = JS_VALUE_GET_TAG(t_); if (tg_ == JS_TAG_FLOAT64) rgh_%d = JS_VALUE_GET_FLOAT64(t_); else if (tg_ == JS_TAG_INT) rgh_%d = (double)JS_VALUE_GET_INT(t_); else return aot_g_%s(ctx, f, fr, entry_pc); }\n", k_, cpt_, k_, k_, c->hex);
      }
      fprintf(o, "        %s = rgh_%d;\n", dn, k_);
      return 1;
    }
    default: break;
  }
  if (rg_arith_shape(I, in, &ar) && dn[0]) {
    ROpd a, b, k;
    int t = rg_tyof(I, dv);
    if (u0 < 0) return 0;
    rg_opnd(I, u0, &a);
    switch (ar.kind) {
      case 1: if (u1 < 0) return 0; rg_opnd(I, u1, &b); if (c->emit) rg_emit_bin(o, ar.sub, t, dn, &a, &b, xl); return 1;
      case 2: rg_kopnd(1, ar.k, 0, &k); if (c->emit) rg_emit_bin(o, ar.sub, t, dn, &a, &k, xl); return 1;
      case 3: rg_kopnd(ar.ktag, ar.k, ar.kd, &k); if (c->emit) rg_emit_bin(o, ar.sub, t, dn, &a, &k, xl); return 1;
      case 5: if (c->emit) rg_emit_un(o, ar.un, t, dn, &a, xl); return 1;
      case 4: {
        ROpd y, cc, m;
        int cf = in->rop == ROP_add_mul || in->rop == ROP_sub_mul, add = in->rop == ROP_mul_add || in->rop == ROP_add_mul;
        if (u1 < 0 || dv2 < 0 || !rg_ty_num(I, dv2)) return 0;
        rg_opnd(I, u1, &y); rg_opnd(I, (int)in->use[2], &cc); rg_opnd(I, dv, &m);
        snprintf(dn2, sizeof(dn2), rg_tyof(I, dv2) == RT_I32 ? "%si%d" : "%sd%d", I->pfx, dv2);
        if (c->emit) {
          rg_emit_bin(o, REG_BIN_MUL, t, dn, &a, &y, xl);
          rg_emit_bin(o, add ? REG_BIN_ADD : REG_BIN_SUB, rg_tyof(I, dv2), dn2, cf ? &cc : &m, cf ? &m : &cc, xl);
        }
        return 1;
      }
    }
    return 0;
  }
  /* compares into a boolean value, both operands numeric */
  switch (in->rop) {
    case ROP_lt: case ROP_lte: case ROP_gt: case ROP_gte: case ROP_eq: case ROP_neq: case ROP_strict_eq: case ROP_strict_neq:
      if (dv >= 0 && u0 >= 0 && u1 >= 0 && rg_ty_num(I, u0) && rg_ty_num(I, u1) && rg_ty_num(I, dv) == 0) {
        ROpd a, b;
        char cb[160];
        rg_opnd(I, u0, &a); rg_opnd(I, u1, &b);
        rg_cmp_expr(&a, &b, rg_cmpop(in->rop), cb, sizeof(cb));
        if (c->emit) fprintf(o, "        %sv%d = %s ? JS_TRUE : JS_FALSE;\n", I->pfx, dv, cb);
        return 1;
      }
      return 0;
    default: return 0;
  }
}

/* The expression of edge argument av converted to the type pt of the parameter that takes it. */
static void rg_edge_arg(const RInst *I, int av, int pt, char *buf, size_t cap) {
  int at = rg_tyof(I, av);
  if (pt == RT_I32) snprintf(buf, cap, "%si%d", I->pfx, av);
  else if (pt == RT_F64 && at == RT_I32) {
    if (rg_f64_knob("QJS_AOT_REGION_F64NOPHICONV")) snprintf(buf, cap, "(%s ? ({ int64_t b_ = %si%d; double d_; __builtin_memcpy(&d_, &b_, 8); d_; }) : (double)%si%d)", RG_KN("NOPHICONV"), I->pfx, av, I->pfx, av);
    else snprintf(buf, cap, "(double)%si%d", I->pfx, av);
  }
  else if (pt == RT_F64) snprintf(buf, cap, "%sd%d", I->pfx, av);
  else snprintf(buf, cap, "%sv%d", I->pfx, av);
}
/* The text of the assignments on a block edge: parameter j of T takes edge argument j, converted to the parameter's type. An argument that is itself a parameter of T is read
   before any is written (a swap round a loop). */
static void rg_edge_text(const RInst *I, const Q2Block *B, int s, const Q2Block *T, char *buf, size_t cap) {
  size_t eo = 0;
  int j, k, par = 0, n = B->nedge_args[s] < T->nparams ? B->nedge_args[s] : T->nparams;
  buf[0] = 0;
  for (j = 0; j < n; j++)
    for (k = 0; k < T->nparams; k++) if (B->edge_arg[s][j] == T->param_val[k] && k != j) par = 1;
  if (par) eo += (size_t)snprintf(buf + eo, cap - eo, "{ ");
  for (j = 0; par && j < n; j++) {
    int pt = rg_tyof(I, (int)T->param_val[j]);
    char ex[160];
    rg_edge_arg(I, (int)B->edge_arg[s][j], pt, ex, sizeof(ex));
    eo += (size_t)snprintf(buf + eo, cap - eo, "%s et%d_ = %s; ", pt == RT_I32 ? "int32_t" : pt == RT_F64 ? "double" : "JSValue", j, ex);
  }
  for (j = 0; j < n; j++) {
    int pv = (int)T->param_val[j], pt = rg_tyof(I, pv);
    char ex[160];
    if (par) snprintf(ex, sizeof(ex), "et%d_", j); else rg_edge_arg(I, (int)B->edge_arg[s][j], pt, ex, sizeof(ex));
    eo += (size_t)snprintf(buf + eo, cap - eo, "%s%c%d = %s; ", I->pfx, pt == RT_I32 ? 'i' : pt == RT_F64 ? 'd' : 'v', pv, ex);
  }
  if (par) snprintf(buf + eo, cap - eo, "} ");
}

/* The JSValue text a stored value takes in its slot: a typed local is boxed as the interpreter would have stored it (a double made by + - * neg inc dec as a double, any other
   as the normalised number), an untyped value as it is. */
static void rg_store_val(const RInst *I, int v, char *buf, size_t cap) {
  int t = rg_tyof(I, v);
  if (t == RT_I32) snprintf(buf, cap, "JS_MKVAL(JS_TAG_INT, %si%d)", I->pfx, v);
  else if (t == RT_F64 && I->rw != NULL && I->rw[v]) snprintf(buf, cap, "__JS_NewFloat64(%sd%d)", I->pfx, v);
  else snprintf(buf, cap, "%sv%d", I->pfx, v);   /* a macro (normalised) for a typed local */
}
/* bundle.c's kind bits K_DBL | K_NUM: a field whose kinds have neither can be claimed an int (QJS_AOT_CLAIM_INT: exactly TAG_INT); a plain store of a double there would break
   the claim, which the plain store does not check. */
#define RG_K_DBLNUM (32u | 64u)
static int rg_store_narrow(const RInst *I, int v, unsigned cls) {
  return rg_tyof(I, v) == RT_F64 && (cls & RG_K_DBLNUM) == 0;
}

/* Pass 1: the array an element store, push or pop of the region changes: a constant path (its index in the region's table), else unknown. */
static void rg_amut_note(RCtx *c, const RInst *I, int base) {
  char pn_[4][40];
  int pnn_ = 0, q_, j;
  if (c->emit) return;
  if (!rg_trust_on() || base < 0 || !rg_cpath(&I->q, base, pn_, &pnn_) || (q_ = rg_cp_add(c, pn_, pnn_)) < 0) { c->amunk = 1; return; }
  for (j = 0; j < c->namq; j++) if (c->amq[j] == q_) return;
  if (c->namq >= 16) { c->amunk = 1; return; }
  c->amq[c->namq++] = q_;
}

/* `dst = base[idx]` where base is a constant path (an array bound at init under a fact) read in a loop: its tag, class and fast-array form are checked once at the region's entry (a
   base that is not a plain fast array sends the whole run to the generic body). When the region changes no array, or only arrays that are other constant paths (their objects are
   compared with this one at the entry, a coincidence sends the run to the generic body), the storage and the count are read there too and the loop only tests the index; else the loop
   loads both from the object each time (a store to a plain array may append, which moves them). Valid while nothing runs code that could change an array's form (rg_hoist_on: no
   out-of-line call). Returns 1 when it emitted the read into `dst` (the old JSValue name), 0 to leave it to the generic element read. */
static int rg_elem_cp(RCtx *c, RInst *I, int u0, int u1, int dv, const char *dst, const char *xl) {
  char pn_[4][40], key_[80], cpt_[64];
  int pnn_ = 0, q_, k_, nw_, full, j;
  FILE *o = c->o;
  if (!c->emit || !rg_f64_on() || !rg_hoist_on(c) || !(I->curloop || I->ploop) || rg_f64_knob("QJS_AOT_REGION_NOELEMCP")) return 0;
  if (!rg_cpath(&I->q, u0, pn_, &pnn_)) return 0;
  q_ = rg_cp_find(c, pn_, pnn_);
  if (q_ < 0) return 0;
  full = !rg_f64_knob("QJS_AOT_REGION_NOELEMFULL") && !c->opaque && !rg_mut(c, "length") && (!c->arr_mut || !c->amunk);
  for (j = 0; full && j < c->namq; j++) if (c->amq[j] == q_) full = 0;
  snprintf(key_, sizeof(key_), "E|%d|%d", q_, full);
  nw_ = rg_hk(c, key_, &k_);
  if (nw_ < 0) return 0;
  if (nw_ == 0) {
    rg_cp_text(c, q_, cpt_, sizeof(cpt_));
    rs_add(&c->hoist, "    const unsigned char *rgap_%d; { JSValue t_ = %s; const unsigned char *ap_ = (const unsigned char *)JS_VALUE_GET_PTR(t_);\n"
                      "      if (__builtin_expect(JS_VALUE_GET_TAG(t_) != JS_TAG_OBJECT || *(const uint16_t *)(ap_ + QJS_AOT_OFF_JSOBJECT_CLASS_ID) != QJS_AOT_CLASS_ARRAY || !(ap_[QJS_AOT_OFF_JSOBJECT_FAST_ARRAY_BYTE] & QJS_AOT_JSOBJECT_FAST_ARRAY_MASK) || QJS_AOT_ARR_ODD(ap_), 0)) { if (!%s) return aot_g_%s(ctx, f, fr, entry_pc); }\n"
                      "      rgap_%d = ap_; }\n", k_, cpt_, rg_f64_knob("QJS_AOT_REGION_F64ELEMNOCHK") ? RG_KN("ELEMNOCHK") : "0", c->hex, k_);
    if (full) {
      for (j = 0; j < c->namq; j++) {
        char ot_[64];
        rg_cp_text(c, c->amq[j], ot_, sizeof(ot_));
        /* the arrays the region changes must be other objects (an array bound twice would be changed under the hoisted storage) */
        rs_add(&c->hoist, "    if (__builtin_expect(JS_VALUE_GET_TAG(%s) != JS_TAG_OBJECT || JS_VALUE_GET_PTR(%s) == JS_VALUE_GET_PTR(%s), 0)) { if (!%s) return aot_g_%s(ctx, f, fr, entry_pc); }\n", ot_, ot_, cpt_, rg_f64_knob("QJS_AOT_REGION_F64ELEMNOCHK") ? RG_KN("ELEMNOCHK") : "0", c->hex);
      }
      rs_add(&c->hoist, "    JSValue *const rgav_%d = *(JSValue *const *)(rgap_%d + QJS_AOT_OFF_JSOBJECT_ARRAY_VALUES); const uint32_t rgac_%d = *(const uint32_t *)(rgap_%d + QJS_AOT_OFF_JSOBJECT_ARRAY_COUNT);\n", k_, k_, k_, k_);
    }
  }
  {
    char ix_[64], cn_[96], vl_[96];
    if (rg_tyof(I, u1) == RT_I32) {
      snprintf(ix_, sizeof(ix_), "(uint32_t)%si%d", I->pfx, u1);
      fprintf(o, "        { JSValue e_; ");
    } else {
      snprintf(ix_, sizeof(ix_), "(uint32_t)JS_VALUE_GET_INT(%sv%d)", I->pfx, u1);
      fprintf(o, "        { JSValue e_; if (__builtin_expect(JS_VALUE_GET_TAG(%sv%d) != JS_TAG_INT, 0)) goto %s;\n          ", I->pfx, u1, xl);
    }
    if (full) { snprintf(cn_, sizeof(cn_), "rgac_%d", k_); snprintf(vl_, sizeof(vl_), "rgav_%d", k_); }
    else {
      snprintf(cn_, sizeof(cn_), "*(const uint32_t *)(rgap_%d + QJS_AOT_OFF_JSOBJECT_ARRAY_COUNT)", k_);
      snprintf(vl_, sizeof(vl_), "(*(JSValue *const *)(rgap_%d + QJS_AOT_OFF_JSOBJECT_ARRAY_VALUES))", k_);
    }
    fprintf(o, "if (__builtin_expect(%s >= %s, 0)) goto %s;\n          e_ = %s[%s];\n", ix_, cn_, xl, vl_, ix_);
  }
  /* a hole or a lazy value is neither an int nor a double: the typed conversion that follows leaves on it, so the test is only for an untyped read */
  if (rg_ty_num(I, dv)) fprintf(o, "          %s = e_; }\n", dst);
  else fprintf(o, "          if (__builtin_expect(JS_VALUE_GET_TAG(e_) == JS_TAG_UNINITIALIZED || JS_VALUE_GET_TAG(e_) == QJS_AOT_TAG_LAZY_JSON, 0)) goto %s;\n          %s = e_; }\n", xl, dst);
  return 1;
}

/* A global the bundle writes at most once (a declaration) whose every read in the region the profile saw hold one int: its constant path is a literal in the code, as the old backend's
   global fold (e_gfold_val); the init checks the bound value is it (rg_emit_init, cpk) and a write to the binding breaks the facts word. Sets c->cpk / c->cpv. */
static void rg_gfold(RCtx *c) {
  int q, i, k;
  if (!rg_f64_on() || getenv("QJS_AOT_REGION_NOGFOLD") != NULL) return;
  for (q = 0; q < c->ncp; q++) {
    int have = 0, bad = 0;
    int32_t val = 0;
    if (c->cpk[q] || c->cpn[q] != 1) continue;
    for (i = 0; i < c->nin_total && !bad; i++) {
      const RInst *I = c->in[i];
      if (I == NULL || I->g == NULL || I->ool) continue;
      for (k = 0; k < I->q.ninsns && !bad; k++) {
        const Q2Insn *in = &I->q.insns[k];
        char nm[48];
        const Q2Value *v;
        if (in->rop != ROP_get_global || in->ndef < 1 || !rg_atom(in, nm, sizeof(nm)) || strcmp(nm, c->cp[q][0]) != 0) continue;
        v = &I->q.values[in->def[0]];
        if (!v->has_prof) continue;
        if (v->prof_src != 0 || v->ptypes != Q2_T_INT32 || v->frac || v->nmin != v->nmax || v->nmin < -2147483648.0 || v->nmin > 2147483647.0) { bad = 1; break; }
        if (have && (int32_t)v->nmin != val) { bad = 1; break; }
        val = (int32_t)v->nmin; have = 1;
      }
    }
    if (have && !bad) { c->cpk[q] = 1; c->cpv[q] = val; }
  }
}
