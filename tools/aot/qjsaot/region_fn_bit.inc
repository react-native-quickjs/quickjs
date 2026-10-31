/* region.c fragment at file scope, before region_emit1: helpers of the bit operations, TDZ, constants and literals (M1).
   Gen-time knobs: QJS_AOT_REGION_NOBITIMM / NOBITREG / NOTDZ / NOLITCONST / NOOBJLIT switch the family off (the instruction is refused, or leaves when cold).
   QJS_AOT_REGION_KNOCKBUILD=1 emits every family with a run-time switch of the same name (set at run time): the emitted code then takes a deliberately WRONG shortcut,
   so a vkit case can prove its test has teeth. Without KNOCKBUILD no getenv is emitted. */
static int rg_knobgen(const char *name) { return getenv(name) != NULL; }
static int rg_knockbuild(void) { static int v = -1; if (v < 0) v = getenv("QJS_AOT_REGION_KNOCKBUILD") != NULL; return v; }

/* `int32_t var` = ToInt32 of the number in `a` (C text of a JSValue); any other tag leaves through xl. The double path is q2_toi32 (total, no exit). */
static void rg_toi32(FILE *o, const char *var, const char *a, const char *xl, const char *knob) {
  if (knob != NULL && rg_knockbuild())
    fprintf(o, "          if (getenv(\"%s\")) %s = JS_VALUE_GET_TAG(%s) == JS_TAG_INT ? JS_VALUE_GET_INT(%s) : JS_VALUE_GET_TAG(%s) == JS_TAG_FLOAT64 ? (int32_t)JS_VALUE_GET_FLOAT64(%s) : 0; else\n", knob, var, a, a, a, a);
  fprintf(o, "          if (JS_VALUE_GET_TAG(%s) == JS_TAG_INT) %s = JS_VALUE_GET_INT(%s); else if (JS_VALUE_GET_TAG(%s) == JS_TAG_FLOAT64) %s = q2_toi32(JS_VALUE_GET_FLOAT64(%s)); else goto %s;\n", a, var, a, a, var, a, xl);
}

/* dst = a <sub> b for OR AND XOR SHL SAR SHR MOD; b is C text of a JSValue, or NULL for the immediate `imm`. Returns 0 when sub is none of them. */
static int rg_bitop(FILE *o, int sub, const char *dst, const char *a, const char *b, int32_t imm, const char *xl, const char *knob) {
  switch (sub) {
    case REG_BIN_OR: case REG_BIN_AND: case REG_BIN_XOR: case REG_BIN_SHL: case REG_BIN_SAR: case REG_BIN_SHR: case REG_BIN_MOD: break;
    default: return 0;
  }
  fprintf(o, "        { int32_t x_, y_;\n");
  if (sub == REG_BIN_MOD) {
    /* ints only: JS's remainder takes the sign of the dividend, so a zero remainder of a negative dividend is -0; a zero divisor is NaN */
    fprintf(o, "          if (JS_VALUE_GET_TAG(%s) != JS_TAG_INT", a);
    if (b != NULL) fprintf(o, " || JS_VALUE_GET_TAG(%s) != JS_TAG_INT", b);
    fprintf(o, ") goto %s;\n          x_ = JS_VALUE_GET_INT(%s); y_ = ", xl, a);
    if (b != NULL) fprintf(o, "JS_VALUE_GET_INT(%s);\n", b); else fprintf(o, "%d;\n", (int)imm);
    fprintf(o, "          if (y_ == 0) %s = __JS_NewFloat64(__builtin_nan(\"\"));\n", dst);
    fprintf(o, "          else { int32_t r_ = y_ == -1 ? 0 : x_ %% y_;\n");
    if (knob != NULL && rg_knockbuild())
      fprintf(o, "            if (r_ == 0 && x_ < 0 && !getenv(\"%s\")) %s = __JS_NewFloat64(-0.0); else %s = JS_MKVAL(JS_TAG_INT, r_); } }\n", knob, dst, dst);
    else
      fprintf(o, "            if (r_ == 0 && x_ < 0) %s = __JS_NewFloat64(-0.0); else %s = JS_MKVAL(JS_TAG_INT, r_); } }\n", dst, dst);
    return 1;
  }
  rg_toi32(o, "x_", a, xl, knob);
  if (b != NULL) rg_toi32(o, "y_", b, xl, knob); else fprintf(o, "          y_ = %d;\n", (int)imm);
  switch (sub) {
    case REG_BIN_OR: fprintf(o, "          %s = JS_MKVAL(JS_TAG_INT, x_ | y_); }\n", dst); break;
    case REG_BIN_AND: fprintf(o, "          %s = JS_MKVAL(JS_TAG_INT, x_ & y_); }\n", dst); break;
    case REG_BIN_XOR: fprintf(o, "          %s = JS_MKVAL(JS_TAG_INT, x_ ^ y_); }\n", dst); break;
    case REG_BIN_SHL: fprintf(o, "          %s = JS_MKVAL(JS_TAG_INT, (int32_t)((uint32_t)x_ << ((uint32_t)y_ & 31))); }\n", dst); break;
    case REG_BIN_SAR: fprintf(o, "          %s = JS_MKVAL(JS_TAG_INT, x_ >> ((uint32_t)y_ & 31)); }\n", dst); break;
    case REG_BIN_SHR:
      /* the logical shift: an int while it fits, a double above 2^31 */
      fprintf(o, "          { uint32_t u_ = (uint32_t)x_ >> ((uint32_t)y_ & 31); %s = u_ <= (uint32_t)INT32_MAX ? JS_MKVAL(JS_TAG_INT, (int32_t)u_) : __JS_NewFloat64((double)u_); } }\n", dst);
      break;
  }
  return 1;
}

