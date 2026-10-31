/*
 * The proof backend (AOT-REDESIGN.md R2): fast bodies for leaf functions, built on the bundle analysis.
 */
#ifndef QJSAOT_FAST_H
#define QJSAOT_FAST_H

#include <stdint.h>
#include <stdio.h>

#include "ir.h"

/* bundle.c: the code generator's view of the bundle analysis. */
void qjs_aot_bundle_init(JSContext *ctx, const JSAotGenFunc *funcs, int n);
int qjs_aot_bundle_report(const char *path);
int qjs_aot_bundle_field(const JSAotGenFunc *g, int pc, int kind, int *slot, unsigned *cls, int *is_add);
int qjs_aot_bundle_field_named(const JSAotGenFunc *g, int pc, int kind, const char *name, int *slot, unsigned *cls, int *is_add);
int qjs_aot_bundle_cls_plain(unsigned cls);
int qjs_aot_bundle_atom_name(uint32_t atom, char *out, size_t cap);
int qjs_aot_bundle_targets(const JSAotGenFunc *g, int pc, const JSAotGenFunc **out, int max);
int qjs_aot_bundle_thru_targets(const JSAotGenFunc *g, int pc, const JSAotGenFunc **out, int max);

/* Class-keyed facts (bundle.c). One receiver class = one abstract allocation site (a constructor call or an object/array literal). */
typedef struct QjsAotRecvClass {
  int site;                 /* the allocation site's id (stable within one generator run) */
  char label[80];           /* "new Foo@pc" or the literal's label */
  char ctor_global[48];     /* the constructor's global name when the constructor function is reachable as a global, else "" */
  int proto_site;           /* the prototype object's site id when the class has exactly one prototype object, else -1 */
  int one_shape;            /* every field is added unconditionally in the construction walk and no other code adds a field */
  int nfields;              /* fields added by the construction walk, in order (slot i = i-th add) */
  int escapes;              /* the object reaches unknown code: fields/shape may change under any fact */
  int ntargets;             /* call sites: distinct functions the method name resolves to for this class (0 = none found) */
  const JSAotGenFunc *target; /* call sites: the one target when ntargets == 1, else NULL */
} QjsAotRecvClass;
/* kind 0 read, 1 write (union of the accesses at pc), 2 method call. Returns the number of classes written to out, or -1 = UNPROVEN: no such
   access, a receiver that is ext/native/unknown/primitive/not an instance-or-literal object, a call whose targets were not resolved per
   object, or more than max classes. 0 = the analysis found no receiver object at all. */
int qjs_aot_bundle_receivers(const JSAotGenFunc *g, int pc, int kind, QjsAotRecvClass *out, int max);
int qjs_aot_bundle_receivers_for(const JSAotGenFunc *g, int pc, int kind, int site, QjsAotRecvClass *out, int max);
/* the slot (position in the add order) and value-class bits of `name` in objects of `site`; 0 if the field is not added unconditionally */
int qjs_aot_bundle_site_field(int site, const char *name, int *slot, unsigned *cls);
/* the name of field `slot` of objects of `site` (the class shape's field list, in add order); 0 if not added unconditionally */
int qjs_aot_bundle_site_field_name(int site, int slot, char *out, size_t cap);
/* the site of `new F()` at (g, pc) when the class has one shape (every context adds the same fields, unconditionally, none later); -1 otherwise */
int qjs_aot_bundle_alloc_site(const JSAotGenFunc *g, int pc);
/* the number of writes of global `name` in the bundle (a declaration is one); -1 when unknown */
int qjs_aot_bundle_global_writes(const char *name);
int qjs_aot_bundle_const_int(const char *gname, const char *fname, int32_t *out);
/* the candidate targets of a method call at (g, pc) for receivers of `site`; -1 when more than max */
int qjs_aot_bundle_class_targets(const JSAotGenFunc *g, int pc, int site, const JSAotGenFunc **out, int max);

/* The region compiler's mode, read once from the environment: 0 = off (QJS_AOT_REGION=0), 1 = regions without TRUST (QJS_AOT_REGION_TRUST=0), 2 = regions with TRUST
   (the default). Every other QJS_AOT_REGION_* knob is read where it applies. */
