#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emit.h"
#include "ir.h"
#include "quickjs-aot-gen.h"
#include "quickjs-aot-rt.h"
#include "quickjs-regvm.h"
#include "quickjs.h"

/* ---- source scan ---------------------------------------------------------
   Every function of the bundle that has a register program is walked once, and every store
   of a named field is listed by field name with the static type of the stored value: a
   literal int, a double, a bool, null, undefined, the result of an arithmetic operation,
   a comparison, or unknown. A name is by design a superset of the stores into one slot of a
   layout (any object with that field name), so a verdict drawn from it only errs toward
   refusing. Stores through a computed key that is not an int constant, writes of private or
   super fields, `with`, `eval`, `delete`, prototype changes and functions without a register
   program are escapes: the source cannot account for every store, so a promise rests on the
   profile and the engine hook (the verdict is CONSISTENT, else PROVEN). */
enum { SC_INT = 1, SC_DBL = 2, SC_NUM = 4, SC_BOOL = 8, SC_NULL = 16, SC_UNDEF = 32, SC_UNK = 64 };
#define SCAN_NAMES 512
typedef struct ScanName { char name[48]; unsigned mask; int nstores; } ScanName;
/* A store whose receiver is `this`, by the function it is in: another constructor's own stores do
   not count against a layout built by a different constructor. */
typedef struct ScanThis { int name; unsigned cls; uint8_t digest[16]; } ScanThis;
#define SCAN_THIS 4096
static ScanThis g_scan_this[SCAN_THIS];
static int g_scan_nthis;
static ScanName g_scan_names[SCAN_NAMES];
static int g_scan_nnames, g_scan_done, g_scan_escapes, g_scan_opaque, g_scan_funcs;
static JSContext *g_unit_ctx;
static const JSAotGenFunc *g_unit_funcs;
static int g_unit_nfuncs;

static ScanName *scan_name(const char *s) {
  int i;
  for (i = 0; i < g_scan_nnames; i++)
    if (strcmp(g_scan_names[i].name, s) == 0) return &g_scan_names[i];
  if (g_scan_nnames >= SCAN_NAMES) return NULL;
  memset(&g_scan_names[g_scan_nnames], 0, sizeof(g_scan_names[0]));
  snprintf(g_scan_names[g_scan_nnames].name, sizeof(g_scan_names[0].name), "%s", s);
  return &g_scan_names[g_scan_nnames++];
}

static unsigned scan_class(const Q2Func *fn, Q2ValueId v) {
  const Q2Value *val;
  if (v < 0 || v >= fn->nvalues) return SC_UNK;
  val = &fn->values[v];
  if (val->is_const) {
    switch (val->con_kind) {
      case Q2_CON_INT: return SC_INT;
      case Q2_CON_DOUBLE: return SC_DBL;
      case Q2_CON_TRUE:
      case Q2_CON_FALSE: return SC_BOOL;
      case Q2_CON_NULL: return SC_NULL;
      case Q2_CON_UNDEF: return SC_UNDEF;
    }
    return SC_UNK;
  }
  if (val->insn >= 0 && val->insn < fn->ninsns) {
    const Q2Insn *d = &fn->insns[val->insn];
    switch (d->rop) {
      case ROP_load_undefined: return SC_UNDEF;
      case ROP_load_null: return SC_NULL;
      case ROP_load_true:
      case ROP_load_false: return SC_BOOL;
      case ROP_load_i8:
      case ROP_load_i8_s:
      case ROP_load_i32: return SC_INT;
    }
    if (d->op == Q2_OP_COMPARE) return SC_BOOL;
    if (d->op == Q2_OP_BINARY && d->sub >= 0 && d->sub != REG_BIN_ADD) return SC_NUM;
  }
  return SC_UNK;
}

