/* A shape built for a list of atoms must find each of them through the AOT slot lookups:
   they hash an atom the way the shape table does (string atoms and integer atoms alike). */
#include <stdio.h>
#include <string.h>
#include "quickjs.h"
#include "quickjs-aot-rt.h"

int main(void) {
  JSRuntime *rt = JS_NewRuntime();
  JSContext *ctx = JS_NewContext(rt);
  uint32_t atoms[64];
  char name[16];
  int n = 0, bad = 0;
  for (int i = 0; i < 6; i++) { snprintf(name, sizeof name, "p%d", i); atoms[n++] = JS_NewAtom(ctx, name); }
  /* integer atoms that are multiples of a power of two, then enough atoms that the shape's hash table
     is resized more than twice, and integer atoms added after the resizes */
  for (int i = 1; i <= 8; i++) atoms[n++] = JS_NewAtomUInt32(ctx, (uint32_t)i << 10);
  for (int i = 6; i < 30; i++) { snprintf(name, sizeof name, "p%d", i); atoms[n++] = JS_NewAtom(ctx, name); }
  for (int i = 1; i <= 12; i++) atoms[n++] = JS_NewAtomUInt32(ctx, (uint32_t)(2 * i + 1) << 12);
  void *sh = qjs_aot_class_shape(ctx, JS_NewObject(ctx), atoms, n);
  if (!sh) { puts("FAIL no shape"); return 1; }
  for (int i = 0; i < n; i++) {
    uint32_t off = 99, roff = 99, woff = 99;
    if (!qjs_aot_shape_slot(sh, atoms[i], &off) || !qjs_aot_shape_rslot(sh, atoms[i], &roff) ||
        !qjs_aot_shape_wslot(sh, atoms[i], &woff) || off != (uint32_t)i * 16u) {
      printf("FAIL atom %d (%u) not found or at offset %u\n", i, atoms[i], off);
      bad++;
    }
  }
  if (!bad) printf("PASS %d atoms found by slot, rslot and wslot\n", n);
  return bad != 0;
}
