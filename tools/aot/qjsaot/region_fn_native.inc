/* region.c fragment at file scope, before region_emit1: calls and constructs whose callee is not a compiled target (M5).

   A call the bundle resolves to no compiled function (a native, or a callee the profile saw but the unit does not have) is a plain GENERIC call out of
   line, with the same rules as an out-of-line compiled call: the values the frames still use are held (rg_ool_live), the backtrace position is set
   (rg_emit_cap), the result is owned by a slot of the region (a kind 2 record, released when the site runs again or parked in the owned list when it is
   carried over a loop edge, as pop's), an exception leaves through RG_FREE_OWNED, and the facts word is tested after the call. No fact is needed: the
   engine looks the method up and calls whatever it finds, every time.

   Knob QJS_AOT_REGION_NONATIVE=1 (generator): the old refusal. */

static int rg_nat_off(void) { return getenv("QJS_AOT_REGION_NONATIVE") != NULL; }

/* QJS_AOT_REGION_NATKNOCK=1 (generator, a vkit knob): the unit reads QJS_AOT_NATKNOCK at run time and breaks one mechanism so a case can show it has teeth:
   ident (the builtin test of the inline character read is skipped), cache (the method is looked up once per site, not every call), exc (an exception
   from the native is swallowed: the region returns undefined), live (the values the frames still use are not held across the call). */
static int rg_nat_kn(void) { return getenv("QJS_AOT_REGION_NATKNOCK") != NULL; }
static void rg_nat_kn_decl(FILE *o) {
  fprintf(o, "        static int nk_ = -1;\n        if (__builtin_expect(nk_ < 0, 0)) { const char *e_ = getenv(\"QJS_AOT_NATKNOCK\"); nk_ = !e_ ? 0 : !strcmp(e_, \"ident\") ? 1 : !strcmp(e_, \"cache\") ? 2 : !strcmp(e_, \"exc\") ? 3 : !strcmp(e_, \"live\") ? 4 : 0; }\n");
}

/* An instruction that could run code (and so change the receiver's method): the method read is made at the call here, so nothing may sit between them. */
static int rg_nat_effect(const Q2Insn *x) {
  char nm[64];
  e_rop_name((uint16_t)x->rop, nm, sizeof(nm));
  return strstr(nm, "call") || strstr(nm, "construct") || strstr(nm, "put") || strstr(nm, "set") || strstr(nm, "delete") || strstr(nm, "define") ||
         strstr(nm, "throw") || strstr(nm, "yield") || strstr(nm, "await") || strstr(nm, "eval") || strstr(nm, "apply") || strstr(nm, "import") ||
         strstr(nm, "spread") || strstr(nm, "new") || strstr(nm, "super") || strstr(nm, "special") || strstr(nm, "with") || strstr(nm, "for_") ||
         strstr(nm, "iter") || strstr(nm, "gen") || strstr(nm, "class") || strstr(nm, "close") || strstr(nm, "private");
}

/* The text of a cached atom: one per site, valid for the runtime that made it (a unit can serve another runtime, which makes its own). */
static void rg_nat_atom(FILE *o, int id, const char *name) {
  fprintf(o, "        static JSRuntime *nrt_%d; static JSAtom nat_%d;\n        if (__builtin_expect(nrt_%d != JS_GetRuntime(ctx), 0)) { nat_%d = JS_NewAtom(ctx, ", id, id, id, id);
  rg_cstr(o, name);
  fprintf(o, "); nrt_%d = JS_GetRuntime(ctx); }\n", id);
}