static void scan_run(void) {
  int i;
  if (g_scan_done) return;
  g_scan_done = 1;
  if (g_unit_funcs == NULL || g_unit_ctx == NULL) return;
  for (i = 0; i < g_unit_nfuncs; i++) {
    const JSAotGenFunc *g = &g_unit_funcs[i];
    Q2Func fn;
    int k;
    if (i + 1 < g_unit_nfuncs && memcmp(g->digest, g_unit_funcs[i + 1].digest, 16) == 0) continue;
    if (g->eligible != AOT_GEN_ELIGIBLE) { g_scan_opaque++; continue; }
    if (q2_build(g, &fn) < 0) { q2_free(&fn); g_scan_opaque++; continue; }
    g_scan_funcs++;
    for (k = 0; k < fn.ninsns; k++) {
      const Q2Insn *in = &fn.insns[k];
      switch (in->rop) {
        case ROP_put_field:
        case ROP_put_field_xfer:
        case ROP_define_field: {
          int a;
          for (a = 0; a < in->narg && in->arg[a].kind != 'a'; a++) {}
          if (a < in->narg) {
            size_t len;
            const char *nm = JS_AtomToCStringLen(g_unit_ctx, &len, (JSAtom)in->arg[a].val);
            ScanName *sn = nm != NULL ? scan_name(nm) : NULL;
            if (nm != NULL) JS_FreeCString(g_unit_ctx, nm);
            if (sn == NULL) { g_scan_escapes++; break; }
            unsigned cls = in->rop == ROP_define_field || in->narg < 4 || in->arg[3].kind != 'r'
                               ? SC_UNK : scan_class(&fn, in->argval[3]);
            int isthis = 0;
            if (in->arg[0].kind == 'r' && in->argval[0] >= 0 && in->argval[0] < fn.nvalues) {
              int di = fn.values[in->argval[0]].insn;
              isthis = di >= 0 && di < fn.ninsns && fn.insns[di].rop == ROP_load_this;
            }
            sn->nstores++;
            if (isthis && g_scan_nthis < SCAN_THIS) {
              ScanThis *st = &g_scan_this[g_scan_nthis++];
              st->name = (int)(sn - g_scan_names);
              st->cls = cls;
              memcpy(st->digest, g->digest, 16);
            } else {
              sn->mask |= cls;
            }
          } else {
            g_scan_escapes++;
          }
          break;
        }
        case ROP_put_element:
        case ROP_put_element_xfer: {
          const Q2Value *kv = in->narg > 1 && in->arg[1].kind == 'r' && in->argval[1] >= 0 ? &fn.values[in->argval[1]] : NULL;
          if (kv == NULL || !kv->is_const || kv->con_kind != Q2_CON_INT) g_scan_escapes++;
          break;
        }
        case ROP_define_element:
        case ROP_put_private:
        case ROP_define_private:
        case ROP_put_super_value:
        case ROP_super_put:
        case ROP_put_ref:
        case ROP_put_ref_value:
        case ROP_copy_props:
        case ROP_set_proto:
        case ROP_delete:
        case ROP_eval:
        case ROP_apply_eval:
        case ROP_with_put:
        case ROP_with_delete:
        case ROP_object_fill:
        case ROP_object_template:
          g_scan_escapes++;
          break;
        default: break;
      }
    }
    q2_free(&fn);
  }
}

/* The verdict on a promise for a slot named `name`: -1 contradicted (some store visibly puts
   another type there), 1 proven, 2 consistent, 0 the source says nothing. */
int qjs_aot_scan_verdict(const char *name, int claim, const uint8_t own[16], const uint8_t (*ctors)[16], int nctors) {
  const ScanName *sn = NULL;
  int i;
  unsigned seen, allowed, definite;
  if (getenv("QJS_AOT_NOSCAN") != NULL) return 0;
  scan_run();
  for (i = 0; i < g_scan_nnames; i++)
    if (strcmp(g_scan_names[i].name, name) == 0) sn = &g_scan_names[i];
  if (getenv("QJS_AOT_SCANDEBUG") && i == g_scan_nnames) fprintf(stderr, "scan %s: stores=%d mask=%x esc=%d opaque=%d funcs=%d\n", name, sn ? sn->nstores : -1, sn ? sn->mask : 0, g_scan_escapes, g_scan_opaque, g_scan_funcs);
  if (sn == NULL || sn->nstores == 0) return 0;
  seen = sn->mask;
  for (i = 0; i < g_scan_nthis; i++) {
    int j, other = 0;
    if (g_scan_this[i].name != (int)(sn - g_scan_names)) continue;
    if (memcmp(g_scan_this[i].digest, own, 16) != 0)
      for (j = 0; j < nctors; j++)
        if (memcmp(g_scan_this[i].digest, ctors[j], 16) == 0) other = 1;
    if (!other) seen |= g_scan_this[i].cls;
  }
  if (claim == QJS_AOT_CLAIM_INT) {
    allowed = SC_INT; definite = SC_DBL | SC_BOOL | SC_NULL | SC_UNDEF;
  } else {
    allowed = SC_INT | SC_DBL | SC_NUM | SC_BOOL | SC_NULL | SC_UNDEF; definite = 0;
  }
  if (seen & definite) return -1;
  if ((seen & ~allowed) == 0) return g_scan_escapes == 0 && g_scan_opaque == 0 ? 1 : 2;
  return 0;
}

void qjs_aot_scan_init(JSContext *ctx, const JSAotGenFunc *funcs, int n) {
  g_unit_ctx = ctx;
  g_unit_funcs = funcs;
  g_unit_nfuncs = n;
}