/* `dv = u0 <op> imm` for the sub-operation in in->arg[1]; returns 1 after emitting, 0 to leave the instruction to the refusal path */
/* dst = (a <cmp> imm) as a boolean VALUE (the branch forms are handled by the terminators). Numbers compare directly; for the strict forms any other tag is a different type
   (never equal); for the loose forms null and undefined are never equal to a number; every other case leaves through xl. */
static int rg_cmpimm(RCtx *c, const RInst *I, const Q2Insn *in, int dv, int u0, const char *xl, int sub) {
  const char *op = NULL;
  int strict = sub == REG_BIN_STRICT_EQ || sub == REG_BIN_STRICT_NEQ, negate = sub == REG_BIN_NEQ || sub == REG_BIN_STRICT_NEQ, eqf = strict || sub == REG_BIN_EQ || sub == REG_BIN_NEQ;
  int32_t imm = (int32_t)in->arg[3].val;
  if (rg_knobgen("QJS_AOT_REGION_NOCMPIMM") || !rg_knobgen("QJS_AOT_REGION_CMPIMM")) return 0;   /* OFF by default until the richards family is at or below its baseline with it (mailbox MSG 140) */
  switch (sub) {
    case REG_BIN_LT: op = "<"; break;
    case REG_BIN_LTE: op = "<="; break;
    case REG_BIN_GT: op = ">"; break;
    case REG_BIN_GTE: op = ">="; break;
    case REG_BIN_EQ: case REG_BIN_STRICT_EQ: op = "=="; break;
    case REG_BIN_NEQ: case REG_BIN_STRICT_NEQ: op = "!="; break;
    default: return 0;
  }
  if (c->emit) {
    FILE *o = c->o;
    char a_[24], d_[24];
    snprintf(a_, sizeof(a_), "%sv%d", I->pfx, u0);
    snprintf(d_, sizeof(d_), "%sv%d", I->pfx, dv);
    fprintf(o, "        { int c_;\n");
    if (rg_knockbuild()) fprintf(o, "          if (getenv(\"QJS_AOT_REGION_NOCMPIMM\") && JS_VALUE_GET_TAG(%s) == JS_TAG_INT) c_ = JS_VALUE_GET_INT(%s) %s %d + 1; else\n", a_, a_, op, (int)imm);
    fprintf(o, "          if (JS_VALUE_GET_TAG(%s) == JS_TAG_INT) c_ = JS_VALUE_GET_INT(%s) %s %d;\n", a_, a_, op, (int)imm);
    fprintf(o, "          else if (JS_VALUE_GET_TAG(%s) == JS_TAG_FLOAT64) c_ = JS_VALUE_GET_FLOAT64(%s) %s (double)%d;\n", a_, a_, op, (int)imm);
    if (strict) fprintf(o, "          else c_ = %d;   /* a different type is never strictly equal */\n", negate ? 1 : 0);
    else if (eqf) fprintf(o, "          else if (JS_VALUE_GET_TAG(%s) == JS_TAG_NULL || JS_VALUE_GET_TAG(%s) == JS_TAG_UNDEFINED) c_ = %d;   /* null and undefined are never == a number */\n          else goto %s;\n", a_, a_, negate ? 1 : 0, xl);
    else fprintf(o, "          else goto %s;\n", xl);
    fprintf(o, "          %s = JS_MKVAL(JS_TAG_BOOL, c_); }\n", d_);
  }
  return 1;
}

static int rg_bitimm(RCtx *c, const RInst *I, const Q2Insn *in, int dv, int u0, const char *xl) {
  char a_[24], d_[24];
  int sub;
  if (rg_knobgen("QJS_AOT_REGION_NOBITIMM")) return 0;
  if (dv < 0 || u0 < 0 || in->narg < 4 || in->arg[1].kind != 'b' || (in->arg[3].kind != 'i' && in->arg[3].kind != 'I')) return 0;
  sub = (int)in->arg[1].val;
  if (sub >= REG_BIN_LT && sub != REG_BIN_INSTANCEOF && sub != REG_BIN_IN && sub != REG_BIN_PRIVATE_IN) return rg_cmpimm(c, I, in, dv, u0, xl, sub);
  if (sub != REG_BIN_OR && sub != REG_BIN_AND && sub != REG_BIN_XOR && sub != REG_BIN_SHL && sub != REG_BIN_SAR && sub != REG_BIN_SHR && sub != REG_BIN_MOD) return 0;
  if (c->emit) {
    snprintf(a_, sizeof(a_), "%sv%d", I->pfx, u0);
    snprintf(d_, sizeof(d_), "%sv%d", I->pfx, dv);
    rg_bitop(c->o, sub, d_, a_, NULL, (int32_t)in->arg[3].val, xl, "QJS_AOT_REGION_NOBITIMM");
  }
  return 1;
}

/* Whether the atom's name is a plain identifier-like string (printable ASCII, not an integer index, short): the unit can make the same atom from the text. */
static int rg_litname_ok(uint32_t atom) {
  char nm[64];
  size_t i, n;
  int digits = 1;
  if (!qjs_aot_bundle_atom_name(atom, nm, sizeof(nm))) return 0;
  n = strlen(nm);
  if (n == 0 || n >= sizeof(nm) - 1) return 0;
  for (i = 0; i < n; i++) {
    unsigned char ch = (unsigned char)nm[i];
    if (ch < 0x21 || ch > 0x7e || ch == '"' || ch == '\\') return 0;
    if (ch < '0' || ch > '9') digits = 0;
  }
  return !digits;
}