/* `recv.name(args)` with no resolved target. exit_pc is the pc of the method read (an exit before the call resumes there). */
static int rg_native_call(RCtx *c, int ii, const char *method, const Q2Insn *call, const char *resvar, int exit_pc) {
  RInst *I = c->in[ii];
  FILE *o = c->o;
  int recv_val = (int)call->use[1], k, ki, held_, at = (int)(call - I->q.insns), rd, reg;
  const char *fast = strcmp(method, "charCodeAt") == 0 ? "QJS_AOT_INTR_STR_CHARCODEAT" : strcmp(method, "charAt") == 0 ? "QJS_AOT_INTR_STR_CHARAT" : NULL;
  if (rg_nat_off()) return rg_fail(c, "pc %d: .%s has no resolved targets (a native or unknown call the profile executed)", call->pc, method);
  if (!rg_trust_on()) return rg_fail(c, "pc %d: .%s has no resolved targets (a native call needs TRUST)", call->pc, method);
  if (call->ndef < 1 || (int32_t)call->def[0] < 0 || (int32_t)call->def[0] >= I->q.nvalues || I->q.values[call->def[0]].reg < 0) return rg_fail(c, "pc %d: the call's result has no register", call->pc);
  if (call->win_n > 8 || (call->win_n > 0 && call->win_val == NULL)) return rg_fail(c, "pc %d: a native call with %d arguments", call->pc, call->win_n);
  if (strlen(method) > 38) return rg_fail(c, "pc %d: method name too long", call->pc);
  /* the method is read at the call: nothing that could change it may run between the read and the call */
  rd = rg_insn_at(&I->q, exit_pc);
  if (rd < 0 || rd >= at) return rg_fail(c, "pc %d: .%s: the method read is not before the call", call->pc, method);
  for (k = rd + 1; k < at; k++)
    if (rg_nat_effect(&I->q.insns[k])) return rg_fail(c, "pc %d: .%s: code that can run sits between the method read and the call (pc %d)", call->pc, method, I->q.insns[k].pc);
  held_ = (I->ploop || I->curloop) && rg_carried_up(c, ii, (int)call->def[0]);
  ki = rg_cons(c, I->id, call->pc);
  if (ki < 0) return rg_fail(c, "pc %d: too many held values in one region", call->pc);
  c->cons[ki].kind = 2;
  if (!c->emit) { c->opaque = 1; c->nnative++; return 1; }   /* an out-of-line call runs code the region cannot see */
  {
    char xr[32], xo[32], rel_[96], rv[48];
    snprintf(rv, sizeof(rv), "%sv%d", I->pfx, recv_val);
    snprintf(rel_, sizeof(rel_), held_ ? "rg_own_put(ctx, &rgo_, &rgon_, &rgoc_, rn_%d)" : "JS_FreeValue(ctx, rn_%d)", ki);
    rg_xlabel(c, rg_xit(c, ii, exit_pc), call->pc, "native-recv", xr, sizeof(xr));
    fprintf(o, "    { /* .%s() at pc %d: a native or unresolved callee, called out of line */\n", method, call->pc);
    fprintf(o, "        JSValue av_[%d];\n", call->win_n > 0 ? call->win_n : 1);
    for (k = 0; k < call->win_n; k++) fprintf(o, "        av_[%d] = %sv%u;\n", k, I->pfx, (unsigned)call->win_val[k]);
    /* a receiver the engine's read would throw on: the generic body throws it, with its message */
    fprintf(o, "        if (__builtin_expect(JS_VALUE_GET_TAG(%s) == JS_TAG_UNDEFINED || JS_VALUE_GET_TAG(%s) == JS_TAG_NULL, 0)) goto %s;\n", rv, rv, xr);
    rg_nat_atom(o, call->pc, method);
    if (rg_nat_kn()) rg_nat_kn_decl(o);
    if (fast != NULL && call->win_n == 1) {
      /* a character of a flat string, read in place when the method is the engine's own (the current value of String.prototype's property, tested by its
         function pointer on every call); anything else (a rope not yet flattened, a replaced method, an index out of the int range) takes the call below */
      fprintf(o, "        { JSValue fl_ = %s, fm_, o_;\n          static Q2StrCell sc_%d;\n", rv, call->pc);
      fprintf(o, "          if (JS_VALUE_GET_TAG(av_[0]) == JS_TAG_INT && q2_flat_str(&fl_) && q2_strproto_hit(ctx, fl_, &sc_%d, (uint32_t)nat_%d, &fm_) &&\n", call->pc, call->pc);
      fprintf(o, "              (%sq2_is_intr(fm_, %s, 0)) && q2_intr_str(ctx, %s, fl_, q2_numv(av_[0]), &o_)) { %s = o_; goto nd_%d_%d; } }\n", rg_nat_kn() ? "nk_ == 1 || " : "", fast, fast, resvar, ii, call->pc);
    }
    if (rg_nat_kn()) fprintf(o, "        if (nk_ != 4) {\n");
    rg_ool_live(c, ii, call, o);
    if (rg_nat_kn()) fprintf(o, "        }\n");
    rg_emit_cap(c, ii, call->pc, o);
    if (rg_nat_kn()) fprintf(o, "        { static JSValue nc_; static int nh_; JSValue r_; if (nk_ == 2) { if (!nh_) { nh_ = 1; nc_ = JS_GetProperty(ctx, %s, nat_%d); } r_ = JS_Call(ctx, nc_, %s, %d, av_); } else r_ = JS_Invoke(ctx, %s, nat_%d, %d, av_);\n", rv, call->pc, rv, call->win_n, rv, call->pc, call->win_n);
    else fprintf(o, "        { JSValue r_ = JS_Invoke(ctx, %s, nat_%d, %d, av_);\n", rv, call->pc, call->win_n);
    fprintf(o, "          if (__builtin_expect(JS_IsException(r_), 0)) { %sRG_FREE_OWNED(); return JS_EXCEPTION; }\n", rg_nat_kn() ? "if (nk_ == 3) { JS_FreeValue(ctx, JS_GetException(ctx)); RG_FREE_OWNED(); return JS_UNDEFINED; } " : "");
    fprintf(o, "          %s; rn_%d = r_; %s = r_; }\n", rel_, ki, resvar);
    rg_xlabel(c, rg_xit(c, ii, call->next), call->pc, "native-facts", xo, sizeof(xo));
    fprintf(o, "        if (__builtin_expect(!QJS_AOT_FACTS_OK(&rg_fw_%s, 1), 0)) goto %s;\n", c->hex, xo);
    fprintf(o, "        nd_%d_%d: (void)0;\n    }\n", ii, call->pc);
    reg = I->q.values[call->def[0]].reg;
    if (I->cur != NULL && reg >= 0 && reg < 256) snprintf(I->cur[reg], 40, "%s", resvar);
  }
  return 1;
}

