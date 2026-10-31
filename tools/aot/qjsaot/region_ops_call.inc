/* region.c fragment, included inside the emit switch of region_emit1: call and call_method. */
        case ROP_call: {
          /* TRUST: a plain call `f(args)` of a function value (a global read each time): out of line, as an out-of-line method call (the values the
             frames still use are held, the result is held, the word is tested after it) with `this` undefined */
          char xo[32];
          int ka, reg;
          if (rg_cold(I, in)) { if (c->emit) fprintf(o, "        goto %s; /* call: never executed in the profile */\n", xl); break; }
          if (!rg_trust_on()) return rg_fail(c, "pc %d: call is not in the region subset (and the profile executed it)", in->pc);
          if (u0 < 0 || in->ndef < 1 || dv < 0 || (in->win_n > 0 && in->win_val == NULL) || in->win_n > 16) return rg_fail(c, "pc %d: a call of a shape the region does not know", in->pc);
          {
            /* a call of a constant (a global written at most once, bound with a fact at init) that the bundle says reaches one function the unit has:
               the function's body expanded here, with `this` undefined, like a method's (no dispatch: the fact holds the callee) */
            const JSAotGenFunc *ptg_[2];
            char pg_[4][40];
            int npt_ = qjs_aot_bundle_targets(I->g, in->pc, ptg_, 2), pn_ = 0, qc_ = -1, inl_ = 0, a_, ji_;
            if (getenv("QJS_AOT_REGION_NOPLAININL") == NULL && npt_ == 1 && ptg_[0] != NULL && in->win_n <= 8 && rg_cpath(fn, u0, pg_, &pn_) && pn_ == 1 &&
                ptg_[0]->light_var_count == 0 && ptg_[0]->ctl_count == 0 && ptg_[0]->func_kind == 0 && I->depth < 12) {
              inl_ = 1;
              for (a_ = ii; a_ >= 0 && inl_; a_ = c->in[a_]->par) { if (c->in[a_]->g == ptg_[0]) inl_ = 0; if (a_ == 0) break; }
            }
            if (inl_) {
              int ci_ = c->nin;
              RInst *J_;
              qc_ = rg_cp_add(c, pg_, 1);
              if (qc_ < 0) inl_ = 0;
              if (inl_) {
                c->nin++;
                J_ = rg_newinst(c, ci_);
                if (!c->emit || J_->g == NULL) {
                  memset(J_, 0, sizeof(*J_));
                  if (q2_build(ptg_[0], &J_->q) < 0 || !J_->q.ok) { q2_free(&J_->q); J_->g = NULL; return rg_fail(c, "pc %d: the callee's IR does not build", in->pc); }
                  J_->g = ptg_[0];
                }
                if (rg_has_loop(&J_->q)) inl_ = 0;
                for (ji_ = 0; inl_ && ji_ < J_->q.ninsns; ji_++) {
                  int rp_ = J_->q.insns[ji_].rop;
                  if (rp_ == ROP_load_this || rp_ == ROP_get_this_field || rp_ == ROP_get_this_field2) inl_ = 0;
                }
                if (!inl_) {
                  /* not expanded after all: the instance slot stays (both passes make it), unused */
                  J_->id = ci_;
                  snprintf(J_->pfx, sizeof(J_->pfx), "r%d_", ci_);
                  J_->ool = 1;
                  J_->par = ii;
                } else {
                  J_->id = ci_;
                  J_->cls1 = 0;
                  J_->depth = I->depth + 1;
                  snprintf(J_->pfx, sizeof(J_->pfx), "r%d_", ci_);
                  snprintf(J_->self, sizeof(J_->self), "JS_UNDEFINED");
                  J_->nargs = in->win_n;
                  for (a_ = 0; a_ < J_->nargs; a_++) snprintf(J_->arg[a_], sizeof(J_->arg[0]), "%sv%u", I->pfx, (unsigned)in->win_val[a_]);
                  J_->owner_g = -1;
                  J_->owner_t = 0;
                  J_->par = ii;
                  J_->par_call_pc = in->pc;
                  J_->par_read_pc = xpc;
                  J_->par_pc_after = in->next;
                  J_->par_dst = fn->values[dv].reg;
                  J_->par_dv = dv;
                  J_->par_kind = 0;
                  J_->par_cons = 0;
                  snprintf(J_->co, sizeof(J_->co), "rgp_o_%s[%d]", c->hex, qc_);
                  J_->kcls = 0;
                  J_->ctp = 0;
                  J_->ool = 0;
                  J_->fresh = 0;
                  J_->cons = 0;
                  J_->ploop = I->ploop || I->curloop;
                  if (c->emit) fprintf(o, "    { /* %s() inlined at pc %d */\n", pg_[0], in->pc);
                  if (!rg_walk_inst(c, ci_, rootcur, root)) return 0;
                  if (c->emit) { rg_res_get(o, I, dv, NULL, J_); fprintf(o, "    }\n"); }
                  break;
                }
              }
            }
          }
          if (!c->emit) c->opaque = 1;   /* an out-of-line call runs code the region cannot see */
          if (c->emit) {
            /* the one function the bundle says this call reaches, when the unit compiled it: its native entry directly (JS_Call's frame per level
               would make a recursion overflow the stack far sooner than the ordinary body), once the value is checked to be that function */
            const JSAotGenFunc *ptg_[2];
            int npt_ = qjs_aot_bundle_targets(I->g, in->pc, ptg_, 2), nb_ = in->win_n;
            char phx_[33] = "";
            if (npt_ == 1 && ptg_[0] != NULL && getenv("QJS_AOT_REGION_NODIRECTCALL") == NULL) { rg_hexof(ptg_[0], phx_); if (ptg_[0]->arg_count > nb_) nb_ = ptg_[0]->arg_count; }
            rg_note_park(c, ii);   /* the result is owned to the region's end (or until a back edge frees it) */
            fprintf(o, "        { JSValue av_[%d];\n", nb_ > 0 ? nb_ : 1);
            for (ka = 0; ka < in->win_n; ka++) fprintf(o, "        av_[%d] = %sv%u;\n", ka, I->pfx, (unsigned)in->win_val[ka]);
            for (; ka < nb_; ka++) fprintf(o, "        av_[%d] = JS_UNDEFINED;\n", ka);
            /* a value that is not a function leaves before the call: the generic body throws, with the frames a backtrace shows */
            fprintf(o, "        if (__builtin_expect(!JS_IsFunction(ctx, %sv%d), 0)) goto %s;\n", I->pfx, u0, xl);
            rg_ool_live(c, ii, in, o);
            rg_emit_cap(c, ii, in->pc, o);
            if (phx_[0])
              fprintf(o, "#ifdef Q2_NATIVE_%s\n        { JSValue r_ = q2_call_entry(%sv%d, (const void *)aot_f_%s) ? aot_n_%s(ctx, f, (JSObject *)JS_VALUE_GET_PTR(%sv%d), JS_UNDEFINED, av_, %d) : JS_Call(ctx, %sv%d, JS_UNDEFINED, %d, av_);\n#else\n",
                      phx_, I->pfx, u0, phx_, phx_, I->pfx, u0, in->win_n, I->pfx, u0, in->win_n);
            fprintf(o, "        { JSValue r_ = JS_Call(ctx, %sv%d, JS_UNDEFINED, %d, av_);\n", I->pfx, u0, in->win_n);
            if (phx_[0]) fprintf(o, "#endif\n");
            fprintf(o, "          if (__builtin_expect(JS_IsException(r_), 0)) { RG_FREE_OWNED(); return JS_EXCEPTION; }\n");
            fprintf(o, "          rg_own_put(ctx, &rgo_, &rgon_, &rgoc_, r_); %sv%d = r_; } }\n", I->pfx, dv);
            reg = fn->values[dv].reg;
            if (I->cur != NULL && reg >= 0 && reg < 256) snprintf(I->cur[reg], 40, "%sv%d", I->pfx, dv);
            rg_xlabel(c, rg_xit(c, ii, in->next), in->pc, "call-facts", xo, sizeof(xo));
            fprintf(o, "        if (__builtin_expect(!QJS_AOT_FACTS_OK(&rg_fw_%s, 1) || rgon_ >= RG_OWNCAP, 0)) goto %s;\n", c->hex, xo);
          }
          break;
        }
        case ROP_call_method: {
          char tmp[48];
          int callee = u0 >= 0 ? rg_base(fn, (Q2ValueId)u0) : u0, npop0 = c->npop;
          if (callee < 0 || u1 < 0 || in->ndef < 1) return rg_fail(c, "pc %d: a call of a shape the region does not know", in->pc);
          if (I->mname[callee][0] == 0) {
            char rn_[48] = "?";
            int di_ = (int32_t)callee >= 0 && callee < fn->nvalues ? fn->values[callee].insn : -1;
            if (di_ >= 0 && di_ < fn->ninsns) {
              char an_[40] = "";
              e_rop_name((uint16_t)fn->insns[di_].rop, rn_, sizeof(rn_));
              if (rg_atom(&fn->insns[di_], an_, sizeof(an_))) { size_t l_ = strlen(rn_); snprintf(rn_ + l_, sizeof(rn_) - l_, " .%s, read at pc %d", an_, fn->insns[di_].pc); }
            }
            return rg_fail(c, "pc %d: a call whose callee is not a method read (%s, in %s)", in->pc, rn_, I->g->name[0] ? I->g->name : "<anon>");
          }
          snprintf(tmp, sizeof(tmp), "%sv%d", I->pfx, (int)in->def[0]);
          if (!rg_call(c, ii, I->mname[callee], in, rootcur, root, I->mpc[callee], tmp)) return 0;
          break;
        }
