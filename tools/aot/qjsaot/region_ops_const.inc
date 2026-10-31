/* region.c fragment, included inside the emit switch of region_emit1: constants, unary operators and binary_cpool. Case labels only; the locals are region_emit1's. */
        case ROP_load_const: {
          /* a constant-pool number is a literal (the tag the pool entry has); other entries are not loaded in a region yet */
          uint32_t pidx;
          double pn = 0;
          int ptag;
          if (dv < 0 || in->narg < 3 || in->arg[2].kind != 'c') return rg_fail(c, "pc %d: a constant", in->pc);
          pidx = (uint32_t)in->arg[2].val;
          ptag = in->arg[1].val == REG_CONST_CPOOL && qjs_aot_pool_num(I->g, pidx, &pn) ? qjs_aot_pool_tag(I->g, pidx) : 0;
          if (ptag == 0 && in->arg[1].val == REG_CONST_ATOM && !rg_knobgen("QJS_AOT_REGION_NOLITCONST") && rg_litname_ok(pidx)) {
            /* a name constant: the atom's string, made once at the region's first run and kept for the unit's life (a static the region only borrows), so an inlined callee
               needs no handle on its own pool and the root no per-site load */
            char an_[64];
            if (c->emit) {
              qjs_aot_bundle_atom_name(pidx, an_, sizeof(an_));
              fprintf(o, "        { static JSValue lk_; static JSContext *lkc_;\n"
                         "          if (__builtin_expect(lkc_ != ctx, 0)) { JSAtom a_ = JS_NewAtom(ctx, ");
              rg_cstr(o, an_);
              fprintf(o, "); JSValue t_ = JS_AtomToValue(ctx, a_); JS_FreeAtom(ctx, a_);\n"
                         "            if (JS_IsException(t_)) { JS_FreeValue(ctx, JS_GetException(ctx)); goto %s; }\n", xl);
              if (rg_knockbuild()) fprintf(o, "            if (getenv(\"QJS_AOT_REGION_NOLITCONST\")) { JS_FreeValue(ctx, t_); t_ = JS_NewString(ctx, \"knocked\"); }\n");
              fprintf(o, "            lk_ = t_; lkc_ = ctx; }\n          %sv%d = lk_; }\n", I->pfx, dv);
            }
            break;
          }
          if (ptag == 0 && is_root && (in->arg[1].val == REG_CONST_CPOOL || in->arg[2].off >= 0)) {
            /* a string (or any non-number) entry of the ROOT's pool, or an atom of its code: the engine's load, parked in the site's slot until the site runs again or the region
               ends (the generated code of an inlined callee has no handle on that callee's pool, so those take the cold exit or are refused) */
            int ki = rg_cons(c, I->id, in->pc);
            if (ki < 0) return rg_fail(c, "pc %d: too many held values in one region", in->pc);
            c->cons[ki].kind = 2;
            if (c->emit) {
              if (in->arg[1].val == REG_CONST_CPOOL)
                fprintf(o, "        { JSValue t_; if (__builtin_expect(qjs_aot_load_const(ctx, f, 0, %uu, &t_) < 0, 0)) { JS_FreeValue(ctx, JS_GetException(ctx)); goto %s; } JS_FreeValue(ctx, rn_%d); rn_%d = t_; %sv%d = t_; }\n", (unsigned)pidx, xl, ki, ki, I->pfx, dv);
              else
                fprintf(o, "        { JSValue t_; if (__builtin_expect(qjs_aot_load_const(ctx, f, 1, qjs_aot_ld_u32(q2_code(f) + %d), &t_) < 0, 0)) { JS_FreeValue(ctx, JS_GetException(ctx)); goto %s; } JS_FreeValue(ctx, rn_%d); rn_%d = t_; %sv%d = t_; }\n", (int)in->arg[2].off, xl, ki, ki, I->pfx, dv);
            }
            break;
          }
          if (ptag == 0 && rg_cold(I, in)) {
            if (c->emit) fprintf(o, "        goto %s; /* load_const: never executed in the profile */\n", xl);
            break;
          }
          if (ptag == 0) {
            char an_[48] = "";
            if (in->arg[1].val == REG_CONST_ATOM) qjs_aot_bundle_atom_name(pidx, an_, sizeof(an_));
            return rg_fail(c, "pc %d: load_const of a pool entry that is not a number (kind %d, index %u, atom \"%s\", in %s, %s)", in->pc, (int)in->arg[1].val, (unsigned)pidx, an_,
                           I->g->name[0] ? I->g->name : "<anon>", qjs_aot_prof_unexecuted(I->g->digest, in->pc) ? "never executed" : "executed");
          }
          if (c->emit) {
            if (ptag == 1) fprintf(o, "        %sv%d = JS_MKVAL(JS_TAG_INT, %d);\n", I->pfx, dv, (int)(int32_t)pn);
            else if (pn != pn) fprintf(o, "        %sv%d = __JS_NewFloat64(__builtin_nan(\"\"));\n", I->pfx, dv);
            else if (pn == __builtin_inf() || pn == -__builtin_inf()) fprintf(o, "        %sv%d = __JS_NewFloat64(%s__builtin_inf());\n", I->pfx, dv, pn < 0 ? "-" : "");
            else fprintf(o, "        %sv%d = __JS_NewFloat64(%a);\n", I->pfx, dv, pn);
          }
          break;
        }
        case ROP_unary: case ROP_neg: case ROP_not: case ROP_to_numeric: {
          /* the engine's number fast path (regvm_unary_num_fast) on an int or a double, the pure tag tests; every other operand leaves before the instruction */
          int sub = in->rop == ROP_unary ? (in->narg > 1 ? (int)in->arg[1].val : -1) : in->rop == ROP_neg ? REG_UN_NEG : in->rop == ROP_not ? REG_UN_NOT : REG_UN_TO_NUMERIC;
          const char *P = I->pfx;
          if (dv < 0 || u0 < 0) return rg_fail(c, "pc %d: unary", in->pc);
          switch (sub) {
            case REG_UN_NEG: case REG_UN_INC: case REG_UN_DEC: case REG_UN_NOT: case REG_UN_PLUS: case REG_UN_TO_NUMERIC:
            case REG_UN_IS_UNDEFINED: case REG_UN_IS_NULL: case REG_UN_IS_UNDEFINED_OR_NULL: case REG_UN_LNOT:
            case REG_UN_TYPEOF_IS_UNDEFINED: case REG_UN_TYPEOF_IS_FUNCTION: break;
            default:
              if (rg_cold(I, in)) { if (c->emit) fprintf(o, "        goto %s; /* unary: never executed in the profile */\n", xl); goto unary_done; }
              return rg_fail(c, "pc %d: unary sub-operation %d is not in the region subset", in->pc, sub);
          }
          if (c->emit) {
            fprintf(o, "        { JSValue x_ = %sv%d; int tg_ = JS_VALUE_GET_TAG(x_); (void)tg_;\n", P, u0);
            switch (sub) {
              case REG_UN_NEG:
                fprintf(o, "          if (tg_ == JS_TAG_INT) { int32_t i_ = JS_VALUE_GET_INT(x_); %sv%d = i_ == 0 ? __JS_NewFloat64(-0.0) : i_ == INT32_MIN ? __JS_NewFloat64(2147483648.0) : JS_MKVAL(JS_TAG_INT, -i_); }\n", P, dv);
                fprintf(o, "          else if (tg_ == JS_TAG_FLOAT64) %sv%d = __JS_NewFloat64(-JS_VALUE_GET_FLOAT64(x_)); else goto %s; }\n", P, dv, xl);
                break;
              case REG_UN_INC:
                fprintf(o, "          if (tg_ == JS_TAG_INT) { int32_t i_ = JS_VALUE_GET_INT(x_); %sv%d = i_ == INT32_MAX ? __JS_NewFloat64(2147483648.0) : JS_MKVAL(JS_TAG_INT, i_ + 1); }\n", P, dv);
                fprintf(o, "          else if (tg_ == JS_TAG_FLOAT64) %sv%d = __JS_NewFloat64(JS_VALUE_GET_FLOAT64(x_) + 1); else goto %s; }\n", P, dv, xl);
                break;
              case REG_UN_DEC:
                fprintf(o, "          if (tg_ == JS_TAG_INT) { int32_t i_ = JS_VALUE_GET_INT(x_); %sv%d = i_ == INT32_MIN ? __JS_NewFloat64(-2147483649.0) : JS_MKVAL(JS_TAG_INT, i_ - 1); }\n", P, dv);
                fprintf(o, "          else if (tg_ == JS_TAG_FLOAT64) %sv%d = __JS_NewFloat64(JS_VALUE_GET_FLOAT64(x_) - 1); else goto %s; }\n", P, dv, xl);
                break;
              case REG_UN_NOT:
                fprintf(o, "          if (tg_ == JS_TAG_INT) %sv%d = JS_MKVAL(JS_TAG_INT, ~JS_VALUE_GET_INT(x_)); else goto %s; }\n", P, dv, xl);
                break;
              case REG_UN_PLUS: case REG_UN_TO_NUMERIC:
                fprintf(o, "          if (tg_ == JS_TAG_INT || tg_ == JS_TAG_FLOAT64) %sv%d = x_; else goto %s; }\n", P, dv, xl);
                break;
              case REG_UN_IS_UNDEFINED:
                fprintf(o, "          %sv%d = tg_ == JS_TAG_UNDEFINED ? JS_TRUE : JS_FALSE; }\n", P, dv);
                break;
              case REG_UN_IS_NULL:
                fprintf(o, "          %sv%d = tg_ == JS_TAG_NULL ? JS_TRUE : JS_FALSE; }\n", P, dv);
                break;
              case REG_UN_IS_UNDEFINED_OR_NULL:
                fprintf(o, "          %sv%d = tg_ == JS_TAG_UNDEFINED || tg_ == JS_TAG_NULL ? JS_TRUE : JS_FALSE; }\n", P, dv);
                break;
              case REG_UN_LNOT:
                /* every tag at or below undefined is its payload's zero test (an int, a bool, null, undefined); a double is zero or NaN; an object, a string, a symbol take the engine's */
                fprintf(o, "          if ((uint32_t)tg_ <= (uint32_t)JS_TAG_UNDEFINED) %sv%d = JS_VALUE_GET_INT(x_) == 0 ? JS_TRUE : JS_FALSE;\n", P, dv);
                fprintf(o, "          else if (tg_ == JS_TAG_FLOAT64) { double d_ = JS_VALUE_GET_FLOAT64(x_); %sv%d = d_ == 0 || d_ != d_ ? JS_TRUE : JS_FALSE; }\n", P, dv);
                /* an object is truthy (no code runs) unless it is an HTMLDDA object, which leaves */
                fprintf(o, "          else if (tg_ == JS_TAG_OBJECT && !Q2_HTMLDDA(ctx)) %sv%d = JS_FALSE; else goto %s; }\n", P, dv, xl);
                break;
              case REG_UN_TYPEOF_IS_UNDEFINED:
                fprintf(o, "          if (tg_ == JS_TAG_INT || tg_ == JS_TAG_BOOL || tg_ == JS_TAG_NULL || tg_ == JS_TAG_UNDEFINED || tg_ == JS_TAG_STRING || tg_ == JS_TAG_FLOAT64) %sv%d = tg_ == JS_TAG_UNDEFINED ? JS_TRUE : JS_FALSE; else goto %s; }\n", P, dv, xl);
                break;
              case REG_UN_TYPEOF_IS_FUNCTION:
                fprintf(o, "          if (tg_ == JS_TAG_INT || tg_ == JS_TAG_BOOL || tg_ == JS_TAG_NULL || tg_ == JS_TAG_UNDEFINED || tg_ == JS_TAG_STRING || tg_ == JS_TAG_FLOAT64) %sv%d = JS_FALSE; else goto %s; }\n", P, dv, xl);
                break;
            }
          }
          unary_done:
          break;
        }
        case ROP_binary_cpool: {
          /* "rbrbc": d, sub, a, kind, pool index: `a <sub> constant` with a number constant of the pool */
          uint32_t pidx;
          double pn = 0;
          int sub, ptag;
          char cs[96], an[48], dn[48];
          if (dv < 0 || u0 < 0 || in->narg < 5) return rg_fail(c, "pc %d: binary_cpool", in->pc);
          sub = (int)in->arg[1].val & 0x3f;
          pidx = (uint32_t)in->arg[4].val;
          ptag = in->arg[3].val == REG_CONST_CPOOL && qjs_aot_pool_num(I->g, pidx, &pn) ? qjs_aot_pool_tag(I->g, pidx) : 0;
          if (ptag == 0 || !(sub == REG_BIN_ADD || sub == REG_BIN_SUB || sub == REG_BIN_MUL || sub == REG_BIN_DIV || rg_subop(sub) != NULL)) {
            if (rg_cold(I, in)) {
              if (c->emit) fprintf(o, "        goto %s; /* binary_cpool: never executed in the profile */\n", xl);
              break;
            }
            return rg_fail(c, "pc %d: binary_cpool (sub %d, %s constant) is not in the region subset (and the profile executed it)", in->pc, sub, ptag ? "a number" : "a non-number");
          }
          if (c->emit) {
            if (ptag == 1) snprintf(cs, sizeof(cs), "JS_MKVAL(JS_TAG_INT, %d)", (int)(int32_t)pn);
            else if (pn != pn) snprintf(cs, sizeof(cs), "__JS_NewFloat64(__builtin_nan(\"\"))");
            else if (pn == __builtin_inf() || pn == -__builtin_inf()) snprintf(cs, sizeof(cs), "__JS_NewFloat64(%s__builtin_inf())", pn < 0 ? "-" : "");
            else snprintf(cs, sizeof(cs), "__JS_NewFloat64(%a)", pn);
            snprintf(an, sizeof(an), "%sv%d", I->pfx, u0);
            snprintf(dn, sizeof(dn), "%sv%d", I->pfx, dv);
            fprintf(o, "        { JSValue kc_ = %s;\n", cs);
            rg_arith(o, sub, dn, an, "kc_", xl);
            fprintf(o, "        }\n");
          }
          break;
        }