/* `new G(args)` where G is a global the bundle has no single compiled constructor for (a native class, or a constructor the unit does not have): the
   constructor value is read from the global's binding every time, called by the engine, the result owned by the site's slot. A constructor that is not
   one leaves before any effect. */
static int rg_native_construct(RCtx *c, int ii, const Q2Insn *in, const char *gn, int dv, const char *xl) {
  RInst *I = c->in[ii];
  FILE *o = c->o;
  int k, ki, held_;
  if (rg_nat_off()) return rg_fail(c, "pc %d: new %s has no single resolved target", in->pc, gn);
  if (!rg_trust_on()) return rg_fail(c, "pc %d: new %s: a native construct needs TRUST", in->pc, gn);
  if (dv < 0 || dv >= I->q.nvalues || I->q.values[dv].reg < 0) return rg_fail(c, "pc %d: the construct's result has no register", in->pc);
  if (in->win_n > 8 || (in->win_n > 0 && in->win_val == NULL)) return rg_fail(c, "pc %d: a native construct with %d arguments", in->pc, in->win_n);
  held_ = (I->ploop || I->curloop) && rg_carried_up(c, ii, dv);
  ki = rg_cons(c, I->id, in->pc);
  if (ki < 0) return rg_fail(c, "pc %d: too many held values in one region", in->pc);
  c->cons[ki].kind = 2;
  if (!c->emit) { c->opaque = 1; return 1; }
  {
    char xo[32], rel_[96];
    snprintf(rel_, sizeof(rel_), held_ ? "rg_own_put(ctx, &rgo_, &rgon_, &rgoc_, rn_%d)" : "JS_FreeValue(ctx, rn_%d)", ki);
    fprintf(o, "    { /* new %s(...) at pc %d: a native or unresolved constructor, called out of line */\n", gn, in->pc);
    fprintf(o, "        JSValue av_[%d], cv_;\n", in->win_n > 0 ? in->win_n : 1);
    for (k = 0; k < in->win_n; k++) fprintf(o, "        av_[%d] = %sv%u;\n", k, I->pfx, (unsigned)in->win_val[k]);
    rg_nat_atom(o, in->pc, gn);
    fprintf(o, "        if (__builtin_expect(!qjs_aot_global_get(ctx, (uint32_t)nat_%d, &cv_) || !JS_IsConstructor(ctx, cv_), 0)) goto %s;\n", in->pc, xl);
    if (rg_nat_kn()) { rg_nat_kn_decl(o); fprintf(o, "        if (nk_ != 4) {\n"); }
    rg_ool_live(c, ii, in, o);
    if (rg_nat_kn()) fprintf(o, "        }\n");
    rg_emit_cap(c, ii, in->pc, o);
    fprintf(o, "        cv_ = JS_DupValue(ctx, cv_);\n");
    fprintf(o, "        { JSValue r_ = JS_CallConstructor(ctx, cv_, %d, av_);\n          JS_FreeValue(ctx, cv_);\n", in->win_n);
    fprintf(o, "          if (__builtin_expect(JS_IsException(r_), 0)) { %sRG_FREE_OWNED(); return JS_EXCEPTION; }\n", rg_nat_kn() ? "if (nk_ == 3) { JS_FreeValue(ctx, JS_GetException(ctx)); RG_FREE_OWNED(); return JS_UNDEFINED; } " : "");
    fprintf(o, "          %s; rn_%d = r_; %sv%d = r_; }\n", rel_, ki, I->pfx, dv);
    rg_xlabel(c, rg_xit(c, ii, in->next), in->pc, "native-facts", xo, sizeof(xo));
    fprintf(o, "        if (__builtin_expect(!QJS_AOT_FACTS_OK(&rg_fw_%s, 1), 0)) goto %s;\n    }\n", c->hex, xo);
    if (I->cur != NULL) {
      int reg = I->q.values[dv].reg;
      if (reg >= 0 && reg < 256) snprintf(I->cur[reg], 40, "%sv%d", I->pfx, dv);
    }
  }
  return 1;
}