int qjsaot_region_mode(void);

/* region.c (R3): the region compiler. Writes the body of `aot_f_HEX` for a root whose calls the bundle resolves and whose loop it can compile with
   everything it inlines; 0 with `why` otherwise. */
int region_emit(FILE *f, const Q2Func *root, const char hex[33], int fi, char *why, size_t whyn);

/* fast.c: write `aot_p_HEX`, the fast body of `fn`, to `f` and return 1; or return 0 with `why` set when the function is not one the backend
   compiles (it then runs as before). */
int fast_emit(FILE *f, const Q2Func *fn, const char hex[33], char *why, size_t whyn);
/* Why the leaf bodies were or were not made, for the report. */
void e_report_fast(const char *hex, const char *name, int ok, const char *why);

/* Whether fast_emit wrote a body for this function. */
int fast_has(const char hex[33]);
/* The lines the native entry adds to try the fast body first. */
const char *fast_entry_text(const char hex[33]);
void fast_emit_entry_call(FILE *f, const char hex[33]);

void fast_counts(int *ok, int *no);
int fast_any(void);
int qjs_aot_layout_claims_any(void); /* driver.c */


/* ---- the profile's shapes of one class (patch 0449's prototype key; E1/E2 of the raytrace push) ----------------------------------------------------
   A class whose instances gain fields after construction has several shapes over its life. The class table keeps up to QJS_AOT_MAXSHAPES of them, the ones the profile
   OBSERVED, keyed by (prototype key, ordered field list). shape 0 is always the construction walk's final shape (the `base`); shapes 1.. are profile shapes that extend
   it by fields the analysis knows are added late (qjs_aot_bundle_site_late_names), ordered by (field count, field names) so the table is deterministic. `parent[i]` is the
   shape one field shorter, when that shape is in the table, and `add[i]` the field that goes from it to shape i: an add-transition parent[i] --add[i]--> i is a constant of
   the region. A shape whose parent is -1 is reachable only through an unobserved intermediate (no transition into it is known). */
#define QJS_AOT_MAXSHAPES 16   /* storage; the cap of a class is 4 (QJS_AOT_REGION_MSHAPE_CAP=n raises it, for experiments, to at most 16) */
enum { QJS_AOT_MS_OK = 0, QJS_AOT_MS_NOPROF = 1, QJS_AOT_MS_AMBIG = 2, QJS_AOT_MS_CAP = 3 };
typedef struct QjsAotClassShapes {
  int n;                    /* shapes in the table (>= 1; 1 = only the base, nothing extra was observed) */
  int status;               /* QJS_AOT_MS_*: NOPROF no profile / no shapes of the constructor, AMBIG shapes of two different prototypes (more than one prototype key), CAP more than MAXSHAPES */
  unsigned long long pk;    /* the prototype key of the observed shapes (0 when none was observed) */
  int nn[QJS_AOT_MAXSHAPES];
  char names[QJS_AOT_MAXSHAPES][32][40];
  int parent[QJS_AOT_MAXSHAPES];
  char add[QJS_AOT_MAXSHAPES][40];
} QjsAotClassShapes;
/* driver.c: the profile's shapes of the class whose constructor has digest `hex`, given the base field list and the names added late. Always fills out->n >= 1. */
/* driver.c: QJS_AOT_REGION_MSHAPE set to something other than "" or "0" (the multi-shape class table is opt-in) */
int qjs_aot_mshape_on(void);
int qjs_aot_prof_class_shapes(const char *hex, char (*base)[40], int nbase, char (*late)[40], int nlate, QjsAotClassShapes *out);
/* bundle.c: the constructor's digest of an allocation site (1 and 32 hex chars), and the names fields are added to its objects outside the construction walk
   (deduplicated; the count, or -1 when the construction walk's own adds are not all unconditional or the site is not an instance). */
int qjs_aot_bundle_site_ctor_digest(int site, char hex[33]);
int qjs_aot_bundle_site_late_names(int site, char (*names)[40], int max);
/* the same, united over the allocation sites of the same constructor and construction-walk fields (the class) */
int qjs_aot_bundle_class_late_names(int site, char (*names)[40], int max);

#endif
