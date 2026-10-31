/* region.c fragment, included inside the emit switch of region_emit1 before its default: closure variables.
   get_var_ref (a read of a captured variable, "ru": d, slot) in the ROOT: the cell of the function being run (the function object the frame names), read EVERY time (nothing is
   assumed about a variable another closure or an out-of-line call can write), a TDZ cell leaves at the read so the generic body raises the ReferenceError. The value is borrowed
   from the cell, and a cell write can free it while the region still holds it: so a reference is taken and parked in the owned list (freed at the region's end, an exit, or
   the loop's compaction), exactly as a store's replaced value is. An inlined callee has its own cell vector (it is not the root's function): refused. */
        case ROP_get_var_ref: {
          int slot_ = in->narg >= 2 ? (int)in->arg[1].val : -1;
          if (dv < 0 || slot_ < 0) return rg_fail(c, "pc %d: get_var_ref", in->pc);
          if (!is_root) return rg_fail(c, "pc %d: get_var_ref in an inlined callee (its cell vector is the callee's, not the root's)", in->pc);
          if (getenv("QJS_AOT_REGION_NOVARREF") != NULL) return rg_fail(c, "pc %d: get_var_ref is not in the region subset (and the profile executed it)", in->pc);
          rg_note_park(c, ii);
          if (c->emit) {
            fprintf(o, "        if (__builtin_expect(rgon_ >= RG_OWNCAP, 0)) goto %s;   /* the owned list is full: leave before the read */\n", xl);
            fprintf(o, "        { JSValue t_ = q2_vref_get(*(JSObject **)((uint8_t *)f + QJS_AOT_OFF_REGRUN_FUNC), %d);\n"
                       "          if (__builtin_expect(JS_VALUE_GET_TAG(t_) == JS_TAG_UNINITIALIZED, 0)) goto %s;\n"
                       "          %s%sv%d = t_;\n"
                       "          rg_own_dup(ctx, &rgo_, &rgon_, &rgoc_, t_); }\n", slot_, xl,
                    /* QJS_AOT_REGION_VARREFSTALE (a vkit knock-out): the cell is read once, ever, and that value is used from then on */
                    getenv("QJS_AOT_REGION_VARREFSTALE") != NULL ? "{ static JSValue kc_; static int kd_; if (!kd_) { kc_ = t_; kd_ = 1; } t_ = kc_; } " : "", I->pfx, dv);
          }
          break;
        }