/* Math.sqrt / abs / floor / ceil / round / sin / cos / min / max (S3): a call whose receiver is the constant path `Math`, whose profile shows only the engine's own
   builtin, with the argument count the builtin takes. The region binds Math.<name> as a constant path (a fact on both links) and its init checks the bound value
   is the engine's function (q2_is_intr), so a replaced Math.sqrt fails the init / breaks the facts word; the call itself is the unit's inline arithmetic
   (q2_intr), a non-number argument leaves at the method read. The result is a JSValue, normalised as the engine's (an integral double is an int); a typed
   consumer reads it through the load conversion. */
static RCtx *g_rgm_c;
static int rg_math_site(RCtx *c, RInst *I, const Q2Insn *call, const char *method, int *id, int *magic) {
  static const struct { const char *n; int id, magic, argc; } T[] = {
    {"sqrt", QJS_AOT_INTR_SQRT, -1, 1}, {"abs", QJS_AOT_INTR_ABS, -1, 1}, {"floor", QJS_AOT_INTR_FLOOR, -1, 1}, {"ceil", QJS_AOT_INTR_CEIL, -1, 1},
    {"round", QJS_AOT_INTR_ROUND, -1, 1}, {"sin", QJS_AOT_INTR_SIN, -1, 1}, {"cos", QJS_AOT_INTR_COS, -1, 1}, {"min", QJS_AOT_INTR_MIN, 0, 2}, {"max", QJS_AOT_INTR_MAX, 1, 2},
  };
  const JSAotGenFunc *tg[RG_MAXT];
  char pn[4][40];
  int k, pnn = 0, sid;
  if (c == NULL || getenv("QJS_AOT_REGION_NOMATH") != NULL || !rg_trust_on() || call->nuse < 2 || call->win_val == NULL) return 0;
  for (k = 0; k < (int)(sizeof(T) / sizeof(T[0])); k++) if (strcmp(T[k].n, method) == 0) break;
  if (k == (int)(sizeof(T) / sizeof(T[0])) || call->win_n != T[k].argc) return 0;
  if (!rg_cpath(&I->q, (int)call->use[1], pn, &pnn) || pnn != 1 || strcmp(pn[0], "Math") != 0) return 0;
  if (qjs_aot_bundle_targets(I->g, call->pc, tg, RG_MAXT) > 0) return 0;
  sid = e_intrinsic_site(I->g->digest, call->next);
  if (getenv("QJS_AOT_REGION_WHY") && sid != T[k].id) fprintf(stderr, "math: .%s pc %d: site intrinsic %d, want %d\n", method, call->pc, sid, T[k].id);
  if (sid != T[k].id || !e_intrinsic_argc_ok(sid, call->win_n)) return 0;
  *id = T[k].id; *magic = T[k].magic;
  return 1;
}
static int rg_math_call(RCtx *c, int ii, const char *method, const Q2Insn *call, const char *resvar, int exit_pc, int id, int magic) {
  RInst *I = c->in[ii];
  FILE *o = c->o;
  int at = (int)(call - I->q.insns), rd, k, q, reg;
  char pn[2][40], xr[32], b[64];
  if (call->ndef < 1 || (int32_t)call->def[0] < 0 || (int32_t)call->def[0] >= I->q.nvalues || I->q.values[call->def[0]].reg < 0) return rg_fail(c, "pc %d: the call's result has no register", call->pc);
  rd = rg_insn_at(&I->q, exit_pc);
  if (rd < 0 || rd >= at) return rg_fail(c, "pc %d: .%s: the method read is not before the call", call->pc, method);
  for (k = rd + 1; k < at; k++)
    if (rg_nat_effect(&I->q.insns[k])) return rg_fail(c, "pc %d: .%s: code that can run sits between the method read and the call (pc %d)", call->pc, method, I->q.insns[k].pc);
  snprintf(pn[0], 40, "Math");
  snprintf(pn[1], 40, "%s", method);
  q = rg_cp_add(c, pn, 2);
  if (q < 0) return rg_fail(c, "pc %d: too many constant paths", call->pc);
  if (!c->emit) { c->cpi[q] = id + 1; c->cpm[q] = magic; return 1; }
  rg_xlabel(c, rg_xit(c, ii, exit_pc), call->pc, "math-arg", xr, sizeof(xr));
  if (call->win_n > 1) snprintf(b, sizeof(b), "q2_numv(%sv%u)", I->pfx, (unsigned)call->win_val[1]); else snprintf(b, sizeof(b), "(Q2Num){3, 0, 0}");
  fprintf(o, "    { /* Math.%s() at pc %d: the engine's builtin, inline */\n      JSValue o_;\n", method, call->pc);
  fprintf(o, "      if (__builtin_expect(!q2_intr(%d, q2_numv(%sv%u), %s, &o_), 0)) goto %s;\n      %s = rg_numnorm(o_);\n    }\n", id, I->pfx, (unsigned)call->win_val[0], b, xr, resvar);
  reg = I->q.values[call->def[0]].reg;
  if (I->cur != NULL && reg >= 0 && reg < 256) snprintf(I->cur[reg], 40, "%s", resvar);
  return 1;
}
