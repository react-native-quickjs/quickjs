/* region.c fragment, included inside the emit switch of region_emit1: compares, add/mul/inc and the immediate forms (binary_imm_s, add_imm_s). */
        case ROP_lt: case ROP_lte: case ROP_gt: case ROP_gte: case ROP_eq: case ROP_neq: case ROP_strict_eq: case ROP_strict_neq: {
          if (dv < 0 || u0 < 0 || u1 < 0) return rg_fail(c, "pc %d: compare", in->pc);
          if (c->emit) {
            if (in->rop == ROP_lt || in->rop == ROP_lte || in->rop == ROP_gt || in->rop == ROP_gte) {
              /* an order compare: ints as ints, other numbers as doubles, anything else leaves */
              char a_[24], b_[24], d_[24];
              snprintf(a_, sizeof(a_), "%sv%d", I->pfx, u0); snprintf(b_, sizeof(b_), "%sv%d", I->pfx, u1); snprintf(d_, sizeof(d_), "%sv%d", I->pfx, dv);
              rg_arith(o, in->rop == ROP_lt ? REG_BIN_LT : in->rop == ROP_lte ? REG_BIN_LTE : in->rop == ROP_gt ? REG_BIN_GT : REG_BIN_GTE, d_, a_, b_, xl);
            } else {
            fprintf(o, "        { int32_t x_, y_; if (__builtin_expect(JS_VALUE_GET_TAG(%sv%d) != JS_TAG_INT || JS_VALUE_GET_TAG(%sv%d) != JS_TAG_INT, 0)) goto %s;\n", I->pfx, u0, I->pfx, u1, xl);
            fprintf(o, "          x_ = JS_VALUE_GET_INT(%sv%d); y_ = JS_VALUE_GET_INT(%sv%d); %sv%d = x_ %s y_ ? JS_TRUE : JS_FALSE; }\n", I->pfx, u0, I->pfx, u1, I->pfx, dv, rg_cmpop(in->rop));
            }
          }
          break;
        }
        case ROP_add: case ROP_sub: case ROP_mul: case ROP_div: {
          /* as the engine's own binary op: two ints give an int when the result is one (a division too), otherwise a double that is not normalised */
          const char *opc = in->rop == ROP_add ? "+" : in->rop == ROP_sub ? "-" : in->rop == ROP_mul ? "*" : "/";
          if (dv < 0 || u0 < 0 || u1 < 0) return rg_fail(c, "pc %d: arithmetic", in->pc);
          if (c->emit) {
            fprintf(o, "        { double x_, y_; int ia_ = JS_VALUE_GET_TAG(%sv%d) == JS_TAG_INT, ib_ = JS_VALUE_GET_TAG(%sv%d) == JS_TAG_INT;\n", I->pfx, u0, I->pfx, u1);
            fprintf(o, "          if (ia_ && ib_) {\n");
            if (in->rop == ROP_div) {
              fprintf(o, "            double d_ = (double)JS_VALUE_GET_INT(%sv%d) / (double)JS_VALUE_GET_INT(%sv%d); int32_t t_ = (int32_t)d_;\n", I->pfx, u0, I->pfx, u1);
              fprintf(o, "            %sv%d = (d_ == (double)t_ && !(t_ == 0 && __builtin_signbit(d_)) && d_ >= -2147483648.0 && d_ <= 2147483647.0) ? JS_MKVAL(JS_TAG_INT, t_) : __JS_NewFloat64(d_);\n", I->pfx, dv);
            } else {
              fprintf(o, "            int64_t r_ = (int64_t)JS_VALUE_GET_INT(%sv%d) %s (int64_t)JS_VALUE_GET_INT(%sv%d);\n", I->pfx, u0, opc, I->pfx, u1);
              if (in->rop == ROP_mul)
                fprintf(o, "            if (r_ == 0 && (JS_VALUE_GET_INT(%sv%d) < 0 || JS_VALUE_GET_INT(%sv%d) < 0)) %sv%d = __JS_NewFloat64(-0.0); else\n", I->pfx, u0, I->pfx, u1, I->pfx, dv);
              fprintf(o, "            %sv%d = (r_ >= INT32_MIN && r_ <= INT32_MAX) ? JS_MKVAL(JS_TAG_INT, (int32_t)r_) : __JS_NewFloat64((double)r_);\n", I->pfx, dv);
            }
            if (in->rop == ROP_add && rg_trust_on() && (!fn->values[u0].has_prof || !fn->values[u1].has_prof || ((fn->values[u0].ptypes | fn->values[u1].ptypes) & Q2_T_STR))) rg_note_park(c, ii);
            if (in->rop == ROP_add && rg_trust_on() && (!fn->values[u0].has_prof || !fn->values[u1].has_prof || ((fn->values[u0].ptypes | fn->values[u1].ptypes) & Q2_T_STR)))
              /* a string with a string or a primitive: the engine's concatenation, no code runs; the new string is held to the region's end */
              fprintf(o, "          } else if (JS_VALUE_GET_TAG(%sv%d) == JS_TAG_STRING || JS_VALUE_GET_TAG(%sv%d) == JS_TAG_STRING) {\n"
                         "            JSValue s_; int r_ = qjs_aot_add_str(ctx, %sv%d, %sv%d, &s_);\n"
                         "            if (__builtin_expect(r_ < 0, 0)) { RG_FREE_OWNED(); return JS_EXCEPTION; }\n"
                         "            if (__builtin_expect(r_ == 0 || rgon_ >= RG_OWNCAP, 0)) { if (r_ > 0) JS_FreeValue(ctx, s_); goto %s; }\n"
                         "            rg_own_put(ctx, &rgo_, &rgon_, &rgoc_, s_); %sv%d = s_;\n", I->pfx, u0, I->pfx, u1, I->pfx, u0, I->pfx, u1, xl, I->pfx, dv);
            fprintf(o, "          } else {\n");
            fprintf(o, "          if (ia_) x_ = JS_VALUE_GET_INT(%sv%d); else if (JS_VALUE_GET_TAG(%sv%d) == JS_TAG_FLOAT64) x_ = JS_VALUE_GET_FLOAT64(%sv%d); else goto %s;\n", I->pfx, u0, I->pfx, u0, I->pfx, u0, xl);
            fprintf(o, "          if (ib_) y_ = JS_VALUE_GET_INT(%sv%d); else if (JS_VALUE_GET_TAG(%sv%d) == JS_TAG_FLOAT64) y_ = JS_VALUE_GET_FLOAT64(%sv%d); else goto %s;\n", I->pfx, u1, I->pfx, u1, I->pfx, u1, xl);
            fprintf(o, "          %sv%d = __JS_NewFloat64(x_ %s y_); } }\n", I->pfx, dv, opc);
          }
          break;
        }
        case ROP_mul_add: case ROP_mul_sub: case ROP_add_mul: case ROP_sub_mul: {
          /* the fused forms: dm = x * y, then da = c +/- dm (add_mul, sub_mul) or dm +/- c (mul_add, mul_sub), each as the engine's own binary op */
          char x_[24], y_[24], c_[24], m_[24], d_[24];
          int u2_ = in->nuse >= 3 ? (int)in->use[2] : -1, cf_ = in->rop == ROP_add_mul || in->rop == ROP_sub_mul;
          int add_ = in->rop == ROP_mul_add || in->rop == ROP_add_mul;
          if (dv < 0 || dv2 < 0 || u0 < 0 || u1 < 0 || u2_ < 0) return rg_fail(c, "pc %d: a fused multiply", in->pc);
          if (c->emit) {
            snprintf(x_, sizeof(x_), "%sv%d", I->pfx, u0);
            snprintf(y_, sizeof(y_), "%sv%d", I->pfx, u1);
            snprintf(c_, sizeof(c_), "%sv%d", I->pfx, u2_);
            snprintf(m_, sizeof(m_), "%sv%d", I->pfx, dv);
            snprintf(d_, sizeof(d_), "%sv%d", I->pfx, dv2);
            rg_arith(o, REG_BIN_MUL, m_, x_, y_, xl);
            rg_arith(o, add_ ? REG_BIN_ADD : REG_BIN_SUB, d_, cf_ ? c_ : m_, cf_ ? m_ : c_, xl);
          }
          break;
        }
        case ROP_inc: case ROP_dec:
          if (dv < 0 || u0 < 0) return rg_fail(c, "pc %d: inc", in->pc);
          if (c->emit)
            fprintf(o, "        { int32_t r_; if (__builtin_expect(JS_VALUE_GET_TAG(%sv%d) != JS_TAG_INT || __builtin_%s_overflow(JS_VALUE_GET_INT(%sv%d), 1, &r_), 0)) goto %s; %sv%d = JS_MKVAL(JS_TAG_INT, r_); }\n",
                    I->pfx, u0, in->rop == ROP_inc ? "add" : "sub", I->pfx, u0, xl, I->pfx, dv);
          break;
        case ROP_binary_imm_s: case ROP_binary_imm:
          /* the additive forms are add_imm_s / sub_imm_s (an int32 result or an exit); a product follows ROP_mul on two ints */
          if (in->narg >= 4 && in->arg[1].kind == 'b' && (int)in->arg[1].val == REG_BIN_MUL && dv >= 0 && u0 >= 0) {
            int32_t km = (int32_t)in->arg[3].val;
            if (c->emit)
              fprintf(o, "        { int64_t r_; if (__builtin_expect(JS_VALUE_GET_TAG(%sv%d) != JS_TAG_INT, 0)) goto %s; r_ = (int64_t)JS_VALUE_GET_INT(%sv%d) * (int64_t)%d;\n"
                         "          if (r_ == 0 && (JS_VALUE_GET_INT(%sv%d) < 0 || %d < 0)) %sv%d = __JS_NewFloat64(-0.0); else\n"
                         "          %sv%d = (r_ >= INT32_MIN && r_ <= INT32_MAX) ? JS_MKVAL(JS_TAG_INT, (int32_t)r_) : __JS_NewFloat64((double)r_); }\n",
                      I->pfx, u0, xl, I->pfx, u0, (int)km, I->pfx, u0, (int)km, I->pfx, dv, I->pfx, dv);
            break;
          }
          if (in->narg >= 4 && in->arg[1].kind == 'b' && (int)in->arg[1].val == REG_BIN_DIV && dv >= 0 && u0 >= 0) {
            /* a quotient by an int immediate: as the binary op with an int constant */
            if (c->emit) {
              char a_[24], d_[24];
              snprintf(a_, sizeof(a_), "%sv%d", I->pfx, u0);
              snprintf(d_, sizeof(d_), "%sv%d", I->pfx, dv);
              fprintf(o, "        { JSValue kc_ = JS_MKVAL(JS_TAG_INT, %d);\n", (int)(int32_t)in->arg[3].val);
              rg_arith(o, REG_BIN_DIV, d_, a_, "kc_", xl);
              fprintf(o, "        }\n");
            }
            break;
          }
          if (in->narg >= 4 && in->arg[1].kind == 'b' && rg_bitimm(c, I, in, dv, u0, xl)) break;   /* region_fn_bit.inc: the bit operations with an immediate */
          if (in->narg < 4 || in->arg[1].kind != 'b' || ((int)in->arg[1].val != REG_BIN_ADD && (int)in->arg[1].val != REG_BIN_SUB)) {
            char nm[48];
            e_rop_name((uint16_t)in->rop, nm, sizeof(nm));
            if (rg_cold(I, in)) {
              if (c->emit) fprintf(o, "        goto %s; /* %s: never executed in the profile */\n", xl, nm);
              break;
            }
            return rg_fail(c, "pc %d: %s (sub %d) is not in the region subset (and the profile executed it)", in->pc, nm, in->narg >= 2 ? (int)in->arg[1].val : -1);
          }
          /* fall through */
        case ROP_add_imm_s: case ROP_add_imm_self_s: case ROP_sub_imm_s: case ROP_sub_imm_self_s: {
          int32_t k = 0;
          int add = in->rop == ROP_add_imm_s || in->rop == ROP_add_imm_self_s || ((in->rop == ROP_binary_imm_s || in->rop == ROP_binary_imm) && (int)in->arg[1].val == REG_BIN_ADD), ai, found = 0;
          for (ai = 0; ai < in->narg; ai++) if (in->arg[ai].kind == 'i' || in->arg[ai].kind == 'I') { k = (int32_t)in->arg[ai].val; found = 1; }
          if (dv < 0 || u0 < 0 || !found) return rg_fail(c, "pc %d: add_imm dv=%d u0=%d narg=%d", in->pc, dv, u0, in->narg);
          if (c->emit && rg_trust_on()) {
            /* TRUST: as the binary op with an int constant: an int result while it fits, a double otherwise or for a double operand */
            char a_[24], d_[24];
            snprintf(a_, sizeof(a_), "%sv%d", I->pfx, u0);
            snprintf(d_, sizeof(d_), "%sv%d", I->pfx, dv);
            fprintf(o, "        { JSValue kc_ = JS_MKVAL(JS_TAG_INT, %d);\n", (int)k);
            rg_arith(o, add ? REG_BIN_ADD : REG_BIN_SUB, d_, a_, "kc_", xl);
            fprintf(o, "        }\n");
          } else if (c->emit)
            fprintf(o, "        { int32_t r_; if (__builtin_expect(JS_VALUE_GET_TAG(%sv%d) != JS_TAG_INT || __builtin_%s_overflow(JS_VALUE_GET_INT(%sv%d), %d, &r_), 0)) goto %s; %sv%d = JS_MKVAL(JS_TAG_INT, r_); }\n",
                    I->pfx, u0, add ? "add" : "sub", I->pfx, u0, (int)k, xl, I->pfx, dv);
          break;
        }
