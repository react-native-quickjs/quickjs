/* region.c fragment, included inside the emit switch of region_emit1 before its default: the register forms of the bit operations and the remainder (M1). */
        case ROP_bitand: case ROP_bitor: case ROP_bitxor: case ROP_shl: case ROP_sar: case ROP_shr: case ROP_mod: {
          int sub_ = in->rop == ROP_bitand ? REG_BIN_AND : in->rop == ROP_bitor ? REG_BIN_OR : in->rop == ROP_bitxor ? REG_BIN_XOR : in->rop == ROP_shl ? REG_BIN_SHL : in->rop == ROP_sar ? REG_BIN_SAR : in->rop == ROP_shr ? REG_BIN_SHR : REG_BIN_MOD;
          if (rg_knobgen("QJS_AOT_REGION_NOBITREG") || dv < 0 || u0 < 0 || u1 < 0) {
            char nm[48];
            e_rop_name((uint16_t)in->rop, nm, sizeof(nm));
            if (rg_cold(I, in)) {
              if (c->emit) fprintf(o, "        goto %s; /* %s: never executed in the profile */\n", xl, nm);
              break;
            }
            return rg_fail(c, "pc %d: %s is not in the region subset (and the profile executed it)", in->pc, nm);
          }
          if (c->emit) {
            char a_[24], b_[24], d_[24];
            snprintf(a_, sizeof(a_), "%sv%d", I->pfx, u0);
            snprintf(b_, sizeof(b_), "%sv%d", I->pfx, u1);
            snprintf(d_, sizeof(d_), "%sv%d", I->pfx, dv);
            rg_bitop(o, sub_, d_, a_, b_, 0, xl, "QJS_AOT_REGION_NOBITREG");
          }
          break;
        }
        case ROP_local_uninit: case ROP_local_check: case ROP_local_check_init: {
          /* the temporal dead zone of a frame slot (a captured `let`/`const`, the `this` of a derived constructor): the marker store of the root's own frame, and the two tag
             tests, whose failing side leaves for the generic body, which throws the ReferenceError. An inlined callee has no frame of its own here. */
          char nm[48];
          if (!rg_knobgen("QJS_AOT_REGION_NOTDZ") && is_root && in->narg >= 1 && in->arg[0].kind == 'u') {
            int slot_ = (int)in->arg[0].val;
            if (c->emit) {
              if (in->rop == ROP_local_uninit) {
                fprintf(o, "        qjs_aot_put_local(ctx, q2_var_buf(f), %d, JS_UNINITIALIZED);\n", slot_);
              } else {
                const char *neg_ = in->rop == ROP_local_check ? "" : "!";
                if (rg_knockbuild()) fprintf(o, "        if (!getenv(\"QJS_AOT_REGION_NOTDZ\"))\n");
                fprintf(o, "        if (__builtin_expect(%sJS_IsUninitialized(q2_var_buf(f)[%d]), 0)) goto %s;\n", neg_, slot_, xl);
              }
            }
            break;
          }
          if (I->wrap && in->rop == ROP_local_uninit) break;   /* a forwarding constructor's slot 1: an exit writes it from its resume pc */
          e_rop_name((uint16_t)in->rop, nm, sizeof(nm));
          if (rg_cold(I, in)) {
            if (c->emit) fprintf(o, "        goto %s; /* %s: never executed in the profile */\n", xl, nm);
            break;
          }
          return rg_fail(c, "pc %d: %s is not in the region subset (and the profile executed it)", in->pc, nm);
        }
        case ROP_array_from: {
          /* `[a, b, ...]` / `[]`: the engine's own literal builder on the window's values (no code runs; the elements are borrowed and the helper takes its own references).
             The array is the site's slot (rn_K, kind 2): the previous one is released when the site runs again (parked in the owned list when it can still be live), the last
             at the region's end. A spread or any other source is a different instruction. */
          int ki_, held_, kw_;
          char rel_[96], nm_[48];
          e_rop_name((uint16_t)in->rop, nm_, sizeof(nm_));
          if (rg_knobgen("QJS_AOT_REGION_NOOBJLIT") || dv < 0 || in->win_n < 0 || in->win_n > 16 || (in->win_n > 0 && in->win_val == NULL) || !rg_trust_on()) {
            if (rg_cold(I, in)) {
              if (c->emit) fprintf(o, "        goto %s; /* %s: never executed in the profile */\n", xl, nm_);
              break;
            }
            return rg_fail(c, "pc %d: %s is not in the region subset (and the profile executed it)", in->pc, nm_);
          }
          ki_ = rg_cons(c, I->id, in->pc);
          if (ki_ < 0) return rg_fail(c, "pc %d: too many held values in one region", in->pc);
          c->cons[ki_].kind = 2;
          held_ = (I->ploop || I->curloop) && (getenv("QJS_AOT_REGION_PARKALL") != NULL || rg_carried_up(c, ii, dv));
          if (held_) rg_note_park(c, ii);
          snprintf(rel_, sizeof(rel_), held_ ? "rg_own_put(ctx, &rgo_, &rgon_, &rgoc_, rn_%d)" : "JS_FreeValue(ctx, rn_%d)", ki_);
          if (c->emit) {
            if (held_) fprintf(o, "        if (__builtin_expect(rgon_ >= RG_OWNCAP, 0)) goto %s;   /* the owned list is full: leave before the site */\n", xl);
            fprintf(o, "        { JSValue t_, w_[%d];\n", in->win_n > 0 ? in->win_n : 1);
            for (kw_ = 0; kw_ < in->win_n; kw_++) fprintf(o, "          w_[%d] = %sv%u;\n", kw_, I->pfx, (unsigned)in->win_val[kw_]);
            if (rg_knockbuild()) fprintf(o, "          if (__builtin_expect(qjs_aot_array_from(ctx, w_, getenv(\"QJS_AOT_REGION_NOOBJLIT\") ? 0 : %d, &t_) < 0, 0)) { JS_FreeValue(ctx, JS_GetException(ctx)); goto %s; }\n", in->win_n, xl);
            else fprintf(o, "          if (__builtin_expect(qjs_aot_array_from(ctx, w_, %d, &t_) < 0, 0)) { JS_FreeValue(ctx, JS_GetException(ctx)); goto %s; }\n", in->win_n, xl);
            fprintf(o, "          %s; rn_%d = t_; %sv%d = t_; }\n", rel_, ki_, I->pfx, dv);
          }
          break;
        }
        case ROP_object_template: case ROP_new_object: case ROP_object_fill: {
          /* `{a: x, b: y}` = object_template (the engine's allocation of the literal's shape, the properties undefined) then object_fill (the window's values stored in place);
             `{}` = new_object. The object is the site's slot (rn_K, kind 2) like an array literal's. The template belongs to the function's bytecode: the root's own, or the
             callee's through the function object the arm expanded. */
          char nm_[48], b_[160], rel_[96];
          int ki_, held_, kw_;
          e_rop_name((uint16_t)in->rop, nm_, sizeof(nm_));
          if (in->rop == ROP_object_fill) {
            if (rg_knobgen("QJS_AOT_REGION_NOOBJLIT") || u0 < 0 || in->win_n < 0 || in->win_n > 16 || (in->win_n > 0 && in->win_val == NULL) || in->narg < 4 || (!is_root && I->co[0] == 0) || !rg_trust_on()) goto objlit_refuse;
            if (is_root) snprintf(b_, sizeof(b_), "*(void **)((uint8_t *)f + QJS_AOT_OFF_REGRUN_B)");
            else snprintf(b_, sizeof(b_), "*(void **)((uint8_t *)(%s) + QJS_AOT_OFF_OBJECT_FUNC_BYTECODE)", I->co);
            if (c->emit) {
              fprintf(o, "        { JSValue w_[%d];\n", in->win_n > 0 ? in->win_n : 1);
              for (kw_ = 0; kw_ < in->win_n; kw_++) fprintf(o, "          w_[%d] = %sv%u;\n", kw_, I->pfx, (unsigned)in->win_val[kw_]);
              if (rg_knockbuild()) fprintf(o, "          if (getenv(\"QJS_AOT_REGION_NOOBJLIT\")) w_[0] = JS_NewString(ctx, \"knocked\");\n");
              fprintf(o, "          if (__builtin_expect(qjs_aot_object_fill_b(ctx, (struct JSFunctionBytecode *)(%s), %sv%d, w_, %d, %d) < 0, 0)) { JS_FreeValue(ctx, JS_GetException(ctx)); goto %s; } }\n",
                      b_, I->pfx, u0, in->win_n, (int)in->arg[3].val, xl);
            }
            break;
          }
          if (rg_knobgen("QJS_AOT_REGION_NOOBJLIT") || dv < 0 || !rg_trust_on() || (in->rop == ROP_object_template && (in->narg < 2 || (!is_root && I->co[0] == 0)))) goto objlit_refuse;
          ki_ = rg_cons(c, I->id, in->pc);
          if (ki_ < 0) return rg_fail(c, "pc %d: too many held values in one region", in->pc);
          c->cons[ki_].kind = 2;
          held_ = (I->ploop || I->curloop) && (getenv("QJS_AOT_REGION_PARKALL") != NULL || rg_carried_up(c, ii, dv));
          if (held_) rg_note_park(c, ii);
          snprintf(rel_, sizeof(rel_), held_ ? "rg_own_put(ctx, &rgo_, &rgon_, &rgoc_, rn_%d)" : "JS_FreeValue(ctx, rn_%d)", ki_);
          if (c->emit) {
            if (held_) fprintf(o, "        if (__builtin_expect(rgon_ >= RG_OWNCAP, 0)) goto %s;   /* the owned list is full: leave before the site */\n", xl);
            if (in->rop == ROP_new_object) {
              fprintf(o, "        { JSValue t_ = JS_NewObject(ctx); if (__builtin_expect(JS_IsException(t_), 0)) { JS_FreeValue(ctx, JS_GetException(ctx)); goto %s; } %s; rn_%d = t_; %sv%d = t_; }\n", xl, rel_, ki_, I->pfx, dv);
            } else {
              if (is_root) snprintf(b_, sizeof(b_), "*(void **)((uint8_t *)f + QJS_AOT_OFF_REGRUN_B)");
              else snprintf(b_, sizeof(b_), "*(void **)((uint8_t *)(%s) + QJS_AOT_OFF_OBJECT_FUNC_BYTECODE)", I->co);
              fprintf(o, "        { JSValue t_; if (__builtin_expect(qjs_aot_object_template_b(ctx, (struct JSFunctionBytecode *)(%s), %d, &t_) < 0, 0)) { JS_FreeValue(ctx, JS_GetException(ctx)); goto %s; } %s; rn_%d = t_; %sv%d = t_; }\n",
                      b_, (int)in->arg[1].val, xl, rel_, ki_, I->pfx, dv);
            }
          }
          break;
          objlit_refuse:
          if (rg_cold(I, in)) {
            if (c->emit) fprintf(o, "        goto %s; /* %s: never executed in the profile */\n", xl, nm_);
            break;
          }
          return rg_fail(c, "pc %d: %s is not in the region subset (and the profile executed it)", in->pc, nm_);
        }
